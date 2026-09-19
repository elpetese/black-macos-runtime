/* xboxrecomp: minimal stand-in for QEMU's osdep.h, enough to build xemu's
 * NV2A PGRAPH (renderer) outside QEMU. Only what the renderer uses. */
#ifndef XEMU_COMPAT_OSDEP_H
#define XEMU_COMPAT_OSDEP_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <glib.h>

typedef uint64_t hwaddr;
typedef uint64_t ram_addr_t;
#define HWADDR_PRIx PRIx64

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define QEMU_ALIGN_DOWN(n, m) ((n) / (m) * (m))
#define QEMU_ALIGN_UP(n, m)   QEMU_ALIGN_DOWN((n) + (m) - 1, (m))
#define ROUND_UP(n, d)        (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d)    (((n) + (d) - 1) / (d))
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define QEMU_PACKED __attribute__((packed))
#define QEMU_NORETURN __attribute__((noreturn))
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), "QEMU_BUILD_BUG_ON")
#define G_NORETURN_ATTR
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
#define TARGET_PAGE_ALIGN(a) QEMU_ALIGN_UP((a), TARGET_PAGE_SIZE)
#define HOST_BIG_ENDIAN 0

#include "qemu/compiler.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/bitops.h"
#include "qemu/units.h"

static inline void qemu_log_mask_dummy(int m, const char *f, ...) { (void)m; (void)f; }
#define qemu_log_mask(...) ((void)0)
#define qemu_log(...) ((void)0)
#define LOG_UNIMP 0
#define LOG_GUEST_ERROR 0

#include "qemu/error-report.h"

#endif
