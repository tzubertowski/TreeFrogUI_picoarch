#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	echo "Usage: $0 CORE_LIBRETRO.SO CONTENT [OUTPUT.LOG]" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLCHAIN_ROOT=${SF3000_TOOLCHAIN:-"$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot"}
CROSS="$TOOLCHAIN_ROOT/opt/ext-toolchain/bin/mips-mti-linux-gnu-"
SYSROOT="$TOOLCHAIN_ROOT/mipsel-buildroot-linux-gnu/sysroot"
QEMU=${QEMU_MIPSEL:-qemu-mipsel-static}
OUT=${3:-"/tmp/picoarch-qemu-memory-$$.log"}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

[ -x "$ROOT/picoarch" ] || { echo "Build picoarch first: ./build_sf3000.sh" >&2; exit 1; }
[ -x "${CROSS}gcc" ] || { echo "Missing compiler: ${CROSS}gcc" >&2; exit 1; }
command -v "$QEMU" >/dev/null || { echo "Missing QEMU: $QEMU" >&2; exit 1; }

"${CROSS}gcc" -shared -fPIC -mips32r2 -mhard-float -EL \
	-o "$TMP/driver.so" "$ROOT/qemu_profile_driver.c"

rm -f "$OUT"
LC_ALL=C LANG=C TF_DEVICE=R36SX PICOARCH_QEMU=1 \
PICOARCH_DRIVER="$TMP/driver.so" PICOARCH_MEMORY_PROFILE="$OUT" \
PICOARCH_PROFILE_FRAMES=${PICOARCH_PROFILE_FRAMES:-60} \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
timeout 30 "$QEMU" -cpu 74Kf -L "$SYSROOT" "$ROOT/picoarch" "$1" "$2"

echo "Memory profile: $OUT"
cat "$OUT"
