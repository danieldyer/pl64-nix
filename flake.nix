{
  description = "ASUS PL64 console NixOS system and offline USB installer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      configurationSource = nixpkgs.lib.fileset.toSource {
        root = ./.;
        fileset = nixpkgs.lib.fileset.unions [
          ./flake.nix ./flake.lock ./nixos ./pkgs ./keys ./cpp ./README.md
        ];
      };
      pl64 = nixpkgs.lib.nixosSystem {
        inherit system;
        modules = [ ./nixos/base.nix ./nixos/hardware.nix ./nixos/pl64.nix ];
      };
      usdr = import ./pkgs/usdr {
        inherit pkgs;
        kernelPackages = pl64.config.boot.kernelPackages;
      };
      installer = nixpkgs.lib.nixosSystem {
        inherit system;
        specialArgs = {
          inherit configurationSource;
          pl64System = pl64.config.system.build.toplevel;
        };
        modules = [ ./nixos/base.nix ./nixos/hardware.nix ./nixos/installer.nix ];
      };
    in
    {
      nixosConfigurations = { inherit pl64 installer; };
      packages.${system} = {
        default = installer.config.system.build.isoImage;
        installer-iso = installer.config.system.build.isoImage;
        system = pl64.config.system.build.toplevel;
        pl64-ins = import ./pkgs/pl64-ins { inherit pkgs; };
        sdrpp = import ./pkgs/sdrpp { inherit pkgs; usdrLibrary = usdr.library; };
        usdr-firmware = usdr.firmware;
        usdr-lib = usdr.library;
        usdr-kernel-module = usdr.kernelModule;
        usdr-native-probe = usdr.probe;
        qemu = pkgs.qemu_kvm;
        ovmf = pkgs.OVMF.fd;
      };
    };
}
