#!/usr/bin/env bash
# Build the template for several targets in one go.
#
#   ./tools/build_matrix.sh                 # the default matrix
#   ./tools/build_matrix.sh esp32c3 esp32   # specific targets
#
# PLAN §1a: a chip-specific assumption should break the build the day it lands, not
# months later. The default matrix therefore spans both instruction sets and both LED
# backends, which is where per-chip divergence actually shows up:
#
#   esp32s3  Xtensa, addressable LED, USB-Serial-JTAG present
#   esp32c6  RISC-V, addressable LED, 8 MB partition table
#   esp32    Xtensa, plain GPIO LED, no USB-Serial-JTAG
#
# Each target builds in its own directory so they do not fight over sdkconfig; the
# checked-out tree is left untouched.

set -uo pipefail

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
    targets=(esp32s3 esp32c6 esp32)
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: ESP-IDF is not activated. Run:" >&2
    echo "  source \"\$HOME/.espressif/tools/activate_idf_v6.0.2.sh\"" >&2
    exit 1
fi

# EIM's activate script defines idf.py as a *shell function*, not an executable on PATH,
# so a script cannot just call `idf.py` -- it gets "command not found" even in a fully
# activated environment. Invoke the real thing instead.
idf() { python "$IDF_PATH/tools/idf.py" "$@"; }

if ! command -v python > /dev/null; then
    echo "error: no python on PATH -- is the IDF environment really activated?" >&2
    exit 1
fi

failed=()
declare -A sizes

for target in "${targets[@]}"; do
    build_dir="build_$target"
    printf '\n=== %s ===\n' "$target"

    if ! idf -B "$build_dir" -DSDKCONFIG="$build_dir/sdkconfig" \
            set-target "$target" > "$build_dir.log" 2>&1; then
        echo "  set-target failed -- see $build_dir.log"
        failed+=("$target")
        continue
    fi
    if ! idf -B "$build_dir" -DSDKCONFIG="$build_dir/sdkconfig" \
            build >> "$build_dir.log" 2>&1; then
        echo "  build failed -- see $build_dir.log"
        grep -E "error:|Error" "$build_dir.log" | head -10 | sed 's/^/    /'
        failed+=("$target")
        continue
    fi

    # An obsolete or misspelled CONFIG_ line is a NOTE, not an error, and scrolls past in
    # a build that otherwise looks perfect -- so it is inert configuration that reads like
    # a decision. Two of them survived here for a whole phase. Surface them.
    if grep -q "unknown kconfig symbol" "$build_dir.log"; then
        echo "  configuration lines that do nothing:"
        grep -o "unknown kconfig symbol '[^']*'" "$build_dir.log" | sort -u | sed 's/^/    /'
    fi

    # Read the image name out of the build rather than hardcoding it. A project forked
    # from this template renames project(), and a hardcoded "esp-ota.bin" then matches
    # nothing -- which used to print "ok" per target and "FAILED" in the summary of a
    # build that had in fact succeeded. Found by actually forking the template (PLAN §6).
    app_bin="$(python -c "
import json
print(json.load(open('$build_dir/project_description.json'))['app_bin'])" 2>/dev/null)"

    line="$(grep -E "^${app_bin:-.*\.bin} binary size" "$build_dir.log" | tail -1)"
    sizes[$target]="${line:-built (size line not found in $build_dir.log)}"
    echo "  ok  ${sizes[$target]}"
done

printf '\n--- summary ---\n'
for target in "${targets[@]}"; do
    printf '%-10s %s\n' "$target" "${sizes[$target]:-FAILED}"
done

if [[ ${#failed[@]} -gt 0 ]]; then
    printf '\n%d target(s) failed: %s\n' "${#failed[@]}" "${failed[*]}"
    exit 1
fi
echo
echo "all targets built"
