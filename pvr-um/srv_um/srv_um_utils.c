/* SPDX-License-Identifier: MIT */
/*
 * Process utilities: heap and library wrappers, monotonic time, process
 * identity, locale, the process-global mutex and the mutex objects behind
 * PVRSRV_MUTEX_HANDLE and PVRSRV_RECMUTEX_HANDLE.
 */
#include "srv_um_priv.h"

#include <dlfcn.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t gsProcessGlobalMutex = PTHREAD_MUTEX_INITIALIZER;

IMG_EXPORT IMG_PVOID PVRSRVAllocUserModeMem(IMG_SIZE_T uiSize)
{
	return malloc(uiSize);
}

IMG_EXPORT IMG_PVOID PVRSRVCallocUserModeMem(IMG_SIZE_T uiSize)
{
	return calloc(1, uiSize);
}

IMG_EXPORT IMG_PVOID PVRSRVReallocUserModeMem(IMG_PVOID pvBase, IMG_SIZE_T uiNewSize)
{
	return realloc(pvBase, uiNewSize);
}

IMG_EXPORT IMG_VOID PVRSRVFreeUserModeMem(IMG_PVOID pvMem)
{
	free(pvMem);
}

IMG_EXPORT IMG_VOID PVRSRVMemCopy(IMG_VOID *pvDst, const IMG_VOID *pvSrc, IMG_SIZE_T uiSize)
{
	memcpy(pvDst, pvSrc, uiSize);
}

IMG_EXPORT IMG_VOID PVRSRVMemSet(IMG_VOID *pvDest, IMG_UINT8 ui8Value, IMG_SIZE_T uiSize)
{
	memset(pvDest, ui8Value, uiSize);
}

IMG_EXPORT IMG_HANDLE PVRSRVLoadLibrary(const IMG_CHAR *pszLibraryName)
{
	return dlopen(pszLibraryName, RTLD_LAZY);
}

IMG_EXPORT PVRSRV_ERROR PVRSRVUnloadLibrary(IMG_HANDLE hExtDrv)
{
	if (hExtDrv == IMG_NULL)
	{
		SRV_ERR("PVRSRVUnloadLibrary: invalid library handle");
		return PVRSRV_ERROR_UNLOAD_LIBRARY_FAILED;
	}
	if (dlclose(hExtDrv) != 0)
	{
		SRV_ERR("PVRSRVUnloadLibrary: dlclose failed: %s", dlerror());
		return PVRSRV_ERROR_UNLOAD_LIBRARY_FAILED;
	}
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVGetLibFuncAddr(IMG_HANDLE hExtDrv,
					     const IMG_CHAR *pszFunctionName,
					     IMG_VOID **ppvFuncAddr)
{
	if (ppvFuncAddr == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	*ppvFuncAddr = dlsym(hExtDrv, pszFunctionName);
	return (*ppvFuncAddr == IMG_NULL) ? PVRSRV_ERROR_UNABLE_GET_FUNC_ADDR : PVRSRV_OK;
}

/* CLOCK_MONOTONIC in microseconds, truncated to 32 bits; callers compare
 * two readings by unsigned subtraction, which tolerates the wrap. */
IMG_EXPORT IMG_UINT32 PVRSRVClockus(void)
{
	struct timespec sNow;

	if (clock_gettime(CLOCK_MONOTONIC, &sNow) != 0)
	{
		SRV_ERR("PVRSRVClockus: clock_gettime failed (%d)", errno);
		abort();
	}
	return (IMG_UINT32)((IMG_UINT64)sNow.tv_sec * 1000000u +
			    (IMG_UINT64)sNow.tv_nsec / 1000u);
}

IMG_EXPORT IMG_VOID PVRSRVWaitus(IMG_UINT32 ui32Timeus)
{
	struct timespec sReq;
	struct timespec sRem;
	int iErr;

	sReq.tv_sec = (time_t)(ui32Timeus / 1000000u);
	sReq.tv_nsec = (long)(ui32Timeus % 1000000u) * 1000L;

	/* clock_nanosleep returns the error number; EINTR resumes with the
	 * remaining interval. */
	while ((iErr = clock_nanosleep(CLOCK_MONOTONIC, 0, &sReq, &sRem)) == EINTR)
	{
		sReq = sRem;
	}
	if (iErr != 0)
	{
		SRV_ERR("PVRSRVWaitus: clock_nanosleep failed (%d)", iErr);
		abort();
	}
}

IMG_EXPORT IMG_VOID PVRSRVReleaseThreadQuanta(void)
{
	(void)sleep(0);
}

/* bionic's getpid() caches the PID and refreshes the cache in the child of
 * fork, so it reports the caller's process after a fork as well. */
IMG_EXPORT IMG_UINT32 PVRSRVGetCurrentProcessID(void)
{
	return (IMG_UINT32)getpid();
}

IMG_EXPORT IMG_CHAR *PVRSRVSetLocale(const IMG_CHAR *pszLocale)
{
	return setlocale(LC_ALL, pszLocale);
}

IMG_EXPORT IMG_VOID PVRSRVLockProcessGlobalMutex(void)
{
	if (pthread_mutex_lock(&gsProcessGlobalMutex) != 0)
	{
		SRV_ERR("PVRSRVLockProcessGlobalMutex: pthread_mutex_lock failed");
		abort();
	}
}

IMG_EXPORT IMG_VOID PVRSRVUnlockProcessGlobalMutex(void)
{
	if (pthread_mutex_unlock(&gsProcessGlobalMutex) != 0)
	{
		SRV_ERR("PVRSRVUnlockProcessGlobalMutex: pthread_mutex_unlock failed");
		abort();
	}
}

/*
 * Mutex objects. The lock state lives in a flag guarded by an internal
 * pthread mutex, and waiters block on a condition variable. Ownership is
 * a property of the flag, so a lock taken by one thread may be released by
 * another, which the services interface permits for PVRSRV_MUTEX_HANDLE.
 * The recursive variant records the owning thread and a depth; the owner
 * re-enters, other threads wait until the depth returns to zero.
 */
struct _PVRSRV_MUTEX_OPAQUE_STRUCT_
{
	pthread_mutex_t	sLock;
	pthread_cond_t	sCond;
	IMG_BOOL	bLocked;
};

struct _PVRSRV_RECMUTEX_OPAQUE_STRUCT_
{
	pthread_mutex_t	sLock;
	pthread_cond_t	sCond;
	pthread_t	sOwner;
	IMG_UINT32	ui32Depth;
};

static PVRSRV_ERROR SrvInitLockPair(pthread_mutex_t *psLock, pthread_cond_t *psCond)
{
	if (pthread_mutex_init(psLock, NULL) != 0)
	{
		SRV_ERR("SrvInitLockPair: pthread_mutex_init failed");
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	if (pthread_cond_init(psCond, NULL) != 0)
	{
		SRV_ERR("SrvInitLockPair: pthread_cond_init failed");
		(void)pthread_mutex_destroy(psLock);
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	return PVRSRV_OK;
}

static PVRSRV_ERROR SrvDestroyLockPair(pthread_mutex_t *psLock, pthread_cond_t *psCond)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (pthread_cond_destroy(psCond) != 0)
	{
		SRV_ERR("SrvDestroyLockPair: pthread_cond_destroy failed");
		eError = PVRSRV_ERROR_MUTEX_DESTROY_FAILED;
	}
	if (pthread_mutex_destroy(psLock) != 0)
	{
		SRV_ERR("SrvDestroyLockPair: pthread_mutex_destroy failed");
		eError = PVRSRV_ERROR_MUTEX_DESTROY_FAILED;
	}
	return eError;
}

static void SrvCheck(int iErr, const char *pszWhat)
{
	if (iErr != 0)
	{
		SRV_ERR("%s failed (%d)", pszWhat, iErr);
		abort();
	}
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCreateMutex(PVRSRV_MUTEX_HANDLE *phMutex)
{
	PVRSRV_MUTEX_HANDLE psMutex;
	PVRSRV_ERROR eError;

	if (phMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	psMutex = malloc(sizeof(*psMutex));
	if (psMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	eError = SrvInitLockPair(&psMutex->sLock, &psMutex->sCond);
	if (eError != PVRSRV_OK)
	{
		free(psMutex);
		return eError;
	}
	psMutex->bLocked = IMG_FALSE;
	*phMutex = psMutex;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDestroyMutex(PVRSRV_MUTEX_HANDLE hMutex)
{
	PVRSRV_ERROR eError;

	if (hMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	eError = SrvDestroyLockPair(&hMutex->sLock, &hMutex->sCond);
	if (eError == PVRSRV_OK)
	{
		free(hMutex);
	}
	return eError;
}

IMG_EXPORT IMG_VOID PVRSRVLockMutex(PVRSRV_MUTEX_HANDLE hMutex)
{
	SrvCheck(pthread_mutex_lock(&hMutex->sLock), "PVRSRVLockMutex: lock");
	while (hMutex->bLocked)
	{
		SrvCheck(pthread_cond_wait(&hMutex->sCond, &hMutex->sLock), "PVRSRVLockMutex: wait");
	}
	hMutex->bLocked = IMG_TRUE;
	SrvCheck(pthread_mutex_unlock(&hMutex->sLock), "PVRSRVLockMutex: unlock");
}

IMG_EXPORT IMG_VOID PVRSRVUnlockMutex(PVRSRV_MUTEX_HANDLE hMutex)
{
	SrvCheck(pthread_mutex_lock(&hMutex->sLock), "PVRSRVUnlockMutex: lock");
	hMutex->bLocked = IMG_FALSE;
	SrvCheck(pthread_cond_signal(&hMutex->sCond), "PVRSRVUnlockMutex: signal");
	SrvCheck(pthread_mutex_unlock(&hMutex->sLock), "PVRSRVUnlockMutex: unlock");
}

IMG_EXPORT PVRSRV_ERROR PVRSRVCreateRecursiveMutex(PVRSRV_RECMUTEX_HANDLE *phMutex)
{
	PVRSRV_RECMUTEX_HANDLE psMutex;
	PVRSRV_ERROR eError;

	if (phMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	psMutex = malloc(sizeof(*psMutex));
	if (psMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	eError = SrvInitLockPair(&psMutex->sLock, &psMutex->sCond);
	if (eError != PVRSRV_OK)
	{
		free(psMutex);
		return eError;
	}
	psMutex->ui32Depth = 0;
	*phMutex = psMutex;
	return PVRSRV_OK;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVDestroyRecursiveMutex(PVRSRV_RECMUTEX_HANDLE hMutex)
{
	PVRSRV_ERROR eError;

	if (hMutex == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	eError = SrvDestroyLockPair(&hMutex->sLock, &hMutex->sCond);
	if (eError == PVRSRV_OK)
	{
		free(hMutex);
	}
	return eError;
}

IMG_EXPORT IMG_VOID PVRSRVLockRecursiveMutex(PVRSRV_RECMUTEX_HANDLE hMutex)
{
	pthread_t sSelf = pthread_self();

	SrvCheck(pthread_mutex_lock(&hMutex->sLock), "PVRSRVLockRecursiveMutex: lock");
	if (hMutex->ui32Depth == 0 || !pthread_equal(hMutex->sOwner, sSelf))
	{
		while (hMutex->ui32Depth != 0)
		{
			SrvCheck(pthread_cond_wait(&hMutex->sCond, &hMutex->sLock),
				 "PVRSRVLockRecursiveMutex: wait");
		}
		hMutex->sOwner = sSelf;
	}
	hMutex->ui32Depth++;
	SrvCheck(pthread_mutex_unlock(&hMutex->sLock), "PVRSRVLockRecursiveMutex: unlock");
}

IMG_EXPORT IMG_VOID PVRSRVUnlockRecursiveMutex(PVRSRV_RECMUTEX_HANDLE hMutex)
{
	SrvCheck(pthread_mutex_lock(&hMutex->sLock), "PVRSRVUnlockRecursiveMutex: lock");
	if (hMutex->ui32Depth == 0)
	{
		SRV_ERR("PVRSRVUnlockRecursiveMutex: mutex is not locked");
		abort();
	}
	hMutex->ui32Depth--;
	if (hMutex->ui32Depth == 0)
	{
		SrvCheck(pthread_cond_signal(&hMutex->sCond), "PVRSRVUnlockRecursiveMutex: signal");
	}
	SrvCheck(pthread_mutex_unlock(&hMutex->sLock), "PVRSRVUnlockRecursiveMutex: unlock");
}
