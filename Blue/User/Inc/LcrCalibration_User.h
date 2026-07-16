#ifndef _LCR_CALIBRATION_USER_H_
#define _LCR_CALIBRATION_USER_H_

#include "LcrMath_User.h"
#include "stm32h7xx_hal.h"

#define LCR_CAL_POINT_COUNT 8U

typedef enum {
  LCR_CAL_STEP_NONE = 0,
  LCR_CAL_STEP_ZERO,
  LCR_CAL_STEP_SHORT,
  LCR_CAL_STEP_LOAD,
  LCR_CAL_STEP_OPEN,
  LCR_CAL_STEP_VERIFY
} LcrCalibrationStep;

typedef enum {
  LCR_CAL_RESULT_OK = 0,
  LCR_CAL_RESULT_ARG,
  LCR_CAL_RESULT_ADC,
  LCR_CAL_RESULT_SIGNAL,
  LCR_CAL_RESULT_MATH,
  LCR_CAL_RESULT_VERIFY,
  LCR_CAL_RESULT_QUALITY,
  LCR_CAL_RESULT_BUSY
} LcrCalibrationResult;

typedef struct {
  uint8_t stage;
  uint8_t busy;
  uint8_t active_valid;
  uint8_t active_applied_last;
  LcrCalibrationStep step;
  LcrCalibrationResult last_result;
  uint8_t frequency_index;
  uint8_t sample_index;
  uint16_t active_sequence;
  uint32_t current_frequency_hz;
  uint32_t revision;
} LcrCalibrationSnapshot;

void LcrCalibration_Init(void);
void LcrCalibration_Task(void);
void LcrCalibration_UsbTask(void);
void LcrCalibration_HandleExcitationReady(const uint8_t *data, uint8_t len);
uint8_t LcrCalibration_IsBusy(void);
double LcrCalibration_GetReferenceOhm(void);
uint8_t LcrCalibration_Apply(uint32_t frequency_hz,
                             LcrComplex raw_impedance,
                             LcrComplex *corrected_impedance);
LcrCalibrationSnapshot LcrCalibration_GetSnapshot(void);

#endif
