#include "FpgaUart_User.h"

#include "usart.h"

#define FPGA_UART_HEADER          0xA5U
#define FPGA_UART_ACK_HEADER      0x5AU
#define FPGA_UART_FRAME_LEN       7U
#define FPGA_UART_ACK_LEN         4U
#define FPGA_UART_SEND_PERIOD_MS  20UL
#define FPGA_UART_TX_TIMEOUT_MS   50U
#define FPGA_UART_SAMPLE_CLK_HZ   100000000ULL

#define FPGA_UART_CMD_FREQ        0x01U
#define FPGA_UART_CMD_PHASE       0x02U
#define FPGA_UART_CMD_AMPLITUDE   0x03U
#define FPGA_UART_CMD_OFFSET      0x04U
#define FPGA_UART_CMD_DUTY        0x05U
#define FPGA_UART_CMD_WAVEFORM    0x06U
#define FPGA_UART_CMD_OUTPUT      0x07U

#define FPGA_UART_DIRTY_FREQ      0x01U
#define FPGA_UART_DIRTY_PHASE     0x02U
#define FPGA_UART_DIRTY_AMPLITUDE 0x04U
#define FPGA_UART_DIRTY_OFFSET    0x08U
#define FPGA_UART_DIRTY_DUTY      0x10U
#define FPGA_UART_DIRTY_WAVEFORM  0x20U
#define FPGA_UART_DIRTY_OUTPUT    0x40U
#define FPGA_UART_DIRTY_ALL       0x7FU

static FpgaUartState fpga_uart_state;
static uint32_t fpga_uart_last_tx_tick;
static uint32_t fpga_uart_ftw;
static uint32_t fpga_uart_phase_word;
static uint16_t fpga_uart_amplitude_code;
static int16_t fpga_uart_offset_code;
static uint16_t fpga_uart_duty_code;
static uint8_t fpga_uart_waveform;
static uint8_t fpga_uart_output_enable;
static uint8_t fpga_uart_dirty_mask;
static uint8_t fpga_uart_ack_buf[FPGA_UART_ACK_LEN];
static uint8_t fpga_uart_ack_pos;

static uint8_t FpgaUart_Checksum(const uint8_t *data, uint8_t len)
{
  uint8_t checksum = 0U;

  for (uint8_t i = 0U; i < len; i++)
  {
    checksum ^= data[i];
  }
  return checksum;
}

static uint32_t FpgaUart_FrequencyToFtw(uint32_t frequency_hz)
{
  uint64_t scaled = ((uint64_t)frequency_hz << 32) + (FPGA_UART_SAMPLE_CLK_HZ / 2ULL);
  return (uint32_t)(scaled / FPGA_UART_SAMPLE_CLK_HZ);
}

static uint32_t FpgaUart_PhaseToWord(uint16_t phase_deg)
{
  uint64_t phase = (uint64_t)(phase_deg % 360U);
  return (uint32_t)(((phase << 32) + 180ULL) / 360ULL);
}

static void FpgaUart_WriteU32(uint8_t *buf, uint32_t value)
{
  buf[2] = (uint8_t)(value & 0xFFUL);
  buf[3] = (uint8_t)((value >> 8) & 0xFFUL);
  buf[4] = (uint8_t)((value >> 16) & 0xFFUL);
  buf[5] = (uint8_t)((value >> 24) & 0xFFUL);
}

static void FpgaUart_ClearUartErrors(void)
{
  uint32_t isr = huart2.Instance->ISR;

  if ((isr & USART_ISR_ORE) != 0UL)
  {
    __HAL_UART_CLEAR_OREFLAG(&huart2);
  }
  if ((isr & USART_ISR_FE) != 0UL)
  {
    __HAL_UART_CLEAR_FEFLAG(&huart2);
  }
  if ((isr & USART_ISR_NE) != 0UL)
  {
    __HAL_UART_CLEAR_NEFLAG(&huart2);
  }
}

static uint8_t FpgaUart_NextDirtyCommand(uint32_t *data, uint8_t *dirty_bit)
{
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_FREQ) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_FREQ;
    *data = fpga_uart_ftw;
    return FPGA_UART_CMD_FREQ;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_PHASE) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_PHASE;
    *data = fpga_uart_phase_word;
    return FPGA_UART_CMD_PHASE;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_AMPLITUDE) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_AMPLITUDE;
    *data = fpga_uart_amplitude_code;
    return FPGA_UART_CMD_AMPLITUDE;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_OFFSET) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_OFFSET;
    *data = (uint16_t)fpga_uart_offset_code;
    return FPGA_UART_CMD_OFFSET;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_DUTY) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_DUTY;
    *data = fpga_uart_duty_code;
    return FPGA_UART_CMD_DUTY;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_WAVEFORM) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_WAVEFORM;
    *data = fpga_uart_waveform;
    return FPGA_UART_CMD_WAVEFORM;
  }
  if ((fpga_uart_dirty_mask & FPGA_UART_DIRTY_OUTPUT) != 0U)
  {
    *dirty_bit = FPGA_UART_DIRTY_OUTPUT;
    *data = fpga_uart_output_enable;
    return FPGA_UART_CMD_OUTPUT;
  }

  *dirty_bit = 0U;
  *data = 0UL;
  return 0U;
}

static void FpgaUart_ReadAck(void)
{
  uint8_t byte;
  uint8_t guard = 0U;

  FpgaUart_ClearUartErrors();

  while ((__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) != RESET) && (guard < 16U))
  {
    byte = (uint8_t)(huart2.Instance->RDR & 0xFFU);
    fpga_uart_state.last_rx_status = HAL_OK;
    guard++;
    fpga_uart_ack_buf[fpga_uart_ack_pos++] = byte;

    if ((fpga_uart_ack_pos == 1U) && (byte != FPGA_UART_ACK_HEADER))
    {
      fpga_uart_ack_pos = 0U;
      fpga_uart_state.error_count++;
      return;
    }

    if (fpga_uart_ack_pos >= FPGA_UART_ACK_LEN)
    {
      fpga_uart_state.rx_value = fpga_uart_ack_buf[1];
      fpga_uart_state.has_rx = 1U;
      fpga_uart_state.rx_count++;

      if (fpga_uart_ack_buf[3] == FpgaUart_Checksum(fpga_uart_ack_buf, 3U))
      {
        fpga_uart_state.last_ack_cmd = fpga_uart_ack_buf[1];
        fpga_uart_state.last_ack_status = fpga_uart_ack_buf[2];
        if (fpga_uart_ack_buf[2] != 0U)
        {
          fpga_uart_state.error_count++;
        }
      }
      else
      {
        fpga_uart_state.error_count++;
      }

      fpga_uart_ack_pos = 0U;
    }
  }
}

void FpgaUart_Init(void)
{
  fpga_uart_ftw = FpgaUart_FrequencyToFtw(1000000UL);
  fpga_uart_phase_word = 0UL;
  fpga_uart_amplitude_code = 8191U;
  fpga_uart_offset_code = 0;
  fpga_uart_duty_code = 32768U;
  fpga_uart_waveform = 0U;
  fpga_uart_output_enable = 1U;
  fpga_uart_dirty_mask = FPGA_UART_DIRTY_ALL;

  fpga_uart_state.last_cmd = 0U;
  fpga_uart_state.last_data = 0UL;
  fpga_uart_state.last_ack_cmd = 0U;
  fpga_uart_state.last_ack_status = 0xFFU;
  fpga_uart_state.rx_value = 0U;
  fpga_uart_state.has_rx = 0U;
  fpga_uart_state.tx_count = 0UL;
  fpga_uart_state.rx_count = 0UL;
  fpga_uart_state.error_count = 0UL;
  fpga_uart_state.dirty_mask = fpga_uart_dirty_mask;
  fpga_uart_state.last_tx_status = HAL_OK;
  fpga_uart_state.last_rx_status = HAL_OK;
  fpga_uart_last_tx_tick = HAL_GetTick();
  fpga_uart_ack_pos = 0U;
}

void FpgaUart_Task(void)
{
  uint32_t now = HAL_GetTick();
  HAL_StatusTypeDef status;

  FpgaUart_ReadAck();

  if (((now - fpga_uart_last_tx_tick) >= FPGA_UART_SEND_PERIOD_MS) &&
      (fpga_uart_dirty_mask != 0U))
  {
    uint8_t frame[FPGA_UART_FRAME_LEN];
    uint32_t data;
    uint8_t dirty_bit;
    uint8_t cmd = FpgaUart_NextDirtyCommand(&data, &dirty_bit);

    fpga_uart_last_tx_tick = now;
    frame[0] = FPGA_UART_HEADER;
    frame[1] = cmd;
    FpgaUart_WriteU32(frame, data);
    frame[6] = FpgaUart_Checksum(frame, 6U);

    FpgaUart_ClearUartErrors();
    status = HAL_UART_Transmit(&huart2, frame, FPGA_UART_FRAME_LEN, FPGA_UART_TX_TIMEOUT_MS);
    fpga_uart_state.last_tx_status = status;
    if (status == HAL_OK)
    {
      fpga_uart_dirty_mask &= (uint8_t)~dirty_bit;
      fpga_uart_state.tx_count++;
      fpga_uart_state.last_cmd = cmd;
      fpga_uart_state.last_data = data;
    }
    else
    {
      fpga_uart_state.error_count++;
      (void)HAL_UART_Abort(&huart2);
      FpgaUart_ClearUartErrors();
    }
  }

  fpga_uart_state.dirty_mask = fpga_uart_dirty_mask;
}

void FpgaUart_SetSignal(uint32_t frequency_hz, uint16_t phase_deg,
                        uint16_t amplitude_code, int16_t offset_code,
                        uint16_t duty_code, uint8_t waveform,
                        uint8_t output_enable)
{
  uint32_t ftw = FpgaUart_FrequencyToFtw(frequency_hz);
  uint32_t phase_word = FpgaUart_PhaseToWord(phase_deg);
  uint8_t dirty = 0U;

  if (amplitude_code > 8191U)
  {
    amplitude_code = 8191U;
  }
  if (offset_code > 8191)
  {
    offset_code = 8191;
  }
  else if (offset_code < -8192)
  {
    offset_code = -8192;
  }
  if (waveform > 3U)
  {
    waveform = 0U;
  }

  if (fpga_uart_ftw != ftw)
  {
    fpga_uart_ftw = ftw;
    dirty |= FPGA_UART_DIRTY_FREQ;
  }
  if (fpga_uart_phase_word != phase_word)
  {
    fpga_uart_phase_word = phase_word;
    dirty |= FPGA_UART_DIRTY_PHASE;
  }
  if (fpga_uart_amplitude_code != amplitude_code)
  {
    fpga_uart_amplitude_code = amplitude_code;
    dirty |= FPGA_UART_DIRTY_AMPLITUDE;
  }
  if (fpga_uart_offset_code != offset_code)
  {
    fpga_uart_offset_code = offset_code;
    dirty |= FPGA_UART_DIRTY_OFFSET;
  }
  if (fpga_uart_duty_code != duty_code)
  {
    fpga_uart_duty_code = duty_code;
    dirty |= FPGA_UART_DIRTY_DUTY;
  }
  if (fpga_uart_waveform != waveform)
  {
    fpga_uart_waveform = waveform;
    dirty |= FPGA_UART_DIRTY_WAVEFORM;
  }
  if (fpga_uart_output_enable != (uint8_t)(output_enable != 0U))
  {
    fpga_uart_output_enable = (uint8_t)(output_enable != 0U);
    dirty |= FPGA_UART_DIRTY_OUTPUT;
  }

  fpga_uart_dirty_mask |= dirty;
  fpga_uart_state.dirty_mask = fpga_uart_dirty_mask;
}

FpgaUartState FpgaUart_GetState(void)
{
  return fpga_uart_state;
}
