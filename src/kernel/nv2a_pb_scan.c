/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* getenv once per call site. These run per pushbuffer method -- millions of
 * times a second -- and each getenv walks the environment under a lock: 12% of
 * the GPU thread in Black's first level. The environment does not change
 * after startup. */
#define CGETENV(name) __extension__ ({ static int _ce_done; static const char *_ce_val; \
    if (!_ce_done) { _ce_val = getenv(name); _ce_done = 1; } _ce_val; })

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls, calls_bad;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
#if defined(XBOX_HAVE_XEMU_NV2A)
extern int xnv2a_method(uint32_t subch, uint32_t method, uint32_t param,
                        uint32_t *params, size_t avail, int inc);
extern int g_pb_exec_no_draw;
/* RECOMP_XEMU_GPU: render through xemu's PGRAPH (third_party/xemu_nv2a). */
static int xemu_gpu_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = CGETENV("RECOMP_XEMU_GPU") != NULL;
        if (on)
            g_pb_exec_no_draw = 1;
    }
    return on;
}
#endif
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

/* Finding the entry again is the hot path: note() runs once per pushbuffer
 * word, and a linear walk of up to PB_MAX_METHODS entries per word cost 42% of
 * the GPU thread in a level (2026-09-19 profile) -- more than the drawing it
 * was measuring. Methods are 4-byte aligned and below 0x2000 on every class
 * this title uses, so index them directly and keep the walk for the rest. */
#define PB_IDX_SUBCH   8
#define PB_IDX_METHODS 0x2000
static uint16_t s_index[PB_IDX_SUBCH][PB_IDX_METHODS / 4]; /* slot + 1, 0 = new */

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = NULL;

    if (subch < PB_IDX_SUBCH && method < PB_IDX_METHODS && !(method & 3)) {
        slot = &s_index[subch][method >> 2];
        if (*slot) {
            s_seen[*slot - 1].count++;
            return;
        }
    } else {
        for (int i = 0; i < s_seen_count; i++) {
            if (s_seen[i].method == method && s_seen[i].subch == subch) {
                s_seen[i].count++;
                return;
            }
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        if (slot)
            *slot = (uint16_t)s_seen_count;   /* slot + 1: 0 stays "new" */
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !CGETENV("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* A title-specific look at inline vertex data before anything consumes it.
 * The runtime has no business editing a title's geometry; a game project that
 * has to work around a specific draw registers this and the change stays in
 * that project. Called with the primitive type of the enclosing BEGIN/END and
 * the INLINE_ARRAY payload, which it may modify in place. */
typedef void (*xbox_pb_inline_filter_fn)(uint32_t prim, uint32_t *words, uint32_t count);
static xbox_pb_inline_filter_fn s_inline_filter;
static uint32_t s_cur_prim;

void xbox_PbSetInlineFilter(xbox_pb_inline_filter_fn fn)
{
    s_inline_filter = fn;
}

static uint32_t s_last_word;

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0;
    uint32_t subr_return = 0, calls = 0;
    int subr_active = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = CGETENV("RECOMP_PB_EXEC") != NULL;
    if (!(CGETENV("RECOMP_PB_SCAN") || s_exec_enabled) || end_va == start_va)
        return;

    /* Walk from GET to PUT the way the GPU does, following jumps: D3D's
     * pushbuffer is a ring, and when it wraps PUT lies below GET with a JMP
     * back to the start in between. Stopping at the jump, or skipping a
     * wrapped segment, dropped every command in it -- texture and shader
     * state included, which xemu's PGRAPH then used unset. */
    while (va != end_va && words < 0x100000u) {
        /* Every pushbuffer address must lie in the 64 MB contiguous window.
         * A word that is not a command -- data read as one -- can carry a
         * JUMP/CALL target anywhere in 256 MB; following it read unmapped
         * memory and took the process down (SIGBUS at 0x8F30DE84). Drop the
         * rest of this segment instead, and say where it went wrong. */
        if (va < 0x80000000u || va >= 0x84000000u) {
            static unsigned bad;
            if (bad++ < 8)
                fprintf(stderr, "[PB] address %08X outside RAM (segment %08X..%08X,"
                        " %u words in, %u jumps, last word %08X); segment dropped\n",
                        va, start_va, end_va, words, jumps, s_last_word);
            break;
        }
        uint32_t w = *(const uint32_t *)(mem + va);
        s_last_word = w;
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            /* Old-style (bit 0) and new-style (0x20000000) jumps both carry
             * the physical target in the low bits. */
            jumps++;
            if (jumps > 64)
                break;
            va = 0x80000000u | (w & 0x0FFFFFFCu);
            continue;
        }
        /* CALL and RETURN: one level of subroutine, as on the hardware.
         * Skipping them dropped every method in a called block -- D3D's
         * recorded pushbuffers and state blocks -- which left Black drawing
         * its level with the blend state of the particles before it. */
        if ((w & 3u) == 2u) {
            if (subr_active) {
                calls_bad++;
                break;                          /* nested CALL: a fault on HW */
            }
            subr_return = va;
            subr_active = 1;
            calls++;
            va = 0x80000000u | (w & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {
            if (!subr_active) {
                calls_bad++;
                continue;
            }
            subr_active = 0;
            va = subr_return;
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            if (s_inline_filter && subch == 0 && method == 0x1818 && s_cur_prim
                && (end_va <= va || (end_va - va) / 4 >= count))
                s_inline_filter(s_cur_prim, (uint32_t *)(mem + va), count);
            for (uint32_t i = 0; i < count && va != end_va; ) {
                uint32_t m = noninc ? method : method + i * 4;
                uint32_t take = 1;
#if defined(XBOX_HAVE_XEMU_NV2A)
                /* xemu's PGRAPH renders; it may consume several words of one
                 * method at once (inline vertex data), so it is told how many
                 * follow and says how many it took. */
                if (xemu_gpu_enabled()) {
                    uint32_t avail = count - i;
                    if (end_va > va && (end_va - va) / 4 < avail)
                        avail = (end_va - va) / 4;
                    take = (uint32_t)xnv2a_method(subch, m,
                                                  *(const uint32_t *)(mem + va),
                                                  (uint32_t *)(mem + va), avail,
                                                  !noninc);
                    if (take < 1 || take > avail)
                        take = 1;
                }
#endif
                for (uint32_t k = 0; k < take; k++) {
                    uint32_t mk = noninc ? method : method + (i + k) * 4;
                    if (subch == 0 && mk == 0x17FC)
                        s_cur_prim = *(const uint32_t *)(mem + va);
                    note(subch, mk);
                    /* Same walk, two consumers: the survey counts, the
                     * executor acts. With xemu rendering, the executor still
                     * tracks surfaces and flips for the window, but draws
                     * nothing (g_pb_exec_no_draw). */
                    if (s_exec_enabled)
                        nv2a_pb_exec_method(subch, mk,
                                            *(const uint32_t *)(mem + va));
                    va += 4;
                    words++;
                }
                i += take;
                (void)m;
            }
            continue;
        }
        unknown++;
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    if (calls && !s_tot_calls) {
        fprintf(stderr, "  [PB] first pushbuffer CALL followed\n");
        fflush(stderr);
    }
    s_tot_calls += calls;
    if (calls_bad) {
        static int shown;
        if (!shown++) {
            fprintf(stderr, "  [PB] CALL/RETURN out of order (%u)\n", calls_bad);
            fflush(stderr);
        }
    }
    s_tot_segments++;
}
