/* Production plugin allocation and release with deterministic ION API outcomes. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#define __unused __attribute__((unused))
#define OMX_TRUE 1
#define OMX_FALSE 0
#define DOMX_ERROR(...) ((void)0)
#define DOMX_DEBUG(...) ((void)0)
#define DOMX_ENTER(...) ((void)0)
#define DOMX_EXIT(...) ((void)0)
#define OMAP_ION_HEAP_SECURE_INPUT 1
#define OMAP_ION_HEAP_TILER_MASK 4
#define MEMPLUGIN_TILER_FORMAT_PAGE 3
#define MEMPLUGIN_ERROR_NONE 0
#define MEMPLUGIN_ERROR_NORESOURCES 1
#define MEMPLUGIN_ERROR_BADPARAMETER 2
#define MEMPLUGIN_ERROR_NOTIMPLEMENTED 3
#define MEMPLUGIN_ERROR_UNDEFINED 4
typedef int MEMPLUGIN_ERRORTYPE;
typedef int16_t OMX_S16;
typedef uint32_t OMX_U32;
typedef int32_t OMX_S32;
typedef void *OMX_PTR;
typedef int OMX_BOOL;
typedef uintptr_t ion_user_handle_t;
enum { DEFAULT, TILER1D, TILER2D };
struct ion_handle { int live; };
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
typedef struct { OMX_PTR pPluginExtendedInfo; } MEMPLUGIN_OBJECT;
typedef struct {
    size_t nAlign;
    off_t nOffset;
    OMX_U32 alloc_flags;
    OMX_S32 map_flags, prot;
} MEMPLUGIN_ION_PARAMS;
static struct ion_handle allocation;
static char mapped_bytes[4096];
static int allocation_fails, mapping_fails, sharing_fails;
static int live_fds, live_maps, free_calls, invalid_release, failures, cases;
static const char *case_name;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #condition); failures++; \
} } while (0)
int ion_alloc(int client, size_t size, size_t alignment, unsigned int heap,
    unsigned int flags, ion_user_handle_t *handle)
{
    (void)client; (void)size; (void)alignment; (void)heap; (void)flags;
    if (allocation_fails)
        return -ENOMEM;
    allocation.live++;
    *handle = (ion_user_handle_t)&allocation;
    return 0;
}
int ion_alloc_tiler(int client, size_t width, size_t height, int format,
    unsigned int flags, struct ion_handle **handle, size_t *stride)
{
    (void)height; (void)format; (void)stride;
    return ion_alloc(client, width, 0, flags, 0, (ion_user_handle_t *)handle);
}
int ion_map(int client, ion_user_handle_t handle, size_t length, int protection,
    int flags, off_t offset, unsigned char **address, int *descriptor)
{
    (void)client; (void)length; (void)protection; (void)flags; (void)offset;
    CHECK(handle == (ion_user_handle_t)&allocation);
    if (mapping_fails)
        return -ENOMEM;
    *address = (unsigned char *)mapped_bytes;
    *descriptor = 77;
    live_fds++;
    live_maps++;
    return 0;
}
int ion_share(int client, ion_user_handle_t handle, int *descriptor)
{
    (void)client;
    CHECK(handle == (ion_user_handle_t)&allocation);
    if (sharing_fails)
        return -EMFILE;
    *descriptor = 77;
    live_fds++;
    return 0;
}
int ion_free(int client, ion_user_handle_t handle)
{
    (void)client;
    if (handle != (ion_user_handle_t)&allocation || !allocation.live)
        invalid_release++;
    else
        allocation.live--;
    free_calls++;
    return 0;
}
static int fake_close(int descriptor)
{
    CHECK(descriptor == 77);
    CHECK(live_fds == 1);
    live_fds--;
    return 0;
}
static int fake_munmap(void *address, size_t length)
{
    CHECK(address == mapped_bytes);
    CHECK(length == sizeof(mapped_bytes));
    CHECK(live_maps == 1);
    live_maps--;
    return 0;
}
#define close fake_close
#define munmap fake_munmap
#include "plugin_production.inc"
static void run_case(const char *name, int mapped, int type, int fail_alloc,
    int fail_map, int fail_share, int expected_error)
{
    MEMPLUGIN_OBJECT plugin = {0};
    MEMPLUGIN_BUFFER_PARAMS parameters = {1, 4096, mapped, 0, type};
    MEMPLUGIN_BUFFER_PROPERTIES properties = {0};
    int result;
    case_name = name;
    cases++;
    allocation.live = live_maps = live_fds = free_calls = invalid_release = 0;
    allocation_fails = fail_alloc;
    mapping_fails = fail_map;
    sharing_fails = fail_share;
    result = MemPlugin_ION_Alloc(&plugin, 5, &parameters, &properties);
    CHECK(result == expected_error);
    if (!expected_error) {
        CHECK(allocation.live == 1);
        CHECK(live_fds == 1);
        CHECK(MemPlugin_ION_Free(&plugin, 5, &parameters, &properties) == MEMPLUGIN_ERROR_NONE);
    } else {
        CHECK(properties.sBuffer_accessor.pBufferHandle == NULL);
        CHECK(properties.sBuffer_accessor.pBufferMappedAddress == NULL);
    }
    CHECK(allocation.live == 0);
    CHECK(live_fds == 0);
    CHECK(live_maps == 0);
    CHECK(invalid_release == 0);
    CHECK(free_calls == (fail_alloc ? 0 : 1));
}
int main(void)
{
    run_case("ION allocation failure", 1, DEFAULT, 1, 0, 0, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("tiler allocation failure", 1, TILER1D, 1, 0, 0, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("ION map failure", 1, DEFAULT, 0, 1, 0, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("tiler map failure", 1, TILER1D, 0, 1, 0, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("ION share failure", 0, DEFAULT, 0, 0, 1, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("tiler share failure", 0, TILER1D, 0, 0, 1, MEMPLUGIN_ERROR_NORESOURCES);
    run_case("ION mapped success", 1, DEFAULT, 0, 0, 0, MEMPLUGIN_ERROR_NONE);
    run_case("ION shared success", 0, DEFAULT, 0, 0, 0, MEMPLUGIN_ERROR_NONE);
    printf("plugin_cases=%d failures=%d\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
