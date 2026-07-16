#include "BoardComm_User.h"
#include "SpectrumDisplay_User.h"
#include "LcrAuto_User.h"
#include "LcrCalibration_User.h"

static UART_HandleTypeDef *board_comm_uart = &huart1;
static uint8_t board_comm_rx_buf[BOARD_COMM_RX_BUF_SIZE];
static uint8_t board_comm_tx_buf[BOARD_COMM_TX_BUF_SIZE];
static uint16_t board_comm_tx_seq = 1U;
static uint8_t board_comm_parsed_src;
static uint8_t board_comm_parsed_dst;
static uint8_t board_comm_parsed_flags;
static uint16_t board_comm_parsed_seq;

#define BOARD_COMM_QUEUE_DEPTH 8U
typedef enum {
  BOARD_COMM_FRAME_NO_MATCH = 0,
  BOARD_COMM_FRAME_READY,
  BOARD_COMM_FRAME_INCOMPLETE,
  BOARD_COMM_FRAME_INVALID
} BoardComm_FrameProbeResult;

typedef BoardComm_FrameProbeResult (*BoardComm_FrameProbe)(const uint8_t *data,
                                                            uint16_t available,
                                                            uint16_t *frame_size);
typedef void (*BoardComm_FrameDispatch)(const uint8_t *frame, uint16_t frame_size);

typedef struct {
  BoardComm_FrameProbe probe;
  BoardComm_FrameDispatch dispatch;
} BoardComm_FrameDecoder;

typedef struct {
  uint8_t cmd;
  uint8_t len;
  uint8_t src;
  uint8_t dst;
  uint8_t flags;
  uint16_t seq;
  BoardComm_Status status;
  uint8_t data[BOARD_COMM_MAX_PAYLOAD];
} BoardComm_QueuedFrame;

static BoardComm_QueuedFrame board_comm_queue[BOARD_COMM_QUEUE_DEPTH];
static volatile uint8_t board_comm_queue_head = 0;
static volatile uint8_t board_comm_queue_tail = 0;
static volatile uint8_t board_comm_queue_count = 0;
static volatile uint8_t board_comm_last_cmd = 0;
static volatile uint8_t board_comm_last_len = 0;
static volatile uint8_t board_comm_last_src = 0;
static volatile uint8_t board_comm_last_dst = 0;
static volatile uint8_t board_comm_last_flags = 0;
static volatile uint16_t board_comm_last_seq = 0;
static volatile BoardComm_Status board_comm_last_status = BOARD_COMM_OK;
static uint8_t board_comm_last_data[BOARD_COMM_MAX_PAYLOAD];
static volatile uint32_t board_comm_rx_count = 0;
static volatile uint32_t board_comm_error_count = 0;
static volatile uint16_t board_comm_last_rx_size = 0;
static volatile uint32_t board_comm_uart_error_code = 0;
static volatile uint8_t board_comm_key_pending = 0;
static volatile char board_comm_key_value = 0;

static uint16_t BoardComm_Crc16(const uint8_t *data, uint16_t len)
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

static void BoardComm_WriteU16(uint8_t *buf, uint8_t *pos, uint16_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFU);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static BoardComm_Status BoardComm_ParseFrame(const uint8_t *frame, uint16_t size,
                                             uint8_t *cmd, const uint8_t **data, uint8_t *len)
{
  uint16_t payload_len;
  uint16_t received_crc;

  if ((frame == 0) || (cmd == 0) || (data == 0) || (len == 0))
  {
    return BOARD_COMM_ERROR;
  }

  if (size < BOARD_COMM_FRAME_OVERHEAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  if ((frame[0] != BOARD_COMM_HEAD1) || (frame[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_ERROR;
  }

  if (frame[2] != BOARD_COMM_VERSION)
  {
    return BOARD_COMM_ERROR;
  }
  if ((frame[3] != BOARD_COMM_NODE_BLUE) &&
      (frame[3] != BOARD_COMM_NODE_BROADCAST))
  {
    return BOARD_COMM_ERROR;
  }
  if ((frame[6] & 0xC0U) != 0U)
  {
    return BOARD_COMM_ERROR;
  }

  payload_len = (uint16_t)frame[9] | ((uint16_t)frame[10] << 8);
  if (payload_len > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }
  if (size != (uint16_t)(payload_len + BOARD_COMM_FRAME_OVERHEAD))
  {
    return BOARD_COMM_LENGTH_ERROR;
  }
  if ((frame[13U + payload_len] != BOARD_COMM_TAIL1) ||
      (frame[14U + payload_len] != BOARD_COMM_TAIL2))
  {
    return BOARD_COMM_ERROR;
  }

  *cmd = frame[5];
  *len = (uint8_t)payload_len;
  *data = &frame[11];
  board_comm_parsed_dst = frame[3];
  board_comm_parsed_src = frame[4];
  board_comm_parsed_flags = frame[6];
  board_comm_parsed_seq = (uint16_t)frame[7] | ((uint16_t)frame[8] << 8);

  received_crc = (uint16_t)frame[11U + payload_len] |
                 ((uint16_t)frame[12U + payload_len] << 8);
  if (received_crc != BoardComm_Crc16(&frame[2], (uint16_t)(9U + payload_len)))
  {
    return BOARD_COMM_CHECKSUM_ERROR;
  }

  return BOARD_COMM_OK;
}

static void BoardComm_StoreRxFrame(uint8_t cmd, const uint8_t *data, uint8_t len, BoardComm_Status status)
{
  uint8_t queue_index;

  if (status == BOARD_COMM_OK)
  {
    if (len > BOARD_COMM_MAX_PAYLOAD)
    {
      len = BOARD_COMM_MAX_PAYLOAD;
    }

    if ((cmd == BOARD_COMM_CMD_KEYPAD) && (len == 1U))
    {
      board_comm_key_value = (char)data[0];
      board_comm_key_pending = 1;
    }

    /* Keep display traffic independent from later ADC requests or key frames. */
    if ((cmd == BOARD_COMM_CMD_SYS_STATUS) ||
        (cmd == BOARD_COMM_CMD_SWEEP_POINT) ||
        (cmd == BOARD_COMM_CMD_SWEEP_RESULT))
    {
      board_comm_last_cmd = cmd;
      board_comm_last_len = len;
      board_comm_last_src = board_comm_parsed_src;
      board_comm_last_dst = board_comm_parsed_dst;
      board_comm_last_flags = board_comm_parsed_flags;
      board_comm_last_seq = board_comm_parsed_seq;
      board_comm_last_status = status;
      for (uint8_t i = 0; i < len; i++)
      {
        board_comm_last_data[i] = data[i];
      }
      for (uint8_t i = len; i < BOARD_COMM_MAX_PAYLOAD; i++)
      {
        board_comm_last_data[i] = 0U;
      }
    }

    board_comm_rx_count++;
  }
  else
  {
    board_comm_error_count++;
  }

  if (board_comm_queue_count >= BOARD_COMM_QUEUE_DEPTH)
  {
    board_comm_error_count++;
    return;
  }

  queue_index = board_comm_queue_head;
  board_comm_queue[queue_index].cmd = cmd;
  board_comm_queue[queue_index].len = len;
  board_comm_queue[queue_index].src = board_comm_parsed_src;
  board_comm_queue[queue_index].dst = board_comm_parsed_dst;
  board_comm_queue[queue_index].flags = board_comm_parsed_flags;
  board_comm_queue[queue_index].seq = board_comm_parsed_seq;
  board_comm_queue[queue_index].status = status;
  for (uint8_t i = 0; i < len; i++)
  {
    board_comm_queue[queue_index].data[i] = data[i];
  }
  board_comm_queue_head = (uint8_t)((board_comm_queue_head + 1U) % BOARD_COMM_QUEUE_DEPTH);
  board_comm_queue_count++;
}

static BoardComm_FrameProbeResult BoardComm_ProbeStandardFrame(const uint8_t *data,
                                                                uint16_t available,
                                                                uint16_t *frame_size)
{
  if ((available < 2U) || (data[0] != BOARD_COMM_HEAD1) || (data[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_FRAME_NO_MATCH;
  }
  if (available < 11U)
  {
    return BOARD_COMM_FRAME_INCOMPLETE;
  }
  if (data[10] != 0U || data[9] > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_FRAME_INVALID;
  }

  *frame_size = (uint16_t)data[9] + BOARD_COMM_FRAME_OVERHEAD;
  return (*frame_size <= available) ? BOARD_COMM_FRAME_READY : BOARD_COMM_FRAME_INCOMPLETE;
}

static void BoardComm_DispatchStandardFrame(const uint8_t *frame, uint16_t frame_size)
{
  uint8_t cmd = 0U;
  uint8_t len = 0U;
  const uint8_t *data = 0;
  BoardComm_Status status = BoardComm_ParseFrame(frame, frame_size, &cmd, &data, &len);

  BoardComm_StoreRxFrame(cmd, data, len, status);
}

static const BoardComm_FrameDecoder board_comm_frame_decoders[] = {
  {BoardComm_ProbeStandardFrame, BoardComm_DispatchStandardFrame}
};

void BoardComm_Init(void)
{
  board_comm_uart = &huart1;
  board_comm_queue_head = 0U;
  board_comm_queue_tail = 0U;
  board_comm_queue_count = 0U;
  board_comm_last_cmd = 0U;
  board_comm_last_len = 0U;
  board_comm_last_src = 0U;
  board_comm_last_dst = 0U;
  board_comm_last_flags = 0U;
  board_comm_last_seq = 0U;
  board_comm_tx_seq = 1U;
  board_comm_parsed_src = 0U;
  board_comm_parsed_dst = 0U;
  board_comm_parsed_flags = 0U;
  board_comm_parsed_seq = 0U;
  board_comm_last_status = BOARD_COMM_OK;
  board_comm_rx_count = 0;
  board_comm_error_count = 0;
  board_comm_last_rx_size = 0;
  board_comm_uart_error_code = 0;
}

BoardComm_Status BoardComm_StartReceiveToIdleIT(void)
{
  if (board_comm_uart == 0)
  {
    return BOARD_COMM_ERROR;
  }

  if (HAL_UARTEx_ReceiveToIdle_IT(board_comm_uart, board_comm_rx_buf, (uint16_t)sizeof(board_comm_rx_buf)) != HAL_OK)
  {
    return BOARD_COMM_ERROR;
  }

  return BOARD_COMM_OK;
}

BoardComm_Status BoardComm_StopReceiveIT(void)
{
  if (board_comm_uart == 0)
  {
    return BOARD_COMM_ERROR;
  }

  if (HAL_UART_AbortReceive_IT(board_comm_uart) != HAL_OK)
  {
    return BOARD_COMM_ERROR;
  }

  return BOARD_COMM_OK;
}

void BoardComm_HandleRxIdleEvent(UART_HandleTypeDef *huart, uint16_t size)
{
  uint16_t offset = 0;

  if ((huart == 0) || (huart != board_comm_uart))
  {
    return;
  }

  board_comm_last_rx_size = size;

  while (offset < size)
  {
    uint8_t matched = 0U;
    uint16_t available = (uint16_t)(size - offset);

    for (uint32_t i = 0U; i < (sizeof(board_comm_frame_decoders) /
                               sizeof(board_comm_frame_decoders[0])); i++)
    {
      uint16_t frame_size = 0U;
      BoardComm_FrameProbeResult result =
          board_comm_frame_decoders[i].probe(&board_comm_rx_buf[offset], available, &frame_size);

      if (result == BOARD_COMM_FRAME_NO_MATCH)
      {
        continue;
      }

      matched = 1U;
      if (result == BOARD_COMM_FRAME_READY)
      {
        board_comm_frame_decoders[i].dispatch(&board_comm_rx_buf[offset], frame_size);
        offset = (uint16_t)(offset + frame_size);
      }
      else
      {
        BoardComm_StoreRxFrame(0, 0, 0,
                               (result == BOARD_COMM_FRAME_INCOMPLETE) ?
                               BOARD_COMM_LENGTH_ERROR : BOARD_COMM_ERROR);
        offset = size;
      }
      break;
    }

    if (matched == 0U)
    {
      BoardComm_StoreRxFrame(0, 0, 0, BOARD_COMM_ERROR);
      offset++;
    }
  }

  (void)BoardComm_StartReceiveToIdleIT();
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  BoardComm_HandleRxIdleEvent(huart, Size);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart == 0) || (huart != board_comm_uart))
  {
    return;
  }

  board_comm_uart_error_code = huart->ErrorCode;
  board_comm_error_count++;
  (void)BoardComm_StartReceiveToIdleIT();
}

void BoardComm_ProcessTask(void)
{
  uint8_t cmd;
  uint8_t len;
  uint8_t src;
  uint8_t flags;
  uint16_t seq;
  uint8_t data_copy[BOARD_COMM_MAX_PAYLOAD];
  BoardComm_Status status;
  uint8_t error_payload[2];

  if (board_comm_queue_count == 0U)
  {
    return;
  }

  __disable_irq();
  cmd = board_comm_queue[board_comm_queue_tail].cmd;
  len = board_comm_queue[board_comm_queue_tail].len;
  src = board_comm_queue[board_comm_queue_tail].src;
  flags = board_comm_queue[board_comm_queue_tail].flags;
  seq = board_comm_queue[board_comm_queue_tail].seq;
  status = board_comm_queue[board_comm_queue_tail].status;
  for (uint8_t i = 0; i < len; i++)
  {
    data_copy[i] = board_comm_queue[board_comm_queue_tail].data[i];
  }
  board_comm_queue_tail = (uint8_t)((board_comm_queue_tail + 1U) % BOARD_COMM_QUEUE_DEPTH);
  board_comm_queue_count--;
  __enable_irq();

  if ((status == BOARD_COMM_OK) && (cmd == BOARD_COMM_CMD_PING))
  {
    (void)BoardComm_SendV2(src, BOARD_COMM_CMD_PING,
                           BOARD_COMM_FLAG_RESPONSE, seq, data_copy, len);
  }
  else if ((status == BOARD_COMM_OK) && (cmd == BOARD_COMM_CMD_KEYPAD))
  {
    /* KEY_EVENT is already captured by BoardComm_StoreRxFrame(). */
  }
  else if ((status == BOARD_COMM_OK) &&
           (cmd == BOARD_COMM_CMD_LCR_EXCITATION_READY))
  {
    LcrAuto_HandleExcitationReady(data_copy, len);
    LcrCalibration_HandleExcitationReady(data_copy, len);
  }
  else if ((status == BOARD_COMM_OK) &&
           ((cmd == BOARD_COMM_CMD_SOURCE_STAGE) ||
            (cmd == BOARD_COMM_CMD_SOURCE_COMMIT)))
  {
    uint8_t ack[8];
    uint8_t ack_pos = 0U;
    uint16_t transaction_id = 0U;
    uint16_t applied_mask = 0U;
    uint8_t protocol_status = SpectrumDisplay_HandleSourceFrame(cmd, data_copy, len,
                                                                 &transaction_id,
                                                                 &applied_mask);
    ack[ack_pos++] = cmd;
    ack[ack_pos++] = protocol_status;
    BoardComm_WriteU16(ack, &ack_pos, 0U);
    BoardComm_WriteU16(ack, &ack_pos, transaction_id);
    BoardComm_WriteU16(ack, &ack_pos, applied_mask);
    (void)BoardComm_SendV2(src, BOARD_COMM_CMD_ACK,
                           (uint8_t)(BOARD_COMM_FLAG_RESPONSE |
                                     ((protocol_status != 0U) ? BOARD_COMM_FLAG_ERROR : 0U)),
                           seq, ack, ack_pos);
  }
  else if ((status == BOARD_COMM_OK) &&
           ((cmd == BOARD_COMM_CMD_SYS_STATUS) ||
            (cmd == BOARD_COMM_CMD_SWEEP_POINT) ||
            (cmd == BOARD_COMM_CMD_SWEEP_RESULT)))
  {
    /* SpectrumDisplay_User reads these frames from BoardComm_GetState(). */
  }
  else
  {
    error_payload[0] = cmd;
    error_payload[1] = (uint8_t)status;
    (void)BoardComm_Send(BOARD_COMM_CMD_ERROR, error_payload, (uint8_t)sizeof(error_payload));
  }

  (void)len;
  (void)flags;
}

BoardComm_Status BoardComm_Send(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint16_t seq = board_comm_tx_seq++;
  return BoardComm_SendV2(BOARD_COMM_NODE_BLACK, cmd, 0U, seq, data, len);
}

BoardComm_Status BoardComm_SendV2(uint8_t dst, uint8_t cmd, uint8_t flags,
                                  uint16_t seq, const uint8_t *data, uint8_t len)
{
  uint16_t crc;
  uint16_t frame_len;

  if (len > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  if ((len != 0U) && (data == 0))
  {
    return BOARD_COMM_ERROR;
  }

  board_comm_tx_buf[0] = BOARD_COMM_HEAD1;
  board_comm_tx_buf[1] = BOARD_COMM_HEAD2;
  board_comm_tx_buf[2] = BOARD_COMM_VERSION;
  board_comm_tx_buf[3] = dst;
  board_comm_tx_buf[4] = BOARD_COMM_NODE_BLUE;
  board_comm_tx_buf[5] = cmd;
  board_comm_tx_buf[6] = flags & 0x3FU;
  board_comm_tx_buf[7] = (uint8_t)(seq & 0xFFU);
  board_comm_tx_buf[8] = (uint8_t)(seq >> 8);
  board_comm_tx_buf[9] = len;
  board_comm_tx_buf[10] = 0U;

  for (uint8_t i = 0; i < len; i++)
  {
    board_comm_tx_buf[11U + i] = data[i];
  }

  crc = BoardComm_Crc16(&board_comm_tx_buf[2], (uint16_t)(9U + len));
  board_comm_tx_buf[11U + len] = (uint8_t)(crc & 0xFFU);
  board_comm_tx_buf[12U + len] = (uint8_t)(crc >> 8);
  board_comm_tx_buf[13U + len] = BOARD_COMM_TAIL1;
  board_comm_tx_buf[14U + len] = BOARD_COMM_TAIL2;
  frame_len = (uint16_t)(len + BOARD_COMM_FRAME_OVERHEAD);

  if (HAL_UART_Transmit(board_comm_uart, board_comm_tx_buf, frame_len,
                        BOARD_COMM_TIMEOUT_MS) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  return BOARD_COMM_OK;
}

BoardComm_Status BoardComm_SendRaw(const uint8_t *frame, uint16_t len, uint32_t timeout)
{
  if ((board_comm_uart == 0) || (frame == 0) || (len == 0U))
  {
    return BOARD_COMM_ERROR;
  }
  if (HAL_UART_Transmit(board_comm_uart, (uint8_t *)frame, len, timeout) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }
  return BOARD_COMM_OK;
}

BoardComm_Status BoardComm_Ping(void)
{
  uint32_t cookie = HAL_GetTick();
  uint8_t payload[4];
  uint16_t seq = board_comm_tx_seq++;

  payload[0] = (uint8_t)(cookie & 0xFFUL);
  payload[1] = (uint8_t)((cookie >> 8) & 0xFFUL);
  payload[2] = (uint8_t)((cookie >> 16) & 0xFFUL);
  payload[3] = (uint8_t)((cookie >> 24) & 0xFFUL);
  return BoardComm_SendV2(BOARD_COMM_NODE_BLACK, BOARD_COMM_CMD_PING,
                          BOARD_COMM_FLAG_ACK_REQ, seq, payload, sizeof(payload));
}

BoardComm_State BoardComm_GetState(void)
{
  BoardComm_State state;

  __disable_irq();
  state.last_cmd = board_comm_last_cmd;
  state.last_len = board_comm_last_len;
  state.last_src = board_comm_last_src;
  state.last_dst = board_comm_last_dst;
  state.last_flags = board_comm_last_flags;
  state.last_seq = board_comm_last_seq;
  for (uint8_t i = 0; i < BOARD_COMM_MAX_PAYLOAD; i++)
  {
    state.last_data[i] = board_comm_last_data[i];
  }
  state.last_status = board_comm_last_status;
  state.last_rx_size = board_comm_last_rx_size;
  state.uart_error_code = board_comm_uart_error_code;
  state.rx_count = board_comm_rx_count;
  state.error_count = board_comm_error_count;
  __enable_irq();

  return state;
}

uint8_t BoardComm_TakeKeypadKey(char *key)
{
  uint8_t has_key;

  if (key == 0)
  {
    return 0;
  }

  __disable_irq();
  has_key = board_comm_key_pending;
  if (has_key != 0U)
  {
    *key = (char)board_comm_key_value;
    board_comm_key_pending = 0;
  }
  __enable_irq();

  return has_key;
}

const char *BoardComm_StatusText(BoardComm_Status status)
{
  switch (status)
  {
    case BOARD_COMM_OK: return "OK";
    case BOARD_COMM_ERROR: return "ERR";
    case BOARD_COMM_TIMEOUT: return "TIMEOUT";
    case BOARD_COMM_LENGTH_ERROR: return "LEN ERR";
    case BOARD_COMM_CHECKSUM_ERROR: return "SUM ERR";
    default: return "UNKNOWN";
  }
}
