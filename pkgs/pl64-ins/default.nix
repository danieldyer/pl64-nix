{ pkgs }:
let
  mcap = import ../mcap { inherit pkgs; };
in pkgs.stdenv.mkDerivation {
  pname = "pl64-ins";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../../cpp;
    fileset = pkgs.lib.fileset.unions [
      ../../cpp/CMakeLists.txt ../../cpp/cmake ../../cpp/tests
      ../../cpp/runtime ../../cpp/ins
    ];
  };
  nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
  nativeCheckInputs = [ pkgs.python3 ];
  buildInputs = [ pkgs.eigen pkgs.flatbuffers pkgs.nlohmann_json pkgs.zlib-ng ];
  cmakeFlags = [ "-DMCAP_INCLUDE_DIR=${mcap}/include" "-DPL64_MCAP_USE_ZLIB_NG=ON" ];
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
