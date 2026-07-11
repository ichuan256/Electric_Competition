#include "SpectrumDisplay_User.h"

#include <stdio.h>
#include <string.h>
#include "AGC_DAC_User.h"
#include "BoardComm_User.h"
#include "FpgaUart_User.h"
#include "LogDetector_User.h"
#include "lcd.h"
#include "led.h"

#define SPECTRUM_DISPLAY_LINE_COUNT 7U
#define SPECTRUM_DISPLAY_LINE_LEN   40U
#define SPECTRUM_DISPLAY_TEXT_X     10U
#define SPECTRUM_DISPLAY_TEXT_W     300U
#define SPECTRUM_DISPLAY_LINE_H     16U
#define SPECTRUM_DISPLAY_UI_BOX_Y   120U
#define SPECTRUM_DISPLAY_UI_BOX_W   94U
#define SPECTRUM_DISPLAY_UI_BOX_H   42U
#define SPECTRUM_DISPLAY_INFO_Y     170U
#define SPECTRUM_DISPLAY_INFO_GAP   18U
#define SPECTRUM_DISPLAY_GRAPH_Y    226U
#define SPECTRUM_DISPLAY_GRAPH_H    80U
#define SPECTRUM_DISPLAY_FPGA_BOX_X 330U
#define SPECTRUM_DISPLAY_FPGA_BOX_Y 42U
#define SPECTRUM_DISPLAY_FPGA_BOX_W 145U
#define SPECTRUM_DISPLAY_FPGA_BOX_H 222U
#define SPECTRUM_DISPLAY_FPGA_FIRST_FIELD 3U
#define SPECTRUM_DISPLAY_FPGA_FIELD_COUNT 7U

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

static int16_t Spectrum_ReadI16(const uint8_t *buf, uint8_t *pos)
{
  return (int16_t)Spectrum_ReadU16(buf, pos);
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
    case 3U: return "FFREQ";
    case 4U: return "PHASE";
    case 5U: return "AMP";
    case 6U: return "OFFS";
    case 7U: return "DUTY";
    case 8U: return "WAVE";
    case 9U: return "EN";
    default: return "?";
  }
}

static uint8_t Spectrum_DisplayFieldEditable(uint8_t mode, uint8_t field)
{
  if (field >= SPECTRUM_DISPLAY_FPGA_FIRST_FIELD)
  {
    return 1U;
  }

  if (mode == 0U)
  {
    return (field == 0U) ? 1U : 0U;
  }
  if (mode == 1U)
  {
    return ((field == 0U) || (field == 1U)) ? 1U : 0U;
  }
  if (mode == 2U)
  {
    return ((field == 1U) || (field == 2U)) ? 1U : 0U;
  }

  return 0U;
}

static void Spectrum_FormatFieldValue(uint8_t field, char *text, uint8_t text_len)
{
  uint16_t sweep_ms = display_state.ui_sweep_time_ms;

  if ((text == 0) || (text_len == 0U))
  {
    return;
  }

  if (sweep_ms == 0U)
  {
    sweep_ms = (uint16_t)display_state.sweep_time_s * 1000U;
  }

  if (field == 0U)
  {
    snprintf(text, text_len, "%u.%03us", sweep_ms / 1000U, sweep_ms % 1000U);
  }
  else if (field == 1U)
  {
    snprintf(text, text_len, "%umV", display_state.agc_target_mv);
  }
  else
  {
    snprintf(text, text_len, "%lu.%03luM",
             display_state.fixed_frequency_khz / 1000UL,
             display_state.fixed_frequency_khz % 1000UL);
  }
}

static void Spectrum_FormatEditText(char *text, uint8_t text_len)
{
  uint8_t len;

  if ((text == 0) || (text_len == 0U))
  {
    return;
  }

  len = display_state.ui_input_len;
  if (len > (uint8_t)(text_len - 1U))
  {
    len = (uint8_t)(text_len - 1U);
  }

  memcpy(text, display_state.ui_input, len);
  if (len < (uint8_t)(text_len - 1U))
  {
    text[len++] = '|';
  }
  text[len] = '\0';
}

static void Spectrum_FormatFpgaFieldValue(uint8_t field, char *text, uint8_t text_len)
{
  if ((text == 0) || (text_len == 0U))
  {
    return;
  }

  if (field == 3U)
  {
    snprintf(text, text_len, "%lu.%03luM",
             display_state.fpga_frequency_hz / 1000000UL,
             (display_state.fpga_frequency_hz / 1000UL) % 1000UL);
  }
  else if (field == 4U)
  {
    snprintf(text, text_len, "%u", display_state.fpga_phase_deg);
  }
  else if (field == 5U)
  {
    snprintf(text, text_len, "%u", display_state.fpga_amplitude_code);
  }
  else if (field == 6U)
  {
    snprintf(text, text_len, "%d", display_state.fpga_offset_code);
  }
  else if (field == 7U)
  {
    uint32_t duty_x10 = ((uint32_t)display_state.fpga_duty_code * 1000UL + 32767UL) / 65535UL;
    snprintf(text, text_len, "%lu.%lu%%", duty_x10 / 10UL, duty_x10 % 10UL);
  }
  else if (field == 8U)
  {
    snprintf(text, text_len, "%u", display_state.fpga_waveform);
  }
  else if (field == 9U)
  {
    snprintf(text, text_len, "%u", display_state.fpga_output_enable);
  }
  else
  {
    snprintf(text, text_len, "-");
  }
}

static void Spectrum_ApplyFpgaSettings(void)
{
  FpgaUart_SetSignal(display_state.fpga_frequency_hz,
                     display_state.fpga_phase_deg,
                     display_state.fpga_amplitude_code,
                     display_state.fpga_offset_code,
                     display_state.fpga_duty_code,
                     display_state.fpga_waveform,
                     display_state.fpga_output_enable);
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

  if (len >= 57U)
  {
    display_state.fpga_frequency_hz = Spectrum_ReadU32(data, &pos);
    display_state.fpga_phase_deg = Spectrum_ReadU16(data, &pos);
    display_state.fpga_amplitude_code = Spectrum_ReadU16(data, &pos);
    display_state.fpga_offset_code = Spectrum_ReadI16(data, &pos);
    display_state.fpga_duty_code = Spectrum_ReadU16(data, &pos);
    display_state.fpga_waveform = data[pos++];
    display_state.fpga_output_enable = data[pos++];
    Spectrum_ApplyFpgaSettings();
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
  uint16_t graph_x = SPECTRUM_DISPLAY_TEXT_X;
  uint16_t graph_y = SPECTRUM_DISPLAY_GRAPH_Y;
  uint16_t graph_w = 300;
  uint16_t graph_h = SPECTRUM_DISPLAY_GRAPH_H;
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
    lcd_fill(SPECTRUM_DISPLAY_TEXT_X, y, 319, (uint16_t)(y + SPECTRUM_DISPLAY_LINE_H - 1U), WHITE);
    lcd_show_string(SPECTRUM_DISPLAY_TEXT_X, y, SPECTRUM_DISPLAY_TEXT_W,
                    SPECTRUM_DISPLAY_LINE_H, SPECTRUM_DISPLAY_LINE_H, (char *)text, BLACK);
    strncpy(display_last_lines[line_index], text, SPECTRUM_DISPLAY_LINE_LEN - 1U);
    display_last_lines[line_index][SPECTRUM_DISPLAY_LINE_LEN - 1U] = '\0';
  }
}

static void Spectrum_DrawUiBoxes(void)
{
  const uint16_t box_y = SPECTRUM_DISPLAY_UI_BOX_Y;
  const uint16_t box_w = SPECTRUM_DISPLAY_UI_BOX_W;
  const uint16_t box_h = SPECTRUM_DISPLAY_UI_BOX_H;
  const uint16_t box_x[3] = {10U, 113U, 216U};
  char value[16];

  lcd_fill(10, box_y, 319, (uint16_t)(box_y + box_h), WHITE);

  for (uint8_t field = 0U; field < 3U; field++)
  {
    uint8_t editable = Spectrum_DisplayFieldEditable(display_state.mode, field);
    uint8_t selected = ((display_state.ui_focus == field) && (editable != 0U)) ? 1U : 0U;
    uint16_t color = (editable != 0U) ? BLACK : GRAY;
    uint16_t border = (editable != 0U) ? LGRAYBLUE : LGRAY;

    if (selected != 0U)
    {
      border = (display_state.ui_editing != 0U) ? RED : BLUE;
    }

    lcd_draw_rectangle(box_x[field], box_y,
                       (uint16_t)(box_x[field] + box_w),
                       (uint16_t)(box_y + box_h), border);
    if (selected != 0U)
    {
      lcd_draw_rectangle((uint16_t)(box_x[field] + 1U), (uint16_t)(box_y + 1U),
                         (uint16_t)(box_x[field] + box_w - 1U),
                         (uint16_t)(box_y + box_h - 1U), border);
    }

    lcd_show_string((uint16_t)(box_x[field] + 6U), (uint16_t)(box_y + 6U),
                    (uint16_t)(box_w - 12U), 16, 16,
                    (char *)Spectrum_UiFieldText(field), color);
    if ((selected != 0U) && (display_state.ui_editing != 0U))
    {
      Spectrum_FormatEditText(value, sizeof(value));
    }
    else
    {
      Spectrum_FormatFieldValue(field, value, sizeof(value));
    }
    lcd_show_string((uint16_t)(box_x[field] + 6U), (uint16_t)(box_y + 24U),
                    (uint16_t)(box_w - 12U), 16, 16, value, color);
  }
}

static void Spectrum_DrawFpgaBox(void)
{
  const uint16_t box_x = SPECTRUM_DISPLAY_FPGA_BOX_X;
  const uint16_t box_y = SPECTRUM_DISPLAY_FPGA_BOX_Y;
  const uint16_t box_w = SPECTRUM_DISPLAY_FPGA_BOX_W;
  const uint16_t box_h = SPECTRUM_DISPLAY_FPGA_BOX_H;
  char value[16];

  lcd_fill(box_x, box_y, (uint16_t)(box_x + box_w), (uint16_t)(box_y + box_h), WHITE);
  lcd_draw_rectangle(box_x, box_y, (uint16_t)(box_x + box_w), (uint16_t)(box_y + box_h), BLUE);
  lcd_show_string((uint16_t)(box_x + 6U), (uint16_t)(box_y + 6U),
                  (uint16_t)(box_w - 12U), 16, 16, "FPGA UART", RED);

  for (uint8_t i = 0U; i < SPECTRUM_DISPLAY_FPGA_FIELD_COUNT; i++)
  {
    uint8_t field = (uint8_t)(SPECTRUM_DISPLAY_FPGA_FIRST_FIELD + i);
    uint16_t row_y = (uint16_t)(box_y + 28U + ((uint16_t)i * 26U));
    uint8_t selected = (display_state.ui_focus == field) ? 1U : 0U;
    uint16_t border = (selected != 0U) ?
                      ((display_state.ui_editing != 0U) ? RED : BLUE) : LGRAY;

    lcd_draw_rectangle((uint16_t)(box_x + 4U), row_y,
                       (uint16_t)(box_x + box_w - 4U), (uint16_t)(row_y + 23U), border);
    lcd_show_string((uint16_t)(box_x + 8U), (uint16_t)(row_y + 4U),
                    52U, 16U, 16U, (char *)Spectrum_UiFieldText(field), BLACK);

    if ((selected != 0U) && (display_state.ui_editing != 0U))
    {
      Spectrum_FormatEditText(value, sizeof(value));
    }
    else
    {
      Spectrum_FormatFpgaFieldValue(field, value, sizeof(value));
    }

    lcd_show_string((uint16_t)(box_x + 62U), (uint16_t)(row_y + 4U),
                    72U, 16U, 16U, value, BLACK);
  }
}

static void Spectrum_RefreshLcd(void)
{
  char line[40];
  FpgaUartState fpga_state = FpgaUart_GetState();
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

  sprintf(line, "MODE:%-9s STATE:%-4s", Spectrum_ModeText(display_state.mode),
          Spectrum_StateText(display_state.state));
  Spectrum_ShowLine(0, 42, line);

  sprintf(line, "RF  :%3lu.%03luMHz  LO:%3lu.%03luMHz",
          display_state.rf_khz / 1000UL, display_state.rf_khz % 1000UL,
          display_state.lo_khz / 1000UL, display_state.lo_khz % 1000UL);
  Spectrum_ShowLine(1, 62, line);

  sprintf(line, "LOCK:%u ADC:%4umV DBM:%d.%d", display_state.pll_locked,
          display_detector_sample.mv,
          display_detector_sample.dbm_x10 / 10,
          (display_detector_sample.dbm_x10 < 0) ? (int)(-display_detector_sample.dbm_x10 % 10)
                                                : (int)(display_detector_sample.dbm_x10 % 10));
  Spectrum_ShowLine(2, 82, line);

  if (display_state.mode == 3U)
  {
    sprintf(line, "STEP:%3u/2   PERIOD:0.5s", display_state.point_index + 1U);
  }
  else
  {
    sprintf(line, "POINT:%3u/%3u SPUR:%3u KEY:%c", display_state.point_index + 1U,
            display_state.active_point_count, display_state.spur_count, display_state.last_key);
  }
  Spectrum_ShowLine(3, 102, line);

  if (display_state.mode == 0U)
  {
    sprintf(line, "PEAK:%3lu.%03luMHz %4umV", peak_rf_khz / 1000UL,
            peak_rf_khz % 1000UL, display_state.peak_amplitude_mv);
    Spectrum_ShowLine(4, SPECTRUM_DISPLAY_INFO_Y, line);
  }
  else if (display_state.ui_editing != 0U)
  {
    char edit_text[12];
    Spectrum_FormatEditText(edit_text, sizeof(edit_text));
    sprintf(line, "EDIT:%-9s         D=OK", edit_text);
    Spectrum_ShowLine(4, SPECTRUM_DISPLAY_INFO_Y, line);
  }
  else if ((display_state.mode == 3U) && (display_state.ui_focus < SPECTRUM_DISPLAY_FPGA_FIRST_FIELD))
  {
    sprintf(line, "LOCK DEMO: no editable value");
    Spectrum_ShowLine(4, SPECTRUM_DISPLAY_INFO_Y, line);
  }
  else
  {
    sprintf(line, "SEL:%-4s  2/4< >6/8  D=EDIT",
            Spectrum_UiFieldText(display_state.ui_focus));
    Spectrum_ShowLine(4, SPECTRUM_DISPLAY_INFO_Y, line);
  }

  sprintf(line, "RX:%u ERR:%u SZ:%u", (unsigned int)display_state.rx_count,
          (unsigned int)display_state.error_count, display_last_rx_size);
  Spectrum_ShowLine(5, SPECTRUM_DISPLAY_INFO_Y + SPECTRUM_DISPLAY_INFO_GAP, line);

  if (fpga_state.has_rx != 0U)
  {
    sprintf(line, "FPGA C:%02X A:%02X/%u D:%02X",
            fpga_state.last_cmd, fpga_state.last_ack_cmd,
            fpga_state.last_ack_status, fpga_state.dirty_mask);
  }
  else
  {
    sprintf(line, "FPGA C:%02X A:--  D:%02X",
            fpga_state.last_cmd, fpga_state.dirty_mask);
  }
  Spectrum_ShowLine(6, SPECTRUM_DISPLAY_INFO_Y + (SPECTRUM_DISPLAY_INFO_GAP * 2U), line);

  Spectrum_DrawUiBoxes();
  Spectrum_DrawFpgaBox();

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
  display_state.fpga_frequency_hz = 1000000UL;
  display_state.fpga_phase_deg = 0U;
  display_state.fpga_amplitude_code = 8191U;
  display_state.fpga_offset_code = 0;
  display_state.fpga_duty_code = 32768U;
  display_state.fpga_waveform = 0U;
  display_state.fpga_output_enable = 1U;
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
