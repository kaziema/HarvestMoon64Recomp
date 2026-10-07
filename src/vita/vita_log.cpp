#include "vita_log.h"
#include "vita_stats.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <typeinfo>
#include <cxxabi.h>
#include <pthread.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

static const char* const LogDir = "ux0:data/hm64";
static const char* const LogPath = "ux0:data/hm64/phase0.log";

// Kernel lightweight mutex, so logging works even if pthreads are broken (one of the hypotheses).
static SceKernelLwMutexWork log_mutex;
static bool log_ready = false;

static void log_write_raw(const char* text, size_t length) {
    SceUID fd = sceIoOpen(LogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, length);
        sceIoClose(fd);
    }
}

void hm64vita::log_line(const char* fmt, ...) {
    if (!log_ready) {
        return;
    }

    char line[512];
    const unsigned long long ms = sceKernelGetProcessTimeWide() / 1000ULL;
    int prefix = snprintf(line, sizeof(line), "[%8llu ms] [th 0x%08X] ", ms, (unsigned int)sceKernelGetThreadId());
    if (prefix < 0) {
        prefix = 0;
    }

    va_list args;
    va_start(args, fmt);
    int body = vsnprintf(line + prefix, sizeof(line) - prefix - 1, fmt, args);
    va_end(args);
    if (body < 0) {
        body = 0;
    }

    // vsnprintf always null-terminates within the buffer, so strlen is bounded.
    size_t length = strlen(line);
    if (length > sizeof(line) - 2) {
        length = sizeof(line) - 2;
    }
    line[length++] = '\n';

    sceKernelLockLwMutex(&log_mutex, 1, nullptr);
    log_write_raw(line, length);
    sceKernelUnlockLwMutex(&log_mutex, 1);
}

void hm64vita::log_memory(const char* where) {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    int result = sceKernelGetFreeMemorySize(&info);
    if (result < 0) {
        log_line("memory [%s]: sceKernelGetFreeMemorySize failed 0x%08X", where, (unsigned int)result);
        return;
    }
    log_line("memory [%s]: free user %u KB, cdram %u KB, phycont %u KB", where,
        (unsigned int)(info.size_user / 1024), (unsigned int)(info.size_cdram / 1024), (unsigned int)(info.size_phycont / 1024));
}

// Hypothesis: an exception escapes somewhere and terminates the program.
// Logs the exception's type and message before aborting.
static void terminate_handler() {
    std::exception_ptr current = std::current_exception();
    if (current) {
        const std::type_info* type = abi::__cxa_current_exception_type();
        const char* type_name = type != nullptr ? type->name() : "unknown";
        try {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e) {
            hm64vita::log_line("TERMINATE: uncaught exception %s: %s", type_name, e.what());
        }
        catch (...) {
            hm64vita::log_line("TERMINATE: uncaught non-std exception %s", type_name);
        }
    }
    else {
        hm64vita::log_line("TERMINATE: std::terminate called with no active exception");
    }
    hm64vita::log_memory("terminate");
    abort();
}

// Written on every build by vita/build_stamp.cmake.
extern "C" const char hm64_build_stamp[];

// Hypothesis (TEST 1 crash): libstdc++ decides threads are usable by checking whether the weak symbol
// pthread_cancel resolved to a real address. If it is 0, every std::thread throws std::system_error.
extern "C" int pthread_cancel(pthread_t thread) __attribute__((weak));

// TEST 3 crash: pthread_once was unresolved, so the call that creates libstdc++'s static-init mutex became a
// no-op. These are the other weak threading symbols libstdc++ needs; 0 means calls to them do nothing.
extern "C" int pthread_once(pthread_once_t* once, void (*func)(void)) __attribute__((weak));
extern "C" int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) __attribute__((weak));
extern "C" int pthread_cond_broadcast(pthread_cond_t* cond) __attribute__((weak));
extern "C" int pthread_cond_destroy(pthread_cond_t* cond) __attribute__((weak));

// Runs before other static constructors (priority 101), because the first crash happened during
// static initialization, before main() and before the old log was opened.
__attribute__((constructor(101))) static void early_log_init() {
    sceKernelCreateLwMutex(&log_mutex, "hm64 log", 0, 0, nullptr);
    sceIoMkdir(LogDir, 0777);

    SceUID fd = sceIoOpen(LogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoClose(fd);
    }
    log_ready = true;

    std::set_terminate(terminate_handler);

    hm64vita::log_line("Harvest Moon 64 Vita, Phase 0 (null renderer). Static init started.");
    hm64vita::log_line("build %s", hm64_build_stamp);
    hm64vita::log_memory("static init");
    hm64vita::log_line("pthread_cancel address %p (0 means std::thread cannot start threads)", (void*)&pthread_cancel);
    hm64vita::log_line("pthread_once %p, pthread_cond_wait %p, pthread_cond_broadcast %p, pthread_cond_destroy %p (0 means calls do nothing)",
        (void*)&pthread_once, (void*)&pthread_cond_wait, (void*)&pthread_cond_broadcast, (void*)&pthread_cond_destroy);
}

// Hypothesis: thread creation fails (pthreads not active, out of memory, too many threads, bad stack size).
// Every pthread_create in the program goes through this wrapper (linked with --wrap=pthread_create).
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

// Every thread starts in this trampoline, which adds it to the stats thread table (CPU time, core, priority)
// before running the real entry point.
namespace {
    struct ThreadStart {
        void* (*start)(void*);
        void* arg;
        int create_index;
    };

    void* thread_trampoline(void* param) {
        ThreadStart info = *static_cast<ThreadStart*>(param);
        free(param);
        hm64vita::stats_register_thread(info.create_index, (const void*)info.start);
        return info.start(info.arg);
    }
}

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    static std::atomic<int> create_count{0};
    const int create_index = ++create_count;

    size_t stack_size = 0;
    if (attr != nullptr) {
        pthread_attr_getstacksize(attr, &stack_size);
    }

    int result;
    ThreadStart* trampoline_info = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (trampoline_info != nullptr) {
        *trampoline_info = { start, arg, create_index };
        result = __real_pthread_create(thread, attr, thread_trampoline, trampoline_info);
        if (result != 0) {
            free(trampoline_info);
        }
    }
    else {
        // Out of memory for 12 bytes: start the thread untracked so the run can continue.
        hm64vita::log_line("pthread_create #%d: no memory for the trampoline, thread will not be tracked", create_index);
        result = __real_pthread_create(thread, attr, start, arg);
    }

    hm64vita::log_line("pthread_create #%d: result %d, attr stack %u (0 = default), entry %p",
        create_index, result, (unsigned int)stack_size, (void*)start);
    if (result != 0) {
        hm64vita::log_memory("pthread_create failed");
    }
    return result;
}
