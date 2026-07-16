#include "LcrAuto_User.h"

#include <math.h>
#include <string.h>
#include "BoardComm_User.h"
#include "LcrCalibration_User.h"

#define LCR_AUTO_EXCITATION_PAYLOAD_LEN   16U
#define LCR_AUTO_READY_PAYLOAD_LEN        12U
#define LCR_AUTO_REFERENCE_MOHM            470000UL
#define LCR_AUTO_EXCITATION_MVPP          200U
#define LCR_AUTO_SETTLE_US                 1000U
#define LCR_AUTO_EXCITATION_TIMEOUT_MS     100UL
#define LCR_AUTO_CAPTURE_TIMEOUT_MS        100UL
#define LCR_AUTO_RESISTOR_FINE_HZ          500000UL

static const uint32_t lcr_auto_frequencies[LCR_AUTO_COARSE_POINT_COUNT] = {
  1000UL, 2000UL, 5000UL, 10000UL, 20000UL,
  50000UL, 100000UL, 200000UL, 500000UL, 1000000UL
};

static LcrAutoSnapshot lcr_auto_snapshot;
static LcrSweepPoint lcr_auto_points[LCR_AUTO_COARSE_POINT_COUNT];
static LcrComplex lcr_auto_fine_sum;
static uint32_t lcr_auto_state_tick;
static uint8_t lcr_auto_fine_mode;

static uint16_t LcrAuto_ReadU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t LcrAuto_ReadU32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static void LcrAuto_WriteU16(uint8_t *data, uint8_t *position, uint16_t value)
{
  data[(*position)++] = (uint8_t)(value & 0xFFU);
  data[(*position)++] = (uint8_t)(value >> 8);
}

static void LcrAuto_WriteU32(uint8_t *data, uint8_t *position, uint32_t value)
{
  data[(*position)++] = (uint8_t)(value & 0xFFUL);
  data[(*position)++] = (uint8_t)((value >> 8) & 0xFFUL);
  data[(*position)++] = (uint8_t)((value >> 16) & 0xFFUL);
  data[(*position)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint32_t LcrAuto_RoundU32(double value)
{
  if (value <= 0.0)
  {
    return 0UL;
  }
  if (value >= 4294967295.0)
  {
    return 0xFFFFFFFFUL;
  }
  return (uint32_t)(value + 0.5);
}

static int32_t LcrAuto_RoundI32(double value)
{
  if (value >= 2147483647.0)
  {
    return 2147483647L;
  }
  if (value <= -2147483648.0)
  {
    return (-2147483647L - 1L);
  }
  return (value >= 0.0) ? (int32_t)(value + 0.5) : (int32_t)(value - 0.5);
}

static uint64_t LcrAuto_RoundU64(double value)
{
  if (value <= 0.0)
  {
    return 0ULL;
  }
  if (value >= 18446744073709549568.0)
  {
    return 0xFFFFFFFFFFFFFFFFULL;
  }
  return (uint64_t)(value + 0.5);
}

static void LcrAuto_SetState(LcrAutoState state)
{
  lcr_auto_snapshot.state = state;
  lcr_auto_state_tick = HAL_GetTick();
  lcr_auto_snapshot.revision++;
}

static void LcrAuto_Fail(LcrAutoError error)
{
  lcr_auto_snapshot.error = error;
  lcr_auto_snapshot.result_valid = 0U;
  lcr_auto_snapshot.error_count++;
  LcrAuto_SetState(LCR_AUTO_ERROR);
}

static uint8_t LcrAuto_SendExcitation(uint32_t frequency_hz)
{
  uint8_t payload[LCR_AUTO_EXCITATION_PAYLOAD_LEN];
  uint8_t position = 0U;
  BoardComm_Status status;

  lcr_auto_snapshot.excitation_request_id++;
  if (lcr_auto_snapshot.excitation_request_id == 0U)
  {
    lcr_auto_snapshot.excitation_request_id = 1U;
  }
  lcr_auto_snapshot.requested_frequency_hz = frequency_hz;
  lcr_auto_snapshot.actual_frequency_hz = 0UL;
  lcr_auto_snapshot.dds_ftw = 0UL;

  LcrAuto_WriteU16(payload, &position, lcr_auto_snapshot.excitation_request_id);
  LcrAuto_WriteU32(payload, &position, frequency_hz);
  LcrAuto_WriteU16(payload, &position, LCR_AUTO_EXCITATION_MVPP);
  LcrAuto_WriteU32(payload, &position, 0UL);
  payload[position++] = 1U;
  payload[position++] = 0U;
  LcrAuto_WriteU16(payload, &position, LCR_AUTO_SETTLE_US);

  status = BoardComm_SendV2(BOARD_COMM_NODE_BLACK,
                            BOARD_COMM_CMD_LCR_EXCITATION_SET,
                            BOARD_COMM_FLAG_ACK_REQ,
                            lcr_auto_snapshot.excitation_request_id,
                            payload, position);
  if (status != BOARD_COMM_OK)
  {
    return 0U;
  }
  LcrAuto_SetState(LCR_AUTO_WAIT_EXCITATION);
  return 1U;
}

static void LcrAuto_ProcessSample(const LcrCaptureSample *sample)
{
  LcrComplex impedance;

  if ((sample == 0) ||
      ((sample->status & LCR_CAPTURE_STATUS_VALID) == 0U) ||
      ((sample->status & (LCR_CAPTURE_STATUS_OVERRANGE |
                          LCR_CAPTURE_STATUS_LOW_SNR)) != 0U))
  {
    LcrAuto_Fail(LCR_AUTO_ERROR_CAPTURE_INVALID);
    return;
  }
  if (LcrMath_CalculateImpedance(sample->vin, sample->vr,
                                 (double)lcr_auto_snapshot.reference_mohm / 1000.0,
                                 &impedance) == 0U)
  {
    LcrAuto_Fail(LCR_AUTO_ERROR_IMPEDANCE_INVALID);
    return;
  }
  if (LcrMath_DeembedParallelResistance(
          impedance, LCR_DUT_PARALLEL_RESISTANCE_OHM, &impedance) == 0U)
  {
    LcrAuto_Fail(LCR_AUTO_ERROR_IMPEDANCE_INVALID);
    return;
  }
  (void)LcrCalibration_Apply((sample->frequency_hz != 0UL) ?
                             sample->frequency_hz :
                             lcr_auto_snapshot.actual_frequency_hz,
                             impedance, &impedance);

  if (lcr_auto_fine_mode == 0U)
  {
    LcrSweepPoint *point = &lcr_auto_points[lcr_auto_snapshot.coarse_index];
    point->frequency_hz = (sample->frequency_hz != 0UL) ?
                          sample->frequency_hz :
                          lcr_auto_snapshot.actual_frequency_hz;
    point->impedance = impedance;
    point->valid = 1U;
    lcr_auto_snapshot.coarse_valid_count++;
    lcr_auto_snapshot.revision++;

    if ((uint8_t)(lcr_auto_snapshot.coarse_index + 1U) >=
        LCR_AUTO_COARSE_POINT_COUNT)
    {
      LcrAuto_SetState(LCR_AUTO_CLASSIFY);
    }
    else
    {
      lcr_auto_snapshot.coarse_index++;
      LcrAuto_SetState(LCR_AUTO_REQUEST_EXCITATION);
    }
    return;
  }

  lcr_auto_fine_sum.real += impedance.real;
  lcr_auto_fine_sum.imag += impedance.imag;
  lcr_auto_snapshot.fine_count++;
  lcr_auto_snapshot.revision++;
  if (lcr_auto_snapshot.fine_count >= LCR_AUTO_FINE_AVERAGE_COUNT)
  {
    LcrAuto_SetState(LCR_AUTO_FINALIZE);
  }
  else
  {
    LcrAuto_SetState(LCR_AUTO_START_CAPTURE);
  }
}

static void LcrAuto_Finalize(void)
{
  LcrComplex average;
  LcrDerivedResult result;

  average.real = lcr_auto_fine_sum.real / (double)LCR_AUTO_FINE_AVERAGE_COUNT;
  average.imag = lcr_auto_fine_sum.imag / (double)LCR_AUTO_FINE_AVERAGE_COUNT;
  if (LcrMath_DeriveResult(lcr_auto_snapshot.type,
                           lcr_auto_snapshot.actual_frequency_hz,
                           average, &result) == 0U)
  {
    LcrAuto_Fail(LCR_AUTO_ERROR_RESULT_INVALID);
    return;
  }

  lcr_auto_snapshot.impedance_mohm =
      LcrAuto_RoundU32(result.magnitude_ohm * 1000.0);
  lcr_auto_snapshot.resistance_mohm =
      LcrAuto_RoundI32(result.resistance_ohm * 1000.0);
  lcr_auto_snapshot.reactance_mohm =
      LcrAuto_RoundI32(result.reactance_ohm * 1000.0);
  lcr_auto_snapshot.phase_mdeg =
      LcrAuto_RoundI32(result.phase_deg * 1000.0);
  lcr_auto_snapshot.inductance_nh =
      LcrAuto_RoundU64(result.inductance_h * 1000000000.0);
  lcr_auto_snapshot.capacitance_ff =
      LcrAuto_RoundU64(result.capacitance_f * 1000000000000000.0);
  lcr_auto_snapshot.quality_factor_x1000 =
      LcrAuto_RoundU32(result.quality_factor * 1000.0);
  lcr_auto_snapshot.result_valid = 1U;
  lcr_auto_snapshot.completed_count++;
  LcrAuto_SetState(LCR_AUTO_DONE);
}

void LcrAuto_Init(void)
{
  memset(&lcr_auto_snapshot, 0, sizeof(lcr_auto_snapshot));
  memset(lcr_auto_points, 0, sizeof(lcr_auto_points));
  lcr_auto_snapshot.reference_mohm = LCR_AUTO_REFERENCE_MOHM;
  lcr_auto_snapshot.hardware_ready = LcrCapture_IsAvailable();
  lcr_auto_snapshot.state = LCR_AUTO_IDLE;
  lcr_auto_state_tick = HAL_GetTick();
}

uint8_t LcrAuto_Start(void)
{
  if (LcrCalibration_IsBusy() != 0U)
  {
    lcr_auto_snapshot.error = LCR_AUTO_ERROR_BUSY;
    lcr_auto_snapshot.revision++;
    return 0U;
  }
  if ((lcr_auto_snapshot.state != LCR_AUTO_IDLE) &&
      (lcr_auto_snapshot.state != LCR_AUTO_DONE) &&
      (lcr_auto_snapshot.state != LCR_AUTO_ERROR))
  {
    lcr_auto_snapshot.error = LCR_AUTO_ERROR_BUSY;
    lcr_auto_snapshot.revision++;
    return 0U;
  }

  memset(lcr_auto_points, 0, sizeof(lcr_auto_points));
  memset(&lcr_auto_fine_sum, 0, sizeof(lcr_auto_fine_sum));
  lcr_auto_snapshot.error = LCR_AUTO_ERROR_NONE;
  lcr_auto_snapshot.type = LCR_COMPONENT_UNKNOWN;
  lcr_auto_snapshot.hardware_ready = LcrCapture_IsAvailable();
  lcr_auto_snapshot.coarse_index = 0U;
  lcr_auto_snapshot.coarse_valid_count = 0U;
  lcr_auto_snapshot.fine_count = 0U;
  lcr_auto_snapshot.result_valid = 0U;
  lcr_auto_snapshot.requested_frequency_hz = 0UL;
  lcr_auto_snapshot.actual_frequency_hz = 0UL;
  lcr_auto_snapshot.dds_ftw = 0UL;
  lcr_auto_snapshot.impedance_mohm = 0UL;
  lcr_auto_snapshot.resistance_mohm = 0L;
  lcr_auto_snapshot.reactance_mohm = 0L;
  lcr_auto_snapshot.phase_mdeg = 0L;
  lcr_auto_snapshot.inductance_nh = 0ULL;
  lcr_auto_snapshot.capacitance_ff = 0ULL;
  lcr_auto_snapshot.quality_factor_x1000 = 0UL;
  lcr_auto_snapshot.median_slope_x1000 = 0L;
  lcr_auto_snapshot.reference_mohm =
      LcrAuto_RoundU32(LcrCalibration_GetReferenceOhm() * 1000.0);
  lcr_auto_snapshot.measurement_id++;
  lcr_auto_snapshot.start_count++;
  lcr_auto_fine_mode = 0U;
  LcrAuto_SetState(LCR_AUTO_REQUEST_EXCITATION);
  return 1U;
}

void LcrAuto_Cancel(void)
{
  lcr_auto_snapshot.error = LCR_AUTO_ERROR_NONE;
  lcr_auto_snapshot.result_valid = 0U;
  LcrAuto_SetState(LCR_AUTO_IDLE);
}

void LcrAuto_HandleExcitationReady(const uint8_t *data, uint8_t len)
{
  uint16_t request_id;
  uint8_t status;
  uint32_t actual_frequency_chz;

  if ((data == 0) || (len != LCR_AUTO_READY_PAYLOAD_LEN) ||
      (lcr_auto_snapshot.state != LCR_AUTO_WAIT_EXCITATION))
  {
    return;
  }

  request_id = LcrAuto_ReadU16(&data[0]);
  if (request_id != lcr_auto_snapshot.excitation_request_id)
  {
    return;
  }
  status = data[2];
  lcr_auto_snapshot.dds_ftw = LcrAuto_ReadU32(&data[4]);
  actual_frequency_chz = LcrAuto_ReadU32(&data[8]);
  lcr_auto_snapshot.actual_frequency_hz = (actual_frequency_chz + 50UL) / 100UL;
  if ((status != 0U) || (lcr_auto_snapshot.actual_frequency_hz == 0UL))
  {
    LcrAuto_Fail(LCR_AUTO_ERROR_EXCITATION_REJECTED);
    return;
  }
  LcrAuto_SetState(LCR_AUTO_SETTLING);
}

void LcrAuto_Task(void)
{
  LcrCaptureSample sample;
  uint32_t now = HAL_GetTick();

  LcrCapture_Task();
  lcr_auto_snapshot.hardware_ready = LcrCapture_IsAvailable();

  switch (lcr_auto_snapshot.state)
  {
    case LCR_AUTO_REQUEST_EXCITATION:
      if (LcrAuto_SendExcitation(
              lcr_auto_frequencies[lcr_auto_snapshot.coarse_index]) == 0U)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_COMMUNICATION);
      }
      break;

    case LCR_AUTO_WAIT_EXCITATION:
      if ((now - lcr_auto_state_tick) >= LCR_AUTO_EXCITATION_TIMEOUT_MS)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_EXCITATION_TIMEOUT);
      }
      break;

    case LCR_AUTO_SETTLING:
      if ((now - lcr_auto_state_tick) >=
          ((LCR_AUTO_SETTLE_US + 999UL) / 1000UL))
      {
        if (LcrCapture_IsAvailable() == 0U)
        {
          LcrAuto_Fail(LCR_AUTO_ERROR_DUAL_ADC_NOT_CONFIGURED);
        }
        else
        {
          LcrAuto_SetState(LCR_AUTO_START_CAPTURE);
        }
      }
      break;

    case LCR_AUTO_START_CAPTURE:
      if (LcrCapture_Start(lcr_auto_snapshot.actual_frequency_hz,
                           lcr_auto_snapshot.reference_mohm) == 0U)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_CAPTURE_START);
      }
      else
      {
        LcrAuto_SetState(LCR_AUTO_WAIT_CAPTURE);
      }
      break;

    case LCR_AUTO_WAIT_CAPTURE:
      if (LcrCapture_TakeSample(&sample) != 0U)
      {
        LcrAuto_ProcessSample(&sample);
      }
      else if ((now - lcr_auto_state_tick) >= LCR_AUTO_CAPTURE_TIMEOUT_MS)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_CAPTURE_TIMEOUT);
      }
      break;

    case LCR_AUTO_CLASSIFY:
    {
      double median_slope = 0.0;
      uint32_t fine_frequency = 0UL;

      lcr_auto_snapshot.type = LcrMath_Classify(
          lcr_auto_points, LCR_AUTO_COARSE_POINT_COUNT, &median_slope);
      lcr_auto_snapshot.median_slope_x1000 =
          LcrAuto_RoundI32(median_slope * 1000.0);
      if (lcr_auto_snapshot.type == LCR_COMPONENT_UNKNOWN)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_CLASSIFY);
        break;
      }
      if (lcr_auto_snapshot.type == LCR_COMPONENT_R)
      {
        fine_frequency = LCR_AUTO_RESISTOR_FINE_HZ;
      }
      else if (LcrMath_SelectBestFrequency(
                   lcr_auto_points, LCR_AUTO_COARSE_POINT_COUNT,
                   (double)lcr_auto_snapshot.reference_mohm / 1000.0,
                   &fine_frequency) == 0U)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_CLASSIFY);
        break;
      }
      lcr_auto_snapshot.requested_frequency_hz = fine_frequency;
      LcrAuto_SetState(LCR_AUTO_REQUEST_FINE);
      break;
    }

    case LCR_AUTO_REQUEST_FINE:
    {
      uint32_t fine_frequency = lcr_auto_snapshot.requested_frequency_hz;
      memset(&lcr_auto_fine_sum, 0, sizeof(lcr_auto_fine_sum));
      lcr_auto_snapshot.fine_count = 0U;
      lcr_auto_fine_mode = 1U;
      if (LcrAuto_SendExcitation(fine_frequency) == 0U)
      {
        LcrAuto_Fail(LCR_AUTO_ERROR_COMMUNICATION);
      }
      break;
    }

    case LCR_AUTO_FINALIZE:
      LcrAuto_Finalize();
      break;

    default:
      break;
  }
}

LcrAutoSnapshot LcrAuto_GetSnapshot(void)
{
  return lcr_auto_snapshot;
}

const char *LcrAuto_StateText(LcrAutoState state)
{
  switch (state)
  {
    case LCR_AUTO_IDLE: return "IDLE";
    case LCR_AUTO_REQUEST_EXCITATION: return "SET FREQ";
    case LCR_AUTO_WAIT_EXCITATION: return "WAIT DDS";
    case LCR_AUTO_SETTLING: return "SETTLING";
    case LCR_AUTO_START_CAPTURE: return "ARM ADC";
    case LCR_AUTO_WAIT_CAPTURE: return "CAPTURE";
    case LCR_AUTO_CLASSIFY: return "CLASSIFY";
    case LCR_AUTO_REQUEST_FINE: return "FINE SET";
    case LCR_AUTO_FINALIZE: return "FINALIZE";
    case LCR_AUTO_DONE: return "DONE";
    case LCR_AUTO_ERROR: return "ERROR";
    default: return "?";
  }
}

const char *LcrAuto_ErrorText(LcrAutoError error)
{
  switch (error)
  {
    case LCR_AUTO_ERROR_NONE: return "NONE";
    case LCR_AUTO_ERROR_BUSY: return "BUSY";
    case LCR_AUTO_ERROR_COMMUNICATION: return "BOARD UART";
    case LCR_AUTO_ERROR_EXCITATION_REJECTED: return "DDS REJECT";
    case LCR_AUTO_ERROR_EXCITATION_TIMEOUT: return "DDS TIMEOUT";
    case LCR_AUTO_ERROR_DUAL_ADC_NOT_CONFIGURED: return "VIN+VR ADC REQUIRED";
    case LCR_AUTO_ERROR_CAPTURE_START: return "ADC START";
    case LCR_AUTO_ERROR_CAPTURE_TIMEOUT: return "ADC TIMEOUT";
    case LCR_AUTO_ERROR_CAPTURE_INVALID: return "ADC INVALID";
    case LCR_AUTO_ERROR_IMPEDANCE_INVALID: return "Z INVALID";
    case LCR_AUTO_ERROR_CLASSIFY: return "TYPE UNKNOWN";
    case LCR_AUTO_ERROR_RESULT_INVALID: return "RESULT INVALID";
    default: return "UNKNOWN";
  }
}

const char *LcrAuto_ComponentText(LcrComponentType type)
{
  switch (type)
  {
    case LCR_COMPONENT_R: return "R";
    case LCR_COMPONENT_L: return "L";
    case LCR_COMPONENT_C: return "C";
    default: return "?";
  }
}

__weak uint8_t LcrCapture_IsAvailable(void)
{
  return 0U;
}

__weak uint8_t LcrCapture_Start(uint32_t frequency_hz, uint32_t reference_mohm)
{
  (void)frequency_hz;
  (void)reference_mohm;
  return 0U;
}

__weak void LcrCapture_Task(void)
{
}

__weak uint8_t LcrCapture_TakeSample(LcrCaptureSample *sample)
{
  (void)sample;
  return 0U;
}
