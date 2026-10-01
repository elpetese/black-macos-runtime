#ifndef XBOXRECOMP_ANGLE_GL_COMPAT_H
#define XBOXRECOMP_ANGLE_GL_COMPAT_H

/* Desktop GL enums referenced by xemu. These aliases permit compilation;
 * the ES renderer must translate unsupported formats before using them. */
/* EXT_texture_format_BGRA8888 / EXT_read_format_bgra: same value as the
 * desktop token. Uploads in BGRA (and the packed desktop types below) are
 * converted to RGBA bytes by angle_gl_tex_image_*; reads use it directly. */
#define GL_BGRA GL_BGRA_EXT
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_RGB5 0x8050
#define GL_R16 0x822A
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV GL_UNSIGNED_BYTE
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_FILL 0x1B02
#define GL_DEPTH_CLAMP 0x864F
#define GL_FIRST_VERTEX_CONVENTION 0x8E4D
#define GL_LINE_SMOOTH 0x0B20
#define GL_POLYGON_OFFSET_LINE 0x2A02
#define GL_POLYGON_OFFSET_POINT 0x2A01
#define GL_POLYGON_SMOOTH 0x0B41
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_SAMPLES_PASSED GL_ANY_SAMPLES_PASSED
#define GL_GEOMETRY_SHADER 0x8DD9
#define GL_SMOOTH_LINE_WIDTH_RANGE GL_ALIASED_LINE_WIDTH_RANGE
/* xemu draws NV2A quads as LINES_ADJACENCY for a geometry shader to split.
 * ES has neither; the token is kept so draw.c can index quads as triangles.
 * A quad strip has the vertex order of a triangle strip. */
#define GL_LINES_ADJACENCY 0x000A
#define GL_LINE_STRIP_ADJACENCY GL_TRIANGLE_STRIP
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
static inline void angle_gl_enable(GLenum cap) {
    if (cap != GL_LINE_SMOOTH &&
        cap != GL_POLYGON_OFFSET_LINE && cap != GL_POLYGON_OFFSET_POINT &&
        cap != GL_POLYGON_SMOOTH && cap != GL_PROGRAM_POINT_SIZE)
        glEnable(cap);
}
static inline void angle_gl_disable(GLenum cap) {
    if (cap != GL_LINE_SMOOTH &&
        cap != GL_POLYGON_OFFSET_LINE && cap != GL_POLYGON_OFFSET_POINT &&
        cap != GL_POLYGON_SMOOTH && cap != GL_PROGRAM_POINT_SIZE)
        glDisable(cap);
}
#define glEnable angle_gl_enable
#define glDisable angle_gl_disable
#define glClearDepth(value) glClearDepthf(value)
/* ANGLE_provoking_vertex; GL_FIRST_VERTEX_CONVENTION has the same value. */
GL_APICALL void GL_APIENTRY glProvokingVertexANGLE(GLenum mode);
#define glProvokingVertex(mode) glProvokingVertexANGLE(mode)
static inline void angle_gl_multi_draw_arrays(GLenum mode, const GLint *first,
                                                const GLsizei *count, GLsizei draws) {
    for (GLsizei i = 0; i < draws; i++)
        glDrawArrays(mode, first[i], count[i]);
}
#define glMultiDrawArrays angle_gl_multi_draw_arrays
void angle_gl_shader_source(GLuint shader, GLsizei count,
                            const GLchar *const *source, const GLint *length);
#define glShaderSource angle_gl_shader_source
/* Both call sites bind the program immediately before setting uniforms. */
#define glProgramUniform1i(program, location, value) glUniform1i(location, value)
#define glProgramUniform2f(program, location, x, y) glUniform2f(location, x, y)
/* The two calls in xemu only request fill, the ES default. */
#define glPolygonMode(face, mode) ((void)0)

/* Texture uploads and reads in formats ES lacks (BGRA, the packed 16-bit
 * _REV types, UNSIGNED_INT_8_8_8_8) go through a CPU conversion to RGBA
 * bytes; TEXTURE_SWIZZLE_RGBA is split into its four ES parameters. */
void angle_gl_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels);
void angle_gl_tex_image_3d(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLsizei depth,
                           GLint border, GLenum format, GLenum type,
                           const void *pixels);
void angle_gl_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, void *pixels);
void angle_gl_tex_parameteriv(GLenum target, GLenum pname, const GLint *params);
#define glTexImage2D angle_gl_tex_image_2d
#define glTexImage3D angle_gl_tex_image_3d
#define glReadPixels angle_gl_read_pixels
#define glTexParameteriv angle_gl_tex_parameteriv

/* No 1D textures, LOD bias or border clamp in ES 3.0 (nor in this ANGLE's
 * extensions): binds of 1D are dropped, the bias ignored, border clamping
 * falls back to edge clamping. */
static inline void angle_gl_bind_texture(GLenum target, GLuint texture) {
    if (target != GL_TEXTURE_1D)
        glBindTexture(target, texture);
}
static inline void angle_gl_tex_parameteri(GLenum target, GLenum pname, GLint v) {
    if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T ||
         pname == GL_TEXTURE_WRAP_R) && v == GL_CLAMP_TO_BORDER)
        v = GL_CLAMP_TO_EDGE;
    glTexParameteri(target, pname, v);
}
static inline void angle_gl_tex_parameterf(GLenum target, GLenum pname, GLfloat v) {
    if (pname != GL_TEXTURE_LOD_BIAS)
        glTexParameterf(target, pname, v);
}
static inline void angle_gl_tex_parameterfv(GLenum target, GLenum pname,
                                            const GLfloat *v) {
    if (pname != GL_TEXTURE_BORDER_COLOR)
        glTexParameterfv(target, pname, v);
}
#define glBindTexture angle_gl_bind_texture
#define glTexParameteri angle_gl_tex_parameteri
#define glTexParameterf angle_gl_tex_parameterf
#define glTexParameterfv angle_gl_tex_parameterfv

#endif
