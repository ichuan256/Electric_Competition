#ifndef _FPGA_UART_USER_H_
#define _FPGA_UART_USER_H_

#include "stm32h7xx_hal.h"

#define FPGA_UART_SUM_MAX_WAVES 4U

#define FPGA_UART_CONTROL_REALTIME 0x00U
#define FPGA_UART_CONTROL_CACHE    0x80U

#define FPGA_UART_CMD_CHANNEL_STAGE 0x20U
#define FPGA_UART_CMD_COMMIT        0x21U
#define FPGA_UART_CMD_GET_STATUS    0x22U
#define FPGA_UART_CMD_STATUS        0x23U
#define FPGA_UART_CMD_ACK           0x7FU

typedef struct {
  uint32_t frequency_hz;
  uint16_t phase_deg;
  uint16_t amplitude_code;
  int16_t offset_code;
  uint16_t duty_code;
  uint8_t waveform;
  uint8_t enable;
} FpgaUartWaveConfig;

typedef struct {
  uint8_t last_cmd;
  uint8_t last_rx_cmd;
  uint32_t last_data;
  uint8_t last_ack_cmd;
  uint8_t last_ack_status;
  uint16_t last_ack_seq;
  uint16_t last_transaction_id;
  uint16_t applied_mask;
  uint8_t rx_value;
  uint8_t has_rx;
  uint32_t tx_count;
  uint32_t rx_count;
  uint32_t error_count;
  uint32_t crc_error_count;
  uint32_t parse_error_count;
  uint8_t dirty_mask;
  uint8_t queue_count;
  uint8_t queue_index;
  uint8_t waiting_ack;
  uint8_t retry_count;
  uint8_t rx_dma_active;
  uint16_t rx_dma_write_pos;
  uint16_t rx_dma_read_pos;
  uint32_t rx_dma_count;
  HAL_StatusTypeDef last_tx_status;
  HAL_StatusTypeDef last_rx_status;
} FpgaUartState;

void FpgaUart_Init(void);
void FpgaUart_Task(void);

/* Queue one FPGA_CHANNEL_STAGE followed by one FPGA_COMMIT. */
void FpgaUart_SetMultiwaveTransaction(uint16_t transaction_id,
                                      uint8_t channel_id, uint8_t wave_count,
                                      const FpgaUartWaveConfig *waves,
                                      int16_t offset_code, uint8_t control_flags,
                                      uint16_t period_points, uint8_t commit_flags);

/* Compatibility entry point for local/manual UI operations. */
void FpgaUart_SetMultiwave(uint8_t target, uint8_t wave_count,
                           const FpgaUartWaveConfig *waves,
                           int16_t offset_code, uint8_t control_flags,
                           uint16_t period_points);
void FpgaUart_SetSignal(uint8_t channel_id, uint32_t frequency_hz, uint16_t phase_deg,
                        uint16_t amplitude_code, int16_t offset_code,
                        uint16_t duty_code, uint8_t waveform,
                        uint8_t output_enable);
void FpgaUart_SetSum(uint8_t channel_id, uint8_t wave_count,
                     const FpgaUartWaveConfig *waves);
FpgaUartState FpgaUart_GetState(void);

#endif
