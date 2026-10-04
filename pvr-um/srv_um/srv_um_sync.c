/* SPDX-License-Identifier: MIT */
/*
 * Sync objects and sync operations.
 *
 * A kernel sync info counts pending and completed read, write and read2
 * operations on a buffer. The flush calls compare a snapshot of the pending
 * counts against the completed counts and report PVRSRV_ERROR_RETRY while
 * operations remain (DoQuerySyncOpsSatisfied in the kernel); callers poll.
 */
#include "srv_um_priv.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* SrvFlushClientOps polls for at most this long, sleeping between polls. */
#define SRV_FLUSH_TIMEOUT_US	500000u
#define SRV_FLUSH_POLL_US	50u

static IMG_BOOL SrvConnValid(const PVRSRV_CONNECTION *psConnection, const char *pszFunc)
{
	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("%s: Invalid connection", pszFunc);
		return IMG_FALSE;
	}
	return IMG_TRUE;
}

/* Every output structure of the sync-ops bridge calls starts with eError. */
_Static_assert(offsetof(PVRSRV_BRIDGE_RETURN, eError) == 0, "eError first");
_Static_assert(offsetof(PVRSRV_BRIDGE_OUT_CREATE_SYNC_INFO_MOD_OBJ, eError) == 0, "eError first");
_Static_assert(offsetof(PVRSRV_BRIDGE_OUT_MODIFY_PENDING_SYNC_OPS, eError) == 0, "eError first");
_Static_assert(offsetof(PVRSRV_BRIDGE_OUT_SYNC_OPS_TAKE_TOKEN, eError) == 0, "eError first");

/* Bridge call returning the kernel's eError. RETRY is the expected
 * not-yet-complete answer of the flush calls and is not logged. */
static PVRSRV_ERROR SrvSyncCall(const PVRSRV_CONNECTION *psConnection, const char *pszFunc,
				IMG_UINT32 ui32BridgeID, IMG_VOID *pvIn, IMG_UINT32 ui32InSize,
				IMG_VOID *pvOut, IMG_UINT32 ui32OutSize)
{
	PVRSRV_ERROR eError;

	if (SrvBridgeCall(SrvServices(psConnection), ui32BridgeID, pvIn, ui32InSize,
			  pvOut, ui32OutSize) != 0)
	{
		SRV_ERR("%s: bridge call failed", pszFunc);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	memcpy(&eError, pvOut, sizeof(eError));
	if (eError != PVRSRV_OK && eError != PVRSRV_ERROR_RETRY)
	{
		SRV_ERR("%s: kernel call failed (%d)", pszFunc, eError);
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCreateSyncInfoModObj(const PVRSRV_CONNECTION *psConnection,
						   IMG_HANDLE *phKernelSyncInfoModObj)
{
	PVRSRV_BRIDGE_OUT_CREATE_SYNC_INFO_MOD_OBJ sOut;
	PVRSRV_ERROR eError;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (phKernelSyncInfoModObj == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sOut, 0, sizeof(sOut));
	eError = SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_CREATE_SYNC_INFO_MOD_OBJ,
			     IMG_NULL, 0, &sOut, sizeof(sOut));
	if (eError == PVRSRV_OK)
	{
		*phKernelSyncInfoModObj = sOut.hKernelSyncInfoModObj;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDestroySyncInfoModObj(const PVRSRV_CONNECTION *psConnection,
						    IMG_HANDLE hKernelSyncInfoModObj)
{
	PVRSRV_BRIDGE_IN_DESTROY_SYNC_INFO_MOD_OBJ sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfoModObj = hKernelSyncInfoModObj;
	return SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_DESTROY_SYNC_INFO_MOD_OBJ,
			   &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

IMG_EXPORT PVRSRV_ERROR PVRSRVModifyPendingSyncOps(const PVRSRV_CONNECTION *psConnection,
						   IMG_HANDLE hKernelSyncInfoModObj,
						   PVRSRV_CLIENT_SYNC_INFO *psSyncInfo,
						   IMG_UINT32 ui32ModifyFlags,
						   IMG_UINT32 *pui32ReadOpsPending,
						   IMG_UINT32 *pui32WriteOpsPending)
{
	PVRSRV_BRIDGE_IN_MODIFY_PENDING_SYNC_OPS sIn;
	PVRSRV_BRIDGE_OUT_MODIFY_PENDING_SYNC_OPS sOut;
	PVRSRV_ERROR eError;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (psSyncInfo == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfoModObj = hKernelSyncInfoModObj;
	sIn.hKernelSyncInfo = psSyncInfo->hKernelSyncInfo;
	sIn.ui32ModifyFlags = ui32ModifyFlags;

	eError = SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_MODIFY_PENDING_SYNC_OPS,
			     &sIn, sizeof(sIn), &sOut, sizeof(sOut));
	if (eError == PVRSRV_OK)
	{
		if (pui32ReadOpsPending != IMG_NULL)
		{
			*pui32ReadOpsPending = sOut.ui32ReadOpsPending;
		}
		if (pui32WriteOpsPending != IMG_NULL)
		{
			*pui32WriteOpsPending = sOut.ui32WriteOpsPending;
		}
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVModifyCompleteSyncOps(const PVRSRV_CONNECTION *psConnection,
						    IMG_HANDLE hKernelSyncInfoModObj)
{
	PVRSRV_BRIDGE_IN_MODIFY_COMPLETE_SYNC_OPS sIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_ERROR eError;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfoModObj = hKernelSyncInfoModObj;

	eError = SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_MODIFY_COMPLETE_SYNC_OPS,
			     &sIn, sizeof(sIn), &sOut, sizeof(sOut));
	if (eError == PVRSRV_ERROR_BAD_SYNC_STATE)
	{
		SRV_ERR("%s: Bad Synchronisation State: the modification object has no "
			"pending operation to complete", __func__);
	}
	else if (eError == PVRSRV_ERROR_RETRY)
	{
		SRV_ERR("%s: kernel call failed (%d)", __func__, eError);
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSyncOpsTakeToken(const PVRSRV_CONNECTION *psConnection,
					       const PVRSRV_CLIENT_SYNC_INFO *psSyncInfo,
					       PVRSRV_SYNC_TOKEN *psSyncToken)
{
	PVRSRV_BRIDGE_IN_SYNC_OPS_TAKE_TOKEN sIn;
	PVRSRV_BRIDGE_OUT_SYNC_OPS_TAKE_TOKEN sOut;
	PVRSRV_ERROR eError;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (psSyncInfo == IMG_NULL || psSyncToken == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfo = psSyncInfo->hKernelSyncInfo;

	eError = SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_SYNC_OPS_TAKE_TOKEN,
			     &sIn, sizeof(sIn), &sOut, sizeof(sOut));
	if (eError == PVRSRV_OK)
	{
		psSyncToken->sPrivate.hKernelSyncInfo = psSyncInfo->hKernelSyncInfo;
		psSyncToken->sPrivate.ui32ReadOpsPendingSnapshot = sOut.ui32ReadOpsPending;
		psSyncToken->sPrivate.ui32WriteOpsPendingSnapshot = sOut.ui32WriteOpsPending;
		psSyncToken->sPrivate.ui32ReadOps2PendingSnapshot = sOut.ui32ReadOps2Pending;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSyncOpsFlushToToken(const PVRSRV_CONNECTION *psConnection,
						  const PVRSRV_CLIENT_SYNC_INFO *psSyncInfo,
						  const PVRSRV_SYNC_TOKEN *psSyncToken,
						  IMG_BOOL bWait)
{
	PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_TOKEN sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (psSyncInfo == IMG_NULL || psSyncToken == IMG_NULL ||
	    psSyncToken->sPrivate.hKernelSyncInfo != psSyncInfo->hKernelSyncInfo)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (bWait)
	{
		SRV_ERR("%s: blocking call not supported", __func__);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfo = psSyncInfo->hKernelSyncInfo;
	sIn.ui32ReadOpsPendingSnapshot = psSyncToken->sPrivate.ui32ReadOpsPendingSnapshot;
	sIn.ui32WriteOpsPendingSnapshot = psSyncToken->sPrivate.ui32WriteOpsPendingSnapshot;
	sIn.ui32ReadOps2PendingSnapshot = psSyncToken->sPrivate.ui32ReadOps2PendingSnapshot;

	return SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_TOKEN,
			   &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSyncOpsFlushToModObj(const PVRSRV_CONNECTION *psConnection,
						   IMG_HANDLE hKernelSyncInfoModObj,
						   IMG_BOOL bWait)
{
	PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_MOD_OBJ sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (bWait)
	{
		SRV_ERR("%s: blocking call not supported", __func__);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfoModObj = hKernelSyncInfoModObj;
	return SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_MOD_OBJ,
			   &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSyncOpsFlushToDelta(const PVRSRV_CONNECTION *psConnection,
						  PVRSRV_CLIENT_SYNC_INFO *psClientSyncInfo,
						  IMG_UINT32 ui32Delta,
						  IMG_BOOL bWait)
{
	PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_DELTA sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (!SrvConnValid(psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (psClientSyncInfo == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if (bWait)
	{
		SRV_ERR("%s: blocking call not supported", __func__);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfo = psClientSyncInfo->hKernelSyncInfo;
	sIn.ui32Delta = ui32Delta;
	return SrvSyncCall(psConnection, __func__, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_DELTA,
			   &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

/* The sync info returned here carries only the kernel handle: the kernel
 * hands out no CPU mapping of the sync data and no device addresses for
 * stand-alone sync objects. */
IMG_EXPORT PVRSRV_ERROR PVRSRVAllocSyncInfo(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					    PVRSRV_CLIENT_SYNC_INFO **ppsSyncInfo)
{
	PVRSRV_BRIDGE_IN_ALLOC_SYNC_INFO sIn;
	PVRSRV_BRIDGE_OUT_ALLOC_SYNC_INFO sOut;
	PVRSRV_CLIENT_SYNC_INFO *psSyncInfo;

	if (psDevData == IMG_NULL || ppsSyncInfo == IMG_NULL ||
	    !SrvConnValid(psDevData->psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psSyncInfo = calloc(1, sizeof(*psSyncInfo));
	if (psSyncInfo == IMG_NULL)
	{
		SRV_ERR("%s: out of memory", __func__);
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_ALLOC_SYNC_INFO,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("%s: bridge call failed", __func__);
		free(psSyncInfo);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("%s: ALLOC_SYNC_INFO failed (%d)", __func__, sOut.eError);
		free(psSyncInfo);
		return sOut.eError;
	}

	psSyncInfo->hKernelSyncInfo = sOut.hKernelSyncInfo;
	*ppsSyncInfo = psSyncInfo;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVFreeSyncInfo(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					   PVRSRV_CLIENT_SYNC_INFO *psSyncInfo)
{
	PVRSRV_BRIDGE_IN_FREE_SYNC_INFO sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevData == IMG_NULL || psSyncInfo == IMG_NULL ||
	    !SrvConnValid(psDevData->psConnection, __func__))
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelSyncInfo = psSyncInfo->hKernelSyncInfo;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_FREE_SYNC_INFO,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("%s: bridge call failed", __func__);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("%s: FREE_SYNC_INFO failed (%d)", __func__, sOut.eError);
		return sOut.eError;
	}
	free(psSyncInfo);
	return PVRSRV_OK;
}

/* Takes a token of the pending counts and polls FLUSH_TO_TOKEN until the
 * completed counts reach it, for at most SRV_FLUSH_TIMEOUT_US. */
IMG_INTERNAL PVRSRV_ERROR SrvFlushClientOps(const PVRSRV_CONNECTION *psConnection,
					    const PVRSRV_CLIENT_SYNC_INFO *psSyncInfo)
{
	PVRSRV_SYNC_TOKEN sToken;
	IMG_UINT32 ui32Start;
	PVRSRV_ERROR eError;

	if (psSyncInfo == IMG_NULL)
	{
		SRV_ERR("%s: no sync info", __func__);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	ui32Start = PVRSRVClockus();
	eError = PVRSRVSyncOpsTakeToken(psConnection, psSyncInfo, &sToken);
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("%s: cannot take a sync token (%d)", __func__, eError);
		return eError;
	}

	while ((eError = PVRSRVSyncOpsFlushToToken(psConnection, psSyncInfo, &sToken,
						   IMG_FALSE)) == PVRSRV_ERROR_RETRY)
	{
		if ((IMG_UINT32)(PVRSRVClockus() - ui32Start) > SRV_FLUSH_TIMEOUT_US)
		{
			SRV_ERR("%s: ops pending timeout", __func__);
			return PVRSRV_ERROR_TIMEOUT_POLLING_FOR_VALUE;
		}
		PVRSRVWaitus(SRV_FLUSH_POLL_US);
	}
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("%s: flush failed (%d)", __func__, eError);
	}
	return eError;
}
