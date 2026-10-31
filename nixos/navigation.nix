{ config, lib, pkgs, ... }:
let
  insEnabled = config.services.pl64-ins.enable;
  com1Service = if insEnabled then "pl64-ins" else "pl64-com1-settings";
  com1Access = lib.optionalString insEnabled ''GROUP="navigation", MODE="0660", '';
  gpsConfigure = pkgs.writeShellApplication {
    name = "pl64-gps-configure";
    runtimeInputs = [ pkgs.gpsd pkgs.gnugrep ];
    text = builtins.readFile ./configure-ublox.sh;
  };
in
{
  nixpkgs.overlays = [
    (_final: prev: { gpsd = prev.gpsd.override { guiSupport = false; }; })
  ];
  environment.systemPackages = [ pkgs.gpsd gpsConfigure pkgs.picocom ];

  services.udev.extraRules = ''
    ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="1546", ATTRS{idProduct}=="01a7", SYMLINK+="gps0", TAG+="systemd", ENV{SYSTEMD_ALIAS}+="/dev/gps0", ENV{SYSTEMD_WANTS}+="gpsd.service", ENV{ID_MM_DEVICE_IGNORE}="1"
    ACTION=="add", SUBSYSTEM=="tty", ENV{ID_SERIAL_SHORT}=="20140416-01.12", ENV{ID_USB_INTERFACE_NUM}=="00", ATTRS{port_number}=="0", SYMLINK+="pl64com1", ${com1Access}TAG+="systemd", ENV{SYSTEMD_ALIAS}+="/dev/pl64com1", ENV{SYSTEMD_WANTS}+="${com1Service}.service", ENV{ID_MM_DEVICE_IGNORE}="1"
    ACTION=="add", SUBSYSTEM=="tty", ENV{ID_SERIAL_SHORT}=="20140416-01.12", ENV{ID_USB_INTERFACE_NUM}=="00", ATTRS{port_number}=="1", SYMLINK+="pl64com2", TAG+="systemd", ENV{SYSTEMD_ALIAS}+="/dev/pl64com2", ENV{SYSTEMD_WANTS}+="pl64-com2-settings.service", ENV{ID_MM_DEVICE_IGNORE}="1"
  '';

  services.gpsd = {
    enable = true;
    devices = [ "/dev/gps0" ];
    nowait = true;
    readonly = true;
    listenany = false;
  };
  services.chrony = {
    enable = true;
    # Keep the standard network servers as fallback and a sanity check.
    extraConfig = ''
      # This USB mouse exposes serial time, not a host-timestamped PPS edge.
      # No unmeasured latency compensation or sub-microsecond claim.
      refclock SOCK /run/chrony.clk.gps0.sock refid GPS poll 2 filter 8 precision 0.1 delay 0.2 prefer
    '';
  };

  # Factory IMU host settings only: do not transmit configuration, calibrate,
  # or save anything to the sensor. It defaults to query/response at 9600 8N1.
  # The connected IMU is verified on /dev/pl64com1 at sensor address 0x00.
  systemd.services = (lib.genAttrs ([ "pl64-com2-settings" ] ++ lib.optional (!insEnabled) "pl64-com1-settings") (name:
    let
      device = if name == "pl64-com1-settings" then "pl64com1" else "pl64com2";
    in {
      description = "Configure ${device} for the IMU factory serial format";
      bindsTo = [ "dev-${device}.device" ];
      after = [ "dev-${device}.device" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = "${pkgs.coreutils}/bin/stty -F /dev/${device} 9600 raw -echo cs8 -parenb -cstopb -crtscts -ixon -ixoff clocal -hupcl";
      };
    })) // {
    gpsd = {
      # Device activation handles boot and replug without blocking an absent GPS.
      wantedBy = lib.mkForce [ ];
      requires = [ "dev-gps0.device" "chronyd.service" ];
      after = [ "dev-gps0.device" "chronyd.service" ];
      bindsTo = [ "dev-gps0.device" ];
      partOf = [ "chronyd.service" ];
      preStart = "${gpsConfigure}/bin/pl64-gps-configure /dev/gps0";
      startLimitIntervalSec = 0;
      serviceConfig.Restart = "always";
      serviceConfig.RestartSec = 5;
      serviceConfig.TimeoutStartSec = 30;
    };
  };
}
