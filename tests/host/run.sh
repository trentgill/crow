#!/bin/sh
# host tests for crow C code that can run off the hardware.
# run from the repo root: sh tests/host/run.sh
# builds into a temp dir so no host objects land next to the arm ones.
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

LUA_SRC=$(ls submodules/lua/src/*.c | grep -v -e '/lua\.c$' -e '/luac\.c$')
CFLAGS="-std=gnu99 -O1 -Wall -Wno-unused-function -Wno-unused-value -DLUA_USE_POSIX -Itests/host/stub -I. -Ilib"

gcc $CFLAGS -o "$out/watchdog_test" tests/host/watchdog_test.c lib/lua_watchdog.c $LUA_SRC -lm
"$out/watchdog_test"

gcc $CFLAGS -o "$out/clock_test" tests/host/clock_test.c lib/clock.c lib/clock_ll.c lib/events.c -lm
"$out/clock_test"
