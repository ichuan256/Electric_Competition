#include "FpgaUart_User.h"

#include <string.h>
#include "usart.h"

#define FPGA_UART_HEAD0              0xD3U
#define FPGA_UART_HEAD1              0x91U
#define FPGA_UART_TAIL0              0x91U
#define FPGA_UART_TAIL1              0xD3U
#define FPGA_UART_VERSION            0x02U
#define FPGA_UART_NODE_BLUE          0x02U
#define FPGA_UART_NODE_FPGA          0x10U
#define FPGA_UART_FLAG_ACK_REQ       0x01U
#define FPGA_UART_FLAG_RESPONSE      0x02U
#define FPGA_UART_FLAG_EVENT         0x04U
#define FPGA_UART_FLAG_ERROR         0x08U
#define FPGA_UART_STATUS_OK          0x00U
#define FPGA_UART_STATUS_TIMEOUT     0x0CU
#define FPGA_UART_FRAME_OVERHEAD     15U
#define FPGA_UART_MAX_PAYLOAD        80U
#define FPGA_UART_MAX_FRAME_LEN      (FPGA_UART_MAX_PAYLOAD + FPGA_UART_FRAME_OVERHEAD)
#define FPGA_UART_TX_QUEUE_DEPTH     3U
#define FPGA_UART_SEND_PERIOD_MS     20UL
#define FPGA_UART_ACK_TIMEOUT_MS     100UL
#define FPGA_UART_RX_FRAME_TIMEOUT_MS 20UL
#define FPGA_UART_MAX_RETRY          2U
#define FPGA_UART_TX_TIMEOUT_MS      50U
#define FPGA_UART_RX_DMA_BUF_LEN     256U
#define FPGA_UART_RX_DMA_BUF_ADDR    0x24070000UL
#define FPGA_UART_SAMPLE_CLK_HZ      100000000ULL
#define FPGA_UART_AD9910_CLK_HZ      1000000000ULL
#define FPGA_UART_COHERENT_CLK_RATIO (FPGA_UART_AD9910_CLK_HZ / FPGA_UART_SAMPLE_CLK_HZ)
#define FPGA_UART_RX_DMA_BUF         ((uint8_t *)FPGA_UART_RX_DMA_BUF_ADDR)

#if ((FPGA_UART_AD9910_CLK_HZ % FPGA_UART_SAMPLE_CLK_HZ) != 0ULL)
#error "AD9910 and FPGA clocks must have an integer coherent ratio"
#endif

typedef struct {
  uint8_t data[FPGA_UART_MAX_FRAME_LEN];
  uint16_t len;
  uint16_t seq;
  uint8_t cmd;
} FpgaUartTxFrame;

static FpgaUartState fpga_uart_state;
static FpgaUartTxFrame fpga_uart_queue[FPGA_UART_TX_QUEUE_DEPTH];
static uint8_t fpga_uart_queue_count;
static uint8_t fpga_uart_queue_index;
static uint8_t fpga_uart_frame_sent;
static uint8_t fpga_uart_waiting_ack;
static uint8_t fpga_uart_retry_count;
static uint16_t fpga_uart_tx_seq;
static uint16_t fpga_uart_local_transaction;
static uint32_t fpga_uart_last_tx_tick;
static uint32_t fpga_uart_last_ack_wait_tick;
static uint32_t fpga_uart_rx_last_byte_tick;
static uint8_t fpga_uart_rx_frame[FPGA_UART_MAX_FRAME_LEN];
static uint16_t fpga_uart_rx_frame_pos;
static uint16_t fpga_uart_rx_expected_len;
static uint16_t fpga_uart_rx_dma_read_pos;
static uint8_t fpga_uart_rx_dma_active;

static uint16_t FpgaUart_Crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFU;

  for (uint16_t i = 0U; i < len; i++)
  {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      crc = ((crc & 0x8000U) != 0U) ?
            (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint16_t FpgaUart_ReadU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void FpgaUart_WriteU16(uint8_t *data, uint8_t *pos, uint16_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFU);
  data[(*pos)++] = (uint8_t)(value >> 8);
}

static void FpgaUart_WriteU32(uint8_t *data, uint8_t *pos, uint32_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  data[(*pos)++] = (uint8_t)(value >> 24);
}

static uint32_t FpgaUart_FrequencyToFtw(uint32_t frequency_hz)
{
  uint64_t ad9910_ftw = (((uint64_t)frequency_hz << 32) +
                         (FPGA_UART_AD9910_CLK_HZ / 2ULL)) /
                        FPGA_UART_AD9910_CLK_HZ;

  /* Both clocks come from the same source: 1 GHz / 100 MHz = 10 exactly. */
  return (uint32_t)(ad9910_ftw * FPGA_UART_COHERENT_CLK_RATIO);
}

static uint32_t FpgaUart_PhaseToWord(uint16_t phase_deg)
{
  uint64_t phase = (uint64_t)(phase_deg % 360U);
  return (uint32_t)(((phase << 32) + 180ULL) / 360ULL);
}

static void FpgaUart_ClearUartErrors(void)
{
  uint32_t isr = huart2.Instance->ISR;

  if ((isr & USART_ISR_ORE) != 0UL) { __HAL_UART_CLEAR_OREFLAG(&huart2); }
  if ((isr & USART_ISR_FE) != 0UL) { __HAL_UART_CLEAR_FEFLAG(&huart2); }
  if ((isr & USART_ISR_NE) != 0UL) { __HAL_UART_CLEAR_NEFLAG(&huart2); }
}

static void FpgaUart_ResetRxParser(void)
{
  fpga_uart_rx_frame_pos = 0U;
  fpga_uart_rx_expected_len = 0U;
}

static void FpgaUart_ClearQueue(void)
{
  fpga_uart_queue_count = 0U;
  fpga_uart_queue_index = 0U;
  fpga_uart_frame_sent = 0U;
  fpga_uart_waiting_ack = 0U;
  fpga_uart_retry_count = 0U;
  fpga_uart_state.dirty_mask = 0U;
  fpga_uart_state.queue_count = 0U;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.waiting_ack = 0U;
  fpga_uart_state.retry_count = 0U;
}

static uint8_t FpgaUart_QueueV2Frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
  FpgaUartTxFrame *entry;
  uint16_t crc;
  uint16_t seq;

  if ((len > FPGA_UART_MAX_PAYLOAD) ||
      ((len != 0U) && (payload == 0)) ||
      (fpga_uart_queue_count >= FPGA_UART_TX_QUEUE_DEPTH))
  {
    return 0U;
  }

  entry = &fpga_uart_queue[fpga_uart_queue_count++];
  seq = fpga_uart_tx_seq++;
  entry->data[0] = FPGA_UART_HEAD0;
  entry->data[1] = FPGA_UART_HEAD1;
  entry->data[2] = FPGA_UART_VERSION;
  entry->data[3] = FPGA_UART_NODE_FPGA;
  entry->data[4] = FPGA_UART_NODE_BLUE;
  entry->data[5] = cmd;
  entry->data[6] = FPGA_UART_FLAG_ACK_REQ;
  entry->data[7] = (uint8_t)(seq & 0xFFU);
  entry->data[8] = (uint8_t)(seq >> 8);
  entry->data[9] = len;
  entry->data[10] = 0U;
  if (len != 0U)
  {
    memcpy(&entry->data[11], payload, len);
  }
  crc = FpgaUart_Crc16(&entry->data[2], (uint16_t)(9U + len));
  entry->data[11U + len] = (uint8_t)(crc & 0xFFU);
  entry->data[12U + len] = (uint8_t)(crc >> 8);
  entry->data[13U + len] = FPGA_UART_TAIL0;
  entry->data[14U + len] = FPGA_UART_TAIL1;
  entry->len = (uint16_t)(len + FPGA_UART_FRAME_OVERHEAD);
  entry->seq = seq;
  entry->cmd = cmd;
  return 1U;
}

static void FpgaUart_CompleteAck(uint8_t request_cmd, uint8_t status,
                                 uint16_t seq, uint16_t transaction_id,
                                 uint16_t applied_mask)
{
  FpgaUartTxFrame *entry;

  fpga_uart_state.last_ack_cmd = request_cmd;
  fpga_uart_state.last_ack_status = status;
  fpga_uart_state.last_ack_seq = seq;
  fpga_uart_state.last_transaction_id = transaction_id;
  fpga_uart_state.applied_mask = applied_mask;
  fpga_uart_state.rx_value = request_cmd;
  fpga_uart_state.has_rx = 1U;
  fpga_uart_state.rx_count++;

  if ((fpga_uart_waiting_ack == 0U) ||
      (fpga_uart_queue_index >= fpga_uart_queue_count))
  {
    return;
  }
  entry = &fpga_uart_queue[fpga_uart_queue_index];
  if ((request_cmd != entry->cmd) || (seq != entry->seq))
  {
    return;
  }

  fpga_uart_waiting_ack = 0U;
  fpga_uart_state.waiting_ack = 0U;
  fpga_uart_retry_count = 0U;
  fpga_uart_state.retry_count = 0U;
  if (status != FPGA_UART_STATUS_OK)
  {
    fpga_uart_state.error_count++;
    FpgaUart_ClearQueue();
    return;
  }

  fpga_uart_queue_index++;
  fpga_uart_state.queue_index = fpga_uart_queue_index;
  fpga_uart_frame_sent = 0U;
  if (fpga_uart_queue_index >= fpga_uart_queue_count)
  {
    fpga_uart_state.dirty_mask = 0U;
  }
}

static void FpgaUart_HandleV2Frame(const uint8_t *frame, uint16_t frame_len)
{
  uint16_t payload_len;
  uint16_t received_crc;
  uint16_t seq;
  const uint8_t *payload;

  if (frame_len < FPGA_UART_FRAME_OVERHEAD)
  {
    fpga_uart_state.parse_error_count++;
    fpga_uart_state.error_count++;
    return;
  }
  payload_len = FpgaUart_ReadU16(&frame[9]);
  if ((payload_len > FPGA_UART_MAX_PAYLOAD) ||
      (frame_len != (uint16_t)(payload_len + FPGA_UART_FRAME_OVERHEAD)) ||
      (frame[2] != FPGA_UART_VERSION) ||
      (frame[3] != FPGA_UART_NODE_BLUE) ||
      (frame[4] != FPGA_UART_NODE_FPGA) ||
      ((frame[6] & 0xC0U) != 0U) ||
      ((frame[6] & (FPGA_UART_FLAG_RESPONSE | FPGA_UART_FLAG_EVENT)) == 0U) ||
      (frame[13U + payload_len] != FPGA_UART_TAIL0) ||
      (frame[14U + payload_len] != FPGA_UART_TAIL1))
  {
    fpga_uart_state.parse_error_count++;
    fpga_uart_state.error_count++;
    return;
  }
  received_crc = FpgaUart_ReadU16(&frame[11U + payload_len]);
  if (received_crc != FpgaUart_Crc16(&frame[2], (uint16_t)(9U + payload_len)))
  {
    fpga_uart_state.crc_error_count++;
    fpga_uart_state.error_count++;
    return;
  }

  fpga_uart_state.last_rx_cmd = frame[5];
  seq = FpgaUart_ReadU16(&frame[7]);
  payload = &frame[11];
  if ((frame[5] == FPGA_UART_CMD_ACK) && (payload_len == 8U))
  {
    FpgaUart_CompleteAck(payload[0], payload[1], seq,
                         FpgaUart_ReadU16(&payload[4]),
                         FpgaUart_ReadU16(&payload[6]));
  }
}

static void FpgaUart_ProcessRxByte(uint8_t byte)
{
  fpga_uart_rx_last_byte_tick = HAL_GetTick();
  if (fpga_uart_rx_frame_pos == 0U)
  {
    if (byte == FPGA_UART_HEAD0)
    {
      fpga_uart_rx_frame[0] = byte;
      fpga_uart_rx_frame_pos = 1U;
    }
    return;
  }
  if (fpga_uart_rx_frame_pos == 1U)
  {
    if (byte == FPGA_UART_HEAD1)
    {
      fpga_uart_rx_frame[1] = byte;
      fpga_uart_rx_frame_pos = 2U;
    }
    else if (byte != FPGA_UART_HEAD0)
    {
      FpgaUart_ResetRxParser();
    }
    return;
  }

  if (fpga_uart_rx_frame_pos >= FPGA_UART_MAX_FRAME_LEN)
  {
    fpga_uart_state.parse_error_count++;
    fpga_uart_state.error_count++;
    FpgaUart_ResetRxParser();
    return;
  }
  fpga_uart_rx_frame[fpga_uart_rx_frame_pos++] = byte;
  if (fpga_uart_rx_frame_pos == 11U)
  {
    uint16_t payload_len = FpgaUart_ReadU16(&fpga_uart_rx_frame[9]);
    if (payload_len > FPGA_UART_MAX_PAYLOAD)
    {
      fpga_uart_state.parse_error_count++;
      fpga_uart_state.error_count++;
      FpgaUart_ResetRxParser();
      return;
    }
    fpga_uart_rx_expected_len = (uint16_t)(payload_len + FPGA_UART_FRAME_OVERHEAD);
  }
  if ((fpga_uart_rx_expected_len != 0U) &&
      (fpga_uart_rx_frame_pos == fpga_uart_rx_expected_len))
  {
    FpgaUart_HandleV2Frame(fpga_uart_rx_frame, fpga_uart_rx_frame_pos);
    FpgaUart_ResetRxParser();
  }
}

static void FpgaUart_StartRxDma(void)
{
  HAL_StatusTypeDef status;

  memset(FPGA_UART_RX_DMA_BUF, 0, FPGA_UART_RX_DMA_BUF_LEN);
  SCB_CleanInvalidateDCache_by_Addr((uint32_t *)FPGA_UART_RX_DMA_BUF_ADDR,
                                    FPGA_UART_RX_DMA_BUF_LEN);
  fpga_uart_rx_dma_read_pos = 0U;
  status = HAL_UART_Receive_DMA(&huart2, FPGA_UART_RX_DMA_BUF, FPGA_UART_RX_DMA_BUF_LEN);
  fpga_uart_state.last_rx_status = status;
  fpga_uart_rx_dma_active = (status == HAL_OK) ? 1U : 0U;
  fpga_uart_state.rx_dma_active = fpga_uart_rx_dma_active;
  if (status == HAL_OK)
  {
    if (huart2.hdmarx != 0)
    {
      __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);
      __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_TC);
    }
  }
  else
  {
    fpga_uart_state.error_count++;
  }
}

static void FpgaUart_ReadDmaBytes(void)
{
  uint16_t write_pos;
  uint16_t guard = 0U;

  if ((fpga_uart_rx_dma_active == 0U) || (huart2.hdmarx == 0))
  {
    FpgaUart_StartRxDma();
    return;
  }
  SCB_InvalidateDCache_by_Addr((uint32_t *)FPGA_UART_RX_DMA_BUF_ADDR,
                               FPGA_UART_RX_DMA_BUF_LEN);
  write_pos = (uint16_t)(FPGA_UART_RX_DMA_BUF_LEN - __HAL_DMA_GET_COUNTER(huart2.hdmarx));
  if (write_pos >= FPGA_UART_RX_DMA_BUF_LEN) { write_pos = 0U; }
  fpga_uart_state.rx_dma_write_pos = write_pos;

  while ((fpga_uart_rx_dma_read_pos != write_pos) && (guard < FPGA_UART_RX_DMA_BUF_LEN))
  {
    uint8_t byte = FPGA_UART_RX_DMA_BUF[fpga_uart_rx_dma_read_pos++];
    if (fpga_uart_rx_dma_read_pos >= FPGA_UART_RX_DMA_BUF_LEN) { fpga_uart_rx_dma_read_pos = 0U; }
    fpga_uart_state.rx_dma_count++;
    FpgaUart_ProcessRxByte(byte);
    guard++;
  }
  fpga_uart_state.rx_dma_read_pos = fpga_uart_rx_dma_read_pos;
}

static void FpgaUart_ReadRx(void)
{
  uint8_t guard = 0U;

  FpgaUart_ClearUartErrors();
  FpgaUart_ReadDmaBytes();
  while ((__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) != RESET) && (guard < 16U))
  {
    FpgaUart_ProcessRxByte((uint8_t)(huart2.Instance->RDR & 0xFFU));
    guard++;
  }
}

void FpgaUart_Init(void)
{
  memset(&fpga_uart_state, 0, sizeof(fpga_uart_state));
  fpga_uart_state.last_ack_status = 0xFFU;
  fpga_uart_state.last_tx_status = HAL_OK;
  fpga_uart_state.last_rx_status = HAL_OK;
  fpga_uart_tx_seq = 1U;
  fpga_uart_local_transaction = 1U;
  fpga_uart_rx_dma_read_pos = 0U;
  fpga_uart_rx_dma_active = 0U;
  FpgaUart_ResetRxParser();
  FpgaUart_ClearQueue();
  fpga_uart_last_tx_tick = HAL_GetTick();
  fpga_uart_last_ack_wait_tick = fpga_uart_last_tx_tick;
  fpga_uart_rx_last_byte_tick = fpga_uart_last_tx_tick;
  FpgaUart_StartRxDma();
}

void FpgaUart_Task(void)
{
  uint32_t now = HAL_GetTick();
  HAL_StatusTypeDef status;
  FpgaUartTxFrame *entry;

  if ((fpga_uart_rx_frame_pos != 0U) &&
      ((now - fpga_uart_rx_last_byte_tick) >= FPGA_UART_RX_FRAME_TIMEOUT_MS))
  {
    fpga_uart_state.parse_error_count++;
    fpga_uart_state.error_count++;
    FpgaUart_ResetRxParser();
  }
  FpgaUart_ReadRx();
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
    }
    else
    {
      entry = &fpga_uart_queue[fpga_uart_queue_index];
      fpga_uart_state.last_ack_cmd = entry->cmd;
      fpga_uart_state.last_ack_status = FPGA_UART_STATUS_TIMEOUT;
      fpga_uart_state.last_ack_seq = entry->seq;
      FpgaUart_ClearQueue();
    }
  }

  if ((fpga_uart_queue_index < fpga_uart_queue_count) &&
      (fpga_uart_frame_sent == 0U) && (fpga_uart_waiting_ack == 0U) &&
      ((now - fpga_uart_last_tx_tick) >= FPGA_UART_SEND_PERIOD_MS))
  {
    entry = &fpga_uart_queue[fpga_uart_queue_index];
    FpgaUart_ClearUartErrors();
    status = HAL_UART_Transmit(&huart2, entry->data, entry->len, FPGA_UART_TX_TIMEOUT_MS);
    fpga_uart_state.last_tx_status = status;
    fpga_uart_last_tx_tick = now;
    if (status == HAL_OK)
    {
      fpga_uart_state.tx_count++;
      fpga_uart_state.last_cmd = entry->cmd;
      fpga_uart_state.last_data = entry->len;
      fpga_uart_frame_sent = 1U;
      fpga_uart_waiting_ack = 1U;
      fpga_uart_state.waiting_ack = 1U;
      fpga_uart_last_ack_wait_tick = now;
    }
    else
    {
      fpga_uart_state.error_count++;
      (void)HAL_UART_AbortTransmit(&huart2);
      FpgaUart_ClearUartErrors();
      if (fpga_uart_retry_count < FPGA_UART_MAX_RETRY)
      {
        fpga_uart_retry_count++;
        fpga_uart_state.retry_count = fpga_uart_retry_count;
      }
      else
      {
        fpga_uart_state.last_ack_cmd = entry->cmd;
        fpga_uart_state.last_ack_status = FPGA_UART_STATUS_TIMEOUT;
        fpga_uart_state.last_ack_seq = entry->seq;
        FpgaUart_ClearQueue();
      }
    }
  }
}

void FpgaUart_SetMultiwaveTransaction(uint16_t transaction_id,
                                      uint8_t channel_id, uint8_t wave_count,
                                      const FpgaUartWaveConfig *waves,
                                      int16_t offset_code, uint8_t control_flags,
                                      uint16_t period_points, uint8_t commit_flags)
{
  uint8_t stage[10U + FPGA_UART_SUM_MAX_WAVES * 16U];
  uint8_t commit[4];
  uint8_t pos = 0U;
  uint8_t enabled = 0U;

  if ((waves == 0) || (channel_id > 1U) || (wave_count > FPGA_UART_SUM_MAX_WAVES))
  {
    return;
  }
  if (offset_code > 8191) { offset_code = 8191; }
  if (offset_code < -8192) { offset_code = -8192; }
  if ((control_flags & FPGA_UART_CONTROL_CACHE) == 0U) { period_points = 0U; }

  FpgaUart_ClearQueue();
  FpgaUart_WriteU16(stage, &pos, transaction_id);
  stage[pos++] = channel_id;
  stage[pos++] = ((control_flags & FPGA_UART_CONTROL_CACHE) != 0U) ? 2U : 1U;
  stage[pos++] = wave_count;
  for (uint8_t i = 0U; i < wave_count; i++) { enabled |= waves[i].enable; }
  stage[pos++] = (uint8_t)(0x02U | ((enabled != 0U) ? 0x01U : 0U));
  FpgaUart_WriteU16(stage, &pos, (uint16_t)offset_code);
  FpgaUart_WriteU16(stage, &pos, period_points);

  for (uint8_t i = 0U; i < wave_count; i++)
  {
    const FpgaUartWaveConfig *wave = &waves[i];
    uint16_t amplitude = (wave->amplitude_code > 8191U) ? 8191U : wave->amplitude_code;
    stage[pos++] = i;
    stage[pos++] = (wave->waveform <= 3U) ? (uint8_t)(wave->waveform + 1U) : 0U;
    FpgaUart_WriteU16(stage, &pos, (wave->enable != 0U) ? 1U : 0U);
    FpgaUart_WriteU32(stage, &pos, FpgaUart_FrequencyToFtw(wave->frequency_hz));
    FpgaUart_WriteU32(stage, &pos, FpgaUart_PhaseToWord(wave->phase_deg));
    FpgaUart_WriteU16(stage, &pos, amplitude);
    FpgaUart_WriteU16(stage, &pos, wave->duty_code);
  }

  if (FpgaUart_QueueV2Frame(FPGA_UART_CMD_CHANNEL_STAGE, stage, pos) == 0U)
  {
    FpgaUart_ClearQueue();
    return;
  }
  pos = 0U;
  FpgaUart_WriteU16(commit, &pos, transaction_id);
  commit[pos++] = (uint8_t)(1U << channel_id);
  commit[pos++] = commit_flags;
  if (FpgaUart_QueueV2Frame(FPGA_UART_CMD_COMMIT, commit, pos) == 0U)
  {
    FpgaUart_ClearQueue();
    return;
  }

  fpga_uart_state.dirty_mask = (uint8_t)(1U << channel_id);
  fpga_uart_state.queue_count = fpga_uart_queue_count;
  fpga_uart_state.queue_index = 0U;
  fpga_uart_state.last_transaction_id = transaction_id;
}

void FpgaUart_SetMultiwave(uint8_t target, uint8_t wave_count,
                           const FpgaUartWaveConfig *waves,
                           int16_t offset_code, uint8_t control_flags,
                           uint16_t period_points)
{
  FpgaUart_SetMultiwaveTransaction(fpga_uart_local_transaction++, target, wave_count,
                                   waves, offset_code, control_flags, period_points,
                                   0x09U);
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
  FpgaUart_SetMultiwave(channel_id, 1U, &wave, offset_code,
                        FPGA_UART_CONTROL_REALTIME, 0U);
}

void FpgaUart_SetSum(uint8_t channel_id, uint8_t wave_count,
                     const FpgaUartWaveConfig *waves)
{
  int16_t offset_code = 0;

  if ((waves != 0) && (wave_count != 0U)) { offset_code = waves[0].offset_code; }
  FpgaUart_SetMultiwave(channel_id, wave_count, waves, offset_code,
                        FPGA_UART_CONTROL_REALTIME, 0U);
}

FpgaUartState FpgaUart_GetState(void)
{
  return fpga_uart_state;
}
