#include "BoardComm_User.h"
#include "LogDetector_User.h"

static UART_HandleTypeDef *board_comm_uart = &huart1;
static uint8_t board_comm_rx_buf[BOARD_COMM_RX_BUF_SIZE];

#define BOARD_COMM_QUEUE_DEPTH 8U

typedef struct {
  uint8_t cmd;
  uint8_t len;
  BoardComm_Status status;
  uint8_t data[BOARD_COMM_MAX_PAYLOAD];
} BoardComm_QueuedFrame;

static BoardComm_QueuedFrame board_comm_queue[BOARD_COMM_QUEUE_DEPTH];
static volatile uint8_t board_comm_queue_head = 0;
static volatile uint8_t board_comm_queue_tail = 0;
static volatile uint8_t board_comm_queue_count = 0;
static volatile uint8_t board_comm_last_cmd = 0;
static volatile uint8_t board_comm_last_len = 0;
static volatile BoardComm_Status board_comm_last_status = BOARD_COMM_OK;
static uint8_t board_comm_last_data[BOARD_COMM_MAX_PAYLOAD];
static volatile uint32_t board_comm_rx_count = 0;
static volatile uint32_t board_comm_error_count = 0;
static volatile uint16_t board_comm_last_rx_size = 0;
static volatile uint32_t board_comm_uart_error_code = 0;
static volatile uint8_t board_comm_key_pending = 0;
static volatile char board_comm_key_value = 0;

static uint8_t BoardComm_Checksum(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint8_t checksum = cmd ^ len;

  for (uint8_t i = 0; i < len; i++)
  {
    checksum ^= data[i];
  }

  return checksum;
}

static uint16_t BoardComm_ReadU16(const uint8_t *buf, uint8_t *pos)
{
  uint16_t value = (uint16_t)buf[*pos];
  value |= (uint16_t)buf[(uint8_t)(*pos + 1U)] << 8;
  *pos = (uint8_t)(*pos + 2U);
  return value;
}

static void BoardComm_WriteU16(uint8_t *buf, uint8_t *pos, uint16_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFU);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static BoardComm_Status BoardComm_ParseFrame(const uint8_t *frame, uint16_t size,
                                             uint8_t *cmd, const uint8_t **data, uint8_t *len)
{
  uint8_t checksum;

  if ((frame == 0) || (cmd == 0) || (data == 0) || (len == 0))
  {
    return BOARD_COMM_ERROR;
  }

  if (size < 5U)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  if ((frame[0] != BOARD_COMM_HEAD1) || (frame[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_ERROR;
  }

  if (frame[3] > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  if (size != ((uint16_t)frame[3] + 5U))
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  *cmd = frame[2];
  *len = frame[3];
  *data = &frame[4];

  checksum = BoardComm_Checksum(*cmd, *data, *len);
  if (checksum != frame[4U + *len])
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
  board_comm_queue[queue_index].status = status;
  for (uint8_t i = 0; i < len; i++)
  {
    board_comm_queue[queue_index].data[i] = data[i];
  }
  board_comm_queue_head = (uint8_t)((board_comm_queue_head + 1U) % BOARD_COMM_QUEUE_DEPTH);
  board_comm_queue_count++;
}

void BoardComm_Init(void)
{
  board_comm_uart = &huart1;
  board_comm_queue_head = 0U;
  board_comm_queue_tail = 0U;
  board_comm_queue_count = 0U;
  board_comm_last_cmd = 0U;
  board_comm_last_len = 0U;
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
  uint8_t cmd = 0;
  uint8_t len = 0;
  const uint8_t *data = 0;
  BoardComm_Status status;
  uint16_t offset = 0;
  uint16_t remain;
  uint16_t frame_size;

  if ((huart == 0) || (huart != board_comm_uart))
  {
    return;
  }

  board_comm_last_rx_size = size;

  while (offset < size)
  {
    cmd = 0;
    len = 0;
    data = 0;
    remain = (uint16_t)(size - offset);

    /* Receive-to-idle may return several back-to-back frames in one callback.
       Parse from the current frame header instead of treating Size as one frame. */
    if (remain < 5U)
    {
      BoardComm_StoreRxFrame(0, 0, 0, BOARD_COMM_LENGTH_ERROR);
      break;
    }

    if ((board_comm_rx_buf[offset] != BOARD_COMM_HEAD1) ||
        (board_comm_rx_buf[offset + 1U] != BOARD_COMM_HEAD2))
    {
      BoardComm_StoreRxFrame(0, 0, 0, BOARD_COMM_ERROR);
      break;
    }

    if (board_comm_rx_buf[offset + 3U] > BOARD_COMM_MAX_PAYLOAD)
    {
      BoardComm_StoreRxFrame(board_comm_rx_buf[offset + 2U], 0, 0, BOARD_COMM_LENGTH_ERROR);
      break;
    }

    frame_size = (uint16_t)board_comm_rx_buf[offset + 3U] + 5U;
    if (frame_size > remain)
    {
      BoardComm_StoreRxFrame(board_comm_rx_buf[offset + 2U], 0, 0, BOARD_COMM_LENGTH_ERROR);
      break;
    }

    status = BoardComm_ParseFrame(&board_comm_rx_buf[offset], frame_size, &cmd, &data, &len);
    BoardComm_StoreRxFrame(cmd, data, len, status);
    offset = (uint16_t)(offset + frame_size);
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
    static const uint8_t pong[] = { 'P', 'O', 'N', 'G' };
    (void)BoardComm_Send(BOARD_COMM_CMD_PONG, pong, (uint8_t)sizeof(pong));
  }
  else if ((status == BOARD_COMM_OK) && (cmd == BOARD_COMM_CMD_KEYPAD))
  {
    /* MergeBlack sends keypad events as CMD=0x10, LEN=1, DATA[0]=ASCII key. */
  }
  else if ((status == BOARD_COMM_OK) && (cmd == BOARD_COMM_CMD_ADC_SAMPLE_REQ))
  {
    uint8_t pos = 0;
    uint8_t resp_pos = 0;
    uint8_t response[7];
    uint16_t point_index = 0;
    LogDetectorSample sample;

    if (len >= 10U)
    {
      point_index = BoardComm_ReadU16(data_copy, &pos);
    }

    sample = LogDetector_ReadAverage(LOG_DETECTOR_DEFAULT_AVG_COUNT);
    BoardComm_WriteU16(response, &resp_pos, point_index);
    BoardComm_WriteU16(response, &resp_pos, sample.mv);
    BoardComm_WriteU16(response, &resp_pos, (uint16_t)sample.dbm_x10);
    response[resp_pos++] = sample.valid;

    (void)BoardComm_Send(BOARD_COMM_CMD_ADC_SAMPLE_RESP, response, resp_pos);
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
}

BoardComm_Status BoardComm_Send(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint8_t frame[BOARD_COMM_TX_BUF_SIZE];

  if (len > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  if ((len != 0U) && (data == 0))
  {
    return BOARD_COMM_ERROR;
  }

  frame[0] = BOARD_COMM_HEAD1;
  frame[1] = BOARD_COMM_HEAD2;
  frame[2] = cmd;
  frame[3] = len;

  for (uint8_t i = 0; i < len; i++)
  {
    frame[4 + i] = data[i];
  }

  frame[4 + len] = BoardComm_Checksum(cmd, data, len);

  if (HAL_UART_Transmit(board_comm_uart, frame, (uint16_t)(len + 5U), BOARD_COMM_TIMEOUT_MS) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  return BOARD_COMM_OK;
}

BoardComm_Status BoardComm_Ping(void)
{
  return BoardComm_Send(BOARD_COMM_CMD_PING, 0, 0);
}

BoardComm_State BoardComm_GetState(void)
{
  BoardComm_State state;

  __disable_irq();
  state.last_cmd = board_comm_last_cmd;
  state.last_len = board_comm_last_len;
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
