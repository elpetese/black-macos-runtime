/**
 * f7_instrumentation.c — F7.0 boundary counters
 *
 * Lightweight counters for audio/graphics/input frontiers.
 * No allocation, no syscalls in the hot path beyond atomic increment.
 * Printed once at shutdown via f7_print_counts().
 */

#ifndef F7_INSTRUMENTATION_H
#define F7_INSTRUMENTATION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio */
extern uint64_t g_f7_apu_frames_submitted;
extern uint64_t g_f7_apu_frames_played;
extern uint64_t g_f7_apu_mixbin_nonzero_samples;
extern uint64_t g_f7_apu_vp_mixbin_nonzero_samples;
extern uint64_t g_f7_apu_framebuf_rms_samples;
extern uint64_t g_f7_apu_vp_voices_processed;
extern uint64_t g_f7_apu_vp_nonzero_voices;
extern uint64_t g_f7_apu_vp_nonzero_samples_read;
extern uint64_t g_f7_apu_dsp_frames_processed;

/* Graphics */
extern uint64_t g_f7_d3d8_create_device;
extern uint64_t g_f7_d3d8_draw_primitive;
extern uint64_t g_f7_d3d8_draw_indexed_primitive;
extern uint64_t g_f7_d3d8_present;
extern uint64_t g_f7_nv2a_clear_count;
extern uint64_t g_f7_nv2a_dma_put_changes;
extern uint64_t g_f7_nv2a_dma_get_changes;
extern uint64_t g_f7_nv2a_pgraph_method_count;
extern uint64_t g_f7_nv2a_pgraph_draw_count;

/* Input */
extern uint64_t g_f7_input_poll_count;
extern uint64_t g_f7_input_nonzero_buttons;

/* Call once at shutdown (e.g. from main_posix.c atexit or similar). */
void f7_print_counts(void);

#ifdef __cplusplus
}
#endif
#endif /* F7_INSTRUMENTATION_H */
