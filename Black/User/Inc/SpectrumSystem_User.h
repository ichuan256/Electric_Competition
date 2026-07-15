#ifndef _SPECTRUM_SYSTEM_USER_H_
#define _SPECTRUM_SYSTEM_USER_H_

#include "stm32h7xx_hal.h"

#define SPECTRUM_UI_INPUT_MAX_LEN 8U
#define SPECTRUM_SUM_MAX_WAVES    4U
#define SPECTRUM_SUM_FIELD_COUNT  12U

#define SPECTRUM_MODE_SUM_WAVEFORM 0U
#define SPECTRUM_MODE_LCR_TEST     1U
#define SPECTRUM_LCR_TEST_FREQ_HZ  1000000UL
#define SPECTRUM_LCR_TEST_AMP_CODE 8191U

#define BOARD_COMM_CMD_SYS_STATUS 0x31U

typedef enum {
  SPECTRUM_HOST_IDLE = 0,
  SPECTRUM_HOST_READY = 1,
  SPECTRUM_HOST_SENT = 2
} SpectrumHostState;

typedef struct {
  uint32_t frequency_hz;
  uint16_t phase_deg;
  uint16_t amplitude_code;
  int16_t offset_code;
  uint16_t duty_code;
  uint8_t waveform;
  uint8_t enable;
} SpectrumWaveConfig;

typedef struct {
  uint8_t mode;
  SpectrumHostState state;
  uint8_t channel_id;
  uint8_t wave_count;
  uint8_t selected_wave;
  uint8_t ui_focus;
  uint8_t ui_editing;
  uint8_t ui_input_len;
  char ui_input[SPECTRUM_UI_INPUT_MAX_LEN + 1U];
  uint8_t apply_counter;
  int16_t output_bias_mv;
  uint8_t fpga_output_mode;
  SpectrumWaveConfig waves[SPECTRUM_SUM_MAX_WAVES];
} SpectrumHostSnapshot;

void SpectrumSystem_Init(void);
void SpectrumSystem_Task(void);
void SpectrumSystem_OnKey(char key);
SpectrumHostSnapshot SpectrumSystem_GetSnapshot(void);

#endif
