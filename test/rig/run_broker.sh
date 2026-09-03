#!/usr/bin/env bash
# A stand-in for Mosquitto on the NAS, for proving app_mqtt on a bench.
#
#   ./test/rig/run_broker.sh                        # this host's LAN address, port 8883
#   ./test/rig/run_broker.sh 192.168.1.50 8883 user pass
#
# Same idea as run.sh: the device validates the broker against the private CA embedded in
# firmware, so a certificate minted from that CA for *this* machine is trusted exactly as
# the NAS's is. Reuses the certificate run.sh mints, so run that first (or just once).
#
# broker.py is NOT a broker -- see the warning at the top of it. It exists so the firmware
# half of PLAN §8 can be tested without deploying anything, and it should not outlive the
# test. The real thing is nas/mosquitto.conf.
#
# Publish a command to the device by appending to the command file it prints:
#   echo 'esp-ota/all/cmd check' >> <cmdfile>
#
# which is the bench equivalent of:
#   mosquitto_pub -h nas.lan -p 8883 --cafile nas/certs/ca.crt -u esp-ota -P '...' \
#                 -t esp-ota/all/cmd -m check

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

host="${1:-$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}')}"
port="${2:-8883}"
user="${3:-bench-user}"
pass="${4:-bench-pass-123}"
rig="test/rig"
cmd="${MQTT_CMD_FILE:-$rig/mqtt-cmd}"

if [[ ! -f "$rig/certs/srv.crt" ]]; then
    echo "error: $rig/certs/srv.crt not found -- run ./test/rig/run.sh first, which mints" >&2
    echo "       a certificate for this host from the project CA. The broker reuses it so" >&2
    echo "       there is one trust anchor on the bench, exactly as there is on the NAS." >&2
    exit 1
fi
# Does the certificate carry an exact SAN entry for this host?
#
# Exact, because the obvious `grep -q "$host"` is a substring match with the host used as
# a regex: a rig on 192.168.1.5 matches "IP Address:192.168.1.50" & then reuses a
# certificate that does not cover the address it serves -- reproducing the handshake
# failure the check exists to prevent.
san_covers() {
    local crt="$1" want="$2"
    openssl x509 -in "$crt" -noout -ext subjectAltName 2>/dev/null \
        | tr ',' '\n' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' \
        | grep -qxF "$want"
}
if ! san_covers "$rig/certs/srv.crt" "IP Address:$host"; then
    echo "error: $rig/certs/srv.crt has no SAN for $host -- re-run ./test/rig/run.sh $host" >&2
    exit 1
fi

echo "==> mqtts://$host:$port   user '$user'   (Ctrl-C to stop)"
echo "    firmware: CONFIG_APP_MQTT_ENABLE=y"
echo "              CONFIG_APP_MQTT_BROKER_URI=\"mqtts://$host:$port\""
echo "              CONFIG_APP_MQTT_USERNAME=\"$user\""
echo "              CONFIG_APP_MQTT_PASSWORD=\"$pass\""
echo "    command:  echo 'esp-ota/all/cmd check' >> $cmd"
exec python3 "$rig/broker.py" "$rig/certs/srv.crt" "$rig/certs/srv.key" "$port" "$user" "$pass" "$cmd"
