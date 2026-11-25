// xapu.dll: the audio hardware's stream decoder (Opus) and sample rate converter.
//
// Titles connect a client for one kind of processing, then queue commands per stream (activate,
// process a packet or a block of PCM, reset, deactivate) and collect one result per command.
//
// Commands complete at once and results report what the hardware would have produced (Opus packet
// durations come from the packet's header), but the output is silence until decoding and conversion
// exist.

#include <windows.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "hle/hle.h"

#define XAPU_E_QUEUE_FULL      MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1000)
#define XAPU_E_PENDING_RESULTS MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1001)
#define XAPU_E_INVALID_COMMAND MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1002)
#define XAPU_E_INVALID_STREAM  MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1003)

enum XApuProcess : u32 { PROCESS_DECODE_OPUS = 1, PROCESS_DECODE_CONVERT_OPUS = 2, PROCESS_CONVERT_PCM = 3 };
enum XApuCommand : u32 {
    COMMAND_ACTIVATE = 1,
    COMMAND_DEACTIVATE = 2,
    COMMAND_PROCESS = 3,
    COMMAND_RESET = 5
};

#define OPTION_SIGNAL_ON_COMPLETION 0x1u
#define CONNECT_RESULT_READY_EVENT  0x8u
#define STATE_OUTPUT_READY          0x1u
#define STATE_NEED_MORE_INPUT       0x2u

#define SAMPLE_RATE 48000u

#pragma pack(push, 1)
struct ConnectInput {
    u32 process;
    u32 max_streams;
    u32 max_queued_per_stream;
    u32 base_data_length;
    u32 max_frames_per_output;
    u32 options;
};
struct ConnectOutput {
    void* base_data;
    HANDLE result_ready;
};
struct CommandId {
    u32 type;
    u32 stream;
    u32 sequence;
    u32 options;
    void* context;
};
struct ActivateCommand {
    CommandId id;
    u32 channels;
    u32 initialize_data_length;
    void* initialize_data;
    f32 start_pitch;
    u32 processing_buffer_length;
    void* processing_buffer;
};
struct ProcessCommand {
    CommandId id;
    f32 pitch_ramp_rate;
    f32 target_pitch;
    u32 first_frame;
    u32 frame_count;
    u32 input_length;
    void* input;
    u32 max_output_length;
    void* output;
};
struct Result {
    CommandId id;
    u32 state;
    u64 state_data;
    u64 execution_time_us;
    HRESULT error;
    u32 output_frames;
    u32 output_length;
    void* output;
    f32 pitch;
    u32 input_length;
    void* input;
};
#pragma pack(pop)
static_assert(sizeof(ConnectInput) == 24 && sizeof(ConnectOutput) == 16 && sizeof(CommandId) == 24 &&
                  sizeof(ActivateCommand) == 56 && sizeof(ProcessCommand) == 64 && sizeof(Result) == 80,
              "XApu layouts");

struct XApuStream {
    u32 channels; // 0: not active
    f32 pitch;
};

struct XApuClient {
    SRWLOCK lock;
    ConnectInput params;
    void* base_data;
    HANDLE result_ready;
    XApuStream* streams;
    Result* results; // ring
    u32 capacity;
    u32 head;
    u32 count;
};

// Samples per channel in an Opus packet at 48 kHz, from its table-of-contents byte (RFC 6716, 3.1).
static u32 opus_packet_frames(const u8* packet, u32 length) {
    if (!packet || !length)
        return 0;
    u32 config = packet[0] >> 3;
    u32 frame_tenths_ms; // frame duration in 0.1 ms
    if (config < 12) {
        static const u32 silk[4] = {100, 200, 400, 600};
        frame_tenths_ms = silk[config & 3];
    } else if (config < 16) {
        frame_tenths_ms = (config & 1) ? 200 : 100;
    } else {
        static const u32 celt[4] = {25, 50, 100, 200};
        frame_tenths_ms = celt[config & 3];
    }
    u32 frames;
    switch (packet[0] & 3) {
    case 0:
        frames = 1;
        break;
    case 1:
    case 2:
        frames = 2;
        break;
    default:
        frames = length > 1 ? packet[1] & 0x3F : 0;
        break;
    }
    return frames * frame_tenths_ms * (SAMPLE_RATE / 10000);
}

// What a command produces, minus the audio itself. Client lock held.
static Result run_command(XApuClient* c, const CommandId* id) {
    Result r = {};
    r.id = *id;
    r.error = S_OK;
    if (id->stream >= c->params.max_streams) {
        r.error = XAPU_E_INVALID_STREAM;
        return r;
    }
    XApuStream& s = c->streams[id->stream];
    switch (id->type) {
    case COMMAND_ACTIVATE: {
        const ActivateCommand* a = (const ActivateCommand*)id;
        s.channels = a->channels == 2 ? 2 : 1;
        s.pitch = a->start_pitch;
        break;
    }
    case COMMAND_DEACTIVATE:
        s.channels = 0;
        break;
    case COMMAND_RESET:
        break;
    case COMMAND_PROCESS: {
        const ProcessCommand* p = (const ProcessCommand*)id;
        u32 channels = s.channels ? s.channels : 1;
        u32 frames;
        if (c->params.process == PROCESS_DECODE_OPUS) {
            frames = opus_packet_frames((const u8*)p->input, p->input_length);
            frames = frames > p->first_frame ? frames - p->first_frame : 0;
            if (p->frame_count < frames)
                frames = p->frame_count;
        } else {
            frames = c->params.max_frames_per_output;
            s.pitch = p->target_pitch;
            r.state = STATE_OUTPUT_READY | STATE_NEED_MORE_INPUT;
        }
        u32 room = p->output ? p->max_output_length / (channels * sizeof(f32)) : 0;
        if (frames > room)
            frames = room;
        if (frames)
            memset(p->output, 0, frames * channels * sizeof(f32));
        r.output_frames = frames;
        r.output_length = frames * channels * sizeof(f32);
        r.output = p->output;
        r.pitch = s.pitch;
        r.input_length = p->input_length;
        r.input = p->input;
        break;
    }
    default:
        r.error = XAPU_E_INVALID_COMMAND;
        break;
    }
    return r;
}

static HRESULT WINAPI XApuConnect(const ConnectInput* params, ConnectOutput* out, XApuClient** handle) {
    if (!params || !out || !handle || params->process < PROCESS_DECODE_OPUS ||
        params->process > PROCESS_CONVERT_PCM || params->max_streams < 1 || params->max_streams > 500 ||
        params->max_queued_per_stream < 1 || params->max_queued_per_stream > 16)
        return E_INVALIDARG;
    XApuClient* c = (XApuClient*)calloc(1, sizeof(XApuClient));
    if (!c)
        return E_OUTOFMEMORY;
    InitializeSRWLock(&c->lock);
    c->params = *params;
    c->capacity = params->max_streams * params->max_queued_per_stream;
    c->streams = (XApuStream*)calloc(params->max_streams, sizeof(XApuStream));
    c->results = (Result*)calloc(c->capacity, sizeof(Result));
    c->base_data = params->base_data_length ? _aligned_malloc(params->base_data_length, 64) : nullptr;
    if (params->options & CONNECT_RESULT_READY_EVENT)
        c->result_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c->streams || !c->results || (params->base_data_length && !c->base_data) ||
        ((params->options & CONNECT_RESULT_READY_EVENT) && !c->result_ready)) {
        free(c->streams);
        free(c->results);
        _aligned_free(c->base_data);
        if (c->result_ready)
            CloseHandle(c->result_ready);
        free(c);
        return E_OUTOFMEMORY;
    }
    if (c->base_data)
        memset(c->base_data, 0, params->base_data_length);
    out->base_data = c->base_data;
    out->result_ready = c->result_ready;
    *handle = c;
    LOG_I("apu",
          "XApuConnect(process %u, %u streams, %u commands each, %u bytes); decoding is not "
          "implemented, output is silent",
          params->process, params->max_streams, params->max_queued_per_stream, params->base_data_length);
    return S_OK;
}

static HRESULT WINAPI XApuDisconnect(XApuClient* c) {
    if (!c)
        return E_INVALIDARG;
    free(c->streams);
    free(c->results);
    _aligned_free(c->base_data);
    if (c->result_ready)
        CloseHandle(c->result_ready);
    free(c);
    return S_OK;
}

static HRESULT WINAPI XApuEnqueueCommand(XApuClient* c, const CommandId* command, u32* total_count) {
    if (!c || !command)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&c->lock);
    HRESULT hr = S_OK;
    if (c->count == c->capacity) {
        hr = XAPU_E_QUEUE_FULL;
    } else {
        c->results[(c->head + c->count) % c->capacity] = run_command(c, command);
        c->count++;
    }
    if (total_count)
        *total_count = c->count;
    ReleaseSRWLockExclusive(&c->lock);
    if (SUCCEEDED(hr) && c->result_ready && (command->options & OPTION_SIGNAL_ON_COMPLETION))
        SetEvent(c->result_ready);
    return hr;
}

static HRESULT WINAPI XApuDequeueResult(XApuClient* c, Result* result) {
    if (!c || !result)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&c->lock);
    bool any = c->count != 0;
    if (any) {
        *result = c->results[c->head];
        c->head = (c->head + 1) % c->capacity;
        c->count--;
    }
    ReleaseSRWLockExclusive(&c->lock);
    return any ? S_OK : XAPU_E_PENDING_RESULTS;
}

static u32 WINAPI XApuGetResultCount(XApuClient* c, u32* total_count) {
    if (!c)
        return 0;
    u32 n = c->count;
    if (total_count)
        *total_count = n;
    return n;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(XApuConnect),       HLE_EXPORT(XApuDisconnect),     HLE_EXPORT(XApuEnqueueCommand),
    HLE_EXPORT(XApuDequeueResult), HLE_EXPORT(XApuGetResultCount),
};

extern const HleModule g_hle_module_xapu = {"xapu.dll", k_exports, ARRAY_COUNT(k_exports)};
