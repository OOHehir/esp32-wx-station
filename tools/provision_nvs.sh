#!/usr/bin/env bash
# Write Wi-Fi credentials straight into the device's NVS partition, skipping the browser
# portal entirely.
#
#   ./tools/provision_nvs.sh -p /dev/ttyUSB0 -s "MySSID" -w "passphrase"
#   APP_WIFI_PASS="passphrase" ./tools/provision_nvs.sh -p /dev/ttyUSB0 -s "MySSID"
#
# The portal (PLAN §4b) is how a device gets commissioned in the field, by someone with
# nothing but a phone. This is the other case: a bench or a production line, where the
# board is already on the end of a USB cable and walking through a captive portal for
# each unit is absurd.
#
# It writes the same keys app_cfg writes, in the same namespace, so the firmware cannot
# tell the difference -- and `POST /api/forget` still returns the device to the portal.
#
# Prefer $APP_WIFI_PASS over -w: an argument is visible in `ps` to every user on the box.

set -euo pipefail

port=""
ssid=""
pass="${APP_WIFI_PASS:-}"
offset=""

usage() {
    echo "usage: $0 -p <port> -s <ssid> [-w <passphrase>] [-o <nvs offset>]" >&2
    echo "  passphrase may also come from \$APP_WIFI_PASS (preferred)" >&2
    exit 1
}

while getopts "p:s:w:o:h" opt; do
    case "$opt" in
        p) port="$OPTARG" ;;
        s) ssid="$OPTARG" ;;
        w) pass="$OPTARG" ;;
        o) offset="$OPTARG" ;;
        *) usage ;;
    esac
done

[[ -n "$port" && -n "$ssid" ]] || usage

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: ESP-IDF is not activated." >&2
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# The namespace has to match CONFIG_APP_CFG_NAMESPACE, and the partition offset has to
# match the table this firmware was actually built with -- so read both out of the build
# rather than hardcoding them here and drifting.
namespace="$(sed -n 's/^#define CONFIG_APP_CFG_NAMESPACE "\(.*\)"$/\1/p' \
                 build/config/sdkconfig.h 2>/dev/null || true)"
namespace="${namespace:-appcfg}"

if [[ -z "$offset" ]]; then
    offset="$(python "$IDF_PATH/components/partition_table/parttool.py" \
                  --partition-table-file build/partition_table/partition-table.bin \
                  get_partition_info --partition-name nvs --info offset 2>/dev/null || true)"
fi
if [[ -z "$offset" ]]; then
    echo "error: could not find the nvs partition offset -- build the project first," >&2
    echo "       or pass it explicitly with -o 0x9000" >&2
    exit 1
fi

size="$(python "$IDF_PATH/components/partition_table/parttool.py" \
            --partition-table-file build/partition_table/partition-table.bin \
            get_partition_info --partition-name nvs --info size 2>/dev/null || echo 0x6000)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
csv="$work/nvs.csv"
img="$work/nvs.bin"

# nvs_partition_gen's CSV is positional: key,type,encoding,value.
{
    echo "key,type,encoding,value"
    echo "$namespace,namespace,,"
    printf 'wifi_ssid,data,string,%s\n' "$ssid"
    printf 'wifi_pass,data,string,%s\n' "$pass"
} > "$csv"

echo "==> generating NVS image (namespace '$namespace', $size bytes)"
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
    generate "$csv" "$img" "$size" > /dev/null

echo "==> writing to $port at $offset"
python -m esptool --port "$port" write-flash "$offset" "$img"

echo
echo "==> done. Reset the board; it should join \"$ssid\" instead of raising the setup AP."
echo "    POST /api/forget still clears this and returns the device to the portal."
