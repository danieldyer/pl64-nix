{ config, lib, pkgs, ... }:
let
  # Copy the Intel firmware into a separate output; the full multi-vendor
  # firmware package is a build dependency, not part of the installed closure.
  intelFirmware = pkgs.runCommand "pl64-intel-firmware" { } ''
    mkdir -p "$out/lib/firmware/intel"
    cp -rL ${pkgs.linux-firmware}/lib/firmware/i915 "$out/lib/firmware/"
    cp -L ${pkgs.linux-firmware}/lib/firmware/iwlwifi-* "$out/lib/firmware/"
    cp -L ${pkgs.linux-firmware}/lib/firmware/intel/ibt-* "$out/lib/firmware/intel/"
  '';
in
{
  nixpkgs.hostPlatform = "x86_64-linux";
  boot.kernelPackages = pkgs.linuxPackages_latest;
  boot.initrd.systemd.enable = true;
  boot.initrd.includeDefaultModules = false;
  boot.initrd.availableKernelModules = [
    "nvme" "vmd" "xhci_pci" "usbhid" "hid_generic"
  ];
  boot.kernelModules = [ "i915" "coretemp" ];
  # Request only the HDMI/local console, so boot never waits for a serial port.
  boot.kernelParams = [ "console=tty0" ];
  boot.consoleLogLevel = 4;

  hardware.enableAllFirmware = lib.mkForce false;
  hardware.enableRedistributableFirmware = lib.mkForce false;
  hardware.wirelessRegulatoryDatabase = true;
  hardware.firmware = lib.mkForce [ intelFirmware pkgs.sof-firmware pkgs.wireless-regdb ];
  hardware.cpu.intel.updateMicrocode = true;
  hardware.cpu.amd.updateMicrocode = lib.mkForce false;

  # Kernel modesetting provides the HDMI text console without Mesa, X11 or a DM.
  hardware.graphics.enable = false;

  # intel_pstate's powersave policy allows HWP to scale dynamically and turbo;
  # it is not the generic governor that pins a CPU to its lowest frequency.
  powerManagement.enable = true;
  powerManagement.cpuFreqGovernor = "powersave";
  services.thermald.enable = true;
  systemd.services.thermald.unitConfig.ConditionVirtualization = false;
  environment.systemPackages = [ config.boot.kernelPackages.turbostat ];
  # Leave ACPI, CPU idle, ASPM, NVMe APST and suspend at their supported defaults.
}
