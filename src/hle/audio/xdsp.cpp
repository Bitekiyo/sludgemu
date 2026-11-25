// chad.dll: the audio hardware's DSP (convolution reverb and FFTs on blocks of samples).
//
// Titles activate streams on a client and submit one block per command; a status block per stream,
// which titles poll, tells which commands are done. Commands are processed in order on a worker
// thread, as the hardware does, so a long impulse response never stalls the submitting thread.
//
// Data formats, as far as the documentation pins them down:
// - Frequency-domain data is complex pairs (real, imaginary). A block of N real samples transforms
//   into N floats: bins 0..N/2-1, with bin 0 holding DC (real part) and Nyquist (imaginary part).
//   Forward transforms are unscaled, inverse transforms divide by N. Polar format stores
//   (magnitude, phase) instead.
// - Convolution is uniformly partitioned overlap-save: the impulse response is a series of
//   partitions, each the transform of one block of response zero-padded to two blocks, packed as
//   above; stereo responses keep one series per channel.

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "hle/hle.h"

#pragma pack(push, 1)
struct DspActivation {
    u32 type; // DSP_CONVOLUTION, DSP_FORWARD_FFT, DSP_INVERSE_FFT
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

enum DspType : u32 { DSP_CONVOLUTION = 1, DSP_FORWARD_FFT = 2, DSP_INVERSE_FFT = 3 };

#define OPTION_DEINTERLEAVED   0x2u
#define OPTION_POLAR           0x4u
#define OPTION_STEREO_RESPONSE 0x8u

#define XDSP_E_QUEUE_FULL                  MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A0)
#define XDSP_E_PENDING_RESULTS             MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A1)
#define XDSP_E_NO_DEVICE_RESOURCES         MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A2)
#define XDSP_E_NOT_ALL_HANDLES_DEACTIVATED MAKE_HRESULT(SEVERITY_ERROR, 0xAC8, 0x10A4)

#define MAX_BLOCK    2048u
#define QUEUE_LENGTH 64u // commands in flight per stream

// --- FFT -----------------------------------------------------------------------------------------

struct Complex {
    f32 re, im;
};

// In-place iterative radix-2 transform of n complex values (n a power of two). sign -1: forward.
static void fft(Complex* x, u32 n, int sign) {
    for (u32 i = 1, j = 0; i < n; i++) {
        u32 bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            Complex t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    for (u32 len = 2; len <= n; len <<= 1) {
        f64 angle = sign * 2.0 * 3.14159265358979323846 / len;
        for (u32 k = 0; k < len / 2; k++) {
            Complex w = {(f32)cos(angle * k), (f32)sin(angle * k)};
            for (u32 i = 0; i < n; i += len) {
                Complex a = x[i + k];
                Complex b = x[i + k + len / 2];
                Complex t = {b.re * w.re - b.im * w.im, b.re * w.im + b.im * w.re};
                x[i + k] = {a.re + t.re, a.im + t.im};
                x[i + k + len / 2] = {a.re - t.re, a.im - t.im};
            }
        }
    }
}

// Real samples -> packed spectrum (n floats). `scratch` holds n complex values.
static void real_forward(const f32* in, u32 n, f32* out, Complex* scratch) {
    for (u32 i = 0; i < n; i++)
        scratch[i] = {in[i], 0.0f};
    fft(scratch, n, -1);
    out[0] = scratch[0].re;
    out[1] = scratch[n / 2].re;
    for (u32 k = 1; k < n / 2; k++) {
        out[2 * k] = scratch[k].re;
        out[2 * k + 1] = scratch[k].im;
    }
}

// Packed spectrum (n floats) -> real samples, divided by n.
static void real_inverse(const f32* in, u32 n, f32* out, Complex* scratch) {
    scratch[0] = {in[0], 0.0f};
    scratch[n / 2] = {in[1], 0.0f};
    for (u32 k = 1; k < n / 2; k++) {
        scratch[k] = {in[2 * k], in[2 * k + 1]};
        scratch[n - k] = {in[2 * k], -in[2 * k + 1]};
    }
    fft(scratch, n, 1);
    f32 scale = 1.0f / (f32)n;
    for (u32 i = 0; i < n; i++)
        out[i] = scratch[i].re * scale;
}

// DC and Nyquist are real, so they stay as they are.
static void to_polar(f32* spectrum, u32 n) {
    for (u32 k = 1; k < n / 2; k++) {
        f32 re = spectrum[2 * k], im = spectrum[2 * k + 1];
        spectrum[2 * k] = sqrtf(re * re + im * im);
        spectrum[2 * k + 1] = atan2f(im, re);
    }
}

static void from_polar(f32* spectrum, u32 n) {
    for (u32 k = 1; k < n / 2; k++) {
        f32 mag = spectrum[2 * k], phase = spectrum[2 * k + 1];
        spectrum[2 * k] = mag * cosf(phase);
        spectrum[2 * k + 1] = mag * sinf(phase);
    }
}

// acc += a * b over packed spectra of n floats (DC and Nyquist multiply as reals).
static void multiply_accumulate(f32* acc, const f32* a, const f32* b, u32 n) {
    acc[0] += a[0] * b[0];
    acc[1] += a[1] * b[1];
    for (u32 k = 1; k < n / 2; k++) {
        f32 ar = a[2 * k], ai = a[2 * k + 1], br = b[2 * k], bi = b[2 * k + 1];
        acc[2 * k] += ar * br - ai * bi;
        acc[2 * k + 1] += ar * bi + ai * br;
    }
}

// --- streams -------------------------------------------------------------------------------------

struct DspClient {
    u32 max_streams;
    volatile LONG active;
};

struct DspStream {
    DspClient* client;
    DspActivation params;
    DspStatus status; // the title polls it
    u32 sequence;
    DspCommand queue[QUEUE_LENGTH];
    u32 queued; // commands waiting, processed oldest first (under the DSP lock)
    u32 head;
    // Convolution state, per channel: the last two input blocks, and the spectra of past windows
    // (a frequency-domain delay line with one slot per response partition).
    u32 partitions;
    f32* history; // channels * 2 * block
    f32* spectra; // channels * partitions * 2 * block
    u32 newest;   // delay line slot of the latest window
    DspStream* next;
};

static struct {
    SRWLOCK lock;            // stream list and queues
    CONDITION_VARIABLE work; // commands queued, or done
    DspStream* streams;
    HANDLE thread;
    INIT_ONCE once;
} g_dsp = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, nullptr, nullptr, INIT_ONCE_STATIC_INIT};

// Channel `ch` of a block, interleaved or not.
static void read_channel(const DspStream* s, const f32* block, u32 ch, f32* out) {
    u32 n = s->params.block_frames, channels = s->params.channels;
    if (channels == 1 || (s->params.options & OPTION_DEINTERLEAVED))
        memcpy(out, block + (usize)ch * n, n * sizeof(f32));
    else
        for (u32 i = 0; i < n; i++)
            out[i] = block[i * channels + ch];
}

// Writes channel `ch` of an output block, with the command's gain ramp.
static void write_channel(const DspStream* s, f32* block, u32 ch, const f32* in, const DspCommand& c) {
    u32 n = s->params.block_frames, channels = s->params.channels;
    bool ramp = c.begin_scale != 1.0f || c.end_scale != 1.0f;
    f32 step = (c.end_scale - c.begin_scale) / (f32)n;
    for (u32 i = 0; i < n; i++) {
        f32 v = ramp ? in[i] * (c.begin_scale + (f32)i * step) : in[i];
        if (channels == 1 || (s->params.options & OPTION_DEINTERLEAVED))
            block[(usize)ch * n + i] = v;
        else
            block[i * channels + ch] = v;
    }
}

// One convolution block for every channel: overlap-save with the partitioned response.
static void convolve(DspStream* s, const DspCommand& c, Complex* scratch) {
    u32 n = s->params.block_frames, window = 2 * n;
    f32 input[MAX_BLOCK], acc[2 * MAX_BLOCK], time[2 * MAX_BLOCK];
    s->newest = (s->newest + 1) % s->partitions;
    bool stereo_response = (s->params.options & OPTION_STEREO_RESPONSE) && s->params.channels == 2;
    for (u32 ch = 0; ch < s->params.channels; ch++) {
        f32* history = s->history + (usize)ch * window;
        memmove(history, history + n, n * sizeof(f32));
        read_channel(s, c.input, ch, input);
        memcpy(history + n, input, n * sizeof(f32));
        f32* spectra = s->spectra + (usize)ch * s->partitions * window;
        real_forward(history, window, spectra + (usize)s->newest * window, scratch);

        const f32* response = s->params.impulse_response +
                              (stereo_response ? (usize)ch * s->params.impulse_response_floats : 0);
        memset(acc, 0, window * sizeof(f32));
        for (u32 p = 0; p < s->partitions; p++) {
            u32 slot = (s->newest + s->partitions - p) % s->partitions;
            multiply_accumulate(acc, spectra + (usize)slot * window, response + (usize)p * window, window);
        }
        real_inverse(acc, window, time, scratch);
        write_channel(s, c.output, ch, time + n, c); // the second half of the window is valid
    }
}

static void run_command(DspStream* s, const DspCommand& c, Complex* scratch) {
    u32 n = s->params.block_frames;
    if (!c.input || !c.output)
        return;
    switch (s->params.type) {
    case DSP_CONVOLUTION:
        if (s->partitions)
            convolve(s, c, scratch);
        else
            memset(c.output, 0, (usize)n * s->params.channels * sizeof(f32));
        break;
    case DSP_FORWARD_FFT: {
        f32 input[MAX_BLOCK];
        for (u32 i = 0; i < n; i++)
            input[i] = c.multiplier ? c.input[i] * c.multiplier[i] : c.input[i];
        real_forward(input, n, c.output, scratch);
        if (s->params.options & OPTION_POLAR)
            to_polar(c.output, n);
        break;
    }
    case DSP_INVERSE_FFT: {
        f32 spectrum[MAX_BLOCK];
        memcpy(spectrum, c.input, n * sizeof(f32));
        if (s->params.options & OPTION_POLAR)
            from_polar(spectrum, n);
        real_inverse(spectrum, n, c.output, scratch);
        break;
    }
    }
}

static DWORD WINAPI dsp_thread(void*) {
    Complex* scratch = (Complex*)malloc(2 * MAX_BLOCK * sizeof(Complex));
    if (!scratch)
        FATAL("out of memory for the DSP worker");
    AcquireSRWLockExclusive(&g_dsp.lock);
    for (;;) {
        DspStream* s = g_dsp.streams;
        while (s && !s->queued)
            s = s->next;
        if (!s) {
            SleepConditionVariableSRW(&g_dsp.work, &g_dsp.lock, INFINITE, 0);
            continue;
        }
        DspCommand c = s->queue[s->head];
        ReleaseSRWLockExclusive(&g_dsp.lock);
        run_command(s, c, scratch);
        AcquireSRWLockExclusive(&g_dsp.lock);
        s->head = (s->head + 1) % QUEUE_LENGTH;
        s->queued--;
        MemoryBarrier(); // the output before the status that announces it
        s->status.last_processed++;
        WakeAllConditionVariable(&g_dsp.work);
    }
}

static BOOL CALLBACK start_worker(PINIT_ONCE, void*, void**) {
    g_dsp.thread = CreateThread(nullptr, 0, dsp_thread, nullptr, 0, nullptr);
    if (g_dsp.thread)
        SetThreadPriority(g_dsp.thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return g_dsp.thread != nullptr;
}

// --- exports -------------------------------------------------------------------------------------

static HRESULT WINAPI XDspConnectWithMaximumStreamLimit(f32* base_buffer, u32 base_buffer_length,
                                                        u32 aggregate_impulse_response_seconds,
                                                        u32 max_streams, u32 max_channels,
                                                        DspClient** handle) {
    if (!handle)
        return E_INVALIDARG;
    if (!InitOnceExecuteOnce(&g_dsp.once, start_worker, nullptr, nullptr))
        return E_OUTOFMEMORY;
    DspClient* c = (DspClient*)calloc(1, sizeof(DspClient));
    if (!c)
        return E_OUTOFMEMORY;
    c->max_streams = max_streams;
    *handle = c;
    LOG_I("apu", "XDspConnect(%u streams, %u channels, %u s of impulse responses, %u-byte buffer at %p)",
          max_streams, max_channels, aggregate_impulse_response_seconds, base_buffer_length, base_buffer);
    return S_OK;
}

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
    if (!c || !params || !status || !handle || params->type < DSP_CONVOLUTION ||
        params->type > DSP_INVERSE_FFT || params->block_frames < 4 || params->block_frames > MAX_BLOCK ||
        (params->block_frames & (params->block_frames - 1)) || params->channels < 1 || params->channels > 2)
        return E_INVALIDARG;
    if ((u32)c->active >= c->max_streams)
        return XDSP_E_NO_DEVICE_RESOURCES;
    DspStream* s = (DspStream*)calloc(1, sizeof(DspStream));
    if (!s)
        return E_OUTOFMEMORY;
    s->client = c;
    s->params = *params;
    if (params->type == DSP_CONVOLUTION) {
        u32 window = 2 * params->block_frames;
        s->partitions = params->impulse_response ? params->impulse_response_floats / window : 0;
        if (s->partitions) {
            s->history = (f32*)calloc((usize)params->channels * window, sizeof(f32));
            s->spectra = (f32*)calloc((usize)params->channels * s->partitions * window, sizeof(f32));
            if (!s->history || !s->spectra) {
                free(s->history);
                free(s->spectra);
                free(s);
                return E_OUTOFMEMORY;
            }
        }
        // The layout of converted impulse responses is inferred (see the top of the file); these
        // numbers show whether a title's responses fit it.
        LOG_I("apu",
              "XDsp convolution: %u-frame blocks, %u channels, %u response floats (%u partitions), "
              "options 0x%x",
              params->block_frames, params->channels, params->impulse_response_floats, s->partitions,
              params->options);
    }
    InterlockedIncrement(&c->active);
    AcquireSRWLockExclusive(&g_dsp.lock);
    s->next = g_dsp.streams;
    g_dsp.streams = s;
    ReleaseSRWLockExclusive(&g_dsp.lock);
    *status = &s->status;
    *handle = s;
    return S_OK;
}

static HRESULT WINAPI XDspDeactivate(DspStream* s) {
    if (!s)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_dsp.lock);
    if (s->queued) {
        ReleaseSRWLockExclusive(&g_dsp.lock);
        return XDSP_E_PENDING_RESULTS;
    }
    for (DspStream** p = &g_dsp.streams; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_dsp.lock);
    InterlockedDecrement(&s->client->active);
    free(s->history);
    free(s->spectra);
    free(s);
    return S_OK;
}

// Commands are numbered from 1 per stream. A stream is submitted to from one thread at a time.
static HRESULT WINAPI XDspSubmitCommand(DspStream* s, const DspCommand* command, u32* sequence) {
    if (!s || !command || !sequence)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_dsp.lock);
    if (s->queued == QUEUE_LENGTH) {
        ReleaseSRWLockExclusive(&g_dsp.lock);
        return XDSP_E_QUEUE_FULL;
    }
    s->queue[(s->head + s->queued) % QUEUE_LENGTH] = *command;
    s->queued++;
    *sequence = ++s->sequence;
    s->status.last_submitted = s->sequence;
    WakeAllConditionVariable(&g_dsp.work);
    ReleaseSRWLockExclusive(&g_dsp.lock);
    return S_OK;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(XDspActivate),      HLE_EXPORT(XDspConnectWithMaximumStreamLimit),
    HLE_EXPORT(XDspDeactivate),    HLE_EXPORT(XDspDisconnect),
    HLE_EXPORT(XDspSubmitCommand),
};

extern const HleModule g_hle_module_chad = {"chad.dll", k_exports, ARRAY_COUNT(k_exports)};
