// CLZERO on hosts without it.
//
// CLZERO (AMD only) zeroes the 64-byte cache line rAX points into. Titles clear large structures
// with loops around it, one line per iteration, e.g.:
//
//     L: clzero                        L: mov rax, rP
//        add rax, 64                      add rP, 64
//        cmp rax, rEnd                    clzero
//        jb  L                            cmp rP, rEnd
//                                         jb  L
//
// Trapping once per line would cost an exception per 64 bytes. Instead the handler runs the loop
// to its exit: it interprets the faulting CLZERO and the instructions after it for as long as they
// are among the few forms these loops use, and stops when the loop's branch falls through or an
// instruction outside those forms comes up. The title then continues natively from there with the
// registers and flags the loop left. One exception per loop.

#include "core/cpu_compat.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>

#include "common/log.h"
#include "loader/loader.h"

#define CACHE_LINE 64

// x86 flag bits.
#define FLAG_CF (1u << 0)
#define FLAG_PF (1u << 2)
#define FLAG_AF (1u << 4)
#define FLAG_ZF (1u << 6)
#define FLAG_SF (1u << 7)
#define FLAG_OF (1u << 11)
#define ARITH_FLAGS (FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)

// A loop that never exits would also hang on the console; the cap only keeps the handler from
// running unbounded, and the next CLZERO traps again.
#define MAX_INSTRUCTIONS (1u << 24)

// The longest instruction we decode, to keep every fetch inside the image.
#define MAX_INSTRUCTION_BYTES 4

static volatile long g_reported = 0;

// Host CPUID: extended leaf 0x80000008, EBX bit 0.
static bool host_has_clzero() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000008u)
        return false;
    __cpuid(r, 0x80000008);
    return (r[1] & 1) != 0;
}

// Flags of a 64-bit add (r = a + b) or subtract/compare (r = a - b).
static u32 arith_flags(u64 a, u64 b, u64 r, bool sub) {
    u32 f = 0;
    if (sub ? a < b : r < a)
        f |= FLAG_CF;
    if (!(__popcnt((u32)(r & 0xff)) & 1))
        f |= FLAG_PF;
    if ((a ^ b ^ r) & 0x10)
        f |= FLAG_AF;
    if (r == 0)
        f |= FLAG_ZF;
    if (r >> 63)
        f |= FLAG_SF;
    u64 overflow = sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
    if (overflow >> 63)
        f |= FLAG_OF;
    return f;
}

// Zeroes the line; false if the memory isn't writable. CLZERO would fault there too.
static bool zero_line(u64 address) {
    __try {
        memset((void*)(address & ~(u64)(CACHE_LINE - 1)), 0, CACHE_LINE);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

static bool is_clzero(const u8* ip) {
    return ip[0] == 0x0f && ip[1] == 0x01 && ip[2] == 0xfc;
}

// Interprets from the faulting CLZERO until the loop exits. The context holds the general purpose
// registers in encoding order, rAX first, so a register number indexes them directly.
static LONG run_clzero_loop(EXCEPTION_POINTERS* ep, const PeImage* image) {
    CONTEXT* ctx = ep->ContextRecord;
    DWORD64* regs = &ctx->Rax;
    const u8* end = image->base + image->size - MAX_INSTRUCTION_BYTES;
    const u8* ip = (const u8*)ctx->Rip;
    u32 flags = ctx->EFlags;

    for (u32 executed = 0; executed < MAX_INSTRUCTIONS; executed++) {
        if (ip < image->base || ip > end)
            break;
        if (is_clzero(ip)) {
            if (!zero_line(regs[0])) {
                // Report what the console would: a write fault at the instruction.
                EXCEPTION_RECORD* rec = ep->ExceptionRecord;
                ctx->Rip = (DWORD64)ip;
                ctx->EFlags = (ctx->EFlags & ~ARITH_FLAGS) | (flags & ARITH_FLAGS);
                rec->ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                rec->ExceptionAddress = (void*)ip;
                rec->NumberParameters = 2;
                rec->ExceptionInformation[0] = 1;
                rec->ExceptionInformation[1] = (ULONG_PTR)(regs[0] & ~(u64)(CACHE_LINE - 1));
                return EXCEPTION_CONTINUE_SEARCH;
            }
            ip += 3;
            continue;
        }
        if (ip[0] == 0x72) { // jb rel8: taken while the last compare came out below
            if (!(flags & FLAG_CF)) {
                ip += 2;
                break;
            }
            ip += 2 + (s8)ip[1];
            continue;
        }
        // The rest are 64-bit register forms: REX.W, opcode, ModRM with mod 11.
        u8 rex = ip[0];
        u8 modrm = ip[2];
        if ((rex & 0xfa) != 0x48 || (modrm & 0xc0) != 0xc0)
            break;
        u32 rm = (modrm & 7) | ((rex & 1) << 3);
        u32 reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
        u8 op = ip[1];
        if (op == 0x83 && reg == 0 && rm != 4) { // add r64, imm8
            u64 a = regs[rm], b = (u64)(s64)(s8)ip[3];
            regs[rm] = a + b;
            flags = arith_flags(a, b, a + b, false);
            ip += 4;
        } else if ((op == 0x89 || op == 0x8b) && rm != 4 && reg != 4) { // mov r64, r64
            if (op == 0x89)
                regs[rm] = regs[reg];
            else
                regs[reg] = regs[rm];
            ip += 3;
        } else if (op == 0x39 || op == 0x3b) { // cmp r64, r64
            u64 a = op == 0x39 ? regs[rm] : regs[reg];
            u64 b = op == 0x39 ? regs[reg] : regs[rm];
            flags = arith_flags(a, b, a - b, true);
            ip += 3;
        } else {
            break;
        }
    }

    ctx->Rip = (DWORD64)ip;
    ctx->EFlags = (ctx->EFlags & ~ARITH_FLAGS) | (flags & ARITH_FLAGS);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK clzero_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // Only title code: its bytes are mapped and readable, and nothing else is built for the console.
    const u8* ip = (const u8*)ep->ContextRecord->Rip;
    const PeImage* image = g_loader.find_containing(ip);
    if (!image || ip + 3 > image->base + image->size || !is_clzero(ip))
        return EXCEPTION_CONTINUE_SEARCH;
    if (!InterlockedExchange(&g_reported, 1))
        LOG_I("cpu", "host CPU lacks CLZERO: emulating it, first at %s+0x%x", image->name, image->rva_of(ip));
    return run_clzero_loop(ep, image);
}

void cpu_compat_init() {
    if (host_has_clzero())
        return;
    if (!AddVectoredExceptionHandler(1, clzero_handler))
        LOG_E("cpu", "could not install the CLZERO handler");
}
