/* GP/EP glue adapted from xemu v0.8.20 hw/xbox/mcpx/apu.c.
 * Copyright (c) 2012 espes; (c) 2018-2019 Jannik Vogel; (c) 2019-2025 Matt Borgerson.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "apu_state.h"
#include "fpconv.h"
#include "dsp/dsp.h"
#include "apu_dsp_real.h"
#define TARGET_PAGE_SIZE 4096
#define GP_OUTPUT_FIFO_COUNT 4
#define GP_INPUT_FIFO_COUNT 2
#define EP_OUTPUT_FIFO_COUNT 4
#define EP_INPUT_FIFO_COUNT 2
#define DPRINTF(...) ((void)0)
static const uint8_t ep_silence[1024];

int mcpx_apu_real_dsp_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("RECOMP_REAL_DSP"); enabled = e && atoi(e) != 0; }
    return enabled;
}
static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        if (page_entry > max_sge || ((sge_base & 0x7fffffffULL) + page_entry * 8 + 4) > 0x4000000) {
            if (!dir) memset(ptr, 0, len);
            fprintf(stderr, "[DSP] invalid scatter table %08llX page %u max %u\n", (unsigned long long)sge_base, page_entry, max_sge);
            return;
        }

        uint32_t prd_address = ldl_le_phys(address_space_memory,
                                           sge_base + page_entry * 8 + 0);
        // uint32_t prd_control = ldl_le_phys(address_space_memory,
        //                                     sge_base + page_entry * 8 + 4);

        hwaddr paddr = apu_guest_addr(prd_address) + offset_in_page;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        if ((paddr & 0x7fffffffULL) + bytes_to_copy > 0x4000000) {
            if (!dir) memset(ptr, 0, len);
            fprintf(stderr, "[DSP] invalid scatter page %08llX\n", (unsigned long long)paddr);
            return;
        }

        if (dir) {
            memcpy(&d->ram_ptr[paddr], ptr, bytes_to_copy);

        } else {
            memcpy(ptr, &d->ram_ptr[paddr], bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "GP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "EP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    if (end <= base || cur < base || cur >= end) {
        if (!dir) memset(ptr, 0, len);
        fprintf(stderr, "[DSP] invalid FIFO %08X..%08X current %08X\n", base, end, cur);
        return base;
    }
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        DPRINTF("circular scatter gather %s in range 0x%x - 0x%x at 0x%x of "
                "length 0x%x / 0x%lx bytes\n",
                dir ? "write" : "read", base, end, cur, bytes_to_copy, len);

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "GP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_GP) return true;
    if (len != sizeof(d->monitor.frame_buf)) {
        fprintf(stderr, "[DSP] unexpected EP PCM transfer: %zu bytes\n", len);
        return false;
    }
    static unsigned transfers;
    if (transfers++ < 4) fprintf(stderr,"[DSP] EP delivered %zu bytes PCM\n",len);
    memcpy(d->monitor.frame_buf, ptr, len);
    for (int i=0;i<256;i++) for(int ch=0;ch<2;ch++)
        d->monitor.float_frame_buf[i][ch] = d->monitor.frame_buf[i][ch] / 32768.0f;
    d->monitor.dsp_wrote_frame = true;
    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "EP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink) {
            /* Since we are sinking, push silence out */
            assert(len <= sizeof(ep_silence));
            ptr = (uint8_t*)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (end > base && cur >= end) {
        cur = base + (cur - base) % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP */
static uint64_t gp_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPXMEM [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPMIXBUF [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPYMEM [%x] -> %x\n", yaddr, r);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        // fprintf(stderr, "read GP NV_PAPU_GPPMEM [%x] -> %x\n", paddr, r);
        break;
    }
    default:
        r = d->gp.regs[addr];
        break;
    }
    DPRINTF("mcpx apu GP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

static void gp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    qemu_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu GP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        // fprintf(stderr, "gp write xmem %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        // fprintf(stderr, "gp write xmixbuf %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr, val);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        // fprintf(stderr, "gp write ymem %x = %x\n", yaddr, val);
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        // fprintf(stderr, "gp write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->gp.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_GPRST:
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
        break;
    default:
        d->gp.regs[addr] = val;
        break;
    }

    qemu_mutex_unlock(&d->lock);
}

static uint64_t ep_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPXMEM [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPYMEM [%x] -> %x\n", yaddr, r);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPPMEM [%x] -> %x\n", paddr, r);
        break;
    }
    default:
        r = d->ep.regs[addr];
        break;
    }
    DPRINTF("mcpx apu EP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

static void ep_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    qemu_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu EP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, val);
        // fprintf(stderr, "ep write xmem %x = %x\n", xaddr, val);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, val);
        // fprintf(stderr, "ep write ymem %x = %x\n", yaddr, val);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        // fprintf(stderr, "ep write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->ep.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_EPRST:
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        d->ep_frame_div = 0; /* FIXME: Still unsure about frame sync */
        break;
    default:
        d->ep.regs[addr] = val;
        break;
    }

    qemu_mutex_unlock(&d->lock);
}


void mcpx_apu_real_dsp_init(MCPXAPUState *d) {
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    d->gp.dsp->is_gp = d->gp.dsp->core.is_gp = true;
    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    d->ep.dsp->is_gp = d->ep.dsp->core.is_gp = false;
    d->gp.realtime = true; d->ep.realtime = false;
    d->monitor.point = MCPX_APU_DEBUG_MON_GP;
    fprintf(stderr, "[APU] DSP56300 GP/EP ARM64 JIT enabled\n");
}
uint64_t mcpx_apu_real_dsp_read(MCPXAPUState *d, uint64_t addr, unsigned size) {
    if (size != 4 || (addr & 3)) {
        uint64_t out=0;
        for(unsigned i=0;i<size;i++) {
            uint64_t word=mcpx_apu_real_dsp_read(d,(addr+i)&~3ULL,4);
            out |= ((word >> (((addr+i)&3)*8)) & 0xff) << (i*8);
        }
        return out;
    }
    qemu_mutex_lock(&d->lock);
    uint64_t val = addr < 0x40000 ? gp_read(d, addr-0x30000, size) : ep_read(d, addr-0x50000, size);
    qemu_mutex_unlock(&d->lock);
    return val;
}
void mcpx_apu_real_dsp_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned size) {
    if (size != 4 || (addr & 3)) {
        for(unsigned i=0;i<size;i++) {
            uint64_t aligned=(addr+i)&~3ULL;
            uint32_t shift=((addr+i)&3)*8;
            uint32_t word=mcpx_apu_real_dsp_read(d,aligned,4);
            word=(word & ~(0xffu<<shift)) | (((val>>(i*8))&0xff)<<shift);
            mcpx_apu_real_dsp_write(d,aligned,word,4);
        }
        return;
    }
    if (addr < 0x40000) gp_write(d,addr-0x30000,val,size);
    else ep_write(d,addr-0x50000,val,size);
}
static int run_frame(DSPState *dsp, const char *name) {
    dsp_start_frame(dsp); dsp_set_halt_requested(dsp,false); dsp_set_cycle_count(dsp,0);
    do { dsp_run(dsp, 1000); } while(!dsp_get_halt_requested(dsp) && dsp_get_cycle_count(dsp) < (dsp->is_gp ? 250000u : 1000u));
    if (!dsp_get_halt_requested(dsp) && dsp->is_gp) {
        dsp_sync_to_vm(dsp);
        static unsigned warned;
        if (!warned && getenv("RECOMP_PB_FAULT_DUMP")) {
            char path[1024]; snprintf(path,sizeof path,"%s/ep-program.bin",getenv("RECOMP_PB_FAULT_DUMP"));
            FILE *f=fopen(path,"wb"); if(f) { for(int i=0;i<0x1000;i++) { uint32_t word=dsp_read_memory(dsp,'P',i); fwrite(&word,4,1,f); } fclose(f); }
        }
        if(warned++<8) fprintf(stderr,"[DSP] %s frame exceeded budget at PC %04X\n",name,dsp->core.pc);
        return 0;
    }
    return 1;
}
void mcpx_apu_real_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]) {
    for(int b=0;b<NUM_MIXBINS;b++) for(int i=0;i<NUM_SAMPLES_PER_FRAME;i++)
        dsp_write_memory(d->gp.dsp,'X',GP_DSP_MIXBUF_BASE+b*NUM_SAMPLES_PER_FRAME+i,float_to_24b(mixbins[b][i]));
    bool gp=(d->gp.regs[NV_PAPU_GPRST]&3)==3;
    bool ep=(d->ep.regs[NV_PAPU_EPRST]&3)==3;
    if(gp) {
        run_frame(d->gp.dsp,"GP");
        if (1) { /* Stereo PCM is the processed GP output, before EP encoding. */
            int off=(d->ep_frame_div%8)*NUM_SAMPLES_PER_FRAME;
            for(int i=0;i<NUM_SAMPLES_PER_FRAME;i++) for(int ch=0;ch<2;ch++) {
                uint32_t s=dsp_read_memory(d->gp.dsp,'X',0x1400+ch*0x20+i);
                d->monitor.float_frame_buf[off+i][ch]=int24_to_float(s);
            }
            d->monitor.dsp_wrote_frame=true;
        }
    }
    if(ep && d->ep_frame_div%8==0) run_frame(d->ep.dsp,"EP");
    if (getenv("RECOMP_AUDIOLOG")) {
        static unsigned count;
        if (++count % 1500 == 0) {
            float peak=0;
            for(int i=0;i<64;i++) { float f=fabsf(int24_to_float(dsp_read_memory(d->gp.dsp,'X',0x1400+i))); if(f>peak)peak=f; }
            fprintf(stderr,"[DSP] GP=%d EP=%d cycles=%u/%u GP output peak=%.3f PCM ready=%d\n",gp,ep,dsp_get_cycle_count(d->gp.dsp),dsp_get_cycle_count(d->ep.dsp),peak,d->monitor.dsp_wrote_frame);
        }
    }
}
