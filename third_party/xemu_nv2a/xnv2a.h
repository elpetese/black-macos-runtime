/* xboxrecomp <-> xemu NV2A renderer. See nv2a_glue.c. */
#ifndef XNV2A_H
#define XNV2A_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the OpenGL contexts. macOS requires this on the main thread. */
void xnv2a_context_init_main_thread(void);

/* vram: host pointer to GPU-visible physical memory (offset 0 = physical 0).
 * aperture: host pointer to the 16 MB NV2A register aperture (0xFD000000),
 * which holds RAMIN at +0x700000 and the PFIFO registers at +0x2000. */
void xnv2a_init(uint8_t *vram, size_t vram_size, uint8_t *aperture);

/* One pushbuffer method. params points at this method's parameter and the
 * ones after it (avail words in total), so a method that consumes several
 * words at once can. Returns how many words were consumed (>= 1). Must be
 * called on one thread only; the first call makes that thread the renderer's. */
int xnv2a_method(uint32_t subch, uint32_t method, uint32_t param,
                 uint32_t *params, size_t avail, int inc);

void xnv2a_report(void);

/* The newest frame at internal resolution (RECOMP_SCALE > 1): BGRA, top row
 * first, w x h with the given row stride in bytes. NULL when there is none.
 * A non-NULL return holds a lock: copy the pixels out, then release. */
const uint8_t *xnv2a_hires_acquire(uint64_t *seq, uint32_t *w, uint32_t *h,
                                   uint32_t *stride);
void xnv2a_hires_release(void);

#ifdef __cplusplus
}
#endif
#endif
