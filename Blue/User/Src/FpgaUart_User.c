#include "FpgaUart_User.h"

#include "usart.h"

#define FPGA_UART_SEND_PERIOD_MS 1000UL

static FpgaUartState fpga_uart_state;
static uint32_t fpga_uart_last_tx_tick;

void FpgaUart_Init(void)
{
  fpga_uart_state.next_tx_value = 0U;
  fpga_uart_state.last_tx_value = 0U;
  fpga_uart_state.rx_value = 0U;
  fpga_uart_state.has_rx = 0U;
  fpga_uart_state.tx_count = 0UL;
  fpga_uart_state.rx_count = 0UL;
  fpga_uart_state.error_count = 0UL;
  fpga_uart_state.last_tx_status = HAL_OK;
  fpga_uart_state.last_rx_status = HAL_OK;
  fpga_uart_last_tx_tick = HAL_GetTick();
}

void FpgaUart_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t rx_value = 0U;
  HAL_StatusTypeDef status;

  status = HAL_UART_Receive(&huart2, &rx_value, 1U, 0U);
  fpga_uart_state.last_rx_status = status;
  if (status == HAL_OK)
  {
    fpga_uart_state.rx_value = rx_value;
    fpga_uart_state.has_rx = 1U;
    fpga_uart_state.rx_count++;
  }
  else if (status == HAL_ERROR)
  {
    fpga_uart_state.error_count++;
  }

  if ((now - fpga_uart_last_tx_tick) >= FPGA_UART_SEND_PERIOD_MS)
  {
    uint8_t tx_value = fpga_uart_state.next_tx_value;

    fpga_uart_last_tx_tick = now;
    status = HAL_UART_Transmit(&huart2, &tx_value, 1U, 10U);
    fpga_uart_state.last_tx_status = status;
    if (status == HAL_OK)
    {
      fpga_uart_state.tx_count++;
      fpga_uart_state.last_tx_value = tx_value;
      fpga_uart_state.next_tx_value++;
    }
    else
    {
      fpga_uart_state.error_count++;
    }
  }
}

FpgaUartState FpgaUart_GetState(void)
{
  return fpga_uart_state;
}
