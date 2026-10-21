{ lib, modulesPath, pkgs, ... }:
{
  imports = [ "${modulesPath}/profiles/minimal.nix" ];

  system.stateVersion = "26.05";
  time.timeZone = "Australia/Melbourne";
  i18n.defaultLocale = "en_AU.UTF-8";
  i18n.supportedLocales = [ "en_AU.UTF-8/UTF-8" "en_US.UTF-8/UTF-8" ];

  # Keep local command manuals, without HTML manuals or development documentation.
  documentation = {
    enable = true;
    man.enable = true;
    doc.enable = false;
    info.enable = false;
    nixos.enable = lib.mkForce false;
  };

  environment.systemPackages = with pkgs; [
    vim curl wget rsync jq ripgrep fd tree file less which
    htop tmux unzip zip zstd lsof strace psmisc
    dnsutils ethtool tcpdump traceroute iperf3 socat netcat-openbsd iw
    pciutils usbutils nvme-cli smartmontools lm_sensors powertop
    parted dosfstools e2fsprogs
  ];
  environment.variables.EDITOR = "vim";
  programs.bash.completion.enable = true;
  programs.git = {
    enable = true;
    package = pkgs.gitMinimal;
  };

  networking = {
    useDHCP = false;
    networkmanager.enable = true;
    firewall.enable = true;
  };
  # DHCP and Wi-Fi association proceed asynchronously; no service needs an uplink
  # before the local console or sshd can start.
  systemd.services.NetworkManager-wait-online.enable = false;

  services.openssh = {
    enable = true;
    openFirewall = true;
    settings = {
      PermitRootLogin = "prohibit-password";
      PasswordAuthentication = false;
      KbdInteractiveAuthentication = false;
    };
  };
  users.users.root.openssh.authorizedKeys.keyFiles = [ ../keys/root.pub ];

  # Use explicit locked flakes, without retaining a full Nixpkgs checkout in
  # the system closure through the implicit registry or legacy NIX_PATH.
  nixpkgs.flake = {
    setFlakeRegistry = false;
    setNixPath = false;
  };
  nix.channel.enable = false;
  nix.settings = {
    experimental-features = [ "nix-command" "flakes" ];
    auto-optimise-store = true;
  };
}
