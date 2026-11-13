# Minimal Meson6 G04 recovery profile

`meson6_g04_recovery_defconfig` is for VB100a Pro / AML8726-MX recovery
and NAND driver bring-up. It is not a complete Android 10 configuration.

The previous defconfig was inherited from `multi_v7`: it enabled unrelated
SoCs, buses and peripherals. The reduced profile targets Meson6 only,
keeps SMP with two Cortex-A9 CPUs, and builds the required drivers in-tree
without loadable modules.

## Retained interfaces

- Raw MTD/NAND, character and block access, the G04 vendor-compatible
  controller, loop devices and DOS partition parsing.
- Meson UART, GPIO and I2C; `pinctrl-single`; Goodix GT82X input events.
- The U-Boot-initialised simple framebuffer and framebuffer console.
- Meson6 USB2 PHY, fixed USB voltage regulators, DWC2 peripheral mode and
  the legacy `/dev/android_adb` interface used by the recovery ramdisk.
- High memory, the existing 64-MiB CMA reservation, VFP/NEON, procfs,
  sysfs, devtmpfs, tmpfs, ext4 and FAT with the existing encodings.
- Root-shell sockets, TCP support, debugfs, kallsyms, embedded config and
  the early persistent RAM console at `0x8a300000`.
- Android ashmem, used by the legacy TWRP `libcutils` graphics allocator.
  Its Kconfig is under STAGING in Linux 3.19, so that menu is enabled;
  unrelated staging drivers and Android timed output remain disabled.

Meson now explicitly selects PINCTRL. The former multi-platform config
obtained it indirectly through other platforms, so merely deleting those
platforms would silently disable the board's pin mux driver.

## Removed from this profile

Other SoC families, PCI, SATA/SCSI, network interface and Wi-Fi drivers,
USB host controllers, cameras, audio, DRM, PS/2/HID, SPI peripherals,
unrelated power/regulator and sensor drivers, RTC devices, LEDs, IIO,
PWM, EDAC and DMA engine drivers are not selected. Performance events,
cgroups, modules, tracing and debug information are disabled.

Debugfs is deliberately retained for driver diagnosis; it selects
DEBUG_KERNEL in this old tree. Heavy tracing and scheduler debug output
remain disabled.

## NAND policy

The source changes accompanying this profile remove automatic BBT repair,
prevent env/BBT updates and markbad operations, validate the saved env CRC
and stock partition records, and preserve explicit writes to normal MTD
partitions. The encrypted bootloader partition is exposed read-only.

The stock normal layout has recovery and boot partitions of 8 MiB each,
system 768 MiB, factory/cache 128 MiB each and userdata 2304 MiB. The final
raw NFTL_Part occupies the remainder. NFTL and UBI initialisation are
disabled; this diagnostic profile does not provide `avnftl*` block devices.

The NAND core advances data/OOB requests in the controller's virtual page
units and erase requests in virtual block units. For the verified two-CE,
two-plane device these are 32-KiB pages and 8-MiB erase blocks. Capacity
is checked against the observed 16-GiB geometry, not forced to that value.

## SD support boundary

MMC/block and FAT support are retained, but this 3.19 tree has no Meson6
SD host driver or corresponding G04 device-tree node. Linux access to the
card still needs that port. SD boot itself is performed by U-Boot, which
loads the kernel and embedded initramfs into RAM before Linux starts.

## Configuration-only validation

With `kconfiglib==14.1.0` installed, run from the kernel checkout:

```sh
python3 scripts/check-g04-recovery-config.py \
  --resolved-config /tmp/g04-recovery.config \
  --report /tmp/g04-recovery-config-check.json
```

`--baseline /path/to/previous.config` additionally reports the previous
symbol counts. The checker evaluates Kconfig dependencies and verifies
51 required built-in symbols, 35 disabled symbols and the CPU/CMA limits.
It does not invoke make, a compiler, the linker or a device.

The initial reduced profile evaluated 436 built-in symbols, down from 1461,
and zero module symbols, down from 52. These are configuration counts,
not a measured build-time speedup. No kernel or DTB was built or booted
for this change.

## First boot observation

The first SD test booted Linux 3.19.8 with root ADB and the Goodix input
device, with the NAND DT node disabled. TWRP repeatedly exited with
SIGSEGV before displaying its menu. A core dump places the fault in
`libcutils` `mspace_malloc` with a NULL heap pointer; that kernel did not
provide `/dev/ashmem`. The profile now requires ASHMEM so this recovery
can create its graphics heap. Hardware validation of this correction is
recorded separately; a successful kernel boot alone does not verify the UI.

The Python parser normalises a doubled line continuation in the inherited
Intel sound Kconfig only in its read view. The source file is unchanged.
Existing global Kconfig style/type warnings are distinguished from new
profile warnings.

For a later authorised build, use the checked-in defconfig directly and
set INITRAMFS_SOURCE to the intended recovery ramdisk. The historical
`build_g04_319_min_recovery_codespaces.py` and its image verifier target a
different NFTL layout and must not be used unchanged for this profile.
