{ runCommandNoCC }:
runCommandNoCC "usdr-rev3-firmware-c53414a7" {} ''
  mkdir -p "$out"
  cp ${./firmware}/* "$out/"
  cd "$out"
  sha256sum --check SHA256SUMS
''
