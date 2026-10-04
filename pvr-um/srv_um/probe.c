/* SPDX-License-Identifier: MIT */
/*
 * Device probe for libsrv_um_cr.so.
 *
 * Exercises the services core against the running pvrsrvkm: connection,
 * device enumeration, SGX device acquisition, a device memory context, a
 * 64 KiB allocation in the SGX general heap with a CPU write/read-back,
 * misc info (DDK version, page size, global event object), an event wait,
 * display-class and buffer-class enumeration, and teardown. It never calls
 * PVRSRVInitSrvConnect and never touches SGX microkernel state.
 *
 * Output: one line per step,
 *   srv_um_probe step=<name> err=<PVRSRV_ERROR> (<name>) [key=value ...]
 * and a final "srv_um_probe result=PASS|FAIL". Exit status 0 on PASS.
 */
#include "srv_um_config.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "img_defs.h"
#include "img_types.h"
#include "services.h"
#include "sgxapi_km.h"
#include "pvrversion.h"

#define PROBE_ALLOC_BYTES	(64u * 1024u)
#define PROBE_DDK_STR_LEN	64u

static int giFailures;

static void ProbeStep(const char *pszStep, PVRSRV_ERROR eError, IMG_BOOL bPass,
		      const char *pszFormat, ...) __attribute__((format(printf, 4, 5)));

static void ProbeStep(const char *pszStep, PVRSRV_ERROR eError, IMG_BOOL bPass,
		      const char *pszFormat, ...)
{
	va_list vaArgs;

	printf("srv_um_probe step=%s err=%d (%s) pass=%d", pszStep, (int)eError,
	       PVRSRVGetErrorString(eError), bPass ? 1 : 0);
	if (pszFormat != NULL)
	{
		putchar(' ');
		va_start(vaArgs, pszFormat);
		vprintf(pszFormat, vaArgs);
		va_end(vaArgs);
	}
	putchar('\n');
	(void)fflush(stdout);
	if (!bPass)
	{
		giFailures++;
	}
}

/* Opens the node directly so a permission or SELinux denial is visible
 * apart from bridge failures; PVRSRVConnect folds both into INIT_FAILURE
 * after its retries. */
static void ProbeNode(void)
{
	int iFd = open("/dev/pvrsrvkm", O_RDWR | O_CLOEXEC);
	int iErr = (iFd < 0) ? errno : 0;

	printf("srv_um_probe step=open_node errno=%d (%s) pass=%d\n", iErr,
	       (iErr != 0) ? strerror(iErr) : "ok", (iErr == 0) ? 1 : 0);
	(void)fflush(stdout);
	if (iFd >= 0)
	{
		(void)close(iFd);
	}
	else
	{
		giFailures++;
	}
}

static void ProbeMemory(const PVRSRV_DEV_DATA *psDevData, const PVRSRV_HEAP_INFO *psHeaps,
			IMG_UINT32 ui32HeapCount)
{
	PVRSRV_CLIENT_MEM_INFO *psMemInfo = NULL;
	IMG_HANDLE hHeap = NULL;
	volatile IMG_UINT32 *pui32Words;
	IMG_UINT32 ui32Mismatch = 0;
	PVRSRV_ERROR eError;
	IMG_UINT32 i;

	for (i = 0; i < ui32HeapCount; i++)
	{
		if (HEAP_IDX(psHeaps[i].ui32HeapID) == SGX_GENERAL_HEAP_ID)
		{
			hHeap = psHeaps[i].hDevMemHeap;
			break;
		}
	}
	if (hHeap == NULL)
	{
		ProbeStep("alloc_mem", PVRSRV_ERROR_INVALID_PARAMS, IMG_FALSE,
			  "reason=no_general_heap");
		return;
	}

	/* No PVRSRV_MEM_NO_SYNCOBJ: the free path then waits on the sync
	 * object through SYNC_OPS_TAKE_TOKEN and SYNC_OPS_FLUSH_TO_TOKEN. */
	eError = PVRSRVAllocDeviceMem(psDevData, hHeap, PVRSRV_MEM_READ | PVRSRV_MEM_WRITE,
				      PROBE_ALLOC_BYTES, 4096, &psMemInfo);
	ProbeStep("alloc_mem", eError, eError == PVRSRV_OK && psMemInfo != NULL &&
		  psMemInfo->pvLinAddr != NULL && psMemInfo->psClientSyncInfo != NULL,
		  "bytes=%u dev_vaddr=0x%08x alloc_size=%u lin=%p sync=%p",
		  PROBE_ALLOC_BYTES,
		  (psMemInfo != NULL) ? psMemInfo->sDevVAddr.uiAddr : 0u,
		  (psMemInfo != NULL) ? (unsigned)psMemInfo->uAllocSize : 0u,
		  (psMemInfo != NULL) ? psMemInfo->pvLinAddr : NULL,
		  (psMemInfo != NULL) ? (void *)psMemInfo->psClientSyncInfo : NULL);
	if (eError != PVRSRV_OK || psMemInfo == NULL)
	{
		return;
	}

	if (psMemInfo->pvLinAddr != NULL)
	{
		pui32Words = psMemInfo->pvLinAddr;
		for (i = 0; i < PROBE_ALLOC_BYTES / 4u; i++)
		{
			pui32Words[i] = i * 0x9e3779b9u;
		}
		for (i = 0; i < PROBE_ALLOC_BYTES / 4u; i++)
		{
			if (pui32Words[i] != i * 0x9e3779b9u)
			{
				ui32Mismatch++;
			}
		}
		ProbeStep("cpu_rw", PVRSRV_OK, ui32Mismatch == 0, "words=%u mismatches=%u",
			  PROBE_ALLOC_BYTES / 4u, ui32Mismatch);
	}

	eError = PVRSRVFreeDeviceMem(psDevData, psMemInfo);
	ProbeStep("free_mem", eError, eError == PVRSRV_OK, NULL);
}

static void ProbeMiscInfo(const PVRSRV_CONNECTION *psConnection)
{
	PVRSRV_MISC_INFO sMisc;
	IMG_CHAR acDDK[PROBE_DDK_STR_LEN];
	PVRSRV_ERROR eError;
	IMG_BOOL bDDKMatch;

	/* Requests that only read kernel state: no timer or clock-gate
	 * register mapping, no cache operation, no reset, no swap to system. */
	memset(&sMisc, 0, sizeof(sMisc));
	memset(acDDK, 0, sizeof(acDDK));
	sMisc.ui32StateRequest = PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT |
				 PVRSRV_MISC_INFO_DDKVERSION_PRESENT |
				 PVRSRV_MISC_INFO_GET_PAGE_SIZE_PRESENT;
	sMisc.pszMemoryStr = acDDK;
	sMisc.ui32MemoryStrLen = sizeof(acDDK);

	eError = PVRSRVGetMiscInfo(psConnection, &sMisc);
	acDDK[sizeof(acDDK) - 1] = '\0';
	bDDKMatch = strcmp(acDDK, PVRVERSION_STRING_NUMERIC) == 0;
	ProbeStep("get_misc_info", eError,
		  eError == PVRSRV_OK && bDDKMatch && sMisc.ui32PageSize == 4096u &&
		  (sMisc.ui32StatePresent & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT) != 0,
		  "present=0x%x ddk=%s expect_ddk=%s page_size=%u global_event=%p",
		  sMisc.ui32StatePresent, acDDK, PVRVERSION_STRING_NUMERIC,
		  sMisc.ui32PageSize, sMisc.hOSGlobalEvent);
	if (eError != PVRSRV_OK)
	{
		return;
	}

	if ((sMisc.ui32StatePresent & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT) != 0)
	{
		/* OSEventObjectWaitKM returns OK on a signal or TIMEOUT after
		 * 100 ms; both prove the wait round trip. */
		eError = PVRSRVEventObjectWait(psConnection, sMisc.hOSGlobalEvent);
		ProbeStep("event_wait", eError,
			  eError == PVRSRV_OK || eError == PVRSRV_ERROR_TIMEOUT, NULL);
	}

	eError = PVRSRVReleaseMiscInfo(psConnection, &sMisc);
	ProbeStep("release_misc_info", eError, eError == PVRSRV_OK, NULL);
}

static void ProbeDeviceClass(const PVRSRV_CONNECTION *psConnection, PVRSRV_DEVICE_CLASS eClass,
			     const char *pszStep)
{
	IMG_UINT32 aui32IDs[PVRSRV_MAX_DEVICES];
	IMG_UINT32 ui32Count = 0;
	PVRSRV_ERROR eError;
	char acIDs[PVRSRV_MAX_DEVICES * 12];
	size_t uiPos = 0;
	IMG_UINT32 i;

	memset(aui32IDs, 0, sizeof(aui32IDs));
	acIDs[0] = '\0';

	eError = PVRSRVEnumerateDeviceClass(psConnection, eClass, &ui32Count, NULL);
	if (eError == PVRSRV_OK && ui32Count != 0)
	{
		eError = PVRSRVEnumerateDeviceClass(psConnection, eClass, &ui32Count, aui32IDs);
	}
	for (i = 0; eError == PVRSRV_OK && i < ui32Count && i < PVRSRV_MAX_DEVICES; i++)
	{
		int iLen = snprintf(acIDs + uiPos, sizeof(acIDs) - uiPos, "%s%u",
				    (i != 0) ? "," : "", aui32IDs[i]);
		if (iLen < 0 || (size_t)iLen >= sizeof(acIDs) - uiPos)
		{
			break;
		}
		uiPos += (size_t)iLen;
	}
	ProbeStep(pszStep, eError, eError == PVRSRV_OK, "count=%u ids=%s", ui32Count,
		  (ui32Count != 0) ? acIDs : "-");
}

int main(void)
{
	PVRSRV_CONNECTION *psConnection = NULL;
	PVRSRV_DEVICE_IDENTIFIER asDevID[PVRSRV_MAX_DEVICES];
	PVRSRV_HEAP_INFO asHeaps[PVRSRV_MAX_CLIENT_HEAPS];
	PVRSRV_DEV_DATA sDevData;
	IMG_HANDLE hDevMemContext = NULL;
	IMG_UINT32 ui32NumDevices = 0;
	IMG_UINT32 ui32HeapCount = 0;
	IMG_UINT32 ui32SGXIndex = 0;
	IMG_BOOL bHaveSGX = IMG_FALSE;
	PVRSRV_ERROR eError;
	IMG_UINT32 i;

	memset(asDevID, 0, sizeof(asDevID));
	memset(asHeaps, 0, sizeof(asHeaps));
	memset(&sDevData, 0, sizeof(sDevData));

	ProbeNode();

	eError = PVRSRVConnect(&psConnection, 0);
	ProbeStep("connect", eError, eError == PVRSRV_OK && psConnection != NULL,
		  "pid=%u", (psConnection != NULL) ? psConnection->ui32ProcessID : 0u);
	if (psConnection == NULL)
	{
		goto out;
	}
	if (eError != PVRSRV_OK)
	{
		goto disconnect;
	}

	eError = PVRSRVEnumerateDevices(psConnection, &ui32NumDevices, asDevID);
	ProbeStep("enumerate_devices", eError, eError == PVRSRV_OK, "count=%u", ui32NumDevices);
	for (i = 0; eError == PVRSRV_OK && i < ui32NumDevices; i++)
	{
		printf("srv_um_probe device=%u type=%d class=%d index=%u\n", i,
		       (int)asDevID[i].eDeviceType, (int)asDevID[i].eDeviceClass,
		       asDevID[i].ui32DeviceIndex);
		if (!bHaveSGX && asDevID[i].eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		{
			bHaveSGX = IMG_TRUE;
			ui32SGXIndex = asDevID[i].ui32DeviceIndex;
		}
	}
	if (!bHaveSGX)
	{
		ProbeStep("acquire_sgx", PVRSRV_ERROR_INVALID_PARAMS, IMG_FALSE, "reason=no_sgx_device");
		goto disconnect;
	}

	eError = PVRSRVAcquireDeviceData(psConnection, ui32SGXIndex, &sDevData,
					 PVRSRV_DEVICE_TYPE_SGX);
	ProbeStep("acquire_sgx", eError, eError == PVRSRV_OK, "index=%u cookie=%p",
		  ui32SGXIndex, sDevData.hDevCookie);
	if (eError != PVRSRV_OK)
	{
		goto disconnect;
	}

	eError = PVRSRVCreateDeviceMemContext(&sDevData, &hDevMemContext, &ui32HeapCount, asHeaps);
	ProbeStep("create_mem_context", eError, eError == PVRSRV_OK && ui32HeapCount != 0,
		  "context=%p heaps=%u", hDevMemContext, ui32HeapCount);
	for (i = 0; eError == PVRSRV_OK && i < ui32HeapCount; i++)
	{
		printf("srv_um_probe heap=%u id=0x%08x dev=%u idx=%u base=0x%08x size=0x%08x attr=0x%08x\n",
		       i, asHeaps[i].ui32HeapID, (unsigned)HEAP_DEV(asHeaps[i].ui32HeapID),
		       (unsigned)HEAP_IDX(asHeaps[i].ui32HeapID), asHeaps[i].sDevVAddrBase.uiAddr,
		       asHeaps[i].ui32HeapByteSize, asHeaps[i].ui32Attribs);
	}
	if (eError != PVRSRV_OK)
	{
		goto disconnect;
	}

	ProbeMemory(&sDevData, asHeaps, ui32HeapCount);
	ProbeMiscInfo(psConnection);
	ProbeDeviceClass(psConnection, PVRSRV_DEVICE_CLASS_DISPLAY, "enum_display_class");
	ProbeDeviceClass(psConnection, PVRSRV_DEVICE_CLASS_BUFFER, "enum_buffer_class");

	eError = PVRSRVDestroyDeviceMemContext(&sDevData, hDevMemContext);
	ProbeStep("destroy_mem_context", eError, eError == PVRSRV_OK, NULL);

disconnect:
	eError = PVRSRVDisconnect(psConnection);
	ProbeStep("disconnect", eError, eError == PVRSRV_OK, NULL);
out:
	printf("srv_um_probe result=%s failures=%d\n", (giFailures == 0) ? "PASS" : "FAIL",
	       giFailures);
	return (giFailures == 0) ? 0 : 1;
}
