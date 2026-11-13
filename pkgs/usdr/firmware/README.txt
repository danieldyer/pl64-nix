uSDR rev3 operational FPGA image

manifest.json identifies the current image, matching committed driver/RTL and
actual hardware validation. SHA256SUMS verifies both the current image and the
retained c53414a7 operational rollback image. The golden recovery partition is
separate and is not included or modified by this package.

The current image adds autonomous blind DC/IQ correction before FIR/DMA. The
matched library enables it after analog calibration; no host solver is needed.
Both register/control status and raw ADC health are available through the VFS.

Nix installs these files at /etc/usdr/firmware but never flashes automatically.
Use the matching usdr_flash tool for an explicitly requested operational-slot
update, preserving full backups and the golden image. The tested hardware is
rev3, XC7A35T-CPG236-2, FPGA ID 0362d093, 4 MiB flash, operational slot 0x1c0000.

Build with Vivado 2025.2 using hw/usdr/build.tcl in the recorded source revision.
The committed source patches are pkgs/usdr/blind-iq{,-fpga}.patch. Timing uses the
original constraints; full-core setup, hold, pulse width and Gray skew pass.
External RF I/O timing and analog converter SFDR/NF at 64 MS/s remain separate
qualification work; the manufacturer datasheet specifies ADC performance at
lower rates. Digital correction does not repair analog clipping or upstream IMD.
