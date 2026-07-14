#ifndef _ADC_FFT_CLIENT_USER_H_
#define _ADC_FFT_CLIENT_USER_H_

#include "stm32h7xx_hal.h"

#define ADC_FFT_CLIENT_TARGET                  0xA3U
#define ADC_FFT_CLIENT_CMD_MEASURE_REQUEST     0x21U
#define ADC_FFT_CLIENT_CMD_REQUEST_ACCEPTED    0x61U
#define ADC_FFT_CLIENT_CMD_BUSY                0x62U
#define ADC_FFT_CLIENT_CMD_MEASURE_RESULT      0xA1U
#define ADC_FFT_CLIENT_CMD_MEASURE_ERROR       0xE1U

#define ADC_FFT_CLIENT_FFT_LENGTH              4096U
#define ADC_FFT_CLIENT_SAMPLE_RATE_HZ          4000000UL
#define ADC_FFT_CLIENT_TARGET_BIN_AUTO         0xFFFFU
#define ADC_FFT_CLIENT_RX_DEBUG_LEN            64U

typedef enum {
  ADC_FFT_CLIENT_IDLE = 0,
  ADC_FFT_CLIENT_WAIT_DDS_SETTLE,
  ADC_FFT_CLIENT_WAIT_ACCEPTED,
  ADC_FFT_CLIENT_WAIT_RESULT,
  ADC_FFT_CLIENT_BUSY_DELAY,
  ADC_FFT_CLIENT_RESULT_READY,
  ADC_FFT_CLIENT_ERROR
} AdcFftClientPhase;

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
  uint8_t request_flags;
} AdcFftClientRequest;

typedef struct {
  uint16_t sweep_id;
  uint16_t point_id;
  uint32_t reference_frequency_hz;
  uint32_t main_frequency_hz;
  uint32_t voltage_uv_rms;
  uint32_t voltage_uv_peak;
  uint32_t sample_rate_hz;
  uint16_t fft_length;
  uint16_t target_bin;
  uint16_t main_bin;
  uint16_t status;
  uint16_t adc_min_code;
  uint16_t adc_max_code;
  uint8_t seq;
} AdcFftClientResult;

typedef struct {
  AdcFftClientPhase phase;
  uint8_t last_cmd;
  uint8_t last_seq;
  uint8_t has_result;
  uint8_t last_error_code;
  uint16_t last_error_detail;
  uint16_t retry_after_ms;
  uint32_t tx_count;
  uint32_t rx_count;
  uint32_t busy_count;
  uint32_t error_count;
  uint32_t crc_error_count;
  uint32_t len_error_count;
  uint32_t timeout_count;
  uint8_t rx_debug_buf[ADC_FFT_CLIENT_RX_DEBUG_LEN];
  uint8_t rx_debug_pos;
  uint32_t rx_debug_count;
  AdcFftClientRequest active_request;
  AdcFftClientResult last_result;
} AdcFftClientState;

extern AdcFftClientState adc_fft_client_state;

void AdcFftClient_Init(void);
uint8_t AdcFftClient_RequestMeasurement(uint32_t reference_frequency_hz,
                                        uint32_t dds_ftw,
                                        uint32_t settle_us);
void AdcFftClient_Task(void);
uint8_t AdcFftClient_IsFrameStart(const uint8_t *data, uint16_t size);
uint8_t AdcFftClient_HandleRxBuffer(const uint8_t *data, uint16_t size);
uint8_t AdcFftClient_TakeResult(AdcFftClientResult *result);
AdcFftClientState AdcFftClient_GetState(void);

#endif
