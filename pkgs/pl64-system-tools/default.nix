{ pkgs }:
let
  mcap = import ../mcap { inherit pkgs; };
  unpack = name: archive: pkgs.runCommand name {} ''
    mkdir -p $out
    tar -xzf ${archive} --strip-components=1 -C $out
  '';
  imgui = unpack "imgui-1.91.9b-docking-source" (pkgs.fetchurl {
    url = "https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.91.9b-docking";
    sha256 = "466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5";
  });
  implot = unpack "implot-0.16-source" (pkgs.fetchurl {
    url = "https://codeload.github.com/epezent/implot/tar.gz/refs/tags/v0.16";
    sha256 = "961df327d8a756304d1b0a67316eebdb1111d13d559f0d3415114ec0eb30abd1";
  });
in pkgs.stdenv.mkDerivation {
  pname = "pl64-system-tools";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../../cpp;
    fileset = pkgs.lib.fileset.unions [
      ../../cpp/CMakeLists.txt ../../cpp/cmake ../../cpp/tests
      ../../cpp/runtime ../../cpp/services ../../cpp/ins/schema
    ];
  };
  nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
  buildInputs = [ pkgs.nlohmann_json pkgs.flatbuffers pkgs.glfw pkgs.libGL pkgs.zlib-ng ];
  cmakeFlags = [
    "-DPL64_BUILD_INS=OFF" "-DPL64_BUILD_SERVICES=ON" "-DPL64_BUILD_GUI=ON"
    "-DMCAP_INCLUDE_DIR=${mcap}/include"
    "-DPL64_MCAP_USE_ZLIB_NG=ON"
    "-DIMGUI_SOURCE_DIR=${imgui}" "-DIMPLOT_SOURCE_DIR=${implot}"
    "-DPL64_FONT_PATH=${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf"
  ];
  doCheck = true;
  meta = {
    description = "PL64 dockable ImGui control, recording and SAR subscription services";
    platforms = [ "x86_64-linux" ];
    mainProgram = "pl64-control";
  };
}
