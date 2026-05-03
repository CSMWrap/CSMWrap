# CSMWrap [![Build Status](https://github.com/CSMWrap/CSMWrap/actions/workflows/build.yml/badge.svg)](https://github.com/CSMWrap/CSMWrap/actions/workflows/build.yml) [![Discord](https://img.shields.io/discord/1390940493873025074?color=5865F2&label=Discord&logo=discord&logoColor=white)](https://discord.gg/3CCgJpzNXH)

CSMWrap is an EFI application designed to be a drop-in solution to enable legacy BIOS booting on modern UEFI-only (class 3) systems.
It achieves this by wrapping a Compatibility Support Module (CSM) build of the [SeaBIOS project](https://www.seabios.org/)
as an out-of-firmware EFI application, effectively creating a compatibility layer for traditional PC BIOS operation.

## Executive Summary

The idea is to drop the 64-bit or 32-bit version of CSMWrap (depending on the hardware, dropping both also works) into a `/EFI/BOOT/`
directory on a FAT (12, 16, or 32) partition on the medium containing the legacy BIOS OS. UEFI firmware will pick this up and show the
medium as a bootable device. Ideally, that's all that would be needed.

1. **Download:** Get the latest `csmwrap<ARCH>.efi` from the [Releases page](https://github.com/CSMWrap/CSMWrap/releases).
2. **Deploy:** Copy `csmwrap<ARCH>.efi` to a FAT-formatted partition, typically as `/EFI/BOOT/BOOTX64.EFI` (for 64-bit)
  or `/EFI/BOOT/BOOTIA32.EFI` (for 32-bit) (the hardcoded path is needed so that the firmware picks it up automatically).
3. **Boot:** Select the UEFI boot entry for the drive onto which CSMWrap was deployed.

It is highly recommended that the partition table used is MBR (MS-DOS partition table), as UEFI firmwares are perfectly capable of
booting off of this format, and because it is the most compatible with most legacy OSes one may want to boot.

## Additional Prerequisites

### Secure Boot

Secure boot should be disabled unless one wants to manually sign the CSMWrap EFI application, which is possible, but beyond the
scope of this README.

### Firmware Settings

CSMWrap is designed to be as drop-in as possible, without requiring changes to firmware for settings that may not even be exposed
(depending on the firmware), or that might conflict with other UEFI OSes being multi-booted on the system. That said, if at all
possible, disabling these settings is highly recommended for best legacy OS compatibility:

1. **X2APIC**

Additional settings to try to disable if things still do not work (ideally this should *not* be necessary, please report an issue
if you need this on your hardware!):

1. **Above 4G Decoding**
2. **Resizable BAR/Smart Access Memory**

### Video Card Considerations

CSMWrap also wraps the "SeaVGABIOS" module of SeaBIOS for providing a bare bones implementation of a legacy Video BIOS. That said,
SeaVGABIOS is far from ideal, and many, **many** things requiring more direct access to legacy video modes won't work properly
(e.g. pretty much all MS-DOS games, MS-DOS Editor, etc.). More modern OSes using the VESA BIOS extensions (VBE) standard only
(e.g. more modern Windows NT, Linux, etc.) should still work fine, though.

Therefore it is **highly recommended**, if possible, to install a legacy-capable video card. If one is present, its Video BIOS
will be used instead of SeaVGABIOS, providing a much better, pretty much native-like, experience.

## Configuration

CSMWrap supports an optional INI-style configuration file. Place a file named `csmwrap.ini` in the same directory as the CSMWrap
EFI executable (e.g. `/EFI/BOOT/csmwrap.ini`). If the file is absent, sensible defaults are used.

### Options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `serial` | bool | `false` | Enable serial debug output |
| `serial_port` | hex/int | `0x3f8` | Serial I/O port address (COM1=`0x3f8`, COM2=`0x2f8`, COM3=`0x3e8`, COM4=`0x2e8`) |
| `serial_baud` | int | `115200` | Serial baud rate |
| `vgabios` | string | *(empty)* | Path to a custom VBIOS file on the ESP (e.g. `\EFI\CSMWrap\vgabios.bin`). When empty, the card's built-in OpROM is used, and, failing that, SeaVGABIOS is used |
| `iommu_disable` | bool | `true` | Disable IOMMUs (Intel VT-d / AMD-Vi) before legacy boot |
| `verbose` | bool | `false` | Show debug output on screen via Flanterm |
| `vga` | PCI address | *(empty)* | PCI address of the VGA card to use (e.g. `00:02.0`). Format: `BB:DD.F` (hex). When empty, the first available card is used |
| `system_thread` | int/hex | *(auto)* | APIC ID of the logical CPU to reserve as the CSMWrap system thread (see [the FAQs](#frequently-asked-questions)). Must be an enabled AP (not the BSP) and have an APIC ID below `0xFF`/`255`. When empty, CSMWrap auto-picks the highest-ID AP below `0xFF`/`255`. The selected CPU is hidden from the OS in both the MADT and the MP table |
| `cpu_allowlist` | int/hex list | *(unset)* | Comma-separated list of APIC IDs of logical CPUs that should be exposed to the OS in the MADT and the MP table. Each entry is either a single ID or an inclusive range `N-M` (e.g. `0,2-4,7`). An empty value (`cpu_allowlist =`) is itself a setting and means "hide every AP" (only the BSP stays visible). The BSP is always exposed regardless. The system thread is always hidden regardless. Mutually exclusive with `cpu_blocklist` |
| `cpu_blocklist` | int/hex list | *(unset)* | Comma-separated list of APIC IDs of logical CPUs that should be hidden from the OS in the MADT and the MP table. Each entry is either a single ID or an inclusive range `N-M` (e.g. `5-7`). An empty value (`cpu_blocklist =`) is a no-op for visibility but still claims the slot, so `cpu_allowlist` cannot also be set. The BSP is always exposed regardless. The system thread is always hidden regardless. Mutually exclusive with `cpu_allowlist` |

Boolean values accept `true`/`yes`/`1` and `false`/`no`/`0` (case-insensitive). Comments start with `;` or `#`.

### Example

```ini
; CSMWrap configuration
serial = true
serial_port = 0x3f8
serial_baud = 115200
vgabios = \EFI\CSMWrap\vgabios.bin
iommu_disable = true

; Pin the system thread to APIC ID 7 and hide APIC IDs 4 through 6 from the OS.
system_thread = 7
cpu_blocklist = 4-6
```

## Frequently Asked Questions

### Is this an emulator?

No! At least not in the sense of it being a full-screened emulator window. Running a legacy OS with CSMWrap means that it is *natively*
running on the system. CSMWrap attempts to recreate, natively, and as closely as possible, a legacy BIOS PC environment on modern
UEFI class 3 systems.

### I booted a multi-core capable OS and I am missing a logical processor (thread), what gives?

This is expected. CSMWrap reserves 1 logical processor for "system" use due to the limitations of running out-of-firmware and not being able to
use [SMM (System Management Mode)](https://en.wikipedia.org/wiki/System_Management_Mode).

Therefore, this means that CSMWrap **does not support running on systems with only 1 logical processor (i.e. only 1 core and no SMT/hyperthreading)**.
That said, most systems that CSMWrap targets (i.e. modern UEFI class 3 systems) will definitely have way more than a single logical processor,
so this is mostly a non-issue.

### Does CSMWrap have any advantages over native CSM?

Yes! Native CSM firmware is often riddled with issues and hardly tested against legacy OSes anymore. CSMWrap ships a reliable, free, and open-source
legacy BIOS implementation - SeaBIOS - and it is tested against legacy OSes. Issues affecting modern, commonly shipped CSM implementations do
not affect CSMWrap, like for example:

- Dirty control register values at handoff. (This is something that [cregfix](https://github.com/mintsuki/cregfix) was created to work around).
- Legacy BIOS routines failing to reliably run when called from Virtual 8086 Mode. EMM386, Windows 3.x under 386 enhanced mode, and more, are affected
  by this issue and it results in crashes. The reason for this is a bit technical for this README file, but CSMWrap is not affected.

And when it comes to improvements that are not necessarily bugs in CSM implementations, CSMWrap, amongst other things:

- Generates MP tables for legacy OSes that support the legacy Intel MultiProcessor Specification standard but not ACPI.
- Allows one to select a non-primary video card for VGA output which native CSM implementations do not allow. This is useful for
  multi-booting modern and legacy OSes.

## Contributing

Contributions are welcome! Whether it's reporting bugs, suggesting features, improving documentation, or submitting code changes, your help is appreciated.

Additionally, one can join our [Discord server](https://discord.gg/3CCgJpzNXH) for any project-related discussion, or to otherwise chat with likeminded
people.

## Credits & Acknowledgements

*   The **[SeaBIOS project](https://www.seabios.org/)** for their CSM and VBIOS code.
*   **[PicoEFI](https://codeberg.org/PicoEFI/PicoEFI)** for the EFI C runtime, build system, and headers.
*   **[EDK2 (TianoCore)](https://github.com/tianocore/edk2)** for UEFI specifications and some code snippets.
*   **[uACPI](https://github.com/uACPI/uACPI)** for ACPI table handling.
*   **@CanonKong** for test feedback and general knowledge.
*   All contributors and testers from the community!
