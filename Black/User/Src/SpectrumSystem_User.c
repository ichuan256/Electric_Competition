#include "SpectrumSystem_User.h"

#include "AD9910_User.h"
#include "AdcFftClient_User.h"
#include "BoardComm_User.h"

static SpectrumHostSnapshot spectrum_snapshot;
static uint32_t spectrum_last_status_tick;
extern float dds_factor;

static void Spectrum_WriteU16(uint8_t *buf, uint8_t *pos, uint16_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFU);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void Spectrum_WriteI16(uint8_t *buf, uint8_t *pos, int16_t value)
{
  Spectrum_WriteU16(buf, pos, (uint16_t)value);
}

static void Spectrum_WriteU32(uint8_t *buf, uint8_t *pos, uint32_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint8_t Spectrum_IsDigit(char key)
{
  return ((key >= '0') && (key <= '9')) ? 1U : 0U;
}

static uint8_t Spectrum_FieldEditable(uint8_t field)
{
  return (field < SPECTRUM_SUM_FIELD_COUNT) ? 1U : 0U;
}

static void Spectrum_ClearInput(void)
{
  spectrum_snapshot.ui_input_len = 0U;
  spectrum_snapshot.ui_input[0] = '\0';
}

static void Spectrum_StartEdit(void)
{
  if (Spectrum_FieldEditable(spectrum_snapshot.ui_focus) == 0U)
  {
    return;
  }
  spectrum_snapshot.ui_editing = 1U;
  Spectrum_ClearInput();
}

static void Spectrum_CancelEdit(void)
{
  spectrum_snapshot.ui_editing = 0U;
  Spectrum_ClearInput();
}

static void Spectrum_AppendInput(char key)
{
  if (key == '#')
  {
    key = '.';
  }

  if (spectrum_snapshot.ui_input_len >= SPECTRUM_UI_INPUT_MAX_LEN)
  {
    return;
  }

  if ((Spectrum_IsDigit(key) == 0U) && (key != '.') && (key != '-'))
  {
    return;
  }

  if (key == '-')
  {
    if (spectrum_snapshot.ui_input_len != 0U)
    {
      return;
    }
  }

  if (key == '.')
  {
    for (uint8_t i = 0U; i < spectrum_snapshot.ui_input_len; i++)
    {
      if (spectrum_snapshot.ui_input[i] == '.')
      {
        return;
      }
    }
  }

  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len++] = key;
  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len] = '\0';
}

static void Spectrum_Backspace(void)
{
  if (spectrum_snapshot.ui_input_len == 0U)
  {
    return;
  }
  spectrum_snapshot.ui_input_len--;
  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len] = '\0';
}

static void Spectrum_ToggleNegativeInput(void)
{
  if ((spectrum_snapshot.ui_focus != 4U) && (spectrum_snapshot.ui_focus != 10U))
  {
    return;
  }

  if ((spectrum_snapshot.ui_input_len > 0U) && (spectrum_snapshot.ui_input[0] == '-'))
  {
    for (uint8_t i = 0U; i < spectrum_snapshot.ui_input_len; i++)
    {
      spectrum_snapshot.ui_input[i] = spectrum_snapshot.ui_input[(uint8_t)(i + 1U)];
    }
    spectrum_snapshot.ui_input_len--;
  }
  else if (spectrum_snapshot.ui_input_len < SPECTRUM_UI_INPUT_MAX_LEN)
  {
    for (uint8_t i = spectrum_snapshot.ui_input_len; i > 0U; i--)
    {
      spectrum_snapshot.ui_input[i] = spectrum_snapshot.ui_input[(uint8_t)(i - 1U)];
    }
    spectrum_snapshot.ui_input[0] = '-';
    spectrum_snapshot.ui_input_len++;
    spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len] = '\0';
  }
}

static uint32_t Spectrum_ParseInputX1000(uint8_t *valid, uint8_t *has_decimal)
{
  uint32_t integer_part = 0UL;
  uint32_t fraction_part = 0UL;
  uint32_t fraction_scale = 1UL;
  uint8_t decimal_seen = 0U;
  uint8_t digit_seen = 0U;

  *valid = 0U;
  *has_decimal = 0U;

  for (uint8_t i = 0U; i < spectrum_snapshot.ui_input_len; i++)
  {
    char ch = spectrum_snapshot.ui_input[i];

    if ((ch >= '0') && (ch <= '9'))
    {
      digit_seen = 1U;
      if (decimal_seen == 0U)
      {
        integer_part = (integer_part * 10UL) + (uint32_t)(ch - '0');
      }
      else if (fraction_scale < 1000UL)
      {
        fraction_part = (fraction_part * 10UL) + (uint32_t)(ch - '0');
        fraction_scale *= 10UL;
      }
    }
    else if ((ch == '.') && (decimal_seen == 0U))
    {
      decimal_seen = 1U;
    }
    else
    {
      return 0UL;
    }
  }

  if (digit_seen == 0U)
  {
    return 0UL;
  }

  while (fraction_scale < 1000UL)
  {
    fraction_part *= 10UL;
    fraction_scale *= 10UL;
  }

  *valid = 1U;
  *has_decimal = decimal_seen;
  return (integer_part * 1000UL) + fraction_part;
}

static int32_t Spectrum_ParseSignedInputX1000(uint8_t *valid, uint8_t *has_decimal)
{
  uint8_t negative = 0U;
  uint8_t original_len = spectrum_snapshot.ui_input_len;
  uint32_t value;

  if ((spectrum_snapshot.ui_input_len > 0U) && (spectrum_snapshot.ui_input[0] == '-'))
  {
    negative = 1U;
    for (uint8_t i = 0U; i < (uint8_t)(spectrum_snapshot.ui_input_len - 1U); i++)
    {
      spectrum_snapshot.ui_input[i] = spectrum_snapshot.ui_input[(uint8_t)(i + 1U)];
    }
    spectrum_snapshot.ui_input_len--;
  }

  value = Spectrum_ParseInputX1000(valid, has_decimal);

  if (negative != 0U)
  {
    for (uint8_t i = spectrum_snapshot.ui_input_len; i > 0U; i--)
    {
      spectrum_snapshot.ui_input[i] = spectrum_snapshot.ui_input[(uint8_t)(i - 1U)];
    }
    spectrum_snapshot.ui_input[0] = '-';
    spectrum_snapshot.ui_input_len = original_len;
  }

  if (*valid == 0U)
  {
    return 0;
  }

  return (negative != 0U) ? -(int32_t)value : (int32_t)value;
}

static uint32_t Spectrum_ParseFrequencyHz(uint32_t value_x1000, uint8_t has_decimal)
{
  uint32_t integer_part = value_x1000 / 1000UL;
  uint32_t frequency_hz;

  if ((has_decimal == 0U) && (integer_part >= 1000UL))
  {
    frequency_hz = integer_part;
  }
  else
  {
    frequency_hz = value_x1000 * 1000UL;
  }

  if (frequency_hz < 1UL)
  {
    frequency_hz = 1UL;
  }

  return frequency_hz;
}

static uint32_t Spectrum_MaxFrequencyHz(uint8_t waveform)
{
  return (waveform == 0U) ? 20000000UL : 4000000UL;
}

static void Spectrum_ClampWaveFrequency(SpectrumWaveConfig *wave)
{
  uint32_t max_hz;

  if (wave == 0)
  {
    return;
  }

  max_hz = Spectrum_MaxFrequencyHz(wave->waveform);
  if (wave->frequency_hz < 1UL)
  {
    wave->frequency_hz = 1UL;
  }
  else if (wave->frequency_hz > max_hz)
  {
    wave->frequency_hz = max_hz;
  }
}

static uint16_t Spectrum_NormalizePhaseDeg(int32_t phase_deg)
{
  int32_t normalized = phase_deg % 360;

  if (normalized < 0)
  {
    normalized += 360;
  }

  return (uint16_t)normalized;
}

static void Spectrum_MoveFocusHorizontal(int8_t step)
{
  uint8_t first;
  uint8_t count;
  int8_t offset;

  if (spectrum_snapshot.ui_focus < 6U)
  {
    first = 0U;
    count = 6U;
  }
  else
  {
    first = 6U;
    count = 5U;
  }

  offset = (int8_t)(spectrum_snapshot.ui_focus - first);
  offset += step;
  if (offset < 0)
  {
    offset = (int8_t)(count - 1U);
  }
  else if (offset >= (int8_t)count)
  {
    offset = 0;
  }

  spectrum_snapshot.ui_focus = (uint8_t)(first + (uint8_t)offset);
}

static void Spectrum_MoveFocusVertical(void)
{
  uint8_t focus = spectrum_snapshot.ui_focus;

  if (focus < 6U)
  {
    focus = (uint8_t)(focus + 6U);
    if (focus >= SPECTRUM_SUM_FIELD_COUNT)
    {
      focus = (uint8_t)(SPECTRUM_SUM_FIELD_COUNT - 1U);
    }
  }
  else
  {
    focus = (uint8_t)(focus - 6U);
  }

  spectrum_snapshot.ui_focus = focus;
}

static void Spectrum_SelectWave(uint8_t wave)
{
  if (wave >= SPECTRUM_SUM_MAX_WAVES)
  {
    wave = (uint8_t)(SPECTRUM_SUM_MAX_WAVES - 1U);
  }
  spectrum_snapshot.selected_wave = wave;
  if (spectrum_snapshot.wave_count <= wave)
  {
    spectrum_snapshot.wave_count = (uint8_t)(wave + 1U);
  }
}

static uint32_t Spectrum_DdsAmplitudeMvpp(uint16_t amplitude_code)
{
  uint32_t mvpp = ((uint32_t)amplitude_code * 780UL + 4095UL) / 8191UL;

  if (mvpp > 780UL)
  {
    mvpp = 780UL;
  }

  return mvpp;
}

static void Spectrum_UpdateDdsSine(void)
{
  uint8_t count = spectrum_snapshot.wave_count;

  if (count > SPECTRUM_SUM_MAX_WAVES)
  {
    count = SPECTRUM_SUM_MAX_WAVES;
  }

  for (uint8_t i = 0U; i < count; i++)
  {
    SpectrumWaveConfig *wave = &spectrum_snapshot.waves[i];

    if ((wave->enable != 0U) && (wave->waveform == 0U))
    {
      dds_output_sine(wave->frequency_hz,
                      dds_factor,
                      Spectrum_DdsAmplitudeMvpp(wave->amplitude_code));
      return;
    }
  }

  dds_output_sine(1000UL, dds_factor, 0UL);
}

static void Spectrum_SetLcrTestSignal(void)
{
  dds_output_sine(SPECTRUM_LCR_TEST_FREQ_HZ,
                  dds_factor,
                  Spectrum_DdsAmplitudeMvpp(SPECTRUM_LCR_TEST_AMP_CODE));
}

static uint32_t Spectrum_LcrDdsFtw(uint32_t frequency_hz)
{
  const uint64_t dds_clock_hz = 1000000000ULL;
  return (uint32_t)((((uint64_t)frequency_hz << 32) + (dds_clock_hz / 2ULL)) /
                    dds_clock_hz);
}

static void Spectrum_CommitInput(void)
{
  uint8_t valid;
  uint8_t has_decimal;
  uint32_t value_x1000 = Spectrum_ParseInputX1000(&valid, &has_decimal);
  uint32_t value = (value_x1000 + 500UL) / 1000UL;
  int32_t signed_value_x1000;
  int32_t signed_value;
  SpectrumWaveConfig *wave = &spectrum_snapshot.waves[spectrum_snapshot.selected_wave];

  if ((valid == 0U) &&
      (spectrum_snapshot.ui_focus != 4U) &&
      (spectrum_snapshot.ui_focus != 10U))
  {
    Spectrum_CancelEdit();
    return;
  }

  switch (spectrum_snapshot.ui_focus)
  {
    case 0U:
      if (value < 1UL) { value = 1UL; }
      if (value > SPECTRUM_SUM_MAX_WAVES) { value = SPECTRUM_SUM_MAX_WAVES; }
      spectrum_snapshot.wave_count = (uint8_t)value;
      if (spectrum_snapshot.selected_wave >= spectrum_snapshot.wave_count)
      {
        spectrum_snapshot.selected_wave = (uint8_t)(spectrum_snapshot.wave_count - 1U);
      }
      break;
    case 1U:
      spectrum_snapshot.channel_id = (value != 0UL) ? 1U : 0U;
      break;
    case 2U:
      if (value > 0UL) { value--; }
      Spectrum_SelectWave((uint8_t)value);
      break;
    case 3U:
      wave->frequency_hz = Spectrum_ParseFrequencyHz(value_x1000, has_decimal);
      Spectrum_ClampWaveFrequency(wave);
      break;
    case 4U:
      signed_value_x1000 = Spectrum_ParseSignedInputX1000(&valid, &has_decimal);
      if (valid == 0U)
      {
        Spectrum_CancelEdit();
        return;
      }
      signed_value = (signed_value_x1000 >= 0) ?
                     ((signed_value_x1000 + 500) / 1000) :
                     ((signed_value_x1000 - 500) / 1000);
      wave->phase_deg = Spectrum_NormalizePhaseDeg(signed_value);
      break;
    case 5U:
      if (value > 8191UL) { value = 8191UL; }
      wave->amplitude_code = (uint16_t)value;
      break;
    case 6U:
      if (value > 8191UL) { value = 8191UL; }
      wave->offset_code = (int16_t)value;
      break;
    case 7U:
      if (value <= 100UL)
      {
        wave->duty_code = (uint16_t)((value * 65535UL + 50UL) / 100UL);
      }
      else
      {
        if (value > 65535UL) { value = 65535UL; }
        wave->duty_code = (uint16_t)value;
      }
      break;
    case 8U:
      if (value > 3UL) { value = 3UL; }
      wave->waveform = (uint8_t)value;
      Spectrum_ClampWaveFrequency(wave);
      break;
    case 9U:
      wave->enable = (value != 0UL) ? 1U : 0U;
      break;
    case 10U:
      signed_value_x1000 = Spectrum_ParseSignedInputX1000(&valid, &has_decimal);
      if (valid == 0U)
      {
        Spectrum_CancelEdit();
        return;
      }
      signed_value = (signed_value_x1000 >= 0) ?
                     ((signed_value_x1000 + 500) / 1000) :
                     ((signed_value_x1000 - 500) / 1000);
      if (signed_value > 5000)
      {
        signed_value = 5000;
      }
      else if (signed_value < -5000)
      {
        signed_value = -5000;
      }
      spectrum_snapshot.output_bias_mv = (int16_t)signed_value;
      break;
    default:
      break;
  }

  spectrum_snapshot.apply_counter++;
  spectrum_snapshot.state = SPECTRUM_HOST_READY;
  Spectrum_UpdateDdsSine();
  Spectrum_CancelEdit();
}

static void Spectrum_SendStatus(void)
{
  uint8_t payload[128];
  uint8_t pos = 0U;

  payload[pos++] = spectrum_snapshot.mode;
  payload[pos++] = (uint8_t)spectrum_snapshot.state;
  payload[pos++] = spectrum_snapshot.channel_id;
  payload[pos++] = spectrum_snapshot.wave_count;
  payload[pos++] = spectrum_snapshot.selected_wave;
  payload[pos++] = spectrum_snapshot.ui_focus;
  payload[pos++] = spectrum_snapshot.ui_editing;
  payload[pos++] = spectrum_snapshot.ui_input_len;
  for (uint8_t i = 0U; i < SPECTRUM_UI_INPUT_MAX_LEN; i++)
  {
    payload[pos++] = (uint8_t)spectrum_snapshot.ui_input[i];
  }
  payload[pos++] = spectrum_snapshot.apply_counter;

  for (uint8_t i = 0U; i < SPECTRUM_SUM_MAX_WAVES; i++)
  {
    SpectrumWaveConfig *wave = &spectrum_snapshot.waves[i];
    Spectrum_WriteU32(payload, &pos, wave->frequency_hz);
    Spectrum_WriteU16(payload, &pos, wave->phase_deg);
    Spectrum_WriteU16(payload, &pos, wave->amplitude_code);
    Spectrum_WriteI16(payload, &pos, wave->offset_code);
    Spectrum_WriteU16(payload, &pos, wave->duty_code);
    payload[pos++] = wave->waveform;
    payload[pos++] = wave->enable;
  }
  Spectrum_WriteI16(payload, &pos, spectrum_snapshot.output_bias_mv);

  (void)BoardComm_Send(BOARD_COMM_CMD_SYS_STATUS, payload, pos);
}

static void Spectrum_LoadDefaults(void)
{
  spectrum_snapshot.mode = SPECTRUM_MODE_SUM_WAVEFORM;
  spectrum_snapshot.state = SPECTRUM_HOST_READY;
  spectrum_snapshot.channel_id = 0U;
  spectrum_snapshot.wave_count = 2U;
  spectrum_snapshot.selected_wave = 0U;
  spectrum_snapshot.ui_focus = 3U;
  spectrum_snapshot.ui_editing = 0U;
  spectrum_snapshot.apply_counter = 1U;
  spectrum_snapshot.output_bias_mv = 0;
  Spectrum_ClearInput();

  for (uint8_t i = 0U; i < SPECTRUM_SUM_MAX_WAVES; i++)
  {
    spectrum_snapshot.waves[i].frequency_hz = 1000000UL + ((uint32_t)i * 100000UL);
    spectrum_snapshot.waves[i].phase_deg = 0U;
    spectrum_snapshot.waves[i].amplitude_code = (i < 2U) ? 2048U : 0U;
    spectrum_snapshot.waves[i].offset_code = 0;
    spectrum_snapshot.waves[i].duty_code = 32768U;
    spectrum_snapshot.waves[i].waveform = (i < 2U) ? 1U : 0U;
    spectrum_snapshot.waves[i].enable = (i < 2U) ? 1U : 0U;
  }
}

void SpectrumSystem_Init(void)
{
  Spectrum_LoadDefaults();
  Spectrum_UpdateDdsSine();
  spectrum_last_status_tick = HAL_GetTick();
  Spectrum_SendStatus();
}

void SpectrumSystem_Task(void)
{
  uint32_t now = HAL_GetTick();

  AdcFftClient_Task();
  if ((now - spectrum_last_status_tick) >= 250UL)
  {
    spectrum_last_status_tick = now;
    Spectrum_SendStatus();
  }
}

void BoardComm_RxFrameCallback(uint8_t cmd, const uint8_t *data, uint8_t len, BoardComm_Status status)
{
  (void)cmd;
  (void)data;
  (void)len;
  (void)status;
}

void SpectrumSystem_OnKey(char key)
{
  if (spectrum_snapshot.ui_editing != 0U)
  {
    if (key == 'D')
    {
      Spectrum_CommitInput();
    }
    else if (key == 'B')
    {
      Spectrum_ClearInput();
    }
    else if (key == '*')
    {
      Spectrum_Backspace();
    }
    else if (key == 'A')
    {
      Spectrum_ToggleNegativeInput();
    }
    else
    {
      Spectrum_AppendInput(key);
    }
  }
  else if (key == 'A')
  {
    Spectrum_LoadDefaults();
    Spectrum_UpdateDdsSine();
  }
  else if (key == 'B')
  {
    spectrum_snapshot.state = SPECTRUM_HOST_READY;
    if (spectrum_snapshot.mode == SPECTRUM_MODE_LCR_TEST)
    {
      spectrum_snapshot.mode = SPECTRUM_MODE_SUM_WAVEFORM;
      Spectrum_UpdateDdsSine();
    }
    else
    {
      spectrum_snapshot.mode = SPECTRUM_MODE_LCR_TEST;
      Spectrum_SetLcrTestSignal();
    }
  }
  else if ((spectrum_snapshot.mode == SPECTRUM_MODE_LCR_TEST) && (key == 'D'))
  {
    Spectrum_SetLcrTestSignal();
    (void)AdcFftClient_RequestMeasurement(SPECTRUM_LCR_TEST_FREQ_HZ,
                                          Spectrum_LcrDdsFtw(SPECTRUM_LCR_TEST_FREQ_HZ),
                                          1000UL);
    spectrum_snapshot.apply_counter++;
    spectrum_snapshot.state = SPECTRUM_HOST_SENT;
  }
  else if (spectrum_snapshot.mode == SPECTRUM_MODE_LCR_TEST)
  {
  }
  else if (key == '4')
  {
    Spectrum_MoveFocusHorizontal(-1);
  }
  else if (key == '6')
  {
    Spectrum_MoveFocusHorizontal(1);
  }
  else if (key == '8')
  {
    Spectrum_MoveFocusVertical();
  }
  else if (key == '2')
  {
    Spectrum_MoveFocusVertical();
  }
  else if (key == 'D')
  {
    Spectrum_StartEdit();
  }

  Spectrum_SendStatus();
}

SpectrumHostSnapshot SpectrumSystem_GetSnapshot(void)
{
  return spectrum_snapshot;
}
