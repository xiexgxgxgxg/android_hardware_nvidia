/*
 * Legacy GraphicBuffer constructor used by the Tegra 4 hwcomposer blob:
 *   GraphicBuffer(w, h, format, usage, stride, handle, keepOwnership)
 * Android 9 has an extra layerCount parameter, so provide the old symbol
 * and forward to the current constructor.
 */
#include <stdint.h>
#include <stdlib.h>

// The legacy blobs allocate objects (notably android::GraphicBuffer) with a
// sizeof() taken from their own, older headers. Android 9 classes are bigger,
// so constructions overflow the allocation and the destructor later faults.
// libshim_atomic is the first NEEDED library of those blobs, therefore these
// replacements are picked for their operator new/delete calls.
#define pisces_operator_new 1
extern "C" void* pisces_padded_alloc(size_t size) {
	void* p = malloc(size + 512);
	if (p == nullptr) {
		abort();
	}
	return p;
}

void* operator new(size_t size) { return pisces_padded_alloc(size); }
void* operator new[](size_t size) { return pisces_padded_alloc(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
#include <cutils/native_handle.h>
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
	// The legacy blob and libui must not free the same native handle, or
	// the GraphicBuffer destructor runs on an already-freed handle. Give
	// GraphicBuffer its own copy when the caller passes one in.
	native_handle_t* owned = handle;
	if (handle != nullptr) {
		native_handle_t* clone = native_handle_clone(handle);
		if (clone != nullptr) {
			owned = clone;
		}
	}
	new (self) android::GraphicBuffer(
		width, height, static_cast<android::PixelFormat>(format),
		1 /* layerCount */, usage, stride, owned, keep);
}
