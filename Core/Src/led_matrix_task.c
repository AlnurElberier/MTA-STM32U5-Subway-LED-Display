/*
 * led_matrix_task.c
 *
 *  Created on: May 21, 2026
 *      Author: aelbe
 */



#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "message_buffer.h"
#include "time.h"

#include "led_matrix_task.h"
#include "hub75_font5x7.h"
#include "hub75_font3x5.h"
#include <string.h>

volatile StreamBufferHandle_t xMtaTimBuf = NULL;
extern RTC_HandleTypeDef hrtc;


HAL_StatusTypeDef getHeaderString(char *timBuf) {

	RTC_DateTypeDef date;
	RTC_TimeTypeDef time;


	if (HAL_OK != HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN)) return HAL_ERROR;

	if (HAL_OK != HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN)) return HAL_ERROR;

	sprintf(timBuf, "%02d %02d:%02d",
	    date.Date,
		time.Hours > 12 ? time.Hours - 12 : time.Hours,
	    time.Minutes);

	return HAL_OK;

}


void vBufferInit(void) {
    // Since sizeof(uint8_t) is 1, size is just the number of trains
    xMtaTimBuf = xStreamBufferCreate(BUFFER_SIZE_BYTES, TRIGGER_LEVEL_BYTES);

    if (xMtaTimBuf == NULL) {
        // Handle error (out of memory)
    }
}


void vLedMatrixTask(void *pvParameters) {

	( void ) pvParameters;

	HUB75_Init();

//	const char *line_header = "CLine";

	char timBuf[16] = {'/0'};

	getHeaderString(timBuf);

	/* Dynamic string buffer to hold the formatted text (e.g., "03m   08m   14m...") */
	char arrival_times[64] = "Waiting for data...";

	uint8_t ucLocalTrainTimes[MAX_TRAIN_TIMES];
	size_t xReceivedBytes = 0;

	/* Horizontal movement trackers */
	int16_t scroll_x = HUB75_COLS;
	// Guess an initial limit; this will recalculate dynamically when data arrives
	int16_t scroll_limit = (int16_t)(-(int16_t)strlen(arrival_times) * FONT_CELL_W);

	/* Guard: Wait until the main setup function initializes the buffer pointer */
	while (xMtaTimBuf == NULL) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	for( ;; )
	{
		/* 1. NON-BLOCKING CHECK: Look for fresh snapshots from the MTA API task.
		 * Timeout is 0 because we cannot let this block and freeze the scrolling animation. */

		xReceivedBytes = xStreamBufferReceive(
			xMtaTimBuf,
			(void *)ucLocalTrainTimes,
			sizeof(ucLocalTrainTimes),
			0  // Do not block
		);

		getHeaderString(timBuf);

		/* 2. If a fresh update arrived, rebuild our display string */
		if (xReceivedBytes > 0) {
			if (ucLocalTrainTimes[0] == 0xFF) {
				snprintf(arrival_times, sizeof(arrival_times), "Connection Lost");
				scroll_limit = (int16_t)(-(int16_t)strlen(arrival_times) * FONT_CELL_W);
				scroll_x = HUB75_COLS; // Reset scroll position so it rolls in cleanly
			}
			else {
				int iOffset = 0;
				arrival_times[0] = '\0'; // Clear previous string

				for (size_t i = 0; i < xReceivedBytes; i++) {
					// Append each time formatted as "XXm   "
					int iWritten = snprintf(arrival_times + iOffset, sizeof(arrival_times) - iOffset,
											"%02dm   ", ucLocalTrainTimes[i]);
					if (iWritten > 0) {
						iOffset += iWritten;
					}
				}

				/* Recalculate precise bounds for the new text string length */
				scroll_limit = (int16_t)(-(int16_t)strlen(arrival_times) * FONT_CELL_W);

				/* Optional: Reset scroll position to the right edge so new times roll in cleanly */
				scroll_x = HUB75_COLS;
			}
		}
		else if (xStreamBufferIsEmpty(xMtaTimBuf) && strcmp(arrival_times, "Waiting for data...") != 0 && strlen(arrival_times) == 0) {
			/* If the buffer was wiped clean (e.g., API reported no trains), show a fallback string */
			snprintf(arrival_times, sizeof(arrival_times), "No Trains Scheduled");
			scroll_limit = (int16_t)(-(int16_t)strlen(arrival_times) * FONT_CELL_W);
		}

		/* 3. Clear the canvas back-buffer to flat black */
		HUB75_Clear();

		/* 4. Top Half (y = 0): Static subway line identifier using 5x7 Font */
		HUB75_DrawString3x5(0, 0, timBuf, 255, 255, 255); // Iconic transit amber-orange

		/* 5. Bottom Half (y = 9): Smooth scrolling arrivals */
		HUB75_ScrollString5x7(scroll_x, 9, arrival_times, 0, 240, 60); // Clean commuter green

		/* 6. Swap buffers to push the frame atomically onto the display matrix */
		HUB75_SwapBuffers();

		/* 7. Progress scrolling location coordinate left by 1 pixel */
		scroll_x--;

		/* 8. Check for limit barrier crossing to recycle ticker array loop */
		if (scroll_x < scroll_limit) {
			scroll_x = HUB75_COLS;
		}

		/* 9. FreeRTOS-friendly framerate stabilization (~30 FPS)
		 * Yields the CPU core to other tasks (like your Wi-Fi/API stack) during the delay */
		vTaskDelay(pdMS_TO_TICKS(50));
	}

}
