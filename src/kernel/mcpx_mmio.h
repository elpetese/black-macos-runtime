#ifndef XBOX_MCPX_MMIO_H
#define XBOX_MCPX_MMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MCPX register interception for hosts without the Win32 VEH decoder.
 *
 * arm: make the AC'97 register page fault, seeded from its current contents.
 * fault: call first from the SIGSEGV/SIGBUS handler; returns 1 when the fault
 * was a register access that has been emulated and the thread may resume.
 * Both are no-ops returning 0 where unsupported. */
typedef uint64_t (*xbox_mmio_read_fn)(void *ctx, uint32_t offset, unsigned size);
typedef void (*xbox_mmio_write_fn)(void *ctx, uint32_t offset, uint64_t value,
                                   unsigned size);

/* Intercept a guest register block (page aligned) and hand every access to a
 * device model; offset is relative to guest_va. Returns 1 on success. */
int xbox_mmio_intercept(uint32_t guest_va, uint32_t size,
                        xbox_mmio_read_fn read, xbox_mmio_write_fn write,
                        void *ctx, const char *name);

/* Read an intercepted register without faulting, for runtime code that polls
 * device state. Returns 1 when guest_va lies in an intercepted region. */
int xbox_mmio_read(uint32_t guest_va, unsigned size, uint64_t *value);

/* Have the timer thread raise `vector` whenever pending(ctx) is nonzero --
 * a level-triggered device interrupt. Returns 0 on success, -1 when full. */
int xbox_RegisterInterruptSource(uint32_t vector, int (*pending)(void *ctx),
                                 void *ctx);

int xbox_mcpx_mmio_arm_ac97(void *ac97_host_base);
int xbox_mcpx_mmio_fault(void *ucontext, uintptr_t fault_addr);

/* Advance AC'97 playback in real time. Returns nonzero when a channel is
 * interrupting, so the caller should raise the AC'97 interrupt. */
int xbox_mcpx_ac97_tick(void);

#ifdef __cplusplus
}
#endif

#endif
