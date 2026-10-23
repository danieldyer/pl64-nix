{ lib, stdenv, kernel, kernelModuleMakeFlags, source, revision }:
stdenv.mkDerivation {
  pname = "usdr-pcie";
  version = "${builtins.substring 0 7 revision}-${kernel.version}";
  src = source;
  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" ];
  preBuild = "cd src/lib/lowlevel/pcie_uram/driver";
  makeFlags = kernelModuleMakeFlags ++ [
    "KERNELDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
  ];
  installPhase = ''
    runHook preInstall
    install -D usdr_pcie_uram.ko "$out/lib/modules/${kernel.modDirVersion}/misc/usdr_pcie_uram.ko"
    runHook postInstall
  '';
  passthru = { inherit kernel; sourceRevision = revision; };
  meta = {
    description = "uSDR PCIe kernel module with coherent DMA ownership and drain fixes";
    license = lib.licenses.gpl2Only;
    platforms = [ "x86_64-linux" ];
  };
}
