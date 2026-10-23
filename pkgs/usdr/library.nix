{ lib, stdenv, cmake, pkg-config, python3, libusb1, source, revision }:
stdenv.mkDerivation {
  pname = "usdr-lib";
  version = "1.0.1-essential-${builtins.substring 0 7 revision}";
  src = source;
  nativeBuildInputs = [ cmake pkg-config (python3.withPackages (p: [ p.pyyaml ])) ];
  propagatedBuildInputs = [ libusb1 ];
  preConfigure = "cd src";
  cmakeFlags = [
    "-DENABLE_GUI=OFF" "-DENABLE_SOAPY=OFF" "-DENABLE_DMONITOR=OFF"
    "-DENABLE_TESTS=OFF" "-DENABLE_AVX512=OFF"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_INSTALL_RPATH=${placeholder "out"}/lib"
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    cmake -S "$src/src/utests/rfic" -B rfic-tests \
      -DREGISTER_HEADER_DIR="$PWD/lib/hw" -DUSDR_LIBRARY="$PWD/lib/libusdr.so"
    cmake --build rfic-tests -j "$NIX_BUILD_CORES"
    ctest --test-dir rfic-tests --output-on-failure
    runHook postCheck
  '';
  passthru.sourceRevision = revision;
  meta = {
    description = "uSDR library and tools with the matched essential PCIe receive fixes";
    homepage = "https://github.com/wavelet-lab/usdr-lib";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
