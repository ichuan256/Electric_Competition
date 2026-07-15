#include "SpectrumDisplay_User.h"

#include <stdio.h>
#include <string.h>
#include "BoardComm_User.h"
#include "AdcFftProtocol_User.h"
#include "FpgaUart_User.h"
#include "lcd.h"
#include "led.h"

#define DISPLAY_STATUS_HEADER_LEN 17U
#define DISPLAY_WAVE_PAYLOAD_LEN  14U
#define DISPLAY_FIELD_BOX_W       75U
#define DISPLAY_FIELD_BOX_H       40U
#define DISPLAY_FIELD_BOX_GAP     3U
#define DISPLAY_FIELD_ROW0_Y      52U
#define DISPLAY_FIELD_ROW1_Y      96U
#define DISPLAY_TABLE_X           8U
#define DISPLAY_TABLE_Y           154U
#define DISPLAY_TABLE_W           464U
#define DISPLAY_TABLE_ROW_H       24U
#define DISPLAY_TABLE_ROWS        5U
#define DISPLAY_INFO_Y            284U
#define DISPLAY_LCR_FREQ_HZ       1000000UL
#define DISPLAY_LCR_DDS_CLK_HZ    1000000000ULL
#define DISPLAY_LCR_ADC_FS_HZ     2500000UL
#define DISPLAY_LCR_FFT_LEN       4096UL
#define DISPLAY_LCR_SETTLE_US     1000UL
#define DISPLAY_FPGA_SAMPLE_HZ    100000000UL
#define DISPLAY_FPGA_POINTS_MIN   16UL
#define DISPLAY_FPGA_POINTS_MAX   4096UL

static SpectrumDisplayState display_state;
static uint32_t display_last_rx_count = 0UL;
static uint32_t display_last_refresh_tick = 0UL;
static uint16_t display_last_rx_size = 0U;
static uint8_t display_force_refresh = 1U;
static uint8_t display_refresh_requested = 0U;
static uint8_t display_info_refresh_requested = 0U;
static uint8_t display_last_apply_counter = 0xFFU;
static uint8_t display_last_fpga_queue_index = 0xFFU;
static uint8_t display_last_fpga_queue_count = 0xFFU;
static uint8_t display_last_fpga_dirty = 0xFFU;
static uint32_t display_last_fpga_tx_count = 0xFFFFFFFFUL;
static uint32_t display_last_fpga_rx_count = 0xFFFFFFFFUL;
static uint32_t display_last_fpga_error_count = 0xFFFFFFFFUL;
static uint8_t display_last_fpga_ack_cmd = 0xFFU;
static uint8_t display_last_fpga_ack_status = 0xFFU;
static uint32_t display_last_fft_rx_count = 0xFFFFFFFFUL;
static uint32_t display_last_fft_error_count = 0xFFFFFFFFUL;
static uint8_t display_last_fft_has_result = 0xFFU;
static uint8_t display_last_fft_seq = 0xFFU;
static uint32_t display_lcr_last_ftw = 0UL;
static uint64_t display_lcr_last_frequency_mHz = 0ULL;
static uint16_t display_lcr_last_bin = 0U;

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

static uint32_t Spectrum_LcrDdsFtw(uint32_t frequency_hz)
{
  uint64_t scaled = ((uint64_t)frequency_hz << 32) + (DISPLAY_LCR_DDS_CLK_HZ / 2ULL);
  return (uint32_t)(scaled / DISPLAY_LCR_DDS_CLK_HZ);
}

static uint64_t Spectrum_LcrFrequencyMilliHz(uint32_t ftw)
{
  uint64_t scaled = ((uint64_t)ftw * DISPLAY_LCR_DDS_CLK_HZ * 1000ULL) + (1ULL << 31);
  return scaled >> 32;
}

static uint16_t Spectrum_LcrTargetBin(uint64_t frequency_mHz)
{
  uint64_t numerator = (frequency_mHz * DISPLAY_LCR_FFT_LEN) +
                       (((uint64_t)DISPLAY_LCR_ADC_FS_HZ * 1000ULL) / 2ULL);
  uint64_t bin = numerator / ((uint64_t)DISPLAY_LCR_ADC_FS_HZ * 1000ULL);

  if (bin > 2048ULL)
  {
    bin = 2048ULL;
  }

  return (uint16_t)bin;
}

static const char *Spectrum_StateText(uint8_t state)
{
  if (state == 1U)
  {
    return "READY";
  }
  if (state == 2U)
  {
    return "SENT";
  }
  return "IDLE";
}

static const char *Spectrum_WaveText(uint8_t waveform)
{
  switch (waveform)
  {
    case 0U: return "SIN";
    case 1U: return "SQR";
    case 2U: return "TRI";
    case 3U: return "SAW";
    default: return "?";
  }
}

static char Spectrum_PrintableKey(uint8_t key)
{
  return ((key >= 0x20U) && (key <= 0x7EU)) ? (char)key : '-';
}

static void Spectrum_FormatFreq(uint32_t hz, char *text, uint8_t text_len)
{
  if ((text == 0) || (text_len == 0U))
  {
    return;
  }

  if (hz >= 1000000UL)
  {
    snprintf(text, text_len, "%lu.%03luM",
             (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz / 1000UL) % 1000UL));
  }
  else
  {
    snprintf(text, text_len, "%lu.%03luk",
             (unsigned long)(hz / 1000UL),
             (unsigned long)(hz % 1000UL));
  }
}

static const char *Spectrum_FieldName(uint8_t field)
{
  switch (field)
  {
    case 0U: return "COUNT";
    case 1U: return "CH";
    case 2U: return "WSEL";
    case 3U: return "FREQ";
    case 4U: return "PHASE";
    case 5U: return "AMP";
    case 6U: return "OFFS";
    case 7U: return "DUTY";
    case 8U: return "WAVE";
    case 9U: return "EN";
    case 10U: return "BIAS";
    case 11U: return "MODE";
    default: return "?";
  }
}

static SpectrumDisplayWaveConfig *Spectrum_SelectedWave(void)
{
  uint8_t index = display_state.selected_wave;

  if (index >= SPECTRUM_DISPLAY_SUM_MAX_WAVES)
  {
    index = 0U;
  }

  return &display_state.waves[index];
}

static void Spectrum_FormatFieldValue(uint8_t field, char *text, uint8_t text_len)
{
  SpectrumDisplayWaveConfig *wave = Spectrum_SelectedWave();

  if ((text == 0) || (text_len == 0U))
  {
    return;
  }

  switch (field)
  {
    case 0U:
      snprintf(text, text_len, "%u", display_state.wave_count);
      break;
    case 1U:
      snprintf(text, text_len, "CH%u", (uint16_t)display_state.channel_id + 1U);
      break;
    case 2U:
      snprintf(text, text_len, "%u", (uint16_t)display_state.selected_wave + 1U);
      break;
    case 3U:
      Spectrum_FormatFreq(wave->frequency_hz, text, text_len);
      break;
    case 4U:
      snprintf(text, text_len, "%u", wave->phase_deg);
      break;
    case 5U:
      snprintf(text, text_len, "%u", wave->amplitude_code);
      break;
    case 6U:
      snprintf(text, text_len, "%d", wave->offset_code);
      break;
    case 7U:
    {
      uint32_t duty_x10 = ((uint32_t)wave->duty_code * 1000UL + 32767UL) / 65535UL;
      snprintf(text, text_len, "%lu.%lu%%",
               (unsigned long)(duty_x10 / 10UL),
               (unsigned long)(duty_x10 % 10UL));
      break;
    }
    case 8U:
      snprintf(text, text_len, "%s", Spectrum_WaveText(wave->waveform));
      break;
    case 9U:
      snprintf(text, text_len, "%u", wave->enable);
      break;
    case 10U:
      snprintf(text, text_len, "%dmV", display_state.output_bias_mv);
      break;
    case 11U:
      snprintf(text, text_len, "%s", display_state.fpga_output_mode ? "CACHE" : "DDS");
      break;
    default:
      snprintf(text, text_len, "-");
      break;
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

static void Spectrum_SendSumToFpga(void)
{
  FpgaUartWaveConfig waves[SPECTRUM_DISPLAY_SUM_MAX_WAVES];
  uint32_t period_points = 0UL;
  uint8_t control_flags = FPGA_UART_CONTROL_REALTIME;
  uint8_t i;

  for (i = 0U; i < SPECTRUM_DISPLAY_SUM_MAX_WAVES; i++)
  {
    waves[i].frequency_hz = display_state.waves[i].frequency_hz;
    waves[i].phase_deg = display_state.waves[i].phase_deg;
    waves[i].amplitude_code = display_state.waves[i].amplitude_code;
    waves[i].offset_code = display_state.waves[i].offset_code;
    waves[i].duty_code = display_state.waves[i].duty_code;
    waves[i].waveform = display_state.waves[i].waveform;
    waves[i].enable = display_state.waves[i].enable;
  }

  if (display_state.fpga_output_mode != 0U)
  {
    control_flags = FPGA_UART_CONTROL_CACHE;
    if (waves[0].frequency_hz != 0UL)
    {
      period_points = (DISPLAY_FPGA_SAMPLE_HZ + waves[0].frequency_hz / 2UL) /
                      waves[0].frequency_hz;
    }
    if (period_points < DISPLAY_FPGA_POINTS_MIN)
    {
      period_points = DISPLAY_FPGA_POINTS_MIN;
    }
    else if (period_points > DISPLAY_FPGA_POINTS_MAX)
    {
      period_points = DISPLAY_FPGA_POINTS_MAX;
    }
  }

  FpgaUart_SetMultiwave(display_state.channel_id,
                        display_state.wave_count,
                        waves,
                        display_state.output_bias_mv,
                        control_flags,
                        (uint16_t)period_points);
}

static void Spectrum_ParseStatus(const uint8_t *data, uint8_t len)
{
  SpectrumDisplayState previous_state;
  uint8_t pos = 0U;
  uint8_t i;
  uint8_t changed;

  if ((data == 0) || (len < DISPLAY_STATUS_HEADER_LEN))
  {
    return;
  }

  previous_state = display_state;

  display_state.mode = data[pos++];
  display_state.state = data[pos++];
  display_state.channel_id = data[pos++] & 0x01U;
  display_state.wave_count = data[pos++];
  display_state.selected_wave = data[pos++];
  display_state.ui_focus = data[pos++];
  display_state.ui_editing = data[pos++];
  display_state.ui_input_len = data[pos++];

  if (display_state.wave_count < 1U)
  {
    display_state.wave_count = 1U;
  }
  if (display_state.wave_count > SPECTRUM_DISPLAY_SUM_MAX_WAVES)
  {
    display_state.wave_count = SPECTRUM_DISPLAY_SUM_MAX_WAVES;
  }
  if (display_state.selected_wave >= SPECTRUM_DISPLAY_SUM_MAX_WAVES)
  {
    display_state.selected_wave = 0U;
  }
  if (display_state.ui_focus >= SPECTRUM_DISPLAY_SUM_FIELD_COUNT)
  {
    display_state.ui_focus = 0U;
  }
  if (display_state.ui_input_len > SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN)
  {
    display_state.ui_input_len = SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN;
  }

  memcpy(display_state.ui_input, &data[pos], SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN);
  display_state.ui_input[display_state.ui_input_len] = '\0';
  pos = (uint8_t)(pos + SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN);

  display_state.apply_counter = data[pos++];

  for (i = 0U; i < SPECTRUM_DISPLAY_SUM_MAX_WAVES; i++)
  {
    if ((uint8_t)(pos + DISPLAY_WAVE_PAYLOAD_LEN) > len)
    {
      break;
    }

    display_state.waves[i].frequency_hz = Spectrum_ReadU32(data, &pos);
    display_state.waves[i].phase_deg = Spectrum_ReadU16(data, &pos);
    display_state.waves[i].amplitude_code = Spectrum_ReadU16(data, &pos);
    display_state.waves[i].offset_code = Spectrum_ReadI16(data, &pos);
    display_state.waves[i].duty_code = Spectrum_ReadU16(data, &pos);
    display_state.waves[i].waveform = data[pos++];
    display_state.waves[i].enable = data[pos++];
  }

  if ((uint8_t)(pos + 2U) <= len)
  {
    display_state.output_bias_mv = Spectrum_ReadI16(data, &pos);
  }
  if (pos < len)
  {
    display_state.fpga_output_mode = (data[pos] != 0U) ? 1U : 0U;
  }

  if (display_state.apply_counter != display_last_apply_counter)
  {
    uint8_t previous_apply_counter = display_last_apply_counter;
    display_last_apply_counter = display_state.apply_counter;
    if (display_state.mode == SPECTRUM_DISPLAY_MODE_LCR_TEST)
    {
      (void)previous_apply_counter;
    }
    else
    {
      Spectrum_SendSumToFpga();
    }
  }

  changed = (memcmp(&previous_state, &display_state, sizeof(display_state)) != 0) ? 1U : 0U;
  if (changed != 0U)
  {
    display_refresh_requested = 1U;
  }
}

static void Spectrum_LoadDefaults(void)
{
  uint8_t i;

  memset(&display_state, 0, sizeof(display_state));
  display_state.state = 1U;
  display_state.wave_count = 2U;
  display_state.ui_focus = 3U;
  display_state.apply_counter = 1U;
  display_state.output_bias_mv = 0;
  display_state.fpga_output_mode = 0U;
  display_state.last_key = '-';
  display_state.last_key_ascii = 0U;
  display_lcr_last_ftw = Spectrum_LcrDdsFtw(DISPLAY_LCR_FREQ_HZ);
  display_lcr_last_frequency_mHz = Spectrum_LcrFrequencyMilliHz(display_lcr_last_ftw);
  display_lcr_last_bin = Spectrum_LcrTargetBin(display_lcr_last_frequency_mHz);

  for (i = 0U; i < SPECTRUM_DISPLAY_SUM_MAX_WAVES; i++)
  {
    display_state.waves[i].frequency_hz = 1000000UL + (uint32_t)i * 100000UL;
    display_state.waves[i].phase_deg = 0U;
    display_state.waves[i].amplitude_code = (i < 2U) ? 2048U : 0U;
    display_state.waves[i].offset_code = 0;
    display_state.waves[i].duty_code = 32768U;
    display_state.waves[i].waveform = (i < 2U) ? 1U : 0U;
    display_state.waves[i].enable = (i < 2U) ? 1U : 0U;
  }
}

static void Spectrum_DrawFieldBox(uint8_t field, uint16_t x, uint16_t y)
{
  char value[18];
  uint16_t border_color = GRAY;

  if (field == display_state.ui_focus)
  {
    border_color = display_state.ui_editing ? RED : BLUE;
  }

  lcd_fill(x, y, (uint16_t)(x + DISPLAY_FIELD_BOX_W - 1U),
           (uint16_t)(y + DISPLAY_FIELD_BOX_H - 1U), WHITE);
  lcd_draw_rectangle(x, y, (uint16_t)(x + DISPLAY_FIELD_BOX_W - 1U),
                     (uint16_t)(y + DISPLAY_FIELD_BOX_H - 1U), border_color);
  lcd_show_string((uint16_t)(x + 5U), (uint16_t)(y + 4U), 65U, 12U, 12U,
                  (char *)Spectrum_FieldName(field), GRAY);

  if ((field == display_state.ui_focus) && (display_state.ui_editing != 0U))
  {
    Spectrum_FormatEditText(value, sizeof(value));
  }
  else
  {
    Spectrum_FormatFieldValue(field, value, sizeof(value));
  }

  lcd_show_string((uint16_t)(x + 5U), (uint16_t)(y + 20U), 65U, 16U, 16U,
                  value, BLACK);
}

static void Spectrum_DrawFields(void)
{
  uint8_t i;
  uint16_t x;
  uint16_t y;

  for (i = 0U; i < SPECTRUM_DISPLAY_SUM_FIELD_COUNT; i++)
  {
    x = (uint16_t)(8U + (uint16_t)(i % 6U) * (DISPLAY_FIELD_BOX_W + DISPLAY_FIELD_BOX_GAP));
    y = (i < 6U) ? DISPLAY_FIELD_ROW0_Y : DISPLAY_FIELD_ROW1_Y;
    Spectrum_DrawFieldBox(i, x, y);
  }
}

static void Spectrum_DrawTable(void)
{
  char line[64];
  char freq[16];
  uint8_t i;
  uint16_t y;
  uint16_t color;

  lcd_fill(DISPLAY_TABLE_X, DISPLAY_TABLE_Y,
           (uint16_t)(DISPLAY_TABLE_X + DISPLAY_TABLE_W),
           (uint16_t)(DISPLAY_TABLE_Y + DISPLAY_TABLE_ROW_H * DISPLAY_TABLE_ROWS),
           WHITE);
  lcd_draw_rectangle(DISPLAY_TABLE_X, DISPLAY_TABLE_Y,
                     (uint16_t)(DISPLAY_TABLE_X + DISPLAY_TABLE_W),
                     (uint16_t)(DISPLAY_TABLE_Y + DISPLAY_TABLE_ROW_H * DISPLAY_TABLE_ROWS),
                     BLACK);
  lcd_fill((uint16_t)(DISPLAY_TABLE_X + 1U), (uint16_t)(DISPLAY_TABLE_Y + 1U),
           (uint16_t)(DISPLAY_TABLE_X + DISPLAY_TABLE_W - 1U),
           (uint16_t)(DISPLAY_TABLE_Y + DISPLAY_TABLE_ROW_H - 1U), LGRAY);
  lcd_show_string(14U, 160U, 452U, 16U, 16U,
                  "#  EN TYPE  FREQ       PHASE AMP  OFFS DUTY", BLACK);

  for (i = 0U; i < SPECTRUM_DISPLAY_SUM_MAX_WAVES; i++)
  {
    y = (uint16_t)(DISPLAY_TABLE_Y + DISPLAY_TABLE_ROW_H * (i + 1U));
    color = (i < display_state.wave_count) ? BLACK : GRAY;

    if (i == display_state.selected_wave)
    {
      lcd_draw_rectangle((uint16_t)(DISPLAY_TABLE_X + 2U), (uint16_t)(y + 2U),
                         (uint16_t)(DISPLAY_TABLE_X + DISPLAY_TABLE_W - 2U),
                         (uint16_t)(y + DISPLAY_TABLE_ROW_H - 2U), BLUE);
    }

    Spectrum_FormatFreq(display_state.waves[i].frequency_hz, freq, sizeof(freq));
    snprintf(line, sizeof(line), "%u  %u  %-3s   %-10s %3u   %4u %4d %3lu%%",
             (uint16_t)i + 1U,
             display_state.waves[i].enable,
             Spectrum_WaveText(display_state.waves[i].waveform),
             freq,
             display_state.waves[i].phase_deg,
             display_state.waves[i].amplitude_code,
             display_state.waves[i].offset_code,
             ((uint32_t)display_state.waves[i].duty_code * 100UL + 32767UL) / 65535UL);
    lcd_show_string(14U, (uint16_t)(y + 5U), 452U, 16U, 16U, line, color);
  }
}

static void Spectrum_DrawInfo(void)
{
  char line[64];
  FpgaUartState fpga = FpgaUart_GetState();

  lcd_fill(0U, DISPLAY_INFO_Y, 479U, 319U, WHITE);

  snprintf(line, sizeof(line), "S:%s W%u/%u F:%s K:%c RX:%lu E:%lu Z:%u",
           Spectrum_StateText(display_state.state),
           (uint16_t)display_state.selected_wave + 1U,
           display_state.wave_count,
           Spectrum_FieldName(display_state.ui_focus),
           display_state.last_key,
           (unsigned long)display_state.rx_count,
           (unsigned long)display_state.error_count,
           display_last_rx_size);
  lcd_show_string(8U, DISPLAY_INFO_Y, 464U, 16U, 16U, line, BLACK);

  snprintf(line, sizeof(line), "FPGA C:%02X A:%02X/%u T:%lu R:%lu E:%lu Q:%u/%u W:%u R:%u",
           fpga.last_cmd,
           fpga.last_ack_cmd,
           fpga.last_ack_status,
           (unsigned long)fpga.tx_count,
           (unsigned long)fpga.rx_count,
           (unsigned long)fpga.error_count,
           fpga.queue_index,
           fpga.queue_count,
           fpga.waiting_ack,
           fpga.retry_count);
  lcd_show_string(8U, 304U, 464U, 16U, 16U, line, BLACK);
}

static void Spectrum_DrawLcrPage(void)
{
  char line[64];
  AdcFftProtocolState adc_fft = AdcFftProtocol_GetState();
  AdcFftMeasurementResult result = adc_fft.last_result;

  lcd_fill(0U, 0U, 479U, 319U, WHITE);
  lcd_show_string(8U, 8U, 360U, 24U, 24U, "LCR ADC FFT TEST", RED);
  lcd_show_string(8U, 34U, 460U, 16U, 16U,
                  "B back   D measure   DDS: 1MHz sine, no DC offset",
                  GRAY);

  lcd_draw_rectangle(8U, 58U, 471U, 146U, BLACK);
  lcd_fill(9U, 59U, 470U, 80U, LGRAY);
  lcd_show_string(16U, 62U, 430U, 16U, 16U, "REQUEST", BLACK);
  snprintf(line, sizeof(line), "FREQ: 1.000000MHz   BIN:%u   SETTLE:%luus",
           display_lcr_last_bin,
           (unsigned long)DISPLAY_LCR_SETTLE_US);
  lcd_show_string(16U, 88U, 440U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "FTW: 0x%08lX   ACT:%lu.%06luMHz",
           (unsigned long)display_lcr_last_ftw,
           (unsigned long)(display_lcr_last_frequency_mHz / 1000000000ULL),
           (unsigned long)((display_lcr_last_frequency_mHz / 1000ULL) % 1000000ULL));
  lcd_show_string(16U, 112U, 440U, 16U, 16U, line, BLACK);

  lcd_draw_rectangle(8U, 158U, 471U, 278U, BLACK);
  lcd_fill(9U, 159U, 470U, 180U, LGRAY);
  lcd_show_string(16U, 162U, 430U, 16U, 16U, "SINE FIT RESULT", BLACK);
  snprintf(line, sizeof(line), "SEQ:%u SW:%u PT:%u BIN:%u/%u STAT:0x%04X",
           adc_fft.last_seq,
           result.sweep_id,
           result.point_id,
           result.main_bin,
           result.target_bin,
           result.status);
  lcd_show_string(16U, 188U, 440U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "RMS:%lu.%03lumV",
           (unsigned long)(result.voltage_uv_rms / 1000UL),
           (unsigned long)(result.voltage_uv_rms % 1000UL));
  lcd_show_string(16U, 212U, 220U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "PEAK:%lu.%03lumV",
           (unsigned long)(result.voltage_uv_peak / 1000UL),
           (unsigned long)(result.voltage_uv_peak % 1000UL));
  lcd_show_string(240U, 212U, 220U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "F:%luHz FS:%luHz ADC:%u..%u",
           (unsigned long)result.main_frequency_hz,
           (unsigned long)result.sample_rate_hz,
           result.adc_min_code,
           result.adc_max_code);
  lcd_show_string(16U, 236U, 440U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "MIN:%ldmV MAX:%ldmV VPP:%lumV",
           (long)(result.voltage_uv_min / 1000L),
           (long)(result.voltage_uv_max / 1000L),
           (unsigned long)(result.voltage_uv_pp / 1000UL));
  lcd_show_string(16U, 260U, 440U, 16U, 16U, line, BLACK);

  snprintf(line, sizeof(line), "ADC FIT CMD:%02X TX:%lu RX:%lu ERR:%lu BUSY:%lu STATE:%u",
           adc_fft.last_cmd,
           (unsigned long)adc_fft.tx_count,
           (unsigned long)adc_fft.rx_count,
           (unsigned long)adc_fft.error_count,
           (unsigned long)adc_fft.busy_count,
           (unsigned int)AdcFftMeasure_GetState());
  lcd_show_string(8U, 284U, 464U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "KEY:%c  BOARD RX:%lu ERR:%lu  UART ERR:%lu",
           display_state.last_key,
           (unsigned long)display_state.rx_count,
           (unsigned long)display_state.error_count,
           (unsigned long)adc_fft.crc_error_count);
  lcd_show_string(8U, 304U, 464U, 16U, 16U, line, BLACK);
}

static void Spectrum_DrawScreen(void)
{
  if (display_state.mode == SPECTRUM_DISPLAY_MODE_LCR_TEST)
  {
    Spectrum_DrawLcrPage();
    return;
  }

  lcd_fill(0U, 0U, 479U, 51U, WHITE);
  lcd_show_string(8U, 8U, 340U, 24U, 24U, "SUM WAVEFORM", RED);
  lcd_show_string(8U, 34U, 460U, 16U, 16U,
                  "D edit  4/2 left  6/8 right  * del  # point",
                  GRAY);
  Spectrum_DrawFields();
  Spectrum_DrawTable();
  Spectrum_DrawInfo();
}

void SpectrumDisplay_Init(void)
{
  Spectrum_LoadDefaults();
  lcd_clear(WHITE);
  Spectrum_DrawScreen();
}

void SpectrumDisplay_Task(void)
{
  BoardComm_State comm_state;
  FpgaUartState fpga_state;
  char key = 0;
  uint32_t now = HAL_GetTick();

  LED0_TOGGLE();

  if (BoardComm_TakeKeypadKey(&key) != 0U)
  {
    display_state.last_key = key;
    display_state.last_key_ascii = (uint8_t)key;
    if ((key == 'C') && (display_state.ui_editing == 0U))
    {
      Spectrum_SendSumToFpga();
    }
    display_info_refresh_requested = 1U;
  }

  comm_state = BoardComm_GetState();
  display_state.rx_count = comm_state.rx_count;
  display_state.error_count = comm_state.error_count;
  display_last_rx_size = comm_state.last_rx_size;

  if (comm_state.rx_count != display_last_rx_count)
  {
    display_last_rx_count = comm_state.rx_count;
    if ((comm_state.last_status == BOARD_COMM_OK) &&
        (comm_state.last_cmd == BOARD_COMM_CMD_SYS_STATUS))
    {
      Spectrum_ParseStatus(comm_state.last_data, comm_state.last_len);
    }
    else
    {
      display_refresh_requested = 1U;
    }
  }

  fpga_state = FpgaUart_GetState();
  if ((fpga_state.queue_index != display_last_fpga_queue_index) ||
      (fpga_state.queue_count != display_last_fpga_queue_count) ||
      (fpga_state.dirty_mask != display_last_fpga_dirty) ||
      (fpga_state.tx_count != display_last_fpga_tx_count) ||
      (fpga_state.rx_count != display_last_fpga_rx_count) ||
      (fpga_state.error_count != display_last_fpga_error_count) ||
      (fpga_state.last_ack_cmd != display_last_fpga_ack_cmd) ||
      (fpga_state.last_ack_status != display_last_fpga_ack_status))
  {
    display_last_fpga_queue_index = fpga_state.queue_index;
    display_last_fpga_queue_count = fpga_state.queue_count;
    display_last_fpga_dirty = fpga_state.dirty_mask;
    display_last_fpga_tx_count = fpga_state.tx_count;
    display_last_fpga_rx_count = fpga_state.rx_count;
    display_last_fpga_error_count = fpga_state.error_count;
    display_last_fpga_ack_cmd = fpga_state.last_ack_cmd;
    display_last_fpga_ack_status = fpga_state.last_ack_status;
    display_info_refresh_requested = 1U;
  }

  {
    AdcFftProtocolState fft_state = AdcFftProtocol_GetState();
    uint8_t has_result = (fft_state.last_result.status != 0U) ? 1U : 0U;
    if ((fft_state.rx_count != display_last_fft_rx_count) ||
        (fft_state.error_count != display_last_fft_error_count) ||
        (has_result != display_last_fft_has_result) ||
        (fft_state.last_seq != display_last_fft_seq))
    {
      display_last_fft_rx_count = fft_state.rx_count;
      display_last_fft_error_count = fft_state.error_count;
      display_last_fft_has_result = has_result;
      display_last_fft_seq = fft_state.last_seq;
      display_info_refresh_requested = 1U;
    }
  }

  if ((display_force_refresh != 0U) || (display_refresh_requested != 0U))
  {
    display_last_refresh_tick = now;
    display_force_refresh = 0U;
    display_refresh_requested = 0U;
    display_info_refresh_requested = 0U;
    Spectrum_DrawScreen();
  }
  else if (display_info_refresh_requested != 0U)
  {
    display_last_refresh_tick = now;
    display_info_refresh_requested = 0U;
    if (display_state.mode == SPECTRUM_DISPLAY_MODE_LCR_TEST)
    {
      Spectrum_DrawLcrPage();
    }
    else
    {
      Spectrum_DrawInfo();
    }
  }
}

SpectrumDisplayState SpectrumDisplay_GetState(void)
{
  return display_state;
}
