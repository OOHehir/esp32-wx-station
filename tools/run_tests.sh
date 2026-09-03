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

# --- cJSON -----------------------------------------------------------------------------
#
# The host tests link the same parser the firmware does, but it arrives as an ESP-IDF
# managed component -- written into a gitignored directory by `idf.py build`. A fresh
# clone therefore has no cJSON at all, which is how CI, which has no ESP-IDF, failed on
# a suite advertised as needing neither a toolchain nor hardware.
#
# Use the component if it is there, so a developer who has built the firmware tests
# exactly what ships. Otherwise fetch a pinned copy once into a gitignored cache.
#
# The pinned source is upstream v1.7.19, the version the component resolves to. Espressif
# add three hunks to it: a NULL guard in cJSON_SetNumberHelper and a nesting-depth check
# in each of print_array and print_object. All three are on the printing path, which
# nothing here calls -- the parse path is byte-identical, & that is what these tests are
# about. Checked, not assumed; check it again before moving the pin.
CJSON_VERSION="v1.7.19"
CJSON_SHA_C="298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d"
CJSON_SHA_H="25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee"

cjson_dir="$repo_root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
    cjson_dir="$repo_root/.cache/cjson/$CJSON_VERSION"
    if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
        echo "=== fetching cJSON $CJSON_VERSION (no managed component in this tree) ==="
        mkdir -p "$cjson_dir"
        for f in cJSON.c cJSON.h; do
            if ! curl -fsSL --max-time 60 \
                 "https://raw.githubusercontent.com/DaveGamble/cJSON/$CJSON_VERSION/$f" \
                 -o "$cjson_dir/$f"; then
                echo "error: could not fetch $f -- no network, and this tree has no" >&2
                echo "       managed_components/. Build the firmware once, or copy a" >&2
                echo "       cJSON $CJSON_VERSION checkout to $cjson_dir" >&2
                exit 1
            fi
        done
        # Verified because an unverified download is a third party choosing what the
        # test suite compiles.
        if ! echo "$CJSON_SHA_C  $cjson_dir/cJSON.c" | sha256sum -c --quiet - ||
           ! echo "$CJSON_SHA_H  $cjson_dir/cJSON.h" | sha256sum -c --quiet -; then
            echo "error: fetched cJSON does not match the pinned checksums -- refusing" >&2
            rm -rf "$cjson_dir"
            exit 1
        fi
    fi
fi

echo "=== host unit tests ==="
make -s -C test/host check CJSON_DIR="$cjson_dir" || fail=1

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
