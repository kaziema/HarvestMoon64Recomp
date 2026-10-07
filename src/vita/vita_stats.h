#pragma once

// Phase 0 measurements and hang watchdog for the Vita build.
//
// A stats thread writes one line per second to the log (display lists, VIs, game CPU busy %, audio microcode
// time) and every 5 seconds a table of every thread's CPU time, core and priority, plus memory and clocks.
// The same thread watches for display lists to stop. If they stop it logs the state of every thread, and if
// they stay stopped it crashes on purpose so the Vita writes a .psp2dmp of every thread.

namespace hm64vita {
    // Adds the calling thread to the thread table. create_index is the pthread_create count (0 for the main thread).
    void stats_register_thread(int create_index, const void* entry);

    // Tags the calling thread with the N64 OS thread it runs.
    void stats_tag_n64_thread(int n64_id, int n64_priority);

    // Called once per pass of the recomp::start main loop (update_gfx).
    void stats_note_main_loop();

    // Called around each audio microcode run.
    void stats_audio_task_begin();
    void stats_audio_task_end();

    // Logs the current ARM, bus, GPU and GPU crossbar clocks, tagged with where they were read.
    void log_clocks(const char* where);

    // Starts the stats and watchdog thread.
    void stats_start();
}
