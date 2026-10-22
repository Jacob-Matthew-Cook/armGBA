/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The menus, messages and on-screen keyboard, drawn over the game. Every menu is built again each
// frame from the settings it shows, so it is never out of date. The keyboard, a controller and the
// mouse all work: arrows or the D-pad or stick move, Enter or A or a click chooses, left and right
// change a value, Esc or B or a right-click goes back.

#include "frontend.h"

#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "text.h"

enum {
	ITEMS_MAX = 512,
	DEPTH_MAX = 8,
	MESSAGES = 4,
	MESSAGE_MS = 3000,
	// Holding a direction on a controller repeats it after this long, then this often.
	REPEAT_DELAY_MS = 400,
	REPEAT_MS = 90,
	CIRCLES = 16,
};

// An item calls its function with 0 when chosen, and with -1 or 1 when its value is changed.
typedef void (*ItemFunction)(int argument, int delta);

struct Item {
	char label[NAME_SIZE + 64];
	char value[NAME_SIZE + 64];
	ItemFunction function;
	int argument;
	bool header;
	bool adjustable;
};

struct Menu {
	void (*build)(void);
	char title[NAME_SIZE + 64];
	int selected;
	int scroll;
};

static struct Item* items;
static int itemCapacity;
static int itemCount;
static struct Menu stack[DEPTH_MAX];
static int depth;
// Where each item was last drawn, for the mouse.
static SDL_Rect itemRects[ITEMS_MAX];
static int firstShown, shownCount;
// Where a selected value's arrows were drawn, or -1.
static int arrowLeft[ITEMS_MAX], arrowRight[ITEMS_MAX];
// The footer shows a controller's buttons once one is used, and keys once the keyboard or mouse is.
static bool lastInputPad;

static struct {
	char text[160];
	Uint32 until;
} messages[MESSAGES];

// The on-screen keyboard.
static struct {
	bool open;
	char title[128];
	char text[256];
	bool hidden;
	void (*done)(const char* text);
	int row, column;
	bool shift;
} entry;
static const char* const keyRows[2][4] = {
	{ "1234567890-=", "qwertyuiop[]", "asdfghjkl;'/", "zxcvbnm,.:@_" },
	{ "!\"#$%^&*()+~", "QWERTYUIOP{}", "ASDFGHJKL|\\?", "ZXCVBNM<>`$ " },
};
// The keyboard's last row: these, each a few keys wide.
static const char* const keySpecials[] = { "Shift", "Space", "Delete", "Done" };

// Waiting for a key or button to bind.
static struct {
	bool open;
	int which;
	bool controller;
	bool hotkey;
} capture;

// A controller's held direction, for repeats.
static struct {
	int dx, dy;
	Uint32 next;
} held;

static char browserPath[PATH_SIZE];

void osd(const char* format, ...) {
	for (int i = MESSAGES - 1; i > 0; --i) {
		messages[i] = messages[i - 1];
	}
	va_list args;
	va_start(args, format);
	vsnprintf(messages[0].text, sizeof(messages[0].text), format, args);
	va_end(args);
	messages[0].until = SDL_GetTicks() + MESSAGE_MS;
	fprintf(stderr, "armgba: %s\n", messages[0].text);
}

// Shapes: rounded rectangles are a circle's quarters at the corners and plain rectangles between.

static struct {
	int radius;
	SDL_Texture* texture;
} circles[CIRCLES];
static int nextCircle;

void uiRecreateTextures(void) {
	textReset();
	for (int i = 0; i < CIRCLES; ++i) {
		if (circles[i].texture) {
			SDL_DestroyTexture(circles[i].texture);
		}
		circles[i].texture = NULL;
	}
}

bool uiInit(void) {
	snprintf(browserPath, sizeof(browserPath), "%s", settings.directory);
	return textInit();
}

static SDL_Texture* circle(int radius) {
	for (int i = 0; i < CIRCLES; ++i) {
		if (circles[i].texture && circles[i].radius == radius) {
			return circles[i].texture;
		}
	}
	int const size = radius * 2;
	uint32_t* pixels = malloc((size_t) size * size * 4);
	if (!pixels) {
		return NULL;
	}
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			float const dx = x + 0.5f - radius, dy = y + 0.5f - radius;
			float const cover = SDL_max(0.0f, SDL_min(1.0f, radius - SDL_sqrtf(dx * dx + dy * dy) + 0.5f));
			pixels[y * size + x] = (uint32_t) (cover * 255 + 0.5f) << 24 | 0xFFFFFF;
		}
	}
	int const slot = nextCircle++ % CIRCLES;
	if (circles[slot].texture) {
		SDL_DestroyTexture(circles[slot].texture);
	}
	circles[slot].radius = radius;
	circles[slot].texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
	SDL_UpdateTexture(circles[slot].texture, NULL, pixels, size * 4);
	SDL_SetTextureBlendMode(circles[slot].texture, SDL_BLENDMODE_BLEND);
	free(pixels);
	return circles[slot].texture;
}

static void fillRect(int x, int y, int w, int h, SDL_Color colour) {
	SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(app.renderer, colour.r, colour.g, colour.b, colour.a);
	SDL_Rect const r = { x, y, w, h };
	SDL_RenderFillRect(app.renderer, &r);
}

static void fillRounded(SDL_Rect r, int radius, SDL_Color colour) {
	radius = SDL_min(radius, SDL_min(r.w, r.h) / 2);
	SDL_Texture* texture = radius > 0 ? circle(radius) : NULL;
	if (!texture) {
		fillRect(r.x, r.y, r.w, r.h, colour);
		return;
	}
	SDL_SetTextureColorMod(texture, colour.r, colour.g, colour.b);
	SDL_SetTextureAlphaMod(texture, colour.a);
	for (int i = 0; i < 4; ++i) {
		int const right = i & 1, bottom = i >> 1;
		SDL_Rect const from = { right * radius, bottom * radius, radius, radius };
		SDL_Rect const to = { right ? r.x + r.w - radius : r.x, bottom ? r.y + r.h - radius : r.y, radius, radius };
		SDL_RenderCopy(app.renderer, texture, &from, &to);
	}
	fillRect(r.x + radius, r.y, r.w - 2 * radius, r.h, colour);
	fillRect(r.x, r.y + radius, radius, r.h - 2 * radius, colour);
	fillRect(r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius, colour);
}

// A rounded rectangle with an edge of another colour.
static void fillBordered(SDL_Rect r, int radius, SDL_Color fill, SDL_Color edge, int edgeWidth) {
	fillRounded(r, radius, edge);
	fillRounded((SDL_Rect) { r.x + edgeWidth, r.y + edgeWidth, r.w - 2 * edgeWidth, r.h - 2 * edgeWidth }, radius - edgeWidth, fill);
}

// The menus' building blocks.

static struct Item* addItem(const char* label, ItemFunction function, int argument) {
	static struct Item spare;
	if (itemCount == itemCapacity) {
		int const capacity = itemCapacity ? itemCapacity * 2 : 64;
		struct Item* const grown = realloc(items, (size_t) capacity * sizeof(*grown));
		if (!grown) {
			return &spare;
		}
		items = grown;
		itemCapacity = capacity;
	}
	struct Item* item = &items[itemCount++];
	memset(item, 0, sizeof(*item));
	snprintf(item->label, sizeof(item->label), "%s", label);
	item->function = function;
	item->argument = argument;
	return item;
}

static void addHeader(const char* label) {
	addItem(label, NULL, 0)->header = true;
}

static void addValue(const char* label, const char* value, ItemFunction function, int argument) {
	struct Item* item = addItem(label, function, argument);
	snprintf(item->value, sizeof(item->value), "%s", value);
	item->adjustable = true;
}

// A value that is chosen, not stepped through.
static void addChoice(const char* label, const char* value, ItemFunction function, int argument) {
	struct Item* item = addItem(label, function, argument);
	snprintf(item->value, sizeof(item->value), "%s", value);
}

static void addInfo(const char* label, const char* value) {
	struct Item* item = addItem(label, NULL, 0);
	snprintf(item->value, sizeof(item->value), "%s", value ? value : "");
}

static void pushMenu(void (*build)(void), const char* title) {
	if (depth < DEPTH_MAX) {
		stack[depth].build = build;
		snprintf(stack[depth].title, sizeof(stack[depth].title), "%s", title);
		stack[depth].selected = -1;
		stack[depth].scroll = 0;
		++depth;
	}
}

static void popMenu(void) {
	if (depth > 1) {
		--depth;
	} else {
		uiCloseMenu();
	}
}

static void openEntry(const char* title, const char* text, bool hidden, void (*done)(const char* text)) {
	entry.open = true;
	snprintf(entry.title, sizeof(entry.title), "%s", title);
	snprintf(entry.text, sizeof(entry.text), "%s", text ? text : "");
	entry.hidden = hidden;
	entry.done = done;
	entry.row = entry.column = 0;
	entry.shift = false;
	SDL_StartTextInput();
}

static void closeEntry(bool accept) {
	entry.open = false;
	SDL_StopTextInput();
	if (accept && entry.done) {
		entry.done(entry.text);
	}
}

static const char* onOff(bool on) {
	return on ? "On" : "Off";
}

static const char* frames(int count) {
	return count == 1 ? "frame" : "frames";
}

static int wrap(int value, int delta, int count) {
	return ((value + delta) % count + count) % count;
}

// The menus.

static void rootMenu(void);

static void resume(int argument, int delta) {
	(void) argument;
	if (!delta) {
		uiCloseMenu();
	}
}

static void stateItem(int load, int delta) {
	if (delta) {
		app.slot = wrap(app.slot, delta, SLOTS);
		return;
	}
	if (cheevosHardcore() || netRunning()) {
		osd(cheevosHardcore() ? "Hardcore mode: no save states" : "Not in netplay");
		return;
	}
	if (load) {
		osd(loadState(app.slot) ? "Loaded state %d" : "No state %d", app.slot);
	} else {
		osd(saveState(app.slot) ? "Saved state %d" : "State %d not saved", app.slot);
	}
	uiCloseMenu();
}

static void restart(int argument, int delta) {
	(void) argument;
	if (!delta && !netRunning()) {
		retro_reset();
		cheevosReset();
		uiCloseMenu();
	}
}

static bool gameFile(const char* name) {
	static const char* const extensions[] = { ".gba", ".gb", ".gbc", ".zip", ".7z", ".gba2", ".gb2", ".bin" };
	const char* dot = strrchr(name, '.');
	for (size_t i = 0; dot && i < sizeof(extensions) / sizeof(*extensions); ++i) {
		if (!strcasecmp(dot, extensions[i])) {
			return true;
		}
	}
	return false;
}

// The game list: the folders, then the games, in browserPath, read once as the folder is entered.
struct BrowserEntry {
	char name[NAME_SIZE];
	bool directory;
};
static struct BrowserEntry* browserEntries;
static int browserCount, browserCapacity;
// The folder browserEntries lists; emptied to read it again.
static char browserListed[PATH_SIZE];

static int compareEntries(const void* a, const void* b) {
	const struct BrowserEntry* const x = a;
	const struct BrowserEntry* const y = b;
	if (x->directory != y->directory) {
		return x->directory ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

static void readBrowser(void) {
	if (!strcmp(browserListed, browserPath)) {
		return;
	}
	snprintf(browserListed, sizeof(browserListed), "%s", browserPath);
	browserCount = 0;
	DIR* dir = opendir(browserPath);
	if (!dir) {
		return;
	}
	struct dirent* e;
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		bool directory = e->d_type == DT_DIR;
		if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
			char path[FILE_SIZE];
			snprintf(path, sizeof(path), "%s/%s", browserPath, e->d_name);
			struct stat st;
			if (stat(path, &st)) {
				continue;
			}
			directory = S_ISDIR(st.st_mode);
		}
		if (!directory && !gameFile(e->d_name)) {
			continue;
		}
		if (browserCount == browserCapacity) {
			int const capacity = browserCapacity ? browserCapacity * 2 : 256;
			struct BrowserEntry* const grown = realloc(browserEntries, (size_t) capacity * sizeof(*grown));
			if (!grown) {
				break;
			}
			browserEntries = grown;
			browserCapacity = capacity;
		}
		snprintf(browserEntries[browserCount].name, NAME_SIZE, "%s", e->d_name);
		browserEntries[browserCount++].directory = directory;
	}
	closedir(dir);
	qsort(browserEntries, (size_t) browserCount, sizeof(*browserEntries), compareEntries);
}

static void browse(int index, int delta);

static void browserMenu(void) {
	addInfo(browserPath, "");
	addItem("..", browse, -1);
	readBrowser();
	if (!browserCount && access(browserPath, R_OK)) {
		addInfo("Can't open this folder", "");
		return;
	}
	for (int i = 0; i < browserCount; ++i) {
		if (browserEntries[i].directory) {
			char label[NAME_SIZE + 2];
			snprintf(label, sizeof(label), "%s/", browserEntries[i].name);
			addItem(label, browse, i);
		} else {
			addItem(browserEntries[i].name, browse, i);
		}
	}
}

static void browse(int index, int delta) {
	if (delta) {
		return;
	}
	if (index < 0) {
		char* slash = strrchr(browserPath, '/');
		if (slash && slash != browserPath) {
			*slash = '\0';
		} else {
			snprintf(browserPath, sizeof(browserPath), "/");
		}
		stack[depth - 1].selected = -1;
		return;
	}
	char path[FILE_SIZE];
	snprintf(path, sizeof(path), "%s%s%s", browserPath, strcmp(browserPath, "/") ? "/" : "", browserEntries[index].name);
	if (browserEntries[index].directory) {
		snprintf(browserPath, sizeof(browserPath), "%s", path);
		stack[depth - 1].selected = -1;
	} else if (loadGame(path)) {
		uiCloseMenu();
	}
}

static void openBrowser(int argument, int delta) {
	(void) argument;
	if (!delta) {
		browserListed[0] = '\0';
		pushMenu(browserMenu, "Load Game");
	}
}

static void closeGame(int argument, int delta) {
	(void) argument;
	if (!delta) {
		unloadGame();
		SDL_SetWindowTitle(app.window, "armGBA");
		depth = 0;
		pushMenu(rootMenu, "No game loaded");
	}
}

// Cheats.

static int cheatShown;

static void cheatMenu(void);

static void cheatToggle(int index, int delta) {
	(void) delta;
	if (index < cheatCount) {
		cheats[index].enabled = !cheats[index].enabled;
		cheatListApply();
		cheatListSave();
	}
}

static void cheatCodeDone(const char* text) {
	if (cheatShown < cheatCount) {
		snprintf(cheats[cheatShown].code, sizeof(cheats[cheatShown].code), "%s", text);
		cheatListApply();
		cheatListSave();
	}
}

static void cheatNameDone(const char* text) {
	if (cheatShown < cheatCount) {
		snprintf(cheats[cheatShown].description, sizeof(cheats[cheatShown].description), "%s", text);
		cheatListSave();
	}
}

static void cheatEdit(int which, int delta) {
	if (delta || cheatShown >= cheatCount) {
		return;
	}
	if (which == 0) {
		openEntry("Cheat code (Action Replay, GameShark, CodeBreaker, Game Genie)", cheats[cheatShown].code, false, cheatCodeDone);
	} else if (which == 1) {
		openEntry("Cheat name", cheats[cheatShown].description, false, cheatNameDone);
	} else {
		cheatListRemove(cheatShown);
		cheatListApply();
		cheatListSave();
		popMenu();
	}
}

static void cheatMenu(void) {
	if (cheatShown >= cheatCount) {
		addInfo("This cheat was deleted", "");
		return;
	}
	struct Cheat* c = &cheats[cheatShown];
	addValue("Enabled", onOff(c->enabled), cheatToggle, cheatShown);
	addChoice("Code", c->code, cheatEdit, 0);
	addChoice("Name", c->description, cheatEdit, 1);
	addItem("Delete", cheatEdit, 2);
}

static void cheatOpen(int index, int delta) {
	if (delta) {
		cheatToggle(index, delta);
		return;
	}
	cheatShown = index;
	pushMenu(cheatMenu, cheats[index].description);
}

static char newCheatName[128];

static void newCheatCode(const char* text) {
	cheatListAdd(newCheatName, text);
	cheatListApply();
	cheatListSave();
}

static void newCheatNamed(const char* text) {
	snprintf(newCheatName, sizeof(newCheatName), "%s", *text ? text : "Cheat");
	openEntry("Cheat code (several lines' codes may be joined with +)", "", false, newCheatCode);
}

static void cheatNew(int argument, int delta) {
	(void) argument;
	if (!delta) {
		openEntry("New cheat's name", "", false, newCheatNamed);
	}
}

static void cheatsMenu(void) {
	if (cheevosHardcore()) {
		addInfo("Hardcore mode: cheats are off", "");
		return;
	}
	addItem("Add Cheat...", cheatNew, 0);
	if (cheatCount) {
		addHeader("Cheats (left / right turns one on or off)");
	}
	for (int i = 0; i < cheatCount; ++i) {
		addValue(cheats[i].description, onOff(cheats[i].enabled), cheatOpen, i);
	}
}

static void openSimple(void (*build)(void), const char* title) {
	pushMenu(build, title);
}

static void openCheats(int argument, int delta) {
	(void) argument;
	if (!delta) {
		openSimple(cheatsMenu, "Cheats");
	}
}

// Core options.

static int categoryShown;

static void optionChange(int index, int delta) {
	struct Option* o = &options[index];
	int at = 0;
	for (int v = 0; v < o->valueCount; ++v) {
		if (!strcmp(o->values[v], o->value)) {
			at = v;
		}
	}
	optionSet(o, o->values[wrap(at, delta ? delta : 1, o->valueCount)]);
	optionsWrite();
}

static void addOptions(const char* category) {
	for (int i = 0; i < optionCount; ++i) {
		struct Option* o = &options[i];
		bool const here = category ? o->category && !strcmp(o->category, category) : !o->category;
		if (!here || !o->visible) {
			continue;
		}
		const char* label = o->value;
		for (int v = 0; v < o->valueCount; ++v) {
			if (!strcmp(o->values[v], o->value)) {
				label = o->labels[v];
			}
		}
		addValue(o->description ? o->description : o->key, label, optionChange, i);
	}
}

static void categoryMenu(void) {
	addOptions(categories[categoryShown].key);
}

static void openCategory(int index, int delta) {
	if (!delta) {
		categoryShown = index;
		pushMenu(categoryMenu, categories[index].description ? categories[index].description : categories[index].key);
	}
}

static void coreOptionsMenu(void) {
	if (!optionCount) {
		addInfo("The core's options appear once it has started", "");
		return;
	}
	for (int c = 0; c < categoryCount; ++c) {
		bool any = false;
		for (int i = 0; i < optionCount; ++i) {
			any |= options[i].visible && options[i].category && !strcmp(options[i].category, categories[c].key);
		}
		if (any) {
			char label[160];
			snprintf(label, sizeof(label), "%s...", categories[c].description ? categories[c].description : categories[c].key);
			addItem(label, openCategory, c);
		}
	}
	addOptions(NULL);
}

static void openCoreOptions(int argument, int delta) {
	(void) argument;
	if (!delta) {
		pushMenu(coreOptionsMenu, "Core Options");
	}
}

// Settings.

enum {
	SET_SCALE,
	SET_FULLSCREEN,
	SET_INTEGER,
	SET_STRETCH,
	SET_FILTER,
	SET_SHADOW,
	SET_VSYNC,
	SET_FPS,
	SET_VOLUME,
	SET_LATENCY,
	SET_REWIND,
	SET_REWIND_SIZE,
	SET_RUNAHEAD,
	SET_AUTO_STATE,
	SET_INPUT_DELAY,
	SET_PORT,
	SET_HARDCORE,
};

static void changeSetting(int which, int delta) {
	int const d = delta ? delta : 1;
	switch (which) {
	case SET_SCALE:
		settings.scale = wrap(settings.scale - 1, d, 8) + 1;
		applyWindowSize();
		break;
	case SET_FULLSCREEN:
		settings.fullscreen = !settings.fullscreen;
		applyWindowSize();
		break;
	case SET_INTEGER:
		settings.integerScale = !settings.integerScale;
		break;
	case SET_STRETCH:
		settings.stretch = !settings.stretch;
		break;
	case SET_FILTER:
		settings.filter = wrap(settings.filter, d, FILTERS);
		break;
	case SET_SHADOW:
		settings.shadow = wrap(settings.shadow, d, 9);
		break;
	case SET_VSYNC:
		settings.vsync = !settings.vsync;
		recreateRenderer();
		break;
	case SET_FPS:
		settings.showFps = !settings.showFps;
		break;
	case SET_VOLUME:
		settings.volume = SDL_max(0, SDL_min(100, settings.volume + d * 10));
		break;
	case SET_LATENCY:
		settings.latency = wrap(settings.latency - 1, d, 8) + 1;
		break;
	case SET_REWIND:
		settings.rewind = !settings.rewind;
		rewindReset();
		break;
	case SET_REWIND_SIZE: {
		static const int sizes[] = { 16, 32, 64, 128, 256, 512 };
		int at = 0;
		for (int i = 0; i < 6; ++i) {
			at = sizes[i] == settings.rewindMegabytes ? i : at;
		}
		settings.rewindMegabytes = sizes[wrap(at, d, 6)];
		rewindReset();
		break;
	}
	case SET_RUNAHEAD:
		settings.runahead = wrap(settings.runahead, d, 5);
		break;
	case SET_AUTO_STATE:
		settings.autoState = !settings.autoState;
		break;
	case SET_INPUT_DELAY:
		settings.inputDelay = wrap(settings.inputDelay, d, 9);
		break;
	case SET_PORT:
		settings.netPort = settings.netPort + d;
		break;
	case SET_HARDCORE:
		settings.hardcore = !settings.hardcore;
		cheevosSetHardcore(settings.hardcore);
		break;
	}
	settingsWrite();
}

static void videoMenu(void) {
	static const char* const filters[FILTERS] = { "Sharp", "Smooth", "LCD Grid", "Scanlines" };
	char text[32];
	snprintf(text, sizeof(text), "%dx", settings.scale);
	addValue("Window Size", text, changeSetting, SET_SCALE);
	addValue("Full Screen", onOff(settings.fullscreen), changeSetting, SET_FULLSCREEN);
	addValue("Integer Scaling", onOff(settings.integerScale), changeSetting, SET_INTEGER);
	addValue("Aspect Ratio", settings.stretch ? "Fill the Window" : "The Console's", changeSetting, SET_STRETCH);
	addValue("Filter", filters[settings.filter], changeSetting, SET_FILTER);
	if (settings.filter == FILTER_LCD) {
		static const char* const shadows[9] = { "Off", "Up", "Up Right", "Right", "Down Right", "Down", "Down Left", "Left", "Up Left" };
		addValue("Game Boy Pixel Shadow", shadows[settings.shadow], changeSetting, SET_SHADOW);
	}
	addValue("VSync", onOff(settings.vsync), changeSetting, SET_VSYNC);
	addValue("Show Frame Rate", onOff(settings.showFps), changeSetting, SET_FPS);
}

static void audioMenu(void) {
	char text[32];
	snprintf(text, sizeof(text), "%d%%", settings.volume);
	addValue("Volume", text, changeSetting, SET_VOLUME);
	snprintf(text, sizeof(text), "%d %s (%d ms)", settings.latency, frames(settings.latency), (int) (settings.latency * 1000 / FRAME_RATE));
	addValue("Latency", text, changeSetting, SET_LATENCY);
}

static void gameplayMenu(void) {
	char text[32];
	addValue("Rewind", onOff(settings.rewind), changeSetting, SET_REWIND);
	snprintf(text, sizeof(text), "%d MB", settings.rewindMegabytes);
	addValue("Rewind Buffer", text, changeSetting, SET_REWIND_SIZE);
	snprintf(text, sizeof(text), settings.runahead ? "%d %s" : "Off", settings.runahead, frames(settings.runahead));
	addValue("Run-Ahead", text, changeSetting, SET_RUNAHEAD);
	addValue("Save and Load State on Exit and Start", onOff(settings.autoState), changeSetting, SET_AUTO_STATE);
}

static void bind(int which, int delta) {
	if (delta) {
		return;
	}
	int const kind = which / 1000, index = which % 1000;
	uiCaptureBinding(index, kind == 1 || kind == 3, kind >= 2);
}

static void resetControls(int argument, int delta) {
	(void) argument;
	if (!delta) {
		struct Settings kept = settings;
		settingsDefaults();
		memcpy(kept.keys, settings.keys, sizeof(kept.keys));
		memcpy(kept.pad, settings.pad, sizeof(kept.pad));
		memcpy(kept.hotkeys, settings.hotkeys, sizeof(kept.hotkeys));
		memcpy(kept.padHotkeys, settings.padHotkeys, sizeof(kept.padHotkeys));
		settings = kept;
		settingsWrite();
		osd("Controls reset");
	}
}

static void controlsMenu(void) {
	addHeader("Keyboard (player 1)");
	for (int i = 0; i < BUTTONS; ++i) {
		addChoice(buttonNames[i], scancodeName(settings.keys[i]), bind, i);
	}
	addHeader("Controllers (one per player)");
	for (int i = 0; i < BUTTONS; ++i) {
		addChoice(buttonNames[i], padName(settings.pad[i]), bind, 1000 + i);
	}
	addItem("Reset Controls and Hotkeys", resetControls, 0);
}

static void hotkeysMenu(void) {
	addHeader("Keyboard");
	for (int i = 0; i < ACTIONS; ++i) {
		addChoice(actionNames[i], scancodeName(settings.hotkeys[i]), bind, 2000 + i);
	}
	addHeader("Controller");
	for (int i = 0; i < ACTIONS; ++i) {
		addChoice(actionNames[i], padName(settings.padHotkeys[i]), bind, 3000 + i);
	}
}

static void openSettingsPage(int which, int delta) {
	static void (*const pages[])(void) = { videoMenu, audioMenu, controlsMenu, hotkeysMenu, gameplayMenu };
	static const char* const titles[] = { "Video", "Audio", "Controls", "Hotkeys", "Rewind, Run-Ahead and States" };
	if (!delta) {
		pushMenu(pages[which], titles[which]);
	}
}

static void settingsMenu(void) {
	addItem("Video...", openSettingsPage, 0);
	addItem("Audio...", openSettingsPage, 1);
	addItem("Controls...", openSettingsPage, 2);
	addItem("Hotkeys...", openSettingsPage, 3);
	addItem("Rewind, Run-Ahead and States...", openSettingsPage, 4);
}

static void openSettings(int argument, int delta) {
	(void) argument;
	if (!delta) {
		pushMenu(settingsMenu, "Settings");
	}
}

// Netplay.

static void nicknameDone(const char* text) {
	snprintf(settings.nickname, sizeof(settings.nickname), "%s", *text ? text : "Player");
	settingsWrite();
}

static void joinDone(const char* text) {
	snprintf(settings.host, sizeof(settings.host), "%s", text);
	settingsWrite();
	if (netJoin(text)) {
		uiCloseMenu();
	}
}

static void netAction(int which, int delta) {
	if (delta) {
		return;
	}
	switch (which) {
	case 0:
		openEntry("Your name", settings.nickname, false, nicknameDone);
		break;
	case 1:
	case 2:
		if (!app.loaded) {
			osd("Load the game to play first");
		} else if (netHost(which == 2)) {
			uiCloseMenu();
		}
		break;
	case 3:
		openEntry("Host's address (name or IP, and :port if not the usual one)", settings.host, false, joinDone);
		break;
	case 4:
		netStop();
		break;
	}
}

static void netplayMenu(void) {
	addInfo("Status", netStatus[0] ? netStatus : "Not connected");
	addChoice("Your Name", settings.nickname, netAction, 0);
	char text[32];
	snprintf(text, sizeof(text), "%d", settings.netPort);
	addValue("Port", text, changeSetting, SET_PORT);
	snprintf(text, sizeof(text), "%d %s", settings.inputDelay, frames(settings.inputDelay));
	addValue("Input Delay (Together)", text, changeSetting, SET_INPUT_DELAY);
	if (netMode == NET_OFF) {
		addHeader("Host the loaded game");
		addItem("Link Cable: each plays their own console (Separate)", netAction, 1);
		addItem("Together: every console runs on every device, with rollback", netAction, 2);
		addHeader("Join");
		addItem("Join a Host...", netAction, 3);
	} else {
		addItem("Disconnect", netAction, 4);
	}
}

static void openNetplay(int argument, int delta) {
	(void) argument;
	if (!delta) {
		pushMenu(netplayMenu, "Netplay");
	}
}

// Achievements.

static char loginUser[64];

static void passwordDone(const char* text) {
	cheevosLogin(loginUser, text);
}

static void userDone(const char* text) {
	snprintf(loginUser, sizeof(loginUser), "%s", text);
	openEntry("RetroAchievements password (sent only to retroachievements.org)", "", true, passwordDone);
}

static void cheevosAction(int which, int delta) {
	if (delta) {
		return;
	}
	if (which == 0) {
		openEntry("RetroAchievements user name", settings.cheevosUser, false, userDone);
	} else {
		cheevosLogout();
	}
}

static void cheevosMenu(void) {
	addInfo("Status", cheevosStatus());
	if (settings.cheevosToken[0]) {
		addItem("Log Out", cheevosAction, 1);
	} else {
		addItem("Log In...", cheevosAction, 0);
	}
	addValue("Hardcore Mode (no states, cheats, rewind or run-ahead)", onOff(settings.hardcore), changeSetting, SET_HARDCORE);
	static char lines[400][160];
	int const n = cheevosList(lines, 400);
	if (n) {
		addHeader("This game's achievements");
	}
	for (int i = 0; i < n; ++i) {
		addInfo(lines[i], "");
	}
}

static void openCheevos(int argument, int delta) {
	(void) argument;
	if (!delta) {
		pushMenu(cheevosMenu, "Achievements");
	}
}

static void misc(int which, int delta) {
	if (delta) {
		return;
	}
	if (which == 0) {
		takeScreenshot();
	} else if (which == 1) {
		recordToggle();
	} else {
		app.running = false;
	}
	uiCloseMenu();
}

static void rootMenu(void) {
	char text[32];
	if (app.loaded) {
		addItem("Resume", resume, 0);
		snprintf(text, sizeof(text), stateExists(app.slot) ? "Slot %d" : "Slot %d \xC2\xB7 Empty", app.slot);
		addValue("Save State", text, stateItem, 0);
		addValue("Load State", text, stateItem, 1);
		addItem("Restart", restart, 0);
	}
	addItem("Load Game...", openBrowser, 0);
	if (app.loaded) {
		addItem("Close Game", closeGame, 0);
		addItem("Cheats...", openCheats, 0);
	}
	addItem("Core Options...", openCoreOptions, 0);
	addItem("Settings...", openSettings, 0);
	addItem("Netplay...", openNetplay, 0);
	addItem("Achievements...", openCheevos, 0);
	if (app.loaded) {
		addItem("Screenshot", misc, 0);
		addItem(recording() ? "Stop Recording" : "Start Recording", misc, 1);
	}
	addItem("Quit", misc, 2);
}

void uiOpenMenu(void) {
	if (app.menuOpen) {
		return;
	}
	app.menuOpen = true;
	depth = 0;
	pushMenu(rootMenu, app.loaded ? app.gameName : "No game loaded");
	snprintf(browserPath, sizeof(browserPath), "%s", settings.directory);
}

void uiCloseMenu(void) {
	if (!app.loaded) {
		// Nothing to go back to: the menu stays.
		depth = 1;
		return;
	}
	app.menuOpen = false;
	depth = 0;
	entry.open = false;
	capture.open = false;
	SDL_StopTextInput();
}

void uiCaptureBinding(int which, bool controller, bool hotkey) {
	capture.open = true;
	capture.which = which;
	capture.controller = controller;
	capture.hotkey = hotkey;
}

static void captured(int value) {
	if (capture.hotkey) {
		if (capture.controller) {
			settings.padHotkeys[capture.which] = value;
		} else {
			settings.hotkeys[capture.which] = (SDL_Scancode) value;
		}
	} else if (capture.controller) {
		settings.pad[capture.which] = value;
	} else {
		settings.keys[capture.which] = (SDL_Scancode) value;
	}
	capture.open = false;
	settingsWrite();
}

// Moving through a menu.

static bool selectable(int i) {
	return i >= 0 && i < itemCount && !items[i].header && items[i].function;
}

static void moveSelection(int delta) {
	struct Menu* m = &stack[depth - 1];
	int i = m->selected;
	for (int tries = 0; tries < itemCount; ++tries) {
		i = wrap(i, delta, itemCount);
		if (selectable(i)) {
			m->selected = i;
			return;
		}
	}
}

static void activate(int delta) {
	struct Menu* m = &stack[depth - 1];
	if (!selectable(m->selected)) {
		return;
	}
	struct Item* item = &items[m->selected];
	if (delta && !item->adjustable) {
		return;
	}
	item->function(item->argument, delta);
}

// The on-screen keyboard: 4 rows of 12 keys, then Shift, Space, Delete and Done.
static void entryType(void) {
	if (entry.row < 4) {
		char const c = keyRows[entry.shift][entry.row][entry.column];
		size_t const length = strlen(entry.text);
		if (length + 1 < sizeof(entry.text)) {
			entry.text[length] = c;
			entry.text[length + 1] = '\0';
		}
		return;
	}
	switch (entry.column / 3) {
	case 0:
		entry.shift = !entry.shift;
		break;
	case 1:
		if (strlen(entry.text) + 1 < sizeof(entry.text)) {
			strcat(entry.text, " ");
		}
		break;
	case 2:
		if (entry.text[0]) {
			entry.text[strlen(entry.text) - 1] = '\0';
		}
		break;
	default:
		closeEntry(true);
		break;
	}
}

static void entryMove(int dx, int dy) {
	entry.row = wrap(entry.row, dy, 5);
	entry.column = wrap(entry.column, dx, 12);
}

// The on-screen keyboard's keys, as drawn last, for the mouse.
static SDL_Rect keyRects[5][12];

static void direction(int dx, int dy) {
	if (entry.open) {
		entryMove(dx, dy);
	} else if (dy) {
		moveSelection(dy);
	} else if (dx) {
		activate(dx);
	}
}

static void confirm(void) {
	if (entry.open) {
		entryType();
	} else {
		activate(0);
	}
}

static void back(void) {
	if (entry.open) {
		closeEntry(false);
	} else {
		popMenu();
	}
}

bool uiEvent(const SDL_Event* event) {
	if (event->type == SDL_CONTROLLERBUTTONDOWN || (event->type == SDL_CONTROLLERAXISMOTION && abs(event->caxis.value) > 20000)) {
		lastInputPad = true;
	} else if (event->type == SDL_KEYDOWN || event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEWHEEL) {
		lastInputPad = false;
	}
	if (capture.open) {
		if (event->type == SDL_KEYDOWN && !capture.controller) {
			SDL_Scancode const code = event->key.keysym.scancode;
			if (code == SDL_SCANCODE_ESCAPE) {
				capture.open = false;
			} else {
				captured(code == SDL_SCANCODE_DELETE ? SDL_SCANCODE_UNKNOWN : code);
			}
			return true;
		}
		if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
			capture.open = false;
			return true;
		}
		if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_DELETE) {
			captured(PAD_NONE);
			return true;
		}
		if (event->type == SDL_CONTROLLERBUTTONDOWN && capture.controller) {
			captured(event->cbutton.button);
			return true;
		}
		if (event->type == SDL_CONTROLLERAXISMOTION && capture.controller && event->caxis.value > 24000 &&
		    (event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
			captured(event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PAD_LEFT_TRIGGER : PAD_RIGHT_TRIGGER);
			return true;
		}
		return event->type != SDL_QUIT;
	}
	if (!app.menuOpen) {
		return false;
	}
	switch (event->type) {
	case SDL_TEXTINPUT:
		if (entry.open && strlen(entry.text) + strlen(event->text.text) < sizeof(entry.text)) {
			strcat(entry.text, event->text.text);
		}
		return true;
	case SDL_KEYDOWN: {
		SDL_Scancode const code = event->key.keysym.scancode;
		if (entry.open) {
			if (code == SDL_SCANCODE_BACKSPACE && entry.text[0]) {
				entry.text[strlen(entry.text) - 1] = '\0';
			} else if (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER) {
				closeEntry(true);
			} else if (code == SDL_SCANCODE_ESCAPE) {
				closeEntry(false);
			} else if (code >= SDL_SCANCODE_RIGHT && code <= SDL_SCANCODE_UP) {
				entryMove(code == SDL_SCANCODE_RIGHT ? 1 : code == SDL_SCANCODE_LEFT ? -1 : 0,
				          code == SDL_SCANCODE_DOWN ? 1 : code == SDL_SCANCODE_UP ? -1 : 0);
			} else if (code == SDL_SCANCODE_V && (event->key.keysym.mod & KMOD_CTRL)) {
				char* clip = SDL_GetClipboardText();
				if (clip && strlen(entry.text) + strlen(clip) < sizeof(entry.text)) {
					strcat(entry.text, clip);
				}
				SDL_free(clip);
			}
			return true;
		}
		switch (code) {
		case SDL_SCANCODE_UP:
			direction(0, -1);
			break;
		case SDL_SCANCODE_DOWN:
			direction(0, 1);
			break;
		case SDL_SCANCODE_LEFT:
			direction(-1, 0);
			break;
		case SDL_SCANCODE_RIGHT:
			direction(1, 0);
			break;
		case SDL_SCANCODE_PAGEUP:
			for (int i = 0; i < 10; ++i) {
				moveSelection(-1);
			}
			break;
		case SDL_SCANCODE_PAGEDOWN:
			for (int i = 0; i < 10; ++i) {
				moveSelection(1);
			}
			break;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER:
		case SDL_SCANCODE_SPACE:
			confirm();
			break;
		case SDL_SCANCODE_ESCAPE:
		case SDL_SCANCODE_BACKSPACE:
			if (code == SDL_SCANCODE_ESCAPE && depth <= 1) {
				uiCloseMenu();
			} else {
				back();
			}
			break;
		default:
			if (code == settings.hotkeys[ACT_FULLSCREEN]) {
				settings.fullscreen = !settings.fullscreen;
				applyWindowSize();
			}
			break;
		}
		return true;
	}
	case SDL_CONTROLLERBUTTONDOWN:
		switch (event->cbutton.button) {
		case SDL_CONTROLLER_BUTTON_DPAD_UP:
			direction(0, -1);
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
			direction(0, 1);
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
			direction(-1, 0);
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
			direction(1, 0);
			break;
		case SDL_CONTROLLER_BUTTON_A:
			confirm();
			break;
		case SDL_CONTROLLER_BUTTON_B:
			back();
			break;
		case SDL_CONTROLLER_BUTTON_X:
			if (entry.open && entry.text[0]) {
				entry.text[strlen(entry.text) - 1] = '\0';
			}
			break;
		case SDL_CONTROLLER_BUTTON_START:
			if (entry.open) {
				closeEntry(true);
			}
			break;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
			for (int i = 0; i < 10; ++i) {
				moveSelection(event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? -1 : 1);
			}
			break;
		default:
			if (event->cbutton.button == settings.padHotkeys[ACT_MENU]) {
				uiCloseMenu();
			}
			break;
		}
		return true;
	case SDL_CONTROLLERAXISMOTION:
		if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
			int const v = event->caxis.value > 20000 ? 1 : event->caxis.value < -20000 ? -1 : 0;
			int const dx = event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? v : held.dx;
			int const dy = event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? v : held.dy;
			if ((dx || dy) && (dx != held.dx || dy != held.dy)) {
				direction(dx, dy);
				held.next = SDL_GetTicks() + REPEAT_DELAY_MS;
			}
			held.dx = dx;
			held.dy = dy;
		}
		return true;
	case SDL_MOUSEMOTION:
		if (!entry.open) {
			for (int i = 0; i < shownCount; ++i) {
				SDL_Point const p = { event->motion.x, event->motion.y };
				if (selectable(firstShown + i) && SDL_PointInRect(&p, &itemRects[i])) {
					stack[depth - 1].selected = firstShown + i;
				}
			}
		}
		return true;
	case SDL_MOUSEBUTTONDOWN: {
		SDL_Point const p = { event->button.x, event->button.y };
		if (event->button.button == SDL_BUTTON_RIGHT) {
			back();
		} else if (event->button.button == SDL_BUTTON_LEFT && entry.open) {
			for (int r = 0; r < 5; ++r) {
				for (int c = 0; c < 12; ++c) {
					if (SDL_PointInRect(&p, &keyRects[r][c])) {
						entry.row = r;
						entry.column = c;
						entryType();
						return true;
					}
				}
			}
		} else if (event->button.button == SDL_BUTTON_LEFT) {
			for (int i = 0; i < shownCount; ++i) {
				if (selectable(firstShown + i) && SDL_PointInRect(&p, &itemRects[i])) {
					bool const already = stack[depth - 1].selected == firstShown + i;
					stack[depth - 1].selected = firstShown + i;
					// A selected value's arrows change it backward or forward; elsewhere chooses.
					if (already && arrowLeft[i] >= 0 && p.x <= arrowLeft[i]) {
						activate(-1);
					} else if (already && arrowRight[i] >= 0 && p.x >= arrowRight[i]) {
						activate(1);
					} else {
						confirm();
					}
					break;
				}
			}
		}
		return true;
	}
	case SDL_MOUSEWHEEL:
		if (!entry.open) {
			stack[depth - 1].scroll -= event->wheel.y * 3;
		}
		return true;
	case SDL_QUIT:
	case SDL_DROPFILE:
	case SDL_CONTROLLERDEVICEADDED:
	case SDL_CONTROLLERDEVICEREMOVED:
	case SDL_WINDOWEVENT:
		return false;
	default:
		return false;
	}
}

// Drawing: a rounded panel in the middle of the window over the dimmed game, sized from a unit u
// that grows with the window.

static const SDL_Color white = { 255, 255, 255, 255 }, text = { 214, 220, 234, 255 }, muted = { 143, 155, 181, 255 },
                       accent = { 111, 168, 255, 255 }, pillFill = { 58, 120, 232, 255 }, panelFill = { 20, 26, 38, 240 }, edge = { 42, 53, 80, 255 },
                       keyFill = { 34, 43, 63, 255 };

static SDL_Color faded(SDL_Color c, int alpha) {
	c.a = (Uint8) (c.a * alpha / 255);
	return c;
}

static int unitFor(int width, int height) {
	return SDL_max(12, SDL_min(height / 24, width / 40));
}

// Section headers: small capitals, spaced out.
static void drawSpaced(enum FaceId face, int size, int x, int y, SDL_Color colour, const char* label, int spacing) {
	char one[2] = { 0, 0 };
	for (const char* p = label; *p; ++p) {
		one[0] = (char) (*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
		x += textDraw(face, size, x, y, colour, one) + spacing;
	}
}

// A button or key in the footer's hints: a circle for a controller's buttons, a key cap otherwise.
static int drawBadge(int x, int y, int u, const char* label, bool round) {
	int const size = u * 3 / 5, h = u + u / 8;
	int const tw = textWidth(FACE_STRONG, size, label);
	int const w = round ? h : SDL_max(h, tw + u * 2 / 3);
	fillRounded((SDL_Rect) { x, y, w, h }, round ? h / 2 : u / 4, keyFill);
	textDraw(FACE_STRONG, size, x + (w - tw) / 2, y + (h - size) / 2, white, label);
	return w;
}

static int drawHint(int x, int y, int u, const char* badge, bool round, const char* label) {
	int const size = u * 7 / 10, h = u + u / 8;
	x += drawBadge(x, y, u, badge, round) + u / 3;
	x += textDraw(FACE_BODY, size, x, y + (h - size) / 2, muted, label);
	return x + u;
}

// The panel, its header (ARMGBA and where you are) and its footer's hints; returns where its
// content starts.
static int drawPanel(SDL_Rect panel, int u, const char* where) {
	fillBordered(panel, u * 3 / 4, panelFill, edge, 1);
	int const headerH = u * 3, inset = u + u / 2;
	int const titleSize = u + u / 6, whereSize = u * 4 / 5;
	int const brand = textDraw(FACE_TITLE, titleSize, panel.x + inset, panel.y + (headerH - titleSize) / 2, accent, "ARMGBA");
	char fitted[512];
	textFit(FACE_BODY, whereSize, where, panel.w - 2 * inset - brand - u, true, fitted, sizeof(fitted));
	int const ww = textWidth(FACE_BODY, whereSize, fitted);
	textDraw(FACE_BODY, whereSize, panel.x + panel.w - inset - ww, panel.y + (headerH - whereSize) / 2, muted, fitted);
	fillRect(panel.x + u, panel.y + headerH, panel.w - 2 * u, 1, edge);
	return panel.y + headerH + u / 4;
}

static int footerTop(SDL_Rect panel, int u) {
	return panel.y + panel.h - u * 9 / 4;
}

static int drawFooter(SDL_Rect panel, int u) {
	int const top = footerTop(panel, u);
	fillRect(panel.x + u, top, panel.w - 2 * u, 1, edge);
	return top + (u * 9 / 4 - (u + u / 8)) / 2;
}

static void drawMessages(int width, int height, int u) {
	Uint32 const now = SDL_GetTicks();
	int const size = u * 3 / 4, h = u * 3 / 2;
	int y = height - u / 2 - h;
	for (int i = 0; i < MESSAGES; ++i) {
		if (!messages[i].text[0] || now >= messages[i].until) {
			continue;
		}
		Uint32 const left = messages[i].until - now;
		int const alpha = left < 400 ? (int) (left * 255 / 400) : 255;
		char fitted[200];
		textFit(FACE_BODY, size, messages[i].text, width - 3 * u, false, fitted, sizeof(fitted));
		int const w = textWidth(FACE_BODY, size, fitted) + u * 3 / 2;
		fillBordered((SDL_Rect) { u / 2, y, w, h }, h / 2, faded(panelFill, alpha), faded(edge, alpha), 1);
		textDraw(FACE_BODY, size, u / 2 + u * 3 / 4, y + (h - size) / 2, faded(text, alpha), fitted);
		y -= h + u / 3;
	}
}

static void drawEntry(int width, int height, int u) {
	int const fieldH = u * 2, keyH = u * 7 / 4, keySize = u * 4 / 5;
	SDL_Rect panel;
	panel.w = SDL_min(width - u, u * 30);
	panel.h = u * 3 + u / 2 + fieldH + u / 2 + 5 * keyH + u / 2 + u * 9 / 4;
	panel.x = (width - panel.w) / 2;
	panel.y = SDL_max(0, (height - panel.h) / 2);
	int y = drawPanel(panel, u, entry.title) + u / 4;
	int const left = panel.x + u, inner = panel.w - 2 * u;
	fillBordered((SDL_Rect) { left, y, inner, fieldH }, u / 2, (SDL_Color) { 13, 18, 28, 255 }, accent, 2);
	char shown[300];
	size_t const n = SDL_min(strlen(entry.text), sizeof(shown) - 1);
	if (entry.hidden) {
		memset(shown, '*', n);
		shown[n] = '\0';
	} else {
		snprintf(shown, sizeof(shown), "%s", entry.text);
	}
	char fitted[320];
	textFit(FACE_BODY, keySize, shown, inner - u * 2, true, fitted, sizeof(fitted));
	int const tw = textDraw(FACE_BODY, keySize, left + u * 2 / 3, y + (fieldH - keySize) / 2, white, fitted);
	if (SDL_GetTicks() / 500 % 2 == 0) {
		fillRect(left + u * 2 / 3 + tw + 1, y + (fieldH - keySize) / 2, 2, keySize, accent);
	}
	y += fieldH + u / 2;
	int const keyW = inner / 12, gap = SDL_max(2, u / 8);
	for (int r = 0; r < 5; ++r) {
		for (int c = 0; c < 12; ++c) {
			SDL_Rect* rect = &keyRects[r][c];
			int const span = r < 4 ? 1 : 3, at = r < 4 ? c : c / 3 * 3;
			*rect = (SDL_Rect) { left + at * keyW, y + r * keyH, span * keyW - gap, keyH - gap };
			if (r == 4 && c % 3) {
				continue;
			}
			bool const on = entry.row == r && (r < 4 ? entry.column == c : entry.column / 3 == c / 3);
			bool const shiftOn = r == 4 && c / 3 == 0 && entry.shift;
			fillRounded(*rect, u / 4, on ? pillFill : shiftOn ? (SDL_Color) { 47, 74, 122, 255 } : keyFill);
			char label[16];
			if (r < 4) {
				snprintf(label, sizeof(label), "%c", keyRows[entry.shift][r][c]);
			} else {
				snprintf(label, sizeof(label), "%s", keySpecials[c / 3]);
			}
			enum FaceId const face = on ? FACE_STRONG : FACE_BODY;
			int const lw = textWidth(face, keySize, label);
			textDraw(face, keySize, rect->x + (rect->w - lw) / 2, rect->y + (rect->h - keySize) / 2, on ? white : text, label);
		}
	}
	int x = panel.x + u + u / 2;
	y = drawFooter(panel, u);
	if (lastInputPad) {
		x = drawHint(x, y, u, "A", true, "Type");
		x = drawHint(x, y, u, "X", true, "Delete");
		x = drawHint(x, y, u, "B", true, "Cancel");
		drawHint(x, y, u, "Start", false, "Done");
	} else {
		x = drawHint(x, y, u, "Enter", false, "Done");
		x = drawHint(x, y, u, "Esc", false, "Cancel");
		drawHint(x, y, u, "Ctrl+V", false, "Paste");
	}
}

static void drawCapture(int width, int height, int u) {
	fillRect(0, 0, width, height, (SDL_Color) { 4, 6, 12, 140 });
	const char* what = capture.hotkey ? actionNames[capture.which] : buttonNames[capture.which];
	char line[256];
	snprintf(line, sizeof(line), capture.controller ? "Press a button for %s" : "Press a key for %s", what);
	int const size = u, small = u * 7 / 10;
	SDL_Rect panel;
	panel.w = SDL_min(width - u, SDL_max(u * 20, textWidth(FACE_STRONG, size, line) + u * 3));
	panel.h = u * 5;
	panel.x = (width - panel.w) / 2;
	panel.y = (height - panel.h) / 2;
	fillBordered(panel, u * 3 / 4, panelFill, accent, 2);
	int const lw = textWidth(FACE_STRONG, size, line);
	textDraw(FACE_STRONG, size, panel.x + (panel.w - lw) / 2, panel.y + u + u / 4, white, line);
	const char* hint = "Esc cancels \xC2\xB7 Delete clears it";
	int const hw = textWidth(FACE_BODY, small, hint);
	textDraw(FACE_BODY, small, panel.x + (panel.w - hw) / 2, panel.y + u * 3, muted, hint);
}

static void drawMenu(int width, int height, int u) {
	// Menus are built again each frame, so their values stay current; the game list, which can hold
	// thousands of games, is kept until its folder changes.
	static void (*built)(void);
	struct Menu* m = &stack[depth - 1];
	if (m->build != browserMenu || built != browserMenu || strcmp(browserListed, browserPath)) {
		itemCount = 0;
		m->build();
		built = m->build;
	}
	if (!selectable(m->selected)) {
		m->selected = -1;
		moveSelection(1);
	}
	int const rowH = u * 7 / 4, size = u * 19 / 20, headerSize = u * 3 / 5;
	int const chrome = u * 3 + u / 4 + u / 4 + u * 9 / 4;
	int const rows = SDL_max(1, SDL_min(itemCount, (height - u - chrome) / rowH));
	SDL_Rect panel;
	panel.w = SDL_min(width - u, u * 30);
	panel.h = chrome + rows * rowH;
	panel.x = (width - panel.w) / 2;
	panel.y = SDL_max(0, (height - panel.h) / 2);
	char where[512];
	if (depth > 1) {
		snprintf(where, sizeof(where), "%s \xE2\x80\xBA %s", stack[depth - 2].title, stack[depth - 1].title);
	} else {
		snprintf(where, sizeof(where), "%s", stack[0].title);
	}
	int const top = drawPanel(panel, u, where);
	// Keep the selection in view; the mouse wheel moves the view too.
	if (m->selected >= 0 && (m->selected < m->scroll || m->selected >= m->scroll + rows)) {
		m->scroll = m->selected < m->scroll ? m->selected : m->selected - rows + 1;
	}
	m->scroll = SDL_max(0, SDL_min(m->scroll, itemCount - rows));
	bool const scrolls = itemCount > rows;
	int const pillL = panel.x + u, pillR = panel.x + panel.w - u - (scrolls ? u / 2 : 0);
	int const textL = pillL + u * 3 / 4, textR = pillR - u * 3 / 4;
	// The highlight glides to the selection.
	static float pillY;
	static const void* lastMenu;
	static int lastDepth;
	static Uint32 lastTicks;
	Uint32 const now = SDL_GetTicks();
	float const target = (float) (top + (m->selected - m->scroll) * rowH);
	if (lastMenu != (const void*) m->build || lastDepth != depth || SDL_fabsf(target - pillY) > rowH * 4) {
		pillY = target;
	} else {
		pillY += (target - pillY) * SDL_min(1.0f, (now - lastTicks) * 0.022f);
	}
	lastMenu = (const void*) m->build;
	lastDepth = depth;
	lastTicks = now;
	if (m->selected >= 0) {
		int const y = (int) (pillY + 0.5f);
		SDL_Rect pill = { pillL, y + u / 8, pillR - pillL, rowH - u / 4 };
		int const clipTop = top, clipBottom = top + rows * rowH;
		if (pill.y < clipTop) {
			pill.h -= clipTop - pill.y;
			pill.y = clipTop;
		}
		pill.h = SDL_min(pill.h, clipBottom - pill.y);
		if (pill.h > 0) {
			fillRounded(pill, pill.h / 2, pillFill);
		}
	}
	firstShown = m->scroll;
	shownCount = 0;
	for (int i = m->scroll; i < itemCount && shownCount < rows; ++i, ++shownCount) {
		struct Item* item = &items[i];
		int const y = top + shownCount * rowH;
		itemRects[shownCount] = (SDL_Rect) { pillL, y, pillR - pillL, rowH };
		arrowLeft[shownCount] = arrowRight[shownCount] = -1;
		if (item->header) {
			drawSpaced(FACE_STRONG, headerSize, textL, y + rowH - headerSize - u / 3, accent, item->label, SDL_max(1, u / 10));
			continue;
		}
		bool const on = i == m->selected;
		enum FaceId const face = on ? FACE_STRONG : FACE_BODY;
		int const ty = y + (rowH - size) / 2;
		int valueL = textR;
		if (item->value[0]) {
			char value[NAME_SIZE + 64];
			int const arrows = on && item->adjustable ? textWidth(face, size, "\xE2\x80\xB9") + u / 2 : 0;
			textFit(face, size, item->value, (textR - textL) / 2 - 2 * arrows, false, value, sizeof(value));
			int const vw = textWidth(face, size, value);
			SDL_Color const colour = on ? white : item->function ? text : muted;
			if (arrows) {
				int const aw = textWidth(face, size, "\xE2\x80\xBA");
				textDraw(face, size, textR - aw, ty, white, "\xE2\x80\xBA");
				arrowRight[shownCount] = textR - aw - u / 4;
				valueL = textR - arrows - vw;
				textDraw(face, size, valueL, ty, colour, value);
				valueL -= arrows;
				textDraw(face, size, valueL, ty, white, "\xE2\x80\xB9");
				arrowLeft[shownCount] = valueL + aw + u / 4;
			} else {
				valueL = textR - vw;
				textDraw(face, size, valueL, ty, item->adjustable && !on ? muted : colour, value);
			}
		}
		char label[NAME_SIZE + 64];
		textFit(face, size, item->label, valueL - textL - u, item->label[0] == '/', label, sizeof(label));
		textDraw(face, size, textL, ty, on ? white : item->function ? text : muted, label);
	}
	if (scrolls) {
		int const trackH = rows * rowH, barH = SDL_max(u, trackH * rows / itemCount);
		int const barY = top + (trackH - barH) * m->scroll / SDL_max(1, itemCount - rows);
		fillRounded((SDL_Rect) { panel.x + panel.w - u, barY, SDL_max(3, u / 6), barH }, u, faded(accent, 110));
	}
	int x = panel.x + u + u / 2;
	int const y = drawFooter(panel, u);
	bool const adjustable = selectable(m->selected) && items[m->selected].adjustable;
	const char* backLabel = depth > 1 ? "Back" : app.loaded ? "Close" : NULL;
	if (lastInputPad) {
		x = drawHint(x, y, u, "A", true, "Select");
		if (backLabel) {
			x = drawHint(x, y, u, "B", true, backLabel);
		}
		if (adjustable) {
			drawHint(x, y, u, "\xE2\x80\xB9 \xE2\x80\xBA", false, "Change");
		}
	} else {
		x = drawHint(x, y, u, "Enter", false, "Select");
		if (backLabel) {
			x = drawHint(x, y, u, "Esc", false, backLabel);
		}
		if (adjustable) {
			drawHint(x, y, u, "\xE2\x86\x90 \xE2\x86\x92", false, "Change");
		}
	}
}

void uiDraw(void) {
	int width, height;
	SDL_GetRendererOutputSize(app.renderer, &width, &height);
	int const u = unitFor(width, height);
	// A held controller direction repeats.
	if ((held.dx || held.dy) && SDL_TICKS_PASSED(SDL_GetTicks(), held.next) && app.menuOpen) {
		direction(held.dx, held.dy);
		held.next = SDL_GetTicks() + REPEAT_MS;
	}
	if (app.menuOpen && depth) {
		fillRect(0, 0, width, height, app.loaded ? (SDL_Color) { 4, 6, 12, 165 } : (SDL_Color) { 11, 15, 24, 255 });
		if (entry.open) {
			drawEntry(width, height, u);
		} else {
			drawMenu(width, height, u);
			if (capture.open) {
				drawCapture(width, height, u);
			}
		}
	} else if (capture.open) {
		capture.open = false;
	}
	if (settings.showFps && app.loaded) {
		char fps[32];
		snprintf(fps, sizeof(fps), "%.1f FPS", app.fps);
		int const size = u * 13 / 20, h = u * 5 / 4;
		int const w = textWidth(FACE_STRONG, size, fps) + u;
		fillRounded((SDL_Rect) { width - w - u / 2, u / 2, w, h }, h / 2, (SDL_Color) { 0, 0, 0, 150 });
		textDraw(FACE_STRONG, size, width - w - u / 2 + u / 2, u / 2 + (h - size) / 2, white, fps);
	}
	drawMessages(width, height, u);
}
