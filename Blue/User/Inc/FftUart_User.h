#ifndef _FFT_UART_USER_H_
#define _FFT_UART_USER_H_

#include "stm32h7xx_hal.h"

#define FFT_UART_MAX_PAYLOAD_LEN        37U
#define FFT_UART_MEASURE_PAYLOAD_LEN    24U
#define FFT_UART_RESULT_PAYLOAD_LEN     37U
#define FFT_UART_FRAME_MAX_LEN          (13U + FFT_UART_MAX_PAYLOAD_LEN)
#define FFT_UART_RX_DEBUG_LEN           64U

#define FFT_UART_RX_CONSUMED            0x01U
#define FFT_UART_RX_RESULT_READY        0x02U

#define FFT_UART_TARGET_AD9226          0xA2U
#define FFT_UART_CMD_MEASURE            0x11U
#define FFT_UART_CMD_RESULT             0x91U

typedef struct {
  uint16_t sweep_id;
  uint16_t point_id;
  uint64_t frequency_mHz;
  uint32_t dds_ftw;
  uint16_t target_bin;
  uint32_t settle_us;
  uint8_t average_count;
  uint8_t measure_flags;
} FftUartMeasureRequest;

typedef struct {
  uint16_t sweep_id;
  uint16_t point_id;
  uint64_t frequency_mHz;
  uint32_t dds_ftw;
  uint32_t sample_rate_hz;
  uint16_t fft_length;
  uint16_t bin_index;
  int32_t real;
  int32_t imag;
  uint8_t block_exponent;
  uint16_t otr_count;
  uint16_t status_word;
  uint8_t seq;
} FftUartResult;

typedef struct {
  uint8_t last_cmd;
  uint8_t last_seq;
  uint8_t last_flags;
  uint8_t has_result;
  uint32_t rx_count;
  uint32_t error_count;
  uint32_t crc_error_count;
  uint32_t len_error_count;
  uint32_t tail_error_count;
  uint8_t rx_debug_buf[FFT_UART_RX_DEBUG_LEN];
  uint8_t rx_debug_pos;
  uint32_t rx_debug_count;
  uint8_t last_byte;
  uint8_t rx_state_snapshot;
  uint16_t rx_pos_snapshot;
  uint16_t rx_expected_len_snapshot;
  uint8_t len_l_snapshot;
  uint8_t len_h_snapshot;
  FftUartResult last_result;
} FftUartState;

void FftUart_Init(void);
uint16_t FftUart_Crc16CcittFalse(const uint8_t *data, uint16_t len);
uint16_t FftUart_BuildMeasureFrame(uint8_t *frame, uint16_t frame_capacity,
                                   const FftUartMeasureRequest *request,
                                   uint8_t seq);
uint8_t FftUart_PushRxByte(uint8_t byte);
uint8_t FftUart_TakeResult(FftUartResult *result);
FftUartState FftUart_GetState(void);

#endif
