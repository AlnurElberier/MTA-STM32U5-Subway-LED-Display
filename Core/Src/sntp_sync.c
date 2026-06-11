/*
 * sntp_sync.h
 *
 *  Created on: May 30, 2026
 *      Author: aelbe
 */

#include "sntp_sync.h"
#include "FreeRTOS.h"
#include "task.h"
#include "time.h"
#include "sys_evt.h"

/* lwIP network layer includes */
#include "lwip/apps/sntp.h"


extern RTC_HandleTypeDef hrtc;


void vUpdateSntpRtc(uint32_t epoch_time)
{
    time_t raw_time = (time_t)epoch_time;
    struct tm time_info; // Allocated on the local stack

    // Apply NYC EDT offset (UTC-4)
    raw_time -= (4 * 3600);

    /* FIX: Use the thread-safe reentrant version */
    gmtime_r(&raw_time, &time_info);

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};
    char cTimeBuf[64];

    sTime.Hours   = time_info.tm_hour;
    sTime.Minutes = time_info.tm_min;
    sTime.Seconds = time_info.tm_sec;

    sDate.Year    = (time_info.tm_year >= 100) ? (time_info.tm_year - 100) : 0;
    sDate.Month   = time_info.tm_mon + 1;
    sDate.Date    = time_info.tm_mday;
    sDate.WeekDay = (time_info.tm_wday == 0) ? RTC_WEEKDAY_SUNDAY : time_info.tm_wday;

    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) == HAL_OK &&
        HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) == HAL_OK)
    {
        /* Pass the pointer to the stack variable */
        strftime(cTimeBuf, sizeof(cTimeBuf), "%Y-%m-%d %H:%M:%S UTC", &time_info);

        LogInfo("SNTP Sync Success! Hardware RTC updated to: %s", cTimeBuf);

        if (xSystemEvents != NULL)
        {
            xEventGroupSetBits(xSystemEvents, EVT_MASK_TIME_SYNCED);
        }
    }
    else
    {
        LogError("SNTP Sync: Failed to write to hardware RTC registers.");
    }
}
