/* SPDX-License-Identifier: MIT */
/*
 * Debug output and error strings.
 *
 * PVRSRVDebugPrintf filters by a process-wide level mask (default 0x0f, the
 * DBGPRIV_FATAL..DBGPRIV_WARNING bits of pvr_debug.h) and writes to the
 * Android log under the tag IMGSRV, with the log priority chosen by level.
 */
#include "srv_um_priv.h"

#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IMG_UINT32 gui32DebugMask = DBGPRIV_FATAL | DBGPRIV_ERROR |
				   DBGPRIV_BUFFERED | DBGPRIV_WARNING;
static pthread_once_t gsDebugMaskOnce = PTHREAD_ONCE_INIT;

/* The PVRDebugLevel environment variable replaces the mask once per process
 * (strtol base 0, so 0x-prefixed hex is accepted). */
static void SrvReadDebugMask(void)
{
	const char *pszLevel = getenv("PVRDebugLevel");

	if (pszLevel != NULL)
	{
		gui32DebugMask = (IMG_UINT32)strtol(pszLevel, NULL, 0);
		printf("\nSetting Debug Level to 0x%x\n", gui32DebugMask);
	}
}

static int SrvLogPriority(IMG_UINT32 ui32DebugLevel)
{
	switch (ui32DebugLevel)
	{
		case DBGPRIV_FATAL:
		case DBGPRIV_ERROR:
			return ANDROID_LOG_ERROR;
		case DBGPRIV_BUFFERED:
			return ANDROID_LOG_SILENT;
		case DBGPRIV_WARNING:
			return ANDROID_LOG_WARN;
		case DBGPRIV_MESSAGE:
			return ANDROID_LOG_INFO;
		case DBGPRIV_VERBOSE:
			return ANDROID_LOG_VERBOSE;
		default:
			return ANDROID_LOG_DEBUG;
	}
}

IMG_EXPORT IMG_VOID PVRSRVDebugPrintf(IMG_UINT32 ui32DebugLevel,
				      const IMG_CHAR *pszFileName,
				      IMG_UINT32 ui32Line,
				      const IMG_CHAR *pszFormat,
				      ...)
{
	char acMessage[PVR_MAX_DEBUG_MESSAGE_LEN];
	const char *pszBase;
	va_list vaArgs;
	int iPriority;

	(void)pthread_once(&gsDebugMaskOnce, SrvReadDebugMask);

	if ((ui32DebugLevel & gui32DebugMask) == 0)
	{
		return;
	}

	iPriority = SrvLogPriority(ui32DebugLevel);
	if (iPriority == ANDROID_LOG_SILENT)
	{
		return;
	}

	pszBase = (pszFileName != NULL) ? strrchr(pszFileName, '/') : NULL;
	pszBase = (pszBase != NULL) ? pszBase + 1 : ((pszFileName != NULL) ? pszFileName : "");

	va_start(vaArgs, pszFormat);
	(void)vsnprintf(acMessage, sizeof(acMessage), pszFormat, vaArgs);
	va_end(vaArgs);

	(void)__android_log_print(iPriority, "IMGSRV", "%s:%lu: %s\n",
				  pszBase, (unsigned long)ui32Line, acMessage);
}

IMG_EXPORT IMG_VOID PVRSRVDebugPrintfDumpCCB(void)
{
}

IMG_EXPORT const IMG_CHAR *PVRSRVGetErrorString(PVRSRV_ERROR eError)
{
/* pvrsrv_errors.h is the body of this function: one switch over eError
 * returning the enumerator name, with a default for unknown values. */
#include "pvrsrv_errors.h"
}
