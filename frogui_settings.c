#include "frogui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FROGUI_SETTINGS_FILE "/mnt/sdcard/frogui/settings.txt"

/* One shared pass over the file: find `key`, copy its raw value into
 * val_buf (newline/CR trimmed).  Returns 0 when found, -1 when absent. */
static int frogui_setting_lookup(const char *key, char *val_buf, int val_len)
{
	FILE *f = fopen(FROGUI_SETTINGS_FILE, "r");
	if (!f)
		return -1;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		if (strcmp(line, key) != 0)
			continue;
		char *val = eq + 1;
		char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
		char *cr = strchr(val, '\r'); if (cr) *cr = '\0';
		snprintf(val_buf, val_len, "%s", val);
		fclose(f);
		return 0;
	}
	fclose(f);
	return -1;
}

int frogui_setting_is(const char *key, const char *val)
{
	char v[96];
	if (frogui_setting_lookup(key, v, sizeof v) < 0)
		return 0;
	return strcmp(v, val) == 0;
}

int frogui_setting_int(const char *key, int def)
{
	char v[32];
	if (frogui_setting_lookup(key, v, sizeof v) < 0)
		return def;
	return atoi(v);
}
