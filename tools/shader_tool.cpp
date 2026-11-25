// shader_tool: console shader code analysis.
//
//   shader_tool disasm <code.bin>          disassemble raw GPU code
//   shader_tool stats <dir> [name filter]  instruction statistics over the unique programs in a dump
//                                          directory; lists instructions that fail to decode

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "shader/rdna.h"

static u8* read_file(const char* path, u32* size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = (u8*)malloc((usize)n + 4);
    if (data && fread(data, 1, (usize)n, f) != (usize)n) {
        free(data);
        data = nullptr;
    }
    fclose(f);
    *size = (u32)n;
    return data;
}

static int disasm(const char* path) {
    u32 size = 0;
    u8* data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    const u32* code = (const u32*)data;
    u32 dwords = size / 4;
    u32 offset = 0;
    char text[256];
    while (offset / 4 < dwords) {
        RdnaInst in;
        if (!rdna_decode(code, dwords, offset, &in)) {
            printf("  %06X: %08X  <invalid>\n", offset, code[offset / 4]);
            offset += 4;
            continue;
        }
        if (in.op == OP_S_CODE_END)
            break;
        rdna_disassemble(in, text, sizeof(text));
        char words[40] = {};
        int n = 0;
        for (u32 i = 0; i < in.dwords && i < 3; i++)
            n += snprintf(words + n, sizeof(words) - n, "%08X ", code[offset / 4 + i]);
        printf("  %06X: %-27s %s\n", offset, words, text);
        offset += in.dwords * 4u;
    }
    free(data);
    return 0;
}

// --- stats -------------------------------------------------------------------------------------------

static u64 hash_bytes(const u8* p, u32 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int stats(const char* dir, const char* filter) {
    static u32 counts[OP_COUNT];
    static char first_file[OP_COUNT][64];
    static u64 seen[1 << 16];
    u32 programs = 0, unique = 0, failures = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*%s*", dir, filter ? filter : "");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no files match %s\n", pattern);
        return 1;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        u32 size = 0;
        u8* data = read_file(path, &size);
        if (!data)
            continue;
        programs++;
        u64 h = hash_bytes(data, size) | 1;
        u32 slot = (u32)h & 0xFFFF;
        bool fresh = true;
        while (seen[slot]) {
            if (seen[slot] == h) {
                fresh = false;
                break;
            }
            slot = (slot + 1) & 0xFFFF;
        }
        if (fresh) {
            seen[slot] = h;
            unique++;
            const u32* code = (const u32*)data;
            u32 dwords = size / 4;
            for (u32 offset = 0; offset / 4 < dwords;) {
                RdnaInst in;
                if (!rdna_decode(code, dwords, offset, &in)) {
                    if (failures++ < 40)
                        printf("decode failure: %s+%X: %08X %08X\n", fd.cFileName, offset, code[offset / 4],
                               offset / 4 + 1 < dwords ? code[offset / 4 + 1] : 0);
                    break;
                }
                if (in.op == OP_S_CODE_END)
                    break;
                if (!counts[in.op]++)
                    snprintf(first_file[in.op], sizeof(first_file[in.op]), "%s+%X", fd.cFileName, offset);
                offset += in.dwords * 4u;
            }
        }
        free(data);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    printf("%u programs, %u unique, %u decode failures\n", programs, unique, failures);
    // Most used first.
    for (;;) {
        u32 best = 0;
        for (u32 op = 1; op < OP_COUNT; op++)
            if (counts[op] > counts[best])
                best = op;
        if (!counts[best])
            break;
        printf("%8u  %-28s %s\n", counts[best], rdna_op_name((RdnaOp)best), first_file[best]);
        counts[best] = 0;
    }
    return 0;
}

int main(int argc, char** argv) {
    rdna_init();
    if (argc >= 3 && strcmp(argv[1], "disasm") == 0)
        return disasm(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "stats") == 0)
        return stats(argv[2], argc >= 4 ? argv[3] : nullptr);
    fprintf(stderr, "usage: shader_tool disasm <code.bin> | stats <dir> [filter]\n");
    return 1;
}
