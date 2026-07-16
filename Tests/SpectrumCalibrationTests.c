#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "SpectrumCalibration_User.h"

static void test_amplitude_calibration_matches_measured_reference(void)
{
  uint16_t ch1 = SpectrumCalibration_AmplitudeUvppToCode(0U, 500000UL);
  uint16_t ch2 = SpectrumCalibration_AmplitudeUvppToCode(1U, 500000UL);

  assert((ch1 >= 464U) && (ch1 <= 466U));
  assert((ch2 >= 497U) && (ch2 <= 499U));
  assert(SpectrumCalibration_AmplitudeCodeToMvpp(0U, ch1) == 500UL);
  assert(SpectrumCalibration_AmplitudeCodeToMvpp(1U, ch2) == 500UL);
}

static void test_channel_ui_mapping_uses_voltage_and_current_units(void)
{
  uint16_t ch1_full = SpectrumCalibration_AmplitudeUvppToCode(0U, 600000UL);
  uint16_t ch2_full = SpectrumCalibration_AmplitudeUvppToCode(1U, 600000UL);
  uint16_t ch1_half = SpectrumCalibration_AmplitudeUvppToCode(0U, 300000UL);
  uint16_t ch2_half = SpectrumCalibration_AmplitudeUvppToCode(1U, 300000UL);

  assert(SpectrumCalibration_AmplitudeCodeToUiValue(0U, ch1_full) == 20000UL);
  assert(SpectrumCalibration_AmplitudeCodeToUiValue(1U, ch2_full) == 250UL);
  assert(SpectrumCalibration_AmplitudeCodeToUiValue(0U, ch1_half) == 10000UL);
  assert(SpectrumCalibration_AmplitudeCodeToUiValue(1U, ch2_half) == 125UL);
}

static void test_duty_cycle_is_inverted_only_for_fpga_transport(void)
{
  assert(SpectrumCalibration_DutyCodeToFpga(0U) == 65535U);
  assert(SpectrumCalibration_DutyCodeToFpga(16384U) == 49151U);
  assert(SpectrumCalibration_DutyCodeToFpga(32768U) == 32767U);
  assert(SpectrumCalibration_DutyCodeToFpga(49151U) == 16384U);
  assert(SpectrumCalibration_DutyCodeToFpga(65535U) == 0U);
}

static void test_bias_calibration_with_dds_compensation(void)
{
  int16_t code = 0;

  assert(SpectrumCalibration_CalculateOffsetCode(
             0U, 0L, SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT,
             &code) != 0U);
  assert(code == 1418);
  assert(SpectrumCalibration_CalculateOffsetCode(
             1U, 0L, SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT,
             &code) != 0U);
  assert(code == 1452);

  assert(SpectrumCalibration_CalculateOffsetCode(
             0U, 100000L, SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT,
             &code) != 0U);
  assert(code == 1131);
  assert(SpectrumCalibration_CalculateOffsetCode(
             1U, -500000L, SPECTRUM_CAL_SOURCE_FLAG_DDS_BIAS_PRESENT,
             &code) != 0U);
  assert(code == 2921);
}

static void test_bias_calibration_without_dds_and_bounds(void)
{
  int16_t code = 0;

  assert(SpectrumCalibration_CalculateOffsetCode(0U, 100000L, 0U,
                                                  &code) != 0U);
  assert(code == -287);
  assert(SpectrumCalibration_CalculateOffsetCode(1U, -100000L, 0U,
                                                  &code) != 0U);
  assert(code == 294);
  assert(SpectrumCalibration_CalculateOffsetCode(2U, 0L, 0U,
                                                  &code) == 0U);
  assert(SpectrumCalibration_CalculateOffsetCode(0U, 5000000L, 0U,
                                                  &code) == 0U);
}

int main(void)
{
  test_amplitude_calibration_matches_measured_reference();
  test_channel_ui_mapping_uses_voltage_and_current_units();
  test_duty_cycle_is_inverted_only_for_fpga_transport();
  test_bias_calibration_with_dds_compensation();
  test_bias_calibration_without_dds_and_bounds();
  puts("SpectrumCalibrationTests: PASS");
  return 0;
}
