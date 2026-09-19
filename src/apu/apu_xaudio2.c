/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"
#include "../kernel/f7_instrumentation.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_f7_apu_frames_submitted++;
    if (g_f7_apu_frames_submitted % 60 == 0 && recomp_verbose()) {
        extern void f7_print_counts(void);
        f7_print_counts();
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL2 queued-audio backend */

#include <SDL.h>

#define SDL_AUDIO_RATE        48000
#define SDL_AUDIO_CHANNELS    2
#define SDL_AUDIO_BUF_SAMPLES 256
#define SDL_AUDIO_MAX_QUEUED  (SDL_AUDIO_BUF_SAMPLES * SDL_AUDIO_CHANNELS * \
                               (int)sizeof(int16_t) * 4)

static SDL_AudioDeviceID g_audio_device;
static int g_audio_initialized;
static unsigned g_audio_submissions;
static int g_audio_nonzero_announced;

int xa2_init(void)
{
    SDL_AudioSpec wanted, obtained;

    if (g_audio_initialized)
        return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL-AUDIO] init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_zero(wanted);
    wanted.freq = SDL_AUDIO_RATE;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = SDL_AUDIO_CHANNELS;
    wanted.samples = SDL_AUDIO_BUF_SAMPLES;
    wanted.callback = NULL;
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (!g_audio_device) {
        fprintf(stderr, "[SDL-AUDIO] open failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    if (obtained.freq != wanted.freq || obtained.format != wanted.format ||
        obtained.channels != wanted.channels) {
        fprintf(stderr, "[SDL-AUDIO] unsupported device format: %d Hz, 0x%X, %u channels\n",
                obtained.freq, obtained.format, obtained.channels);
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(g_audio_device, 0);
    g_audio_submissions = 0;
    g_audio_nonzero_announced = 0;
    g_audio_initialized = 1;
    fprintf(stderr, "[SDL-AUDIO] initialized (%d Hz stereo 16-bit, %u samples)\n",
            obtained.freq, obtained.samples);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_audio_initialized)
        return;
    SDL_ClearQueuedAudio(g_audio_device);
    SDL_CloseAudioDevice(g_audio_device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[SDL-AUDIO] shut down (%u submissions)\n", g_audio_submissions);
    g_audio_device = 0;
    g_audio_initialized = 0;
}

int xa2_is_active(void)
{
    return g_audio_initialized;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t bytes;
    int peak = 0;

    if (!g_audio_initialized || !samples || num_samples <= 0)
        return 0;
    bytes = (uint32_t)num_samples * SDL_AUDIO_CHANNELS * sizeof(int16_t);
    if (SDL_GetQueuedAudioSize(g_audio_device) > SDL_AUDIO_MAX_QUEUED)
        return 0;
    if (SDL_QueueAudio(g_audio_device, samples, bytes) != 0) {
        fprintf(stderr, "[SDL-AUDIO] queue failed: %s\n", SDL_GetError());
        return 0;
    }
    g_audio_submissions++;
    g_f7_apu_frames_submitted++;
    g_f7_apu_frames_played++;   /* POSIX: account for SDL-submitted frames (was Windows-only stub) */
    for (int i = 0; i < num_samples * SDL_AUDIO_CHANNELS; i++) {
        int v = samples[i] < 0 ? -(int)samples[i] : (int)samples[i];
        if (v > peak) peak = v;
    }
    if (peak > 0 && !g_audio_nonzero_announced) {
        g_audio_nonzero_announced = 1;
        fprintf(stderr, "[SDL-AUDIO] first non-silent PCM buffer (peak=%d)\n", peak);
    }
    if (g_f7_apu_frames_submitted % 120 == 0 && peak == 0 && recomp_verbose()) {
        fprintf(stderr, "[SDL-AUDIO] silent buffer submitted (frames=%llu)\n",
            (unsigned long long)g_f7_apu_frames_submitted);
    }
    if (g_f7_apu_frames_submitted % 60 == 0 && recomp_verbose()) {
        extern void f7_print_counts(void);
        f7_print_counts();
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return SDL_AUDIO_BUF_SAMPLES;
}

#endif /* _WIN32 */
