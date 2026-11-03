# android_hardware_nvidia

NVIDIA Tegra HAL sources for Xiaomi Mi 3 TD (pisces, Tegra 4 / T114),
used by LineageOS 16.0 (branch `lineage-16.0`).

- `pisces/lights`     — lights.pisces (backlight/notification LED)
- `pisces/vibrator`   — vibrator.pisces (Immersion/Tegra vibrator)
- `pisces/camera`     — camera.pisces legacy wrapper + nvcamera config
- `pisces/shims`      — libshim_icuuc / libshim_list for old blobs
- `pisces/immersion`  — immvibed + libImmVibeJ

These were moved out of the device tree so HAL code lives in hardware/.
