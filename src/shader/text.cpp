#include "shader/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reserve(Text& t, usize extra) {
    if (t.failed)
        return false;
    usize need = t.length + extra + 1;
    if (need <= t.capacity)
        return true;
    usize capacity = t.capacity ? t.capacity * 2 : 4096;
    while (capacity < need)
        capacity *= 2;
    char* data = (char*)realloc(t.data, capacity);
    if (!data) {
        t.failed = true;
        return false;
    }
    t.data = data;
    t.capacity = capacity;
    return true;
}

static void put_va(Text& t, const char* format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (n < 0 || !reserve(t, (usize)n))
        return;
    vsnprintf(t.data + t.length, t.capacity - t.length, format, args);
    t.length += (usize)n;
}

void Text::put(const char* format, ...) {
    va_list args;
    va_start(args, format);
    put_va(*this, format, args);
    va_end(args);
}

void Text::put_raw(const char* text, usize count) {
    if (!reserve(*this, count))
        return;
    memcpy(data + length, text, count);
    length += count;
    data[length] = 0;
}

void Text::line(u32 depth, const char* format, ...) {
    if (!reserve(*this, depth * 4 + 1))
        return;
    memset(data + length, ' ', depth * 4);
    length += depth * 4;
    data[length] = 0;
    va_list args;
    va_start(args, format);
    put_va(*this, format, args);
    va_end(args);
    put_raw("\n", 1);
}

void Text::release() {
    free(data);
    data = nullptr;
    length = capacity = 0;
    failed = false;
}
