#include "BoardComm_User.h"

/* Black 与 Blue 的板间通信固定使用 USART3，并且只解析统一 V2 帧。 */

static UART_HandleTypeDef *board_comm_uart = &huart3;
static uint16_t board_comm_tx_seq = 1U;
static uint8_t board_comm_tx_buf[BOARD_COMM_RX_BUF_SIZE];


static uint8_t board_comm_rx_buf[BOARD_COMM_RX_BUF_SIZE];

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
  if ((frame[3] != BOARD_COMM_NODE_BLACK) &&
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

  received_crc = (uint16_t)frame[11U + payload_len] |
                 ((uint16_t)frame[12U + payload_len] << 8);
  if (received_crc != BoardComm_Crc16(&frame[2], (uint16_t)(9U + payload_len)))
  {
    return BOARD_COMM_CHECKSUM_ERROR;
  }

  return BOARD_COMM_OK;
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

  BoardComm_RxFrameCallback(cmd, data, len, status);
}

static const BoardComm_FrameDecoder board_comm_frame_decoders[] = {
  {BoardComm_ProbeStandardFrame, BoardComm_DispatchStandardFrame}
};


void BoardComm_Init(void)
{
  board_comm_uart = &huart3;
  board_comm_tx_seq = 1U;
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
  uint16_t offset = 0U;

  if ((huart == 0) || (huart != board_comm_uart))
  {
    return;
  }

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
        BoardComm_RxFrameCallback(0U, 0, 0U,
                                  (result == BOARD_COMM_FRAME_INCOMPLETE) ?
                                  BOARD_COMM_LENGTH_ERROR : BOARD_COMM_ERROR);
        offset = size;
      }
      break;
    }

    if (matched == 0U)
    {
      BoardComm_RxFrameCallback(0U, 0, 0U, BOARD_COMM_ERROR);
      offset++;
    }
  }

  /* ReceiveToIdle 每次回调后都要重新启动。 */
  (void)BoardComm_StartReceiveToIdleIT();
}


void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  BoardComm_HandleRxIdleEvent(huart, Size);
}


__weak void BoardComm_RxFrameCallback(uint8_t cmd, const uint8_t *data, uint8_t len, BoardComm_Status status)
{
  (void)cmd;
  (void)data;
  (void)len;
  (void)status;
}


BoardComm_Status BoardComm_Send(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint16_t seq = board_comm_tx_seq++;
  return BoardComm_SendV2(BOARD_COMM_NODE_BLUE, cmd, 0U, seq, data, len);
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
  board_comm_tx_buf[4] = BOARD_COMM_NODE_BLACK;
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


BoardComm_Status BoardComm_Receive(uint8_t *cmd, uint8_t *data, uint8_t *len, uint32_t timeout)
{
  uint8_t header[11];
  uint8_t trailer[4];
  uint16_t payload_len;
  uint16_t crc;

  if ((cmd == 0) || (data == 0) || (len == 0))
  {
    return BOARD_COMM_ERROR;
  }

  if (HAL_UART_Receive(board_comm_uart, header, sizeof(header), timeout) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  if ((header[0] != BOARD_COMM_HEAD1) || (header[1] != BOARD_COMM_HEAD2) ||
      (header[2] != BOARD_COMM_VERSION) ||
      ((header[3] != BOARD_COMM_NODE_BLACK) &&
       (header[3] != BOARD_COMM_NODE_BROADCAST)))
  {
    return BOARD_COMM_ERROR;
  }

  payload_len = (uint16_t)header[9] | ((uint16_t)header[10] << 8);
  if (payload_len > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  *cmd = header[5];
  *len = (uint8_t)payload_len;

  if (*len != 0U)
  {
    if (HAL_UART_Receive(board_comm_uart, data, *len, timeout) != HAL_OK)
    {
      return BOARD_COMM_TIMEOUT;
    }
  }

  if (HAL_UART_Receive(board_comm_uart, trailer, sizeof(trailer), timeout) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  for (uint8_t i = 0U; i < 9U; i++)
  {
    board_comm_tx_buf[i] = header[i + 2U];
  }
  for (uint8_t i = 0U; i < *len; i++)
  {
    board_comm_tx_buf[9U + i] = data[i];
  }
  crc = BoardComm_Crc16(board_comm_tx_buf, (uint16_t)(9U + *len));
  if ((trailer[0] != (uint8_t)(crc & 0xFFU)) ||
      (trailer[1] != (uint8_t)(crc >> 8)) ||
      (trailer[2] != BOARD_COMM_TAIL1) ||
      (trailer[3] != BOARD_COMM_TAIL2))
  {
    return BOARD_COMM_CHECKSUM_ERROR;
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
  return BoardComm_SendV2(BOARD_COMM_NODE_BLUE, BOARD_COMM_CMD_PING,
                          BOARD_COMM_FLAG_ACK_REQ, seq, payload, sizeof(payload));
}
