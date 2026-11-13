#!/usr/bin/env python3
"""Resolve and check the G04 recovery Kconfig without invoking make or a compiler.

Requires kconfiglib==14.1.0. Optionally compares a previous full .config.
"""

import argparse
import io
import json
import os
from pathlib import Path

import kconfiglib


REQUIRED = (
    "ARCH_MULTI_V7", "ARCH_MESON", "MACH_MESON6", "SMP", "HIGHMEM",
    "CMA", "DMA_CMA", "VFP", "NEON", "ARM_APPENDED_DTB",
    "ARM_ATAG_DTB_COMPAT", "MESON6_G04_RAW_BOOT_DTB", "BINFMT_ELF",
    "BINFMT_SCRIPT", "DEVTMPFS", "PROC_FS", "SYSFS", "TMPFS", "UNIX",
    "MTD", "MTD_BLOCK", "MTD_NAND", "MTD_NAND_MESON6_AML",
    "PINCTRL", "PINCTRL_SINGLE", "GPIOLIB", "GPIO_MESON6", "I2C_MESON",
    "REGULATOR_FIXED_VOLTAGE", "TOUCHSCREEN_GT82X_G04", "INPUT_EVDEV",
    "SERIAL_MESON_CONSOLE", "FB_SIMPLE", "FRAMEBUFFER_CONSOLE",
    "USB_SUPPORT", "USB_GADGET", "USB_DWC2", "USB_DWC2_PERIPHERAL",
    "USB_DWC2_PLATFORM", "PHY_MESON6_USB2", "USB_G_ANDROID_ADB",
    "MMC", "MMC_BLOCK", "MSDOS_PARTITION", "EXT4_FS", "VFAT_FS",
    "ANDROID_RAM_CONSOLE", "ANDROID_RAM_CONSOLE_EARLY_INIT", "DEBUG_FS",
    "STAGING", "ASHMEM",
)
FORBIDDEN = (
    "MACH_MESON8", "MODULES", "PERF_EVENTS", "CGROUPS", "PCI", "SCSI",
    "ATA", "NETDEVICES", "WIRELESS", "RFKILL", "CAN", "IPV6",
    "INPUT_MOUSEDEV", "SERIO", "HID", "SPI", "MTD_UBI",
    "MTD_NAND_MESON6_AML_NFTL", "SOUND", "MEDIA_SUPPORT", "DRM", "USB",
    "USB_DWC2_HOST", "USB_DWC2_DUAL_ROLE", "ANDROID_TIMED_OUTPUT", "EDAC", "IIO",
    "PWM", "DMADEVICES", "DEBUG_INFO", "FTRACE", "LOCKUP_DETECTOR",
    "SQUASHFS", "NFS_FS", "NTFS_FS",
)


class LegacyKconfig(kconfiglib.Kconfig):
    """Accept the doubled continuation in the inherited Intel sound Kconfig.

    Only the parser's read view is normalised; the source file is not changed.
    """

    def _open(self, filename, mode):
        stream = super()._open(filename, mode)
        if mode != "r":
            return stream
        data = stream.read()
        stream.close()
        return io.StringIO(data.replace("\\\\\n", "\\\n"))


def counts(kconf):
    return {
        "built_in_symbols": sum(s.str_value == "y" for s in kconf.unique_defined_syms),
        "module_symbols": sum(s.str_value == "m" for s in kconf.unique_defined_syms),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--defconfig", type=Path,
                        default=Path("arch/arm/configs/meson6_g04_recovery_defconfig"))
    parser.add_argument("--resolved-config", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = args.kernel_dir.resolve()
    defconfig = args.defconfig if args.defconfig.is_absolute() else root / args.defconfig
    resolved = args.resolved_config.resolve()
    baseline = args.baseline.resolve() if args.baseline else None
    report_path = args.report.resolve() if args.report else None
    os.environ.update(ARCH="arm", SRCARCH="arm", srctree=root.as_posix(), KERNELVERSION="3.19.8")
    os.chdir(root)
    kconf = LegacyKconfig("Kconfig", warn_to_stderr=False)
    # Old global Kconfig files emit unrelated style warnings with a modern
    # parser. Evaluate the defaults once, then distinguish profile warnings.
    counts(kconf)
    inherited_warnings = list(kconf.warnings)
    before = None
    if baseline:
        kconf.load_config(str(baseline))
        before = counts(kconf)
    kconf.warnings.clear()
    kconf.load_config(str(defconfig))
    after = counts(kconf)
    errors = []
    for key in REQUIRED:
        if kconf.syms[key].str_value != "y":
            errors.append(f"{key} must remain built-in")
    for key in FORBIDDEN:
        if kconf.syms[key].str_value != "n":
            errors.append(f"{key} must remain disabled")
    for key, expected in (("NR_CPUS", "2"), ("CMA_SIZE_MBYTES", "64")):
        if kconf.syms[key].str_value != expected:
            errors.append(f"{key} must equal {expected}")
    # Also catch a misspelled requested symbol or an unsatisfied dependency.
    for line in defconfig.read_text().splitlines():
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        key, value = line[7:].split("=", 1)
        sym = kconf.syms.get(key)
        if sym is None or (value in ("y", "m") and sym.str_value != value):
            errors.append(f"unmet defconfig assignment: {line}")
    for warning in kconf.warnings:
        if ("is being evaluated in a logical context" in warning and
                any(f"symbol {name} " in warning for name in ("BASE_SMALL", "DRAM_BASE"))):
            inherited_warnings.append(warning)
        else:
            errors.append(warning)
    if errors:
        raise SystemExit("\n".join(errors))
    resolved.parent.mkdir(parents=True, exist_ok=True)
    kconf.write_config(str(resolved), save_old=False)
    result = {
        "status": "passed", "before": before, "after": after,
        "required_symbols_checked": len(REQUIRED),
        "disabled_symbols_checked": len(FORBIDDEN),
        "kernel_build_performed": False,
        "hardware_validation": "not performed",
        "inherited_kconfig_warnings": len(inherited_warnings),
    }
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
