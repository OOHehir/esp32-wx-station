#!/usr/bin/env bash
# Generate a private CA and a NAS server certificate for ESP-IDF OTA over HTTPS.
#
# The device embeds the CA certificate (not the leaf), so the server certificate
# can be re-issued without reflashing firmware.
#
#   ./tools/gen_certs.sh nas.lan 192.168.1.10 [more-names-or-ips ...]
#
# Outputs:
#   nas/certs/ca.crt          CA cert   -> copied to server_certs/ca_cert.pem (embed in firmware)
#   nas/certs/ca.key          CA key    -> keep offline, never leaves your machine
#   nas/certs/server.crt      server cert+chain -> nginx on the NAS
#   nas/certs/server.key      server key        -> nginx on the NAS

set -euo pipefail

CA_DAYS="${CA_DAYS:-3650}"
SERVER_DAYS="${SERVER_DAYS:-825}"   # browsers cap at 398d; mbedTLS does not, but stay modest
KEY_BITS="${KEY_BITS:-2048}"        # RSA-2048: smaller handshake footprint on-device than 4096

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
certs_dir="$repo_root/nas/certs"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <hostname-or-ip> [additional-hostnames-or-ips ...]" >&2
    echo "  e.g. $0 nas.lan 192.168.1.10" >&2
    exit 1
fi

mkdir -p "$certs_dir"
cd "$certs_dir"

# Build the SAN list. The device must find *whatever it dials* in here, so pass both
# the hostname and the IP if you are not certain which one firmware will use.
san=""
dns_n=0
ip_n=0
for name in "$@"; do
    if [[ "$name" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        ip_n=$((ip_n + 1))
        san+="IP.$ip_n = $name"$'\n'
    else
        dns_n=$((dns_n + 1))
        san+="DNS.$dns_n = $name"$'\n'
    fi
done
primary="$1"

# ---- CA -------------------------------------------------------------------
if [[ -f ca.key && -f ca.crt ]]; then
    echo "==> reusing existing CA (delete nas/certs/ca.* to start over)"
else
    echo "==> generating CA (${CA_DAYS}d)"
    openssl req -x509 -nodes -newkey "rsa:$KEY_BITS" \
        -keyout ca.key -out ca.crt -days "$CA_DAYS" -sha256 \
        -subj "/CN=esp-ota local CA/O=esp-ota" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
fi

# ---- server certificate ---------------------------------------------------
echo "==> generating server certificate for: $* (${SERVER_DAYS}d)"
cat > server.ext << EXT
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
$san
EXT

openssl req -nodes -newkey "rsa:$KEY_BITS" \
    -keyout server.key -out server.csr \
    -subj "/CN=$primary/O=esp-ota" 2>/dev/null

openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$SERVER_DAYS" -sha256 -extfile server.ext 2>/dev/null

rm -f server.csr server.ext
chmod 600 ca.key server.key
chmod 644 ca.crt server.crt

# The firmware embeds this one.
cp ca.crt "$repo_root/server_certs/ca_cert.pem"

echo
echo "==> done"
openssl x509 -in server.crt -noout -subject -enddate -ext subjectAltName | sed 's/^/    /'
echo
echo "    nas/certs/server.{crt,key}   -> deploy to the NAS (nas/certs is bind-mounted)"
echo "    server_certs/ca_cert.pem     -> embedded in firmware (NOT in git -- yours alone)"
echo "    nas/certs/ca.key             -> keep this off the NAS"
