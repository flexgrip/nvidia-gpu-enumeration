# libnvenc_fix.so | Fixing NVIDIA's Broken GPU Encoding in Containers
Fixing NVIDIA's Broken GPU Encoding in Containers using LD_PRELOAD. Nvenc/cuvid solution for "OpenEncodeSessionEx", "unsupported device", and "No capable devices found" inside containers.

## The Problem

If you run GPU-accelerated video encoding inside a Docker container on a machine with multiple GPUs, there's a good chance it doesn't work. The encoder crashes with a cryptic error — "unsupported device" — even though your GPU is perfectly fine and the exact same command works on the host machine.

This isn't a configuration mistake. It's a bug in NVIDIA's driver, introduced in the 570.x series, that breaks NVENC (NVIDIA's hardware video encoder) and NVDEC (the decoder) inside containers on any multi-GPU system. It affects everyone: Kubernetes pods, Docker containers, LXC, serverless GPU platforms like RunPod — anywhere a container is assigned a subset of the host's GPUs.

The error looks like this:

```
[h264_nvenc @ 0x55de29791c00] OpenEncodeSessionEx failed: unsupported device (2): (no details)
[h264_nvenc @ 0x55de29791c00] No capable devices found
```

Your GPU is right there. `nvidia-smi` can see it. CUDA compute tasks work. But the moment you try to encode or decode video, it fails.

## Who's Affected

This isn't just an FFmpeg problem. The bug lives in NVIDIA's user-space driver libraries (`libnvidia-encode.so` and `libnvcuvid.so`), which means any application that uses NVENC or NVDEC is affected: FFmpeg, GStreamer, OBS, custom applications using the NVIDIA Video Codec SDK, PyTorch video processing pipelines — anything that touches hardware video encoding or decoding inside a container.

The scope of the damage is visible across multiple open issue trackers:

- **NVIDIA Container Toolkit** [#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) — NVENC fails in Kubernetes pods on all but the last GPU with driver 570.x or 580.x.
- **NVIDIA Container Toolkit** [#1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209) — Multi-GPU NVDEC errors with `NVIDIA_VISIBLE_DEVICES` and FFmpeg 7.
- **NVIDIA Container Toolkit** [#1197](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1197) — Passing individual GPUs to a container doesn't work, but passing all does. Even PyTorch's `torch.cuda.is_available()` returns `False`.
- **NVIDIA k8s Device Plugin** [#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282) — FFmpeg NVENC fails in pods unless `/dev/nvidia#` index matches the GPU's `nvidia-smi` index.
- **FFmpeg Bug Tracker** [#11694](https://trac.ffmpeg.org/ticket/11694) — NVENC unsupported device in container environments.
- **NVIDIA Developer Forums** [thread](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361) — Consolidated report confirming the issue across multiple driver versions >= 565.

Users across these threads report the same pattern: downgrading to driver 550.x fixes it. The host machine encodes fine on every GPU. It's only containers that break.

## The Root Cause

A contributor to the Container Toolkit issue (#1249) reverse-engineered the exact failure path inside NVIDIA's driver. Here's what happens:

1. When NVENC initializes, it makes a call through `/dev/nvidiactl` to the NVIDIA Resource Manager in the kernel, asking for the list of all attached GPUs. This is RM command `0x0201` (`NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS`), and it returns an array of internal GPU identifiers.

2. The kernel driver returns **every GPU on the host machine** — not just the ones assigned to the container. If the host has 8 GPUs and your container has 1, the driver says "there are 8 GPUs."

3. When the returned list contains multiple GPUs, NVENC takes a multi-GPU initialization path. It attempts to set up peer connections with every GPU in the list.

4. The peer-init step tries to open the device nodes for the other GPUs (`/dev/nvidia1`, `/dev/nvidia2`, etc.), but those nodes don't exist inside the container — only `/dev/nvidia0` (or whichever single device was mounted) is present.

5. The peer-init fails, and NVENC bails out entirely, returning `NV_ENC_ERR_UNSUPPORTED_DEVICE` — even though the one GPU it actually has access to is perfectly capable.

The critical detail: this is a regression. In driver 550.x and earlier, the NVENC initialization code apparently didn't take this multi-GPU peer-init path, or it handled failures more gracefully. Something changed in 570.x that made NVENC unable to tolerate the presence of other GPUs it can't reach.

## The Fix: Intercepting the Driver at the Library Level

Since the bug is in NVIDIA's proprietary user-space library and we can't modify it, we need to intercept the conversation between the library and the kernel driver, and correct the kernel's response before the library sees it.

Linux provides a mechanism for this: `LD_PRELOAD`. By setting this environment variable to point at a custom shared library, we can instruct the dynamic linker to load our library before any other — including libc. If our library exports a function with the same name as a libc function (like `ioctl()`), our version gets called instead. We can then forward the call to the real function, inspect the result, modify it if needed, and return the modified version to the caller.

The approach:

1. **Wrap `ioctl()`** — every ioctl call from NVENC flows through our function first.
2. **Forward to the real kernel** — we call the real `ioctl()` via `dlsym(RTLD_NEXT, "ioctl")` and let the kernel do its thing.
3. **Detect the target call** — after the real ioctl returns, we check if it was `NV_ESC_RM_CONTROL` (escape code `0x2A`) with command `NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS` (`0x0201`).
4. **Identify our GPU** — we need to figure out which GPU ID in the returned array corresponds to the GPU(s) actually assigned to our container.
5. **Rewrite the response** — we remove every GPU ID that doesn't belong to us, compact the array, and fill the rest with `0xFFFFFFFF` (the "invalid" sentinel).
6. **Return to NVENC** — the library now sees a single-GPU system, takes the single-GPU init path, and works.

The tricky part is step 4: identifying which GPU is ours. The `gpuIds[]` array contains internal Resource Manager handles (like `0x11800`, `0x500`), not CUDA device indices or UUIDs. We tried using another RM ioctl (`NV0000_CTRL_CMD_GPU_GET_ID_INFO`, command `0x0202`) to map these handles to device instance numbers, but inside a container this call fails with status `0x1f` (`NV_ERR_OBJECT_NOT_FOUND`).

### The /proc Matching Strategy

What does work is `/proc/driver/nvidia/gpus/`. Even inside a container, this directory contains an entry for every GPU on the host, organized by PCI bus address. Each entry has an `information` file that includes the GPU's **Device Minor** number — the `N` in `/dev/nvidiaN`.

By parsing these files, we can build a map: PCI bus address → Device Minor. Then we extract the PCI bus address encoded in each gpuId (the bus number appears at `gpuId >> 8`), look it up in our map, get the Device Minor, and check whether that `/dev/nvidiaN` device node exists in the container.

For example, on an 8-GPU host where our container has `/dev/nvidia0`:

```
/proc/driver/nvidia/gpus/0001:18:00.0/information → Device Minor: 0
/proc/driver/nvidia/gpus/0001:1f:00.0/information → Device Minor: 4
...

gpuId 0x11800 → bus 0x18 → Minor 0 → /dev/nvidia0 exists → KEEP
gpuId 0x11f00 → bus 0x1f → Minor 4 → /dev/nvidia4 missing → REMOVE
```

The container gets a corrected single-GPU view. NVENC initializes cleanly.

## Implementation

The fix is a single C file, compiled into a shared library:

```bash
gcc -shared -fPIC -O2 -o libnvenc_fix.so nvenc_fix.c -ldl
```

Integrated into a Dockerfile:

```dockerfile
COPY nvenc_fix.c /opt/nvenc_fix.c
RUN gcc -shared -fPIC -O2 -o /opt/libnvenc_fix.so /opt/nvenc_fix.c -ldl
ENV LD_PRELOAD=/opt/libnvenc_fix.so
```

That's all. No code changes to FFmpeg, no changes to the application, no kernel modules, no elevated privileges. The fix is invisible to everything above the ioctl layer.

Debug logging can be enabled via environment variable:

```bash
# Log to stderr
NVENC_FIX_DEBUG=1

# Log to a file
NVENC_FIX_DEBUG=/tmp/nvenc_fix.log
```

Which produces output like:

```
[nvenc_fix] GET_ATTACHED_IDS returned 8 GPUs from host
[nvenc_fix] available device node bitmask: 0x00000004
[nvenc_fix] all GET_ID_INFO calls failed, trying /proc-based matching
[nvenc_fix] gpuId 0x500: bus 0x05 matches 0000:05:00.0 -> minor 2
[nvenc_fix] KEEPING gpuId 0x500 (minor 2 via /proc)
[nvenc_fix] filtered: 8 -> 1 GPUs
```

## The Code

```c
/*
 * nvenc_fix.c - LD_PRELOAD interposer to fix NVENC in multi-GPU containers
 *
 * Problem: On NVIDIA driver >= 570, when NVENC initializes inside a container
 * that has only 1 GPU assigned, libnvidia-encode queries /dev/nvidiactl for
 * ALL host GPUs (NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS, cmd 0x2A). When it
 * sees multiple GPUs, it tries to peer-init with the others, fails because
 * their /dev/nvidiaX nodes aren't mounted, and returns NV_ENC_ERR_UNSUPPORTED_DEVICE.
 *
 * Fix: Intercept the ioctl() call, let it pass through to the real kernel driver,
 * then post-process the GET_ATTACHED_IDS response to only include GPUs whose
 * /dev/nvidiaX device nodes actually exist in this container.
 *
 * Build:
 *   gcc -shared -fPIC -o libnvenc_fix.so nvenc_fix.c -ldl
 *
 * Usage:
 *   LD_PRELOAD=/path/to/libnvenc_fix.so ffmpeg -hwaccel cuda ...
 *
 * Or in Dockerfile:
 *   ENV LD_PRELOAD=/opt/libnvenc_fix.so
 *
 * Logging (NVENC_FIX_DEBUG environment variable):
 *   unset or empty                      -> no logging (production default)
 *   NVENC_FIX_DEBUG=1                   -> log to stderr
 *   NVENC_FIX_DEBUG=stderr              -> log to stderr
 *   NVENC_FIX_DEBUG=/tmp/nvenc_fix.log  -> log to file (appended)
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

/* ============================================================
 * NVIDIA RM ioctl structures (from open-gpu-kernel-modules)
 * ============================================================ */

typedef uint32_t NvV32;
typedef uint32_t NvU32;
typedef NvU32    NvHandle;
typedef void*    NvP64;

#define NV_ALIGN_BYTES(size) __attribute__((aligned(size)))

#define NV_IOCTL_MAGIC 'F'
#define NV_ESC_RM_CONTROL 0x2A

typedef struct {
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;

#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS 0x0201
#define NV0000_CTRL_GPU_MAX_ATTACHED_GPUS    32
#define NV0000_CTRL_GPU_INVALID_ID           0xFFFFFFFF

typedef struct {
    NvU32 gpuIds[NV0000_CTRL_GPU_MAX_ATTACHED_GPUS];
} NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS;

#define NV0000_CTRL_CMD_GPU_GET_ID_INFO 0x0202

typedef struct {
    NvU32 gpuId;
    NvU32 gpuFlags;
    NvU32 deviceInstance;
    NvU32 subDeviceInstance;
    NvU32 boardId;
    NvU32 szName;
    NvU32 sliStatus;
    NvU32 numaId;
} NV0000_CTRL_GPU_GET_ID_INFO_PARAMS;

/* ============================================================
 * Logging
 * ============================================================ */

static int   log_initialized = 0;
static int   log_enabled     = 0;
static FILE *log_file        = NULL;

static void log_init(void) {
    if (log_initialized) return;
    log_initialized = 1;
    const char *val = getenv("NVENC_FIX_DEBUG");
    if (!val || val[0] == '\0') { log_enabled = 0; return; }
    log_enabled = 1;
    if (strcmp(val, "1") == 0 || strcmp(val, "stderr") == 0) {
        log_file = stderr;
    } else {
        log_file = fopen(val, "a");
        if (!log_file) { log_file = stderr; }
    }
}

static void log_msg(const char *fmt, ...) {
    log_init();
    if (!log_enabled) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(log_file, "[nvenc_fix] ");
    vfprintf(log_file, fmt, ap);
    fprintf(log_file, "\n");
    fflush(log_file);
    va_end(ap);
}

/* ============================================================
 * Real ioctl
 * ============================================================ */

typedef int (*ioctl_fn_t)(int fd, unsigned long request, ...);
static ioctl_fn_t real_ioctl = NULL;

static void ensure_real_ioctl(void) {
    if (!real_ioctl) {
        real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl) { fprintf(stderr, "[nvenc_fix] FATAL: cannot find real ioctl\n"); _exit(1); }
    }
}

/* ============================================================
 * Device node helpers
 * ============================================================ */

static int device_node_exists(NvU32 device_instance) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/nvidia%u", device_instance);
    return (access(path, F_OK) == 0);
}

static uint32_t get_available_devices(void) {
    uint32_t mask = 0;
    for (int i = 0; i < 32; i++)
        if (device_node_exists(i)) mask |= (1u << i);
    return mask;
}

/* ============================================================
 * Strategy 1: ioctl-based GPU ID resolution
 * ============================================================ */

static NvU32 resolve_gpu_id_to_device(int fd, NvHandle hClient, NvU32 gpuId) {
    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS id_info;
    memset(&id_info, 0, sizeof(id_info));
    id_info.gpuId = gpuId;
    NVOS54_PARAMETERS ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.hClient    = hClient;
    ctrl.hObject    = hClient;
    ctrl.cmd        = NV0000_CTRL_CMD_GPU_GET_ID_INFO;
    ctrl.params     = &id_info;
    ctrl.paramsSize = sizeof(id_info);
    unsigned long req = _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, sizeof(NVOS54_PARAMETERS));
    int ret = real_ioctl(fd, req, &ctrl);
    if (ret != 0 || ctrl.status != 0) {
        log_msg("GET_ID_INFO failed for gpuId 0x%x: ioctl=%d status=0x%x", gpuId, ret, ctrl.status);
        return (NvU32)-1;
    }
    log_msg("gpuId 0x%x -> deviceInstance %u", gpuId, id_info.deviceInstance);
    return id_info.deviceInstance;
}

/* ============================================================
 * Strategy 2: /proc-based GPU matching
 * ============================================================ */

#define MAX_GPU_MAP 32

typedef struct {
    unsigned int domain, bus, slot, func;
    int device_minor;
} gpu_proc_entry_t;

static int gpu_map_count = 0;
static gpu_proc_entry_t gpu_map[MAX_GPU_MAP];
static int gpu_map_loaded = 0;

static void load_gpu_map(void) {
    if (gpu_map_loaded) return;
    gpu_map_loaded = 1;
    gpu_map_count = 0;
    DIR *dir = opendir("/proc/driver/nvidia/gpus");
    if (!dir) { log_msg("WARNING: cannot open /proc/driver/nvidia/gpus"); return; }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && gpu_map_count < MAX_GPU_MAP) {
        if (ent->d_name[0] == '.') continue;
        unsigned int domain, bus, slot, func;
        if (sscanf(ent->d_name, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4) continue;
        char info_path[512];
        snprintf(info_path, sizeof(info_path), "/proc/driver/nvidia/gpus/%s/information", ent->d_name);
        FILE *f = fopen(info_path, "r");
        if (!f) continue;
        int dev_minor = -1;
        char line[256];
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "Device Minor: %d", &dev_minor) == 1) break;
        fclose(f);
        if (dev_minor < 0) continue;
        gpu_map[gpu_map_count].domain = domain;
        gpu_map[gpu_map_count].bus = bus;
        gpu_map[gpu_map_count].slot = slot;
        gpu_map[gpu_map_count].func = func;
        gpu_map[gpu_map_count].device_minor = dev_minor;
        gpu_map_count++;
        log_msg("proc map: %04x:%02x:%02x.%x -> Device Minor %d", domain, bus, slot, func, dev_minor);
    }
    closedir(dir);
    log_msg("loaded %d GPU entries from /proc", gpu_map_count);
}

static int match_gpuid_to_minor(NvU32 gpuId) {
    load_gpu_map();
    unsigned int extracted_bus = (gpuId >> 8) & 0xFF;
    unsigned int extracted_full = gpuId >> 8;
    for (int i = 0; i < gpu_map_count; i++) {
        if (gpu_map[i].bus == extracted_bus) {
            log_msg("gpuId 0x%x: bus 0x%02x matches %04x:%02x:%02x.%x -> minor %d",
                    gpuId, extracted_bus, gpu_map[i].domain, gpu_map[i].bus,
                    gpu_map[i].slot, gpu_map[i].func, gpu_map[i].device_minor);
            return gpu_map[i].device_minor;
        }
        unsigned int combined = (gpu_map[i].domain << 8) | gpu_map[i].bus;
        if (combined == extracted_full) {
            log_msg("gpuId 0x%x: domain:bus 0x%x matches %04x:%02x:%02x.%x -> minor %d",
                    gpuId, extracted_full, gpu_map[i].domain, gpu_map[i].bus,
                    gpu_map[i].slot, gpu_map[i].func, gpu_map[i].device_minor);
            return gpu_map[i].device_minor;
        }
    }
    log_msg("gpuId 0x%x: no /proc match found (bus=0x%02x, full=0x%x)", gpuId, extracted_bus, extracted_full);
    return -1;
}

/* ============================================================
 * Main ioctl interposer
 * ============================================================ */

int ioctl(int fd, unsigned long request, ...) {
    ensure_real_ioctl();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    int ret = real_ioctl(fd, request, arg);
    if (ret != 0) return ret;
    if (_IOC_NR(request) != NV_ESC_RM_CONTROL) return ret;

    NVOS54_PARAMETERS *ctrl = (NVOS54_PARAMETERS *)arg;
    if (!ctrl || !ctrl->params) return ret;
    if (ctrl->cmd != NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS) return ret;
    if (ctrl->status != 0) return ret;

    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS *gpu_params =
        (NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS *)ctrl->params;

    int total_host_gpus = 0;
    for (int i = 0; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++) {
        if (gpu_params->gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID) break;
        total_host_gpus++;
    }
    log_msg("GET_ATTACHED_IDS returned %d GPUs from host", total_host_gpus);
    if (total_host_gpus <= 1) return ret;

    uint32_t available = get_available_devices();
    log_msg("available device node bitmask: 0x%08x", available);
    if (available == 0) { log_msg("WARNING: no /dev/nvidiaX nodes found, not filtering"); return ret; }

    NvU32 filtered[NV0000_CTRL_GPU_MAX_ATTACHED_GPUS];
    int filtered_count = 0, resolve_failures = 0;

    /* Strategy 1: ioctl-based resolve */
    for (int i = 0; i < total_host_gpus; i++) {
        NvU32 dev_inst = resolve_gpu_id_to_device(fd, ctrl->hClient, gpu_params->gpuIds[i]);
        if (dev_inst == (NvU32)-1) { resolve_failures++; continue; }
        if (dev_inst < 32 && (available & (1u << dev_inst))) {
            log_msg("KEEPING gpuId 0x%x (deviceInstance %u via ioctl)", gpu_params->gpuIds[i], dev_inst);
            filtered[filtered_count++] = gpu_params->gpuIds[i];
        } else {
            log_msg("REMOVING gpuId 0x%x (deviceInstance %u - not in container)", gpu_params->gpuIds[i], dev_inst);
        }
    }

    /* Strategy 2: /proc-based matching */
    if (resolve_failures == total_host_gpus) {
        log_msg("all GET_ID_INFO calls failed, trying /proc-based matching");
        filtered_count = 0;
        for (int i = 0; i < total_host_gpus; i++) {
            int dev_minor = match_gpuid_to_minor(gpu_params->gpuIds[i]);
            if (dev_minor >= 0 && dev_minor < 32 && (available & (1u << dev_minor))) {
                log_msg("KEEPING gpuId 0x%x (minor %d via /proc)", gpu_params->gpuIds[i], dev_minor);
                filtered[filtered_count++] = gpu_params->gpuIds[i];
            } else if (dev_minor >= 0) {
                log_msg("REMOVING gpuId 0x%x (minor %d - not in container)", gpu_params->gpuIds[i], dev_minor);
            }
        }
    }

    if (filtered_count == 0) {
        log_msg("WARNING: could not determine correct GPU, not filtering (NVENC may fail)");
        return ret;
    }

    /* Write back the filtered list */
    for (int i = 0; i < filtered_count; i++)
        gpu_params->gpuIds[i] = filtered[i];
    for (int i = filtered_count; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++)
        gpu_params->gpuIds[i] = NV0000_CTRL_GPU_INVALID_ID;
    log_msg("filtered: %d -> %d GPUs", total_host_gpus, filtered_count);

    return ret;
}
```

## What Should Be Fixed Upstream

### NVIDIA (driver)

This is fundamentally NVIDIA's bug to fix. The NVENC initialization code in `libnvidia-encode.so` should not attempt to peer-init with GPUs that the process cannot access. The `NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS` ioctl should either respect container visibility boundaries (only returning GPUs whose device nodes are accessible), or the NVENC multi-GPU init path should gracefully handle unreachable peers instead of aborting entirely. The regression was introduced in the 570.x driver series and persists through 580.x.

### NVIDIA Container Toolkit

The Container Toolkit could filter the RM's GPU enumeration at the cgroup/namespace level before it reaches user-space libraries. The toolkit already manages device node mounting and `NVIDIA_VISIBLE_DEVICES` — extending that isolation to the RM's internal GPU list would be a natural fit. Alternatively, the toolkit could ship an LD_PRELOAD fix like this one as a standard component.

### FFmpeg

FFmpeg itself isn't at fault here — it calls the NVENC API correctly and the failure happens deep inside NVIDIA's library. However, FFmpeg could potentially improve error reporting by detecting the multi-GPU container mismatch scenario and printing a more helpful error message pointing users toward the driver bug.

## A Note on Container Isolation

The fix relies on reading `/proc/driver/nvidia/gpus/*/information` to discover the PCI bus addresses and Device Minor numbers of **every GPU on the host** — from inside an isolated container. This is worth highlighting.

A container that is assigned a single GPU can enumerate every GPU in the host machine, including their PCI bus addresses, UUIDs, VBIOS versions, and firmware versions. This information leaks the host's hardware topology to any container running on it. While this alone doesn't grant access to those GPUs, it does violate the principle that a container should only be aware of resources assigned to it.

This isn't a novel observation. Security researchers at Wiz have documented critical container escape vulnerabilities in the NVIDIA Container Toolkit (CVE-2024-0132, CVE-2025-23266), and the broader security community has noted that GPU containers get direct device file access to the host kernel's GPU driver — with the "isolation" consisting of cgroup device permissions and namespace boundaries. The `/proc/driver/nvidia/gpus/` information leak is a relatively minor symptom of a much larger architectural reality: GPU container isolation is fundamentally weaker than CPU container isolation.

For our purposes, this weak isolation is what makes the fix possible. But it also means that in a true multi-tenant environment, a containerized workload can fingerprint the host's GPU hardware, which may be undesirable.

## References

- [NVIDIA Container Toolkit #1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) — Primary issue with root cause analysis and kernel hook proposal
- [NVIDIA Container Toolkit #1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209) — Multi-GPU NVDEC failures with NVIDIA_VISIBLE_DEVICES
- [NVIDIA Container Toolkit #1197](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1197) — Individual GPU passthrough broken on driver 570.x
- [NVIDIA k8s Device Plugin #1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282) — NVENC fails unless device index matches nvidia-smi index
- [NVIDIA Open GPU Kernel Modules Discussion #336](https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/336) — Mapping PCI bus IDs to /dev/nvidiaN via Device Minor
- [NVIDIA Developer Forums](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361) — Consolidated multi-GPU NVENC/NVDEC failure report
- [FFmpeg Trac #11694](https://trac.ffmpeg.org/ticket/11694) — FFmpeg NVENC unsupported device in containers
