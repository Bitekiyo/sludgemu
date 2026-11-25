// Tests for the console audio DSP (XDsp) through the module's exports: transforms round-trip, and
// convolution with simple impulse responses behaves like the filters they describe.

#include <windows.h>
#include <math.h>
#include <string.h>

#include "hle/hle.h"
#include "test.h"

extern const HleModule g_hle_module_chad;

namespace {

#pragma pack(push, 1)
struct DspActivation {
    u32 type, options, block_frames, channels, impulse_response_floats;
    f32* impulse_response;
};
struct DspCommand {
    f32 begin_scale, end_scale;
    f32 *input, *output, *multiplier;
};
struct DspStatus {
    u32 last_submitted, last_processed, first_failed;
    HRESULT result;
};
#pragma pack(pop)

typedef HRESULT(WINAPI* ConnectFn)(f32*, u32, u32, u32, u32, void**);
typedef HRESULT(WINAPI* DisconnectFn)(void*);
typedef HRESULT(WINAPI* ActivateFn)(void*, const DspActivation*, DspStatus**, void**);
typedef HRESULT(WINAPI* DeactivateFn)(void*);
typedef HRESULT(WINAPI* SubmitFn)(void*, const DspCommand*, u32*);

void* chad(const char* name) {
    for (u32 i = 0; i < g_hle_module_chad.export_count; i++)
        if (!strcmp(g_hle_module_chad.exports[i].name, name))
            return g_hle_module_chad.exports[i].fn;
    return nullptr;
}

ConnectFn dsp_connect;
DisconnectFn dsp_disconnect;
ActivateFn dsp_activate;
DeactivateFn dsp_deactivate;
SubmitFn dsp_submit;

// Submits one block and waits for the worker to finish it.
bool run_block(void* stream, DspStatus* status, f32* in, f32* out, f32 begin = 1.0f, f32 end = 1.0f) {
    DspCommand c = {begin, end, in, out, nullptr};
    u32 sequence = 0;
    if (FAILED(dsp_submit(stream, &c, &sequence)))
        return false;
    for (int i = 0; i < 2000 && *(volatile u32*)&status->last_processed < sequence; i++)
        Sleep(1);
    return status->last_processed >= sequence;
}

const u32 kBlock = 512;

} // namespace

void test_xdsp() {
    dsp_connect = (ConnectFn)chad("XDspConnectWithMaximumStreamLimit");
    dsp_disconnect = (DisconnectFn)chad("XDspDisconnect");
    dsp_activate = (ActivateFn)chad("XDspActivate");
    dsp_deactivate = (DeactivateFn)chad("XDspDeactivate");
    dsp_submit = (SubmitFn)chad("XDspSubmitCommand");
    CHECK(dsp_connect && dsp_disconnect && dsp_activate && dsp_deactivate && dsp_submit);
    if (!dsp_connect || !dsp_disconnect || !dsp_activate || !dsp_deactivate || !dsp_submit)
        return;

    static f32 in[2 * kBlock], spectrum[2 * kBlock], out[2 * kBlock], response[4 * 2 * kBlock];
    void* client = nullptr;
    CHECK(SUCCEEDED(dsp_connect(in, sizeof(in), 4, 8, 2, &client)));

    // Forward then inverse transform gives the input back; a constant has only DC.
    DspActivation fwd = {2, 0, kBlock, 1, 0, nullptr};
    DspActivation inv = {3, 0, kBlock, 1, 0, nullptr};
    DspStatus *fs = nullptr, *is = nullptr;
    void *fstream = nullptr, *istream = nullptr;
    CHECK(SUCCEEDED(dsp_activate(client, &fwd, &fs, &fstream)));
    CHECK(SUCCEEDED(dsp_activate(client, &inv, &is, &istream)));
    for (u32 i = 0; i < kBlock; i++)
        in[i] = 1.0f;
    CHECK(run_block(fstream, fs, in, spectrum));
    CHECK(fabsf(spectrum[0] - (f32)kBlock) < 1e-2f && fabsf(spectrum[1]) < 1e-3f &&
          fabsf(spectrum[2]) < 1e-3f);
    for (u32 i = 0; i < kBlock; i++)
        in[i] = sinf((f32)i * 0.37f) + 0.25f * cosf((f32)i * 1.9f);
    CHECK(run_block(fstream, fs, in, spectrum));
    CHECK(run_block(istream, is, spectrum, out));
    f32 worst = 0.0f;
    for (u32 i = 0; i < kBlock; i++)
        worst = fmaxf(worst, fabsf(out[i] - in[i]));
    CHECK(worst < 1e-4f);
    CHECK(SUCCEEDED(dsp_deactivate(fstream)) && SUCCEEDED(dsp_deactivate(istream)));

    // Convolution with a unit impulse passes blocks through, and an impulse two blocks later
    // (partition 2) adds the block from two blocks before. A unit impulse at the start of a
    // partition transforms to all ones: DC and Nyquist 1, every bin (1, 0).
    const u32 window = 2 * kBlock;
    memset(response, 0, sizeof(response));
    for (u32 k = 0; k < window; k += 2) {
        response[k] = 1.0f;              // partition 0: impulse at 0
        response[2 * window + k] = 1.0f; // partition 2: impulse two blocks later
    }
    response[1] = 1.0f;
    response[2 * window + 1] = 1.0f;
    DspActivation conv = {1, 0, kBlock, 1, 3 * window, response};
    DspStatus* cs = nullptr;
    void* cstream = nullptr;
    CHECK(SUCCEEDED(dsp_activate(client, &conv, &cs, &cstream)));
    f32 blocks[4][kBlock];
    for (u32 b = 0; b < 4; b++)
        for (u32 i = 0; i < kBlock; i++)
            blocks[b][i] = sinf((f32)(b * kBlock + i) * 0.05f);
    for (u32 b = 0; b < 4; b++) {
        memcpy(in, blocks[b], sizeof(blocks[b]));
        CHECK(run_block(cstream, cs, in, out));
        // out = this block + the block from two blocks ago
        worst = 0.0f;
        for (u32 i = 0; i < kBlock; i++) {
            f32 expect = blocks[b][i] + (b >= 2 ? blocks[b - 2][i] : 0.0f);
            worst = fmaxf(worst, fabsf(out[i] - expect));
        }
        CHECK(worst < 1e-3f);
    }
    // The gain ramp scales the output from begin to end across the block.
    memcpy(in, blocks[0], sizeof(blocks[0]));
    CHECK(run_block(cstream, cs, in, out, 0.0f, 0.0f));
    CHECK(fabsf(out[100]) < 1e-6f);
    CHECK(SUCCEEDED(dsp_deactivate(cstream)));
    CHECK(SUCCEEDED(dsp_disconnect(client)));
}
