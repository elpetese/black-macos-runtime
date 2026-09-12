/*
 * test_guest_layout.c - checks the POSIX platform layer can build the Xbox
 * guest address space the way xbox_MemoryLayoutInit does on macOS:
 * one 4 GB reservation, RAM + mirrors + tiled alias as views of one shared
 * object, separate regions for the contiguous window and the MMIO apertures.
 *
 * Build and run (from the repo root, after building the `platform` target):
 *   cc -O1 -Isrc -Isrc/platform -Isrc/kernel \
 *      tests/guest_layout/test_guest_layout.c \
 *      build-macos/src/platform/libplatform.a -lpthread -lm \
 *      -o /tmp/test_guest_layout
 *   /tmp/test_guest_layout
 */
#include "win32_compat.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define MB (1024u * 1024u)
static int fails;
#define CHECK(cond, ...) do { if (cond) printf("  OK    " __VA_ARGS__); \
                               else { printf("  FAIL  " __VA_ARGS__); fails++; } \
                               printf("\n"); } while (0)


int main(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    printf("host page size: %ld bytes\n", ps);

    uint8_t *g = (uint8_t *)win32_compat_reserve_guest_space((SIZE_T)1 << 32);
    CHECK(g != NULL, "reserve 4 GB guest space at %p", (void *)g);
    if (!g) return 1;

    HANDLE ram = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, 64 * MB, NULL);
    CHECK(ram != NULL, "CreateFileMapping 64 MB (anonymous shared object)");
    if (!ram) return 1;

    uint8_t *base   = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, g);
    uint8_t *mirror = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, g + 64 * MB);
    uint8_t *tiled  = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, g + 0xF0000000u);
    CHECK(base == g,                    "RAM view at guest 0x00000000");
    CHECK(mirror == g + 64 * MB,        "mirror view at guest 0x04000000");
    CHECK(tiled == g + 0xF0000000u,     "tiled view at guest 0xF0000000");

    if (base && mirror && tiled) {
        *(volatile uint32_t *)(base + 0x10448) = 0xB1ACB1ACu;
        CHECK(*(volatile uint32_t *)(mirror + 0x10448) == 0xB1ACB1ACu,
              "write via RAM is visible via mirror (true alias)");
        *(volatile uint32_t *)(tiled + 0x2000) = 0x12345678u;
        CHECK(*(volatile uint32_t *)(base + 0x2000) == 0x12345678u,
              "write via tiled aperture is visible via RAM");
    }

    struct { const char *name; uint32_t va; uint32_t size; } regions[] = {
        { "contiguous window", 0x80000000u, 64 * MB },
        { "NV2A aperture",     0xFD000000u, 16 * MB },
        { "MCPX registers",    0xFE800000u,  8 * MB },
        { "flash",             0xFF000000u,  1 * MB },
    };
    for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        void *want = g + regions[i].va;
        void *p = VirtualAlloc(want, regions[i].size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(p == want, "%s at guest 0x%08X", regions[i].name, regions[i].va);
        if (p) { memset(p, 0xA5, 64);
                 CHECK(((uint8_t *)p)[63] == 0xA5, "%s is writable", regions[i].name); }
    }

    /* ---- Contiguous window is private storage, NOT an alias (Phase 6) ----
     *
     * Guest VAs are identity-mapped here, so guest 0x000F4000 is the title's
     * own .text. If the window folded bit 31 onto low RAM, a title clearing
     * its contiguous block through 0x800F4000 would erase its own code -- which
     * is exactly what happened to Black before this contract was pinned down.
     * A bit-31 read that returns zeros means nothing filled that block, not
     * that the mapping is missing.
     */
    {
        uint32_t va_low  = 0x000F9FB8u;
        uint32_t va_phys = 0x800F9FB8u;   /* the bit-31 "physical" form */
        uint32_t value   = 0xDEADBEEFu;

        *(volatile uint32_t *)(g + va_low)  = value;
        *(volatile uint32_t *)(g + va_phys) = ~value;
        CHECK(*(volatile uint32_t *)(g + va_low) == value,
              "write 0x800F9FB8 does NOT disturb 0x000F9FB8 (window is private)");
        CHECK(*(volatile uint32_t *)(g + va_phys) == ~value,
              "the window keeps its own bytes at 0x800F9FB8");
        *(volatile uint32_t *)(g + va_low) = 0;

        /* The fake kernel PE page must likewise stay off the XBE header. */
        *(volatile uint32_t *)(g + 0x8001003Cu) = value;
        CHECK(*(volatile uint32_t *)(g + 0x0001003Cu) != value,
              "write 0x8001003C does NOT alter 0x0001003C (PE page isolated from XBE header)");
    }

    /* The guest thinks in 4 KB pages; protecting one must not fail on a
     * 16 KB-page host (it widens to the enclosing host page instead). */
    DWORD old;
    CHECK(VirtualProtect(base + 0x1000, 0x1000, PAGE_READONLY, &old),
          "VirtualProtect on a 4 KB guest page (widened to host page)");
    VirtualProtect(base, 0x10000, PAGE_READWRITE, &old);

    /* Outside the reservation an address is only a hint: asking for one that
     * is already taken (here, this thread's stack) must fail, never clobber. */
    static char probe;
    void *taken = (void *)((uintptr_t)&old & ~(uintptr_t)(ps - 1));
    void *clash = VirtualAlloc(taken, (SIZE_T)ps, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(clash == NULL, "VirtualAlloc on an occupied address outside the reserve fails");
    (void)probe;

    MEMORYSTATUSEX ms; memset(&ms, 0, sizeof ms);
    CHECK(GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0,
          "GlobalMemoryStatusEx: %llu MB total", (unsigned long long)(ms.ullTotalPhys / MB));

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
