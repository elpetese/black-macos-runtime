/*
 * NV2A MMIO Hook - instruction decoder for GPU register access
 *
 * When recompiled code accesses NV2A MMIO registers (0xFD000000+),
 * the access faults because no physical page is mapped. This module
 * decodes the faulting host instruction, extracts the read/write
 * operation, routes it through the NV2A register handlers, and
 * advances PC past the instruction.
 *
 * This is the key bridge between recompiled Xbox D3D8 code and
 * the xemu NV2A GPU emulation.
 */

#ifndef BURNOUT3_NV2A_MMIO_HOOK_H
#define BURNOUT3_NV2A_MMIO_HOOK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#endif

struct NV2AState;
typedef struct NV2AState NV2AState;

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

/*
 * Initialize the NV2A GPU subsystem.
 * Call this during startup before the game code runs.
 * Allocates VRAM, RAMIN, and initializes register state.
 */
void nv2a_hook_init(ptrdiff_t xbox_mem_offset);

/* Get the NV2A state pointer, or NULL if not initialized. */
NV2AState *nv2a_hook_get_state(void);

/*
 * Handle an NV2A MMIO access fault.
 *
 * Called from the signal/VEH handler when fault_xbox_va is in GPU register
 * space (0xFD000000-0xFDFFFFFF).
 *
 * Decodes the faulting instruction, performs the MMIO read/write
 * through the NV2A register handlers, updates CPU context, and
 * returns true if handled successfully.
 *
 * Returns false if the instruction couldn't be decoded (caller
 * should fall back to allocating a zero page).
 */
#if defined(_WIN32)
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);
#endif

/*
 * Handle a GPU framebuffer/push buffer access.
 *
 * For 0xF0000000-0xFCFFFFFF range. Currently just allocates
 * pages backed by NV2A VRAM where appropriate.
 *
 * Returns true if handled.
 */
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va);

#endif /* BURNOUT3_NV2A_MMIO_HOOK_H */
