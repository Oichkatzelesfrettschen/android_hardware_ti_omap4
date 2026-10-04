/* SPDX-License-Identifier: MIT */
/*
 * Internal interfaces of the services user-mode library: the bridge
 * transport, the kernel-memory mapping helpers and the helpers shared
 * between the per-area source files. Nothing here is exported.
 */
#ifndef SRV_UM_PRIV_H
#define SRV_UM_PRIV_H

#include "srv_um_config.h"

#include "img_defs.h"
#include "img_types.h"
#include "services.h"
#include "pvr_bridge.h"
#include "pvr_debug.h"

/*
 * Object behind PVRSRV_CONNECTION.hServices: the /dev/pvrsrvkm descriptor
 * and the per-process handle PVRSRV_BRIDGE_CONNECT_SERVICES returns
 * (PVRSRVConnectBW hands out psPerProc->hPerProcData). Every bridge package
 * carries hKernelServices; PVRSRV_BridgeDispatchKM resolves the per-process
 * data from it for every ID except CONNECT_SERVICES.
 */
typedef struct SRV_SERVICES_TAG
{
	IMG_INT		iFd;
	IMG_HANDLE	hKernelServices;
} SRV_SERVICES;

/* Init-server entry points; services.h carries no prototype for them. */
IMG_IMPORT PVRSRV_ERROR PVRSRVInitSrvConnect(PVRSRV_CONNECTION **ppsConnection);
IMG_IMPORT PVRSRV_ERROR PVRSRVInitSrvDisconnect(PVRSRV_CONNECTION *psConnection,
						IMG_BOOL bInitSuccesful);

/* Error-level log line. The PVR_DBG_ERROR level selects ANDROID_LOG_ERROR
 * in PVRSRVDebugPrintf; the file and line fields stay empty. */
#define SRV_ERR(...) PVRSRVDebugPrintf(PVR_DBG_ERROR, "", 0, __VA_ARGS__)

static inline const SRV_SERVICES *SrvServices(const PVRSRV_CONNECTION *psConnection)
{
	return (const SRV_SERVICES *)psConnection->hServices;
}

static inline const SRV_SERVICES *SrvDevServices(const PVRSRV_DEV_DATA *psDevData)
{
	return SrvServices(psDevData->psConnection);
}

/* Issues one PVRSRV_BRIDGE_PACKAGE through ioctl(). Returns the ioctl
 * result: 0 when the KM dispatcher ran the handler and copied ui32OutSize
 * bytes back, negative when the ioctl failed. */
IMG_INTERNAL IMG_INT SrvBridgeCall(const SRV_SERVICES *psServices,
				   IMG_UINT32 ui32BridgeID,
				   IMG_VOID *pvIn, IMG_UINT32 ui32InSize,
				   IMG_VOID *pvOut, IMG_UINT32 ui32OutSize);

/* Maps the kernel allocation behind hMHandle into this process through
 * PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA and mmap of the services descriptor.
 * *phMappingBase receives the mapping base, *ppvLinAddr the base plus the
 * kernel-reported byte offset. */
IMG_INTERNAL PVRSRV_ERROR SrvMapKernelMem(const SRV_SERVICES *psServices,
					  IMG_VOID **ppvLinAddr,
					  IMG_HANDLE *phMappingBase,
					  IMG_HANDLE hMHandle);

/* Drops one mapping reference through PVRSRV_BRIDGE_RELEASE_MMAP_DATA and
 * unmaps the region the kernel names once the last reference goes. */
IMG_INTERNAL IMG_BOOL SrvUnmapKernelMem(const SRV_SERVICES *psServices,
					IMG_HANDLE hMappingBase,
					IMG_HANDLE hMHandle);

/* Waits until every operation pending on psSyncInfo has completed. */
IMG_INTERNAL PVRSRV_ERROR SrvFlushClientOps(const PVRSRV_CONNECTION *psConnection,
					    const PVRSRV_CLIENT_SYNC_INFO *psSyncInfo);

IMG_INTERNAL PVRSRV_ERROR SrvEventObjectOpen(const PVRSRV_CONNECTION *psConnection,
					     const PVRSRV_EVENTOBJECT *psEventObject,
					     IMG_HANDLE *phOSEvent);
IMG_INTERNAL PVRSRV_ERROR SrvEventObjectClose(const PVRSRV_CONNECTION *psConnection,
					      const PVRSRV_EVENTOBJECT *psEventObject,
					      IMG_HANDLE hOSEventKM);
IMG_INTERNAL PVRSRV_ERROR SrvEventObjectWait(const PVRSRV_CONNECTION *psConnection,
					     IMG_HANDLE hOSEventKM);

/* Heap list of an existing device memory context
 * (PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO). Device-specific context code uses
 * it; it is not part of the exported interface. */
IMG_INTERNAL PVRSRV_ERROR SrvGetDeviceMemHeapInfo(const PVRSRV_DEV_DATA *psDevData,
						  IMG_HANDLE hDevMemContext,
						  IMG_UINT32 *pui32HeapCount,
						  PVRSRV_HEAP_INFO *psHeapInfo);

/* Releases the CPU side of a meminfo (sync mapping, buffer mapping, heap
 * blocks) and leaves the kernel allocation in place. */
IMG_INTERNAL PVRSRV_ERROR SrvUnrefDeviceMem(const PVRSRV_DEV_DATA *psDevData,
					    PVRSRV_CLIENT_MEM_INFO *psMemInfo);

#endif /* SRV_UM_PRIV_H */
