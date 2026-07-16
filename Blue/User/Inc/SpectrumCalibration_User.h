#ifndef _SPECTRUM_CALIBRATION_USER_H_
#define _SPECTRUM_CALIBRATION_USER_H_

#include <stdint.h>

#define SPECTRUM_CAL_CHANNEL_COUNT 2U
#define SPECTRUM_CAL_AMPLITUDE_CODE_MAX 8191U
#define SPECTRUM_CAL_TARGET_MAX_UVPP 600000UL
#define SPECTRUM_CAL_CH1_UI_MAX_MVPP 20000UL
#define SPECTRUM_CAL_CH2_UI_MAX_MAPP 250UL
#define SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT 0x04U

static inline int32_t SpectrumCalibration_RoundDivSigned(int64_t numerator,
                                                          int64_t denominator)
{
  if (numerator >= 0)
  {
    return (int32_t)((numerator + (denominator / 2)) / denominator);
  }
  return -(int32_t)(((-numerator) + (denominator / 2)) / denominator);
}

static inline uint32_t SpectrumCalibration_AmplitudeFullScaleUvpp(uint8_t channel_id)
{
  static const uint32_t full_scale_uvpp[SPECTRUM_CAL_CHANNEL_COUNT] = {
    8816320UL, 8223814UL
  };
  return (channel_id < SPECTRUM_CAL_CHANNEL_COUNT) ?
         full_scale_uvpp[channel_id] : full_scale_uvpp[0];
}

static inline uint16_t SpectrumCalibration_AmplitudeUvppToCode(
    uint8_t channel_id, uint32_t amplitude_uvpp)
{
  uint32_t full_scale_uvpp =
      SpectrumCalibration_AmplitudeFullScaleUvpp(channel_id);
  uint64_t code;

  if (amplitude_uvpp > SPECTRUM_CAL_TARGET_MAX_UVPP)
  {
    amplitude_uvpp = SPECTRUM_CAL_TARGET_MAX_UVPP;
  }
  code = ((uint64_t)amplitude_uvpp * SPECTRUM_CAL_AMPLITUDE_CODE_MAX) +
         (full_scale_uvpp / 2UL);
  code /= full_scale_uvpp;
  return (uint16_t)code;
}

static inline uint32_t SpectrumCalibration_AmplitudeCodeToMvpp(
    uint8_t channel_id, uint16_t amplitude_code)
{
  uint32_t full_scale_uvpp =
      SpectrumCalibration_AmplitudeFullScaleUvpp(channel_id);
  uint64_t amplitude_uvpp;

  if (amplitude_code > SPECTRUM_CAL_AMPLITUDE_CODE_MAX)
  {
    amplitude_code = SPECTRUM_CAL_AMPLITUDE_CODE_MAX;
  }
  amplitude_uvpp = ((uint64_t)amplitude_code * full_scale_uvpp) +
                   (SPECTRUM_CAL_AMPLITUDE_CODE_MAX / 2U);
  amplitude_uvpp /= SPECTRUM_CAL_AMPLITUDE_CODE_MAX;
  return (uint32_t)((amplitude_uvpp + 500ULL) / 1000ULL);
}

static inline uint32_t SpectrumCalibration_AmplitudeCodeToUiValue(
    uint8_t channel_id, uint16_t amplitude_code)
{
  uint32_t internal_mvpp = SpectrumCalibration_AmplitudeCodeToMvpp(
      channel_id, amplitude_code);
  uint32_t ui_maximum = (channel_id == 0U) ?
      SPECTRUM_CAL_CH1_UI_MAX_MVPP : SPECTRUM_CAL_CH2_UI_MAX_MAPP;

  return (uint32_t)((((uint64_t)internal_mvpp * ui_maximum) + 300ULL) /
                    600ULL);
}

static inline uint16_t SpectrumCalibration_DutyCodeToFpga(
    uint16_t ui_duty_code)
{
  return (uint16_t)(65535U - ui_duty_code);
}

static inline uint8_t SpectrumCalibration_CalculateOffsetCode(
    uint8_t channel_id, int32_t user_bias_uv, uint8_t source_flags,
    int16_t *offset_code)
{
  static const int32_t code_per_mv_x100000[SPECTRUM_CAL_CHANNEL_COUNT] = {
    287293L, 293785L
  };
  static const int16_t dds_cancel_code[SPECTRUM_CAL_CHANNEL_COUNT] = {
    1418, 1452
  };
  int64_t numerator;
  int32_t code;

  if ((offset_code == 0) || (channel_id >= SPECTRUM_CAL_CHANNEL_COUNT))
  {
    return 0U;
  }

  numerator = -(int64_t)user_bias_uv *
              code_per_mv_x100000[channel_id];
  code = SpectrumCalibration_RoundDivSigned(numerator, 100000000LL);
  if ((source_flags & SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT) != 0U)
  {
    code += dds_cancel_code[channel_id];
  }
  if ((code < -8192L) || (code > 8191L))
  {
    return 0U;
  }
  *offset_code = (int16_t)code;
  return 1U;
}

#endif
