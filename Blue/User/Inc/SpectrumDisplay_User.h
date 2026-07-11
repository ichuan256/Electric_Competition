#ifndef _SPECTRUM_DISPLAY_USER_H_
#define _SPECTRUM_DISPLAY_USER_H_

#include "stm32h7xx_hal.h"

#define SPECTRUM_DISPLAY_POINT_COUNT  221U
#define SPECTRUM_DISPLAY_RF_START_KHZ 79000UL
#define SPECTRUM_DISPLAY_IF_KHZ       10700UL
#define SPECTRUM_DISPLAY_STEP_KHZ     100UL
#define SPECTRUM_DISPLAY_PLL_START_KHZ 90000UL
#define SPECTRUM_DISPLAY_PLL_POINTS    201U
#define SPECTRUM_DISPLAY_REFRESH_MS   250UL
#define SPECTRUM_DISPLAY_LED_HEARTBEAT 0U
#define SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN 8U

typedef struct {
  uint8_t mode;
  uint8_t state;
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
  uint16_t agc_target_mv;
  uint16_t agc_control_mv;
  uint8_t agc_output_connected;
  uint8_t ui_focus;
  uint8_t ui_editing;
  uint8_t ui_input_len;
  char ui_input[SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN + 1U];
  uint16_t ui_sweep_time_ms;
  uint32_t fpga_frequency_hz;
  uint16_t fpga_phase_deg;
  uint16_t fpga_amplitude_code;
  int16_t fpga_offset_code;
  uint16_t fpga_duty_code;
  uint8_t fpga_waveform;
  uint8_t fpga_output_enable;
  char last_key;
  uint8_t last_key_ascii;
  uint32_t rx_count;
  uint32_t error_count;
} SpectrumDisplayState;

void SpectrumDisplay_Init(void);
void SpectrumDisplay_Task(void);
SpectrumDisplayState SpectrumDisplay_GetState(void);

#endif
