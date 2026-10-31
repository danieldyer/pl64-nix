{ pkgs }:
let
  mcapArchive = pkgs.fetchurl {
    url = "https://codeload.github.com/foxglove/mcap/tar.gz/refs/tags/releases/cpp/v2.1.1";
    sha256 = "f5ec006beccfb6d56a26fee8e460e79bed02f125eb2604159ddfbb9906d57da4";
  };
  mcap = pkgs.runCommand "mcap-cpp-2.1.1-headers" { } ''
    mkdir source
    tar -xzf ${mcapArchive} --strip-components=1 -C source
    mkdir -p $out/include
    cp -r source/cpp/mcap/include/mcap $out/include/
  '';
in pkgs.stdenv.mkDerivation {
  pname = "pl64-ins";
  version = "0.1.0";
  src = ../../cpp;
  nativeBuildInputs = [ pkgs.cmake ];
  nativeCheckInputs = [ pkgs.python3 ];
  buildInputs = [ pkgs.eigen pkgs.flatbuffers pkgs.nlohmann_json ];
  cmakeFlags = [ "-DMCAP_INCLUDE_DIR=${mcap}/include" ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    python3 ../ins/tests/live_serial.py ./ins/pl64-ins ./ins/navigation.bfbs ./ins/ins-capture
    python3 ../ins/tests/recovery.py ${../../nixos/recover-gps.sh}
    runHook postCheck
  '';
  meta = {
    description = "Causal IMU/GNSS navigation estimator with raw MCAP recording";
    platforms = [ "x86_64-linux" ];
    mainProgram = "pl64-ins";
  };
}
