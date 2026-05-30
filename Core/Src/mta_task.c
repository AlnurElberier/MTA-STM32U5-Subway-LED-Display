#include "main.h"
#include "logging.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>  /* FIX: Explicitly provides bool, true, and false definitions */

/* lwIP network layer includes */
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* mbedTLS security layer includes */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

/* LED Matrix */
#include "led_matrix_task.h"

/* FIX: Explicitly define missing mbedTLS network module error codes for custom profiles */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED                      -0x004A
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED                      -0x004C
#endif

/* MTA API Configurations (Completely Unauthenticated Public Endpoint) */
#define MTA_HOST          "api-endpoint.mta.info"
#define MTA_PORT          "443"
#define MTA_URI           "/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace"
#define MAX_ARRIVALS      32

/* Global storage array for matched transit timestamps */
static uint32_t gulArrivalTimestamps[MAX_ARRIVALS];
static int giArrivalCount = 0;

/* =====================================================================
 * CORE EMBEDDED PROTOCOL BUFFER STREAM DECODERS
 * ===================================================================== */

/**
 * @brief Decodes a standard Base-128 Varint safely from the stream.
 */
static bool prvReadVarint(uint8_t **ppucPtr, uint8_t *pucEnd, uint32_t *pulValue)
{
    uint32_t ulResult = 0;
    int iShift = 0;
    while (*ppucPtr < pucEnd)
    {
        uint8_t ucByte = **ppucPtr;
        (*ppucPtr)++;
        ulResult |= (ucByte & 0x7F) << iShift;
        if (!(ucByte & 0x80))
        {
            *pulValue = ulResult;
            return true;
        }
        iShift += 7;
        if (iShift >= 32) return false; /* Overflow Protection */
    }
    return false; /* FIX: Added explicit terminal return to eliminate warning path */
}

/**
 * @brief Parses the TripDescriptor sub-message block to identify route_id.
 */
static void prvParseTripDescriptor(uint8_t *pucData, uint32_t ulLen, char *pcRouteIdOut)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    while (p < end)
    {
        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);
            if (ulField == 5) /* route_id string identifier field */
            {
                size_t xCopyLen = (ulLength < 7) ? ulLength : 7;
                memcpy(pcRouteIdOut, p, xCopyLen);
                pcRouteIdOut[xCopyLen] = '\0';
            }
            p += ulLength;
        }
        else
        {
            if (ulWire == 0) { uint32_t v; prvReadVarint(&p, end, &v); }
            else if (ulWire == 1) p += 8;
            else if (ulWire == 5) p += 4;
            else break;
        }
    }
}

/**
 * @brief Parses the StopTimeEvent (Arrival/Departure) sub-message for Unix Timestamps.
 */
static void prvParseStopTimeEvent(uint8_t *pucData, uint32_t ulLen, uint32_t *pulTimeOut)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    while (p < end)
    {
        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulField == 2 && ulWire == 0) /* time integer varint field */
        {
            prvReadVarint(&p, end, pulTimeOut);
        }
        else
        {
            if (ulWire == 0) { uint32_t v; prvReadVarint(&p, end, &v); }
            else if (ulWire == 2) { uint32_t l; prvReadVarint(&p, end, &l); p += l; }
            else if (ulWire == 1) p += 8;
            else if (ulWire == 5) p += 4;
            else break;
        }
    }
}

/**
 * @brief Parses StopTimeUpdate components to extract arrival times if stop matches.
 */
static void prvParseStopTimeUpdate(uint8_t *pucData, uint32_t ulLen, uint32_t *pulTimeOut, bool *pfFoundStop)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    char cStopId[8] = "";
    uint32_t ulTimestamp = 0;

    while (p < end)
    {
        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);
            if (ulField == 4) /* stop_id text block descriptor */
            {
                size_t xCopyLen = (ulLength < 7) ? ulLength : 7;
                memcpy(cStopId, p, xCopyLen);
                cStopId[xCopyLen] = '\0';
            }
            else if (ulField == 2 || ulField == 3) /* arrival (2) or departure (3) object sub-blocks */
            {
                prvParseStopTimeEvent(p, ulLength, &ulTimestamp);
            }
            p += ulLength;
        }
        else
        {
            if (ulWire == 0) { uint32_t v; prvReadVarint(&p, end, &v); }
            else if (ulWire == 1) p += 8;
            else if (ulWire == 5) p += 4;
            else break;
        }
    }

    /* Verify Target Station Filters */
    if (strcmp(cStopId, "A44N") == 0 && ulTimestamp != 0)
    {
        *pulTimeOut = ulTimestamp;
        *pfFoundStop = true;
    }
}

/**
 * @brief Processes individual TripUpdate wrappers to collect candidate timestamps.
 */
static void prvParseTripUpdate(uint8_t *pucData, uint32_t ulLen)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    char cRouteId[8] = "";
    uint32_t ulLocalArrivals[16];
    int iLocalCount = 0;

    while (p < end)
    {
        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);
            if (ulField == 1) /* TripDescriptor block */
            {
                prvParseTripDescriptor(p, ulLength, cRouteId);
            }
            else if (ulField == 2) /* StopTimeUpdate array item block */
            {
                uint32_t ulTimeVal = 0;
                bool fMatched = false;
                prvParseStopTimeUpdate(p, ulLength, &ulTimeVal, &fMatched);
                if (fMatched && iLocalCount < 16)
                {
                    ulLocalArrivals[iLocalCount++] = ulTimeVal;
                }
            }
            p += ulLength;
        }
        else
        {
            if (ulWire == 0) { uint32_t v; prvReadVarint(&p, end, &v); }
            else if (ulWire == 1) p += 8;
            else if (ulWire == 5) p += 4;
            else break;
        }
    }

    /* Sibling Promotion: If this specific trip is confirmed as the C Line, register the times */
    if (strcmp(cRouteId, "C") == 0)
    {
        for (int i = 0; i < iLocalCount; i++)
        {
            if (giArrivalCount < MAX_ARRIVALS)
            {
                gulArrivalTimestamps[giArrivalCount++] = ulLocalArrivals[i];
            }
        }
    }
}

/* Quick comparison sorting helper for chronological timestamp arrangements */
static int prvCompareTimestamps(const void *a, const void *b)
{
    uint32_t ulA = *(const uint32_t *)a;
    uint32_t ulB = *(const uint32_t *)b;
    return (ulA > ulB) - (ulA < ulB);
}

/* =====================================================================
 * NETWORK DRIVER SOCKET AND MBEDTLS DATASTREAM LOOP
 * ===================================================================== */

static int lwip_mbedtls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int ret = lwip_send(fd, buf, len, 0);
    if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (ret < 0) ? MBEDTLS_ERR_NET_SEND_FAILED : ret;
}

static int lwip_mbedtls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int ret = lwip_recv(fd, buf, len, 0);
    if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return MBEDTLS_ERR_SSL_WANT_READ;
    return (ret < 0) ? MBEDTLS_ERR_NET_RECV_FAILED : ret;
}

void vMtaApiTask(void *pvParameters)
{
    int ret;
    int socket_fd = -1;
    struct addrinfo hints, *res;

    /* Dynamically allocate network buffer chunk from FreeRTOS heap memory */
    const size_t xHttpBufSize = 96 * 1024;
    uint8_t *pucHttpBuf = NULL;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;

    (void)pvParameters;

    for (;;)
    {
    	int networkErr = 0;
    	/* Check if the network interface is actually up and has an IP address */
		if (netif_default == NULL ||
			!netif_is_up(netif_default) ||
			ip_addr_isany_val(*netif_ip_addr4(netif_default)))
		{
			LogWarn("Network is down. Skipping MTA API cycle.");

			// Send a "Connection Lost" sentinel token to the display task if needed
			if (xMtaTimBuf != NULL) {
				xStreamBufferReset(xMtaTimBuf);
				uint8_t ucErrorToken = 0xFF;
				xStreamBufferSend(xMtaTimBuf, &ucErrorToken, 1, 0);
			}

			vTaskDelay(pdMS_TO_TICKS(10000)); // Check link status again in 10 seconds
			continue; // Jump back to the start of the for(;;) loop safely
		}

        LogInfo("Connecting to api-endpoint.mta.info on port 443...");
        giArrivalCount = 0;

        pucHttpBuf = pvPortMalloc(xHttpBufSize);
        if (!pucHttpBuf)
        {
            LogError("Dynamic memory allocation failed. Postponing query cycle.");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);

        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"mta", 3) != 0 ||
            mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        {
        	networkErr = 1;
            goto loop_cleanup;
        }

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, MTA_HOST) != 0)
		{
        	networkErr = 1;
        	goto loop_cleanup;
		}

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (lwip_getaddrinfo(MTA_HOST, MTA_PORT, &hints, &res) != 0)
		{
        	networkErr = 1;
        	goto loop_cleanup;
		}

        socket_fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (socket_fd < 0 || lwip_connect(socket_fd, res->ai_addr, res->ai_addrlen) < 0)
        {
            lwip_freeaddrinfo(res);
            networkErr = 1;
            goto loop_cleanup;
        }
        lwip_freeaddrinfo(res);

        mbedtls_ssl_set_bio(&ssl, &socket_fd, lwip_mbedtls_send, lwip_mbedtls_recv, NULL);
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
        {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
            	networkErr = 1;
            	goto loop_cleanup;
            }
        }

        /* Generate plain, unauthenticated public HTTP/1.1 Header payload */
        int iTxLen = snprintf((char *)pucHttpBuf, xHttpBufSize,
                              "GET %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "User-Agent: STM32U5\r\n"
                              "Accept: */*\r\n"
                              "Connection: close\r\n\r\n",
                              MTA_URI, MTA_HOST);

        mbedtls_ssl_write(&ssl, pucHttpBuf, iTxLen);

        /* Download incoming binary packages sequentially until network stream terminates */
        size_t xTotalReadBytes = 0;
        do {
            ret = mbedtls_ssl_read(&ssl, pucHttpBuf + xTotalReadBytes, xHttpBufSize - 1 - xTotalReadBytes);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
            if (ret < 0)
			{
            	networkErr = 1;
            	goto loop_cleanup;
			}
            xTotalReadBytes += ret;
        } while (xTotalReadBytes < xHttpBufSize - 1);
        pucHttpBuf[xTotalReadBytes] = '\0';

        /* Isolate the Binary Protobuf Payload Body from HTTP text headers */
        uint8_t *pucBody = (uint8_t *)strstr((char *)pucHttpBuf, "\r\n\r\n");
        if (!pucBody)
        {
        	networkErr = 1;
        	goto loop_cleanup;
        }
        pucBody += 4;
        size_t xBodyLength = xTotalReadBytes - (pucBody - pucHttpBuf);

        /* --- Master Protobuf Stream Iterator --- */
        uint32_t ulFeedTimestamp = 0; /* New baseline internal clock variable */
        uint8_t *p = pucBody;
        uint8_t *end = pucBody + xBodyLength;
        while (p < end)
        {
            uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
            uint32_t ulField = ulTag >> 3;
            uint32_t ulWire = ulTag & 0x07;

            if (ulWire == 2)
            {
                uint32_t ulLength; prvReadVarint(&p, end, &ulLength);

                /* EXTRACT BASELINE CLOCK: Parse FeedHeader (Field 1) */
                if (ulField == 1)
                {
                    uint8_t *hp = p;
                    uint8_t *h_end = p + ulLength;
                    while (hp < h_end)
                    {
                        uint32_t ulHTag; if (!prvReadVarint(&hp, h_end, &ulHTag)) break;
                        uint32_t ulHField = ulHTag >> 3;
                        uint32_t ulHWire = ulHTag & 0x07;

                        if (ulHField == 3 && ulHWire == 0) /* timestamp field */
                        {
                            prvReadVarint(&hp, h_end, &ulFeedTimestamp);
                        }
                        else
                        {
                            if (ulHWire == 0) { uint32_t v; prvReadVarint(&hp, h_end, &v); }
                            else if (ulHWire == 2) { uint32_t l; prvReadVarint(&hp, h_end, &l); hp += l; }
                            else if (ulHWire == 1) hp += 8;
                            else if (ulHWire == 5) hp += 4;
                            else break;
                        }
                    }
                }
                /* PROCESS LIVE TRAIN ENTITIES: Parse FeedEntity (Field 2) */
                else if (ulField == 2)
                {
                    uint8_t *ep = p;
                    uint8_t *e_end = p + ulLength;
                    while (ep < e_end)
                    {
                        uint32_t ulETag; if (!prvReadVarint(&ep, e_end, &ulETag)) break;
                        uint32_t ulEField = ulETag >> 3;
                        uint32_t ulEWire = ulETag & 0x07;

                        if (ulEWire == 2)
                        {
                            uint32_t ulELength; prvReadVarint(&ep, e_end, &ulELength);
                            if (ulEField == 3) /* TripUpdate block match verified */
                            {
                                prvParseTripUpdate(ep, ulELength);
                            }
                            ep += ulELength;
                        }
                        else
                        {
                            if (ulEWire == 0) { uint32_t v; prvReadVarint(&ep, e_end, &v); }
                            else if (ulEWire == 1) ep += 8;
                            else if (ulEWire == 5) ep += 4;
                            else break;
                        }
                    }
                }
                p += ulLength;
            }
            else
            {
                if (ulWire == 0) { uint32_t v; prvReadVarint(&p, end, &v); }
                else if (ulWire == 1) p += 8;
                else if (ulWire == 5) p += 4;
                else break;
            }
        }

        /* Organize and output chronological timeline schedule data structures */
		if (giArrivalCount > 0)
		{
			qsort(gulArrivalTimestamps, giArrivalCount, sizeof(uint32_t), prvCompareTimestamps);

			/* Fall back to HTTP response header timestamp if protobuf header parser was skipped */
			if (ulFeedTimestamp == 0)
			{
				ulFeedTimestamp = gulArrivalTimestamps[0] - 120; /* Safety guestimate fallback */
			}

			// Local staging array to hold up to 5 bytes
			uint8_t ucMinsToEnqueue[5];
			int iEnqueueCount = 0;

			LogInfo("===============================================================");
			LogInfo("Upcoming C trains at Clinton-Washington Avs (Manhattan-bound):");
			LogInfo("===============================================================");

			for (int i = 0; i < giArrivalCount; i++)
			{
				/* MATH CORRECTION: Subtraction against the server data generation clock timestamp */
				int iMinsRemaining = ((int)gulArrivalTimestamps[i] - (int)ulFeedTimestamp) / 60;

				/* Guard layout check to hide records from trains that already passed */
				if (iMinsRemaining >= 0)
				{
					LogInfo("  In %2d min   [Timestamp: %lu]", iMinsRemaining, gulArrivalTimestamps[i]);

					/* ENQUEUE STEP: Collect the top 5 closest trains */
					if (iEnqueueCount < 5)
					{
						// Guard against overflow if a train is somehow > 255 mins away
						ucMinsToEnqueue[iEnqueueCount++] = (iMinsRemaining > 255) ? 255 : (uint8_t)iMinsRemaining;
					}
				}
			}
			LogInfo("===============================================================");

			/* Send the fresh snapshot to the stream buffer */
			if (xMtaTimBuf != NULL)
			{
				xStreamBufferReset(xMtaTimBuf); // Flush out the 30-second old batch
				if (iEnqueueCount > 0)
				{
					xStreamBufferSend(xMtaTimBuf, (const void *)ucMinsToEnqueue, iEnqueueCount, 0);
				}
			}
		}
		else
		{
			LogWarn("Feed synchronization success. No Manhattan-bound C lines located.");

			/* If the API says 0 trains, clear the display buffer so it doesn't show old data */
			if (xMtaTimBuf != NULL)
			{
				xStreamBufferReset(xMtaTimBuf);
			}
		}

loop_cleanup:

		// Send a "Connection Lost" sentinel token to the display task if needed
		if (networkErr && xMtaTimBuf != NULL) {
			LogWarn("Network Error: Clearing ETA Buffer.");
			xStreamBufferReset(xMtaTimBuf);
			uint8_t ucErrorToken = 0xFF;
			xStreamBufferSend(xMtaTimBuf, &ucErrorToken, 1, 0);
		}

        if (socket_fd >= 0)
        {
            mbedtls_ssl_close_notify(&ssl);
            lwip_close(socket_fd);
            socket_fd = -1;
        }
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);

        /* Wipe heap allocations immediately to maintain system stability */
        if (pucHttpBuf)
        {
            vPortFree(pucHttpBuf);
            pucHttpBuf = NULL;
        }

        /* Pull down data frames every 30 seconds */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}


