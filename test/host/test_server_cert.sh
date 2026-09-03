#!/usr/bin/env bash
# The CA guard in cmake/check_server_cert.cmake is the one that only ever fires on a
# fresh clone -- which is precisely why it needs a test. Without it the failure is
# CMake's own "Cannot find source file: .../ca_cert.pem" at generate time, which names a
# certificate as a source file and suggests nothing.
#
# Runs on the host with cmake alone; no ESP-IDF, no toolchain.
#
#   ./test/host/test_server_cert.sh

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script="$repo_root/cmake/check_server_cert.cmake"
pass=0 fail=0

# The guard looks for server_certs/ca_cert.pem relative to cmake/. Run it against a
# throwaway tree so the developer's real certificate is neither needed nor consulted.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/with/cmake" "$tmp/with/server_certs" "$tmp/without/cmake" "$tmp/without/server_certs"
cp "$script" "$tmp/with/cmake/"
cp "$script" "$tmp/without/cmake/"
: > "$tmp/with/server_certs/ca_cert.pem"

run_check() {   # $1 tree, $2 value for CONFIG_APP_TRUST_PRIVATE_CA
    cmake -DCONFIG_APP_TRUST_PRIVATE_CA="$2" \
          -P "$tmp/$1/cmake/check_server_cert.cmake" 2>&1
}

expect() {      # $1 ok|fail, $2 tree, $3 trust value, $4 description
    local want="$1" out rc
    out="$(run_check "$2" "$3")"; rc=$?
    local got=ok; [[ $rc -ne 0 ]] && got=fail
    if [[ "$got" == "$want" ]]; then
        pass=$((pass + 1)); printf '  ok    %s\n' "$4"
    else
        fail=$((fail + 1)); printf '  FAIL  %s (expected %s, got %s)\n%s\n' "$4" "$want" "$got" "$out"
    fi
}

echo "check_server_cert.cmake"

# The case it exists for.
expect fail without y  "a missing ca_cert.pem stops the build"
expect ok   with    y  "an existing ca_cert.pem passes"

# The one legitimate way not to need a CA. Deliberately no longer gated on OTA being
# enabled: the trust anchor became project-wide when app_mqtt started needing it too, so
# a build with OTA compiled out and MQTT on still embeds it.
expect ok   without n  "trusting the public root bundle needs no CA"
expect ok   without "" "unset config needs no CA"

# The message has to name the fix and the alternative, not just the problem.
out="$(run_check without y)"
for want in "gen_certs.sh" "CONFIG_APP_TRUST_CERT_BUNDLE"; do
    if grep -q "$want" <<< "$out"; then
        pass=$((pass + 1)); printf '  ok    the error mentions %s\n' "$want"
    else
        fail=$((fail + 1)); printf '  FAIL  the error does not mention %s\n%s\n' "$want" "$out"
    fi
done

echo
echo "$((pass + fail)) checks, $fail failed"
[[ $fail -eq 0 ]]
