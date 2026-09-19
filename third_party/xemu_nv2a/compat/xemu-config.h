/* xboxrecomp: the slice of xemu's generated settings the renderer reads. */
#ifndef XEMU_COMPAT_CONFIG_H
#define XEMU_COMPAT_CONFIG_H
#include <stdbool.h>
typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT
} CONFIG_DISPLAY_RENDERER;
struct config {
    struct { CONFIG_DISPLAY_RENDERER renderer;
             struct { int surface_scale; } quality;
             struct { bool validation_layers; bool debug_shaders;
                      bool assert_on_validation_msg; } vulkan; } display;
    struct { bool cache_shaders; } perf;
};
extern struct config g_config;
#endif
