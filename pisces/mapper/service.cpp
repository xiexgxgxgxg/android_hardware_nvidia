/*
 * Passthrough IMapper (gralloc) service for pisces.
 * Android 9 SurfaceFlinger waits for android.hardware.graphics.mapper@2.0;
 * our gralloc is a gralloc0 module, which GrallocLoader wraps via Gralloc0Hal.
 */
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <hidl/LegacySupport.h>

using android::hardware::graphics::mapper::V2_0::IMapper;

int main() {
    return android::hardware::defaultPassthroughServiceImplementation<IMapper>("default");
}
