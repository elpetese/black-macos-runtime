/* Experimental ANGLE Metal context for the xemu renderer. */
#include "hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/gloffscreen.h"
#undef glShaderSource
#undef glTexImage2D
#undef glTexImage3D
#undef glReadPixels
#undef glTexParameteriv
#undef glTexParameteri
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

struct _GloContext {
    EGLContext context;
    EGLSurface surface;
};

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLConfig config;
static EGLContext shared = EGL_NO_CONTEXT;

static void GL_APIENTRY angle_debug(GLenum source, GLenum type, GLuint id,
                                    GLenum severity, GLsizei length,
                                    const GLchar *message, const void *user)
{
    static unsigned count;
    (void)source; (void)type; (void)severity; (void)length; (void)user;
    if (count++ < 100)
        fprintf(stderr, "[ANGLE GL] %u: %s\n", id, message);
}

/* All xemu shaders start with a desktop #version directive. GLES requires a
 * different version and explicit default precision for floating point. */
void angle_gl_shader_source(GLuint shader, GLsizei count,
                            const GLchar *const *source, const GLint *length)
{
    const GLchar **converted = calloc((size_t)count, sizeof(*converted));
    if (!converted)
        abort();
    for (GLsizei i = 0; i < count; i++) {
        const char *src = source[i];
        const char *end = !strncmp(src, "#version ", 9) ? strchr(src, '\n') : NULL;
        if (end) {
            const char *prefix = "#version 300 es\nprecision highp float;\n"
                                 "precision highp int;\n#define precise\n"
                                 "#define fma(a,b,c) ((a)*(b)+(c))\n";
            size_t size = strlen(prefix) + strlen(end + 1) + 1;
            char *dst = malloc(size);
            if (!dst)
                abort();
            snprintf(dst, size, "%s%s", prefix, end + 1);
            converted[i] = dst;
        } else {
            converted[i] = src;
        }
    }
    glShaderSource(shader, count, converted, NULL);
    for (GLsizei i = 0; i < count; i++)
        if (converted[i] != source[i])
            free((void *)converted[i]);
    free(converted);
}

static void check(EGLBoolean ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "[ANGLE] %s failed: EGL 0x%x\n", operation, eglGetError());
        abort();
    }
}

GloContext *glo_context_create(void)
{
    if (display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!platform_display) {
            fprintf(stderr, "[ANGLE] eglGetPlatformDisplayEXT unavailable\n");
            abort();
        }
        const EGLint platform_attrs[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
            EGL_NONE
        };
        display = platform_display(EGL_PLATFORM_ANGLE_ANGLE,
                                   EGL_DEFAULT_DISPLAY, platform_attrs);
        EGLint major = 0, minor = 0;
        check(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor),
              "eglInitialize Metal");
        check(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
        const EGLint config_attrs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        EGLint count = 0;
        check(eglChooseConfig(display, config_attrs, &config, 1, &count) && count,
              "eglChooseConfig");
    }
    GloContext *out = calloc(1, sizeof(*out));
    if (!out)
        abort();
    const EGLint pbuffer_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    out->surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
    check(out->surface != EGL_NO_SURFACE, "eglCreatePbufferSurface");
    out->context = eglCreateContext(display, config, shared, context_attrs);
    check(out->context != EGL_NO_CONTEXT, "eglCreateContext");
    if (shared == EGL_NO_CONTEXT)
        shared = out->context;
    glo_set_current(out);
    if (getenv("RECOMP_ANGLE_DIAG")) {
        PFNGLDEBUGMESSAGECALLBACKKHRPROC debug_callback =
            (PFNGLDEBUGMESSAGECALLBACKKHRPROC)eglGetProcAddress("glDebugMessageCallbackKHR");
        if (!debug_callback)
            debug_callback = (PFNGLDEBUGMESSAGECALLBACKKHRPROC)
                eglGetProcAddress("glDebugMessageCallback");
        fprintf(stderr, "[ANGLE] debug callback %p\n", (void *)debug_callback);
        if (debug_callback) {
            glEnable(GL_DEBUG_OUTPUT_KHR);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
            debug_callback(angle_debug, NULL);
        }
    }
    fprintf(stderr, "[ANGLE] Metal context: %s / %s\n",
            glGetString(GL_VERSION), glGetString(GL_RENDERER));
    return out;
}

void glo_set_current(GloContext *context)
{
    EGLSurface surface = context ? context->surface : EGL_NO_SURFACE;
    EGLContext gl = context ? context->context : EGL_NO_CONTEXT;
    check(eglMakeCurrent(display, surface, surface, gl), "eglMakeCurrent");
}

void glo_context_destroy(GloContext *context)
{
    if (!context)
        return;
    glo_set_current(NULL);
    eglDestroyContext(display, context->context);
    eglDestroySurface(display, context->surface);
    free(context);
}

/* ── Formats ES lacks ─────────────────────────────────────────────────── */

#define T8888     0x8035u   /* GL_UNSIGNED_INT_8_8_8_8 */
#define T1555REV  0x8366u   /* GL_UNSIGNED_SHORT_1_5_5_5_REV */
#define T4444REV  0x8365u   /* GL_UNSIGNED_SHORT_4_4_4_4_REV */

static int is_foreign(GLenum format, GLenum type)
{
    return format == GL_BGRA_EXT || type == T8888 || type == T1555REV ||
           type == T4444REV;
}

static unsigned src_bpp(GLenum type)
{
    return type == T1555REV || type == T4444REV ? 2 : 4;
}

/* One pixel in (format, type) to RGBA bytes. */
static void to_rgba(GLenum format, GLenum type, const uint8_t *p, uint8_t *o)
{
    if (type == T1555REV) {
        unsigned v = p[0] | p[1] << 8, b = v & 31, g = (v >> 5) & 31,
                 r = (v >> 10) & 31;
        o[0] = r << 3 | r >> 2; o[1] = g << 3 | g >> 2; o[2] = b << 3 | b >> 2;
        o[3] = v & 0x8000 ? 255 : 0;
    } else if (type == T4444REV) {
        unsigned v = p[0] | p[1] << 8;
        o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17;
        o[2] = (v & 15) * 17;        o[3] = (v >> 12) * 17;
    } else if (type == T8888) {
        /* Component order runs from the most significant byte. */
        if (format == GL_BGRA_EXT) {
            o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0];
        } else {
            o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0];
        }
    } else { /* BGRA bytes */
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
    }
}

static void from_rgba(GLenum format, GLenum type, const uint8_t *i, uint8_t *p)
{
    if (type == T1555REV) {
        unsigned v = (i[2] >> 3) | (i[1] >> 3) << 5 | (i[0] >> 3) << 10 |
                     (i[3] >= 128) << 15;
        p[0] = v; p[1] = v >> 8;
    } else if (type == T4444REV) {
        unsigned v = (i[2] >> 4) | (i[1] >> 4) << 4 | (i[0] >> 4) << 8 |
                     (i[3] >> 4) << 12;
        p[0] = v; p[1] = v >> 8;
    } else if (type == T8888) {
        if (format == GL_BGRA_EXT) {
            p[1] = i[0]; p[2] = i[1]; p[3] = i[2]; p[0] = i[3];
        } else {
            p[3] = i[0]; p[2] = i[1]; p[1] = i[2]; p[0] = i[3];
        }
    } else {
        p[2] = i[0]; p[1] = i[1]; p[0] = i[2]; p[3] = i[3];
    }
}

/* Sized ES format taking RGBA bytes; *opaque when the source has no alpha. */
static GLint rgba_internal(GLint internal, int *opaque)
{
    *opaque = 0;
    switch (internal) {
    case GL_RGB8:  *opaque = 1; return GL_RGBA8;
    case 0x8050:   *opaque = 1; return GL_RGB5_A1;   /* GL_RGB5 */
    case GL_RGB5_A1:
    case GL_RGBA4:
    case GL_RGBA8: return internal;
    default:       return GL_RGBA8;
    }
}

/* Tightly packed RGBA copy of an upload, honouring the unpack state. */
static uint8_t *convert_upload(GLenum format, GLenum type, GLsizei w,
                               GLsizei h, GLsizei d, const void *pixels,
                               int opaque)
{
    GLint rl, skip_px, skip_rows, align, ih, skip_img;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_px);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &ih);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_img);
    unsigned bpp = src_bpp(type);
    size_t row = (size_t)(rl ? rl : w) * bpp;
    row = (row + align - 1) / align * align;
    size_t slice = row * (ih ? ih : h);
    const uint8_t *src = (const uint8_t *)pixels + skip_img * slice +
                         skip_rows * row + skip_px * bpp;
    uint8_t *out = malloc((size_t)w * h * d * 4), *o = out;
    if (!out)
        abort();
    for (GLsizei z = 0; z < d; z++)
        for (GLsizei y = 0; y < h; y++) {
            const uint8_t *p = src + z * slice + y * row;
            for (GLsizei x = 0; x < w; x++, p += bpp, o += 4) {
                to_rgba(format, type, p, o);
                if (opaque)
                    o[3] = 255;
            }
        }
    return out;
}

typedef struct { GLint rl, skip_px, skip_rows, align, ih, skip_img; } Unpack;

static void unpack_tight(Unpack *u)
{
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &u->rl);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &u->skip_px);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &u->skip_rows);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &u->align);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &u->ih);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &u->skip_img);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}

static void unpack_restore(const Unpack *u)
{
    glPixelStorei(GL_UNPACK_ROW_LENGTH, u->rl);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, u->skip_px);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, u->skip_rows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, u->align);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, u->ih);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, u->skip_img);
}

void angle_gl_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels)
{
    if (!is_foreign(format, type)) {
        glTexImage2D(target, level, internalformat, width, height, border,
                     format, type, pixels);
        return;
    }
    int opaque;
    GLint internal = rgba_internal(internalformat, &opaque);
    if (!pixels) {
        glTexImage2D(target, level, internal, width, height, border, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        return;
    }
    uint8_t *rgba = convert_upload(format, type, width, height, 1, pixels,
                                   opaque);
    Unpack u;
    unpack_tight(&u);
    glTexImage2D(target, level, internal, width, height, border, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    unpack_restore(&u);
    free(rgba);
}

void angle_gl_tex_image_3d(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLsizei depth,
                           GLint border, GLenum format, GLenum type,
                           const void *pixels)
{
    if (!is_foreign(format, type)) {
        glTexImage3D(target, level, internalformat, width, height, depth,
                     border, format, type, pixels);
        return;
    }
    int opaque;
    GLint internal = rgba_internal(internalformat, &opaque);
    if (!pixels) {
        glTexImage3D(target, level, internal, width, height, depth, border,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        return;
    }
    uint8_t *rgba = convert_upload(format, type, width, height, depth, pixels,
                                   opaque);
    Unpack u;
    unpack_tight(&u);
    glTexImage3D(target, level, internal, width, height, depth, border,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    unpack_restore(&u);
    free(rgba);
}

/* BGRA bytes read directly (EXT_read_format_bgra); the packed types are read
 * as RGBA and packed on the CPU. Not for reads into a pack buffer. */
void angle_gl_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, void *pixels)
{
    if (!is_foreign(format, type) ||
        (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE)) {
        glReadPixels(x, y, width, height, format, type, pixels);
        return;
    }
    GLint rl, align, skip_px, skip_rows;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_px);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    uint8_t *rgba = malloc((size_t)width * height * 4);
    if (!rgba)
        abort();
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_px);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);

    unsigned bpp = src_bpp(type);
    size_t row = (size_t)(rl ? rl : width) * bpp;
    row = (row + align - 1) / align * align;
    uint8_t *dst = (uint8_t *)pixels + skip_rows * row + skip_px * bpp;
    for (GLsizei j = 0; j < height; j++) {
        uint8_t *p = dst + j * row;
        const uint8_t *i = rgba + (size_t)j * width * 4;
        for (GLsizei k = 0; k < width; k++, p += bpp, i += 4)
            from_rgba(format, type, i, p);
    }
    free(rgba);
}

void angle_gl_tex_parameteriv(GLenum target, GLenum pname, const GLint *params)
{
    if (pname == 0x8E46) { /* GL_TEXTURE_SWIZZLE_RGBA */
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, params[0]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, params[1]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, params[2]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, params[3]);
        return;
    }
    glTexParameteriv(target, pname, params);
}
