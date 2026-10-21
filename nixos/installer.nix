{ lib, modulesPath, configurationSource, pl64System, ... }:
{
  imports = [ "${modulesPath}/installer/cd-dvd/installation-cd-minimal.nix" ];

  networking.hostName = "pl64-installer";
  hardware.enableAllHardware = lib.mkForce false;
  boot.swraid.enable = lib.mkForce false;
  boot.supportedFilesystems = lib.mkForce [ "ext4" "vfat" "iso9660" "squashfs" "overlay" ];
  boot.initrd.availableKernelModules = [
    "usb_storage" "uas" "sd_mod" "sr_mod" "loop"
  ];
  boot.loader.timeout = lib.mkForce 1;

  services.getty.autologinUser = lib.mkForce "root";
  services.getty.helpLine = lib.mkForce ''
    PL64 offline installer. Nothing is installed automatically.
    System: /etc/pl64-system
    Configuration: /etc/pl64-config
    Follow the installation commands in /etc/pl64-config/README.md.
  '';

  # Installation uses --system, so no compiler, Nixpkgs checkout or evaluation
  # dependencies are needed on the USB stick.
  system.extraDependencies = lib.mkForce [ ];
  system.installer.channel.enable = false;
  environment.etc."pl64-system".source = pl64System;
  environment.etc."pl64-config".source = configurationSource;
  isoImage = {
    edition = "pl64";
    volumeID = "PL64_INSTALL";
    storeContents = [ pl64System ];
    squashfsCompression = "zstd -Xcompression-level 6";
  };
}
