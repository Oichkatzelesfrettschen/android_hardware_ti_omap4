/* SPDX-License-Identifier: MIT */
/*
 * Display-class and buffer-class devices.
 *
 * PVRSRVOpenDCDevice and PVRSRVOpenBCDevice return a handle object that
 * pairs the kernel device handle with the services object of the opening
 * connection; every later call on that handle travels through the same
 * descriptor.
 */
#include "srv_um_priv.h"

#include <stdlib.h>
#include <string.h>

typedef struct SRV_CLASS_DEVICE_TAG
{
	IMG_HANDLE		hDeviceKM;
	const SRV_SERVICES	*psServices;
} SRV_CLASS_DEVICE;

static IMG_HANDLE SrvOpenClassDevice(const PVRSRV_DEV_DATA *psDevData, IMG_UINT32 ui32DeviceID,
				     IMG_BOOL bDisplay)
{
	PVRSRV_BRIDGE_IN_OPEN_DISPCLASS_DEVICE sIn;
	PVRSRV_BRIDGE_OUT_OPEN_DISPCLASS_DEVICE sOut;
	SRV_CLASS_DEVICE *psDevice;

	_Static_assert(sizeof(PVRSRV_BRIDGE_IN_OPEN_DISPCLASS_DEVICE) ==
		       sizeof(PVRSRV_BRIDGE_IN_OPEN_BUFFERCLASS_DEVICE), "open input layout");
	_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_OPEN_DISPCLASS_DEVICE) ==
		       sizeof(PVRSRV_BRIDGE_OUT_OPEN_BUFFERCLASS_DEVICE), "open output layout");

	if (psDevData == IMG_NULL || psDevData->psConnection == IMG_NULL ||
	    psDevData->psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("SrvOpenClassDevice: invalid parameters");
		return IMG_NULL;
	}

	psDevice = malloc(sizeof(*psDevice));
	if (psDevice == IMG_NULL)
	{
		SRV_ERR("SrvOpenClassDevice: out of memory");
		return IMG_NULL;
	}

	/* The display-class and buffer-class open structures share one
	 * layout: {flags, ui32DeviceID, hDevCookie} in, {eError, hDeviceKM}
	 * out. */
	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.ui32DeviceID = ui32DeviceID;
	sIn.hDevCookie = psDevData->hDevCookie;

	if (SrvBridgeCall(SrvDevServices(psDevData),
			  bDisplay ? PVRSRV_BRIDGE_OPEN_DISPCLASS_DEVICE :
				     PVRSRV_BRIDGE_OPEN_BUFFERCLASS_DEVICE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0 ||
	    sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("SrvOpenClassDevice: cannot open %s-class device %u (%d)",
			bDisplay ? "display" : "buffer", ui32DeviceID, sOut.eError);
		free(psDevice);
		return IMG_NULL;
	}

	psDevice->hDeviceKM = sOut.hDeviceKM;
	psDevice->psServices = SrvDevServices(psDevData);
	return psDevice;
}

IMG_EXPORT IMG_HANDLE PVRSRVOpenDCDevice(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					 IMG_UINT32 ui32DeviceID)
{
	return SrvOpenClassDevice(psDevData, ui32DeviceID, IMG_TRUE);
}

IMG_EXPORT IMG_HANDLE PVRSRVOpenBCDevice(IMG_CONST PVRSRV_DEV_DATA *psDevData,
					 IMG_UINT32 ui32DeviceID)
{
	return SrvOpenClassDevice(psDevData, ui32DeviceID, IMG_FALSE);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCloseDCDevice(IMG_CONST PVRSRV_CONNECTION *psConnection,
					    IMG_HANDLE hDevice)
{
	SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_CLOSE_DISPCLASS_DEVICE sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL || psDevice == IMG_NULL)
	{
		SRV_ERR("PVRSRVCloseDCDevice: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_CLOSE_DISPCLASS_DEVICE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVCloseDCDevice: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVCloseDCDevice: CLOSE_DISPCLASS_DEVICE failed (%d)", sOut.eError);
		return sOut.eError;
	}
	free(psDevice);
	return PVRSRV_OK;
}

/* The handle object is freed once the kernel call ran, whatever the kernel
 * answered. */
IMG_EXPORT PVRSRV_ERROR PVRSRVCloseBCDevice(IMG_CONST PVRSRV_CONNECTION *psConnection,
					    IMG_HANDLE hDevice)
{
	SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_CLOSE_BUFFERCLASS_DEVICE sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL || psDevice == IMG_NULL)
	{
		SRV_ERR("PVRSRVCloseBCDevice: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_CLOSE_BUFFERCLASS_DEVICE,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVCloseBCDevice: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	free(psDevice);
	return sOut.eError;
}

/* Bridge call on a class device whose output starts with eError: a bridge
 * failure becomes BRIDGE_CALL_FAILED, a kernel error is logged when bLog. */
static PVRSRV_ERROR SrvClassCall(const SRV_CLASS_DEVICE *psDevice, const char *pszFunc,
				 IMG_UINT32 ui32BridgeID, IMG_VOID *pvIn, IMG_UINT32 ui32InSize,
				 IMG_VOID *pvOut, IMG_UINT32 ui32OutSize, IMG_BOOL bLog)
{
	PVRSRV_ERROR eError;

	if (SrvBridgeCall(psDevice->psServices, ui32BridgeID, pvIn, ui32InSize,
			  pvOut, ui32OutSize) != 0)
	{
		SRV_ERR("%s: bridge call failed", pszFunc);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	memcpy(&eError, pvOut, sizeof(eError));
	if (eError != PVRSRV_OK && bLog)
	{
		SRV_ERR("%s: kernel call failed (%d)", pszFunc, eError);
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVEnumDCFormats(IMG_HANDLE hDevice,
					    IMG_UINT32 *pui32Count,
					    DISPLAY_FORMAT *psFormat)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_ENUM_DISPCLASS_FORMATS sIn;
	PVRSRV_BRIDGE_OUT_ENUM_DISPCLASS_FORMATS sOut;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32Count;

	if (psDevice == IMG_NULL || pui32Count == IMG_NULL)
	{
		SRV_ERR("PVRSRVEnumDCFormats: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_ENUM_DISPCLASS_FORMATS,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	ui32Count = sOut.ui32Count;
	if (ui32Count > PVRSRV_MAX_DC_DISPLAY_FORMATS)
	{
		ui32Count = PVRSRV_MAX_DC_DISPLAY_FORMATS;
	}
	*pui32Count = ui32Count;
	if (psFormat != IMG_NULL)
	{
		memcpy(psFormat, sOut.asFormat, ui32Count * sizeof(DISPLAY_FORMAT));
	}
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVEnumDCDims(IMG_HANDLE hDevice,
					 IMG_UINT32 *pui32Count,
					 DISPLAY_FORMAT *psFormat,
					 DISPLAY_DIMS *psDims)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_ENUM_DISPCLASS_DIMS sIn;
	PVRSRV_BRIDGE_OUT_ENUM_DISPCLASS_DIMS sOut;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32Count;

	if (psDevice == IMG_NULL || pui32Count == IMG_NULL || psFormat == IMG_NULL)
	{
		SRV_ERR("PVRSRVEnumDCDims: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.sFormat = *psFormat;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_ENUM_DISPCLASS_DIMS,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	ui32Count = sOut.ui32Count;
	if (ui32Count > PVRSRV_MAX_DC_DISPLAY_DIMENSIONS)
	{
		ui32Count = PVRSRV_MAX_DC_DISPLAY_DIMENSIONS;
	}
	*pui32Count = ui32Count;
	if (psDims != IMG_NULL)
	{
		memcpy(psDims, sOut.asDim, ui32Count * sizeof(DISPLAY_DIMS));
	}
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetDCInfo(IMG_HANDLE hDevice, DISPLAY_INFO *psDisplayInfo)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_GET_DISPCLASS_INFO sIn;
	PVRSRV_BRIDGE_OUT_GET_DISPCLASS_INFO sOut;
	PVRSRV_ERROR eError;

	if (psDevice == IMG_NULL || psDisplayInfo == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetDCInfo: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_GET_DISPCLASS_INFO,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError == PVRSRV_OK)
	{
		*psDisplayInfo = sOut.sDisplayInfo;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetBCBufferInfo(IMG_HANDLE hDevice, BUFFER_INFO *psBuffer)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_GET_BUFFERCLASS_INFO sIn;
	PVRSRV_BRIDGE_OUT_GET_BUFFERCLASS_INFO sOut;
	PVRSRV_ERROR eError;

	if (psDevice == IMG_NULL || psBuffer == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetBCBufferInfo: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_GET_BUFFERCLASS_INFO,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError == PVRSRV_OK)
	{
		*psBuffer = sOut.sBufferInfo;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetBCBuffer(IMG_HANDLE hDevice,
					  IMG_UINT32 ui32BufferIndex,
					  IMG_HANDLE *phBuffer)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_GET_BUFFERCLASS_BUFFER sIn;
	PVRSRV_BRIDGE_OUT_GET_BUFFERCLASS_BUFFER sOut;
	PVRSRV_ERROR eError;

	if (psDevice == IMG_NULL || phBuffer == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetBCBuffer: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	*phBuffer = IMG_NULL;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.ui32BufferIndex = ui32BufferIndex;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_GET_BUFFERCLASS_BUFFER,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError == PVRSRV_OK)
	{
		*phBuffer = sOut.hBuffer;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetDCBuffers2(IMG_HANDLE hDevice,
					    IMG_HANDLE hSwapChain,
					    IMG_HANDLE *phBuffer,
					    IMG_SYS_PHYADDR *psPhyAddr)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_GET_DISPCLASS_BUFFERS sIn;
	PVRSRV_BRIDGE_OUT_GET_DISPCLASS_BUFFERS sOut;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32Count;

	if (psDevice == IMG_NULL || hSwapChain == IMG_NULL || phBuffer == IMG_NULL)
	{
		SRV_ERR("PVRSRVGetDCBuffers2: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hSwapChain;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_GET_DISPCLASS_BUFFERS,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	ui32Count = sOut.ui32BufferCount;
	if (ui32Count > PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS)
	{
		ui32Count = PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS;
	}
	memcpy((void *)phBuffer, (const void *)sOut.ahBuffer, ui32Count * sizeof(IMG_HANDLE));
	if (psPhyAddr != IMG_NULL)
	{
		memcpy(psPhyAddr, sOut.asPhyAddr, ui32Count * sizeof(IMG_SYS_PHYADDR));
	}
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetDCBuffers(IMG_HANDLE hDevice,
					   IMG_HANDLE hSwapChain,
					   IMG_HANDLE *phBuffer)
{
	return PVRSRVGetDCBuffers2(hDevice, hSwapChain, phBuffer, IMG_NULL);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCreateDCSwapChain(IMG_HANDLE hDevice,
						IMG_UINT32 ui32Flags,
						DISPLAY_SURF_ATTRIBUTES *psDstSurfAttrib,
						DISPLAY_SURF_ATTRIBUTES *psSrcSurfAttrib,
						IMG_UINT32 ui32BufferCount,
						IMG_UINT32 ui32OEMFlags,
						IMG_UINT32 *pui32SwapChainID,
						IMG_HANDLE *phSwapChain)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_CREATE_DISPCLASS_SWAPCHAIN sIn;
	PVRSRV_BRIDGE_OUT_CREATE_DISPCLASS_SWAPCHAIN sOut;
	PVRSRV_ERROR eError;

	if (psDevice == IMG_NULL || psDstSurfAttrib == IMG_NULL || psSrcSurfAttrib == IMG_NULL ||
	    pui32SwapChainID == IMG_NULL || phSwapChain == IMG_NULL)
	{
		SRV_ERR("PVRSRVCreateDCSwapChain: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.ui32Flags = ui32Flags;
	sIn.sDstSurfAttrib = *psDstSurfAttrib;
	sIn.sSrcSurfAttrib = *psSrcSurfAttrib;
	sIn.ui32BufferCount = ui32BufferCount;
	sIn.ui32OEMFlags = ui32OEMFlags;
	sIn.ui32SwapChainID = *pui32SwapChainID;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_CREATE_DISPCLASS_SWAPCHAIN,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	if (eError == PVRSRV_OK)
	{
		*phSwapChain = sOut.hSwapChain;
		*pui32SwapChainID = sOut.ui32SwapChainID;
	}
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDestroyDCSwapChain(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_DESTROY_DISPCLASS_SWAPCHAIN sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevice == IMG_NULL)
	{
		SRV_ERR("PVRSRVDestroyDCSwapChain: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hSwapChain;

	return SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_DESTROY_DISPCLASS_SWAPCHAIN,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_FALSE);
}

static PVRSRV_ERROR SrvSetDCRect(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
				 const IMG_RECT *psRect, IMG_UINT32 ui32BridgeID,
				 const char *pszFunc)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_SET_DISPCLASS_RECT sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevice == IMG_NULL || psRect == IMG_NULL)
	{
		SRV_ERR("%s: invalid parameters", pszFunc);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hSwapChain;
	sIn.sRect = *psRect;

	return SrvClassCall(psDevice, pszFunc, ui32BridgeID, &sIn, sizeof(sIn),
			    &sOut, sizeof(sOut), IMG_TRUE);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSetDCDstRect(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
					   IMG_RECT *psDstRect)
{
	return SrvSetDCRect(hDevice, hSwapChain, psDstRect,
			    PVRSRV_BRIDGE_SET_DISPCLASS_DSTRECT, __func__);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSetDCSrcRect(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
					   IMG_RECT *psSrcRect)
{
	return SrvSetDCRect(hDevice, hSwapChain, psSrcRect,
			    PVRSRV_BRIDGE_SET_DISPCLASS_SRCRECT, __func__);
}

static PVRSRV_ERROR SrvSetDCColourKey(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
				      IMG_UINT32 ui32CKColour, IMG_UINT32 ui32BridgeID,
				      const char *pszFunc)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_SET_DISPCLASS_COLOURKEY sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevice == IMG_NULL)
	{
		SRV_ERR("%s: invalid parameters", pszFunc);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hSwapChain;
	sIn.ui32CKColour = ui32CKColour;

	return SrvClassCall(psDevice, pszFunc, ui32BridgeID, &sIn, sizeof(sIn),
			    &sOut, sizeof(sOut), IMG_TRUE);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSetDCDstColourKey(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
						IMG_UINT32 ui32CKColour)
{
	return SrvSetDCColourKey(hDevice, hSwapChain, ui32CKColour,
				 PVRSRV_BRIDGE_SET_DISPCLASS_DSTCOLOURKEY, __func__);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSetDCSrcColourKey(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain,
						IMG_UINT32 ui32CKColour)
{
	return SrvSetDCColourKey(hDevice, hSwapChain, ui32CKColour,
				 PVRSRV_BRIDGE_SET_DISPCLASS_SRCCOLOURKEY, __func__);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSwapToDCSystem(IMG_HANDLE hDevice, IMG_HANDLE hSwapChain)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_SWAP_DISPCLASS_TO_SYSTEM sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevice == IMG_NULL)
	{
		SRV_ERR("PVRSRVSwapToDCSystem: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hSwapChain;

	return SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_SWAP_DISPCLASS_TO_SYSTEM,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVSwapToDCBuffer(IMG_HANDLE hDevice,
					     IMG_HANDLE hBuffer,
					     IMG_UINT32 ui32ClipRectCount,
					     IMG_RECT *psClipRect,
					     IMG_UINT32 ui32SwapInterval,
					     IMG_HANDLE hPrivateTag)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_SWAP_DISPCLASS_TO_BUFFER sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psDevice == IMG_NULL || ui32ClipRectCount > PVRSRV_MAX_DC_CLIP_RECTS ||
	    (ui32ClipRectCount != 0 && psClipRect == IMG_NULL))
	{
		SRV_ERR("PVRSRVSwapToDCBuffer: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hBuffer = hBuffer;
	sIn.ui32SwapInterval = ui32SwapInterval;
	sIn.hPrivateTag = hPrivateTag;
	sIn.ui32ClipRectCount = ui32ClipRectCount;
	if (ui32ClipRectCount != 0)
	{
		memcpy(sIn.sClipRect, psClipRect, ui32ClipRectCount * sizeof(IMG_RECT));
	}

	return SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_SWAP_DISPCLASS_TO_BUFFER,
			    &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
}

/*
 * Queues a flip to hBuffer (the swap chain buffer handle) together with the
 * meminfos the composition read. PVRSRVSwapToDCBuffer2BW resolves the
 * meminfo handles; with PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC it ignores the
 * sync handles and returns a release fence descriptor in hFence (-1 when
 * the display driver produced none). *phFence is written on every path
 * past validation; it reads -1 unless the kernel installed a fence.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVSwapToDCBuffer2(IMG_HANDLE hDevice,
					      IMG_HANDLE hBuffer,
					      IMG_UINT32 ui32SwapInterval,
					      PVRSRV_CLIENT_MEM_INFO **ppsMemInfos,
					      PVRSRV_CLIENT_SYNC_INFO **ppsSyncInfos,
					      IMG_UINT32 ui32NumMemSyncInfos,
					      IMG_PVOID pvPrivData,
					      IMG_UINT32 ui32PrivDataLength,
					      IMG_HANDLE *phFence)
{
	const SRV_CLASS_DEVICE *psDevice = hDevice;
	PVRSRV_BRIDGE_IN_SWAP_DISPCLASS_TO_BUFFER2 sIn;
	PVRSRV_BRIDGE_OUT_SWAP_DISPCLASS_TO_BUFFER2 sOut;
	IMG_HANDLE *phMemHandles;
	IMG_HANDLE *phSyncHandles;
	PVRSRV_ERROR eError;
	IMG_UINT32 i;

	if (psDevice == IMG_NULL || hBuffer == IMG_NULL || phFence == IMG_NULL ||
	    ppsMemInfos == IMG_NULL || ui32NumMemSyncInfos == 0)
	{
		SRV_ERR("PVRSRVSwapToDCBuffer2: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	for (i = 0; i < ui32NumMemSyncInfos; i++)
	{
		if (ppsMemInfos[i] == IMG_NULL)
		{
			SRV_ERR("PVRSRVSwapToDCBuffer2: meminfo %u is NULL", i);
			return PVRSRV_ERROR_INVALID_PARAMS;
		}
	}

	phMemHandles = (IMG_HANDLE *)calloc(ui32NumMemSyncInfos, sizeof(IMG_HANDLE));
	phSyncHandles = (IMG_HANDLE *)calloc(ui32NumMemSyncInfos, sizeof(IMG_HANDLE));
	if (phMemHandles == IMG_NULL || phSyncHandles == IMG_NULL)
	{
		SRV_ERR("PVRSRVSwapToDCBuffer2: out of memory");
		free((void *)phMemHandles);
		free((void *)phSyncHandles);
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	for (i = 0; i < ui32NumMemSyncInfos; i++)
	{
		phMemHandles[i] = ppsMemInfos[i]->hKernelMemInfo;
		if (ppsSyncInfos != IMG_NULL && ppsSyncInfos[i] != IMG_NULL)
		{
			phSyncHandles[i] = ppsSyncInfos[i]->hKernelSyncInfo;
		}
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.hDeviceKM = psDevice->hDeviceKM;
	sIn.hSwapChain = hBuffer;
	sIn.ui32SwapInterval = ui32SwapInterval;
	sIn.ui32NumMemInfos = ui32NumMemSyncInfos;
	sIn.ppsKernelMemInfos = (PVRSRV_KERNEL_MEM_INFO **)phMemHandles;
	sIn.ppsKernelSyncInfos = (PVRSRV_KERNEL_SYNC_INFO **)phSyncHandles;
	sIn.ui32PrivDataLength = ui32PrivDataLength;
	sIn.pvPrivData = pvPrivData;

	eError = SrvClassCall(psDevice, __func__, PVRSRV_BRIDGE_SWAP_DISPCLASS_TO_BUFFER2,
			      &sIn, sizeof(sIn), &sOut, sizeof(sOut), IMG_TRUE);
	free((void *)phMemHandles);
	free((void *)phSyncHandles);
	/*
	 * The kernel copies its whole output buffer back even when the handler
	 * fails a handle lookup without writing hFence, and that buffer still
	 * holds the previous bridge call's output; only a successful swap
	 * carries a fence descriptor.
	 */
	*phFence = (eError == PVRSRV_OK) ? sOut.hFence : (IMG_HANDLE)(intptr_t)-1;
	return eError;
}
