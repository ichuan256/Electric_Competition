#include "LcrCalibration_User.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BoardComm_User.h"
#include "LcrAuto_User.h"
#include "UsbCdc_User.h"

#define LCR_CAL_REFERENCE_DEFAULT_OHM       470.0
#define LCR_CAL_LOAD_DEFAULT_OHM            470.0
#define LCR_CAL_EXCITATION_MVPP             200U
#define LCR_CAL_SETTLE_US                   2000U
#define LCR_CAL_EXCITATION_TIMEOUT_MS       150UL
#define LCR_CAL_CAPTURE_TIMEOUT_MS          100UL
#define LCR_CAL_STANDARD_AVERAGES           4U
#define LCR_CAL_OPEN_AVERAGES               7U
#define LCR_CAL_ZERO_AVERAGES               8U
#define LCR_CAL_VERIFY_LIMIT                0.05
#define LCR_CAL_READY_PAYLOAD_LEN            12U
#define LCR_CAL_EXCITATION_PAYLOAD_LEN       16U

typedef enum {
  LCR_CAL_IDLE = 0,
  LCR_CAL_REQUEST_EXCITATION,
  LCR_CAL_WAIT_EXCITATION,
  LCR_CAL_SETTLING,
  LCR_CAL_START_CAPTURE,
  LCR_CAL_WAIT_CAPTURE
} LcrCalibrationState;

typedef struct {
  double reference_ohm;
  double load_ohm;
  double zero_v1_uv;
  double zero_v2_uv;
  LcrComplex short_z[LCR_CAL_POINT_COUNT];
  LcrComplex gain[LCR_CAL_POINT_COUNT];
  LcrComplex open_y[LCR_CAL_POINT_COUNT];
  uint32_t crc32;
} LcrCalibrationProfile;

static const uint32_t lcr_cal_frequencies[LCR_CAL_POINT_COUNT] = {
  100UL, 200UL, 500UL, 1000UL, 2000UL, 5000UL, 10000UL, 20000UL
};

static LcrCalibrationSnapshot lcr_cal_snapshot;
static LcrCalibrationProfile lcr_cal_staging;
static LcrCalibrationProfile lcr_cal_active;
static LcrCalibrationState lcr_cal_state;
static LcrComplex lcr_cal_sum;
static LcrComplex lcr_cal_open_samples[LCR_CAL_OPEN_AVERAGES];
static uint32_t lcr_cal_state_tick;
static uint16_t lcr_cal_request_id = 0x8000U;
static double lcr_cal_zero_v1_sum;
static double lcr_cal_zero_v2_sum;
static uint8_t lcr_cal_verify_failed;

static uint16_t LcrCal_ReadU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t LcrCal_ReadU32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void LcrCal_WriteU16(uint8_t *data, uint8_t *position, uint16_t value)
{
  data[(*position)++] = (uint8_t)value;
  data[(*position)++] = (uint8_t)(value >> 8);
}

static void LcrCal_WriteU32(uint8_t *data, uint8_t *position, uint32_t value)
{
  data[(*position)++] = (uint8_t)value;
  data[(*position)++] = (uint8_t)(value >> 8);
  data[(*position)++] = (uint8_t)(value >> 16);
  data[(*position)++] = (uint8_t)(value >> 24);
}

static LcrComplex LcrCal_Add(LcrComplex a, LcrComplex b)
{
  LcrComplex value = {a.real + b.real, a.imag + b.imag};
  return value;
}

static LcrComplex LcrCal_Subtract(LcrComplex a, LcrComplex b)
{
  LcrComplex value = {a.real - b.real, a.imag - b.imag};
  return value;
}

static LcrComplex LcrCal_Multiply(LcrComplex a, LcrComplex b)
{
  LcrComplex value = {
    (a.real * b.real) - (a.imag * b.imag),
    (a.real * b.imag) + (a.imag * b.real)
  };
  return value;
}

static uint8_t LcrCal_Divide(LcrComplex numerator, LcrComplex denominator,
                             LcrComplex *result)
{
  double power;
  if (result == 0)
  {
    return 0U;
  }
  power = (denominator.real * denominator.real) +
          (denominator.imag * denominator.imag);
  if (power <= 1.0e-24)
  {
    return 0U;
  }
  result->real = ((numerator.real * denominator.real) +
                  (numerator.imag * denominator.imag)) / power;
  result->imag = ((numerator.imag * denominator.real) -
                  (numerator.real * denominator.imag)) / power;
  return 1U;
}

static uint8_t LcrCal_Reciprocal(LcrComplex value, LcrComplex *result)
{
  LcrComplex one = {1.0, 0.0};
  return LcrCal_Divide(one, value, result);
}

static uint8_t LcrCal_ApplyProfile(const LcrCalibrationProfile *profile,
                                   uint8_t index, LcrComplex raw,
                                   LcrComplex *corrected)
{
  LcrComplex base;
  LcrComplex admittance;
  if ((profile == 0) || (corrected == 0) || (index >= LCR_CAL_POINT_COUNT))
  {
    return 0U;
  }
  base = LcrCal_Multiply(profile->gain[index],
                         LcrCal_Subtract(raw, profile->short_z[index]));
  if (LcrCal_Reciprocal(base, &admittance) == 0U)
  {
    return 0U;
  }
  admittance = LcrCal_Subtract(admittance, profile->open_y[index]);
  return LcrCal_Reciprocal(admittance, corrected);
}

static uint8_t LcrCal_FindFrequency(uint32_t frequency_hz, uint8_t *index)
{
  for (uint8_t i = 0U; i < LCR_CAL_POINT_COUNT; i++)
  {
    if (lcr_cal_frequencies[i] == frequency_hz)
    {
      if (index != 0) *index = i;
      return 1U;
    }
  }
  return 0U;
}

static const char *LcrCal_ResultText(LcrCalibrationResult result)
{
  static const char *const text[] = {
    "OK", "ARG", "ADC", "SIGNAL", "MATH", "VERIFY", "QUALITY", "BUSY"
  };
  return (result <= LCR_CAL_RESULT_BUSY) ? text[result] : "?";
}

static const char *LcrCal_StepText(LcrCalibrationStep step)
{
  static const char *const text[] = {
    "NONE", "ZERO", "SHORT", "LOAD", "OPEN", "VERIFY"
  };
  return (step <= LCR_CAL_STEP_VERIFY) ? text[step] : "?";
}

static void LcrCal_SetState(LcrCalibrationState state)
{
  lcr_cal_state = state;
  lcr_cal_state_tick = HAL_GetTick();
  lcr_cal_snapshot.revision++;
}

static void LcrCal_Fail(LcrCalibrationResult result)
{
  char line[64];
  lcr_cal_snapshot.last_result = result;
  lcr_cal_snapshot.busy = 0U;
  lcr_cal_state = LCR_CAL_IDLE;
  lcr_cal_snapshot.revision++;
  (void)snprintf(line, sizeof(line), "ERR %s STEP=%s STAGE=0x%02X",
                 LcrCal_ResultText(result), LcrCal_StepText(lcr_cal_snapshot.step),
                 lcr_cal_snapshot.stage);
  (void)UsbCdc_WriteLine(line);
}

static uint8_t LcrCal_SendExcitation(uint32_t frequency_hz, uint8_t enable)
{
  uint8_t payload[LCR_CAL_EXCITATION_PAYLOAD_LEN];
  uint8_t position = 0U;
  lcr_cal_request_id++;
  if (lcr_cal_request_id < 0x8000U) lcr_cal_request_id = 0x8000U;
  LcrCal_WriteU16(payload, &position, lcr_cal_request_id);
  LcrCal_WriteU32(payload, &position, frequency_hz);
  LcrCal_WriteU16(payload, &position, enable ? LCR_CAL_EXCITATION_MVPP : 0U);
  LcrCal_WriteU32(payload, &position, 0UL);
  payload[position++] = enable;
  payload[position++] = 0U;
  LcrCal_WriteU16(payload, &position, LCR_CAL_SETTLE_US);
  return (BoardComm_SendV2(BOARD_COMM_NODE_BLACK,
                           BOARD_COMM_CMD_LCR_EXCITATION_SET,
                           BOARD_COMM_FLAG_ACK_REQ, lcr_cal_request_id,
                           payload, position) == BOARD_COMM_OK) ? 1U : 0U;
}

static uint8_t LcrCal_SampleTarget(void)
{
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_ZERO) return LCR_CAL_ZERO_AVERAGES;
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_OPEN) return LCR_CAL_OPEN_AVERAGES;
  return LCR_CAL_STANDARD_AVERAGES;
}

static void LcrCal_ExportProfile(void)
{
  char line[192];
  (void)UsbCdc_WriteLine("LCR_CAL_BEGIN");
  (void)snprintf(line, sizeof(line), "SEQ=%u STAGE=0x3F RREF_MOHM=%lu LOAD_MOHM=%lu",
                 lcr_cal_snapshot.active_sequence,
                 (unsigned long)(lcr_cal_active.reference_ohm * 1000.0 + 0.5),
                 (unsigned long)(lcr_cal_active.load_ohm * 1000.0 + 0.5));
  (void)UsbCdc_WriteLine(line);
  for (uint8_t i = 0U; i < LCR_CAL_POINT_COUNT; i++)
  {
    (void)snprintf(line, sizeof(line),
      "P=%u F=%lu ZS_R_MOHM=%ld ZS_I_MOHM=%ld G_R_PPB=%ld G_I_PPB=%ld YO_R_NS=%ld YO_I_NS=%ld",
      i, (unsigned long)lcr_cal_frequencies[i],
      (long)(lcr_cal_active.short_z[i].real * 1000.0),
      (long)(lcr_cal_active.short_z[i].imag * 1000.0),
      (long)(lcr_cal_active.gain[i].real * 1000000000.0),
      (long)(lcr_cal_active.gain[i].imag * 1000000000.0),
      (long)(lcr_cal_active.open_y[i].real * 1000000000.0),
      (long)(lcr_cal_active.open_y[i].imag * 1000000000.0));
    (void)UsbCdc_WriteLine(line);
  }
  (void)snprintf(line, sizeof(line), "CRC32=%08lX",
                 (unsigned long)lcr_cal_active.crc32);
  (void)UsbCdc_WriteLine(line);
  (void)UsbCdc_WriteLine("LCR_CAL_END");
}

static uint32_t LcrCal_Crc32(const uint8_t *data, uint32_t length)
{
  uint32_t crc = 0xFFFFFFFFUL;
  while (length-- != 0UL)
  {
    crc ^= *data++;
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      crc = ((crc & 1UL) != 0UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static void LcrCal_FinishStep(void)
{
  char line[80];
  switch (lcr_cal_snapshot.step)
  {
    case LCR_CAL_STEP_ZERO:  lcr_cal_snapshot.stage = 0x03U; break;
    case LCR_CAL_STEP_SHORT: lcr_cal_snapshot.stage = 0x07U; break;
    case LCR_CAL_STEP_LOAD:  lcr_cal_snapshot.stage = 0x0FU; break;
    case LCR_CAL_STEP_OPEN:  lcr_cal_snapshot.stage = 0x1FU; break;
    case LCR_CAL_STEP_VERIFY:
      if (lcr_cal_verify_failed != 0U)
      {
        LcrCal_Fail(LCR_CAL_RESULT_VERIFY);
        return;
      }
      lcr_cal_active = lcr_cal_staging;
      lcr_cal_active.crc32 = 0UL;
      lcr_cal_active.crc32 = LcrCal_Crc32((const uint8_t *)&lcr_cal_active,
                                         sizeof(lcr_cal_active));
      lcr_cal_snapshot.active_valid = 1U;
      lcr_cal_snapshot.active_sequence++;
      lcr_cal_snapshot.stage = 0x3FU;
      break;
    default:
      LcrCal_Fail(LCR_CAL_RESULT_ARG);
      return;
  }
  lcr_cal_snapshot.last_result = LCR_CAL_RESULT_OK;
  lcr_cal_snapshot.busy = 0U;
  lcr_cal_state = LCR_CAL_IDLE;
  lcr_cal_snapshot.revision++;
  (void)snprintf(line, sizeof(line), "OK %s STAGE=0x%02X ACTIVE=%u SEQ=%u",
                 LcrCal_StepText(lcr_cal_snapshot.step), lcr_cal_snapshot.stage,
                 lcr_cal_snapshot.active_valid, lcr_cal_snapshot.active_sequence);
  (void)UsbCdc_WriteLine(line);
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_VERIFY)
  {
    LcrCal_ExportProfile();
  }
}

static void LcrCal_CommitFrequency(void)
{
  uint8_t index = lcr_cal_snapshot.frequency_index;
  double count = (double)LcrCal_SampleTarget();
  LcrComplex average = {lcr_cal_sum.real / count, lcr_cal_sum.imag / count};
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_OPEN)
  {
    uint8_t excluded[LCR_CAL_OPEN_AVERAGES] = {0};
    LcrComplex center = average;
    for (uint8_t drop = 0U; drop < 2U; drop++)
    {
      uint8_t worst = 0U;
      double worst_distance = -1.0;
      for (uint8_t i = 0U; i < LCR_CAL_OPEN_AVERAGES; i++)
      {
        if (excluded[i] == 0U)
        {
          LcrComplex delta = LcrCal_Subtract(lcr_cal_open_samples[i], center);
          double distance = (delta.real * delta.real) + (delta.imag * delta.imag);
          if (distance > worst_distance)
          {
            worst_distance = distance;
            worst = i;
          }
        }
      }
      excluded[worst] = 1U;
    }
    average.real = average.imag = 0.0;
    for (uint8_t i = 0U; i < LCR_CAL_OPEN_AVERAGES; i++)
    {
      if (excluded[i] == 0U) average = LcrCal_Add(average, lcr_cal_open_samples[i]);
    }
    average.real /= (double)(LCR_CAL_OPEN_AVERAGES - 2U);
    average.imag /= (double)(LCR_CAL_OPEN_AVERAGES - 2U);
  }
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_SHORT)
  {
    lcr_cal_staging.short_z[index] = average;
  }
  else if (lcr_cal_snapshot.step == LCR_CAL_STEP_LOAD)
  {
    LcrComplex denominator = LcrCal_Subtract(average, lcr_cal_staging.short_z[index]);
    LcrComplex load = {lcr_cal_staging.load_ohm, 0.0};
    if (LcrCal_Divide(load, denominator, &lcr_cal_staging.gain[index]) == 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_MATH);
      return;
    }
  }
  else if (lcr_cal_snapshot.step == LCR_CAL_STEP_OPEN)
  {
    lcr_cal_staging.open_y[index] = average;
  }
  else if (lcr_cal_snapshot.step == LCR_CAL_STEP_VERIFY)
  {
    LcrComplex expected = {lcr_cal_staging.load_ohm, 0.0};
    LcrComplex error = LcrCal_Subtract(average, expected);
    if ((LcrMath_Magnitude(error) / lcr_cal_staging.load_ohm) > LCR_CAL_VERIFY_LIMIT)
    {
      lcr_cal_verify_failed = 1U;
    }
  }
  if ((uint8_t)(index + 1U) >= LCR_CAL_POINT_COUNT)
  {
    LcrCal_FinishStep();
  }
  else
  {
    lcr_cal_snapshot.frequency_index++;
    lcr_cal_snapshot.current_frequency_hz =
        lcr_cal_frequencies[lcr_cal_snapshot.frequency_index];
    lcr_cal_snapshot.sample_index = 0U;
    lcr_cal_sum.real = lcr_cal_sum.imag = 0.0;
    memset(lcr_cal_open_samples, 0, sizeof(lcr_cal_open_samples));
    LcrCal_SetState(LCR_CAL_REQUEST_EXCITATION);
  }
}

static void LcrCal_ProcessSample(const LcrCaptureSample *sample)
{
  LcrComplex value;
  uint8_t index = lcr_cal_snapshot.frequency_index;
  if ((sample == 0) || ((sample->status & LCR_CAPTURE_STATUS_VALID) == 0U) ||
      ((sample->status & LCR_CAPTURE_STATUS_OVERRANGE) != 0U))
  {
    LcrCal_Fail(LCR_CAL_RESULT_ADC);
    return;
  }
  if (lcr_cal_snapshot.step == LCR_CAL_STEP_ZERO)
  {
    lcr_cal_zero_v1_sum += sample->vin_dc_uv;
    lcr_cal_zero_v2_sum += sample->vr_dc_uv;
  }
  else if (lcr_cal_snapshot.step == LCR_CAL_STEP_OPEN)
  {
    LcrComplex raw_y;
    LcrComplex raw_z;
    LcrComplex base;
    if (LcrMath_CalculateAdmittance(sample->vin, sample->vr,
                                    lcr_cal_staging.reference_ohm, &raw_y) == 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_MATH);
      return;
    }
    if (LcrMath_DeembedParallelResistanceAdmittance(
            raw_y, LCR_DUT_PARALLEL_RESISTANCE_OHM, &raw_y) == 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_MATH);
      return;
    }
    if (LcrCal_Reciprocal(raw_y, &raw_z) == 0U)
    {
      value.real = value.imag = 0.0;
    }
    else
    {
      base = LcrCal_Multiply(lcr_cal_staging.gain[index],
                             LcrCal_Subtract(raw_z, lcr_cal_staging.short_z[index]));
      if (LcrCal_Reciprocal(base, &value) == 0U)
      {
        value.real = value.imag = 0.0;
      }
    }
    lcr_cal_open_samples[lcr_cal_snapshot.sample_index] = value;
    lcr_cal_sum = LcrCal_Add(lcr_cal_sum, value);
  }
  else
  {
    if ((sample->status & LCR_CAPTURE_STATUS_LOW_SNR) != 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_QUALITY);
      return;
    }
    if (LcrMath_CalculateImpedance(sample->vin, sample->vr,
                                   lcr_cal_staging.reference_ohm, &value) == 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_MATH);
      return;
    }
    if (LcrMath_DeembedParallelResistance(
            value, LCR_DUT_PARALLEL_RESISTANCE_OHM, &value) == 0U)
    {
      LcrCal_Fail(LCR_CAL_RESULT_MATH);
      return;
    }
    if (lcr_cal_snapshot.step == LCR_CAL_STEP_VERIFY)
    {
      if (LcrCal_ApplyProfile(&lcr_cal_staging, index, value, &value) == 0U)
      {
        LcrCal_Fail(LCR_CAL_RESULT_MATH);
        return;
      }
    }
    lcr_cal_sum = LcrCal_Add(lcr_cal_sum, value);
  }
  lcr_cal_snapshot.sample_index++;
  lcr_cal_snapshot.revision++;
  if (lcr_cal_snapshot.sample_index >= LcrCal_SampleTarget())
  {
    if (lcr_cal_snapshot.step == LCR_CAL_STEP_ZERO)
    {
      lcr_cal_staging.zero_v1_uv = lcr_cal_zero_v1_sum / LCR_CAL_ZERO_AVERAGES;
      lcr_cal_staging.zero_v2_uv = lcr_cal_zero_v2_sum / LCR_CAL_ZERO_AVERAGES;
      LcrCal_FinishStep();
    }
    else
    {
      LcrCal_CommitFrequency();
    }
  }
  else
  {
    LcrCal_SetState(LCR_CAL_START_CAPTURE);
  }
}

static uint8_t LcrCal_Start(LcrCalibrationStep step)
{
  uint8_t required = 0U;
  if ((lcr_cal_snapshot.busy != 0U) ||
      ((LcrAuto_GetSnapshot().state != LCR_AUTO_IDLE) &&
       (LcrAuto_GetSnapshot().state != LCR_AUTO_DONE) &&
       (LcrAuto_GetSnapshot().state != LCR_AUTO_ERROR)))
  {
    lcr_cal_snapshot.last_result = LCR_CAL_RESULT_BUSY;
    return 0U;
  }
  if (step == LCR_CAL_STEP_SHORT) required = 0x03U;
  else if (step == LCR_CAL_STEP_LOAD) required = 0x07U;
  else if (step == LCR_CAL_STEP_OPEN) required = 0x0FU;
  else if (step == LCR_CAL_STEP_VERIFY) required = 0x1FU;
  if ((step == LCR_CAL_STEP_NONE) ||
      ((required != 0U) && (lcr_cal_snapshot.stage != required)))
  {
    lcr_cal_snapshot.last_result = LCR_CAL_RESULT_ARG;
    return 0U;
  }
  if (step == LCR_CAL_STEP_ZERO)
  {
    double reference = lcr_cal_staging.reference_ohm;
    double load = lcr_cal_staging.load_ohm;
    memset(&lcr_cal_staging, 0, sizeof(lcr_cal_staging));
    lcr_cal_staging.reference_ohm = reference;
    lcr_cal_staging.load_ohm = load;
    lcr_cal_snapshot.stage = 0U;
  }
  lcr_cal_snapshot.step = step;
  lcr_cal_snapshot.last_result = LCR_CAL_RESULT_OK;
  lcr_cal_snapshot.busy = 1U;
  lcr_cal_snapshot.frequency_index = 0U;
  lcr_cal_snapshot.sample_index = 0U;
  lcr_cal_snapshot.current_frequency_hz =
      (step == LCR_CAL_STEP_ZERO) ? 1000UL : lcr_cal_frequencies[0];
  lcr_cal_zero_v1_sum = lcr_cal_zero_v2_sum = 0.0;
  lcr_cal_sum.real = lcr_cal_sum.imag = 0.0;
  memset(lcr_cal_open_samples, 0, sizeof(lcr_cal_open_samples));
  lcr_cal_verify_failed = 0U;
  LcrCal_SetState(LCR_CAL_REQUEST_EXCITATION);
  return 1U;
}

void LcrCalibration_Init(void)
{
  memset(&lcr_cal_snapshot, 0, sizeof(lcr_cal_snapshot));
  memset(&lcr_cal_staging, 0, sizeof(lcr_cal_staging));
  memset(&lcr_cal_active, 0, sizeof(lcr_cal_active));
  lcr_cal_staging.reference_ohm = LCR_CAL_REFERENCE_DEFAULT_OHM;
  lcr_cal_staging.load_ohm = LCR_CAL_LOAD_DEFAULT_OHM;
  lcr_cal_snapshot.last_result = LCR_CAL_RESULT_OK;
  lcr_cal_state = LCR_CAL_IDLE;
}

uint8_t LcrCalibration_IsBusy(void)
{
  return lcr_cal_snapshot.busy;
}

double LcrCalibration_GetReferenceOhm(void)
{
  return (lcr_cal_snapshot.active_valid != 0U) ?
         lcr_cal_active.reference_ohm : lcr_cal_staging.reference_ohm;
}

LcrCalibrationSnapshot LcrCalibration_GetSnapshot(void)
{
  return lcr_cal_snapshot;
}

uint8_t LcrCalibration_Apply(uint32_t frequency_hz, LcrComplex raw,
                             LcrComplex *corrected)
{
  uint8_t index;
  if (corrected == 0) return 0U;
  *corrected = raw;
  lcr_cal_snapshot.active_applied_last = 0U;
  if ((lcr_cal_snapshot.active_valid == 0U) ||
      (LcrCal_FindFrequency(frequency_hz, &index) == 0U))
  {
    return 0U;
  }
  if (LcrCal_ApplyProfile(&lcr_cal_active, index, raw, corrected) == 0U)
  {
    *corrected = raw;
    return 0U;
  }
  lcr_cal_snapshot.active_applied_last = 1U;
  return 1U;
}

void LcrCalibration_HandleExcitationReady(const uint8_t *data, uint8_t len)
{
  if ((lcr_cal_snapshot.busy == 0U) || (lcr_cal_state != LCR_CAL_WAIT_EXCITATION) ||
      (data == 0) || (len != LCR_CAL_READY_PAYLOAD_LEN) ||
      (LcrCal_ReadU16(data) != lcr_cal_request_id))
  {
    return;
  }
  if ((data[2] != 0U) || (LcrCal_ReadU32(&data[8]) == 0UL))
  {
    LcrCal_Fail(LCR_CAL_RESULT_SIGNAL);
    return;
  }
  LcrCal_SetState(LCR_CAL_SETTLING);
}

void LcrCalibration_Task(void)
{
  LcrCaptureSample sample;
  uint32_t now = HAL_GetTick();
  if (lcr_cal_snapshot.busy == 0U) return;
  switch (lcr_cal_state)
  {
    case LCR_CAL_REQUEST_EXCITATION:
      if (LcrCal_SendExcitation(lcr_cal_snapshot.current_frequency_hz,
                               (lcr_cal_snapshot.step == LCR_CAL_STEP_ZERO) ? 0U : 1U) == 0U)
        LcrCal_Fail(LCR_CAL_RESULT_SIGNAL);
      else LcrCal_SetState(LCR_CAL_WAIT_EXCITATION);
      break;
    case LCR_CAL_WAIT_EXCITATION:
      if ((now - lcr_cal_state_tick) >= LCR_CAL_EXCITATION_TIMEOUT_MS)
        LcrCal_Fail(LCR_CAL_RESULT_SIGNAL);
      break;
    case LCR_CAL_SETTLING:
      if ((now - lcr_cal_state_tick) >= ((LCR_CAL_SETTLE_US + 999UL) / 1000UL))
        LcrCal_SetState(LCR_CAL_START_CAPTURE);
      break;
    case LCR_CAL_START_CAPTURE:
      if (LcrCapture_Start(lcr_cal_snapshot.current_frequency_hz,
                           (uint32_t)(lcr_cal_staging.reference_ohm * 1000.0)) == 0U)
        LcrCal_Fail(LCR_CAL_RESULT_ADC);
      else LcrCal_SetState(LCR_CAL_WAIT_CAPTURE);
      break;
    case LCR_CAL_WAIT_CAPTURE:
      if (LcrCapture_TakeSample(&sample) != 0U) LcrCal_ProcessSample(&sample);
      else if ((now - lcr_cal_state_tick) >= LCR_CAL_CAPTURE_TIMEOUT_MS)
        LcrCal_Fail(LCR_CAL_RESULT_ADC);
      break;
    default: break;
  }
}

static void LcrCal_WriteStatus(void)
{
  char line[160];
  (void)snprintf(line, sizeof(line),
    "STATUS LAST=%s STAGE=0x%02X BUSY=%u STEP=%s F=%lu POINT=%u/%u SAMPLE=%u ACTIVE=%u SEQ=%u",
    LcrCal_ResultText(lcr_cal_snapshot.last_result), lcr_cal_snapshot.stage,
    lcr_cal_snapshot.busy, LcrCal_StepText(lcr_cal_snapshot.step),
    (unsigned long)lcr_cal_snapshot.current_frequency_hz,
    (unsigned int)(lcr_cal_snapshot.frequency_index + 1U), LCR_CAL_POINT_COUNT,
    lcr_cal_snapshot.sample_index, lcr_cal_snapshot.active_valid,
    lcr_cal_snapshot.active_sequence);
  (void)UsbCdc_WriteLine(line);
}

void LcrCalibration_UsbTask(void)
{
  char line[96];
  char upper[96];
  LcrCalibrationStep step = LCR_CAL_STEP_NONE;
  if (UsbCdc_ReadLine(line, sizeof(line)) == 0U) return;
  for (uint8_t i = 0U; i < sizeof(upper); i++)
  {
    upper[i] = (char)toupper((unsigned char)line[i]);
    if (line[i] == '\0') break;
  }
  if (strcmp(upper, "HELP") == 0)
  {
    (void)UsbCdc_WriteLine("CMD: STATUS | SET RREF ohm | SET LOAD ohm | ZERO | SHORT | LOAD | OPEN | VERIFY | EXPORT");
    return;
  }
  if (strcmp(upper, "STATUS") == 0) { LcrCal_WriteStatus(); return; }
  if (strcmp(upper, "EXPORT") == 0)
  {
    if (lcr_cal_snapshot.active_valid != 0U) LcrCal_ExportProfile();
    else (void)UsbCdc_WriteLine("ERR ARG NO_ACTIVE_CAL");
    return;
  }
  if (strncmp(upper, "SET RREF ", 9U) == 0)
  {
    double value = strtod(&line[9], 0);
    if ((lcr_cal_snapshot.busy == 0U) &&
        (value >= 10.0) && (value <= 100000.0))
    {
      lcr_cal_staging.reference_ohm = value;
      (void)UsbCdc_WriteLine("OK SET RREF");
    }
    else (void)UsbCdc_WriteLine("ERR ARG RREF");
    return;
  }
  if (strncmp(upper, "SET LOAD ", 9U) == 0)
  {
    double value = strtod(&line[9], 0);
    if ((lcr_cal_snapshot.busy == 0U) &&
        (value >= 10.0) && (value <= 100000.0))
    {
      lcr_cal_staging.load_ohm = value;
      (void)UsbCdc_WriteLine("OK SET LOAD");
    }
    else (void)UsbCdc_WriteLine("ERR ARG LOAD");
    return;
  }
  if (strcmp(upper, "ZERO") == 0) step = LCR_CAL_STEP_ZERO;
  else if (strcmp(upper, "SHORT") == 0) step = LCR_CAL_STEP_SHORT;
  else if (strcmp(upper, "LOAD") == 0) step = LCR_CAL_STEP_LOAD;
  else if (strcmp(upper, "OPEN") == 0) step = LCR_CAL_STEP_OPEN;
  else if (strcmp(upper, "VERIFY") == 0) step = LCR_CAL_STEP_VERIFY;
  if ((step == LCR_CAL_STEP_NONE) || (LcrCal_Start(step) == 0U))
  {
    (void)UsbCdc_WriteLine((step == LCR_CAL_STEP_NONE) ? "ERR ARG COMMAND" : "ERR ARG ORDER_OR_BUSY");
  }
  else
  {
    (void)snprintf(line, sizeof(line), "OK %s START", LcrCal_StepText(step));
    (void)UsbCdc_WriteLine(line);
  }
}
