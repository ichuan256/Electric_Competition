#include "SpectrumDisplay_User.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BoardComm_User.h"
#include "FpgaUart_User.h"
#include "LcrAuto_User.h"
#include "LcrDualAdc_User.h"
#include "lcd.h"
#include "led.h"

#define DISPLAY_STATUS_HEADER_LEN 6U
#define DISPLAY_FIELD_BOX_W       75U
#define DISPLAY_FIELD_BOX_H       40U
#define DISPLAY_FIELD_BOX_GAP     3U
#define DISPLAY_FIELD_ROW0_Y      52U
#define DISPLAY_FIELD_ROW1_Y      96U
#define DISPLAY_TABLE_X           8U
#define DISPLAY_TABLE_Y           154U
#define DISPLAY_TABLE_W           464U
#define DISPLAY_TABLE_ROW_H       22U
#define DISPLAY_TABLE_ROWS        5U
#define DISPLAY_INFO_Y            268U
#define DISPLAY_INFO_LINE_GAP     20U
#define DISPLAY_FPGA_SAMPLE_HZ    100000000UL
#define DISPLAY_FPGA_POINTS_MIN   16UL
#define DISPLAY_FPGA_POINTS_MAX   4096UL
#define DISPLAY_AMPLITUDE_CODE_MAX 8191U
#define DISPLAY_AMPLITUDE_PEAK_MAX_MV 3500UL
#define DISPLAY_OUTPUT_FULL_SCALE_UVPP 7000000ULL

static SpectrumDisplayState display_state;
static uint32_t display_last_rx_count = 0UL;
static uint32_t display_last_refresh_tick = 0UL;
static uint16_t display_last_rx_size = 0U;
static uint8_t display_force_refresh = 1U;
static uint8_t display_refresh_requested = 0U;
static uint8_t display_info_refresh_requested = 0U;
static uint8_t display_last_fpga_queue_index = 0xFFU;
static uint8_t display_last_fpga_queue_count = 0xFFU;
static uint8_t display_last_fpga_dirty = 0xFFU;
static uint32_t display_last_fpga_tx_count = 0xFFFFFFFFUL;
static uint32_t display_last_fpga_rx_count = 0xFFFFFFFFUL;
static uint32_t display_last_fpga_error_count = 0xFFFFFFFFUL;
static uint8_t display_last_fpga_ack_cmd = 0xFFU;
static uint8_t display_last_fpga_ack_status = 0xFFU;
static uint32_t display_last_lcr_revision = 0xFFFFFFFFUL;

typedef struct {
  uint16_t transaction_id;
  uint8_t valid;
  uint8_t channel_id;
  uint8_t wave_count;
  uint8_t generation_mode;
  uint16_t cache_points;
  int16_t offset_code;
  FpgaUartWaveConfig waves[SPECTRUM_DISPLAY_SUM_MAX_WAVES];
} SpectrumSourceStage;

static SpectrumSourceStage display_source_stage;
static uint16_t display_source_last_transaction;
static uint16_t display_source_last_applied_mask;
static uint8_t display_source_commit_pending;
static uint16_t display_source_pending_transaction;
static uint16_t display_source_pending_mask;
static uint32_t display_source_pending_rx_count;
static uint32_t display_source_pending_error_count;

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

static uint16_t Spectrum_AmplitudeUvppToCode(uint32_t amplitude_uvpp)
{
  if ((uint64_t)amplitude_uvpp > DISPLAY_OUTPUT_FULL_SCALE_UVPP)
  {
    amplitude_uvpp = (uint32_t)DISPLAY_OUTPUT_FULL_SCALE_UVPP;
  }

  return (uint16_t)((((uint64_t)amplitude_uvpp * DISPLAY_AMPLITUDE_CODE_MAX) +
                     (DISPLAY_OUTPUT_FULL_SCALE_UVPP / 2ULL)) /
                    DISPLAY_OUTPUT_FULL_SCALE_UVPP);
}

static uint32_t Spectrum_AmplitudeCodeToMv(uint16_t amplitude_code)
{
  if (amplitude_code > DISPLAY_AMPLITUDE_CODE_MAX)
  {
    amplitude_code = DISPLAY_AMPLITUDE_CODE_MAX;
  }

  return (uint32_t)((((uint64_t)amplitude_code * DISPLAY_AMPLITUDE_PEAK_MAX_MV) +
                     (DISPLAY_AMPLITUDE_CODE_MAX / 2U)) /
                    DISPLAY_AMPLITUDE_CODE_MAX);
}

uint8_t SpectrumDisplay_HandleSourceFrame(uint8_t cmd, const uint8_t *data,
                                          uint8_t len, uint16_t *transaction_id,
                                          uint16_t *applied_mask)
{
  uint8_t pos = 0U;

  if ((data == 0) || (transaction_id == 0) || (applied_mask == 0))
  {
    return 0x02U;
  }
  *transaction_id = 0U;
  *applied_mask = 0U;

  if (cmd == BOARD_COMM_CMD_SOURCE_STAGE)
  {
    uint8_t channel_id;
    uint8_t output_enable;
    uint8_t generation_mode;
    uint8_t count;
    uint8_t slot_mask = 0U;
    int32_t offset_uunit;
    int32_t offset_code;

    if (len < 18U)
    {
      return 0x02U;
    }
    *transaction_id = Spectrum_ReadU16(data, &pos);
    channel_id = data[pos++];
    output_enable = data[pos++];
    generation_mode = data[pos++];
    count = data[pos++];
    pos = (uint8_t)(pos + 2U);
    offset_uunit = (int32_t)Spectrum_ReadU32(data, &pos);
    display_source_stage.cache_points = Spectrum_ReadU16(data, &pos);
    pos = (uint8_t)(pos + 4U);

    if ((count > SPECTRUM_DISPLAY_SUM_MAX_WAVES) ||
        (len != (uint8_t)(18U + count * 20U)) ||
        (generation_mode > 2U))
    {
      return 0x06U;
    }
    if (channel_id > 1U)
    {
      return 0x04U;
    }
    if ((display_source_commit_pending != 0U) &&
        (*transaction_id != display_source_pending_transaction))
    {
      return 0x08U;
    }

    display_source_stage.transaction_id = *transaction_id;
    display_source_stage.channel_id = channel_id;
    display_source_stage.wave_count = count;
    display_source_stage.generation_mode = generation_mode;
    offset_code = offset_uunit / 1000L;
    if ((offset_code < -8192L) || (offset_code > 8191L))
    {
      return 0x07U;
    }
    display_source_stage.offset_code = (int16_t)offset_code;
    display_source_stage.valid = 0U;

    for (uint8_t i = 0U; i < count; i++)
    {
      uint8_t slot = data[pos++];
      uint8_t waveform = data[pos++];
      uint16_t component_flags = Spectrum_ReadU16(data, &pos);
      uint32_t frequency_cHz = Spectrum_ReadU32(data, &pos);
      int32_t phase_mdeg = (int32_t)Spectrum_ReadU32(data, &pos);
      uint32_t amplitude_uunit = Spectrum_ReadU32(data, &pos);
      uint32_t duty_ppm = Spectrum_ReadU32(data, &pos);
      FpgaUartWaveConfig *wave;

      if ((slot >= count) || (waveform > 4U) ||
          ((slot_mask & (uint8_t)(1U << slot)) != 0U))
      {
        return 0x06U;
      }
      slot_mask |= (uint8_t)(1U << slot);
      if (((uint64_t)amplitude_uunit > DISPLAY_OUTPUT_FULL_SCALE_UVPP) ||
          (duty_ppm > 999000UL) ||
          ((waveform == 2U) && (duty_ppm < 1000UL)))
      {
        return 0x07U;
      }
      wave = &display_source_stage.waves[slot];
      wave->frequency_hz = (frequency_cHz + 50UL) / 100UL;
      phase_mdeg %= 360000L;
      if (phase_mdeg < 0L)
      {
        phase_mdeg += 360000L;
      }
      wave->phase_deg = (uint16_t)((phase_mdeg + 500L) / 1000L);
      wave->amplitude_code = Spectrum_AmplitudeUvppToCode(amplitude_uunit);
      wave->duty_code = (uint16_t)((((duty_ppm / 1000UL) * 65535UL) + 500UL) / 1000UL);
      wave->offset_code = 0;
      wave->waveform = (waveform == 0U) ? 0U : (uint8_t)(waveform - 1U);
      wave->enable = ((output_enable != 0U) && (waveform != 0U) &&
                      ((component_flags & 1U) != 0U)) ? 1U : 0U;
    }
    display_source_stage.valid = 1U;
    return 0x00U;
  }

  if (cmd == BOARD_COMM_CMD_SOURCE_COMMIT)
  {
    uint8_t channel_mask;
    uint8_t commit_flags;
    uint8_t control = FPGA_UART_CONTROL_REALTIME;
    uint16_t period_points;
    FpgaUartState fpga;

    if (len != 8U)
    {
      return 0x02U;
    }
    *transaction_id = Spectrum_ReadU16(data, &pos);
    channel_mask = data[pos++];
    commit_flags = data[pos++];
    if ((*transaction_id == display_source_last_transaction) &&
        (channel_mask == display_source_last_applied_mask))
    {
      *applied_mask = channel_mask;
      return 0x00U;
    }
    if (display_source_commit_pending != 0U)
    {
      if ((*transaction_id != display_source_pending_transaction) ||
          (channel_mask != display_source_pending_mask))
      {
        return 0x08U;
      }
      fpga = FpgaUart_GetState();
      if ((fpga.rx_count > display_source_pending_rx_count) &&
          (fpga.last_ack_cmd == FPGA_UART_CMD_COMMIT) &&
          (fpga.last_ack_status == 0U) &&
          (fpga.last_transaction_id == *transaction_id) &&
          (fpga.applied_mask == channel_mask) &&
          (fpga.dirty_mask == 0U))
      {
        display_source_commit_pending = 0U;
        display_source_last_transaction = *transaction_id;
        display_source_last_applied_mask = channel_mask;
        display_state.channel_id = display_source_stage.channel_id;
        display_state.wave_count = display_source_stage.wave_count;
        display_state.output_bias_mv = display_source_stage.offset_code;
        display_state.fpga_output_mode =
            (display_source_stage.generation_mode == 2U) ? 1U : 0U;
        display_state.apply_counter = (uint8_t)*transaction_id;
        memcpy(display_state.waves, display_source_stage.waves, sizeof(display_state.waves));
        display_source_stage.valid = 0U;
        display_refresh_requested = 1U;
        *applied_mask = channel_mask;
        return 0x00U;
      }
      if ((fpga.error_count > display_source_pending_error_count) &&
          (fpga.waiting_ack == 0U) && (fpga.queue_count == 0U))
      {
        display_source_commit_pending = 0U;
        display_source_stage.valid = 0U;
        if ((fpga.last_ack_cmd == FPGA_UART_CMD_COMMIT) &&
            (fpga.last_ack_status != 0U) && (fpga.last_ack_status <= 0x0DU))
        {
          return fpga.last_ack_status;
        }
        return 0x0CU;
      }
      return 0x08U;
    }
    if ((display_source_stage.valid == 0U) ||
        (display_source_stage.transaction_id != *transaction_id))
    {
      return 0x09U;
    }
    if (channel_mask != (uint8_t)(1U << display_source_stage.channel_id))
    {
      return 0x06U;
    }
    if (display_source_stage.generation_mode == 2U)
    {
      control = FPGA_UART_CONTROL_CACHE;
    }
    period_points = display_source_stage.cache_points;
    if ((control == FPGA_UART_CONTROL_CACHE) && (period_points == 0U) &&
        (display_source_stage.wave_count != 0U) &&
        (display_source_stage.waves[0].frequency_hz != 0UL))
    {
      uint32_t points = (DISPLAY_FPGA_SAMPLE_HZ +
                         display_source_stage.waves[0].frequency_hz / 2UL) /
                        display_source_stage.waves[0].frequency_hz;
      if (points < DISPLAY_FPGA_POINTS_MIN) { points = DISPLAY_FPGA_POINTS_MIN; }
      if (points > DISPLAY_FPGA_POINTS_MAX) { points = DISPLAY_FPGA_POINTS_MAX; }
      period_points = (uint16_t)points;
    }
    FpgaUart_SetMultiwaveTransaction(*transaction_id,
                                     display_source_stage.channel_id,
                                     display_source_stage.wave_count,
                                     display_source_stage.waves,
                                     display_source_stage.offset_code,
                                     control, period_points, commit_flags);
    fpga = FpgaUart_GetState();
    display_source_commit_pending = 1U;
    display_source_pending_transaction = *transaction_id;
    display_source_pending_mask = channel_mask;
    display_source_pending_rx_count = fpga.rx_count;
    display_source_pending_error_count = fpga.error_count;
    return 0x08U;
  }

  return 0x05U;
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
    case 5U: return "AMP(mV)";
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
      snprintf(text, text_len, "%lumV",
               (unsigned long)Spectrum_AmplitudeCodeToMv(wave->amplitude_code));
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
  if ((display_state.ui_focus == 5U) && (len < (uint8_t)(text_len - 1U)))
  {
    text[len++] = 'm';
  }
  if ((display_state.ui_focus == 5U) && (len < (uint8_t)(text_len - 1U)))
  {
    text[len++] = 'V';
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
  uint8_t changed;

  if ((data == 0) || (len < DISPLAY_STATUS_HEADER_LEN))
  {
    return;
  }

  previous_state = display_state;

  display_state.mode = data[pos++];
  display_state.channel_id = data[pos++] & 0x01U;
  display_state.selected_wave = data[pos++];
  display_state.ui_focus = data[pos++];
  display_state.ui_editing = data[pos++];
  display_state.ui_input_len = data[pos++];

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

  if (len < (uint8_t)(pos + SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN))
  {
    return;
  }
  memcpy(display_state.ui_input, &data[pos], SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN);
  display_state.ui_input[display_state.ui_input_len] = '\0';

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
                  "#  EN TYPE  FREQ       PHASE AMP(mV) OFFS DUTY", BLACK);

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
    snprintf(line, sizeof(line), "%u  %u  %-3s   %-10s %3u   %4lu %4d %3lu%%",
             (uint16_t)i + 1U,
             display_state.waves[i].enable,
             Spectrum_WaveText(display_state.waves[i].waveform),
             freq,
             display_state.waves[i].phase_deg,
             (unsigned long)Spectrum_AmplitudeCodeToMv(display_state.waves[i].amplitude_code),
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
  lcd_show_string(8U, (uint16_t)(DISPLAY_INFO_Y + DISPLAY_INFO_LINE_GAP),
                  464U, 16U, 16U, line, BLACK);
}

static const char *Spectrum_LcrAdcErrorText(LcrDualAdcErrorSource source)
{
  switch (source)
  {
    case LCR_DUAL_ADC_ERROR_NONE: return "NONE";
    case LCR_DUAL_ADC_ERROR_TIMER_CONFIG: return "TMR_CFG";
    case LCR_DUAL_ADC_ERROR_DMA_START: return "DMA_START";
    case LCR_DUAL_ADC_ERROR_TIMER_START: return "TMR_START";
    case LCR_DUAL_ADC_ERROR_ADC_IRQ: return "ADC_IRQ";
    case LCR_DUAL_ADC_ERROR_CAPTURE_TIMEOUT: return "TIMEOUT";
    case LCR_DUAL_ADC_ERROR_FIT: return "FIT";
    default: return "?";
  }
}

static void Spectrum_DrawLcrPage(void)
{
  char line[64];
  LcrAutoSnapshot lcr = LcrAuto_GetSnapshot();
  LcrDualAdcSnapshot adc = LcrDualAdc_GetSnapshot();
  BoardComm_State board = BoardComm_GetState();

  lcd_fill(0U, 0U, 479U, 319U, WHITE);
  lcd_show_string(8U, 8U, 360U, 24U, 24U, "LCR AUTO", RED);
  lcd_show_string(8U, 34U, 460U, 16U, 16U,
                  "B back   D start   Blue ADC processing -> LCD",
                  GRAY);

  lcd_draw_rectangle(8U, 58U, 471U, 132U, BLACK);
  lcd_fill(9U, 59U, 470U, 80U, LGRAY);
  lcd_show_string(16U, 62U, 430U, 16U, 16U, "AUTO PROGRESS", BLACK);
  snprintf(line, sizeof(line), "STATE:%-10s  TYPE:%s  HW:%s",
           LcrAuto_StateText(lcr.state),
           LcrAuto_ComponentText(lcr.type),
           (lcr.hardware_ready != 0U) ? "READY" : "NEED 2ADC");
  lcd_show_string(16U, 86U, 440U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "F:%luHz  COARSE:%u/%u  AVG:%u/%u",
           (unsigned long)lcr.requested_frequency_hz,
           (unsigned int)((lcr.coarse_index < LCR_AUTO_COARSE_POINT_COUNT) ?
                          (lcr.coarse_index + 1U) : LCR_AUTO_COARSE_POINT_COUNT),
           (unsigned int)LCR_AUTO_COARSE_POINT_COUNT,
           (unsigned int)lcr.fine_count,
           (unsigned int)LCR_AUTO_FINE_AVERAGE_COUNT);
  lcd_show_string(16U, 108U, 440U, 16U, 16U, line, BLACK);

  lcd_draw_rectangle(8U, 144U, 471U, 268U, BLACK);
  lcd_fill(9U, 145U, 470U, 168U, LGRAY);
  lcd_show_string(16U, 148U, 430U, 16U, 16U, "RESULT", BLACK);

  if (lcr.result_valid != 0U)
  {
    if (lcr.type == LCR_COMPONENT_R)
    {
      snprintf(line, sizeof(line), "R = %lu.%03lu ohm",
               (unsigned long)(lcr.impedance_mohm / 1000UL),
               (unsigned long)(lcr.impedance_mohm % 1000UL));
    }
    else if (lcr.type == LCR_COMPONENT_L)
    {
      snprintf(line, sizeof(line), "L = %lu.%03lu uH",
               (unsigned long)(lcr.inductance_nh / 1000ULL),
               (unsigned long)(lcr.inductance_nh % 1000ULL));
    }
    else
    {
      snprintf(line, sizeof(line), "C = %lu.%03lu pF",
               (unsigned long)(lcr.capacitance_ff / 1000ULL),
               (unsigned long)(lcr.capacitance_ff % 1000ULL));
    }
    lcd_show_string(16U, 178U, 430U, 24U, 24U, line, RED);
    snprintf(line, sizeof(line), "Z:%lu.%03luohm  PH:%ld.%03lddeg",
             (unsigned long)(lcr.impedance_mohm / 1000UL),
             (unsigned long)(lcr.impedance_mohm % 1000UL),
             (long)(lcr.phase_mdeg / 1000L),
             (long)labs(lcr.phase_mdeg % 1000L));
    lcd_show_string(16U, 212U, 440U, 16U, 16U, line, BLACK);
    snprintf(line, sizeof(line), "Rs:%ldmohm X:%ldmohm Q:%lu.%03lu",
             (long)lcr.resistance_mohm,
             (long)lcr.reactance_mohm,
             (unsigned long)(lcr.quality_factor_x1000 / 1000UL),
             (unsigned long)(lcr.quality_factor_x1000 % 1000UL));
    lcd_show_string(16U, 238U, 440U, 16U, 16U, line, BLACK);
  }
  else
  {
    snprintf(line, sizeof(line), "STATUS: %s", LcrAuto_ErrorText(lcr.error));
    lcd_show_string(16U, 176U, 440U, 24U, 24U, line,
                    (lcr.state == LCR_AUTO_ERROR) ? RED : BLACK);
    snprintf(line, sizeof(line), "RAW R:%u V1:%u..%u V2:%u..%u",
             (unsigned int)adc.raw_valid,
             (unsigned int)adc.vin_min_code,
             (unsigned int)adc.vin_max_code,
             (unsigned int)adc.vr_min_code,
             (unsigned int)adc.vr_max_code);
    lcd_show_string(16U, 207U, 440U, 16U, 16U, line, BLACK);
    snprintf(line, sizeof(line), "FIT:%lu/%luuV ST:%04X FS:%lu",
             (unsigned long)adc.vin_peak_uv,
             (unsigned long)adc.vr_peak_uv,
             (unsigned int)adc.last_status,
             (unsigned long)adc.sample_rate_hz);
    lcd_show_string(16U, 227U, 440U, 16U, 16U, line, BLACK);
    snprintf(line, sizeof(line), "SRC:%s H:%lX D:%lX ND:%lu C:%u E:%u",
             Spectrum_LcrAdcErrorText(adc.error_source),
             (unsigned long)adc.last_hal_error,
             (unsigned long)adc.last_dma_error,
             (unsigned long)adc.dma_remaining,
             (unsigned int)adc.frame_complete,
             (unsigned int)adc.irq_error_seen);
    lcd_show_string(16U, 247U, 440U, 16U, 16U, line, BLACK);
  }

  snprintf(line, sizeof(line), "KEY:%c BOARD RX:%lu ERR:%lu FTW:%08lX",
           display_state.last_key,
           (unsigned long)board.rx_count,
           (unsigned long)board.error_count,
           (unsigned long)lcr.dds_ftw);
  lcd_show_string(8U, 276U, 464U, 16U, 16U, line, BLACK);
  snprintf(line, sizeof(line), "MEAS:%u START:%lu DONE:%lu ERR:%lu",
           lcr.measurement_id,
           (unsigned long)lcr.start_count,
           (unsigned long)lcr.completed_count,
           (unsigned long)lcr.error_count);
  lcd_show_string(8U, 296U, 464U, 16U, 16U, line, BLACK);
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
  memset(&display_source_stage, 0, sizeof(display_source_stage));
  display_source_last_transaction = 0xFFFFU;
  display_source_last_applied_mask = 0U;
  display_source_commit_pending = 0U;
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
    if ((key == 'D') &&
        (display_state.mode == SPECTRUM_DISPLAY_MODE_LCR_TEST))
    {
      (void)LcrAuto_Start();
    }
    else if (key == 'B')
    {
      LcrAuto_Cancel();
    }
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
    LcrAutoSnapshot lcr = LcrAuto_GetSnapshot();
    if (lcr.revision != display_last_lcr_revision)
    {
      display_last_lcr_revision = lcr.revision;
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
