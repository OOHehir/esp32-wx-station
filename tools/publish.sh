#!/usr/bin/env bash
# Publish a built firmware image to the OTA server on the NAS.
#
#   ./tools/publish.sh [build-dir]
#
# Everything -- project name, version, and target chip -- is read from the build
# output, so this script is chip-agnostic: build for any target and publish the
# same way. Nothing here needs to know which chip you chose.
#
# Layout produced on the server:
#   firmware/<project>/<chip>/manifest.json
#   firmware/<project>/<chip>/<version>/<project>.bin
#
# Destination is either a local path or a remote rsync target:
#   NAS_FIRMWARE_DIR=/volume1/docker/esp-ota/firmware   (local mount)
#   NAS_HOST=admin@nas.lan                              (rsync over ssh)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-build}"
desc="$build_dir/project_description.json"

if [[ ! -f "$desc" ]]; then
    echo "error: $desc not found -- build the project first (idf.py build)" >&2
    exit 1
fi

read -r project version chip app_bin << PY
$(python3 - "$desc" << 'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
print(d["project_name"], d["project_version"], d["target"], d["app_bin"])
PYEOF
)
PY

bin_path="$build_dir/$app_bin"
[[ -f "$bin_path" ]] || { echo "error: $bin_path not found" >&2; exit 1; }

if [[ "$version" == *"dirty"* ]]; then
    echo "warning: version '$version' contains uncommitted changes" >&2
fi

sha256="$(sha256sum "$bin_path" | cut -d' ' -f1)"
size="$(stat -c%s "$bin_path")"
base_url="${OTA_BASE_URL:-https://nas.lan:8443/fw}"

echo "==> $project $version ($chip), $((size / 1024)) KiB"

# Say out loud whether this image is signed. A device built with signing enabled refuses
# an unsigned image -- correctly -- but only after downloading the whole thing, and the
# error it reports names a signature, not a build setting. Cheaper to notice here.
signed="$(python3 - "$build_dir/config/sdkconfig.json" << 'PYEOF' 2>/dev/null || true
import json, sys
print("yes" if json.load(open(sys.argv[1])).get("SECURE_SIGNED_APPS_NO_SECURE_BOOT") else "no")
PYEOF
)"
case "$signed" in
    yes) echo "    signed: yes" ;;
    no)  echo "warning: this image is NOT signed. Devices built with the template's" >&2
         echo "         defaults will refuse it -- see keys/README.md." >&2 ;;
    *)   echo "    signed: unknown (no build config found)" ;;
esac

# Stage the release locally, then ship the whole tree in one go.
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
rel="$project/$chip"
mkdir -p "$staging/$rel/$version"
cp "$bin_path" "$staging/$rel/$version/$project.bin"

cat > "$staging/$rel/manifest.json" << MANIFEST
{
  "project": "$project",
  "chip": "$chip",
  "version": "$version",
  "url": "$base_url/$rel/$version/$project.bin",
  "sha256": "$sha256",
  "size": $size,
  "mandatory": false
}
MANIFEST

# mktemp -d creates the staging tree 0700, and `rsync -a` with a trailing slash on the
# source propagates the source directory's mode onto the *destination* directory. Publish
# without this and /volume1/docker/esp-ota/firmware becomes 0700 owned by whoever ran the
# script -- so nginx, which runs as a different user inside its container, answers 403 to
# every request including the ones it was serving fine a minute earlier.
#
# a+rX, not a+rx: the capital X adds the search bit to directories only, leaving the image
# 0644 rather than making a firmware binary executable for no reason.
#
# The bench rig cannot catch this. It serves as the same user that published, so 0700 is
# perfectly readable and everything looks fine right up until a real server tries.
# go-w as well as a+rX: a+rX only adds bits, so a developer with a permissive umask
# would otherwise publish a group-writable web root.
chmod -R a+rX,go-w "$staging"

if [[ -n "${NAS_HOST:-}" ]]; then
    dest="${NAS_FIRMWARE_DIR:-/volume1/docker/esp-ota/firmware}"
    echo "==> rsync -> $NAS_HOST:$dest"
    rsync -a "$staging/" "$NAS_HOST:$dest/"
elif [[ -n "${NAS_FIRMWARE_DIR:-}" ]]; then
    echo "==> copy -> $NAS_FIRMWARE_DIR"
    mkdir -p "$NAS_FIRMWARE_DIR"
    cp -a "$staging/." "$NAS_FIRMWARE_DIR/"
else
    echo "error: set NAS_HOST (rsync over ssh) or NAS_FIRMWARE_DIR (local path)" >&2
    exit 1
fi

echo "==> published"
echo "    manifest: $base_url/$rel/manifest.json"
echo "    image:    $base_url/$rel/$version/$project.bin"

# Then check the server will actually hand it back. Copying a file somewhere is not the
# same as publishing it -- the permissions trap above proved that by turning a working
# server into a wall of 403s, and neither rsync nor this script noticed until a device
# failed to update hours later. Fetching what we just wrote costs a second.
ca="$repo_root/server_certs/ca_cert.pem"
if ! command -v curl > /dev/null; then
    echo "    (no curl -- publish not verified)"
    exit 0
fi
body="$(mktemp)"
trap 'rm -rf "$staging" "$body"' EXIT

verify_args=(--silent --show-error --max-time 15 -o "$body" -w '%{http_code}')
[[ -f "$ca" ]] && verify_args+=(--cacert "$ca")

code="$(curl "${verify_args[@]}" "$base_url/$rel/manifest.json" 2>/dev/null || echo "000")"
case "$code" in
    200)
        # Not just "something answered": the *version we just published* has to be the one
        # the server hands back. Otherwise a base URL pointing at a different server than
        # the one written to reads as a successful publish, which is the exact mistake
        # this check exists to catch.
        if grep -q "\"version\": *\"$version\"" "$body"; then
            echo "    verified: the server returns this release (HTTP 200, $version)"
        else
            echo >&2
            echo "error: $base_url is serving a manifest, but not the one just published." >&2
            echo "       Expected version $version; got:" >&2
            sed 's/^/         /' "$body" >&2
            echo "       OTA_BASE_URL and the destination probably name different servers." >&2
            exit 1
        fi
        ;;
    000) echo "    (could not reach $base_url from here -- publish not verified)" ;;
    403) echo >&2
         echo "error: the server has the manifest but will not serve it (HTTP 403)." >&2
         echo "       Almost always permissions: the web server runs as a different user" >&2
         echo "       than the one that published. Check that everything under the" >&2
         echo "       firmware directory -- and the directory itself -- is world-readable." >&2
         exit 1 ;;
    *)   echo >&2
         echo "error: published, but the server answered HTTP $code for the manifest." >&2
         echo "       Devices will not see this release." >&2
         exit 1 ;;
esac
