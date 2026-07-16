#!/usr/bin/env bash
# Pull a RELEASE ROM — the exact bytes CI built and published — instead of
# building one locally.
#
# Why bother when `make release` exists: a local build is NOT the released
# artifact. CI pins marsdev (sh-elf-gcc 13.1.0); this machine has 15.2.0. Same
# source, different codegen, different bytes — under the same version number.
# When you want to play/verify what the world downloaded, fetch it; don't
# rebuild it. This matters more as contributors trigger their own builds.
#
#   ./fetch-release.sh                   # latest release
#   ./fetch-release.sh build-135         # a specific one
#   ./fetch-release.sh --deploy          # latest, then push to the MiSTers
#   ./fetch-release.sh build-135 --deploy
#
# The ROM lands in rom/release/ (NOT rom/backrooms.32x — that's your local
# build, and clobbering it would leave you unsure which one you're testing).
set -euo pipefail
cd "$(dirname "$0")"

TAG=""; DEPLOY=0
for a in "$@"; do
    case "$a" in
        --deploy) DEPLOY=1 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) TAG="$a" ;;
    esac
done

command -v gh >/dev/null || { echo "error: gh CLI not found" >&2; exit 1; }

if [ -z "$TAG" ]; then
    TAG=$(gh release list --limit 1 --json tagName -q '.[0].tagName')
    [ -n "$TAG" ] || { echo "error: no releases found" >&2; exit 1; }
fi

OUT="rom/release"
ROM="$OUT/backrooms-$TAG.32x"
mkdir -p "$OUT"

echo "==> Fetching $TAG"
rm -f "$ROM"
gh release download "$TAG" --pattern 'backrooms.32x' --output "$ROM" --clobber

# The ROM stamps its own identity (pause -> CREDITS), and those strings survive
# into the binary — so the download can prove what it is rather than trusting
# the filename. A mismatch here means the release asset isn't what its tag says.
BUILD=$(strings -a "$ROM" | sed -n 's/^BUILD \([0-9]*\)$/\1/p' | head -1)
SHA=$(strings -a "$ROM"   | sed -n 's/^SHA   \(.*\)$/\1/p'     | head -1)
DATE=$(strings -a "$ROM"  | sed -n 's/^DATE  \(.*\)$/\1/p'     | head -1)
echo "    stamped: build $BUILD  sha $SHA  date $DATE"

WANT="${TAG#build-}"
if [ -n "$BUILD" ] && [ "$((10#$BUILD))" != "$WANT" ]; then
    echo "    WARNING: $TAG contains a ROM stamped build $((10#$BUILD))." >&2
    echo "             The tag and the artifact disagree." >&2
fi
echo "    -> $ROM  ($(stat -f%z "$ROM" 2>/dev/null || stat -c%s "$ROM") bytes)"

if [ "$DEPLOY" = "1" ]; then
    # Reuse the Makefile's MiSTer probing (usb0 then usb1) rather than
    # duplicating it, by pointing it at the fetched ROM.
    # deploy-rom, NOT deploy: the latter depends on `release` and would rebuild
    # with this machine's toolchain, which is the whole thing we're avoiding.
    echo "==> Deploying $TAG to the MiSTers (exact release bytes, no rebuild)"
    make deploy-rom    ROM="$ROM" || echo "    (office MiSTer unreachable)"
    make deploy-rom-tv ROM="$ROM" || echo "    (tv MiSTer unreachable)"
fi

echo
echo "Play it:  open -a ares $ROM"
echo "CREDITS should read BUILD $BUILD ($SHA)"
