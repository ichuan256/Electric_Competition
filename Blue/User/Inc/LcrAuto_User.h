#ifndef _LCR_AUTO_USER_H_
#define _LCR_AUTO_USER_H_

#include "LcrMath_User.h"
#include "stm32h7xx_hal.h"

#define LCR_AUTO_COARSE_POINT_COUNT 10U
#define LCR_AUTO_FINE_AVERAGE_COUNT 8U

#define LCR_CAPTURE_STATUS_VALID       0x0001U
#define LCR_CAPTURE_STATUS_OVERRANGE   0x0002U
#define LCR_CAPTURE_STATUS_LOW_SNR     0x0004U

typedef enum {
  LCR_AUTO_IDLE = 0,
  LCR_AUTO_REQUEST_EXCITATION,
  LCR_AUTO_WAIT_EXCITATION,
  LCR_AUTO_SETTLING,
  LCR_AUTO_START_CAPTURE,
  LCR_AUTO_WAIT_CAPTURE,
  LCR_AUTO_CLASSIFY,
  LCR_AUTO_REQUEST_FINE,
  LCR_AUTO_FINALIZE,
  LCR_AUTO_DONE,
  LCR_AUTO_ERROR
} LcrAutoState;

typedef enum {
  LCR_AUTO_ERROR_NONE = 0,
  LCR_AUTO_ERROR_BUSY,
  LCR_AUTO_ERROR_COMMUNICATION,
  LCR_AUTO_ERROR_EXCITATION_REJECTED,
  LCR_AUTO_ERROR_EXCITATION_TIMEOUT,
  LCR_AUTO_ERROR_DUAL_ADC_NOT_CONFIGURED,
  LCR_AUTO_ERROR_CAPTURE_START,
  LCR_AUTO_ERROR_CAPTURE_TIMEOUT,
  LCR_AUTO_ERROR_CAPTURE_INVALID,
  LCR_AUTO_ERROR_IMPEDANCE_INVALID,
  LCR_AUTO_ERROR_CLASSIFY,
  LCR_AUTO_ERROR_RESULT_INVALID
} LcrAutoError;

typedef struct {
  uint32_t frequency_hz;
  uint32_t sample_rate_hz;
  uint16_t status;
  LcrComplex vin;
  LcrComplex vr;
  double vin_dc_uv;
  double vr_dc_uv;
} LcrCaptureSample;

typedef struct {
  LcrAutoState state;
  LcrAutoError error;
  LcrComponentType type;
  uint8_t hardware_ready;
  uint8_t coarse_index;
  uint8_t coarse_valid_count;
  uint8_t fine_count;
  uint8_t result_valid;
  uint16_t measurement_id;
  uint16_t excitation_request_id;
  uint32_t requested_frequency_hz;
  uint32_t actual_frequency_hz;
  uint32_t dds_ftw;
  uint32_t reference_mohm;
  uint32_t impedance_mohm;
  int32_t resistance_mohm;
  int32_t reactance_mohm;
  int32_t phase_mdeg;
  uint64_t inductance_nh;
  uint64_t capacitance_ff;
  uint32_t quality_factor_x1000;
  int32_t median_slope_x1000;
  uint32_t start_count;
  uint32_t completed_count;
  uint32_t error_count;
  uint32_t revision;
} LcrAutoSnapshot;

void LcrAuto_Init(void);
uint8_t LcrAuto_Start(void);
void LcrAuto_Cancel(void);
void LcrAuto_Task(void);
void LcrAuto_HandleExcitationReady(const uint8_t *data, uint8_t len);
LcrAutoSnapshot LcrAuto_GetSnapshot(void);
const char *LcrAuto_StateText(LcrAutoState state);
const char *LcrAuto_ErrorText(LcrAutoError error);
const char *LcrAuto_ComponentText(LcrComponentType type);

/*
 * The dual-ADC driver overrides these weak hooks. Raw ADC arrays stay inside
 * that Blue-side driver; only the fitted Vin/Vr phasors enter LcrAuto.
 */
uint8_t LcrCapture_IsAvailable(void);
uint8_t LcrCapture_Start(uint32_t frequency_hz, uint32_t reference_mohm);
void LcrCapture_Task(void);
uint8_t LcrCapture_TakeSample(LcrCaptureSample *sample);

#endif
