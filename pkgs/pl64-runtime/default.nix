{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "pl64-runtime";
  version = "0.1.0";
  src = ../../cpp/runtime;
  nativeBuildInputs = [ pkgs.cmake ];
  propagatedBuildInputs = [ pkgs.nlohmann_json ];
  doCheck = true;
  meta = {
    description = "Bounded local control and immutable shared-memory record transport";
    platforms = [ "x86_64-linux" ];
  };
}
