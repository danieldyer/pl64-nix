{ config, pkgs, ... }:
let
  usdr = import ../pkgs/usdr {
    inherit pkgs;
    kernelPackages = config.boot.kernelPackages;
  };
in
{
  # Deploy together with the validated matching FPGA image; no flashing service.
  boot.extraModulePackages = [ usdr.kernelModule ];
  boot.kernelModules = [ "usdr_pcie_uram" ];
  environment.systemPackages = [ usdr.library usdr.probe pkgs.vulkan-tools ];
  # Headless Intel Vulkan compute for the DVB-T pipeline; no display manager.
  hardware.graphics.enable = true;
}
