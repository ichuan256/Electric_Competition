#ifndef _ADC_FFT_PROTOCOL_USER_H_
#define _ADC_FFT_PROTOCOL_USER_H_

#include "AdcFftMeasure_User.h"
#include "stm32h7xx_hal.h"

#define ADC_FFT_PROTO_TARGET                 0xA3U
#define ADC_FFT_PROTO_CMD_MEASURE_REQUEST    0x21U
#define ADC_FFT_PROTO_CMD_REQUEST_ACCEPTED   0x61U
#define ADC_FFT_PROTO_CMD_BUSY               0x62U
#define ADC_FFT_PROTO_CMD_MEASURE_RESULT     0xA1U
#define ADC_FFT_PROTO_CMD_MEASURE_ERROR      0xE1U

#define ADC_FFT_PROTO_ERR_INVALID_LENGTH           1U
#define ADC_FFT_PROTO_ERR_UNSUPPORTED_SAMPLE_RATE  2U
#define ADC_FFT_PROTO_ERR_REFERENCE_OUT_OF_RANGE   3U
#define ADC_FFT_PROTO_ERR_INVALID_TARGET_BIN       4U
#define ADC_FFT_PROTO_ERR_ADC_DMA_ERROR            5U
#define ADC_FFT_PROTO_ERR_FRAME_TIMEOUT            6U
#define ADC_FFT_PROTO_ERR_FFT_ERROR                7U
#define ADC_FFT_PROTO_ERR_CRC_ERROR                8U

typedef struct {
  uint8_t last_cmd;
  uint8_t last_seq;
  uint8_t last_error_code;
  uint32_t rx_count;
  uint32_t tx_count;
  uint32_t error_count;
  uint32_t crc_error_count;
  uint32_t len_error_count;
  uint32_t busy_count;
  uint32_t pending_count;
  uint16_t last_rx_size;
  uint8_t rx_debug_buf[64];
  uint8_t rx_debug_pos;
  uint32_t rx_debug_count;
  AdcFftMeasurementRequest last_request;
  AdcFftMeasurementResult last_result;
} AdcFftProtocolState;

extern AdcFftProtocolState adc_fft_protocol_state;

void AdcFftProtocol_Init(UART_HandleTypeDef *huart);
uint8_t AdcFftProtocol_IsFrameStart(const uint8_t *data, uint16_t size);
uint8_t AdcFftProtocol_HandleRxBuffer(const uint8_t *data, uint16_t size);
void AdcFftProtocol_Task(void);
AdcFftProtocolState AdcFftProtocol_GetState(void);

#endif
