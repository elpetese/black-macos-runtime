/* xboxrecomp glue for xemu's NV2A renderer.
 *
 * xemu drives PGRAPH from its PFIFO puller thread, with guest memory, RAMIN
 * and the PFIFO registers owned by its PCI device model. Here the title is
 * recompiled code running natively: its pushbuffer is walked by
 * src/kernel/nv2a_pb_scan.c, its RAMIN and PFIFO registers are the plain RAM
 * backing the NV2A aperture at 0xFD000000, and its GPU-visible memory is the
 * contiguous window. This file stands in for the parts of xemu's device model
 * (nv2a.c, pfifo.c) that PGRAPH calls, and exposes a small API (xnv2a_*) to
 * the runtime. See xnv2a.h.
 */
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"
#include "qemu/bitmap.h"
#include "xnv2a.h"
#include <SDL3/SDL.h>

struct config g_config = {
    .display = { .renderer = CONFIG_DISPLAY_RENDERER_OPENGL, .quality = { .surface_scale = 1 } },
    .perf = { .cache_shaders = false },
};

const char *xemu_settings_get_base_path(void) { return "/tmp/"; }

/* -- the pieces of qemu/util and xemu's device model PGRAPH links against -- */

void bitmap_set(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++)
        map[i / (8 * sizeof(long))] |= 1ul << (i % (8 * sizeof(long)));
}

/* Interrupts are the title's business through the runtime, not PGRAPH's. */
void nv2a_update_irq(NV2AState *d) { (void)d; }
void pfifo_kick(NV2AState *d) { (void)d; }

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);

    dma.address &= 0x07FFFFFF;
    if (dma.address >= memory_region_size(d->vram))
        dma.address = 0;
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

static const uint8_t *s_aperture;          /* host view of 0xFD000000 */

/* nv2a.c's tile clip, reading the PFB tile registers where the title wrote
 * them: the PFB block of the aperture (+0x100000). */
hwaddr nv_clip_gpu_tile_blit(NV2AState *d, hwaddr blit_base_address, hwaddr len)
{
    hwaddr blit_end = blit_base_address + len;
    (void)d;
    for (int i = 0; i < NV_NUM_GPU_TILES; ++i) {
        uint32_t base_and_flags =
            ldl_le_p(s_aperture + 0x100000 + NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i));
        if (!(base_and_flags & NV_PFB_TILE_FLAGS_VALID))
            continue;
        uint32_t limit = ldl_le_p(s_aperture + 0x100000 + NV_PFB_TILE_LIMIT(i));
        if (blit_base_address < limit && blit_end > limit)
            return limit + 1 - blit_base_address;
    }
    return len;
}

/* -- RAMHT, as pfifo.c looks it up -- */

typedef struct RAMHTEntry {
    uint32_t handle;
    hwaddr instance;
    unsigned int engine, channel_id;
    bool valid;
} RAMHTEntry;

static uint32_t pfifo_reg(uint32_t r)
{
    return ldl_le_p(s_aperture + 0x2000 + r);
}

static RAMHTEntry ramht_lookup(NV2AState *d, uint32_t handle)
{
    uint32_t ramht = pfifo_reg(NV_PFIFO_RAMHT);
    hwaddr size = 1u << (GET_MASK(ramht, NV_PFIFO_RAMHT_SIZE) + 12);
    unsigned int bits = ctz32(size) - 1;
    uint32_t hash = 0, h = handle;

    while (h) {
        hash ^= (h & ((1u << bits) - 1));
        h >>= bits;
    }
    hash ^= GET_MASK(pfifo_reg(NV_PFIFO_CACHE1_PUSH1), NV_PFIFO_CACHE1_PUSH1_CHID)
            << (bits - 4);

    hwaddr base = (hwaddr)GET_MASK(ramht, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    if (hash * 8 >= size || base + hash * 8 + 8 > memory_region_size(&d->ramin))
        return (RAMHTEntry){ .valid = false };

    const uint8_t *e = d->ramin_ptr + base + hash * 8;
    uint32_t ctx = ldl_le_p(e + 4);
    return (RAMHTEntry){
        .handle     = ldl_le_p(e),
        .instance   = (ctx & NV_RAMHT_INSTANCE) << 4,
        .engine     = (ctx & NV_RAMHT_ENGINE) >> 16,
        .channel_id = (ctx & NV_RAMHT_CHID) >> 24,
        .valid      = (ctx & NV_RAMHT_STATUS) != 0,
    };
}

/* -- the runtime-facing API -- */

static NV2AState *s_d;
static bool s_thread_ready;
static uint64_t s_methods, s_ramht_misses;
static unsigned s_flips;

void xnv2a_context_init_main_thread(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO) && *SDL_GetError())
        fprintf(stderr, "  [XNV2A] SDL video init failed: %s\n", SDL_GetError());
    nv2a_context_init();
}

void xnv2a_init(uint8_t *vram, size_t vram_size, uint8_t *aperture)
{
    NV2AState *d = g_new0(NV2AState, 1);
    PGRAPHState *pg = &d->pgraph;

    s_aperture = aperture;
    d->vram = g_new0(MemoryRegion, 1);
    d->vram->ptr = vram;
    d->vram->size = vram_size;
    d->vram_ptr = vram;
    d->ramin.ptr = aperture + 0x700000;
    d->ramin.size = 0x100000;
    d->ramin_ptr = d->ramin.ptr;
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    pgraph_init(d);

    /* One channel, always current, FIFO access on: the context switch the
     * title's driver would service from an interrupt has nothing to do. */
    pgraph_reg_w(pg, NV_PGRAPH_CTX_CONTROL,
                 pgraph_reg_r(pg, NV_PGRAPH_CTX_CONTROL) | NV_PGRAPH_CTX_CONTROL_CHID);
    pgraph_reg_w(pg, NV_PGRAPH_FIFO, NV_PGRAPH_FIFO_ACCESS);
    s_d = d;
}

static void thread_init(void)
{
    if (s_thread_ready)
        return;
    pgraph_init_thread(s_d);
    s_thread_ready = true;
    fprintf(stderr, "  [XNV2A] xemu NV2A renderer (%s) running\n",
            s_d->pgraph.renderer ? s_d->pgraph.renderer->name : "none");
}

/* The flip completes as soon as it is asked for, as the runtime's own
 * executor already does: advance the read index to the write index, and put
 * what was rendered into guest memory, where the framebuffer window reads. */
static void complete_flip(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t s = pgraph_reg_r(pg, NV_PGRAPH_SURFACE);

    SET_MASK(s, NV_PGRAPH_SURFACE_READ_3D, GET_MASK(s, NV_PGRAPH_SURFACE_WRITE_3D));
    pgraph_reg_w(pg, NV_PGRAPH_SURFACE, s);
    pg->waiting_for_flip = false;
    s_flips++;
    {
        static int shown;
        if (shown < 6 && getenv("RECOMP_XEMU_VERBOSE")) {
            PGRAPHGLState *r = pg->gl_renderer_state;
            SurfaceBinding *sb;
            shown++;
            fprintf(stderr, "  [XNV2A] flip: surfaces:");
            QTAILQ_FOREACH(sb, &r->surfaces, entry)
                fprintf(stderr, " [vram 0x%08" PRIx64 " %ux%u pitch %u color %d dirty %d]",
                        (uint64_t)sb->vram_addr, sb->width, sb->height, sb->pitch,
                        sb->color, sb->draw_dirty);
            fprintf(stderr, "\n");
        }
    }
    pgraph_gl_download_dirty_surfaces(d);
    if (getenv("RECOMP_XEMU_VERBOSE")) {
        static unsigned flips;
        if ((flips++ % 100) == 0) {
            fprintf(stderr, "  [XNV2A] flip %u counters:", flips);
            for (unsigned i = 0; i < NV2A_PROF__COUNT; i++) {
                int v = nv2a_profile_get_counter_value(i);
                if (v)
                    fprintf(stderr, " %s=%d", nv2a_profile_get_counter_name(i), v);
            }
            fprintf(stderr, "\n");
        }
    }
}

int xnv2a_method(uint32_t subch, uint32_t method, uint32_t param,
                 uint32_t *params, size_t avail, int inc)
{
    NV2AState *d = s_d;
    int n;

    if (!d)
        return 1;
    thread_init();
    s_methods++;

    if (method == 0) {
        RAMHTEntry e = ramht_lookup(d, param);
        if (!e.valid) {
            s_ramht_misses++;
            return 1;
        }
        param = (uint32_t)e.instance;
    } else if (method >= 0x180 && method < 0x200) {
        RAMHTEntry e = ramht_lookup(d, param);
        if (e.valid)
            param = (uint32_t)e.instance;
        else
            s_ramht_misses++;
    }

    if (getenv("RECOMP_XEMU_VERBOSE") && method >= 0x1B04 && method < 0x1C00
        && ((method - 0x1B04) % 0x40) == 0 && ((param >> 4) & 0xF) == 0) {
        static int shown;
        if (shown++ < 10)
            fprintf(stderr, "  [XNV2A] SET_TEXTURE_FORMAT stage %u param 0x%08X (dim 0) avail %zu inc %d"
                    " next 0x%08X 0x%08X\n", (method - 0x1B04) / 0x40, param, avail, inc,
                    avail > 1 ? params[1] : 0, avail > 2 ? params[2] : 0);
    }
    qemu_mutex_lock(&d->pgraph.lock);
    n = pgraph_method(d, subch, method, param, params, avail, avail, inc != 0);
    /* RECOMP_XEMU_DRAWLOG=<flip>: PGRAPH's texture/shader state at each draw
     * of that frame, as xemu sees it (all four stages). */
    if (method == 0x17FC && param == 0 && getenv("RECOMP_XEMU_DRAWLOG")
        && (int)s_flips == atoi(getenv("RECOMP_XEMU_DRAWLOG"))) {
        PGRAPHState *pg = &d->pgraph;
        static int idx;
        fprintf(stderr, "  [XDRAW %d] shaderprog=%08X blend=%08X combctl=%08X",
                idx++, pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG),
                pgraph_reg_r(pg, NV_PGRAPH_BLEND), pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL));
        for (int t = 0; t < 4; t++)
            fprintf(stderr, " | t%d en=%d off=%08X fmt=%08X", t,
                    (pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + t * 4) >> 30) & 1,
                    pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + t * 4),
                    pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + t * 4));
        fprintf(stderr, "\n");
    }
    /* Nothing services PGRAPH interrupts here -- on hardware the title's
     * driver acknowledges them from its ISR -- so acknowledge them now, and
     * say which method raised an error, once per method. */
    if (d->pgraph.pending_interrupts) {
        if (d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR) {
            static uint32_t seen[64]; static int nseen;
            int k;
            for (k = 0; k < nseen && seen[k] != (method | (subch << 16)); k++) ;
            if (k == nseen && nseen < 64) {
                seen[nseen++] = method | (subch << 16);
                fprintf(stderr, "  [XNV2A] PGRAPH error on subch %u method 0x%04X param 0x%08X"
                        " (class 0x%02X)\n", subch, method, param,
                        pgraph_reg_r(&d->pgraph, NV_PGRAPH_CTX_SWITCH1) & 0xFF);
            }
        }
        d->pgraph.pending_interrupts = 0;
    }
    if (d->pgraph.waiting_for_flip)
        complete_flip(d);
    pgraph_process_pending(d);
    qemu_mutex_unlock(&d->pgraph.lock);
    return n > 0 ? n : 1;
}

void xnv2a_report(void)
{
    fprintf(stderr, "[XNV2A] %llu methods, %llu RAMHT misses\n",
            (unsigned long long)s_methods, (unsigned long long)s_ramht_misses);
}
