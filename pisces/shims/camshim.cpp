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

#include <sensor/SensorManager.h>
#include <sensor/SensorEventQueue.h>
#include <utils/String16.h>
#include <utils/String8.h>

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
