#!/bin/sh
set -e
cd /home/tomaszz/sf3000-work/picoarch

TC="$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-"
SYSROOT="$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot"
SDL_CONFIG="${SYSROOT}/usr/bin-o32/sdl-config"

[ -x "${TC}gcc" ] || { echo "TC not found: ${TC}gcc"; exit 1; }
[ -x "${SDL_CONFIG}" ] || { echo "sdl-config not found: ${SDL_CONFIG}"; exit 1; }

SDL_CFLAGS=$(${SDL_CONFIG} --cflags)
CC_FLAGS="-mips32r2 -march=mips32r2 -mtune=74kc -mdspr2 -mfp32 -mhard-float -mlong-calls -EL"

# Clean only picoarch objects (not cores). Include hwdisp.o
rm -f libpicofe/input.o libpicofe/in_sdl.o libpicofe/linux/in_evdev.o \
      libpicofe/linux/plat.o libpicofe/fonts.o libpicofe/readpng.o \
      libpicofe/config_file.o cheat.o config.o content.o core.o menu.o menu_font.o \
      i18n.o main.o options.o overrides.o patch.o scale.o scaler_neon.o \
      unzip.o util.o plat_sf3000.o hwdisp.o picoarch

make platform=sf3000 \
     CC="${TC}gcc" \
     SYSROOT="${SYSROOT}" \
     CFLAGS="${CC_FLAGS} --sysroot=${SYSROOT} -Wall -fdata-sections -ffunction-sections -I./ -I../FrogUI/common -I./libretro-common/include/ -I${SYSROOT}/usr/include ${SDL_CFLAGS} -DPICO_HOME_DIR='\"/.picoarch/\"' -DCONTENT_DIR='\"/mnt/SDCARD/Roms\"' -DUSE_C_SCALER -DPLATFORM_SF3000 -Ofast -DNDEBUG" \
     LDFLAGS="${CC_FLAGS} --sysroot=${SYSROOT} -L${SYSROOT}/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng16 -lz -Wl,--gc-sections -lpthread -s" \
     picoarch \
     -j4 2>&1 | tail -40

if [ -f picoarch ]; then
    ${TC}strip picoarch
    echo "=== BUILD SUCCESS ==="
    ls -la picoarch
    file picoarch
else
    echo "=== BUILD FAILED ==="
    exit 1
fi
