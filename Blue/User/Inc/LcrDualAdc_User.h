#ifndef _LCR_DUAL_ADC_USER_H_
#define _LCR_DUAL_ADC_USER_H_

#include "stm32h7xx_hal.h"

#define LCR_DUAL_ADC_SAMPLE_COUNT 4096U

typedef enum {
  LCR_DUAL_ADC_IDLE = 0,
  LCR_DUAL_ADC_CAPTURING,
  LCR_DUAL_ADC_FRAME_READY,
  LCR_DUAL_ADC_PROCESSING,
  LCR_DUAL_ADC_RESULT_READY,
  LCR_DUAL_ADC_ERROR
} LcrDualAdcState;

typedef enum {
  LCR_DUAL_ADC_ERROR_NONE = 0,
  LCR_DUAL_ADC_ERROR_TIMER_CONFIG,
  LCR_DUAL_ADC_ERROR_DMA_START,
  LCR_DUAL_ADC_ERROR_TIMER_START,
  LCR_DUAL_ADC_ERROR_ADC_IRQ,
  LCR_DUAL_ADC_ERROR_CAPTURE_TIMEOUT,
  LCR_DUAL_ADC_ERROR_FIT
} LcrDualAdcErrorSource;

typedef struct {
  LcrDualAdcState state;
  uint8_t hardware_ready;
  uint32_t frequency_hz;
  uint32_t sample_rate_hz;
  uint32_t adc_clock_hz;
  uint16_t vin_min_code;
  uint16_t vin_max_code;
  uint16_t vr_min_code;
  uint16_t vr_max_code;
  uint32_t vin_peak_uv;
  uint32_t vr_peak_uv;
  uint16_t last_status;
  uint8_t raw_valid;
  uint8_t frame_complete;
  uint8_t irq_error_seen;
  LcrDualAdcErrorSource error_source;
  uint32_t dma_remaining;
  uint32_t capture_count;
  uint32_t error_count;
  uint32_t last_hal_error;
  uint32_t last_dma_error;
} LcrDualAdcSnapshot;

/*
 * Blue LCR analog inputs on the ATK-M100Z-M7 header:
 *   P1 pin 9 / PC0 / ADC1_INP10 = Vin
 *   P1 pin 8 / PC1 / ADC2_INP11 = Vr
 * Both inputs must remain within 0..3.3 V. A DC bias is permitted because
 * the fitting routine estimates and removes each channel's DC component.
 */
void LcrDualAdc_Init(void);
LcrDualAdcSnapshot LcrDualAdc_GetSnapshot(void);

#endif
