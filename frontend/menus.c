/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The menus: each one's rows, built again each frame from what they show, and what choosing or
// changing a row does. ui.c draws them and moves through them.

#include "frontend.h"
#include "ui.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lcd.h"

char browserPath[PATH_SIZE];
// Recent games, read as the menu opens.
static char recentGames[RECENT_MAX][PATH_SIZE];
static int recentCount;

static void resume(int argument, int delta) {
	(void) argument;
	if (!delta) {
		uiCloseMenu();
	}
}

// Save State and Load State: left and right change the slot.
static void stateItem(int load, int delta) {
	if (delta) {
		app.slot = wrap(app.slot, delta, SLOTS);
		return;
	}
	if (stateCommand(load)) {
		uiCloseMenu();
	}
}

static void undoLoad(int argument, int delta) {
	(void) argument;
	if (!delta && undoLoadState()) {
		osd("Back to before the state loaded");
		uiCloseMenu();
	}
}

// Not during netplay, where every device runs the same game from the same start.
static void restart(int argument, int delta) {
	(void) argument;
	if (!delta && !netRunning()) {
		retro_reset();
		cheevosReset();
		uiCloseMenu();
	}
}

// The files the game list shows: games, and archives that may hold one.
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

static bool cheatFile(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot && !strcasecmp(dot, ".cht");
}

// The list lists RetroArch cheat files to add to the game's instead of games; the game list's folder
// is kept meanwhile.
static bool browserCheats;
static char browserGamesPath[PATH_SIZE];
// A folder's name to select once the list shows it: the one just left by "..".
static char browserReselect[NAME_SIZE];

// The game list: the folders, then the games, in browserPath, read once as the folder is entered.
struct BrowserEntry {
	char name[NAME_SIZE];
	bool directory;
};
static struct BrowserEntry* browserEntries;
static int browserCount, browserCapacity;
// The folder browserEntries lists; emptied to read it again.
char browserListed[PATH_SIZE];

// Folders first, then by name, case ignored.
static int compareEntries(const void* a, const void* b) {
	const struct BrowserEntry* const x = a;
	const struct BrowserEntry* const y = b;
	if (x->directory != y->directory) {
		return x->directory ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

// Reads browserPath's folders and games (or cheat files), sorted, unless it's the folder read last.
// File systems that don't say what an entry is, and symbolic links, are asked with stat.
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
		// Only folders and the files the list is for.
		if (!directory && !(browserCheats ? cheatFile(e->d_name) : gameFile(e->d_name))) {
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

// "3,753" for 3753.
static void grouped(char* out, size_t size, int n) {
	if (n >= 1000) {
		snprintf(out, size, "%d,%03d", n / 1000, n % 1000);
	} else {
		snprintf(out, size, "%d", n);
	}
}

// The game list: the folder's name and counts, "..", then each folder and game.
void browserMenu(void) {
	readBrowser();
	// The folder's name, and how many games and folders it holds.
	const char* slash = strrchr(browserPath, '/');
	const char* name = slash && slash[1] ? slash + 1 : browserPath;
	int folders = 0;
	for (int i = 0; i < browserCount; ++i) {
		folders += browserEntries[i].directory;
	}
	char games[16], folderCount[32] = "", counts[80];
	grouped(games, sizeof(games), browserCount - folders);
	if (folders) {
		snprintf(folderCount, sizeof(folderCount), ", %d %s", folders, folders == 1 ? "folder" : "folders");
	}
	snprintf(counts, sizeof(counts), "%s %s%s", games, browserCheats ? "cheat files" : browserCount - folders == 1 ? "game" : "games", folderCount);
	addInfo(name, counts);
	addItem("..", browse, -1);
	helpFor(browserPath);
	if (!browserCount && access(browserPath, R_OK)) {
		addInfo("Can't open this folder", "");
		return;
	}
	for (int i = 0; i < browserCount; ++i) {
		if (browserEntries[i].directory) {
			char label[NAME_SIZE + 2];
			snprintf(label, sizeof(label), "%s/", browserEntries[i].name);
			addItem(label, browse, i);
			if (browserReselect[0] && !strcmp(browserReselect, browserEntries[i].name)) {
				stack[depth - 1].selected = itemCount - 1;
				browserReselect[0] = '\0';
			}
		} else {
			addItem(browserEntries[i].name, browse, i);
		}
	}
	browserReselect[0] = '\0';
}

// A name's first letter, upper case, with digits and the rest as '#'.
static char initialOf(const char* name) {
	char const c = (char) toupper((unsigned char) name[0]);
	return c >= 'A' && c <= 'Z' ? c : '#';
}

// Selects the row of the game list's entry index.
static void selectEntry(int index) {
	for (int i = 0; i < itemCount; ++i) {
		if (items[i].function == browse && items[i].argument == index) {
			stack[depth - 1].selected = i;
			return;
		}
	}
}

// Folders, then games, by first letter.
static int groupOf(int index) {
	return browserEntries[index].directory * 256 + initialOf(browserEntries[index].name);
}

// Moves to the first game or folder of the next letter, or back to the start of this one or the one
// before.
void browserJump(int delta) {
	struct Menu* m = &stack[depth - 1];
	if (!browserCount) {
		return;
	}
	int at = selectable(m->selected) && items[m->selected].function == browse ? items[m->selected].argument : -1;
	// With nothing selected: the first entry, or the last going back.
	if (at < 0) {
		selectEntry(delta > 0 ? 0 : browserCount - 1);
		return;
	}
	int const group = groupOf(at);
	if (delta > 0) {
		while (at < browserCount && groupOf(at) == group) {
			++at;
		}
		selectEntry(at < browserCount ? at : 0);
		return;
	}
	// Going back: to this letter's first entry, or the letter before's if already there.
	int start = at;
	while (start > 0 && groupOf(start - 1) == group) {
		--start;
	}
	if (start == at) {
		start = at > 0 ? at - 1 : browserCount - 1;
		int const before = groupOf(start);
		while (start > 0 && groupOf(start - 1) == before) {
			--start;
		}
	}
	selectEntry(start);
}

// Typing a name's start selects the first game or folder that begins so; a pause starts it again.
void browserFind(char c) {
	static char typed[32];
	static Uint32 typedAt;
	Uint32 const now = SDL_GetTicks();
	size_t length = now - typedAt > 1000 ? 0 : strlen(typed);
	typedAt = now;
	if (length + 1 < sizeof(typed)) {
		typed[length++] = c;
		typed[length] = '\0';
	}
	for (int i = 0; i < browserCount; ++i) {
		if (!strncasecmp(browserEntries[i].name, typed, length)) {
			selectEntry(i);
			return;
		}
	}
}

// A game list row: ".." (index -1) goes up, selecting the folder left; a folder opens; a game loads; a
// cheat file's cheats are added to the game's.
static void browse(int index, int delta) {
	if (delta) {
		return;
	}
	if (index < 0) {
		char* slash = strrchr(browserPath, '/');
		snprintf(browserReselect, sizeof(browserReselect), "%s", slash ? slash + 1 : "");
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
	} else if (browserCheats) {
		int const added = cheatListImport(path);
		if (added < 0) {
			osd("Can't read %s", browserEntries[index].name);
		} else {
			cheatListApply();
			cheatListSave();
			osd(added == 1 ? "Added 1 cheat" : "Added %d cheats", added);
			popMenu();
		}
	} else if (loadGame(path)) {
		uiCloseMenu();
	}
}

// The game list, for games: back in the games folder if it was showing cheat files.
static void openBrowser(int argument, int delta) {
	(void) argument;
	if (!delta) {
		if (browserCheats) {
			browserCheats = false;
			snprintf(browserPath, sizeof(browserPath), "%s", browserGamesPath);
		}
		browserListed[0] = '\0';
		pushMenu(browserMenu, "Load Game");
	}
}

// Then General, with the recent games.
static void closeGame(int argument, int delta) {
	(void) argument;
	if (!delta) {
		unloadGame();
		SDL_SetWindowTitle(app.window, "armGBA");
		recentCount = recentList(recentGames, RECENT_MAX);
		showTab(TAB_GENERAL, 0);
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

// A cheat's Code (0), Name (1) and Delete (2).
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

// A cheat's row: left and right turn it on or off; choosing it opens its menu.
static void cheatOpen(int index, int delta) {
	if (delta) {
		cheatToggle(index, delta);
		return;
	}
	cheatShown = index;
	pushMenu(cheatMenu, cheats[index].description);
}

// Adding a cheat asks for its name, then its code.
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

// The game list, for RetroArch cheat files; where it was in the games is kept for when it goes back.
static void openCheatFiles(int argument, int delta) {
	(void) argument;
	if (!delta) {
		if (!browserCheats) {
			snprintf(browserGamesPath, sizeof(browserGamesPath), "%s", browserPath);
			browserCheats = true;
		}
		browserListed[0] = '\0';
		pushMenu(browserMenu, "Load Cheat File");
	}
}

static void cheatsMenu(void) {
	static char fileHelp[PATH_SIZE + 128];
	if (cheevosHardcore()) {
		addInfo("Hardcore mode: cheats are off", "");
		return;
	}
	addItem("Add Cheat...", cheatNew, 0);
	helpFor("Type a code: Action Replay, GameShark, CodeBreaker or Game Genie.");
	addItem("Load Cheat File...", openCheatFiles, 0);
	snprintf(fileHelp, sizeof(fileHelp), "Adds the cheats in a RetroArch .cht file. This game's cheats are kept in %s.", cheatDirectory);
	helpFor(fileHelp);
	if (!cheatCount) {
		addInfo("No cheats for this game yet", "");
	} else {
		addHeader("Cheats");
	}
	for (int i = 0; i < cheatCount; ++i) {
		addValue(cheats[i].description, onOff(cheats[i].enabled), cheatOpen, i);
		helpFor("Left and right turn it on or off; choose it to change it.");
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

// A core option's row: left and right step through its values; choosing it steps forward.
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
	refreshShownOptions();
}

// The visible core options in category, or those in none when category is NULL.
static void addOptions(const char* category) {
	for (int i = 0; i < optionCount; ++i) {
		struct Option* o = &options[i];
		bool const here = category ? o->category && !strcmp(o->category, category) : !o->category;
		// The core's LCD grid and pixel shadow show only with LCD Rendering on the CPU.
		bool const cpuLcd = !strcmp(o->key, "armgba_lcd_grid") || !strcmp(o->key, "armgba_lcd_shadow");
		if (!here || !o->visible || (cpuLcd && settings.lcdOnGpu)) {
			continue;
		}
		const char* label = o->value;
		for (int v = 0; v < o->valueCount; ++v) {
			if (!strcmp(o->values[v], o->value)) {
				label = o->labels[v];
			}
		}
		addValue(o->description ? o->description : o->key, label, optionChange, i);
		helpFor(o->info);
	}
}

// Whether category is one of parent's own groups, "link_players" of "link", shown on its page.
static bool isGroupOf(const char* category, const char* parent) {
	size_t const length = strlen(parent);
	return category && !strncmp(category, parent, length) && category[length] == '_';
}

// Whether any of the category's options shows.
static bool anyShown(const char* category) {
	for (int i = 0; i < optionCount; ++i) {
		if (options[i].visible && options[i].category && !strcmp(options[i].category, category)) {
			return true;
		}
	}
	return false;
}

static void addLcdRendering(void);

// A category's options, then each of its groups' under a header: the group's name after its
// category's ("Link: Players" shows as Players). Video starts with LCD Rendering, which Shaders shows
// too.
static void categoryMenu(void) {
	const char* const key = categories[categoryShown].key;
	if (!strcmp(key, "video")) {
		addLcdRendering();
	}
	addOptions(key);
	for (int c = 0; c < categoryCount; ++c) {
		if (isGroupOf(categories[c].key, key) && anyShown(categories[c].key)) {
			const char* const name = categories[c].description ? categories[c].description : categories[c].key;
			const char* const colon = strstr(name, ": ");
			addHeader(colon ? colon + 2 : name);
			addOptions(categories[c].key);
		}
	}
}

static void openCategory(int index, int delta) {
	if (!delta) {
		categoryShown = index;
		pushMenu(categoryMenu, categories[index].description ? categories[index].description : categories[index].key);
	}
}

// A row for each category with options to show, then the options in none.
static void coreOptionsMenu(void) {
	if (!optionCount) {
		addInfo("The core's options appear once it has started", "");
		return;
	}
	for (int c = 0; c < categoryCount; ++c) {
		// Groups are on their category's page.
		if (strchr(categories[c].key, '_')) {
			continue;
		}
		bool any = anyShown(categories[c].key);
		for (int g = 0; g < categoryCount; ++g) {
			any |= isGroupOf(categories[g].key, categories[c].key) && anyShown(categories[g].key);
		}
		if (any) {
			char label[160];
			snprintf(label, sizeof(label), "%s...", categories[c].description ? categories[c].description : categories[c].key);
			addItem(label, openCategory, c);
		}
	}
	addOptions(NULL);
}

// Settings.

enum {
	SET_SCALE,
	SET_FULLSCREEN,
	SET_INTEGER,
	SET_STRETCH,
	SET_FILTER,
	SET_LCD_RENDERING,
	SET_GPU_GRID,
	SET_SHADOW,
	SET_VSYNC,
	SET_FPS,
	SET_VOLUME,
	SET_LATENCY,
	SET_REWIND,
	SET_REWIND_SIZE,
	SET_RUNAHEAD,
	SET_AUTO_STATE,
	SET_FAST_FORWARD,
	SET_MESSAGES,
	SET_INPUT_DELAY,
	SET_HARDCORE,
	SET_ACCENT,
	SET_MENU_SIZE,
	SET_DIM,
};

// A setting's row (SET_*): left and right step a value or turn it over, as choosing it does. The
// settings file is written each time.
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
	// The core reads its grid again, now off or back at its size.
	case SET_LCD_RENDERING:
		settings.lcdOnGpu = !settings.lcdOnGpu;
		optionsChanged = true;
		refreshShownOptions();
		break;
	case SET_GPU_GRID:
		settings.gpuGrid = !settings.gpuGrid;
		break;
	// Off, then the eight directions.
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
	// 16 to 512 MB, doubling.
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
	// As fast as it goes, then 2x to 8x.
	case SET_FAST_FORWARD: {
		static const int speeds[] = { 0, 2, 3, 4, 6, 8 };
		int at = 0;
		for (int i = 0; i < 6; ++i) {
			at = speeds[i] == settings.fastForward ? i : at;
		}
		settings.fastForward = speeds[wrap(at, d, 6)];
		break;
	}
	case SET_MESSAGES:
		settings.showMessages = !settings.showMessages;
		break;
	case SET_INPUT_DELAY:
		settings.inputDelay = wrap(settings.inputDelay, d, 9);
		break;
	case SET_HARDCORE:
		settings.hardcore = !settings.hardcore;
		cheevosSetHardcore(settings.hardcore);
		break;
	case SET_ACCENT:
		settings.accent = wrap(settings.accent, d, ACCENTS);
		break;
	case SET_MENU_SIZE: {
		int const count = MENU_SIZES;
		int at = 1;
		for (int i = 0; i < count; ++i) {
			at = menuSizes[i].percent == settings.menuSize ? i : at;
		}
		settings.menuSize = menuSizes[wrap(at, d, count)].percent;
		break;
	}
	case SET_DIM:
		settings.dim = wrap(settings.dim, d, 3);
		break;
	}
	settingsWrite();
}

// LCD Rendering, the same row on Core > Video and Shaders: which of them draws the LCD grid and pixel
// shadow.
static void addLcdRendering(void) {
	addValue("LCD Rendering", settings.lcdOnGpu ? "GPU" : "CPU", changeSetting, SET_LCD_RENDERING);
	helpFor("Who draws the LCD grid and the Game Boy pixel shadow: CPU, the emulator (set in Core > Video), or GPU, at the size "
	        "the picture is shown (set in Shaders). Only one is used.");
}

// General > Video: the window, how the picture is scaled and shown, and what's drawn over it. The
// emulator's own picture options are in Core > Video.
static void videoMenu(void) {
	char text[32];
	addHeader("Window");
	if (!settings.fullscreen) {
		snprintf(text, sizeof(text), "%dx", settings.scale);
		addValue("Window Size", text, changeSetting, SET_SCALE);
		helpFor("The window's size, in times the console's screen.");
	}
	addValue("Full Screen", onOff(settings.fullscreen), changeSetting, SET_FULLSCREEN);
	helpFor("Fills the display. On a handheld it's on from the start.");
	addHeader("Picture");
	addValue("Integer Scaling", onOff(settings.integerScale), changeSetting, SET_INTEGER);
	helpFor("Scales by whole numbers only, so every pixel is the same size; leaves a border.");
	addValue("Aspect Ratio", settings.stretch ? "Fill the Window" : "The Console's", changeSetting, SET_STRETCH);
	helpFor("The Console's keeps the screen's shape; Fill the Window stretches it.");
	addValue("VSync", onOff(settings.vsync), changeSetting, SET_VSYNC);
	helpFor("Shows each frame as the display refreshes, so the picture doesn't tear.");
	addHeader("On-Screen Display");
	addValue("Show Frame Rate", onOff(settings.showFps), changeSetting, SET_FPS);
	addValue("Show Messages", onOff(settings.showMessages), changeSetting, SET_MESSAGES);
	helpFor("The notes at the bottom left, such as a state saved or a controller connected.");
}

// General > Shaders: what the GPU does to the picture as it scales it; its LCD grid and pixel
// shadow only with LCD Rendering on the GPU, the shadow only on the first Game Boy's and the Pocket's
// screens, or before a game is loaded.
static void shadersMenu(void) {
	static const char* const filters[FILTERS] = { "Sharp", "Smooth" };
	addValue("Filter", filters[settings.filter], changeSetting, SET_FILTER);
	helpFor("Sharp keeps pixels square; Smooth blends them.");
	addLcdRendering();
	if (!settings.lcdOnGpu) {
		return;
	}
	addValue("LCD Grid", onOff(settings.gpuGrid), changeSetting, SET_GPU_GRID);
	helpFor("Draws the gaps between the console screen's pixels at any size, fainter the smaller it is. On a small screen, "
	        "the CPU's at 2x with Integer Scaling looks cleanest.");
	uint32_t gap;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	if (settings.gpuGrid && (!app.loaded || lcd == LIBRETRO_LCD_DMG || lcd == LIBRETRO_LCD_POCKET)) {
		static const char* const shadows[9] = { "Off", "Up", "Up Right", "Right", "Down Right", "Down", "Down Left", "Left", "Up Left" };
		addValue("Game Boy Pixel Shadow", shadows[settings.shadow], changeSetting, SET_SHADOW);
		helpFor("On the first Game Boy and the Pocket, each pixel casts a soft shadow on the screen behind it; this is where it falls.");
	}
}

static void audioMenu(void) {
	char text[32];
	snprintf(text, sizeof(text), "%d%%", settings.volume);
	addValue("Volume", text, changeSetting, SET_VOLUME);
	snprintf(text, sizeof(text), "%d %s (%d ms)", settings.latency, frames(settings.latency), (int) (settings.latency * 1000 / FRAME_RATE));
	addValue("Latency", text, changeSetting, SET_LATENCY);
	helpFor("How much sound waits to play. Less answers sooner but may crackle on a slow device.");
}

static void gameplayMenu(void) {
	char text[32];
	addValue("Rewind", onOff(settings.rewind), changeSetting, SET_REWIND);
	helpFor("Hold the Rewind hotkey to go back in time.");
	snprintf(text, sizeof(text), "%d MB", settings.rewindMegabytes);
	addValue("Rewind Buffer", text, changeSetting, SET_REWIND_SIZE);
	helpFor("Memory kept for rewinding: more goes further back.");
	snprintf(text, sizeof(text), settings.runahead ? "%d %s" : "Off", settings.runahead, frames(settings.runahead));
	addValue("Run-Ahead", text, changeSetting, SET_RUNAHEAD);
	helpFor("Runs frames ahead and shows the last, so the game answers that many frames sooner. Costs speed.");
}

// which: the button's or hotkey's index, plus 1000 for a controller's, plus 2000 for a hotkey.
static void bind(int which, int delta) {
	if (delta) {
		return;
	}
	int const kind = which / 1000, index = which % 1000;
	uiCaptureBinding(index, kind == 1 || kind == 3, kind >= 2);
}

// The default keys, buttons and hotkeys; the other settings are kept.
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

// The buttons as listed: the face buttons and shoulders first, then the turbo buttons and extra
// shoulders.
static const int buttonOrder[BUTTONS] = { BUTTON_A, BUTTON_B, 10, 11, 3, 2, 4, 5, 6, 7, 9, 1, 12, 13, 14, 15 };

// Fast Forward and Rewind work while held.
static const char* hotkeyHelp(int action) {
	return action == ACT_FAST_FORWARD ? "Runs the game faster while held." : action == ACT_REWIND ? "Goes back in time while held." : NULL;
}

static void padControlsMenu(void) {
	addHeader("Buttons");
	for (int k = 0; k < BUTTONS; ++k) {
		int const i = buttonOrder[k];
		addChoice(buttonNames[i], padName(settings.pad[i]), bind, 1000 + i);
		helpFor(i == BUTTON_A ? "A also chooses in these menus." : i == BUTTON_B ? "B also goes back in these menus." : NULL);
	}
	addHeader("Hotkeys");
	for (int i = 0; i < ACTIONS; ++i) {
		addChoice(actionNames[i], padName(settings.padHotkeys[i]), bind, 3000 + i);
		helpFor(hotkeyHelp(i));
	}
}

static void keyControlsMenu(void) {
	addHeader("Buttons");
	for (int k = 0; k < BUTTONS; ++k) {
		int const i = buttonOrder[k];
		addChoice(buttonNames[i], scancodeName(settings.keys[i]), bind, i);
	}
	addHeader("Hotkeys");
	for (int i = 0; i < ACTIONS; ++i) {
		addChoice(actionNames[i], scancodeName(settings.hotkeys[i]), bind, 2000 + i);
		helpFor(hotkeyHelp(i));
	}
}

static void openControlsPage(int which, int delta) {
	if (!delta) {
		pushMenu(which ? keyControlsMenu : padControlsMenu, which ? "Keyboard" : "Controller");
	}
}

static void controlsMenu(void) {
	addItem("Controller...", openControlsPage, 0);
	helpFor("The buttons and hotkeys on a controller, the same for each player's.");
	addItem("Keyboard...", openControlsPage, 1);
	helpFor("The keys and hotkeys on the keyboard, which plays player 1.");
	char text[32];
	snprintf(text, sizeof(text), settings.fastForward ? "%dx" : "Max", settings.fastForward);
	addValue("Fast-Forward Speed", text, changeSetting, SET_FAST_FORWARD);
	helpFor("How fast the Fast Forward hotkey runs the game while it's held. Max is as fast as it goes, up to 12x.");
	addItem("Reset Controls and Hotkeys", resetControls, 0);
	items[itemCount - 1].confirm = true;
}

static void appearanceMenu(void) {
	addValue("Accent Color", accents[settings.accent].name, changeSetting, SET_ACCENT);
	helpFor("The color of the title, the headers and the selection.");
	const char* size = "Normal";
	for (int i = 0; i < MENU_SIZES; ++i) {
		size = menuSizes[i].percent == settings.menuSize ? menuSizes[i].name : size;
	}
	addValue("Menu Size", size, changeSetting, SET_MENU_SIZE);
	helpFor("The menus' text and rows, larger for a TV across the room or smaller for more rows.");
	addValue("Menu Background", dims[settings.dim], changeSetting, SET_DIM);
	helpFor("How dark the menus are over the game: Light shows the game most, Dark hides it.");
}

static void foldersMenu(void);

static void openSettingsPage(int which, int delta) {
	static void (*const pages[])(void) = { videoMenu, shadersMenu, audioMenu, controlsMenu, gameplayMenu, foldersMenu, appearanceMenu };
	static const char* const titles[] = { "Video", "Shaders", "Audio", "Controls", "Rewind and Run-Ahead", "Saves and Folders", "Appearance" };
	if (!delta) {
		pushMenu(pages[which], titles[which]);
	}
}

static void addSettingsPages(void) {
	addItem("Video...", openSettingsPage, 0);
	helpFor("The window, scaling, vsync, and the frame rate and messages on screen.");
	addItem("Shaders...", openSettingsPage, 1);
	helpFor("The filter, and the GPU's LCD grid and Game Boy pixel shadow.");
	addItem("Audio...", openSettingsPage, 2);
	addItem("Controls...", openSettingsPage, 3);
	helpFor("Buttons, hotkeys and fast-forward speed, for controllers and the keyboard.");
	addItem("Rewind and Run-Ahead...", openSettingsPage, 4);
	addItem("Saves and Folders...", openSettingsPage, 5);
	helpFor("Saving a state on exit, and where games, BIOS files, saves, states and screenshots are.");
	addItem("Appearance...", openSettingsPage, 6);
	helpFor("The menus' color and size, and how dark the game is behind them.");
}

// Saves and Folders: where games, BIOS files, saves, states and screenshots are. Each folder's row
// opens its choices: Auto, armGBA's and RetroArch's where it has them, or a folder typed in.
enum { FOLDER_GAMES, FOLDER_BIOS, FOLDER_SAVES, FOLDER_STATES, FOLDER_SHOTS, FOLDERS };
static const struct {
	const char* label;
	char* setting;
	// armGBA's own folder; what retroarchFolder calls RetroArch's, or NULL; and whether "" is Auto
	// rather than armGBA's.
	const char* own;
	const char* retroarch;
	bool automatic;
	const char* help;
} folders[FOLDERS] = {
	{ "Games", settings.directory, NULL, NULL, false, "Where Load Game opens; the folder of the last game loaded." },
	{ "BIOS", settings.biosFolder, ownSystemDirectory, "bios", true, "Read as each game loads." },
	{ "Saves", settings.saveFolder, ownSaveDirectory, "saves", true, "Games' saves; in RetroArch's, shared with RetroArch. Applies when a game loads." },
	{ "States", settings.stateFolder, ownStateDirectory, NULL, false, "Save states, kept apart from RetroArch's, which it compresses." },
	{ "Screenshots", settings.shotFolder, ownShotDirectory, "screenshots", false, "Screenshots and recordings." },
};
static int folderShown;

// The folder in use, for the help line; the save folder is the game's.
static const char* folderPath(int which) {
	switch (which) {
	case FOLDER_GAMES:
		return settings.directory;
	case FOLDER_BIOS:
		return systemDirectory;
	case FOLDER_SAVES:
		return app.loaded ? saveDirectory : NULL;
	case FOLDER_STATES:
		return stateDirectory;
	default:
		return shotDirectory;
	}
}

static bool folderTypedIn(int which) {
	const char* const setting = folders[which].setting;
	return which == FOLDER_GAMES || (setting[0] && strcmp(setting, "armgba") && strcmp(setting, "retroarch"));
}

// A folder's name without the folders it's in.
static const char* folderName(const char* path) {
	const char* const slash = strrchr(path, '/');
	return slash && slash[1] ? slash + 1 : path;
}

static void folderTyped(const char* text) {
	snprintf(folders[folderShown].setting, PATH_SIZE, "%s", text);
	chooseFolders();
	settingsWrite();
}

// A choice on a folder's page: 0 Auto, 1 armGBA's, 2 RetroArch's, 3 typed in.
static void folderPick(int kind, int delta) {
	if (delta) {
		return;
	}
	if (kind == 3) {
		const char* const path = folderPath(folderShown);
		openEntry(folders[folderShown].label, path ? path : folders[folderShown].own, false, folderTyped);
		return;
	}
	const char* const kinds[3] = { "", folders[folderShown].automatic ? "armgba" : "", "retroarch" };
	snprintf(folders[folderShown].setting, PATH_SIZE, "%s", kinds[kind]);
	chooseFolders();
	settingsWrite();
}

static void folderMenu(void) {
	int const which = folderShown;
	const char* const setting = folders[which].setting;
	bool const typed = folderTypedIn(which);
	static char help[4][PATH_SIZE + 200];
	if (folders[which].automatic) {
		addChoice("Auto", !setting[0] ? "In Use" : "", folderPick, 0);
		helpFor(which == FOLDER_BIOS ? "armGBA's own folder if it has a BIOS or boot ROM, RetroArch's otherwise."
		                             : "armGBA's own folder for a game with its save there, RetroArch's otherwise.");
	}
	if (folders[which].own) {
		bool const own = folders[which].automatic ? !strcmp(setting, "armgba") : !setting[0];
		addChoice("armGBA's", own ? "In Use" : "", folderPick, 1);
		snprintf(help[1], sizeof(help[1]), "%s", folders[which].own);
		helpFor(help[1]);
	}
	const char* const theirs = folders[which].retroarch ? retroarchFolder(folders[which].retroarch) : NULL;
	if (theirs) {
		addChoice("RetroArch's", !strcmp(setting, "retroarch") ? "In Use" : "", folderPick, 2);
		snprintf(help[2], sizeof(help[2]), "%s, read from RetroArch's retroarch.cfg.", theirs);
		helpFor(help[2]);
	}
	addChoice("Type a Folder...", typed ? folderName(setting) : "", folderPick, 3);
	snprintf(help[3], sizeof(help[3]), typed ? "In use: %s" : "Any folder, typed in.", setting);
	helpFor(help[3]);
}

static void openFolder(int which, int delta) {
	if (!delta) {
		folderShown = which;
		char title[64];
		snprintf(title, sizeof(title), "%s Folder", folders[which].label);
		pushMenu(folderMenu, title);
	}
}

static void addFolder(int which) {
	const char* const setting = folders[which].setting;
	const char* const path = folderPath(which);
	char label[64], value[NAME_SIZE];
	snprintf(label, sizeof(label), "%s...", folders[which].label);
	if (folderTypedIn(which)) {
		snprintf(value, sizeof(value), "%s", folderName(setting));
	} else if (setting[0] || !folders[which].automatic) {
		snprintf(value, sizeof(value), "%s", !strcmp(setting, "retroarch") ? "RetroArch's" : "armGBA's");
	} else if (path) {
		snprintf(value, sizeof(value), "Auto (%s)", !strcmp(path, folders[which].own) ? "armGBA's" : "RetroArch's");
	} else {
		snprintf(value, sizeof(value), "Auto");
	}
	addChoice(label, value, openFolder, which);
	static char help[FOLDERS][PATH_SIZE + 300];
	snprintf(help[which], sizeof(help[which]), "%s%s%s%s", path ? "Now: " : "", path ? path : "", path ? ". " : "", folders[which].help);
	helpFor(help[which]);
}

static void foldersMenu(void) {
	addHeader("Saves");
	addValue("Auto Save State", onOff(settings.autoState), changeSetting, SET_AUTO_STATE);
	helpFor("Saves a state when the game closes and loads it when the game starts again.");
	addHeader("Folders");
	for (int i = 0; i < FOLDERS; ++i) {
		addFolder(i);
	}
}

// Reset Config: the settings and the core's options as they first were, but for the RetroAchievements
// login, the netplay name and host, the folders and the recent games.
static void resetConfig(int argument, int delta) {
	(void) argument;
	if (delta) {
		return;
	}
	struct Settings const kept = settings;
	settingsDefaults();
	memcpy(settings.cheevosUser, kept.cheevosUser, sizeof(settings.cheevosUser));
	memcpy(settings.cheevosToken, kept.cheevosToken, sizeof(settings.cheevosToken));
	memcpy(settings.nickname, kept.nickname, sizeof(settings.nickname));
	memcpy(settings.host, kept.host, sizeof(settings.host));
	settings.netPort = kept.netPort;
	memcpy(settings.directory, kept.directory, sizeof(settings.directory));
	memcpy(settings.biosFolder, kept.biosFolder, sizeof(settings.biosFolder));
	memcpy(settings.saveFolder, kept.saveFolder, sizeof(settings.saveFolder));
	memcpy(settings.stateFolder, kept.stateFolder, sizeof(settings.stateFolder));
	memcpy(settings.shotFolder, kept.shotFolder, sizeof(settings.shotFolder));
	for (int i = 0; i < optionCount; ++i) {
		optionSet(&options[i], options[i].defaultValue);
	}
	chooseFolders();
	cheevosSetHardcore(settings.hardcore);
	rewindReset();
	applyWindowSize();
	recreateRenderer();
	refreshShownOptions();
	settingsWrite();
	optionsWrite();
	osd("Settings and emulator options reset");
}

// Netplay.

static void nicknameDone(const char* text) {
	snprintf(settings.nickname, sizeof(settings.nickname), "%s", *text ? text : "Player");
	settingsWrite();
}

// The host's address, kept for the next time.
static void joinDone(const char* text) {
	snprintf(settings.host, sizeof(settings.host), "%s", text);
	settingsWrite();
	if (netJoin(text)) {
		uiCloseMenu();
	}
}

static void portDone(const char* text) {
	int const port = atoi(text);
	if (port < 1 || port > 65535) {
		osd("A port is a number from 1 to 65535");
		return;
	}
	settings.netPort = port;
	settingsWrite();
}

// Netplay's rows: Your Name (0), hosting Separate (1) or Together (2), Join (3), Disconnect (4) and
// Port (5).
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
	case 5: {
		char port[16];
		snprintf(port, sizeof(port), "%d", settings.netPort);
		openEntry("Port (1 to 65535)", port, false, portDone);
		break;
	}
	}
}

static void netplayMenu(void) {
	addInfo("Status", netStatus[0] ? netStatus : "Not connected");
	addChoice("Your Name", settings.nickname, netAction, 0);
	helpFor("Shown to the other players.");
	char text[32];
	snprintf(text, sizeof(text), "%d", settings.netPort);
	addChoice("Port", text, netAction, 5);
	helpFor("The network port the host listens on; everyone uses the same one.");
	snprintf(text, sizeof(text), "%d %s", settings.inputDelay, frames(settings.inputDelay));
	addValue("Input Delay", text, changeSetting, SET_INPUT_DELAY);
	helpFor("How long each player's buttons wait in Together, so fewer corrections show. More suits a slow network.");
	if (netMode == NET_OFF) {
		addHeader("Host");
		addItem("Separate", netAction, 1);
		helpFor("Hosts the loaded game: each device runs its own console and game, joined by the link cable over the network. Two "
		        "players.");
		addItem("Together", netAction, 2);
		helpFor("Hosts the loaded game: every device runs every player's console, with rollback, so the link never waits on the network.");
		addHeader("Join");
		addItem("Join a Host...", netAction, 3);
		helpFor("Type the host's name or address.");
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

// Logging in asks for the user name, then the password; cheevos.c keeps the token, not the password.
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
	addValue("Hardcore Mode", onOff(settings.hardcore), changeSetting, SET_HARDCORE);
	helpFor("Earns achievements as hardcore: save states, cheats, rewind and run-ahead are off, and turning it on restarts the game.");
	static char lines[400][160];
	int const n = cheevosList(lines, 400);
	if (n) {
		addHeader("Achievements");
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

// Screenshot (0), recording (1) and Quit (2).
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

static void recentChoose(int index, int delta) {
	if (!delta && index < recentCount && loadGame(recentGames[index])) {
		uiCloseMenu();
	}
}

// A recent game's row: its name without the extension, and the folder it's in.
static void addRecent(int index) {
	const char* path = recentGames[index];
	const char* slash = strrchr(path, '/');
	char name[NAME_SIZE], folder[NAME_SIZE] = "";
	snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
	char* dot = strrchr(name, '.');
	if (dot && dot != name) {
		*dot = '\0';
	}
	// The folder's own name, not its path.
	if (slash && slash != path) {
		int const length = (int) (slash - path);
		const char* start = path + length;
		while (start > path && start[-1] != '/') {
			--start;
		}
		snprintf(folder, sizeof(folder), "%.*s", (int) (path + length - start), start);
	}
	addChoice(name, folder, recentChoose, index);
}

static void recentMenu(void) {
	for (int i = 0; i < recentCount; ++i) {
		addRecent(i);
	}
}

static void openRecent(int argument, int delta) {
	(void) argument;
	if (!delta) {
		pushMenu(recentMenu, "Recent Games");
	}
}

// The tabs.

// General: the games, then the settings. Without a game, the five most recent games first.
static void generalTab(void) {
	if (!app.loaded && recentCount) {
		addHeader("Recent Games");
		for (int i = 0; i < recentCount && i < 5; ++i) {
			addRecent(i);
		}
		if (recentCount > 5) {
			addItem("More Recent Games...", openRecent, 0);
		}
	} else if (recentCount) {
		addItem("Recent Games...", openRecent, 0);
	}
	addItem("Load Game...", openBrowser, 0);
	addHeader("Settings");
	addSettingsPages();
	addItem("Reset Config", resetConfig, 0);
	items[itemCount - 1].confirm = true;
	helpFor("Puts every setting and emulator option back as it first was. Your RetroAchievements login, netplay name, folders "
	        "and recent games stay.");
	addItem("Quit", misc, 2);
	items[itemCount - 1].confirm = app.loaded;
}

// Game: the game loaded, its states, and closing it.
static void gameTab(void) {
	char text[32];
	if (!app.loaded) {
		addInfo("No game loaded", "");
		addItem("Load Game...", openBrowser, 0);
		return;
	}
	addHeader(app.gameName);
	addItem("Resume", resume, 0);
	snprintf(text, sizeof(text), stateExists(app.slot) ? "Slot %d" : "Slot %d \xC2\xB7 Empty", app.slot);
	addValue("Save State", text, stateItem, 0);
	helpFor("Saves the game as it is now in the slot shown; left and right change the slot.");
	addValue("Load State", text, stateItem, 1);
	helpFor("Goes back to the state in the slot shown. Undo Load State brings back what you had.");
	if (canUndoLoad()) {
		addItem("Undo Load State", undoLoad, 0);
		helpFor("Goes back to the game as it was before the last state loaded.");
	}
	addItem("Restart", restart, 0);
	items[itemCount - 1].confirm = true;
	helpFor("Starts the game again from the beginning; the game's own save is kept.");
	addItem("Screenshot", misc, 0);
	addItem(recording() ? "Stop Recording" : "Start Recording", misc, 1);
	addItem("Close Game", closeGame, 0);
	items[itemCount - 1].confirm = true;
}

// Extras: cheats for the game loaded, netplay and RetroAchievements.
static void extrasTab(void) {
	if (app.loaded) {
		addItem("Cheats...", openCheats, 0);
	}
	addItem("Netplay...", openNetplay, 0);
	addItem("Achievements...", openCheevos, 0);
}

const char* const tabNames[TABS] = { "General", "Game", "Core", "Extras" };
void (*const tabBuilds[TABS])(void) = { generalTab, gameTab, coreOptionsMenu, extrasTab };

// Opens the menus where they were left, the game list back in the games folder.
void uiOpenMenu(void) {
	if (app.menuOpen) {
		return;
	}
	app.menuOpen = true;
	recentCount = recentList(recentGames, RECENT_MAX);
	// Where the menus were left, the first time General; a game list, left by loading a game, back to
	// its tab.
	if (!depth) {
		showTab(TAB_GENERAL, 0);
	}
	while (depth > 1 && (stack[depth - 1].build == browserMenu || stack[depth - 1].build == recentMenu)) {
		--depth;
	}
	snprintf(browserPath, sizeof(browserPath), "%s", settings.directory);
	browserCheats = false;
}
