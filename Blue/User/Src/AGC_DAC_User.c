#include "AGC_DAC_User.h"
#include <math.h>

static DAC_HandleTypeDef agc_dac_handle;
static uint16_t agc_dac_last_mv = 0;
static uint16_t agc_dac_last_code = 0;

static uint16_t AGC_DAC_ClampMv(uint16_t mv)
{
  return (mv > AGC_DAC_MAX_OUTPUT_MV) ? AGC_DAC_MAX_OUTPUT_MV : mv;
}

static uint16_t AGC_DAC_RoundToU16(float value)
{
  if (value <= 0.0f)
  {
    return 0;
  }

  if (value >= 65535.0f)
  {
    return 65535U;
  }

  return (uint16_t)(value + 0.5f);
}

void HAL_DAC_MspInit(DAC_HandleTypeDef *hdac)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (hdac->Instance == DAC1)
  {
    __HAL_RCC_DAC12_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  }
}

HAL_StatusTypeDef AGC_DAC_Init(void)
{
  DAC_ChannelConfTypeDef config = {0};

  agc_dac_handle.Instance = DAC1;

  if (HAL_DAC_Init(&agc_dac_handle) != HAL_OK)
  {
    return HAL_ERROR;
  }

  config.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  config.DAC_Trigger = DAC_TRIGGER_NONE;
  config.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  config.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
  config.DAC_UserTrimming = DAC_TRIMMING_FACTORY;

  if (HAL_DAC_ConfigChannel(&agc_dac_handle, &config, DAC_CHANNEL_2) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (HAL_DAC_Start(&agc_dac_handle, DAC_CHANNEL_2) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return AGC_DAC_SetOutputMv(0);
}

HAL_StatusTypeDef AGC_DAC_SetOutputMv(uint16_t mv)
{
  uint16_t clamped_mv = AGC_DAC_ClampMv(mv);
  uint16_t code = AGC_DAC_MvToCode(clamped_mv);

  if (HAL_DAC_SetValue(&agc_dac_handle, DAC_CHANNEL_2, DAC_ALIGN_12B_R, code) != HAL_OK)
  {
    return HAL_ERROR;
  }

  agc_dac_last_mv = clamped_mv;
  agc_dac_last_code = code;

  return HAL_OK;
}

HAL_StatusTypeDef AGC_DAC_SetTargetOutputMv(float agc_out_mv)
{
  uint16_t dac_mv = AGC_DAC_CalcInputMvFromOutputMv(agc_out_mv);

  return AGC_DAC_SetOutputMv(dac_mv);
}

uint16_t AGC_DAC_CalcInputMvFromOutputMv(float agc_out_mv)
{
  float log_out;
  float agc_in_mv;

  if (agc_out_mv < AGC_TARGET_OUTPUT_MIN_MV)
  {
    agc_out_mv = AGC_TARGET_OUTPUT_MIN_MV;
  }
  else if (agc_out_mv > AGC_TARGET_OUTPUT_MAX_MV)
  {
    agc_out_mv = AGC_TARGET_OUTPUT_MAX_MV;
  }

  log_out = logf(agc_out_mv);
  agc_in_mv = (-5.395426f * log_out * log_out) +
              (307.122507f * log_out) +
              169.549668f;

  return AGC_DAC_ClampMv(AGC_DAC_RoundToU16(agc_in_mv));
}

uint16_t AGC_DAC_MvToCode(uint16_t mv)
{
  uint32_t clamped_mv = AGC_DAC_ClampMv(mv);

  return (uint16_t)((clamped_mv * 4095U + (AGC_DAC_VREF_MV / 2U)) / AGC_DAC_VREF_MV);
}

uint16_t AGC_DAC_GetLastMv(void)
{
  return agc_dac_last_mv;
}

uint16_t AGC_DAC_GetLastCode(void)
{
  return agc_dac_last_code;
}
