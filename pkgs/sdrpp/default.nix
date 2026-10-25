{ pkgs, usdrLibrary }:
let
  base = pkgs.sdrpp.override {
    airspy_source = false;
    airspyhf_source = false;
    audio_source = false;
    bladerf_source = false;
    dragonlabs_source = false;
    hackrf_source = false;
    hermes_source = false;
    limesdr_source = false;
    network_source = false;
    plutosdr_source = false;
    rfspace_source = false;
    rtl_sdr_source = false;
    rtl_tcp_source = false;
    sdrpp_server_source = false;
    soapy_source = false;
    spectran_http_source = false;
    spyserver_source = false;
    usrp_source = false;
    network_sink = false;
    atv_decoder = false;
    meteor_demodulator = false;
    pager_decoder = false;
    discord_presence = false;
    iq_exporter = false;
    rigctl_client = false;
    rigctl_server = false;
    scanner = false;
  };
in
base.overrideAttrs (old: {
  upstreamVersion = "1.3.2";
  version = "1.3.2-native-usdr-8e348bc";
  passthru = (old.passthru or {}) // {
    sourceRevision = "8e348bc134ea664de57b4c4f11dd99470dfb1595";
  };
  # Ordinary archive hash, verified locally; no sibling checkout is required.
  src = pkgs.fetchurl {
    url = "https://codeload.github.com/AlexandreRouma/SDRPlusPlus/tar.gz/f10fb98af8b80de01de94e60c17608e4281e2515";
    name = "sdrpp-f10fb98af8b80de01de94e60c17608e4281e2515.tar.gz";
    hash = "sha256-AUmy4ZaudGC5VfjrywuzlAYu9LhQNbimwI9CHjAEtfU=";
  };
  patches = (old.patches or []) ++ [ ./native-usdr.patch ];
  buildInputs = old.buildInputs ++ [ usdrLibrary ];
  cmakeFlags = old.cmakeFlags ++ [
    "-DOPT_BUILD_USDR_SOURCE=ON"
    "-DOPT_TEST_USDR_SOURCE=ON"
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ctest --test-dir source_modules/usdr_source/tests --output-on-failure
    runHook postCheck
  '';
})
