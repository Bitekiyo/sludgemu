// xapu.dll: the audio hardware's stream decoder (Opus) and sample rate converter.
//
// Titles connect a client for one kind of processing, then queue commands per stream (activate,
// process a packet or a block of PCM, reset, deactivate) and collect one result per command.
//
// Opus packets are decoded with libopus. Sample rate conversion (the convert process types) runs
// each stream's frames through a FIFO and a linear interpolator that follows the requested pitch,
// producing output in blocks of the client's output size, as the hardware does. Commands complete
// as they are queued.

#include <windows.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#include "common/log.h"
#include "hle/hle.h"

#define XAPU_E_QUEUE_FULL       MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1000)
#define XAPU_E_PENDING_RESULTS  MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1001)
#define XAPU_E_INVALID_COMMAND  MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1002)
#define XAPU_E_INVALID_STREAM   MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1003)
#define XAPU_E_BUFFER_TOO_SMALL MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1006)
#define XAPU_E_BAD_OPUS_PACKET  MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x1008)

enum XApuProcess : u32 { PROCESS_DECODE_OPUS = 1, PROCESS_DECODE_CONVERT_OPUS = 2, PROCESS_CONVERT_PCM = 3 };
enum XApuCommand : u32 {
    COMMAND_ACTIVATE = 1,
    COMMAND_DEACTIVATE = 2,
    COMMAND_PROCESS = 3,
    COMMAND_RESET = 5,
};

#define OPTION_SIGNAL_ON_COMPLETION  0x1u
#define OPTION_FLUSH                 0x2u
#define OPTION_RESET_DECODER         0x4u
#define CONNECT_DEINTERLEAVED_INPUT  0x2u
#define CONNECT_DEINTERLEAVED_OUTPUT 0x4u
#define CONNECT_RESULT_READY_EVENT   0x8u
#define STATE_OUTPUT_READY           0x1u
#define STATE_NEED_MORE_INPUT        0x2u
#define STATE_MORE_OUTPUT_AVAILABLE  0x4u

#define SAMPLE_RATE     48000u
#define MAX_OPUS_FRAMES 5760u // 120 ms at 48 kHz, the longest packet
#define MAX_CHANNELS    2u

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
    OpusDecoder* opus;
    f32 pitch;
    // Converter input: interleaved frames waiting to be resampled, and the read position in them.
    f32* fifo;
    u32 fifo_frames;
    u32 fifo_capacity;
    f64 position;
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
    f32 scratch[MAX_OPUS_FRAMES * MAX_CHANNELS]; // decoded frames, interleaved
};

static void release_stream(XApuStream* s) {
    if (s->opus)
        opus_decoder_destroy(s->opus);
    free(s->fifo);
    memset(s, 0, sizeof(*s));
}

// Decodes one packet into the client's scratch buffer; returns the frames decoded, or -1. Hardware
// takes input lengths rounded up to 16 bytes, so a packet that fails as given is retried without
// trailing zero bytes.
static int decode_packet(XApuClient* c, XApuStream* s, const u8* packet, u32 length) {
    if (!packet || !length)
        return -1;
    int frames = opus_decode_float(s->opus, packet, (opus_int32)length, c->scratch, MAX_OPUS_FRAMES, 0);
    if (frames < 0) {
        u32 trimmed = length;
        while (trimmed > 1 && packet[trimmed - 1] == 0)
            trimmed--;
        if (trimmed != length)
            frames = opus_decode_float(s->opus, packet, (opus_int32)trimmed, c->scratch, MAX_OPUS_FRAMES, 0);
    }
    return frames;
}

// Writes `frames` interleaved frames to the title's buffer, deinterleaved if it asked for that.
static void write_frames(const XApuClient* c, u32 channels, const f32* src, u32 frames, f32* dst) {
    if (!(c->params.options & CONNECT_DEINTERLEAVED_OUTPUT) || channels == 1) {
        memcpy(dst, src, (usize)frames * channels * sizeof(f32));
        return;
    }
    for (u32 ch = 0; ch < channels; ch++)
        for (u32 f = 0; f < frames; f++)
            dst[ch * frames + f] = src[f * channels + ch];
}

// Appends interleaved frames to the stream's converter input.
static bool fifo_push(XApuStream* s, const f32* frames, u32 count) {
    if (s->fifo_frames + count > s->fifo_capacity) {
        u32 capacity = s->fifo_capacity ? s->fifo_capacity : 4096;
        while (capacity < s->fifo_frames + count)
            capacity *= 2;
        f32* grown = (f32*)realloc(s->fifo, (usize)capacity * s->channels * sizeof(f32));
        if (!grown)
            return false;
        s->fifo = grown;
        s->fifo_capacity = capacity;
    }
    memcpy(s->fifo + (usize)s->fifo_frames * s->channels, frames, (usize)count * s->channels * sizeof(f32));
    s->fifo_frames += count;
    return true;
}

// Input frames the converter needs for `out` more output frames at the current pitch.
static f64 frames_needed(const XApuStream* s, u32 out) {
    return s->position + (f64)s->pitch * out + 1.0;
}

// Resamples up to `out` frames into `dst` (interleaved), moving the pitch toward `target` by
// `ramp` per frame. Stops early when input runs out, unless `flush` lets it hold the last frame.
static u32 convert(XApuStream* s, f32* dst, u32 out, f32 target, f32 ramp, bool flush) {
    u32 ch = s->channels;
    u32 produced = 0;
    while (produced < out) {
        u32 i = (u32)s->position;
        if (i + 1 >= s->fifo_frames && !(flush && i < s->fifo_frames))
            break;
        f32 frac = (f32)(s->position - i);
        const f32* a = s->fifo + (usize)i * ch;
        const f32* b = i + 1 < s->fifo_frames ? a + ch : a;
        for (u32 c = 0; c < ch; c++)
            dst[(usize)produced * ch + c] = a[c] + (b[c] - a[c]) * frac;
        produced++;
        s->position += s->pitch;
        if (s->pitch < target)
            s->pitch = s->pitch + ramp < target ? s->pitch + ramp : target;
        else if (s->pitch > target)
            s->pitch = s->pitch - ramp > target ? s->pitch - ramp : target;
    }
    // Drop the frames behind the read position.
    u32 consumed = (u32)s->position < s->fifo_frames ? (u32)s->position : s->fifo_frames;
    memmove(s->fifo, s->fifo + (usize)consumed * ch, (usize)(s->fifo_frames - consumed) * ch * sizeof(f32));
    s->fifo_frames -= consumed;
    s->position -= consumed;
    return produced;
}

static void process(XApuClient* c, XApuStream* s, const ProcessCommand* p, Result* r) {
    u32 ch = s->channels;
    r->input_length = p->input_length;
    r->input = p->input;
    r->output = p->output;

    // The frames this command contributes: a decoded packet or the title's PCM, minus the frames
    // before first_frame and past frame_count.
    const f32* frames = nullptr;
    u32 count = 0;
    if (c->params.process == PROCESS_CONVERT_PCM) {
        count = p->frame_count != 0xFFFFFFFFu ? p->frame_count : p->input_length / (ch * (u32)sizeof(f32));
        count = count < MAX_OPUS_FRAMES ? count : MAX_OPUS_FRAMES;
        const f32* in = (const f32*)p->input;
        if (in && (c->params.options & CONNECT_DEINTERLEAVED_INPUT) && ch > 1) {
            for (u32 f = 0; f < count; f++)
                for (u32 k = 0; k < ch; k++)
                    c->scratch[f * ch + k] = in[k * count + f];
            frames = c->scratch;
        } else {
            frames = in;
        }
        if (!frames)
            count = 0;
    } else if (p->input && p->input_length) {
        int decoded = decode_packet(c, s, (const u8*)p->input, p->input_length);
        if (decoded < 0) {
            r->error = XAPU_E_BAD_OPUS_PACKET;
            return;
        }
        u32 first = p->first_frame < (u32)decoded ? p->first_frame : (u32)decoded;
        frames = c->scratch + (usize)first * ch;
        count = (u32)decoded - first;
        if (p->frame_count < count)
            count = p->frame_count;
    }

    if (c->params.process == PROCESS_DECODE_OPUS) {
        u32 room = p->output ? p->max_output_length / (ch * (u32)sizeof(f32)) : 0;
        if (count > room) {
            r->error = XAPU_E_BUFFER_TOO_SMALL;
            return;
        }
        if (count)
            write_frames(c, ch, frames, count, (f32*)p->output);
        r->output_frames = count;
        r->output_length = count * ch * (u32)sizeof(f32);
        return;
    }

    // Converting: queue the frames, then produce one output block if there is input for it.
    if (count && !fifo_push(s, frames, count)) {
        r->error = E_OUTOFMEMORY;
        return;
    }
    u32 block = c->params.max_frames_per_output;
    u32 room = p->output ? p->max_output_length / (ch * (u32)sizeof(f32)) : 0;
    block = block < room ? block : room;
    bool flush = (p->id.options & OPTION_FLUSH) != 0;
    u32 produced = 0;
    if (block && (flush || frames_needed(s, block) <= s->fifo_frames)) {
        produced = convert(s, c->scratch, block, p->target_pitch, p->pitch_ramp_rate, flush);
        write_frames(c, ch, c->scratch, produced, (f32*)p->output);
    }
    r->output_frames = produced;
    r->output_length = produced * ch * (u32)sizeof(f32);
    r->pitch = s->pitch;
    r->state = (produced ? STATE_OUTPUT_READY : 0) |
               (block && frames_needed(s, block) <= s->fifo_frames ? STATE_MORE_OUTPUT_AVAILABLE
                                                                   : STATE_NEED_MORE_INPUT);
}

// Runs a command. Client lock held.
static Result run_command(XApuClient* c, const CommandId* id) {
    Result r = {};
    r.id = *id;
    r.error = S_OK;
    if (id->stream >= c->params.max_streams) {
        r.error = XAPU_E_INVALID_STREAM;
        return r;
    }
    XApuStream* s = &c->streams[id->stream];
    if (id->type != COMMAND_ACTIVATE && !s->channels) {
        r.error = XAPU_E_INVALID_COMMAND;
        return r;
    }
    switch (id->type) {
    case COMMAND_ACTIVATE: {
        const ActivateCommand* a = (const ActivateCommand*)id;
        release_stream(s);
        s->channels = a->channels == 2 ? 2 : 1;
        s->pitch = a->start_pitch > 0.0f ? a->start_pitch : 1.0f;
        if (c->params.process != PROCESS_CONVERT_PCM) {
            int err = 0;
            s->opus = opus_decoder_create(SAMPLE_RATE, (int)s->channels, &err);
            if (!s->opus) {
                LOG_E("apu", "cannot create an Opus decoder (%d)", err);
                s->channels = 0;
                r.error = E_OUTOFMEMORY;
            }
        }
        break;
    }
    case COMMAND_DEACTIVATE:
        release_stream(s);
        break;
    case COMMAND_RESET:
        if (s->opus)
            opus_decoder_ctl(s->opus, OPUS_RESET_STATE);
        if (!(id->options & OPTION_RESET_DECODER) || c->params.process != PROCESS_DECODE_CONVERT_OPUS) {
            s->fifo_frames = 0;
            s->position = 0.0;
        }
        break;
    case COMMAND_PROCESS:
        process(c, s, (const ProcessCommand*)id, &r);
        break;
    default:
        r.error = XAPU_E_INVALID_COMMAND;
        break;
    }
    return r;
}

static void free_client(XApuClient* c) {
    for (u32 i = 0; c->streams && i < c->params.max_streams; i++)
        release_stream(&c->streams[i]);
    free(c->streams);
    free(c->results);
    _aligned_free(c->base_data);
    if (c->result_ready)
        CloseHandle(c->result_ready);
    free(c);
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
        free_client(c);
        return E_OUTOFMEMORY;
    }
    if (c->base_data)
        memset(c->base_data, 0, params->base_data_length);
    out->base_data = c->base_data;
    out->result_ready = c->result_ready;
    *handle = c;
    LOG_I("apu", "XApuConnect(process %u, %u streams, %u commands each, %u bytes, options 0x%x)",
          params->process, params->max_streams, params->max_queued_per_stream, params->base_data_length,
          params->options);
    return S_OK;
}

static HRESULT WINAPI XApuDisconnect(XApuClient* c) {
    if (!c)
        return E_INVALIDARG;
    free_client(c);
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
