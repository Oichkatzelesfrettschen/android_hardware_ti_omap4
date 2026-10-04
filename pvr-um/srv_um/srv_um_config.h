/* SPDX-License-Identifier: MIT */
/*
 * Build configuration of the pvrsrvkm services module this library binds to
 * (omap_android, SGX540 revision 120). PVRSRV_CLIENT_MEM_INFO, the bridge
 * structures of pvr_bridge.h and the bridge index numbering change with
 * these macros, so every translation unit includes this header before any
 * pvr-source header. The set mirrors the configuration the kernel module is
 * compiled with; srv_um_abi.c checks the resulting sizes and ioctl values.
 */
#ifndef SRV_UM_CONFIG_H
#define SRV_UM_CONFIG_H

#ifndef LINUX
#define LINUX
#endif
#define SUPPORT_SGX
#define SGX540
#define SUPPORT_SGX540
#define SGX_CORE_REV 120
#define TRANSFER_QUEUE
#define PVR_SECURE_HANDLES
#define SUPPORT_PERCONTEXT_PB
#define SUPPORT_HW_RECOVERY
#define SUPPORT_ACTIVE_POWER_MANAGEMENT
#define SUPPORT_SGX_HWPERF
#define SUPPORT_SGX_LOW_LATENCY_SCHEDULING
#define SUPPORT_MEMINFO_IDS
#define SUPPORT_SGX_NEW_STATUS_VALS
#define SUPPORT_ION
#define SUPPORT_PVRSRV_DEVICE_CLASS
#define SUPPORT_LARGE_GENERAL_HEAP
#define PVR_LINUX_USING_WORKQUEUES
#define IMG_ADDRSPACE_PHYSADDR_BITS 32

/* pvr_debug.h declares PVRSRVDebugPrintf and the PVR_DBG_* levels only
 * under these two macros. */
#define PVRSRV_NEED_PVR_DPF
#define PVRSRV_NEW_PVR_DPF

#endif /* SRV_UM_CONFIG_H */
