#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export TMPDIR=/var/tmp

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo bash $0 <builder-user>" >&2
    exit 1
fi

builder_user=${1:-${SUDO_USER:-}}
if [[ -z "$builder_user" || "$builder_user" == root ]]; then
    echo "Specify the ordinary user who will run builds." >&2
    exit 1
fi
builder_home=$(getent passwd "$builder_user" | cut -d: -f6)
builder_group=$(id -gn "$builder_user")
[[ -n "$builder_home" && -d "$builder_home" ]]
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]]

if ! command -v curl >/dev/null || ! command -v xz >/dev/null || ! command -v rsync >/dev/null; then
    apt-get update
    apt-get install -y ca-certificates curl xz-utils rsync
fi

if [[ ! -x /nix/var/nix/profiles/default/bin/nix ]]; then
    installer=$(mktemp /var/tmp/pl64-nix-install.XXXXXXXX)
    trap 'rm -f "$installer"' EXIT
    curl --fail --location --proto '=https' --tlsv1.2 \
        https://nixos.org/nix/install -o "$installer"
    env NIX_INSTALLER_YES=1 bash "$installer" --daemon --no-channel-add
fi

install -d -m 0755 /etc/nix
cat > /etc/nix/pl64-builder.conf <<'NIX_CONF'
experimental-features = nix-command flakes
max-jobs = 4
cores = 2
auto-optimise-store = true
min-free = 5368709120
max-free = 10737418240
NIX_CONF

if ! grep -Fxq 'include /etc/nix/pl64-builder.conf' /etc/nix/nix.conf; then
    printf '\ninclude /etc/nix/pl64-builder.conf\n' >> /etc/nix/nix.conf
fi

systemctl restart nix-daemon.service
install -d -o "$builder_user" -g "$builder_group" -m 0755 "$builder_home/pl64"
runuser -u "$builder_user" -- /nix/var/nix/profiles/default/bin/nix store info --store daemon
printf '%s\n' PL64_NIX_BUILDER_READY
