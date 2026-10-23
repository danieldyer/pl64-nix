{ pkgs, kernelPackages }:
let
  # Keep the unpublished receive fixes reproducible without sibling checkouts.
  upstream = pkgs.fetchFromGitHub {
    owner = "wavelet-lab";
    repo = "usdr-lib";
    rev = "f7e869df29582934b87d3759e3c108d60a27e451";
    hash = "sha256-N7EEYY+K6phRr6y/6o9Tyg93pP68tHQwvNsiU2mJDFY=";
  };
  revision = "ac47648183cdfa0d2f4d48b50b8cd51f8a4a345e";
  source = pkgs.applyPatches {
    name = "usdr-lib-essential-${builtins.substring 0 7 revision}";
    src = upstream;
    patches = [ ./essential-rx.patch ];
  };
  library = pkgs.callPackage ./library.nix { inherit source revision; };
in
{
  inherit source library;
  kernelModule = kernelPackages.callPackage ./kernel-module.nix { inherit source revision; };
  probe = pkgs.callPackage ./probe.nix { inherit source library revision; };
}
