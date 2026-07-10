#include "SpectrumSystem_User.h"

#include "ADF4351_User.h"
#include "AGC_Controller_User.h"
#include "BoardComm_User.h"

static SpectrumHostSnapshot spectrum_snapshot;
static uint16_t spectrum_values_mv[SPECTRUM_POINT_COUNT];
static uint32_t spectrum_sweep_time_ms = 3000UL;
static uint32_t spectrum_last_step_tick = 0UL;
static uint32_t spectrum_sweep_start_tick = 0UL;
static uint32_t spectrum_adc_request_tick = 0UL;
static uint8_t spectrum_sweep_started = 0U;
static uint8_t spectrum_point_phase = 0U;
static uint8_t spectrum_adc_retry_count = 0U;
static uint8_t spectrum_lock_demo_high = 0U;

typedef enum {
  SPECTRUM_UI_FIELD_SWEEP_TIME = 0,
  SPECTRUM_UI_FIELD_OUTPUT_MV = 1,
  SPECTRUM_UI_FIELD_FIXED_FREQ = 2,
  SPECTRUM_UI_FIELD_COUNT
} SpectrumUiField;

static volatile uint8_t spectrum_adc_sample_pending = 0U;
static volatile uint16_t spectrum_adc_sample_point = 0U;
static volatile uint16_t spectrum_adc_sample_mv = 0U;
static volatile int16_t spectrum_adc_sample_dbm_x10 = 0;
static volatile uint8_t spectrum_adc_sample_valid = 0U;

volatile uint32_t Spectrum_Debug_AdcRespCount = 0U;
volatile uint32_t Spectrum_Debug_AdcTimeoutCount = 0U;
volatile uint16_t Spectrum_Debug_LastAdcMv = 0U;

static void Spectrum_StartPllSweep(void);
static void Spectrum_SetPllFrequency(uint32_t frequency_khz, uint16_t point_index);

static void Spectrum_WriteU16(uint8_t *buf, uint8_t *pos, uint16_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFU);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void Spectrum_WriteU32(uint8_t *buf, uint8_t *pos, uint32_t value)
{
  buf[(*pos)++] = (uint8_t)(value & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  buf[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint16_t Spectrum_ReadU16(const uint8_t *buf, uint8_t *pos)
{
  uint16_t value = (uint16_t)buf[*pos];
  value |= (uint16_t)buf[(uint8_t)(*pos + 1U)] << 8;
  *pos = (uint8_t)(*pos + 2U);
  return value;
}

static uint8_t Spectrum_IsDigit(char key)
{
  return ((key >= '0') && (key <= '9')) ? 1U : 0U;
}

static void Spectrum_UiClearInput(void)
{
  spectrum_snapshot.ui_input_len = 0U;
  spectrum_snapshot.ui_input[0] = '\0';
}

static void Spectrum_UiStartEdit(void)
{
  spectrum_snapshot.ui_editing = 1U;
  Spectrum_UiClearInput();
}

static void Spectrum_UiCancelEdit(void)
{
  spectrum_snapshot.ui_editing = 0U;
  Spectrum_UiClearInput();
}

static uint8_t Spectrum_UiInputHasDecimal(void)
{
  for (uint8_t i = 0U; i < spectrum_snapshot.ui_input_len; i++)
  {
    if (spectrum_snapshot.ui_input[i] == '.')
    {
      return 1U;
    }
  }
  return 0U;
}

static void Spectrum_UiAppendInput(char key)
{
  if (spectrum_snapshot.ui_input_len >= SPECTRUM_UI_INPUT_MAX_LEN)
  {
    return;
  }

  if (key == '#')
  {
    if (Spectrum_UiInputHasDecimal() != 0U)
    {
      return;
    }
    key = '.';
  }
  else if (Spectrum_IsDigit(key) == 0U)
  {
    return;
  }

  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len++] = key;
  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len] = '\0';
}

static void Spectrum_UiBackspace(void)
{
  if (spectrum_snapshot.ui_input_len == 0U)
  {
    return;
  }

  spectrum_snapshot.ui_input_len--;
  spectrum_snapshot.ui_input[spectrum_snapshot.ui_input_len] = '\0';
}

static uint32_t Spectrum_UiParseInputX1000(uint8_t *valid, uint8_t *has_decimal)
{
  uint32_t integer_part = 0UL;
  uint32_t fraction_part = 0UL;
  uint32_t fraction_scale = 1UL;
  uint8_t decimal_seen = 0U;
  uint8_t digit_seen = 0U;

  if (valid != 0)
  {
    *valid = 0U;
  }
  if (has_decimal != 0)
  {
    *has_decimal = 0U;
  }

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

  if (valid != 0)
  {
    *valid = 1U;
  }
  if (has_decimal != 0)
  {
    *has_decimal = decimal_seen;
  }
  return (integer_part * 1000UL) + fraction_part;
}

static void Spectrum_UiMoveFocus(int8_t step)
{
  int8_t focus = (int8_t)spectrum_snapshot.ui_focus + step;

  if (focus < 0)
  {
    focus = (int8_t)SPECTRUM_UI_FIELD_COUNT - 1;
  }
  else if (focus >= (int8_t)SPECTRUM_UI_FIELD_COUNT)
  {
    focus = 0;
  }

  spectrum_snapshot.ui_focus = (uint8_t)focus;
}

static void Spectrum_SetSweepTimeMs(uint32_t sweep_time_ms)
{
  if (sweep_time_ms < SPECTRUM_SWEEP_TIME_MIN_MS)
  {
    sweep_time_ms = SPECTRUM_SWEEP_TIME_MIN_MS;
  }
  else if (sweep_time_ms > SPECTRUM_SWEEP_TIME_MAX_MS)
  {
    sweep_time_ms = SPECTRUM_SWEEP_TIME_MAX_MS;
  }

  spectrum_sweep_time_ms = sweep_time_ms;
  spectrum_snapshot.sweep_time_s = (uint8_t)((sweep_time_ms + 500UL) / 1000UL);
  spectrum_snapshot.ui_sweep_time_ms = (uint16_t)sweep_time_ms;
}

static void Spectrum_UiCommitInput(void)
{
  uint8_t valid;
  uint8_t has_decimal;
  uint32_t value_x1000 = Spectrum_UiParseInputX1000(&valid, &has_decimal);

  if (valid == 0U)
  {
    Spectrum_UiCancelEdit();
    return;
  }

  if (spectrum_snapshot.ui_focus == SPECTRUM_UI_FIELD_SWEEP_TIME)
  {
    Spectrum_SetSweepTimeMs(value_x1000);
    if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_SWEEP_AGC)
    {
      Spectrum_StartPllSweep();
    }
  }
  else if (spectrum_snapshot.ui_focus == SPECTRUM_UI_FIELD_OUTPUT_MV)
  {
    uint32_t target_mv = (value_x1000 + 500UL) / 1000UL;
    if (target_mv < AGC_TARGET_MIN_MV)
    {
      target_mv = AGC_TARGET_MIN_MV;
    }
    else if (target_mv > AGC_TARGET_MAX_MV)
    {
      target_mv = AGC_TARGET_MAX_MV;
    }
    AGC_Controller_SetTarget((uint16_t)target_mv);
  }
  else
  {
    uint32_t frequency_khz;

    if ((has_decimal == 0U) && ((value_x1000 / 1000UL) >= 1000UL))
    {
      frequency_khz = value_x1000 / 1000UL;
    }
    else
    {
      frequency_khz = value_x1000;
    }

    if (frequency_khz < PLL_SWEEP_START_KHZ)
    {
      frequency_khz = PLL_SWEEP_START_KHZ;
    }
    else if (frequency_khz > PLL_SWEEP_STOP_KHZ)
    {
      frequency_khz = PLL_SWEEP_STOP_KHZ;
    }

    spectrum_snapshot.fixed_frequency_khz = frequency_khz;
    if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_FIXED_AGC)
    {
      Spectrum_SetPllFrequency(frequency_khz, 0U);
    }
  }

  Spectrum_UiCancelEdit();
}

static uint32_t Spectrum_AnalyzerIndexToRfKHz(uint16_t index)
{
  return SPECTRUM_RF_START_KHZ + ((uint32_t)index * SPECTRUM_STEP_KHZ);
}

static uint32_t Spectrum_RfToLoKHz(uint32_t rf_khz)
{
  return rf_khz + SPECTRUM_IF_KHZ;
}

static uint32_t Spectrum_PllIndexToKHz(uint16_t index)
{
  return PLL_SWEEP_START_KHZ + ((uint32_t)index * PLL_SWEEP_STEP_KHZ);
}

static void Spectrum_UpdateAgcSnapshot(void)
{
  AGC_ControllerState agc = AGC_Controller_GetState();

  spectrum_snapshot.agc_target_mv = agc.target_mv;
  spectrum_snapshot.agc_control_mv = agc.control_mv;
  spectrum_snapshot.agc_output_connected = agc.output_connected;
}

static void Spectrum_SendAdcSampleRequest(void)
{
  uint8_t payload[10];
  uint8_t pos = 0U;

  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.point_index);
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.rf_khz);
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.lo_khz);
  (void)BoardComm_Send(BOARD_COMM_CMD_ADC_SAMPLE_REQ, payload, pos);
  spectrum_adc_request_tick = HAL_GetTick();
}

static void Spectrum_SendStatus(void)
{
  uint8_t payload[48];
  uint8_t pos = 0U;

  Spectrum_UpdateAgcSnapshot();
  payload[pos++] = (uint8_t)spectrum_snapshot.mode;
  payload[pos++] = (uint8_t)spectrum_snapshot.state;
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.rf_khz);
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.lo_khz);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.point_index);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.amplitude_mv);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.peak_index);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.peak_amplitude_mv);
  payload[pos++] = spectrum_snapshot.spur_count;
  payload[pos++] = spectrum_snapshot.pll_locked;
  payload[pos++] = spectrum_snapshot.sweep_time_s;
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.active_point_count);
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.fixed_frequency_khz);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.agc_target_mv);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.agc_control_mv);
  payload[pos++] = spectrum_snapshot.agc_output_connected;
  payload[pos++] = spectrum_snapshot.ui_focus;
  payload[pos++] = spectrum_snapshot.ui_editing;
  payload[pos++] = spectrum_snapshot.ui_input_len;
  for (uint8_t i = 0U; i < SPECTRUM_UI_INPUT_MAX_LEN; i++)
  {
    payload[pos++] = (uint8_t)spectrum_snapshot.ui_input[i];
  }
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.ui_sweep_time_ms);
  (void)BoardComm_Send(BOARD_COMM_CMD_SYS_STATUS, payload, pos);
}

static void Spectrum_SendPoint(void)
{
  uint8_t payload[9];
  uint8_t pos = 0U;

  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.point_index);
  Spectrum_WriteU32(payload, &pos, spectrum_snapshot.rf_khz);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.amplitude_mv);
  payload[pos++] = spectrum_snapshot.pll_locked;
  (void)BoardComm_Send(BOARD_COMM_CMD_SWEEP_POINT, payload, pos);
}

static void Spectrum_SendResult(void)
{
  uint8_t payload[5];
  uint8_t pos = 0U;

  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.peak_index);
  Spectrum_WriteU16(payload, &pos, spectrum_snapshot.peak_amplitude_mv);
  payload[pos++] = spectrum_snapshot.spur_count;
  (void)BoardComm_Send(BOARD_COMM_CMD_SWEEP_RESULT, payload, pos);
}

static void Spectrum_UpdatePeakAndSpur(void)
{
  uint16_t peak = 0U;
  uint16_t peak_value = 0U;
  uint8_t spur_count = 0U;

  for (uint16_t i = 0U; i < SPECTRUM_POINT_COUNT; i++)
  {
    if (spectrum_values_mv[i] > peak_value)
    {
      peak_value = spectrum_values_mv[i];
      peak = i;
    }
  }

  if (peak_value != 0U)
  {
    for (uint16_t i = 0U; i < SPECTRUM_POINT_COUNT; i++)
    {
      /* Exclude the main-frequency bin. Every other bin whose amplitude is
       * strictly greater than 2% of the main peak is counted as a spur. */
      if ((i != peak) &&
          (((uint32_t)spectrum_values_mv[i] * 100UL) >
           ((uint32_t)peak_value * SPECTRUM_SPUR_THRESHOLD_PERCENT)) &&
          (spur_count < 255U))
      {
        spur_count++;
      }
    }
  }

  spectrum_snapshot.peak_index = peak;
  spectrum_snapshot.peak_amplitude_mv = peak_value;
  spectrum_snapshot.spur_count = spur_count;
}

static void Spectrum_SetAnalyzerPoint(uint16_t index)
{
  uint32_t rf_khz = Spectrum_AnalyzerIndexToRfKHz(index);
  uint32_t lo_khz = Spectrum_RfToLoKHz(rf_khz);

  spectrum_snapshot.point_index = index;
  spectrum_snapshot.rf_khz = rf_khz;
  spectrum_snapshot.lo_khz = lo_khz;
  spectrum_snapshot.pll_locked = 1U;
  ADF4351_SetFreq(lo_khz / 100UL);
}

static void Spectrum_SetPllFrequency(uint32_t frequency_khz, uint16_t point_index)
{
  spectrum_snapshot.point_index = point_index;
  spectrum_snapshot.rf_khz = frequency_khz;
  spectrum_snapshot.lo_khz = frequency_khz;
  spectrum_snapshot.pll_locked = 1U;
  ADF4351_SetFreq(frequency_khz / 100UL);
}

static void Spectrum_StartAnalyzer(void)
{
  for (uint16_t i = 0U; i < SPECTRUM_POINT_COUNT; i++)
  {
    spectrum_values_mv[i] = 0U;
  }

  spectrum_snapshot.mode = SPECTRUM_MODE_ANALYZER;
  spectrum_snapshot.state = SPECTRUM_HOST_SWEEPING;
  spectrum_snapshot.active_point_count = SPECTRUM_POINT_COUNT;
  spectrum_snapshot.point_index = 0U;
  spectrum_snapshot.peak_index = 0U;
  spectrum_snapshot.peak_amplitude_mv = 0U;
  spectrum_snapshot.spur_count = 0U;
  spectrum_sweep_started = 1U;
  spectrum_point_phase = 0U;
  spectrum_adc_retry_count = 0U;
  spectrum_last_step_tick = 0UL;
  spectrum_adc_sample_pending = 0U;
}

static void Spectrum_StartPllSweep(void)
{
  spectrum_snapshot.mode = SPECTRUM_MODE_PLL_SWEEP_AGC;
  spectrum_snapshot.state = SPECTRUM_HOST_SWEEPING;
  spectrum_snapshot.active_point_count = PLL_SWEEP_POINT_COUNT;
  spectrum_snapshot.point_index = 0U;
  spectrum_snapshot.peak_index = 0U;
  spectrum_snapshot.peak_amplitude_mv = 0U;
  spectrum_snapshot.spur_count = 0U;
  spectrum_sweep_start_tick = HAL_GetTick();
  spectrum_last_step_tick = 0UL;
  spectrum_sweep_started = 1U;
  Spectrum_SetPllFrequency(PLL_SWEEP_START_KHZ, 0U);
  Spectrum_SendAdcSampleRequest();
  Spectrum_SendStatus();
}

static void Spectrum_EnterFixedMode(void)
{
  spectrum_snapshot.mode = SPECTRUM_MODE_PLL_FIXED_AGC;
  spectrum_snapshot.state = SPECTRUM_HOST_SWEEPING;
  spectrum_snapshot.active_point_count = 1U;
  spectrum_snapshot.point_index = 0U;
  spectrum_sweep_started = 0U;
  spectrum_last_step_tick = HAL_GetTick();
  Spectrum_SetPllFrequency(spectrum_snapshot.fixed_frequency_khz, 0U);
  Spectrum_SendAdcSampleRequest();
  Spectrum_SendStatus();
}

static void Spectrum_EnterLockDemoMode(void)
{
  spectrum_snapshot.mode = SPECTRUM_MODE_PLL_LOCK_DEMO;
  spectrum_snapshot.state = SPECTRUM_HOST_SWEEPING;
  spectrum_snapshot.active_point_count = 2U;
  spectrum_snapshot.point_index = 0U;
  spectrum_sweep_started = 0U;
  spectrum_lock_demo_high = 0U;
  spectrum_last_step_tick = HAL_GetTick();
  Spectrum_SetPllFrequency(PLL_SWEEP_START_KHZ, 0U);
  Spectrum_SendStatus();
}

static void Spectrum_FinishAnalyzerPoint(uint16_t amplitude_mv)
{
  spectrum_snapshot.amplitude_mv = amplitude_mv;
  spectrum_values_mv[spectrum_snapshot.point_index] = amplitude_mv;

  if (amplitude_mv > spectrum_snapshot.peak_amplitude_mv)
  {
    spectrum_snapshot.peak_amplitude_mv = amplitude_mv;
    spectrum_snapshot.peak_index = spectrum_snapshot.point_index;
  }

  Spectrum_SendPoint();
  if ((spectrum_snapshot.point_index % 10U) == 0U)
  {
    Spectrum_SendStatus();
  }

  if (spectrum_snapshot.point_index >= (SPECTRUM_POINT_COUNT - 1U))
  {
    Spectrum_UpdatePeakAndSpur();
    spectrum_snapshot.state = SPECTRUM_HOST_DONE;
    spectrum_sweep_started = 0U;
    spectrum_point_phase = 0U;
    Spectrum_SendStatus();
    Spectrum_SendResult();
  }
  else
  {
    spectrum_snapshot.point_index++;
    spectrum_point_phase = 0U;
  }
}

static void Spectrum_AnalyzerTask(uint32_t now)
{
  uint32_t dwell_ms = spectrum_sweep_time_ms / SPECTRUM_POINT_COUNT;
  uint8_t has_sample = 0U;
  uint16_t sample_point = 0U;
  uint16_t sample_mv = 0U;
  uint8_t sample_valid = 0U;

  if (dwell_ms == 0UL)
  {
    dwell_ms = 1UL;
  }

  if (spectrum_snapshot.state != SPECTRUM_HOST_SWEEPING)
  {
    return;
  }

  if (spectrum_point_phase == 0U)
  {
    if ((spectrum_last_step_tick != 0UL) && ((now - spectrum_last_step_tick) < dwell_ms))
    {
      return;
    }
    spectrum_last_step_tick = now;
    Spectrum_SetAnalyzerPoint(spectrum_snapshot.point_index);
    spectrum_adc_retry_count = 0U;
    spectrum_point_phase = 1U;
    return;
  }

  if (spectrum_point_phase == 1U)
  {
    if ((now - spectrum_last_step_tick) < SPECTRUM_ADC_SETTLE_MS)
    {
      return;
    }
    Spectrum_SendAdcSampleRequest();
    spectrum_point_phase = 2U;
    return;
  }

  __disable_irq();
  if (spectrum_adc_sample_pending != 0U)
  {
    has_sample = 1U;
    sample_point = spectrum_adc_sample_point;
    sample_mv = spectrum_adc_sample_mv;
    sample_valid = spectrum_adc_sample_valid;
    spectrum_adc_sample_pending = 0U;
  }
  __enable_irq();

  if ((has_sample != 0U) && (sample_point == spectrum_snapshot.point_index))
  {
    Spectrum_Debug_LastAdcMv = sample_mv;
    Spectrum_FinishAnalyzerPoint((sample_valid != 0U) ? sample_mv : 0U);
  }
  else if ((now - spectrum_adc_request_tick) >= SPECTRUM_ADC_TIMEOUT_MS)
  {
    if (spectrum_adc_retry_count < SPECTRUM_ADC_MAX_RETRIES)
    {
      spectrum_adc_retry_count++;
      Spectrum_SendAdcSampleRequest();
    }
    else
    {
      Spectrum_Debug_AdcTimeoutCount++;
      Spectrum_FinishAnalyzerPoint(0U);
    }
  }
}

static void Spectrum_PllSweepTask(uint32_t now)
{
  uint32_t elapsed = now - spectrum_sweep_start_tick;
  uint16_t next_index;

  if (elapsed >= spectrum_sweep_time_ms)
  {
    Spectrum_SetPllFrequency(PLL_SWEEP_STOP_KHZ, (PLL_SWEEP_POINT_COUNT - 1U));
    Spectrum_SendPoint();
    spectrum_sweep_start_tick = now;
    spectrum_last_step_tick = 0UL;
    return;
  }

  next_index = (uint16_t)(((uint64_t)elapsed * (PLL_SWEEP_POINT_COUNT - 1U)) / spectrum_sweep_time_ms);
  if ((spectrum_last_step_tick == 0UL) || (next_index != spectrum_snapshot.point_index))
  {
    Spectrum_SetPllFrequency(Spectrum_PllIndexToKHz(next_index), next_index);
    spectrum_last_step_tick = now;
    Spectrum_SendAdcSampleRequest();
    Spectrum_SendPoint();
    if ((next_index % 20U) == 0U)
    {
      Spectrum_SendStatus();
    }
  }
}

static void Spectrum_FixedTask(uint32_t now)
{
  if ((now - spectrum_last_step_tick) >= PLL_AGC_SAMPLE_PERIOD_MS)
  {
    spectrum_last_step_tick = now;
    Spectrum_SendAdcSampleRequest();
  }
}

static void Spectrum_LockDemoTask(uint32_t now)
{
  if ((now - spectrum_last_step_tick) < PLL_LOCK_DEMO_PERIOD_MS)
  {
    return;
  }

  spectrum_last_step_tick = now;
  spectrum_lock_demo_high ^= 1U;
  if (spectrum_lock_demo_high != 0U)
  {
    Spectrum_SetPllFrequency(PLL_SWEEP_STOP_KHZ, 1U);
  }
  else
  {
    Spectrum_SetPllFrequency(PLL_SWEEP_START_KHZ, 0U);
  }
  Spectrum_SendStatus();
}

void SpectrumSystem_Init(void)
{
  AGC_Controller_Init();
  spectrum_snapshot.ui_focus = SPECTRUM_UI_FIELD_SWEEP_TIME;
  spectrum_snapshot.ui_editing = 0U;
  Spectrum_UiClearInput();
  Spectrum_SetSweepTimeMs(3000UL);
  spectrum_snapshot.fixed_frequency_khz = PLL_FIXED_DEFAULT_KHZ;
  spectrum_snapshot.amplitude_mv = 0U;
  spectrum_snapshot.pll_locked = 0U;
  Spectrum_UpdateAgcSnapshot();
  Spectrum_StartAnalyzer();
}

void SpectrumSystem_Task(void)
{
  uint32_t now = HAL_GetTick();

  if (spectrum_snapshot.mode == SPECTRUM_MODE_ANALYZER)
  {
    Spectrum_AnalyzerTask(now);
  }
  else if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_SWEEP_AGC)
  {
    Spectrum_PllSweepTask(now);
  }
  else if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_FIXED_AGC)
  {
    Spectrum_FixedTask(now);
  }
  else
  {
    Spectrum_LockDemoTask(now);
  }
}

void BoardComm_RxFrameCallback(uint8_t cmd, const uint8_t *data, uint8_t len, BoardComm_Status status)
{
  uint8_t pos = 0U;
  uint16_t point;
  uint16_t sample_mv;
  uint8_t valid;

  if ((status != BOARD_COMM_OK) || (cmd != BOARD_COMM_CMD_ADC_SAMPLE_RESP) || (data == 0) || (len < 7U))
  {
    return;
  }

  point = Spectrum_ReadU16(data, &pos);
  sample_mv = Spectrum_ReadU16(data, &pos);
  spectrum_adc_sample_dbm_x10 = (int16_t)Spectrum_ReadU16(data, &pos);
  valid = data[pos++];
  Spectrum_Debug_AdcRespCount++;
  Spectrum_Debug_LastAdcMv = sample_mv;

  if (spectrum_snapshot.mode == SPECTRUM_MODE_ANALYZER)
  {
    spectrum_adc_sample_point = point;
    spectrum_adc_sample_mv = sample_mv;
    spectrum_adc_sample_valid = valid;
    spectrum_adc_sample_pending = 1U;
  }
  else
  {
    spectrum_snapshot.amplitude_mv = (valid != 0U) ? sample_mv : 0U;
    AGC_Controller_ProcessSample(sample_mv, valid);
    Spectrum_UpdateAgcSnapshot();
  }
}

void SpectrumSystem_OnKey(char key)
{
  if (key == 'A')
  {
    Spectrum_UiCancelEdit();
    SpectrumMode next = (SpectrumMode)((spectrum_snapshot.mode + 1U) % SPECTRUM_MODE_COUNT);
    if (next == SPECTRUM_MODE_ANALYZER)
    {
      Spectrum_StartAnalyzer();
    }
    else if (next == SPECTRUM_MODE_PLL_SWEEP_AGC)
    {
      Spectrum_StartPllSweep();
    }
    else if (next == SPECTRUM_MODE_PLL_FIXED_AGC)
    {
      Spectrum_EnterFixedMode();
    }
    else
    {
      Spectrum_EnterLockDemoMode();
    }
  }
  else if (spectrum_snapshot.ui_editing != 0U)
  {
    if (key == 'D')
    {
      Spectrum_UiCommitInput();
    }
    else if (key == 'B')
    {
      Spectrum_UiClearInput();
    }
    else if (key == '*')
    {
      Spectrum_UiBackspace();
    }
    else
    {
      Spectrum_UiAppendInput(key);
    }
  }
  else if (key == 'B')
  {
    if (spectrum_snapshot.mode == SPECTRUM_MODE_ANALYZER)
    {
      Spectrum_StartAnalyzer();
    }
    else if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_SWEEP_AGC)
    {
      Spectrum_StartPllSweep();
    }
    else if (spectrum_snapshot.mode == SPECTRUM_MODE_PLL_FIXED_AGC)
    {
      Spectrum_EnterFixedMode();
    }
    else
    {
      Spectrum_EnterLockDemoMode();
    }
  }
  else if ((key == '4') || (key == '2'))
  {
    Spectrum_UiMoveFocus(-1);
  }
  else if ((key == '6') || (key == '8'))
  {
    Spectrum_UiMoveFocus(1);
  }
  else if (key == 'D')
  {
    Spectrum_UiStartEdit();
  }
  else if (Spectrum_IsDigit(key) != 0U)
  {
    Spectrum_UiStartEdit();
    Spectrum_UiAppendInput(key);
  }
  else if (key == '#')
  {
    Spectrum_UiStartEdit();
    Spectrum_UiAppendInput(key);
  }

  Spectrum_UpdateAgcSnapshot();
  Spectrum_SendStatus();
}

SpectrumHostSnapshot SpectrumSystem_GetSnapshot(void)
{
  return spectrum_snapshot;
}
