Kernel source for the General Mobile E-Tab5 (hulkbuster)
=========================================================

Open-source Linux 3.10 kernel for the General Mobile E-Tab5, used by the
LineageOS 14.1 device tree
[android_device_generalmobile_hulkbuster](https://github.com/enesuzun2002/android_device_generalmobile_hulkbuster).
General Mobile never released the source; this tree started from the MediaTek
MT6592 3.10.72 kernel of the Fly Tornado One and was brought up for the E-Tab5
by recovering board values from the stock kernel binary.

Basic   | Spec sheet
-------:|:-------------------------
SoC     | MediaTek MT6592, 8x Cortex-A7 up to 1.66 GHz
GPU     | Mali-450 MP4
Memory  | 2 GB RAM
Display | 10.1" 1920x1200 (NT71397, MIPI DSI video mode)
Network | Wi-Fi + Bluetooth (MT6592 combo), no modem

Branches
--------

Branch | Contents
------ | --------
`hulkbuster-3.10.108` | **Current.** `hulkbuster-3.10.72` plus linux-stable 3.10.73 through 3.10.108, one commit per release.
`hulkbuster-3.10.72`  | The first booting tree, before the stable updates.

On the 3.10.108 branch, every stable release is its own commit
(`Linux 3.10.N (stable)`). When upstream and MTK code conflicted, the MTK change
was kept and the upstream fix applied on top of it. If something breaks,
bisect by release.

Building
--------

Use the GCC 4.9 toolchain from the LineageOS 14.1 tree; newer compilers are
not supported by this 3.10 tree.

```sh
export CROSS_COMPILE=<los-14.1>/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel-
make O=out ARCH=arm hulkbuster_defconfig
make O=out ARCH=arm -j$(nproc) zImage
```

The output is `out/arch/arm/boot/zImage`. Inside the LineageOS tree, the device
tree's BoardConfig builds this kernel automatically (`TARGET_KERNEL_SOURCE :=
kernel/mediatek/mt6592`, `TARGET_KERNEL_CONFIG := hulkbuster_defconfig`).

### Boot image

The MTK bootloader expects the kernel inside `boot.img` to have a 512-byte MTK
header in front of it (magic `0x58881688`, name `KERNEL`, then the zImage size).
The Lineage build adds it. To swap only the kernel in an existing `boot.img`,
keep the header and ramdisk, replace the zImage, and update the kernel size in
both the MTK header and the Android boot header.

### Flashing (from recovery, over adb)

```sh
adb push boot.img /tmp/boot.img
adb shell 'dd if=/tmp/boot.img of=/dev/bootimg bs=2048 && sync'
```

Use `bs=2048`. Reading `/dev/bootimg` back with large block sizes returns
corrupted data, so do not use a 1 MB-block dump as a base image.

Hardware status
---------------

Working: display, touch (FT5x06) with the three capacitive keys and their
backlight, volume keys, Wi-Fi, Bluetooth, audio (speaker and headset), charging
(BQ24196), battery gauge, accelerometer (BMA255), gyroscope (BMG160), compass
(BMM050), light/proximity sensor (CM36283), SD card, USB (MTP/adb), suspend and
resume.

* Volume Down is the PMIC FCHR key. It is flaky on this hardware, and on the
  stock kernel too.
* There is no barometer, and no modem (the CCCI driver is disabled; it
  deadlocks the clock framework on this Wi-Fi-only board).
* Cameras are configured from the stock pin map but not tested.

Board notes
-----------

All values below come from disassembling the stock kernel; the original tree
had the Fly/P55 values.

* **DCT**: `arch/arm/mach-mt6592/hulkbuster/dct/dct/` holds the complete
  DrvGen output (`cust_eint.h`, `cust_gpio_usage.h`, `cust_gpio_boot.h`,
  `cust_kpd.h`, `cust_adc.h`, `cust_power.h`, `pmic_drv.*`, ...). The set must
  stay complete; otherwise the build regenerates it from `codegen.dws` and edits
  to `inc/` headers are silently ignored.
* MT6333 EINT 8, touch GPIO13 (reset) / GPIO14 (EINT14), ALS EINT18, accdet
  EINT168 (active high), SD card detect EINT9, speaker amp GPIO20, charger CE
  GPIO19.
* Sensors on I2C bus 2: BMA255 @0x10, BMG160 @0x68, BMM050 @0x12 (all direction
  5), CM36283 @0x60 with the stock ALS tables; BQ24196 @0x6b.
* Panel NT71397: burst video mode, 4 lanes, PLL 441, porches 2/18/15 (vertical)
  and 16/32/16 (horizontal), panel enables on GPIO119 and GPIO118.
* `CONFIG_MTK_RAM_CONSOLE_DRAM_ADDR` must be `0x83F00000`. Outside DRAM, it
  crashes before the first log line.

Debugging
---------

`hulkbuster_defconfig` is a release (user) config. To chase a crash or a lock
bug, re-enable what you need:

```
CONFIG_LOCALVERSION=".debug"
CONFIG_MT_ENG_BUILD=y
CONFIG_PROVE_LOCKING=y          # lockdep (+ DEBUG_LOCK_ALLOC, LOCKDEP)
CONFIG_DEBUG_SPINLOCK=y
CONFIG_DEBUG_MUTEXES=y
CONFIG_SLUB_DEBUG_ON=y          # slab red zones / poisoning
CONFIG_DETECT_HUNG_TASK=y
```

These make the system noticeably slower (lockdep and SLUB poisoning hit every
lock and allocation), so turn them off again for daily use.

* After a crash, the previous boot's log is in `/proc/last_kmsg`, but only
  when you boot this kernel again. The stock recovery runs its own 3.4 kernel
  and does not see it.
* `adb root` hangs adbd on this device until USB is replugged. Use the
  bugreport, logcat, and dmesg paths that work without root.
* Until `/proc/bootprof` receives `0`, the kernel mirrors every message to
  the debug UART, synchronously. The ROM's init does this at
  `sys.boot_completed=1`.

Credits
-------

MediaTek, the Fly Tornado One (j608) kernel source this tree is based on, and
the linux-stable maintainers.
