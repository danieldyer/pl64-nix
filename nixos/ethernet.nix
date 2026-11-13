{ ... }:
let
  wired = id: uuid: mac: {
    connection = {
      inherit id uuid;
      type = "ethernet";
      autoconnect = true;
      autoconnect-priority = 100;
      autoconnect-retries = 0;
    };
    ethernet.mac-address = mac;
    ipv4 = {
      method = "auto";
      # NetworkManager >= 1.52: retain DHCP, self-assign while no lease exists.
      link-local = 4; # NM_SETTING_IP4_LL_FALLBACK (keyfiles require the numeric enum)
      may-fail = true;
    };
    ipv6 = { method = "auto"; may-fail = true; };
  };
in {
  networking.networkmanager.ensureProfiles.profiles = {
    pl64-ethernet-1 = wired "PL64 Ethernet 1" "8503e189-3b11-4a29-9a49-dd7f1f63bdf1" "cc:28:aa:47:ca:07";
    pl64-ethernet-2 = wired "PL64 Ethernet 2" "53ab0225-5baa-4f7c-aead-89b7211b4376" "cc:28:aa:47:cc:2f";
  };
  # Both ports advertise the same host; never reflect multicast between links.
  services.avahi = {
    ipv4 = true;
    ipv6 = true;
  };
}
