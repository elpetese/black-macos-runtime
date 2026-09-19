/* xbox_dirty_tracking.h -- page-granular dirty tracking for guest VRAM.
 *
 * xemu's NV2A renderer decides whether a texture or vertex buffer needs
 * re-uploading to OpenGL by polling memory_region_test_and_clear_dirty() on
 * the VRAM MemoryRegion. Under xemu/QEMU that consults a bitset fed by the
 * CPU's write-interception (TCG or KVM). In this static recompiler the guest
 * writes VRAM directly with no such hook, so the compat stub in
 * xemu_compat_types.h reported every page as dirty -- which made the renderer
 * re-hash and re-upload every texture on every one of the ~600 draws per
 * frame (20% of the GPU thread: texture.c P2b/m2).
 *
 * This module supplies the real implementation, behind an opt-in flag so the
 * working boot path is untouched:
 *
 *   - RECOMP_XEMU_GPU=1      : enable xemu's OpenGL renderer
 *   - RECOMP_DIRTY_TRACKING=1: arm the VRAM window for mprotect(RO)+SIGSEGV
 *
 * Arming mprotects VRAM read-only and allocates a 1-bit-per-page bitmap. A
 * guest store traps; the host SIGSEGV handler (xbox_dirty_fault) sets the bit
 * for the faulting page and flips it back writable, so the trapping
 * instruction retries on the now-writable page. The renderer's
 * test_and_clear checks the bitmap, re-uploads only the dirty pages, clears
 * the bits and re-protects the pages read-only -- closing the loop so the
 * next store traps again.
 *
 * When the region is NOT armed, test_and_clear returns true (everything
 * dirty), i.e. identical to the old stub: this module is a pure no-op until
 * xbox_dirty_arm() is called by the host under RECOMP_DIRTY_TRACKING=1.
 */
#ifndef XBOX_DIRTY_TRACKING_H
#define XBOX_DIRTY_TRACKING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* True (nonzero) iff write-tracking is armed on the host region identified by
 * host_base/size. Hot path: called once per texture bind, so keep it cheap. */
int xbox_dirty_armed(const void *host_base, size_t size);

/* Arm a host memory region: allocate a per-page bitmap and mprotect every
 * page PROT_READ so writes trap. Call once at startup, before the guest
 * writes to the region. host_base need not be page-aligned; pages straddling
 * the boundary are included for safety. */
void xbox_dirty_arm(void *host_base, size_t size);

/* Test-and-clear the dirty bits covering [offset, offset+size) within the
 * region. Pages that were dirty have their bit cleared and are re-protected
 * read-only, so the next store to them traps. Returns true if any were dirty.
 * Called from the renderer (GPU) thread, NOT the signal handler. */
bool xbox_dirty_test_and_clear(const void *host_base, size_t region_size,
                               size_t offset, size_t size);

/* Mark the [offset, offset+size) range dirty and make those pages writable
 * (xemu's memory_region_set_dirty). No-op for unarmed regions. */
void xbox_dirty_set(const void *host_base, size_t region_size,
                    size_t offset, size_t size);

/* Track a second mapping of the same pages (the tiled aperture at
 * 0xF0000000 aliases the contiguous window): both views are protected and
 * released together, so a write through either is seen. */
void xbox_dirty_add_alias(const void *host_base, void *alias_base);

/* Before writing tracked memory in a way that does not fault -- a read()
 * syscall (EFAULT on a protected page), a GL readback -- mark the range dirty
 * and make it writable. No-op when nothing is armed or the range is outside. */
void xbox_dirty_prepare_write(void *host_ptr, size_t len);

/* Signal-handler entry (must be async-signal-safe). If fault_addr falls inside
 * an armed region, mark the faulting page dirty and mprotect it PROT_READ|
 * PROT_WRITE so the trapped store retries; return 1 (handled). Otherwise
 * return 0 so the caller falls through to its other handlers (e.g. the MMIO
 * fault resolver). The kernel leaves the PC unchanged for a store fault, so
 * no ucontext fiddling is needed -- the write simply lands on the writable
 * page on retry. */
int xbox_dirty_fault(uintptr_t fault_addr);

#endif /* XBOX_DIRTY_TRACKING_H */
