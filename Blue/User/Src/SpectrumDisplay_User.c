#include "SpectrumDisplay_User.h"

#include <stdio.h>
#include <string.h>
#include "AGC_DAC_User.h"
#include "BoardComm_User.h"
#include "LogDetector_User.h"
#include "lcd.h"
#include "led.h"

#define SPECTRUM_DISPLAY_LINE_COUNT 7U
#define SPECTRUM_DISPLAY_LINE_LEN   40U

static SpectrumDisplayState display_state;
static uint16_t display_points_mv[SPECTRUM_DISPLAY_POINT_COUNT];
static uint32_t display_last_rx_count = 0;
static uint32_t display_last_refresh_tick = 0;
static uint16_t display_last_rx_size = 0;
static uint32_t display_uart_error_code = 0;
static uint32_t display_last_adc_tick = 0;
static LogDetectorSample display_detector_sample = {0};
static uint8_t display_force_refresh = 1;
static uint8_t display_refresh_requested = 0;
static uint8_t display_graph_dirty = 1;
static char display_last_lines[SPECTRUM_DISPLAY_LINE_COUNT][SPECTRUM_DISPLAY_LINE_LEN];

static uint16_t Spectrum_ReadU16(const uint8_t *buf, uint8_t *pos)
{
  uint16_t value = (uint16_t)buf[*pos];
  value |= (uint16_t)buf[(uint8_t)(*pos + 1U)] << 8;
  *pos = (uint8_t)(*pos + 2U);
  return value;
}

static uint32_t Spectrum_ReadU32(const uint8_t *buf, uint8_t *pos)
{
  uint32_t value = (uint32_t)buf[*pos];
  value |= (uint32_t)buf[(uint8_t)(*pos + 1U)] << 8;
  value |= (uint32_t)buf[(uint8_t)(*pos + 2U)] << 16;
  value |= (uint32_t)buf[(uint8_t)(*pos + 3U)] << 24;
  *pos = (uint8_t)(*pos + 4U);
  return value;
}

static const char *Spectrum_ModeText(uint8_t mode)
{
  switch (mode)
  {
    case 0U: return "ANALYZER";
    case 1U: return "PLL-SWEEP";
    case 2U: return "PLL-FIXED";
    case 3U: return "PLL-LOCK";
    default: return "UNKNOWN";
  }
}

static const char *Spectrum_StateText(uint8_t state)
{
  switch (state)
  {
    case 0U: return "IDLE";
    case 1U: return "RUN";
    case 2U: return "DONE";
    default: return "UNK";
  }
}

static char Spectrum_PrintableKey(uint8_t key)
{
  return ((key >= 0x20U) && (key <= 0x7EU)) ? (char)key : '-';
}

static const char *Spectrum_UiFieldText(uint8_t focus)
{
  switch (focus)
  {
    case 0U: return "TIME";
    case 1U: return "OUT";
    case 2U: return "FREQ";
    default: return "?";
  }
}

static void Spectrum_ParseStatus(const uint8_t *data, uint8_t len)
{
  uint8_t pos = 0;
  uint8_t previous_mode = display_state.mode;

  if ((data == 0) || (len < 20U))
  {
    return;
  }

  display_state.mode = data[pos++];
  display_state.state = data[pos++];
  display_state.rf_khz = Spectrum_ReadU32(data, &pos);
  display_state.lo_khz = Spectrum_ReadU32(data, &pos);
  display_state.point_index = Spectrum_ReadU16(data, &pos);
  display_state.amplitude_mv = Spectrum_ReadU16(data, &pos);
  display_state.peak_index = Spectrum_ReadU16(data, &pos);
  display_state.peak_amplitude_mv = Spectrum_ReadU16(data, &pos);
  display_state.spur_count = data[pos++];
  display_state.pll_locked = data[pos++];

  if (display_state.mode != previous_mode)
  {
    display_graph_dirty = 1U;
  }

  if (len >= 32U)
  {
    display_state.sweep_time_s = data[pos++];
    display_state.active_point_count = Spectrum_ReadU16(data, &pos);
    display_state.fixed_frequency_khz = Spectrum_ReadU32(data, &pos);
    display_state.agc_target_mv = Spectrum_ReadU16(data, &pos);
    display_state.agc_control_mv = Spectrum_ReadU16(data, &pos);
    display_state.agc_output_connected = data[pos++];
  }

  if (len >= 43U)
  {
    display_state.ui_focus = data[pos++];
    display_state.ui_editing = data[pos++];
    display_state.ui_input_len = data[pos++];
    if (display_state.ui_input_len > SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN)
    {
      display_state.ui_input_len = SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN;
    }
    for (uint8_t i = 0U; i < SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN; i++)
    {
      display_state.ui_input[i] = (char)data[pos++];
    }
    display_state.ui_input[display_state.ui_input_len] = '\0';
  }

  if (len >= 45U)
  {
    display_state.ui_sweep_time_ms = Spectrum_ReadU16(data, &pos);
  }
}

static void Spectrum_ParsePoint(const uint8_t *data, uint8_t len)
{
  uint8_t pos = 0;
  uint16_t index;

  if ((data == 0) || (len < 9U))
  {
    return;
  }

  index = Spectrum_ReadU16(data, &pos);
  display_state.rf_khz = Spectrum_ReadU32(data, &pos);
  display_state.amplitude_mv = Spectrum_ReadU16(data, &pos);
  display_state.pll_locked = data[pos++];
  display_state.point_index = index;
  display_state.state = 1U;

  if ((display_state.mode == 0U) && (index < SPECTRUM_DISPLAY_POINT_COUNT))
  {
    display_points_mv[index] = display_state.amplitude_mv;
    display_graph_dirty = 1;
    if (display_state.amplitude_mv > display_state.peak_amplitude_mv)
    {
      display_state.peak_amplitude_mv = display_state.amplitude_mv;
      display_state.peak_index = index;
    }
  }
}

static void Spectrum_ParseResult(const uint8_t *data, uint8_t len)
{
  uint8_t pos = 0;

  if ((data == 0) || (len < 5U))
  {
    return;
  }

  display_state.peak_index = Spectrum_ReadU16(data, &pos);
  display_state.peak_amplitude_mv = Spectrum_ReadU16(data, &pos);
  display_state.spur_count = data[pos++];
  display_state.state = 2U;
  display_graph_dirty = 1;
}

static uint32_t Spectrum_IndexToRfKHz(uint16_t index)
{
  if (display_state.mode != 0U)
  {
    return SPECTRUM_DISPLAY_PLL_START_KHZ + ((uint32_t)index * SPECTRUM_DISPLAY_STEP_KHZ);
  }
  return SPECTRUM_DISPLAY_RF_START_KHZ + ((uint32_t)index * SPECTRUM_DISPLAY_STEP_KHZ);
}

static void Spectrum_DrawBars(void)
{
  uint16_t graph_x = 10;
  uint16_t graph_y = 180;
  uint16_t graph_w = 300;
  uint16_t graph_h = 90;
  uint16_t max_mv = display_state.peak_amplitude_mv;
  uint16_t point_count = display_state.active_point_count;

  if (display_state.mode != 0U)
  {
    lcd_fill(graph_x, graph_y, graph_x + graph_w, graph_y + graph_h, WHITE);
    return;
  }

  if ((point_count == 0U) || (point_count > SPECTRUM_DISPLAY_POINT_COUNT))
  {
    point_count = SPECTRUM_DISPLAY_POINT_COUNT;
  }

  if (max_mv == 0U)
  {
    max_mv = 1U;
  }

  lcd_fill(graph_x, graph_y, graph_x + graph_w, graph_y + graph_h, WHITE);
  lcd_draw_rectangle(graph_x, graph_y, graph_x + graph_w, graph_y + graph_h, BLUE);

  for (uint16_t x = 0; x < graph_w; x++)
  {
    uint16_t index = (uint16_t)((uint32_t)x * point_count / graph_w);
    uint16_t value = display_points_mv[index];
    uint16_t bar_h = (uint16_t)((uint32_t)value * graph_h / max_mv);

    if (bar_h > graph_h)
    {
      bar_h = graph_h;
    }

    lcd_draw_line((uint16_t)(graph_x + x), (uint16_t)(graph_y + graph_h),
                  (uint16_t)(graph_x + x), (uint16_t)(graph_y + graph_h - bar_h), GREEN);
  }
}

static void Spectrum_ShowLine(uint8_t line_index, uint16_t y, const char *text)
{
  if ((line_index >= SPECTRUM_DISPLAY_LINE_COUNT) || (text == 0))
  {
    return;
  }

  if ((display_force_refresh != 0U) ||
      (strncmp(display_last_lines[line_index], text, SPECTRUM_DISPLAY_LINE_LEN) != 0))
  {
    lcd_fill(10, y, 319, (uint16_t)(y + 15U), WHITE);
    lcd_show_string(10, y, 300, 16, 16, (char *)text, BLACK);
    strncpy(display_last_lines[line_index], text, SPECTRUM_DISPLAY_LINE_LEN - 1U);
    display_last_lines[line_index][SPECTRUM_DISPLAY_LINE_LEN - 1U] = '\0';
  }
}

static void Spectrum_RefreshLcd(void)
{
  char line[40];
  uint32_t peak_rf_khz = Spectrum_IndexToRfKHz(display_state.peak_index);
  uint16_t sweep_ms = display_state.ui_sweep_time_ms;

  if (sweep_ms == 0U)
  {
    sweep_ms = (uint16_t)display_state.sweep_time_s * 1000U;
  }

  if (display_force_refresh != 0U)
  {
    lcd_show_string(10, 10, 300, 24, 24, "RF CONTROL", RED);
  }

  sprintf(line, "%s %s T:%u.%03us", Spectrum_ModeText(display_state.mode),
          Spectrum_StateText(display_state.state), sweep_ms / 1000U, sweep_ms % 1000U);
  Spectrum_ShowLine(0, 45, line);

  sprintf(line, "RF:%lu.%03luMHz", display_state.rf_khz / 1000UL, display_state.rf_khz % 1000UL);
  Spectrum_ShowLine(1, 65, line);

  sprintf(line, "LO:%lu.%03luMHz LOCK:%u", display_state.lo_khz / 1000UL, display_state.lo_khz % 1000UL, display_state.pll_locked);
  Spectrum_ShowLine(2, 85, line);

  if (display_state.mode == 3U)
  {
    sprintf(line, "PERIOD:0.5s STEP:%u/2", display_state.point_index + 1U);
  }
  else
  {
    sprintf(line, "P:%u/%u ADC:%umV", display_state.point_index + 1U,
            display_state.active_point_count, display_detector_sample.mv);
  }
  Spectrum_ShowLine(3, 105, line);

  if (display_state.mode == 0U)
  {
    sprintf(line, "PEAK:%lu.%03luMHz %umV", peak_rf_khz / 1000UL,
            peak_rf_khz % 1000UL, display_state.peak_amplitude_mv);
  }
  else if (display_state.ui_focus == 0U)
  {
    sprintf(line, "BOX:%s %u.%03us", Spectrum_UiFieldText(display_state.ui_focus),
            sweep_ms / 1000U, sweep_ms % 1000U);
  }
  else if (display_state.ui_focus == 1U)
  {
    sprintf(line, "BOX:%s %umV", Spectrum_UiFieldText(display_state.ui_focus),
            display_state.agc_target_mv);
  }
  else
  {
    sprintf(line, "BOX:%s %lu.%03luMHz", Spectrum_UiFieldText(display_state.ui_focus),
            display_state.fixed_frequency_khz / 1000UL, display_state.fixed_frequency_khz % 1000UL);
  }
  Spectrum_ShowLine(4, 125, line);

  if (display_state.ui_editing != 0U)
  {
    sprintf(line, "EDIT:%s  D=OK", display_state.ui_input);
  }
  else
  {
    sprintf(line, "SEL:%s 2468 D=EDIT", Spectrum_UiFieldText(display_state.ui_focus));
  }
  Spectrum_ShowLine(5, 145, line);

  sprintf(line, "KEY:%c SPUR:%u DBM:%d.%d", display_state.last_key, display_state.spur_count,
          display_detector_sample.dbm_x10 / 10,
          (display_detector_sample.dbm_x10 < 0) ? (int)(-display_detector_sample.dbm_x10 % 10)
                                                : (int)(display_detector_sample.dbm_x10 % 10));
  Spectrum_ShowLine(6, 165, line);

  if ((display_force_refresh != 0U) || (display_graph_dirty != 0U))
  {
    Spectrum_DrawBars();
    display_graph_dirty = 0;
  }

  display_force_refresh = 0;
  display_refresh_requested = 0;
}

void SpectrumDisplay_Init(void)
{
  for (uint16_t i = 0; i < SPECTRUM_DISPLAY_POINT_COUNT; i++)
  {
    display_points_mv[i] = 0;
  }

  display_state.mode = 1U;
  display_state.state = 0U;
  display_state.rf_khz = SPECTRUM_DISPLAY_RF_START_KHZ;
  display_state.lo_khz = SPECTRUM_DISPLAY_RF_START_KHZ + SPECTRUM_DISPLAY_IF_KHZ;
  display_state.point_index = 0;
  display_state.amplitude_mv = 0;
  display_state.peak_index = 0;
  display_state.peak_amplitude_mv = 0;
  display_state.spur_count = 0;
  display_state.pll_locked = 0;
  display_state.sweep_time_s = 3U;
  display_state.active_point_count = SPECTRUM_DISPLAY_POINT_COUNT;
  display_state.fixed_frequency_khz = 100000UL;
  display_state.agc_target_mv = 50U;
  display_state.agc_control_mv = 500U;
  display_state.agc_output_connected = 0U;
  display_state.ui_focus = 0U;
  display_state.ui_editing = 0U;
  display_state.ui_input_len = 0U;
  display_state.ui_input[0] = '\0';
  display_state.ui_sweep_time_ms = 3000U;
  display_state.last_key = '-';
  display_state.last_key_ascii = 0;
  display_state.rx_count = 0;
  display_state.error_count = 0;
  display_force_refresh = 1;
  display_refresh_requested = 0;
  display_graph_dirty = 1;
  memset(display_last_lines, 0, sizeof(display_last_lines));

  lcd_clear(WHITE);
  Spectrum_RefreshLcd();
}

void SpectrumDisplay_Task(void)
{
  BoardComm_State comm_state = BoardComm_GetState();
  uint32_t now = HAL_GetTick();
  char received_key;

  display_state.rx_count = comm_state.rx_count;
  display_state.error_count = comm_state.error_count;
  display_last_rx_size = comm_state.last_rx_size;
  display_uart_error_code = comm_state.uart_error_code;

  if ((now - display_last_adc_tick) >= 100U)
  {
    display_last_adc_tick = now;
    display_detector_sample = LogDetector_ReadAverage(LOG_DETECTOR_DEFAULT_AVG_COUNT);
    if (display_detector_sample.valid != 0U)
    {
      display_state.amplitude_mv = display_detector_sample.mv;
    }
  }

  if (BoardComm_TakeKeypadKey(&received_key) != 0U)
  {
    display_state.last_key_ascii = (uint8_t)received_key;
    display_state.last_key = Spectrum_PrintableKey((uint8_t)received_key);
    display_refresh_requested = 1U;
  }

  if (comm_state.rx_count != display_last_rx_count)
  {
    display_last_rx_count = comm_state.rx_count;

    if ((comm_state.last_status == BOARD_COMM_OK) && (comm_state.last_cmd == BOARD_COMM_CMD_SYS_STATUS))
    {
      Spectrum_ParseStatus(comm_state.last_data, comm_state.last_len);
      display_refresh_requested = 1U;
    }
    else if ((comm_state.last_status == BOARD_COMM_OK) && (comm_state.last_cmd == BOARD_COMM_CMD_SWEEP_POINT))
    {
      Spectrum_ParsePoint(comm_state.last_data, comm_state.last_len);
    }
    else if ((comm_state.last_status == BOARD_COMM_OK) && (comm_state.last_cmd == BOARD_COMM_CMD_SWEEP_RESULT))
    {
      Spectrum_ParseResult(comm_state.last_data, comm_state.last_len);
    }
    else if ((comm_state.last_status == BOARD_COMM_OK) &&
             (comm_state.last_cmd == BOARD_COMM_CMD_KEYPAD) &&
             (comm_state.last_len == 1U))
    {
      display_state.last_key_ascii = comm_state.last_data[0];
      display_state.last_key = Spectrum_PrintableKey(comm_state.last_data[0]);
    }
  }

  if ((display_force_refresh != 0U) || (display_refresh_requested != 0U) ||
      ((now - display_last_refresh_tick) >= SPECTRUM_DISPLAY_REFRESH_MS))
  {
    display_last_refresh_tick = now;
    Spectrum_RefreshLcd();
#if (SPECTRUM_DISPLAY_LED_HEARTBEAT != 0U)
    LED0_TOGGLE();
#endif
  }
}

SpectrumDisplayState SpectrumDisplay_GetState(void)
{
  return display_state;
}
