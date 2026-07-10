#include "AGC_Controller_User.h"

static AGC_ControllerState agc_state;

static uint16_t AGC_Controller_ClampTarget(int32_t target)
{
  if (target < (int32_t)AGC_TARGET_MIN_MV)
  {
    target = AGC_TARGET_MIN_MV;
  }
  else if (target > (int32_t)AGC_TARGET_MAX_MV)
  {
    target = AGC_TARGET_MAX_MV;
  }

  return (uint16_t)target;
}

static void AGC_Controller_ApplyOutput(void)
{
  agc_state.output_connected = AGC_Controller_OutputVoltageMv(agc_state.control_mv);
}

void AGC_Controller_Init(void)
{
  agc_state.target_mv = AGC_TARGET_DEFAULT_MV;
  agc_state.measured_mv = 0U;
  agc_state.control_mv = AGC_CONTROL_DEFAULT_MV;
  agc_state.sample_valid = 0U;
  agc_state.output_connected = 0U;
  AGC_Controller_ApplyOutput();
}

void AGC_Controller_ProcessSample(uint16_t measured_mv, uint8_t valid)
{
  agc_state.measured_mv = measured_mv;
  agc_state.sample_valid = valid;

  if (valid == 0U)
  {
    return;
  }

  if ((uint32_t)measured_mv + AGC_DEADBAND_MV < agc_state.target_mv)
  {
    if (agc_state.control_mv <= (AGC_CONTROL_MAX_MV - AGC_CONTROL_STEP_MV))
    {
      agc_state.control_mv += AGC_CONTROL_STEP_MV;
    }
    else
    {
      agc_state.control_mv = AGC_CONTROL_MAX_MV;
    }
    AGC_Controller_ApplyOutput();
  }
  else if (measured_mv > ((uint32_t)agc_state.target_mv + AGC_DEADBAND_MV))
  {
    if (agc_state.control_mv >= (AGC_CONTROL_MIN_MV + AGC_CONTROL_STEP_MV))
    {
      agc_state.control_mv -= AGC_CONTROL_STEP_MV;
    }
    else
    {
      agc_state.control_mv = AGC_CONTROL_MIN_MV;
    }
    AGC_Controller_ApplyOutput();
  }
}

void AGC_Controller_AdjustTarget(int16_t delta_mv)
{
  int32_t target = (int32_t)agc_state.target_mv + delta_mv;
  agc_state.target_mv = AGC_Controller_ClampTarget(target);
}

void AGC_Controller_SetTarget(uint16_t target_mv)
{
  agc_state.target_mv = AGC_Controller_ClampTarget((int32_t)target_mv);
}

AGC_ControllerState AGC_Controller_GetState(void)
{
  return agc_state;
}

__weak uint8_t AGC_Controller_OutputVoltageMv(uint16_t control_mv)
{
  (void)control_mv;
  return 0U;
}
