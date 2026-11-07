/*
 * ABI shim for the 2013 tegra camera HAL (camera.tegra.so / camera.vendor.tegra.so).
 *
 * The blob was linked against the Android 4.x SensorManager, which lived in
 * libgui as a Singleton.  In Android 9 SensorManager moved to libsensor, is no
 * longer a Singleton and its createEventQueue() gained arguments, so the
 * loader fails with:
 *
 *   cannot locate symbol "_ZN7android9SingletonINS_13SensorManagerEE5sLockE"
 *
 * and, once that is satisfied, the camera open still fails with
 * "NvCameraHalCore: Fail to initialize SensorListener" because
 * createEventQueue() has to hand back a usable queue.
 *
 * The Android 9 SensorEventQueue takes the sensor callback fd / read / enable
 * calls as *non-virtual* member functions, i.e. the blob resolves them by
 * symbol.  So this shim exports those symbols and forwards them to the real
 * Android 9 object, and createEventQueue() returns a real queue obtained from
 * libsensor's SensorManager.  The rest is stubbed.
 *
 * LD_PRELOADed into the camera provider service.
 */

#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <android/log.h>
#include <dlfcn.h>

#include <sensor/SensorManager.h>
#include <sensor/SensorEventQueue.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/CallStack.h>
#include <string.h>
#include <cutils/properties.h>

using android::SensorEventQueue;
using android::Sensor;
using android::String16;
using android::String8;

// The blob is Android 4.x ABI: `this` is the first argument for the member
// functions it resolves by symbol, and createEventQueue() returns a
// non-trivial sp<> through a hidden pointer (r0 = return slot, r1 = this).
#define PISCES_METHOD(name) extern "C" __attribute__((visibility("default"))) name

// The blobs (libFaceProc.so in particular) still carry text relocations, which
// bionic refuses to load for a process targeting API 23 or newer.  Dropping the
// process target SDK to 22 is the documented workaround for pre-M vendor
// libraries.
extern "C" void android_set_application_target_sdk_version(uint32_t target);

__attribute__((constructor)) static void pisces_camshim_init(void) {
    android_set_application_target_sdk_version(22);
}

// android::Singleton<android::SensorManager>::sLock / sInstance.
// The blob's inlined getInstance() uses sLock as an android::Mutex; zeroed
// storage is a valid (unlocked) bionic mutex.
__attribute__((aligned(16), visibility("default")))
char _ZN7android9SingletonINS_13SensorManagerEE5sLockE[256];

__attribute__((aligned(16), visibility("default")))
char _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE[8];

// android::SensorManager::SensorManager(): the blob allocates the object with
// its own (2013) sizeof; the constructor body does nothing on purpose.
PISCES_METHOD(void) _ZN7android13SensorManagerC1Ev(void *self) { (void)self; }
PISCES_METHOD(void) _ZN7android13SensorManagerC2Ev(void *self) { (void)self; }

// const android::Sensor *SensorManager::getDefaultSensor(int) const
//
// Do NOT forward this to the real SensorManager: there is no sensors HAL on
// this device, so the real getInstanceForPackage()/getDefaultSensor() blocks
// forever in ServiceManager::getService("sensorservice") (observed as an
// endless "Waiting for service 'sensorservice'" loop in the camera provider).
// The camera HAL only uses the returned pointer for enableSensor()/
// setEventRate(), which are stubbed, so returning NULL is enough; the sensor
// listener then simply has no sensors to report.
PISCES_METHOD(void *) _ZN7android13SensorManager16getDefaultSensorEi(void *self, int type) {
    (void)self;
    (void)type;
    return nullptr;
}

// android::sp<android::SensorEventQueue> SensorManager::createEventQueue()
//
// There is no sensors HAL on this device, so the real SensorManager cannot
// create a queue ("NvSensorListener: createEventQueue returned NULL" made the
// whole camera open fail).  The sensor listener only needs a live object: the
// five SensorEventQueue methods the blob uses are resolved by symbol and hit
// the stubs below, so a bare RefBase subclass is enough - it just has to have
// valid refcount storage for the sp<> the caller keeps.
namespace {
class CameraDummyQueue : public android::RefBase {};
CameraDummyQueue gDummyQueue;
// The HAL drops its sp<> again when the sensor listener goes away; with the
// last reference gone RefBase would "delete this" on the static object above
// and the camera provider died in ifree().  Keep one reference forever.
android::sp<SensorEventQueue> gDummyQueueKeepAlive;
}  // namespace

PISCES_METHOD(void *) _ZN7android13SensorManager16createEventQueueEv(void *ret, void *self) {
    (void)self;
    if (gDummyQueueKeepAlive == nullptr) {
        gDummyQueueKeepAlive =
                android::sp<SensorEventQueue>(
                        reinterpret_cast<SensorEventQueue*>(&gDummyQueue));
    }
    new (ret) android::sp<SensorEventQueue>(gDummyQueueKeepAlive);
    return ret;
}

// android::SensorEventQueue members.
//
// These can NOT forward to the real Android 9 object: the real methods have
// exactly the same mangled names as these definitions, so a call such as
// static_cast<SensorEventQueue*>(self)->read(...) resolves to *this* function,
// i.e. the shim used to call itself.  With -O2 the compiler turned that tail
// recursion into an infinite loop (a two byte "b ." at the entry point), the
// camera HAL's sensor-listener thread never came back from initialize(), and
// NvCameraHal::setupHalInLock() blocked forever in pthread_join() - that was
// the camera deadlock (13 more threads sat in tegra_sema_wait behind it).
//
// There is also no sensors HAL on this device, so there is no real queue to
// forward to anyway: a dummy queue is created by createEventQueue() above and
// the listener only needs these calls to return instead of hanging.  read()
// reports "no events", getFd() a dead fd, and the rest success.
PISCES_METHOD(int) _ZN7android16SensorEventQueue4readEP12ASensorEventj(void *self,
                                                                       void *events,
                                                                       unsigned int count) {
    (void)self;
    (void)events;
    (void)count;
    return 0;
}

PISCES_METHOD(int) _ZNK7android16SensorEventQueue5getFdEv(void *self) {
    (void)self;
    /*
     * A real (but永远 empty) pipe: the HAL puts this fd into a Looper
     * ("Error adding epoll events for fd -1" when it was -1) and polls it for
     * sensor events.  Nobody ever writes to it, so read() above never has
     * anything to report, but the listener thread stays healthy.
     */
    static int fds[2] = {-1, -1};
    if (fds[0] < 0 && pipe(fds) != 0) {
        return -1;
    }
    return fds[0];
}

PISCES_METHOD(int) _ZNK7android16SensorEventQueue12enableSensorEPKNS_6SensorE(void *self,
                                                                             void *sensor) {
    (void)self;
    (void)sensor;
    return 0;
}

PISCES_METHOD(int) _ZNK7android16SensorEventQueue13disableSensorEPKNS_6SensorE(void *self,
                                                                              void *sensor) {
    (void)self;
    (void)sensor;
    return 0;
}

PISCES_METHOD(int) _ZNK7android16SensorEventQueue12setEventRateEPKNS_6SensorEx(void *self,
                                                                              void *sensor,
                                                                              long long rate) {
    (void)self;
    (void)sensor;
    (void)rate;
    return 0;
}

/*
 * The vendor HAL reports its whole internal state machine through the imported
 * NvOsDebugPrintf() symbol (VI/CSI initialisation, "already initialized"
 * guards, block container errors).  The shim is loaded RTLD_GLOBAL before the
 * vendor module, so defining it here routes those messages to logcat with a
 * known tag, which is what makes debugging the rear->front switch possible.
 */
extern "C" __attribute__((visibility("default")))
__attribute__((format(printf, 1, 2)))
int NvOsDebugPrintf(const char *fmt, ...);

extern "C" __attribute__((visibility("default")))
__attribute__((format(printf, 1, 2)))
int NvOsDebugPrintf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const char *format = (fmt != NULL && fmt[0] != '\0') ? fmt : "(null)";
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (n > 0) {
        __android_log_write(ANDROID_LOG_DEBUG, "NvOsDbg", line);
        /* pisces: dump the call stack for the messages that mark the video
         * path state, so the blob's internal functions can be located by
         * address (see /proc/<pid>/maps for the library base). */
        if (strstr(line, "ViCsi") != NULL ||
                strstr(line, "already initialized") != NULL ||
                strstr(line, "syncpt") != NULL) {
            android::CallStack stack("NvOsDbg");
            stack.log("NvOsDbg");
        }
    }
    return n;
}

/*
 * The vendor HAL programs the host1x VI/ISP modules through
 * NvRmHostModuleRegWr() (provided by libnvrm_graphics.so).  Intercepting it
 * makes the VI/CSI register programming visible, which is what a rear->front
 * switch fails to redo.  The real function is resolved through an explicit
 * dlopen() handle because RTLD_NEXT does not see the vendor namespace.
 */
typedef int (*rm_regwr_t)(void *hDevice, unsigned int module, unsigned int count,
        const unsigned int *offsets, const unsigned int *values);
typedef int (*rm_regrd_t)(void *hDevice, unsigned int module, unsigned int count,
        const unsigned int *offsets, unsigned int *values);

static rm_regwr_t gRealRegWr = NULL;
static rm_regrd_t gRealRegRd = NULL;

static void resolveRmGraphics(void)
{
    if (gRealRegWr != NULL && gRealRegRd != NULL) {
        return;
    }
    void *handle = dlopen("libnvrm_graphics.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, "NvRmReg", "no libnvrm_graphics: %s", dlerror());
        return;
    }
    if (gRealRegWr == NULL) {
        gRealRegWr = (rm_regwr_t)dlsym(handle, "NvRmHostModuleRegWr");
    }
    if (gRealRegRd == NULL) {
        gRealRegRd = (rm_regrd_t)dlsym(handle, "NvRmHostModuleRegRd");
    }
}

extern "C" __attribute__((visibility("default")))
int NvRmHostModuleRegWr(void *hDevice, unsigned int module, unsigned int count,
        const unsigned int *offsets, const unsigned int *values)
{
    resolveRmGraphics();
    for (unsigned int i = 0; i < count && i < 32; i++) {
        __android_log_print(ANDROID_LOG_DEBUG, "NvRmReg",
                "WR mod=%u off=0x%04x val=0x%08x", module, offsets[i], values[i]);
    }
    if (gRealRegWr != NULL) {
        return gRealRegWr(hDevice, module, count, offsets, values);
    }
    return 0;
}

extern "C" __attribute__((visibility("default")))
int NvRmHostModuleRegRd(void *hDevice, unsigned int module, unsigned int count,
        const unsigned int *offsets, unsigned int *values)
{
    resolveRmGraphics();
    int ret = 0;
    if (gRealRegRd != NULL) {
        ret = gRealRegRd(hDevice, module, count, offsets, values);
    }
    for (unsigned int i = 0; i < count && i < 32; i++) {
        __android_log_print(ANDROID_LOG_DEBUG, "NvRmReg",
                "RD mod=%u off=0x%04x val=0x%08x", module, offsets[i], values[i]);
    }
    return ret;
}

/*
 * Trace which library loads libnvmm_camera.so (the blob's camera core, where
 * the per-process static guards live) and how: it is not in any of the camera
 * libraries' DT_NEEDED lists, so it must be an explicit dlopen().
 */
static void *loadPrivateNvmmCamera(void);
static void *getSessionNvmmCamera(void);

extern "C" __attribute__((visibility("default")))
void *dlopen(const char *filename, int flags)
{
    static void *(*real_dlopen)(const char *, int) = NULL;
    if (real_dlopen == NULL) {
        real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    }
    if (filename != NULL && strstr(filename, "nvmm_camera") != NULL &&
            strstr(filename, "nvmmcam_") == NULL) {
        void *priv = getSessionNvmmCamera();
        __android_log_print(ANDROID_LOG_DEBUG, "DlOpen",
                "dlopen(%s, 0x%x) -> private %p (from %p)", filename, flags,
                priv, __builtin_return_address(0));
        if (priv != NULL) {
            return priv;
        }
    }
    void *handle = (real_dlopen != NULL) ? real_dlopen(filename, flags) : NULL;
    if (filename != NULL && strstr(filename, "nvmm_camera") != NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "DlOpen",
                "dlopen(%s, 0x%x) -> %p (from %p)", filename, flags, handle,
                __builtin_return_address(0));
    }
    return handle;
}

/*
 * The NvMM core (libnvmm.so) loads libnvmm_camera.so at runtime with
 * __android_dlopen_ext().  That library keeps per-process static state (the
 * focus-motor guard, the VI/CSI programming, the FixFence bookkeeping), which
 * is what makes a second camera sensor in the same process fail: the sensor
 * opens and streams but its CSI never delivers frames.
 *
 * Redirect that load to a private copy with a unique SONAME, and drop the copy
 * when the camera is closed, so every camera session gets a freshly
 * initialised NvMM camera core without restarting the provider.
 */
#define CAM_TEMPLATE "/data/misc/pisces_camera/nvmmcam.tpl"
#define CAM_COPY_DIR "/data/misc/pisces_camera"
#define CAM_SONAME_PREFIX "libpisces_nvmm_cam_"
#define CAM_SONAME_LEN 17   /* "libnvmm_camera.so" */

static void *gNvmmHandles[8];
static int gNvmmHandleCount;
static int gNvmmSession;

static int copyFile(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[16384];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            close(in);
            close(out);
            return -1;
        }
    }
    close(in);
    close(out);
    return 0;
}

/* Rewrite the template's SONAME in the copy so the loader sees a new object. */
static void patchSoname(const char *path, char tag)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    char *map = (char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map != MAP_FAILED) {
        char from[CAM_SONAME_LEN + 1];
        snprintf(from, sizeof(from), "libnvmm_camera.so");
        char to[CAM_SONAME_LEN + 1];
        snprintf(to, sizeof(to), "libnvmm_camer%c.so", tag);
        for (off_t i = 0; i + CAM_SONAME_LEN < size; i++) {
            if (memcmp(map + i, from, CAM_SONAME_LEN) == 0) {
                memcpy(map + i, to, CAM_SONAME_LEN);
            }
        }
        munmap(map, size);
    }
    close(fd);
}

static void *loadPrivateNvmmCamera(void)
{
    int n = gNvmmSession++ % 36;
    char tag = (n < 10) ? ('0' + n) : ('a' + (n - 10));
    char path[128];
    snprintf(path, sizeof(path), CAM_COPY_DIR "/nvmmcam_%d.so", n);

    if (copyFile(CAM_TEMPLATE, path) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NvmmCam",
                "cannot copy %s: %s", CAM_TEMPLATE, strerror(errno));
        return NULL;
    }
    patchSoname(path, tag);

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    __android_log_print(ANDROID_LOG_INFO, "NvmmCam",
            "private NvMM camera core: %s -> %p (%s)", path, handle,
            handle ? "" : dlerror());
    if (handle != NULL && gNvmmHandleCount < 8) {
        gNvmmHandles[gNvmmHandleCount++] = handle;
    }
    return handle;
}

/* The blob dlopen()s its camera core several times per session; hand out the
 * same private copy until the session ends (see pisces_cam_reset_nvmm). */
static void *gSessionNvmm;

/*
 * Per-session private copy of libnvmm_camera.so.
 *
 * The copy exists so that a *single provider process* can serve more than one
 * camera session: the core keeps per-process static state (the focus motor
 * guard that makes the second NvMMCameraOpen fail with 0xd) and never resets
 * it.  When the wrapper restarts the provider after every close
 * (persist.pisces.camkill, the default) the next session already runs in a
 * fresh process, so the copy is pure overhead (1 MB written to /data per
 * camera close) and defaults to off there.  With camkill=0 it defaults to on.
 * persist.pisces.campriv overrides the default.
 */
static int privateNvmmEnabled(void)
{
    char value[PROP_VALUE_MAX];
    if (property_get("persist.pisces.campriv", value, NULL) >= 1 && value[0] != '\0') {
        return value[0] != '0';
    }
    if (property_get("persist.pisces.camkill", value, "1") >= 1 && value[0] == '1') {
        return 0;
    }
    return 1;
}

static void *getSessionNvmmCamera(void)
{
    if (!privateNvmmEnabled()) {
        return NULL;
    }
    if (gSessionNvmm == NULL) {
        gSessionNvmm = loadPrivateNvmmCamera();
    }
    return gSessionNvmm;
}

/* Called from the wrapper when a camera is closed: drop the private copies. */
extern "C" __attribute__((visibility("default")))
void pisces_cam_reset_nvmm(void)
{
    /* The NvMM keeps pointers into the camera core until its own unload path
     * runs, so unloading here crashed the provider (SIGSEGV in the HwBinder
     * thread).  Keep the copies loaded instead; each session gets a new one
     * through a unique SONAME. */
    gSessionNvmm = NULL;
    __android_log_print(ANDROID_LOG_INFO, "NvmmCam", "session %d done",
            gNvmmSession);
}

extern "C" __attribute__((visibility("default")))
void *__android_dlopen_ext(const char *filename, int flags, const void *extinfo)
{
    static void *(*real_ext)(const char *, int, const void *) = NULL;
    if (real_ext == NULL) {
        real_ext = (void *(*)(const char *, int, const void *))
                dlsym(RTLD_NEXT, "__android_dlopen_ext");
    }

    if (filename != NULL && strstr(filename, "nvmm_camera") != NULL &&
            strstr(filename, "nvmmcam_") == NULL) {
        void *handle = getSessionNvmmCamera();
        if (handle != NULL) {
            return handle;
        }
    }
    return (real_ext != NULL) ? real_ext(filename, flags, extinfo) : NULL;
}


