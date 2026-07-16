#include <assert.h>
#include <stdio.h>

#include "SpectrumDds_User.h"

static void test_pure_sine_reaches_ad9910_full_scale(void)
{
  assert(SpectrumDds_AmplitudeCommandMvpp(0U, 1U) == 0UL);
  assert(SpectrumDds_AmplitudeCommandMvpp(4096U, 1U) == 359UL);
  assert(SpectrumDds_AmplitudeCommandMvpp(8191U, 1U) == 718UL);
}

static void test_composite_wave_keeps_existing_amplitude_mapping(void)
{
  assert(SpectrumDds_AmplitudeCommandMvpp(0U, 0U) == 0UL);
  assert(SpectrumDds_AmplitudeCommandMvpp(8191U, 0U) == 627UL);
}

static void test_amplitude_code_is_clamped(void)
{
  assert(SpectrumDds_AmplitudeCommandMvpp(9000U, 1U) == 718UL);
}

int main(void)
{
  test_pure_sine_reaches_ad9910_full_scale();
  test_composite_wave_keeps_existing_amplitude_mapping();
  test_amplitude_code_is_clamped();
  puts("SpectrumDdsTests: PASS");
  return 0;
}
