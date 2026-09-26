#!/bin/sh
# Publish a release: the FIRE's firmware and the Mac app, built from main, as a GitHub release.
# The Mac app checks the latest release and offers it as an update (companion/Sources/Updater.swift).
#
#   1. set the new version in firmware/tock/system.h (TOCK_VERSION) and companion/Info.plist
#      (CFBundleShortVersionString), and add a "## <version>" section to CHANGELOG.md
#   2. commit that on main and push it
#   3. ./release.sh
#
# Needs the gh CLI, signed in. Only runs on main, clean and the same as origin/main, so a
# release is always exactly what's on main.
set -e
cd "$(dirname "$0")"
die() { echo "release: $*" >&2; exit 1; }

V=$(sed -n 's/^#define TOCK_VERSION "\([^"]*\)".*/\1/p' firmware/tock/system.h)
APP_V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" companion/Info.plist)
[ -n "$V" ] || die "no TOCK_VERSION in firmware/tock/system.h"
[ "$V" = "$APP_V" ] || die "the firmware says $V, companion/Info.plist says $APP_V"
grep -qE "^## $V( |$)" CHANGELOG.md || die "CHANGELOG.md has no \"## $V\" section"

[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "releases come from main"
[ -z "$(git status --porcelain)" ] || die "commit or stash your changes first"
git fetch -q origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || die "main isn't the same as origin/main: push (or pull) first"
if gh release view "v$V" >/dev/null 2>&1; then die "v$V is already released: bump the version"; fi
if git rev-parse -q --verify "refs/tags/v$V" >/dev/null && [ "$(git rev-parse "v$V^{commit}")" != "$(git rev-parse HEAD)" ]; then
  die "tag v$V exists and isn't main's tip: bump the version"
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "building the firmware"
firmware/flash.sh --release >/dev/null
cp firmware/build/tock.ino.bin "$OUT/tock-fire.bin"

echo "building the Mac app"
companion/build.sh >/dev/null
ditto -c -k --keepParent companion/build/Tock.app "$OUT/Tock.zip"

(cd "$OUT" && shasum -a 256 Tock.zip tock-fire.bin > SHA256SUMS)
# the notes: this version's section of the changelog
awk -v v="$V" '$0 ~ "^## " v "( |$)" { on = 1; next } /^## / { on = 0 } on' CHANGELOG.md > "$OUT/notes.md"

[ -z "$(git status --porcelain)" ] || die "the build changed tracked files (git status): commit them, then run again"

echo "publishing v$V"
if git rev-parse -q --verify "refs/tags/v$V" >/dev/null; then
  git push -q origin "v$V"
  gh release create "v$V" --verify-tag --title "Tock $V" --notes-file "$OUT/notes.md" \
    "$OUT/Tock.zip" "$OUT/tock-fire.bin" "$OUT/SHA256SUMS"
else
  gh release create "v$V" --target main --title "Tock $V" --notes-file "$OUT/notes.md" \
    "$OUT/Tock.zip" "$OUT/tock-fire.bin" "$OUT/SHA256SUMS"
fi
git fetch -q --tags
