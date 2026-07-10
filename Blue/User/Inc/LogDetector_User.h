#ifndef _LOG_DETECTOR_USER_H_
#define _LOG_DETECTOR_USER_H_

#include "adc.h"

/*
 * Log detector ADC input for Blue.
 *
 * Wiring:
 *   Log detector Vout -> Blue PC0 / ADC1_INP10
 *   Log detector GND  -> Blue GND
 *
 * Electrical limits:
 *   PC0 ADC input must stay in 0.0 V to 3.3 V.
 *   Add an RC low-pass near PC0 if the detector output has visible ripple.
 *
 * Formula used for dBm conversion:
 *   Vout = 0.025 * (Pin_dBm - (-84))
 *   Pin_dBm = Vout / 0.025 - 84
 */

#define LOG_DETECTOR_ADC_FULL_SCALE_RAW  65535UL
#define LOG_DETECTOR_ADC_VREF_MV         3300UL
#define LOG_DETECTOR_DEFAULT_AVG_COUNT   32U

typedef struct {
  uint32_t raw;
  uint16_t mv;
  int16_t dbm_x10;
  uint8_t valid;
} LogDetectorSample;

void LogDetector_Init(void);
LogDetectorSample LogDetector_ReadAverage(uint16_t sample_count);
LogDetectorSample LogDetector_GetLastSample(void);
const char *LogDetector_StatusText(uint8_t valid);

#endif
