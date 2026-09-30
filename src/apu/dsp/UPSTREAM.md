DSP56300 adapter and DMA engine imported from xemu master, retrieved 2026-09-30.
Snapshot reference: 478b4f496102379c7eaa7f3ec10e714a703c4300
https://github.com/xemu-project/xemu/tree/478b4f496102379c7eaa7f3ec10e714a703c4300/hw/xbox/mcpx/apu/dsp

Execution backend: mborgerson/dsp56300 ARM64 JIT, vendored at commit
c60aeedb6a6014850204c71e53bc0cf5b3104222.
https://github.com/mborgerson/dsp56300/tree/c60aeedb6a6014850204c71e53bc0cf5b3104222

Original GPL-2.0-or-later and LGPL-2.0-or-later notices are retained per file.
Adaptations: standalone portability header instead of QEMU allocation/endian
helpers; JIT backend selected explicitly; QEMU trace calls removed.
GP/EP bootstrap, MMIO, scatter/gather and FIFO glue lives in ../apu_dsp_real.c
and follows the xemu v0.8.20 APU layout, with GP stereo monitoring.
The older C interpreter is not used: it lacks DSP instructions used by BLACK.
