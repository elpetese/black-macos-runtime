#ifndef XEMU_COMPAT_MAIN_LOOP_H
#define XEMU_COMPAT_MAIN_LOOP_H
#include "qemu/thread.h"
/* The big QEMU lock has nothing to guard here: the renderer is driven from
 * one thread. */
static inline void bql_lock(void) {}
static inline void bql_unlock(void) {}
static inline bool bql_locked(void) { return true; }
#endif
