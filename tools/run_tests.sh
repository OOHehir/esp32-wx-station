#!/usr/bin/env bash
# Every test that runs without hardware.
#
#   ./tools/run_tests.sh
#
# Two kinds, both host-only and both fast:
#   test/host            pure C units -- JSON escaping, DNS reply packing, update and
#                        rollback policy, digest comparison
#   test_*.sh            the build-time guards: partition table, signing key, CA
#
# Neither needs ESP-IDF activated. Anything that genuinely needs a chip belongs on the
# bench, not here.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
fail=0

echo "=== host unit tests ==="
make -s -C test/host check || fail=1

echo
echo "=== build-time guards ==="
./test/host/test_partitions.sh || fail=1
echo
./test/host/test_signing_key.sh || fail=1
echo
./test/host/test_server_cert.sh || fail=1

echo
if [[ $fail -eq 0 ]]; then
    echo "all tests passed"
else
    echo "FAILURES"
fi
exit $fail
