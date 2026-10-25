{ config, pkgs, ... }:
let
  usdr = import ../pkgs/usdr {
    inherit pkgs;
    kernelPackages = config.boot.kernelPackages;
  };
in
{
  # Deploy together with the validated matching FPGA image; no flashing service.
  environment.etc."usdr/firmware".source = usdr.firmware;
  boot.extraModulePackages = [ usdr.kernelModule ];
  boot.kernelModules = [ "usdr_pcie_uram" ];
  environment.systemPackages = [ usdr.library usdr.probe pkgs.vulkan-tools ];
  # Intel graphics for Vulkan compute and the local SDR++ kiosk.
  hardware.graphics.enable = true;
}
