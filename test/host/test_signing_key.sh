#!/usr/bin/env bash
# The signing-key guard in cmake/check_signing_key.cmake turns "esptool cannot find
# secure_boot_signing_key.pem" into "run ./tools/gen_signing_key.sh". Same reasoning as
# test_partitions.sh: a guard that never fires is indistinguishable from one that always
# passes, and this one only fires on a fresh clone -- which is exactly when nobody is
# around to notice it stopped working.
#
# Runs on the host with cmake alone; no ESP-IDF, no toolchain.
#
#   ./test/host/test_signing_key.sh

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script="$repo_root/cmake/check_signing_key.cmake"
pass=0 fail=0

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
: > "$tmp/present.pem"

run_check() {
    # $1 CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES, $2 key path
    cmake \
        -DCONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES="$1" \
        -DCONFIG_SECURE_BOOT_SIGNING_KEY="$2" \
        -P "$script" 2>&1
}

expect() {
    # $1 expect ok|fail, $2 signing on/off, $3 key path, $4 description
    local want="$1" out rc
    out="$(run_check "$2" "$3")"; rc=$?
    local got=ok; [[ $rc -ne 0 ]] && got=fail
    if [[ "$got" == "$want" ]]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$4"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s (expected %s, got %s)\n%s\n' "$4" "$want" "$got" "$out"
    fi
}

echo "check_signing_key.cmake"

# The case the guard exists for: signing on, no key. A fresh clone.
expect fail y "keys/definitely_not_here.pem" "a missing key stops the build"

expect ok   y "$tmp/present.pem"             "an existing key (absolute path) passes"
expect ok   y "keys/README.md"               "an existing key (relative to the project) passes"

# Signing off is a legitimate configuration, and the guard must stay out of its way.
expect ok   n "keys/definitely_not_here.pem" "signing disabled ignores a missing key"
expect ok   ""  "keys/definitely_not_here.pem" "unset signing config ignores a missing key"

# The message has to name the fix, not just the problem -- that is the whole point of it.
out="$(run_check y keys/definitely_not_here.pem)"
if grep -q "gen_signing_key.sh" <<< "$out"; then
    pass=$((pass + 1)); printf '  ok    %s\n' "the error names the script that fixes it"
else
    fail=$((fail + 1)); printf '  FAIL  the error does not name gen_signing_key.sh\n%s\n' "$out"
fi

echo
echo "$((pass + fail)) checks, $fail failed"
[[ $fail -eq 0 ]]
