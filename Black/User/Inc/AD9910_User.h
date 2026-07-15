#include "AD9910.h"

#ifndef _AD9910_USER_H
#define _AD9910_USER_H

#define DDS_SINE 0
#define DDS_RAM  1

void dds_output_sine(uint32_t hz, float factor, uint32_t mvpp);
void dds_output_sine_phase(uint32_t hz, float factor, uint32_t mvpp,
                           uint16_t phase_deg);


#endif
