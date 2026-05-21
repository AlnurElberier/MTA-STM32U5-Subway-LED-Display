/*
 * timer.h
 *
 *  Created on: May 21, 2026
 *      Author: aelbe
 */

#ifndef INC_TIMER_H_
#define INC_TIMER_H_

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern TIM_HandleTypeDef htim2;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_TIM2_Init(void);


/* USER CODE BEGIN Prototypes */

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif


#endif /* INC_TIMER_H_ */
