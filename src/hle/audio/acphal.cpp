// AcpHal.dll: the audio processor's memory (APU heap) and its control interface.
//
// Console titles run their audio on dedicated hardware: SHAPE (sample rate conversion, EQ and
// compression, filters and volume, DMA) and XMA decoding, programmed through per-voice context
// arrays and flowgraphs in APU memory, and driven by an audio control processor (ACP) that takes
// commands and sends messages back every audio frame.
//
// The APU heap and the context arrays are real. The ACP side keeps time and keeps the protocol
// going (commands complete, notifications are written, frame and flowgraph messages arrive on
// schedule), but doesn't process audio yet, so output is silent.

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "hle/hle.h"

// --- APU heap ------------------------------------------------------------------------------------
//
// The APU addresses its memory with 32-bit addresses. Both heaps (cached and non-cached) share one
// allocation; an APU address is the offset into it plus APU_ADDRESS_BASE, so 0 is never valid.

typedef u32 ApuAddress;

#define APU_ADDRESS_BASE    0x10000u
#define APU_ALLOC_CACHED    0x1u
#define APU_ALLOC_NONCACHED 0x2u

#define FACILITY_APU                 MAKE_HRESULT(SEVERITY_ERROR, 0xACA, 0)
#define APU_E_HEAP_ALREADY_ALLOCATED (FACILITY_APU | 0x001)
#define APU_E_HEAP_NOT_ALLOCATED     (FACILITY_APU | 0x002)

struct ApuBlock {
    u32 offset; // from the heap's start
    u32 size;
};

struct ApuHeap {
    u8* base;
    u32 size;
    u32 offset;       // of base within the APU address space, after APU_ADDRESS_BASE
    ApuBlock* blocks; // sorted by offset
    u32 count;
    u32 capacity;
};

static struct {
    SRWLOCK lock;
    u8* memory;
    u64 memory_size;
    ApuHeap heaps[2]; // cached, non-cached
} g_apu = {SRWLOCK_INIT};

// Lowest-addressed gap that fits. Heap lock held.
static bool heap_alloc(ApuHeap* h, u32 size, u32 alignment, u32* out_offset) {
    u32 start = 0;
    for (u32 i = 0; i <= h->count; i++) {
        u32 end = i < h->count ? h->blocks[i].offset : h->size;
        u64 at = ALIGN_UP((u64)start, (u64)alignment);
        if (at + size <= end) {
            if (h->count == h->capacity) {
                u32 capacity = h->capacity ? h->capacity * 2 : 64;
                ApuBlock* grown = (ApuBlock*)realloc(h->blocks, capacity * sizeof(ApuBlock));
                if (!grown)
                    return false;
                h->blocks = grown;
                h->capacity = capacity;
            }
            memmove(&h->blocks[i + 1], &h->blocks[i], (h->count - i) * sizeof(ApuBlock));
            h->blocks[i] = {(u32)at, size};
            h->count++;
            *out_offset = (u32)at;
            return true;
        }
        if (i < h->count)
            start = h->blocks[i].offset + h->blocks[i].size;
    }
    return false;
}

static bool heap_free(ApuHeap* h, u32 offset) {
    u32 lo = 0, hi = h->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (h->blocks[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == h->count || h->blocks[lo].offset != offset)
        return false;
    memmove(&h->blocks[lo], &h->blocks[lo + 1], (h->count - lo - 1) * sizeof(ApuBlock));
    h->count--;
    return true;
}

static ApuHeap* heap_containing(const void* p) {
    for (ApuHeap& h : g_apu.heaps) {
        if (h.base && (const u8*)p >= h.base && (const u8*)p < h.base + h.size)
            return &h;
    }
    return nullptr;
}

static HRESULT WINAPI ApuCreateHeap(u32 cached_size, u32 noncached_size);

static HRESULT WINAPI ApuAlloc(void** virtual_address, ApuAddress* physical_address, u32 size, u32 alignment,
                               u32 flags) {
    if (virtual_address)
        *virtual_address = nullptr;
    if (physical_address)
        *physical_address = 0;
    if (!virtual_address || !size || alignment < 4 || (alignment & (alignment - 1)))
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_apu.lock);
    if (!g_apu.memory) {
        ReleaseSRWLockExclusive(&g_apu.lock);
        return APU_E_HEAP_NOT_ALLOCATED;
    }
    ApuHeap* h = &g_apu.heaps[(flags & APU_ALLOC_NONCACHED) ? 1 : 0];
    u32 offset;
    bool ok = heap_alloc(h, size, alignment, &offset);
    // The heap is only reserved: blocks commit their pages as they are handed out (and keep them).
    if (ok && !VirtualAlloc(h->base + offset, size, MEM_COMMIT, PAGE_READWRITE)) {
        heap_free(h, offset);
        ok = false;
    }
    ReleaseSRWLockExclusive(&g_apu.lock);
    if (!ok) {
        LOG_W("apu", "ApuAlloc(%u bytes, alignment %u, flags 0x%x): out of memory", size, alignment, flags);
        return E_OUTOFMEMORY;
    }
    memset(h->base + offset, 0, size);
    *virtual_address = h->base + offset;
    if (physical_address)
        *physical_address = APU_ADDRESS_BASE + h->offset + offset;
    return S_OK;
}

static void WINAPI ApuFree(void* virtual_address) {
    if (!virtual_address)
        return;
    AcquireSRWLockExclusive(&g_apu.lock);
    ApuHeap* h = heap_containing(virtual_address);
    bool ok = h && heap_free(h, (u32)((u8*)virtual_address - h->base));
    ReleaseSRWLockExclusive(&g_apu.lock);
    if (!ok)
        LOG_W("apu", "ApuFree(%p): not an APU heap allocation", virtual_address);
}

static ApuAddress WINAPI ApuMapVirtualAddress(const void* virtual_address) {
    const u8* p = (const u8*)virtual_address;
    if (!g_apu.memory || p < g_apu.memory || p >= g_apu.memory + g_apu.memory_size)
        return 0;
    return APU_ADDRESS_BASE + (u32)(p - g_apu.memory);
}

static void* WINAPI ApuMapApuAddress(ApuAddress address) {
    if (!g_apu.memory || address < APU_ADDRESS_BASE || address - APU_ADDRESS_BASE >= g_apu.memory_size)
        return nullptr;
    return g_apu.memory + (address - APU_ADDRESS_BASE);
}

static bool WINAPI ApuIsVirtualAddressValid(const void* virtual_address, u32 alignment) {
    ApuAddress a = ApuMapVirtualAddress(virtual_address);
    return a && (!alignment || a % alignment == 0);
}

// --- SHAPE context arrays ------------------------------------------------------------------------

// Sizes of the console context records, in the order the request lists them.
enum ShapeContextKind {
    SHAPE_SRC,
    SHAPE_EQCOMP,
    SHAPE_FILTVOL,
    SHAPE_DMA,
    SHAPE_XMA,
    SHAPE_PCM,
    SHAPE_KINDS
};
static const u32 k_context_size[SHAPE_KINDS] = {32, 160, 32, 16, 64, 32};
static const u32 k_context_max[SHAPE_KINDS] = {4096, 4096, 4096, 4096, 512, 3584};

struct ShapeContexts {
    u32 count[SHAPE_KINDS];
    void* arrays[SHAPE_KINDS];
    ApuAddress apu_arrays[SHAPE_KINDS];
};
static_assert(sizeof(ShapeContexts) == 96, "SHAPE context request layout");

#define FACILITY_ACP            MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0)
#define ACP_E_ALREADY_ALLOCATED (FACILITY_ACP | 0x001)
#define ACP_E_NOT_ALLOCATED     (FACILITY_ACP | 0x002)
#define ACP_E_NOT_CONNECTED     (FACILITY_ACP | 0x00a)
#define ACP_E_STILL_CONNECTED   (FACILITY_ACP | 0x010)

static void* g_context_arrays[SHAPE_KINDS];
static volatile long g_connections = 0;

// Context arrays come from the non-cached heap, which by default is sized for the maximum of each
// kind.
#define SHAPE_CONTEXT_ALIGNMENT 256

static HRESULT WINAPI AcpHalReleaseShapeContexts();

static HRESULT WINAPI AcpHalAllocateShapeContexts(ShapeContexts* request) {
    if (!request)
        return E_INVALIDARG;
    for (u32 k = 0; k < SHAPE_KINDS; k++) {
        if (request->count[k] > k_context_max[k])
            return E_INVALIDARG;
        if (g_context_arrays[k])
            return ACP_E_ALREADY_ALLOCATED;
    }
    for (u32 k = 0; k < SHAPE_KINDS; k++) {
        request->arrays[k] = nullptr;
        request->apu_arrays[k] = 0;
        if (!request->count[k])
            continue;
        HRESULT hr =
            ApuAlloc(&request->arrays[k], &request->apu_arrays[k], request->count[k] * k_context_size[k],
                     SHAPE_CONTEXT_ALIGNMENT, APU_ALLOC_NONCACHED);
        if (FAILED(hr)) {
            AcpHalReleaseShapeContexts();
            return E_OUTOFMEMORY;
        }
        g_context_arrays[k] = request->arrays[k];
    }
    LOG_I("apu", "SHAPE contexts: %u SRC, %u EQ/comp, %u filter/volume, %u DMA, %u XMA, %u PCM",
          request->count[0], request->count[1], request->count[2], request->count[3], request->count[4],
          request->count[5]);
    return S_OK;
}

static HRESULT WINAPI AcpHalReleaseShapeContexts() {
    if (g_connections)
        return ACP_E_STILL_CONNECTED;
    bool any = false;
    for (u32 k = 0; k < SHAPE_KINDS; k++) {
        if (g_context_arrays[k]) {
            ApuFree(g_context_arrays[k]);
            g_context_arrays[k] = nullptr;
            any = true;
        }
    }
    return any ? S_OK : ACP_E_NOT_ALLOCATED;
}

static HRESULT WINAPI ApuCreateHeap(u32 cached_size, u32 noncached_size) {
    if (!noncached_size) {
        for (u32 k = 0; k < SHAPE_KINDS; k++)
            noncached_size += ALIGN_UP(k_context_max[k] * k_context_size[k], SHAPE_CONTEXT_ALIGNMENT);
    }
    u64 cached = ALIGN_UP((u64)cached_size, KiB(64));
    u64 noncached = ALIGN_UP((u64)noncached_size, KiB(64));
    if (cached + noncached > 0xFFFFFFFFull - APU_ADDRESS_BASE)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_apu.lock);
    HRESULT hr = S_OK;
    if (g_apu.memory) {
        hr = APU_E_HEAP_ALREADY_ALLOCATED;
    } else {
        u8* memory = (u8*)VirtualAlloc(nullptr, cached + noncached, MEM_RESERVE, PAGE_READWRITE);
        if (!memory) {
            hr = E_OUTOFMEMORY;
        } else {
            g_apu.memory = memory;
            g_apu.memory_size = cached + noncached;
            g_apu.heaps[0] = {memory, (u32)cached, 0, nullptr, 0, 0};
            g_apu.heaps[1] = {memory + cached, (u32)noncached, (u32)cached, nullptr, 0, 0};
        }
    }
    ReleaseSRWLockExclusive(&g_apu.lock);
    LOG_I("apu", "ApuCreateHeap(%u KiB cached, %u KiB non-cached) -> 0x%08lx", cached_size >> 10,
          noncached_size >> 10, (unsigned long)hr);
    return hr;
}

// --- audio control processor ---------------------------------------------------------------------

#define ACP_COMMAND_LOAD_SHAPE_FLOWGRAPH 1
#define ACP_COMMAND_REGISTER_MESSAGE     2
#define ACP_COMMAND_UNREGISTER_MESSAGE   3
#define ACP_COMMAND_START_FLOWGRAPH      4
#define ACP_COMMAND_LAST                 27

#define ACP_MESSAGE_AUDIO_FRAME_START   0x01
#define ACP_MESSAGE_FLOWGRAPH_COMPLETED 0x02
#define ACP_MESSAGE_COMMAND_COMPLETED   0x10
#define ACP_MESSAGE_DISCONNECTED        0x80

// The hardware's audio frame: 256 samples at 48 kHz.
#define ACP_FRAME_US 5333

#pragma pack(push, 1)
struct AcpCommandCompleted {
    u32 command_type;
    u64 command_id; // as submitted
    u32 audio_frame;
};
struct AcpMessage {
    u32 type;
    u32 dropped; // messages dropped before this one (queue full)
    u32 usec;    // when the message was made
    union {
        u32 audio_frame;
        ApuAddress flowgraph;
        AcpCommandCompleted completed;
        u8 raw[16];
    };
};
struct AcpLoadFlowgraph {
    u32 command_count;
    ApuAddress flowgraph;
    u32 persist;
    u32 wait_for_start;
};
#pragma pack(pop)
static_assert(sizeof(AcpMessage) == 28, "ACP message layout");

struct Acp;

// The console interface has no IUnknown: six methods taking the instance first.
struct AcpVtable {
    HRESULT(STDMETHODCALLTYPE* connect)(Acp* self, u32 command_count, u32 message_count);
    HRESULT(STDMETHODCALLTYPE* disconnect)(Acp* self);
    HRESULT(STDMETHODCALLTYPE* submit_command)(Acp* self, u32 type, u64 id, u32 audio_frame, const void* data,
                                               ApuAddress notification);
    bool(STDMETHODCALLTYPE* pop_message)(Acp* self, AcpMessage* message);
    u32(STDMETHODCALLTYPE* get_message_count)(Acp* self);
    void(STDMETHODCALLTYPE* release)(Acp* self);
};

struct Acp {
    const AcpVtable* vtable;
    SRWLOCK lock;
    bool connected;
    u32 registered;       // message types the title wants
    AcpMessage* messages; // ring
    u32 message_capacity;
    u32 message_head;
    u32 message_count;
    u32 dropped;
    u32 frame;            // current audio frame
    ApuAddress flowgraph; // loaded flowgraph, 0 if none
    bool flowgraph_persist;
    bool flowgraph_running;
    HANDLE thread;
    HANDLE stop;
    LARGE_INTEGER start;
};

static LARGE_INTEGER g_qpc_frequency;

static u32 elapsed_us(const Acp* self) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (u32)((now.QuadPart - self->start.QuadPart) * 1000000 / g_qpc_frequency.QuadPart);
}

// Queues a message the title registered for. Instance lock held.
static void post_message(Acp* self, u32 type, const void* data, u32 size) {
    if (!(self->registered & type) || !self->message_capacity)
        return;
    if (self->message_count == self->message_capacity) {
        self->dropped++;
        return;
    }
    AcpMessage& m = self->messages[(self->message_head + self->message_count) % self->message_capacity];
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.dropped = self->dropped;
    m.usec = elapsed_us(self);
    memcpy(m.raw, data, size);
    self->dropped = 0;
    self->message_count++;
}

// One audio frame: the frame message, then the flowgraph's run (processed instantly, as nothing is
// rendered yet).
static void run_frame(Acp* self) {
    AcquireSRWLockExclusive(&self->lock);
    self->frame++;
    post_message(self, ACP_MESSAGE_AUDIO_FRAME_START, &self->frame, sizeof(self->frame));
    if (self->flowgraph && self->flowgraph_running) {
        post_message(self, ACP_MESSAGE_FLOWGRAPH_COMPLETED, &self->flowgraph, sizeof(self->flowgraph));
        if (!self->flowgraph_persist)
            self->flowgraph = 0;
    }
    ReleaseSRWLockExclusive(&self->lock);
}

// Frames follow the clock, not the timer: late wakeups catch up, so time never drifts.
static DWORD WINAPI frame_thread(void* param) {
    Acp* self = (Acp*)param;
    HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    u64 frames_done = 0;
    for (;;) {
        u64 due = (frames_done + 1) * ACP_FRAME_US;
        u64 now = elapsed_us(self);
        if (now < due) {
            if (timer) {
                LARGE_INTEGER wait;
                wait.QuadPart = -(LONGLONG)(due - now) * 10;
                SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE);
                HANDLE handles[2] = {self->stop, timer};
                if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
                    break;
            } else if (WaitForSingleObject(self->stop, (DWORD)((due - now) / 1000 + 1)) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        run_frame(self);
        frames_done++;
    }
    if (timer)
        CloseHandle(timer);
    return 0;
}

static void stop_frames(Acp* self) {
    if (!self->thread)
        return;
    SetEvent(self->stop);
    WaitForSingleObject(self->thread, INFINITE);
    CloseHandle(self->thread);
    CloseHandle(self->stop);
    self->thread = nullptr;
    self->stop = nullptr;
}

static HRESULT STDMETHODCALLTYPE acp_connect(Acp* self, u32 command_count, u32 message_count) {
    if (!command_count)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&self->lock);
    bool already = self->connected;
    ReleaseSRWLockExclusive(&self->lock);
    if (already)
        return (FACILITY_ACP | 0x009); // already connected
    AcpMessage* messages = message_count ? (AcpMessage*)calloc(message_count, sizeof(AcpMessage)) : nullptr;
    if (message_count && !messages)
        return E_OUTOFMEMORY;
    self->messages = messages;
    self->message_capacity = message_count;
    self->message_head = self->message_count = self->dropped = 0;
    self->registered = 0;
    self->frame = 0;
    self->flowgraph = 0;
    QueryPerformanceCounter(&self->start);
    self->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    self->thread = self->stop ? CreateThread(nullptr, 0, frame_thread, self, 0, nullptr) : nullptr;
    if (!self->thread) {
        if (self->stop)
            CloseHandle(self->stop);
        self->stop = nullptr;
        free(messages);
        self->messages = nullptr;
        return E_OUTOFMEMORY;
    }
    SetThreadPriority(self->thread, THREAD_PRIORITY_TIME_CRITICAL);
    self->connected = true;
    InterlockedIncrement(&g_connections);
    LOG_I("apu",
          "ACP connected (%u commands, %u messages); audio processing is not implemented, output is silent",
          command_count, message_count);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE acp_disconnect(Acp* self) {
    if (!self->connected)
        return ACP_E_NOT_CONNECTED;
    stop_frames(self);
    AcquireSRWLockExclusive(&self->lock);
    self->connected = false;
    self->flowgraph = 0;
    post_message(self, ACP_MESSAGE_DISCONNECTED, nullptr, 0);
    ReleaseSRWLockExclusive(&self->lock);
    InterlockedDecrement(&g_connections);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE acp_submit_command(Acp* self, u32 type, u64 id, u32 audio_frame,
                                                    const void* data, ApuAddress notification) {
    if (!self->connected)
        return ACP_E_NOT_CONNECTED;
    if (type == 0 || type > ACP_COMMAND_LAST)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&self->lock);
    switch (type) {
    case ACP_COMMAND_REGISTER_MESSAGE:
        if (data)
            self->registered |= *(const u32*)data;
        break;
    case ACP_COMMAND_UNREGISTER_MESSAGE:
        if (data)
            self->registered &= ~*(const u32*)data;
        break;
    case ACP_COMMAND_LOAD_SHAPE_FLOWGRAPH:
        if (data) {
            const AcpLoadFlowgraph* f = (const AcpLoadFlowgraph*)data;
            self->flowgraph = f->flowgraph;
            self->flowgraph_persist = f->persist != 0;
            self->flowgraph_running = !f->wait_for_start;
        }
        break;
    case ACP_COMMAND_START_FLOWGRAPH:
        self->flowgraph_running = true;
        break;
    default:
        // Context updates and pointer increments only matter to audio processing.
        break;
    }
    if (id) {
        AcpCommandCompleted done = {type, id, self->frame};
        post_message(self, ACP_MESSAGE_COMMAND_COMPLETED, &done, sizeof(done));
    }
    ReleaseSRWLockExclusive(&self->lock);
    if (notification) {
        if (volatile u32* flag = (volatile u32*)ApuMapApuAddress(notification))
            *flag = 1;
        else
            LOG_W_ONCE("apu", "ACP command notification at APU address 0x%x is outside the APU heap",
                       notification);
    }
    (void)audio_frame; // processed now; the frame only orders commands on hardware
    return S_OK;
}

static bool STDMETHODCALLTYPE acp_pop_message(Acp* self, AcpMessage* message) {
    AcquireSRWLockExclusive(&self->lock);
    bool any = self->message_count != 0;
    if (any) {
        *message = self->messages[self->message_head];
        self->message_head = (self->message_head + 1) % self->message_capacity;
        self->message_count--;
    }
    ReleaseSRWLockExclusive(&self->lock);
    return any;
}

static u32 STDMETHODCALLTYPE acp_get_message_count(Acp* self) {
    return self->message_count;
}

static void STDMETHODCALLTYPE acp_release(Acp* self) {
    if (self->connected)
        acp_disconnect(self);
    free(self->messages);
    free(self);
}

static const AcpVtable k_acp_vtable = {acp_connect,     acp_disconnect,        acp_submit_command,
                                       acp_pop_message, acp_get_message_count, acp_release};

static HRESULT WINAPI AcpHalCreate(Acp** out) {
    if (!out)
        return E_INVALIDARG;
    QueryPerformanceFrequency(&g_qpc_frequency);
    Acp* acp = (Acp*)calloc(1, sizeof(Acp));
    if (!acp) {
        *out = nullptr;
        return E_OUTOFMEMORY;
    }
    acp->vtable = &k_acp_vtable;
    InitializeSRWLock(&acp->lock);
    *out = acp;
    return S_OK;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(AcpHalAllocateShapeContexts),
    HLE_EXPORT(AcpHalCreate),
    HLE_EXPORT(AcpHalReleaseShapeContexts),
    HLE_EXPORT(ApuAlloc),
    HLE_EXPORT(ApuCreateHeap),
    HLE_EXPORT(ApuFree),
    HLE_EXPORT(ApuIsVirtualAddressValid),
    HLE_EXPORT(ApuMapApuAddress),
    HLE_EXPORT(ApuMapVirtualAddress),
};

extern const HleModule g_hle_module_acphal = {"acphal.dll", k_exports, ARRAY_COUNT(k_exports)};
