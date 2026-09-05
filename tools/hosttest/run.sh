#!/bin/sh
# Build and run the host-side tests.
#
# These compile the REAL src/*.c on the PC so config logic can be exercised
# without a Switch.  Run from inside devkitPro's MSYS2 shell:
#
#   D:\...\devkitPro\msys2\usr\bin\bash.exe -lc \
#     "cd '<repo>' && sh tools/hosttest/run.sh"
#
# Only SDL2's HEADERS are needed (for key codes); nothing links against SDL, so
# no desktop SDL install is required.
#
# NOTE: the repo path contains a space, so every include is RELATIVE and the
# build runs from the repo root.  An absolute -I would be split by the shell
# and the compiler would look for a directory called "Switch".
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDL_INC=${SDL_INC:-/opt/devkitpro/portlibs/switch/include/SDL2}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

cd "$ROOT"
CFLAGS="-O0 -g -std=gnu11 -I. -Isrc -I$SDL_INC -Wall -Wno-parentheses -Wno-unused"

echo "building host tests..."
gcc $CFLAGS -c src/config.c                  -o "$OUT/config.o"
gcc $CFLAGS -c src/util.c                    -o "$OUT/util.o"
gcc $CFLAGS -c tools/hosttest/stubs.c        -o "$OUT/stubs.o"
gcc $CFLAGS -c tools/hosttest/test_config.c  -o "$OUT/test_config.o"
gcc "$OUT/config.o" "$OUT/util.o" "$OUT/stubs.o" "$OUT/test_config.o" -o "$OUT/test_config"

# Run in a scratch directory: the tests write a zelda3.ini next to themselves
# and must never touch the one in the repo.
mkdir -p "$OUT/run"
cd "$OUT/run"
fails=0
n=0
while [ $n -lt 9 ]; do
  if ! "$OUT/test_config" $n; then fails=$((fails+1)); fi
  n=$((n+1))
done

echo
if [ $fails -eq 0 ]; then
  echo "all host config tests passed"
else
  echo "$fails scenario(s) FAILED"
  exit 1
fi
