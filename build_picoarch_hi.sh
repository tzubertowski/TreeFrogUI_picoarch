#!/bin/bash
# Build picoarch_hi: identical to picoarch but linked at 0x20000000.
# Frees low 512MB so lightrec can identity-map PSX RAM at 0x0 (offset_ram=0,
# "perfect map" codegen path — the only working path on MIPS host).
set -e
CROSS=$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-
SYSROOT=$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot
CC=${CROSS}gcc
CFLAGS="-mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls -EL"
CFLAGS="$CFLAGS --sysroot=$SYSROOT"
CFLAGS="$CFLAGS -Wall -fdata-sections -ffunction-sections"
CFLAGS="$CFLAGS -I./ -I../FrogUI/common -I./libretro-common/include/"
CFLAGS="$CFLAGS -I$SYSROOT/usr/include"
CFLAGS="$CFLAGS -I$SYSROOT/usr/bin-o32/../../usr/include/SDL"
CFLAGS="$CFLAGS -D_GNU_SOURCE=1 -D_REENTRANT"
CFLAGS="$CFLAGS -DUSE_C_SCALER -DPLATFORM_SF3000 -Ofast -DNDEBUG"
DEFINES=(
    '-DPICO_HOME_DIR="/.picoarch/"'
    '-DCONTENT_DIR="/mnt/SDCARD/Roms"'
)

# High text base: frees 0x0-0x1fffffff for lightrec identity map.
# -mlong-calls ensures all GOT/PLT refs work at high addr; -Ttext-segment
# sets ELF load address.
LDFLAGS="$CFLAGS -L$SYSROOT/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng16 -lz -Wl,--gc-sections -lpthread -s"
LDFLAGS="$LDFLAGS -Wl,-Ttext-segment=0x20000000"

OBJS="libpicofe/input.o libpicofe/in_sdl.o libpicofe/linux/in_evdev.o libpicofe/linux/plat.o libpicofe/fonts.o libpicofe/readpng.o libpicofe/config_file.o cheat.o config.o content.o core.o menu.o menu_font.o i18n.o main.o options.o overrides.o patch.o scale.o scaler_neon.o unzip.o util.o frogui_settings.o plat_sf3000.o hwdisp.o"

cd $HOME/sf3000-work/picoarch

for src in libpicofe/input.c libpicofe/in_sdl.c libpicofe/linux/in_evdev.c libpicofe/linux/plat.c libpicofe/fonts.c libpicofe/readpng.c libpicofe/config_file.c cheat.c config.c content.c core.c menu.c menu_font.c main.c options.c overrides.c patch.c scale.c scaler_neon.c unzip.c util.c frogui_settings.c; do
    obj="${src%.c}.o"
    $CC $CFLAGS "${DEFINES[@]}" -c -o "$obj" "$src"
done

$CC $CFLAGS "${DEFINES[@]}" -c -o i18n.o ../FrogUI/common/i18n.c

$CC $CFLAGS "${DEFINES[@]}" -c -o plat_sf3000.o plat_sf3000.c
$CC $CFLAGS "${DEFINES[@]}" -c -o hwdisp.o hwdisp.c

$CC $OBJS $LDFLAGS -o picoarch_hi

echo "=== BUILD SUCCESS ==="
ls -la picoarch_hi
file picoarch_hi
