/* xbox_dirty_tracking.c -- implementation (see xbox_dirty_tracking.h).
 *
 * Page-granular write tracking for guest VRAM via mprotect(RO) + fault.
 * Armed only under RECOMP_XEMU_GPU=1 + RECOMP_DIRTY_TRACKING=1; otherwise all
 * accessors are no-ops / report-dirty and the boot path is unaffected.
 *
 * Three things a naive version gets wrong, and this one handles:
 *
 *  - Aliases. The contiguous window is also mapped at 0xF0000000 (the tiled
 *    aperture), and D3D's LockRect hands out addresses there. Protecting one
 *    view only would miss every write through the other, so both views of a
 *    page are protected and unprotected together.
 *  - Writes that do not fault. A read() into a protected page fails with
 *    EFAULT instead of trapping, and a GL readback may be done by the driver.
 *    Callers that write guest VRAM that way call xbox_dirty_prepare_write()
 *    first: the range is marked dirty and made writable.
 *  - The re-arm race. A page's bit is cleared and THEN the page is
 *    re-protected. A store between the two lands on the still-writable page
 *    and is included in the upload the caller does after this returns; a
 *    store after the protect faults and sets the bit again. The opposite
 *    order loses writes: a fault between protect and clear re-opens the page
 *    and then has its bit cleared, leaving a writable page nobody watches.
 *
 * The bitmap is updated with atomics: the fault handler runs on whichever
 * thread stored, concurrently with the renderer thread. */
#include "xbox_dirty_tracking.h"

#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct DirtyRegion {
    uint8_t *host_base;     /* page-aligned start of the tracked range     */
    uint8_t *alias_base;    /* second view of the same pages, or NULL      */
    size_t   size;          /* bytes, a multiple of page_size              */
    size_t   page_size;
    size_t   page_count;
    uint8_t *bitmap;        /* 1 bit per page: 1 = written since last test */
} DirtyRegion;

/* One region is all this runtime needs (the 64 MiB VRAM window). */
static DirtyRegion s_region;
static volatile int s_armed;

static size_t page_size(void)
{
    static long cached;
    if (!cached) {
        long v = sysconf(_SC_PAGESIZE);
        cached = v > 0 ? v : 4096;
    }
    return (size_t)cached;
}

/* Page index for a host address in either view, or -1. */
static long page_of(uintptr_t addr)
{
    uintptr_t lo = (uintptr_t)s_region.host_base;
    if (addr >= lo && addr < lo + s_region.size)
        return (long)((addr - lo) / s_region.page_size);
    if (s_region.alias_base) {
        uintptr_t al = (uintptr_t)s_region.alias_base;
        if (addr >= al && addr < al + s_region.size)
            return (long)((addr - al) / s_region.page_size);
    }
    return -1;
}

static void protect_page(size_t pg, int prot)
{
    size_t off = pg * s_region.page_size;
    (void)mprotect(s_region.host_base + off, s_region.page_size, prot);
    if (s_region.alias_base)
        (void)mprotect(s_region.alias_base + off, s_region.page_size, prot);
}

static void mark_page(size_t pg)
{
    __atomic_fetch_or(&s_region.bitmap[pg / 8], (uint8_t)(1u << (pg % 8)),
                      __ATOMIC_SEQ_CST);
}

int xbox_dirty_armed(const void *host_base, size_t size)
{
    (void)size;
    return s_armed && (const uint8_t *)host_base == s_region.host_base;
}

void xbox_dirty_arm(void *host_base, size_t size)
{
    size_t ps = page_size();
    size_t full, count;
    uint8_t *bitmap;

    if (s_armed || !host_base || !size || ((uintptr_t)host_base & (ps - 1))) {
        if (host_base && ((uintptr_t)host_base & (ps - 1)))
            fprintf(stderr, "[DIRTY] %p is not page aligned; not armed\n", host_base);
        return;
    }
    full = (size + ps - 1) & ~(ps - 1);
    count = full / ps;
    bitmap = (uint8_t *)calloc((count + 7) / 8, 1);
    if (!bitmap) {
        perror("[DIRTY] calloc bitmap");
        return;
    }
    s_region.host_base = (uint8_t *)host_base;
    s_region.size = full;
    s_region.page_size = ps;
    s_region.page_count = count;
    s_region.bitmap = bitmap;
    /* Every page starts dirty: nothing has been uploaded yet, and a
     * texture's first check must say so. The pages stay writable until a
     * check re-arms them, so startup does not fault on every page. */
    for (size_t i = 0; i < (count + 7) / 8; i++)
        bitmap[i] = 0xFF;
    __atomic_store_n(&s_armed, 1, __ATOMIC_SEQ_CST);
    fprintf(stderr, "[DIRTY] armed %zu pages (%.1f MiB) at %p (host page %zu)\n",
            count, full / (1024.0 * 1024.0), host_base, ps);
}

void xbox_dirty_add_alias(const void *host_base, void *alias_base)
{
    if (!s_armed || (const uint8_t *)host_base != s_region.host_base || !alias_base
        || ((uintptr_t)alias_base & (s_region.page_size - 1)))
        return;
    s_region.alias_base = (uint8_t *)alias_base;
    fprintf(stderr, "[DIRTY] alias view at %p tracked with the window\n", alias_base);
}

bool xbox_dirty_test_and_clear(const void *host_base, size_t region_size,
                               size_t offset, size_t size)
{
    size_t start, end, pg;
    bool dirty = false;

    if (!xbox_dirty_armed(host_base, region_size))
        return true;
    if (offset >= s_region.size)
        return true;
    if (size == 0)
        size = 1;
    start = offset / s_region.page_size;
    end = (offset + size - 1) / s_region.page_size;
    if (end >= s_region.page_count)
        end = s_region.page_count - 1;

    for (pg = start; pg <= end; pg++) {
        uint8_t mask = (uint8_t)(1u << (pg % 8));
        uint8_t old = __atomic_fetch_and(&s_region.bitmap[pg / 8], (uint8_t)~mask,
                                         __ATOMIC_SEQ_CST);   /* clear ...  */
        if (old & mask) {
            protect_page(pg, PROT_READ);                      /* ... re-arm */
            dirty = true;
        }
    }
    return dirty;
}

void xbox_dirty_set(const void *host_base, size_t region_size,
                    size_t offset, size_t size)
{
    if (!xbox_dirty_armed(host_base, region_size) || offset >= s_region.size)
        return;
    xbox_dirty_prepare_write(s_region.host_base + offset,
                             size < s_region.size - offset ? size : s_region.size - offset);
}

void xbox_dirty_prepare_write(void *host_ptr, size_t len)
{
    long first, last;
    uintptr_t a = (uintptr_t)host_ptr;

    if (!s_armed || len == 0)
        return;
    first = page_of(a);
    last = page_of(a + len - 1);
    if (first < 0 && last < 0)
        return;
    /* A range straddling the window's edge: clamp to the part inside it. */
    if (first < 0)
        first = 0;
    if (last < 0)
        last = (long)s_region.page_count - 1;
    for (long pg = first; pg <= last; pg++) {
        mark_page((size_t)pg);
        protect_page((size_t)pg, PROT_READ | PROT_WRITE);
    }
}

/* Async-signal-safe: an atomic byte OR and two mprotect calls. */
int xbox_dirty_fault(uintptr_t fault_addr)
{
    long pg;

    if (!s_armed)
        return 0;
    pg = page_of(fault_addr);
    if (pg < 0)
        return 0;
    mark_page((size_t)pg);
    protect_page((size_t)pg, PROT_READ | PROT_WRITE);
    return 1;
}
