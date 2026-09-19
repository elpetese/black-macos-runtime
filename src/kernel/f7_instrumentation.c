/**
 * f7_instrumentation.c — F7.0 boundary counters
 */

#include "f7_instrumentation.h"
#include <stdio.h>
#include <string.h>

/* Audio */
uint64_t g_f7_apu_frames_submitted;
uint64_t g_f7_apu_frames_played;
uint64_t g_f7_apu_mixbin_nonzero_samples;
uint64_t g_f7_apu_vp_mixbin_nonzero_samples;
uint64_t g_f7_apu_framebuf_rms_samples;
uint64_t g_f7_apu_vp_voices_processed;
uint64_t g_f7_apu_vp_nonzero_voices;
uint64_t g_f7_apu_vp_nonzero_samples_read;
uint64_t g_f7_apu_dsp_frames_processed;

/* Graphics */
uint64_t g_f7_d3d8_create_device;
uint64_t g_f7_d3d8_draw_primitive;
uint64_t g_f7_d3d8_draw_indexed_primitive;
uint64_t g_f7_d3d8_present;
uint64_t g_f7_nv2a_clear_count;
uint64_t g_f7_nv2a_dma_put_changes;
uint64_t g_f7_nv2a_dma_get_changes;
uint64_t g_f7_nv2a_pgraph_method_count;
uint64_t g_f7_nv2a_pgraph_draw_count;

/* Input */
uint64_t g_f7_input_poll_count;
uint64_t g_f7_input_nonzero_buttons;

void f7_print_counts(void)
{
    fprintf(stderr, "\n"
        "===== F7.0 boundary counts =====\n"
        "[AUDIO] frames_submitted=%llu frames_played=%llu\n"
        "[AUDIO] mixbin_nonzero_samples=%llu vp_mixbin_nonzero_samples=%llu framebuf_rms_samples=%llu\n"
        "[AUDIO] vp_voices_processed=%llu vp_nonzero_voices=%llu vp_nonzero_samples_read=%llu dsp_frames_processed=%llu\n"
        "[GRAPHICS] D3D8_CreateDevice=%llu DrawPrimitive=%llu DrawIndexedPrimitive=%llu Present=%llu\n"
        "[GRAPHICS] NV2A_clears=%llu DMA_PUT_changes=%llu DMA_GET_changes=%llu PGRAPH_methods=%llu draws=%llu\n"
        "[INPUT] polls=%llu nonzero_buttons=%llu\n"
        "================================\n",
        (unsigned long long)g_f7_apu_frames_submitted,
        (unsigned long long)g_f7_apu_frames_played,
        (unsigned long long)g_f7_apu_mixbin_nonzero_samples,
        (unsigned long long)g_f7_apu_vp_mixbin_nonzero_samples,
        (unsigned long long)g_f7_apu_framebuf_rms_samples,
        (unsigned long long)g_f7_apu_vp_voices_processed,
        (unsigned long long)g_f7_apu_vp_nonzero_voices,
        (unsigned long long)g_f7_apu_vp_nonzero_samples_read,
        (unsigned long long)g_f7_apu_dsp_frames_processed,
        (unsigned long long)g_f7_d3d8_create_device,
        (unsigned long long)g_f7_d3d8_draw_primitive,
        (unsigned long long)g_f7_d3d8_draw_indexed_primitive,
        (unsigned long long)g_f7_d3d8_present,
        (unsigned long long)g_f7_nv2a_clear_count,
        (unsigned long long)g_f7_nv2a_dma_put_changes,
        (unsigned long long)g_f7_nv2a_dma_get_changes,
        (unsigned long long)g_f7_nv2a_pgraph_method_count,
        (unsigned long long)g_f7_nv2a_pgraph_draw_count,
        (unsigned long long)g_f7_input_poll_count,
        (unsigned long long)g_f7_input_nonzero_buttons);
    fflush(stderr);
}
