#ifndef _FPGA_UART_USER_H_
#define _FPGA_UART_USER_H_

#include "stm32h7xx_hal.h"

#define FPGA_UART_SUM_MAX_WAVES 4U

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
  uint32_t last_data;
  uint8_t last_ack_cmd;
  uint8_t last_ack_status;
  uint8_t rx_value;
  uint8_t has_rx;
  uint32_t tx_count;
  uint32_t rx_count;
  uint32_t error_count;
  uint8_t dirty_mask;
  uint8_t queue_count;
  uint8_t queue_index;
  uint8_t waiting_ack;
  uint8_t retry_count;
  HAL_StatusTypeDef last_tx_status;
  HAL_StatusTypeDef last_rx_status;
} FpgaUartState;

void FpgaUart_Init(void);
void FpgaUart_Task(void);
void FpgaUart_SetSignal(uint8_t channel_id, uint32_t frequency_hz, uint16_t phase_deg,
                        uint16_t amplitude_code, int16_t offset_code,
                        uint16_t duty_code, uint8_t waveform,
                        uint8_t output_enable);
void FpgaUart_SetSum(uint8_t channel_id, uint8_t wave_count,
                     const FpgaUartWaveConfig *waves);
FpgaUartState FpgaUart_GetState(void);

#endif
