// chad.dll: the audio hardware's DSP (convolution and FFTs on blocks of samples).
//
// Titles activate streams on a client and submit one block per command; a status block per stream,
// which titles poll, tells which commands are done.
//
// Commands complete at once and statuses advance as on hardware, but the output blocks are silent
// until the processing exists.

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "hle/hle.h"

#pragma pack(push, 1)
struct DspActivation {
    u32 type; // convolution, forward FFT, inverse FFT
    u32 options;
    u32 block_frames;
    u32 channels;
    u32 impulse_response_floats;
    f32* impulse_response;
};
struct DspCommand {
    f32 begin_scale;
    f32 end_scale;
    f32* input;
    f32* output;
    f32* multiplier;
};
struct DspStatus {
    u32 last_submitted;
    u32 last_processed;
    u32 first_failed; // 0: none
    HRESULT result;
};
#pragma pack(pop)
static_assert(sizeof(DspActivation) == 28 && sizeof(DspCommand) == 32 && sizeof(DspStatus) == 16,
              "XDsp layouts");

#define DSP_TYPE_LAST 3

struct DspClient {
    u32 max_streams;
    volatile long active;
};

struct DspStream {
    DspClient* client;
    DspActivation params;
    DspStatus status; // the title reads it
    u32 sequence;
};

static HRESULT WINAPI XDspConnectWithMaximumStreamLimit(f32* base_buffer, u32 base_buffer_length,
                                                        u32 aggregate_impulse_response_seconds,
                                                        u32 max_streams, u32 max_channels,
                                                        DspClient** handle) {
    if (!handle)
        return E_INVALIDARG;
    DspClient* c = (DspClient*)calloc(1, sizeof(DspClient));
    if (!c)
        return E_OUTOFMEMORY;
    c->max_streams = max_streams;
    *handle = c;
    LOG_I("apu",
          "XDspConnect(%u streams, %u channels, %u s of impulse responses, %u-byte buffer at %p); DSP "
          "processing is not implemented, output is silent",
          max_streams, max_channels, aggregate_impulse_response_seconds, base_buffer_length, base_buffer);
    return S_OK;
}

#define XDSP_E_NOT_ALL_HANDLES_DEACTIVATED MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A4)
#define XDSP_E_NO_DEVICE_RESOURCES         MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A2)

static HRESULT WINAPI XDspDisconnect(DspClient* c) {
    if (!c)
        return E_INVALIDARG;
    if (c->active)
        return XDSP_E_NOT_ALL_HANDLES_DEACTIVATED;
    free(c);
    return S_OK;
}

static HRESULT WINAPI XDspActivate(DspClient* c, const DspActivation* params, DspStatus** status,
                                   DspStream** handle) {
    if (!c || !params || !status || !handle || params->type == 0 || params->type > DSP_TYPE_LAST ||
        !params->block_frames || (params->block_frames & (params->block_frames - 1)))
        return E_INVALIDARG;
    if ((u32)c->active >= c->max_streams)
        return XDSP_E_NO_DEVICE_RESOURCES;
    DspStream* s = (DspStream*)calloc(1, sizeof(DspStream));
    if (!s)
        return E_OUTOFMEMORY;
    s->client = c;
    s->params = *params;
    InterlockedIncrement(&c->active);
    *status = &s->status;
    *handle = s;
    return S_OK;
}

static HRESULT WINAPI XDspDeactivate(DspStream* s) {
    if (!s)
        return E_INVALIDARG;
    InterlockedDecrement(&s->client->active);
    free(s);
    return S_OK;
}

// Commands are numbered from 1 per stream. Streams are used from one thread at a time.
static HRESULT WINAPI XDspSubmitCommand(DspStream* s, const DspCommand* command, u32* sequence) {
    if (!s || !command || !sequence)
        return E_INVALIDARG;
    u32 n = ++s->sequence;
    if (command->output)
        memset(command->output, 0,
               (u64)s->params.block_frames * (s->params.channels ? s->params.channels : 1) * sizeof(f32));
    s->status.last_submitted = n;
    MemoryBarrier();
    s->status.last_processed = n;
    *sequence = n;
    return S_OK;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(XDspActivate),      HLE_EXPORT(XDspConnectWithMaximumStreamLimit),
    HLE_EXPORT(XDspDeactivate),    HLE_EXPORT(XDspDisconnect),
    HLE_EXPORT(XDspSubmitCommand),
};

extern const HleModule g_hle_module_chad = {"chad.dll", k_exports, ARRAY_COUNT(k_exports)};
