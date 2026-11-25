// xcrdapi.dll: console DirectStorage, backed by desktop DirectStorage.
//
// The console API is the ancestor of the desktop one: queues of requests, status array writes,
// fence signals and event sets, with priorities and tag-based cancellation. Each console object
// wraps its desktop counterpart and requests are translated as they are enqueued.
//
// The console's hardware decoders (zlib, BCPack) and destination swizzling have no desktop
// equivalent. Requests that need them are enqueued with a custom compression format encoding the
// operations, and DirectStorage hands them to our decoder threads through its custom
// decompression queue. Their failures therefore surface through DirectStorage's own status
// arrays and error records, in order, like any other failure.

#include <windows.h>
#include <dstorage.h>
#include <dstorageerr.h>
#include <libdeflate.h>

#include "common/log.h"
#include "core/memory.h"
#include "core/vfs.h"
#include "hle/hle.h"

// --- console ABI ---------------------------------------------------------------------------------

#define CONSOLE_PAGE_SIZE 0x10000ull // physical page arrays list 64 KiB pages

// Request option bits.
#define OPT_ZLIB              (1ull << 0)
#define OPT_BCPACK_SHIFT      1
#define OPT_BCPACK_MASK       0xFull
#define OPT_SWIZZLE_SHIFT     5
#define OPT_SWIZZLE_MASK      0xFFFFull
#define OPT_DESTINATION_PAGES (1ull << 21)
#define OPT_SOURCE_MEMORY     (1ull << 22)
#define OPT_SOURCE_PAGES      (1ull << 23)

class ConsoleFile;
class ConsoleStatusArray;

struct ConsoleQueueDesc {
    u64 source_type;
    u16 capacity;
    s8 priority;
    const char* name;
};

struct ConsoleQueueDesc1 {
    ConsoleQueueDesc base;
    u64 options; // bit 0: auto-submit disabled
};

struct ConsoleQueueInfo {
    ConsoleQueueDesc desc;
    u16 empty_slot_count;
    u16 requests_until_auto_submit;
};

struct ConsoleQueueInfo1 {
    ConsoleQueueDesc1 desc;
    u16 empty_slot_count;
    u16 requests_until_auto_submit;
};

struct ConsoleRequest {
    u64 options;
    union {
        void* destination;
        struct {
            const ULONG_PTR* destination_pages;
            u16 destination_page_offset;
        };
    };
    u32 destination_size;
    union {
        struct {
            ConsoleFile* file;
            u64 file_offset;
        };
        const void* source;
        struct {
            const ULONG_PTR* source_pages;
            u16 source_page_offset;
        };
    };
    u32 source_size;
    u32 intermediate_size; // BCPack data size when both zlib and BCPack are requested
    u64 cancellation_tag;
    const char* name;
};
static_assert(sizeof(ConsoleRequest) == 72, "console request layout");

struct ConsoleErrorRequest {
    u64 options;
    void* destination;
    u32 destination_size;
    wchar_t filename[MAX_PATH];
    u64 file_offset;
    const void* source;
    u32 source_size;
    u64 cancellation_tag;
    char name[64];
};

struct ConsoleErrorRecord {
    u32 failure_count;
    struct {
        HRESULT hr;
        s32 command_type;
        union {
            ConsoleErrorRequest request;
            struct {
                ConsoleStatusArray* status_array;
                u32 index;
            } status;
            struct {
                ID3D12Fence* fence;
                u64 value;
            } signal;
            struct {
                HANDLE handle;
            } event;
        };
    } first_failure;
};
static_assert(sizeof(ConsoleErrorRecord) == 656, "console error record layout");

#define CONSOLE_STATS_SIZE 256

static const GUID IID_ConsoleFile = {
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
static const GUID IID_ConsoleFactory = {
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
static const GUID IID_ConsoleFactory1 = {
    0xc74c2a5b, 0x1e43, 0x46a5, {0xa2, 0x24, 0x91, 0x16, 0x35, 0x8e, 0x11, 0x18}};
static const GUID IID_ConsoleFactory2 = {
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
static const GUID IID_ConsoleStatusArray = {
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
static const GUID IID_ConsoleQueue = {
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
static const GUID IID_ConsoleQueue1 = {
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
static const GUID IID_ConsoleQueue2 = {
    0x3af5c948, 0x6297, 0x426c, {0xbf, 0xfc, 0x8f, 0x18, 0x95, 0x9f, 0xb8, 0x7b}};

// --- custom formats ------------------------------------------------------------------------------

// Custom compression format byte: DSTORAGE_CUSTOM_COMPRESSION_0 | operations.
#define FORMAT_ZLIB          0x01
#define FORMAT_BCPACK        0x02
#define FORMAT_SWIZZLE_SHIFT 2    // console swizzle mode (1-4) in bits 2-4
#define FORMAT_UNSUPPORTED   0xFF // the request can't be expressed; it fails with E_NOTIMPL

static u8 custom_format(u64 options) {
    u8 format = 0;
    if (options & OPT_ZLIB)
        format |= FORMAT_ZLIB;
    if ((options >> OPT_BCPACK_SHIFT) & OPT_BCPACK_MASK)
        format |= FORMAT_BCPACK;
    u64 swizzle = (options >> OPT_SWIZZLE_SHIFT) & OPT_SWIZZLE_MASK;
    if (swizzle > 7)
        return FORMAT_UNSUPPORTED;
    format |= (u8)(swizzle << FORMAT_SWIZZLE_SHIFT);
    return format ? (u8)(DSTORAGE_CUSTOM_COMPRESSION_0 | format) : (u8)DSTORAGE_COMPRESSION_FORMAT_NONE;
}

// --- request statistics --------------------------------------------------------------------------
// The title's request mix decides where decoding effort has to go; logged until it's known.

#define DETAILED_REQUEST_LOGS 32

static volatile long g_request_count = 0;
static volatile long g_zlib_count = 0;
static volatile long g_bcpack_count = 0;
static volatile long g_swizzle_count = 0;
static volatile long g_page_count = 0;
static volatile s64 g_request_bytes = 0;

static void count_request(const ConsoleRequest& r) {
    long n = InterlockedIncrement(&g_request_count);
    InterlockedAdd64(&g_request_bytes, r.destination_size);
    if (r.options & OPT_ZLIB)
        InterlockedIncrement(&g_zlib_count);
    if ((r.options >> OPT_BCPACK_SHIFT) & OPT_BCPACK_MASK)
        InterlockedIncrement(&g_bcpack_count);
    if ((r.options >> OPT_SWIZZLE_SHIFT) & OPT_SWIZZLE_MASK)
        InterlockedIncrement(&g_swizzle_count);
    if (r.options & (OPT_DESTINATION_PAGES | OPT_SOURCE_PAGES))
        InterlockedIncrement(&g_page_count);

    if (n <= DETAILED_REQUEST_LOGS) {
        LOG_I(
            "dstorage",
            "request #%ld: options=0x%llx source=%u bytes%s at 0x%llx, dest=%u bytes at %p, intermediate=%u "
            "name=%s",
            n, r.options, r.source_size, (r.options & OPT_SOURCE_MEMORY) ? " (memory)" : "",
            (r.options & OPT_SOURCE_MEMORY) ? (u64)r.source : r.file_offset, r.destination_size,
            r.destination, r.intermediate_size, r.name ? r.name : "");
    }
    if ((n & 4095) == 0) {
        LOG_I("dstorage", "%ld requests (%lld MiB): zlib %ld, bcpack %ld, swizzle %ld, physical pages %ld", n,
              g_request_bytes >> 20, g_zlib_count, g_bcpack_count, g_swizzle_count, g_page_count);
    }
}

// --- physical pages ------------------------------------------------------------------------------

// Pointer to a byte range described by an array of 64 KiB pages (by page number), if those pages
// follow each other in the linear view of physical memory.
static u8* contiguous_pages(const ULONG_PTR* pages, u16 offset, u32 size) {
    u64 count = (offset + (u64)size + CONSOLE_PAGE_SIZE - 1) / CONSOLE_PAGE_SIZE;
    u8* base = mem_physical_ptr(pages[0]);
    for (u64 i = 1; base && i < count; i++) {
        if (mem_physical_ptr(pages[i]) != base + i * CONSOLE_PAGE_SIZE)
            return nullptr;
    }
    return base ? base + offset : nullptr;
}

// --- objects -------------------------------------------------------------------------------------

static bool iid_in(REFIID iid, const GUID* const* list, u32 count) {
    if (iid == IID_IUnknown)
        return true;
    for (u32 i = 0; i < count; i++) {
        if (iid == *list[i])
            return true;
    }
    return false;
}

#define REFCOUNTED_IUNKNOWN(iid_list)                                                                        \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {                              \
        if (!iid_in(iid, iid_list, ARRAY_COUNT(iid_list))) {                                                 \
            *out = nullptr;                                                                                  \
            return E_NOINTERFACE;                                                                            \
        }                                                                                                    \
        AddRef();                                                                                            \
        *out = this;                                                                                         \
        return S_OK;                                                                                         \
    }                                                                                                        \
    ULONG STDMETHODCALLTYPE AddRef() override {                                                              \
        return (ULONG)InterlockedIncrement(&m_refs);                                                         \
    }                                                                                                        \
    ULONG STDMETHODCALLTYPE Release() override {                                                             \
        long refs = InterlockedDecrement(&m_refs);                                                           \
        if (refs == 0)                                                                                       \
            delete this;                                                                                     \
        return (ULONG)refs;                                                                                  \
    }

static const GUID* const k_file_iids[] = {&IID_ConsoleFile};

class ConsoleFile : public IUnknown {
  public:
    REFCOUNTED_IUNKNOWN(k_file_iids)

    virtual void Close() { m_desktop->Close(); }

    // Desktop DirectStorage doesn't expose its handle; titles get one of their own.
    virtual HANDLE GetHandle() {
        if (!m_handle) {
            HANDLE h = CreateFileW(m_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (InterlockedCompareExchangePointer(&m_handle, h, nullptr) != nullptr &&
                h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
        return m_handle;
    }

    ConsoleFile(IDStorageFile* desktop, const wchar_t* path) : m_desktop(desktop) {
        wcsncpy(m_path, path, MAX_PATH - 1);
    }
    ~ConsoleFile() {
        m_desktop->Release();
        if (m_handle && m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    IDStorageFile* m_desktop;
    wchar_t m_path[MAX_PATH] = {};
    void* volatile m_handle = nullptr;
    volatile long m_refs = 1;
};

static const GUID* const k_status_iids[] = {&IID_ConsoleStatusArray};

class ConsoleStatusArray : public IUnknown {
  public:
    REFCOUNTED_IUNKNOWN(k_status_iids)

    virtual bool IsComplete(UINT32 index) { return m_desktop->IsComplete(index); }
    virtual HRESULT GetHResult(UINT32 index) { return m_desktop->GetHResult(index); }

    explicit ConsoleStatusArray(IDStorageStatusArray* desktop) : m_desktop(desktop) {}
    ~ConsoleStatusArray() { m_desktop->Release(); }

    IDStorageStatusArray* m_desktop;
    volatile long m_refs = 1;
};

static const GUID* const k_queue_iids[] = {&IID_ConsoleQueue, &IID_ConsoleQueue1, &IID_ConsoleQueue2};

class ConsoleQueue : public IUnknown {
  public:
    REFCOUNTED_IUNKNOWN(k_queue_iids)

    virtual void EnqueueRequest(const ConsoleRequest* request);

    virtual void EnqueueStatus(ConsoleStatusArray* array, UINT32 index) {
        m_desktop->EnqueueStatus(array->m_desktop, index);
    }

    virtual void EnqueueSignal(ID3D12Fence* fence, UINT64 value) {
        // Needs the D3D12 layer, which will hand out the desktop fence behind a title fence.
        static volatile long logged = 0;
        if (!InterlockedExchange(&logged, 1))
            LOG_W("dstorage", "EnqueueSignal(%p, %llu): fences are not supported yet; signal dropped", fence,
                  value);
    }

    virtual void Submit() { m_desktop->Submit(); }
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) {
        m_desktop->CancelRequestsWithTag(mask, value);
    }
    virtual void Close() { m_desktop->Close(); }
    virtual HANDLE GetErrorEvent() { return m_desktop->GetErrorEvent(); }
    virtual void RetrieveErrorRecord(ConsoleErrorRecord* record);

    virtual void Query(ConsoleQueueInfo* info) {
        DSTORAGE_QUEUE_INFO d;
        m_desktop->Query(&d);
        info->desc = m_desc.base;
        info->empty_slot_count = d.EmptySlotCount;
        info->requests_until_auto_submit = d.RequestCountUntilAutoSubmit;
    }

    // IDStorageQueueX1
    virtual void EnqueueSetEvent(HANDLE handle) { m_desktop->EnqueueSetEvent(handle); }

    // IDStorageQueueX2
    virtual void Query1(ConsoleQueueInfo1* info) {
        DSTORAGE_QUEUE_INFO d;
        m_desktop->Query(&d);
        info->desc = m_desc;
        info->empty_slot_count = d.EmptySlotCount;
        info->requests_until_auto_submit = d.RequestCountUntilAutoSubmit;
    }

    ConsoleQueue(IDStorageQueue1* desktop, const ConsoleQueueDesc1& desc)
        : m_desktop(desktop), m_desc(desc) {}
    ~ConsoleQueue() { m_desktop->Release(); }

    IDStorageQueue1* m_desktop;
    ConsoleQueueDesc1 m_desc;
    volatile long m_refs = 1;
};

void ConsoleQueue::EnqueueRequest(const ConsoleRequest* r) {
    count_request(*r);

    DSTORAGE_REQUEST d = {};
    d.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)custom_format(r->options);
    d.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    d.UncompressedSize = r->destination_size;
    d.CancellationTag = r->cancellation_tag;
    d.Name = r->name;

    void* destination = r->destination;
    if (r->options & OPT_DESTINATION_PAGES)
        destination = contiguous_pages(r->destination_pages, r->destination_page_offset, r->destination_size);
    // DirectStorage may write and read title memory from the kernel, which doesn't fault pages in.
    if (destination && !(r->options & OPT_DESTINATION_PAGES))
        mem_prepare(destination, r->destination_size);
    d.Destination.Memory.Buffer = destination;
    d.Destination.Memory.Size = r->destination_size;

    const void* source = nullptr;
    if (r->options & OPT_SOURCE_MEMORY) {
        d.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        source = (r->options & OPT_SOURCE_PAGES)
                     ? contiguous_pages(r->source_pages, r->source_page_offset, r->source_size)
                     : r->source;
        if (source && !(r->options & OPT_SOURCE_PAGES))
            mem_prepare(source, r->source_size);
        d.Source.Memory.Source = source;
        d.Source.Memory.Size = r->source_size;
    } else {
        d.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        d.Source.File.Source = r->file->m_desktop;
        d.Source.File.Offset = r->file_offset;
        d.Source.File.Size = r->source_size;
    }

    if (!destination || ((r->options & OPT_SOURCE_MEMORY) && !source)) {
        // Scattered physical pages: not expressible as one desktop request yet. Keep the request
        // in the queue (so ordering and status reporting stay intact) but make it fail.
        LOG_E("dstorage", "request %s: non-contiguous physical pages are not supported yet",
              r->name ? r->name : "");
        static u8 dummy[1];
        d.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)FORMAT_UNSUPPORTED;
        d.Destination.Memory.Buffer = dummy;
        d.Destination.Memory.Size = 1;
        d.UncompressedSize = 1;
        if (d.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
            d.Source.Memory.Source = dummy;
            d.Source.Memory.Size = 1;
        } else {
            d.Source.File.Size = 1;
        }
    }
    m_desktop->EnqueueRequest(&d);
}

void ConsoleQueue::RetrieveErrorRecord(ConsoleErrorRecord* record) {
    DSTORAGE_ERROR_RECORD d;
    m_desktop->RetrieveErrorRecord(&d);
    memset(record, 0, sizeof(*record));
    record->failure_count = d.FailureCount;
    record->first_failure.hr = d.FirstFailure.HResult;
    record->first_failure.command_type = d.FirstFailure.CommandType;
    switch (d.FirstFailure.CommandType) {
    case DSTORAGE_COMMAND_TYPE_REQUEST: {
        const DSTORAGE_REQUEST& req = d.FirstFailure.Request.Request;
        ConsoleErrorRequest& out = record->first_failure.request;
        out.destination = req.Destination.Memory.Buffer;
        out.destination_size = req.Destination.Memory.Size;
        memcpy(out.filename, d.FirstFailure.Request.Filename, sizeof(out.filename));
        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            out.file_offset = req.Source.File.Offset;
            out.source_size = req.Source.File.Size;
        } else {
            out.options |= OPT_SOURCE_MEMORY;
            out.source = req.Source.Memory.Source;
            out.source_size = req.Source.Memory.Size;
        }
        out.cancellation_tag = req.CancellationTag;
        memcpy(out.name, d.FirstFailure.Request.RequestName, sizeof(out.name));
        LOG_W("dstorage", "request failed (0x%08lx): %s", (unsigned long)d.FirstFailure.HResult, out.name);
        break;
    }
    case DSTORAGE_COMMAND_TYPE_STATUS:
        // The desktop status array can't be mapped back to the title's object; the index can.
        record->first_failure.status.index = d.FirstFailure.Status.Index;
        break;
    case DSTORAGE_COMMAND_TYPE_SIGNAL:
        record->first_failure.signal.fence = d.FirstFailure.Signal.Fence;
        record->first_failure.signal.value = d.FirstFailure.Signal.Value;
        break;
    case DSTORAGE_COMMAND_TYPE_EVENT:
        record->first_failure.event.handle = d.FirstFailure.Event.Handle;
        break;
    default:
        break;
    }
}

// --- decoding ------------------------------------------------------------------------------------

static IDStorageCustomDecompressionQueue* g_decompression_queue = nullptr;

static HRESULT inflate(libdeflate_decompressor* d, const void* src, u64 src_size, void* dst, u64 dst_size) {
    const u8* s = (const u8*)src;
    bool zlib_header = src_size >= 2 && (s[0] & 0x0F) == 8 && ((s[0] << 8) | s[1]) % 31 == 0;
    size_t actual = 0;
    libdeflate_result r = zlib_header
                              ? libdeflate_zlib_decompress(d, src, src_size, dst, dst_size, &actual)
                              : libdeflate_deflate_decompress(d, src, src_size, dst, dst_size, &actual);
    if (r != LIBDEFLATE_SUCCESS || actual != dst_size) {
        LOG_W("dstorage", "zlib decode failed: result %d, %zu of %llu bytes", (int)r, actual, dst_size);
        return E_DSTORAGE_DECOMPRESSION_ERROR;
    }
    return S_OK;
}

static HRESULT decode(libdeflate_decompressor* d, const DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST& req) {
    u8 format = (u8)req.CompressionFormat;
    if (format == FORMAT_UNSUPPORTED)
        return E_NOTIMPL;
    if (format & (FORMAT_BCPACK | (7 << FORMAT_SWIZZLE_SHIFT))) {
        static volatile long logged = 0;
        if (InterlockedIncrement(&logged) <= 8)
            LOG_E("dstorage", "custom format 0x%02x (BCPack/swizzle) is not implemented yet", format);
        return E_NOTIMPL;
    }
    if (format & FORMAT_ZLIB)
        return inflate(d, req.SrcBuffer, req.SrcSize, req.DstBuffer, req.DstSize);
    return E_NOTIMPL;
}

static DWORD WINAPI decoder_thread(void*) {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    HANDLE event = g_decompression_queue->GetEvent();
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST requests[16];
    DSTORAGE_CUSTOM_DECOMPRESSION_RESULT results[16];
    for (;;) {
        WaitForSingleObject(event, INFINITE);
        for (;;) {
            UINT32 count = 0;
            if (FAILED(g_decompression_queue->GetRequests(ARRAY_COUNT(requests), requests, &count)) ||
                count == 0)
                break;
            for (UINT32 i = 0; i < count; i++) {
                results[i].Id = requests[i].Id;
                results[i].Result = decode(d, requests[i]);
            }
            g_decompression_queue->SetRequestResults(count, results);
        }
    }
}

// --- factory -------------------------------------------------------------------------------------

static IDStorageFactory* g_desktop_factory = nullptr;
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

static const GUID* const k_factory_iids[] = {&IID_ConsoleFactory, &IID_ConsoleFactory1, &IID_ConsoleFactory2};

// Process-wide singleton; reference counting is a no-op.
class ConsoleFactory : public IUnknown {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!iid_in(iid, k_factory_iids, ARRAY_COUNT(k_factory_iids))) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    virtual HRESULT CreateQueue(const ConsoleQueueDesc* desc, REFIID iid, void** out) {
        ConsoleQueueDesc1 desc1 = {*desc, 0};
        return CreateQueue1(&desc1, iid, out);
    }

    virtual HRESULT OpenFile(const wchar_t* path, REFIID iid, void** out) {
        *out = nullptr;
        wchar_t host_path[MAX_PATH * 2];
        if (!vfs_translate(path, host_path, ARRAY_COUNT(host_path)))
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        IDStorageFile* desktop = nullptr;
        HRESULT hr = g_desktop_factory->OpenFile(host_path, IID_PPV_ARGS(&desktop));
        if (FAILED(hr)) {
            LOG_I("dstorage", "OpenFile(%ls) failed (0x%08lx)", host_path, (unsigned long)hr);
            return hr;
        }
        LOG_D("dstorage", "OpenFile(%ls)", host_path);
        ConsoleFile* file = new ConsoleFile(desktop, host_path);
        hr = file->QueryInterface(iid, out);
        file->Release();
        return hr;
    }

    virtual HRESULT CreateStatusArray(UINT32 capacity, const char* name, REFIID iid, void** out) {
        *out = nullptr;
        IDStorageStatusArray* desktop = nullptr;
        HRESULT hr = g_desktop_factory->CreateStatusArray(capacity, name, IID_PPV_ARGS(&desktop));
        if (FAILED(hr))
            return hr;
        ConsoleStatusArray* array = new ConsoleStatusArray(desktop);
        hr = array->QueryInterface(iid, out);
        array->Release();
        return hr;
    }

    // Console core affinity has no desktop meaning; the host schedules DirectStorage's threads.
    virtual HRESULT SetCpuAffinity(UINT64 affinity) { return S_OK; }
    virtual void SetDebugFlags(UINT32 flags) { g_desktop_factory->SetDebugFlags(flags); }
    virtual HRESULT SetStagingBufferSize(UINT32 size) {
        return g_desktop_factory->SetStagingBufferSize(size);
    }

    // IDStorageFactoryX1
    virtual void GetStats(void* stats) { memset(stats, 0, CONSOLE_STATS_SIZE); }

    // IDStorageFactoryX2
    virtual HRESULT CreateQueue1(const ConsoleQueueDesc1* desc, REFIID iid, void** out) {
        *out = nullptr;
        u16 capacity = desc->base.capacity;
        if (capacity < DSTORAGE_MIN_QUEUE_CAPACITY)
            capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        if (capacity > DSTORAGE_MAX_QUEUE_CAPACITY)
            capacity = DSTORAGE_MAX_QUEUE_CAPACITY;

        DSTORAGE_QUEUE_DESC d = {};
        d.SourceType = (DSTORAGE_REQUEST_SOURCE_TYPE)desc->base.source_type;
        d.Capacity = capacity;
        d.Priority = (DSTORAGE_PRIORITY)desc->base.priority;
        d.Name = desc->base.name;
        d.Device = nullptr; // memory destinations only: the console has unified memory

        IDStorageQueue1* desktop = nullptr;
        HRESULT hr = g_desktop_factory->CreateQueue(&d, IID_PPV_ARGS(&desktop));
        LOG_I("dstorage", "CreateQueue(%s, %s source, capacity %u, priority %d) -> 0x%08lx",
              desc->base.name ? desc->base.name : "", desc->base.source_type ? "memory" : "file", capacity,
              desc->base.priority, (unsigned long)hr);
        if (FAILED(hr))
            return hr;
        ConsoleQueue* queue = new ConsoleQueue(desktop, *desc);
        hr = queue->QueryInterface(iid, out);
        queue->Release();
        return hr;
    }
};

static ConsoleFactory g_factory;

static BOOL CALLBACK init_desktop(PINIT_ONCE, void*, void**) {
    DSTORAGE_CONFIGURATION config = {};
    config.DisableTelemetry = TRUE;
    config.NumBuiltInCpuDecompressionThreads = DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION; // no GDeflate data
    HRESULT hr = DStorageSetConfiguration(&config);
    if (SUCCEEDED(hr))
        hr = DStorageGetFactory(IID_PPV_ARGS(&g_desktop_factory));
    if (SUCCEEDED(hr))
        hr = g_desktop_factory->QueryInterface(IID_PPV_ARGS(&g_decompression_queue));
    if (FAILED(hr)) {
        LOG_E("dstorage", "desktop DirectStorage unavailable (0x%08lx)", (unsigned long)hr);
        return FALSE;
    }

    // Decoding stands in for the console's hardware decoders. Requests are independent, so they
    // spread over a few threads.
    u32 threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) / 4;
    if (threads < 2)
        threads = 2;
    if (threads > 8)
        threads = 8;
    for (u32 i = 0; i < threads; i++) {
        HANDLE t = CreateThread(nullptr, 0, decoder_thread, nullptr, 0, nullptr);
        if (!t)
            FATAL("cannot create DirectStorage decoder thread (%lu)", GetLastError());
        SetThreadDescription(t, L"sludgemu dstorage decoder");
        CloseHandle(t);
    }
    LOG_I("dstorage", "desktop DirectStorage ready, %u decoder threads", threads);
    return TRUE;
}

// --- exports -------------------------------------------------------------------------------------

static HRESULT WINAPI hle_DStorageGetFactory(REFIID iid, void** out) {
    if (!InitOnceExecuteOnce(&g_init_once, init_desktop, nullptr, nullptr)) {
        *out = nullptr;
        return E_FAIL;
    }
    return g_factory.QueryInterface(iid, out);
}

static const HleExport k_exports[] = {
    HLE_EXPORT_AS(hle_DStorageGetFactory, "DStorageGetFactory"),
};

extern const HleModule g_hle_module_xcrdapi = {"xcrdapi.dll", k_exports, ARRAY_COUNT(k_exports)};
