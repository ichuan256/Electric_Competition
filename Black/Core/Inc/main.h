/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define MRT1_Pin GPIO_PIN_0
#define MRT1_GPIO_Port GPIOA
#define SCK1_Pin GPIO_PIN_1
#define SCK1_GPIO_Port GPIOA
#define IUP1_Pin GPIO_PIN_2
#define IUP1_GPIO_Port GPIOA
#define DRC1_Pin GPIO_PIN_3
#define DRC1_GPIO_Port GPIOA
#define OSK1_Pin GPIO_PIN_4
#define OSK1_GPIO_Port GPIOA
#define PF1_1_Pin GPIO_PIN_5
#define PF1_1_GPIO_Port GPIOA
#define PF2_1_Pin GPIO_PIN_6
#define PF2_1_GPIO_Port GPIOA
#define PF01_Pin GPIO_PIN_7
#define PF01_GPIO_Port GPIOA
#define CLK_Pin GPIO_PIN_0
#define CLK_GPIO_Port GPIOB
#define DATA_Pin GPIO_PIN_1
#define DATA_GPIO_Port GPIOB
#define LE_Pin GPIO_PIN_2
#define LE_GPIO_Port GPIOB
#define DRH1_Pin GPIO_PIN_8
#define DRH1_GPIO_Port GPIOA
#define SDI1_Pin GPIO_PIN_9
#define SDI1_GPIO_Port GPIOA
#define CSN1_Pin GPIO_PIN_10
#define CSN1_GPIO_Port GPIOA
#define CE_Pin GPIO_PIN_3
#define CE_GPIO_Port GPIOB
#define KEY_ROW0_Pin GPIO_PIN_0
#define KEY_ROW0_GPIO_Port GPIOC
#define KEY_ROW1_Pin GPIO_PIN_1
#define KEY_ROW1_GPIO_Port GPIOC
#define KEY_ROW2_Pin GPIO_PIN_8
#define KEY_ROW2_GPIO_Port GPIOC
#define KEY_ROW3_Pin GPIO_PIN_3
#define KEY_ROW3_GPIO_Port GPIOC
#define KEY_COL0_Pin GPIO_PIN_4
#define KEY_COL0_GPIO_Port GPIOC
#define KEY_COL1_Pin GPIO_PIN_5
#define KEY_COL1_GPIO_Port GPIOC
#define KEY_COL2_Pin GPIO_PIN_6
#define KEY_COL2_GPIO_Port GPIOC
#define KEY_COL3_Pin GPIO_PIN_7
#define KEY_COL3_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
