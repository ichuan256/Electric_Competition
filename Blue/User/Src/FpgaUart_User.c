#include "FpgaUart_User.h"

#include "usart.h"

#define FPGA_UART_HEADER          0xA5U
#define FPGA_UART_ACK_HEADER      0x5AU
#define FPGA_UART_FRAME_LEN       7U
#define FPGA_UART_ACK_LEN         4U
#define FPGA_UART_SEND_PERIOD_MS  20UL
#define FPGA_UART_ACK_TIMEOUT_MS  100UL
#define FPGA_UART_MAX_RETRY       2U
#define FPGA_UART_TX_TIMEOUT_MS   50U
#define FPGA_UART_SAMPLE_CLK_HZ   100000000ULL
#define FPGA_UART_QUEUE_MAX       48U

#define FPGA_UART_CMD_FREQ        0x01U
#define FPGA_UART_CMD_PHASE       0x02U
#define FPGA_UART_CMD_AMPLITUDE   0x03U
#define FPGA_UART_CMD_OFFSET      0x04U
#define FPGA_UART_CMD_DUTY        0x05U
#define FPGA_UART_CMD_WAVEFORM    0x06U
#define FPGA_UART_CMD_OUTPUT      0x07U

#define FPGA_UART_CMD_SUM_BEGIN      0x20U
#define FPGA_UART_CMD_SUM_WAVE_BEGIN 0x21U
#define FPGA_UART_CMD_SUM_FREQ       0x22U
#define FPGA_UART_CMD_SUM_PHASE      0x23U
#define FPGA_UART_CMD_SUM_AMPLITUDE  0x24U
#define FPGA_UART_CMD_SUM_OFFSET     0x25U
#define FPGA_UART_CMD_SUM_DUTY       0x26U
#define FPGA_UART_CMD_SUM_WAVEFORM   0x27U
#define FPGA_UART_CMD_SUM_ENABLE     0x28U
#define FPGA_UART_CMD_SUM_ABORT      0x2DU
#define FPGA_UART_CMD_SUM_COMMIT     0x2EU
#define FPGA_UART_CMD_SUM_END        0x2FU

typedef struct {
  uint8_t cmd;
  uint32_t data;
} FpgaUartCommand;

static FpgaUartState fpga_uart_state;
static uint32_t fpga_uart_last_tx_tick;
static uint8_t fpga_uart_ack_buf[FPGA_UART_ACK_LEN];
static uint8_t fpga_uart_ack_pos;
static FpgaUartCommand fpga_uart_queue[FPGA_UART_QUEUE_MAX];
static uint8_t fpga_uart_queue_count;
static uint8_t fpga_uart_queue_index;
static uint8_t fpga_uart_waiting_ack;
static uint8_t fpga_uart_retry_count;
static uint8_t fpga_uart_pending_cmd;
static uint32_t fpga_uart_last_ack_wait_tick;

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

        if ((fpga_uart_waiting_ack != 0U) &&
            (fpga_uart_ack_buf[1] == fpga_uart_pending_cmd))
        {
          fpga_uart_waiting_ack = 0U;
          fpga_uart_state.waiting_ack = 0U;
          if (fpga_uart_ack_buf[2] == 0U)
          {
            fpga_uart_queue_index++;
          }
          else
          {
            fpga_uart_queue_count = fpga_uart_queue_index;
            fpga_uart_state.error_count++;
          }
          fpga_uart_retry_count = 0U;
          fpga_uart_state.retry_count = 0U;
        }
        else if (fpga_uart_ack_buf[2] != 0U)
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

static void FpgaUart_ClearQueue(void)
{
  fpga_uart_queue_count = 0U;
  fpga_uart_queue_index = 0U;
  fpga_uart_waiting_ack = 0U;
  fpga_uart_retry_count = 0U;
  fpga_uart_pending_cmd = 0U;
  fpga_uart_state.queue_count = 0U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.waiting_ack = 0U;
  fpga_uart_state.retry_count = 0U;
  fpga_uart_state.dirty_mask = 0U;
}

static void FpgaUart_QueueCommand(uint8_t cmd, uint32_t data)
{
  if (fpga_uart_queue_count >= FPGA_UART_QUEUE_MAX)
  {
    fpga_uart_state.error_count++;
    return;
  }
  fpga_uart_queue[fpga_uart_queue_count].cmd = cmd;
  fpga_uart_queue[fpga_uart_queue_count].data = data;
  fpga_uart_queue_count++;
  fpga_uart_state.queue_count = fpga_uart_queue_count;
  fpga_uart_state.dirty_mask = (fpga_uart_queue_index < fpga_uart_queue_count) ? 1U : 0U;
}

static uint32_t FpgaUart_BeginData(uint8_t channel_id, uint8_t wave_count)
{
  uint32_t data = 0UL;
  data |= (uint32_t)(channel_id & 0x01U);
  data |= (uint32_t)wave_count << 8;
  data |= 0x03UL << 16;
  data |= 0x01UL << 24;
  return data;
}

static uint32_t FpgaUart_WaveBeginData(uint8_t channel_id, uint8_t wave_index, uint8_t enable)
{
  uint32_t data = 0UL;
  data |= (uint32_t)wave_index;
  data |= (uint32_t)(channel_id & 0x01U) << 8;
  data |= (uint32_t)(enable != 0U) << 16;
  return data;
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
  fpga_uart_state.dirty_mask = 0U;
  fpga_uart_state.queue_count = 0U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.waiting_ack = 0U;
  fpga_uart_state.retry_count = 0U;
  fpga_uart_state.last_tx_status = HAL_OK;
  fpga_uart_state.last_rx_status = HAL_OK;
  fpga_uart_last_tx_tick = HAL_GetTick();
  fpga_uart_last_ack_wait_tick = fpga_uart_last_tx_tick;
  fpga_uart_ack_pos = 0U;
  FpgaUart_ClearQueue();
}

void FpgaUart_Task(void)
{
  uint32_t now = HAL_GetTick();
  HAL_StatusTypeDef status;
  uint8_t should_send = 0U;

  FpgaUart_ReadAck();

  if ((fpga_uart_waiting_ack != 0U) &&
      ((now - fpga_uart_last_ack_wait_tick) >= FPGA_UART_ACK_TIMEOUT_MS))
  {
    fpga_uart_state.error_count++;
    if (fpga_uart_retry_count < FPGA_UART_MAX_RETRY)
    {
      fpga_uart_retry_count++;
      fpga_uart_state.retry_count = fpga_uart_retry_count;
      fpga_uart_waiting_ack = 0U;
      fpga_uart_state.waiting_ack = 0U;
    }
    else
    {
      fpga_uart_waiting_ack = 0U;
      fpga_uart_state.waiting_ack = 0U;
      fpga_uart_queue_count = fpga_uart_queue_index;
      fpga_uart_retry_count = 0U;
      fpga_uart_state.retry_count = 0U;
    }
  }

  if ((fpga_uart_waiting_ack == 0U) &&
      ((now - fpga_uart_last_tx_tick) >= FPGA_UART_SEND_PERIOD_MS) &&
      (fpga_uart_queue_index < fpga_uart_queue_count))
  {
    should_send = 1U;
  }

  if (should_send != 0U)
  {
    uint8_t frame[FPGA_UART_FRAME_LEN];
    FpgaUartCommand *command = &fpga_uart_queue[fpga_uart_queue_index];

    fpga_uart_last_tx_tick = now;
    frame[0] = FPGA_UART_HEADER;
    frame[1] = command->cmd;
    FpgaUart_WriteU32(frame, command->data);
    frame[6] = FpgaUart_Checksum(frame, 6U);

    FpgaUart_ClearUartErrors();
    status = HAL_UART_Transmit(&huart2, frame, FPGA_UART_FRAME_LEN, FPGA_UART_TX_TIMEOUT_MS);
    fpga_uart_state.last_tx_status = status;
    if (status == HAL_OK)
    {
      fpga_uart_state.tx_count++;
      fpga_uart_state.last_cmd = command->cmd;
      fpga_uart_state.last_data = command->data;
      fpga_uart_pending_cmd = command->cmd;
      fpga_uart_waiting_ack = 1U;
      fpga_uart_state.waiting_ack = 1U;
      fpga_uart_last_ack_wait_tick = now;
    }
    else
    {
      fpga_uart_state.error_count++;
      (void)HAL_UART_Abort(&huart2);
      FpgaUart_ClearUartErrors();
    }
  }

  if (fpga_uart_queue_index >= fpga_uart_queue_count)
  {
    fpga_uart_state.dirty_mask = 0U;
  }
  else
  {
    fpga_uart_state.dirty_mask = 1U;
  }
  fpga_uart_state.queue_count = fpga_uart_queue_count;
  fpga_uart_state.queue_index = fpga_uart_queue_index;
}

void FpgaUart_SetSignal(uint32_t frequency_hz, uint16_t phase_deg,
                        uint16_t amplitude_code, int16_t offset_code,
                        uint16_t duty_code, uint8_t waveform,
                        uint8_t output_enable)
{
  FpgaUart_ClearQueue();
  FpgaUart_QueueCommand(FPGA_UART_CMD_FREQ, FpgaUart_FrequencyToFtw(frequency_hz));
  FpgaUart_QueueCommand(FPGA_UART_CMD_PHASE, FpgaUart_PhaseToWord(phase_deg));
  FpgaUart_QueueCommand(FPGA_UART_CMD_AMPLITUDE, amplitude_code);
  FpgaUart_QueueCommand(FPGA_UART_CMD_OFFSET, (uint16_t)offset_code);
  FpgaUart_QueueCommand(FPGA_UART_CMD_DUTY, duty_code);
  FpgaUart_QueueCommand(FPGA_UART_CMD_WAVEFORM, waveform);
  FpgaUart_QueueCommand(FPGA_UART_CMD_OUTPUT, output_enable);
}

void FpgaUart_SetSum(uint8_t channel_id, uint8_t wave_count,
                     const FpgaUartWaveConfig *waves)
{
  if (waves == 0)
  {
    return;
  }
  if (wave_count == 0U)
  {
    wave_count = 1U;
  }
  if (wave_count > FPGA_UART_SUM_MAX_WAVES)
  {
    wave_count = FPGA_UART_SUM_MAX_WAVES;
  }

  FpgaUart_ClearQueue();
  FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_BEGIN, FpgaUart_BeginData(channel_id, wave_count));

  for (uint8_t i = 0U; i < wave_count; i++)
  {
    const FpgaUartWaveConfig *wave = &waves[i];
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_WAVE_BEGIN, FpgaUart_WaveBeginData(channel_id, i, wave->enable));
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_FREQ, FpgaUart_FrequencyToFtw(wave->frequency_hz));
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_PHASE, FpgaUart_PhaseToWord(wave->phase_deg));
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_AMPLITUDE, wave->amplitude_code);
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_OFFSET, (uint16_t)wave->offset_code);
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_DUTY, wave->duty_code);
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_WAVEFORM, wave->waveform);
    FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_ENABLE, wave->enable);
  }

  FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_COMMIT, ((uint32_t)wave_count << 8) | (uint32_t)(channel_id & 0x01U));
  FpgaUart_QueueCommand(FPGA_UART_CMD_SUM_END, (uint32_t)(channel_id & 0x01U));
}

FpgaUartState FpgaUart_GetState(void)
{
  return fpga_uart_state;
}
