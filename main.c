// #include <png.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#define FROGUI_CORE "/mnt/sdcard/cubegm/cores/frogui_libretro.so"
#define PICOARCH_BIN    "/mnt/sdcard/cubegm/picoarch"
#define PICOARCH_HI_BIN "/mnt/sdcard/cubegm/picoarch_hi"

/* gpsp (GBA) and pcsx_rearmed (PS1) dynarecs need the low 512MB of address space
 * free for their fixed memory maps — run them via the high-linked picoarch_hi
 * (text @0x20000000). Everything else uses the normal picoarch. */
static const char *picoarch_for_core(const char *core) {
	if (core && (strstr(core, "gpsp") || strstr(core, "pcsx") || strstr(core, "ps1")))
		if (access(PICOARCH_HI_BIN, F_OK) == 0)   /* vfat has no exec bit → F_OK */
			return PICOARCH_HI_BIN;
	return PICOARCH_BIN;
}
#define LAUNCH_FILE "/tmp/frogui_launch.txt"
#include "core.h"
#include "config.h"
#include "content.h"
#include "frogui_settings.h"
#include "libpicofe/config_file.h"
#include "libpicofe/input.h"
#include "main.h"
#include "menu.h"
#include "overrides.h"
#include "plat.h"
#include "util.h"

#ifdef MMENU
#include <dlfcn.h>
#include <mmenu.h>
#include <SDL/SDL.h>
void* mmenu = NULL;
char save_template_path[MAX_PATH];
#endif

bool should_quit = false;
unsigned current_audio_buffer_size;
char core_name[MAX_PATH];
int config_override = 0;
int g_debug_frame = 0;

#ifdef PLATFORM_SF3000
/* FrogUI owns the nearest/bilinear filter choice (single setting, applies to
 * every game).  Picoarch reads /mnt/sdcard/frogui/settings.txt at startup and
 * overrides scale_filter accordingly.  No in-game menu option to change.
 * The file is parsed through the ONE shared helper (frogui_settings.h) -
 * main.c, plat_sdl.c and menu.c all read FrogUI settings through it, so the
 * parsing cannot drift between consumers. */
#define LAST_GAME_FILE       "/mnt/sdcard/picoarch/last_game.txt"
/* Two independent settings (were one "auto_resume" toggle):
 *  - g_quick_resume: boot behavior — skip FrogUI and jump straight into the
 *    last-played game on power-on. Settings key stays "auto_resume" for
 *    upgrade compat (same behavior as before the split).
 *  - g_autosave_autoload: save-state behavior — auto-load the last auto-save
 *    on ANY game launch (quick-resume boot OR manual pick from FrogUI), and
 *    keep auto-saving on pause/quit. New key, defaults off. */
static int g_quick_resume = 0;
static int g_autosave_autoload = 0;
static int g_brightness = -1;   /* parsed from settings; -1 = absent */
/* Apply the FrogUI brightness setting to /dev/backlight. The display driver
 * resets the backlight to default when it re-inits on each game launch, and
 * only FrogUI (the frontend) re-applied it — so games ran at default brightness.
 * Applied on the GAME path only (FrogUI does its own, after its cubevol restart).
 * Formula must match FrogUI's cube_set_backlight exactly so frontend and game agree. */
static void apply_brightness(int level) {
	if (level < 0)   level = 0;
	if (level > 100) level = 100;
	int out = (level < 43) ? (level + 23) : (level * level / 43 + 23);
	if (out > 255) out = 255;
	int fd = open("/dev/backlight", O_RDWR);
	if (fd < 0) { DBG("DBG apply_brightness: /dev/backlight open fail errno=%d\n", errno); return; }
	if (write(fd, &out, sizeof(int)) != sizeof(int))
		DBG("DBG apply_brightness: write fail errno=%d\n", errno);
	close(fd);
	DBG("DBG apply_brightness: level=%d -> %d\n", level, out);
}
static void load_frogui_settings(void) {
	/* filter: FrogUI's choice is only the DEFAULT: if this game has its own
	 * config (config_override), its in-menu Filter choice wins, so don't
	 * clobber it here. sharp has no FrogUI keyword — per-game only. */
	if (!config_override) {
		if (frogui_setting_is("filter", "bilinear"))     scale_filter = SCALE_FILTER_BILINEAR;
		else if (frogui_setting_is("filter", "nearest")) scale_filter = SCALE_FILTER_NEAREST;
	}
	DBG("DBG load_frogui_settings: filter override=%d → scale_filter=%d\n",
	        config_override, scale_filter);
	g_quick_resume     = frogui_setting_is("auto_resume", "on");
	g_autosave_autoload = frogui_setting_is("autosave_autoload", "on");
	g_brightness       = frogui_setting_int("brightness", -1);   /* applied on game path only */
	DBG("DBG load_frogui_settings: quick_resume=%d autosave_autoload=%d\n",
	        g_quick_resume, g_autosave_autoload);
}
static int read_last_game(char *core, size_t cs, char *rom, size_t rs) {
	FILE *f = fopen(LAST_GAME_FILE, "r");
	if (!f) { DBG("DBG read_last_game: file missing (good)\n"); return 0; }
	core[0] = rom[0] = 0;
	if (fgets(core, cs, f)) core[strcspn(core, "\r\n")] = 0;
	if (fgets(rom,  rs, f)) rom[strcspn(rom, "\r\n")]   = 0;
	fclose(f);
	DBG("DBG read_last_game: core=%s rom=%s\n", core, rom);
	return (core[0] && rom[0]);
}
static void write_last_game(const char *core, const char *rom) {
	FILE *f = fopen(LAST_GAME_FILE, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n", core, rom);
	fflush(f); fsync(fileno(f));
	fclose(f);
	sync();
}
static void clear_last_game(void) {
	int r = unlink(LAST_GAME_FILE);
	DBG("DBG clear_last_game: unlink ret=%d errno=%d\n", r, errno);
	sync();
}
#endif

void dbg_log(const char *fmt, ...) {
	/* Write directly to log.txt (append) so every picoarch process — including
	 * the game process exec'd from FrogUI — is captured, regardless of whether
	 * its stderr is redirected.  The file is created automatically on the
	 * diagnostic builds; a missing marker must not silently discard the evidence
	 * needed for display-mode debugging. */
	static int enabled = -1;
	static FILE *lf = NULL;
	static unsigned log_lines;
	if (enabled == -1) {
		enabled = 1;
		lf = fopen("/mnt/sdcard/log.txt", "a");
	}
	if (!enabled || !lf) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(lf, fmt, ap);
	va_end(ap);
	fflush(lf);
	/* Keep diagnostics durable without forcing an SD sync for every frame. */
	if ((++log_lines & 31u) == 0) fsync(fileno(lf));
}

static void sig_hex(char *b, int *n, unsigned long v) {
	b[(*n)++] = '0'; b[(*n)++] = 'x';
	for (int i = 28; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		b[(*n)++] = d < 10 ? ('0' + d) : ('a' + d - 10);
	}
}

static void sigsegv_handler(int sig, siginfo_t *si, void *ucv) {
	/* async-signal-safe: write() + manual formatting only */
	char buf[160];
	int n = 0;
	buf[n++] = 'S'; buf[n++] = 'I'; buf[n++] = 'G'; buf[n++] = '=';
	if (sig >= 10) buf[n++] = '0' + sig / 10;
	buf[n++] = '0' + sig % 10;
	buf[n++] = ' '; buf[n++] = 'f';
	int f = g_debug_frame;
	if (f >= 1000) buf[n++] = '0' + f / 1000;
	if (f >= 100)  buf[n++] = '0' + (f / 100) % 10;
	if (f >= 10)   buf[n++] = '0' + (f / 10) % 10;
	buf[n++] = '0' + f % 10;
	const char *as = " addr="; for (const char *p = as; *p; p++) buf[n++] = *p;
	sig_hex(buf, &n, si ? (unsigned long)si->si_addr : 0);
	const char *ps = " pc="; for (const char *p = ps; *p; p++) buf[n++] = *p;
	unsigned long pc = 0;
	if (ucv) pc = (unsigned long)((ucontext_t *)ucv)->uc_mcontext.pc;
	sig_hex(buf, &n, pc);
	buf[n++] = '\n';
	write(2, buf, n);
	signal(sig, SIG_DFL);
	raise(sig);
}

static uint32_t vsyncs;
static uint32_t renders;

/* Fast-forward speed level, cycled by SELECT+R1: 0=off, 1=2x, 2=3x, 3=uncapped.
 * 2x/3x: the frame limiter paces emulation at (level+1)*60fps and the
 * SF3000/R36SX present skips to keep display at 60fps. Uncapped: no pacing at
 * all (max the CPU can do), present every 2nd frame (frameskip). See
 * plat_sdl.c. Audio mutes while FF. */
int g_ff_level = 0;

static void toggle_fast_forward(int force_off)
{
	static int enable_audio_was = 1;

	if (force_off) {
		if (g_ff_level) { g_ff_level = 0; enable_audio = enable_audio_was; }
		return;
	}
	if (!ff_enabled) return;   /* per-core: fast-forward off */
	if (g_ff_level == 0) enable_audio_was = enable_audio;  /* snapshot on entry */
	g_ff_level = (g_ff_level + 1) % 4;               /* off → 2x → 3x → uncapped → off */
	enable_audio = g_ff_level ? 0 : enable_audio_was;      /* mute while FF */
}

// static int screenshot_file_name(char *name, size_t len) {
// 	char suffix[MAX_PATH];
//
// 	for (int i = last_screenshot; i < 10000; i++) {
// 		snprintf(suffix, MAX_PATH, "IMG_%04d.png", i);
// 		save_relative_path(name, len, suffix);
//
// 		if (access(name, F_OK) == -1) {
// 			last_screenshot = i;
// 			return 0;
// 		}
// 	}
// 	*name = '\0';
// 	return -1;
// }
//
// static int png_write_rgb565(const uint16_t *buf, png_structp png_ptr, int w, int h) {
// 	png_byte *row_pointer = calloc(w * 3, sizeof(png_byte));
// 	int ret = -1;
//
// 	if (!row_pointer)
// 		return ret;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	for (int i = 0; i < h; i++) {
// 		uint16_t *pbuf = &((uint16_t *)buf)[i * w];
// 		png_byte *prow = row_pointer;
//
// 		for (int j = 0; j < w; j++) {
// 			uint16_t px = *pbuf++;
// 			*prow++ = ((((px & 0xF800) >> 11) * 255 + 15) / 31);
// 			*prow++ = ((((px & 0x07E0) >> 5)  * 255 + 31) / 63);
// 			*prow++ = ((((px & 0x001F))       * 255 + 15) / 31);
// 		}
// 		png_write_row(png_ptr, row_pointer);
// 	}
// 	ret = 0;
//
// finish:
// 	if (row_pointer)
// 		free(row_pointer);
//
// 	return ret;
// }
//
// static int write_png(const uint16_t *buf, int w, int h, FILE *file) {
// 	png_structp png_ptr = NULL;
// 	png_infop info_ptr = NULL;
// 	int ret = -1;
//
// 	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
// 	if (!png_ptr)
// 		goto finish;
//
// 	info_ptr = png_create_info_struct(png_ptr);
// 	if (!info_ptr)
// 		goto finish;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	png_init_io(png_ptr, file);
//
// 	png_set_IHDR(png_ptr, info_ptr, w, h, 8,
// 	             PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
// 	             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
//
// 	png_write_info(png_ptr, info_ptr);
//
// 	if (png_write_rgb565(buf, png_ptr, w, h))
// 		goto finish;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	png_write_end(png_ptr, info_ptr);
// 	ret = 0;
//
// finish:
// 	png_destroy_write_struct(&png_ptr, &info_ptr);
// 	return ret;
// }

int screenshot(void) {
	return 0;
// 	FILE *fp;
// 	char filename[MAX_PATH];
// 	int w, h;
// 	void *buf = plat_prepare_screenshot(&w, &h, NULL);
// 	int ret = -1;
//
// 	if (screenshot_file_name(filename, MAX_PATH)) {
// 		PA_ERROR("No available filename for screenshot\n");
// 		return -1;
// 	}
//
// 	fp = fopen(filename, "wb");
// 	if (!fp)
// 		goto finish;
//
// 	if (write_png(buf, w, h, fp))
// 		goto finish;
//
// 	PA_INFO("Wrote screenshot to %s\n", filename);
// 	ret = 0;
//
// finish:
// 	if (fp)
// 		fclose(fp);
// 	return ret;
}

/* OSD overlay (battery, volume) is drawn by cubevol on /dev/fb1 (ARGB plane).
 * Hide: mmap fb1, memset zeros (cubevol has no signal handler so it won't
 *       redraw on its own — clear once is enough until next restart).
 * Show: restart cubevol via killall + spawn; the new instance draws battery
 *       and volume on init. cubevol shmem (/tmp/joy_key) is kernel-owned so
 *       readers (game cores) survive the restart. */
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdlib.h>
int g_is_frogui = 0;
static void fb1_clear(void) {
	/* Persistent mmap (open once): the old per-call open+mmap+munmap+close ran
	 * during gameplay (every 30 frames) and churned syscalls + a full-plane
	 * mmap each time. Map once, then just memset. */
	static void *mem = NULL; static size_t len = 0; static int inited = 0;
	if (!inited) {
		inited = 1;
		int fd = open("/dev/fb1", O_RDWR);
		if (fd < 0) return;
		struct fb_fix_screeninfo finfo;
		if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0) {
			void *m = mmap(NULL, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
			if (m != MAP_FAILED) { mem = m; len = finfo.smem_len; }
		}
		close(fd);   /* mmap survives close */
	}
	if (mem) memset(mem, 0, len);
}
/* Only FrogUI restores OSD (via its own retro_init restart). picoarch's
 * menu and game-resume keep fb1 cleared. */
static void fb1_blank(int blank) {
	if (!blank) return;
	if (getenv("PICOARCH_AUTO_RESUME")) {
		/* Quick-resume boot: cubevol was just spawned (backgrounded, no
		 * sync) and may not have drawn its first OSD frame yet — a single
		 * clear here can land BEFORE that first draw and get overwritten.
		 * Retry across cubevol's typical startup window; it never redraws
		 * on its own, so once we win the race we stay hidden. */
		unsetenv("PICOARCH_AUTO_RESUME");
		for (int i = 0; i < 5; i++) {
			fb1_clear();
			if (i < 4) usleep(100000);
		}
		return;
	}
	fb1_clear();
}
static void fb1_menu_enter(void) {
	if (g_is_frogui) return;
	/* Filter is locked per-process (set from FrogUI settings at startup);
	 * no need to track or react to filter changes mid-game. */
}
static void fb1_menu_exit(void) {
	if (g_is_frogui) return;
}

void set_defaults(void)
{
	show_fps = 0;
	show_cpu = 0;
	show_hud = 1;
	limit_frames = 1;
	enable_audio = 1;
	ff_enabled = 0;       /* off by default; enable per-core in the menu */
	rewind_enabled = 0;   /* off by default; reserves RAM + slows frames when on */
	audio_mono = 1;       /* single-speaker SF3000/R36SX default; stereo is optional */
	audio_buffer_size = 5;
	scale_size = SCALE_SIZE_ASPECT;  /* default: aspect — integer & full are slower on the HW scaler */
	aspect_ratio_mode = 1;           /* Native (merged Aspect-ratio control default) */
	scale_filter = SCALE_FILTER_NEAREST;
	scale_effect = default_scale_effect;
	optimize_text = 1;
	// max_upscale = 8;
	
	scale_update_scaler();

	if (current_audio_buffer_size < audio_buffer_size)
		current_audio_buffer_size = audio_buffer_size;

	for (size_t i = 0; i < core_options.len; i++) {
		const char *key = options_get_key(i);
		if (key)
			core_options.entries[i].value = core_options.entries[i].default_value;
	}

	options_update_changed();
}

int save_config(int is_game)
{
	char config_filename[MAX_PATH];
	FILE *config_file;

	config_file_name(config_filename, MAX_PATH, is_game);
	DBG("DBG save_config(is_game=%d) → %s\n", is_game, config_filename);
	config_file = fopen(config_filename, "wb");
	if (!config_file) {
		fprintf(stderr, "Could not write config to %s (errno=%d)\n", config_filename, errno);
		return -1;
	}

	config_write(config_file);
	config_write_keys(config_file);

	fflush(config_file);
	fsync(fileno(config_file));
	fclose(config_file);
	DBG("DBG save_config: fclose done\n");

	if (is_game)
		config_override = 1;

	return 0;
}

static void alloc_config_buffer(char **config_ptr) {
	char config_filename[MAX_PATH];
	FILE *config_file;
	size_t length;
	config_override = 0;

	config_file_name(config_filename, MAX_PATH, 1);
	DBG("DBG alloc_config_buffer: try game-cfg=%s\n", config_filename);
	config_file = fopen(config_filename, "rb");
	if (config_file) {
		config_override = 1;
		DBG("DBG alloc_config_buffer: game-cfg HIT\n");
	} else {
		config_file_name(config_filename, MAX_PATH, 0);
		DBG("DBG alloc_config_buffer: try global-cfg=%s\n", config_filename);
		config_file = fopen(config_filename, "rb");
		DBG("DBG alloc_config_buffer: global-cfg %s\n", config_file ? "HIT" : "MISS");
	}

	if (!config_file)
		return;

	PA_INFO("Loading config from %s\n", config_filename);

	fseek(config_file, 0, SEEK_END);
	length = ftell(config_file);
	fseek(config_file, 0, SEEK_SET);

	*config_ptr = calloc(1, length);

	fread(*config_ptr, 1, length, config_file);
	fclose(config_file);
}

void load_config(void)
{
	char *config = NULL;
	alloc_config_buffer(&config);

	if (config) {
		config_read(config);
		free(config);
	}
	plat_reinit();
}

void load_config_keys(void)
{
	char *config = NULL;
	int kcount = 0;
	const int *defbinds = NULL;

	alloc_config_buffer(&config);

	if (config) {
		config_read_keys(config);
		free(config);

		/* Force input device 0 menu to be bound to the default key */
		in_get_config(0, IN_CFG_BIND_COUNT, &kcount);
		defbinds = in_get_dev_def_binds(0);

		for(int i = 0; i < kcount; i++) {
			if (defbinds[IN_BIND_OFFS(i, IN_BINDTYPE_EMU)] == 1 << EACTION_MENU) {
				in_bind_key(0, i, 1 << EACTION_MENU, IN_BINDTYPE_EMU, 0);
			}
		}
	}
}

int remove_config(int is_game) {
	char config_filename[MAX_PATH];
	int ret;

	config_file_name(config_filename, MAX_PATH, is_game);
	ret = remove(config_filename);
	if (ret == 0)
		config_override = 0;

	return ret;
}

static void rewind_apply(void);   /* defined below; applies rewind toggle live */

/* ---- Local play-time stats (no clock/RTC needed; monotonic elapsed). ----
 * Accumulate seconds per game into /mnt/sdcard/frogui/playtime.txt as
 * "<seconds>\t<content path>" lines; FrogUI reads it to show play time. */
#define PLAYTIME_FILE "/mnt/sdcard/frogui/playtime.txt"
#define PLAYSESSION_FILE "/mnt/sdcard/frogui/play_sessions.txt"
static long playtime_mono(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec;
}
static void playtime_add(const char *path, long add) {
	if (add <= 0 || !path || !*path) return;
	char (*paths)[MAX_PATH] = NULL; long *secs = NULL; int n = 0, cap = 0, found = 0;
	FILE *f = fopen(PLAYTIME_FILE, "r");
	if (f) {
		char line[MAX_PATH + 32];
		while (fgets(line, sizeof line, f)) {
			char *tab = strchr(line, '\t'); if (!tab) continue;
			*tab = 0; char *p = tab + 1;
			size_t pl = strcspn(p, "\r\n"); p[pl] = 0;
			if (n >= cap) { cap = cap ? cap*2 : 64;
				paths = realloc(paths, cap*sizeof(*paths)); secs = realloc(secs, cap*sizeof(*secs)); }
			snprintf(paths[n], MAX_PATH, "%s", p); secs[n] = atol(line); n++;
		}
		fclose(f);
	}
	for (int i = 0; i < n; i++) if (!strcmp(paths[i], path)) { secs[i] += add; found = 1; break; }
	f = fopen(PLAYTIME_FILE, "w");
	if (f) {
		for (int i = 0; i < n; i++) fprintf(f, "%ld\t%s\n", secs[i], paths[i]);
		if (!found) fprintf(f, "%ld\t%s\n", add, path);
		fflush(f); fsync(fileno(f)); fclose(f);
	}
	free(paths); free(secs);
}

/* Keep an append-only session ledger as well as the compact all-time totals.
 * Older cards only have playtime.txt; FrogUI falls back to that file. */
static void play_session_add(const char *path, long seconds) {
	if (!path || !*path || seconds <= 0) return;
	FILE *f = fopen(PLAYSESSION_FILE, "a");
	if (!f) return;
	time_t now = time(NULL);
	fprintf(f, "%ld\t%ld\t%s\n", (long)now, seconds, path);
	fflush(f); fsync(fileno(f)); fclose(f);
}

void handle_emu_action(emu_action action)
{
	static emu_action prev_action = EACTION_NONE;
	if (prev_action != EACTION_NONE && prev_action == action) return;

	switch (action)
	{
	case EACTION_NONE:
		break;
	case EACTION_MENU:
	{
		plat_sound_pause_for_menu();
#ifdef PLATFORM_SF3000
		extern int sf3000_use_hwdisp;
		DBG("DBG EACTION_MENU: use_hwdisp=%d filter=%d\n",
		        sf3000_use_hwdisp, scale_filter);
#endif
		toggle_fast_forward(1); /* Force FF off */
		sram_write();
		DBG("DBG EACTION_MENU: sram_write done\n");
#ifdef PLATFORM_SF3000
		/* Auto-save snapshot on menu open — only safe point to save
		 * mid-game (game is paused while menu is up). */
		if (g_autosave_autoload && !g_is_frogui) {
			int prev = state_slot;
			state_slot = 9;  /* reserved auto-save/auto-load slot (10th) */
			pa_state_write();
			state_slot = prev;
			sync();
			DBG("DBG autosave_autoload: state saved to slot 99 (menu open)\n");
		}
#endif
	}
#ifdef MMENU
		if (mmenu && content && content->path) {
			ShowMenu_t ShowMenu = (ShowMenu_t)dlsym(mmenu, "ShowMenu");
			MenuReturnStatus status = ShowMenu(content->path, state_allowed() ? save_template_path : NULL, plat_clean_screen());
			char disc_path[256];
			ChangeDisc_t ChangeDisc = (ChangeDisc_t)dlsym(mmenu, "ChangeDisc");

			if (status == kStatusExitGame) {
				should_quit = 1;
				plat_video_menu_leave();
			} else if (status == kStatusChangeDisc && ChangeDisc(disc_path)) {
				disc_replace_index(0, disc_path);
			} else if (status == kStatusOpenMenu) {
				plat_video_flip();
				fb1_menu_enter();
				menu_loop();
				fb1_menu_exit();
			} else if (status >= kStatusLoadSlot) {
				state_slot = status - kStatusLoadSlot;
				pa_state_read();
			} else if (status >= kStatusSaveSlot) {
				state_slot = status - kStatusSaveSlot;
				pa_state_write();
			}

			// release that menu key
			SDL_Event sdlevent;
			sdlevent.type = SDL_KEYUP;
			sdlevent.key.keysym.sym = SDLK_ESCAPE;
			SDL_PushEvent(&sdlevent);
		}
		else {
#endif
			fb1_menu_enter();
			DBG("DBG menu_loop: ENTER\n");
			menu_loop();
			DBG("DBG menu_loop: EXIT\n");
			fb1_menu_exit();
#ifdef MMENU
		}
#endif
		rewind_apply();   /* apply rewind on/off toggled in the menu, no restart */
		plat_sound_resume_from_menu();
		break;
	case EACTION_TOGGLE_HUD:
		show_hud = !show_hud;
		/* Force the hud to clear */
		plat_video_set_msg(NULL, 0, 0);
		break;
	case EACTION_SAVE_STATE:
		pa_state_write();
		break;
	case EACTION_LOAD_STATE:
		pa_state_read();
		break;
	case EACTION_TOGGLE_FF:
		toggle_fast_forward(0);
		break;
	case EACTION_SCREENSHOT:
		screenshot();
		break;
	case EACTION_QUIT:
		should_quit = 1;
		break;
	default:
		break;
	}

	prev_action = action;
}

void pa_log(enum retro_log_level level, const char *fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch(level) {
#ifdef DEBUG
	case RETRO_LOG_DEBUG:
		printf("DEBUG: %s", buf);
		break;
#endif
	case RETRO_LOG_INFO:
		printf("INFO: %s", buf);
		break;
	case RETRO_LOG_WARN:
		fprintf(stderr, "WARN: %s", buf);
		break;
	case RETRO_LOG_ERROR:
		fprintf(stderr, "ERROR: %s", buf);
		break;
	default:
		break;
	}
}

static void show_startup_message(void) {
	const struct core_override *override = get_overrides();
	if (override && override->startup_msg) {
		plat_video_set_msg(override->startup_msg->msg, 2, override->startup_msg->msec);
	}
}

void pa_track_render(void) {
	renders++;
}

#define CPU_MSG_LEN 10
static void count_fps(void)
{
	char msg[HUD_LEN];
	static unsigned int nextsec = 0;
	static unsigned last_cpu_ticks = 0;
	unsigned int ticks = 0;

	if (show_hud && (show_fps || show_cpu)) {
		ticks = plat_get_ticks_ms();
		if (ticks > nextsec) {
			float last_time = (ticks - nextsec + 1000) / 1000.0f;

			if (last_time > 0) {
				char cpu_msg[CPU_MSG_LEN];
				char fps_msg[HUD_LEN - CPU_MSG_LEN];

				cpu_msg[0] = fps_msg[0] = '\0';
				nextsec = ticks + 1000;

				if (show_fps) {
					float vsyncsps = vsyncs / last_time;
					float rendersps = renders / last_time;
					vsyncs = 0;
					renders = 0;
					snprintf(fps_msg, sizeof(fps_msg), "FPS: %.1f (%.0f)", rendersps, vsyncsps);
				}

				if (show_cpu) {
					unsigned cpu_ticks = plat_cpu_ticks();
					if (cpu_ticks && last_cpu_ticks) {
						float cpu_percent = (cpu_ticks - last_cpu_ticks) / last_time;
						snprintf(cpu_msg, sizeof(cpu_msg), "%.1f%%", cpu_percent);
					}
					last_cpu_ticks = cpu_ticks;
				}

				snprintf(msg, HUD_LEN, "%-*s%*s",
				         (HUD_LEN - CPU_MSG_LEN - 1), fps_msg,
				         CPU_MSG_LEN - 1, cpu_msg);
				plat_video_set_msg(msg, 1, 1100);
			}
		}
		vsyncs++;

	}
}

static void adjust_audio(void) {
	static unsigned prev_audio_buffer_size = 0;

	if (!prev_audio_buffer_size)
		prev_audio_buffer_size = current_audio_buffer_size;

	current_audio_buffer_size = MAX(audio_buffer_size, audio_buffer_size_override);

	if (prev_audio_buffer_size != current_audio_buffer_size) {
		PA_INFO("Resizing audio buffer from %d to %d frames\n",
			prev_audio_buffer_size,
			current_audio_buffer_size);

		plat_sound_resize_buffer();
		prev_audio_buffer_size = current_audio_buffer_size;
	}

	if (current_core.retro_audio_buffer_status) {
		float occupancy = 1.0 - plat_sound_capacity();
		current_core.retro_audio_buffer_status(true, (int)(occupancy * 100), occupancy < 0.50);
	}
}

static void get_tag_name(const char* in_path, char* out_tag) {
	char tmp[MAX_PATH];
	char *slash, *paren;

	strncpy(tmp, in_path, MAX_PATH - 1);
	tmp[MAX_PATH - 1] = '\0';

	/* strip filename: remove everything after last '/' */
	slash = strrchr(tmp, '/');
	if (slash) *slash = '\0';

	/* parent dir name is the tag (e.g. "gb" from .../roms/gb/game.gbc) */
	slash = strrchr(tmp, '/');
	strncpy(out_tag, slash ? slash + 1 : tmp, MAX_PATH - 1);
	out_tag[MAX_PATH - 1] = '\0';

	/* extract pak name from parentheses if present: "PS1 (PCSX)" → "PCSX" */
	paren = strchr(out_tag, '(');
	if (paren) {
		char *close = strchr(paren + 1, ')');
		if (close) {
			*close = '\0';
			memmove(out_tag, paren + 1, strlen(paren + 1) + 1);
		}
	}
}

#ifdef PLATFORM_SF3000
/* ---- Rewind: per-frame serialized-state ring. Hold SELECT+B to rewind. ----
 * Each normal frame we serialize the core state into a ring. While rewinding we
 * drop the newest slot and unserialize the now-newest, then retro_run() renders
 * it (its +1 advance is discarded by the next step) → motion goes backward.
 * Capped by a RAM budget; disabled for cores whose state is too big. */
#define REWIND_BUDGET (16 * 1024 * 1024)
#define REWIND_INTERVAL 6              /* capture every 6 frames (~10Hz) */
static unsigned char *g_rw_buf  = NULL;
static size_t         g_rw_ssize = 0;
static int            g_rw_cap = 0, g_rw_head = 0, g_rw_count = 0;

static void rewind_init(void) {
	if (g_is_frogui || !rewind_enabled || !current_core.retro_serialize_size) return;
	/* picodrive's retro_serialize writes more than retro_serialize_size() reports,
	 * overflowing the rewind ring → heap corruption (hang/black after a few
	 * frames). Disable rewind for it until that's addressed upstream. */
	if (core_name[0] && strstr(core_name, "picodrive")) {
		DBG("DBG rewind: disabled for picodrive (serialize size unreliable)\n");
		return;
	}
	size_t s = current_core.retro_serialize_size();
	if (s == 0 || s > REWIND_BUDGET) return;
	int cap = (int)(REWIND_BUDGET / s);
	if (cap < 2) return;
	if (cap > 1200) cap = 1200;          /* ~20s @60fps */
	g_rw_buf = malloc((size_t)cap * s);
	if (!g_rw_buf) return;
	g_rw_ssize = s; g_rw_cap = cap; g_rw_head = 0; g_rw_count = 0;
	DBG("DBG rewind: state=%zu cap=%d (%.1fs)\n", s, cap, cap / 60.0);
}

/* Apply the rewind toggle live (called when the menu closes) so enabling/disabling
 * it takes effect without restarting picoarch. */
static void rewind_apply(void) {
	if (rewind_enabled && !g_rw_buf) {
		rewind_init();
	} else if (!rewind_enabled && g_rw_buf) {
		free(g_rw_buf); g_rw_buf = NULL;
		g_rw_cap = g_rw_count = g_rw_head = 0; g_rw_ssize = 0;
	}
}

static void rewind_capture(void) {
	if (!g_rw_buf) return;
	if (current_core.retro_serialize(g_rw_buf + (size_t)g_rw_head * g_rw_ssize, g_rw_ssize)) {
		g_rw_head = (g_rw_head + 1) % g_rw_cap;
		if (g_rw_count < g_rw_cap) g_rw_count++;
	}
}

static int rewind_back(void) {
	if (!g_rw_buf || g_rw_count <= 1) return 0;
	g_rw_head = (g_rw_head - 1 + g_rw_cap) % g_rw_cap;   /* drop newest */
	g_rw_count--;
	int slot = (g_rw_head - 1 + g_rw_cap) % g_rw_cap;    /* now-newest */
	current_core.retro_unserialize(g_rw_buf + (size_t)slot * g_rw_ssize, g_rw_ssize);
	return 1;
}

static int rewind_held(void) {
	extern volatile uint32_t *sf3000_keys_ptr;
	extern volatile uint32_t sf3000_keys_filtered;
	if (!sf3000_keys_ptr) return 0;
	uint32_t r = sf3000_keys_filtered;  /* race-filtered */
	return (r & ((1u << 0) | (1u << 14))) == ((1u << 0) | (1u << 14)); /* SELECT+B */
}
#endif

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	/* The device launches us with a very low open-file limit (~16). Steady state
	 * is ~14 fds, so saving a state (state file + screenshot BMP) tips it over →
	 * "Too many open files" → freeze. Raise it. Try hard+soft to 8192 (works as
	 * root); fall back to raising soft up to the hard cap. */
	{
		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
			struct rlimit want = { 8192, 8192 };
			if (setrlimit(RLIMIT_NOFILE, &want) != 0) {
				rl.rlim_cur = rl.rlim_max;   /* soft up to hard */
				setrlimit(RLIMIT_NOFILE, &rl);
			}
		}
	}
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = sigsegv_handler;
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS,  &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGILL,  &sa, NULL);
		sigaction(SIGFPE,  &sa, NULL);
	}
#ifdef PLATFORM_SF3000
	dbg_log("DBG picoarch start: text~%p (hi if 0x2x) argv1=%s next-bin=%s\n",
	        (void *)&picoarch_for_core, argc > 1 ? argv[1] : "?",
	        argc > 1 ? picoarch_for_core(argv[1]) : "?");
	fprintf(stderr, "TFDBG start pid=%d argv1=%s next-bin=%s\n", getpid(),
	        argc > 1 ? argv[1] : "?", argc > 1 ? picoarch_for_core(argv[1]) : "?");
	/* QUICK RESUME: if FrogUI launch and a last-game marker exists with
	 * quick-resume (settings key "auto_resume") on, redirect to that game.
	 * State restore itself only happens if autosave_autoload is also on
	 * (handled uniformly below for both this redirect and manual launches).
	 * Marker cleared on clean exit to FrogUI (Quit). */
	if (argc > 1 && strcmp(argv[1], FROGUI_CORE) == 0) {
		load_frogui_settings();
		if (g_quick_resume) {
			char lg_core[512], lg_rom[512];
			if (read_last_game(lg_core, sizeof(lg_core), lg_rom, sizeof(lg_rom))) {
				/* ANTI-SOFT-BRICK: a stale/invalid marker (missing rom) or a
				 * game that dies instantly would relaunch forever: black
				 * screen until the user pulls the card and edits
				 * last_game.txt on a PC. Validate the target and cap resume
				 * attempts per boot (/tmp resets on power cycle; the counter
				 * is cleared once content actually loads). */
				int tries = 0;
				FILE *tf = fopen("/tmp/resume_tries", "r");
				if (tf) { if (fscanf(tf, "%d", &tries) != 1) tries = 0; fclose(tf); }
				if (access(lg_rom, F_OK) != 0 || access(lg_core, F_OK) != 0) {
					DBG("DBG main: quick-resume target missing (%s) → clearing marker\n", lg_rom);
					clear_last_game();
				} else if (tries >= 2) {
					DBG("DBG main: quick-resume failed %d times this boot → clearing marker\n", tries);
					clear_last_game();
				} else {
					tf = fopen("/tmp/resume_tries", "w");
					if (tf) { fprintf(tf, "%d", tries + 1); fclose(tf); }
					DBG("DBG main: quick-resume redirect → %s + %s (try %d)\n",
					        lg_core, lg_rom, tries + 1);
					setenv("PICOARCH_AUTO_RESUME", "1", 1);
					execl(picoarch_for_core(lg_core), "picoarch", lg_core, lg_rom, NULL);
					/* fall through on execl failure */
				}
			}
		}
	}
#endif
	char content_path[MAX_PATH];
	char tag_name[MAX_PATH];

	if (argc > 1) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			printf("Usage: picoarch libretro_core content [NONE DMG LCD SCANLINE]\n");
			return 0;
		}
	}

	/* FrogUI is PicoArch's launcher core, not a game.  Establish this before
	 * plat_init(): the SF3000 audio driver powers the DAC independently of
	 * software volume, so starting it for the launcher leaves audible analogue
	 * noise even when FrogUI's volume setting is zero. */
	g_is_frogui = argc > 1 && argv[1] && strcmp(argv[1], FROGUI_CORE) == 0;

	if (plat_init()) {
		quit(-1);
	}

	if (menu_init()) {
		quit(-1);
	}

	if (argc > 1 && argv[1]) {
		strncpy(core_path, argv[1], sizeof(core_path) - 1);
	} else {
		quit(-1);
	}

	if (argc > 2 && argv[2]) {
		strncpy(content_path, argv[2], sizeof(content_path) - 1);
	} else {
		quit(-1);
	}
	
	if (argc > 3 && argv[3]) {
		if (!strcmp(argv[3],"NONE")) default_scale_effect = SCALE_EFFECT_NONE;
		else if (!strcmp(argv[3],"DMG")) default_scale_effect = SCALE_EFFECT_DMG;
		else if (!strcmp(argv[3],"LCD")) default_scale_effect = SCALE_EFFECT_LCD;
		else default_scale_effect = SCALE_EFFECT_SCANLINE;
	}
	
	get_tag_name(content_path, tag_name);
	core_extract_name(core_path, core_name, sizeof(core_name));

	if (core_open(core_path, tag_name)) {
		quit(-1);
	}

	content = content_init(content_path);
	if (!content) {
		PA_ERROR("Couldn't allocate memory for content path\n");
		quit(-1);
	}

	set_defaults();
	load_config();
	DBG("DBG main: post-load_config scale_filter=%d scale_size=%d override=%d\n",
	        scale_filter, scale_size, config_override);
#ifdef PLATFORM_SF3000
	/* Filter + quick-resume/autosave-autoload from FrogUI settings (games
	 * only; FrogUI stays SW). */
	if (strcmp(core_path, FROGUI_CORE) != 0) {
		load_frogui_settings();
		/* Re-apply brightness for games: plat_init's driver re-init reset the
		 * backlight to default, and only FrogUI restores its own. */
		if (g_brightness >= 0) apply_brightness(g_brightness);
		/* Record current game as "last game" so quick-resume can jump
		 * straight into it on next boot — independent of autosave/autoload. */
		if (g_quick_resume) {
			write_last_game(core_path, content_path);
		}
		/* Always try slot 99 auto state on ANY game launch (quick-resume
		 * boot redirect OR manual FrogUI pick) when autosave_autoload is on.
		 * state_resume silently falls through if slot 99 file doesn't exist. */
		if (g_autosave_autoload) {
			resume_slot = 9;  /* reserved auto-save/auto-load slot (10th) */
			DBG("DBG main: autosave_autoload → resume_slot=99\n");
		}
	}
	DBG("DBG main: post-FrogUI-override scale_filter=%d quick_resume=%d autosave_autoload=%d\n",
	        scale_filter, g_quick_resume, g_autosave_autoload);
#endif
	dbg_log("DBG M4: pre core_load\n");
	core_load();
	dbg_log("DBG M5: core_load done\n");

	if (core_load_content(content)) {
		quit(-1);
	}
	dbg_log("DBG M6: content loaded, entering run loop\n");
	unlink("/tmp/resume_tries");   /* content runs: reset the quick-resume failure cap */

	/* Hide cubevol's battery/volume OSD (/dev/fb1) during gameplay. Skipped
	 * for FrogUI (the menu core) so the menu still shows battery. */
	if (!g_is_frogui) fb1_blank(1);

	load_config_keys();

#ifdef MMENU

	mmenu = dlopen("libmmenu.so", RTLD_LAZY);
	if (mmenu) {
		ResumeSlot_t ResumeSlot = (ResumeSlot_t)dlsym(mmenu, "ResumeSlot");
		if (ResumeSlot) resume_slot = ResumeSlot();
	}
#endif
	show_startup_message();
	state_resume();
#ifdef PLATFORM_SF3000
	rewind_init();
#endif

	long play_start = playtime_mono();   /* for local play-time stats */

	do {
		count_fps();
		adjust_audio();
		core_frame_time_tick();
#ifdef PLATFORM_SF3000
		if (g_rw_buf && rewind_held() && rewind_back()) {
			current_core.retro_run();          /* render restored frame */
		} else {
			static int rw_fc = 0;
			#ifdef PLATFORM_SF3000
			static unsigned sf_run_n;
			if (sf_run_n < 24 || (sf_run_n && (sf_run_n % 600) == 0))
				DBG("DBG retro_run begin pid=%d n=%u core=%s quit=%d\n", getpid(), sf_run_n + 1, core_name, should_quit);
			++sf_run_n;
			#endif
			current_core.retro_run();
			/* capture every REWIND_INTERVAL frames — per-frame serialize is too
			 * heavy (chops audio) and drains the ring too fast. */
			if (g_rw_buf && (++rw_fc % REWIND_INTERVAL) == 0)
				rewind_capture();
		}
#else
		current_core.retro_run();
#endif
		if (!should_quit)
			plat_video_flip();
#ifdef PLATFORM_SF3000
		/* Pace once per EMULATED frame (not per present) — frameskipping cores
		 * (mame2000) otherwise run their skipped frames unpaced -> too fast. */
		if (!g_is_frogui) {
			extern void sf3000_frame_limit(void);
			sf3000_frame_limit();
		}
#endif
		if (!g_is_frogui)
			sram_autosave();   /* flush in-game saves without waiting for a clean exit */
#ifdef PLATFORM_SF3000
		/* cubevol repaints its fb1 OSD only on a charge-% tick or volume press
		 * (rare), so re-clearing ~once a minute is plenty to keep it hidden with
		 * effectively zero in-game cost. */
		if (!g_is_frogui) {
			static unsigned fb1_fc = 0;
			if ((++fb1_fc % 3600) == 0) fb1_clear();
		}
#endif
	} while (!should_quit);

	if (!g_is_frogui) {  /* don't count time spent in the menu */
		long played = playtime_mono() - play_start;
		playtime_add(content_path, played);
		play_session_add(content_path, played);
	}
	/* Screenshot for recents/switcher art is updated on menu-enter (one route),
	 * not here: at exit the framebuffer is already torn down (black). */

	return quit(0);
}

/* moved to top of file */

int quit(int code) {
	menu_finish();
#ifdef PLATFORM_SF3000
	/* Final autosave before unloading core. */
	if (g_autosave_autoload && !g_is_frogui && current_core.retro_unload_game) {
		int prev = state_slot;
		state_slot = 9;  /* reserved auto-save/auto-load slot (10th) */
		pa_state_write();
		state_slot = prev;
		sync();
		DBG("DBG quit: autosave_autoload final save to slot 99\n");
	}
#endif
	core_unload();
	/* FrogUI also runs with cubevol's fb1 overlay visible for its battery
	 * indicator.  On shutdown that layer must be cleared completely; clearing
	 * only fb0 leaves the old corner glyphs over the shutdown logo. */
	fb1_clear();
	fb1_blank(0);   /* restore OSD overlay; next process re-blanks if needed */

#ifdef PLATFORM_SF3000
	/* exec() BEFORE plat_finish() — keeps fb0/dis fds open across exec.
	 * But first tear down HW display path: if hwdisp was active the
	 * display controller is pointed at driver.so's buffer, not fb0.
	 * video_driver_deinit restores fb0 so the next process can use it. */
	extern int sf3000_use_hwdisp;
	extern int  hwdisp_init(void);
	extern void hwdisp_deinit(void);

	/* Read launch.txt first to know the next process. */
	int next_is_standalone = 0;
	int next_is_video_player = 0;
	int next_is_image_viewer = 0;
	char core_path[512] = "", rom_path[512] = "", standalone_rom[512] = "";
	FILE *lf = fopen(LAUNCH_FILE, "r");
	if (lf) {
		if (fgets(core_path,      sizeof(core_path),      lf)) core_path[strcspn(core_path,           "\n")] = 0;
		if (fgets(rom_path,       sizeof(rom_path),       lf)) rom_path[strcspn(rom_path,             "\n")] = 0;
		if (fgets(standalone_rom, sizeof(standalone_rom), lf)) standalone_rom[strcspn(standalone_rom, "\n")] = 0;
		fclose(lf);
		unlink(LAUNCH_FILE);
		next_is_standalone = (strcmp(core_path, "standalone") == 0 && rom_path[0]);
		next_is_video_player = next_is_standalone &&
			strstr(rom_path, "/video_player") != NULL;
		next_is_image_viewer = next_is_standalone &&
			strstr(rom_path, "/image_viewer") != NULL;
	}

	DBG("DBG quit: use_hwdisp=%d next_standalone=%d core_path[0]=%d rom_path[0]=%d\n",
	        sf3000_use_hwdisp, next_is_standalone, !!core_path[0], !!rom_path[0]);
	/* Clean exit to FrogUI: clear last-game marker so next boot lands on
	 * FrogUI instead of auto-resuming.  Going to another game/standalone
	 * doesn't clear — that new launch will overwrite the marker. */
	if (!core_path[0] && !rom_path[0]) {
		clear_last_game();
		DBG("DBG quit: cleared last_game marker (going to FrogUI)\n");
	}
	/* WARM-BOOT marker: if we leave HCGE active, the next picoarch process
	 * (same PPID = same icube shell child) early-inits hwdisp in
	 * sf3000_fb_init so FrogUI doesn't try to SW-render onto a fb0 still
	 * routed through driver.so (would squish). */
	if (sf3000_use_hwdisp || next_is_standalone) {
		FILE *m = fopen("/tmp/picoarch_hcge_was_active", "w");
		if (m) { fprintf(m, "%d", (int)getppid()); fclose(m); }
	}
	/* Most legacy standalone apps expect the warm HCGE state. libffplayer is
	 * different: it owns the hardware media plane itself, and blocks forever
	 * if picoarch leaves driver.so/HCGE active across exec. Release HCGE for
	 * our native video and image viewers; preserve the PS1/DOS/Rockbox path. */
	if (!next_is_standalone || next_is_video_player || next_is_image_viewer) {
		hwdisp_deinit();
		DBG("DBG quit: hwdisp_deinit done\n");
	}
	/* libffplayer also owns AUDDEC/I2SO. Release only picoarch's proprietary
	 * audio engine here. A full plat_finish() closes SDL/fb0 and prevents the
	 * firmware player loader from reaching main(), while leaving audio active
	 * makes audio-master video wait forever. The amp line must stay OPEN:
	 * libffplayer re-inits the whole digital audio path but never touches the
	 * physical mute pad - a closed line here meant a silent video (the line
	 * now lives in the caller, not in finish_for_exec). */
	if (next_is_video_player || next_is_image_viewer) {
		extern void sf3000_sound_finish_for_exec(void);
		extern void sf3000_sound_open_for_exec(void);
		sf3000_sound_finish_for_exec();
		sf3000_sound_open_for_exec();
		DBG("DBG quit: audio cleanup done + speaker line open for media app\n");
	}
	/* Standalone apps own their audio and have no gate: hand the amp line
	 * over OPEN (the video player above keeps it closed because libffplayer
	 * re-inits the whole audio path itself).
	 * ORDER MATTERS - fixed the silent-PS1 race (device log 253-255): the
	 * launcher's audio thread used to re-close the amp line ~2 ms after
	 * this open, so pcsx4all exec'd with a physically muted speaker and
	 * games like Worms Armageddon stayed totally silent.  Release our DAC
	 * FIRST (the consumer thread deinits AUDDEC/I2SO on its own thread,
	 * giving the audio daemon a clean handoff instead of a half-alive
	 * stream), THEN open the amp line as the LAST audio action before
	 * exec - nothing runs after this that could close it again. */
	if (next_is_standalone && !next_is_video_player && !next_is_image_viewer) {
		extern void sf3000_sound_release_for_exec(void);
		sf3000_sound_release_for_exec();
		extern void sf3000_sound_open_for_exec(void);
		sf3000_sound_open_for_exec();
		DBG("DBG quit: DAC released + speaker line opened for standalone app\n");
	}
	/* Keep fb0/dis fds open across exec — closing them breaks the panel
	 * state recovery (was working in commit 6537239). */
	fflush(stderr);
	fsync(STDERR_FILENO);
	if (core_path[0] && rom_path[0]) {
		if (next_is_standalone) {
			chmod(rom_path, 0755);
			execl(rom_path, rom_path, standalone_rom[0] ? standalone_rom : NULL, NULL);
		} else {
			const char *next_bin = picoarch_for_core(core_path);
			DBG("DBG exec game: bin=%s core=%s rom=%s\n", next_bin, core_path, rom_path);
			execl(next_bin, "picoarch", core_path, rom_path, NULL);
			DBG("DBG exec game FAILED: errno=%d (%s)\n", errno, strerror(errno));
		}
	}
	/* A game hands off to FrogUI with exec(), so normal plat_finish() is not
	 * reached.  Stop the game DAC before that handoff; FrogUI deliberately does
	 * not initialise it, preventing idle hiss/static in the main menu. Also
	 * close the amp line: the launcher mutes it on arrival anyway (see
	 * plat_init), but this closes the window where the next process inherits
	 * a live amp with no DAC owner. */
	{
		extern void sf3000_sound_finish_for_exec(void);
		extern void sf3000_sound_close_for_exec(void);
		sf3000_sound_finish_for_exec();
		sf3000_sound_close_for_exec();
	}
	DBG("DBG exec FrogUI fallback: bin=%s\n", PICOARCH_BIN);
	execl(PICOARCH_BIN, "picoarch", FROGUI_CORE, FROGUI_CORE, NULL);
	DBG("DBG exec FrogUI FAILED: errno=%d (%s)\n", errno, strerror(errno));
	/* execl failed — fall through to normal cleanup and exit */
#endif

	plat_finish();
	exit(code);
}
