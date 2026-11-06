{ pkgs }:
let
  archive = pkgs.fetchurl {
    url = "https://codeload.github.com/foxglove/mcap/tar.gz/refs/tags/releases/cpp/v2.1.1";
    sha256 = "f5ec006beccfb6d56a26fee8e460e79bed02f125eb2604159ddfbb9906d57da4";
  };
in pkgs.runCommand "mcap-cpp-2.1.1-headers" {} ''
  mkdir source
  tar -xzf ${archive} --strip-components=1 -C source
  mkdir -p $out/include
  cp -r source/cpp/mcap/include/mcap $out/include/
  cp ${./crc32.hpp} $out/include/mcap/crc32.hpp
''
