#ifndef _ADC_FFT_MEASURE_USER_H_
#define _ADC_FFT_MEASURE_USER_H_

#include "stm32h7xx_hal.h"

#define ADC_FFT_SAMPLE_COUNT          4096U
#define ADC_FFT_DEFAULT_SAMPLE_RATE   2500000UL
#define ADC_FFT_TARGET_BIN_AUTO       0xFFFFU

#define ADC_FFT_STATUS_VALID                    0x0001U
#define ADC_FFT_STATUS_REFERENCE_OUT_OF_RANGE   0x0002U
#define ADC_FFT_STATUS_ADC_OVERRANGE            0x0004U
#define ADC_FFT_STATUS_BUSY                     0x0008U
#define ADC_FFT_STATUS_FRAME_TIMEOUT            0x0010U
#define ADC_FFT_STATUS_PEAK_NOT_NEAR_REFERENCE  0x0020U
#define ADC_FFT_STATUS_AMPLITUDE_SATURATED      0x0040U
#define ADC_FFT_STATUS_LOW_SNR                  0x0080U
#define ADC_FFT_STATUS_USED_HANN_WINDOW         0x0100U
#define ADC_FFT_STATUS_USED_RECT_WINDOW         0x0200U
#define ADC_FFT_STATUS_TIMER_TRIGGERED_DMA_CAPTURE 0x0400U
#define ADC_FFT_STATUS_ADC_DMA_ERROR            0x0800U
#define ADC_FFT_STATUS_USED_LEAST_SQUARES        0x1000U

typedef enum {
  ADC_FFT_WINDOW_AUTO = 0,
  ADC_FFT_WINDOW_RECTANGULAR = 1,
  ADC_FFT_WINDOW_HANN = 2
} AdcFftWindowMode;

typedef enum {
  ADC_FFT_IDLE = 0,
  ADC_FFT_ACCEPTED,
  ADC_FFT_PRE_DELAY,
  ADC_FFT_ARM_DMA,
  ADC_FFT_CAPTURING,
  ADC_FFT_FRAME_READY,
  ADC_FFT_PROCESSING,
  ADC_FFT_RESULT_READY,
  ADC_FFT_ERROR
} AdcFftMeasureState;

typedef struct {
  uint16_t sweep_id;
  uint16_t point_id;
  uint32_t reference_frequency_hz;
  uint32_t dds_ftw;
  uint32_t sample_rate_hz;
  uint16_t fft_length;
  uint16_t target_bin;
  uint32_t settle_us;
  uint16_t pre_capture_delay_us;
  uint8_t window_mode;
  uint8_t flags;
} AdcFftMeasurementRequest;

typedef struct {
  uint16_t sweep_id;
  uint16_t point_id;
  uint32_t reference_frequency_hz;
  uint32_t main_frequency_hz;
  uint32_t voltage_uv_rms;
  uint32_t voltage_uv_peak;
  int32_t voltage_uv_min;
  int32_t voltage_uv_max;
  uint32_t voltage_uv_pp;
  uint32_t sample_rate_hz;
  uint16_t target_bin;
  uint16_t main_bin;
  uint16_t status;
  uint16_t adc_min_code;
  uint16_t adc_max_code;
} AdcFftMeasurementResult;

typedef struct {
  AdcFftMeasureState state;
  uint32_t request_count;
  uint32_t result_count;
  uint32_t error_count;
  uint32_t last_capture_ticks;
  uint32_t last_process_ticks;
  uint32_t silicon_revision;
  uint32_t adc_clock_hz;
  uint32_t timer_clock_hz;
  uint32_t timer_prescaler;
  uint32_t timer_period;
  uint32_t actual_sample_rate_hz;
  int32_t fit_sin_uv;
  int32_t fit_cos_uv;
  int32_t fit_offset_uv;
  uint16_t last_error;
  AdcFftMeasurementRequest active_request;
  AdcFftMeasurementResult last_result;
} AdcFftMeasureSnapshot;

extern AdcFftMeasureSnapshot adc_fft_measure_state;
extern uint16_t adc_fft_raw[ADC_FFT_SAMPLE_COUNT];

void AdcFftMeasure_Init(void);
uint8_t AdcFftMeasure_IsSupportedSampleRate(uint32_t sample_rate_hz);
uint16_t AdcFftMeasure_CalculateTargetBin(uint32_t frequency_hz, uint32_t sample_rate_hz);
uint8_t AdcFftMeasure_Start(const AdcFftMeasurementRequest *request);
void AdcFftMeasure_Task(void);
uint8_t AdcFftMeasure_TakeResult(AdcFftMeasurementResult *result);
AdcFftMeasureState AdcFftMeasure_GetState(void);
AdcFftMeasureSnapshot AdcFftMeasure_GetSnapshot(void);

#endif
