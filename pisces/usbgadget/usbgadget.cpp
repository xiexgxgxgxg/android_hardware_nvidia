/*
 * android.hardware.usb.gadget@1.0 implementation for pisces.
 *
 * Android 9's UsbDeviceManager talks to IUsbGadget instead of writing
 * sys.usb.config itself, and the AOSP implementation drives a configfs gadget
 * (/config/usb_gadget) that this 3.4 kernel does not have - it only has the
 * legacy android_usb gadget.  Without this service every function switch (MTP,
 * PTP, USB tethering) failed with
 *   "Could not find service hosting interface
 *    android.hardware.usb.gadget@1.0::IUsbGadget/default"
 * and the tethering UI toggled on and straight back off.
 *
 * This maps the HAL's function bits onto the legacy sysfs interface.
 */

#define LOG_TAG "pisces-usb-gadget"

#include <android/hardware/usb/gadget/1.0/IUsbGadget.h>
#include <android/hardware/usb/gadget/1.0/IUsbGadgetCallback.h>
#include <android/hardware/usb/gadget/1.0/types.h>
#include <hidl/LegacySupport.h>
#include <cutils/properties.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>

using android::hardware::Return;
using android::hardware::Void;
using android::hardware::hidl_string;
using android::hardware::usb::gadget::V1_0::GadgetFunction;
using android::hardware::usb::gadget::V1_0::IUsbGadget;
using android::hardware::usb::gadget::V1_0::IUsbGadgetCallback;
using android::hardware::usb::gadget::V1_0::Status;

namespace {

constexpr const char* kGadgetDir = "/sys/class/android_usb/android0/";

bool writeNode(const char* name, const std::string& value) {
    std::string path = std::string(kGadgetDir) + name;
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        return false;
    }
    ssize_t rc = write(fd, value.c_str(), value.size());
    close(fd);
    return rc == (ssize_t)value.size();
}

std::string readNode(const char* name) {
    std::string path = std::string(kGadgetDir) + name;
    char buf[128] = {0};
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return "";
    }
    ssize_t rc = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (rc <= 0) {
        return "";
    }
    std::string out(buf, rc);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

class UsbGadget : public IUsbGadget {
  public:
    Return<void> setCurrentUsbFunctions(uint64_t functions,
                                        const android::sp<IUsbGadgetCallback>& callback,
                                        uint64_t timeout) override {
        (void)timeout;

        std::string fns;
        /* Data function first, adb last.  Every rule in init.pisces.rc is
         * keyed on that spelling ("mtp,adb", "ptp,adb"); building "adb,mtp"
         * instead made the property trigger miss, the callback time out and the
         * framework revert the request, which is why choosing "File transfer"
         * never stuck. */
        if (functions & GadgetFunction::MTP) {
            fns = "mtp";
        } else if (functions & GadgetFunction::PTP) {
            fns = "ptp";
        } else if (functions & GadgetFunction::RNDIS) {
            fns = "rndis";
        }
        if (functions & GadgetFunction::ADB) {
            fns = fns.empty() ? "adb" : fns + ",adb";
        }
        if (fns.empty()) {
            fns = "none";
        }

        /* adb/mtp/ptp go through sys.usb.config and init's rules, which is the
         * path that has always brought the gadget up at boot; writing the sysfs
         * here as well raced with those rules and left the gadget disabled.
         * rndis is different: the legacy gadget needs a pause between
         * "enable 0" and the new composition, which init cannot do, so that one
         * is programmed directly below. */
        bool ok;
        if (functions & GadgetFunction::RNDIS) {
            std::string current = readNode("functions");
            if (current == fns) {
                ALOGI("setCurrentUsbFunctions(%llu): \"%s\" already active",
                      (unsigned long long)functions, fns.c_str());
                if (callback != nullptr) {
                    callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS);
                }
                return Void();
            }
            ok = writeNode("enable", "0");
            if (ok) {
                usleep(500 * 1000);
                /* adb and rndis cannot be composed together on this kernel:
                 * with "adb,rndis" the gadget enumerates but the RNDIS function
                 * never binds, so no netdev appears and Tethering times out and
                 * switches back off.  "rndis" alone works (verified: the host
                 * gets a usbteth interface). */
                fns = "rndis";
                ok = writeNode("functions", fns);
            }
            if (ok) {
                const char* product = "4ee3";
                writeNode("idVendor", "18d1");
                writeNode("idProduct", product);
                writeNode("bDeviceClass", "0");
                usleep(100 * 1000);
                ok = writeNode("enable", "1");
                /* The kernel creates rndis0 but leaves it DOWN; nothing in the
                 * framework brings it up on its own, so EthernetTracker reports
                 * it as down, the IpClient never starts, no DHCP runs and
                 * Tethering concludes the interface is inactive and gives up.
                 * Bring it up here so the link-state event fires. */
                /* rndis0 comes up on its own and EthernetTracker sees the
                 * link-up event without help; forcing a down/up bounce here
                 * only raced with the framework and made Tethering give up. */
                usleep(300 * 1000);
            }
        } else {
            ok = property_set("sys.usb.config", fns.c_str()) == 0;
            if (ok) {
                for (int i = 0; i < 20; i++) {
                    char state[PROPERTY_VALUE_MAX] = {0};
                    property_get("sys.usb.state", state, "");
                    if (fns == state) {
                        break;
                    }
                    usleep(50 * 1000);
                }
                char state[PROPERTY_VALUE_MAX] = {0};
                property_get("sys.usb.state", state, "");
                ok = (fns == state);
            }
        }

        ALOGI("setCurrentUsbFunctions(%llu) -> \"%s\" %s",
              (unsigned long long)functions, fns.c_str(), ok ? "ok" : "FAILED");

        /* UsbDeviceManager only accepts Status::SUCCESS here; anything else
         * (including FUNCTIONS_APPLIED) makes it log "Setting default fuctions"
         * and revert the request, which is what made USB tethering switch back
         * off immediately. */
        if (callback != nullptr) {
            callback->setCurrentUsbFunctionsCb(functions, ok ? Status::SUCCESS : Status::ERROR);
        }
        return Void();
    }

    Return<void> getCurrentUsbFunctions(const android::sp<IUsbGadgetCallback>& callback) override {
        /* The kernel reports the functions it actually bound, and adb is bound
         * as ffs here (see init.pisces.rc), so map both names to ADB. */
        std::string fns = readNode("functions");
        uint64_t bits = 0;
        if (fns.find("adb") != std::string::npos) bits |= GadgetFunction::ADB;
        if (fns.find("ffs") != std::string::npos) bits |= GadgetFunction::ADB;
        if (fns.find("mtp") != std::string::npos) bits |= GadgetFunction::MTP;
        if (fns.find("ptp") != std::string::npos) bits |= GadgetFunction::PTP;
        if (fns.find("rndis") != std::string::npos) bits |= GadgetFunction::RNDIS;
        if (fns.find("midi") != std::string::npos) bits |= GadgetFunction::MIDI;

        if (callback != nullptr) {
            callback->getCurrentUsbFunctionsCb(bits, Status::SUCCESS);
        }
        return Void();
    }

  private:

};

}  // namespace

int main() {
    android::sp<UsbGadget> service = new UsbGadget();
    if (service->registerAsService("default") != android::OK) {
        return 1;
    }
    /* keep the process alive, otherwise the service disappears immediately */
    android::hardware::joinRpcThreadpool();
    return 0;
}
