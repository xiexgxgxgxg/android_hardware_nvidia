/*
 * Legacy GraphicBuffer constructor used by the Tegra 4 hwcomposer blob:
 *   GraphicBuffer(w, h, format, usage, stride, handle, keepOwnership)
 * Android 9 has an extra layerCount parameter, so provide the old symbol
 * and forward to the current constructor.
 */
#include <stdint.h>
#include <ui/GraphicBuffer.h>

extern "C" void shim_GraphicBuffer_ctor(void* self, uint32_t width,
					uint32_t height, int32_t format,
					uint32_t usage, uint32_t stride,
					native_handle_t* handle, bool keep)
	__asm__("_ZN7android13GraphicBufferC1EjjijjP13native_handleb");

extern "C" void shim_GraphicBuffer_ctor(void* self, uint32_t width,
					uint32_t height, int32_t format,
					uint32_t usage, uint32_t stride,
					native_handle_t* handle, bool keep)
{
	new (self) android::GraphicBuffer(
		width, height, static_cast<android::PixelFormat>(format),
		1 /* layerCount */, usage, stride, handle, keep);
}
