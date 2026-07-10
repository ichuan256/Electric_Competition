#ifndef _AGC_DAC_USER_H_
#define _AGC_DAC_USER_H_

#include "main.h"

#define AGC_DAC_VREF_MV             3098U//3136U//3290U
#define AGC_DAC_MAX_OUTPUT_MV       AGC_DAC_VREF_MV
#define AGC_DAC_RECOMMENDED_MIN_MV  100U
#define AGC_DAC_RECOMMENDED_MAX_MV  1000U
#define AGC_TARGET_OUTPUT_MIN_MV    400.0f
#define AGC_TARGET_OUTPUT_MAX_MV    5000.0f

HAL_StatusTypeDef AGC_DAC_Init(void);
HAL_StatusTypeDef AGC_DAC_SetOutputMv(uint16_t mv);
HAL_StatusTypeDef AGC_DAC_SetTargetOutputMv(float agc_out_mv);
uint16_t AGC_DAC_CalcInputMvFromOutputMv(float agc_out_mv);
uint16_t AGC_DAC_MvToCode(uint16_t mv);
uint16_t AGC_DAC_GetLastMv(void);
uint16_t AGC_DAC_GetLastCode(void);

#endif
