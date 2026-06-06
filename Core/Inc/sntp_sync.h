/* USER CODE BEGIN Header */
/**
 * @file sntp_sync.h
 * @brief Application layer logic for SNTP clock synchronization
 */
/* USER CODE END Header */

#ifndef INC_SNTP_SYNC_H_
#define INC_SNTP_SYNC_H_

#include <stdint.h>

/* The function prototype lwIP needs to see */
void vUpdateSntpRtc(uint32_t epoch_time);

#endif /* INC_SNTP_SYNC_H_ */
