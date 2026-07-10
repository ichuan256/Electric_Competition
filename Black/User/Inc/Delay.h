#ifndef _DELAY_H
#define _DELAY_H

#include "stm32h7xx_hal.h"

void Delay_Init(void);
void Delay_ns(uint32_t ns);
void Delay_us(uint32_t us);
void Delay_ms(uint32_t ms);

#endif
