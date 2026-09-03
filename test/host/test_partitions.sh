#!/usr/bin/env bash
# The partition-table guard in cmake/check_partitions.cmake exists so that a table that
# does not fit the configured flash size stops the build (PLAN §1a). Its failure mode is
# silence -- a guard that never fires looks exactly like a guard that always passes -- so
# it is worth proving that it fails when it should.
#
# Runs on the host with cmake alone; no ESP-IDF, no toolchain.
#
#   ./test/host/test_partitions.sh

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script="$repo_root/cmake/check_partitions.cmake"
pass=0 fail=0

run_check() {
    # $1 flash size, $2 table filename
    cmake \
        -DCONFIG_PARTITION_TABLE_CUSTOM=y \
        -DCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="$2" \
        -DCONFIG_ESPTOOLPY_FLASHSIZE="$1" \
        -DCONFIG_IDF_TARGET=esp32s3 \
        -P "$script" 2>&1
}

expect() {
    # $1 expect ok|fail, $2 flash size, $3 table, $4 description
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

echo "check_partitions.cmake"

expect ok   4MB partitions_4mb.csv "4 MB table on 4 MB flash"
expect ok   8MB partitions_8mb.csv "8 MB table on 8 MB flash"
expect ok  16MB partitions_8mb.csv "8 MB table on 16 MB flash (room to spare)"

# The one that matters. Without this guard the build succeeds, the device flashes, and
# the second OTA slot silently does not exist.
expect fail 4MB partitions_8mb.csv "8 MB table on 4 MB flash is rejected"
expect fail 2MB partitions_4mb.csv "4 MB table on 2 MB flash is rejected"

# A table whose partitions have no explicit offsets cannot be checked, and silently
# skipping it would defeat the purpose.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cat > "$repo_root/partitions_test_implicit.csv" << 'CSV'
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     ,        0x2000,
CSV
expect fail 4MB partitions_test_implicit.csv "a table with an implicit offset is rejected"
rm -f "$repo_root/partitions_test_implicit.csv"

echo
echo "$((pass + fail)) checks, $fail failed"
[[ $fail -eq 0 ]]
