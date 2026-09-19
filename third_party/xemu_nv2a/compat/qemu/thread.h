/* xboxrecomp: QEMU thread primitives on pthreads. */
#ifndef XEMU_COMPAT_THREAD_H
#define XEMU_COMPAT_THREAD_H
#include <pthread.h>
#include <stdbool.h>

typedef struct QemuMutex { pthread_mutex_t m; } QemuMutex;
typedef struct QemuCond  { pthread_cond_t  c; } QemuCond;
typedef struct QemuThread { pthread_t t; } QemuThread;
typedef struct QemuEvent {
    pthread_mutex_t m; pthread_cond_t c; bool set;
} QemuEvent;
typedef struct QemuSpin { pthread_mutex_t m; } QemuSpin;
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

static inline void qemu_mutex_init(QemuMutex *m) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a); pthread_mutexattr_destroy(&a);
}
static inline void qemu_mutex_destroy(QemuMutex *m) { pthread_mutex_destroy(&m->m); }
static inline void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(&m->m); }
static inline void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(&m->m); }
static inline int  qemu_mutex_trylock(QemuMutex *m) { return pthread_mutex_trylock(&m->m); }
static inline void qemu_cond_init(QemuCond *c) { pthread_cond_init(&c->c, NULL); }
static inline void qemu_cond_destroy(QemuCond *c) { pthread_cond_destroy(&c->c); }
static inline void qemu_cond_signal(QemuCond *c) { pthread_cond_signal(&c->c); }
static inline void qemu_cond_broadcast(QemuCond *c) { pthread_cond_broadcast(&c->c); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) { pthread_cond_wait(&c->c, &m->m); }
static inline void qemu_event_init(QemuEvent *e, bool init) {
    pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL); e->set = init;
}
static inline void qemu_event_destroy(QemuEvent *e) {
    pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->c);
}
static inline void qemu_event_set(QemuEvent *e) {
    pthread_mutex_lock(&e->m); e->set = true; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_reset(QemuEvent *e) {
    pthread_mutex_lock(&e->m); e->set = false; pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_wait(QemuEvent *e) {
    pthread_mutex_lock(&e->m); while (!e->set) pthread_cond_wait(&e->c, &e->m); pthread_mutex_unlock(&e->m);
}
static inline void qemu_spin_init(QemuSpin *s) { pthread_mutex_init(&s->m, NULL); }
static inline void qemu_spin_lock(QemuSpin *s) { pthread_mutex_lock(&s->m); }
static inline void qemu_spin_unlock(QemuSpin *s) { pthread_mutex_unlock(&s->m); }
static inline void qemu_thread_create(QemuThread *t, const char *name,
                                      void *(*fn)(void *), void *arg, int mode) {
    (void)name; pthread_create(&t->t, NULL, fn, arg);
    if (mode == QEMU_THREAD_DETACHED) pthread_detach(t->t);
}
static inline void *qemu_thread_join(QemuThread *t) { void *r = NULL; pthread_join(t->t, &r); return r; }
#endif
