/* Host ownership test: the included function bodies and macros are production code. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USE_ION
#define ENABLE_GRALLOC_BUFFERS
#define ALLOCATE_TILER_BUFFER_IN_PROXY
#define OMX_IN
#define OMX_INOUT
#define OMX_TRUE 1
#define OMX_FALSE 0
#define OMX_VER_MAJOR 1
#define OMX_VER_MINOR 1
#define TIMM_OSAL_TRUE 1
#define TIMMOSAL_MEM_SEGMENT_INT 0
#define LINUX_PAGE_SIZE 4096
#define MAX_NUM_PROXY_BUFFERS 100
#define PROXY_MAXNUMOFPORTS 8
#define DEFAULT 0
#define OMX_TI_IndexParamMetaDataBufferInfo 1
#define DOMX_ERROR(...) ((void)0)
#define DOMX_DEBUG(...) ((void)0)
#define DOMX_ENTER(...) ((void)0)
#define DOMX_EXIT(...) ((void)0)
#define TIMM_OSAL_Memset memset

_Static_assert(sizeof(void *) == 4, "DOMX requires 32-bit pointers");
typedef void *OMX_PTR;
typedef void *OMX_HANDLETYPE;
typedef uint32_t OMX_U32;
typedef uint8_t OMX_U8;
typedef int OMX_BOOL;
typedef int OMX_INDEXTYPE;
typedef int OMX_ERRORTYPE;
typedef int RPC_OMX_ERRORTYPE;
typedef int MEMPLUGIN_ERRORTYPE;
typedef uintptr_t ion_user_handle_t;
enum {
    OMX_ErrorNone, OMX_ErrorNoMore, OMX_ErrorInsufficientResources,
    OMX_ErrorBadParameter, OMX_ErrorHardware, OMX_ErrorUnsupportedIndex,
    OMX_ErrorTimeout, OMX_ErrorUndefined, OMX_ErrorVersionMismatch
};
enum {
    RPC_OMX_ErrorNone, RPC_OMX_ErrorInsufficientResources,
    RPC_OMX_ErrorBadParameter, RPC_OMX_ErrorHardware,
    RPC_OMX_ErrorUnsupportedIndex, RPC_OMX_ErrorTimeout
};
enum { MEMPLUGIN_ERROR_NONE, MEMPLUGIN_ERROR_NORESOURCES };
enum { OMX_IOCIONREGISTER = 2, OMX_IOCIONUNREGISTER, OMX_IOCPVRREGISTER };
typedef enum {
    VirtualPointers, GrallocPointers, IONPointers,
    EncoderMetadataPointers, BufferDescriptorVirtual2D
} PROXY_BUFFER_TYPE;
typedef union {
    struct {
        uint8_t nVersionMajor, nVersionMinor, nRevision, nStep;
    } s;
    uint32_t nVersion;
} OMX_VERSIONTYPE;
typedef struct {
    OMX_U32 nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U8 *pBuffer;
    OMX_U32 nAllocLen;
    OMX_PTR pPlatformPrivate;
} OMX_BUFFERHEADERTYPE;
typedef struct {
    OMX_U32 nSize;
    OMX_PTR pAuxBuf1;
    OMX_PTR pMetaDataBuffer;
    OMX_U32 nMetaDataSize;
} OMX_TI_PLATFORMPRIVATE;
typedef struct {
    OMX_U32 nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32 nPortIndex;
    OMX_BOOL bIsMetaDataEnabledOnPort;
    OMX_U32 nMetaDataSize;
} OMX_TI_PARAM_METADATABUFFERINFO;
typedef struct { OMX_PTR pBuf[2]; } OMX_TI_BUFFERDESCRIPTOR_TYPE;
typedef struct { int version, numFds, numInts; int fd[2]; } IMG_native_handle_t;
typedef IMG_native_handle_t native_handle_t;
typedef struct {
    OMX_PTR pBufferHandle;
    OMX_U32 bufferFd;
    OMX_PTR pRegBufferHandle;
    OMX_PTR pBufferMappedAddress;
} MEMPLUGIN_BUFFER_ACCESSOR;
typedef struct {
    OMX_U32 nHeight, nWidth;
    OMX_BOOL bMap;
    int eTiler_format, eBuffer_type;
} MEMPLUGIN_BUFFER_PARAMS;
typedef struct {
    MEMPLUGIN_BUFFER_ACCESSOR sBuffer_accessor;
    OMX_U32 nStride;
} MEMPLUGIN_BUFFER_PROPERTIES;
typedef struct {
    OMX_BUFFERHEADERTYPE *pBufHeader;
    OMX_U32 pBufHeaderRemote;
    MEMPLUGIN_BUFFER_ACCESSOR bufferAccessors[3];
} PROXY_BUFFER_INFO;
typedef struct { PROXY_BUFFER_TYPE proxyBufferType; } PROXY_PORT_TYPE;
typedef struct {
    OMX_HANDLETYPE hRemoteComp;
    PROXY_BUFFER_INFO tBufList[MAX_NUM_PROXY_BUFFERS];
    PROXY_PORT_TYPE proxyPortBuffers[PROXY_MAXNUMOFPORTS];
    OMX_U32 nTotalBuffers, nAllocatedBuffers, nMemmgrClientDesc;
    OMX_BOOL bMapBuffers;
    OMX_PTR pMemPluginHandle;
} PROXY_COMPONENT_PRIVATE;
typedef struct { OMX_PTR pComponentPrivate; } OMX_COMPONENTTYPE;
typedef struct { int fd_omx; } RPC_OMX_CONTEXT;
struct ion_fd_data { ion_user_handle_t handle; int fd; };
struct omx_pvr_data { int fds[2]; unsigned int num_handles; void *handles[2]; };

struct import_record { unsigned int references; };
static struct import_record imported[3];
static unsigned int registration_calls, unregister_calls, remote_calls;
static unsigned int heap_allocations, heap_calls, plugin_allocations, plugin_calls;
static unsigned int failures, cases;
static int fail_registration, registration_errno, null_registration;
static int fail_heap, fail_plugin, fail_parameter, fail_remote, fail_component;
static int pvr_planes, pvr_missing_first, pvr_missing_second, same_plane;
static int second_descriptor, registration_second_descriptor;
static int metadata_enabled, invalid_release, fail_unregister, allocate_primary;
static unsigned int unregister_attempts;
static void *plugin_buffers[2];
static int plugin_buffer_freed[2];
static const char *case_name;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static OMX_PTR import_buffer(unsigned int slot)
{
    imported[slot].references++;
    return &imported[slot];
}

static int fake_ioctl(int descriptor, unsigned long request, void *argument)
{
    CHECK(descriptor == 99);
    if (request == OMX_IOCIONUNREGISTER) {
        struct ion_fd_data *data = argument;
        unsigned int index;
        unregister_attempts++;
        if ((int)unregister_attempts == fail_unregister) {
            errno = EIO;
            return -1;
        }
        for (index = 0; index < 3; index++) {
            if ((OMX_PTR)data->handle == &imported[index] && imported[index].references) {
                imported[index].references--;
                unregister_calls++;
                return 0;
            }
        }
        invalid_release++;
        errno = EINVAL;
        return -1;
    }
    registration_calls++;
    if ((int)registration_calls == fail_registration) {
        errno = registration_errno;
        return -1;
    }
    if (request == OMX_IOCIONREGISTER) {
        struct ion_fd_data *data = argument;
        unsigned int slot = data->fd == 12 ? 2 : (data->fd == 11 || data->fd == 0 ? 1 : 0);
        data->handle = (int)registration_calls == null_registration ? 0 :
            (ion_user_handle_t)import_buffer(slot);
        return 0;
    }
    CHECK(request == OMX_IOCPVRREGISTER);
    {
        struct omx_pvr_data *data = argument;
        registration_second_descriptor = data->fds[1];
        CHECK(data->fds[1] == (pvr_planes > 1 ? second_descriptor : -1));
        data->num_handles = 0;
        data->handles[0] = pvr_missing_first ? NULL : import_buffer(0);
        data->handles[1] = pvr_planes > 1 && !pvr_missing_second ?
            import_buffer(same_plane ? 0 : 1) : NULL;
        if (data->handles[0])
            data->num_handles = 1;
        if (data->handles[1])
            data->num_handles = 2;
    }
    return 0;
}
#define ioctl fake_ioctl

static OMX_PTR TIMM_OSAL_Malloc(size_t size, int contiguous, int alignment, int segment)
{
    (void)contiguous; (void)alignment; (void)segment;
    heap_calls++;
    if ((int)heap_calls == fail_heap)
        return NULL;
    heap_allocations++;
    return calloc(1, size);
}
static void TIMM_OSAL_Free(OMX_PTR allocation)
{
    CHECK(heap_allocations > 0);
    heap_allocations--;
    free(allocation);
}
static MEMPLUGIN_ERRORTYPE MemPlugin_Alloc(OMX_PTR plugin, OMX_U32 client,
    MEMPLUGIN_BUFFER_PARAMS *parameters, MEMPLUGIN_BUFFER_PROPERTIES *properties)
{
    unsigned int slot;
    (void)plugin; (void)client;
    plugin_calls++;
    if ((int)plugin_calls == fail_plugin)
        return MEMPLUGIN_ERROR_NORESOURCES;
    slot = plugin_calls - 1;
    CHECK(slot < 2);
    plugin_buffers[slot] = calloc(1, parameters->nWidth * parameters->nHeight);
    plugin_allocations++;
    memset(properties, 0, sizeof(*properties));
    properties->sBuffer_accessor.pBufferHandle = plugin_buffers[slot];
    properties->sBuffer_accessor.pBufferMappedAddress = plugin_buffers[slot];
    properties->sBuffer_accessor.bufferFd = allocate_primary && slot == 0 ? 10 : 12;
    return MEMPLUGIN_ERROR_NONE;
}
static MEMPLUGIN_ERRORTYPE MemPlugin_Free(OMX_PTR plugin, OMX_U32 client,
    MEMPLUGIN_BUFFER_PARAMS *parameters, MEMPLUGIN_BUFFER_PROPERTIES *properties)
{
    unsigned int slot;
    (void)plugin; (void)client; (void)parameters;
    for (slot = 0; slot < 2; slot++) {
        if (plugin_buffers[slot] == properties->sBuffer_accessor.pBufferHandle &&
            !plugin_buffer_freed[slot]) {
            plugin_buffer_freed[slot] = 1;
            plugin_allocations--;
            free(plugin_buffers[slot]);
            return MEMPLUGIN_ERROR_NONE;
        }
    }
    invalid_release++;
    return MEMPLUGIN_ERROR_NORESOURCES;
}
static OMX_ERRORTYPE PROXY_GetParameter(OMX_HANDLETYPE component, OMX_INDEXTYPE index, OMX_PTR argument)
{
    OMX_TI_PARAM_METADATABUFFERINFO *metadata = argument;
    (void)component;
    CHECK(index == OMX_TI_IndexParamMetaDataBufferInfo);
    if (fail_parameter)
        return OMX_ErrorHardware;
    metadata->bIsMetaDataEnabledOnPort = metadata_enabled;
    metadata->nMetaDataSize = 128;
    return OMX_ErrorNone;
}
static RPC_OMX_ERRORTYPE RPC_UseBuffer(OMX_HANDLETYPE context,
    OMX_BUFFERHEADERTYPE **header, OMX_U32 port, OMX_PTR application,
    OMX_U32 size, OMX_U8 *buffer, OMX_U32 *remote_header, OMX_ERRORTYPE *component_error)
{
    OMX_TI_PLATFORMPRIVATE *platform = (*header)->pPlatformPrivate;
    (void)context; (void)port; (void)application;
    remote_calls++;
    CHECK(buffer == (OMX_U8 *)&imported[0]);
    if (metadata_enabled)
        CHECK(platform->pMetaDataBuffer == &imported[2]);
    if (fail_remote)
        return RPC_OMX_ErrorHardware;
    *component_error = fail_component ? OMX_ErrorBadParameter : OMX_ErrorNone;
    (*header)->nSize = sizeof(**header);
    (*header)->nVersion.s.nVersionMajor = OMX_VER_MAJOR;
    (*header)->nVersion.s.nVersionMinor = OMX_VER_MINOR;
    (*header)->nAllocLen = size;
    *remote_header = 123;
    return RPC_OMX_ErrorNone;
}
static RPC_OMX_ERRORTYPE RPC_FreeBuffer(OMX_HANDLETYPE context, OMX_U32 port,
    OMX_U32 remote_header, OMX_U32 buffer, OMX_ERRORTYPE *component_error)
{
    (void)context; (void)port; (void)buffer;
    CHECK(remote_header == 123);
    *component_error = OMX_ErrorNone;
    return RPC_OMX_ErrorNone;
}
static OMX_ERRORTYPE PROXY_UseBuffer(OMX_HANDLETYPE component,
    OMX_BUFFERHEADERTYPE **header, OMX_U32 port, OMX_PTR application,
    OMX_U32 size, OMX_U8 *buffer);

#include "ownership_production.inc"

static void reset_case(const char *name)
{
    unsigned int slot;
    for (slot = 0; slot < 2; slot++) {
        if (plugin_buffers[slot] && !plugin_buffer_freed[slot])
            free(plugin_buffers[slot]);
    }
    memset(imported, 0, sizeof(imported));
    memset(plugin_buffers, 0, sizeof(plugin_buffers));
    memset(plugin_buffer_freed, 0, sizeof(plugin_buffer_freed));
    registration_calls = unregister_calls = remote_calls = 0;
    heap_allocations = heap_calls = plugin_allocations = plugin_calls = 0;
    fail_registration = null_registration = fail_heap = fail_plugin = 0;
    fail_parameter = fail_remote = fail_component = 0;
    pvr_missing_first = pvr_missing_second = same_plane = metadata_enabled = 0;
    invalid_release = fail_unregister = allocate_primary = 0;
    unregister_attempts = 0;
    registration_errno = EIO;
    pvr_planes = 2;
    second_descriptor = 11;
    registration_second_descriptor = -99;
    case_name = name;
    cases++;
}
static void assert_released(void)
{
    CHECK(imported[0].references == (fail_remote ? (same_plane ? 2u : 1u) : (fail_unregister ? 1u : 0u)));
    CHECK(imported[1].references == (fail_remote && pvr_planes > 1 && !same_plane && !allocate_primary ? 1u : 0u));
    CHECK(imported[2].references == (fail_remote && metadata_enabled ? 1u : 0u));
    CHECK(heap_allocations == 0);
    CHECK(plugin_allocations == (fail_remote && metadata_enabled ? 1u : 0u));
    CHECK(invalid_release == 0);
}
static void registration_case(PROXY_BUFFER_TYPE type, int expected_error)
{
    RPC_OMX_CONTEXT context = {99};
    OMX_PTR first = NULL, second = NULL;
    int result = RPC_RegisterBuffer(&context, 10, pvr_planes > 1 ? second_descriptor : -1, &first, &second, type);
    CHECK(result == expected_error);
    if (expected_error == RPC_OMX_ErrorNone) {
        CHECK(first == &imported[0]);
        CHECK(second == (pvr_planes == 1 ? NULL : &imported[1]));
        CHECK(RPC_UnRegisterBuffer(&context, first, second, type) == RPC_OMX_ErrorNone);
    } else {
        CHECK(first == NULL);
        CHECK(second == NULL);
    }
    assert_released();
}
static void use_case(PROXY_BUFFER_TYPE type, int expected_error, int allocate)
{
    RPC_OMX_CONTEXT context = {99};
    PROXY_COMPONENT_PRIVATE state = {0};
    OMX_COMPONENTTYPE component = {&state};
    IMG_native_handle_t native = {0, pvr_planes, 0, {10, second_descriptor}};
    OMX_TI_BUFFERDESCRIPTOR_TYPE descriptor = {{(void *)10, (void *)(uintptr_t)second_descriptor}};
    OMX_BUFFERHEADERTYPE *header = NULL;
    OMX_U8 *input = type == GrallocPointers ? (OMX_U8 *)&native :
        type == BufferDescriptorVirtual2D ? (OMX_U8 *)&descriptor : (OMX_U8 *)10;
    int result;
    unsigned int slot;
    state.hRemoteComp = &context;
    state.bMapBuffers = OMX_TRUE;
    state.proxyPortBuffers[0].proxyBufferType = type;
    allocate_primary = allocate;
    result = allocate ? PROXY_AllocateBuffer(&component, &header, 0, NULL, 4096) :
        PROXY_UseBuffer(&component, &header, 0, NULL, 4096, input);
    CHECK(result == expected_error);
    if (expected_error == OMX_ErrorNone) {
        CHECK(header != NULL);
        CHECK(state.nAllocatedBuffers == 1);
        CHECK(PROXY_FreeBuffer(&component, 0, header) == OMX_ErrorNone);
    } else {
        CHECK(header == NULL);
        CHECK(state.nAllocatedBuffers == 0);
        CHECK(state.nTotalBuffers == 0);
    }
    for (slot = 0; slot < 3; slot++) {
        CHECK(state.tBufList[0].bufferAccessors[slot].pRegBufferHandle == NULL);
        CHECK(state.tBufList[0].bufferAccessors[slot].pBufferHandle == NULL);
    }
    assert_released();
}

int main(void)
{
    reset_case("PVR ENOTTY"); fail_registration = 1; registration_errno = ENOTTY;
    registration_case(GrallocPointers, RPC_OMX_ErrorInsufficientResources);
    reset_case("PVR EIO"); fail_registration = 1;
    registration_case(GrallocPointers, RPC_OMX_ErrorInsufficientResources);
    reset_case("2D second ioctl failure"); fail_registration = 2;
    registration_case(BufferDescriptorVirtual2D, RPC_OMX_ErrorInsufficientResources);
    reset_case("2D second null handle"); null_registration = 2;
    registration_case(BufferDescriptorVirtual2D, RPC_OMX_ErrorBadParameter);
    reset_case("PVR missing first handle"); pvr_missing_first = 1;
    registration_case(GrallocPointers, RPC_OMX_ErrorBadParameter);
    reset_case("PVR requested UV import fails with kernel count one"); pvr_missing_second = 1;
    registration_case(GrallocPointers, RPC_OMX_ErrorBadParameter);
    reset_case("PVR one plane"); pvr_planes = 1;
    registration_case(GrallocPointers, RPC_OMX_ErrorNone);
    reset_case("PVR two planes"); registration_case(GrallocPointers, RPC_OMX_ErrorNone);
    reset_case("2D two planes"); registration_case(BufferDescriptorVirtual2D, RPC_OMX_ErrorNone);
    reset_case("header allocation failure"); fail_heap = 1;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    reset_case("platform allocation failure"); fail_heap = 2;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    reset_case("metadata parameter failure"); fail_parameter = 1;
    use_case(GrallocPointers, OMX_ErrorHardware, 0);
    reset_case("metadata allocation failure"); metadata_enabled = 1; fail_plugin = 1;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    reset_case("main registration failure after metadata allocation"); metadata_enabled = 1; fail_registration = 1;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    reset_case("metadata registration failure"); metadata_enabled = 1; fail_registration = 2;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    reset_case("RPC unknown acceptance retains imports"); fail_remote = 1;
    use_case(GrallocPointers, OMX_ErrorHardware, 0);
    reset_case("remote component rejection with metadata"); metadata_enabled = 1; fail_component = 1;
    use_case(GrallocPointers, OMX_ErrorBadParameter, 0);
    reset_case("shared plane references survive unknown acceptance"); same_plane = 1; fail_remote = 1;
    use_case(GrallocPointers, OMX_ErrorHardware, 0);
    reset_case("2D use second registration failure"); fail_registration = 2;
    use_case(BufferDescriptorVirtual2D, OMX_ErrorInsufficientResources, 0);
    reset_case("allocate owns primary buffer on use failure"); fail_remote = 1;
    use_case(IONPointers, OMX_ErrorHardware, 1);
    reset_case("use success one plane"); pvr_planes = 1;
    use_case(GrallocPointers, OMX_ErrorNone, 0);
    reset_case("use success two planes and metadata"); metadata_enabled = 1;
    use_case(GrallocPointers, OMX_ErrorNone, 0);
    reset_case("allocate success"); use_case(IONPointers, OMX_ErrorNone, 1);
    reset_case("allocate metadata rejection"); metadata_enabled = 1; fail_component = 1;
    use_case(IONPointers, OMX_ErrorBadParameter, 1);
    CHECK(plugin_calls == 2);
    reset_case("allocate metadata success"); metadata_enabled = 1;
    use_case(IONPointers, OMX_ErrorNone, 1);
    CHECK(plugin_calls == 2);
    reset_case("allocate metadata allocation failure"); metadata_enabled = 1; fail_plugin = 2;
    use_case(IONPointers, OMX_ErrorInsufficientResources, 1);
    CHECK(plugin_calls == 2);
    reset_case("PVR use ENOTTY"); fail_registration = 1; registration_errno = ENOTTY;
    use_case(GrallocPointers, OMX_ErrorInsufficientResources, 0);
    CHECK(remote_calls == 0);
    reset_case("rollback continues after unregister failure");
    metadata_enabled = 1; fail_component = 1; fail_unregister = 1;
    use_case(GrallocPointers, OMX_ErrorBadParameter, 0);
    CHECK(unregister_attempts == 3);
    reset_case("registration preserves failure when rollback fails");
    fail_registration = 2; fail_unregister = 1;
    registration_case(BufferDescriptorVirtual2D, RPC_OMX_ErrorInsufficientResources);
    CHECK(unregister_attempts == 1);
    reset_case("PVR requested descriptor zero succeeds"); second_descriptor = 0;
    registration_case(GrallocPointers, RPC_OMX_ErrorNone);
    CHECK(registration_second_descriptor == 0);
    reset_case("PVR use requested descriptor zero succeeds"); second_descriptor = 0;
    use_case(GrallocPointers, OMX_ErrorNone, 0);
    CHECK(registration_second_descriptor == 0);
    reset_case("PVR use missing UV releases Y and metadata");
    metadata_enabled = 1; pvr_missing_second = 1;
    use_case(GrallocPointers, OMX_ErrorBadParameter, 0);
    CHECK(remote_calls == 0);
    CHECK(unregister_attempts == 1);
    reset_case("RPC unknown acceptance retains metadata");
    metadata_enabled = 1; fail_remote = 1;
    use_case(GrallocPointers, OMX_ErrorHardware, 0);
    CHECK(unregister_attempts == 0);
    reset_case("2D use requested descriptor zero succeeds"); second_descriptor = 0;
    use_case(BufferDescriptorVirtual2D, OMX_ErrorNone, 0);
    printf("cases=%u failures=%u\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
