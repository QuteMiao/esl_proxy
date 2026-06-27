/*
 * AICPU Dispatcher — transient bootstrap-only upload helper.
 */
#include "tools.h"
#include "onboard_log.h"

#include <stddef.h>
#include <stdio.h>

struct DispatcherKernelArgs {
    uint64_t unused[5];
    void *device_args;
    void *runtime_args;
    uint64_t regs;
};

struct DispatcherDeviceArgs {
    uint64_t unused[12];
    uint64_t aicpu_so_bin;
    uint64_t aicpu_so_len;
    uint64_t device_id;
    uint64_t inner_so_bin;
    uint64_t inner_so_len;
};

_Static_assert(offsetof(struct DispatcherKernelArgs, device_args) == 40, "device_args offset drift");
_Static_assert(offsetof(struct DispatcherDeviceArgs, aicpu_so_bin) == 96, "aicpu_so_bin offset drift");
_Static_assert(offsetof(struct DispatcherDeviceArgs, aicpu_so_len) == 104, "aicpu_so_len offset drift");
_Static_assert(offsetof(struct DispatcherDeviceArgs, device_id) == 112, "device_id offset drift");
_Static_assert(offsetof(struct DispatcherDeviceArgs, inner_so_bin) == 120, "inner_so_bin offset drift");
_Static_assert(offsetof(struct DispatcherDeviceArgs, inner_so_len) == 128, "inner_so_len offset drift");

static void make_inner_so_path(uint64_t fp, uint64_t device_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size,
        "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/" ESL_INNER_SO_BASENAME_FMT,
        (unsigned long)fp, (int)device_id);
}

int StaticTileFwkBackendKernelServer(void *args)
{
    (void)args;
    onboard_dispatcher_log("Static: stub (not expected to be called)");
    return 0;
}

uint32_t DynTileFwkBackendKernelServer(void *args)
{
    (void)args;
    onboard_dispatcher_log("Server: stub (dispatcher is upload-only, not expected to be called)");
    return 0;
}

uint32_t DynTileFwkBackendKernelServerInit(void *args)
{
    struct DispatcherKernelArgs *k;
    struct DispatcherDeviceArgs *d;
    const char *inner_bytes;
    uint64_t fp;
    char path[256];

    if (args == NULL) {
        onboard_dispatcher_log("Init: args==nullptr");
        return 1;
    }
    k = (struct DispatcherKernelArgs *)args;
    d = (struct DispatcherDeviceArgs *)k->device_args;
    if (d == NULL) {
        onboard_dispatcher_log("Init: device_args==nullptr");
        return 1;
    }
    if (d->inner_so_bin == 0 || d->inner_so_len == 0) {
        onboard_dispatcher_log("Init: empty inner SO bundle (bin=%lx len=%lu", (unsigned long)d->inner_so_bin,
                               (unsigned long)d->inner_so_len);
        return 1;
    }
    inner_bytes = (const char *)(uintptr_t)d->inner_so_bin;
    fp = esl_fingerprint_bytes(inner_bytes, (size_t)d->inner_so_len);
    make_inner_so_path(fp, d->device_id, path, sizeof(path));
    if (!write_bytes(path, inner_bytes, d->inner_so_len)) {
        return 1;
    }
    onboard_dispatcher_log("Init: wrote %s (%lu bytes)", path, (unsigned long)d->inner_so_len);
    return 0;
}
