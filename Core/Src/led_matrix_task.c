/*
 * led_matrix_task.c
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
#include <stdio.h>

volatile StreamBufferHandle_t xMtaTimBuf = NULL;
extern RTC_HandleTypeDef hrtc;


/* ─── RTC Header String ──────────────────────────────────────────────────── */
HAL_StatusTypeDef getHeaderString(char *timBuf) {

    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    if (HAL_OK != HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN)) return HAL_ERROR;
    if (HAL_OK != HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN)) return HAL_ERROR;

    sprintf(timBuf, "%d %02d:%02d",
            date.Date,
            time.Hours > 12 ? time.Hours - 12 : time.Hours,
            time.Minutes);

    return HAL_OK;
}


/* ─── Stream Buffer Init ─────────────────────────────────────────────────── */
void vBufferInit(void) {
    /* Ensure BUFFER_SIZE_BYTES and TRIGGER_LEVEL_BYTES are configured to 6 */
    xMtaTimBuf = xStreamBufferCreate(6, 6);
    if (xMtaTimBuf == NULL) {
        /* Handle error */
    }
}


/* ─── Vertical Scroller Tick ─────────────────────────────────────────────── */
static void vTickVertScroller(VertScroller_t *s, uint8_t count)
{
    if (count <= 1) return;  /* Nothing to scroll if 0 or 1 entry */

    switch (s->state) {

        case VERT_DWELL:
            s->dwell_ticks++;
            if (s->dwell_ticks >= DWELL_TICKS) {
                s->dwell_ticks = 0;
                s->state       = VERT_SCROLL;
                s->scroll_y    = 0;
            }
            break;

        case VERT_SCROLL:
            s->scroll_y--;
            if (s->scroll_y <= -FONT3X5_H) {
                /* Snap: advance to next index, return to dwell */
                s->current_idx = (s->current_idx + 1) % count;
                s->scroll_y    = 0;
                s->state       = VERT_DWELL;
            }
            break;
    }
}


/* ─── LED Matrix Task ────────────────────────────────────────────────────── */
void vLedMatrixTask(void *pvParameters)
{
    (void) pvParameters;

    HUB75_Init();

    /* ── Buffers ─────────────────────────────────────────────────────────── */
    char timBuf[16] = {'\0'};

    /* ── Train snapshot ──────────────────────────────────────────────────── */
    TrainSnapshot_t snapshot = {
        .c_times = {0}, .c_count = 0,
        .g_times = {0}, .g_count = 0
    };

    /* ── Vertical scroll state — one per line ────────────────────────────── */
    VertScroller_t c_scroller = { VERT_DWELL, 0, 0, 0 };
    VertScroller_t g_scroller = { VERT_DWELL, 0, 0, 0 };

    /* ── Fixed Position 6-Byte Receive Buffer ────────────────────────────── */
    uint8_t ucLocalTrainTimes[6];
    size_t  xReceivedBytes = 0;

    /* Guard: wait for buffer init */
    while (xMtaTimBuf == NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (;;)
    {
        /* 1. Non-blocking check for new positional MTA data block */
        xReceivedBytes = xStreamBufferReceive(
            xMtaTimBuf,
            (void *)ucLocalTrainTimes,
            6,
            0
        );

        /* 2. Update RTC header */
        getHeaderString(timBuf);

        /* 3. Process incoming fixed positional train data */
        if (xReceivedBytes == 6) {

            /* --- Parse C Trains (Bytes 0, 1, 2) --- */
            uint8_t ucNewCCount = 0;
            for (int i = 0; i < 3; i++) {
                if (ucLocalTrainTimes[i] != 0xFF) {
                    snapshot.c_times[ucNewCCount++] = ucLocalTrainTimes[i];
                }
            }
            snapshot.c_count = ucNewCCount;

            /* --- Parse G Trains (Bytes 3, 4, 5) --- */
            uint8_t ucNewGCount = 0;
            for (int i = 3; i < 6; i++) {
                if (ucLocalTrainTimes[i] != 0xFF) {
                    snapshot.g_times[ucNewGCount++] = ucLocalTrainTimes[i];
                }
            }
            snapshot.g_count = ucNewGCount;

            /* Reset scrollers on fresh data arrival */
            c_scroller.current_idx = 0;
            c_scroller.state       = VERT_DWELL;
            c_scroller.dwell_ticks = 0;
            c_scroller.scroll_y    = 0;

            g_scroller.current_idx = 0;
            g_scroller.state       = VERT_DWELL;
            g_scroller.dwell_ticks = 0;
            g_scroller.scroll_y    = 0;
        }

        /* 4. Tick vertical scrollers independently */
        vTickVertScroller(&c_scroller, snapshot.c_count);
        vTickVertScroller(&g_scroller, snapshot.g_count);

        /* 5. Build display strings for C and G trains */
        char c_top[6]  = "--";
        char c_next[6] = "--";
        char g_top[6]  = "--";
        char g_next[6] = "--";

        /* Populate C-line viewports */
        if (snapshot.c_count > 0) {
            snprintf(c_top,  sizeof(c_top),  "%02d",
                     snapshot.c_times[c_scroller.current_idx]);
            snprintf(c_next, sizeof(c_next), "%02d",
                     snapshot.c_times[(c_scroller.current_idx + 1) % snapshot.c_count]);
        }

        /* Populate G-line viewports */
		if (snapshot.g_count > 0) {
			snprintf(g_top,  sizeof(g_top),  "%02d",
					 snapshot.g_times[g_scroller.current_idx]);
			snprintf(g_next, sizeof(g_next), "%02d",
					 snapshot.g_times[(g_scroller.current_idx + 1) % snapshot.g_count]);
		}

        /* 6. Clear canvas */
        HUB75_Clear();

        /* 7. Top half (rows 0–7): date and time in 5x7 */
        char *spacePtr = strchr(timBuf, ' ');
        if (spacePtr != NULL) {
            *spacePtr = '\0';
            /* Day */
            HUB75_DrawString3x5(0, 1, timBuf, 255, 100, 0 );
            /* Time offset by day string width */
            HUB75_DrawString3x5((int16_t)(strlen(timBuf) * FONT5X7_CHAR_W), 1,
                                spacePtr + 1, 255, 0, 0);
            /* Restore the space so timBuf stays valid next frame */
            *spacePtr = ' ';
        } else {
            HUB75_DrawString3x5(0, 1, timBuf, 255, 0, 0);
        }

        /* 8. Bottom half (rows 8–15): C and G train columns
		 *
		 * Pixel map updated for 5x7 times (32px wide panel):
		 * x=0       : 'C' Label (5x7) -> Occupies x=0..5
		 * x=6       : C 5x7 Scroll Area (2 chars) -> Occupies x=6..17
		 * x=16      : 'G' Label (5x7) -> Occupies x=16..21 (2-pixel intentional overlay gap)
		 * x=22      : G 5x7 Scroll Area (2 chars) -> Occupies x=22..31
		 */

		/* C label — MTA blue */
		HUB75_DrawChar5x7(0, 8, 'C', 0, 60, 255);

		/* UPDATED: C train numbers changed from DrawVertScroll3x5 to DrawVertScroll5x7 */
		HUB75_DrawVertScroll5x7(5, 8, c_top, c_next,
								 c_scroller.scroll_y,
								 250, 250, 255);

		/* G label — MTA green */
		HUB75_DrawChar5x7(16, 8, 'G', 0, 185, 80);

		/* UPDATED: G train numbers changed from DrawVertScroll3x5 to DrawVertScroll5x7 */
		HUB75_DrawVertScroll5x7(21, 8, g_top, g_next,
								 g_scroller.scroll_y,
								 250, 255, 250);

		/* 9. Swap buffers */
		HUB75_SwapBuffers();

        /* 10. 50ms tick ~20fps */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
