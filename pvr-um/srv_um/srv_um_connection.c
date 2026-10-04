/* SPDX-License-Identifier: MIT */
/*
 * Connection, device enumeration and device acquisition.
 *
 * A connection owns one /dev/pvrsrvkm descriptor. The kernel creates the
 * per-process data on the first bridge call of a PID and returns its handle
 * from PVRSRV_BRIDGE_CONNECT_SERVICES; every later package names it.
 */
#include "srv_um_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pvrversion.h"

#define SRV_CONNECT_ATTEMPTS	10
#define SRV_CONNECT_RETRY_US	500000u

static PVRSRV_ERROR SrvOpenServices(IMG_HANDLE *phServices, IMG_UINT32 ui32SrvFlags)
{
	PVRSRV_BRIDGE_IN_CONNECT_SERVICES sIn;
	PVRSRV_BRIDGE_OUT_CONNECT_SERVICES sOut;
	SRV_SERVICES *psServices;
	int iFd;

	iFd = open("/dev/pvrsrvkm", O_RDWR | O_CLOEXEC);
	if (iFd < 0)
	{
		SRV_ERR("SrvOpenServices: cannot open /dev/pvrsrvkm: %s", strerror(errno));
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	psServices = malloc(sizeof(*psServices));
	if (psServices == IMG_NULL)
	{
		SRV_ERR("SrvOpenServices: out of memory");
		(void)close(iFd);
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	psServices->iFd = iFd;
	psServices->hKernelServices = IMG_NULL;

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.ui32Flags = ui32SrvFlags;

	if (SrvBridgeCall(psServices, PVRSRV_BRIDGE_CONNECT_SERVICES,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0 ||
	    sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("SrvOpenServices: CONNECT_SERVICES failed (%d)", sOut.eError);
		free(psServices);
		(void)close(iFd);
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	psServices->hKernelServices = sOut.hKernelServices;
	*phServices = psServices;
	return PVRSRV_OK;
}

/* Disconnects and closes the descriptor. The services object stays
 * allocated when close() fails, matching UNABLE_TO_CLOSE_SERVICES. */
static PVRSRV_ERROR SrvCloseServices(IMG_HANDLE hServices)
{
	SRV_SERVICES *psServices = hServices;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psServices == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* PVRSRVDisconnectBW always reports PVRSRV_OK; the per-process data
	 * goes when the last descriptor of the process closes. */
	memset(&sOut, 0, sizeof(sOut));
	(void)SrvBridgeCall(psServices, PVRSRV_BRIDGE_DISCONNECT_SERVICES,
			    IMG_NULL, 0, &sOut, sizeof(sOut));

	if (close(psServices->iFd) != 0)
	{
		SRV_ERR("SrvCloseServices: close failed: %s", strerror(errno));
		return PVRSRV_ERROR_UNABLE_TO_CLOSE_SERVICES;
	}
	free(psServices);
	return PVRSRV_OK;
}

static PVRSRV_ERROR SrvConnectInternal(PVRSRV_CONNECTION **ppsConnection, IMG_UINT32 ui32SrvFlags)
{
	PVRSRV_CONNECTION *psConnection;
	PVRSRV_ERROR eError;

	psConnection = calloc(1, sizeof(*psConnection));
	if (psConnection == IMG_NULL)
	{
		SRV_ERR("SrvConnectInternal: out of memory");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = SrvOpenServices(&psConnection->hServices, ui32SrvFlags);
	if (eError != PVRSRV_OK)
	{
		free(psConnection);
		*ppsConnection = IMG_NULL;
		return eError;
	}

	psConnection->ui32ProcessID = PVRSRVGetCurrentProcessID();
	*ppsConnection = psConnection;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVConnect(PVRSRV_CONNECTION **ppsConnection, IMG_UINT32 ui32SrvFlags)
{
	PVRSRV_CONNECTION *psConnection = IMG_NULL;
	PVRSRV_CLIENT_DEV_DATA *psDevs;
	PVRSRV_ERROR eError = PVRSRV_ERROR_INIT_FAILURE;
	IMG_UINT32 i;

	if (ppsConnection == IMG_NULL)
	{
		SRV_ERR("PVRSRVConnect: Invalid connection");
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	/* The services node appears late in boot and the kernel refuses
	 * non-init processes until the init server has run, so connecting
	 * retries for up to SRV_CONNECT_ATTEMPTS * SRV_CONNECT_RETRY_US. */
	for (i = 0; i < SRV_CONNECT_ATTEMPTS; i++)
	{
		if (i != 0)
		{
			PVRSRVWaitus(SRV_CONNECT_RETRY_US);
		}
		eError = SrvConnectInternal(&psConnection, ui32SrvFlags);
		if (eError == PVRSRV_OK)
		{
			break;
		}
	}
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVConnect: Unable to open connection");
		*ppsConnection = IMG_NULL;
		return eError;
	}

	/* From here on the connection belongs to the caller on every path,
	 * also when device enumeration or the device connect checks fail. */
	*ppsConnection = psConnection;
	psConnection->ui32SrvFlags = ui32SrvFlags;
	psDevs = &psConnection->sClientDevData;

	for (i = 0; i < SRV_CONNECT_ATTEMPTS; i++)
	{
		if (i != 0)
		{
			PVRSRVWaitus(SRV_CONNECT_RETRY_US);
		}
		eError = PVRSRVEnumerateDevices(psConnection, &psDevs->ui32NumDevices,
						psDevs->asDevID);
		if (eError == PVRSRV_OK)
		{
			break;
		}
	}
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVConnect: Unable to enumerate devices (%d)", eError);
		return eError;
	}

	/* The per-device connect check and dump-trace callbacks belong to the
	 * device-specific client layer; the core services layer installs
	 * none, so both tables stay empty. */
	for (i = 0; i < psDevs->ui32NumDevices; i++)
	{
		psDevs->apfnDevConnect[i] = IMG_NULL;
		psDevs->apfnDumpTrace[i] = IMG_NULL;
	}

	for (i = 0; i < psDevs->ui32NumDevices; i++)
	{
		PVRSRV_DEV_DATA sDevData;

		eError = PVRSRVAcquireDeviceData(psConnection, psDevs->asDevID[i].ui32DeviceIndex,
						 &sDevData, PVRSRV_DEVICE_TYPE_UNKNOWN);
		if (eError != PVRSRV_OK)
		{
			SRV_ERR("PVRSRVConnect: cannot acquire device %u (%d)",
				psDevs->asDevID[i].ui32DeviceIndex, eError);
			return eError;
		}
		if (psDevs->apfnDevConnect[i] != IMG_NULL)
		{
			eError = psDevs->apfnDevConnect[i](&sDevData);
			if (eError != PVRSRV_OK)
			{
				SRV_ERR("PVRSRVConnect: device %u connect check failed (%d)",
					psDevs->asDevID[i].ui32DeviceIndex, eError);
				return eError;
			}
		}
	}

	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDisconnect(IMG_CONST PVRSRV_CONNECTION *psConnection)
{
	PVRSRV_ERROR eError;

	if (psConnection == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	eError = SrvCloseServices(psConnection->hServices);
	free((PVRSRV_CONNECTION *)psConnection);
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVEnumerateDevices(IMG_CONST PVRSRV_CONNECTION *psConnection,
					       IMG_UINT32 *puiNumDevices,
					       PVRSRV_DEVICE_IDENTIFIER *puiDevIDs)
{
	PVRSRV_BRIDGE_OUT_ENUMDEVICE sOut;
	IMG_UINT32 ui32Count;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL ||
	    puiNumDevices == IMG_NULL || puiDevIDs == IMG_NULL)
	{
		SRV_ERR("PVRSRVEnumerateDevices: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sOut, 0, sizeof(sOut));
	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_ENUM_DEVICES,
			  IMG_NULL, 0, &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVEnumerateDevices: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVEnumerateDevices: ENUM_DEVICES failed (%d)", sOut.eError);
		return sOut.eError;
	}

	ui32Count = sOut.ui32NumDevices;
	if (ui32Count > PVRSRV_MAX_DEVICES)
	{
		ui32Count = PVRSRV_MAX_DEVICES;
	}
	*puiNumDevices = ui32Count;
	memcpy(puiDevIDs, sOut.asDeviceIdentifier, ui32Count * sizeof(PVRSRV_DEVICE_IDENTIFIER));
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVAcquireDeviceData(IMG_CONST PVRSRV_CONNECTION *psConnection,
						IMG_UINT32 uiDevIndex,
						PVRSRV_DEV_DATA *psDevData,
						PVRSRV_DEVICE_TYPE eDeviceType)
{
	PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO sIn;
	PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL ||
	    psDevData == IMG_NULL)
	{
		SRV_ERR("PVRSRVAcquireDeviceData: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.uiDevIndex = uiDevIndex;
	sIn.eDeviceType = eDeviceType;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVAcquireDeviceData: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVAcquireDeviceData: ACQUIRE_DEVICEINFO failed (%d)", sOut.eError);
		return sOut.eError;
	}

	psDevData->psConnection = psConnection;
	psDevData->hDevCookie = sOut.hDevCookie;
	return PVRSRV_OK;
}

/* Two-call contract: with pui32DevID NULL the count goes to
 * *pui32DevCount; with pui32DevID set the IDs go there and *pui32DevCount
 * stays untouched, so callers size the array from the first call. */
IMG_EXPORT PVRSRV_ERROR PVRSRVEnumerateDeviceClass(IMG_CONST PVRSRV_CONNECTION *psConnection,
						   PVRSRV_DEVICE_CLASS DeviceClass,
						   IMG_UINT32 *pui32DevCount,
						   IMG_UINT32 *pui32DevID)
{
	PVRSRV_BRIDGE_IN_ENUMCLASS sIn;
	PVRSRV_BRIDGE_OUT_ENUMCLASS sOut;
	IMG_UINT32 ui32Count;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL ||
	    pui32DevCount == IMG_NULL)
	{
		SRV_ERR("PVRSRVEnumerateDeviceClass: invalid parameters");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.sDeviceClass = DeviceClass;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_ENUM_CLASS,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVEnumerateDeviceClass: bridge call failed");
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVEnumerateDeviceClass: ENUM_CLASS failed (%d)", sOut.eError);
		return sOut.eError;
	}

	ui32Count = sOut.ui32NumDevices;
	if (ui32Count > PVRSRV_MAX_DEVICES)
	{
		ui32Count = PVRSRV_MAX_DEVICES;
	}
	if (pui32DevID != IMG_NULL)
	{
		memcpy(pui32DevID, sOut.ui32DevID, ui32Count * sizeof(IMG_UINT32));
	}
	else
	{
		*pui32DevCount = ui32Count;
	}
	return PVRSRV_OK;
}

/*
 * Connection of the init server. PVRSRV_BRIDGE_INITSRV_CONNECT succeeds
 * only for a CAP_SYS_MODULE process while the init server has neither run
 * nor started; the kernel then marks this process as the init process.
 * This entry point has no services.h prototype.
 */
IMG_EXPORT PVRSRV_ERROR PVRSRVInitSrvConnect(PVRSRV_CONNECTION **ppsConnection)
{
	PVRSRV_BRIDGE_IN_COMPAT_CHECK sCompatIn;
	PVRSRV_BRIDGE_RETURN sOut;
	PVRSRV_ERROR eError;

	if (ppsConnection == IMG_NULL)
	{
		SRV_ERR("PVRSRVInitSrvConnect: Invalid connection");
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	eError = SrvConnectInternal(ppsConnection, 0);
	if (eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVInitSrvConnect: Unable to open connection (%d)", eError);
		return eError;
	}

	/* PVRSRVCompatCheckKM compares these words with the kernel's own
	 * PVRVERSION_* values and reports DDK_VERSION_MISMATCH otherwise. */
	memset(&sCompatIn, 0, sizeof(sCompatIn));
	memset(&sOut, 0, sizeof(sOut));
	sCompatIn.ui32DDKVersion = (PVRVERSION_MAJ << 16) | (PVRVERSION_MIN << 8);
	sCompatIn.ui32DDKBuild = PVRVERSION_BUILD;

	if (SrvBridgeCall(SrvServices(*ppsConnection), PVRSRV_BRIDGE_UM_KM_COMPAT_CHECK,
			  &sCompatIn, sizeof(sCompatIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVInitSrvConnect: compatibility check bridge call failed");
		(void)PVRSRVDisconnect(*ppsConnection);
		*ppsConnection = IMG_NULL;
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	/* A connection that fails the check never issues INITSRV_CONNECT, so
	 * this process never becomes the init process; the descriptor closes
	 * and *ppsConnection reads NULL, as on the bridge-failure paths. */
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVInitSrvConnect: UM/KM compatibility check failed (%d)", sOut.eError);
		(void)PVRSRVDisconnect(*ppsConnection);
		*ppsConnection = IMG_NULL;
		return sOut.eError;
	}

	memset(&sOut, 0, sizeof(sOut));
	if (SrvBridgeCall(SrvServices(*ppsConnection), PVRSRV_BRIDGE_INITSRV_CONNECT,
			  IMG_NULL, 0, &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVInitSrvConnect: INITSRV_CONNECT bridge call failed");
		(void)PVRSRVDisconnect(*ppsConnection);
		*ppsConnection = IMG_NULL;
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	return sOut.eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVInitSrvDisconnect(PVRSRV_CONNECTION *psConnection,
						IMG_BOOL bInitSuccesful)
{
	PVRSRV_BRIDGE_IN_INITSRV_DISCONNECT sIn;
	PVRSRV_BRIDGE_RETURN sOut;

	if (psConnection == IMG_NULL || psConnection->hServices == IMG_NULL)
	{
		SRV_ERR("PVRSRVInitSrvDisconnect: Invalid connection");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	memset(&sIn, 0, sizeof(sIn));
	memset(&sOut, 0, sizeof(sOut));
	sIn.bInitSuccesful = bInitSuccesful;

	if (SrvBridgeCall(SrvServices(psConnection), PVRSRV_BRIDGE_INITSRV_DISCONNECT,
			  &sIn, sizeof(sIn), &sOut, sizeof(sOut)) != 0)
	{
		SRV_ERR("PVRSRVInitSrvDisconnect: bridge call failed");
		(void)PVRSRVDisconnect(psConnection);
		return PVRSRV_ERROR_BRIDGE_CALL_FAILED;
	}
	if (sOut.eError != PVRSRV_OK)
	{
		SRV_ERR("PVRSRVInitSrvDisconnect: INITSRV_DISCONNECT failed (%d)", sOut.eError);
		(void)PVRSRVDisconnect(psConnection);
		return sOut.eError;
	}
	return PVRSRVDisconnect(psConnection);
}
