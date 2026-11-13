{ runCommandNoCC }:
runCommandNoCC "usdr-rev3-firmware-${(builtins.fromJSON (builtins.readFile ./firmware/manifest.json)).firmware_id}" {} ''
  mkdir -p "$out"
  cp ${./firmware}/* "$out/"
  cd "$out"
  sha256sum --check SHA256SUMS
''
