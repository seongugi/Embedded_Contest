#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

# Run from this folder or any working directory:
#   sudo bash apply-nm-backport.sh [Wi-Fi profile name or UUID]
#   sudo bash apply-nm-backport.sh --check [Wi-Fi profile name or UUID]
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BASE='1.52.1-1+rpt4'
PREVIOUS="${BASE}+codex1"
TARGET="${BASE}+codex2"
WIFI_IFACE='wlan1'
MANUAL_IFACE='wlan0'
CHECK=0
PROFILE=''
BACKUP=''

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
installed() {
    [[ "$(dpkg-query -W -f='${Status}' "$1" 2>/dev/null || true)" == 'install ok installed' ]]
}
trap 'printf "ERROR at line %s. Backup: %s\n" "$LINENO" "${BACKUP:-not created}" >&2' ERR

for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        -h|--help)
            printf 'Usage: sudo bash %s [--check] [Wi-Fi profile name or UUID]\n' "$0"
            exit 0 ;;
        --*) die "Unknown option: $arg" ;;
        *) [[ -z "$PROFILE" ]] || die 'Specify only one Wi-Fi profile.'
           PROFILE="$arg" ;;
    esac
done

if (( EUID != 0 )); then
    exec sudo bash "$ROOT/apply-nm-backport.sh" "$@"
fi

for cmd in dpkg dpkg-query dpkg-deb nmcli apt-get sha256sum awk systemctl python3 NetworkManager; do
    command -v "$cmd" >/dev/null || die "Required command missing: $cmd"
done
[[ "$(dpkg --print-architecture)" == 'arm64' ]] || die 'arm64 is required.'
. /etc/os-release
[[ "${VERSION_CODENAME:-}" == 'trixie' ]] || die 'Trixie is required.'
[[ -f "$ROOT/SHA256SUMS" ]] || die "Missing $ROOT/SHA256SUMS"

printf '\n[1/5] Verify files and installed version\n'
(cd "$ROOT" && sha256sum -c SHA256SUMS)
for pkg in network-manager libnm0; do
    installed "$pkg" || die "$pkg is not fully installed."
    version="$(dpkg-query -W -f='${Version}' "$pkg")"
    [[ "$version" == "$BASE" || "$version" == "$PREVIOUS" || "$version" == "$TARGET" ]] \
        || die "$pkg: expected $BASE, $PREVIOUS or $TARGET, found $version"
done
systemctl is-active --quiet NetworkManager || die 'NetworkManager is not running.'
[[ "$(nmcli -g GENERAL.NM-MANAGED device show "$MANUAL_IFACE")" == 'no' ]] \
    || die 'wlan0 must already be unmanaged by NetworkManager. Nothing was changed.'
[[ "$(nmcli -g GENERAL.NM-MANAGED device show "$WIFI_IFACE")" == 'yes' ]] \
    || die 'wlan1 must be present and managed by NetworkManager.'
python3 "$ROOT/tools/check_wlan0_policy.py"

printf '\n[2/5] Select Wi-Fi profile\n'
if [[ -n "$PROFILE" ]]; then
    UUID="$(nmcli -g connection.uuid connection show "$PROFILE")"
else
    active_profiles="$(nmcli -t -f UUID,TYPE,DEVICE connection show --active)"
    mapfile -t candidates < <(printf '%s\n' "$active_profiles" |
        awk -F: '$2 == "802-11-wireless" && $3 == "wlan1" {print $1}')
    if (( ${#candidates[@]} == 0 )); then
        saved_profiles="$(nmcli -t -f UUID,TYPE connection show)"
        candidates=()
        while IFS=: read -r saved_uuid saved_type; do
            [[ "$saved_type" == '802-11-wireless' ]] || continue
            if [[ "$(nmcli -g connection.interface-name connection show uuid "$saved_uuid")" == "$WIFI_IFACE" ]]; then
                candidates+=("$saved_uuid")
            fi
        done <<< "$saved_profiles"
    fi
    if (( ${#candidates[@]} != 1 )); then
        nmcli -f NAME,UUID,TYPE connection show
        die 'Pass the wlan1 Wi-Fi profile name or UUID as the last argument.'
    fi
    UUID="${candidates[0]}"
fi
[[ "$UUID" =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}$ ]] \
    || die 'The profile must resolve to one UUID.'
[[ "$(nmcli -g connection.type connection show uuid "$UUID")" == '802-11-wireless' ]] \
    || die 'The selected profile is not Wi-Fi.'
PROFILE="$(nmcli -g connection.id connection show uuid "$UUID")"
bound_iface="$(nmcli -g connection.interface-name connection show uuid "$UUID")"
[[ -z "$bound_iface" || "$bound_iface" == '--' || "$bound_iface" == "$WIFI_IFACE" ]] \
    || die "This profile is bound to $bound_iface, not wlan1."
active_profiles="$(nmcli -t -f UUID,DEVICE connection show --active)"
other_device="$(printf '%s\n' "$active_profiles" | awk -F: -v id="$UUID" \
    '$1 == id && $2 != "wlan1" {print $2}')"
[[ -z "$other_device" ]] || die "This profile is active on another device: $other_device"
printf 'Profile: %s (%s)\n' "$PROFILE" "$UUID"

declare -a debs=() names=()
for pkg in network-manager libnm0 gir1.2-nm-1.0 libnm-dev \
    network-manager-dev network-manager-l10n network-manager-config-connectivity-debian \
    network-manager-dbgsym libnm0-dbgsym; do
    if [[ "$pkg" != 'network-manager' && "$pkg" != 'libnm0' ]] && ! installed "$pkg"; then
        continue
    fi
    deb="$ROOT/packages/${pkg}_${TARGET}_arm64.deb"
    [[ -f "$deb" ]] || deb="$ROOT/packages/${pkg}_${TARGET}_all.deb"
    [[ -f "$deb" ]] || die "Missing package: $pkg"
    [[ "$(dpkg-deb -f "$deb" Package)" == "$pkg" ]] || die "Package name mismatch: $deb"
    [[ "$(dpkg-deb -f "$deb" Version)" == "$TARGET" ]] || die "Version mismatch: $deb"
    arch="$(dpkg-deb -f "$deb" Architecture)"
    [[ "$arch" == 'arm64' || "$arch" == 'all' ]] || die "Architecture mismatch: $deb"
    debs+=("$deb")
    names+=("$pkg")
done

printf '\n[3/5] Check installation plan\n'
plan="$(apt-get --simulate --no-remove install "${debs[@]}")"
printf '%s\n' "$plan"
# Do not let dependency resolution update wpasupplicant or unrelated software.
while read -r changed; do
    [[ -n "$changed" ]] || continue
    allowed=0
    for pkg in "${names[@]}"; do
        [[ "${changed%%:*}" != "$pkg" ]] || allowed=1
    done
    (( allowed )) || die "APT would change an unrelated package: $changed"
done < <(printf '%s\n' "$plan" | awk '$1 == "Inst" || $1 == "Conf" || $1 == "Remv" {print $2}')
if (( CHECK )); then
    printf '\nCHECK PASSED. No packages or settings were changed.\n'
    exit 0
fi

printf '\n[4/5] Back up settings and install packages\n'
umask 077
BACKUP="$(mktemp -d /var/backups/nm-backport.XXXXXXXX)"
cp -a /etc/NetworkManager "$BACKUP/"
[[ ! -d /etc/netplan ]] || cp -a /etc/netplan "$BACKUP/"
printf '%s\n' "$UUID" > "$BACKUP/profile-uuid.txt"
nmcli -g connection.interface-name,connection.auth-retries,connection.autoconnect,connection.autoconnect-retries \
    connection show uuid "$UUID" > "$BACKUP/retry-settings.txt"
dpkg-query -W -f='${binary:Package}\t${Version}\n' "${names[@]}" > "$BACKUP/package-versions.txt"
printf 'Backup: %s\n' "$BACKUP"
NEEDRESTART_MODE=l apt-get -y --no-remove install "${debs[@]}"
for pkg in "${names[@]}"; do
    installed "$pkg" || die "$pkg is not fully installed."
    [[ "$(dpkg-query -W -f='${Version}' "$pkg")" == "$TARGET" ]] || die "$pkg version check failed."
done

printf '\n[5/5] Enable unlimited retries and restart NetworkManager\n'
nmcli connection modify uuid "$UUID" \
    connection.interface-name "$WIFI_IFACE" \
    connection.auth-retries 0 \
    connection.autoconnect yes \
    connection.autoconnect-retries 0
systemctl restart NetworkManager
systemctl is-active --quiet NetworkManager || die 'NetworkManager failed to start.'
[[ "$(nmcli -g GENERAL.NM-MANAGED device show "$MANUAL_IFACE")" == 'no' ]] \
    || die 'wlan0 unmanaged state changed after restart.'
nmcli --wait 60 connection up uuid "$UUID" ifname "$WIFI_IFACE"
settings="$(nmcli -g connection.interface-name,connection.auth-retries,connection.autoconnect,connection.autoconnect-retries \
    connection show uuid "$UUID")"
[[ "$settings" == $'wlan1\n0\nyes\n0' ]] || die 'Retry settings verification failed.'
printf '\nDONE: %s installed; unlimited retries enabled for %s.\n' "$TARGET" "$PROFILE"
printf 'Backup: %s\n' "$BACKUP"
