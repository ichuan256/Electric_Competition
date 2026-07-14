#include "BoardComm_User.h"
#include "AdcFftClient_User.h"

/*
 * 本模块当前使用的 UART 句柄。
 * 默认绑定 USART3，也就是 CubeMX 生成的 huart3。
 */
static UART_HandleTypeDef *board_comm_uart = &huart3;

/*
 * USART3 空闲中断接收缓冲区。
 *
 * 为什么必须是 static 全局数组：
 *   HAL_UARTEx_ReceiveToIdle_IT() 启动后，HAL 会在中断中持续向该缓冲区写入。
 *   如果使用局部变量，函数退出后内存失效，接收会出错。
 */
static uint8_t board_comm_rx_buf[BOARD_COMM_RX_BUF_SIZE];

#define BOARD_COMM_ADC_FFT_HEADER_LEN     9U
#define BOARD_COMM_ADC_FFT_OVERHEAD       13U
#define BOARD_COMM_ADC_FFT_MAX_FRAME_LEN  49U

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

/*
 * 计算协议校验字节。
 *
 * 校验规则：
 *   CHECKSUM = CMD ^ LEN ^ DATA[0] ^ DATA[1] ^ ...
 *
 * 这种异或校验计算简单，方便用串口助手手工验证。若后续通信环境干扰较强，
 * 可以把这里升级为 CRC8 或 CRC16，但收发双方必须同时修改。
 */
static uint8_t BoardComm_Checksum(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint8_t checksum = cmd ^ len;

  for (uint8_t i = 0; i < len; i++)
  {
    checksum ^= data[i];
  }

  return checksum;
}

/*
 * 解析一帧原始字节。
 *
 * 输入：
 *   frame - 原始接收缓冲区，格式应为 A5 5A CMD LEN DATA... CHECKSUM。
 *   size  - 本次空闲中断实际收到的字节数。
 *
 * 输出：
 *   cmd  - 命令字。
 *   data - 数据区指针，直接指向 frame 内部。
 *   len  - 数据区长度。
 *
 * 返回：
 *   BOARD_COMM_OK              帧合法。
 *   BOARD_COMM_ERROR           帧头错误或参数错误。
 *   BOARD_COMM_LENGTH_ERROR    长度错误、半包或粘包。
 *   BOARD_COMM_CHECKSUM_ERROR  校验错误。
 */
static BoardComm_Status BoardComm_ParseFrame(const uint8_t *frame, uint16_t size,
                                             uint8_t *cmd, const uint8_t **data, uint8_t *len)
{
  uint8_t checksum;

  if ((frame == 0) || (cmd == 0) || (data == 0) || (len == 0))
  {
    return BOARD_COMM_ERROR;
  }

  /* 最短帧为：A5 5A CMD LEN CHECKSUM，共 5 字节。 */
  if (size < 5U)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  /* 校验帧头，确认这确实是一帧板间通信数据。 */
  if ((frame[0] != BOARD_COMM_HEAD1) || (frame[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_ERROR;
  }

  /* LEN 不能超过协议允许的最大数据区长度。 */
  if (frame[3] > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  /*
   * 本协议要求一次空闲中断收到的字节数刚好等于 LEN + 5。
   * 如果不相等，通常说明出现了半包、粘包或对方协议格式不一致。
   */
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

static BoardComm_FrameProbeResult BoardComm_ProbeStandardFrame(const uint8_t *data,
                                                                uint16_t available,
                                                                uint16_t *frame_size)
{
  if ((available < 2U) || (data[0] != BOARD_COMM_HEAD1) || (data[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_FRAME_NO_MATCH;
  }
  if (available < 4U)
  {
    return BOARD_COMM_FRAME_INCOMPLETE;
  }
  if (data[3] > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_FRAME_INVALID;
  }

  *frame_size = (uint16_t)data[3] + 5U;
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

static BoardComm_FrameProbeResult BoardComm_ProbeAdcFftFrame(const uint8_t *data,
                                                              uint16_t available,
                                                              uint16_t *frame_size)
{
  uint16_t payload_len;

  if (AdcFftClient_IsFrameStart(data, available) == 0U)
  {
    return BOARD_COMM_FRAME_NO_MATCH;
  }
  if (available < BOARD_COMM_ADC_FFT_HEADER_LEN)
  {
    return BOARD_COMM_FRAME_INCOMPLETE;
  }

  payload_len = (uint16_t)data[7] | ((uint16_t)data[8] << 8);
  *frame_size = (uint16_t)(BOARD_COMM_ADC_FFT_OVERHEAD + payload_len);
  if (*frame_size > BOARD_COMM_ADC_FFT_MAX_FRAME_LEN)
  {
    return BOARD_COMM_FRAME_INVALID;
  }
  return (*frame_size <= available) ? BOARD_COMM_FRAME_READY : BOARD_COMM_FRAME_INCOMPLETE;
}

static void BoardComm_DispatchAdcFftFrame(const uint8_t *frame, uint16_t frame_size)
{
  (void)AdcFftClient_HandleRxBuffer(frame, frame_size);
}

static const BoardComm_FrameDecoder board_comm_frame_decoders[] = {
  {BoardComm_ProbeStandardFrame, BoardComm_DispatchStandardFrame},
  {BoardComm_ProbeAdcFftFrame, BoardComm_DispatchAdcFftFrame}
};

/*
 * 初始化板间通信模块。
 * 这里只绑定 UART 句柄，不重复初始化 USART3 硬件。
 * USART3 的 GPIO、时钟、波特率由 MX_USART3_UART_Init() 完成。
 */
void BoardComm_Init(void)
{
  board_comm_uart = &huart3;
}

/*
 * 启动接收到空闲中断。
 *
 * 调用后，HAL 会开始接收 USART3 数据。出现以下情况之一会触发
 * HAL_UARTEx_RxEventCallback()：
 *   1. 缓冲区接收满。
 *   2. 收到若干字节后，串口线路进入 IDLE 空闲状态。
 */
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

/*
 * 停止当前中断接收。
 * 一般不用主动调用，除非要临时关闭板间通信或切换到其他接收方式。
 */
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

/*
 * 空闲中断接收事件处理。
 *
 * 这个函数会在 HAL_UARTEx_RxEventCallback() 中被调用。
 * 它负责：
 *   1. 判断是不是 USART3 的接收事件。
 *   2. 解析接收缓冲区中的协议帧。
 *   3. 调用用户回调 BoardComm_RxFrameCallback()。
 *   4. 重新启动下一次空闲中断接收。
 */
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

  /*
   * ReceiveToIdle 每触发一次事件，本轮接收就结束。
   * 必须重新启动一次，才能继续接收下一帧数据。
   */
  (void)BoardComm_StartReceiveToIdleIT();
}

/*
 * HAL 接收到空闲事件回调。
 *
 * 因为当前工程 USE_HAL_UART_REGISTER_CALLBACKS = 0，所以不能动态注册回调，
 * 需要直接实现 HAL 的弱回调函数。HAL_UART_IRQHandler() 处理到 IDLE 事件后，
 * 会进入这里。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  BoardComm_HandleRxIdleEvent(huart, Size);
}

/*
 * 用户接收回调的默认实现。
 *
 * 这是一个弱函数，默认什么都不做。后续需要处理接收到的数据时，在其他
 * 用户 .c 文件中写一个同名函数即可覆盖它。
 *
 * 中断注意事项：
 *   该函数在中断上下文中执行，不要 HAL_Delay，不要 printf，不要做长时间运算。
 */
__weak void BoardComm_RxFrameCallback(uint8_t cmd, const uint8_t *data, uint8_t len, BoardComm_Status status)
{
  (void)cmd;
  (void)data;
  (void)len;
  (void)status;
}

/*
 * 阻塞式发送一帧数据。
 *
 * 帧格式：
 *   A5 5A CMD LEN DATA... CHECKSUM
 */
BoardComm_Status BoardComm_Send(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  uint8_t frame[BOARD_COMM_RX_BUF_SIZE];

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

/*
 * 阻塞式接收一帧数据。
 *
 * 该函数会一直等待指定字节数，直到接收完成或 timeout 超时。
 * 现在已经加入空闲中断接收，因此这个函数主要用于早期调试或临时测试。
 */
BoardComm_Status BoardComm_Receive(uint8_t *cmd, uint8_t *data, uint8_t *len, uint32_t timeout)
{
  uint8_t header[4];
  uint8_t checksum;

  if ((cmd == 0) || (data == 0) || (len == 0))
  {
    return BOARD_COMM_ERROR;
  }

  if (HAL_UART_Receive(board_comm_uart, header, sizeof(header), timeout) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  if ((header[0] != BOARD_COMM_HEAD1) || (header[1] != BOARD_COMM_HEAD2))
  {
    return BOARD_COMM_ERROR;
  }

  if (header[3] > BOARD_COMM_MAX_PAYLOAD)
  {
    return BOARD_COMM_LENGTH_ERROR;
  }

  *cmd = header[2];
  *len = header[3];

  if (*len != 0U)
  {
    if (HAL_UART_Receive(board_comm_uart, data, *len, timeout) != HAL_OK)
    {
      return BOARD_COMM_TIMEOUT;
    }
  }

  if (HAL_UART_Receive(board_comm_uart, &checksum, 1, timeout) != HAL_OK)
  {
    return BOARD_COMM_TIMEOUT;
  }

  if (checksum != BoardComm_Checksum(*cmd, data, *len))
  {
    return BOARD_COMM_CHECKSUM_ERROR;
  }

  return BOARD_COMM_OK;
}

/*
 * 发送 PING 命令。
 * 当前约定 0x01 表示 PING，用来检查板间串口是否连通。
 */
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
  return BoardComm_Send(0x01, 0, 0);
}
