#ifndef __AD9226_H
#define __AD9226_H

#include "dma.h"

typedef enum {
	Collect_complete_not=0,
  Collect_complete=1
}CollectFlag;

void AD9226_Set_DMA_collection_flag(CollectFlag collectflag);
CollectFlag AD9226_Get_DMA_Complete_Flag();
double Read_GPIOD_12bit(void);
double adc_to_volt(double adc);

#endif