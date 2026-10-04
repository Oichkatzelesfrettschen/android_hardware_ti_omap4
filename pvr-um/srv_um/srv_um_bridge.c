/* SPDX-License-Identifier: MIT */
/*
 * Bridge transport and kernel-memory mapping.
 *
 * Every services call is one ioctl on /dev/pvrsrvkm carrying a
 * PVRSRV_BRIDGE_PACKAGE. PVRSRV_BridgeDispatchKM copies the package,
 * BridgedDispatchKM copies ui32InBufferSize bytes in, runs the handler and
 * copies ui32OutBufferSize bytes back, so the output size is a kernel write
 * length and always equals sizeof the handler's output structure.
 */
#include "srv_um_priv.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* mmap2 takes its offset in 4096-byte units; PVRMMapOSMemHandleToMMapData
 * scales uiMMapOffset to that unit before returning it. */
#define SRV_MMAP2_UNIT_SHIFT 12

/*
 * PVRMMapOSMemHandleToMMapData registers an offset structure keyed by
 * (uiMMapOffset, size, PID, and for physical offsets the TID) that PVRMMap
 * looks up when the mmap arrives. The lock keeps each MHANDLE_TO_MMAP_DATA
 * + mmap pair and each RELEASE_MMAP_DATA + munmap pair atomic within the
 * process, so two threads never race on one memory area's offset structure.
 */
static pthread_mutex_t gsMapLock = PTHREAD_MUTEX_INITIALIZER;

/* bionic's strerror returns a constant string or a thread-local buffer, so
 * it is safe from any thread. */
static void SrvErrno(const char *pszWhat, int iErr)
{
	SRV_ERR("%s: %s", pszWhat, strerror(iErr));
}

static void SrvMapLock(void)
{
	if (pthread_mutex_lock(&gsMapLock) != 0)
	{
		SRV_ERR("SrvMapLock: pthread_mutex_lock failed");
		abort();
	}
}

static void SrvMapUnlock(void)
{
	if (pthread_mutex_unlock(&gsMapLock) != 0)
	{
		SRV_ERR("SrvMapUnlock: pthread_mutex_unlock failed");
		abort();
	}
}

IMG_INTERNAL IMG_INT SrvBridgeCall(const SRV_SERVICES *psServices,
				   IMG_UINT32 ui32BridgeID,
				   IMG_VOID *pvIn, IMG_UINT32 ui32InSize,
				   IMG_VOID *pvOut, IMG_UINT32 ui32OutSize)
{
	PVRSRV_BRIDGE_PACKAGE sPackage;
	int iRet;

	sPackage.ui32BridgeID = ui32BridgeID;
	sPackage.ui32Size = (IMG_UINT32)sizeof(sPackage);
	sPackage.pvParamIn = pvIn;
	sPackage.ui32InBufferSize = ui32InSize;
	sPackage.pvParamOut = pvOut;
	sPackage.ui32OutBufferSize = ui32OutSize;
	sPackage.hKernelServices = psServices->hKernelServices;

	/* The KM reads the command from sPackage.ui32BridgeID; the request
	 * value carries the same _IOWR('g', index, PVRSRV_BRIDGE_PACKAGE). */
	iRet = ioctl(psServices->iFd, (int)ui32BridgeID, &sPackage);
	if (iRet < 0)
	{
		int iErr = errno;
		char acWhat[48];

		(void)snprintf(acWhat, sizeof(acWhat), "ioctl 0x%08x", ui32BridgeID);
		SrvErrno(acWhat, iErr);
	}
	return iRet;
}

/* Undoes the reference MHANDLE_TO_MMAP_DATA took when the following mmap
 * failed; the kernel has no user address recorded, so bMUnmap is false. */
static void SrvReleaseMMapData(const SRV_SERVICES *psServices, IMG_HANDLE hMHandle)
{
	PVRSRV_BRIDGE_IN_RELEASE_MMAP_DATA sIn;
	PVRSRV_BRIDGE_OUT_RELEASE_MMAP_DATA sOut;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hMHandle = hMHandle;
	(void)SrvBridgeCall(psServices, PVRSRV_BRIDGE_RELEASE_MMAP_DATA,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut));
}

IMG_INTERNAL PVRSRV_ERROR SrvMapKernelMem(const SRV_SERVICES *psServices,
					  IMG_VOID **ppvLinAddr,
					  IMG_HANDLE *phMappingBase,
					  IMG_HANDLE hMHandle)
{
	PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA sIn;
	PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA sOut;
	IMG_UINT8 *pui8Base;

	*ppvLinAddr = IMG_NULL;
	*phMappingBase = IMG_NULL;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hMHandle = hMHandle;

	SrvMapLock();

	if (SrvBridgeCall(psServices, PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0 ||
	    sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("SrvMapKernelMem: MHANDLE_TO_MMAP_DATA failed (%d)", sOut.eError);
		SrvMapUnlock();
		return PVRSRV_ERROR_BAD_MAPPING;
	}

	if (sOut.uiUserVAddr != 0)
	{
		/* The kernel already holds a mapping of this area for this
		 * process and took another reference on it. */
		pui8Base = (IMG_UINT8 *)sOut.uiUserVAddr;
	}
	else
	{
		void *pvMap = mmap64(IMG_NULL, sOut.uiRealByteSize,
				     PROT_READ | PROT_WRITE, MAP_SHARED,
				     psServices->iFd,
				     (off64_t)sOut.uiMMapOffset << SRV_MMAP2_UNIT_SHIFT);

		if (pvMap == MAP_FAILED)
		{
			SrvErrno("SrvMapKernelMem: mmap", errno);
			SrvReleaseMMapData(psServices, hMHandle);
			SrvMapUnlock();
			return PVRSRV_ERROR_BAD_MAPPING;
		}
		pui8Base = pvMap;
	}

	SrvMapUnlock();

	*phMappingBase = pui8Base;
	*ppvLinAddr = pui8Base + sOut.uiByteOffset;
	return PVRSRV_OK;
}

IMG_INTERNAL IMG_BOOL SrvUnmapKernelMem(const SRV_SERVICES *psServices,
					IMG_HANDLE hMappingBase,
					IMG_HANDLE hMHandle)
{
	PVRSRV_BRIDGE_IN_RELEASE_MMAP_DATA sIn;
	PVRSRV_BRIDGE_OUT_RELEASE_MMAP_DATA sOut;

	if (hMappingBase == IMG_NULL)
	{
		return IMG_TRUE;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hMHandle = hMHandle;

	SrvMapLock();

	if (SrvBridgeCall(psServices, PVRSRV_BRIDGE_RELEASE_MMAP_DATA,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("SrvUnmapKernelMem: RELEASE_MMAP_DATA bridge call failed");
		SrvMapUnlock();
		return IMG_FALSE;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("SrvUnmapKernelMem: RELEASE_MMAP_DATA failed (%d)", sOut.eError);
		SrvMapUnlock();
		return IMG_FALSE;
	}

	/* PVRMMapReleaseMMapData sets bMUnmap when the last reference of this
	 * process goes and reports the address and size it recorded at mmap
	 * time; that region is the one to unmap. */
	if (sOut.bMUnmap && munmap((void *)sOut.uiUserVAddr, sOut.uiRealByteSize) != 0)
	{
		SrvErrno("SrvUnmapKernelMem: munmap", errno);
		SrvMapUnlock();
		return IMG_FALSE;
	}

	SrvMapUnlock();
	return IMG_TRUE;
}
