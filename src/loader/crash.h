#pragma once

#include "common/types.h"

// Exception logging + minidumps.
void crash_install(const wchar_t* dump_path);

// Logs a backtrace of every thread in the process (except the caller).
void crash_dump_all_threads(const char* reason);

// Starts a background thread that dumps all thread stacks after `seconds` (and every `seconds`
// afterwards). For diagnosing hangs.
void crash_start_thread_dump_timer(u32 seconds);

// Hardware write watchpoint (debug register 0) on an 8-byte location, on every thread. Each write
// is logged with the new value and the writer's backtrace. For catching stray memory writes.
// `spec` is "module.dll+0xRVA" (module must already be loaded) or an absolute "0xADDRESS".
bool crash_arm_write_watch(const char* spec);
// Hardware execution breakpoints (debug registers 1-3) on up to three comma-separated locations
// (same forms as above): each hit logs the registers, the first few times. For seeing what a
// function receives or returns while the title runs.
bool crash_arm_exec_trace(const char* spec);
// Applies armed watchpoints and traces to the calling thread (called for new threads).
void crash_watch_current_thread();

// "title.exe+0x1234 [0x140001234]" for title images,
// "kernelbase.dll+0x5678" for host modules, raw pointer otherwise.
void describe_address(const void* addr, char* out, usize cap);
