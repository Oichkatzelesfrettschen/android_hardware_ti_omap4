/* SPDX-License-Identifier: MIT */
/*
 * Device memory: contexts, allocation, export and import, wrapping,
 * device-class buffers and ION buffers.
 *
 * Every kernel call that creates a meminfo returns a PVRSRV_CLIENT_MEM_INFO
 * and a PVRSRV_CLIENT_SYNC_INFO by value. The library copies them into heap
 * blocks the caller owns, replaces the kernel-side address fields with CPU
 * mappings made through SrvMapKernelMem (keyed by hKernelMemInfo for the
 * buffer and hKernelSyncInfo for the sync data), and links the sync block
 * from psClientSyncInfo.
 */
#include "srv_um_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SRV_PAGE_SHIFT	12
#define SRV_PAGE_SIZE	(1u << SRV_PAGE_SHIFT)

/* CPU view of a new meminfo's buffer. */
typedef enum
{
	SRV_BUF_MAP,		/* map the buffer through SrvMapKernelMem */
	SRV_BUF_NO_CPU_MAP,	/* PVRSRV_MAP_NOUSERVIRTUAL: pvLinAddr NULL */
	SRV_BUF_AS_REPORTED	/* keep the fields the kernel reported */
} SRV_BUF_MODE;

static PVRSRV_ERROR SrvSyncInfoCreate(const SRV_SERVICES *psServices,
				      const PVRSRV_CLIENT_SYNC_INFO *psSrc,
				      PVRSRV_CLIENT_SYNC_INFO **ppsSyncInfo)
{
	PVRSRV_CLIENT_SYNC_INFO *psSyncInfo = malloc(sizeof(*psSyncInfo));
	IMG_VOID *pvSyncData;

	if (psSyncInfo == IMG_NULL)
	{
		SRV_ERR("SrvSyncInfoCreate: out of memory");
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	*psSyncInfo = *psSrc;

	if (SrvMapKernelMem(psServices, &pvSyncData, &psSyncInfo->hMappingInfo,
			    psSyncInfo->hKernelSyncInfo) != PVRSRV_OK)
	{
		SRV_ERR("SrvSyncInfoCreate: cannot map the sync data");
		free(psSyncInfo);
		return PVRSRV_ERROR_BAD_MAPPING;
	}
	psSyncInfo->psSyncData = pvSyncData;
	*ppsSyncInfo = psSyncInfo;
	return PVRSRV_OK;
}

static void SrvSyncInfoRelease(const SRV_SERVICES *psServices, PVRSRV_CLIENT_SYNC_INFO *psSyncInfo)
{
	(void)SrvUnmapKernelMem(psServices, psSyncInfo->hMappingInfo, psSyncInfo->hKernelSyncInfo);
	free(psSyncInfo);
}

/* Builds the caller's meminfo from the kernel's copy. psSrcSync NULL means
 * the allocation has no sync object. On failure nothing stays mapped or
 * allocated; the caller undoes the kernel side. */
static PVRSRV_ERROR SrvMemInfoCreate(const SRV_SERVICES *psServices,
				     const PVRSRV_CLIENT_MEM_INFO *psSrcMem,
				     const PVRSRV_CLIENT_SYNC_INFO *psSrcSync,
				     SRV_BUF_MODE eMode,
				     PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	PVRSRV_CLIENT_MEM_INFO *psMemInfo = malloc(sizeof(*psMemInfo));
	PVRSRV_CLIENT_SYNC_INFO *psSyncInfo = IMG_NULL;
	PVRSRV_ERROR eError;

	if (psMemInfo == IMG_NULL)
	{
		SRV_ERR("SrvMemInfoCreate: out of memory");
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	*psMemInfo = *psSrcMem;
	psMemInfo->psClientSyncInfo = IMG_NULL;

	switch (eMode)
	{
		case SRV_BUF_MAP:
			if (SrvMapKernelMem(psServices, &psMemInfo->pvLinAddr,
					    &psMemInfo->hMappingInfo,
					    psMemInfo->hKernelMemInfo) != PVRSRV_OK ||
			    psMemInfo->pvLinAddr == IMG_NULL)
			{
				SRV_ERR("SrvMemInfoCreate: cannot map the buffer");
				free(psMemInfo);
				return PVRSRV_ERROR_BAD_MAPPING;
			}
			break;
		case SRV_BUF_NO_CPU_MAP:
			psMemInfo->pvLinAddr = IMG_NULL;
			break;
		case SRV_BUF_AS_REPORTED:
		default:
			break;
	}

	if (psSrcSync != IMG_NULL)
	{
		eError = SrvSyncInfoCreate(psServices, psSrcSync, &psSyncInfo);
		if (eError != PVRSRV_OK)
		{
			if (eMode == SRV_BUF_MAP)
			{
				(void)SrvUnmapKernelMem(psServices, psMemInfo->hMappingInfo,
							psMemInfo->hKernelMemInfo);
			}
			free(psMemInfo);
			return eError;
		}
		psMemInfo->psClientSyncInfo = psSyncInfo;
	}

	*ppsMemInfo = psMemInfo;
	return PVRSRV_OK;
}

/* The sync info the kernel attached, or NULL when it attached none. */
static const PVRSRV_CLIENT_SYNC_INFO *SrvReportedSync(const PVRSRV_CLIENT_SYNC_INFO *psSync)
{
	return (psSync->hKernelSyncInfo != IMG_NULL) ? psSync : IMG_NULL;
}

/* Releases the sync block and both CPU mappings, then the meminfo. */
static void SrvMemInfoRelease(const SRV_SERVICES *psServices, PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	if (psMemInfo->psClientSyncInfo != IMG_NULL)
	{
		SrvSyncInfoRelease(psServices, psMemInfo->psClientSyncInfo);
		psMemInfo->psClientSyncInfo = IMG_NULL;
	}
	if (psMemInfo->pvLinAddr != IMG_NULL)
	{
		(void)SrvUnmapKernelMem(psServices, psMemInfo->hMappingInfo, psMemInfo->hKernelMemInfo);
		psMemInfo->pvLinAddr = IMG_NULL;
	}
	free(psMemInfo);
}

/* Waits for the operations on the meminfo's sync object and releases the
 * sync block. Leaves the meminfo untouched when the wait fails. */
static PVRSRV_ERROR SrvRetireSync(const PVRSRV_DEV_DATA *psDevData, PVRSRV_CLIENT_MEM_INFO *psMemInfo,
				  const char *pszFunc)
{
	PVRSRV_ERROR eError;

	if (psMemInfo->psClientSyncInfo == IMG_NULL)
	{
		return PVRSRV_OK;
	}
	eError = SrvFlushClientOps(psDevData->psConnection, psMemInfo->psClientSyncInfo);
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("%s: pending operations did not complete (%d)", pszFunc, eError);
		return eError;
	}
	SrvSyncInfoRelease(SrvDevServices(psDevData), psMemInfo->psClientSyncInfo);
	psMemInfo->psClientSyncInfo = IMG_NULL;
	return PVRSRV_OK;
}

static IMG_BOOL SrvDevDataValid(const PVRSRV_DEV_DATA *psDevData)
{
	return psDevData != IMG_NULL && psDevData->psConnection != IMG_NULL &&
	       psDevData->psConnection->hServices != IMG_NULL;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCreateDeviceMemContext(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						     IMG_HANDLE *phDevMemContext,
						     IMG_UINT32 *pui32SharedHeapCount,
						     PVRSRV_HEAP_INFO *psHeapInfo)
{
	PVRSRV_BRIDGE_IN_CREATE_DEVMEMCONTEXT sIn;
	PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT sOut;
	IMG_UINT32 ui32Count;

	if (!SrvDevDataValid(psDevData) || phDevMemContext == IMG_NULL ||
	    pui32SharedHeapCount == IMG_NULL || psHeapInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVCreateDeviceMemContext: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVCreateDeviceMemContext: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVCreateDeviceMemContext: CREATE_DEVMEMCONTEXT failed (%d)", sOut.eError);
		return sOut.eError;
	}

	ui32Count = sOut.ui32ClientHeapCount;
	if (ui32Count > PVRSRV_MAX_CLIENT_HEAPS)
	{
		ui32Count = PVRSRV_MAX_CLIENT_HEAPS;
	}
	*phDevMemContext = sOut.hDevMemContext;
	*pui32SharedHeapCount = ui32Count;
	memcpy(psHeapInfo, sOut.sHeapInfo, ui32Count * sizeof(PVRSRV_HEAP_INFO));
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDestroyDeviceMemContext(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						      IMG_HANDLE hDevMemContext)
{
	PVRSRV_BRIDGE_IN_DESTROY_DEVMEMCONTEXT sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (!SrvDevDataValid(psDevData) || hDevMemContext == IMG_NULL)
	{
		SRV_ERR("PVRSRVDestroyDeviceMemContext: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.hDevMemContext = hDevMemContext;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_DESTROY_DEVMEMCONTEXT,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVDestroyDeviceMemContext: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVDestroyDeviceMemContext: allocations still exist in the memory "
			"context to be destroyed");
		SRV_ERR("PVRSRVDestroyDeviceMemContext: Likely Cause: client drivers not freeing "
			"allocations before destroying devmemcontext");
	}
	return sOut.eError;
}

IMG_INTERNAL PVRSRV_ERROR SrvGetDeviceMemHeapInfo(const PVRSRV_DEV_DATA *psDevData,
						  IMG_HANDLE hDevMemContext,
						  IMG_UINT32 *pui32HeapCount,
						  PVRSRV_HEAP_INFO *psHeapInfo)
{
	PVRSRV_BRIDGE_IN_GET_DEVMEM_HEAPINFO sIn;
	PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO sOut;
	IMG_UINT32 ui32Count;

	if (!SrvDevDataValid(psDevData) || hDevMemContext == IMG_NULL ||
	    pui32HeapCount == IMG_NULL || psHeapInfo == IMG_NULL)
	{
		SRV_ERR("SrvGetDeviceMemHeapInfo: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.hDevMemContext = hDevMemContext;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("SrvGetDeviceMemHeapInfo: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("SrvGetDeviceMemHeapInfo: GET_DEVMEM_HEAPINFO failed (%d)", sOut.eError);
		return sOut.eError;
	}

	ui32Count = sOut.ui32ClientHeapCount;
	if (ui32Count > PVRSRV_MAX_CLIENT_HEAPS)
	{
		ui32Count = PVRSRV_MAX_CLIENT_HEAPS;
	}
	*pui32HeapCount = ui32Count;
	memcpy(psHeapInfo, sOut.sHeapInfo, ui32Count * sizeof(PVRSRV_HEAP_INFO));
	return PVRSRV_OK;
}

/* Releases a kernel allocation whose client meminfo could not be built. */
static void SrvFreeKernelMem(const PVRSRV_DEV_DATA *psDevData, IMG_HANDLE hKernelMemInfo)
{
	PVRSRV_BRIDGE_IN_FREEDEVICEMEM sIn;
	PVRSRV_BRIDGE_OUT_FREEDEVICEMEM sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.psKernelMemInfo = hKernelMemInfo;
	(void)SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_FREE_DEVICEMEM,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

static PVRSRV_ERROR SrvAllocDeviceMem(const PVRSRV_DEV_DATA *psDevData,
				      IMG_HANDLE hDevMemHeap,
				      IMG_UINT32 ui32Attribs,
				      IMG_SIZE_T uSize,
				      IMG_SIZE_T uAlignment,
				      IMG_PVOID pvPrivData,
				      IMG_UINT32 ui32PrivDataLength,
				      IMG_UINT32 ui32ChunkSize,
				      IMG_UINT32 ui32NumVirtChunks,
				      IMG_UINT32 ui32NumPhysChunks,
				      IMG_BOOL *pabMapChunk,
				      PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM sIn;
	PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || ppsMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVAllocDeviceMem: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* PVRSRV_MEM_XPROC allocations are shared page-granular between
	 * processes: the alignment is at least one page and the size a
	 * multiple of the alignment. */
	if ((ui32Attribs & PVRSRV_MEM_XPROC) != 0)
	{
		if (uAlignment < SRV_PAGE_SIZE)
		{
			uAlignment = SRV_PAGE_SIZE;
		}
		uSize = ((uSize - 1u) | (uAlignment - 1u)) + 1u;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.hDevMemHeap = hDevMemHeap;
	sIn.ui32Attribs = ui32Attribs & ~(IMG_UINT32)PVRSRV_MAP_NOUSERVIRTUAL;
	sIn.uSize = uSize;
	sIn.uAlignment = uAlignment;
	sIn.pvPrivData = pvPrivData;
	sIn.ui32PrivDataLength = ui32PrivDataLength;
	sIn.ui32ChunkSize = ui32ChunkSize;
	sIn.ui32NumVirtChunks = ui32NumVirtChunks;
	sIn.ui32NumPhysChunks = ui32NumPhysChunks;
	sIn.pabMapChunk = pabMapChunk;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_ALLOC_DEVICEMEM,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVAllocDeviceMem: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVAllocDeviceMem: ALLOC_DEVICEMEM failed (%d)", sOut.eError);
		return sOut.eError;
	}

	/* PVRSRVAllocDeviceMemBW attaches a sync info unless the caller asked
	 * for PVRSRV_MEM_NO_SYNCOBJ. */
	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sClientMemInfo,
				  ((ui32Attribs & PVRSRV_MEM_NO_SYNCOBJ) != 0) ? IMG_NULL : &sOut.sClientSyncInfo,
				  ((ui32Attribs & PVRSRV_MAP_NOUSERVIRTUAL) != 0) ? SRV_BUF_NO_CPU_MAP : SRV_BUF_MAP,
				  ppsMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvFreeKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
		*ppsMemInfo = IMG_NULL;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVAllocDeviceMem(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					     IMG_HANDLE hDevMemHeap,
					     IMG_UINT32 ui32Attribs,
					     IMG_SIZE_T ui32Size,
					     IMG_SIZE_T ui32Alignment,
					     PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	return SrvAllocDeviceMem(psDevData, hDevMemHeap, ui32Attribs, ui32Size, ui32Alignment,
				 IMG_NULL, 0, 0, 0, 0, IMG_NULL, ppsMemInfo);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVAllocDeviceMem2(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					      IMG_HANDLE hDevMemHeap,
					      IMG_UINT32 ui32Attribs,
					      IMG_SIZE_T ui32Size,
					      IMG_SIZE_T ui32Alignment,
					      IMG_PVOID pvPrivData,
					      IMG_UINT32 ui32PrivDataLength,
					      PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	return SrvAllocDeviceMem(psDevData, hDevMemHeap, ui32Attribs, ui32Size, ui32Alignment,
				 pvPrivData, ui32PrivDataLength, 0, 0, 0, IMG_NULL, ppsMemInfo);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVAllocDeviceMemSparse(const PVRSRV_DEV_DATA *psDevData,
						   IMG_HANDLE hDevMemHeap,
						   IMG_UINT32 ui32Attribs,
						   IMG_SIZE_T uAlignment,
						   IMG_UINT32 ui32ChunkSize,
						   IMG_UINT32 ui32NumVirtChunks,
						   IMG_UINT32 ui32NumPhysChunks,
						   IMG_BOOL *pabMapChunk,
						   PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	return SrvAllocDeviceMem(psDevData, hDevMemHeap, ui32Attribs | PVRSRV_MEM_SPARSE, 0,
				 uAlignment, IMG_NULL, 0, ui32ChunkSize, ui32NumVirtChunks,
				 ui32NumPhysChunks, pabMapChunk, ppsMemInfo);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVFreeDeviceMem(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					    PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	PVRSRV_BRIDGE_IN_FREEDEVICEMEM sIn;
	PVRSRV_BRIDGE_OUT_FREEDEVICEMEM sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVFreeDeviceMem: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = SrvRetireSync(psDevData, psMemInfo, "PVRSRVFreeDeviceMem");
	if (eError != PVRSRV_OK)
	{
		return eError;
	}
	if (psMemInfo->pvLinAddr != IMG_NULL)
	{
		(void)SrvUnmapKernelMem(SrvDevServices(psDevData), psMemInfo->hMappingInfo,
					psMemInfo->hKernelMemInfo);
		psMemInfo->pvLinAddr = IMG_NULL;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_FREE_DEVICEMEM,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVFreeDeviceMem: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError == PVRSRV_OK)
	{
		free(psMemInfo);
	}
	return sOut.eError;
}

IMG_INTERNAL PVRSRV_ERROR SrvUnrefDeviceMem(const PVRSRV_DEV_DATA *psDevData,
					    PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("SrvUnrefDeviceMem: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	SrvMemInfoRelease(SrvDevServices(psDevData), psMemInfo);
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVExportDeviceMem(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					      PVRSRV_CLIENT_MEM_INFO *psMemInfo,
					      IMG_HANDLE *phMemInfo)
{
	PVRSRV_BRIDGE_IN_EXPORTDEVICEMEM sIn;
	PVRSRV_BRIDGE_OUT_EXPORTDEVICEMEM sOut;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL || phMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVExportDeviceMem: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_EXPORT_DEVICEMEM,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVExportDeviceMem: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError == PVRSRV_OK)
	{
		*phMemInfo = sOut.hMemInfo;
	}
	return sOut.eError;
}

/*
 * Exports a meminfo as a new /dev/pvrsrvkm descriptor. PVRSRV_BridgeDispatchKM
 * binds the exported meminfo to the file the EXPORT_DEVICEMEM_2 ioctl
 * arrives on, holds a reference until that file closes, and accepts only
 * MAP_DEV_MEMORY_2 on it afterwards. On success the caller owns the
 * descriptor; on failure it is closed here.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVExportDeviceMem2(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					       PVRSRV_CLIENT_MEM_INFO *psMemInfo,
					       IMG_INT *iFd)
{
	PVRSRV_BRIDGE_IN_EXPORTDEVICEMEM sIn;
	PVRSRV_BRIDGE_OUT_EXPORTDEVICEMEM sOut;
	SRV_SERVICES sExport;
	int iNewFd;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL || iFd == IMG_NULL)
	{
		SRV_ERR("PVRSRVExportDeviceMem2: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	iNewFd = open("/dev/pvrsrvkm", O_RDWR | O_CLOEXEC);
	if (iNewFd < 0)
	{
		SRV_ERR("PVRSRVExportDeviceMem2: cannot open /dev/pvrsrvkm: %s", strerror(errno));
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	sExport.iFd = iNewFd;
	sExport.hKernelServices = SrvDevServices(psDevData)->hKernelServices;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;

	if (SrvBridgeCall(&sExport, PVRSRV_BRIDGE_EXPORT_DEVICEMEM_2,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVExportDeviceMem2: bridge call failed");
		(void)close(iNewFd);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		(void)close(iNewFd);
		return sOut.eError;
	}

	*iFd = iNewFd;
	psMemInfo->ui64Stamp = sOut.ui64Stamp;
	return PVRSRV_OK;
}

static void SrvUnmapDevMem(const SRV_SERVICES *psServices, IMG_HANDLE hKernelMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_DEV_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = hKernelMemInfo;
	(void)SrvBridgeCall(psServices, PVRSRV_BRIDGE_UNMAP_DEV_MEMORY,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

/* Imports a meminfo exported by PVRSRVExportDeviceMem2. The kernel writes
 * the exported hKernelMemInfo of iFd into the input before dispatch, so the
 * call names only the destination heap. iFd stays open. */
IMG_EXPORT PVRSRV_ERROR PVRSRVMapDeviceMemory2(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					       IMG_INT iFd,
					       IMG_HANDLE hDstDevMemHeap,
					       PVRSRV_CLIENT_MEM_INFO **ppsDstMemInfo)
{
	PVRSRV_BRIDGE_IN_MAP_DEV_MEMORY sIn;
	PVRSRV_BRIDGE_OUT_MAP_DEV_MEMORY sOut;
	SRV_SERVICES sImport;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || iFd < 0 || hDstDevMemHeap == IMG_NULL ||
	    ppsDstMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVMapDeviceMemory2: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	sImport.iFd = iFd;
	sImport.hKernelServices = SrvDevServices(psDevData)->hKernelServices;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDstDevMemHeap = hDstDevMemHeap;

	if (SrvBridgeCall(&sImport, PVRSRV_BRIDGE_MAP_DEV_MEMORY_2,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapDeviceMemory2: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		return sOut.eError;
	}

	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sDstClientMemInfo,
				  SrvReportedSync(&sOut.sDstClientSyncInfo), SRV_BUF_MAP,
				  ppsDstMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvUnmapDevMem(SrvDevServices(psDevData), sOut.sDstClientMemInfo.hKernelMemInfo);
		*ppsDstMemInfo = IMG_NULL;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVMapDeviceMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					      IMG_HANDLE hKernelMemInfo,
					      IMG_HANDLE hDstDevMemHeap,
					      PVRSRV_CLIENT_MEM_INFO **ppsDstMemInfo)
{
	PVRSRV_BRIDGE_IN_MAP_DEV_MEMORY sIn;
	PVRSRV_BRIDGE_OUT_MAP_DEV_MEMORY sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || hKernelMemInfo == IMG_NULL ||
	    hDstDevMemHeap == IMG_NULL || ppsDstMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVMapDeviceMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelMemInfo = hKernelMemInfo;
	sIn.hDstDevMemHeap = hDstDevMemHeap;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_MAP_DEV_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapDeviceMemory: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		return sOut.eError;
	}

	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sDstClientMemInfo,
				  SrvReportedSync(&sOut.sDstClientSyncInfo), SRV_BUF_MAP,
				  ppsDstMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvUnmapDevMem(SrvDevServices(psDevData), sOut.sDstClientMemInfo.hKernelMemInfo);
		*ppsDstMemInfo = IMG_NULL;
	}
	return eError;
}

/* Releases a mapping made by PVRSRVMapDeviceMemory or
 * PVRSRVMapDeviceMemory2. The meminfo is freed before the kernel call, so
 * it is gone whatever the kernel answers once the sync wait succeeded. */
IMG_EXPORT PVRSRV_ERROR PVRSRVUnmapDeviceMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_DEV_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVUnmapDeviceMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = SrvRetireSync(psDevData, psMemInfo, "PVRSRVUnmapDeviceMemory");
	if (eError != PVRSRV_OK)
	{
		return eError;
	}
	(void)SrvUnmapKernelMem(SrvDevServices(psDevData), psMemInfo->hMappingInfo,
				psMemInfo->hKernelMemInfo);

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;
	sIn.sClientMemInfo = *psMemInfo;
	free(psMemInfo);

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAP_DEV_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVUnmapDeviceMemory: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return sOut.eError;
}

/* PVRSRV_BRIDGE_MAPPHYSTOUSERSPACE and UNMAPPHYSTOUSERSPACE dispatch to
 * DummyBW in this kernel, which fails the ioctl; both calls therefore
 * report BRIDGE_CALL_FAILED. */
IMG_EXPORT PVRSRV_ERROR PVRSRVMapPhysToUserSpace(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						 IMG_SYS_PHYADDR sSysPhysAddr,
						 IMG_UINT32 uiSizeInBytes,
						 IMG_PVOID *ppvUserAddr,
						 IMG_UINT32 *puiActualSize,
						 IMG_PVOID *ppvProcess)
{
	PVRSRV_BRIDGE_IN_MAPPHYSTOUSERSPACE sIn;
	PVRSRV_BRIDGE_OUT_MAPPHYSTOUSERSPACE sOut;

	if (!SrvDevDataValid(psDevData) || ppvUserAddr == IMG_NULL ||
	    puiActualSize == IMG_NULL || ppvProcess == IMG_NULL)
	{
		SRV_ERR("PVRSRVMapPhysToUserSpace: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.sSysPhysAddr = sSysPhysAddr;
	sIn.uiSizeInBytes = uiSizeInBytes;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_MAPPHYSTOUSERSPACE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapPhysToUserSpace: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	*ppvUserAddr = sOut.pvUserAddr;
	*puiActualSize = sOut.uiActualSize;
	*ppvProcess = sOut.pvProcess;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVUnmapPhysToUserSpace(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						   IMG_PVOID pvUserAddr,
						   IMG_PVOID pvProcess)
{
	PVRSRV_BRIDGE_IN_UNMAPPHYSTOUSERSPACE sIn;

	if (!SrvDevDataValid(psDevData))
	{
		SRV_ERR("PVRSRVUnmapPhysToUserSpace: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.pvUserAddr = pvUserAddr;
	sIn.pvProcess = pvProcess;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAPPHYSTOUSERSPACE,
			  &sIn, sizeof(sIn), IMG_NULL, 0) != 0)
	{
		SRV_ERR("PVRSRVUnmapPhysToUserSpace: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return PVRSRV_OK;
}

static void SrvUnwrapKernelMem(const PVRSRV_DEV_DATA *psDevData, IMG_HANDLE hKernelMemInfo)
{
	PVRSRV_BRIDGE_IN_UNWRAP_EXT_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelMemInfo = hKernelMemInfo;
	(void)SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNWRAP_EXT_MEMORY,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

/*
 * Makes caller memory device-accessible. With psSysPAddr the kernel wraps
 * the listed physical pages (one entry for contiguous memory, else one per
 * 4 KiB page spanned by offset + size); otherwise it wraps the pages behind
 * pvLinAddr. The library keeps the address fields the kernel reports and
 * maps only the sync data.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVWrapExtMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					    IMG_HANDLE hDevMemContext,
					    IMG_SIZE_T ui32ByteSize,
					    IMG_SIZE_T ui32PageOffset,
					    IMG_BOOL bPhysContig,
					    IMG_SYS_PHYADDR *psSysPAddr,
					    IMG_VOID *pvLinAddr,
					    IMG_UINT32 ui32Flags,
					    PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY sIn;
	PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY sOut;
	IMG_SYS_PHYADDR *psPAddrCopy = IMG_NULL;
	IMG_UINT32 ui32NumPages = 0;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || hDevMemContext == IMG_NULL || ppsMemInfo == IMG_NULL ||
	    (psSysPAddr == IMG_NULL && pvLinAddr == IMG_NULL))
	{
		SRV_ERR("PVRSRVWrapExtMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (psSysPAddr != IMG_NULL)
	{
		IMG_UINT64 ui64Pages = bPhysContig ? 1u :
			((IMG_UINT64)ui32ByteSize + ui32PageOffset + SRV_PAGE_SIZE - 1u) >> SRV_PAGE_SHIFT;

		ui32NumPages = (IMG_UINT32)ui64Pages;
		psPAddrCopy = malloc((size_t)ui32NumPages * sizeof(IMG_SYS_PHYADDR));
		if (psPAddrCopy == IMG_NULL)
		{
			SRV_ERR("PVRSRVWrapExtMemory: out of memory");
			*ppsMemInfo = IMG_NULL;
			return PVRSRV_ERROR_OUT_OF_MEMORY;
		}
		memcpy(psPAddrCopy, psSysPAddr, (size_t)ui32NumPages * sizeof(IMG_SYS_PHYADDR));
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.hDevMemContext = hDevMemContext;
	sIn.pvLinAddr = pvLinAddr;
	sIn.uByteSize = ui32ByteSize;
	sIn.uPageOffset = ui32PageOffset;
	sIn.bPhysContig = bPhysContig;
	sIn.ui32NumPageTableEntries = ui32NumPages;
	sIn.psSysPAddr = psPAddrCopy;
	sIn.ui32Flags = ui32Flags;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_WRAP_EXT_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVWrapExtMemory: bridge call failed");
		free(psPAddrCopy);
		*ppsMemInfo = IMG_NULL;
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	/* PVRSRVWrapExtMemoryBW copied the page list into the kernel. */
	free(psPAddrCopy);
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVWrapExtMemory: WRAP_EXT_MEMORY failed (%d)", sOut.eError);
		*ppsMemInfo = IMG_NULL;
		return sOut.eError;
	}

	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sClientMemInfo,
				  &sOut.sClientSyncInfo, SRV_BUF_AS_REPORTED, ppsMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvUnwrapKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
		*ppsMemInfo = IMG_NULL;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVUnwrapExtMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					      PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	PVRSRV_BRIDGE_IN_UNWRAP_EXT_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_CLIENT_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVUnwrapExtMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hKernelMemInfo = psMemInfo->hKernelMemInfo;
	sIn.sClientMemInfo = *psMemInfo;

	psSyncInfo = psMemInfo->psClientSyncInfo;
	if (psSyncInfo != IMG_NULL)
	{
		eError = SrvFlushClientOps(psDevData->psConnection, psSyncInfo);
		if (eError != PVRSRV_OK)
		{
			SRV_ERR("PVRSRVUnwrapExtMemory: pending operations did not complete (%d)",
				eError);
			return eError;
		}
		sIn.sClientSyncInfo = *psSyncInfo;
		(void)SrvUnmapKernelMem(SrvDevServices(psDevData), psSyncInfo->hMappingInfo,
					psSyncInfo->hKernelSyncInfo);
		psSyncInfo->psSyncData = IMG_NULL;
		psSyncInfo->hMappingInfo = IMG_NULL;
	}

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNWRAP_EXT_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVUnwrapExtMemory: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError == PVRSRV_OK)
	{
		free(psSyncInfo);
		free(psMemInfo);
	}
	return sOut.eError;
}

static void SrvUnmapDeviceClassKernelMem(const PVRSRV_DEV_DATA *psDevData, IMG_HANDLE hKernelMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_DEVICECLASS_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = hKernelMemInfo;
	(void)SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAP_DEVICECLASS_MEMORY,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

/* Maps a display-class or buffer-class buffer (from PVRSRVGetDCBuffers or
 * PVRSRVGetBCBuffer) into a device memory context and into this process. */
IMG_EXPORT PVRSRV_ERROR PVRSRVMapDeviceClassMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						   IMG_HANDLE hDevMemContext,
						   IMG_HANDLE hDeviceClassBuffer,
						   PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	PVRSRV_BRIDGE_IN_MAP_DEVICECLASS_MEMORY sIn;
	PVRSRV_BRIDGE_OUT_MAP_DEVICECLASS_MEMORY sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || hDeviceClassBuffer == IMG_NULL || ppsMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVMapDeviceClassMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceClassBuffer = hDeviceClassBuffer;
	sIn.hDevMemContext = hDevMemContext;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_MAP_DEVICECLASS_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapDeviceClassMemory: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		return sOut.eError;
	}

	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sClientMemInfo,
				  SrvReportedSync(&sOut.sClientSyncInfo), SRV_BUF_MAP, ppsMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvUnmapDeviceClassKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
		*ppsMemInfo = IMG_NULL;
	}
	return eError;
}

/* The meminfo is freed before the kernel call, so it is gone whatever the
 * kernel answers once the sync wait succeeded. */
IMG_EXPORT PVRSRV_ERROR PVRSRVUnmapDeviceClassMemory(IMG_CONST PVRSRV_DEV_DATA *psDevData,
						     PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_DEVICECLASS_MEMORY sIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVUnmapDeviceClassMemory: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;
	sIn.sClientMemInfo = *psMemInfo;
	if (psMemInfo->psClientSyncInfo != IMG_NULL)
	{
		sIn.sClientSyncInfo = *psMemInfo->psClientSyncInfo;
	}

	eError = SrvRetireSync(psDevData, psMemInfo, "PVRSRVUnmapDeviceClassMemory");
	if (eError != PVRSRV_OK)
	{
		return eError;
	}
	(void)SrvUnmapKernelMem(SrvDevServices(psDevData), psMemInfo->hMappingInfo,
				psMemInfo->hKernelMemInfo);
	free(psMemInfo);

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAP_DEVICECLASS_MEMORY,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVUnmapDeviceClassMemory: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return sOut.eError;
}

static void SrvUnmapIonKernelMem(const PVRSRV_DEV_DATA *psDevData, IMG_HANDLE hKernelMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_ION_HANDLE sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = hKernelMemInfo;
	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAP_ION_HANDLE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapIonHandle: UNMAP_ION_HANDLE bridge call failed");
	}
	else if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVMapIonHandle: UNMAP_ION_HANDLE failed (%d)", sOut.eError);
	}
}

/*
 * Imports up to ION_IMPORT_MAX_FDS ION buffers as one meminfo. The kernel
 * maps the chunks into the device heap; the CPU mapping is an mmap of the
 * single ION descriptor, so a CPU view exists only for single-descriptor
 * imports. PVRSRV_MAP_NOUSERVIRTUAL skips the CPU view.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVMapIonHandle(const PVRSRV_DEV_DATA *psDevData,
					   IMG_HANDLE hDevMemHeap,
					   IMG_UINT32 ui32NumFDs,
					   IMG_INT *paiBufferFDs,
					   IMG_UINT32 ui32ChunkCount,
					   IMG_SIZE_T *pauiOffset,
					   IMG_SIZE_T *pauiSize,
					   IMG_UINT32 ui32Attribs,
					   PVRSRV_CLIENT_MEM_INFO **ppsMemInfo)
{
	PVRSRV_BRIDGE_IN_MAP_ION_HANDLE sIn;
	PVRSRV_BRIDGE_OUT_MAP_ION_HANDLE sOut;
	PVRSRV_CLIENT_MEM_INFO *psMemInfo;
	PVRSRV_ERROR eError;
	IMG_UINT32 i;

	if (!SrvDevDataValid(psDevData) || ppsMemInfo == IMG_NULL ||
	    (ui32NumFDs != 0 && paiBufferFDs == IMG_NULL) ||
	    (ui32ChunkCount != 0 && (pauiOffset == IMG_NULL || pauiSize == IMG_NULL)))
	{
		SRV_ERR("PVRSRVMapIonHandle: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	*ppsMemInfo = IMG_NULL;

	if (ui32ChunkCount > ION_IMPORT_MAX_CHUNK_COUNT)
	{
		SRV_ERR("PVRSRVMapIonHandle: too many chunks (%u)", ui32ChunkCount);
		return PVRSRV_ERROR_TOOMANYBUFFERS;
	}
	if (ui32NumFDs > ION_IMPORT_MAX_FDS)
	{
		SRV_ERR("PVRSRVMapIonHandle: too many descriptors (%u)", ui32NumFDs);
		return PVRSRV_ERROR_TOOMANYBUFFERS;
	}
	for (i = 0; i < ui32NumFDs; i++)
	{
		if (paiBufferFDs[i] < 0)
		{
			SRV_ERR("PVRSRVMapIonHandle: descriptor %u is invalid (%d)", i, paiBufferFDs[i]);
			return PVRSRV_ERROR_HANDLE_INDEX_OUT_OF_RANGE;
		}
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.ui32NumFDs = ui32NumFDs;
	for (i = 0; i < ION_IMPORT_MAX_FDS; i++)
	{
		sIn.ai32BufferFDs[i] = (i < ui32NumFDs) ? paiBufferFDs[i] : -1;
	}
	sIn.ui32Attribs = ui32Attribs;
	sIn.ui32ChunkCount = ui32ChunkCount;
	for (i = 0; i < ui32ChunkCount; i++)
	{
		sIn.auiOffset[i] = pauiOffset[i];
		sIn.auiSize[i] = pauiSize[i];
	}
	sIn.hDevCookie = psDevData->hDevCookie;
	sIn.hDevMemHeap = hDevMemHeap;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_MAP_ION_HANDLE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVMapIonHandle: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		return sOut.eError;
	}

	eError = SrvMemInfoCreate(SrvDevServices(psDevData), &sOut.sClientMemInfo, IMG_NULL,
				  SRV_BUF_NO_CPU_MAP, &psMemInfo);
	if (eError != PVRSRV_OK)
	{
		SrvUnmapIonKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
		return eError;
	}

	if ((ui32Attribs & PVRSRV_MAP_NOUSERVIRTUAL) == 0)
	{
		void *pvMap = MAP_FAILED;

		if (ui32NumFDs == 1)
		{
			pvMap = mmap(IMG_NULL, sOut.uiIonBufferSize, PROT_READ | PROT_WRITE,
				     MAP_SHARED, paiBufferFDs[0], 0);
			if (pvMap == MAP_FAILED)
			{
				SRV_ERR("PVRSRVMapIonHandle: mmap of the ION buffer failed: %s",
					strerror(errno));
			}
		}
		else
		{
			SRV_ERR("PVRSRVMapIonHandle: a CPU mapping needs exactly one descriptor (%u)",
				ui32NumFDs);
		}
		if (pvMap == MAP_FAILED)
		{
			free(psMemInfo);
			SrvUnmapIonKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
			return PVRSRV_ERROR_BAD_MAPPING;
		}
		psMemInfo->pvLinAddr = pvMap;
		psMemInfo->uiIonBufferSize = sOut.uiIonBufferSize;
	}

	if ((ui32Attribs & PVRSRV_MEM_NO_SYNCOBJ) == 0)
	{
		eError = SrvSyncInfoCreate(SrvDevServices(psDevData), &sOut.sClientSyncInfo,
					   &psMemInfo->psClientSyncInfo);
		if (eError != PVRSRV_OK)
		{
			if (psMemInfo->pvLinAddr != IMG_NULL)
			{
				(void)munmap(psMemInfo->pvLinAddr, psMemInfo->uiIonBufferSize);
			}
			free(psMemInfo);
			SrvUnmapIonKernelMem(psDevData, sOut.sClientMemInfo.hKernelMemInfo);
			return eError;
		}
	}

	*ppsMemInfo = psMemInfo;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVUnmapIonHandle(const PVRSRV_DEV_DATA *psDevData,
					     PVRSRV_CLIENT_MEM_INFO *psMemInfo)
{
	PVRSRV_BRIDGE_IN_UNMAP_ION_HANDLE sIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_ERROR eError;

	if (!SrvDevDataValid(psDevData) || psMemInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVUnmapIonHandle: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = SrvRetireSync(psDevData, psMemInfo, "PVRSRVUnmapIonHandle");
	if (eError != PVRSRV_OK)
	{
		return eError;
	}
	if (psMemInfo->pvLinAddr != IMG_NULL)
	{
		(void)munmap(psMemInfo->pvLinAddr, psMemInfo->uiIonBufferSize);
		psMemInfo->pvLinAddr = IMG_NULL;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.psKernelMemInfo = psMemInfo->hKernelMemInfo;

	if (SrvBridgeCall(SrvDevServices(psDevData), PVRSRV_BRIDGE_UNMAP_ION_HANDLE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVUnmapIonHandle: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVUnmapIonHandle: UNMAP_ION_HANDLE failed (%d)", sOut.eError);
		return sOut.eError;
	}
	free(psMemInfo);
	return PVRSRV_OK;
}
