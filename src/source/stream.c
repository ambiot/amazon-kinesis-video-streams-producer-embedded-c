/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Thirdparty headers */
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/xlogging.h"

/* Public headers */
#include "kvs/errors.h"
#include "kvs/mkv_generator.h"
#include "kvs/stream.h"

typedef struct DataFrame
{
    DataFrameIn_t xDataFrameIn;

    DLIST_ENTRY xClusterEntry;
    DLIST_ENTRY xDataFrameEntry;

    size_t uMkvHdrLen;
    char *pMkvHdr;
} DataFrame_t;

typedef struct Stream
{
    LOCK_HANDLE xLock;

    char *pMkvEbmlSeg;
    size_t uMkvEbmlSegLen;

    uint64_t uEarliestClusterTimestamp;
    DLIST_ENTRY xClusterPending;
    DLIST_ENTRY xDataFramePending;

    bool bHasVideoTrack;
    bool bHasAudioTrack;
} Stream_t;

static DataFrameHandle prvStreamPop(StreamHandle xStreamHandle, bool bPeek)
{
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;

    if (pxStream == NULL)
    {
        LogError("invalid argument");
    }
    else
    {
        if (Lock(pxStream->xLock) != LOCK_OK)
        {
            LogError("Failed to Lock");
        }
        else
        {
            if (DList_IsListEmpty(&(pxStream->xDataFramePending)))
            {
                /* LogInfo("No data frame to pop"); */
            }
            else
            {
                pxListHead = &(pxStream->xDataFramePending);

                if (!bPeek)
                {
                    pxListItem = DList_RemoveHeadList(pxListHead);
                }
                else
                {
                    pxListItem = pxListHead->Flink;
                }
                pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);

                if (!bPeek)
                {
                    if (pxDataFrame->xDataFrameIn.xClusterType == MKV_CLUSTER)
                    {
                        pxStream->uEarliestClusterTimestamp = pxDataFrame->xDataFrameIn.uTimestampMs;
                    }
                }
            }

            Unlock(pxStream->xLock);
        }
    }

    return pxDataFrame;
}

StreamHandle Kvs_streamCreate(VideoTrackInfo_t *pVideoTrackInfo, AudioTrackInfo_t *pAudioTrackInfo)
{
    Stream_t *pxStream = NULL;
    MkvHeader_t xMkvHeader = {0};

    if (pVideoTrackInfo == NULL)
    {
        LogError("Invalid argument");
    }
    else if ((pxStream = (Stream_t *)malloc(sizeof(Stream_t))) == NULL)
    {
        LogError("OOM: pxStream");
    }
    else
    {
        memset(pxStream, 0, sizeof(Stream_t));

        DList_InitializeListHead(&(pxStream->xClusterPending));
        DList_InitializeListHead(&(pxStream->xDataFramePending));

        if (Mkv_initializeHeaders(&xMkvHeader, pVideoTrackInfo, pAudioTrackInfo) != KVS_ERRNO_NONE)
        {
            LogError("Failed to initialize mkv headers");
            free(pxStream);
            pxStream = NULL;
        }
        else if ((pxStream->xLock = Lock_Init()) == NULL)
        {
            LogError("Failed to initialize lock");
            free(pxStream);
            pxStream = NULL;
        }
        else
        {
            pxStream->pMkvEbmlSeg = (char *)(xMkvHeader.pHeader);
            pxStream->uMkvEbmlSegLen = (size_t)(xMkvHeader.uHeaderLen);
            pxStream->bHasVideoTrack = true;
            pxStream->bHasAudioTrack = (pAudioTrackInfo == NULL) ? false : true;
        }
    }

    return pxStream;
}

void Kvs_streamTermintate(StreamHandle xStreamHandle)
{
    Stream_t *pxStream = xStreamHandle;

    if (pxStream != NULL)
    {
        free(pxStream->pMkvEbmlSeg);
        Lock_Deinit(pxStream->xLock);
        free(pxStream);
    }
}

int Kvs_streamGetMkvEbmlSegHdr(StreamHandle xStreamHandle, uint8_t **ppMkvHeader, size_t *puMkvHeaderLen)
{
    int xRes = KVS_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;

    if (pxStream == NULL || ppMkvHeader == NULL || puMkvHeaderLen == NULL)
    {
        LogError("Invalid argument");
        xRes = KVS_ERRNO_FAIL;
    }
    else if(pxStream->pMkvEbmlSeg == NULL || pxStream->uMkvEbmlSegLen == 0)
    {
        LogError("Mkv EBML and segment are not initialized");
        xRes = KVS_ERRNO_FAIL;
    }
    else
    {
        *ppMkvHeader = (uint8_t *)(pxStream->pMkvEbmlSeg);
        *puMkvHeaderLen = pxStream->uMkvEbmlSegLen;
    }

    return xRes;
}

DataFrameHandle Kvs_streamAddDataFrame(StreamHandle xStreamHandle, DataFrameIn_t *pxDataFrameIn)
{
    int xRes = KVS_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    size_t uMkvHdrLen = 0;
    DataFrame_t *pxDataFrameCurrent = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    bool bListAdded = false;
    uint64_t uClusterTimestamp = 0;
    uint16_t uDeltaTimestampMs = 0;

    if (pxStream == NULL || pxDataFrameIn == NULL)
    {
        LogError("Invalid argument");
        xRes = KVS_ERRNO_FAIL;
    }
    else if ((uMkvHdrLen = Mkv_getClusterHdrLen(pxDataFrameIn->xClusterType)) == 0 ||
        (pxDataFrame = (DataFrame_t *)malloc(sizeof(DataFrame_t) + uMkvHdrLen)) == NULL
    )
    {
        LogError("Failed to create data frame");
        xRes = KVS_ERRNO_FAIL;
    }
    else if (Lock(pxStream->xLock) != LOCK_OK)
    {
        LogError("Failed to Lock");
        xRes = KVS_ERRNO_FAIL;
    }
    else
    {
        memset(pxDataFrame, 0, sizeof(DataFrame_t));
        memcpy(pxDataFrame, pxDataFrameIn, sizeof(DataFrameIn_t));
        DList_InitializeListHead(&(pxDataFrame->xClusterEntry));
        DList_InitializeListHead(&(pxDataFrame->xDataFrameEntry));
        pxDataFrame->uMkvHdrLen = uMkvHdrLen;
        pxDataFrame->pMkvHdr = (char *)pxDataFrame + sizeof(DataFrame_t);
        uClusterTimestamp = pxStream->uEarliestClusterTimestamp;

        pxListHead = &(pxStream->xDataFramePending);
        pxListItem = pxListHead->Flink;
        while (pxListItem != pxListHead)
        {
            pxDataFrameCurrent = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
            if (pxDataFrame->xDataFrameIn.uTimestampMs < pxDataFrameCurrent->xDataFrameIn.uTimestampMs)
            {
                DList_InsertTailList(pxListItem, &(pxDataFrame->xDataFrameEntry));
                uDeltaTimestampMs = (uint16_t)(pxDataFrame->xDataFrameIn.uTimestampMs - uClusterTimestamp);
                bListAdded = true;
                break;
            }
            if (pxDataFrameCurrent->xDataFrameIn.xClusterType == MKV_CLUSTER)
            {
                uClusterTimestamp = pxDataFrameCurrent->xDataFrameIn.uTimestampMs;
            }
            pxListItem = pxListItem->Flink;
        }
        if (!bListAdded)
        {
            uDeltaTimestampMs = (uint16_t)(pxDataFrame->xDataFrameIn.uTimestampMs - uClusterTimestamp);
            DList_InsertTailList(&(pxStream->xDataFramePending), &(pxDataFrame->xDataFrameEntry));
            bListAdded = true;
        }

        Mkv_initializeClusterHdr((uint8_t *)(pxDataFrame->pMkvHdr), pxDataFrame->uMkvHdrLen, pxDataFrameIn->xClusterType, pxDataFrameIn->uDataLen, pxDataFrameIn->xTrackType, pxDataFrameIn->bIsKeyFrame, pxDataFrameIn->uTimestampMs, uDeltaTimestampMs);

        Unlock(pxStream->xLock);
    }

    if (xRes != KVS_ERRNO_NONE)
    {
        if (pxDataFrame != NULL)
        {
            free(pxDataFrame);
            pxDataFrame = NULL;
        }
    }

    return pxDataFrame;
}

DataFrameHandle Kvs_streamPop(StreamHandle xStreamHandle)
{
    return prvStreamPop(xStreamHandle, false);
}

DataFrameHandle Kvs_streamPeek(StreamHandle xStreamHandle)
{
    return prvStreamPop(xStreamHandle, true);
}

bool Kvs_streamIsEmpty(StreamHandle xStreamHandle)
{
    bool bRes = true;
    Stream_t *pxStream = xStreamHandle;

    if (pxStream != NULL)
    {
        if (Lock(pxStream->xLock) != LOCK_OK)
        {
            LogError("Failed to Lock");
        }
        else
        {
            if (!DList_IsListEmpty(&(pxStream->xDataFramePending)))
            {
                bRes = false;
            }
            Unlock(pxStream->xLock);
        }
    }

    return bRes;
}

bool Kvs_streamAvailOnTrack(StreamHandle xStreamHandle, TrackType_t xTrackType)
{
    bool bRes = false;
    Stream_t *pxStream = xStreamHandle;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    DataFrame_t *pxDataFrame = NULL;

    if (pxStream != NULL)
    {
        if (Lock(pxStream->xLock) != LOCK_OK)
        {
            LogError("Failed to Lock");
        }
        else
        {
            pxListHead = &(pxStream->xDataFramePending);
            pxListItem = pxListHead->Flink;
            while (pxListItem != pxListHead)
            {
                pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
                if (pxDataFrame->xDataFrameIn.xTrackType == xTrackType)
                {
                    bRes = true;
                    break;
                }

                pxListItem = pxListItem->Flink;
            }

            Unlock(pxStream->xLock);
        }
    }

    return bRes;
}

int Kvs_streamMemStatTotal(StreamHandle xStreamHandle, size_t *puMemTotal)
{
    int xRes = KVS_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    size_t uMemTotal = 0;

    if (pxStream == NULL || puMemTotal == NULL)
    {
        LogError("Invalid argument");
    }
    else if (Lock(pxStream->xLock) != LOCK_OK)
    {
        LogError("Failed to Lock");
    }
    else
    {
        uMemTotal += sizeof(Stream_t) + pxStream->uMkvEbmlSegLen;

        pxListHead = &(pxStream->xDataFramePending);
        pxListItem = pxListHead->Flink;
        while (pxListItem != pxListHead)
        {
            pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
            uMemTotal += pxDataFrame->xDataFrameIn.uDataLen;
            uMemTotal += sizeof(DataFrame_t) + pxDataFrame->uMkvHdrLen;
            pxListItem = pxListItem->Flink;
        }

        *puMemTotal = uMemTotal;
        Unlock(pxStream->xLock);
    }

    return xRes;
}

int Kvs_dataFrameGetContent(DataFrameHandle xDataFrameHandle, uint8_t **ppMkvHeader, size_t *puMkvHeaderLen, uint8_t **ppData, size_t *puDataLen)
{
    int xRes = KVS_ERRNO_NONE;
    DataFrame_t *pxDataFrame = xDataFrameHandle;

    if (pxDataFrame == NULL || ppMkvHeader == NULL || puMkvHeaderLen == NULL || ppData == NULL || puDataLen == NULL)
    {
        LogError("Invalid argument");
        xRes = KVS_ERRNO_FAIL;
    }
    else
    {
        *ppMkvHeader = (uint8_t *)(pxDataFrame->pMkvHdr);
        *puMkvHeaderLen = pxDataFrame->uMkvHdrLen;
        *ppData = (uint8_t *)(pxDataFrame->xDataFrameIn.pData);
        *puDataLen = pxDataFrame->xDataFrameIn.uDataLen;
    }

    return xRes;
}

void Kvs_dataFrameTerminate(DataFrameHandle xDataFrameHandle)
{
    DataFrame_t *pxDataFrame = xDataFrameHandle;

    if (pxDataFrame != NULL)
    {
        free(pxDataFrame);
    }
}