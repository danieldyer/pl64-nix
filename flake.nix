{
  description = "ASUS PL64 console NixOS system and offline USB installer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  inputs.radar-src = {
    url = "git+ssh://git@github.com/danieldyer/dvbt-passive-radar.git?ref=main";
    flake = false;
  };

  outputs = { nixpkgs, radar-src, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      configurationSource = nixpkgs.lib.fileset.toSource {
        root = ./.;
        fileset = nixpkgs.lib.fileset.unions [
          ./flake.nix ./flake.lock ./nixos ./pkgs ./keys ./cpp ./scripts ./README.md
        ];
      };
      pl64 = nixpkgs.lib.nixosSystem {
        inherit system;
        specialArgs = { radarPackage = radar; systemTools = tools; };
        modules = [ ./nixos/base.nix ./nixos/hardware.nix ./nixos/pl64.nix ];
      };
      usdr = import ./pkgs/usdr {
        inherit pkgs;
        kernelPackages = pl64.config.boot.kernelPackages;
      };
      runtime = import ./pkgs/pl64-runtime { inherit pkgs; };
      tools = import ./pkgs/pl64-system-tools { inherit pkgs; };
      radar = import ./pkgs/dvbt-passive-radar {
        inherit pkgs runtime;
        src = radar-src;
        usdrLibrary = usdr.library;
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
        pl64-runtime = runtime;
        pl64-system-tools = tools;
        dvbt-passive-radar = radar;
        sdrpp = import ./pkgs/sdrpp { inherit pkgs; usdrLibrary = usdr.library; };
        usdr-firmware = usdr.firmware;
        usdr-lib = usdr.library;
        usdr-kernel-module = usdr.kernelModule;
        usdr-native-probe = usdr.probe;
        qemu = pkgs.qemu_kvm;
        ovmf = pkgs.OVMF.fd;
      };
      devShells.${system}.radar = pkgs.mkShell {
        inputsFrom = [ radar ];
        packages = [ pkgs.ninja (pkgs.writeShellScriptBin "radar-cmake" ''
          set -euo pipefail
          source_dir="''${1:?Usage: radar-cmake /path/to/dvbt_passive_radar [CMake options]}"
          shift
          exec cmake -S "$source_dir" -B "$source_dir/build/nix" -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo ${nixpkgs.lib.escapeShellArgs radar.developmentFlags} "$@"
        '') ];
      };
    };
}
