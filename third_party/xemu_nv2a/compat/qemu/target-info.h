#ifndef XEMU_COMPAT_TARGET_INFO_H
#define XEMU_COMPAT_TARGET_INFO_H
#include <stdbool.h>
static inline bool target_big_endian(void) { return false; }
static inline const char *target_name(void) { return "i386"; }
#endif
