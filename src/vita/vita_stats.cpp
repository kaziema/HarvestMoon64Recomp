#include "vita_stats.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "null_renderer.h"
#if defined(HM64_VITA_RT64)
#include "plume_null.h"
#include "rt64_vita_profile_totals.h"
#endif
#include "vita_log.h"
#include "vita_timeline.h"
#include "gxm/rt64_gxm_renderer.h"

// VI interrupt counter in ultramodern's events.cpp. Only the low word is read (one aligned 32-bit load),
// so a read can never be torn.
extern uint64_t total_vis;

namespace {
    // Thread table. Each pthread adds itself as it starts (see the pthread_create wrapper in vita_log.cpp).
    struct ThreadEntry {
        std::atomic<int> uid{0}; // 0 until the entry is filled in
        int create_index = 0;
        const void* entry = nullptr;
        std::atomic<int> n64_id{-1};
        std::atomic<int> n64_priority{0};
        // Name ultramodern gave the thread (VI Thread, Gfx Thread, ...), empty if none.
        char role[32] = {};
        // Only touched by the stats thread.
        uint64_t last_run_us = 0;
        bool gone = false;
    };

    constexpr int MaxThreads = 64;
    ThreadEntry thread_table[MaxThreads];
    std::atomic<int> thread_table_count{0};

    std::atomic<uint32_t> main_loop_count{0};

    std::atomic<uint64_t> audio_task_start_us{0}; // 0 when no audio task is running
    std::atomic<uint32_t> audio_task_count{0};
    std::atomic<uint64_t> audio_task_total_us{0};
    std::atomic<uint32_t> audio_task_max_us{0}; // reset by the stats thread every second

    // Display lists must keep arriving. Before the first one the game may still be booting, so it gets longer.
    constexpr uint64_t StallLimitBeforeFirstListUs = 30 * 1000000ULL;
    constexpr uint64_t StallLimitUs = 8 * 1000000ULL;
    // On the second report for the same stall, crash on purpose so the system writes a .psp2dmp of every thread.
    constexpr int ReportsBeforeForcedDump = 2;

    uint64_t now_us() {
        return sceKernelGetProcessTimeWide();
    }

    uint32_t vi_count() {
        return *reinterpret_cast<volatile uint32_t*>(&total_vis);
    }

    // Idle time including a wait that is still in progress.
    uint64_t idle_wait_total_us(uint32_t* wait_count, uint64_t* current_wait_start_us, uint64_t now) {
        uint64_t wait_us = 0;
        ultramodern_vita_idle_stats(&wait_us, wait_count, current_wait_start_us);
        if (*current_wait_start_us != 0 && now > *current_wait_start_us) {
            wait_us += now - *current_wait_start_us;
        }
        return wait_us;
    }

    // Logs every known thread: kernel status, wait type, core, affinity, priority, and CPU time since the last
    // table (the kernel's runClocks, assumed to be microseconds; the sum line checks that against wall time).
    void log_thread_table(const char* why, uint64_t window_us) {
        const int count = thread_table_count.load() < MaxThreads ? thread_table_count.load() : MaxThreads;
        uint64_t sum_run_us = 0;
        hm64vita::log_line("threads [%s]: %d registered, window %u ms", why, count, (unsigned int)(window_us / 1000));
        for (int i = 0; i < count; i++) {
            ThreadEntry& t = thread_table[i];
            const int uid = t.uid.load();
            if (uid == 0 || t.gone) {
                continue;
            }

            SceKernelThreadInfo info{};
            info.size = sizeof(info);
            const int result = sceKernelGetThreadInfo(uid, &info);
            if (result < 0) {
                hm64vita::log_line("  thread #%d uid 0x%08X: gone (sceKernelGetThreadInfo 0x%08X)", t.create_index, (unsigned int)uid, (unsigned int)result);
                t.gone = true;
                continue;
            }

            const uint64_t run_us = info.runClocks;
            const uint64_t delta_us = run_us - t.last_run_us;
            t.last_run_us = run_us;
            sum_run_us += delta_us;
            const unsigned int percent = window_us > 0 ? (unsigned int)((delta_us * 100) / window_us) : 0;

            hm64vita::log_line("  thread #%d uid 0x%08X '%s' n64 %d (pri %d) entry %p: status 0x%X wait 0x%X/0x%08X pri %d cpu %d aff 0x%X, run +%u ms (%u%% of a core), stack %d KB",
                t.create_index, (unsigned int)uid, t.role[0] != '\0' ? t.role : info.name, t.n64_id.load(), t.n64_priority.load(), t.entry,
                (unsigned int)info.status, (unsigned int)info.waitType, (unsigned int)info.waitId,
                (int)info.currentPriority, (int)info.currentCpuId, (unsigned int)info.currentCpuAffinityMask,
                (unsigned int)(delta_us / 1000), percent, (int)(info.stackSize / 1024));
        }
        hm64vita::log_line("threads [%s]: total run +%u ms in a %u ms window (%u%% of one core, 300%% = all three)",
            why, (unsigned int)(sum_run_us / 1000), (unsigned int)(window_us / 1000),
            window_us > 0 ? (unsigned int)((sum_run_us * 100) / window_us) : 0);
    }

    void* stats_thread_func(void*) {
        // Highest user priority, so the stats and watchdog keep running even if game threads never yield.
        const int pri_result = sceKernelChangeThreadPriority(0, 64);
        hm64vita::log_line("stats thread started (set priority 64: 0x%08X)", (unsigned int)pri_result);
        // Records each thread's CPU time so far, so the first 5 second table only counts its own window.
        log_thread_table("stats start", 0);

        uint64_t last_us = now_us();
        uint64_t last_table_us = last_us;
        uint32_t seconds = 0;

        uint32_t last_dl = 0, last_scr = 0, last_loop = 0, last_vi = vi_count();
        uint32_t last_waits = 0, last_resumes = 0, last_audio_count = 0;
        uint32_t last_yield_waits = 0, last_yield_early = 0, last_gfx_waits = 0, last_gfx_early = 0;
        uint32_t last_retries = 0, last_retry_loops = 0, last_resume_failures = 0;
        uint64_t last_dl_us = 0, last_screen_us = 0;
#if defined(HM64_VITA_RT64)
        hm64vita::PlumeNullStats last_plume = hm64vita::get_plume_null_stats();
        uint64_t last_zone_us[RT64_ZONE_COUNT] = {};
        uint32_t last_zone_calls[RT64_ZONE_COUNT] = {};
#endif
        uint64_t last_idle_us = 0, last_audio_us = 0;
        {
            uint64_t start_us;
            last_idle_us = idle_wait_total_us(&last_waits, &start_us, last_us);
        }

        // Always-on frame time summary, every 30 seconds (one log line), when diagnostics are off.
        uint64_t window_start_us = last_us;
        uint32_t window_dl = 0, window_vi = last_vi, window_audio = 0;
        uint64_t window_idle_us = last_idle_us, window_audio_us = 0, window_dl_us = 0, window_screen_us = 0, window_renderer_us = 0;
        uint32_t window_renderer_frames = 0;
#if defined(HM64_VITA_RT64)
        uint64_t window_zone_us[RT64_ZONE_COUNT] = {};
#endif

        // Watchdog state.
        uint32_t progress_dl = 0;
        uint64_t progress_us = last_us;
        uint32_t progress_loop = 0;
        uint32_t progress_vi = last_vi;
        int reports = 0;

        while (true) {
            sceKernelDelayThread(1000000);

            const uint64_t now = now_us();
            const uint64_t wall_us = now - last_us;
            last_us = now;
            seconds++;

            const uint32_t dl = hm64vita::get_display_list_count();
            const uint32_t scr = hm64vita::get_screen_update_count();
            const uint32_t loop = main_loop_count.load();
            const uint32_t vi = vi_count();

            uint32_t waits = 0;
            uint64_t wait_start_us = 0;
            const uint64_t idle_us = idle_wait_total_us(&waits, &wait_start_us, now);

            int32_t last_thread = -1;
            uint64_t last_resume_us = 0;
            uint32_t resumes = 0;
            ultramodern_vita_sched_stats(&last_thread, &last_resume_us, &resumes);

            const uint32_t audio_count = audio_task_count.load();
            const uint64_t audio_us = audio_task_total_us.load();
            const uint32_t audio_max_us = audio_task_max_us.exchange(0);

            // Game CPU busy %: ultramodern runs one game thread at a time, and when none can run the game's idle
            // thread waits for an external message. Busy = wall time minus that wait.
            const uint64_t idle_delta = idle_us - last_idle_us;
            const uint32_t wait_delta = waits - last_waits;
            char busy[64];
            if (waits == 0 && wait_start_us == 0) {
                snprintf(busy, sizeof(busy), "n/a (idle thread has not waited yet)");
            }
            else {
                const uint64_t busy_us = wall_us > idle_delta ? wall_us - idle_delta : 0;
                snprintf(busy, sizeof(busy), "%u%% (idle %u ms, %u waits)",
                    (unsigned int)((busy_us * 100) / (wall_us ? wall_us : 1)), (unsigned int)(idle_delta / 1000), (unsigned int)wait_delta);
            }

            // Per-second statistics only with diagnostics on (ux0:data/hm64/diagnostics.txt): each log line is a
            // memory-card write, and these made the game lag (tests 22 and 23).
            if (hm64vita::diagnostics()) {
                hm64vita::log_line("second %u: display lists %u (+%u), screen updates %u (+%u), VIs +%u, main loop +%u | game busy %s, thread resumes +%u | audio tasks +%u, %u ms total, max %u us",
                    (unsigned int)seconds, (unsigned int)dl, (unsigned int)(dl - last_dl), (unsigned int)scr, (unsigned int)(scr - last_scr),
                    (unsigned int)(vi - last_vi), (unsigned int)(loop - last_loop), busy, (unsigned int)(resumes - last_resumes),
                    (unsigned int)(audio_count - last_audio_count), (unsigned int)((audio_us - last_audio_us) / 1000), (unsigned int)audio_max_us);

                // TEST 5 hang hypotheses: 1 ms waits that do not really wait (busy threads), the queue semaphore reporting
                // items the queue does not have (try_dequeue spin), resume waits failing (two game threads running at
                // once), and POSIX semaphore calls failing.
                uint32_t yield_waits = 0, yield_early = 0, gfx_waits = 0, gfx_early = 0;
                ultramodern_vita_timed_wait_stats(&yield_waits, &yield_early);
                ultramodern_vita_gfx_wait_stats(&gfx_waits, &gfx_early);
                const uint32_t retries = moodycamel::vita_dequeue_retries.load();
                const uint32_t retry_loops = moodycamel::vita_dequeue_retry_loops.load();
                const uint32_t resume_failures = ultramodern_vita_resume_wait_failures();
                auto& sema = moodycamel::details::vita_sema_stats;
                uint32_t running_signals = 0, running_wakes = 0;
                ultramodern_vita_running_stats(&running_signals, &running_wakes);
                hm64vita::log_line("waits second %u: yield 1ms +%u (early +%u), gfx 1ms +%u (early +%u), dequeue retries +%u (loops +%u), resume wait failures +%u | thread resumes signaled %u, woken %u (woken above signaled = woke without a resume) | kernel semas %u, failures: create %u (0x%08X), wait %u (0x%08X), signal %u (0x%08X)",
                    (unsigned int)seconds, (unsigned int)(yield_waits - last_yield_waits), (unsigned int)(yield_early - last_yield_early),
                    (unsigned int)(gfx_waits - last_gfx_waits), (unsigned int)(gfx_early - last_gfx_early),
                    (unsigned int)(retries - last_retries), (unsigned int)(retry_loops - last_retry_loops),
                    (unsigned int)(resume_failures - last_resume_failures),
                    (unsigned int)running_signals, (unsigned int)running_wakes,
                    (unsigned int)sema.created.load(),
                    (unsigned int)sema.init_failures.load(), (unsigned int)sema.init_last_error.load(),
                    (unsigned int)sema.wait_failures.load(), (unsigned int)sema.wait_last_error.load(),
                    (unsigned int)sema.post_failures.load(), (unsigned int)sema.post_last_error.load());
                last_yield_waits = yield_waits;
                last_yield_early = yield_early;
                last_gfx_waits = gfx_waits;
                last_gfx_early = gfx_early;
                last_retries = retries;
                last_retry_loops = retry_loops;
                last_resume_failures = resume_failures;

                // Renderer cost (Phase 0 step 2): RT64's front end runs inside send_dl (on the gfx thread) and
                // update_screen. Its worker threads show up separately in the 5 second thread table.
                const hm64vita::RendererTimes renderer_times = hm64vita::take_renderer_times();
                const uint32_t dl_delta = dl - last_dl;
                const uint64_t dl_us = renderer_times.display_list_us - last_dl_us;
                const uint64_t screen_us = renderer_times.screen_update_us - last_screen_us;
                hm64vita::log_line("renderer second %u: send_dl %u ms total (%u us avg, max %u us), update_screen %u ms total (max %u us)",
                    (unsigned int)seconds, (unsigned int)(dl_us / 1000), dl_delta ? (unsigned int)(dl_us / dl_delta) : 0,
                    (unsigned int)renderer_times.display_list_max_us, (unsigned int)(screen_us / 1000), (unsigned int)renderer_times.screen_update_max_us);
                last_dl_us = renderer_times.display_list_us;
                last_screen_us = renderer_times.screen_update_us;
    #if defined(HM64_VITA_RT64)
                const hm64vita::PlumeNullStats plume = hm64vita::get_plume_null_stats();
                hm64vita::log_line("gpu work second %u (dropped by the null backend): command lists +%u, draws +%u, dispatches +%u, copies +%u, presents +%u | alive: %u buffers, %u textures, %u KB mapped",
                    (unsigned int)seconds, (unsigned int)(plume.command_lists_executed - last_plume.command_lists_executed),
                    (unsigned int)(plume.draws - last_plume.draws), (unsigned int)(plume.dispatches - last_plume.dispatches),
                    (unsigned int)(plume.copies - last_plume.copies), (unsigned int)(plume.presents - last_plume.presents),
                    (unsigned int)plume.buffers_alive, (unsigned int)plume.textures_alive, (unsigned int)(plume.mapped_bytes / 1024));
                last_plume = plume;

                // TEST 9: where RT64's time goes. Zones nest (the fs.* zones are inside fullSync, fs.pairTiles is inside
                // fs.renderToRAM), so they do not add up. Each timed call also costs two timer reads (see the self-test).
                uint64_t zone_us[RT64_ZONE_COUNT];
                uint32_t zone_calls[RT64_ZONE_COUNT];
                hm64vita::rt64_zone_totals(zone_us, zone_calls);
                char zone_line[640];
                int zone_length = snprintf(zone_line, sizeof(zone_line), "rt64 zones second %u (ms/calls):", (unsigned int)seconds);
                for (int z = 0; z < RT64_ZONE_COUNT && zone_length > 0 && zone_length < (int)sizeof(zone_line); z++) {
                    zone_length += snprintf(zone_line + zone_length, sizeof(zone_line) - zone_length, " %s %u/%u", hm64vita::rt64_zone_name(z),
                        (unsigned int)((zone_us[z] - last_zone_us[z]) / 1000), (unsigned int)(zone_calls[z] - last_zone_calls[z]));
                    last_zone_us[z] = zone_us[z];
                    last_zone_calls[z] = zone_calls[z];
                }
                hm64vita::log_line("%s", zone_line);
    #endif

                last_dl = dl;
                last_scr = scr;
                last_loop = loop;
                last_vi = vi;
                last_idle_us = idle_us;
                last_waits = waits;
                last_resumes = resumes;
                last_audio_count = audio_count;
                last_audio_us = audio_us;

                // TEST 11: one two-second frame timeline, 40 seconds in (in game by then in earlier runs).
                hm64vita::timeline_tick(seconds, 40);

                if ((seconds % 5) == 0) {
                    hm64vita::log_memory("running");
                    hm64vita::log_clocks("running");
                    log_thread_table("every 5 s", now - last_table_us);
                    last_table_us = now;
                }
            }

            if (!hm64vita::diagnostics() && (seconds % 30) == 0) {
                const uint64_t span_us = now - window_start_us;
                const uint32_t frames = dl - window_dl;
                const hm64vita::RendererTimes rt = hm64vita::take_renderer_times();
                const rt64gxm::Stats gxm = rt64gxm::get_stats();
                const uint64_t idle = idle_us - window_idle_us;
                const uint32_t busy_pct = span_us ? uint32_t(((span_us > idle ? span_us - idle : 0) * 100) / span_us) : 0;
                const uint32_t audio_tasks = audio_count - window_audio;
                const uint32_t renderer_frames = gxm.frames - window_renderer_frames;
                char zones[200] = "";
#if defined(HM64_VITA_RT64)
                uint64_t zone_us[RT64_ZONE_COUNT];
                uint32_t zone_calls[RT64_ZONE_COUNT];
                hm64vita::rt64_zone_totals(zone_us, zone_calls);
                int zl = 0;
                for (int z = 0; z < RT64_ZONE_COUNT && zl >= 0 && zl < (int)sizeof(zones); z++) {
                    const uint64_t per_frame = frames ? (zone_us[z] - window_zone_us[z]) / frames : 0;
                    if (per_frame >= 300) {  // only the zones worth reading (0.3 ms per frame or more)
                        zl += snprintf(zones + zl, sizeof(zones) - zl, " %s %.1f", hm64vita::rt64_zone_name(z), per_frame / 1000.0);
                    }
                    window_zone_us[z] = zone_us[z];
                }
#endif
                hm64vita::log_line("frame time, last %u s: %.1f fps (VIs %.1f/s), game busy %u%% | per frame ms: send_dl %.1f (max %.1f), update_screen %.1f, gxm renderer %.1f, audio task %.1f (x%.1f per frame) | RT64 zones ms/frame:%s",
                    (unsigned int)(span_us / 1000000), span_us ? frames * 1e6 / span_us : 0.0, span_us ? (vi - window_vi) * 1e6 / span_us : 0.0, (unsigned int)busy_pct,
                    frames ? (rt.display_list_us - window_dl_us) / 1000.0 / frames : 0.0, rt.display_list_max_us / 1000.0,
                    frames ? (rt.screen_update_us - window_screen_us) / 1000.0 / frames : 0.0,
                    renderer_frames ? (gxm.cpu_us - window_renderer_us) / 1000.0 / renderer_frames : 0.0,
                    audio_tasks ? (audio_us - window_audio_us) / 1000.0 / audio_tasks : 0.0, frames ? double(audio_tasks) / frames : 0.0, zones);
                window_start_us = now;
                window_dl = dl;
                window_vi = vi;
                window_audio = audio_count;
                window_idle_us = idle_us;
                window_audio_us = audio_us;
                window_dl_us = rt.display_list_us;
                window_screen_us = rt.screen_update_us;
                window_renderer_us = gxm.cpu_us;
                window_renderer_frames = gxm.frames;
            }

            // Hang watchdog.
            if (dl != progress_dl) {
                if (reports > 0) {
                    hm64vita::log_line("WATCHDOG: display lists resumed after %u ms", (unsigned int)((now - progress_us) / 1000));
                }
                progress_dl = dl;
                progress_us = now;
                progress_loop = loop;
                progress_vi = vi;
                reports = 0;
                continue;
            }

            const uint64_t limit_us = (dl == 0) ? StallLimitBeforeFirstListUs : StallLimitUs;
            const uint64_t stalled_us = now - progress_us;
            if (stalled_us < limit_us * (uint64_t)(reports + 1)) {
                continue;
            }
            reports++;

            const uint64_t audio_start = audio_task_start_us.load();
            hm64vita::log_line("WATCHDOG report %d: no new display list for %u ms (display lists so far %u)",
                reports, (unsigned int)(stalled_us / 1000), (unsigned int)dl);
            // Hypothesis: the whole program stopped (main loop and VIs stopped too) vs only the game stopped.
            hm64vita::log_line("WATCHDOG: since the last display list: main loop +%u, VIs +%u",
                (unsigned int)(loop - progress_loop), (unsigned int)(vi - progress_vi));
            // Hypothesis: a game thread is stuck running (no resumes, idle thread not waiting).
            hm64vita::log_line("WATCHDOG: last N64 thread resumed: id %d, %u ms ago, total resumes %u",
                (int)last_thread, last_resume_us ? (unsigned int)((now - last_resume_us) / 1000) : 0, (unsigned int)resumes);
            // Hypothesis: every game thread is blocked waiting for an event (VI, SP, DP, AI) that never comes.
            if (wait_start_us != 0) {
                hm64vita::log_line("WATCHDOG: game idle thread has been waiting for an external message for %u ms",
                    (unsigned int)((now - wait_start_us) / 1000));
            }
            else {
                hm64vita::log_line("WATCHDOG: game idle thread is not waiting (a game thread is running or blocked elsewhere)");
            }
            // Hypothesis: the audio microcode never returns.
            if (audio_start != 0) {
                hm64vita::log_line("WATCHDOG: audio microcode has been running for %u ms", (unsigned int)((now - audio_start) / 1000));
            }
            else {
                hm64vita::log_line("WATCHDOG: no audio microcode running, %u audio tasks so far", (unsigned int)audio_count);
            }
            hm64vita::log_memory("watchdog");
            log_thread_table("watchdog", now - last_table_us);
            last_table_us = now;

            if (reports >= ReportsBeforeForcedDump) {
                hm64vita::log_line("WATCHDOG: forcing a crash so the system writes a .psp2dmp of every thread");
                *(volatile int*)0 = 0;
            }
        }
        return nullptr;
    }
}

void hm64vita::stats_register_thread(int create_index, const void* entry) {
    const int slot = thread_table_count.fetch_add(1);
    if (slot >= MaxThreads) {
        log_line("thread table full, thread #%d not tracked", create_index);
        return;
    }
    ThreadEntry& t = thread_table[slot];
    t.create_index = create_index;
    t.entry = entry;
    t.uid.store(sceKernelGetThreadId());
}

void hm64vita::stats_tag_n64_thread(int n64_id, int n64_priority) {
    const int uid = sceKernelGetThreadId();
    const int count = thread_table_count.load() < MaxThreads ? thread_table_count.load() : MaxThreads;
    for (int i = 0; i < count; i++) {
        if (thread_table[i].uid.load() == uid) {
            thread_table[i].n64_id = n64_id;
            thread_table[i].n64_priority = n64_priority;
            return;
        }
    }
    log_line("N64 thread %d runs on an untracked thread uid 0x%08X", n64_id, (unsigned int)uid);
}

// Called by ultramodern (threads.cpp) when one of its threads names itself.
extern "C" void ultramodern_vita_thread_named(const char* name) {
    const int uid = sceKernelGetThreadId();
    const int count = thread_table_count.load() < MaxThreads ? thread_table_count.load() : MaxThreads;
    for (int i = 0; i < count; i++) {
        if (thread_table[i].uid.load() == uid) {
            strncpy(thread_table[i].role, name, sizeof(thread_table[i].role) - 1);
            break;
        }
    }
    hm64vita::log_line("thread uid 0x%08X is '%s'", (unsigned int)uid, name);
}

void hm64vita::stats_note_main_loop() {
    main_loop_count++;
}

void hm64vita::stats_audio_task_begin() {
    audio_task_start_us = now_us();
}

void hm64vita::stats_audio_task_end() {
    const uint64_t start = audio_task_start_us.exchange(0);
    const uint64_t elapsed = now_us() - start;
    audio_task_total_us += elapsed;
    audio_task_count++;
    uint32_t prev_max = audio_task_max_us.load();
    while (elapsed > prev_max && !audio_task_max_us.compare_exchange_weak(prev_max, (uint32_t)elapsed)) {
    }
}

void hm64vita::log_clocks(const char* where) {
    log_line("clocks [%s]: arm %d MHz, bus %d MHz, gpu %d MHz, gpu xbar %d MHz", where,
        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
        scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
}

void hm64vita::stats_start() {
    pthread_t thread;
    const int result = pthread_create(&thread, nullptr, stats_thread_func, nullptr);
    if (result != 0) {
        log_line("stats thread could not start: %d (no per-second stats or watchdog this run)", result);
        return;
    }
    pthread_detach(thread);
}
