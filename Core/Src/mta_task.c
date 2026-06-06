#include "main.h"
#include "logging.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* lwIP network layer includes */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"

/* mbedTLS security layer includes */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

/* LED Matrix Shared Buffer */
#include "led_matrix_task.h"

/* Explicitly define missing mbedTLS network module error codes for custom profiles */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED                      -0x004A
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED                      -0x004C
#endif

/* MTA API Configurations */
#define MTA_HOST          "api-endpoint.mta.info"
#define MTA_PORT          "443"
#define MTA_URI           "/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace"
#define MAX_ARRIVALS      32
#define HTTP_BUF_SIZE     (160 * 1024)

/* Network context wrapper structure to minimize function parameters */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    int socket_fd;
} MtaNetContext_t;

/* Global static storage array for matched transit timestamps */
static uint32_t gulArrivalTimestamps[MAX_ARRIVALS];
static int giArrivalCount = 0;

/* =====================================================================
 * PARSER LAYER: CORE EMBEDDED PROTOCOL BUFFER STREAM DECODERS
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
    return false;
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
        /* Cooperative Yield: Prevent display starvation during large array steps */
        vTaskDelay(0);

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

/**
 * @brief Master decoding parser loop that sweeps through the payload buffer.
 */
static uint32_t prvMtaDecodeProtobufFeed(uint8_t *pucBody, size_t xBodyLength)
{
    uint32_t ulFeedTimestamp = 0;
    uint8_t *p = pucBody;
    uint8_t *end = pucBody + xBodyLength;

    while (p < end)
    {
        /* Cooperative Yield: Keeps the LED Matrix tracking rows fluid during decompression */
        vTaskDelay(0);

        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);

            if (ulField == 1) /* Parse FeedHeader */
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
            else if (ulField == 2) /* Parse FeedEntity array */
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
                        if (ulEField == 3) /* TripUpdate block match */
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
    return ulFeedTimestamp;
}

static int prvCompareTimestamps(const void *a, const void *b)
{
    uint32_t ulA = *(const uint32_t *)a;
    uint32_t ulB = *(const uint32_t *)b;
    return (ulA > ulB) - (ulA < ulB);
}

/* =====================================================================
 * NETWORK LAYER: DRIVER SOCKETS AND MBEDTLS DATASTREAM INTERFACES
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

/**
 * @brief Establishes connection, configures SO_LINGER, and completes the TLS handshake.
 */
static int prvMtaConnectSecureEndpoint(MtaNetContext_t *pxNet)
{
    int ret;
    struct addrinfo hints, *res;

    mbedtls_ssl_init(&pxNet->ssl);
    mbedtls_ssl_config_init(&pxNet->conf);
    mbedtls_ctr_drbg_init(&pxNet->ctr_drbg);
    mbedtls_entropy_init(&pxNet->entropy);
    pxNet->socket_fd = -1;

    if (mbedtls_ctr_drbg_seed(&pxNet->ctr_drbg, mbedtls_entropy_func, &pxNet->entropy, (const unsigned char *)"mta", 3) != 0 ||
        mbedtls_ssl_config_defaults(&pxNet->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        return -1;
    }

    mbedtls_ssl_conf_authmode(&pxNet->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&pxNet->conf, mbedtls_ctr_drbg_random, &pxNet->ctr_drbg);

    if (mbedtls_ssl_setup(&pxNet->ssl, &pxNet->conf) != 0 || mbedtls_ssl_set_hostname(&pxNet->ssl, MTA_HOST) != 0)
    {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (lwip_getaddrinfo(MTA_HOST, MTA_PORT, &hints, &res) != 0)
    {
        return -1;
    }

    pxNet->socket_fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (pxNet->socket_fd < 0)
    {
        lwip_freeaddrinfo(res);
        return -1;
    }

    /* SO_LINGER 0: Force instant socket deletion on close to clear out PBUF memory pools */
    struct linger xLingerOpt = { .l_onoff = 1, .l_linger = 0 };
    lwip_setsockopt(pxNet->socket_fd, SOL_SOCKET, SO_LINGER, &xLingerOpt, sizeof(xLingerOpt));

    /* RCVTIMEO: Prevent tasks from blocking infinitely if the Wi-Fi link flakes */
    uint32_t ulTimeoutMs = 6000;
    lwip_setsockopt(pxNet->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &ulTimeoutMs, sizeof(ulTimeoutMs));

    if (lwip_connect(pxNet->socket_fd, res->ai_addr, res->ai_addrlen) < 0)
    {
        lwip_freeaddrinfo(res);
        lwip_close(pxNet->socket_fd);
        return -1;
    }
    lwip_freeaddrinfo(res);

    mbedtls_ssl_set_bio(&pxNet->ssl, &pxNet->socket_fd, lwip_mbedtls_send, lwip_mbedtls_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&pxNet->ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            lwip_close(pxNet->socket_fd);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Handles the secure streaming read loop and separates HTTP header metadata.
 */
static int prvMtaFetchPayload(mbedtls_ssl_context *psSsl, uint8_t *pucBuffer, size_t xBufSize, uint8_t **ppucBodyOut, size_t *pxBodyLenOut)
{
    size_t xTotalReadBytes = 0;
    int iConsecutiveTimeouts = 0;
    size_t xExpectedBodyLength = 0;
    bool fParsedHeader = false;
    uint8_t *pucBodyStart = NULL;
    int ret;

    /* Assemble and transmit public plaintext GET string */
    int iTxLen = snprintf((char *)pucBuffer, xBufSize,
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: STM32U5\r\n"
                          "Accept: */*\r\n"
                          "Connection: close\r\n\r\n",
                          MTA_URI, MTA_HOST);
    mbedtls_ssl_write(psSsl, pucBuffer, iTxLen);

    /* Ingress streaming engine */
    do {
        size_t xRemainingSpace = xBufSize - 1 - xTotalReadBytes;
        if (xRemainingSpace == 0) break;

        ret = mbedtls_ssl_read(psSsl, &pucBuffer[xTotalReadBytes], xRemainingSpace);

        if (ret > 0)
        {
            xTotalReadBytes += ret;
            iConsecutiveTimeouts = 0;
            pucBuffer[xTotalReadBytes] = '\0';

            if (!fParsedHeader)
            {
                char *pcContentLengthStr = strstr((char *)pucBuffer, "Content-Length: ");
                if (pcContentLengthStr != NULL)
                {
                    xExpectedBodyLength = strtoul(pcContentLengthStr + 16, NULL, 10);
                    fParsedHeader = true;
                    LogInfo("Found HTTP Content-Length: %d bytes", xExpectedBodyLength);
                }
            }

            if (fParsedHeader && xExpectedBodyLength > 0)
            {
                if (pucBodyStart == NULL)
                {
                    pucBodyStart = (uint8_t *)strstr((char *)pucBuffer, "\r\n\r\n");
                    if (pucBodyStart != NULL) pucBodyStart += 4;
                }

                if (pucBodyStart != NULL)
                {
                    size_t xCurrentBodyBytes = xTotalReadBytes - (pucBodyStart - pucBuffer);
                    if (xCurrentBodyBytes >= xExpectedBodyLength) break; /* Download complete! */
                }
            }
        }
        else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (++iConsecutiveTimeouts >= 3) {
                LogWarn("Data stream stalled. Processing partial payload of %d bytes.", xTotalReadBytes);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
        {
            break;
        }
        else
        {
            LogError("mbedTLS secure read error: %d", ret);
            return -1;
        }
    } while (xTotalReadBytes < xBufSize - 1);

    if (pucBodyStart == NULL)
    {
        pucBodyStart = (uint8_t *)strstr((char *)pucBuffer, "\r\n\r\n");
        if (pucBodyStart == NULL) return -1;
        pucBodyStart += 4;
    }

    *ppucBodyOut = pucBodyStart;
    *pxBodyLenOut = xTotalReadBytes - (pucBodyStart - pucBuffer);
    return 0;
}

/**
 * @brief Tears down secure context handles and closes the physical socket descriptor.
 */
static void prvMtaDisconnectSecureEndpoint(MtaNetContext_t *pxNet)
{
    if (pxNet->socket_fd >= 0)
    {
        mbedtls_ssl_close_notify(&pxNet->ssl);
        lwip_close(pxNet->socket_fd);
        pxNet->socket_fd = -1;
    }
    mbedtls_ssl_free(&pxNet->ssl);
    mbedtls_ssl_config_free(&pxNet->conf);
    mbedtls_ctr_drbg_free(&pxNet->ctr_drbg);
    mbedtls_entropy_free(&pxNet->entropy);
}

/* =====================================================================
 * POST-PROCESSING LAYER: CALCULATION PIPELINES & IPC DISPATCHERS
 * ===================================================================== */

/**
 * @brief Computes delta relative minutes and writes snapshots to the StreamBuffer.
 */
static void prvMtaPostProcessArrivals(uint32_t ulFeedTimestamp)
{
    if (giArrivalCount > 0)
    {
        qsort(gulArrivalTimestamps, giArrivalCount, sizeof(uint32_t), prvCompareTimestamps);

        if (ulFeedTimestamp == 0)
        {
            ulFeedTimestamp = gulArrivalTimestamps[0] - 120; /* Guestimate offset safety window */
        }

        uint8_t ucMinsToEnqueue[5];
        int iEnqueueCount = 0;

        LogInfo("===============================================================");
        LogInfo("Upcoming C trains at Clinton-Washington Avs (Manhattan-bound):");
        LogInfo("===============================================================");

        for (int i = 0; i < giArrivalCount; i++)
        {
            int iMinsRemaining = ((int)gulArrivalTimestamps[i] - (int)ulFeedTimestamp) / 60;

            if (iMinsRemaining >= 0)
            {
                LogInfo("  In %2d min   [Timestamp: %lu]", iMinsRemaining, gulArrivalTimestamps[i]);

                if (iEnqueueCount < 5)
                {
                    ucMinsToEnqueue[iEnqueueCount++] = (iMinsRemaining > 255) ? 255 : (uint8_t)iMinsRemaining;
                }
            }
        }
        LogInfo("===============================================================");

        if (xMtaTimBuf != NULL)
        {
            xStreamBufferReset(xMtaTimBuf);
            if (iEnqueueCount > 0)
            {
                xStreamBufferSend(xMtaTimBuf, (const void *)ucMinsToEnqueue, iEnqueueCount, 0);
            }
        }
    }
    else
    {
        LogWarn("Feed synchronization success. No Manhattan-bound C lines located.");
        if (xMtaTimBuf != NULL) xStreamBufferReset(xMtaTimBuf);
    }
}

/**
 * @brief Dispatch error sentinel token to prevent the display from showing frozen metrics.
 */
static void prvMtaDispatchErrorToken(void)
{
    if (xMtaTimBuf != NULL)
    {
        xStreamBufferReset(xMtaTimBuf);
        uint8_t ucErrorToken = 0xFF;
        xStreamBufferSend(xMtaTimBuf, &ucErrorToken, 1, 0);
    }
}

/* =====================================================================
 * ORCHESTRATOR LAYER: THE MASTER FreeRTOS LOOP ENGINE
 * ===================================================================== */

void vMtaApiTask(void *pvParameters)
{
    MtaNetContext_t xNetCtx;
    uint8_t *pucHttpBuf = NULL;
    (void)pvParameters;

    for (;;)
    {
        bool fNetworkError = false;

        /* 1. Network hardware guard check */
        if (netif_default == NULL || !netif_is_up(netif_default) ||
            ip_addr_isany_val(*netif_ip_addr4(netif_default)))
        {
            LogWarn("Network interface is offline. Postponing cycle.");
            prvMtaDispatchErrorToken();
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        giArrivalCount = 0;

        /* 2. FreeRTOS Heap allocation */
        pucHttpBuf = pvPortMalloc(HTTP_BUF_SIZE);
        if (!pucHttpBuf)
        {
            LogError("RAM allocation failed. Postponing cycle.");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* 3. Secure Handshake Connection Execution */
        if (prvMtaConnectSecureEndpoint(&xNetCtx) != 0)
        {
            LogError("Secure endpoint connection failed.");
            fNetworkError = true;
            goto loop_cleanup;
        }

        /* 4. Stream extraction parsing */
        uint8_t *pucBody = NULL;
        size_t xBodyLength = 0;
        if (prvMtaFetchPayload(&xNetCtx.ssl, pucHttpBuf, HTTP_BUF_SIZE, &pucBody, &xBodyLength) != 0)
        {
            LogError("Payload retrieval failed.");
            fNetworkError = true;
            goto loop_cleanup;
        }

        /* 5. Zero-Copy Protobuf Data Parse */
        uint32_t ulFeedTimestamp = prvMtaDecodeProtobufFeed(pucBody, xBodyLength);

        /* 6. Post-Process Chronology Sorting & IPC Dispatch */
        prvMtaPostProcessArrivals(ulFeedTimestamp);

loop_cleanup:

        if (fNetworkError)
        {
            prvMtaDispatchErrorToken();
        }

        /* 7. Drop socket tracking blocks instantly */
        prvMtaDisconnectSecureEndpoint(&xNetCtx);

        if (pucHttpBuf)
        {
            vPortFree(pucHttpBuf);
            pucHttpBuf = NULL;
        }

        /* 8. Wait 10 seconds before downloading the next live snapshot */
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
