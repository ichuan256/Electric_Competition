#include "FpgaUart_User.h"

#include "usart.h"

#define FPGA_UART_FRAME_HEAD0       0xA5U
#define FPGA_UART_FRAME_HEAD1       0x5AU
#define FPGA_UART_FRAME_TAIL0       0x5AU
#define FPGA_UART_FRAME_TAIL1       0xA5U
#define FPGA_UART_ACK_HEADER        0x5AU
#define FPGA_UART_ACK_CMD           0xC0U
#define FPGA_UART_ACK_OK            0x00U
#define FPGA_UART_ACK_XOR_OK        0x9AU
#define FPGA_UART_ACK_LEN           4U
#define FPGA_UART_SEND_PERIOD_MS    20UL
#define FPGA_UART_ACK_TIMEOUT_MS    100UL
#define FPGA_UART_MAX_RETRY         2U
#define FPGA_UART_TX_TIMEOUT_MS     50U
#define FPGA_UART_SAMPLE_CLK_HZ     100000000ULL
#define FPGA_UART_ENTRY_LEN         13U
#define FPGA_UART_FRAME_MAX_LEN     (2U + 2U + (FPGA_UART_SUM_MAX_WAVES * FPGA_UART_ENTRY_LEN) + 2U + 1U + 2U)

static FpgaUartState fpga_uart_state;
static uint32_t fpga_uart_last_tx_tick;
static uint32_t fpga_uart_last_ack_wait_tick;
static uint8_t fpga_uart_ack_buf[FPGA_UART_ACK_LEN];
static uint8_t fpga_uart_ack_pos;
static uint8_t fpga_uart_frame[FPGA_UART_FRAME_MAX_LEN];
static uint8_t fpga_uart_frame_len;
static uint8_t fpga_uart_frame_sent;
static uint8_t fpga_uart_waiting_ack;
static uint8_t fpga_uart_retry_count;

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

static uint8_t FpgaUart_WaveformType(const FpgaUartWaveConfig *wave)
{
  if ((wave == 0) || (wave->enable == 0U))
  {
    return 0U;
  }
  if (wave->waveform > 3U)
  {
    return 0U;
  }

  return wave->waveform;
}

static void FpgaUart_PushU16(uint8_t *pos, uint16_t value)
{
  fpga_uart_frame[(*pos)++] = (uint8_t)(value & 0xFFU);
  fpga_uart_frame[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void FpgaUart_PushU32(uint8_t *pos, uint32_t value)
{
  fpga_uart_frame[(*pos)++] = (uint8_t)(value & 0xFFUL);
  fpga_uart_frame[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  fpga_uart_frame[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  fpga_uart_frame[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
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

static void FpgaUart_ClearFrame(void)
{
  fpga_uart_frame_len = 0U;
  fpga_uart_frame_sent = 0U;
  fpga_uart_waiting_ack = 0U;
  fpga_uart_retry_count = 0U;
  fpga_uart_state.dirty_mask = 0U;
  fpga_uart_state.queue_count = 0U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.waiting_ack = 0U;
  fpga_uart_state.retry_count = 0U;
}

static void FpgaUart_LoadFrame(const uint8_t *frame, uint8_t frame_len)
{
  if ((frame == 0) || (frame_len == 0U) || (frame_len > FPGA_UART_FRAME_MAX_LEN))
  {
    return;
  }

  FpgaUart_ClearFrame();
  for (uint8_t i = 0U; i < frame_len; i++)
  {
    fpga_uart_frame[i] = frame[i];
  }
  fpga_uart_frame_len = frame_len;
  fpga_uart_frame_sent = 0U;
  fpga_uart_state.dirty_mask = 1U;
  fpga_uart_state.queue_count = 1U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.last_cmd = frame[2];
  fpga_uart_state.last_data = frame_len;
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

        if ((fpga_uart_ack_buf[1] == FPGA_UART_ACK_CMD) &&
            (fpga_uart_ack_buf[2] == FPGA_UART_ACK_OK) &&
            (fpga_uart_ack_buf[3] == FPGA_UART_ACK_XOR_OK))
        {
          fpga_uart_waiting_ack = 0U;
          fpga_uart_state.waiting_ack = 0U;
          fpga_uart_retry_count = 0U;
          fpga_uart_state.retry_count = 0U;
          fpga_uart_state.queue_index = fpga_uart_state.queue_count;
          fpga_uart_state.dirty_mask = 0U;
        }
        else
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
  fpga_uart_state.last_cmd = 0U;
  fpga_uart_state.last_data = 0UL;
  fpga_uart_state.last_ack_cmd = 0U;
  fpga_uart_state.last_ack_status = 0xFFU;
  fpga_uart_state.rx_value = 0U;
  fpga_uart_state.has_rx = 0U;
  fpga_uart_state.tx_count = 0UL;
  fpga_uart_state.rx_count = 0UL;
  fpga_uart_state.error_count = 0UL;
  fpga_uart_state.last_tx_status = HAL_OK;
  fpga_uart_state.last_rx_status = HAL_OK;
  fpga_uart_ack_pos = 0U;
  fpga_uart_last_tx_tick = HAL_GetTick();
  fpga_uart_last_ack_wait_tick = fpga_uart_last_tx_tick;
  FpgaUart_ClearFrame();
}

void FpgaUart_Task(void)
{
  uint32_t now = HAL_GetTick();
  HAL_StatusTypeDef status;

  FpgaUart_ReadAck();

  if ((fpga_uart_waiting_ack != 0U) &&
      ((now - fpga_uart_last_ack_wait_tick) >= FPGA_UART_ACK_TIMEOUT_MS))
  {
    fpga_uart_state.error_count++;
    fpga_uart_waiting_ack = 0U;
    fpga_uart_state.waiting_ack = 0U;
    if (fpga_uart_retry_count < FPGA_UART_MAX_RETRY)
    {
      fpga_uart_retry_count++;
      fpga_uart_state.retry_count = fpga_uart_retry_count;
      fpga_uart_frame_sent = 0U;
      fpga_uart_state.queue_index = 0U;
    }
    else
    {
      FpgaUart_ClearFrame();
    }
  }

  if ((fpga_uart_frame_len != 0U) &&
      (fpga_uart_frame_sent == 0U) &&
      (fpga_uart_waiting_ack == 0U) &&
      ((now - fpga_uart_last_tx_tick) >= FPGA_UART_SEND_PERIOD_MS))
  {
    FpgaUart_ClearUartErrors();
    status = HAL_UART_Transmit(&huart2, fpga_uart_frame, fpga_uart_frame_len, FPGA_UART_TX_TIMEOUT_MS);
    fpga_uart_last_tx_tick = now;
    fpga_uart_state.last_tx_status = status;
    if (status == HAL_OK)
    {
      fpga_uart_state.tx_count++;
      fpga_uart_state.last_cmd = fpga_uart_frame[2];
      fpga_uart_state.last_data = fpga_uart_frame_len;
      fpga_uart_frame_sent = 1U;
      fpga_uart_waiting_ack = 1U;
      fpga_uart_state.waiting_ack = 1U;
      fpga_uart_state.queue_index = 1U;
      fpga_uart_last_ack_wait_tick = now;
    }
    else
    {
      fpga_uart_state.error_count++;
      (void)HAL_UART_Abort(&huart2);
      FpgaUart_ClearUartErrors();
    }
  }

  if ((fpga_uart_frame_len != 0U) && (fpga_uart_state.dirty_mask != 0U))
  {
    fpga_uart_state.queue_count = 1U;
  }
}

void FpgaUart_SetMultiwave(uint8_t target, uint8_t wave_count,
                           const FpgaUartWaveConfig *waves,
                           int16_t offset_code)
{
  uint8_t pos = 0U;
  uint8_t checksum_start;
  uint16_t safe_offset;

  if (waves == 0)
  {
    return;
  }
  if (wave_count > FPGA_UART_SUM_MAX_WAVES)
  {
    wave_count = FPGA_UART_SUM_MAX_WAVES;
  }

  FpgaUart_ClearFrame();

  fpga_uart_frame[pos++] = FPGA_UART_FRAME_HEAD0;
  fpga_uart_frame[pos++] = FPGA_UART_FRAME_HEAD1;
  checksum_start = pos;
  fpga_uart_frame[pos++] = target;
  fpga_uart_frame[pos++] = wave_count;

  for (uint8_t i = 0U; i < wave_count; i++)
  {
    const FpgaUartWaveConfig *wave = &waves[i];
    fpga_uart_frame[pos++] = FpgaUart_WaveformType(wave);
    FpgaUart_PushU32(&pos, FpgaUart_FrequencyToFtw(wave->frequency_hz));
    FpgaUart_PushU32(&pos, FpgaUart_PhaseToWord(wave->phase_deg));
    FpgaUart_PushU16(&pos, wave->amplitude_code);
    FpgaUart_PushU16(&pos, wave->duty_code);
  }

  safe_offset = (uint16_t)offset_code;
  FpgaUart_PushU16(&pos, safe_offset);
  fpga_uart_frame[pos] = FpgaUart_Checksum(&fpga_uart_frame[checksum_start],
                                           (uint8_t)(pos - checksum_start));
  pos++;
  fpga_uart_frame[pos++] = FPGA_UART_FRAME_TAIL0;
  fpga_uart_frame[pos++] = FPGA_UART_FRAME_TAIL1;

  fpga_uart_frame_len = pos;
  fpga_uart_frame_sent = 0U;
  fpga_uart_state.dirty_mask = 1U;
  fpga_uart_state.queue_count = 1U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.last_cmd = target;
  fpga_uart_state.last_data = fpga_uart_frame_len;
}

void FpgaUart_SetSignal(uint8_t channel_id, uint32_t frequency_hz, uint16_t phase_deg,
                        uint16_t amplitude_code, int16_t offset_code,
                        uint16_t duty_code, uint8_t waveform,
                        uint8_t output_enable)
{
  FpgaUartWaveConfig wave;

  wave.frequency_hz = frequency_hz;
  wave.phase_deg = phase_deg;
  wave.amplitude_code = amplitude_code;
  wave.offset_code = 0;
  wave.duty_code = duty_code;
  wave.waveform = waveform;
  wave.enable = output_enable;

  FpgaUart_SetMultiwave(channel_id, 1U, &wave, offset_code);
}

void FpgaUart_SetSum(uint8_t channel_id, uint8_t wave_count,
                     const FpgaUartWaveConfig *waves)
{
  int16_t offset_code = 0;

  if ((waves != 0) && (wave_count != 0U))
  {
    offset_code = waves[0].offset_code;
  }

  FpgaUart_SetMultiwave(channel_id, wave_count, waves, offset_code);
}

void FpgaUart_SendTestFrame(void)
{
  static const uint8_t test_frame[] = {
    0xA5U, 0x5AU, 0x00U, 0x01U, 0x01U, 0x29U, 0x5CU, 0x8FU,
    0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x1FU, 0x00U,
    0x80U, 0x00U, 0x00U, 0x98U, 0x5AU, 0xA5U
  };

  FpgaUart_LoadFrame(test_frame, (uint8_t)sizeof(test_frame));
}

FpgaUartState FpgaUart_GetState(void)
{
  return fpga_uart_state;
}
