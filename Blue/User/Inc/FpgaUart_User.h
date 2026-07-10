#ifndef _FPGA_UART_USER_H_
#define _FPGA_UART_USER_H_

#include "stm32h7xx_hal.h"

typedef struct {
  uint8_t next_tx_value;
  uint8_t last_tx_value;
  uint8_t rx_value;
  uint8_t has_rx;
  uint32_t tx_count;
  uint32_t rx_count;
  uint32_t error_count;
  HAL_StatusTypeDef last_tx_status;
  HAL_StatusTypeDef last_rx_status;
} FpgaUartState;

void FpgaUart_Init(void);
void FpgaUart_Task(void);
FpgaUartState FpgaUart_GetState(void);

#endif
