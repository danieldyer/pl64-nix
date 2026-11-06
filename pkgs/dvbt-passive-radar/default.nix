{ pkgs, src, usdrLibrary, runtime }:
let
  vkfftArchive = pkgs.fetchurl {
    url = "https://github.com/DTolm/VkFFT/archive/066a17c17068c0f11c9298d848c2976c71fad1c1.tar.gz";
    sha256 = "9b6a03cce4a9aab758b4bb57357cb5e2cc15f4ab6cd00f2e35c37edf03ec2424";
  };
  vkfft = pkgs.runCommand "vkfft-1.3.4-source" {} ''
    mkdir -p $out
    tar -xzf ${vkfftArchive} --strip-components=1 -C $out
  '';
  flags = [
    "-DFETCHCONTENT_SOURCE_DIR_VKFFT=${vkfft}"
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
    "-DCMAKE_CXX_FLAGS=-march=alderlake"
    "-DRADAR_BUILD_UHD=OFF" "-DRADAR_BUILD_USDR=ON"
    "-DRADAR_BUILD_TUI=OFF" "-DRADAR_BUILD_LIVE_DISPLAY=OFF"
    "-DRADAR_BUILD_SERVICE=ON"
    "-DUSDR_INCLUDE_DIR=${usdrLibrary}/include/usdr"
    "-DUSDR_LIBRARY=${usdrLibrary}/lib/libusdr.so"
  ];
in pkgs.stdenv.mkDerivation {
  pname = "dvbt-passive-radar";
  version = "0.1.0";
  outputs = [ "out" "tests" ];
  inherit src;
  nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.shaderc pkgs.flatbuffers ];
  nativeCheckInputs = [ pkgs.python3 ];
  buildInputs = [ pkgs.boost pkgs.fftwFloat pkgs.vulkan-loader pkgs.vulkan-headers pkgs.glslang pkgs.flatbuffers runtime usdrLibrary ];
  cmakeFlags = flags;
  preConfigure = ''
    test -f src/capture_service.cpp || {
      echo "This pinned radar revision predates service integration. Use scripts/radar-build with your working checkout, then update radar-src after committing it." >&2
      exit 1
    }
  '';
  doCheck = true;
  # Hardware checks are run explicitly outside the build sandbox. Keep their
  # executables out of the deployed system's runtime closure.
  postInstall = ''
    mkdir -p "$tests/bin"
    cp unusable_reference_tests response_tests matched_tests sar_tests pipeline_coherence_tests "$tests/bin/"
  '';
  checkPhase = ''
    runHook preCheck
    ctest -L cpu --output-on-failure -j "$NIX_BUILD_CORES"
    runHook postCheck
  '';
  passthru = {
    inherit vkfft;
    developmentFlags = flags;
    usdrSourceRevision = usdrLibrary.sourceRevision;
  };
  meta = {
    description = "DVB-T profile capture daemon with native uSDR DMA and shared-memory publication";
    platforms = [ "x86_64-linux" ];
    mainProgram = "dvbt-service";
    license = pkgs.lib.licenses.gpl3Plus;
  };
}
