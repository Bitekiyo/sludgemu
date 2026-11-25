// Console CPU instructions on hosts without them: CLZERO, MONITORX and MWAITX (all AMD only).
//
// MONITORX/MWAITX wait for a store to a watched address, but may also return early for any reason,
// so callers always re-check what they wait for. A MONITORX becomes a no-op and an MWAITX a PAUSE:
// an early return every time, which turns the wait into a polite spin. Those are patched in place
// on first use, as the replacement is exact.
//
// CLZERO:
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
#define FLAG_CF     (1u << 0)
#define FLAG_PF     (1u << 2)
#define FLAG_AF     (1u << 4)
#define FLAG_ZF     (1u << 6)
#define FLAG_SF     (1u << 7)
#define FLAG_OF     (1u << 11)
#define ARITH_FLAGS (FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)

// A loop that never exits would also hang on the console; the cap only keeps the handler from
// running unbounded, and the next CLZERO traps again.
#define MAX_INSTRUCTIONS (1u << 24)

// The longest instruction we decode, to keep every fetch inside the image.
#define MAX_INSTRUCTION_BYTES 4

static volatile long g_reported = 0;
static bool g_emulate_clzero = false;
static bool g_patch_monitorx = false;
static SRWLOCK g_patch_lock = SRWLOCK_INIT;

// Host CPUID: CLZERO is extended leaf 0x80000008 EBX bit 0, MONITORX/MWAITX leaf 0x80000001 ECX bit 29.
static bool host_has_clzero() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000008u)
        return false;
    __cpuid(r, 0x80000008);
    return (r[1] & 1) != 0;
}

static bool host_has_monitorx() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000001u)
        return false;
    __cpuid(r, 0x80000001);
    return (r[2] & (1 << 29)) != 0;
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

// MONITORX (0F 01 FA) becomes a 3-byte NOP, MWAITX (0F 01 FB) a PAUSE and a NOP.
static const u8 k_monitorx_patch[3] = {0x0f, 0x1f, 0x00};
static const u8 k_mwaitx_patch[3] = {0xf3, 0x90, 0x90};

// Rewrites the instruction at `ip` (or finds another thread already did) and resumes there.
static LONG patch_wait_instruction(u8* ip) {
    AcquireSRWLockExclusive(&g_patch_lock);
    bool ok = true;
    if (ip[0] == 0x0f && ip[1] == 0x01 && (ip[2] == 0xfa || ip[2] == 0xfb)) {
        const u8* patch = ip[2] == 0xfa ? k_monitorx_patch : k_mwaitx_patch;
        DWORD old;
        ok = VirtualProtect(ip, 3, PAGE_EXECUTE_READWRITE, &old) != 0;
        if (ok) {
            memcpy(ip, patch, 3);
            VirtualProtect(ip, 3, old, &old);
            FlushInstructionCache(GetCurrentProcess(), ip, 3);
        }
    } else {
        ok = memcmp(ip, k_monitorx_patch, 3) == 0 || memcmp(ip, k_mwaitx_patch, 3) == 0;
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH; // resumes on the replacement
}

static LONG CALLBACK compat_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // Only title code: its bytes are mapped and readable, and nothing else is built for the console.
    u8* ip = (u8*)ep->ContextRecord->Rip;
    const PeImage* image = g_loader.find_containing(ip);
    if (!image || ip + 3 > image->base + image->size)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_emulate_clzero && is_clzero(ip)) {
        if (!InterlockedExchange(&g_reported, 1))
            LOG_I("cpu", "host CPU lacks CLZERO: emulating it, first at %s+0x%x", image->name,
                  image->rva_of(ip));
        return run_clzero_loop(ep, image);
    }
    if (g_patch_monitorx)
        return patch_wait_instruction(ip);
    return EXCEPTION_CONTINUE_SEARCH;
}

void cpu_compat_init() {
    g_emulate_clzero = !host_has_clzero();
    g_patch_monitorx = !host_has_monitorx();
    if (!g_emulate_clzero && !g_patch_monitorx)
        return;
    LOG_I("cpu", "host CPU lacks%s%s; handling them for console code", g_emulate_clzero ? " CLZERO" : "",
          g_patch_monitorx ? " MONITORX/MWAITX" : "");
    if (!AddVectoredExceptionHandler(1, compat_handler))
        LOG_E("cpu", "could not install the CPU compatibility handler");
}
