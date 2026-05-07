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
 *   unset or empty                      → no logging (production default)
 *   NVENC_FIX_DEBUG=1                   → log to stderr
 *   NVENC_FIX_DEBUG=stderr              → log to stderr
 *   NVENC_FIX_DEBUG=/tmp/nvenc_fix.log  → log to file (appended)
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

/* NVOS54_PARAMETERS - the RM control ioctl parameter struct */
typedef struct {
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;

/* NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS = 0x0201
 * The gpuIds array is NV0000_CTRL_GPU_MAX_ATTACHED_GPUS = 32 entries */
#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS 0x0201
#define NV0000_CTRL_GPU_MAX_ATTACHED_GPUS    32
#define NV0000_CTRL_GPU_INVALID_ID           0xFFFFFFFF

typedef struct {
    NvU32 gpuIds[NV0000_CTRL_GPU_MAX_ATTACHED_GPUS];
} NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS;

/* NV0000_CTRL_CMD_GPU_GET_ID_INFO = 0x0202
 * Used to map a gpuId to its device instance (i.e. the X in /dev/nvidiaX) */
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO 0x0202

typedef struct {
    NvU32 gpuId;
    NvU32 gpuFlags;
    NvU32 deviceInstance;
    NvU32 subDeviceInstance;
    NvU32 boardId;          /* may not exist in all versions, but padding is safe */
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

/*
 * NVENC_FIX_DEBUG controls logging:
 *   unset / empty       → logging disabled
 *   "1" or "stderr"     → log to stderr
 *   "/some/path.log"    → log to that file (appended)
 */
static void log_init(void) {
    if (log_initialized)
        return;
    log_initialized = 1;

    const char *val = getenv("NVENC_FIX_DEBUG");
    if (!val || val[0] == '\0') {
        log_enabled = 0;
        return;
    }

    log_enabled = 1;

    if (strcmp(val, "1") == 0 || strcmp(val, "stderr") == 0) {
        log_file = stderr;
    } else {
        log_file = fopen(val, "a");
        if (!log_file) {
            /* Fall back to stderr if the file can't be opened */
            log_file = stderr;
            fprintf(stderr, "[nvenc_fix] WARNING: could not open log file '%s', falling back to stderr\n", val);
        }
    }
}

static void log_msg(const char *fmt, ...) {
    log_init();
    if (!log_enabled)
        return;
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
        if (!real_ioctl) {
            fprintf(stderr, "[nvenc_fix] FATAL: cannot find real ioctl: %s\n", dlerror());
            _exit(1);
        }
    }
}

/* ============================================================
 * Check if /dev/nvidiaX exists in this container
 * ============================================================ */

static int device_node_exists(NvU32 device_instance) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/nvidia%u", device_instance);
    return (access(path, F_OK) == 0);
}

/* ============================================================
 * Determine which device instances are available.
 * We scan /dev/nvidia0 through /dev/nvidia31.
 * Returns a bitmask of available device instances.
 * ============================================================ */

static uint32_t get_available_devices(void) {
    uint32_t mask = 0;
    for (int i = 0; i < 32; i++) {
        if (device_node_exists(i))
            mask |= (1u << i);
    }
    return mask;
}

/* ============================================================
 * Try to resolve a gpuId to a device instance using
 * NV0000_CTRL_CMD_GPU_GET_ID_INFO (cmd 0x0202).
 *
 * We need the nvidiactl fd and a valid RM client handle.
 * We extract these from the original ioctl call.
 *
 * Returns: device instance on success, (NvU32)-1 on failure.
 * ============================================================ */

static NvU32 resolve_gpu_id_to_device(int fd, NvHandle hClient, NvU32 gpuId) {
    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS id_info;
    memset(&id_info, 0, sizeof(id_info));
    id_info.gpuId = gpuId;

    /* GET_ID_INFO is a client-level control: hObject must equal hClient */
    NVOS54_PARAMETERS ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.hClient    = hClient;
    ctrl.hObject    = hClient;  /* root client object, NOT the subdevice */
    ctrl.cmd        = NV0000_CTRL_CMD_GPU_GET_ID_INFO;
    ctrl.params     = &id_info;
    ctrl.paramsSize = sizeof(id_info);

    unsigned long req = _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, sizeof(NVOS54_PARAMETERS));
    int ret = real_ioctl(fd, req, &ctrl);
    if (ret != 0 || ctrl.status != 0) {
        log_msg("GET_ID_INFO failed for gpuId 0x%x: ioctl=%d status=0x%x", gpuId, ret, ctrl.status);
        return (NvU32)-1;
    }

    log_msg("gpuId 0x%x -> deviceInstance %u", gpuId, id_info.deviceInstance);
    return id_info.deviceInstance;
}

/* ============================================================
 * Parse /proc/driver/nvidia/gpus/ to build a mapping from
 * PCI bus location → Device Minor number.
 *
 * Each GPU has a directory like /proc/driver/nvidia/gpus/0001:18:00.0/
 * containing an "information" file with "Device Minor: N" and
 * "Bus Location: XXXX:XX:XX.X" fields.
 *
 * We use this to figure out which gpuId corresponds to the
 * /dev/nvidiaX nodes that exist in this container.
 * ============================================================ */

#include <dirent.h>

#define MAX_GPU_MAP 32

typedef struct {
    unsigned int domain;
    unsigned int bus;
    unsigned int slot;
    unsigned int func;
    int          device_minor;
} gpu_proc_entry_t;

static int gpu_map_count = 0;
static gpu_proc_entry_t gpu_map[MAX_GPU_MAP];
static int gpu_map_loaded = 0;

static void load_gpu_map(void) {
    if (gpu_map_loaded)
        return;
    gpu_map_loaded = 1;
    gpu_map_count = 0;

    DIR *dir = opendir("/proc/driver/nvidia/gpus");
    if (!dir) {
        log_msg("WARNING: cannot open /proc/driver/nvidia/gpus");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && gpu_map_count < MAX_GPU_MAP) {
        if (ent->d_name[0] == '.')
            continue;

        /* Parse PCI address from directory name: "0001:18:00.0" */
        unsigned int domain, bus, slot, func;
        if (sscanf(ent->d_name, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
            continue;

        /* Read the information file to get Device Minor */
        char info_path[512];
        snprintf(info_path, sizeof(info_path),
                 "/proc/driver/nvidia/gpus/%s/information", ent->d_name);

        FILE *f = fopen(info_path, "r");
        if (!f)
            continue;

        int dev_minor = -1;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "Device Minor: %d", &dev_minor) == 1)
                break;
        }
        fclose(f);

        if (dev_minor < 0)
            continue;

        gpu_map[gpu_map_count].domain       = domain;
        gpu_map[gpu_map_count].bus          = bus;
        gpu_map[gpu_map_count].slot         = slot;
        gpu_map[gpu_map_count].func         = func;
        gpu_map[gpu_map_count].device_minor = dev_minor;
        gpu_map_count++;

        log_msg("proc map: %04x:%02x:%02x.%x -> Device Minor %d",
                domain, bus, slot, func, dev_minor);
    }
    closedir(dir);

    log_msg("loaded %d GPU entries from /proc", gpu_map_count);
}

/*
 * Given a gpuId from GET_ATTACHED_IDS, try to match it to a Device Minor
 * by extracting the PCI bus number encoded in the gpuId.
 *
 * From observed data:
 *   gpuId 0x11800 → bus 0x18, gpuId 0x11900 → bus 0x19, etc.
 *   gpuId 0x300   → bus 0x03, gpuId 0x400   → bus 0x04, etc.
 *
 * The bus number appears to be encoded in bits 8+, specifically:
 *   bus = gpuId >> 8  (masking off the lower byte which is always 0x00)
 *
 * But the full encoding may also include domain/slot info in higher bits.
 * We try matching just the bus number against our proc map.
 *
 * Returns: device minor on success, -1 on failure.
 */
static int match_gpuid_to_minor(NvU32 gpuId) {
    load_gpu_map();

    /* Extract what we think is the bus number */
    unsigned int extracted_bus = (gpuId >> 8) & 0xFF;

    /* Also try the full value shifted: some gpuIds encode domain+bus */
    unsigned int extracted_full = gpuId >> 8;

    for (int i = 0; i < gpu_map_count; i++) {
        /* Match by bus number (most common case) */
        if (gpu_map[i].bus == extracted_bus) {
            log_msg("gpuId 0x%x: bus 0x%02x matches %04x:%02x:%02x.%x -> minor %d",
                    gpuId, extracted_bus,
                    gpu_map[i].domain, gpu_map[i].bus,
                    gpu_map[i].slot, gpu_map[i].func,
                    gpu_map[i].device_minor);
            return gpu_map[i].device_minor;
        }

        /* For larger gpuIds, try matching domain:bus combined */
        unsigned int combined = (gpu_map[i].domain << 8) | gpu_map[i].bus;
        if (combined == extracted_full) {
            log_msg("gpuId 0x%x: domain:bus 0x%x matches %04x:%02x:%02x.%x -> minor %d",
                    gpuId, extracted_full,
                    gpu_map[i].domain, gpu_map[i].bus,
                    gpu_map[i].slot, gpu_map[i].func,
                    gpu_map[i].device_minor);
            return gpu_map[i].device_minor;
        }
    }

    log_msg("gpuId 0x%x: no /proc match found (extracted bus=0x%02x, full=0x%x)",
            gpuId, extracted_bus, extracted_full);
    return -1;
}

/* ============================================================
 * Main ioctl interposer
 * ============================================================ */

int ioctl(int fd, unsigned long request, ...) {
    ensure_real_ioctl();

    /* Extract the vararg pointer */
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    /* Pass through to real ioctl first */
    int ret = real_ioctl(fd, request, arg);

    /* Only process successful ioctls */
    if (ret != 0)
        return ret;

    /* Check if this is an NV_ESC_RM_CONTROL ioctl.
     * The ioctl number encodes the escape code in bits 0-7 of the NR field. */
    unsigned int nr = _IOC_NR(request);
    if (nr != NV_ESC_RM_CONTROL)
        return ret;

    /* It's an RM_CONTROL. Check the cmd field. */
    NVOS54_PARAMETERS *ctrl = (NVOS54_PARAMETERS *)arg;
    if (!ctrl || !ctrl->params)
        return ret;

    if (ctrl->cmd != NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS)
        return ret;

    if (ctrl->status != 0)
        return ret;

    /* === This is the GET_ATTACHED_IDS response. Filter it. === */

    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS *gpu_params =
        (NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS *)ctrl->params;

    /* Count how many GPUs the kernel reported */
    int total_host_gpus = 0;
    for (int i = 0; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++) {
        if (gpu_params->gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID)
            break;
        total_host_gpus++;
    }

    log_msg("GET_ATTACHED_IDS returned %d GPUs from host", total_host_gpus);

    if (total_host_gpus <= 1) {
        /* Single GPU or empty, nothing to filter */
        return ret;
    }

    /* Get the bitmask of /dev/nvidiaX nodes that exist in this container */
    uint32_t available = get_available_devices();
    log_msg("available device node bitmask: 0x%08x", available);

    if (available == 0) {
        /* Can't determine available devices, don't filter (fail-safe) */
        log_msg("WARNING: no /dev/nvidiaX nodes found, not filtering");
        return ret;
    }

    /*
     * Strategy 1: Try GET_ID_INFO ioctl to map gpuId → deviceInstance.
     * Strategy 2: If that fails, use /proc/driver/nvidia/gpus/ to map
     *             gpuId → PCI bus → Device Minor.
     * Strategy 3: If both fail, we can't determine which GPU is ours,
     *             so we don't filter (fail-safe, same as no fix).
     */

    NvU32 filtered[NV0000_CTRL_GPU_MAX_ATTACHED_GPUS];
    int filtered_count = 0;
    int resolve_failures = 0;

    /* Strategy 1: ioctl-based resolve */
    for (int i = 0; i < total_host_gpus; i++) {
        NvU32 dev_inst = resolve_gpu_id_to_device(fd, ctrl->hClient, gpu_params->gpuIds[i]);

        if (dev_inst == (NvU32)-1) {
            resolve_failures++;
            continue;
        }

        if (dev_inst < 32 && (available & (1u << dev_inst))) {
            log_msg("KEEPING gpuId 0x%x (deviceInstance %u via ioctl)", gpu_params->gpuIds[i], dev_inst);
            filtered[filtered_count++] = gpu_params->gpuIds[i];
        } else {
            log_msg("REMOVING gpuId 0x%x (deviceInstance %u - not in container)", gpu_params->gpuIds[i], dev_inst);
        }
    }

    /* Strategy 2: /proc-based matching if ioctl failed */
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
            /* If dev_minor == -1, this gpuId couldn't be matched at all — skip it */
        }
    }

    /* Strategy 3: If we still have nothing, don't filter */
    if (filtered_count == 0) {
        log_msg("WARNING: could not determine correct GPU, not filtering (NVENC may fail)");
        return ret;
    }

    /* Write back the filtered list */
    for (int i = 0; i < filtered_count; i++) {
        gpu_params->gpuIds[i] = filtered[i];
    }
    for (int i = filtered_count; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++) {
        gpu_params->gpuIds[i] = NV0000_CTRL_GPU_INVALID_ID;
    }

    log_msg("filtered: %d -> %d GPUs", total_host_gpus, filtered_count);

    return ret;
}
