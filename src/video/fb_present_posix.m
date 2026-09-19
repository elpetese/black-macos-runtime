/**
 * Present the guest framebuffer in a Metal window on macOS.
 *
 * The title and the software push-buffer executor render into guest RAM. This
 * module scans that RAM at 60 Hz, converts the two front-buffer formats used by
 * Xbox titles to BGRA8, and presents the result without involving D3D8.
 */

#if defined(__APPLE__)

#include <AppKit/AppKit.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Forward-declared here, not via keyboard_input.h: that header pulls in the
 * Win32 type vocabulary (xinput_xbox.h -> xbox_winnt.h -> win32_compat.h), whose
 * `BOOL`/`GetCurrentThread`/`GetCurrentProcess` clash with AppKit in this
 * Objective-C translation unit. main_posix.c includes the header safely (pure
 * C); only this Cocoa file needs the forward declarations. */
int  xbox_KeyboardSlotForKey(unsigned char c);
int  xbox_KeyboardSlotForKeyCode(unsigned short keycode);
void xbox_KeyboardPressSlot(int slot);
void xbox_KeyboardReleaseSlot(int slot);

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int xbox_IsXboxAddress(uintptr_t);
extern void pb_exec_get_surface(uint32_t *out_offset, uint32_t *out_pitch);

static pthread_mutex_t s_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_pixels_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t s_fb_va;
static volatile uint32_t s_fb_pitch;
#define FB_HISTORY 4
static uint32_t s_fb_va_history[FB_HISTORY];
static uint32_t s_fb_pitch_history[FB_HISTORY];
static int s_fb_history_count;
static uint32_t s_width = 640;
static uint32_t s_height = 480;
static uint8_t *s_pixels;
static size_t s_pixels_size;
static volatile int s_running;
static int s_test_pattern;
static int s_fb_debug; /* RECOMP_FB_DEBUG: gate per-frame [FBWIN]/[FRAME] spam */
static uint64_t s_fb_frames_presented;

static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_pipeline;
static id<MTLTexture> s_texture;
static NSWindow *s_window;
static CAMetalLayer *s_layer;
static pthread_t s_render_thread;
static int s_render_thread_started;
static uint64_t s_dump_at;
static int s_dumped;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");
    const char *msg = "[FBWIN] xbox_FramebufferWindowSet CALLED\n";
    write(STDERR_FILENO, msg, strlen(msg));

    fprintf(stderr, "[FBWIN] before lock: va=0x%08X pitch=%u\n", fb_va, pitch);
    fflush(stderr);
    pthread_mutex_lock(&s_state_lock);
    fprintf(stderr, "[FBWIN] after lock\n");
    fflush(stderr);
    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
    for (int i = s_fb_history_count; i > 0; i--)
        s_fb_va_history[i] = s_fb_va_history[i - 1];
    s_fb_va_history[0] = s_fb_va;
    if (pitch)
        s_fb_pitch_history[0] = s_fb_pitch;
    if (s_fb_history_count < FB_HISTORY)
        s_fb_history_count++;
    fprintf(stderr, "[FBWIN] set va=0x%08X pitch=%u (result va=0x%08X pitch=%u)\n",
            fb_va, pitch, s_fb_va, s_fb_pitch);
    fflush(stderr);
    pthread_mutex_unlock(&s_state_lock);
    fprintf(stderr, "[FBWIN] after unlock\n");
    fflush(stderr);
}

static int ensure_pixels(void)
{
    size_t size = (size_t)s_width * s_height * 4u;
    uint8_t *p;

    if (s_pixels && s_pixels_size == size)
        return 1;
    p = (uint8_t *)realloc(s_pixels, size);
    if (!p)
        return 0;
    s_pixels = p;
    s_pixels_size = size;
    return 1;
}

static int read_fb(uint32_t fb_va, uint32_t pitch, uint32_t bpp)
{
    uint32_t x, y;
    const uint8_t *src = (const uint8_t *)
        ((uintptr_t)xbox_GetMemoryOffset() + fb_va);
    for (y = 0; y < s_height; y++) {
        const uint8_t *src_row = src + (size_t)y * pitch;
        uint8_t *dst_row = s_pixels + (size_t)y * s_width * 4u;
        if (bpp == 4) {
            memcpy(dst_row, src_row, (size_t)s_width * 4u);
        } else {
            const uint16_t *p = (const uint16_t *)src_row;
            for (x = 0; x < s_width; x++) {
                uint16_t v = p[x];
                dst_row[x * 4u + 0] = (uint8_t)((v & 0x1Fu) * 255u / 31u);
                dst_row[x * 4u + 1] = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
                dst_row[x * 4u + 2] = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
                dst_row[x * 4u + 3] = 0xFF;
            }
        }
    }
    return 1;
}

static int read_fb_to(uint32_t fb_va, uint32_t pitch, uint32_t bpp, uint8_t *out_pixels)
{
    uint32_t x, y;
    const uint8_t *src = (const uint8_t *)
        ((uintptr_t)xbox_GetMemoryOffset() + fb_va);
    for (y = 0; y < s_height; y++) {
        const uint8_t *src_row = src + (size_t)y * pitch;
        uint8_t *dst_row = out_pixels + (size_t)y * s_width * 4u;
        if (bpp == 4) {
            memcpy(dst_row, src_row, (size_t)s_width * 4u);
        } else {
            const uint16_t *p = (const uint16_t *)src_row;
            for (x = 0; x < s_width; x++) {
                uint16_t v = p[x];
                dst_row[x * 4u + 0] = (uint8_t)((v & 0x1Fu) * 255u / 31u);
                dst_row[x * 4u + 1] = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
                dst_row[x * 4u + 2] = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
                dst_row[x * 4u + 3] = 0xFF;
            }
        }
    }
    return 1;
}

#define CANDIDATE_MAX 12
typedef struct {
    uint32_t va;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t first_pixel;
    uint32_t nonzero_score;
} SurfaceCandidate;

static uint32_t score_surface_32x32(uint32_t fb_va, uint32_t pitch, uint32_t bpp)
{
    const uint8_t *src = (const uint8_t *)
        ((uintptr_t)xbox_GetMemoryOffset() + fb_va);
    if (!xbox_IsXboxAddress(fb_va))
        return 0; /* unmapped/IO VA (stray gpu offset): skip, do not deref */
    uint32_t step_x = s_width < 32 ? 1 : s_width / 32;
    uint32_t step_y = s_height < 32 ? 1 : s_height / 32;
    uint32_t count = 0;
    for (uint32_t y = 0; y < s_height; y += step_y) {
        const uint8_t *row = src + (size_t)y * pitch;
        if (bpp == 4) {
            const uint32_t *p = (const uint32_t *)row;
            for (uint32_t x = 0; x < s_width; x += step_x) {
                if (p[x] != 0)
                    count++;
            }
        } else {
            const uint16_t *p = (const uint16_t *)row;
            for (uint32_t x = 0; x < s_width; x += step_x) {
                if (p[x] != 0)
                    count++;
            }
        }
    }
    return count;
}

static int build_candidates(SurfaceCandidate *candidates, int *count)
{
    uint32_t exec_offset = 0, exec_pitch = 0;
    uint32_t gpu_offset = 0, gpu_pitch = 0;
    uint32_t current_va = 0, current_pitch = 0;
    int n = 0;

    pb_exec_get_surface(&exec_offset, &exec_pitch);

    pthread_mutex_lock(&s_state_lock);
    current_va = s_fb_va;
    current_pitch = s_fb_pitch;
    pthread_mutex_unlock(&s_state_lock);

    if (current_pitch)
        gpu_pitch = current_pitch;
    if (exec_offset)
        gpu_offset = exec_offset;

    if (!current_va && !gpu_offset)
        return 0;

    uint32_t base = current_va ? current_va : gpu_offset;
    uint32_t pitch = current_pitch ? current_pitch : (gpu_pitch ? gpu_pitch : s_width * 4u);
    uint32_t bpp = pitch / s_width;
    if (bpp != 2 && bpp != 4)
        bpp = 4;

    uint32_t stride = s_width * bpp;

    if (base) {
        candidates[n].va = base;
        candidates[n].pitch = pitch;
        candidates[n].bpp = bpp;
        candidates[n].first_pixel = 0;
        candidates[n].nonzero_score = 0;
        n++;
    }
    if (gpu_offset && gpu_offset != base) {
        candidates[n].va = gpu_offset;
        candidates[n].pitch = gpu_pitch ? gpu_pitch : pitch;
        candidates[n].bpp = bpp;
        candidates[n].first_pixel = 0;
        candidates[n].nonzero_score = 0;
        n++;
    }

    if (base) {
        uint32_t offsets[] = { 0x12C000, s_width * s_height * 2 };
        for (int i = 0; i < 2 && n < CANDIDATE_MAX; i++) {
            uint32_t try = base + offsets[i];
            candidates[n].va = try;
            candidates[n].pitch = pitch;
            candidates[n].bpp = bpp;
            candidates[n].first_pixel = 0;
            candidates[n].nonzero_score = 0;
            n++;
            if (n < CANDIDATE_MAX && base >= offsets[i]) {
                try = base - offsets[i];
                candidates[n].va = try;
                candidates[n].pitch = pitch;
                candidates[n].bpp = bpp;
                candidates[n].first_pixel = 0;
                candidates[n].nonzero_score = 0;
                n++;
            }
        }
    }

    pthread_mutex_lock(&s_state_lock);
    int hist = s_fb_history_count;
    uint32_t hva[FB_HISTORY], hpitch[FB_HISTORY];
    for (int i = 0; i < hist && n < CANDIDATE_MAX; i++) {
        hva[i] = s_fb_va_history[i];
        hpitch[i] = s_fb_pitch_history[i];
    }
    pthread_mutex_unlock(&s_state_lock);
    for (int i = 0; i < hist && n < CANDIDATE_MAX; i++) {
        if (!hva[i])
            continue;
        candidates[n].va = hva[i];
        candidates[n].pitch = hpitch[i] ? hpitch[i] : pitch;
        candidates[n].bpp = candidates[n].pitch / s_width;
        if (candidates[n].bpp != 2 && candidates[n].bpp != 4)
            candidates[n].bpp = bpp;
        candidates[n].first_pixel = 0;
        candidates[n].nonzero_score = 0;
        n++;
    }

    for (int i = 0; i < n; i++) {
        if (!xbox_IsXboxAddress(candidates[i].va)) {
            candidates[i].first_pixel = 0;
            candidates[i].nonzero_score = 0;
            continue; /* candidate VA outside mapped RAM (stray gpu offset/history): skip */
        }
        const uint8_t *src = (const uint8_t *)
            ((uintptr_t)xbox_GetMemoryOffset() + candidates[i].va);
        const uint32_t *first = (const uint32_t *)src;
        candidates[i].first_pixel = first[0];
        candidates[i].nonzero_score = score_surface_32x32(
            candidates[i].va, candidates[i].pitch, candidates[i].bpp);
    }

    *count = n;
    return n;
}

static int update_pixels(void)
{
    uint32_t fb_va, pitch, bpp;
    uint32_t x, y;
    uint32_t va_history[FB_HISTORY];
    uint32_t pitch_history[FB_HISTORY];
    int history_count;

    pthread_mutex_lock(&s_state_lock);
    fb_va = s_fb_va;
    pitch = s_fb_pitch;
    history_count = s_fb_history_count;
    for (int i = 0; i < history_count; i++) {
        va_history[i] = s_fb_va_history[i];
        pitch_history[i] = s_fb_pitch_history[i];
    }
    pthread_mutex_unlock(&s_state_lock);

    pthread_mutex_lock(&s_pixels_lock);
    if (!ensure_pixels()) {
        pthread_mutex_unlock(&s_pixels_lock);
        return 0;
    }

    if (s_test_pattern) {
        for (y = 0; y < s_height; y++) {
            uint32_t *dst = (uint32_t *)s_pixels + (size_t)y * s_width;
            for (x = 0; x < s_width; x++) {
                uint8_t r = (uint8_t)(x * 255u / (s_width - 1u));
                uint8_t g = (uint8_t)(y * 255u / (s_height - 1u));
                uint8_t b = ((x / 32u) ^ (y / 32u)) & 1u ? 0xE0 : 0x20;
                dst[x] = 0xFF000000u | ((uint32_t)r << 16)
                       | ((uint32_t)g << 8) | b;
            }
        }
        fprintf(stderr, "[FBWIN] using test pattern\n");
        fflush(stderr);
        pthread_mutex_unlock(&s_pixels_lock);
        return 1;
    }

    if (!fb_va || !pitch) {
        fprintf(stderr, "[FBWIN] update_pixels: no fb_va=0x%08X pitch=%u\n", fb_va, pitch);
        pthread_mutex_unlock(&s_pixels_lock);
        return 0;
    }
    bpp = pitch / s_width;
    if (bpp != 2 && bpp != 4) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[FBWIN] unsupported pitch %u for width %u\n",
                    pitch, s_width);
        pthread_mutex_unlock(&s_pixels_lock);
        return 0;
    }

    if (!read_fb(fb_va, pitch, bpp)) {
        pthread_mutex_unlock(&s_pixels_lock);
        return 0;
    }

    {
        const uint32_t *first = (const uint32_t *)s_pixels;
        if (first[0] == 0 && first[1] == 0 && first[2] == 0 && first[3] == 0) {
            for (int i = 1; i < history_count; i++) {
                if (!va_history[i] || !pitch_history[i])
                    continue;
                uint32_t pbpp = pitch_history[i] / s_width;
                if (pbpp != 2 && pbpp != 4)
                    continue;
                if (!read_fb(va_history[i], pitch_history[i], pbpp))
                    continue;
                const uint32_t *check = (const uint32_t *)s_pixels;
                if (check[0] != 0 || check[1] != 0 || check[2] != 0 || check[3] != 0) {
                    fprintf(stderr, "[FBWIN] switched to history[%d] va=0x%08X pitch=%u\n",
                            i, va_history[i], pitch_history[i]);
                    fflush(stderr);
                    break;
                }
            }
        }
    }

    {
        static uint32_t last_first;
        const uint32_t *first = (const uint32_t *)s_pixels;
        if (first[0] != last_first) {
            fprintf(stderr, "[FBWIN] first_pixel CHANGED 0x%08X -> 0x%08X\n",
                    last_first, first[0]);
            fflush(stderr);
            last_first = first[0];
        }
    }

    pthread_mutex_unlock(&s_pixels_lock);
    return 1;
}

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_width * 3u) + 3u) & ~3u;
    uint32_t image_size = row * s_height;
    uint32_t file_size = 54u + image_size;
    uint8_t header[54] = {0};
    uint8_t *line;
    uint32_t x, y;

    if (!path || !update_pixels())
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    line = (uint8_t *)calloc(1, row);
    if (!line) {
        fclose(f);
        return -1;
    }

    header[0] = 'B'; header[1] = 'M';
    memcpy(header + 2, &file_size, 4);
    header[10] = 54; header[14] = 40;
    memcpy(header + 18, &s_width, 4);
    memcpy(header + 22, &s_height, 4);
    header[26] = 1; header[28] = 24;
    memcpy(header + 34, &image_size, 4);
    fwrite(header, 1, sizeof header, f);

    pthread_mutex_lock(&s_pixels_lock);
    for (y = s_height; y-- > 0;) {
        const uint8_t *src = s_pixels + (size_t)y * s_width * 4u;
        for (x = 0; x < s_width; x++) {
            line[x * 3u + 0] = src[x * 4u + 0];
            line[x * 3u + 1] = src[x * 4u + 1];
            line[x * 3u + 2] = src[x * 4u + 2];
        }
        fwrite(line, 1, row, f);
    }
    pthread_mutex_unlock(&s_pixels_lock);
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u)\n", path, s_width, s_height);
    return 0;
}

static int dump_candidate_bmp(const char *prefix, int frame, const SurfaceCandidate *c)
{
    char path[256];
    snprintf(path, sizeof(path), "%sf%04d-%08X.bmp", prefix, frame, c->va);
    return xbox_FramebufferDumpBmp(path);
}

static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Out { float4 position [[position]]; float2 uv; };\n"
    "vertex Out vs(uint id [[vertex_id]]) {\n"
    "  const float2 p[] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };\n"
    "  const float2 t[] = { {0,1}, {1,1}, {0,0}, {1,0} };\n"
    "  Out o; o.position=float4(p[id],0,1); o.uv=t[id]; return o;\n"
    "}\n"
    "fragment float4 fs(Out in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
    "  constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);\n"
    "  float4 t = tex.sample(s, in.uv);\n"
    "  if (t.x == 0 && t.y == 0 && t.z == 0) return float4(in.uv, 1.0, 1.0);\n"
    "  return t;\n"
    "}\n";

static int init_metal(void)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:shader_source];
    id<MTLLibrary> library;
    id<MTLFunction> vs, fs;
    MTLRenderPipelineDescriptor *desc;
    MTLTextureDescriptor *texture_desc;

    s_device = MTLCreateSystemDefaultDevice();
    if (!s_device)
        return 0;
    s_queue = [s_device newCommandQueue];
    library = [s_device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        fprintf(stderr, "[FBWIN] Metal shader error: %s\n",
                error.localizedDescription.UTF8String);
        return 0;
    }
    vs = [library newFunctionWithName:@"vs"];
    fs = [library newFunctionWithName:@"fs"];
    desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_pipeline = [s_device newRenderPipelineStateWithDescriptor:desc error:&error];
    [desc release];
    [vs release];
    [fs release];
    [library release];
    if (!s_pipeline) {
        fprintf(stderr, "[FBWIN] Metal pipeline error: %s\n",
                error.localizedDescription.UTF8String);
        return 0;
    }

    texture_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:s_width height:s_height mipmapped:NO];
    texture_desc.usage = MTLTextureUsageShaderRead;
    s_texture = [s_device newTextureWithDescriptor:texture_desc];
    return s_texture != nil;
}

/* A content view that accepts first responder so the framebuffer window itself
 * receives key events. The default NSView contentView refuses first responder,
 * so without this keyDown: never fired: once RECOMP_FB_WINDOW made the window
 * key, Terminal was backgrounded and the tty reader in keyboard_input.c got no
 * bytes. This view forwards keyDown/keyUp to the same hold timers, so the
 * window is fully keyboard-playable on its own (and no longer depends on stdin
 * being a TTY, e.g. when launched from Finder). */
@interface XBFrameKeyView : NSView
@end
@implementation XBFrameKeyView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event  { [self xek_handleKey:event down:YES]; }
- (void)keyUp:(NSEvent *)event    { [self xek_handleKey:event down:NO]; }
- (void)xek_handleKey:(NSEvent *)event down:(BOOL)down
{
    NSString *chars = [event characters];
    int slot = -1;

    if (chars && [chars length] >= 1) {
        unichar u = [chars characterAtIndex:0];
        if (u < 128)
            slot = xbox_KeyboardSlotForKey((unsigned char)u);
    }
    /* Arrow keys arrive as function-key glyphs with no ASCII, so map their
     * virtual key code (handled in keyboard_input.c under __APPLE__). */
    if (slot < 0)
        slot = xbox_KeyboardSlotForKeyCode((unsigned short)[event keyCode]);

    if (slot >= 0) {
        if (down)  xbox_KeyboardPressSlot(slot);
        else       xbox_KeyboardReleaseSlot(slot);
    } else if (down) {
        [super keyDown:event];   /* let unbound keys fall through to defaults */
    }
}
@end

static void render_frame(void)
{
    id<CAMetalDrawable> drawable;
    MTLRenderPassDescriptor *pass;
    id<MTLCommandBuffer> command;
    id<MTLRenderCommandEncoder> encoder;

    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: starting\n"); fflush(stderr); }

    if (!update_pixels()) {
        fprintf(stderr, "[FBWIN] render_frame: update_pixels failed\n");
        fflush(stderr);
        return;
    }
    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: update_pixels OK\n"); fflush(stderr); }

    pthread_mutex_lock(&s_pixels_lock);
    [s_texture replaceRegion:MTLRegionMake2D(0, 0, s_width, s_height)
                 mipmapLevel:0 withBytes:s_pixels bytesPerRow:s_width * 4u];
    pthread_mutex_unlock(&s_pixels_lock);

    drawable = [s_layer nextDrawable];
    if (!drawable) {
        fprintf(stderr, "[FBWIN] render_frame: nextDrawable returned nil\n");
        fflush(stderr);
        return;
    }
    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: nextDrawable OK\n"); fflush(stderr); }

    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
    command = [s_queue commandBuffer];
    encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:s_pipeline];
    [encoder setFragmentTexture:s_texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: encoder end OK\n"); fflush(stderr); }

    [command presentDrawable:drawable];
    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: presentDrawable OK\n"); fflush(stderr); }
    [command commit];
    if (s_fb_debug) { fprintf(stderr, "[FBWIN] render_frame: commit OK\n"); fflush(stderr); }

    static int frame_count = 0;
    if (frame_count < 3) {
        fprintf(stderr, "[FBWIN] render_frame: committed frame %d\n", frame_count);
        fflush(stderr);
        frame_count++;
    }
    s_fb_frames_presented++;
}

static void *render_thread(void *unused)
{
    (void)unused;
    uint64_t last_print = now_ms();
    int frame = 0;
    fprintf(stderr, "[FBWIN] render thread started\n");
    fflush(stderr);
    while (s_running) {
        uint32_t fb_va = 0, fb_pitch = 0;
        pthread_mutex_lock(&s_state_lock);
        fb_va = s_fb_va;
        fb_pitch = s_fb_pitch;
        pthread_mutex_unlock(&s_state_lock);
        if (!fb_va || !fb_pitch) {
            struct timespec delay = { .tv_sec = 0, .tv_nsec = 16666667 };
            nanosleep(&delay, NULL);
            continue;
        }

        SurfaceCandidate candidates[CANDIDATE_MAX];
        int candidate_count = 0;
        if (build_candidates(candidates, &candidate_count) > 0) {
            int best = -1;
            uint32_t best_score = 0;
            const char *prefix = getenv("RECOMP_FB_CANDIDATES");
            for (int i = 0; i < candidate_count; i++) {
                if (candidates[i].nonzero_score > best_score) {
                    best_score = candidates[i].nonzero_score;
                    best = i;
                }
                if (prefix && frame < 30) {
                    uint8_t *tmp = calloc(1, (size_t)s_width * s_height * 4u);
                    if (tmp && read_fb_to(candidates[i].va, candidates[i].pitch, candidates[i].bpp, tmp)) {
                        char path[256];
                        snprintf(path, sizeof(path), "%sf%04d-%08X.bmp", prefix, frame, candidates[i].va);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            uint32_t row = ((s_width * 3u) + 3u) & ~3u;
                            uint8_t header[54] = {0};
                            uint8_t *line = calloc(1, row);
                            header[0] = 'B'; header[1] = 'M';
                            uint32_t image_size = row * s_height;
                            uint32_t file_size = 54u + image_size;
                            memcpy(header + 2, &file_size, 4);
                            header[10] = 54; header[14] = 40;
                            memcpy(header + 18, &s_width, 4);
                            memcpy(header + 22, &s_height, 4);
                            header[26] = 1; header[28] = 24;
                            memcpy(header + 34, &image_size, 4);
                            fwrite(header, 1, sizeof header, f);
                            for (int y = s_height; y-- > 0;) {
                                const uint8_t *src = tmp + (size_t)y * s_width * 4u;
                                for (uint32_t x = 0; x < s_width; x++) {
                                    line[x * 3u + 0] = src[x * 4u + 0];
                                    line[x * 3u + 1] = src[x * 4u + 1];
                                    line[x * 3u + 2] = src[x * 4u + 2];
                                }
                                fwrite(line, 1, row, f);
                            }
                            free(line);
                            fclose(f);
                            fprintf(stderr, "  [FBWIN] wrote %s score=%u first=0x%08X\n",
                                    path, candidates[i].nonzero_score, candidates[i].first_pixel);
                            fflush(stderr);
                        }
                    }
                    free(tmp);
                }
            }
            if (best >= 0) {
                if (s_fb_debug) {
                    fprintf(stderr, "[FRAME] cand[%d]=0x%08X score=%u first=0x%08X chosen\n",
                    best, candidates[best].va, candidates[best].nonzero_score, candidates[best].first_pixel);
                    fflush(stderr);
                }
                pthread_mutex_lock(&s_state_lock);
                s_fb_va = candidates[best].va;
                s_fb_pitch = candidates[best].pitch;
                pthread_mutex_unlock(&s_state_lock);
            }
        }

        @autoreleasepool {
            render_frame();
        }
        if (now_ms() - last_print > 1000) {
            fprintf(stderr, "[FBWIN] frames_presented=%llu\n",
                    (unsigned long long)s_fb_frames_presented);
            fflush(stderr);
            last_print = now_ms();
        }
        {
            const char *dump = getenv("RECOMP_FB_DUMP");
            static int dumped;
            if (dump && !dumped && s_fb_frames_presented > 600) {
                dumped = 1;
                xbox_FramebufferDumpBmp(dump);
            }
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 16666667 };
        nanosleep(&delay, NULL);
        frame++;
    }
    fprintf(stderr, "[FBWIN] render thread exiting\n");
    fflush(stderr);
    return NULL;
}

void xbox_FramebufferWindowStart(void)
{
    const char *width, *height;

    if (!getenv("RECOMP_FB_WINDOW") || s_running)
        return;
    if (![NSThread isMainThread]) {
        fprintf(stderr, "[FBWIN] initialization must run on the main thread\n");
        return;
    }
    width = getenv("RECOMP_FB_WIDTH");
    height = getenv("RECOMP_FB_HEIGHT");
    if (width && strtoul(width, NULL, 0) > 0)
        s_width = (uint32_t)strtoul(width, NULL, 0);
    if (height && strtoul(height, NULL, 0) > 0)
        s_height = (uint32_t)strtoul(height, NULL, 0);
    s_test_pattern = getenv("RECOMP_FB_TEST_PATTERN") != NULL;
    s_fb_debug = getenv("RECOMP_FB_DEBUG") != NULL;

    @autoreleasepool {
        NSRect rect;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (!init_metal()) {
            fprintf(stderr, "[FBWIN] Metal initialization failed\n");
            return;
        }
        rect = NSMakeRect(0, 0, s_width, s_height);
        s_window = [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered defer:NO];
        [s_window setReleasedWhenClosed:NO];
        [s_window setTitle:@"Black - Xbox Recomp Framebuffer"];
        [s_window center];
        [s_window setLevel:NSFloatingWindowLevel];
        [s_window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorTransient];
        s_layer = [CAMetalLayer layer];
        s_layer.device = s_device;
        s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_layer.framebufferOnly = YES;
        s_layer.opaque = YES;
        s_layer.drawableSize = CGSizeMake(s_width * s_window.backingScaleFactor,
                                          s_height * s_window.backingScaleFactor);
        {
            /* Install a key-capable content view so the window receives
             * keyboard events (the Metal framebuffer is otherwise click-through
             * for input: Terminal owns stdin, and once this window is key the
             * tty reader sees nothing). See XBFrameKeyView above. */
            XBFrameKeyView *kv = [[XBFrameKeyView alloc] initWithFrame:rect];
            kv.wantsLayer = YES;
            s_layer.frame = kv.bounds;
            kv.layer = s_layer;
            [s_window setContentView:kv];
            [s_window makeFirstResponder:kv];
            [kv release];
        }
        [s_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        s_dump_at = now_ms() + 4000u;
        s_dumped = 0;
        s_running = 1;
        if (pthread_create(&s_render_thread, NULL, render_thread, NULL) != 0) {
            s_running = 0;
            fprintf(stderr, "[FBWIN] failed to start render thread\n");
            return;
        }
        s_render_thread_started = 1;
        fprintf(stderr, "  [FBWIN] window open (%ux%u)\n", s_width, s_height);
    }
}

/*
 * Drains NSApplication's pending events without blocking. Must be called
 * from the main thread, in a loop, by whoever owns the main thread while
 * the guest CPU runs on its own pthread - otherwise WindowServer never
 * finishes compositing the window itself even though this module's render
 * thread keeps presenting Metal drawables successfully.
 */
void xbox_PumpMainRunLoop(void)
{
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

void xbox_FramebufferWindowStop(void)
{
    s_running = 0;
    if (s_render_thread_started) {
        pthread_join(s_render_thread, NULL);
        s_render_thread_started = 0;
    }
    @autoreleasepool {
        [s_window close];
        [s_window release];
        [s_texture release];
        [s_pipeline release];
        [s_queue release];
        [s_device release];
        s_window = nil;
        s_layer = nil;
    }
}

#endif
