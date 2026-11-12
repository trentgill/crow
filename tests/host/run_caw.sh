#!/bin/sh
# Build and run the host-side tests of the USB serial transport
# (lib/caw.c and usbd/usbd_cdc_interface.c). Exits non-zero on failure.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c99 -Wall -Werror -g -O1 -DTELEMETRY \
    -I"$here/stub_usb" \
    "$here/caw_test.c" "$root/usbd/usbd_cdc_interface.c" \
    -o "$tmp/caw_test"
"$tmp/caw_test"
