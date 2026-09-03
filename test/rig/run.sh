#!/usr/bin/env bash
# A stand-in for the NAS, for proving the firmware half of OTA on a bench.
#
#   ./test/rig/run.sh                 # serve on this host's LAN address, port 8443
#   ./test/rig/run.sh 192.168.1.50 9443
#
# Why this exists: the device validates the server against the private CA embedded in
# firmware, not against a specific host. So a certificate minted from the same CA for
# *this* machine is trusted exactly as the NAS's is -- which means the whole OTA path can
# be exercised without write access to the NAS.
#
# This is a bench rig, NOT a firmware server. It has no access control, no rate limiting
# and no range-request support, and it is single-purpose Python rather than the nginx
# config in nas/. Do not leave it running.
#
# Publish into it with:
#   NAS_FIRMWARE_DIR=test/rig/firmware OTA_BASE_URL=https://<host>:8443/fw ./tools/publish.sh
# and point firmware at it by setting CONFIG_APP_OTA_BASE_URL to the same URL.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

host="${1:-$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}')}"
port="${2:-8443}"
rig="test/rig"

if [[ -z "$host" ]]; then
    echo "error: could not work out this host's address; pass it explicitly" >&2
    exit 1
fi
if [[ ! -f nas/certs/ca.key ]]; then
    echo "error: nas/certs/ca.key not found -- run ./tools/gen_certs.sh first." >&2
    echo "       The rig signs its certificate with the same CA the firmware embeds;" >&2
    echo "       without that key the device would reject it, which is the point." >&2
    exit 1
fi

mkdir -p "$rig/certs" "$rig/firmware"

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

# Re-mint whenever the host changes: the address has to be in the SAN list or the device
# rejects the handshake, and that failure looks exactly like a network fault.
if [[ ! -f "$rig/certs/srv.crt" ]] || ! san_covers "$rig/certs/srv.crt" "IP Address:$host"; then
    echo "==> minting a rig certificate for $host from the project CA"
    cat > "$rig/certs/ext.cnf" << EXT
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = IP:$host
EXT
    openssl req -nodes -newkey rsa:2048 \
        -keyout "$rig/certs/srv.key" -out "$rig/certs/srv.csr" \
        -subj "/CN=$host/O=esp-ota bench rig" 2>/dev/null
    openssl x509 -req -in "$rig/certs/srv.csr" \
        -CA nas/certs/ca.crt -CAkey nas/certs/ca.key -CAcreateserial \
        -out "$rig/certs/srv.crt" -days 30 -sha256 -extfile "$rig/certs/ext.cnf" 2>/dev/null
    rm -f "$rig/certs/srv.csr" "$rig/certs/ext.cnf"
    chmod 600 "$rig/certs/srv.key"
fi

echo "==> https://$host:$port/fw/   (Ctrl-C to stop)"
echo "    publish:  NAS_FIRMWARE_DIR=$rig/firmware OTA_BASE_URL=https://$host:$port/fw ./tools/publish.sh"
echo "    firmware: CONFIG_APP_OTA_BASE_URL=\"https://$host:$port/fw\""
exec python3 "$rig/serve_firmware.py" "$rig/firmware" "$rig/certs/srv.crt" "$rig/certs/srv.key" "$port"
