/* Copyright (c) 2026 Magnominus
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
#include <sys/stat.h>

struct Settings settings;
struct Option options[OPTIONS_MAX];
int optionCount;
struct Category categories[32];
int categoryCount;
bool optionsChanged;

const char* actionNames[ACTIONS] = {
	"Menu", "Fast-Forward", "Rewind", "Pause", "Save State", "Load State", "Previous Slot", "Next Slot",
	"Screenshot", "Record", "Full Screen", "Restart", "Quit",
};

const char* buttonNames[BUTTONS] = {
	"B", "Turbo B", "Select", "Start", "Up", "Down", "Left", "Right", "A", "Turbo A", "L", "R", "L2", "R2", "L3", "R3",
};

// Values read from the files before the core names its options.
static char* savedKeys[OPTIONS_MAX];
static char* savedValues[OPTIONS_MAX];
static int savedCount;

// The settings before the file is read, and what Reset Controls goes back to.
void settingsDefaults(void) {
	memset(&settings, 0, sizeof(settings));
	settings.scale = 3;
	// Full screen unless on a desktop, which names itself; a handheld's session doesn't.
	settings.fullscreen = !getenv("XDG_CURRENT_DESKTOP");
	settings.vsync = true;
	settings.filter = FILTER_SHARP;
	settings.lcdOnGpu = true;
	settings.showMessages = true;
	settings.shadow = 4;
	settings.volume = 100;
	settings.latency = 3;
	settings.rewindMegabytes = 64;
	settings.netPort = 55437;
	settings.inputDelay = 1;
	settings.menuSize = 120;
	settings.dim = 1;
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
		SDL_SCANCODE_UNKNOWN,
	};
	memcpy(settings.keys, keys, sizeof(keys));
	memcpy(settings.pad, pad, sizeof(pad));
	memcpy(settings.hotkeys, hotkeys, sizeof(hotkeys));
	// Home + North for the menu and Home + Start to quit, as on ROCKNIX.
	memset(settings.padHotkeys, 0, sizeof(settings.padHotkeys));
	settings.padHotkeys[ACT_MENU] = padBit(SDL_CONTROLLER_BUTTON_GUIDE) | padBit(SDL_CONTROLLER_BUTTON_Y);
	settings.padHotkeys[ACT_QUIT] = padBit(SDL_CONTROLLER_BUTTON_GUIDE) | padBit(SDL_CONTROLLER_BUTTON_START);
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

// A text setting, cut to fit.
static void copyText(char* out, size_t size, const char* value) {
	snprintf(out, size, "%s", value);
}

// One line of frontend.cfg. Numbers out of range take the nearest allowed, or the default; keys it
// doesn't know are passed over.
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
		settings.filter = n >= 0 && n < FILTERS ? n : FILTER_SHARP;
		// The GPU LCD grid was once the third filter.
		if (n == 2) {
			settings.gpuGrid = true;
		}
	} else if (!strcmp(key, "lcd_rendering")) {
		settings.lcdOnGpu = strcmp(value, "cpu") != 0;
	} else if (!strcmp(key, "gpu_lcd_grid")) {
		settings.gpuGrid = yes;
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
	} else if (!strcmp(key, "ui_accent")) {
		settings.accent = n >= 0 && n < ACCENTS ? n : 0;
	} else if (!strcmp(key, "ui_menu_size")) {
		settings.menuSize = n < 100 ? 100 : n > 160 ? 160 : n;
	} else if (!strcmp(key, "ui_size")) {
		// Before the sizes moved up a step: the same name's size now.
		settings.menuSize = n < 100 ? 100 : n < 120 ? 120 : n < 140 ? 140 : 160;
	} else if (!strcmp(key, "ui_dim")) {
		settings.dim = n >= 0 && n <= 2 ? n : 1;
	} else if (!strcmp(key, "directory")) {
		copyText(settings.directory, sizeof(settings.directory), value);
	} else if (!strcmp(key, "bios_folder")) {
		copyText(settings.biosFolder, sizeof(settings.biosFolder), strcmp(value, "auto") ? value : "");
	} else if (!strcmp(key, "save_folder")) {
		copyText(settings.saveFolder, sizeof(settings.saveFolder), strcmp(value, "auto") ? value : "");
	} else if (!strcmp(key, "state_folder")) {
		copyText(settings.stateFolder, sizeof(settings.stateFolder), strcmp(value, "armgba") ? value : "");
	} else if (!strcmp(key, "screenshot_folder")) {
		copyText(settings.shotFolder, sizeof(settings.shotFolder), strcmp(value, "armgba") ? value : "");
	} else if (!strcmp(key, "fast_forward")) {
		settings.fastForward = n >= 2 && n <= 12 ? n : 0;
	} else if (!strcmp(key, "show_messages")) {
		settings.showMessages = yes;
	} else if (!strncmp(key, "key_", 4) || !strncmp(key, "pad_", 4) || !strncmp(key, "hotkey_", 7) || !strncmp(key, "padhotkey_", 10) ||
	           !strncmp(key, "padcombo_", 9)) {
		int const index = atoi(strrchr(key, '_') + 1);
		if (!strncmp(key, "key_", 4) && index >= 0 && index < BUTTONS) {
			settings.keys[index] = (SDL_Scancode) n;
		} else if (!strncmp(key, "pad_", 4) && index >= 0 && index < BUTTONS) {
			settings.pad[index] = n;
		} else if (!strncmp(key, "hotkey_", 7) && index >= 0 && index < ACTIONS) {
			settings.hotkeys[index] = (SDL_Scancode) n;
		} else if (!strncmp(key, "padhotkey_", 10) && index >= 0 && index < ACTIONS) {
			// Before hotkeys were buttons held together: one button, the menu's on Home alone then.
			if (index != ACT_MENU || n != SDL_CONTROLLER_BUTTON_GUIDE) {
				settings.padHotkeys[index] = padBit(n);
			}
		} else if (!strncmp(key, "padcombo_", 9) && index >= 0 && index < ACTIONS) {
			settings.padHotkeys[index] = (uint32_t) strtoul(value, NULL, 10);
		}
	}
}

// frontend.cfg, over the defaults.
void settingsRead(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/frontend.cfg", configDirectory);
	readLines(path, takeSetting);
}

// Every setting to frontend.cfg, as the menu changes one.
void settingsWrite(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/frontend.cfg", configDirectory);
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	// It holds the RetroAchievements token: for this user's eyes only.
	fchmod(fileno(f), 0600);
	const char* const no[2] = { "false", "true" };
	fprintf(f, "# armgba's own settings; the menu changes them. Keys and buttons are SDL's numbers.\n");
	fprintf(f, "scale = %d\nfullscreen = %s\ninteger_scale = %s\nstretch = %s\nvsync = %s\nfilter = %d\nshow_fps = %s\n", settings.scale,
	        no[settings.fullscreen], no[settings.integerScale], no[settings.stretch], no[settings.vsync], settings.filter, no[settings.showFps]);
	fprintf(f, "lcd_rendering = %s\ngpu_lcd_grid = %s\nlcd_shadow = %d\n", settings.lcdOnGpu ? "gpu" : "cpu", no[settings.gpuGrid],
	        settings.shadow);
	fprintf(f, "volume = %d\nlatency = %d\nrunahead = %d\nrewind = %s\nrewind_megabytes = %d\nauto_state = %s\n", settings.volume,
	        settings.latency, settings.runahead, no[settings.rewind], settings.rewindMegabytes, no[settings.autoState]);
	fprintf(f, "nickname = \"%s\"\nnetplay_host = \"%s\"\nnetplay_port = %d\ninput_delay = %d\n", settings.nickname, settings.host,
	        settings.netPort, settings.inputDelay);
	fprintf(f, "cheevos_user = \"%s\"\ncheevos_token = \"%s\"\nhardcore = %s\n", settings.cheevosUser, settings.cheevosToken,
	        no[settings.hardcore]);
	fprintf(f, "ui_accent = %d\nui_menu_size = %d\nui_dim = %d\n", settings.accent, settings.menuSize, settings.dim);
	fprintf(f, "directory = \"%s\"\nbios_folder = \"%s\"\nsave_folder = \"%s\"\nstate_folder = \"%s\"\nscreenshot_folder = \"%s\"\n",
	        settings.directory, settings.biosFolder[0] ? settings.biosFolder : "auto", settings.saveFolder[0] ? settings.saveFolder : "auto",
	        settings.stateFolder[0] ? settings.stateFolder : "armgba", settings.shotFolder[0] ? settings.shotFolder : "armgba");
	fprintf(f, "fast_forward = %d\nshow_messages = %s\n", settings.fastForward, no[settings.showMessages]);
	for (int i = 0; i < BUTTONS; ++i) {
		fprintf(f, "key_%d = %d\npad_%d = %d\n", i, settings.keys[i], i, settings.pad[i]);
	}
	for (int i = 0; i < ACTIONS; ++i) {
		fprintf(f, "hotkey_%d = %d\npadcombo_%d = %u\n", i, settings.hotkeys[i], i, (unsigned) settings.padHotkeys[i]);
	}
	fclose(f);
}

// The core's options.

// One line of armgba.cfg, kept until the core names its options.
static void takeSaved(const char* key, const char* value) {
	if (savedCount < OPTIONS_MAX) {
		savedKeys[savedCount] = strdup(key);
		savedValues[savedCount] = strdup(value);
		++savedCount;
	}
}

// armgba.cfg's values, read before the core starts.
void optionsReadSaved(void) {
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s/armgba.cfg", configDirectory);
	readLines(path, takeSaved);
}

// The core's options to armgba.cfg, each with its name, values and default in a comment above it.
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
	fprintf(f, "# armGBA's options: the menu's Core tab changes them, or edit each line's value, one of those\n# listed above it.\n\n");
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

// A copy of the core's text, which it may not keep; NULL stays NULL.
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

// A core option's value; the core is told at its next frame.
void optionSet(struct Option* option, const char* value) {
	if (strcmp(option->value, value)) {
		free(option->value);
		option->value = strdup(value);
		optionsChanged = true;
	}
}

// The core's options and their categories, as it defines them, each taking its value from armgba.cfg.
// An option defined again keeps its first definition.
void optionsDefine(const struct retro_core_options_v2* definitions) {
	for (const struct retro_core_option_v2_category* c = definitions->categories; c && c->key && categoryCount < 32; ++c) {
		bool known = false;
		for (int i = 0; i < categoryCount; ++i) {
			known |= !strcmp(categories[i].key, c->key);
		}
		if (!known) {
			categories[categoryCount].key = strdup(c->key);
			categories[categoryCount].description = copyString(c->desc);
			categories[categoryCount].info = copyString(c->info);
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
		o->info = copyString(d->info);
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

// The core shows or hides an option as others change.
void optionsDisplay(const struct retro_core_option_display* display) {
	struct Option* o = display && display->key ? optionFind(display->key) : NULL;
	if (o) {
		o->visible = display->visible;
	}
}

// A key's name, or None for none.
const char* scancodeName(SDL_Scancode code) {
	const char* name = code ? SDL_GetScancodeName(code) : "";
	return *name ? name : "None";
}

// A controller button by where it is, which every layout agrees on: the face buttons' letters differ
// between makers.
const char* padName(int binding) {
	static const char* const names[SDL_CONTROLLER_BUTTON_MAX] = {
		[SDL_CONTROLLER_BUTTON_A] = "South",
		[SDL_CONTROLLER_BUTTON_B] = "East",
		[SDL_CONTROLLER_BUTTON_X] = "West",
		[SDL_CONTROLLER_BUTTON_Y] = "North",
		[SDL_CONTROLLER_BUTTON_BACK] = "Select",
		[SDL_CONTROLLER_BUTTON_GUIDE] = "Home",
		[SDL_CONTROLLER_BUTTON_START] = "Start",
		[SDL_CONTROLLER_BUTTON_LEFTSTICK] = "Left Stick Press",
		[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "Right Stick Press",
		[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "Left Shoulder",
		[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "Right Shoulder",
		[SDL_CONTROLLER_BUTTON_DPAD_UP] = "D-Pad Up",
		[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "D-Pad Down",
		[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "D-Pad Left",
		[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "D-Pad Right",
	};
	if (binding == PAD_NONE) {
		return "None";
	}
	if (binding == PAD_LEFT_TRIGGER) {
		return "Left Trigger";
	}
	if (binding == PAD_RIGHT_TRIGGER) {
		return "Right Trigger";
	}
	if (binding >= 0 && binding < SDL_CONTROLLER_BUTTON_MAX && names[binding]) {
		return names[binding];
	}
	const char* name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton) binding);
	return name ? name : "?";
}

// A button's bit in a hotkey: the buttons' own numbers, the triggers the top two.
uint32_t padBit(int binding) {
	if (binding == PAD_LEFT_TRIGGER || binding == PAD_RIGHT_TRIGGER) {
		return 1u << (binding == PAD_LEFT_TRIGGER ? 30 : 31);
	}
	return binding >= 0 && binding < SDL_CONTROLLER_BUTTON_MAX ? 1u << binding : 0;
}

// A hotkey's buttons, Home, Select and Start first: Home + North.
const char* padComboName(uint32_t combo) {
	enum { FIRST = 3 };
	static const int first[FIRST] = { SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START };
	static char name[160];
	name[0] = '\0';
	uint32_t left = combo;
	for (int k = 0; left && k < FIRST + 32; ++k) {
		int const b = k < FIRST ? first[k] : k - FIRST;
		int const binding = b == 30 ? PAD_LEFT_TRIGGER : b == 31 ? PAD_RIGHT_TRIGGER : b;
		if (left & padBit(binding)) {
			left &= ~padBit(binding);
			snprintf(name + strlen(name), sizeof(name) - strlen(name), "%s%s", name[0] ? " + " : "", padName(binding));
		}
	}
	return name[0] ? name : "None";
}

// Recent games.

static void recentPath(char out[FILE_SIZE]) {
	snprintf(out, FILE_SIZE, "%s/recent.txt", configDirectory);
}

// The recent games, newest first, up to max.
int recentList(char (*out)[PATH_SIZE], int max) {
	char path[FILE_SIZE];
	recentPath(path);
	FILE* f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	int n = 0;
	while (n < max && fgets(out[n], PATH_SIZE, f)) {
		out[n][strcspn(out[n], "\r\n")] = '\0';
		n += out[n][0] != '\0';
	}
	fclose(f);
	return n;
}

// Puts game first in the recent games, once.
void recentAdd(const char* game) {
	static char kept[RECENT_MAX][PATH_SIZE];
	int const n = recentList(kept, RECENT_MAX);
	char path[FILE_SIZE];
	recentPath(path);
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "%s\n", game);
	for (int i = 0, written = 1; i < n && written < RECENT_MAX; ++i) {
		if (strcmp(kept[i], game)) {
			fprintf(f, "%s\n", kept[i]);
			++written;
		}
	}
	fclose(f);
}
