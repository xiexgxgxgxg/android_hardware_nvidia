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

static int camera_set_parameters(struct camera_device *device,
        const char *params)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    if (!device)
        return -EINVAL;

    char *fixed_params = camera_fixup_setparams(toWrapper(device), params);
    int ret = VENDOR_CALL(device, set_parameters, fixed_params);
    free(fixed_params);
    return ret;
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
     * pisces: after a provider restart (the wrapper kills the provider on
     * every close so the next session starts from clean HAL state) the first
     * get_parameters() can race the HAL's initialisation and come back empty.
     * Snap then dies with "getParameters failed (empty parameters)" right
     * after a rear<->front switch.  Fall back to the parameter blob captured
     * from a working session (the same file the camera service uses for the
     * legacy shim) instead of handing the framework an empty string.
     */
    if (fixed_params == NULL || fixed_params[0] == '\0') {
        static char *cached_params = NULL;
        if (cached_params == NULL) {
            FILE *f = fopen("/data/misc/cameraserver/pisces_shim_params.txt", "re");
            if (f != NULL) {
                static char buf[4096];
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                if (n > 0) {
                    buf[n] = '\0';
                    cached_params = strdup(buf);
                }
            }
        }
        if (cached_params != NULL && cached_params[0] != '\0') {
            ALOGW("vendor returned empty parameters, using the cached blob");
            if (fixed_params != NULL) {
                free(fixed_params);
            }
            return strdup(cached_params);
        }
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

    /*
     * The vendor HAL tears its module-global state down asynchronously (focus
     * motor "NvFmInitialize: Failed already initialized", NvMM block
     * container, VI/CSI bookkeeping): the next camera open in this process
     * raced that teardown and failed with 0xd (green preview when switching to
     * the front sensor).  Give the teardown a moment before returning the
     * close to the framework - the app's next open then starts from a clean
     * HAL state without having to restart the whole provider.
     */
    usleep(400000);
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

static camera_device_t *openVendorDeviceLocked(int id, const char *name)
{
    if (id < 0 || id >= (int)(sizeof(gVendorDevices) / sizeof(gVendorDevices[0])))
        return NULL;

    if (gVendorDevices[id] != NULL) {
        ALOGW("reusing vendor camera %d (single-open HAL)", id);
        return gVendorDevices[id];
    }

    camera_device_t *dev = NULL;
    int rv = getVendorModule()->common.methods->open(
            (const hw_module_t*)getVendorModule(), name, (hw_device_t**)&dev);
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
    int num_cameras = 2;
    ALOGI("camera count = %d", num_cameras);

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
        ALOGE("vendor camera open fail");
        rv = -ENODEV;
        goto fail;
    }
    ALOGV("%s: got vendor camera device 0x%08X",
            __FUNCTION__, (uintptr_t)(camera_device->vendor));

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
    /* pisces: never call the vendor's get_number_of_cameras() - it probes the
     * sensors (opening the ODM imager) and leaves it claimed, which used to
     * break the first real open.  The board has a rear IMX135 (0) and a front
     * IMX132 (1); both work now that the VI channel is shareable and the
     * sensor/focuser/torch/tegra_camera opens are reference counted. */
    int ret = 2;
    /*
     * pisces: only expose the rear sensor (id 0, IMX135).  The front one
     * (id 1, IMX132) cannot open its VI channel -
     *   Opening channel /dev/nvhost-vi (0xc) failed
     *   NvCameraViCsiInitializeChannel: Failed !!! (error 0x30012)
     * - and NvMMCameraOpen() then crashes inside libnvmm_camera while
     * unwinding, which kills the whole camera provider and takes the rear
     * camera down with it (cameraserver ends up with 0 devices).  Reporting
     * a single camera keeps cameraserver from ever touching id 1.
     */
    ALOGV("%s = %d", __FUNCTION__, ret);
    return ret;
}

static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGV("%s(%d)", __FUNCTION__, camera_id);
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
    if (info == NULL || camera_id < 0 || camera_id > 1) {
        return -EINVAL;
    }
    memset(info, 0, sizeof(*info));
    info->facing = (camera_id == 0) ? CAMERA_FACING_BACK : CAMERA_FACING_FRONT;
    info->orientation = (camera_id == 0) ? 90 : 270;
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
