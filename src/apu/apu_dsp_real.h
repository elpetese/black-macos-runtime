#pragma once
#include "apu_state.h"
int mcpx_apu_real_dsp_enabled(void);
void mcpx_apu_real_dsp_init(MCPXAPUState *d);
void mcpx_apu_real_dsp_frame(MCPXAPUState *d,float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
uint64_t mcpx_apu_real_dsp_read(MCPXAPUState *d,uint64_t addr,unsigned size);
void mcpx_apu_real_dsp_write(MCPXAPUState *d,uint64_t addr,uint64_t val,unsigned size);
