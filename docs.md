# picoarch for SF3000 — Build & Deploy Guide

## Hardware Target
- **CPU:** MIPS32r2 little-endian (LSB)
- **Kernel:** Linux 4.4.x
- **Libc:** glibc, dynamic loader `/lib/ld.so.1`
- **Display:** 720x1280 fb0 (physical 854x480, rotated)
- **SD Mount:** `/mnt/sdcard`

## Prerequisites
- **Docker** (user must be in `docker` group)
- **Toolchain** at `~/sf3000-work/sf3000toolchain/` (mipsel-buildroot-linux-gnu SDK)
- **Docker image:** `bookworm_mips_docker` (Debian bookworm with basic build tools)

## Building picoarch

### Quick build (from picoarch directory):
```bash
cd ~/sf3000-work/picoarch
sg docker -c 'docker run --rm -v "$(pwd)":/work -v ~/sf3000-work/sf3000toolchain:/tc -w /work bookworm_mips_docker bash /work/build_sf3000.sh'
```

### What build_sf3000.sh does:
1. Uses cross-compiler: `/tc/.../opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc`
2. Uses buildroot sysroot for SDL1.2, libpng16, zlib headers and libs
3. Compiles with `platform=sf3000` (adds `plat_sf3000.o`, SF3000-specific flags)
4. Key flags: `-mips32r2 -mtune=24kc -mfp32 -mhard-float -EL` (little-endian)
5. Defines: `-DPLATFORM_SF3000 -DUSE_C_SCALER`
6. Links: `-lSDL -lpng16 -lz -lpthread -ldl`
7. Strips binary (~320KB output)

### Build only picoarch (not cores):
The build script targets `picoarch` only. Cores (gambatte, fceumm, etc.) need separate cross-compilation — they have their own Makefiles and some need `CROSS_COMPILE` set explicitly.

## Deploy to SD card

```bash
# Copy to SD card (adjust mount point as needed)
cp picoarch /run/media/tomaszz/596B-E19B/cubegm/picoarch
sync
```

The binary goes to `cubegm/picoarch` on the SD card. On device this becomes `/mnt/sdcard/cubegm/picoarch`.

## Architecture Notes

### Key source files:
- `plat_sf3000.c` — SF3000 button mapping (cubevol shm), SDL fallback keybinds, includes `plat_sdl.c`
- `plat_sdl.c` — Shared platform code. SF3000-specific sections guarded by `#ifdef PLATFORM_SF3000`:
  - Raw fb0 framebuffer init/blit (bypasses SDL video, uses SDL dummy driver)
  - cubevol shared memory input polling → SDL event injection
  - Display controller wiring via `/dev/dis` ioctl
- `plat.h` — Screen dimensions (320x240 @ 16bpp), audio sample rate
- `main.c` — Entry point, signal handlers, game loop

### Display pipeline:
1. SDL_VIDEODRIVER=dummy (no actual SDL video)
2. Off-screen SDL_Surface for menu rendering
3. Game frames: core → `plat_video_process()` → `sf3000_fb_blit()` → direct fb0 mmap write
4. fb0 is 720x1280 ARGB32, mapped to 854x480 physical display (rotated 90°)
5. Nearest-neighbor scaling with rotation correction in blit

### Input pipeline:
1. `sf3000_keys_init()` attaches to cubevol shared memory (`ftok("/tmp/joy_key", 'a')`)
2. Background thread polls cubevol, converts button bits → SDL key events
3. `sf3000_keymap[]` maps hardware bit positions to RETRO_DEVICE_ID_JOYPAD_*
4. Keymap confirmed and hardcoded (saved to `/mnt/sdcard/sf3000_keymap.txt`)

### Platform rules (from PROJECT_RULES):
- Use SDL1.2 for video/input/audio
- Platform-specific code goes in `plat_sf3000.c` only if SDL can't handle it
- Reuse `plat_sdl.c` code where possible
- Exclude Miyoo Mini hardware scaling with `#ifndef PLATFORM_SF3000`
- Makefile target: `platform=sf3000`

## Toolchain Details
- **Cross-compiler:** `mips-mti-linux-gnu-gcc` (Codescape GNU Tools 2018.09-02, GCC 6.3.0)
- **Sysroot (buildroot):** `.../mipsel-buildroot-linux-gnu/sysroot` — has SDL, libpng16, zlib
- **Sysroot (gcc):** `.../opt/ext-toolchain/sysroot/mips-r2-hard` — does NOT have SDL/libs
- **Critical:** Must pass `--sysroot=<buildroot_sysroot>` and `-L<buildroot_sysroot>/usr/lib` so the linker finds SDL/png/z
- **SDL config:** `<buildroot_sysroot>/usr/bin-o32/sdl-config`

## Known Issues
- `fbdev_init()` from driver.so crashes the device — never call it
- LTO disabled for sf3000 (breaks MIPS ABICALLS GP setup)
- Cores need separate cross-compilation (Makefile `all` target tries to build them with host g++)
- Docker container lacks `file` command (cosmetic only)
