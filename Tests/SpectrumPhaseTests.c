#include <assert.h>
#include <stdio.h>

#include "SpectrumPhase_User.h"

static void test_sine_phase_adds_fixed_offset_and_wraps(void)
{
  assert(SpectrumPhase_SineUiToDds(0U) == 126U);
  assert(SpectrumPhase_SineUiToDds(1U) == 127U);
  assert(SpectrumPhase_SineUiToDds(233U) == 359U);
  assert(SpectrumPhase_SineUiToDds(234U) == 0U);
  assert(SpectrumPhase_SineUiToDds(359U) == 125U);
  assert(SpectrumPhase_SineUiToDds(360U) == 126U);
}

int main(void)
{
  test_sine_phase_adds_fixed_offset_and_wraps();
  puts("SpectrumPhaseTests: PASS");
  return 0;
}
