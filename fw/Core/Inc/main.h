/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.h
  * @brief   Main application header — UltraLogic R1 SVPWM Inverter
  * @version 2.0
  *
  * @company PE Info
  * @author  Umit Kayacik
  * @date    2026
  *
  * @details
  *   CubeMX-generated pin defines for SVPWM inverter:
  *   ADC (AN_IN1, TEMP_ADC, HALL_ADC), USB signals, GPIO.
  *   TIM1 PWM pins (PE8-PE13) are configured by ul_drivers.c.
  *
  ******************************************************************************
  * Copyright (c) 2026 PE Info.  All rights reserved.
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
#include "stm32f7xx_hal.h"

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

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define FAN_SW_Pin GPIO_PIN_13
#define FAN_SW_GPIO_Port GPIOC
#define VBUS_MON_Pin GPIO_PIN_8
#define VBUS_MON_GPIO_Port GPIOF
#define SHUNT3_AN_Pin GPIO_PIN_9
#define SHUNT3_AN_GPIO_Port GPIOF
#define SHUNT1_AN_Pin GPIO_PIN_5
#define SHUNT1_AN_GPIO_Port GPIOA
#define BRK_CUR_CPU_Pin GPIO_PIN_10
#define BRK_CUR_CPU_GPIO_Port GPIOD
#define BRK_B_CPU_Pin GPIO_PIN_11
#define BRK_B_CPU_GPIO_Port GPIOD
#define NRST2_Pin GPIO_PIN_15
#define NRST2_GPIO_Port GPIOD
#define SOL_CPU_Pin GPIO_PIN_7
#define SOL_CPU_GPIO_Port GPIOG
#define USB_FAULT_Pin GPIO_PIN_8
#define USB_FAULT_GPIO_Port GPIOG
#define VBUS_SENSE_Pin GPIO_PIN_9
#define VBUS_SENSE_GPIO_Port GPIOA
#define USB_EN_Pin GPIO_PIN_10
#define USB_EN_GPIO_Port GPIOA
#define BRK_BD1_CPU_Pin GPIO_PIN_3
#define BRK_BD1_CPU_GPIO_Port GPIOD
#define BRK_BD2_CPU_Pin GPIO_PIN_4
#define BRK_BD2_CPU_GPIO_Port GPIOD
#define CHG_OUT_CPU_Pin GPIO_PIN_7
#define CHG_OUT_CPU_GPIO_Port GPIOD
#define AUX_REL2_Pin GPIO_PIN_15
#define AUX_REL2_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* BKIN — TIM1 break input for overcurrent (PE15, configured in MSP user code) */
#define BKIN_Pin           GPIO_PIN_15
#define BKIN_GPIO_Port     GPIOE

/* Dynamic braking chopper IGBTs (active HIGH) */
#define BRK_ON_Pin         GPIO_PIN_13
#define BRK_ON_GPIO_Port   GPIOD
#define BRK_EN_Pin         GPIO_PIN_14
#define BRK_EN_GPIO_Port   GPIOD

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
