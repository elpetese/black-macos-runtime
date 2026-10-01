# xemu's NV2A PGRAPH renderer (OpenGL path), built outside QEMU.
# Sources are xemu's hw/xbox/nv2a, unmodified where possible; compat/ stands
# in for the QEMU headers they include. See XEMU_REVISION for the upstream
# commit. Licences: COPYING.LIB (LGPL-2.1+, most of pgraph), COPYING (GPL-2,
# glsl/psh.c and glsl/vsh-prog.c), nv2a_vsh_cpu/LICENSE (public domain).
find_package(PkgConfig REQUIRED)
set(XEMU_ANGLE_LIB_DIR "" CACHE PATH "Directory containing ANGLE libEGL.dylib and libGLESv2.dylib")
if(XEMU_ANGLE_LIB_DIR)
    pkg_check_modules(XEMU_NV2A_DEPS REQUIRED glib-2.0 sdl3 libxxhash)
    set(GLO_CONTEXT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/angle_context.c)
else()
    pkg_check_modules(XEMU_NV2A_DEPS REQUIRED glib-2.0 epoxy sdl3 libxxhash)
    set(GLO_CONTEXT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/sdl.c)
endif()

set(N ${CMAKE_CURRENT_SOURCE_DIR}/hw/xbox/nv2a)
add_library(xemu_nv2a OBJECT
    ${N}/pgraph/pgraph.c ${N}/pgraph/profile.c ${N}/pgraph/rdi.c ${N}/pgraph/s3tc.c
    ${N}/pgraph/swizzle.c ${N}/pgraph/texture.c ${N}/pgraph/vertex.c
    ${N}/pgraph/gl/blit.c ${N}/pgraph/gl/debug.c ${N}/pgraph/gl/display.c ${N}/pgraph/gl/draw.c
    ${N}/pgraph/gl/gpuprops.c ${N}/pgraph/gl/renderer.c ${N}/pgraph/gl/reports.c
    ${N}/pgraph/gl/shaders.c ${N}/pgraph/gl/surface.c ${N}/pgraph/gl/texture.c ${N}/pgraph/gl/vertex.c
    ${N}/pgraph/glsl/common.c ${N}/pgraph/glsl/geom.c ${N}/pgraph/glsl/psh.c ${N}/pgraph/glsl/shaders.c
    ${N}/pgraph/glsl/vsh.c ${N}/pgraph/glsl/vsh-ff.c ${N}/pgraph/glsl/vsh-prog.c
    ${N}/pgraph/thirdparty/gloffscreen/common.c ${GLO_CONTEXT_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/qemu/fast-hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src/nv2a_vsh_cpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src/nv2a_vsh_emulator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src/nv2a_vsh_emulator_execution_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_glue.c
)
target_include_directories(xemu_nv2a PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/compat ${CMAKE_CURRENT_SOURCE_DIR} ${N}
    ${N}/pgraph ${N}/pgraph/thirdparty/gloffscreen
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src
    ${CMAKE_CURRENT_SOURCE_DIR}/nv2a_vsh_cpu/src/nv2a_vsh_emulator
    ${XEMU_NV2A_DEPS_INCLUDE_DIRS})
target_compile_options(xemu_nv2a PRIVATE -std=gnu11 -w -O2)
# OBJECT, not STATIC: the OpenGL renderer registers itself from a constructor
# nothing references, which a static archive would silently drop.
target_compile_definitions(xemu_nv2a PRIVATE CONFIG_OPENGL=1 XBOXRECOMP_NO_DIRTY_TRACKING=1)
if(XEMU_ANGLE_LIB_DIR)
    target_compile_definitions(xemu_nv2a PRIVATE XBOXRECOMP_ANGLE=1)
    target_include_directories(xemu_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../angle_headers)
    find_library(XEMU_EGL_LIBRARY NAMES EGL libEGL.dylib PATHS ${XEMU_ANGLE_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    find_library(XEMU_GLES_LIBRARY NAMES GLESv2 libGLESv2.dylib PATHS ${XEMU_ANGLE_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
endif()
# src/nv2a (the older register-level extraction behind RECOMP_NV2A_MMIO)
# defines some of the same functions; keep xemu's under their own names.
target_compile_definitions(xemu_nv2a PRIVATE
    pgraph_method=xemu_pgraph_method pgraph_read=xemu_pgraph_read
    pgraph_write=xemu_pgraph_write nv_dma_load=xemu_nv_dma_load
    nv_dma_map=xemu_nv_dma_map nv2a_update_irq=xemu_nv2a_update_irq)
target_link_directories(xemu_nv2a PUBLIC ${XEMU_NV2A_DEPS_LIBRARY_DIRS})
if(XEMU_ANGLE_LIB_DIR)
    target_link_libraries(xemu_nv2a PUBLIC ${XEMU_NV2A_DEPS_LIBRARIES} ${XEMU_EGL_LIBRARY} ${XEMU_GLES_LIBRARY})
else()
    target_link_libraries(xemu_nv2a PUBLIC ${XEMU_NV2A_DEPS_LIBRARIES} "-framework OpenGL")
endif()
