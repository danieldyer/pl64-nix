# PL64 NixOS

The remote builder is `daniel@daniel-ubuntu-vm.local`, an x86_64 Ubuntu VM.
The Mac uses ordinary SSH, rsync and scp; no local Nix installation or
shell-profile changes are needed.

The temporary EC2 builder and its 100 GiB disk were deleted on 20 October 2026.
The tested installer remains in `artifacts/`. Builds now run on the Ubuntu VM;
the installed PL64 can also build its own configuration updates.

Run the following Mac commands from this repository:

```sh
cd /Users/danielrd/sources/pl64-nix
```

## Connect

```sh
ssh daniel@daniel-ubuntu-vm.local
```

On the builder:

```sh
cd ~/pl64
nix --version
```

## Set up Nix on a fresh Ubuntu builder

From the Mac:

```sh
ssh daniel@daniel-ubuntu-vm.local 'mkdir -p ~/pl64/builder'
scp builder/bootstrap.sh daniel@daniel-ubuntu-vm.local:pl64/builder/bootstrap.sh
ssh -t daniel@daniel-ubuntu-vm.local \
  'sudo bash /home/daniel/pl64/builder/bootstrap.sh daniel'
```

Enter the Ubuntu user's sudo password in the terminal. The script installs
the official multi-user Nix daemon, enables flakes and sets four concurrent
build jobs with two cores each. Open a fresh SSH session afterward so the
official shell setup adds Nix to `PATH`. Normal builds need no sudo.

If VMware's virtual disk has been enlarged, inspect `lsblk` and `df -h /`
inside Ubuntu. On this VM, the disk is `/dev/sda` and root is ext4 on
partition 2; expand the partition and filesystem with:

```sh
sudo growpart /dev/sda 2
sudo resize2fs /dev/sda2
df -h /
```

## Sync local sources to the builder

Run on the Mac after editing files:

```sh
rsync -az \
  --exclude='/builder/' --exclude='/.git/' \
  --exclude='/artifacts/' --exclude='/result*' --exclude='.DS_Store' \
  ./ daniel@daniel-ubuntu-vm.local:/home/daniel/pl64/
```

## Build

Run on the builder:

```sh
cd ~/pl64
nix build .#installer-iso -L -o result-iso
```

To build only the installed system closure:

```sh
nix build .#system -L -o result-system
```

## Download the ISO to the Mac

```sh
mkdir -p artifacts
scp 'daniel@daniel-ubuntu-vm.local:/home/daniel/pl64/result-iso/iso/*.iso' artifacts/
```

To bring a remotely generated lock file into the repository:

```sh
scp daniel@daniel-ubuntu-vm.local:/home/daniel/pl64/flake.lock ./
```

## Install on the PL64

Write the ISO as an image to a USB stick, then boot its UEFI entry. The image is
unsigned, so disable Secure Boot. Ethernet uses DHCP; use `nmtui` for Wi-Fi.
The live console logs in as root. SSH accepts the public key in `keys/root.pub`.
Nothing is installed automatically, and the installer needs no internet access.

Identify the intended NVMe disk by model, capacity and serial number:

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL,MOUNTPOINTS
test -d /sys/firmware/efi/efivars
```

The following commands erase the selected disk. Set `PL64_DISK` to the intended
NVMe device before running them. The layout is a 1 GiB EFI partition followed
by an unencrypted ext4 root partition occupying the remaining space.

```sh
PL64_DISK=/dev/nvme0n1
parted --script "$PL64_DISK" mklabel gpt \
  mkpart ESP fat32 1MiB 1025MiB set 1 esp on \
  mkpart root ext4 1025MiB 100%
udevadm settle
mkfs.fat -F 32 -n PL64_EFI "${PL64_DISK}p1"
mkfs.ext4 -L PL64_ROOT "${PL64_DISK}p2"
mount -t ext4 "${PL64_DISK}p2" /mnt
mkdir -p /mnt/boot /mnt/etc/nixos
mount -t vfat "${PL64_DISK}p1" /mnt/boot
cp -rL /etc/pl64-config/. /mnt/etc/nixos/
chmod -R u+w /mnt/etc/nixos
nixos-install --root /mnt --no-channel-copy \
  --system "$(readlink -f /etc/pl64-system)"
```

Set a root password at the installer's prompt for local HDMI-console login.
SSH remains key-only. If installing through a non-interactive SSH session, set
the console password explicitly before rebooting:

```sh
nixos-enter --root /mnt -c passwd
umount -R /mnt
poweroff
```

Remove the USB stick and power the PL64 on. The installed system has HDMI text
consoles, NetworkManager, SSH, local man pages and standard administration,
network, storage and diagnostic tools. It has no desktop or display manager.
Avahi advertises the installed system as `pl64.local` on the local network:

```sh
ssh root@pl64.local
```

The pinned kernel is Linux 7.2.11. Intel microcode, i915 graphics firmware,
Intel Wi-Fi/Bluetooth firmware and Intel audio-DSP firmware are included.
The initrd contains NVMe/VMD and USB keyboard support rather than the generic
SATA/RAID/storage collection. Intel HWP frequency scaling, CPU idle states,
normal ACPI/PCIe/NVMe power management and suspend are retained; thermald
handles the fanless system's Intel thermal policy. Zram supplies compressed
memory swap; disk hibernation is not configured.

Use BIOS 2.10.00 or newer: BIOS 1.16.00 exposed broken thermal tables and no
active policy in thermald's adaptive mode. After updating to 2.10.00, the
original `proc_thermal_add` error disappeared and the firmware supplies working
thermal and power tables. The installed system uses the NixOS thermald
service with `services.thermald.configFile` selecting its default engine.
The small profile in `nixos/thermal-conf.xml` leaves temperatures and limits
to the firmware: an active `TCPU` zone with a 90 C passive trip, a sustained
CPU-package limit of 15 W and a short-term limit of 39 W. Adaptive mode was
observed relaxing the MSR limit while the MMIO limiter remained disabled,
so it is avoided. No custom trip temperatures or service override is needed.

The systemd-boot menu waits one second. Networking does not block console
startup on DHCP or a missing cable. Hold Space during UEFI boot to enter the
boot menu if needed. On the PL64, inspect actual boot and power behavior with:

```sh
systemd-analyze
systemd-analyze critical-chain
nmcli device status
cpupower frequency-info
sensors
systemctl suspend
```

For live temperature and CPU power readings, run as root:

```sh
watch -n 1 sensors
turbostat --quiet --Summary \
  --show TSC_MHz,Busy%,Bzy_MHz,PkgTmp,PkgWatt,SysWatt --interval 2
powertop
```

`turbostat` reports CPU-package watts, temperatures, frequency and idle-state
residency (omit `--show` for all counters). `SysWatt` is the processor's platform
energy estimate. These readings do not measure total power at the wall. `powertop`
shows activity and tunables; its power estimates on an AC-only machine can be
unavailable. Firmware power limits and supported idle states remain enabled.

The 21 October 2026 check after BIOS 2.10.00 and a fresh boot measured about
3.7 W CPU-package power at idle. A 60-second CPU load initially drew about
24 W, then settled to 15 W after the boost window, with a peak package
temperature of 67 C and no CPU thermal throttling. The power limits remained
15 W sustained / 39 W short-term. This was a brief functional check, not a
thermal heat-soak test. These limits are configurable; a future performance
profile can raise the sustained limit while retaining thermal protection.

The previous processor-thermal initialization error is gone. The boot log
still contains a Wi-Fi-related ACPI `CNVW._DSM` / missing `WIST` message;
Wi-Fi connectivity and hostname SSH were verified working.

VM validation cannot establish physical HDMI output, Intel network/Wi-Fi
compatibility, actual PL64 boot latency or suspend/resume reliability.

## Update the installed system

After syncing configuration changes to `/etc/nixos` on the PL64:

```sh
ssh -t root@pl64.local 'nixos-rebuild switch --flake /etc/nixos#pl64'
```

Use this explicit flake workflow. The image omits legacy channels and the
implicit Nixpkgs source checkout; updates fetch the inputs pinned by
`flake.lock` when needed.

## Firmware updates

The installed system enables the standard NixOS fwupd service. Check its signed
firmware catalog from the PL64 with:

```sh
fwupdmgr refresh
fwupdmgr get-updates
```

On 21 October 2026, BIOS 1.16.00 was updated to ASUS PL64 BIOS 2.10.00 using
EZ Flash at the local console. NixOS subsequently reported version 2.10.00
and ESRT confirmed a successful update to firmware version 528 (0x210).
No BIOS release matched this machine in LVFS. The ASUS download was verified
against its published SHA-256; the extracted file's SHA-256 is:

```text
41069f5c96fa310dd3c369e9e417a4949c31c140be1cd6619091412f3fccfd3c
```

For a future manual update using
[ASUS EZ Flash](https://www.asus.com/us/support/faq/1008859/), download the
matching PL64 release from ASUS, verify its checksum, and copy the extracted
`.CAP` file to `/boot/` on the PL64 or a FAT32 USB drive. Connect a keyboard
and HDMI screen, then enter firmware setup:

```sh
systemctl reboot --firmware-setup
```

Choose **Advanced → ASUS EZ Flash 3 Utility** (the menu name can vary), open
the FAT32 partition labelled `PL64_EFI`, and select the `.CAP` file at its
root. Confirm the PL64 model and intended version before starting. If EZ Flash
does not list the internal partition, copy the same file onto a FAT32 USB
drive and select that instead. Leave AC power connected while it updates and
restarts. Keep Secure Boot disabled for this unsigned NixOS installation.

After it returns to NixOS, verify:

```sh
cat /sys/class/dmi/id/bios_version
systemctl --failed
```

## Retired EC2 builder

Instance `i-0323946f24932fdf9` is terminated. Its 100 GiB root volume
`vol-023ec41b1145422be`, network interface and dedicated security group
`sg-074f433622a367ebb` are deleted. No snapshots, AMIs or Elastic IPs remain
from this build setup. The original Codex instance and its disk were preserved.

The local ISO checksum was verified again before deletion. Cleanup evidence is
in `artifacts/validation/cleanup.json`. The project-local SSH key, host-key pin
and instance state remain ignored by Git; the old builder cannot be restarted.
