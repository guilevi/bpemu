#!/bin/sh
# windeps.sh --- fetch what `make windows` links against, into deps/windows:
# SDL3's MinGW development package and mpg123's Win64 build (libmpg123
# DLL and headers).  Versions and checksums are pinned.
set -e
cd "$(dirname "$0")/.."
D=deps/windows
SDL=3.4.18
MPG=1.33.7
mkdir -p "$D/dl"

fetch () {			# url file sha256
  [ -f "$D/dl/$2" ] || curl -fsSL -o "$D/dl/$2" "$1"
  echo "$3  $D/dl/$2" | shasum -a 256 -c - >/dev/null || {
    echo "windeps: checksum mismatch for $2" >&2; rm -f "$D/dl/$2"; exit 1; }
}

fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-devel-$SDL-mingw.tar.gz" \
  "SDL3-devel-$SDL-mingw.tar.gz" 049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf
fetch "https://www.mpg123.de/download/win64/$MPG/mpg123-$MPG-x86-64.zip" \
  "mpg123-$MPG-x86-64.zip" 91deafd94ef9254c3c88db656e9ad8db651e22b6d6c06512c2d7f134a80dbfa0

rm -rf "$D/SDL3" "$D/mpg123" "$D/tmp"
mkdir -p "$D/tmp"
tar -xzf "$D/dl/SDL3-devel-$SDL-mingw.tar.gz" -C "$D/tmp"
mv "$D/tmp/SDL3-$SDL/x86_64-w64-mingw32" "$D/SDL3"
unzip -q "$D/dl/mpg123-$MPG-x86-64.zip" -d "$D/tmp"
mv "$D/tmp/mpg123-$MPG-x86-64" "$D/mpg123"
rm -rf "$D/tmp"
echo "windeps: SDL3 $SDL and mpg123 $MPG in $D"
