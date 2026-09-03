#!/usr/bin/env bash
# Generate the app signing keys this template builds against.
#
#   ./tools/gen_signing_key.sh              # both keys, if missing
#   ./tools/gen_signing_key.sh --force      # replace them (invalidates every deployed device)
#
# Images are signed at build time and the signature is verified before an OTA image is
# accepted (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT). No eFuse is burned and the
# bootloader is not locked: this is the reversible half of secure boot -- it stops a
# compromised file server, not somebody holding the board.
#
# Two keys, because the signing *scheme* follows the chip:
#
#   keys/app_signing_rsa3072.pem    secure boot v2, RSA-3072 -- esp32s3, esp32c3, esp32c6
#                                   and any classic ESP32 built for revision v3.0+
#   keys/app_signing_ecdsa256.pem   secure boot v1, ECDSA NIST P-256 -- classic ESP32
#                                   below revision v3.0, which has no v2 support
#
# Which one a build uses is CONFIG_SECURE_BOOT_SIGNING_KEY in sdkconfig.defaults[.target].
#
# Plain openssl rather than `espsecure generate-signing-key`, so this works before
# ESP-IDF is activated and in CI. The key files are byte-identical in kind to what
# espsecure produces; espsecure is what consumes them during the build.
#
# THE KEY IS THE FLEET. Losing it means no device can ever be updated again -- the
# trusted key is taken from the signature block of the app already running. Back it up
# somewhere that is not this working tree, and never commit it.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
keys_dir="$repo_root/keys"
force=0

for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "error: unknown argument '$arg'" >&2; exit 1 ;;
    esac
done

command -v openssl > /dev/null || { echo "error: openssl not found" >&2; exit 1; }
mkdir -p "$keys_dir"

make_key() {
    local path="$1" kind="$2" what="$3"
    if [[ -f "$path" && $force -eq 0 ]]; then
        echo "==> keeping existing $(basename "$path")"
        return
    fi
    if [[ -f "$path" ]]; then
        echo "==> REPLACING $(basename "$path") -- every device signed with the old key is now unreachable over OTA"
    fi
    echo "==> generating $(basename "$path")  ($what)"
    case "$kind" in
        rsa)   openssl genrsa -out "$path" 3072 2>/dev/null ;;
        ecdsa) openssl ecparam -name prime256v1 -genkey -noout -out "$path" 2>/dev/null ;;
    esac
    chmod 600 "$path"
}

make_key "$keys_dir/app_signing_rsa3072.pem"  rsa   "secure boot v2, RSA-3072"
make_key "$keys_dir/app_signing_ecdsa256.pem" ecdsa "secure boot v1, ECDSA NIST P-256"

echo
echo "Keys are in keys/ and are gitignored. Back them up off this machine:"
echo "  losing one means no device signed with it can ever be updated again."
