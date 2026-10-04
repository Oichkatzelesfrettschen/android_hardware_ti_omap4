/* SPDX-License-Identifier: MIT */
/*
 * Misc info, event objects, value polling and client events.
 */
#include "srv_um_priv.h"

#include <stdint.h>
#include <string.h>

/* hw_get_module as declared in hardware/libhardware/include/hardware/
 * hardware.h; the module is used only as an opaque byte layout here. */
struct hw_module_t;
extern int hw_get_module(const char *id, const struct hw_module_t **module);

/* Byte offset of the debug-dump entry in the OMAP4 gralloc module's private
 * structure; the entry takes the module as its only argument. */
#define SRV_GRALLOC_DUMP_OFFSET	212u

IMG_INTERNAL PVRSRV_ERROR SrvEventObjectOpen(const PVRSRV_CONNECTION *psConnection,
					     const PVRSRV_EVENTOBJECT *psEventObject,
					     IMG_HANDLE *phOSEvent)
{
	PVRSRV_BRIDGE_IN_EVENT_OBJECT_OPEN sIn;
	PVRSRV_BRIDGE_OUT_EVENT_OBJECT_OPEN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("SrvEventObjectOpen: Invalid connection");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* PVRSRVEventObjectOpenBW resolves sEventObject.hOSEventKM (a shared
	 * event-object handle from GET_MISC_INFO) and returns a per-process
	 * connection handle for EVENT_OBJECT_WAIT. */
	memset(&sOut, 0, sizeof(sOut));
	sIn.sEventObject = *psEventObject;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_EVENT_OBJECT_OPEN,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("SrvEventObjectOpen: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError == PVRSRV_OK)
	{
		*phOSEvent = sOut.hOSEvent;
	}
	return sOut.eError;
}

IMG_INTERNAL PVRSRV_ERROR SrvEventObjectClose(const PVRSRV_CONNECTION *psConnection,
					      const PVRSRV_EVENTOBJECT *psEventObject,
					      IMG_HANDLE hOSEventKM)
{
	PVRSRV_BRIDGE_IN_EVENT_OBJECT_CLOSE sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("SrvEventObjectClose: Invalid connection");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sOut, 0, sizeof(sOut));
	sIn.sEventObject = *psEventObject;
	sIn.hOSEventKM = hOSEventKM;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_EVENT_OBJECT_CLOSE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("SrvEventObjectClose: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return sOut.eError;
}

/* OSEventObjectWaitKM returns after a signal or after EVENT_OBJECT_TIMEOUT_MS
 * (100 ms on hardware builds), reporting PVRSRV_ERROR_TIMEOUT in the latter
 * case. */
IMG_INTERNAL PVRSRV_ERROR SrvEventObjectWait(const PVRSRV_CONNECTION *psConnection,
					     IMG_HANDLE hOSEventKM)
{
	PVRSRV_BRIDGE_IN_EVENT_OBJECT_WAIT sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("SrvEventObjectWait: Invalid connection");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hOSEventKM = hOSEventKM;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_EVENT_OBJECT_WAIT,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("SrvEventObjectWait: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return sOut.eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVEventObjectWait(const PVRSRV_CONNECTION *psConnection,
					      IMG_HANDLE hOSEvent)
{
	return SrvEventObjectWait(psConnection, hOSEvent);
}

/*
 * PVRSRVGetMiscInfoBW copies the request structure, fills the requested
 * fields and copies it back. Two request kinds name a client meminfo that
 * the kernel knows only by its kernel handle: the cache-operation target
 * and the ref-count target. The client pointers are swapped for the
 * kernel handles across the call and restored afterwards on every path.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVGetMiscInfo(IMG_CONST PVRSRV_CONNECTION *psConnection,
					  PVRSRV_MISC_INFO *psMiscInfo)
{
	PVRSRV_BRIDGE_IN_GET_MISC_INFO sIn;
	PVRSRV_BRIDGE_OUT_GET_MISC_INFO sOut;
	PVRSRV_CLIENT_MEM_INFO *psCacheOpMemInfo = IMG_NULL;
	PVRSRV_CLIENT_MEM_INFO *psRefCountMemInfo = IMG_NULL;
	IMG_UINT32 ui32Request;
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_ERROR eEventError;

	if (psMiscInfo == IMG_NULL || psConnection == IMG_NULL ||
	    psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetMiscInfo: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	ui32Request = psMiscInfo->ui32StateRequest;
	if (ui32Request == 0)
	{
		return PVRSRV_OK;
	}
	if ((ui32Request & PVRSRV_MISC_INFO_GET_REF_COUNT_PRESENT) != 0 &&
	    psMiscInfo->sGetRefCountCtl.u.psClientMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetMiscInfo: ref-count request without a meminfo");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* The cache operation runs in the kernel (PVRSRVGetMiscInfoKM handles
	 * CPUCACHEOP_PRESENT for both deferred and immediate requests). */
	if ((ui32Request & PVRSRV_MISC_INFO_CPUCACHEOP_PRESENT) != 0)
	{
		psCacheOpMemInfo = psMiscInfo->sCacheOpCtl.u.psClientMemInfo;
		if (psCacheOpMemInfo != IMG_NULL)
		{
			psMiscInfo->sCacheOpCtl.u.psKernelMemInfo = psCacheOpMemInfo->hKernelMemInfo;
		}
	}
	if ((ui32Request & PVRSRV_MISC_INFO_GET_REF_COUNT_PRESENT) != 0)
	{
		psRefCountMemInfo = psMiscInfo->sGetRefCountCtl.u.psClientMemInfo;
		psMiscInfo->sGetRefCountCtl.u.psKernelMemInfo = psRefCountMemInfo->hKernelMemInfo;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.sMiscInfo = *psMiscInfo;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_GET_MISC_INFO,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVGetMiscInfo: bridge call failed");
		eError = PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	else if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVGetMiscInfo: GET_MISC_INFO failed (%d)", sOut.eError);
		eError = sOut.eError;
	}
	else
	{
		*psMiscInfo = sOut.sMiscInfo;
	}

	if (psCacheOpMemInfo != IMG_NULL)
	{
		psMiscInfo->sCacheOpCtl.u.psClientMemInfo = psCacheOpMemInfo;
	}
	if (psRefCountMemInfo != IMG_NULL)
	{
		psMiscInfo->sGetRefCountCtl.u.psClientMemInfo = psRefCountMemInfo;
	}
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	/*
	 * The SOC timer register block is a kernel allocation; map it for the
	 * caller. GET_MISC_INFO returns a shared handle for the global event
	 * object; EVENT_OBJECT_OPEN turns it into a wait handle for this
	 * process. The call succeeds only with both resources held. On a
	 * failure the one already acquired is released and both present bits
	 * are cleared, so the caller holds nothing and a following
	 * PVRSRVReleaseMiscInfo releases nothing.
	 */
	if ((psMiscInfo->ui32StatePresent & PVRSRV_MISC_INFO_TIMER_PRESENT) != 0)
	{
		eError = SrvMapKernelMem(SrvServices(psConnection),
					 &psMiscInfo->pvSOCTimerRegisterUM,
					 &psMiscInfo->hSOCTimerRegisterMappingInfo,
					 psMiscInfo->hSOCTimerRegisterOSMemHandle);
		if (eError != PVRSRV_OK)
		{
			SRV_ERR("PVRSRVGetMiscInfo: cannot map the SOC timer register (%d)", eError);
			psMiscInfo->ui32StatePresent &= ~(IMG_UINT32)(PVRSRV_MISC_INFO_TIMER_PRESENT |
								     PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT);
			return eError;
		}
	}

	if ((psMiscInfo->ui32StatePresent & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT) != 0)
	{
		eEventError = SrvEventObjectOpen(psConnection, &psMiscInfo->sGlobalEventObject,
						 &psMiscInfo->hOSGlobalEvent);
		if (eEventError != PVRSRV_OK)
		{
			SRV_ERR("PVRSRVGetMiscInfo: cannot open the global event object (%d)",
				eEventError);
			if ((psMiscInfo->ui32StatePresent & PVRSRV_MISC_INFO_TIMER_PRESENT) != 0 &&
			    psMiscInfo->pvSOCTimerRegisterUM != IMG_NULL)
			{
				(void)SrvUnmapKernelMem(SrvServices(psConnection),
							psMiscInfo->hSOCTimerRegisterMappingInfo,
							psMiscInfo->hSOCTimerRegisterOSMemHandle);
				psMiscInfo->pvSOCTimerRegisterUM = IMG_NULL;
			}
			psMiscInfo->ui32StatePresent &= ~(IMG_UINT32)(PVRSRV_MISC_INFO_TIMER_PRESENT |
								     PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT);
			return eEventError;
		}
	}

	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVReleaseMiscInfo(IMG_CONST PVRSRV_CONNECTION *psConnection,
					      PVRSRV_MISC_INFO *psMiscInfo)
{
	if (psMiscInfo == IMG_NULL || psConnection == IMG_NULL ||
	    psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("PVRSRVReleaseMiscInfo: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if ((psMiscInfo->ui32StatePresent & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT) != 0)
	{
		(void)SrvEventObjectClose(psConnection, &psMiscInfo->sGlobalEventObject,
					  psMiscInfo->hOSGlobalEvent);
	}
	if ((psMiscInfo->ui32StatePresent & PVRSRV_MISC_INFO_TIMER_PRESENT) != 0 &&
	    psMiscInfo->pvSOCTimerRegisterUM != IMG_NULL)
	{
		(void)SrvUnmapKernelMem(SrvServices(psConnection),
					psMiscInfo->hSOCTimerRegisterMappingInfo,
					psMiscInfo->hSOCTimerRegisterOSMemHandle);
	}
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVPollForValue(const PVRSRV_CONNECTION *psConnection,
					   IMG_HANDLE hOSEvent,
					   volatile IMG_UINT32 *pui32LinMemAddr,
					   IMG_UINT32 ui32Value,
					   IMG_UINT32 ui32Mask,
					   IMG_UINT32 ui32Waitus,
					   IMG_UINT32 ui32Tries)
{
	IMG_UINT64 ui64Budget = (IMG_UINT64)ui32Waitus * ui32Tries;
	IMG_UINT64 ui64Elapsed = 0;
	IMG_UINT32 ui32Last;
	IMG_UINT32 ui32Waits = 0;

	if (pui32LinMemAddr == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/*
	 * Times out only after both the time budget and the wait count are
	 * spent, so early event signals cannot cut the poll short. The budget
	 * is a 64-bit product while PVRSRVClockus wraps every 2^32 us, so the
	 * elapsed time sums the unsigned 32-bit differences of consecutive
	 * readings; each difference spans one wait, which stays below the
	 * 2^32 us one clock difference can measure.
	 */
	ui32Last = PVRSRVClockus();
	while ((*pui32LinMemAddr & ui32Mask) != ui32Value)
	{
		IMG_UINT32 ui32Now = PVRSRVClockus();

		ui64Elapsed += (IMG_UINT32)(ui32Now - ui32Last);
		ui32Last = ui32Now;
		if (ui64Elapsed > ui64Budget && ui32Waits >= ui32Tries)
		{
			return PVRSRV_ERROR_TIMEOUT_POLLING_FOR_VALUE;
		}
		if (hOSEvent != IMG_NULL)
		{
			(void)PVRSRVEventObjectWait(psConnection, hOSEvent);
		}
		else
		{
			PVRSRVWaitus(ui32Waitus);
		}
		ui32Waits++;
	}
	return PVRSRV_OK;
}

/* Called by client drivers when the GPU missed a deadline: dumps gralloc's
 * buffer state and each device's trace through its dump-trace callback. */
IMG_EXPORT PVRSRV_ERROR PVRSRVClientEvent(IMG_CONST PVRSRV_CLIENT_EVENT eEvent,
					  PVRSRV_DEV_DATA *psDevData,
					  IMG_PVOID pvData)
{
	const struct hw_module_t *psModule = pvData;
	void (*pfnDump)(const struct hw_module_t *psModule) = IMG_NULL;

	if (eEvent != PVRSRV_CLIENT_EVENT_HWTIMEOUT)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	SRV_ERR("HW operation timeout occurred.");

	if (psModule == IMG_NULL && hw_get_module("gralloc", &psModule) != 0)
	{
		psModule = IMG_NULL;
	}
	if (psModule != IMG_NULL)
	{
		memcpy((void *)&pfnDump, (const IMG_UINT8 *)psModule + SRV_GRALLOC_DUMP_OFFSET,
		       sizeof(pfnDump));
		if (pfnDump != IMG_NULL)
		{
			pfnDump(psModule);
		}
	}

	if (psDevData != IMG_NULL && psDevData->psConnection != IMG_NULL)
	{
		const PVRSRV_CLIENT_DEV_DATA *psDevs = &psDevData->psConnection->sClientDevData;
		IMG_UINT32 i;

		for (i = 0; i < psDevs->ui32NumDevices && i < PVRSRV_MAX_DEVICES; i++)
		{
			if (psDevs->apfnDumpTrace[i] != IMG_NULL &&
			    psDevs->apfnDumpTrace[i](psDevData) != PVRSRV_OK)
			{
				SRV_ERR("HWOpTimeout: Failure to write debug trace info");
			}
		}
	}
	return PVRSRV_OK;
}
