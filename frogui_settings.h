#ifndef FROGUI_SETTINGS_H
#define FROGUI_SETTINGS_H

/* Single parser for FrogUI's /mnt/sdcard/frogui/settings.txt inside
 * picoarch (review: the file used to be parsed in three separate places -
 * main.c, plat_sdl.c and menu.c - and those copies could drift as soon
 * as the settings format changes).
 *
 * FORMAT CONTRACT (must match FrogUI's settings_save_file):
 *   - one "key=value" pair per line, '\n' terminated, optional '\r';
 *   - keys are case-sensitive; values are trimmed of trailing newline;
 *   - a missing file or a missing key means "absent" (default applies).
 * Adding a new consumer of a FrogUI setting goes through HERE, never
 * through another fopen/strncmp of the file. */

/* Returns 1 when the key exists and its value equals `val` exactly,
 * 0 when the key is absent or differs. */
int frogui_setting_is(const char *key, const char *val);

/* Returns the integer value of `key` (atoi semantics), or `def` when the
 * key is absent or the file is missing. */
int frogui_setting_int(const char *key, int def);

#endif
