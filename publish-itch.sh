#!/usr/bin/env bash
# Publish the current ROM to the itch.io page via butler — the manual path.
#
# The automatic path lives in .github/workflows/release.yml: every release
# pushes the CI-built ROM to itch, so the GitHub Release asset and the itch
# download are byte-identical. Use THIS script for an out-of-band push (a
# build that never got a release, or re-seeding a channel).
#
# Auth: butler reads the credentials `butler login` stored on disk
# (~/Library/Application Support/itch/butler_creds on macOS). No key here.
#
#   ./publish-itch.sh            # build + push
#   ./publish-itch.sh --no-build # push the ROM already in rom/
set -euo pipefail

cd "$(dirname "$0")"

BUTLER=${BUTLER:-$HOME/bin/butler}
ITCH_TARGET=${ITCH_TARGET:-paisleyboxers/backrooms32x:32x}
ROM=rom/backrooms.32x

[ -x "$BUTLER" ] || { echo "error: butler not found at $BUTLER (set BUTLER=...)" >&2; exit 1; }

if [ "${1:-}" != "--no-build" ]; then
    echo "[pub 1/3] Building release ROM..."
    make release
else
    echo "[pub 1/3] Skipping build (--no-build)"
fi

echo "[pub 2/3] Reading the build stamp..."
[ -f sh_src/version.h ] || { echo "error: sh_src/version.h missing — run make release" >&2; exit 1; }
stamp() { sed -n "s/.*$1 *\"\(.*\)\".*/\1/p" sh_src/version.h; }
BUILD=$(stamp VERSION_BUILD_STR)          # e.g. 00131
SHA=$(stamp VERSION_SHA_STR)              # e.g. d990d15
VERSION=$((10#$BUILD))                    # 00131 -> 131, what itch shows

# The ROM stamps its own identity (pause -> CREDITS), so the ROM must have been
# built from the tree we're publishing. A stamp older than HEAD means a stale
# ROM is about to ship under a fresh version number.
HEAD_SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "$SHA")
if [ "$SHA" != "$HEAD_SHA" ]; then
    echo "  warning: ROM stamp SHA ($SHA) != HEAD ($HEAD_SHA)."
    echo "           The ROM was built from a different commit; run 'make release' first."
    printf "           Push anyway? [y/N] "
    read -r ans; [ "$ans" = "y" ] || { echo "aborted."; exit 1; }
fi
[ -f "$ROM" ] || { echo "error: $ROM missing" >&2; exit 1; }
echo "          build $BUILD ($SHA)  ->  itch version $VERSION"

echo "[pub 3/3] Pushing to $ITCH_TARGET ..."
# --if-changed: don't burn a version number when the bytes are identical.
"$BUTLER" push "$ROM" "$ITCH_TARGET" --userversion "$VERSION" --if-changed

echo
echo "=== Published build $BUILD to itch as version $VERSION ==="
"$BUTLER" status "${ITCH_TARGET%%:*}" || true
