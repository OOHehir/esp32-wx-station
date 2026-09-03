#!/usr/bin/env bash
# Tag a release. PROJECT_VER comes from `git describe --tags --dirty`, so in this
# template "bumping the version" means exactly one thing: creating a tag.
#
#   ./tools/bump_version.sh patch          # v0.5.1 -> v0.5.2
#   ./tools/bump_version.sh minor          # v0.5.1 -> v0.6.0
#   ./tools/bump_version.sh major          # v0.5.1 -> v1.0.0
#   ./tools/bump_version.sh v1.2.3         # exactly that
#
# It only tags. Building and publishing stay separate on purpose: a tag that has been
# pushed is hard to take back, and `publish.sh` is what actually puts an image in front
# of devices.
#
# Refuses on a dirty tree. An image built from uncommitted changes gets a `-dirty`
# version, which is a different string from the tag it claims to be -- devices would
# update to it and then never recognise the real release as an update, because policy is
# string equality (PLAN §4).

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

[[ $# -eq 1 ]] || { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//' >&2; exit 1; }

if [[ -n "$(git status --porcelain)" ]]; then
    echo "error: working tree is dirty. Commit or stash first -- a tag on a dirty tree" >&2
    echo "       produces images versioned '<tag>-dirty', which is not the tag." >&2
    git status --short >&2
    exit 1
fi

current="$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")"

case "$1" in
    v[0-9]*)
        next="$1"
        ;;
    major|minor|patch)
        if [[ ! "$current" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
            echo "error: latest tag '$current' is not vMAJOR.MINOR.PATCH, so it cannot be" >&2
            echo "       bumped automatically. Pass the version you want instead." >&2
            exit 1
        fi
        ma=${BASH_REMATCH[1]} mi=${BASH_REMATCH[2]} pa=${BASH_REMATCH[3]}
        case "$1" in
            major) next="v$((ma + 1)).0.0" ;;
            minor) next="v$ma.$((mi + 1)).0" ;;
            patch) next="v$ma.$mi.$((pa + 1))" ;;
        esac
        ;;
    *)
        echo "error: expected major, minor, patch, or an explicit vX.Y.Z -- got '$1'" >&2
        exit 1
        ;;
esac

if git rev-parse -q --verify "refs/tags/$next" > /dev/null; then
    echo "error: tag $next already exists" >&2
    exit 1
fi

git tag -a "$next" -m "$next"
echo "==> tagged $next (was $current)"
echo
echo "The version only reaches an image at CMake *configure* time, so build next:"
echo "    idf.py build"
echo "    ./tools/publish.sh"
echo
echo "Not pushed. When you are ready:  git push origin $next"
