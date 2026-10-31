{ config, lib, pkgs, ... }:
let
  cfg = config.services.pl64-ins;
  settings = pkgs.writeText "pl64-ins.json" (builtins.toJSON cfg.settings);
  recoverGps = pkgs.writeShellApplication {
    name = "pl64-ins-recover-gps";
    runtimeInputs = [ pkgs.systemd ];
    text = builtins.readFile ./recover-gps.sh;
  };
in {
  options.services.pl64-ins = {
    enable = lib.mkEnableOption "PL64 IMU/GNSS navigation and raw recording";
    package = lib.mkOption {
      type = lib.types.package;
      default = import ../pkgs/pl64-ins { inherit pkgs; };
      description = "Navigation service package.";
    };
    settings = lib.mkOption {
      type = (pkgs.formats.json { }).type;
      default = { };
      description = "Navigation, service and sensor parameters; see cpp/ins/README.md.";
    };
  };
  config = lib.mkIf cfg.enable {
    users.groups.navigation = { };
    users.users.navigation = {
      isSystemUser = true;
      group = "navigation";
    };
    environment.systemPackages = [ cfg.package ];
    services.pl64-ins.settings = {
      service = {
        imu_device = lib.mkDefault "/dev/pl64com1";
        state_socket = lib.mkDefault "/run/pl64-ins/state.sock";
        log_directory = lib.mkDefault "/var/lib/pl64-ins";
        rotation_s = lib.mkDefault 900;
      };
      navigation = {
        history_s = lib.mkDefault 3;
        # Until surveyed, assume coincident IMU and GNSS reference points.
        # Replace with the measured IMU-to-GNSS vector in body axes.
        gnss_lever_m = lib.mkDefault [ 0.0 0.0 0.0 ];
        sensor_to_body_xyzw = lib.mkDefault [ 0.0 0.0 0.0 1.0 ];
        imu_delay_s = lib.mkDefault 0.001;
        imu_timing_sigma_s = lib.mkDefault 0.01;
        gnss_timing_sigma_s = lib.mkDefault 0.1;
      };
    };
    systemd.services.pl64-ins = {
      description = "IMU/GNSS navigation and raw sensor recording";
      bindsTo = [ "dev-pl64com1.device" ];
      after = [ "dev-pl64com1.device" ];
      # A missing/powered-off IMU behind the present USB serial adapter must
      # keep retrying, including after unsuccessful startup configuration.
      startLimitIntervalSec = 0;
      # GPS hotplug starts gpsd independently; an absent GPS does not prevent
      # IMU recording. Transport faults restart acquisition, while ordinary
      # loss of fix allows inertial propagation with degraded status.
      serviceConfig = {
        Type = "simple";
        User = "navigation";
        Group = "navigation";
        ExecStart = "${cfg.package}/bin/pl64-ins live --config ${settings}";
        Restart = "always";
        RestartSec = 5;
        # Only the explicit GNSS transport-fault exit code requests privileged
        # receiver recovery. The estimator itself remains unprivileged.
        ExecStopPost = "+${recoverGps}/bin/pl64-ins-recover-gps";
        TimeoutStopSec = 30;
        StateDirectory = "pl64-ins";
        StateDirectoryMode = "0750";
        RuntimeDirectory = "pl64-ins";
        RuntimeDirectoryMode = "0770";
        UMask = "0027";
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        RestrictAddressFamilies = [ "AF_UNIX" "AF_INET" ];
      };
    };
  };
}
