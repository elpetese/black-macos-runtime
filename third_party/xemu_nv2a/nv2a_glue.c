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
#include <time.h>

/* getenv once per call site. These run per pushbuffer method -- millions of
 * times a second -- and each getenv walks the environment under a lock: 12% of
 * the GPU thread in Black's first level. The environment does not change
 * after startup. */
#define CGETENV(name) __extension__ ({ static int _ce_done; static const char *_ce_val; \
    if (!_ce_done) { _ce_val = getenv(name); _ce_done = 1; } _ce_val; })
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
static struct {
    unsigned draws, ztest, zwrite, zbound, aa, w, h, zpitch, zfmt;
    uint32_t c0, setupraster, zclipmin, zclipmax, zoff, coff;
    unsigned zfunc[8], clears, zclears;
    uint32_t clear_param, zclear_value;
} s_z;

/* RECOMP_XEMU_DRAWLOG=<flip> logs that frame's draws; =L<n> logs the n-th
 * frame with more than 300 draws, which lands in gameplay whatever the time
 * the menus took. */
static unsigned s_frame_draws, s_big_frames;
/* The runtime's side of GPU software methods (xbox_memory_layout.c). Weak, so
 * the renderer still links into a host that does not provide it. */
extern int xbox_Nv2aSoftwareInterrupt(uint32_t trapped_addr, uint32_t trapped_data,
                                      uint32_t nsource) __attribute__((weak));
extern uint32_t xbox_ContiguousAllocatedBytes(void);

static int drawlog_this_frame(void)
{
    const char *e = CGETENV("RECOMP_XEMU_DRAWLOG");
    if (!e)
        return 0;
    if (*e == 'L')
        return s_big_frames == (unsigned)atoi(e + 1);
    return (int)s_flips == atoi(e);
}

static int drawlog_active(void)
{
    static int cached = -1;
    const char *e;
    if (cached < 0)
        cached = CGETENV("RECOMP_XEMU_DRAWLOG") != NULL;
    if (!cached)
        return 0;
    e = CGETENV("RECOMP_XEMU_DRAWLOG");
    return drawlog_this_frame();
}

static void complete_flip(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t s = pgraph_reg_r(pg, NV_PGRAPH_SURFACE);

    /* The flip completes at the next vertical blank. On hardware FLIP_STALL
     * holds the GPU until then, and that stall -- D3D's pushbuffer filling up
     * behind it -- is what paces a title to 60 Hz. Completing it at once let
     * Black's menus and videos run at ~260 frames/s. RECOMP_FLIP_UNCAPPED=1
     * restores the old behaviour. */
    {
        static int uncapped = -1;
        static int64_t next_us;
        struct timespec ts;
        int64_t now_us;

        if (uncapped < 0)
            uncapped = CGETENV("RECOMP_FLIP_UNCAPPED") != NULL;
        if (!uncapped) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            if (next_us == 0 || now_us - next_us > 50000)
                next_us = now_us;          /* first flip, or fell far behind */
            if (next_us > now_us) {
                struct timespec w = { (time_t)((next_us - now_us) / 1000000),
                                      (long)((next_us - now_us) % 1000000) * 1000 };
                qemu_mutex_unlock(&pg->lock);
                nanosleep(&w, NULL);
                qemu_mutex_lock(&pg->lock);
            }
            next_us += 16683;              /* 59.94 Hz, NTSC */
        }
    }

    SET_MASK(s, NV_PGRAPH_SURFACE_READ_3D, GET_MASK(s, NV_PGRAPH_SURFACE_WRITE_3D));
    pgraph_reg_w(pg, NV_PGRAPH_SURFACE, s);
    pg->waiting_for_flip = false;
    s_flips++;
    if (s_frame_draws > 300)
        s_big_frames++;
    s_frame_draws = 0;
    {
        static int shown;
        if (shown < 6 && CGETENV("RECOMP_XEMU_VERBOSE")) {
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
    if (CGETENV("RECOMP_XEMU_ZLOG")) {
        if ((s_flips % 60) == 0)
            fprintf(stderr, "  [XZ] flip %u: %u draws, %u z-test, %u z-write, %u with zeta bound;"
                    " last: control0=%08X setupraster=%08X zclip=[%08X,%08X] zeta off=%08X pitch=%u fmt=%u"
                    " color off=%08X aa=%u clip %ux%u\n",
                    s_flips, s_z.draws, s_z.ztest, s_z.zwrite, s_z.zbound, s_z.c0,
                    s_z.setupraster, s_z.zclipmin, s_z.zclipmax, s_z.zoff, s_z.zpitch,
                    s_z.zfmt, s_z.coff, s_z.aa, s_z.w, s_z.h);
        if ((s_flips % 60) == 0)
            fprintf(stderr, "  [XZ]   zfunc never/less/eq/lequal/greater/ne/gequal/always ="
                    " %u %u %u %u %u %u %u %u; %u clears (%u with Z), last param %08X zclear %08X\n",
                    s_z.zfunc[0], s_z.zfunc[1], s_z.zfunc[2], s_z.zfunc[3], s_z.zfunc[4],
                    s_z.zfunc[5], s_z.zfunc[6], s_z.zfunc[7], s_z.clears, s_z.zclears,
                    s_z.clear_param, s_z.zclear_value);
        memset(&s_z, 0, sizeof s_z);
    }
    pgraph_gl_download_dirty_surfaces(d);
    if (CGETENV("RECOMP_XEMU_VERBOSE")) {
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

    if (CGETENV("RECOMP_XEMU_VERBOSE") && method >= 0x1B04 && method < 0x1C00
        && ((method - 0x1B04) % 0x40) == 0 && ((param >> 4) & 0xF) == 0) {
        static int shown;
        if (shown++ < 10)
            fprintf(stderr, "  [XNV2A] SET_TEXTURE_FORMAT stage %u param 0x%08X (dim 0) avail %zu inc %d"
                    " next 0x%08X 0x%08X\n", (method - 0x1B04) / 0x40, param, avail, inc,
                    avail > 1 ? params[1] : 0, avail > 2 ? params[2] : 0);
    }
    /* RECOMP_XEMU_SKIP_COMPOSITE (debugging): drop full-screen draws made
     * without a depth buffer during gameplay -- Black's blurred-copy
     * composite -- to see the scene as the draws before it left it. */
    {
        static int skipping;
        if (method == 0x17FC && CGETENV("RECOMP_XEMU_SKIP_COMPOSITE")) {
            PGRAPHState *pg = &d->pgraph;
            if (param != 0 && s_big_frames > 0 && pg->surface_zeta.offset == 0
                && pg->surface_shape.clip_width == 640)
                skipping = 1;
            else if (param == 0 && skipping) {
                skipping = 0;
                return 1;
            }
        }
        if (skipping)
            return 1;
    }
    /* In the logged frame, the blend and depth methods as they arrive. */
    if (drawlog_active() && subch == 0
        && (method == 0x304 || method == 0x344 || method == 0x348 || method == 0x350
            || method == 0x30C || method == 0x354 || method == 0x35C))
        fprintf(stderr, "  [XM] %04X = %08X\n", method, param);
    qemu_mutex_lock(&d->pgraph.lock);
    n = pgraph_method(d, subch, method, param, params, avail, avail, inc != 0);
    /* RECOMP_XEMU_DRAWLOG=<flip>: PGRAPH's texture/shader state at each draw
     * of that frame, as xemu sees it (all four stages). */
    if (method == 0x17FC && param != 0)
        s_frame_draws++;
    if (method == 0x17FC && param == 0 && drawlog_this_frame()) {
        PGRAPHState *pg = &d->pgraph;
        static int idx;
        fprintf(stderr, "  [XDRAW %d] color=%08X zeta=%08X c0=%08X clip %ux%u shaderprog=%08X blend=%08X combctl=%08X",
                idx++, pg->surface_color.offset, pg->surface_zeta.offset,
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                pg->surface_shape.clip_width, pg->surface_shape.clip_height,
                pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG),
                pgraph_reg_r(pg, NV_PGRAPH_BLEND), pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL));
        for (int t = 0; t < 4; t++)
            fprintf(stderr, " | t%d en=%d off=%08X fmt=%08X", t,
                    (pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + t * 4) >> 30) & 1,
                    pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + t * 4),
                    pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + t * 4));
        fprintf(stderr, "\n        attrs:");
        for (int k = 0; k < NV2A_VERTEXSHADER_ATTRIBUTES; k++) {
            VertexAttribute *va = &pg->vertex_attributes[k];
            if (va->count)
                fprintf(stderr, " a%d=%c:%08X/%u", k, va->dma_select ? 'B' : 'A',
                        (uint32_t)va->offset, va->stride);
        }
        fprintf(stderr, " dmaA=%08X dmaB=%08X contig_used=%08X",
                (uint32_t)pg->dma_vertex_a, (uint32_t)pg->dma_vertex_b, xbox_ContiguousAllocatedBytes());
        fprintf(stderr, "\n        comb: ci0=%08X co0=%08X ai0=%08X ao0=%08X f0=%08X sf0=%08X sf1=%08X ci1=%08X ai1=%08X\n",
                pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0), pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORO0),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0), pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAO0),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0), pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1), pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + 4),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + 4));
    }
    /* RECOMP_XEMU_ZLOG: per frame, how the draws use depth -- how many test
     * it, how many write it, and whether a zeta surface was bound -- printed
     * every 60 flips. */
    if (method == 0x17FC && param != 0 && CGETENV("RECOMP_XEMU_ZLOG")) {
        PGRAPHState *pg = &d->pgraph;
        PGRAPHGLState *r = pg->gl_renderer_state;
        uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
        s_z.draws++;
        if (c0 & NV_PGRAPH_CONTROL_0_ZENABLE)
            s_z.zfunc[(c0 >> 16) & 7]++;
        if (c0 & NV_PGRAPH_CONTROL_0_ZENABLE) s_z.ztest++;
        if (c0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE) s_z.zwrite++;
        if (r && r->zeta_binding) s_z.zbound++;
        s_z.c0 = c0;
        s_z.zoff = pg->surface_zeta.offset;
        s_z.zpitch = pg->surface_zeta.pitch;
        s_z.zfmt = pg->surface_shape.zeta_format;
        s_z.coff = pg->surface_color.offset;
        s_z.aa = pg->surface_shape.anti_aliasing;
        s_z.w = pg->surface_shape.clip_width;
        s_z.h = pg->surface_shape.clip_height;
        s_z.setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
        s_z.zclipmax = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMAX);
        s_z.zclipmin = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMIN);
    }
    if (method == 0x1D94 && CGETENV("RECOMP_XEMU_ZLOG")) {
        s_z.clears++;
        if (param & 1)
            s_z.zclears++;
        s_z.clear_param = param;
        s_z.zclear_value = pgraph_reg_r(&d->pgraph, NV_PGRAPH_ZSTENCILCLEARVALUE);
    }
    /* A software method -- NO_OPERATION with a parameter. D3D uses these to be
     * called back by the GPU: fence wake-ups (5), swap bookkeeping (1),
     * InsertCallback (12, 13). On hardware PFIFO stalls, the title's GPU ISR
     * reads TRAPPED_ADDR/DATA and acknowledges. Hand it over the same way
     * when the runtime emulates the interrupt registers; the pushbuffer waits
     * here until the title has handled it. Dropping them, as this used to,
     * left a D3D wait on the fence event with nothing to end it. */
    if ((d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR) && method == 0x100
        && xbox_Nv2aSoftwareInterrupt) {
        PGRAPHState *pg = &d->pgraph;
        uint32_t ta = pgraph_reg_r(pg, NV_PGRAPH_TRAPPED_ADDR);
        uint32_t td = pgraph_reg_r(pg, NV_PGRAPH_TRAPPED_DATA_LOW);
        uint32_t ns = pgraph_reg_r(pg, NV_PGRAPH_NSOURCE);
        int handled;

        qemu_mutex_unlock(&pg->lock);
        handled = xbox_Nv2aSoftwareInterrupt(ta, td, ns);
        qemu_mutex_lock(&pg->lock);
        if (handled) {
            pg->pending_interrupts &= ~NV_PGRAPH_INTR_ERROR;
            pg->waiting_for_nop = false;
        }
    }
    /* Anything else nothing here services -- on hardware the title's driver
     * acknowledges it from its ISR -- is acknowledged now, and the method
     * that raised an error named once. */
    if (d->pgraph.pending_interrupts) {
        d->pgraph.waiting_for_nop = false;
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

/* Frames presented so far; the hang monitor's sign of life. */
unsigned xnv2a_flip_count(void)
{
    return s_flips;
}

void xnv2a_report(void)
{
    fprintf(stderr, "[XNV2A] %llu methods, %llu RAMHT misses\n",
            (unsigned long long)s_methods, (unsigned long long)s_ramht_misses);
}
