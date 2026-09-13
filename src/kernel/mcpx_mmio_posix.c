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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
#define AC97_SR_DCH             0x01u
#define AC97_SR_W1C             0x1Cu   /* LVBCI | BCIS | FIFOE */

static uint8_t   g_ac97[AC97_SHADOW];
static uintptr_t g_ac97_host = 0;
static size_t    g_ac97_len  = 0;
static int       g_trace     = -1;
static unsigned  g_trace_n   = 0;

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
            return;
        }
        if (reg == 0xB) {
            /* Run/Pause Bus Master: the controller leaves the halted state as
             * soon as it is told to run, and halts when told to pause. With
             * DCH stuck at 1 DirectSound saw every started channel as dead and
             * reset it again, hundreds of times. */
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
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void ac97_write(uint32_t off, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        ac97_write_byte(off + i, (uint8_t)(v >> (8 * i)));
}

int xbox_mcpx_mmio_arm_ac97(void *ac97_host_base)
{
    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0 || (size_t)ps > AC97_SHADOW)
        return 0;
    g_ac97_host = (uintptr_t)ac97_host_base;
    g_ac97_len  = (size_t)ps;
    memcpy(g_ac97, ac97_host_base, g_ac97_len);
    g_ac97[AC97_GLOB_STA] |= (uint8_t)AC97_GLOB_STA_CODEC_RDY;
    g_ac97[AC97_GLOB_STA + 1] |= (uint8_t)(AC97_GLOB_STA_CODEC_RDY >> 8);
    if (mprotect(ac97_host_base, g_ac97_len, PROT_NONE) != 0) {
        perror("[MCPX] mprotect AC'97");
        g_ac97_host = 0;
        return 0;
    }
    fprintf(stderr, "  AC97: 0x%08X..0x%08X intercepted (%zu-byte host page)\n",
            AC97_VA, AC97_VA + (uint32_t)g_ac97_len, g_ac97_len);
    return 1;
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

    if (!g_ac97_host || fault_addr < g_ac97_host ||
        fault_addr >= g_ac97_host + g_ac97_len)
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

    if (fault_addr < ea || fault_addr >= ea + nbytes) {
        fprintf(stderr, "[MCPX] decoded ea %p does not cover fault %p (insn %08X)\n",
                (void *)(uintptr_t)ea, (void *)fault_addr, insn);
        return 0;
    }

    {
        uint32_t off = (uint32_t)(ea - g_ac97_host);
        int is_write = (opc == 0);
        uint64_t v = 0;

        if (is_write) {
            v = reg_get(uc, rt, 0);
            if (nbytes < 8)
                v &= (1ull << (8 * nbytes)) - 1;
            ac97_write(off, v, nbytes);
        } else {
            v = ac97_read(off, nbytes);
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
            fprintf(stderr, "  [AC97] %s 0x%08X/%u = %0*llX\n",
                    is_write ? "write" : "read ", AC97_VA + off, nbytes,
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
int xbox_mcpx_mmio_fault(void *ucontext, uintptr_t fault_addr)
{
    (void)ucontext; (void)fault_addr;
    return 0;
}

#endif
