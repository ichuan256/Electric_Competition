#include "Keypad_User.h"

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} Keypad_Pin;

static const Keypad_Pin keypad_rows[4] = {
  {KEY_ROW0_GPIO_Port, KEY_ROW0_Pin},
  {KEY_ROW1_GPIO_Port, KEY_ROW1_Pin},
  {KEY_ROW2_GPIO_Port, KEY_ROW2_Pin},
  {KEY_ROW3_GPIO_Port, KEY_ROW3_Pin}
};

static const Keypad_Pin keypad_cols[4] = {
  {KEY_COL0_GPIO_Port, KEY_COL0_Pin},
  {KEY_COL1_GPIO_Port, KEY_COL1_Pin},
  {KEY_COL2_GPIO_Port, KEY_COL2_Pin},
  {KEY_COL3_GPIO_Port, KEY_COL3_Pin}
};

static const char keypad_map[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

static Keypad_State keypad_state = KEYPAD_IDLE;
static uint32_t keypad_last_scan_tick = 0;
static uint8_t keypad_stable_count = 0;
static char keypad_candidate_key = KEYPAD_NO_KEY;
static char keypad_current_key = KEYPAD_NO_KEY;
static char keypad_event_key = KEYPAD_NO_KEY;

char Keypad_Debug_CurrentKey = KEYPAD_NO_KEY;
char Keypad_Debug_EventKey = KEYPAD_NO_KEY;
uint8_t Keypad_Debug_RawRow = 0xFFU;
uint8_t Keypad_Debug_RawCol = 0xFFU;
uint8_t Keypad_Debug_DetectedCount = 0U;

static void Keypad_SetAllRows(GPIO_PinState state)
{
  for (uint8_t row = 0; row < 4U; row++)
  {
    HAL_GPIO_WritePin(keypad_rows[row].port, keypad_rows[row].pin, state);
  }
}

static char Keypad_ReadRawKey(void)
{
  char detected_key = KEYPAD_NO_KEY;
  uint8_t detected_count = 0;

  Keypad_Debug_RawRow = 0xFFU;
  Keypad_Debug_RawCol = 0xFFU;
  Keypad_Debug_DetectedCount = 0U;

  Keypad_SetAllRows(GPIO_PIN_SET);

  for (uint8_t row = 0; row < 4U; row++)
  {
    HAL_GPIO_WritePin(keypad_rows[row].port, keypad_rows[row].pin, GPIO_PIN_RESET);

    /*
     * 行线刚切换后�?GPIO 输入一点点稳定时间�?     * 这里只用几个 NOP，不会像 HAL_Delay() 那样阻塞主循环�?     */
    __NOP();
    __NOP();
    __NOP();

    for (uint8_t col = 0; col < 4U; col++)
    {
      if (HAL_GPIO_ReadPin(keypad_cols[col].port, keypad_cols[col].pin) == GPIO_PIN_RESET)
      {
        detected_key = keypad_map[row][col];
        detected_count++;
        Keypad_Debug_RawRow = row;
        Keypad_Debug_RawCol = col;
      }
    }

    HAL_GPIO_WritePin(keypad_rows[row].port, keypad_rows[row].pin, GPIO_PIN_SET);
  }

  /*
   * 如果同时检测到多个按键，先忽略本次结果�?   * 这样可以避免普通矩阵键盘未加二极管时出现鬼键误判�?   */
  if (detected_count != 1U)
  {
    Keypad_Debug_DetectedCount = detected_count;
    return KEYPAD_NO_KEY;
  }

  Keypad_Debug_DetectedCount = detected_count;
  return detected_key;
}

void Keypad_Init(void)
{
  Keypad_SetAllRows(GPIO_PIN_SET);

  keypad_state = KEYPAD_IDLE;
  keypad_last_scan_tick = HAL_GetTick();
  keypad_stable_count = 0;
  keypad_candidate_key = KEYPAD_NO_KEY;
  keypad_current_key = KEYPAD_NO_KEY;
  keypad_event_key = KEYPAD_NO_KEY;
  Keypad_Debug_CurrentKey = KEYPAD_NO_KEY;
  Keypad_Debug_EventKey = KEYPAD_NO_KEY;
  Keypad_Debug_RawRow = 0xFFU;
  Keypad_Debug_RawCol = 0xFFU;
  Keypad_Debug_DetectedCount = 0U;
}

void Keypad_ScanTask(void)
{
  uint32_t now = HAL_GetTick();
  char raw_key;

  if ((now - keypad_last_scan_tick) < KEYPAD_SCAN_PERIOD_MS)
  {
    return;
  }

  keypad_last_scan_tick = now;
  raw_key = Keypad_ReadRawKey();

  switch (keypad_state)
  {
    case KEYPAD_IDLE:
      if (raw_key != KEYPAD_NO_KEY)
      {
        keypad_candidate_key = raw_key;
        keypad_stable_count = 1U;
        keypad_state = KEYPAD_DEBOUNCE_PRESS;
      }
      break;

    case KEYPAD_DEBOUNCE_PRESS:
      if (raw_key == keypad_candidate_key)
      {
        keypad_stable_count++;
        if (keypad_stable_count >= KEYPAD_DEBOUNCE_TICKS)
        {
          keypad_current_key = keypad_candidate_key;
          keypad_event_key = keypad_candidate_key;
          Keypad_Debug_CurrentKey = keypad_current_key;
          Keypad_Debug_EventKey = keypad_event_key;
          Keypad_EventCallback(keypad_event_key);
          keypad_state = KEYPAD_PRESSED;
        }
      }
      else
      {
        keypad_stable_count = 0;
        keypad_candidate_key = KEYPAD_NO_KEY;
        keypad_state = KEYPAD_IDLE;
      }
      break;

    case KEYPAD_PRESSED:
      if (raw_key == KEYPAD_NO_KEY)
      {
        keypad_stable_count = 1U;
        keypad_state = KEYPAD_DEBOUNCE_RELEASE;
      }
      break;

    case KEYPAD_DEBOUNCE_RELEASE:
      if (raw_key == KEYPAD_NO_KEY)
      {
        keypad_stable_count++;
        if (keypad_stable_count >= KEYPAD_DEBOUNCE_TICKS)
        {
          keypad_current_key = KEYPAD_NO_KEY;
          keypad_candidate_key = KEYPAD_NO_KEY;
          Keypad_Debug_CurrentKey = KEYPAD_NO_KEY;
          keypad_state = KEYPAD_IDLE;
        }
      }
      else
      {
        keypad_stable_count = 0;
        keypad_state = KEYPAD_PRESSED;
      }
      break;

    default:
      keypad_state = KEYPAD_IDLE;
      keypad_stable_count = 0;
      keypad_candidate_key = KEYPAD_NO_KEY;
      keypad_current_key = KEYPAD_NO_KEY;
      Keypad_Debug_CurrentKey = KEYPAD_NO_KEY;
      break;
  }
}

void Keypad_DelayWithScan(uint32_t delay_ms)
{
  uint32_t start = HAL_GetTick();

  /*
   * 用于替代原来�?HAL_Delay()�?   * 这样保持原有等待时间不变，同时等待期间仍然持续运行键盘扫描和防抖�?   */
  while ((HAL_GetTick() - start) < delay_ms)
  {
    Keypad_ScanTask();
  }
}

uint8_t Keypad_GetKey(char *key)
{
  if ((key == 0) || (keypad_event_key == KEYPAD_NO_KEY))
  {
    return 0U;
  }

  *key = keypad_event_key;
  keypad_event_key = KEYPAD_NO_KEY;
  Keypad_Debug_EventKey = KEYPAD_NO_KEY;
  return 1U;
}

char Keypad_GetCurrentKey(void)
{
  return keypad_current_key;
}

/*
 * �����¼��ص���
 * Ĭ��Ϊ����������ȷ��һ���°��������һ�Ρ�
 */
__weak void Keypad_EventCallback(char key)
{
  (void)key;
}
