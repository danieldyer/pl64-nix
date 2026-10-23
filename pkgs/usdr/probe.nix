{ lib, stdenv, pkg-config, source, library, revision }:
stdenv.mkDerivation {
  pname = "usdr-native-rx-probe";
  version = builtins.substring 0 7 revision;
  dontUnpack = true;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ library ];
  buildPhase = ''
    runHook preBuild
    $CC -std=gnu11 -O2 -Wall -Wextra ${source}/audit/2026-10-22/native_rx_probe.c \
      $(pkg-config --cflags --libs usdr) -o usdr-native-rx-probe
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = "./usdr-native-rx-probe -h";
  installPhase = ''
    install -Dm755 usdr-native-rx-probe "$out/bin/usdr-native-rx-probe"
  '';
  meta = {
    description = "Native uSDR DMA loan and single-copy receive probe";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "usdr-native-rx-probe";
  };
}
