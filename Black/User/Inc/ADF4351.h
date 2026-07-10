//-----------------------------------------------------------------
// ADF4351子程序的头文件
// 头文件名:adf4351.h
// 作    者:凌智电子
// 开始日期：	2019-02-07
// 完成日期：	2019-07-09
// 修改日期：	2019-07-09
// 当前版本: 	V1.0
// 历史版本:
//	  -V1.0:基本用法
//-----------------------------------------------------------------

#ifndef _ADF4351_H_
#define _ADF4351_H_

#include "stm32h7xx_hal.h"


#define ADF_CE_Set HAL_GPIO_WritePin(GPIOB,GPIO_PIN_3,GPIO_PIN_SET)        
#define ADF_CE_Clr HAL_GPIO_WritePin(GPIOB,GPIO_PIN_3,GPIO_PIN_RESET)

#define ADF_LE_Set HAL_GPIO_WritePin(GPIOB,GPIO_PIN_2,GPIO_PIN_SET)         
#define ADF_LE_Clr HAL_GPIO_WritePin(GPIOB,GPIO_PIN_2,GPIO_PIN_RESET)

#define ADF_DATA_Set HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_SET)         
#define ADF_DATA_Clr HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_RESET)

#define ADF_CLK_Set HAL_GPIO_WritePin(GPIOB,GPIO_PIN_0,GPIO_PIN_SET)         
#define ADF_CLK_Clr HAL_GPIO_WritePin(GPIOB,GPIO_PIN_0,GPIO_PIN_RESET)

//-----------------------------------------------------------------------------
// 函数声明
//-----------------------------------------------------------------------------

extern void ADF4351_Wdata(uint32_t date);
extern void ADF4351_Init(uint32_t date); 
extern void GPIO_AD4351_Init(void);
	
#endif
