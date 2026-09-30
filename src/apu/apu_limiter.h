/* Host output limiter: preserve the floating-point mix until final conversion.
 * One 256-frame lookahead block adds 5.33 ms at 48 kHz. Both channels share
 * one gain, so their relative level and phase remain unchanged. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#define APU_LIMIT_FRAMES 256
#define APU_LIMIT_CEILING 0.95f

typedef struct ApuOutputLimiter {
    float delay[APU_LIMIT_FRAMES][2];
    float gain;
    int initialized;
    float input_peak, output_peak;
} ApuOutputLimiter;

static inline void apu_limit_pcm(ApuOutputLimiter *state,
                                const float input[APU_LIMIT_FRAMES][2],
                                int16_t output[APU_LIMIT_FRAMES][2])
{
    float samples[APU_LIMIT_FRAMES * 2][2];
    float peaks[APU_LIMIT_FRAMES * 2];
    unsigned deque[APU_LIMIT_FRAMES * 2], head = 0, tail = 0, next = 0;
    if (!state->initialized) {
        state->gain = 1.0f;
        state->initialized = 1;
    }
    memcpy(samples, state->delay, sizeof(state->delay));
    state->input_peak = state->output_peak = 0.0f;
    for (unsigned i = 0; i < APU_LIMIT_FRAMES; i++) {
        for (unsigned ch = 0; ch < 2; ch++) {
            float sample = input[i][ch];
            if (!isfinite(sample)) sample = 0.0f;
            samples[APU_LIMIT_FRAMES + i][ch] = sample;
            float magnitude = fabsf(sample);
            if (magnitude > state->input_peak) state->input_peak = magnitude;
        }
    }
    for (unsigned i = 0; i < APU_LIMIT_FRAMES * 2; i++)
        peaks[i] = fmaxf(fabsf(samples[i][0]), fabsf(samples[i][1]));

    for (unsigned i = 0; i < APU_LIMIT_FRAMES; i++) {
        /* Maximum of this sample and its 256 future samples, in O(n). */
        while (next <= i + APU_LIMIT_FRAMES) {
            while (tail > head && peaks[deque[tail - 1]] <= peaks[next]) tail--;
            deque[tail++] = next++;
        }
        while (head < tail && deque[head] < i) head++;
        float peak = peaks[deque[head]];
        float target = peak > APU_LIMIT_CEILING ? APU_LIMIT_CEILING / peak : 1.0f;
        /* 1 ms attack, 100 ms release, evaluated per sample at 48 kHz. */
        float coefficient = target < state->gain ? 0.020617819f : 0.000208312f;
        state->gain += (target - state->gain) * coefficient;
        /* Guard unusually abrupt signals without ever hard-clipping PCM. */
        float current = peaks[i];
        float safe = current > APU_LIMIT_CEILING ? APU_LIMIT_CEILING / current : 1.0f;
        if (state->gain > safe) state->gain = safe;
        for (unsigned ch = 0; ch < 2; ch++) {
            float value = samples[i][ch] * state->gain;
            float magnitude = fabsf(value);
            if (magnitude > state->output_peak) state->output_peak = magnitude;
            output[i][ch] = (int16_t)(value * 32767.0f);
        }
    }
    memcpy(state->delay, samples + APU_LIMIT_FRAMES, sizeof(state->delay));
}
