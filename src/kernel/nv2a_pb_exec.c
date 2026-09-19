/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "../kernel/f7_instrumentation.h"
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_ALPHA_TEST_ENABLE       0x0300
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_ALPHA_FUNC              0x033C
#define NV097_SET_ALPHA_REF               0x0340
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV097_SET_BLEND_EQUATION          0x0350
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL0        0x1B0C
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
#define NV097_SET_TEXTURE_CONTROL0_ENABLE (1u << 30)
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
#define NV097_INLINE_ARRAY                0x1818

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

#define NV_BLEND_ZERO                     0x0000
#define NV_BLEND_ONE                      0x0001
#define NV_BLEND_SRC_COLOR                0x0300
#define NV_BLEND_ONE_MINUS_SRC_COLOR      0x0301
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV_BLEND_DST_ALPHA                0x0304
#define NV_BLEND_ONE_MINUS_DST_ALPHA      0x0305
#define NV_BLEND_DST_COLOR                0x0306
#define NV_BLEND_ONE_MINUS_DST_COLOR      0x0307
#define NV_BLEND_SRC_ALPHA_SATURATE       0x0308
#define NV_BLEND_FUNC_ADD                 0x8006
#define NV_BLEND_FUNC_SUBTRACT            0x800A
#define NV_BLEND_FUNC_REVERSE_SUBTRACT    0x800B

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096
#define NV_MAX_INLINE   4096            /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    int      enabled;
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t alpha_func, alpha_ref;
    uint32_t blend_sfactor, blend_dfactor, blend_equation;
    int alpha_test_enable, blend_enable;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t tex_invalid_enabled, tex_invalid_offset, tex_invalid_size, tex_invalid_format;
    Texture  tex;
} s_gpu;

void pb_exec_get_surface(uint32_t *out_offset, uint32_t *out_pitch)
{
    if (out_offset) *out_offset = s_gpu.color_offset;
    if (out_pitch) *out_pitch = s_gpu.pitch;
}

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static int tex_bytes_per_pixel(uint32_t fmt)
{
    switch (fmt) {
    case 0x10: case 0x11: case 0x1C: case 0x1D: return 2;
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    default: return 0;
    }
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    if (method == NV097_SET_TEXTURE_CONTROL1)
        s_gpu.tex.pitch = param >> 16;
    if (!s_gpu.tex.pitch && !tex_size_from_format(s_gpu.tex.color)) {
        uint32_t bpp = tex_bytes_per_pixel(s_gpu.tex.color);
        if (bpp && s_gpu.tex.width)
            s_gpu.tex.pitch = s_gpu.tex.width * bpp;
    }
    s_gpu.tex.valid = s_gpu.tex.enabled && s_gpu.tex.offset
                   && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
    if (!s_gpu.tex.valid) {
        if (!s_gpu.tex.enabled) s_gpu.tex_invalid_enabled++;
        else if (!s_gpu.tex.offset) s_gpu.tex_invalid_offset++;
        else if (!s_gpu.tex.width || !s_gpu.tex.height) s_gpu.tex_invalid_size++;
        else s_gpu.tex_invalid_format++;
    }
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    g_f7_nv2a_clear_count++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    if (getenv("RECOMP_FB_WINDOW")) {
        uint32_t fb_va = dma_resolve(s_gpu.color_offset);
        uint32_t fb_pitch = s_gpu.pitch;
        fprintf(stderr, "  [GPU] FBWIN check va=0x%08X pitch=%u\n", fb_va, fb_pitch);
        if (fb_va && fb_pitch) {
            fprintf(stderr, "  [GPU] FBWIN set va=0x%08X pitch=%u\n", fb_va, fb_pitch);
            xbox_FramebufferWindowSet(fb_va, fb_pitch);
        } else {
            fprintf(stderr, "  [GPU] FBWIN skip va=0x%08X pitch=%u\n", fb_va, fb_pitch);
        }
    }

    xbox_FramebufferWindowStart();

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!s_gpu.tex.valid)
        return 0;
    u = wrap_coord(u, s_gpu.tex.width,  s_gpu.tex.addr_u);
    v = wrap_coord(v, s_gpu.tex.height, s_gpu.tex.addr_v);

    fmt = s_gpu.tex.color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + s_gpu.tex.offset, fmt, u, v,
                                     s_gpu.tex.width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + s_gpu.tex.offset;
        u = swizzle_offset(u, v, s_gpu.tex.width, s_gpu.tex.height);
    } else {
        p = mem + s_gpu.tex.offset + (size_t)v * s_gpu.tex.pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    default:
        return 0;
    }
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    /* Same reason the clear checks: a rasterised triangle writes guest memory
     * too, and a surface address that lands on the image is no safer one pixel
     * at a time than 4.9 MB at once. */
    if (surface_hits_image(dma_resolve(s_gpu.color_offset),
                           (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return;
    row = mem + dma_resolve(s_gpu.color_offset) + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

static uint32_t get_pixel(uint8_t *mem, uint32_t bpp, int x, int y)
{
    const uint8_t *row = mem + dma_resolve(s_gpu.color_offset)
                       + (size_t)y * s_gpu.pitch;
    if (bpp == 4)
        return ((const uint32_t *)row)[x];
    if (bpp == 2) {
        uint16_t v = ((const uint16_t *)row)[x];
        uint32_t r = ((v >> 11) & 0x1Fu) * 255u / 31u;
        uint32_t g = ((v >> 5) & 0x3Fu) * 255u / 63u;
        uint32_t b = (v & 0x1Fu) * 255u / 31u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    return 0;
}

static uint32_t modulate_argb(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        uint32_t c = (((a >> shift) & 0xFFu) * ((b >> shift) & 0xFFu) + 127u) / 255u;
        out |= c << shift;
    }
    return out;
}

/* Diagnostic: when this env var is set, skip the vertex-color multiply so we
 * can tell whether the dark menu comes from the texture itself or from the
 * diffuse/vertex colour the title feeds into the rasteriser. */
static int modulate_disabled(void)
{
    const char *p = getenv("RECOMP_PB_NO_MODULATE");
    return p && *p && strcmp(p, "0") != 0;
}

static int white_diffuse(void)
{
    const char *p = getenv("RECOMP_PB_WHITE_DIFFUSE");
    if (p)
        return *p && strcmp(p, "0") != 0;
    return 1;
}

static int alpha_passes(uint32_t argb)
{
    uint32_t a = argb >> 24;
    uint32_t ref = s_gpu.alpha_ref & 0xFFu;

    if (!s_gpu.alpha_test_enable)
        return 1;
    switch (s_gpu.alpha_func) {
    case 0x0200: return 0;          /* NEVER */
    case 0x0201: return a < ref;
    case 0x0202: return a == ref;
    case 0x0203: return a <= ref;
    case 0x0204: return a > ref;
    case 0x0205: return a != ref;
    case 0x0206: return a >= ref;
    case 0x0207: return 1;          /* ALWAYS */
    default:     return 1;
    }
}

static uint32_t blend_factor(uint32_t factor, uint32_t src, uint32_t dst,
                             unsigned shift)
{
    uint32_t sc = (src >> shift) & 0xFFu;
    uint32_t dc = (dst >> shift) & 0xFFu;
    uint32_t sa = src >> 24;
    uint32_t da = dst >> 24;

    switch (factor) {
    case NV_BLEND_ZERO:                return 0;
    case NV_BLEND_ONE:                 return 255;
    case NV_BLEND_SRC_COLOR:           return sc;
    case NV_BLEND_ONE_MINUS_SRC_COLOR: return 255u - sc;
    case NV_BLEND_SRC_ALPHA:           return sa;
    case NV_BLEND_ONE_MINUS_SRC_ALPHA: return 255u - sa;
    case NV_BLEND_DST_ALPHA:           return da;
    case NV_BLEND_ONE_MINUS_DST_ALPHA: return 255u - da;
    case NV_BLEND_DST_COLOR:           return dc;
    case NV_BLEND_ONE_MINUS_DST_COLOR: return 255u - dc;
    case NV_BLEND_SRC_ALPHA_SATURATE:
        return shift == 24 ? 255u : (sa < 255u - da ? sa : 255u - da);
    default:                            return 255;
    }
}

static uint32_t blend_argb(uint32_t src, uint32_t dst)
{
    uint32_t out = 0;

    if (!s_gpu.blend_enable)
        return src;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        int s = (int)((src >> shift) & 0xFFu)
              * (int)blend_factor(s_gpu.blend_sfactor, src, dst, shift) / 255;
        int d = (int)((dst >> shift) & 0xFFu)
              * (int)blend_factor(s_gpu.blend_dfactor, src, dst, shift) / 255;
        int c;

        if (s_gpu.blend_equation == NV_BLEND_FUNC_SUBTRACT)
            c = s - d;
        else if (s_gpu.blend_equation == NV_BLEND_FUNC_REVERSE_SUBTRACT)
            c = d - s;
        else
            c = s + d;
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        out |= (uint32_t)c << shift;
    }
    return out;
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    uint32_t mod = (modulate_disabled() || white_diffuse()) ? 0xFFFFFFFFu : argb;
                    uint32_t src = modulate_disabled() ? texel : modulate_argb(texel, mod);
                    if (alpha_passes(src))
                        put_pixel(mem, bpp, x, y,
                                  blend_argb(src, get_pixel(mem, bpp, x, y)));
                    continue;
                }
            }
            if (alpha_passes(argb))
                put_pixel(mem, bpp, x, y,
                          blend_argb(argb, get_pixel(mem, bpp, x, y)));
        }
    }
    s_gpu.tris_drawn++;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];
    uint32_t argb;

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    argb = ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
    if (white_diffuse())
        argb = 0xFFFFFFFFu;
    return argb;
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types that are triangles under some winding. */
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON       10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ── Vertex transform ─────────────────────────────────────────────────────
 *
 * Everything above draws only batches whose positions already arrive in
 * screen space. Black draws its title that way and everything after it -- the
 * profile menu past "CHECKING XBOX HARD DISK", and every level -- through the
 * transform stage, so those batches were counted as "not screen-space" and
 * dropped, which is a black screen with the title still running underneath.
 *
 * This is that stage, on the CPU:
 *
 *   - program mode (SET_TRANSFORM_EXECUTION_MODE = 2): the title's own NV2A
 *     vertex program, interpreted per vertex. Xbox D3D appends the viewport
 *     transform and the perspective divide to every program it assembles
 *     (mul/rcc/mad through c[-38]/c[-37]), so oPos.xyz leaves it already in
 *     screen space and oPos.w is the clip w, kept for perspective-correct
 *     interpolation;
 *   - fixed-function mode: position times the composite matrix, which Xbox D3D
 *     builds with the viewport folded in, then divided by w.
 *
 * The instruction layout is the NV2A one xemu documents (hw/xbox/nv2a/vsh.c):
 * 128-bit words, a MAC and an ILU operation per word, inputs from temporaries,
 * vertex attributes or constants, R12 aliased to oPos.
 *
 * ponytail: no clipping against the near plane (a triangle with any w <= 0 is
 * dropped), no culling, no lighting in fixed-function mode, register combiners
 * approximated as texture * diffuse, and a host-side depth buffer rather than
 * the title's zeta surface. Enough to put menus and levels on screen; the
 * upgrade path is still the GPU translator.
 */
#define NV097_SET_COMPOSITE_MATRIX          0x0680   /* 16 floats */
#define NV097_SET_SURFACE_ZETA_OFFSET       0x0214
#define NV097_SET_DEPTH_TEST_ENABLE         0x030C
#define NV097_SET_DEPTH_FUNC                0x0354
#define NV097_SET_DEPTH_MASK                0x035C
#define NV097_SET_TRANSFORM_PROGRAM         0x0B00   /* 32-dword window */
#define NV097_SET_TRANSFORM_CONSTANT        0x0B80   /* 32-dword window */
#define NV097_SET_ZSTENCIL_CLEAR_VALUE      0x1D8C
#define NV097_SET_TRANSFORM_EXECUTION_MODE  0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_LOAD    0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START   0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD   0x1EA4
#define NV097_ARRAY_ELEMENT32               0x1808
#define NV097_DRAW_ARRAYS                   0x1810

#define VSH_SLOTS  136
#define VSH_CONSTS 192

static struct {
    uint32_t prog[VSH_SLOTS][4];
    uint32_t prog_load, prog_word, prog_start;
    float    c[VSH_CONSTS][4];
    uint32_t const_load, const_word;
    float    composite[16];
    int      composite_set;
    uint32_t exec_mode;                 /* 0 fixed, 2 program */
    int      depth_test, depth_write;
    uint32_t depth_func;
    uint32_t zeta_offset, zclear;
    uint32_t batches, tris, culled_w, prog_errors;
} s_xf = { .depth_write = 1, .depth_func = 0x0203 };

/* Host depth buffer for the current colour surface. */
static float   *s_zbuf;
static uint32_t s_zbuf_w, s_zbuf_h;

static float *zbuf(void)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h;
    if (!w || !h)
        return NULL;
    if (w != s_zbuf_w || h != s_zbuf_h) {
        free(s_zbuf);
        s_zbuf = (float *)malloc(sizeof(float) * w * h);
        if (!s_zbuf) { s_zbuf_w = s_zbuf_h = 0; return NULL; }
        s_zbuf_w = w; s_zbuf_h = h;
        for (uint32_t i = 0; i < w * h; i++) s_zbuf[i] = INFINITY;
    }
    return s_zbuf;
}

static void zbuf_clear(void)
{
    float *z = zbuf();
    /* D24S8 keeps depth in the top 24 bits, Z16 in the low 16. The usual clear
     * is "farthest", which is kept farther than anything drawn. */
    float v = (float)(s_xf.zclear >> 8);
    if (!z)
        return;
    if ((s_xf.zclear >> 8) >= 0xFFFF00u || (s_xf.zclear & 0xFFFFu) == 0xFFFFu)
        v = INFINITY;
    for (uint32_t i = 0; i < s_zbuf_w * s_zbuf_h; i++) z[i] = v;
}

static int depth_passes(float z, float d)
{
    switch (s_xf.depth_func) {
    case 0x0200: return 0;
    case 0x0201: return z <  d;
    case 0x0202: return z == d;
    case 0x0203: return z <= d;
    case 0x0204: return z >  d;
    case 0x0205: return z != d;
    case 0x0206: return z >= d;
    default:     return 1;
    }
}

static uint32_t vsh_field(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

typedef struct { float v[4]; } Vec4;

typedef struct {
    Vec4 r[13];          /* R0..R11, R12 == oPos */
    Vec4 o[13];          /* oPos oD0.. by output address; see VSH_O_* */
    int  a0;
} VshState;

#define VSH_O_POS 0
#define VSH_O_D0  3
#define VSH_O_T0  9

static Vec4 vsh_input(const VshState *st, const Vec4 *in, const uint32_t *t,
                      int mux, int reg, int neg, int sx, int sy, int sz, int sw)
{
    Vec4 src = {{0, 0, 0, 1}}, out;
    int ci;

    switch (mux) {
    case 1:                                    /* temporary */
        src = reg == 12 ? st->o[VSH_O_POS] : st->r[reg > 12 ? 0 : reg];
        break;
    case 2:                                    /* vertex attribute */
        src = in[vsh_field(t, 1, 9, 4)];
        break;
    case 3:                                    /* constant */
        ci = (int)vsh_field(t, 1, 13, 8);
        if (vsh_field(t, 3, 1, 1))
            ci += st->a0;
        if (ci >= 0 && ci < VSH_CONSTS)
            memcpy(src.v, s_xf.c[ci], sizeof src.v);
        break;
    default:
        break;
    }
    out.v[0] = src.v[sx]; out.v[1] = src.v[sy];
    out.v[2] = src.v[sz]; out.v[3] = src.v[sw];
    if (neg)
        for (int i = 0; i < 4; i++) out.v[i] = -out.v[i];
    return out;
}

static void vsh_write(Vec4 *dst, const Vec4 *val, uint32_t mask)
{
    if (mask & 8) dst->v[0] = val->v[0];
    if (mask & 4) dst->v[1] = val->v[1];
    if (mask & 2) dst->v[2] = val->v[2];
    if (mask & 1) dst->v[3] = val->v[3];
}

static float dot3(const Vec4 *a, const Vec4 *b)
{
    return a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2];
}

/* Run the loaded program for one vertex. Returns 0 if it never reached a
 * final instruction. */
static int vsh_run(const Vec4 in[16], VshState *st)
{
    uint32_t pc;

    memset(st, 0, sizeof *st);
    for (int i = 0; i < 13; i++) st->o[i].v[3] = 1.0f;
    for (pc = s_xf.prog_start; pc < VSH_SLOTS; pc++) {
        const uint32_t *t = s_xf.prog[pc];
        uint32_t ilu = vsh_field(t, 1, 25, 3), mac = vsh_field(t, 1, 21, 4);
        Vec4 a = vsh_input(st, in, t, vsh_field(t, 2, 26, 2), vsh_field(t, 2, 28, 4),
                           vsh_field(t, 1, 8, 1), vsh_field(t, 1, 6, 2),
                           vsh_field(t, 1, 4, 2), vsh_field(t, 1, 2, 2), vsh_field(t, 1, 0, 2));
        Vec4 b = vsh_input(st, in, t, vsh_field(t, 2, 11, 2), vsh_field(t, 2, 13, 4),
                           vsh_field(t, 2, 25, 1), vsh_field(t, 2, 23, 2),
                           vsh_field(t, 2, 21, 2), vsh_field(t, 2, 19, 2), vsh_field(t, 2, 17, 2));
        Vec4 c = vsh_input(st, in, t, vsh_field(t, 3, 28, 2),
                           (vsh_field(t, 2, 0, 2) << 2) | vsh_field(t, 3, 30, 2),
                           vsh_field(t, 2, 10, 1), vsh_field(t, 2, 8, 2),
                           vsh_field(t, 2, 6, 2), vsh_field(t, 2, 4, 2), vsh_field(t, 2, 2, 2));
        Vec4 mr = {{0, 0, 0, 0}}, ir = {{0, 0, 0, 0}};
        uint32_t mac_mask = vsh_field(t, 3, 24, 4), out_r = vsh_field(t, 3, 20, 4);
        uint32_t ilu_mask = vsh_field(t, 3, 16, 4), o_mask = vsh_field(t, 3, 12, 4);
        uint32_t orb = vsh_field(t, 3, 11, 1), oaddr = vsh_field(t, 3, 3, 8);
        uint32_t omux = vsh_field(t, 3, 2, 1);
        int i;

        switch (mac) {
        case 1: mr = a; break;                                           /* MOV */
        case 2: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] * b.v[i]; break; /* MUL */
        case 3: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] + c.v[i]; break; /* ADD */
        case 4: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] * b.v[i] + c.v[i]; break; /* MAD */
        case 5: mr.v[0] = mr.v[1] = mr.v[2] = mr.v[3] = dot3(&a, &b); break; /* DP3 */
        case 6: mr.v[0] = mr.v[1] = mr.v[2] = mr.v[3] = dot3(&a, &b) + b.v[3]; break; /* DPH */
        case 7: mr.v[0] = mr.v[1] = mr.v[2] = mr.v[3] = dot3(&a, &b) + a.v[3] * b.v[3]; break; /* DP4 */
        case 8: mr.v[0] = 1.0f; mr.v[1] = a.v[1] * b.v[1];               /* DST */
                mr.v[2] = a.v[2]; mr.v[3] = b.v[3]; break;
        case 9:  for (i = 0; i < 4; i++) mr.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
        case 10: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; break;
        case 11: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] <  b.v[i] ? 1.0f : 0.0f; break;
        case 12: for (i = 0; i < 4; i++) mr.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
        case 13: st->a0 = (int)floorf(a.v[0]); break;                     /* ARL */
        default: break;
        }
        switch (ilu) {
        case 1: ir = c; break;                                            /* MOV */
        case 2: case 3: {                                                 /* RCP, RCC */
            float x = c.v[0], r = x == 0.0f ? INFINITY : 1.0f / x;
            if (ilu == 3) {
                if (r > 0 && r < 5.42101e-20f) r = 5.42101e-20f;
                if (r > 1.84467e+19f) r = 1.84467e+19f;
                if (r < 0 && r > -5.42101e-20f) r = -5.42101e-20f;
                if (r < -1.84467e+19f) r = -1.84467e+19f;
            }
            ir.v[0] = ir.v[1] = ir.v[2] = ir.v[3] = r;
            break;
        }
        case 4: {                                                         /* RSQ */
            float x = fabsf(c.v[0]);
            ir.v[0] = ir.v[1] = ir.v[2] = ir.v[3] = x == 0.0f ? INFINITY : 1.0f / sqrtf(x);
            break;
        }
        case 5: {                                                         /* EXP */
            float x = c.v[0], f = floorf(x);
            ir.v[0] = exp2f(f); ir.v[1] = x - f; ir.v[2] = exp2f(x); ir.v[3] = 1.0f;
            break;
        }
        case 6: {                                                         /* LOG */
            float x = fabsf(c.v[0]);
            if (x == 0.0f) { ir.v[0] = ir.v[2] = -INFINITY; ir.v[1] = 1.0f; }
            else { float e = floorf(log2f(x)); ir.v[0] = e; ir.v[1] = x / exp2f(e); ir.v[2] = log2f(x); }
            ir.v[3] = 1.0f;
            break;
        }
        case 7: {                                                         /* LIT */
            float d = c.v[0] > 0 ? c.v[0] : 0, s = c.v[1] > 0 ? c.v[1] : 0;
            float p = c.v[3]; if (p < -127.9961f) p = -127.9961f; if (p > 127.9961f) p = 127.9961f;
            ir.v[0] = 1.0f; ir.v[1] = d; ir.v[2] = c.v[0] > 0 ? powf(s, p) : 0.0f; ir.v[3] = 1.0f;
            break;
        }
        default: break;
        }

        /* Temporaries: the MAC writes R[out_r]; the ILU does too, unless the
         * MAC is also writing, in which case the ILU's goes to R1. */
        if (mac && mac != 13 && mac_mask) {
            Vec4 *d = out_r == 12 ? &st->o[VSH_O_POS] : &st->r[out_r > 12 ? 0 : out_r];
            vsh_write(d, &mr, mac_mask);
        }
        if (ilu && ilu_mask) {
            uint32_t ri = mac ? 1 : out_r;
            Vec4 *d = ri == 12 ? &st->o[VSH_O_POS] : &st->r[ri > 12 ? 0 : ri];
            vsh_write(d, &ir, ilu_mask);
        }
        /* The output register file (orb == 1), or a constant write-back. */
        if (o_mask) {
            const Vec4 *val = omux ? &ir : &mr;
            if (orb) {
                if (oaddr < 13)
                    vsh_write(&st->o[oaddr], val, o_mask);
            } else if (oaddr < VSH_CONSTS) {
                Vec4 cv; memcpy(cv.v, s_xf.c[oaddr], sizeof cv.v);
                vsh_write(&cv, val, o_mask);
                memcpy(s_xf.c[oaddr], cv.v, sizeof cv.v);
            }
        }
        if (vsh_field(t, 3, 0, 1))                                       /* FINAL */
            return 1;
    }
    return 0;
}

typedef struct {
    float x, y, z, inv_w;
    float u, v;           /* already divided by w */
    float col[4];         /* r g b a, 0..1 */
    int   ok;
} XfVertex;

static void transform_vertex(uint32_t index, XfVertex *out)
{
    Vec4 in[16];
    float w;
    uint32_t a;

    memset(out, 0, sizeof *out);
    for (a = 0; a < 16; a++) {
        in[a].v[0] = in[a].v[1] = in[a].v[2] = 0.0f;
        in[a].v[3] = 1.0f;
        if (s_gpu.attr[a].size)
            fetch_attr(&s_gpu.attr[a], index, in[a].v);
    }

    if (s_xf.exec_mode == 2) {
        VshState st;
        if (getenv("RECOMP_XF_DUMPPROG")) {
            static int dumped;
            if (s_xf.batches == 200 && !dumped++) {
                fprintf(stderr, "  [XF] c190=(%g %g %g %g) c191=(%g %g %g %g)\n",
                        s_xf.c[190][0], s_xf.c[190][1], s_xf.c[190][2], s_xf.c[190][3],
                        s_xf.c[191][0], s_xf.c[191][1], s_xf.c[191][2], s_xf.c[191][3]);
                fprintf(stderr, "  [XF] program from slot %u; v0=(%g %g %g %g) v3=(%g %g %g %g)\n",
                        s_xf.prog_start, in[0].v[0], in[0].v[1], in[0].v[2], in[0].v[3],
                        in[3].v[0], in[3].v[1], in[3].v[2], in[3].v[3]);
                for (uint32_t pc = s_xf.prog_start; pc < VSH_SLOTS; pc++) {
                    const uint32_t *t = s_xf.prog[pc];
                    fprintf(stderr, "  [XF] %3u: %08X %08X %08X %08X ilu%u mac%u c%u v%u | A mux%u r%u | B mux%u r%u | C mux%u r%u |"
                                    " macm%X r%u ilum%X om%X orb%u oaddr%u omux%u a0x%u final%u\n",
                            pc, t[0], t[1], t[2], t[3], vsh_field(t,1,25,3), vsh_field(t,1,21,4),
                            vsh_field(t,1,13,8), vsh_field(t,1,9,4),
                            vsh_field(t,2,26,2), vsh_field(t,2,28,4), vsh_field(t,2,11,2), vsh_field(t,2,13,4),
                            vsh_field(t,3,28,2), (vsh_field(t,2,0,2) << 2) | vsh_field(t,3,30,2),
                            vsh_field(t,3,24,4), vsh_field(t,3,20,4), vsh_field(t,3,16,4), vsh_field(t,3,12,4),
                            vsh_field(t,3,11,1), vsh_field(t,3,3,8), vsh_field(t,3,2,1), vsh_field(t,3,1,1),
                            vsh_field(t,3,0,1));
                    if (vsh_field(t, 3, 0, 1)) break;
                }
                if (vsh_run(in, &st))
                    fprintf(stderr, "  [XF] oPos=(%g %g %g %g) oD0=(%g %g %g %g) oT0=(%g %g)\n",
                            st.o[0].v[0], st.o[0].v[1], st.o[0].v[2], st.o[0].v[3],
                            st.o[3].v[0], st.o[3].v[1], st.o[3].v[2], st.o[3].v[3], st.o[9].v[0], st.o[9].v[1]);
            }
        }
        if (!vsh_run(in, &st)) {
            s_xf.prog_errors++;
            return;
        }
        w = st.o[VSH_O_POS].v[3];
        /* Xbox D3D's program for pre-transformed (XYZRHW) vertices leaves
         * oPos.w at 0, and the hardware rasterises that as w = 1 (xemu does
         * the same). Black's menu text and icons arrive exactly this way. */
        if (w == 0.0f || !isfinite(w))
            w = 1.0f;
        out->x = st.o[VSH_O_POS].v[0];
        out->y = st.o[VSH_O_POS].v[1];
        out->z = st.o[VSH_O_POS].v[2];
        memcpy(out->col, st.o[VSH_O_D0].v, sizeof out->col);
        out->u = st.o[VSH_O_T0].v[0];
        out->v = st.o[VSH_O_T0].v[1];
    } else {
        const float *m = s_xf.composite;
        float p[4];
        if (!s_xf.composite_set)
            return;
        for (int j = 0; j < 4; j++)
            p[j] = in[0].v[0] * m[4 * j] + in[0].v[1] * m[4 * j + 1]
                 + in[0].v[2] * m[4 * j + 2] + in[0].v[3] * m[4 * j + 3];
        w = p[3];
        if (w <= 1e-6f)
            return;
        out->x = p[0] / w; out->y = p[1] / w; out->z = p[2] / w;
        memcpy(out->col, in[3].v, sizeof out->col);
        if (!s_gpu.attr[3].size)
            out->col[0] = out->col[1] = out->col[2] = out->col[3] = 1.0f;
        out->u = in[9].v[0];
        out->v = in[9].v[1];
    }
    if (!(w > 1e-6f) || !isfinite(out->x) || !isfinite(out->y))
        return;
    if (tex_size_from_format(s_gpu.tex.color)) {
        out->u *= (float)s_gpu.tex.width;
        out->v *= (float)s_gpu.tex.height;
    }
    for (int k = 0; k < 4; k++) {
        if (out->col[k] < 0) out->col[k] = 0;
        if (out->col[k] > 1) out->col[k] = 1;
    }
    out->inv_w = 1.0f / w;
    out->u *= out->inv_w;
    out->v *= out->inv_w;
    out->ok = 1;
}

static void raster_triangle_3d(const XfVertex *A, const XfVertex *B, const XfVertex *C)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float *zb = (s_xf.depth_test || s_xf.depth_write) ? zbuf() : NULL;
    int textured = s_gpu.tex.valid;
    float area;
    int minx, maxx, miny, maxy, x, y;

    if ((bpp != 4 && bpp != 2) || !A->ok || !B->ok || !C->ok)
        return;
    area = (B->x - A->x) * (C->y - A->y) - (B->y - A->y) * (C->x - A->x);
    if (area == 0.0f || !isfinite(area))
        return;

    minx = (int)floorf(fminf(A->x, fminf(B->x, C->x)));
    maxx = (int)ceilf (fmaxf(A->x, fmaxf(B->x, C->x)));
    miny = (int)floorf(fminf(A->y, fminf(B->y, C->y)));
    maxy = (int)ceilf (fmaxf(A->y, fmaxf(B->y, C->y)));
    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy)
        return;

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (C->x - B->x) * (py - B->y) - (C->y - B->y) * (px - B->x);
            float w1 = (A->x - C->x) * (py - C->y) - (A->y - C->y) * (px - C->x);
            float w2 = (B->x - A->x) * (py - A->y) - (B->y - A->y) * (px - A->x);
            float l0, l1, l2, iw, z, col[4];
            uint32_t argb;

            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            l0 = w0 / area; l1 = w1 / area; l2 = w2 / area;
            z = l0 * A->z + l1 * B->z + l2 * C->z;
            if (zb) {
                float *d = &zb[(size_t)y * s_zbuf_w + x];
                if (s_xf.depth_test && !depth_passes(z, *d))
                    continue;
                if (s_xf.depth_write)
                    *d = z;
            }
            iw = l0 * A->inv_w + l1 * B->inv_w + l2 * C->inv_w;
            if (iw == 0.0f)
                continue;
            for (int k = 0; k < 4; k++)
                col[k] = (l0 * A->col[k] * A->inv_w + l1 * B->col[k] * B->inv_w
                        + l2 * C->col[k] * C->inv_w) / iw;
            argb = ((uint32_t)(col[3] * 255.0f) << 24) | ((uint32_t)(col[0] * 255.0f) << 16)
                 | ((uint32_t)(col[1] * 255.0f) << 8) | (uint32_t)(col[2] * 255.0f);
            if (textured) {
                float su = (l0 * A->u + l1 * B->u + l2 * C->u) / iw;
                float sv = (l0 * A->v + l1 * B->v + l2 * C->v) / iw;
                uint32_t texel;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel))
                    argb = modulate_argb(texel, argb);
            }
            if (alpha_passes(argb))
                put_pixel(mem, bpp, x, y, blend_argb(argb, get_pixel(mem, bpp, x, y)));
        }
    }
    s_xf.tris++;
}

/* A batch the screen-space path refused: transform it and draw it. Returns 0
 * when there is no transform state to use, so the caller still counts it. */
static int raster_batch_transformed(void)
{
    static XfVertex v[NV_MAX_INDICES];
    uint32_t i, n = s_gpu.idx_count;

    if (s_xf.exec_mode != 2 && !s_xf.composite_set)
        return 0;
    for (i = 0; i < n; i++)
        transform_vertex(s_gpu.idx[i], &v[i]);
    s_xf.batches++;

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) raster_triangle_3d(&v[i], &v[i+1], &v[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) raster_triangle_3d(&v[i], &v[i+1], &v[i+2]);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) raster_triangle_3d(&v[0], &v[i], &v[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_triangle_3d(&v[i], &v[i+1], &v[i+2]);
            raster_triangle_3d(&v[i], &v[i+2], &v[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_triangle_3d(&v[i], &v[i+1], &v[i+3]);
            raster_triangle_3d(&v[i], &v[i+3], &v[i+2]);
        }
        break;
    default:
        break;
    }

    if (getenv("RECOMP_XF_VERBOSE")) {
        static int shown_attr;
        if (shown_attr++ < 6 && n) {
            for (uint32_t k = 0; k < 16; k++)
                if (s_gpu.attr[k].size)
                    fprintf(stderr, "  [XF]   attr%u off 0x%08X type %u size %u stride %u%s\n", k,
                            s_gpu.attr[k].offset, s_gpu.attr[k].type, s_gpu.attr[k].size,
                            s_gpu.attr[k].stride, s_gpu.inline_active ? " (inline)" : "");
            if (s_gpu.inline_active) {
                fprintf(stderr, "  [XF]   inline %u dwords:", s_gpu.inline_count);
                for (uint32_t k = 0; k < 20 && k < s_gpu.inline_count; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
            }
            for (uint32_t k = 0; k < n && k < 6; k++)
                fprintf(stderr, "  [XF]   idx %u -> (%.2f, %.2f, %.3f) ok %d\n", s_gpu.idx[k],
                        v[k].x, v[k].y, v[k].z, v[k].ok);
        }
    }
    if (getenv("RECOMP_XF_VERBOSE")) {
        static int shown;
        if (shown++ < 12 && n) {
            fprintf(stderr, "  [XF] mode %u start %u prim %u n %u v0 ok %d (%.1f,%.1f,%.3f w=%.3f)"
                            " uv (%.2f,%.2f) col %.2f %.2f %.2f %.2f\n",
                    s_xf.exec_mode, s_xf.prog_start, s_gpu.prim, n, v[0].ok, v[0].x, v[0].y,
                    v[0].z, v[0].inv_w ? 1.0f / v[0].inv_w : 0.0f,
                    v[0].inv_w ? v[0].u / v[0].inv_w : 0.0f, v[0].inv_w ? v[0].v / v[0].inv_w : 0.0f,
                    v[0].col[0], v[0].col[1], v[0].col[2], v[0].col[3]);
        }
    }
    return 1;
}

/* The transform-stage methods. Returns 1 if `method` was one of them. */
static int xf_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } cv;

    cv.u = param;
    if (method >= NV097_SET_COMPOSITE_MATRIX && method < NV097_SET_COMPOSITE_MATRIX + 64) {
        s_xf.composite[(method - NV097_SET_COMPOSITE_MATRIX) / 4] = cv.f;
        s_xf.composite_set = 1;
        return 1;
    }
    /* The component comes from the method address, and the load slot
     * advances when the fourth one is written -- as the hardware does it, so a
     * write that does not start at the window's base lands where it should. */
    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_PROGRAM + 128) {
        uint32_t k = ((method - NV097_SET_TRANSFORM_PROGRAM) / 4) & 3;
        if (s_xf.prog_load < VSH_SLOTS)
            s_xf.prog[s_xf.prog_load][k] = param;
        if (k == 3) s_xf.prog_load++;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < NV097_SET_TRANSFORM_CONSTANT + 128) {
        uint32_t k = ((method - NV097_SET_TRANSFORM_CONSTANT) / 4) & 3;
        if (s_xf.const_load < VSH_CONSTS)
            s_xf.c[s_xf.const_load][k] = cv.f;
        if (k == 3) s_xf.const_load++;
        return 1;
    }
    switch (method) {
    case NV097_SET_TRANSFORM_EXECUTION_MODE: s_xf.exec_mode = param & 3; return 1;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:   s_xf.prog_load = param; s_xf.prog_word = 0; return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START:  s_xf.prog_start = param; return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        s_xf.const_load = param; s_xf.const_word = 0;
        if (getenv("RECOMP_XF_DUMPPROG")) {
            static int shown;
            if (shown++ < 40) fprintf(stderr, "  [XF] CONSTANT_LOAD %u\n", param);
        }
        return 1;
    case NV097_SET_DEPTH_TEST_ENABLE:        s_xf.depth_test = param != 0; return 1;
    case NV097_SET_DEPTH_FUNC:               s_xf.depth_func = param; return 1;
    case NV097_SET_DEPTH_MASK:               s_xf.depth_write = param != 0; return 1;
    case NV097_SET_SURFACE_ZETA_OFFSET:      s_xf.zeta_offset = param; return 1;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE:     s_xf.zclear = param; return 1;
    default:                                 return 0;
    }
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    if (!batch_is_screen_space()) {
        if (!raster_batch_transformed())
            s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    g_f7_nv2a_pgraph_draw_count++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    raster_batch();

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;
    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        s_gpu.unhandled_total++;
        return;
    }
    if (xf_method(method, param))
        return;
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static uint32_t last;
            if (param != last) {
                last = param;
                fprintf(stderr, "  [GPU] SET_SURFACE_COLOR_OFFSET=0x%08X\n", param);
                fflush(stderr);
            }
        }
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (param & 0x03)                  /* Z and/or stencil */
            zbuf_clear();
        clear_surface(param);
        break;
    case NV097_SET_ALPHA_TEST_ENABLE:
        s_gpu.alpha_test_enable = param != 0;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param != 0;
        break;
    case NV097_SET_ALPHA_FUNC:
        s_gpu.alpha_func = param;
        break;
    case NV097_SET_ALPHA_REF:
        s_gpu.alpha_ref = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
        } else {
            if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_ARRAY_ELEMENT32:
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES)
            s_gpu.idx[s_gpu.idx_count++] = param;
        break;
    case NV097_DRAW_ARRAYS: {
        /* A run of consecutive vertices: start in the low 24 bits, count-1 in
         * the top 8. Several runs may share one BEGIN/END. */
        uint32_t start = param & 0xFFFFFFu, count = (param >> 24) + 1u, k;
        for (k = 0; k < count && s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES; k++)
            s_gpu.idx[s_gpu.idx_count++] = start + k;
        break;
    }
    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static uint32_t last;
            if (param != last) {
                last = param;
                fprintf(stderr, "  [GPU] SET_FLIP_READ=%u\n", param);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static uint32_t last;
            if (param != last) {
                last = param;
                fprintf(stderr, "  [GPU] SET_FLIP_WRITE=%u\n", param);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static uint32_t last;
            if (param != last) {
                last = param;
                fprintf(stderr, "  [GPU] SET_FLIP_MODULO=%u\n", param);
                fflush(stderr);
            }
        }
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            fprintf(stderr, "  [GPU] FLIP_INCREMENT_WRITE -> write=%u read=%u modulo=%u\n",
                    s_gpu.flip_write, s_gpu.flip_read, s_gpu.flip_modulo);
            fflush(stderr);
        }
        return;

    case NV097_FLIP_STALL:
        s_gpu.flip_read = s_gpu.flip_write;
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u modulo=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write, s_gpu.flip_modulo);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL0:
        s_gpu.tex.enabled = (param & NV097_SET_TEXTURE_CONTROL0_ENABLE) != 0;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    case 0x0848:
    case 0x084C:
    case 0x0850:
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static int shown;
            if (shown++ < 8)
                fprintf(stderr, "  [GPU] surface base/pitch method 0x%04X param 0x%08X\n",
                        method, param);
        }
        break;

    case 0x09F8:
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static int shown;
            if (shown++ < 8)
                fprintf(stderr, "  [GPU] surface/tiling method 0x%04X param 0x%08X\n",
                        method, param);
        }
        break;

    case 0x17BC:
    case 0x17C4:
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static int shown;
            if (shown++ < 8)
                fprintf(stderr, "  [GPU] vertex/begin method 0x%04X param 0x%08X\n",
                        method, param);
        }
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        else if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            uint32_t attr_idx = (method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4;
            s_gpu.attr[attr_idx].offset = dma_resolve(param);
            if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
                static int shown[32];
                if (shown[attr_idx]++ < 4)
                    fprintf(stderr, "  [GPU] attr[%u] offset: 0x%08X\n", attr_idx, s_gpu.attr[attr_idx].offset);
            }
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            uint32_t attr_idx = (method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
            if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
                static int shown[32];
                if (shown[attr_idx]++ < 4)
                    fprintf(stderr, "  [GPU] attr[%u] format: type=%u size=%u stride=%u\n",
                            attr_idx, a->type, a->size, a->stride);
            }
        } else if ((method >= 0x0000 && method < 0x0200) ||  /* NOP, SET_OBJECT, etc. */
                   (method >= 0x0200 && method < 0x0300) ||  /* Surface/clear state */
                   (method >= 0x0300 && method < 0x0440) ||  /* Alpha/blend/cull/alpha test + nearby */
                   (method >= 0x0A00 && method < 0x0C00) ||  /* Transform/vertex state */
                   (method >= 0x0E00 && method < 0x1000) ||  /* Transform constants */
                   (method >= 0x1680 && method < 0x1780) ||  /* More vertex arrays */
                   (method >= 0x1900 && method < 0x1A00) ||  /* Fog, point params, etc. */
                   (method >= 0x1D60 && method < 0x1EA8)) {  /* Combiners + nearby */
            s_gpu.unhandled_total++;
        } else {
            note_unhandled(method);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    fprintf(stderr, "[GPU] flip read=%u write=%u modulo=%u flips=%u\n",
            s_gpu.flip_read, s_gpu.flip_write, s_gpu.flip_modulo, s_gpu.flips);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] transform: mode %u, %u batches, %u triangles drawn, %u program errors\n",
            s_xf.exec_mode, s_xf.batches, s_xf.tris, s_xf.prog_errors);
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage"
                    " (disabled=%u no_offset=%u no_size=%u bad_fmt=%u)\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex,
            s_gpu.tex_invalid_enabled, s_gpu.tex_invalid_offset,
            s_gpu.tex_invalid_size, s_gpu.tex_invalid_format);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        fprintf(stderr, "  [GPU]   0x%04X x%u\n",
                s_unhandled[i].method, s_unhandled[i].count);
    }
    }    fflush(stderr);
}
