#ifndef _SPECTRUM_SYSTEM_USER_H_
#define _SPECTRUM_SYSTEM_USER_H_

#include "stm32h7xx_hal.h"

/*
 * Black spectrum-system host module.
 *
 * Role:
 *   Black is the measurement/control host. It drives the ADF4351 LO,
 *   requests the detector ADC value from MergeBlue, analyzes peak/spur data, and
 *   sends compact display packets to MergeBlue over BoardComm UART.
 *
 * Current small-system scope:
 *   RF input 79.0~101.0 MHz is represented by a 221-point sweep.
 *   LO frequency is RF + 10.7 MHz, so ADF4351 sweeps 89.7~111.7 MHz.
 *   MergeBlue samples the log-detector output on PC0/ADC1_INP10 and returns
 *   the measured millivolts for each requested sweep point.
 */

#define SPECTRUM_RF_START_KHZ       79000UL
#define SPECTRUM_RF_STOP_KHZ        101000UL
#define SPECTRUM_IF_KHZ             10800UL
#define SPECTRUM_STEP_KHZ           100UL
#define SPECTRUM_POINT_COUNT        221U
#define SPECTRUM_SPUR_THRESHOLD_PERCENT 2UL
/* 2% in the linear detector input domain is about -16.99 dB. */
#define SPECTRUM_SPUR_THRESHOLD_DB_X10 170
#define SPECTRUM_SWEEP_TIME_MIN_MS  1000UL
#define SPECTRUM_SWEEP_TIME_MAX_MS  5000UL
#define SPECTRUM_ADC_SETTLE_MS      5UL
#define SPECTRUM_ADC_TIMEOUT_MS     50UL
#define SPECTRUM_ADC_MAX_RETRIES    2U

#define PLL_SWEEP_START_KHZ         90000UL
#define PLL_SWEEP_STOP_KHZ          110000UL
#define PLL_SWEEP_STEP_KHZ          100UL
#define PLL_SWEEP_POINT_COUNT       201U
#define PLL_FIXED_DEFAULT_KHZ       100000UL
#define PLL_AGC_SAMPLE_PERIOD_MS    20UL
#define PLL_LOCK_DEMO_PERIOD_MS     500UL
#define SPECTRUM_UI_INPUT_MAX_LEN   8U

/* BoardComm command words shared by Black and MergeBlue. */
#define BOARD_COMM_CMD_SYS_STATUS      0x20U
#define BOARD_COMM_CMD_SWEEP_POINT     0x30U
#define BOARD_COMM_CMD_SWEEP_RESULT    0x32U

typedef enum {
  SPECTRUM_MODE_ANALYZER = 0,
  SPECTRUM_MODE_PLL_SWEEP_AGC = 1,
  SPECTRUM_MODE_PLL_FIXED_AGC = 2,
  SPECTRUM_MODE_PLL_LOCK_DEMO = 3,
  SPECTRUM_MODE_COUNT
} SpectrumMode;

typedef enum {
  SPECTRUM_HOST_IDLE = 0,
  SPECTRUM_HOST_SWEEPING = 1,
  SPECTRUM_HOST_DONE = 2
} SpectrumHostState;

typedef struct {
  SpectrumMode mode;
  SpectrumHostState state;
  uint32_t rf_khz;
  uint32_t lo_khz;
  uint16_t point_index;
  uint16_t amplitude_mv;
  uint16_t peak_index;
  uint16_t peak_amplitude_mv;
  uint8_t spur_count;
  uint8_t pll_locked;
  uint8_t sweep_time_s;
  uint16_t active_point_count;
  uint32_t fixed_frequency_khz;
  /* Desired output after the fixed attenuation network, adjustable 10~100 mV. */
  uint16_t agc_target_mv;
  uint16_t agc_control_mv;
  uint8_t agc_output_connected;
  uint8_t ui_focus;
  uint8_t ui_editing;
  uint8_t ui_input_len;
  char ui_input[SPECTRUM_UI_INPUT_MAX_LEN + 1U];
  uint16_t ui_sweep_time_ms;
  uint32_t fpga_frequency_hz;
  uint16_t fpga_phase_deg;
  uint16_t fpga_amplitude_code;
  int16_t fpga_offset_code;
  uint16_t fpga_duty_code;
  uint8_t fpga_waveform;
  uint8_t fpga_output_enable;
} SpectrumHostSnapshot;

void SpectrumSystem_Init(void);
void SpectrumSystem_Task(void);
void SpectrumSystem_OnKey(char key);
SpectrumHostSnapshot SpectrumSystem_GetSnapshot(void);

#endif
