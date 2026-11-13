{ lib, ... }:
{
  imports = [ ./navigation.nix ./ins.nix ./usdr.nix ./sdrpp.nix ./instrument.nix ./ethernet.nix ];
  services.pl64-ins.enable = true;
  # Accepted motion-inferred mounting: antenna +Z = -packet Z, +Y = packet Y,
  # +X = -packet X. Decoder first normalizes gyro [y,-x,z] into packet accel axes.
  services.pl64-ins.imuToAntennaQuaternion = [ 0.0 1.0 0.0 0.0 ];
  networking.hostName = "pl64";
  services.avahi = {
    enable = true;
    openFirewall = true;
    publish = {
      enable = true;
      addresses = true;
    };
  };
  services.fwupd.enable = true;
  # Select thermald's default engine and retain the BIOS's TCPU/PPCC tables.
  # Adaptive mode relaxes MSR PL1 while this machine's MMIO limiter is disabled.
  services.thermald.configFile = ./thermal-conf.xml;
  boot.supportedFilesystems = lib.mkForce [ "ext4" "vfat" ];

  boot.loader.systemd-boot = {
    enable = true;
    configurationLimit = 5;
  };
  boot.loader.efi.canTouchEfiVariables = true;
  boot.loader.timeout = 1;

  fileSystems."/" = {
    device = "/dev/disk/by-label/PL64_ROOT";
    fsType = "ext4";
  };
  fileSystems."/boot" = {
    device = "/dev/disk/by-label/PL64_EFI";
    fsType = "vfat";
    options = [ "umask=0077" ];
  };
  services.fstrim.enable = true;
  zramSwap = {
    enable = true;
    algorithm = "zstd";
    memoryPercent = 25;
  };
}
