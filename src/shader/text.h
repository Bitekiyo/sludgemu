// A growable text buffer for generated source code.

#pragma once

#include "common/types.h"

struct Text {
    char* data = nullptr;
    usize length = 0;
    usize capacity = 0;
    bool failed = false; // an allocation failed; the text is incomplete

    // printf-style append.
    void put(const char* format, ...);
    void put_raw(const char* text, usize count);
    // Indentation-aware line: `depth` levels of four spaces, the formatted text, a newline.
    void line(u32 depth, const char* format, ...);
    void clear() {
        length = 0;
        failed = false;
        if (data)
            data[0] = 0;
    }
    void release();
};
