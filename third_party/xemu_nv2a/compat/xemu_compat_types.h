/* xboxrecomp: the few QEMU device/memory types xemu's NV2A renderer names.
 * NV2AState embeds PCI/VGA/timer members it never touches in the renderer
 * path; they only need to exist. Memory regions are plain host buffers. */
#ifndef XEMU_COMPAT_TYPES_H
#define XEMU_COMPAT_TYPES_H
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include <sys/stat.h>
#include <time.h>

typedef struct Error { char *msg; } Error;
static inline const char *error_get_pretty(const Error *e) { return e && e->msg ? e->msg : "error"; }
static inline void error_free(Error *e) { if (e) { g_free(e->msg); g_free(e); } }
#define error_setg(errp, ...) do { if (errp) { *(errp) = g_new0(Error, 1);     (*(errp))->msg = g_strdup_printf(__VA_ARGS__); } } while (0)
#define error_propagate(dst, src) do { if (dst) *(dst) = (src); else error_free(src); } while (0)
#define qemu_build_assert(x) _Static_assert((x), "qemu_build_assert")
typedef struct Object { int unused; } Object;
typedef struct CPUState { int unused; } CPUState;
typedef struct DeviceState { int unused; } DeviceState;
typedef struct PCIDevice { int unused; } PCIDevice;
typedef struct QEMUTimer { int unused; } QEMUTimer;
typedef struct QemuConsole QemuConsole;
typedef struct GraphicHwOps { int unused; } GraphicHwOps;
typedef struct PCIBus PCIBus;
typedef struct VGADisplayParams { uint32_t line_offset, start_addr, line_compare; } VGADisplayParams;
typedef struct VGACommonState {
    uint8_t *vram_ptr;
    uint8_t cr[256];
    void (*get_resolution)(struct VGACommonState *s, int *w, int *h);
    void (*get_params)(struct VGACommonState *s, VGADisplayParams *p);
} VGACommonState;
/* Guest-memory access callbacks: xemu uses them to learn that the CPU touched
 * a surface. There is no such hook on direct guest RAM here. */
typedef struct MemAccessCallback MemAccessCallback;
static inline MemAccessCallback *mem_access_callback_insert(CPUState *c, void *mr, hwaddr a, hwaddr l,
                                                           void *cb, void *opaque)
{ (void)c; (void)mr; (void)a; (void)l; (void)cb; (void)opaque; return NULL; }
static inline void mem_access_callback_remove_by_ref(CPUState *c, MemAccessCallback *cb)
{ (void)c; (void)cb; }
typedef void *qemu_irq;
typedef struct VMStateDescription { int unused; } VMStateDescription;

typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t data, unsigned size);
} MemoryRegionOps;

typedef struct MemoryRegion {
    uint8_t *ptr;
    uint64_t size;
} MemoryRegion;

#define DIRTY_MEMORY_VGA        0
#define DIRTY_MEMORY_CODE       1
#define DIRTY_MEMORY_MIGRATION  2
#define DIRTY_MEMORY_NV2A       3
#define DIRTY_MEMORY_NV2A_TEX   4

static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline void *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ptr; }
/* No write tracking on guest RAM here: report everything as changed and let
 * the renderer's content hashes decide what actually needs re-uploading. */
static inline bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr a,
                                                      hwaddr s, unsigned c)
{ (void)mr; (void)a; (void)s; (void)c; return true; }
static inline bool memory_region_get_dirty(MemoryRegion *mr, hwaddr a, hwaddr s, unsigned c)
{ (void)mr; (void)a; (void)s; (void)c; return true; }
static inline void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr a,
                                                  hwaddr s, unsigned c)
{ (void)mr; (void)a; (void)s; (void)c; }
static inline void memory_region_set_dirty(MemoryRegion *mr, hwaddr a, hwaddr s)
{ (void)mr; (void)a; (void)s; }

#define OBJECT_CHECK(type, obj, name) ((type *)(obj))

static inline CPUState *qemu_get_cpu(int i) { (void)i; static CPUState c; return &c; }
static inline void cpu_exit(CPUState *c) { (void)c; }
typedef union run_on_cpu_data { void *host_ptr; int host_int; unsigned long target_ptr; } run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){ .host_ptr = (p) })
#define RUN_ON_CPU_NULL        ((run_on_cpu_data){ .host_ptr = NULL })
/* The guest CPU is the recompiled code on its own threads; there is no vCPU
 * to schedule work on, so the work runs now. */
static inline void async_safe_run_on_cpu(CPUState *c, void (*fn)(CPUState *, run_on_cpu_data),
                                         run_on_cpu_data d) { fn(c, d); }
static inline void async_run_on_cpu(CPUState *c, void (*fn)(CPUState *, run_on_cpu_data),
                                    run_on_cpu_data d) { fn(c, d); }
/* xemu runs the guest under TCG, and its surface code keys off that: with TCG
 * it learns about CPU writes to a surface from access callbacks instead of
 * re-uploading the surface from RAM before every draw, which here would throw
 * away each draw as soon as the next one began. */
static inline bool tcg_enabled(void) { return true; }

enum { QEMU_CLOCK_REALTIME, QEMU_CLOCK_VIRTUAL, QEMU_CLOCK_HOST, QEMU_CLOCK_VIRTUAL_RT };
static inline int64_t qemu_clock_get_ns(int t) {
    struct timespec ts; (void)t; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static inline int64_t qemu_clock_get_us(int t) { return qemu_clock_get_ns(t) / 1000; }
static inline int64_t qemu_clock_get_ms(int t) { return qemu_clock_get_ns(t) / 1000000; }
static inline int64_t get_clock_realtime(void) { return qemu_clock_get_ns(0); }

static inline FILE *qemu_fopen(const char *p, const char *m) { return fopen(p, m); }
static inline int qemu_mkdir(const char *p) { return mkdir(p, 0755); }
static inline int qemu_unlink(const char *p) { return unlink(p); }

/* xemu UI hooks the renderer calls; stderr is the UI here. */
static inline void xemu_queue_notification(const char *msg) { fprintf(stderr, "[nv2a] %s\n", msg); }
static inline void xemu_queue_error_message(const char *msg) { fprintf(stderr, "[nv2a] error: %s\n", msg); }
const char *xemu_settings_get_base_path(void);

#endif
