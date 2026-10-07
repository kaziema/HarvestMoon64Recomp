#include "vita_selftest.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <functional>
#include <pthread.h>
#include <semaphore.h>
#include <thread>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "blockingconcurrentqueue.h"

#include "vita_log.h"

namespace {
    uint64_t now_us() {
        return sceKernelGetProcessTimeWide();
    }

    struct Durations {
        uint64_t min_us = UINT64_MAX;
        uint64_t max_us = 0;
        uint64_t total_us = 0;
        int count = 0;

        void add(uint64_t us) {
            if (us < min_us) min_us = us;
            if (us > max_us) max_us = us;
            total_us += us;
            count++;
        }
    };

    void log_durations(const char* what, const Durations& d, int returned_true, int extra, const char* extra_name) {
        hm64vita::log_line("selftest %s: %d calls, min %u us, avg %u us, max %u us, returned true %d, %s %d", what, d.count,
            (unsigned int)(d.count ? d.min_us : 0), (unsigned int)(d.count ? d.total_us / d.count : 0), (unsigned int)d.max_us,
            returned_true, extra_name, extra);
    }

    // Hypothesis: sem_timedwait with a 1 ms deadline returns at once instead of waiting (timeout rounding or a
    // clock mismatch), which turns every 1 ms wait into a busy loop.
    void test_sem_timedwait() {
        sem_t sem;
        if (sem_init(&sem, 0, 0) != 0) {
            hm64vita::log_line("selftest sem_timedwait: sem_init failed, errno %d", errno);
            return;
        }
        Durations d;
        int returned_zero = 0;
        int last_errno = 0;
        for (int i = 0; i < 20; i++) {
            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_nsec -= 1000000000;
                ts.tv_sec++;
            }
            const uint64_t start = now_us();
            const int rc = sem_timedwait(&sem, &ts);
            d.add(now_us() - start);
            if (rc == 0) {
                returned_zero++;
            }
            else {
                last_errno = errno;
            }
        }
        sem_destroy(&sem);
        log_durations("sem_timedwait 1 ms (expect about 1000 us, true 0)", d, returned_zero, last_errno, "last errno (ETIMEDOUT is 116)");
    }

    // Hypothesis: the same, one level up, in moodycamel's semaphore (includes its 10000-iteration spin).
    void test_lightweight_timed() {
        moodycamel::LightweightSemaphore sema;
        Durations d;
        int returned_true = 0;
        for (int i = 0; i < 20; i++) {
            const uint64_t start = now_us();
            if (sema.wait(1000)) {
                returned_true++;
            }
            d.add(now_us() - start);
        }
        log_durations("LightweightSemaphore wait(1000) (expect about 1000 us, true 0)", d, returned_true, 0, "unused");
    }

    // Hypothesis: the queue's 1 ms timed dequeue (used by yield_self_1ms and the gfx thread) does not wait.
    void test_queue_timed() {
        moodycamel::BlockingConcurrentQueue<int> queue;
        Durations d;
        int returned_true = 0;
        for (int i = 0; i < 20; i++) {
            int item;
            const uint64_t start = now_us();
            if (queue.wait_dequeue_timed(item, std::chrono::milliseconds(1))) {
                returned_true++;
            }
            d.add(now_us() - start);
        }
        log_durations("BlockingConcurrentQueue wait_dequeue_timed(1 ms) (expect about 1000 us, true 0)", d, returned_true, 0, "unused");
    }

    // Hypothesis: an untimed wait returns without being signaled (a parked N64 thread wakes up on its own), or a
    // signal does not wake the waiter.
    void test_untimed_wait() {
        // Static: if the waiter never wakes it is detached and must not outlive these.
        static moodycamel::LightweightSemaphore sema;
        static std::atomic<int> state{0}; // 0 waiting, 1 returned true, 2 returned false
        static std::atomic<uint64_t> returned_at{0};
        std::thread waiter([]() {
            const bool result = sema.wait();
            returned_at = now_us();
            state = result ? 1 : 2;
        });
        sceKernelDelayThread(200 * 1000);
        const int state_before_signal = state.load();
        const uint64_t signal_at = now_us();
        sema.signal();
        for (int i = 0; i < 100 && state.load() == 0; i++) {
            sceKernelDelayThread(1000);
        }
        const int state_after_signal = state.load();
        if (state_after_signal != 0) {
            waiter.join();
        }
        else {
            waiter.detach();
        }
        hm64vita::log_line("selftest untimed LightweightSemaphore wait: before signal %s, after signal %s, woke %d us after signal (expect: still waiting, then returned true)",
            state_before_signal == 0 ? "still waiting" : (state_before_signal == 1 ? "RETURNED TRUE WITHOUT SIGNAL" : "RETURNED FALSE WITHOUT SIGNAL"),
            state_after_signal == 0 ? "NEVER WOKE" : (state_after_signal == 1 ? "returned true" : "RETURNED FALSE"),
            state_after_signal != 0 ? (int)(returned_at.load() - signal_at) : -1);
    }

    // Hypothesis: a thread that waits for a signal burns CPU while it waits (measured with the kernel's per-thread
    // CPU time while it waits 300 ms on an untimed wait, and on repeated 1 ms timed waits).
    void test_wait_cpu() {
        for (int timed = 0; timed < 2; timed++) {
            moodycamel::BlockingConcurrentQueue<int> queue;
            std::atomic<bool> stop{false};
            std::atomic<int> uid{0};
            std::thread waiter([&]() {
                uid = sceKernelGetThreadId();
                int item;
                if (timed) {
                    while (!stop.load()) {
                        queue.wait_dequeue_timed(item, std::chrono::milliseconds(1));
                    }
                }
                else {
                    queue.wait_dequeue(item);
                }
            });
            while (uid.load() == 0) {
                sceKernelDelayThread(1000);
            }
            SceKernelThreadInfo before{};
            before.size = sizeof(before);
            sceKernelGetThreadInfo(uid.load(), &before);
            const uint64_t start = now_us();
            sceKernelDelayThread(300 * 1000);
            SceKernelThreadInfo after{};
            after.size = sizeof(after);
            sceKernelGetThreadInfo(uid.load(), &after);
            const uint64_t wall = now_us() - start;
            stop = true;
            queue.enqueue(0);
            waiter.join();
            const uint64_t run = after.runClocks - before.runClocks;
            hm64vita::log_line("selftest CPU while waiting (%s): %u ms of CPU in %u ms (%u%%, expect near 0%%)",
                timed ? "repeated 1 ms wait_dequeue_timed" : "untimed wait_dequeue",
                (unsigned int)(run / 1000), (unsigned int)(wall / 1000), wall ? (unsigned int)(run * 100 / wall) : 0);
        }
    }

    // Hypothesis: thread IDs collide (moodycamel uses std::this_thread::get_id on ARM to pick each producer's
    // slot; shared IDs would corrupt the queue and lose items the semaphore already counted).
    // Both threads stay alive until both have read their IDs: a finished thread's handle is reused, which made
    // TEST 6's version of this check report a collision that was not one.
    void test_thread_ids() {
        std::thread::id ids[3];
        pthread_t selves[3];
        std::atomic<int> ready{0};
        ids[0] = std::this_thread::get_id();
        selves[0] = pthread_self();
        auto body = [&](int index) {
            ids[index] = std::this_thread::get_id();
            selves[index] = pthread_self();
            ready++;
            while (ready.load() < 2) {
                sceKernelDelayThread(1000);
            }
        };
        std::thread a(body, 1);
        std::thread b(body, 2);
        a.join();
        b.join();
        const bool ids_distinct = ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2];
        hm64vita::log_line("selftest thread ids: std::thread::id %s, pthread_self %p %p %p",
            ids_distinct ? "distinct" : "COLLIDE", (void*)(uintptr_t)selves[0], (void*)(uintptr_t)selves[1], (void*)(uintptr_t)selves[2]);
    }

    // Hypothesis: CLOCK_REALTIME (used for sem_timedwait deadlines) disagrees with the kernel's process clock.
    void test_clocks() {
        timespec rt0, rt1;
        clock_gettime(CLOCK_REALTIME, &rt0);
        const uint64_t p0 = now_us();
        sceKernelDelayThread(20 * 1000);
        clock_gettime(CLOCK_REALTIME, &rt1);
        const uint64_t p1 = now_us();
        const int64_t rt_us = (int64_t)(rt1.tv_sec - rt0.tv_sec) * 1000000 + (rt1.tv_nsec - rt0.tv_nsec) / 1000;
        timespec res{};
        clock_getres(CLOCK_REALTIME, &res);
        hm64vita::log_line("selftest clocks: over a 20 ms delay, CLOCK_REALTIME moved %d us, process clock moved %u us, CLOCK_REALTIME resolution %ld ns",
            (int)rt_us, (unsigned int)(p1 - p0), (long)res.tv_nsec);
    }
}

namespace {
    uint32_t next_random(uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }

    // Hypothesis (TEST 6): a counting semaphore drifts ahead of its real signals under the pattern ultramodern
    // uses (one producer signaling at random times, one consumer doing 1 ms timed waits). Run against the VitaSDK
    // POSIX semaphore and against moodycamel's semaphore (now the kernel semaphore). Any extra count is drift.
    void test_semaphore_drift() {
        constexpr int Posts = 2000;

        // POSIX (pthread-embedded). On a timeout, retry with sem_trywait like moodycamel's re-adjust path.
        {
            sem_t sem;
            sem_init(&sem, 0, 0);
            std::thread producer([&]() {
                uint32_t rng = 12345;
                for (int i = 0; i < Posts; i++) {
                    sceKernelDelayThread(next_random(rng) % 300);
                    sem_post(&sem);
                }
            });
            int consumed = 0;
            const uint64_t deadline = now_us() + 4000000;
            while (consumed < Posts && now_us() < deadline) {
                timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 1000000;
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_nsec -= 1000000000;
                    ts.tv_sec++;
                }
                if (sem_timedwait(&sem, &ts) == 0 || sem_trywait(&sem) == 0) {
                    consumed++;
                }
            }
            producer.join();
            int extra = 0;
            while (extra < 1000 && sem_trywait(&sem) == 0) {
                extra++;
            }
            int value = 0;
            sem_getvalue(&sem, &value);
            sem_destroy(&sem);
            hm64vita::log_line("selftest drift POSIX sem: posted %d, consumed %d, extra after drain %d, final value %d (expect %d, %d, 0, 0)",
                Posts, consumed, extra, value, Posts, Posts);
        }

        // moodycamel's semaphore and queue, the exact path the gfx thread uses.
        {
            moodycamel::BlockingConcurrentQueue<int> queue;
            const uint32_t retries_before = moodycamel::vita_dequeue_retries.load();
            std::thread producer([&]() {
                uint32_t rng = 54321;
                for (int i = 0; i < Posts; i++) {
                    sceKernelDelayThread(next_random(rng) % 300);
                    queue.enqueue(i);
                }
            });
            int consumed = 0;
            int out_of_order = 0;
            int expected = 0;
            const uint64_t deadline = now_us() + 4000000;
            while (consumed < Posts && now_us() < deadline) {
                int item;
                if (queue.wait_dequeue_timed(item, std::chrono::milliseconds(1))) {
                    if (item != expected) {
                        out_of_order++;
                    }
                    expected = item + 1;
                    consumed++;
                }
            }
            producer.join();
            int item;
            const bool extra = queue.wait_dequeue_timed(item, std::chrono::milliseconds(5));
            hm64vita::log_line("selftest drift queue (kernel sema): enqueued %d, dequeued %d, out of order %d, dequeue retries %u, extra item %s (expect %d, %d, 0, 0, no)",
                Posts, consumed, out_of_order, (unsigned int)(moodycamel::vita_dequeue_retries.load() - retries_before),
                extra ? "YES" : "no", Posts, Posts);
        }
    }
}

namespace {
    // The RT64 profiling zones read the process clock twice per timed call. Its cost tells how much of a zone's
    // total is the measurement itself (TEST 9).
    void test_timer_cost() {
        constexpr int Reads = 20000;
        const uint64_t start = now_us();
        uint64_t sink = 0;
        for (int i = 0; i < Reads; i++) {
            sink += sceKernelGetProcessTimeWide();
        }
        const uint64_t elapsed = now_us() - start;
        hm64vita::log_line("selftest timer cost: %d reads of the process clock in %u us (%u ns each)%s", Reads,
            (unsigned int)elapsed, (unsigned int)((elapsed * 1000) / Reads), sink == 0 ? " " : "");
    }
}

void hm64vita::run_threading_self_test() {
    log_line("selftest: start");
    test_timer_cost();
    test_clocks();
    test_sem_timedwait();
    test_lightweight_timed();
    test_queue_timed();
    test_untimed_wait();
    test_wait_cpu();
    test_thread_ids();
    test_semaphore_drift();
    log_line("selftest: done");
}
