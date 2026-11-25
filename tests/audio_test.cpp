// Tests for the console audio decoder (XApu): Opus packets made with libopus's encoder go through
// the module's exports as a title would send them.

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <opus.h>

#include "hle/hle.h"
#include "test.h"

extern const HleModule g_hle_module_xapu;

namespace {

#pragma pack(push, 1)
struct ConnectInput {
    u32 process, max_streams, max_queued_per_stream, base_data_length, max_frames_per_output, options;
};
struct ConnectOutput {
    void* base_data;
    HANDLE result_ready;
};
struct CommandId {
    u32 type, stream, sequence, options;
    void* context;
};
struct ActivateCommand {
    CommandId id;
    u32 channels, initialize_data_length;
    void* initialize_data;
    f32 start_pitch;
    u32 processing_buffer_length;
    void* processing_buffer;
};
struct ProcessCommand {
    CommandId id;
    f32 pitch_ramp_rate, target_pitch;
    u32 first_frame, frame_count, input_length;
    void* input;
    u32 max_output_length;
    void* output;
};
struct Result {
    CommandId id;
    u32 state;
    u64 state_data, execution_time_us;
    HRESULT error;
    u32 output_frames, output_length;
    void* output;
    f32 pitch;
    u32 input_length;
    void* input;
};
#pragma pack(pop)

typedef HRESULT(WINAPI* ConnectFn)(const ConnectInput*, ConnectOutput*, void**);
typedef HRESULT(WINAPI* DisconnectFn)(void*);
typedef HRESULT(WINAPI* EnqueueFn)(void*, const CommandId*, u32*);
typedef HRESULT(WINAPI* DequeueFn)(void*, Result*);

void* xapu(const char* name) {
    for (u32 i = 0; i < g_hle_module_xapu.export_count; i++)
        if (!strcmp(g_hle_module_xapu.exports[i].name, name))
            return g_hle_module_xapu.exports[i].fn;
    return nullptr;
}

const u32 kFrames = 960; // 20 ms at 48 kHz

// One 20 ms stereo packet of a 440 Hz tone, continuing from `phase`.
u32 encode_tone(OpusEncoder* enc, u32* phase, u8* packet, u32 capacity) {
    f32 pcm[kFrames * 2];
    for (u32 i = 0; i < kFrames; i++, (*phase)++) {
        f32 v = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (f32)*phase / 48000.0f);
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = v;
    }
    int n = opus_encode_float(enc, pcm, kFrames, packet, (opus_int32)capacity);
    return n > 0 ? (u32)n : 0;
}

f32 rms(const f32* s, u32 n) {
    f64 sum = 0;
    for (u32 i = 0; i < n; i++)
        sum += (f64)s[i] * s[i];
    return n ? (f32)sqrt(sum / n) : 0.0f;
}

} // namespace

void test_xapu_decode() {
    ConnectFn connect = (ConnectFn)xapu("XApuConnect");
    DisconnectFn disconnect = (DisconnectFn)xapu("XApuDisconnect");
    EnqueueFn enqueue = (EnqueueFn)xapu("XApuEnqueueCommand");
    DequeueFn dequeue = (DequeueFn)xapu("XApuDequeueResult");
    CHECK(connect && disconnect && enqueue && dequeue);
    if (!connect || !disconnect || !enqueue || !dequeue)
        return;

    ConnectInput in = {1 /* decode Opus */, 1, 2, 64 * 1024, 0, 0};
    ConnectOutput out = {};
    void* client = nullptr;
    CHECK(SUCCEEDED(connect(&in, &out, &client)) && out.base_data);

    ActivateCommand activate = {};
    activate.id.type = 1;
    activate.channels = 2;
    Result r = {};
    CHECK(SUCCEEDED(enqueue(client, &activate.id, nullptr)));
    CHECK(SUCCEEDED(dequeue(client, &r)) && r.error == S_OK);
    CHECK(FAILED(dequeue(client, &r))); // nothing pending

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    CHECK(enc != nullptr);
    u8* packet = (u8*)out.base_data;
    f32* pcm = (f32*)((u8*)out.base_data + 4096);
    u32 phase = 0;
    f32 level = 0.0f;
    for (u32 p = 0; p < 5; p++) {
        ProcessCommand cmd = {};
        cmd.id.type = 3;
        cmd.id.sequence = p;
        cmd.frame_count = 0xFFFFFFFFu;
        cmd.input_length = encode_tone(enc, &phase, packet, 4000);
        cmd.input = packet;
        cmd.output = pcm;
        cmd.max_output_length = kFrames * 2 * sizeof(f32);
        CHECK(cmd.input_length > 0);
        CHECK(SUCCEEDED(enqueue(client, &cmd.id, nullptr)));
        CHECK(SUCCEEDED(dequeue(client, &r)));
        CHECK(r.error == S_OK && r.id.sequence == p);
        CHECK(r.output_frames == kFrames && r.output_length == kFrames * 2 * sizeof(f32));
        level = rms(pcm, kFrames * 2);
    }
    // A 0.5 amplitude sine has an RMS of about 0.35; the codec gets close once past its delay.
    CHECK(level > 0.25f && level < 0.45f);

    // Frame selection: skip the first 100 frames and keep at most 500.
    ProcessCommand cmd = {};
    cmd.id.type = 3;
    cmd.first_frame = 100;
    cmd.frame_count = 500;
    cmd.input_length = encode_tone(enc, &phase, packet, 4000);
    cmd.input = packet;
    cmd.output = pcm;
    cmd.max_output_length = kFrames * 2 * sizeof(f32);
    CHECK(SUCCEEDED(enqueue(client, &cmd.id, nullptr)));
    CHECK(SUCCEEDED(dequeue(client, &r)) && r.error == S_OK && r.output_frames == 500);

    // Garbage is reported as a bad packet, not decoded.
    memset(packet, 0xFF, 64);
    cmd.first_frame = 0;
    cmd.frame_count = 0xFFFFFFFFu;
    cmd.input_length = 64;
    CHECK(SUCCEEDED(enqueue(client, &cmd.id, nullptr)));
    CHECK(SUCCEEDED(dequeue(client, &r)) && FAILED(r.error));

    opus_encoder_destroy(enc);
    CHECK(SUCCEEDED(disconnect(client)));
}

// Converting at half pitch: each output block takes half as many input frames, and output comes in
// blocks of the client's size once there is input for one.
void test_xapu_convert() {
    ConnectFn connect = (ConnectFn)xapu("XApuConnect");
    DisconnectFn disconnect = (DisconnectFn)xapu("XApuDisconnect");
    EnqueueFn enqueue = (EnqueueFn)xapu("XApuEnqueueCommand");
    DequeueFn dequeue = (DequeueFn)xapu("XApuDequeueResult");
    if (!connect || !disconnect || !enqueue || !dequeue)
        return;
    ConnectInput in = {3 /* convert PCM */, 1, 4, 256 * 1024, 512, 0};
    ConnectOutput out = {};
    void* client = nullptr;
    CHECK(SUCCEEDED(connect(&in, &out, &client)));
    ActivateCommand activate = {};
    activate.id.type = 1;
    activate.channels = 1;
    activate.start_pitch = 0.5f;
    Result r = {};
    CHECK(SUCCEEDED(enqueue(client, &activate.id, nullptr)) && SUCCEEDED(dequeue(client, &r)));

    f32* input = (f32*)out.base_data;
    f32* output = input + 4096;
    for (u32 i = 0; i < 160; i++)
        input[i] = (f32)i; // a ramp, so interpolation is easy to check
    ProcessCommand cmd = {};
    cmd.id.type = 3;
    cmd.target_pitch = 0.5f;
    cmd.frame_count = 160;
    cmd.input_length = 160 * sizeof(f32);
    cmd.input = input;
    cmd.output = output;
    cmd.max_output_length = 512 * sizeof(f32);
    // 160 input frames at half pitch can't fill a 512-frame block yet.
    CHECK(SUCCEEDED(enqueue(client, &cmd.id, nullptr)) && SUCCEEDED(dequeue(client, &r)));
    CHECK(r.output_frames == 0 && (r.state & 2 /* need more input */));
    for (u32 i = 0; i < 160; i++)
        input[i] = (f32)(160 + i);
    // 320 frames cover 512 output frames at half pitch.
    CHECK(SUCCEEDED(enqueue(client, &cmd.id, nullptr)) && SUCCEEDED(dequeue(client, &r)));
    CHECK(r.output_frames == 512 && (r.state & 1 /* output ready */) && (r.state & 2 /* need more input */));
    CHECK(fabsf(output[0] - 0.0f) < 1e-4f && fabsf(output[1] - 0.5f) < 1e-4f &&
          fabsf(output[511] - 255.5f) < 1e-3f);
    CHECK(SUCCEEDED(disconnect(client)));
}
