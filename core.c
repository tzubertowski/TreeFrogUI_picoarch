#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "cheat.h"
#include "core.h"
#include "libpicofe/input.h"
#include "main.h"
#include "options.h"
#include "overrides.h"
#include "plat.h"
#include "util.h"

extern int g_debug_frame;
struct core_cbs current_core;
char core_path[MAX_PATH];
struct content *content;
static struct string_list *extensions;
struct cheats *cheats;

double sample_rate;
double frame_rate;
char core_lib_name[64] = "";      /* retro_system_info.library_name */
char core_lib_version[32] = "";   /* retro_system_info.library_version */
double aspect_ratio;
unsigned audio_buffer_size_override;
int state_slot;
int resume_slot = -1;

static char config_dir[MAX_PATH];
static char save_dir[MAX_PATH];
static char system_dir[MAX_PATH];
static struct retro_disk_control_ext_callback disk_control_ext;
static struct retro_frame_time_callback frame_time_cb_info;

static uint32_t buttons = 0;
int current_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

static int core_load_game_info(struct content *content, struct retro_game_info *game_info) {
	struct retro_system_info info = {};
	current_core.retro_get_system_info(&info);

	return content_load_game_info(content, game_info, info.need_fullpath);
}

void config_file_name(char *buf, size_t len, int is_game)
{
	if (is_game && content) {
		content_based_name(content, buf, len, save_dir, NULL, ".cfg");
	} else {
		snprintf(buf, len, "%s%s", config_dir, "picoarch.cfg");
	}
}

void save_relative_path(char *buf, size_t len, const char *basename) {
	snprintf(buf, len, "%s%s", save_dir, basename);
}

void sram_write(void) {
	char filename[MAX_PATH];
	FILE *sram_file = NULL;
	void *sram;

	size_t sram_size = current_core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) {
		return;
	}

	content_based_name(content, filename, MAX_PATH, save_dir, NULL, ".sav");

	sram_file = fopen(filename, "w");
	if (!sram_file) {
		PA_ERROR("Error opening SRAM file: %s\n", strerror(errno));
		return;
	}

	sram = current_core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || sram_size != fwrite(sram, 1, sram_size, sram_file)) {
		PA_ERROR("Error writing SRAM data to file\n");
	}

	fflush(sram_file);
	fsync(fileno(sram_file));
	fclose(sram_file);
	sync();   /* flush FAT metadata too: device is often powered off right after */
}

/* Periodic in-game battery-save flush. picoarch only wrote SRAM on pause-menu
 * open / clean exit, so with the game-switcher flow (power off straight from a
 * game) an in-game save made since the last menu open was lost. Called every
 * frame; rate-limited to ~10s by a frame counter (no RTC on this device), and
 * only writes when the SRAM buffer actually changed, so idle games cost one
 * memcmp and never touch the card. */
void sram_autosave(void) {
	static void *cache = NULL;
	static size_t cache_size = 0;
	static unsigned fc = 0;

	if ((++fc % 600) != 0) return;   /* ~every 10s at 60fps */

	size_t sram_size = current_core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;
	void *sram = current_core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!sram) return;

	if (cache_size != sram_size) {   /* first sight / size change: baseline only */
		free(cache);
		cache = malloc(sram_size);
		cache_size = sram_size;
		if (cache) memcpy(cache, sram, sram_size);
		return;
	}
	if (!cache || memcmp(cache, sram, sram_size) == 0) return;  /* unchanged */

	memcpy(cache, sram, sram_size);
	sram_write();
	DBG("DBG sram_autosave: SRAM changed → flushed\n");
}

void sram_read(void) {
	char filename[MAX_PATH];
	FILE *sram_file = NULL;
	void *sram;

	size_t sram_size = current_core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) {
		return;
	}

	content_based_name(content, filename, MAX_PATH, save_dir, NULL, ".sav");

	sram_file = fopen(filename, "r");
	if (!sram_file) {
		return;
	}

	sram = current_core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		PA_ERROR("Error reading SRAM data\n");
	}

	fclose(sram_file);
}

bool state_allowed(void) {
	return current_core.retro_serialize_size() > 0;
}

void state_file_name(char *name, size_t size, int slot) {
	char extension[8] = {0};

	snprintf(extension, sizeof(extension), ".st%d", slot);
	content_based_name(content, name, MAX_PATH, config_dir, NULL, extension);
}

/* Per-game "last screen" snapshot (<base>.scr.bmp) for the recents/switcher art,
 * even when there's no box art or save state. */
void save_game_screenshot(void) {
	if (!content) return;   /* caller (main) already skips this for FrogUI */
	char name[MAX_PATH];
	content_based_name(content, name, MAX_PATH, config_dir, NULL, ".scr");
	plat_dump_screen(name);   /* writes name + ".bmp" */
}

int pa_state_read(void) {
	char filename[MAX_PATH];
	FILE *state_file = NULL;
	void *state = NULL;
	int ret = -1;

	size_t state_size = current_core.retro_serialize_size();
	if (!state_size) {
		return 0;
	}

	state = calloc(1, state_size);
	if (!state) {
		PA_ERROR("Couldn't allocate memory for state\n");
		goto error;
	}

	state_file_name(filename, MAX_PATH, state_slot);

	state_file = fopen(filename, "r");
	if (!state_file) {
		PA_ERROR("Error opening state file: %s\n", strerror(errno));
		goto error;
	}

	if (state_size != fread(state, 1, state_size, state_file)) {
		PA_ERROR("Error reading state data from file\n");
		goto error;
	}

	if (!current_core.retro_unserialize(state, state_size)) {
		PA_ERROR("Error restoring save state\n", strerror(errno));
		goto error;
	}

	ret = 0;
error:
	if (state)
		free(state);
	if (state_file)
		fclose(state_file);
	return ret;
}

int pa_state_write(void) {
	char filename[MAX_PATH];
	FILE *state_file = NULL;
	void *state = NULL;
	int ret = -1;

	size_t state_size = current_core.retro_serialize_size();
	if (!state_size) {
		return false;
	}

	state = calloc(1, state_size);
	if (!state) {
		PA_ERROR("Couldn't allocate memory for state\n");
		goto error;
	}

	state_file_name(filename, MAX_PATH, state_slot);

	state_file = fopen(filename, "w");
	if (!state_file) {
		PA_ERROR("Error opening state file: %s\n", strerror(errno));
		goto error;
	}

	if (!current_core.retro_serialize(state, state_size)) {
		PA_ERROR("Error creating save state\n", strerror(errno));
		goto error;
	}

	if (state_size != fwrite(state, 1, state_size, state_file)) {
		PA_ERROR("Error writing state data to file\n");
		goto error;
	}

	plat_dump_screen(filename);

	if (state_file) {
		fflush(state_file);
		fsync(fileno(state_file));
	}
	ret = 0;
error:
	if (state)
		free(state);
	if (state_file)
		fclose(state_file);

	return ret;
}

int state_resume(void) {
	int ret = 0;
	if (resume_slot != -1) {
		state_slot = resume_slot;
		ret = pa_state_read();
		resume_slot = -1;
	}
	return ret;
}

unsigned disc_get_count(void) {
	if (disk_control_ext.get_num_images)
		return disk_control_ext.get_num_images();

	return 0;
}

unsigned disc_get_index(void) {
	if (disk_control_ext.get_image_index)
		return disk_control_ext.get_image_index();

	return 0;
}

bool disc_switch_index(unsigned index) {
	bool ret = false;
	if (!disk_control_ext.set_eject_state || !disk_control_ext.set_image_index)
		return false;

	disk_control_ext.set_eject_state(true);
	ret = disk_control_ext.set_image_index(index);
	disk_control_ext.set_eject_state(false);

	return ret;
}

bool disc_replace_index(unsigned index, const char *content_path) {
	bool ret = false;
	struct retro_game_info info = {};
	struct content *content;
	if (!disk_control_ext.replace_image_index)
		return false;

	content = content_init(content_path);
	if (!content)
		goto finish;

	if (core_load_game_info(content, &info))
		goto finish;

	ret = disk_control_ext.replace_image_index(index, &info);

finish:
	content_free(content);
	return ret;
}

static void set_directories(const char *core_name, const char *tag_name) {
	const char *home = getenv("HOME");
	const char *sdcard_path = getenv("SDCARD_PATH");
	// char *dst = save_dir;
	// int len = MAX_PATH;
// #ifndef MINUI
// 	char cwd[MAX_PATH];
// #endif
	const char *sd = "/mnt/sdcard/picoarch";
	snprintf(config_dir, MAX_PATH, "%s/%s/", sd, tag_name);
	mkdir("/mnt/sdcard/picoarch", 0755);
	mkdir(config_dir, 0755);

	snprintf(save_dir, MAX_PATH, "%s/%s/", sd, tag_name);
	snprintf(system_dir, MAX_PATH, "/mnt/sdcard/cubegm/bios");
	mkdir("/mnt/sdcard/cubegm/bios", 0755);
	(void)home; (void)sdcard_path;

// #ifdef MINUI
// 	strncpy(system_dir, save_dir, MAX_PATH-1);
// #else
// 	if (getcwd(cwd, MAX_PATH)) {
// 		snprintf(system_dir, MAX_PATH, "%s/.picoarch_system", cwd);
// 		mkdir(system_dir, 0755);
// 	} else {
// 		PA_FATAL("Can't find system directory");
// 	}
// #endif
}

// based on eggs pokemini miyoominin rumble
static bool pa_set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	// PA_INFO("Rumble (strength: %u)\n", (unsigned int)strength);

	uint32_t val = strength>0; // TODO: can we do better than just off or on?
	
	int fd;
	const char str_export[] = "48";
	const char str_direction[] = "out";
	char value[1];
	value[0] = ((val&1)^1) + 0x30;

	fd = open("/sys/class/gpio/export",O_WRONLY);
		if (fd > 0) {
			write(fd, str_export, 2);
			close(fd);
		}
	fd = open("/sys/class/gpio/gpio48/direction",O_WRONLY);
		if (fd > 0) {
			write(fd, str_direction, 3);
			close(fd);
		}
	fd = open("/sys/class/gpio/gpio48/value",O_WRONLY);
		if (fd > 0) {
			write(fd, value, 1);
			close(fd);
		}
	
	return true;
}

static bool pa_environment(unsigned cmd, void *data) {
	switch(cmd) {
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: { /* 3 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: { /* 6 */
		const struct retro_message *message = (const struct retro_message*)data;
		if (message) {
			PA_INFO("%s\n", message->msg);
		}

		break;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: { /* 9 */
		const char **out = (const char **)data;
		if (out)
			*out = system_dir;
		break;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { /* 10 */
		if (data) current_pixel_format = *(const enum retro_pixel_format *)data;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: { /* 13 */
		const struct retro_disk_control_callback *var =
			(const struct retro_disk_control_callback *)data;

		if (var) {
			memset(&disk_control_ext, 0, sizeof(struct retro_disk_control_ext_callback));
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_callback));
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: { /* 21 */
		const struct retro_frame_time_callback *var =
			(const struct retro_frame_time_callback *)data;

		if (var) {
			memcpy(&frame_time_cb_info, var, sizeof(struct retro_frame_time_callback));
		} else {
			memset(&frame_time_cb_info, 0, sizeof(struct retro_frame_time_callback));
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: { /* 15 */
		struct retro_variable *var = (struct retro_variable *)data;
		if (var && var->key) {
			var->value = options_get_value(var->key);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLES: { /* 16 */
		const struct retro_variable *vars = (const struct retro_variable *)data;
		options_free();
		if (vars) {
			options_init_variables(vars);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: { /* 17 */
		bool *out = (bool *)data;
		if (out)
			*out = options_changed();
		break;
	}
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: { /* 23 */
        struct retro_rumble_interface *iface =
           (struct retro_rumble_interface*)data;

        PA_INFO("Setup rumble interface.\n");
        iface->set_rumble_state = pa_set_rumble_state;
		break;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: { /* 27 */
		struct retro_log_callback *log_cb = (struct retro_log_callback *)data;
		if (log_cb)
			log_cb->log = core_log_cb;
		break;
	}
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: { /* 31 */
		const char **out = (const char **)data;
		if (out)
			*out = save_dir;
		break;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: { /* 52 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
		unsigned *out = (unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: { /* 53 */
		options_free();
		if (data) {
			/* data IS the array pointer (core passes (void*)options), not a
			 * pointer-to-pointer. The extra deref read options[0].key as the
			 * array base → garbage key → strlen segfault (FBA2012 hit this). */
			options_init((const struct retro_core_option_definition *)data);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: { /* 54 */
		const struct retro_core_options_intl *options = (const struct retro_core_options_intl *)data;
		if (options && options->us) {
			options_free();
			options_init(options->us);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: { /* 55 */
		const struct retro_core_option_display *display =
			(const struct retro_core_option_display *)data;

		if (display)
			options_set_visible(display->key, display->visible);
		break;
	}
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: { /* 57 */
		unsigned *out =	(unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: { /* 58 */
		const struct retro_disk_control_ext_callback *var =
			(const struct retro_disk_control_ext_callback *)data;

		if (var) {
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_ext_callback));
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: { /* 62 */
		const struct retro_audio_buffer_status_callback *cb =
			(const struct retro_audio_buffer_status_callback *)data;
		if (cb) {
			current_core.retro_audio_buffer_status = cb->callback;
		} else {
			current_core.retro_audio_buffer_status = NULL;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: { /* 63 */
		const unsigned *latency_ms = (const unsigned *)data;
		if (latency_ms) {
			unsigned frames = *latency_ms * frame_rate / 1000;
			if (frames < 30)
				audio_buffer_size_override = frames;
			else
				PA_WARN("Audio buffer change out of range (%d), ignored\n", frames);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SHUTDOWN: { /* 7 */
		extern bool should_quit;
		should_quit = true;
		break;
	}
	default:
		PA_DEBUG("Unsupported environment cmd: %u\n", cmd);
		return false;
	}

	return true;
}

static void pa_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
	#ifdef PLATFORM_SF3000
		/* Trace the callback before filtering NULL/quit frames.  This is the
		 * authoritative boundary between the libretro core and the frontend. */
		static unsigned sf_refresh_n;
		if (sf_refresh_n < 24 || (sf_refresh_n && (sf_refresh_n % 600) == 0)) {
			dbg_log("DBG video_refresh entry pid=%d n=%u data=%p size=%ux%u pitch=%zu quit=%d\n",
			        getpid(), sf_refresh_n + 1, data, width, height, pitch, should_quit);
			fprintf(stderr, "TFDBG video_refresh pid=%d n=%u data=%p size=%ux%u pitch=%zu quit=%d\n",
			        getpid(), sf_refresh_n + 1, data, width, height, pitch, should_quit);
		}
		++sf_refresh_n;
	#endif
	if (data && !should_quit) {
		pa_track_render();
		plat_video_process(data, width, height, pitch);
	}
}

static void pa_audio_sample(int16_t left, int16_t right) {
	const struct audio_frame frame = { .left = left, .right = right };
	if (!should_quit && enable_audio)
		plat_sound_write(&frame, 1);
}

static size_t pa_audio_sample_batch(const int16_t *data, size_t frames) {
	if (!should_quit && enable_audio)
		plat_sound_write((const struct audio_frame *)data, frames);
	return frames;
}

static void pa_input_poll(void) {
	int actions[IN_BINDTYPE_COUNT] = { 0, };
	in_update(actions);
	handle_emu_action(EACTION_NONE);

#ifdef PLATFORM_SF3000
	extern volatile uint32_t *sf3000_keys_ptr;
	extern volatile uint32_t sf3000_keys_filtered;
	extern uint32_t sf3000_keys_to_buttons(uint32_t);

	if (sf3000_keys_ptr) {
		uint32_t raw = sf3000_keys_filtered;  /* race-filtered (see plat_sdl.c) */
		static uint32_t prev_raw = 0;   /* for rising-edge combo detection */

		/* Hardware hotkeys: FN or SELECT+START = menu; SELECT+R2 save; SELECT+L2 load; SELECT+R1 fast-forward. */
		const uint32_t SEL_BIT   = (1u << 0);   /* SELECT = bit 0  */
		const uint32_t START_BIT = (1u << 3);   /* START  = bit 3  */
		const uint32_t FN_BIT    = (1u << 16);  /* FN     = bit 16 */
		const uint32_t R1_BIT    = (1u << 11);  /* R1     = bit 11 */
		const uint32_t L2_BIT    = (1u << 8);   /* L2     = bit 8  */
		const uint32_t R2_BIT    = (1u << 9);   /* R2     = bit 9  */

		/* Fire each combo only on the rising edge (newly fully-pressed this poll),
		 * else it repeats every frame held. */
		#define COMBO_EDGE(m) (((raw & (m)) == (m)) && ((prev_raw & (m)) != (m)))
		/* MENU is special: menu_loop() blocks, so prev_raw is frozen while it's
		 * open. If SELECT+START is still held (or the bits flicker) on return, a
		 * plain edge re-fires and the menu strobes open/closed — which on SF3500
		 * also churns the driver geometry and garbles the screen. So LATCH it:
		 * fire once, then refuse until SELECT is fully released. */
		/* The FrogUI launcher core gets NO hotkeys: the pause menu makes no
		 * sense there, and reopening/leaving it corrupts the launcher screen
		 * (fb-write path). It also must not inherit a game's remappable Player
		 * Control bindings: those can turn physical B into Left, so FrogUI's
		 * visible "B Back" prompts no longer match. Feed its stable physical
		 * cubevol mapping; FrogUI applies its own keymap.txt remaps itself. */
		extern int g_is_frogui;
		static int menu_armed = 1;
		static int ss_armed = 1;   /* save/load-state latch */
		static int menu_off_cnt = 0;
		static int suppress_menu_buttons = 0;
		if (g_is_frogui) {
			prev_raw = raw;
			buttons = sf3000_keys_to_buttons(raw);
			goto frogui_no_hotkeys;
		}
		if (menu_armed && (((raw & (SEL_BIT | START_BIT)) == (SEL_BIT | START_BIT)) || (raw & FN_BIT))) {
			menu_armed = 0;
			menu_off_cnt = 0;
			handle_emu_action(EACTION_MENU);
			/* PCE maps START to Run and soft-resets on Run+Select. The input
			 * snapshot above predates the blocking menu loop, so forwarding it on
			 * the return frame resets the core after changing an option. Suppress
			 * both libretro buttons until the physical combo is fully released. */
			suppress_menu_buttons = 1;
		}
		/* Save/load state are LATCHED like the menu: fire once, then refuse until
		 * SELECT is fully released. The two-writer input race flickers L2/R2 while
		 * SELECT is held, and a plain edge re-fired the save every flicker → on a
		 * big-state core (picodrive ~663KB) hundreds of rapid saves exhaust RAM,
		 * serialize starts faulting, and the console hard-freezes. One per hold. */
		else if (ss_armed && COMBO_EDGE(SEL_BIT | L2_BIT)) {
			ss_armed = 0;
			handle_emu_action(EACTION_LOAD_STATE);
		}
		else if (ss_armed && COMBO_EDGE(SEL_BIT | R2_BIT)) {
			ss_armed = 0;
			handle_emu_action(EACTION_SAVE_STATE);
		}
		else if (COMBO_EDGE(SEL_BIT | R1_BIT))
			handle_emu_action(EACTION_TOGGLE_FF);  /* SELECT+R1 = fast-forward */
		#undef COMBO_EDGE

		/* Re-arm the menu only after SELECT has been CONTINUOUSLY released for a
		 * stretch. A plain "SELECT==0" re-arm let the two-writer bit-flicker (a
		 * 1-2 poll dropout) re-arm instantly, so the menu strobed open/closed and
		 * only ever showed one black re-init frame. Require ~8 clean polls. */
		if (!(raw & (SEL_BIT | FN_BIT))) { if (++menu_off_cnt >= 8) { menu_armed = 1; ss_armed = 1; } }
		else menu_off_cnt = 0;
		prev_raw = raw;
		/* Game buttons come from the REMAPPABLE bind table (in_update filled
		 * actions[] above from the per-button SDL keys the input thread emits).
		 * This is the adapter: OPTION→PLAYER CONTROL edits these binds and they
		 * persist in the config. Hotkeys above stay on raw physical bits so
		 * SELECT+START always opens the menu regardless of remapping. */
		buttons = actions[IN_BINDTYPE_PLAYER12];
		if (suppress_menu_buttons) {
			buttons &= ~((1u << RETRO_DEVICE_ID_JOYPAD_SELECT) |
			             (1u << RETRO_DEVICE_ID_JOYPAD_START));
			if (!(raw & (SEL_BIT | START_BIT)))
				suppress_menu_buttons = 0;
		}
frogui_no_hotkeys: ;
	} else {
		buttons = actions[IN_BINDTYPE_PLAYER12];
	}
#else
	buttons = actions[IN_BINDTYPE_PLAYER12];
#endif
}

static int16_t pa_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return buttons;

		return (buttons >> id) & 1;
	}

	return 0;
}

void core_extract_name(const char* core_file, char *buf, size_t len) {
	char *suffix = NULL;

	strncpy(buf, basename(core_file), MAX_PATH);
	buf[len - 1] = 0;

	suffix = strrchr(buf, '_');
	if (suffix && !strcmp(suffix, "_libretro.so"))
		*suffix = 0;
	else {
		suffix = strrchr(buf, '.');
		if (suffix && !strcmp(suffix, ".so"))
			*suffix = 0;
	}
}

int core_open(const char *corefile, const char *tag_name) {
	struct retro_system_info info = {};

	void (*set_environment)(retro_environment_t) = NULL;
	void (*set_video_refresh)(retro_video_refresh_t) = NULL;
	void (*set_audio_sample)(retro_audio_sample_t) = NULL;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
	void (*set_input_poll)(retro_input_poll_t) = NULL;
	void (*set_input_state)(retro_input_state_t) = NULL;

	PA_INFO("Loading core %s\n", corefile);

	/* Pre-load C++ runtime so cores that need it (e.g. Nestopia) can resolve
	   their symbols even though libstdc++ isn't a declared NEEDED dependency */
	dlopen("libstdc++.so.6", RTLD_NOW | RTLD_GLOBAL);

	memset(&current_core, 0, sizeof(current_core));
	memset(&frame_time_cb_info, 0, sizeof(frame_time_cb_info));
	current_core.handle = dlopen(corefile, RTLD_NOW | RTLD_GLOBAL);

	if (!current_core.handle) {
		PA_ERROR("Couldn't load core: %s\n", dlerror());
		return -1;
	}

	set_directories(core_name, tag_name);
	set_overrides(core_name);

	current_core.retro_init = dlsym(current_core.handle, "retro_init");
	current_core.retro_deinit = dlsym(current_core.handle, "retro_deinit");
	current_core.retro_get_system_info = dlsym(current_core.handle, "retro_get_system_info");
	current_core.retro_get_system_av_info = dlsym(current_core.handle, "retro_get_system_av_info");
	current_core.retro_set_controller_port_device = dlsym(current_core.handle, "retro_set_controller_port_device");
	current_core.retro_reset = dlsym(current_core.handle, "retro_reset");
	current_core.retro_run = dlsym(current_core.handle, "retro_run");
	current_core.retro_serialize_size = dlsym(current_core.handle, "retro_serialize_size");
	current_core.retro_serialize = dlsym(current_core.handle, "retro_serialize");
	current_core.retro_unserialize = dlsym(current_core.handle, "retro_unserialize");
	current_core.retro_cheat_reset = dlsym(current_core.handle, "retro_cheat_reset");
	current_core.retro_cheat_set = dlsym(current_core.handle, "retro_cheat_set");
	current_core.retro_load_game = dlsym(current_core.handle, "retro_load_game");
	current_core.retro_load_game_special = dlsym(current_core.handle, "retro_load_game_special");
	current_core.retro_unload_game = dlsym(current_core.handle, "retro_unload_game");
	current_core.retro_get_region = dlsym(current_core.handle, "retro_get_region");
	current_core.retro_get_memory_data = dlsym(current_core.handle, "retro_get_memory_data");
	current_core.retro_get_memory_size = dlsym(current_core.handle, "retro_get_memory_size");

	set_environment = dlsym(current_core.handle, "retro_set_environment");
	set_video_refresh = dlsym(current_core.handle, "retro_set_video_refresh");
	set_audio_sample = dlsym(current_core.handle, "retro_set_audio_sample");
	set_audio_sample_batch = dlsym(current_core.handle, "retro_set_audio_sample_batch");
	set_input_poll = dlsym(current_core.handle, "retro_set_input_poll");
	set_input_state = dlsym(current_core.handle, "retro_set_input_state");
	#ifdef PLATFORM_SF3000
	DBG("DBG core symbols: video_set=%p audio_set=%p run=%p load=%p\n",
	    (void *)set_video_refresh, (void *)set_audio_sample,
	    (void *)current_core.retro_run, (void *)current_core.retro_load_game);
	#endif

	dlerror();
	set_environment(pa_environment);
	set_video_refresh(pa_video_refresh);
	#ifdef PLATFORM_SF3000
	DBG("DBG video callback registered: core=%s fn=%p\n", corefile, (void *)pa_video_refresh);
	#endif
	set_audio_sample(pa_audio_sample);
	set_audio_sample_batch(pa_audio_sample_batch);
	set_input_poll(pa_input_poll);
	set_input_state(pa_input_state);

	current_core.retro_get_system_info(&info);
	if (info.valid_extensions)
		extensions = string_split(info.valid_extensions, '|');
	if (info.library_name)
		strncpy(core_lib_name, info.library_name, sizeof(core_lib_name) - 1);
	if (info.library_version)
		strncpy(core_lib_version, info.library_version, sizeof(core_lib_version) - 1);

	return 0;
}

void core_load(void) {
	current_core.retro_init();
	current_core.initialized = true;
	PA_INFO("Finished loading core\n");
}

int core_load_content(struct content *content) {
	struct retro_game_info game_info = {};
	struct retro_system_av_info av_info = {};
	int ret = -1;
	char cheats_path[MAX_PATH] = {0};

	DBG("DBG M5a: load_game_info (unzip if needed)\n");
	if (core_load_game_info(content, &game_info)) {
		goto finish;
	}

	DBG("DBG M5b: retro_load_game path=%s\n", game_info.path ? game_info.path : "(null)");
	if (!current_core.retro_load_game(&game_info)) {
		PA_ERROR("Couldn't load content\n");
		goto finish;
	}
	DBG("DBG M5c: retro_load_game returned\n");

	content_based_name(content, cheats_path, sizeof(cheats_path), config_dir, "cheats/", ".cht");
	if (cheats_path[0] != '\0') {
		cheats = cheats_load(cheats_path);
		core_apply_cheats(cheats);
	}

	DBG("DBG M5d: sram_read\n");
	sram_read();

	if (!strcmp(core_name, "fmsx") && current_core.retro_set_controller_port_device) {
		/* fMSX works best with joypad + keyboard */
		current_core.retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0));
	}
	if (!strcmp(core_name, "fuse") && current_core.retro_set_controller_port_device) {
		/* fuse defaults port 0 to the Cursor joystick; most ZX Spectrum games
		 * expect Kempston. Force port 0 = Kempston (RETRO_DEVICE_JOYPAD subclass 1). */
		current_core.retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));
	}
	if (!strcmp(core_name, "tyrquake") && current_core.retro_set_controller_port_device) {
		/* tyrquake only applies its key bindings inside
		 * retro_set_controller_port_device; without this call no input is
		 * bound (controls dead + console spam on button press). */
		current_core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	}
	if (!strncmp(core_name, "vice_", 5) && current_core.retro_set_controller_port_device) {
		/* VICE initializes all ports to RETRO_DEVICE_NONE and relies on the
		 * frontend to plug one in. Without this, it deliberately skips both
		 * joystick input and RetroPad hotkeys such as the virtual keyboard. */
		current_core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	}

	DBG("DBG M5e: get_system_av_info\n");
	current_core.retro_get_system_av_info(&av_info);
	DBG("DBG M5f: av %dx%d rate=%d fps*100=%d, plat_reinit\n",
	    av_info.geometry.base_width, av_info.geometry.base_height,
	    (int)av_info.timing.sample_rate, (int)(av_info.timing.fps * 100));

	PA_INFO("Screen: %dx%d\n", av_info.geometry.base_width, av_info.geometry.base_height);
	PA_INFO("Audio sample rate: %f\n", av_info.timing.sample_rate);
	PA_INFO("Frame rate: %f\n", av_info.timing.fps);
	PA_INFO("Reported aspect ratio: %f\n", av_info.geometry.aspect_ratio);

	sample_rate = av_info.timing.sample_rate;
	frame_rate = av_info.timing.fps;
	aspect_ratio = av_info.geometry.aspect_ratio;
	plat_reinit();

#ifdef MMENU
	content_based_name(content, save_template_path, MAX_PATH, config_dir, NULL, ".st%i");
#endif

	ret = 0;
finish:
	return ret;
}

void core_apply_cheats(struct cheats *cheats) {
	if (!cheats)
		return;

	if (!current_core.retro_cheat_reset || !current_core.retro_cheat_set)
		return;

	current_core.retro_cheat_reset();
	for (int i = 0; i < cheats->count; i++) {
		if (cheats->cheats[i].enabled) {
			current_core.retro_cheat_set(i, cheats->cheats[i].enabled, cheats->cheats[i].code);
		}
	}
}

void core_unload_content(void) {
	sram_write();

	cheats_free(cheats);
	cheats = NULL;

	current_core.retro_unload_game();
	content_free(content);
	content = NULL;
}

const char **core_extensions(void) {
	if (extensions)
		return extensions->list;

	return NULL;
}

void core_frame_time_tick(void) {
	/* Pass the fixed reference interval, not a measured delta: picoarch
	 * paces frames at the core's reported fps, and ecwolf itself falls
	 * back to reference when its dynamic-fps option is off. */
	if (frame_time_cb_info.callback)
		frame_time_cb_info.callback(frame_time_cb_info.reference);
}

void core_unload(void) {
	PA_INFO("Unloading core...\n");

	if (current_core.initialized) {
		core_unload_content();
		current_core.retro_deinit();
		current_core.initialized = false;
	}

	string_list_free(extensions);
	extensions = NULL;

	options_free();

	if (current_core.handle) {
		dlclose(current_core.handle);
		current_core.handle = NULL;
	}
}
