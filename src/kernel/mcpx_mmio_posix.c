/**
 * MCPX register interception on POSIX / ARM64.
 *
 * Windows routes APU register faults through a vectored exception handler
 * that decodes the faulting x86-64 instruction (src/apu/apu_mmio_hook.c).
 * Nothing equivalent existed elsewhere, so on macOS the MCPX aperture is plain
 * memory -- and plain memory cannot answer a register whose value depends on
 * the write that preceded it.
 *
 * The case that forced this: DirectSound's AC'97 channel reset (Black,
 * sub_002228D9) writes Reset Registers (bit 1) to a channel's control register,
 * reads it back ONCE, and then loops `test cl,cl; jne self`. The bit
 * self-clears on hardware; in plain memory it stays set and the thread spins
 * forever. Only an emulated write can clear it before the read.
 *
 * Mechanism: the register page is PROT_NONE. On a fault inside it the handler
 * decodes the host instruction (integer LDR/STR family), recomputes the
 * effective address from the saved registers, checks it against the fault
 * address, performs the access against a shadow register block, and resumes
 * past the instruction.
 */

#include "mcpx_mmio.h"

#if !defined(_WIN32) && defined(__aarch64__)

#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#define AC97_VA       0xFEC00000u
#define AC97_SHADOW   0x10000u

/* AC'97 bus master (MCPX ACI), offsets from the register base. */
#define AC97_GLOB_STA           0x130u
#define AC97_GLOB_STA_CODEC_RDY 0x100u
/* The XDK's channel table at 0x2245E8 holds 0x10 and 0x70: analog out at
 * 0x110 and S/PDIF out at 0x170. Per channel: BDBAR +0, CIV +4, LVI +5,
 * SR +6 (2 bytes), PICB +8 (2 bytes), PIV +0xA, CR +0xB. */
#define AC97_CR_RPBM            0x01u
#define AC97_CR_RESET_REGS      0x02u
#define AC97_CR_LVBIE           0x04u
#define AC97_CR_FEIE            0x08u
#define AC97_CR_IOCE            0x10u
#define AC97_SR_DCH             0x01u
#define AC97_SR_CELV            0x02u
#define AC97_SR_LVBCI           0x04u
#define AC97_SR_BCIS            0x08u
#define AC97_SR_FIFOE           0x10u
#define AC97_SR_W1C             0x1Cu   /* LVBCI | BCIS | FIFOE */
#define AC97_BDL_IOC            0x80000000u
#define AC97_RATE_HZ            48000u

extern ptrdiff_t xbox_GetMemoryOffset(void);

static uint8_t   g_ac97[AC97_SHADOW];
static uintptr_t g_ac97_host = 0;
static int       g_trace     = -1;
static unsigned  g_trace_n   = 0;

/* Intercepted register blocks. Each is a run of host pages made PROT_NONE
 * whose accesses are decoded and handed to a device model. */
#define MMIO_MAX_REGIONS 8
static struct {
    uintptr_t          host_base;   /* host address of guest_va */
    size_t             len;         /* whole host pages */
    uint32_t           guest_va;
    xbox_mmio_read_fn  read;
    xbox_mmio_write_fn write;
    void              *ctx;
    const char        *name;
} g_regions[MMIO_MAX_REGIONS];
static int g_region_count;

/* The shadow is touched from the fault handler on any guest thread and from
 * the playback tick on the timer thread. Never held while guest code runs:
 * the ISR the tick asks for faults straight back in here. */
static atomic_flag g_ac97_lock = ATOMIC_FLAG_INIT;
static void ac97_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_ac97_lock, memory_order_acquire))
        sched_yield();
}
static void ac97_unlock(void)
{
    atomic_flag_clear_explicit(&g_ac97_lock, memory_order_release);
}

/* Bus-master output channels, and the GLOB_STA bit each one raises. The bits
 * are the XDK's own table at 0x2245F4 (0x40, 0x10), which its ISR tests before
 * reading the channel's SR. */
static struct {
    uint32_t base;
    uint8_t  glob_bit;
    uint64_t next_due_ns;       /* 0: not playing a buffer */
} g_chan[2] = { { 0x110u, 0x40u, 0 }, { 0x170u, 0x10u, 0 } };

static uint16_t sh16(uint32_t off) { return (uint16_t)(g_ac97[off] | (g_ac97[off + 1] << 8)); }
static void set16(uint32_t off, uint16_t v) { g_ac97[off] = (uint8_t)v; g_ac97[off + 1] = (uint8_t)(v >> 8); }
static uint32_t sh32(uint32_t off) { return (uint32_t)sh16(off) | ((uint32_t)sh16(off + 2) << 16); }

/* A channel whose SR holds a status its CR enables is interrupting. */
static int ac97_channel_irq(uint32_t base)
{
    uint8_t cr = g_ac97[base + 0xB];
    uint8_t sr = g_ac97[base + 0x6];
    return ((sr & AC97_SR_BCIS) && (cr & AC97_CR_IOCE))
        || ((sr & AC97_SR_LVBCI) && (cr & AC97_CR_LVBIE))
        || ((sr & AC97_SR_FIFOE) && (cr & AC97_CR_FEIE));
}

static int ac97_channel_base(uint32_t off, uint32_t *base)
{
    if (off >= 0x110u && off < 0x120u) { *base = 0x110u; return 1; }
    if (off >= 0x170u && off < 0x180u) { *base = 0x170u; return 1; }
    return 0;
}

static void ac97_write_byte(uint32_t off, uint8_t v)
{
    uint32_t base;

    if (off >= AC97_SHADOW)
        return;
    if (ac97_channel_base(off, &base)) {
        uint32_t reg = off - base;
        if (reg == 0xB && (v & AC97_CR_RESET_REGS)) {
            /* Reset Registers: every channel register returns to its reset
             * value, and RR itself reads back clear. */
            memset(g_ac97 + base, 0, 0xC);
            g_ac97[base + 6] = AC97_SR_DCH;
            g_chan[base == 0x110u ? 0 : 1].next_due_ns = 0;
            return;
        }
        if (reg == 0xB) {
            /* Run/Pause Bus Master: the controller leaves the halted state as
             * soon as it is told to run, and halts when told to pause. The
             * playback tick only consumes buffers on a running channel. */
            if (v & AC97_CR_RPBM)
                g_ac97[base + 6] &= (uint8_t)~AC97_SR_DCH;
            else
                g_ac97[base + 6] |= AC97_SR_DCH;
        }
        if (reg == 0x6) {
            uint8_t keep = g_ac97[off] & (uint8_t)~(v & AC97_SR_W1C);
            g_ac97[off] = keep;
            return;
        }
    }
    g_ac97[off] = v;
}

static uint64_t ac97_read(uint32_t off, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t o = off + i;
        uint8_t b = o < AC97_SHADOW ? g_ac97[o] : 0;
        if (o == AC97_GLOB_STA + 1)
            b |= (uint8_t)(AC97_GLOB_STA_CODEC_RDY >> 8);
        if (o == AC97_GLOB_STA) {
            /* Channel interrupt bits mirror their SR, as on hardware: the ISR
             * clears SR, never these, and writes GLOB_STA back unchanged. */
            b &= (uint8_t)~(g_chan[0].glob_bit | g_chan[1].glob_bit);
            for (int c = 0; c < 2; c++)
                if (ac97_channel_irq(g_chan[c].base))
                    b |= g_chan[c].glob_bit;
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void ac97_write(uint32_t off, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        ac97_write_byte(off + i, (uint8_t)(v >> (8 * i)));
}

int xbox_mmio_read(uint32_t guest_va, unsigned size, uint64_t *value)
{
    for (int r = 0; r < g_region_count; r++) {
        uint32_t base = g_regions[r].guest_va;
        if (guest_va >= base && guest_va - base + size <= g_regions[r].len) {
            *value = g_regions[r].read(g_regions[r].ctx, guest_va - base, size);
            return 1;
        }
    }
    return 0;
}

static uint64_t ac97_region_read(void *ctx, uint32_t off, unsigned n)
{
    uint64_t v;
    (void)ctx;
    ac97_lock();
    v = ac97_read(off, n);
    ac97_unlock();
    return v;
}

static void ac97_region_write(void *ctx, uint32_t off, uint64_t v, unsigned n)
{
    (void)ctx;
    ac97_lock();
    ac97_write(off, v, n);
    ac97_unlock();
}

int xbox_mcpx_mmio_arm_ac97(void *ac97_host_base)
{
    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0 || (size_t)ps > AC97_SHADOW)
        return 0;
    memcpy(g_ac97, ac97_host_base, (size_t)ps);
    g_ac97[AC97_GLOB_STA + 1] |= (uint8_t)(AC97_GLOB_STA_CODEC_RDY >> 8);
    if (!xbox_mmio_intercept(AC97_VA, (uint32_t)ps, ac97_region_read,
                             ac97_region_write, NULL, "AC97"))
        return 0;
    g_ac97_host = (uintptr_t)ac97_host_base;
    return 1;
}

int xbox_mmio_intercept(uint32_t guest_va, uint32_t size,
                        xbox_mmio_read_fn read, xbox_mmio_write_fn write,
                        void *ctx, const char *name)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t host;
    size_t len;

    if (ps <= 0 || g_region_count >= MMIO_MAX_REGIONS || !read || !write)
        return 0;
    if (guest_va & (uint32_t)(ps - 1)) {
        fprintf(stderr, "[MMIO] %s at 0x%08X is not page aligned\n", name, guest_va);
        return 0;
    }
    host = (uintptr_t)xbox_GetMemoryOffset() + guest_va;
    len = ((size_t)size + (size_t)ps - 1) & ~((size_t)ps - 1);

    /* Register the handler BEFORE making the page PROT_NONE. Host code reads
     * the guest address space directly (xbox_mmio_read, and the NV2A/APU/AC97
     * ack threads dereference the guest window as an ordinary pointer), so a
     * page may be made inaccessible only once xbox_mmio_read is able to route
     * accesses to this handler. Doing mprotect first leaves a window in which
     * the page faults but no handler is registered yet, so a direct touch
     * faults instead of being emulated. Observed as a SIGBUS in nv2a_ack_thread
     * on the APU GP counter at 0xFE820010 during boot: g_apu_mmio_trapped is
     * Windows-only, so the ack thread's "leave device registers alone" guard
     * never trips on POSIX, and its check-then-poke hit the unmapped page. */
    g_regions[g_region_count].host_base = host;
    g_regions[g_region_count].len       = len;
    g_regions[g_region_count].guest_va  = guest_va;
    g_regions[g_region_count].read      = read;
    g_regions[g_region_count].write     = write;
    g_regions[g_region_count].ctx       = ctx;
    g_regions[g_region_count].name      = name;
    g_region_count++;
    if (mprotect((void *)host, len, PROT_NONE) != 0) {
        perror("[MMIO] mprotect");
        /* Roll the partial registration back so the plain-RAM fallback below
         * reads the zeroed backing store instead of dangling callbacks. */
        g_region_count--;
        return 0;
    }
    fprintf(stderr, "  %s: 0x%08X..0x%08X intercepted\n", name, guest_va,
            guest_va + (uint32_t)len);
    return 1;
}

/* ── Playback ──────────────────────────────────────────────────
 *
 * DirectSound drives AC'97 output entirely by interrupt: it programs a buffer
 * descriptor list, sets Run, and refills from its DPC each time the ISR sees
 * Buffer Completion. Without a controller consuming buffers the audio clock
 * never moves -- and Black's intro video, which paces itself by audio, never
 * produces a frame.
 *
 * Buffers are consumed in real time. A BDL entry is { address, control },
 * control = IOC (bit 31) | BUP (bit 30) | length in 16-bit samples; measured on
 * Black: 0x1000 samples over an 8192-byte buffer. At 48 kHz stereo one sample
 * pair is a frame. When the last valid buffer completes the controller keeps
 * replaying it (the underrun policy the entries ask for) and reports LVBCI and
 * CELV rather than halting, so a driver that loops one buffer keeps getting
 * completions. Nothing is mixed to the host yet. */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int bdl_entry(uint32_t base, uint32_t idx, uint32_t *ctl)
{
    uint32_t va = sh32(base) + idx * 8u;
    if (!(va >= 0x80000000u && va < 0x84000000u - 8u) &&
        !(va >= 0x00010000u && va < 0x04000000u - 8u))
        return 0;
    *ctl = *(const uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va + 4u);
    return 1;
}

static uint64_t buffer_ns(uint32_t ctl)
{
    uint64_t samples = ctl & 0xFFFFu;
    if (samples < 2)
        samples = 2;
    return (samples / 2u) * 1000000000ull / AC97_RATE_HZ;
}

int xbox_mcpx_ac97_tick(void)
{
    uint64_t now;
    int raise = 0;

    if (!g_ac97_host)
        return 0;
    now = now_ns();
    ac97_lock();
    for (int c = 0; c < 2; c++) {
        uint32_t base = g_chan[c].base, ctl, civ;
        int steps = 0;

        if (!(g_ac97[base + 0xB] & AC97_CR_RPBM) || (g_ac97[base + 6] & AC97_SR_DCH)) {
            g_chan[c].next_due_ns = 0;
            continue;
        }
        civ = g_ac97[base + 4] & 0x1Fu;
        if (!bdl_entry(base, civ, &ctl))
            continue;
        if (!g_chan[c].next_due_ns) {
            set16(base + 8, (uint16_t)ctl);
            g_ac97[base + 0xA] = (uint8_t)((civ + 1) & 0x1Fu);
            g_chan[c].next_due_ns = now + buffer_ns(ctl);
            continue;
        }
        while (now >= g_chan[c].next_due_ns && steps++ < 8) {
            uint8_t sr = g_ac97[base + 6];
            if (ctl & AC97_BDL_IOC)
                sr |= AC97_SR_BCIS;
            if (civ == (g_ac97[base + 5] & 0x1Fu)) {
                sr |= AC97_SR_LVBCI | AC97_SR_CELV;
            } else {
                civ = (civ + 1) & 0x1Fu;
                sr &= (uint8_t)~AC97_SR_CELV;
            }
            g_ac97[base + 6] = sr;
            g_ac97[base + 4] = (uint8_t)civ;
            g_ac97[base + 0xA] = (uint8_t)((civ + 1) & 0x1Fu);
            if (!bdl_entry(base, civ, &ctl))
                break;
            set16(base + 8, (uint16_t)ctl);
            g_chan[c].next_due_ns += buffer_ns(ctl);
        }
        /* A stalled host thread must not come back to a backlog of seconds. */
        if (now > g_chan[c].next_due_ns + 1000000000ull)
            g_chan[c].next_due_ns = now + buffer_ns(ctl);
        if (ac97_channel_irq(base))
            raise = 1;
    }
    ac97_unlock();
    return raise;
}

/* ── ARM64 register access ─────────────────────────────────── */

#if defined(__APPLE__)
#define UC_X(uc, n)  ((uc)->uc_mcontext->__ss.__x[n])
#define UC_FP(uc)    ((uc)->uc_mcontext->__ss.__fp)
#define UC_LR(uc)    ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc)    ((uc)->uc_mcontext->__ss.__sp)
#define UC_PC(uc)    ((uc)->uc_mcontext->__ss.__pc)
#else
#define UC_X(uc, n)  ((uc)->uc_mcontext.regs[n])
#define UC_FP(uc)    ((uc)->uc_mcontext.regs[29])
#define UC_LR(uc)    ((uc)->uc_mcontext.regs[30])
#define UC_SP(uc)    ((uc)->uc_mcontext.sp)
#define UC_PC(uc)    ((uc)->uc_mcontext.pc)
#endif

/* reg 31 is SP as a base register and XZR as a data/index register. */
static uint64_t reg_get(ucontext_t *uc, int n, int is_sp)
{
    if (n < 29) return UC_X(uc, n);
    if (n == 29) return UC_FP(uc);
    if (n == 30) return UC_LR(uc);
    return is_sp ? UC_SP(uc) : 0;
}

static void reg_set(ucontext_t *uc, int n, uint64_t v, int is_sp)
{
    if (n < 29) UC_X(uc, n) = v;
    else if (n == 29) UC_FP(uc) = v;
    else if (n == 30) UC_LR(uc) = v;
    else if (is_sp) UC_SP(uc) = v;
}

int xbox_mcpx_mmio_fault(void *ucontext, uintptr_t fault_addr)
{
    ucontext_t *uc = (ucontext_t *)ucontext;
    uint64_t pc, base, ea, wb = 0;
    uint32_t insn;
    int size, opc, rt, rn, writeback = 0;
    unsigned nbytes;

    int r;

    for (r = 0; r < g_region_count; r++)
        if (fault_addr >= g_regions[r].host_base &&
            fault_addr < g_regions[r].host_base + g_regions[r].len)
            break;
    if (r == g_region_count)
        return 0;

    pc   = UC_PC(uc);
    insn = *(const uint32_t *)(uintptr_t)pc;
    size = (int)(insn >> 30);
    opc  = (int)((insn >> 22) & 3);
    rt   = (int)(insn & 31);
    rn   = (int)((insn >> 5) & 31);
    base = reg_get(uc, rn, 1);
    nbytes = 1u << size;

    if ((insn & 0x3F000000u) == 0x39000000u) {            /* unsigned offset */
        ea = base + ((uint64_t)((insn >> 10) & 0xFFF) << size);
    } else if ((insn & 0x3F200000u) == 0x38000000u) {     /* imm9 forms */
        int64_t imm9 = (int64_t)((insn >> 12) & 0x1FF);
        if (imm9 & 0x100) imm9 -= 0x200;
        switch ((insn >> 10) & 3) {
        case 0: ea = base + (uint64_t)imm9; break;                          /* unscaled */
        case 1: ea = base; writeback = 1; wb = base + (uint64_t)imm9; break; /* post */
        case 3: ea = base + (uint64_t)imm9; writeback = 1; wb = ea; break;  /* pre */
        default: goto undecoded;
        }
    } else if ((insn & 0x3F200C00u) == 0x38200800u) {     /* register offset */
        uint64_t off = reg_get(uc, (int)((insn >> 16) & 31), 0);
        switch ((insn >> 13) & 7) {
        case 2: off = (uint32_t)off; break;                 /* UXTW */
        case 3: break;                                      /* LSL/UXTX */
        case 6: off = (uint64_t)(int64_t)(int32_t)off; break; /* SXTW */
        case 7: break;                                      /* SXTX */
        default: goto undecoded;
        }
        if ((insn >> 12) & 1) off <<= size;
        ea = base + off;
    } else {
        goto undecoded;
    }

    if (fault_addr < ea || fault_addr >= ea + nbytes
            || ea + nbytes > g_regions[r].host_base + g_regions[r].len) {
        fprintf(stderr, "[MCPX] decoded ea %p does not cover fault %p (insn %08X)\n",
                (void *)(uintptr_t)ea, (void *)fault_addr, insn);
        return 0;
    }

    {
        uint32_t off = (uint32_t)(ea - g_regions[r].host_base);
        int is_write = (opc == 0);
        uint64_t v = 0;

        if (is_write) {
            v = reg_get(uc, rt, 0);
            if (nbytes < 8)
                v &= (1ull << (8 * nbytes)) - 1;
            g_regions[r].write(g_regions[r].ctx, off, v, nbytes);
        } else {
            v = g_regions[r].read(g_regions[r].ctx, off, nbytes);
            if (opc == 2 && size < 3) {             /* sign-extend to 64 */
                unsigned bits = 8 * nbytes;
                v = (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
            } else if (opc == 3 && size < 2) {      /* sign-extend to 32 */
                unsigned bits = 8 * nbytes;
                v = (uint32_t)((int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits));
            } else if (opc != 1) {
                goto undecoded;
            }
            reg_set(uc, rt, v, 0);
        }
        if (writeback)
            reg_set(uc, rn, wb, 1);

        if (g_trace < 0) {
            const char *t = getenv("RECOMP_MCPX_TRACE");
            g_trace = t ? (atoi(t) > 1 ? atoi(t) : 400) : 0;
        }
        if (g_trace && g_trace_n++ < (unsigned)g_trace)
            fprintf(stderr, "  [%s] %s 0x%08X/%u = %0*llX\n", g_regions[r].name,
                    is_write ? "write" : "read ", g_regions[r].guest_va + off, nbytes,
                    (int)(nbytes * 2), (unsigned long long)v);
    }

    UC_PC(uc) = pc + 4;
    return 1;

undecoded:
    fprintf(stderr, "[MCPX] undecoded register access at pc=%p insn=%08X fault=%p\n",
            (void *)(uintptr_t)pc, insn, (void *)fault_addr);
    return 0;
}

#else

int xbox_mcpx_mmio_arm_ac97(void *ac97_host_base) { (void)ac97_host_base; return 0; }
int xbox_mcpx_ac97_tick(void) { return 0; }
int xbox_mmio_read(uint32_t guest_va, unsigned size, uint64_t *value)
{
    (void)guest_va; (void)size; (void)value;
    return 0;
}
int xbox_mmio_intercept(uint32_t guest_va, uint32_t size,
                        xbox_mmio_read_fn read, xbox_mmio_write_fn write,
                        void *ctx, const char *name)
{
    (void)guest_va; (void)size; (void)read; (void)write; (void)ctx; (void)name;
    return 0;
}
int xbox_mcpx_mmio_fault(void *ucontext, uintptr_t fault_addr)
{
    (void)ucontext; (void)fault_addr;
    return 0;
}

#endif
