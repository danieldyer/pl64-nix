uSDR rev3 operational FPGA image

This is the exact c53414a7 binary flashed, read back, power-cycled and functionally
checked on the PL64 on 24 October 2026. Its SHA256 and hardware/source identities
are in manifest.json and SHA256SUMS. It is checked directly into pl64-nix, not
fetched from the build VM. Nix verifies its checksum while packaging it.

The installed system makes these files available at /etc/usdr/firmware.
Rebuilding or booting NixOS does not write them to the SDR. They are available
for a deliberate, separately performed update of a compatible replacement.

This file is an operational-slot image, not a full flash dump or golden image.
The tested board is rev3, XC7A35T-CPG236-2, FPGA ID 0362d093, 4 MiB flash, with
the operational slot at 0x1c0000 and golden firmware 14302173. Check replacement
hardware and its flash/golden layout and back up both slots before using it.
Use the matching packaged usdr_flash tooling, which validates image identity,
role, partition bounds, CRC and full write readback. Preserve the golden slot.

Hardware tests covered PCIe startup, short DMA/calibration checks, native SDR++
reception and HDMI audio, plus a 107 MHz injected tone through X3. Analog DC
calibration passed with no RF input; startup with RF present can fail because
LNA3 input isolation is incomplete. See the repository README for this known
issue and the test limits. Full RF characterization and long-duration operation
remain pending. Passing these checks does not guarantee recovery from an
interrupted update or a PCIe fault.

RTL revision: 04a3c7812b13b55a1e7c356fae45deb111fec815
Matching driver: 49c3d57288826dfd1e97e9ec5638df18285fcdfe
Vivado: ML Standard 2025.2
The reproducible build flow is in usdr-fpga commit a2e830c. Its clean rebuild
matches this configuration payload but has a different timestamp/CRC. This file
retains the exact binary already tested in hardware.
