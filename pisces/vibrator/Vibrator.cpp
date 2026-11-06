/*
 * Copyright (C) 2011 CyanogenMod Project
 * Copyright (C) 2014 Xuefer
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

/*
 * pisces vibrator HAL.
 *
 * The stock implementation drives the motor through Immersion's libImmVibeJ
 * (VibeTonz).  On this bring-up that stack cannot initialise:
 *
 *   E VibeTonz: VibeDriverInitialize: Insufficient kernel parameters.
 *   E Vibrator: Initialization Failed: status=-4
 *
 * and every vibration is dropped.  The kernel driver itself is fine - it
 * reports "DRV2604 Rev:1 v5.0.36.6" and exposes the Xiaomi timed_output
 * interface, which is exactly what the AOSP default HAL uses:
 *
 *   /sys/class/timed_output/vibrator/enable   (write timeout in ms, 0 = off)
 *
 * Writing the timeout there makes the kernel switch the DRV2604 into RTP mode
 * and run its own hrtimer for the requested duration, so no userspace
 * scheduling is involved.  Use that instead of the broken ImmVibe stack.
 */

#include <hardware/vibrator.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "stdio.h"
#include "string.h"
#include "stdlib.h"

//#define LOG_NDEBUG 0
#define LOG_TAG "Vibrator"
#include <utils/Log.h>

#define THE_DEVICE "/sys/class/timed_output/vibrator/enable"

static int write_value(const char* file, const char* value) {
    int to_write, written, ret, fd;

    fd = TEMP_FAILURE_RETRY(open(file, O_WRONLY));
    if (fd < 0) {
        return -errno;
    }

    to_write = strlen(value) + 1;
    written = TEMP_FAILURE_RETRY(write(fd, value, to_write));
    if (written == -1) {
        ret = -errno;
    } else if (written != to_write) {
        /* even though Linux isn't guaranteed to do this in practice,
         * we assume it's an error to write less than requested */
        ret = -EIO;
    } else {
        ret = 0;
    }

    close(fd);
    return ret;
}

static int vibra_exists(void) {
    return access(THE_DEVICE, W_OK) == 0;
}

static int sendit(int timeout_ms) {
    char value[20];

    if (!vibra_exists()) {
        ALOGE("Vibrator device does not exist. Cannot start vibrator");
        return -ENODEV;
    }

    snprintf(value, sizeof(value), "%d", timeout_ms);

    ALOGV("vibrate sendit(%d)", timeout_ms);
    return write_value(THE_DEVICE, value);
}

extern "C" {

static int vibra_on(vibrator_device_t* vibradev __unused, unsigned int timeout_ms) {
    /* constant on, up to maximum allowed time */
    return sendit(timeout_ms);
}

static int vibra_off(vibrator_device_t* vibradev __unused) {
    return sendit(0);
}

static int vibra_close(hw_device_t *device) {
    free(device);
    sendit(0);
    return 0;
}

} // extern C

static int vibra_open(const hw_module_t* module, const char* id __unused,
                      hw_device_t** device) {
    vibrator_device_t *vibradev = new vibrator_device_t;

    if (!vibradev) {
        ALOGE("Can not allocate memory for the vibrator device");
        return -ENOMEM;
    }

    vibradev->common.tag = HARDWARE_DEVICE_TAG;
    vibradev->common.module = (hw_module_t *) module;
    vibradev->common.version = HARDWARE_DEVICE_API_VERSION(1,0);
    vibradev->common.close = vibra_close;

    vibradev->vibrator_on = vibra_on;
    vibradev->vibrator_off = vibra_off;

    *device = (hw_device_t *) vibradev;

    if (!vibra_exists()) {
        ALOGE("Vibrator device does not exist. Cannot start vibrator");
    } else {
        ALOGI("Vibrator using timed_output");
    }
    return 0;
}

/*===========================================================================*/
/* Default vibrator HW module interface definition                           */
/*===========================================================================*/

static struct hw_module_methods_t vibrator_module_methods = {
    .open = vibra_open,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = VIBRATOR_API_VERSION,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = VIBRATOR_HARDWARE_MODULE_ID,
    .name = "Default vibrator HAL",
    .author = "The Android Open Source Project",
    .methods = &vibrator_module_methods,
    .dso = NULL,
    .reserved = {0},
};
