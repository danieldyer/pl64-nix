{ config, lib, pkgs, radarPackage, systemTools, ... }:
let
  group = "instrument";
  gui = pkgs.writeShellApplication {
    name = "pl64-gui";
    runtimeInputs = [ pkgs.systemd ];
    text = ''
      if [ -n "''${WAYLAND_DISPLAY:-}" ] || [ -n "''${DISPLAY:-}" ]; then
        exec ${systemTools}/bin/pl64-control "$@"
      fi
      exec systemctl start pl64-control-gui.service
    '';
  };
  sdrGui = pkgs.writeShellApplication {
    name = "pl64-sdrpp";
    runtimeInputs = [ pkgs.systemd pkgs.util-linux ];
    text = ''
      if ! flock --nonblock /run/lock/pl64-radio.lock true; then
        echo "Stop the radar capture before launching SDR++." >&2
        exit 1
      fi
      exec systemctl start cage-tty1.service
    '';
  };
  cage = config.systemd.services.cage-tty1;
  service = description: user: runtime: command: {
    inherit description;
    wantedBy = [ "multi-user.target" ];
    after = [ "systemd-udev-trigger.service" ];
    serviceConfig = {
      Type = "simple";
      User = user;
      Group = group;
      ExecStart = command;
      Restart = "on-failure";
      RestartSec = 3;
      RuntimeDirectory = runtime;
      RuntimeDirectoryMode = "0770";
      UMask = "0007";
      NoNewPrivileges = true;
      ProtectSystem = "strict";
      ProtectHome = true;
      PrivateTmp = true;
      RestrictAddressFamilies = [ "AF_UNIX" ];
      TimeoutStopSec = 120;
    };
  };
in {
  users.groups.${group} = {};
  users.users.radar = { isSystemUser = true; inherit group; };
  users.users.recorder = { isSystemUser = true; inherit group; };
  users.users.sar = { isSystemUser = true; inherit group; };
  users.users.sdr.extraGroups = [ group "navigation" ];
  # Each service has its own process identity; this group grants only local
  # control/data socket access. Recording never depends on the SAR lifecycle.
  users.users.navigation.extraGroups = [ group ];
  services.pl64-ins.settings.service = {
    standalone_recording = false;
    records_socket = "/run/pl64-ins/records.sock";
    control_socket = "/run/pl64-ins/control.sock";
  };
  systemd.services.pl64-ins.serviceConfig = {
    Group = lib.mkForce group;
    SupplementaryGroups = [ "navigation" ];
  };
  environment.systemPackages = [ radarPackage systemTools gui sdrGui ];
  systemd.services.pl64-radar = lib.recursiveUpdate
    (service "DVB-T capture (idle until requested)" "radar" "pl64-radar"
      "${radarPackage}/bin/dvbt-service --metadata ${radarPackage}/share/dvbt_passive_radar/recordings/live_usdr_mt_dandenong.json --config /var/lib/pl64-radar/settings.json") {
      serviceConfig.ReadWritePaths = [ "/run/lock/pl64-radio.lock" ];
      serviceConfig.SupplementaryGroups = [ "usdr" "render" "video" ];
      serviceConfig.StateDirectory = "pl64-radar";
      serviceConfig.StateDirectoryMode = "0750";
      serviceConfig.CacheDirectory = "pl64-radar";
      serviceConfig.CacheDirectoryMode = "0750";
      environment.XDG_CACHE_HOME = "/var/cache/pl64-radar";
    };
  systemd.services.pl64-recorder = lib.recursiveUpdate
    (service "Shared MCAP session recorder" "recorder" "pl64-recorder" "${systemTools}/bin/pl64-recorder") {
      # Reverse shutdown ordering keeps the recorder alive while producers drain.
      before = [ "pl64-ins.service" "pl64-radar.service" "pl64-sar.service" ];
      serviceConfig = {
        StateDirectory = "pl64-recordings";
        StateDirectoryMode = "0750";
      };
    };
  systemd.services.pl64-sar = lib.recursiveUpdate
    (service "SAR/ISAR subscription foundation (idle until requested)" "sar" "pl64-sar" "${systemTools}/bin/pl64-sar") {
      serviceConfig.SupplementaryGroups = [ "render" "video" ];
    };
  # Clone the existing, tested Cage/seat setup for an independent on-demand UI.
  systemd.services.pl64-control-gui = {
    description = "PL64 control GUI";
    inherit (cage) after requires bindsTo wants environment;
    conflicts = lib.filter (unit: unit != "pl64-control-gui.service") cage.conflicts ++ [ "cage-tty1.service" ];
    unitConfig = {
      ConditionPathExists = "/dev/tty1";
      OnSuccess = "pl64-display-return.service";
      OnFailure = "pl64-display-return.service";
    };
    serviceConfig = cage.serviceConfig // {
      ExecStart = "${pkgs.cage}/bin/cage -s -- ${systemTools}/bin/pl64-control";
      Restart = "no";
    };
  };
  systemd.services.cage-tty1 = {
    conflicts = [ "pl64-control-gui.service" ];
    unitConfig = { OnSuccess = "pl64-display-return.service"; OnFailure = "pl64-display-return.service"; };
    serviceConfig.Restart = lib.mkForce "no";
  };
  systemd.services.pl64-display-return = {
    description = "Return to the console after the last display application exits";
    after = [ "cage-tty1.service" "pl64-control-gui.service" ];
    serviceConfig.Type = "oneshot";
    path = [ pkgs.systemd ];
    script = ''
      # Switching GUIs also stops a successful unit. Do not let its OnSuccess
      # handler start getty over the replacement display session.
      for unit in cage-tty1.service pl64-control-gui.service; do
        state=$(systemctl show --property=ActiveState --value "$unit")
        case "$state" in active|activating|reloading) exit 0;; esac
      done
      systemctl --no-block start getty@tty1.service
    '';
  };
}
