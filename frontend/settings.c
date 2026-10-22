/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The frontend's settings, in ~/.config/armgba/frontend.cfg, and the core's options, in
// ~/.config/armgba/armgba.cfg: lines of key = "value".

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Settings settings;
struct Option options[OPTIONS_MAX];
int optionCount;
struct Category categories[32];
int categoryCount;
bool optionsChanged;

const char* actionNames[ACTIONS] = {
	"Menu", "Fast Forward (hold)", "Rewind (hold)", "Pause", "Save State", "Load State", "Previous Slot", "Next Slot",
	"Screenshot", "Record", "Full Screen", "Restart",
};

const char* buttonNames[BUTTONS] = {
	"B", "Turbo B", "Select", "Start", "Up", "Down", "Left", "Right", "A", "Turbo A", "L", "R", "L2", "R2", "L3", "R3",
};

// Values read from the files before the core names its options.
static char* savedKeys[OPTIONS_MAX];
static char* savedValues[OPTIONS_MAX];
static int savedCount;

void settingsDefaults(void) {
	memset(&settings, 0, sizeof(settings));
	settings.scale = 3;
	settings.filter = FILTER_SHARP;
	settings.shadow = 4;
	settings.volume = 100;
	settings.latency = 3;
	settings.rewindMegabytes = 64;
	settings.netPort = 55437;
	settings.inputDelay = 1;
	snprintf(settings.nickname, sizeof(settings.nickname), "Player");
	// mGBA's keys; C and V are the turbo buttons, 1 to 4 L2 to R3.
	static const SDL_Scancode keys[BUTTONS] = {
		SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
		SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_X, SDL_SCANCODE_V, SDL_SCANCODE_A, SDL_SCANCODE_S,
		SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
	};
	// Libretro's A is the right face button and B the bottom one, as on a Game Boy; SDL names them
	// by an Xbox pad's labels.
	static const int pad[BUTTONS] = {
		SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
		SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
		SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y,
		SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_LEFT_TRIGGER, PAD_RIGHT_TRIGGER,
		SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
	};
	static const SDL_Scancode hotkeys[ACTIONS] = {
		SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_R, SDL_SCANCODE_P, SDL_SCANCODE_F5, SDL_SCANCODE_F7,
		SDL_SCANCODE_F6, SDL_SCANCODE_F8, SDL_SCANCODE_F12, SDL_SCANCODE_F9, SDL_SCANCODE_F11, SDL_SCANCODE_F2,
	};
	memcpy(settings.keys, keys, sizeof(keys));
	memcpy(settings.pad, pad, sizeof(pad));
	memcpy(settings.hotkeys, hotkeys, sizeof(hotkeys));
	for (int i = 0; i < ACTIONS; ++i) {
		settings.padHotkeys[i] = PAD_NONE;
	}
	settings.padHotkeys[ACT_MENU] = SDL_CONTROLLER_BUTTON_GUIDE;
	const char* home = getenv("HOME");
	snprintf(settings.directory, sizeof(settings.directory), "%s", home ? home : "/");
}

// Reads key = "value" lines; # starts a comment. Calls take(key, value) for each.
static void readLines(const char* path, void (*take)(const char* key, const char* value)) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[PATH_SIZE + 256];
	while (fgets(line, sizeof(line), f)) {
		char* key = line + strspn(line, " \t");
		char* equals = strchr(key, '=');
		if (*key == '#' || !equals) {
			continue;
		}
		char* end = equals;
		while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
			--end;
		}
		*end = '\0';
		char* value = equals + 1 + strspn(equals + 1, " \t");
		if (*value == '"') {
			++value;
			char* close = strrchr(value, '"');
			if (close) {
				*close = '\0';
			}
		} else {
			value[strcspn(value, "\r\n")] = '\0';
		}
		take(key, value);
	}
	fclose(f);
}

static void copyText(char* out, size_t size, const char* value) {
	snprintf(out, size, "%s", value);
}

static void takeSetting(const char* key, const char* value) {
	int const n = atoi(value);
	bool const yes = !strcmp(value, "true");
	if (!strcmp(key, "scale")) {
		settings.scale = n < 1 ? 1 : n > 8 ? 8 : n;
	} else if (!strcmp(key, "fullscreen")) {
		settings.fullscreen = yes;
	} else if (!strcmp(key, "integer_scale")) {
		settings.integerScale = yes;
	} else if (!strcmp(key, "stretch")) {
		settings.stretch = yes;
	} else if (!strcmp(key, "vsync")) {
		settings.vsync = yes;
	} else if (!strcmp(key, "filter")) {
		settings.filter = n >= 0 && n < FILTERS ? n : 0;
	} else if (!strcmp(key, "lcd_shadow")) {
		settings.shadow = n >= 0 && n <= 8 ? n : 4;
	} else if (!strcmp(key, "show_fps")) {
		settings.showFps = yes;
	} else if (!strcmp(key, "volume")) {
		settings.volume = n < 0 ? 0 : n > 100 ? 100 : n;
	} else if (!strcmp(key, "latency")) {
		settings.latency = n < 1 ? 1 : n > 8 ? 8 : n;
	} else if (!strcmp(key, "runahead")) {
		settings.runahead = n < 0 ? 0 : n > 4 ? 4 : n;
	} else if (!strcmp(key, "rewind")) {
		settings.rewind = yes;
	} else if (!strcmp(key, "rewind_megabytes")) {
		settings.rewindMegabytes = n < 16 ? 16 : n > 1024 ? 1024 : n;
	} else if (!strcmp(key, "auto_state")) {
		settings.autoState = yes;
	} else if (!strcmp(key, "nickname")) {
		copyText(settings.nickname, sizeof(settings.nickname), value);
	} else if (!strcmp(key, "netplay_host")) {
		copyText(settings.host, sizeof(settings.host), value);
	} else if (!strcmp(key, "netplay_port")) {
		settings.netPort = n > 0 && n < 65536 ? n : 55437;
	} else if (!strcmp(key, "input_delay")) {
		settings.inputDelay = n < 0 ? 0 : n > 8 ? 8 : n;
	} else if (!strcmp(key, "cheevos_user")) {
		copyText(settings.cheevosUser, sizeof(settings.cheevosUser), value);
	} else if (!strcmp(key, "cheevos_token")) {
		copyText(settings.cheevosToken, sizeof(settings.cheevosToken), value);
	} else if (!strcmp(key, "hardcore")) {
		settings.hardcore = yes;
	} else if (!strcmp(key, "directory")) {
		copyText(settings.directory, sizeof(settings.directory), value);
	} else if (!strncmp(key, "key_", 4) || !strncmp(key, "pad_", 4) || !strncmp(key, "hotkey_", 7) || !strncmp(key, "padhotkey_", 10)) {
		int const index = atoi(strrchr(key, '_') + 1);
		if (!strncmp(key, "key_", 4) && index >= 0 && index < BUTTONS) {
			settings.keys[index] = (SDL_Scancode) n;
		} else if (!strncmp(key, "pad_", 4) && index >= 0 && index < BUTTONS) {
			settings.pad[index] = n;
		} else if (!strncmp(key, "hotkey_", 7) && index >= 0 && index < ACTIONS) {
			settings.hotkeys[index] = (SDL_Scancode) n;
		} else if (!strncmp(key, "padhotkey_", 10) && index >= 0 && index < ACTIONS) {
			settings.padHotkeys[index] = n;
		}
	}
}

void settingsRead(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/frontend.cfg", configDirectory);
	readLines(path, takeSetting);
}

void settingsWrite(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/frontend.cfg", configDirectory);
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	const char* const no[2] = { "false", "true" };
	fprintf(f, "# armgba's own settings; the menu changes them. Keys and buttons are SDL's numbers.\n");
	fprintf(f, "scale = %d\nfullscreen = %s\ninteger_scale = %s\nstretch = %s\nvsync = %s\nfilter = %d\nlcd_shadow = %d\nshow_fps = %s\n",
	        settings.scale, no[settings.fullscreen], no[settings.integerScale], no[settings.stretch], no[settings.vsync], settings.filter,
	        settings.shadow, no[settings.showFps]);
	fprintf(f, "volume = %d\nlatency = %d\nrunahead = %d\nrewind = %s\nrewind_megabytes = %d\nauto_state = %s\n", settings.volume,
	        settings.latency, settings.runahead, no[settings.rewind], settings.rewindMegabytes, no[settings.autoState]);
	fprintf(f, "nickname = \"%s\"\nnetplay_host = \"%s\"\nnetplay_port = %d\ninput_delay = %d\n", settings.nickname, settings.host,
	        settings.netPort, settings.inputDelay);
	fprintf(f, "cheevos_user = \"%s\"\ncheevos_token = \"%s\"\nhardcore = %s\n", settings.cheevosUser, settings.cheevosToken,
	        no[settings.hardcore]);
	fprintf(f, "directory = \"%s\"\n", settings.directory);
	for (int i = 0; i < BUTTONS; ++i) {
		fprintf(f, "key_%d = %d\npad_%d = %d\n", i, settings.keys[i], i, settings.pad[i]);
	}
	for (int i = 0; i < ACTIONS; ++i) {
		fprintf(f, "hotkey_%d = %d\npadhotkey_%d = %d\n", i, settings.hotkeys[i], i, settings.padHotkeys[i]);
	}
	fclose(f);
}

// The core's options.

static void takeSaved(const char* key, const char* value) {
	if (savedCount < OPTIONS_MAX) {
		savedKeys[savedCount] = strdup(key);
		savedValues[savedCount] = strdup(value);
		++savedCount;
	}
}

void optionsReadSaved(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/armgba.cfg", configDirectory);
	readLines(path, takeSaved);
}

void optionsWrite(void) {
	if (!optionCount) {
		return;
	}
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/armgba.cfg", configDirectory);
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "# armGBA's options: the menu's Core Options change them, or edit each line's value, one of those\n# listed above it.\n\n");
	for (int i = 0; i < optionCount; ++i) {
		struct Option* o = &options[i];
		fprintf(f, "# %s:", o->description ? o->description : o->key);
		for (int v = 0; v < o->valueCount; ++v) {
			fprintf(f, "%s \"%s\"", v ? "," : "", o->values[v]);
		}
		fprintf(f, " (default \"%s\")\n%s = \"%s\"\n\n", o->defaultValue, o->key, o->value);
	}
	fclose(f);
}

static char* copyString(const char* s) {
	return s ? strdup(s) : NULL;
}

struct Option* optionFind(const char* key) {
	for (int i = 0; i < optionCount; ++i) {
		if (!strcmp(options[i].key, key)) {
			return &options[i];
		}
	}
	return NULL;
}

void optionSet(struct Option* option, const char* value) {
	if (strcmp(option->value, value)) {
		free(option->value);
		option->value = strdup(value);
		optionsChanged = true;
	}
}

void optionsDefine(const struct retro_core_options_v2* definitions) {
	for (const struct retro_core_option_v2_category* c = definitions->categories; c && c->key && categoryCount < 32; ++c) {
		bool known = false;
		for (int i = 0; i < categoryCount; ++i) {
			known |= !strcmp(categories[i].key, c->key);
		}
		if (!known) {
			categories[categoryCount].key = strdup(c->key);
			categories[categoryCount].description = copyString(c->desc);
			++categoryCount;
		}
	}
	for (const struct retro_core_option_v2_definition* d = definitions->definitions; d->key && optionCount < OPTIONS_MAX; ++d) {
		if (optionFind(d->key)) {
			continue;
		}
		struct Option* o = &options[optionCount++];
		o->key = strdup(d->key);
		o->description = copyString(d->desc);
		o->category = copyString(d->category_key);
		o->visible = true;
		while (o->valueCount < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[o->valueCount].value) {
			++o->valueCount;
		}
		o->values = calloc(o->valueCount, sizeof(*o->values));
		o->labels = calloc(o->valueCount, sizeof(*o->labels));
		for (int v = 0; v < o->valueCount; ++v) {
			o->values[v] = strdup(d->values[v].value);
			o->labels[v] = strdup(d->values[v].label ? d->values[v].label : d->values[v].value);
		}
		o->defaultValue = copyString(d->default_value ? d->default_value : d->values[0].value);
		o->value = copyString(o->defaultValue);
		for (int i = 0; i < savedCount; ++i) {
			if (!strcmp(savedKeys[i], o->key)) {
				free(o->value);
				o->value = strdup(savedValues[i]);
			}
		}
	}
}

void optionsDisplay(const struct retro_core_option_display* display) {
	struct Option* o = display && display->key ? optionFind(display->key) : NULL;
	if (o) {
		o->visible = display->visible;
	}
}

const char* scancodeName(SDL_Scancode code) {
	const char* name = code ? SDL_GetScancodeName(code) : "";
	return *name ? name : "none";
}

const char* padName(int binding) {
	if (binding == PAD_NONE) {
		return "none";
	}
	if (binding == PAD_LEFT_TRIGGER) {
		return "left trigger";
	}
	if (binding == PAD_RIGHT_TRIGGER) {
		return "right trigger";
	}
	const char* name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton) binding);
	return name ? name : "?";
}
