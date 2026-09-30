#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)

static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static inline void stl_le_p(void *p,uint32_t v) { memcpy(p,&v,4); }

#define g_new0(t,n) ((t *)calloc((n),sizeof(t)))
#define g_new(t,n) ((t *)malloc((n)*sizeof(t)))
#define g_free(p) free(p)

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
