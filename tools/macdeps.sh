#!/bin/sh
# macdeps.sh --- fetch what `make mac` links against, into deps/mac: SDL3's
# official macOS framework (universal) and libmpg123 built from source as a
# static universal library, both for macOS $MIN and later, so the release
# does not depend on Homebrew.  Versions and checksums are pinned.
set -e
cd "$(dirname "$0")/.."
D=deps/mac
SDL=3.4.18
MPG=1.33.7
MIN=11.0
mkdir -p "$D/dl"

fetch () {			# url file sha256
  [ -f "$D/dl/$2" ] || curl -fsSL -o "$D/dl/$2" "$1"
  echo "$3  $D/dl/$2" | shasum -a 256 -c - >/dev/null || {
    echo "macdeps: checksum mismatch for $2" >&2; rm -f "$D/dl/$2"; exit 1; }
}

fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.dmg" \
  "SDL3-$SDL.dmg" c1adf3da91108dbc3e4205da5dacebc203085d283fa89e86eb2c889abd03a853
fetch "https://www.mpg123.de/download/mpg123-$MPG.tar.bz2" \
  "mpg123-$MPG.tar.bz2" 31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee

rm -rf "$D/SDL3.framework" "$D/mpg123" "$D/tmp"
mkdir -p "$D/tmp/mnt"
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$D/tmp/mnt" "$D/dl/SDL3-$SDL.dmg"
ditto "$D/tmp/mnt/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework" "$D/SDL3.framework"
hdiutil detach -quiet "$D/tmp/mnt"

# libmpg123 alone, portable C decoder (no per-CPU assembly), so one pass
# builds both architectures.
tar -xjf "$D/dl/mpg123-$MPG.tar.bz2" -C "$D/tmp"
P=$(pwd)/$D/mpg123
( cd "$D/tmp/mpg123-$MPG" &&
  ./configure -q --prefix="$P" --disable-shared --enable-static \
    --disable-components --enable-libmpg123 --with-cpu=generic_fpu \
    CFLAGS="-O2 -arch arm64 -arch x86_64 -mmacosx-version-min=$MIN" &&
  make -s -j8 && make -s install ) >"$D/mpg123-build.log" 2>&1 || {
  tail -20 "$D/mpg123-build.log" >&2; exit 1; }
rm -rf "$D/tmp"
echo "macdeps: SDL3 $SDL and mpg123 $MPG in $D"
