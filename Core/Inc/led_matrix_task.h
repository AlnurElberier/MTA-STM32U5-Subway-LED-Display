/*
 * led_matrix_task.h
 *
 *  Created on: May 21, 2026
 *      Author: aelbe
 */

#ifndef INC_LED_MATRIX_TASK_H_
#define INC_LED_MATRIX_TASK_H_

#include "FreeRTOS.h"      // MUST be included before stream_buffer.h
#include "stream_buffer.h"


#define MAX_TRAIN_TIMES       5
#define BUFFER_SIZE_BYTES     MAX_TRAIN_TIMES  // 5 bytes total
#define TRIGGER_LEVEL_BYTES   1                // Wake up reading task as soon as 1 byte arrives


extern volatile StreamBufferHandle_t xMtaTimBuf;

void vBufferInit(void);
void vLedMatrixTask(void *pvParameters);


#endif /* INC_LED_MATRIX_TASK_H_ */
