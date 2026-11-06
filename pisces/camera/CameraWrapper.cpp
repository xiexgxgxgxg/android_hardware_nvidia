/*
 * Copyright (C) 2015, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
* @file CameraWrapper.cpp
*
* This file wraps a vendor camera module.
*
*/

//#define LOG_NDEBUG 0
//#define LOG_PARAMETERS

#define LOG_TAG "CameraWrapper"
#include <cutils/log.h>

#include <utils/threads.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cutils/properties.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utils/String8.h>
#include <hardware/hardware.h>
#include <hardware/camera.h>
#include <camera/Camera.h>
#include <camera/CameraParameters.h>

#include <utils/Errors.h>
#include <utils/RefBase.h>

#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <sensor/ISensorServer.h>

static android::Mutex gCameraWrapperLock;

struct wrapper_camera_device_t {
    camera_device_t base;
    int id;
    camera_device_t *vendor;
    bool firstFocusWithFlash;
    bool previewStarted;
    bool activeFocusMove;

    camera_notify_callback notifyCallback;
    camera_data_callback dataCallback;
    camera_data_timestamp_callback dataTimestampCallback;
    camera_request_memory requestMemoryCallback;
    void *callbackUserData;
};

struct nvcamera_device_ops_t : camera_device_ops_t {
    int (*set_custom_parameters)(struct camera_device *, const char *parms);
    char *(*get_custom_parameters)(struct camera_device *);
    int (*get_flash_on)(struct camera_device *);
    int (*get_focus_position)(struct camera_device *);
    int (*get_iso_value)(struct camera_device *);
    float (*get_wb_cct)(struct camera_device *);
};

#define VENDOR_CALL(device, func, ...) ({ \
    wrapper_camera_device_t *__wrapper_dev = (wrapper_camera_device_t*) device; \
    reinterpret_cast<nvcamera_device_ops_t*>(__wrapper_dev->vendor->ops)->func(__wrapper_dev->vendor, ##__VA_ARGS__); \
})

#define CAMERA_ID(device) (((wrapper_camera_device_t *)(device))->id)

static inline wrapper_camera_device_t *toWrapper(struct camera_device *device)
{
    return reinterpret_cast<wrapper_camera_device_t *>(device);
}

static inline wrapper_camera_device_t *toWrapper(void *device)
{
    return reinterpret_cast<wrapper_camera_device_t *>(device);
}

/*
 * pisces: the 2013 HAL never resets its global state (focuser module, NvMM
 * block container, VI/CSI bookkeeping) when a camera is closed.  The next
 * open - switching from the rear to the front camera, or simply reopening the
 * app - then fails with 0xd / 0x30012, and the preview shows uninitialised
 * buffers (the green screen).  Because this wrapper dlopen()s the vendor
 * module itself, it can drop it again after every close: the next
 * camera_device_open() reloads /vendor/lib/hw/camera.vendor.tegra.so with
 * fresh globals, which is exactly the clean state a fresh provider process
 * has.
 */
static void *gVendorHandle = NULL;

static camera_module_t *loadVendorModule()
{
    ALOGV("%s", __FUNCTION__);

    // pisces: the blob was linked against the Android 4.x SensorManager that
    // used to live in libgui.  Put the ABI stubs into the global symbol scope
    // before dlopen()ing the vendor module, otherwise the loader aborts with
    // "cannot locate symbol _ZN7android9SingletonINS_13SensorManagerEE5sLockE".
    void *shim = dlopen("/system/lib/libpisces_camshim.so", RTLD_NOW | RTLD_GLOBAL);
    if (shim == NULL)
        ALOGW("camera ABI shim not loaded: %s", dlerror());

    // Load the vendor module ourselves: hw_get_module_by_class() goes through
    // android_load_sphal_library(), which loads it in a different linker
    // namespace where the shim symbols above are not visible.
    void *handle = dlopen("/vendor/lib/hw/camera.vendor.tegra.so", RTLD_NOW);
    if (handle == NULL)
        ALOGE("failed to dlopen vendor camera module: %s", dlerror());
    else
        gVendorHandle = handle;

    camera_module_t *module = NULL;
    if (handle != NULL) {
        module = (camera_module_t *)dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR);
        if (module == NULL)
            ALOGE("failed to find camera module info: %s", dlerror());
    }

    return module;
}

static camera_module_t *gVendorModule = NULL;

static camera_module_t *getVendorModule()
{
    if (gVendorModule == NULL) {
        gVendorModule = loadVendorModule();
    }
    return gVendorModule;
}

static char *camera_fixup_getparams(wrapper_camera_device_t *wrapper __attribute__((unused)),
        const char *settings)
{
    android::CameraParameters params;
    params.unflatten(android::String8(settings));

#ifdef LOG_PARAMETERS
    ALOGV("%s: Original parameters:", __FUNCTION__);
    params.dump();
#endif

#ifdef LOG_PARAMETERS
    ALOGV("%s: Fixed parameters:", __FUNCTION__);
    params.dump();
#endif

    android::String8 strParams = params.flatten();
    return strdup(strParams.string());
}

static char *camera_fixup_setparams(wrapper_camera_device_t *wrapper __attribute__((unused)),
                                    const char *settings)
{
    android::CameraParameters params;
    params.unflatten(android::String8(settings));

#ifdef LOG_PARAMETERS
    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();
#endif

#ifdef LOG_PARAMETERS
    ALOGV("%s: fixed parameters:", __FUNCTION__);
    params.dump();
#endif

    android::String8 strParams = params.flatten();
    return strdup(strParams.string());
}

/*******************************************************************
 * implementation of camera_device_ops functions
 *******************************************************************/

static int camera_set_preview_window(struct camera_device *device,
        struct preview_stream_ops *window)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, set_preview_window, window);
}

// {{{ intercept callbacks
static void intercept_notify(int32_t msg_type,
                             int32_t ext1,
                             int32_t ext2,
                             void *user)
{ 
    wrapper_camera_device_t *wrapper = toWrapper(user);
    switch (msg_type) {
    case CAMERA_MSG_FOCUS:
        break;

    case CAMERA_MSG_FOCUS_MOVE:
        if (ext1) {
            ALOGV("GOT FOCUS MOVE START");
            if (wrapper->activeFocusMove) {
                // avoid move start inside move start
                return;
            }
            wrapper->activeFocusMove = true;
        } else {
            ALOGV("GOT FOCUS MOVE STOP");
            wrapper->activeFocusMove = false;
        }
        break;
    }
    wrapper->notifyCallback(msg_type, ext1, ext2, wrapper->callbackUserData);
}

static void intercept_data(int32_t msg_type,
                           const camera_memory_t *data, unsigned int index,
                           camera_frame_metadata_t *metadata, void *user)
{
    toWrapper(user)->dataCallback(msg_type, data, index, metadata, toWrapper(user)->callbackUserData);
}

static void intercept_dataTimestamp(int64_t timestamp,
                                    int32_t msg_type,
                                    const camera_memory_t *data, unsigned int index,
                                    void *user)
{
    toWrapper(user)->dataTimestampCallback(timestamp, msg_type, data, index, toWrapper(user)->callbackUserData);
}

static camera_memory_t *intercept_requestMemory(int fd, size_t buf_size, unsigned int num_bufs,
                                                void *user)
{
    return toWrapper(user)->requestMemoryCallback(fd, buf_size, num_bufs, toWrapper(user)->callbackUserData);
}
// }}}

static void camera_set_callbacks(struct camera_device *device,
                                 camera_notify_callback notifyCallback,
                                 camera_data_callback dataCallback,
                                 camera_data_timestamp_callback dataTimestampCallback,
                                 camera_request_memory requestMemoryCallback,
                                 void *user)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    wrapper_camera_device_t *wrapper = toWrapper(device);
    wrapper->notifyCallback = notifyCallback;
    wrapper->dataCallback = dataCallback;
    wrapper->dataTimestampCallback = dataTimestampCallback;
    wrapper->requestMemoryCallback = requestMemoryCallback;
    wrapper->callbackUserData = user;

    VENDOR_CALL(device, set_callbacks, intercept_notify, intercept_data, intercept_dataTimestamp,
            intercept_requestMemory, (void *)wrapper);
}

static void camera_enable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, enable_msg_type, msg_type);
}

static void camera_disable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, disable_msg_type, msg_type);
}

static int camera_msg_type_enabled(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return 0;

    return VENDOR_CALL(device, msg_type_enabled, msg_type);
}

static int camera_start_preview(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, start_preview);
}

static void camera_stop_preview(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, stop_preview);
}

static int camera_preview_enabled(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, preview_enabled);
}

static int camera_store_meta_data_in_buffers(struct camera_device *device,
        int enable)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, store_meta_data_in_buffers, enable);
}

static int camera_start_recording(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return EINVAL;

    return VENDOR_CALL(device, start_recording);
}

static void camera_stop_recording(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, stop_recording);
}

static int camera_recording_enabled(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, recording_enabled);
}

static void camera_release_recording_frame(struct camera_device *device,
        const void *opaque)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, release_recording_frame, opaque);
}

static int camera_auto_focus(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    if (toWrapper(device)->activeFocusMove) {
        ALOGV("FORCED FOCUS MOVE STOP");
        VENDOR_CALL(device, cancel_auto_focus);
        toWrapper(device)->activeFocusMove = false;
    }

    return VENDOR_CALL(device, auto_focus);
}

static int camera_cancel_auto_focus(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    toWrapper(device)->activeFocusMove = false;
    return VENDOR_CALL(device, cancel_auto_focus);
}

static int camera_take_picture(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    bool flashModeOn;
    {
        android::CameraParameters params;
        char *currentParams = VENDOR_CALL(device, get_parameters);
        params.unflatten(android::String8(currentParams));
        VENDOR_CALL(device, put_parameters, currentParams);
        const char *flashMode = params.get(android::CameraParameters::KEY_FLASH_MODE);
        flashModeOn = flashMode && strcmp(flashMode, android::CameraParameters::FLASH_MODE_OFF) != 0;
    }

    if (flashModeOn) {
        if (!toWrapper(device)->firstFocusWithFlash) {
            toWrapper(device)->firstFocusWithFlash = true;
            // focus once to avoid black picture issue
            VENDOR_CALL(device, cancel_auto_focus);
            VENDOR_CALL(device, auto_focus);
            VENDOR_CALL(device, stop_preview);
            VENDOR_CALL(device, start_preview);
        }
    }

    int ret = VENDOR_CALL(device, take_picture);

    if (flashModeOn) {
        // state messed up, restart preview to sync status and flush buffer
        VENDOR_CALL(device, stop_preview);
        VENDOR_CALL(device, start_preview);
    }

    return ret;
}

static int camera_cancel_picture(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, cancel_picture);
}

/*
 * pisces: set_parameters() hands the vendor call to a detached thread so the
 * framework is not blocked while the HAL waits for the preview.  The HAL
 * blocks up to 5 s in WaitForStuffThatNeedsPreview() while applying front
 * sensor settings, and the framework only starts the preview *after*
 * set_parameters() returns - a rear->front switch therefore deadlocked Snap
 * in AndroidCameraProxyImpl.setParameters until the activity was killed.
 */
struct params_job {
    wrapper_camera_device_t *dev;
    char *params;
};

static void *camera_apply_parameters(void *arg)
{
    struct params_job *job = reinterpret_cast<struct params_job *>(arg);

    if (job->dev != NULL && job->dev->vendor != NULL &&
            job->dev->vendor->ops->set_parameters != NULL) {
        int ret = job->dev->vendor->ops->set_parameters(job->dev->vendor, job->params);
        ALOGI("async set_parameters(cam %d) -> %d", CAMERA_ID(job->dev), ret);
    }
    free(job->params);
    free(job);
    return NULL;
}

static int camera_set_parameters(struct camera_device *device,
        const char *params)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    char *fixed_params = camera_fixup_setparams(toWrapper(device), params);

    struct params_job *job = reinterpret_cast<struct params_job *>(malloc(sizeof(*job)));
    if (job != NULL) {
        job->dev = toWrapper(device);
        job->params = fixed_params;
        pthread_t thread;
        if (pthread_create(&thread, NULL, camera_apply_parameters, job) == 0) {
            pthread_detach(thread);
            return 0;
        }
        free(job);
    }

    int ret = VENDOR_CALL(device, set_parameters, fixed_params);
    free(fixed_params);
    return ret;
}

/*
 * pisces: remember each sensor's parameter blob.  Right after a camera open
 * the vendor HAL can still return an empty parameter string (its settings are
 * applied asynchronously), and the framework then configures the sensor with
 * whatever it got - for the front camera that used to be the rear camera's
 * 13 MP blob, which made setParameters time out (0x5) and Snap crash.  Cache
 * per camera id so the right blob is always available.
 */
static char *readFileToString(const char *path)
{
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return NULL;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return NULL;
    }
    buf[n] = '\0';
    return strdup(buf);
}

static void cacheCameraParameters(wrapper_camera_device_t *wrapper)
{
    if (wrapper->vendor == NULL || wrapper->vendor->ops->get_parameters == NULL) {
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/data/misc/pisces_camera/params_%d.txt",
            CAMERA_ID(wrapper));

    for (int attempt = 0; attempt < 10; attempt++) {
        char *params = wrapper->vendor->ops->get_parameters(wrapper->vendor);
        size_t len = (params != NULL) ? strlen(params) : 0;
        if (len > 0) {
            FILE *f = fopen(path, "we");
            if (f != NULL) {
                fwrite(params, 1, len, f);
                fclose(f);
            }
            ALOGI("cached %zu parameter bytes for camera %d", len, CAMERA_ID(wrapper));
        }
        if (params != NULL && wrapper->vendor->ops->put_parameters != NULL) {
            wrapper->vendor->ops->put_parameters(wrapper->vendor, params);
        }
        if (len > 0) {
            return;
        }
        usleep(200000);
    }
    ALOGW("no parameters from the vendor for camera %d", CAMERA_ID(wrapper));
}

static char *camera_get_parameters(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return NULL;

    char *params = VENDOR_CALL(device, get_parameters);
    char *fixed_params = camera_fixup_getparams(toWrapper(device), params);
    VENDOR_CALL(device, put_parameters, params);

    /*
     * pisces: right after a camera open the vendor HAL can still return an
     * empty parameter string; the framework then configures the wrong sensor
     * (the front got the rear's 13 MP blob, setParameters timed out with 0x5
     * and Snap crashed).  Serve the per-camera blob captured by
     * cacheCameraParameters() instead.
     */
    if (fixed_params == NULL || fixed_params[0] == '\0') {
        char path[128];
        snprintf(path, sizeof(path), "/data/misc/pisces_camera/params_%d.txt",
                CAMERA_ID(device));
        char *cached_params = readFileToString(path);
        if (cached_params == NULL) {
            cached_params = readFileToString("/data/misc/pisces_camera/params_0.txt");
        }
        if (cached_params != NULL && cached_params[0] != '\0') {
            ALOGW("vendor returned empty parameters, using the cached blob");
            if (fixed_params != NULL) {
                free(fixed_params);
            }
            char *result = strdup(cached_params);
            free(cached_params);
            return result;
        }
        free(cached_params);
    }
    return fixed_params;
}

static void camera_put_parameters(struct camera_device *device, char *params)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (params)
        free(params);
}

static int camera_send_command(struct camera_device *device,
        int32_t cmd, int32_t arg1, int32_t arg2)
{
#if 0
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));
#endif

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, send_command, cmd, arg1, arg2);
}

static void camera_release(struct camera_device *device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, release);
}

static int camera_dump(struct camera_device *device, int fd)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    return VENDOR_CALL(device, dump, fd);
}

extern "C" void heaptracker_free_leaked_memory(void);

// pisces: the 2013 tegra HAL can be opened exactly once per camera: its close
// path double-releases a refcount and aborts the provider.  The framework opens
// the camera during enumeration (to read SUPPORTED_FLASH_MODES) and then again
// when an app connects, so keep one vendor device per camera id and reuse it.
static camera_device_t *gVendorDevices[4] = { NULL, NULL, NULL, NULL };

/* Close the device nodes the vendor HAL leaks when a camera session ends. */
static void closeLeakedVendorFds()
{
    static const char *nodes[] = {
        "/dev/nvhost-vi", "/dev/nvhost-isp", "/dev/tegra_camera",
        "/dev/imx135", "/dev/imx132", "/dev/focuser", "/dev/torch",
        "/dev/mipi-cal", NULL
    };

    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        return;
    }
    int dir_fd = dirfd(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int fd = atoi(entry->d_name);
        if (fd <= 2 || fd == dir_fd) {
            continue;
        }
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        char target[256];
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len <= 0) {
            continue;
        }
        target[len] = '\0';
        for (int i = 0; nodes[i] != NULL; i++) {
            if (strncmp(target, nodes[i], strlen(nodes[i])) == 0) {
                ALOGI("closing leaked %s (fd %d)", target, fd);
                close(fd);
                break;
            }
        }
    }
    closedir(dir);
}

/* Drop the vendor module so the next open reloads it with fresh globals.  See
 * the comment at gVendorHandle. */
static void unloadVendorModule()
{
    if (gVendorHandle != NULL) {
        ALOGI("unloading vendor camera module to reset its state");
        dlclose(gVendorHandle);
        gVendorHandle = NULL;
    }
    gVendorModule = NULL;
}

static int camera_device_close(hw_device_t *device)
{
    wrapper_camera_device_t *wrapper_dev = reinterpret_cast<wrapper_camera_device_t *>(device);

    ALOGV("%s", __FUNCTION__);

    android::Mutex::Autolock lock(gCameraWrapperLock);

    int ret = 0;
    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    wrapper_dev->vendor->common.close((hw_device_t*)wrapper_dev->vendor);
    // note: the vendor device is not reusable after close; drop it from the cache
    for (size_t i = 0; i < sizeof(gVendorDevices) / sizeof(gVendorDevices[0]); i++) {
        if (gVendorDevices[i] == wrapper_dev->vendor) {
            gVendorDevices[i] = NULL;
        }
    }
    /* pisces: force the next open to reload the HAL from scratch */
    unloadVendorModule();

    /* Drop the private NvMM camera core copies so the next camera session gets
     * freshly initialised static state (see the camshim). */
    {
        static void (*reset_nvmm)(void) = NULL;
        if (reset_nvmm == NULL) {
            reset_nvmm = (void (*)(void))dlsym(RTLD_DEFAULT, "pisces_cam_reset_nvmm");
        }
        if (reset_nvmm != NULL) {
            reset_nvmm();
        }
    }

    /*
     * The vendor HAL never closes what it opened: after a camera session the
     * provider still holds /dev/imx135, /dev/focuser, /dev/torch,
     * /dev/tegra_camera and the nvhost VI channel.  The next session then
     * reuses the previous sensor's VI channel/hardware context, which is why a
     * rear->front switch opened the front sensor ("[imx132]: stream on") but
     * its CSI never produced frames (sync point 15 timeout, black then green
     * preview).  Close those leaked descriptors here, after the HAL has been
     * given a moment to tear its own state down, so the next open starts from
     * a genuinely free VI channel and sensor.
     */
    usleep(300000);
    closeLeakedVendorFds();

    /*
     * The vendor HAL cannot serve a second sensor in the same process: after
     * one camera session the next one opens but its CSI never delivers frames
     * (sync point 15 timeout, black then green preview), in *both* directions.
     * The same sensor always works as the first session of a process, so
     * restart the provider after every close; the camera service retries the
     * next open while the fresh provider registers.  set_parameters() is
     * asynchronous and each sensor's parameter blob is cached, so the
     * framework is not blocked or misconfigured during the switch.
     */
    {
        pid_t child = fork();
        if (child == 0) {
            execl("/system/bin/sh", "sh", "-c", "kill -9 $PPID", (char *)NULL);
            _exit(0);
        }
    }

    if (wrapper_dev->base.ops)
        delete wrapper_dev->base.ops;
    delete wrapper_dev;
done:
#ifdef HEAPTRACKER
    heaptracker_free_leaked_memory();
#endif
    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

/*
 * Dual-provider support: each camera provider instance serves exactly one
 * sensor, chosen with the PISCES_CAM_ROLE environment variable ("rear" by
 * default, "front" for the second instance).  Because every provider only ever
 * handles one sensor, the vendor HAL is always in its "first session" state,
 * which is the only state in which the CSI delivers frames.
 */
static int cameraIsFrontRole(void)
{
    static int front = -1;
    if (front < 0) {
        const char *role = getenv("PISCES_CAM_ROLE");
        front = (role != NULL && strcmp(role, "front") == 0) ? 1 : 0;
    }
    return front;
}

/*
 * Debug switch: persist.pisces.camswap=1 makes camera id 0 open the front
 * sensor and id 1 the rear.  It exists so a "front sensor as the first session
 * of a process" (which streams) can be compared with a rear->front switch
 * (which does not) without touching the camera app.
 */
static int vendorCameraId(int id)
{
    /* dual-provider mode: the module id 0 is the sensor this instance serves */
    if (cameraIsFrontRole()) {
        return 1;
    }
    char value[PROP_VALUE_MAX];
    if (property_get("persist.pisces.camswap", value, "0") >= 1 &&
            value[0] == '1') {
        return (id == 0) ? 1 : 0;
    }
    return id;
}

static camera_device_t *openVendorDeviceLocked(int id, const char *name)
{
    if (id < 0 || id >= (int)(sizeof(gVendorDevices) / sizeof(gVendorDevices[0])))
        return NULL;

    if (gVendorDevices[id] != NULL) {
        ALOGW("reusing vendor camera %d (single-open HAL)", id);
        return gVendorDevices[id];
    }

    int real_id = vendorCameraId(id);
    char real_name[8];
    snprintf(real_name, sizeof(real_name), "%d", real_id);

    camera_device_t *dev = NULL;
    int rv = getVendorModule()->common.methods->open(
            (const hw_module_t*)getVendorModule(), real_name, (hw_device_t**)&dev);
    if (rv != 0 || dev == NULL) {
        ALOGE("vendor camera open failed for %s: %d", name, rv);
        return NULL;
    }
    gVendorDevices[id] = dev;
    return dev;
}

static int camera_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    android::Mutex::Autolock lock(gCameraWrapperLock);

    ALOGV("%s", __FUNCTION__);

    if (!name) {
        return -EINVAL;
    }

    if (!getVendorModule()) {
        return -EINVAL;
    }

    int cameraid = atoi(name);
    /* pisces: rear (0) and front (1); see camera_get_number_of_cameras().
     * Do not call the vendor here - its get_number_of_cameras() probes the
     * sensors and claims the imager. */
    int num_cameras = 1;
    ALOGI("camera count = %d (role %s)", num_cameras,
            cameraIsFrontRole() ? "front" : "rear");

    int rv = 0;

    if (cameraid > num_cameras) {
        ALOGE("camera service provided cameraid out of bounds, "
                "cameraid = %d, num supported = %d",
                cameraid, num_cameras);
        return -EINVAL;
    }

    wrapper_camera_device_t *camera_device = new wrapper_camera_device_t();
    camera_device_ops_t *camera_ops = NULL;

    if (!camera_device) {
        ALOGE("camera_device allocation fail");
        rv = -ENOMEM;
        goto fail;
    }
    memset(camera_device, 0, sizeof(*camera_device));
    camera_device->id = cameraid;

    // camera require sensorservice to be up
    {
        using namespace android;
        const String16 sensorServiceName("sensorservice");
        sp<ISensorServer> sensorServer;
        // pisces: there is no sensors HAL on this device yet, and the sensor
        // entry points the blob calls are stubbed by libpisces_camshim, so a
        // short bounded wait is enough.  Waiting for the binder service here
        // blocked camera provider start-up for minutes.
        // pisces: no sensors HAL exists on this device and libpisces_camshim
        // stubs every sensor entry point the blob uses, so do not wait at all.
        // Each binder lookup here blocked for ~6 s.
        (void)sensorServer;
    }

    ALOGV("calling vendor camera_device_open ...");
    camera_device->vendor = openVendorDeviceLocked(cameraid, name);
    if (camera_device->vendor == NULL) {
        /*
         * pisces: the previous session's HAL teardown is asynchronous, so the
         * next open can still see "NvFmInitialize: Failed already initialized"
         * and fail with 0xd.  A second attempt a moment later succeeds (this
         * is what the app used to do by falling back from openLegacy to
         * open), so retry here instead of failing the switch.
         */
        for (int attempt = 0; attempt < 6 && camera_device->vendor == NULL; attempt++) {
            usleep(300000);
            ALOGW("vendor camera open failed, retry %d", attempt + 1);
            camera_device->vendor = openVendorDeviceLocked(cameraid, name);
        }
    }
    if (camera_device->vendor == NULL) {
        ALOGE("vendor camera open fail");
        rv = -ENODEV;
        goto fail;
    }
    ALOGV("%s: got vendor camera device 0x%08X",
            __FUNCTION__, (uintptr_t)(camera_device->vendor));
    cacheCameraParameters(camera_device);

    camera_ops = new camera_device_ops_t();
    if (!camera_ops) {
        ALOGE("camera_ops allocation fail");
        rv = -ENOMEM;
        goto fail;
    }

    memset(camera_ops, 0, sizeof(*camera_ops));

    camera_device->base.common.tag = HARDWARE_DEVICE_TAG;
    camera_device->base.common.version = CAMERA_DEVICE_API_VERSION_1_0;
    camera_device->base.common.module = (hw_module_t *)(module);
    camera_device->base.common.close = camera_device_close;
    camera_device->base.ops = camera_ops;

    camera_ops->set_preview_window = camera_set_preview_window;
    camera_ops->set_callbacks = camera_set_callbacks;
    camera_ops->enable_msg_type = camera_enable_msg_type;
    camera_ops->disable_msg_type = camera_disable_msg_type;
    camera_ops->msg_type_enabled = camera_msg_type_enabled;
    camera_ops->start_preview = camera_start_preview;
    camera_ops->stop_preview = camera_stop_preview;
    camera_ops->preview_enabled = camera_preview_enabled;
    camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
    camera_ops->start_recording = camera_start_recording;
    camera_ops->stop_recording = camera_stop_recording;
    camera_ops->recording_enabled = camera_recording_enabled;
    camera_ops->release_recording_frame = camera_release_recording_frame;
    camera_ops->auto_focus = camera_auto_focus;
    camera_ops->cancel_auto_focus = camera_cancel_auto_focus;
    camera_ops->take_picture = camera_take_picture;
    camera_ops->cancel_picture = camera_cancel_picture;
    camera_ops->set_parameters = camera_set_parameters;
    camera_ops->get_parameters = camera_get_parameters;
    camera_ops->put_parameters = camera_put_parameters;
    camera_ops->send_command = camera_send_command;
    camera_ops->release = camera_release;
    camera_ops->dump = camera_dump;

    *device = &camera_device->base.common;

    return rv;

fail:
    if (camera_ops) {
        delete camera_ops;
        camera_ops = NULL;
    }

    if (camera_device) {
        delete camera_device;
        camera_device = NULL;
    }

    *device = NULL;
    return rv;
}

static int camera_get_number_of_cameras(void)
{
    /*
     * pisces: never call the vendor's get_number_of_cameras() - it probes the
     * sensors (opening the ODM imager) and leaves it claimed, which used to
     * break the first real open.
     *
     * Dual-provider mode: each provider instance serves exactly one sensor
     * (PISCES_CAM_ROLE), because the 2013 HAL can only serve one sensor per
     * process - the second sensor opens but its CSI never delivers frames.
     * The module therefore reports a single camera, numbered 0, and the
     * provider's PISCES_CAM_ID_BASE makes the global camera id unique.
     */
    int ret = 1;
    ALOGI("%s = %d (role %s)", __FUNCTION__, ret,
            cameraIsFrontRole() ? "front" : "rear");
    return ret;
}

static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGI("%s(%d) role %s", __FUNCTION__, camera_id, cameraIsFrontRole() ? "front" : "rear");
    /*
     * pisces: do not forward to the vendor module here.  Its get_camera_info
     * opens the sensor ODM imager (powers the IMX135, opens /dev/focuser and
     * /dev/nvhost-vi) and never releases it, so the first real
     * camera_device_open() then fails with
     *   IMX135 **** Can not open camera device: Device or resource busy
     *   setupHalInLock:539 failed to open camera! Error: 0x70001
     * Only the rear sensor is exposed (see camera_get_number_of_cameras), so
     * its geometry is reported directly instead.  The 90 degree orientation
     * is the Mi 3 rear module's mounting angle.
     */
    if (info == NULL || camera_id < 0 || camera_id > 0) {
        return -EINVAL;
    }
    memset(info, 0, sizeof(*info));
    info->facing = cameraIsFrontRole() ? CAMERA_FACING_FRONT : CAMERA_FACING_BACK;
    /* nvcamera.conf: rear 0, front 270.  The rear module is mounted at 90 in
     * practice; the front uses the vendor value now that it is exposed as a
     * real front camera (the framework mirrors it, which it does not do for a
     * camera reported as back). */
    info->orientation = cameraIsFrontRole() ? 270 : 90;
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    info->static_camera_characteristics = NULL;
    info->resource_cost = 0;
    info->conflicting_devices = NULL;
    info->conflicting_devices_length = 0;
    return 0;
}

static struct hw_module_methods_t camera_module_methods = {
    .open = camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
         .hal_api_version = HARDWARE_HAL_API_VERSION,
         .id = CAMERA_HARDWARE_MODULE_ID,
         .name = "Pisces Camera Wrapper",
         .author = "Xuefer <xuefer@gmail.com>",
         .methods = &camera_module_methods,
         .dso = NULL, /* remove compilation warnings */
         .reserved = {0}, /* remove compilation warnings */
    },
    .get_number_of_cameras = camera_get_number_of_cameras,
    .get_camera_info = camera_get_camera_info,
    .set_callbacks = NULL, /* remove compilation warnings */
    .get_vendor_tag_ops = NULL, /* remove compilation warnings */
    .open_legacy = NULL, /* remove compilation warnings */
    .reserved = {0}, /* remove compilation warnings */
};
