#include "BluetoothLink_User.h"

#include "lcd.h"

#define BT_UART_BAUD_RATE             115200U
#define BT_STATE_GPIO_PORT            GPIOC
#define BT_STATE_GPIO_PIN             GPIO_PIN_13
#define BT_STATE_DEBOUNCE_MS          30U
#define BT_STATE_SETTLE_MS            200U
#define BT_HANDSHAKE_TIMEOUT_MS        1000U
#define BT_CONTINUOUS_SEND_INTERVAL_MS 500U
#define BT_CONTINUOUS_TEST_MODE        0U
#define BT_RX_DMA_BUF_LEN              256U

#define BT_FRAME_HEAD1                0xD3U
#define BT_FRAME_HEAD2                0x91U
#define BT_FRAME_TAIL1                0x91U
#define BT_FRAME_TAIL2                0xD3U
#define BT_PROTOCOL_VERSION           0x01U
#define BT_NODE_BLUE                  0x02U
#define BT_NODE_RED                   0x03U
#define BT_FLAG_ACK_REQ               0x01U
#define BT_FLAG_RESPONSE              0x02U
#define BT_CMD_HELLO                  0x50U
#define BT_CMD_ACK                    0x7FU
#define BT_MAX_PAYLOAD                128U
#define BT_FRAME_OVERHEAD             15U
#define BT_HELLO_VALIDATION_MASK      0x001FU

static UART_HandleTypeDef bt_uart;
static DMA_HandleTypeDef bt_rx_dma;
static BluetoothLink_State bt_state;
static GPIO_PinState bt_raw_state;
static GPIO_PinState bt_stable_state;
static uint32_t bt_raw_change_tick;
static uint32_t bt_state_high_tick;
static uint32_t bt_handshake_start_tick;
static uint32_t bt_last_send_tick;
static uint8_t bt_handshake_pending;
static uint16_t bt_handshake_seq;
static uint16_t bt_next_seq;
static uint8_t bt_rx_frame[BT_MAX_PAYLOAD + BT_FRAME_OVERHEAD];
static uint16_t bt_rx_count;
static uint16_t bt_rx_expected;
__attribute__((aligned(32))) static uint8_t bt_rx_dma_buf[BT_RX_DMA_BUF_LEN];
static uint16_t bt_rx_dma_read_pos;

static uint16_t BluetoothLink_Crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;

  for (uint16_t i = 0U; i < length; i++)
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

static void BluetoothLink_ShowState(BluetoothLink_State state)
{
  lcd_clear(WHITE);
  lcd_show_string(56U, 82U, 380U, 32U, 32U, "BLUETOOTH", BLACK);

  if (state == BLUETOOTH_LINK_CONNECTED)
  {
    lcd_show_string(64U, 150U, 380U, 32U, 32U, "CONNECTED", GREEN);
    lcd_show_string(56U, 208U, 400U, 16U, 16U,
                    "STATE AND PROTOCOL CHECK PASSED", GREEN);
  }
  else if (state == BLUETOOTH_LINK_VERIFYING)
  {
    lcd_show_string(64U, 150U, 380U, 32U, 32U, "VERIFYING...", BLACK);
  }
  else if (state == BLUETOOTH_LINK_PROTOCOL_ERROR)
  {
    lcd_show_string(48U, 150U, 410U, 32U, 32U, "LINK CHECK FAILED", RED);
    lcd_show_string(44U, 208U, 420U, 16U, 16U,
                    "STATE HIGH, PROTOCOL INVALID", RED);
  }
  else
  {
    lcd_show_string(48U, 150U, 410U, 32U, 32U, "NOT CONNECTED", RED);
    lcd_show_string(66U, 208U, 380U, 16U, 16U,
                    "BLUETOOTH STATE IS LOW", RED);
  }
}

static void BluetoothLink_SetState(BluetoothLink_State state)
{
  if (bt_state != state)
  {
    bt_state = state;
    BluetoothLink_ShowState(state);
  }
}

static void BluetoothLink_HardwareInit(void)
{
  GPIO_InitTypeDef gpio = {0};
  RCC_PeriphCLKInitTypeDef clock = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_UART4_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  clock.PeriphClockSelection = RCC_PERIPHCLK_UART4;
  clock.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;
  if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }

  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF8_UART4;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = BT_STATE_GPIO_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BT_STATE_GPIO_PORT, &gpio);

  bt_uart.Instance = UART4;
  bt_uart.Init.BaudRate = BT_UART_BAUD_RATE;
  bt_uart.Init.WordLength = UART_WORDLENGTH_8B;
  bt_uart.Init.StopBits = UART_STOPBITS_1;
  bt_uart.Init.Parity = UART_PARITY_NONE;
  bt_uart.Init.Mode = UART_MODE_TX_RX;
  bt_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  bt_uart.Init.OverSampling = UART_OVERSAMPLING_16;
  bt_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  bt_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  bt_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&bt_uart) != HAL_OK)
  {
    Error_Handler();
  }
  (void)HAL_UARTEx_DisableFifoMode(&bt_uart);

  bt_rx_dma.Instance = DMA1_Stream3;
  bt_rx_dma.Init.Request = DMA_REQUEST_UART4_RX;
  bt_rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  bt_rx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  bt_rx_dma.Init.MemInc = DMA_MINC_ENABLE;
  bt_rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  bt_rx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
  bt_rx_dma.Init.Mode = DMA_CIRCULAR;
  bt_rx_dma.Init.Priority = DMA_PRIORITY_HIGH;
  bt_rx_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&bt_rx_dma) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&bt_uart, hdmarx, bt_rx_dma);
  SCB_CleanInvalidateDCache_by_Addr((uint32_t *)bt_rx_dma_buf, BT_RX_DMA_BUF_LEN);
  if (HAL_UART_Receive_DMA(&bt_uart, bt_rx_dma_buf, BT_RX_DMA_BUF_LEN) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_DMA_DISABLE_IT(&bt_rx_dma, DMA_IT_HT);
  __HAL_DMA_DISABLE_IT(&bt_rx_dma, DMA_IT_TC);
  bt_rx_dma_read_pos = 0U;
}

static HAL_StatusTypeDef BluetoothLink_SendFrame(uint8_t command, uint8_t flags,
                                                  uint16_t sequence,
                                                  const uint8_t *payload,
                                                  uint16_t length)
{
  uint8_t frame[BT_MAX_PAYLOAD + BT_FRAME_OVERHEAD];
  uint16_t crc;
  uint16_t frame_length;

  if ((length > BT_MAX_PAYLOAD) || ((length != 0U) && (payload == 0)))
  {
    return HAL_ERROR;
  }

  frame[0] = BT_FRAME_HEAD1;
  frame[1] = BT_FRAME_HEAD2;
  frame[2] = BT_PROTOCOL_VERSION;
  frame[3] = BT_NODE_RED;
  frame[4] = BT_NODE_BLUE;
  frame[5] = command;
  frame[6] = (uint8_t)(flags & 0x1FU);
  frame[7] = (uint8_t)sequence;
  frame[8] = (uint8_t)(sequence >> 8);
  frame[9] = (uint8_t)length;
  frame[10] = (uint8_t)(length >> 8);
  for (uint16_t i = 0U; i < length; i++)
  {
    frame[11U + i] = payload[i];
  }
  crc = BluetoothLink_Crc16(&frame[2], (uint16_t)(9U + length));
  frame[11U + length] = (uint8_t)crc;
  frame[12U + length] = (uint8_t)(crc >> 8);
  frame[13U + length] = BT_FRAME_TAIL1;
  frame[14U + length] = BT_FRAME_TAIL2;
  frame_length = (uint16_t)(length + BT_FRAME_OVERHEAD);

  return HAL_UART_Transmit(&bt_uart, frame, frame_length, 100U);
}

static void BluetoothLink_StartHandshake(void)
{
  uint8_t hello[8] = {BT_NODE_BLUE, BT_PROTOCOL_VERSION, 0x80U, 0x00U,
                      0x00U, 0x00U, 0x00U, 0x00U};

  bt_handshake_seq = bt_next_seq++;
#if (BT_CONTINUOUS_TEST_MODE == 0U)
  bt_rx_count = 0U;
  bt_rx_expected = 0U;
#endif
  bt_handshake_start_tick = HAL_GetTick();
  if (bt_state != BLUETOOTH_LINK_CONNECTED)
  {
    BluetoothLink_SetState(BLUETOOTH_LINK_VERIFYING);
  }
  if (BluetoothLink_SendFrame(BT_CMD_HELLO, BT_FLAG_ACK_REQ,
                              bt_handshake_seq, hello, sizeof(hello)) != HAL_OK)
  {
    BluetoothLink_SetState(BLUETOOTH_LINK_PROTOCOL_ERROR);
  }
}

static uint8_t BluetoothLink_ValidateAck(const uint8_t *frame, uint16_t size)
{
  uint16_t length;
  uint16_t sequence;
  uint16_t received_crc;
  uint16_t detail;

  if ((frame == 0) || (size < BT_FRAME_OVERHEAD)) return 0U;
  length = (uint16_t)frame[9] | ((uint16_t)frame[10] << 8);
  if (size != (uint16_t)(length + BT_FRAME_OVERHEAD)) return 0U;
  if ((frame[0] != BT_FRAME_HEAD1) || (frame[1] != BT_FRAME_HEAD2) ||
      (frame[2] != BT_PROTOCOL_VERSION) || (frame[3] != BT_NODE_BLUE) ||
      (frame[4] != BT_NODE_RED) || (frame[5] != BT_CMD_ACK) ||
      ((frame[6] & BT_FLAG_RESPONSE) == 0U) ||
      (frame[13U + length] != BT_FRAME_TAIL1) ||
      (frame[14U + length] != BT_FRAME_TAIL2)) return 0U;
  sequence = (uint16_t)frame[7] | ((uint16_t)frame[8] << 8);
  if ((sequence != bt_handshake_seq) || (length != 4U)) return 0U;
  received_crc = (uint16_t)frame[11U + length] |
                 ((uint16_t)frame[12U + length] << 8);
  if (received_crc != BluetoothLink_Crc16(&frame[2], (uint16_t)(9U + length))) return 0U;
  detail = (uint16_t)frame[13] | ((uint16_t)frame[14] << 8);
  return ((frame[11] == BT_CMD_HELLO) && (frame[12] == 0U) &&
          (detail == BT_HELLO_VALIDATION_MASK)) ? 1U : 0U;
}

static void BluetoothLink_ParseByte(uint8_t byte)
{
  if (bt_rx_count == 0U)
  {
    if (byte == BT_FRAME_HEAD1) bt_rx_frame[bt_rx_count++] = byte;
    return;
  }
  if (bt_rx_count == 1U)
  {
    if (byte == BT_FRAME_HEAD2) bt_rx_frame[bt_rx_count++] = byte;
    else bt_rx_count = (byte == BT_FRAME_HEAD1) ? 1U : 0U;
    return;
  }
  if (bt_rx_count >= sizeof(bt_rx_frame))
  {
    bt_rx_count = 0U;
    bt_rx_expected = 0U;
    return;
  }
  bt_rx_frame[bt_rx_count++] = byte;
  if (bt_rx_count == 11U)
  {
    uint16_t length = (uint16_t)bt_rx_frame[9] | ((uint16_t)bt_rx_frame[10] << 8);
    if (length > BT_MAX_PAYLOAD)
    {
      bt_rx_count = 0U;
      return;
    }
    bt_rx_expected = (uint16_t)(length + BT_FRAME_OVERHEAD);
  }
  if ((bt_rx_expected != 0U) && (bt_rx_count == bt_rx_expected))
  {
    if (BluetoothLink_ValidateAck(bt_rx_frame, bt_rx_count) != 0U)
    {
      BluetoothLink_SetState(BLUETOOTH_LINK_CONNECTED);
    }
    bt_rx_count = 0U;
    bt_rx_expected = 0U;
  }
}

void BluetoothLink_Init(void)
{
  BluetoothLink_HardwareInit();
  bt_state = BLUETOOTH_LINK_DISCONNECTED;
  bt_stable_state = GPIO_PIN_RESET;
  bt_raw_state = HAL_GPIO_ReadPin(BT_STATE_GPIO_PORT, BT_STATE_GPIO_PIN);
  bt_raw_change_tick = HAL_GetTick();
  bt_state_high_tick = 0U;
  bt_handshake_pending = 0U;
  bt_next_seq = 1U;
  bt_last_send_tick = HAL_GetTick() - BT_CONTINUOUS_SEND_INTERVAL_MS;
  bt_rx_count = 0U;
  bt_rx_expected = 0U;
  BluetoothLink_ShowState(BLUETOOTH_LINK_DISCONNECTED);
}

void BluetoothLink_Task(void)
{
  GPIO_PinState sample = HAL_GPIO_ReadPin(BT_STATE_GPIO_PORT, BT_STATE_GPIO_PIN);
  uint16_t write_pos;

  if (sample != bt_raw_state)
  {
    bt_raw_state = sample;
    bt_raw_change_tick = HAL_GetTick();
  }
  if ((bt_raw_state != bt_stable_state) &&
      ((HAL_GetTick() - bt_raw_change_tick) >= BT_STATE_DEBOUNCE_MS))
  {
    bt_stable_state = bt_raw_state;
#if (BT_CONTINUOUS_TEST_MODE == 0U)
    if (bt_stable_state == GPIO_PIN_SET)
    {
      bt_state_high_tick = HAL_GetTick();
      bt_handshake_pending = 1U;
      BluetoothLink_SetState(BLUETOOTH_LINK_VERIFYING);
    }
    else
    {
      bt_handshake_pending = 0U;
      BluetoothLink_SetState(BLUETOOTH_LINK_DISCONNECTED);
    }
#endif
  }

#if (BT_CONTINUOUS_TEST_MODE == 0U)
  if ((bt_handshake_pending != 0U) &&
      (bt_stable_state == GPIO_PIN_SET) &&
      (bt_raw_state == GPIO_PIN_SET) &&
      ((HAL_GetTick() - bt_state_high_tick) >= BT_STATE_SETTLE_MS))
  {
    bt_handshake_pending = 0U;
    BluetoothLink_StartHandshake();
  }
#endif

  SCB_InvalidateDCache_by_Addr((uint32_t *)bt_rx_dma_buf, BT_RX_DMA_BUF_LEN);
  write_pos = (uint16_t)(BT_RX_DMA_BUF_LEN - __HAL_DMA_GET_COUNTER(&bt_rx_dma));
  while (bt_rx_dma_read_pos != write_pos)
  {
    BluetoothLink_ParseByte(bt_rx_dma_buf[bt_rx_dma_read_pos]);
    bt_rx_dma_read_pos++;
    if (bt_rx_dma_read_pos >= BT_RX_DMA_BUF_LEN)
    {
      bt_rx_dma_read_pos = 0U;
    }
  }

#if (BT_CONTINUOUS_TEST_MODE != 0U)
  if ((HAL_GetTick() - bt_last_send_tick) >= BT_CONTINUOUS_SEND_INTERVAL_MS)
  {
    bt_last_send_tick = HAL_GetTick();
    BluetoothLink_StartHandshake();
  }
#endif

  if ((bt_state == BLUETOOTH_LINK_VERIFYING) &&
      (bt_handshake_pending == 0U) &&
      ((HAL_GetTick() - bt_handshake_start_tick) >= BT_HANDSHAKE_TIMEOUT_MS))
  {
    BluetoothLink_SetState(BLUETOOTH_LINK_PROTOCOL_ERROR);
  }
}

BluetoothLink_State BluetoothLink_GetState(void)
{
  return bt_state;
}
