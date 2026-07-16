#ifndef _SPECTRUM_PHASE_USER_H_
#define _SPECTRUM_PHASE_USER_H_

#include <stdint.h>

#define SPECTRUM_SINE_PHASE_OFFSET_DEG 126U

static inline uint16_t SpectrumPhase_SineUiToDds(uint16_t ui_phase_deg)
{
  return (uint16_t)(((uint32_t)(ui_phase_deg % 360U) +
                     SPECTRUM_SINE_PHASE_OFFSET_DEG) % 360U);
}

#endif
