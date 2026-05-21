/*
 * timer.c
 *
 *  Created on: May 21, 2026
 *      Author: aelbe
 */


/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "timer.h"

/* USER CODE BEGIN 0 */

#include "hub75.h"

/* USER CODE END 0 */

TIM_HandleTypeDef htim2;

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 180;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/* USER CODE BEGIN 1 */

/**
  * @brief TIM MSP Initialization
  * This function configures the hardware resources (Clock, NVIC interrupts)
  */
void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* htim_base)
{
  if(htim_base->Instance == TIM2)
  {
    /* 1. Enable the peripheral register clock for Timer 2 */
    __HAL_RCC_TIM2_CLK_ENABLE();

    /* 2. Configure the Nested Vectored Interrupt Controller (NVIC) */
    /* Priority 5 is safe for FreeRTOS if configMAX_SYSCALL_INTERRUPT_PRIORITY is 5 */
    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
  }
}

/**
 * @brief  Period elapsed callback in non-blocking mode
 * This function intercepts the hardware timer interrupts.
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    /* Route Timer 2 overflows instantly to the display bitplane rendering pipeline */
    if (htim->Instance == TIM2)
    {
        HUB75_ISR();
    }
}

/* USER CODE END 1 */

