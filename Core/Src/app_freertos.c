/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : app_freertos.c
 * Description        : FreeRTOS applicative file
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_freertos.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "logging_levels.h"
/* define LOG_LEVEL here if you want to modify the logging level from the default */
#if defined(LOG_LEVEL)
#undef LOG_LEVEL
#endif

#define LOG_LEVEL    LOG_INFO

#include "logging.h"

#include "sys_evt.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>


#include "hal_init.h"

#include "mx_netconn.h"

#include "led_matrix_task.h"

#include "lwip/tcpip.h"
#include "lwip/apps/sntp.h"
#include "sntp_sync.h"

#if defined(__USE_STSAFE__)
#include "stsafe.h"
#endif

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
EventGroupHandle_t xSystemEvents = NULL;


/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 1024 * 4
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void vInitTask(void *pvArgs);
static void vHeartbeatTask(void *pvParameters);
/* USER CODE END FunctionPrototypes */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
  LogError("Malloc Fail\n");
  vDoSystemReset();
}
/* USER CODE END 5 */

/* USER CODE BEGIN 2 */
void vApplicationIdleHook(void)
{
  vPetWatchdog();
}
/* USER CODE END 2 */

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  taskENTER_CRITICAL();

  LogSys("Stack overflow in %s", pcTaskName);
  (void) xTask;

  vDoSystemReset();

  taskEXIT_CRITICAL();
}
/* USER CODE END 4 */

/* USER CODE BEGIN 1 */
/* Functions needed when configGENERATE_RUN_TIME_STATS is on */
__weak void configureTimerForRunTimeStats(void)
{

}

__weak unsigned long getRunTimeCounterValue(void)
{
  return 0;
}
/* USER CODE END 1 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  hw_init();

  /* Initialize uart for logging before cli is up and running */
  vInitLoggingEarly();

  vLoggingInit();

  LogInfo("HW Init Complete.");
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  vBufferInit();
  /* USER CODE END RTOS_QUEUES */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}
/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief Function implementing the defaultTask thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN defaultTask */
  BaseType_t xResult;

  (void) argument;

  xSystemEvents = xEventGroupCreate();

  xResult = xTaskCreate(vHeartbeatTask, "Heartbeat", 128, NULL, tskIDLE_PRIORITY, NULL);
  configASSERT(xResult == pdTRUE);

//  /* 2. Launch your UI Task immediately so it starts animating right away */
//  xResult = xTaskCreate(vLedMatrixTask, "MatrixUI", 512, NULL, 11, NULL);
//  configASSERT(xResult == pdTRUE);

  /* Keeps core networking engine active. This will handle initialization and connect to Wi-Fi */
  xResult = xTaskCreate(net_main, "MxNet", 1024, NULL, 23, NULL);
  configASSERT(xResult == pdTRUE);

  xResult = xTaskCreate(vLoggingConsumerTask, "LogTask", 1024, NULL, 10, NULL);
  configASSERT(xResult == pdTRUE);

  ( void ) xEventGroupWaitBits( xSystemEvents,
                                  EVT_MASK_NET_CONNECTED,
                                  pdFALSE,          /* Don't clear the bit on exit */
                                  pdTRUE,           /* Wait for the bit */
                                  portMAX_DELAY );  /* Block indefinitely until it happens */


  LogInfo("Wi-Fi link detected!");

  LOCK_TCPIP_CORE(); /* Claims exclusive ownership of the network stack */

  sntp_setoperatingmode(SNTP_OPMODE_POLL);

  ip_addr_t ntp_server_ip;
  IP_ADDR4(&ntp_server_ip, 216, 239, 35, 0); /* Google Anycast NTP */
  sntp_setserver(0, &ntp_server_ip);
  sntp_init(); /* Safe to call now because the core is locked */

  UNLOCK_TCPIP_CORE(); /* Releases control back to the system */

  ( void ) xEventGroupWaitBits( xSystemEvents,
		  	  	  	  	  	  	  EVT_MASK_TIME_SYNCED,
                                  pdFALSE,          /* Don't clear the bit on exit */
                                  pdTRUE,           /* Wait for the bit */
                                  portMAX_DELAY );  /* Block indefinitely until it happens */

  /* 2. Spawn your MTA Client Task */
  LogInfo("Spawning MTA Protobuf Client...");
  xResult = xTaskCreate(vMtaApiTask, "MtaClient", 4096, NULL, 20, NULL);
  configASSERT(xResult == pdTRUE);


  /* Infinite loop */
  for (;;)
  {
    vTaskSuspend( NULL);
    vTaskDelay(1);
  }
  /* USER CODE END defaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void vHeartbeatTask(void *pvParameters)
{
  (void) pvParameters;

  HAL_GPIO_WritePin( GPIOH, GPIO_PIN_7, GPIO_PIN_RESET); // LED GREEN
  HAL_GPIO_WritePin( GPIOH, GPIO_PIN_6, GPIO_PIN_SET); // LED RED

  while (1)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
    HAL_GPIO_TogglePin( GPIOH, GPIO_PIN_7);
  }
}
/* USER CODE END Application */

