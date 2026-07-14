#ifndef _SPECTRUM_DISPLAY_USER_H_
#define _SPECTRUM_DISPLAY_USER_H_

#include "stm32h7xx_hal.h"

#define SPECTRUM_DISPLAY_REFRESH_MS 250UL
#define SPECTRUM_DISPLAY_LED_HEARTBEAT 0U
#define SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN 8U
#define SPECTRUM_DISPLAY_SUM_MAX_WAVES 4U
#define SPECTRUM_DISPLAY_SUM_FIELD_COUNT 11U

#define SPECTRUM_DISPLAY_MODE_SUM_WAVEFORM 0U
#define SPECTRUM_DISPLAY_MODE_LCR_TEST     1U

typedef struct {
  uint32_t frequency_hz;
  uint16_t phase_deg;
  uint16_t amplitude_code;
  int16_t offset_code;
  uint16_t duty_code;
  uint8_t waveform;
  uint8_t enable;
} SpectrumDisplayWaveConfig;

typedef struct {
  uint8_t mode;
  uint8_t state;
  uint8_t channel_id;
  uint8_t wave_count;
  uint8_t selected_wave;
  uint8_t ui_focus;
  uint8_t ui_editing;
  uint8_t ui_input_len;
  char ui_input[SPECTRUM_DISPLAY_UI_INPUT_MAX_LEN + 1U];
  uint8_t apply_counter;
  int16_t output_bias_mv;
  SpectrumDisplayWaveConfig waves[SPECTRUM_DISPLAY_SUM_MAX_WAVES];
  char last_key;
  uint8_t last_key_ascii;
  uint32_t rx_count;
  uint32_t error_count;
} SpectrumDisplayState;

void SpectrumDisplay_Init(void);
void SpectrumDisplay_Task(void);
SpectrumDisplayState SpectrumDisplay_GetState(void);

#endif
