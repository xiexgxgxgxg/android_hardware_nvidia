/*
 * Legacy android_atomic_* helpers for pre-Android-7 Tegra blobs.
 * libcutils stopped exporting them in Android 9.
 */
#include <stdint.h>

int32_t android_atomic_acquire_load(volatile const int32_t *addr)
{
	return __atomic_load_n(addr, __ATOMIC_ACQUIRE);
}

int32_t android_atomic_release_load(volatile const int32_t *addr)
{
	return __atomic_load_n(addr, __ATOMIC_RELAXED);
}

void android_atomic_acquire_store(int32_t value, volatile int32_t *addr)
{
	__atomic_store_n(addr, value, __ATOMIC_RELEASE);
}

void android_atomic_release_store(int32_t value, volatile int32_t *addr)
{
	__atomic_store_n(addr, value, __ATOMIC_RELAXED);
}

int android_atomic_inc(volatile int32_t *addr)
{
	return __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
}

int android_atomic_dec(volatile int32_t *addr)
{
	return __atomic_fetch_sub(addr, 1, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_add(int32_t value, volatile int32_t *addr)
{
	return __atomic_fetch_add(addr, value, __ATOMIC_SEQ_CST);
}

int android_atomic_cmpxchg(int32_t oldvalue, int32_t newvalue,
			   volatile int32_t *addr)
{
	return __atomic_compare_exchange_n(addr, &oldvalue, newvalue, 0,
					   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
		       ? 0
		       : 1;
}

int android_atomic_or(int32_t value, volatile int32_t *addr)
{
	return __atomic_fetch_or(addr, value, __ATOMIC_SEQ_CST);
}

int android_atomic_and(int32_t value, volatile int32_t *addr)
{
	return __atomic_fetch_and(addr, value, __ATOMIC_SEQ_CST);
}
