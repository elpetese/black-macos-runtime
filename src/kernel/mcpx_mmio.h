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
int xbox_mcpx_mmio_arm_ac97(void *ac97_host_base);
int xbox_mcpx_mmio_fault(void *ucontext, uintptr_t fault_addr);

#ifdef __cplusplus
}
#endif

#endif
