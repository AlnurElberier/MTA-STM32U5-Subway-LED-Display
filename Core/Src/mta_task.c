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

#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED                      -0x004A
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED                      -0x004C
#endif

/* MTA API Configurations */
#define MTA_HOST          "api-endpoint.mta.info"
#define MTA_PORT          "443"
#define MAX_FEED_ARRIVALS 32
#define HTTP_BUF_SIZE     (160 * 1024)
#define STREAM_BUF_BYTES  6

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    int socket_fd;
} MtaNetContext_t;

/* UPDATED: Independent tracking arrays for positional routing */
static uint32_t gulCArrivals[MAX_FEED_ARRIVALS];
static int giCCount = 0;

static uint32_t gulGArrivals[MAX_FEED_ARRIVALS];
static int giGCount = 0;

/* =====================================================================
 * PARSER LAYER: CORE EMBEDDED PROTOCOL BUFFER STREAM DECODERS
 * ===================================================================== */

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
        if (iShift >= 32) return false;
    }
    return false;
}

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
            if (ulField == 5)
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

static void prvParseStopTimeEvent(uint8_t *pucData, uint32_t ulLen, uint32_t *pulTimeOut)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    while (p < end)
    {
        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulField == 2 && ulWire == 0)
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
            if (ulField == 4)
            {
                size_t xCopyLen = (ulLength < 7) ? ulLength : 7;
                memcpy(cStopId, p, xCopyLen);
                cStopId[xCopyLen] = '\0';
            }
            else if (ulField == 2 || ulField == 3)
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

    if ((strcmp(cStopId, "A44N") == 0 || strcmp(cStopId, "G35N") == 0) && ulTimestamp != 0)
    {
        *pulTimeOut = ulTimestamp;
        *pfFoundStop = true;
    }
}

static void prvParseTripUpdate(uint8_t *pucData, uint32_t ulLen)
{
    uint8_t *p = pucData;
    uint8_t *end = pucData + ulLen;
    char cRouteId[8] = "";
    uint32_t ulLocalArrivals[16];
    int iLocalCount = 0;

    while (p < end)
    {
        vTaskDelay(0);

        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);
            if (ulField == 1)
            {
                prvParseTripDescriptor(p, ulLength, cRouteId);
            }
            else if (ulField == 2)
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

    /* UPDATED: Route arrivals to separate storage locations based on Route ID */
    if (strcmp(cRouteId, "C") == 0)
    {
        for (int i = 0; i < iLocalCount; i++)
        {
            if (giCCount < MAX_FEED_ARRIVALS)
            {
                gulCArrivals[giCCount++] = ulLocalArrivals[i];
            }
        }
    }
    else if (strcmp(cRouteId, "G") == 0)
    {
        for (int i = 0; i < iLocalCount; i++)
        {
            if (giGCount < MAX_FEED_ARRIVALS)
            {
                gulGArrivals[giGCount++] = ulLocalArrivals[i];
            }
        }
    }
}

static uint32_t prvMtaDecodeProtobufFeed(uint8_t *pucBody, size_t xBodyLength)
{
    uint32_t ulFeedTimestamp = 0;
    uint8_t *p = pucBody;
    uint8_t *end = pucBody + xBodyLength;

    while (p < end)
    {
        vTaskDelay(0);

        uint32_t ulTag; if (!prvReadVarint(&p, end, &ulTag)) break;
        uint32_t ulField = ulTag >> 3;
        uint32_t ulWire = ulTag & 0x07;

        if (ulWire == 2)
        {
            uint32_t ulLength; prvReadVarint(&p, end, &ulLength);

            if (ulField == 1)
            {
                uint8_t *hp = p;
                uint8_t *h_end = p + ulLength;
                while (hp < h_end)
                {
                    uint32_t ulHTag; if (!prvReadVarint(&hp, h_end, &ulHTag)) break;
                    uint32_t ulHField = ulHTag >> 3;
                    uint32_t ulHWire = ulHTag & 0x07;

                    if (ulHField == 3 && ulHWire == 0)
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
                        if (ulEField == 3)
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

    struct linger xLingerOpt = { .l_onoff = 1, .l_linger = 0 };
    lwip_setsockopt(pxNet->socket_fd, SOL_SOCKET, SO_LINGER, &xLingerOpt, sizeof(xLingerOpt));

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

static int prvMtaFetchPayload(mbedtls_ssl_context *psSsl, uint8_t *pucBuffer, size_t xBufSize, const char *pcUri, uint8_t **ppucBodyOut, size_t *pxBodyLenOut)
{
    size_t xTotalReadBytes = 0;
    int iConsecutiveTimeouts = 0;
    size_t xExpectedBodyLength = 0;
    bool fParsedHeader = false;
    uint8_t *pucBodyStart = NULL;
    int ret;

    int iTxLen = snprintf((char *)pucBuffer, xBufSize,
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: STM32U5\r\n"
                          "Accept: */*\r\n"
                          "Connection: close\r\n\r\n",
                          pcUri, MTA_HOST);
    mbedtls_ssl_write(psSsl, pucBuffer, iTxLen);

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
                    if (xCurrentBodyBytes >= xExpectedBodyLength) break;
                }
            }
        }
        else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (++iConsecutiveTimeouts >= 3) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
        {
            break;
        }
        else
        {
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
 * POST-PROCESSING LAYER: POSITIONAL ALIGNMENT PIPELINE
 * ===================================================================== */

/* UPDATED: Parses lines independently into fixed array positions */
static void prvMtaPostProcessArrivals(uint32_t ulFeedTimestamp)
{
    uint8_t ucMinsToEnqueue[STREAM_BUF_BYTES];

    /* Initialize entire structural frame to 0xFF (Empty/Inactive) */
    memset(ucMinsToEnqueue, 0xFF, sizeof(ucMinsToEnqueue));

    /* 1. Process C-Train Arrivals */
    if (giCCount > 0)
    {
        qsort(gulCArrivals, giCCount, sizeof(uint32_t), prvCompareTimestamps);
        uint32_t ulBaseTime = (ulFeedTimestamp != 0) ? ulFeedTimestamp : gulCArrivals[0] - 120;

        int iIdx = 0;
        for (int i = 0; i < giCCount && iIdx < 3; i++)
        {
            int iMinsRemaining = ((int)gulCArrivals[i] - (int)ulBaseTime) / 60;
            if (iMinsRemaining >= 0)
            {
                /* 254 limit guarantees 0xFF is reserved exclusively for "No Train Available" */
                ucMinsToEnqueue[iIdx++] = (iMinsRemaining > 254) ? 254 : (uint8_t)iMinsRemaining;
            }
        }
    }

    /* 2. Process G-Train Arrivals */
    if (giGCount > 0)
    {
        qsort(gulGArrivals, giGCount, sizeof(uint32_t), prvCompareTimestamps);
        uint32_t ulBaseTime = (ulFeedTimestamp != 0) ? ulFeedTimestamp : gulGArrivals[0] - 120;

        int iIdx = 3; /* Start filling at index offset 3 */
        for (int i = 0; i < giGCount && iIdx < 6; i++)
        {
            int iMinsRemaining = ((int)gulGArrivals[i] - (int)ulBaseTime) / 60;
            if (iMinsRemaining >= 0)
            {
                ucMinsToEnqueue[iIdx++] = (iMinsRemaining > 254) ? 254 : (uint8_t)iMinsRemaining;
            }
        }
    }

    LogInfo("Positional Packet Out: C=[%d, %d, %d] | G=[%d, %d, %d]",
            ucMinsToEnqueue[0], ucMinsToEnqueue[1], ucMinsToEnqueue[2],
            ucMinsToEnqueue[3], ucMinsToEnqueue[4], ucMinsToEnqueue[5]);

    if (xMtaTimBuf != NULL)
    {
        xStreamBufferReset(xMtaTimBuf);
        xStreamBufferSend(xMtaTimBuf, (const void *)ucMinsToEnqueue, STREAM_BUF_BYTES, 0);
    }
}

static void prvMtaDispatchErrorToken(void)
{
    if (xMtaTimBuf != NULL)
    {
        xStreamBufferReset(xMtaTimBuf);
        uint8_t ucErrorTokens[STREAM_BUF_BYTES] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        xStreamBufferSend(xMtaTimBuf, ucErrorTokens, STREAM_BUF_BYTES, 0);
    }
}

/* =====================================================================
 * ORCHESTRATOR LAYER: FreeRTOS LOOP ENGINE
 * ===================================================================== */

void vMtaApiTask(void *pvParameters)
{
    MtaNetContext_t xNetCtx;
    uint8_t *pucHttpBuf = NULL;
    (void)pvParameters;

    const char *pcMtaUris[TRAIN_COUNT] = {
        "/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace",
        "/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-g"
    };
    const size_t xNumFeeds = sizeof(pcMtaUris) / sizeof(pcMtaUris[0]);

    for (;;)
    {
        bool fNetworkError = false;

        if (netif_default == NULL || !netif_is_up(netif_default) ||
            ip_addr_isany_val(*netif_ip_addr4(netif_default)))
        {
            LogWarn("Network interface offline.");
            prvMtaDispatchErrorToken();
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Clear line metrics array counters cleanly for the query interval */
        giCCount = 0;
        giGCount = 0;
        uint32_t ulLatestFeedTimestamp = 0;

        pucHttpBuf = pvPortMalloc(HTTP_BUF_SIZE);
        if (!pucHttpBuf)
        {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        for (size_t i = 0; i < xNumFeeds; i++)
        {
            if (prvMtaConnectSecureEndpoint(&xNetCtx) != 0)
            {
                fNetworkError = true;
                prvMtaDisconnectSecureEndpoint(&xNetCtx);
                break;
            }

            uint8_t *pucBody = NULL;
            size_t xBodyLength = 0;
            if (prvMtaFetchPayload(&xNetCtx.ssl, pucHttpBuf, HTTP_BUF_SIZE, pcMtaUris[i], &pucBody, &xBodyLength) != 0)
            {
                fNetworkError = true;
                prvMtaDisconnectSecureEndpoint(&xNetCtx);
                break;
            }

            uint32_t ulFeedTimestamp = prvMtaDecodeProtobufFeed(pucBody, xBodyLength);
            if (ulFeedTimestamp > ulLatestFeedTimestamp)
            {
                ulLatestFeedTimestamp = ulFeedTimestamp;
            }

            prvMtaDisconnectSecureEndpoint(&xNetCtx);
        }

        if (!fNetworkError)
        {
            prvMtaPostProcessArrivals(ulLatestFeedTimestamp);
        }
        else
        {
            prvMtaDispatchErrorToken();
        }

        if (pucHttpBuf)
        {
            vPortFree(pucHttpBuf);
            pucHttpBuf = NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}
