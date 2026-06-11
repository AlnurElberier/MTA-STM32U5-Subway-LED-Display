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

#define TRAIN_COUNT			  2
#define MAX_TRAIN_TIMES       3
#define BUFFER_SIZE_BYTES     MAX_TRAIN_TIMES * TRAIN_COUNT  // 5 bytes total
#define TRIGGER_LEVEL_BYTES   1                // Wake up reading task as soon as 1 byte arrives


/* ─── Vertical Scroller ──────────────────────────────────────────────────── */
#define DWELL_TICKS  60    /* 60 * 50ms = 3 seconds */

typedef enum {
    VERT_DWELL,
    VERT_SCROLL
} VertScrollState_t;

typedef struct {
    VertScrollState_t state;
    uint8_t           current_idx;
    int8_t            scroll_y;
    uint16_t          dwell_ticks;
} VertScroller_t;

/* ─── Train Data Snapshot ────────────────────────────────────────────────── */
typedef struct {
    uint8_t c_times[8];
    uint8_t c_count;
    uint8_t g_times[8];
    uint8_t g_count;
} TrainSnapshot_t;


extern volatile StreamBufferHandle_t xMtaTimBuf;

void vBufferInit(void);
void vLedMatrixTask(void *pvParameters);


#endif /* INC_LED_MATRIX_TASK_H_ */
