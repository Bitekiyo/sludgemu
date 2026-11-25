#pragma once

// Instructions the console's CPU has and the host's may not.
//
// The console runs a Zen 2 CPU, and titles are compiled for it. Where the host lacks one of its
// instructions, the title faults on it and an exception handler carries it out instead.

// Installs the handler if the host needs it. Call after crash_install, so the handler sees the
// fault before crash reporting does.
void cpu_compat_init();
