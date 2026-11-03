# android_hardware_nvidia

NVIDIA Tegra HAL 源码，用于 Xiaomi Mi 3 TD（pisces，Tegra 4 / T114）的
LineageOS 16.0。

## 分支

| 分支 | 用途 |
|---|---|
| `lineage-16.0` | **当前**：LineageOS 16.0（Android 9） |

## 目录

| 路径 | 模块 | 说明 |
|---|---|---|
| `pisces/lights` | `lights.pisces` | 背光 / 通知灯 HAL（写 `/sys/class/backlight/lm3533-*`） |
| `pisces/vibrator` | `vibrator.pisces` | Tegra 振动器 HAL |
| `pisces/camera` | `camera.pisces` | 旧版 camera HAL wrapper + `nvcamera.conf`/`model_frontal.xml` |
| `pisces/shims` | `libshim_icuuc`、`libshim_list` | 给旧 blob 用的符号 shim |
| `pisces/immersion` | `immvibed`、`libImmVibeJ` | Immersion 触觉反馈 daemon 与 JNI 库 |

这些模块原先放在 device/xiaomi/pisces 下，现按 Android 布局迁到 `hardware/nvidia/pisces`。

## 编译

模块名不变，设备树 `device_pisces.mk` 的 `PRODUCT_PACKAGES` 直接引用：

```
lights.pisces
vibrator.pisces
camera.pisces        # 暂未启用（A9 需要 camera provider HIDL）
libshim_icuuc
libshim_list
immvibed
libImmVibeJ
```

构建时放在 `hardware/nvidia` 即可，AOSP 会自动扫描子目录 `Android.mk`。

## Android 9 适配状态

- `lights.pisces`：已能在 A9 编译（补 `stdlib.h`、关闭 unused-parameter 警告）
- `vibrator.pisces`：待验证（A9 需要 `android.hardware.vibrator@1.0` HIDL 包装）
- `camera.pisces`：CM12 接口，A9 未启用；后续需要 camera provider 2.4 HIDL
- `libshim_*`：暂未启用，等旧 blob 链接错误出现再逐个恢复
