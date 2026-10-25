{ config, lib, pkgs, ... }:
let
  usdr = import ../pkgs/usdr {
    inherit pkgs;
    kernelPackages = config.boot.kernelPackages;
  };
  sdrpp = import ../pkgs/sdrpp { inherit pkgs; usdrLibrary = usdr.library; };
  initialConfig = pkgs.writeText "sdrpp-initial.json" (builtins.toJSON {
    moduleInstances = {
      "uSDR Source" = { module = "usdr_source"; enabled = true; };
      "File Source" = { module = "file_source"; enabled = true; };
      "Audio Sink" = { module = "audio_sink"; enabled = true; };
      "Radio" = { module = "radio"; enabled = true; };
      "Frequency Manager" = { module = "frequency_manager"; enabled = true; };
      "Recorder" = { module = "recorder"; enabled = true; };
    };
    source = "uSDR";
    frequency = 202000000;
    fullscreen = true;
    fftRate = 60;
    modulesDirectory = "@prefix@/lib/sdrpp/plugins";
    resourcesDirectory = "@prefix@/share/sdrpp";
  });
  kiosk = pkgs.writeShellApplication {
    name = "sdrpp-kiosk";
    runtimeInputs = [ pkgs.coreutils pkgs.jq ];
    text = ''
      umask 077
      config_dir="$HOME/.config/sdrpp"
      mkdir -p "$config_dir"
      if [ ! -e "$config_dir/config.json" ]; then
        install -m 600 ${initialConfig} "$config_dir/config.json"
      fi
      # Keep user tuning/display preferences, but always launch fullscreen.
      jq '.fullscreen = true' "$config_dir/config.json" > "$config_dir/config.json.new"
      mv -f "$config_dir/config.json.new" "$config_dir/config.json"
      exec ${sdrpp}/bin/sdrpp --root "$config_dir"
    '';
  };
in
{
  environment.systemPackages = [ sdrpp kiosk ];
  users.groups.usdr = {};
  users.users.sdr = {
    isNormalUser = true;
    home = "/home/sdr";
    createHome = true;
    extraGroups = [ "usdr" "video" "render" "audio" ];
    hashedPassword = "!";
  };
  services.udev.extraRules = ''
    SUBSYSTEM=="usdr", KERNEL=="usdr*", GROUP="usdr", MODE="0660"
  '';

  # One fullscreen application on tty1; no desktop, display manager or remote server.
  services.cage = {
    enable = true;
    user = "sdr";
    program = "${kiosk}/bin/sdrpp-kiosk";
    extraArguments = [ "-s" ]; # Allow Ctrl+Alt+Fn to switch to a text console.
    environment.WLR_LIBINPUT_NO_DEVICES = "1";
  };
  # Keep the fullscreen session available, but start it only on request.
  systemd.defaultUnit = lib.mkForce "multi-user.target";
  systemd.targets.graphical.wants = lib.mkForce [];
  systemd.services.cage-tty1 = {
    wantedBy = lib.mkForce [];
    after = [ "systemd-udev-trigger.service" ];
    serviceConfig = {
      # PAM places the application in a session scope; stop it before Cage waits.
      ExecStop = "-${pkgs.procps}/bin/pkill -TERM -P $MAINPID";
      Restart = "always";
      RestartSec = 3;
    };
  };
  security.rtkit.enable = true;
  services.pipewire = {
    enable = true;
    alsa.enable = true;
    pulse.enable = true;
    # The local monitor is connected to the first HDMI audio endpoint.
    wireplumber.extraConfig."51-pl64-hdmi" = {
      "device.profile.priority.rules" = [{
        matches = [{ "device.name" = "alsa_card.pci-0000_00_1f.3"; }];
        actions.update-props.priorities = [
          "output:hdmi-stereo"
        ];
      }];
    };
  };
}
