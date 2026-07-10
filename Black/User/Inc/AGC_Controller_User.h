#ifndef _AGC_CONTROLLER_USER_H_
#define _AGC_CONTROLLER_USER_H_

#include "stm32h7xx_hal.h"

#define AGC_TARGET_MIN_MV       10U
#define AGC_TARGET_MAX_MV       100U
#define AGC_TARGET_STEP_MV      10U
#define AGC_TARGET_DEFAULT_MV   50U
#define AGC_CONTROL_MIN_MV      100U
#define AGC_CONTROL_MAX_MV      1000U
#define AGC_CONTROL_DEFAULT_MV  500U
#define AGC_CONTROL_STEP_MV     10U
#define AGC_DEADBAND_MV         2U

typedef struct {
  uint16_t target_mv;
  uint16_t measured_mv;
  uint16_t control_mv;
  uint8_t sample_valid;
  uint8_t output_connected;
} AGC_ControllerState;

void AGC_Controller_Init(void);
void AGC_Controller_ProcessSample(uint16_t measured_mv, uint8_t valid);
void AGC_Controller_AdjustTarget(int16_t delta_mv);
void AGC_Controller_SetTarget(uint16_t target_mv);
AGC_ControllerState AGC_Controller_GetState(void);

/*
 * Hardware adaptation point. Override this weak function after an external
 * DAC, PWM+RC output, or digital gain device is connected.
 * Return 1 when the requested control voltage was applied, otherwise return 0.
 */
uint8_t AGC_Controller_OutputVoltageMv(uint16_t control_mv);

#endif
