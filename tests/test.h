#pragma once

// Shared bits of the unit tests. Each test counts its failures in g_failures; the exit code of
// sludgemu_tests.exe is the total.

#include <stdio.h>

#include "common/types.h"

extern int g_failures;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            g_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)
