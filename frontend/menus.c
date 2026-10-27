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

#ifdef __ANDROID__
#include <jni.h>
#include <limits.h>
#define ANDROID_STORAGE "/storage"
#endif

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

// State Slot: left and right, or choosing it, change the slot Save State and Load State use.
static void stateSlot(int argument, int delta) {
	(void) argument;
	app.slot = wrap(app.slot, delta ? delta : 1, SLOTS);
}

static void stateItem(int load, int delta) {
	if (!delta && stateCommand(load)) {
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
		netGameStarted();
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

// Room for count entries.
static bool ensureEntries(int count) {
	if (count <= browserCapacity) {
		return true;
	}
	int const capacity = browserCapacity ? browserCapacity * 2 : 256;
	struct BrowserEntry* const grown = realloc(browserEntries, (size_t) capacity * sizeof(*grown));
	if (!grown) {
		return false;
	}
	browserEntries = grown;
	browserCapacity = capacity;
	return true;
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
#ifdef __ANDROID__
	// Android's top is its storage: the shared storage, emulated/0, and each card's partition.
	if (!strcmp(browserPath, ANDROID_STORAGE)) {
		ensureEntries(1);
		snprintf(browserEntries[browserCount].name, NAME_SIZE, "emulated/0");
		browserEntries[browserCount++].directory = true;
	}
#endif
	if (!dir) {
		return;
	}
	struct dirent* e;
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.') {
			continue;
		}
#ifdef __ANDROID__
		if (!strcmp(browserPath, ANDROID_STORAGE) && (!strcmp(e->d_name, "emulated") || !strcmp(e->d_name, "self"))) {
			continue;
		}
#endif
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
		if (!ensureEntries(browserCount + 1)) {
			break;
		}
		snprintf(browserEntries[browserCount].name, NAME_SIZE, "%s", e->d_name);
		browserEntries[browserCount++].directory = directory;
	}
	closedir(dir);
	qsort(browserEntries, (size_t) browserCount, sizeof(*browserEntries), compareEntries);
}

static void browse(int index, int delta);
static void browseTo(int which, int delta);

// A folder's name without the folders it's in.
static const char* folderName(const char* path) {
	const char* const slash = strrchr(path, '/');
	return slash && slash[1] ? slash + 1 : path;
}


// "3,753" for 3753.
static void grouped(char* out, size_t size, int n) {
	if (n >= 1000) {
		snprintf(out, size, "%d,%03d", n / 1000, n % 1000);
	} else {
		snprintf(out, size, "%d", n);
	}
}

// A folder's path as shown: on Android, the shared storage as Internal and a card as Card.
static void shownPath(char* out, size_t size, const char* path) {
#ifdef __ANDROID__
	static const char shared[] = "/storage/emulated/0";
	size_t const sharedLength = sizeof(shared) - 1;
	if (!strncmp(path, shared, sharedLength) && (!path[sharedLength] || path[sharedLength] == '/')) {
		snprintf(out, size, "Internal%s", path + sharedLength);
		return;
	}
	if (!strncmp(path, "/storage/", 9) && path[9] && strncmp(path + 9, "emulated", 8) && strncmp(path + 9, "self", 4)) {
		const char* const rest = strchr(path + 9, '/');
		snprintf(out, size, "Card%s", rest ? rest : "");
		return;
	}
#endif
	snprintf(out, size, "%s", path);
}

// The game list: the folder's path and counts, "..", then each folder and game.
void browserMenu(void) {
	readBrowser();
	// The folder's name, and how many games and folders it holds.
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
	char where[PATH_SIZE];
	shownPath(where, sizeof(where), browserPath);
	// Only the end of a path too long for the row.
	size_t const length = strlen(where);
	addInfo(length > NAME_SIZE ? where + length - NAME_SIZE : where, counts);
	items[itemCount - 1].path = true;
	// Shortcuts, under their own header: in the Games folder, its consoles' folders and the last
	// game's; outside it, the way back to it. Then this folder's own.
	bool const home = !strcmp(browserPath, gamesDirectory);
	bool listed = !strcmp(settings.directory, browserPath);
	int shortcuts = 0;
	for (int i = 0; i < CONSOLES; ++i) {
		listed |= !strcmp(settings.directory, consoleDirectories[i]);
		shortcuts += home && consoleDirectories[i][0];
	}
	bool const last = home && settings.directory[0] && !listed && !access(settings.directory, R_OK);
	// Inside the Games folder, ".." leads back to it.
	size_t const gamesLength = strlen(gamesDirectory);
	bool const inside = gamesLength && !strncmp(browserPath, gamesDirectory, gamesLength) && browserPath[gamesLength] == '/';
	bool const away = !home && !inside && gamesDirectory[0];
	shortcuts += last + away;
	if (!browserCheats && shortcuts) {
		addHeader("Shortcuts");
		for (int i = 0; home && i < CONSOLES; ++i) {
			if (consoleDirectories[i][0]) {
				addChoice(consoleNames[i], folderName(consoleDirectories[i]), browseTo, i);
				helpFor(consoleDirectories[i]);
			}
		}
		if (last) {
			addChoice("Last Game's Folder", folderName(settings.directory), browseTo, CONSOLES);
			helpFor(settings.directory);
		}
		if (away) {
			addChoice("Games Folder", folderName(gamesDirectory), browseTo, CONSOLES + 1);
			helpFor(gamesDirectory);
		}
		addHeader("This Folder");
	}
	addItem("..", browse, -1);
	helpFor(browserPath);
	// Outside the Games folder, a folder opens on "..", as before the shortcuts.
	if (!home && stack[depth - 1].selected < 0) {
		stack[depth - 1].selected = itemCount - 1;
	}
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
#ifdef __ANDROID__
		// Above the shared storage or a card is the list of them; Android lets apps read nothing higher.
		// /sdcard's paths are named by where they lead.
		char real[PATH_MAX];
		if (strncmp(browserPath, ANDROID_STORAGE, strlen(ANDROID_STORAGE)) && realpath(browserPath, real) && strlen(real) < sizeof(browserPath)) {
			snprintf(browserPath, sizeof(browserPath), "%s", real);
		}
		if (!strcmp(browserPath, ANDROID_STORAGE)) {
			return;
		}
		size_t const top = strlen(ANDROID_STORAGE "/");
		char* const after = strncmp(browserPath, ANDROID_STORAGE "/", top) ? NULL : browserPath + top;
		char* const inside = after ? strchr(!strncmp(after, "emulated/", 9) ? after + 9 : after, '/') : NULL;
		if (!inside) {
			snprintf(browserReselect, sizeof(browserReselect), "%s", after ? after : "emulated/0");
			snprintf(browserPath, sizeof(browserPath), "%s", ANDROID_STORAGE);
			stack[depth - 1].selected = -1;
			return;
		}
#endif
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
		size_t const length = strlen(path);
		if (length >= sizeof(browserPath)) {
			osd("That folder's path is too long");
			return;
		}
		memcpy(browserPath, path, length + 1);
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

// A game list shortcut: a console's folder, the last game's, or the Games folder.
static void browseTo(int which, int delta) {
	if (!delta) {
		const char* const path = which < CONSOLES ? consoleDirectories[which] : which == CONSOLES ? settings.directory : gamesDirectory;
		snprintf(browserPath, sizeof(browserPath), "%s", path);
		stack[depth - 1].selected = -1;
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
		openEntry("Cheat Code", "Action Replay, GameShark, CodeBreaker or Game Genie.", cheats[cheatShown].code, false, cheatCodeDone);
	} else if (which == 1) {
		openEntry("Cheat Name", NULL, cheats[cheatShown].description, false, cheatNameDone);
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
	helpFor("Whether the cheat is on while you play.");
	addChoice("Code", c->code, cheatEdit, 0);
	helpFor("The cheat's code; choose it to change it.");
	addChoice("Name", c->description, cheatEdit, 1);
	helpFor("What the cheat list calls it; choose it to change it.");
	addItem("Delete", cheatEdit, 2);
	helpFor("Takes the cheat off this game's list.");
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
	openEntry("Cheat Code", "Action Replay, GameShark, CodeBreaker or Game Genie; join several lines with +.", "", false, newCheatCode);
}

static void cheatNew(int argument, int delta) {
	(void) argument;
	if (!delta) {
		openEntry("Cheat Name", "What the cheat list calls it.", "", false, newCheatNamed);
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
	helpFor("Name a cheat, then type its code: Action Replay, GameShark, CodeBreaker or Game Genie.");
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
// A core option's values as a list to pick from, the one in use marked, as a tap or A opens it for an
// option of more than two values.
static int pickerOption;

static void pickValue(int value, int delta) {
	if (delta) {
		return;
	}
	struct Option* o = &options[pickerOption];
	optionSet(o, o->values[value]);
	optionsWrite();
	refreshShownOptions();
	popMenu();
}

static void pickerMenu(void) {
	struct Option* o = &options[pickerOption];
	for (int v = 0; v < o->valueCount; ++v) {
		bool const now = !strcmp(o->values[v], o->value);
		addChoice(o->labels[v] ? o->labels[v] : o->values[v], now ? "In Use" : "", pickValue, v);
		helpFor(o->info);
		if (now && stack[depth - 1].selected < 0) {
			stack[depth - 1].selected = itemCount - 1;
		}
	}
}

static void optionChange(int index, int delta) {
	struct Option* o = &options[index];
	if (!delta && o->valueCount > 2) {
		pickerOption = index;
		pushMenu(pickerMenu, o->description ? o->description : o->key);
		return;
	}
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

// A core option's row, if it shows.
static void addOption(int i) {
	struct Option* o = &options[i];
	// The core's LCD grid and pixel shadow show only with LCD Rendering on the CPU; the shadow's
	// distance, on the GPU, with the app's own shadow on.
	bool const cpuLcd = !strcmp(o->key, "armgba_lcd_grid") || !strcmp(o->key, "armgba_lcd_shadow");
	bool const distance = !strcmp(o->key, "armgba_lcd_shadow_distance");
	bool const shown = distance && settings.lcdOnGpu ? settings.gpuGrid && settings.shadow && libretroLcdShadow() : o->visible;
	if (!shown || (cpuLcd && settings.lcdOnGpu)) {
		return;
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

// The visible core options in category, or those in none when category is NULL.
static void addOptions(const char* category) {
	for (int i = 0; i < optionCount; ++i) {
		struct Option* o = &options[i];
		if (category ? o->category && !strcmp(o->category, category) : !o->category) {
			addOption(i);
		}
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
// category's ("Link: Players" shows as Players). LCD Effects starts with LCD Rendering, which Shaders
// shows too.
static void categoryMenu(void) {
	const char* const key = categories[categoryShown].key;
	addOptions(key);
	for (int c = 0; c < categoryCount; ++c) {
		if (isGroupOf(categories[c].key, key) && anyShown(categories[c].key)) {
			const char* const name = categories[c].description ? categories[c].description : categories[c].key;
			const char* const colon = strstr(name, ": ");
			addHeader(colon ? colon + 2 : name);
			if (!strcmp(categories[c].key, "video_lcd")) {
				addLcdRendering();
			}
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
			helpFor(categories[c].info);
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
	SET_TOUCH,
	SET_SWAP_FACE_BUTTONS,
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
		settings.shadow = wrap(settings.shadow, d, tiltAvailable() ? SHADOW_TILT + 1 : SHADOW_TILT);
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
	case SET_SWAP_FACE_BUTTONS:
		settings.swapFaceButtons = !settings.swapFaceButtons;
		break;
	case SET_TOUCH:
		settings.touchControls = !settings.touchControls;
		setPadForPort();
		break;
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
	helpFor("Who draws the LCD grid and the Game Boy pixel shadow: CPU, the emulator (set in Core \xE2\x80\xBA Video), or GPU, at the size "
	        "the picture is shown (set in General \xE2\x80\xBA Shaders). Only one is used.");
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
	helpFor("Frames a second, at the top right.");
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
	if (settings.gpuGrid && (!app.loaded || libretroLcdShadow())) {
		static const char* const shadows[SHADOW_TILT + 1] = { "Off", "Up", "Up Right", "Right", "Down Right", "Down", "Down Left", "Left", "Up Left", "Tilt" };
		addValue("Game Boy Pixel Shadow", shadows[settings.shadow], changeSetting, SET_SHADOW);
		helpFor(tiltAvailable() ? "On the first Game Boy and the Pocket, each pixel casts a soft shadow on the screen behind it; this is where it falls. "
		                          "Tilt moves it as the device tilts, as light from above would."
		                        : "On the first Game Boy and the Pocket, each pixel casts a soft shadow on the screen behind it; this is where it falls.");
	}
}

static void audioMenu(void) {
	char text[32];
	snprintf(text, sizeof(text), "%d%%", settings.volume);
	addValue("Volume", text, changeSetting, SET_VOLUME);
	helpFor("How loud the game is.");
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
		kept.padHotkey = settings.padHotkey;
		settings = kept;
		settingsWrite();
		osd("Controls reset");
	}
}

// The buttons as listed: the face buttons and shoulders first, then the turbo buttons and extra
// shoulders.
static const int buttonOrder[BUTTONS] = { BUTTON_A, BUTTON_B, 10, 11, 3, 2, 4, 5, 6, 7, 9, 1, 12, 13, 14, 15 };

// Fast-Forward and Rewind work while held.
static const char* hotkeyHelp(int action, bool controller) {
	switch (action) {
	case ACT_MENU:
		return "Opens and closes this menu.";
	case ACT_FAST_FORWARD:
		return controller ? "Turns fast-forward on, and off again." : "Runs the game faster while held.";
	case ACT_REWIND:
		return "Goes back in time while held.";
	case ACT_QUIT:
		return "Quits armGBA on a second press, writing the game's save first.";
	case ACT_FPS:
		return "Shows or hides the frame rate.";
	default:
		return NULL;
	}
}

static void padControlsMenu(void) {
	addHeader("Buttons");
	for (int k = 0; k < BUTTONS; ++k) {
		int const i = buttonOrder[k];
		addChoice(buttonNames[i], padName(settings.pad[i]), bind, 1000 + i);
		helpFor(i == BUTTON_A ? "A also chooses in these menus." : i == BUTTON_B ? "B also goes back in these menus." : NULL);
	}
	addHeader("Hotkeys");
	addChoice("Hotkey", padHotkeyName(), bind, 3000 + ACTIONS);
	helpFor("Held with each action's button below. Auto is Home, or Select on a controller without Home, as on ROCKNIX.");
	for (int i = 0; i < ACTIONS; ++i) {
		char value[64];
		snprintf(value, sizeof(value), "Hotkey + %s", padName(settings.padHotkeys[i]));
		addChoice(actionNames[i], settings.padHotkeys[i] == PAD_NONE ? "None" : value, bind, 3000 + i);
		helpFor(hotkeyHelp(i, true));
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
		helpFor(hotkeyHelp(i, false));
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
#ifdef __ANDROID__
	addValue("On-Screen Controls", onOff(settings.touchControls), changeSetting, SET_TOUCH);
	helpFor("A controller on the screen, played by touch, there while no other controller is connected.");
#endif
	addValue("Swap A/B and X/Y", onOff(settings.swapFaceButtons), changeSetting, SET_SWAP_FACE_BUTTONS);
	helpFor("Off: A is the right face button and B the bottom one, as on a Game Boy and in RetroArch. On: A is the bottom one and B the right; X and Y swap too. On every controller.");
	addValue("Fast-Forward Speed", text, changeSetting, SET_FAST_FORWARD);
	helpFor("How fast the Fast-Forward hotkey runs the game while it's held. Max is as fast as it goes, up to 12x.");
	addItem("Reset Controls and Hotkeys", resetControls, 0);
	helpFor("Puts every button, key and hotkey back as it first was.");
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
	helpFor("The volume, and how long sound waits to play.");
	addItem("Controls...", openSettingsPage, 3);
	helpFor("Buttons, hotkeys and fast-forward speed, for controllers and the keyboard.");
	addItem("Rewind and Run-Ahead...", openSettingsPage, 4);
	helpFor("Going back in time, and answering the buttons sooner.");
	addItem("Saves and Folders...", openSettingsPage, 5);
	helpFor("Saving a state on exit, and where games, BIOS files, saves, states and screenshots are.");
	addItem("Appearance...", openSettingsPage, 6);
	helpFor("The menus' color and size, and how dark the game is behind them.");
}

// Saves and Folders: where games, BIOS files, saves, states and screenshots are. Each folder's row
// opens its choices: Auto, armGBA's and RetroArch's where it has them, or a folder typed in.
enum { FOLDER_GAMES, FOLDER_GB, FOLDER_GBC, FOLDER_GBA, FOLDER_BIOS, FOLDER_SAVES, FOLDER_STATES, FOLDER_SHOTS, FOLDERS };
static const struct {
	const char* label;
	char* setting;
	// armGBA's own folder; what retroarchFolder calls RetroArch's, or NULL; and whether "" is Auto
	// rather than armGBA's, and what Auto is.
	const char* own;
	const char* retroarch;
	bool automatic;
	const char* autoHelp;
	const char* help;
} folders[FOLDERS] = {
	{ "Games", settings.gamesFolder, NULL, NULL, true, "ROCKNIX's roms folder, or a roms folder with gb, gbc or gba in it, in internal storage or on a card; else found from the last game's folder.",
	  "The folder with your games (ROMs), where Load Game opens." },
	{ "Game Boy", settings.consoleFolders[0], NULL, NULL, true, "A folder in the Games folder named gb or Game Boy.",
	  "Listed first in Load Game." },
	{ "Game Boy Color", settings.consoleFolders[1], NULL, NULL, true, "A folder in the Games folder named gbc or Game Boy Color.",
	  "Listed first in Load Game." },
	{ "Game Boy Advance", settings.consoleFolders[2], NULL, NULL, true, "A folder in the Games folder named gba or Game Boy Advance.",
	  "Listed first in Load Game." },
	{ "BIOS", settings.biosFolder, ownSystemDirectory, "bios", true, "armGBA's own folder if it has a BIOS or boot ROM, RetroArch's otherwise.",
	  "Read as each game loads." },
	{ "Saves", settings.saveFolder, ownSaveDirectory, "saves", true, "armGBA's own folder for a game with its save there, RetroArch's otherwise.",
	  "Games' saves; in RetroArch's, shared with RetroArch. Applies when a game loads." },
	{ "States", settings.stateFolder, ownStateDirectory, NULL, false, NULL, "Save states, kept apart from RetroArch's, which it compresses." },
	{ "Screenshots", settings.shotFolder, ownShotDirectory, "screenshots", false, NULL, "Screenshots and recordings." },
};
static int folderShown;

// The folder in use, for the help line; the save folder is the game's.
static const char* folderPath(int which) {
	switch (which) {
	case FOLDER_GAMES:
		return gamesDirectory;
	case FOLDER_GB:
	case FOLDER_GBC:
	case FOLDER_GBA:
		return consoleDirectories[which - FOLDER_GB][0] ? consoleDirectories[which - FOLDER_GB] : NULL;
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
	return setting[0] && strcmp(setting, "armgba") && strcmp(setting, "retroarch");
}

static void folderTyped(const char* text) {
	snprintf(folders[folderShown].setting, PATH_SIZE, "%s", text);
	chooseFolders();
	settingsWrite();
}

static void folderPick(int kind, int delta);

#ifdef __ANDROID__
// Android's folder picker (Armgba.java), for the folder shown; its answer is polled as the menus draw.
static bool pickerOpen;
static int pickerFolder;

static jclass armgbaClass(JNIEnv* env) {
	jclass const found = (*env)->FindClass(env, "org/armgba/app/Armgba");
	if (!found && (*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	}
	return found;
}

static void openPicker(void) {
	JNIEnv* const env = SDL_AndroidGetJNIEnv();
	jclass const armgba = env ? armgbaClass(env) : NULL;
	jmethodID const pick = armgba ? (*env)->GetStaticMethodID(env, armgba, "pickFolder", "()V") : NULL;
	if (!pick) {
		folderPick(3, 0);
		return;
	}
	(*env)->CallStaticVoidMethod(env, armgba, pick);
	(*env)->DeleteLocalRef(env, armgba);
	pickerOpen = true;
	pickerFolder = folderShown;
}

void folderPickerPoll(void) {
	if (!pickerOpen) {
		return;
	}
	JNIEnv* const env = SDL_AndroidGetJNIEnv();
	jclass const armgba = env ? armgbaClass(env) : NULL;
	jmethodID const take = armgba ? (*env)->GetStaticMethodID(env, armgba, "takeFolder", "()Ljava/lang/String;") : NULL;
	jstring const answer = take ? (jstring) (*env)->CallStaticObjectMethod(env, armgba, take) : NULL;
	if (armgba) {
		(*env)->DeleteLocalRef(env, armgba);
	}
	if (!answer) {
		return;
	}
	pickerOpen = false;
	const char* const path = (*env)->GetStringUTFChars(env, answer, NULL);
	// No picker on this device: the folder's typed in instead.
	folderShown = pickerFolder;
	if (!strcmp(path, "?")) {
		folderPick(3, 0);
	} else if (path[0] && access(path, R_OK)) {
		osd("Can't read %s", path);
	} else if (path[0]) {
		snprintf(folders[pickerFolder].setting, PATH_SIZE, "%s", path);
		chooseFolders();
		settingsWrite();
		osd("%s Folder: %s", folders[pickerFolder].label, folderName(path));
	}
	(*env)->ReleaseStringUTFChars(env, answer, path);
	(*env)->DeleteLocalRef(env, answer);
}
#endif

// A choice on a folder's page: 0 Auto, 1 armGBA's, 2 RetroArch's, 3 typed in, 4 from Android's picker.
static void folderPick(int kind, int delta) {
	if (delta) {
		return;
	}
#ifdef __ANDROID__
	if (kind == 4) {
		openPicker();
		return;
	}
#endif
	if (kind == 3) {
		const char* const path = folderPath(folderShown);
		char folderTitle[64];
		snprintf(folderTitle, sizeof(folderTitle), "%s Folder", folders[folderShown].label);
		openEntry(folderTitle, "The folder's full path.", path ? path : folders[folderShown].own, false, folderTyped);
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
		helpFor(folders[which].autoHelp);
	}
	if (folders[which].own) {
		bool const own = folders[which].automatic ? !strcmp(setting, "armgba") : !setting[0];
		addChoice("armGBA's", own ? "In Use" : "", folderPick, 1);
		snprintf(help[1], sizeof(help[1]), "armGBA's own: %s", folders[which].own);
		helpFor(help[1]);
	}
	const char* const theirs = folders[which].retroarch ? retroarchFolder(folders[which].retroarch) : NULL;
	if (theirs) {
		addChoice("RetroArch's", !strcmp(setting, "retroarch") ? "In Use" : "", folderPick, 2);
		snprintf(help[2], sizeof(help[2]), "RetroArch's, from its retroarch.cfg: %s", theirs);
		helpFor(help[2]);
	}
#ifdef __ANDROID__
	addChoice("Choose a Folder...", typed ? folderName(setting) : "", folderPick, 4);
	snprintf(help[3], sizeof(help[3]), typed ? "In use: %s" : "Any folder, chosen with Android's folder picker.", setting);
#else
	addChoice("Type a Folder...", typed ? folderName(setting) : "", folderPick, 3);
	snprintf(help[3], sizeof(help[3]), typed ? "In use: %s" : "Any folder, typed in.", setting);
#endif
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
	} else if (!setting[0] && !folders[which].own) {
		snprintf(value, sizeof(value), "Auto (%s)", path ? folderName(path) : "None");
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
	addHeader("Game Folders");
	for (int i = FOLDER_GAMES; i <= FOLDER_GBA; ++i) {
		addFolder(i);
	}
	addHeader("Data Folders");
	for (int i = FOLDER_BIOS; i < FOLDERS; ++i) {
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
	memcpy(settings.gamesFolder, kept.gamesFolder, sizeof(settings.gamesFolder));
	memcpy(settings.consoleFolders, kept.consoleFolders, sizeof(settings.consoleFolders));
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

// Netplay's rows: Your Name (0), Create a Host Session (1), Join (3), Disconnect (4) and Port (5).
static void netAction(int which, int delta) {
	if (delta) {
		return;
	}
	switch (which) {
	case 0:
		openEntry("Your Name", "Shown to the other players.", settings.nickname, false, nicknameDone);
		break;
	case 1:
		if (!app.loaded) {
			osd("Netplay: load your game first");
		} else if (netHost(NULL)) {
			uiCloseMenu();
		}
		break;
	case 3:
		openEntry("Host Address", "The host's name or IP address, and :port if it isn't the usual one.", settings.host, false, joinDone);
		break;
	case 4:
		netStop();
		break;
	case 5: {
		char port[16];
		snprintf(port, sizeof(port), "%d", settings.netPort);
		openEntry("Port", "A number from 1 to 65535.", port, false, portDone);
		break;
	}
	}
}

// Link Mode and Shared Controls, as Core's Link page shows them, and that page, which the host's
// session follows.
static void addNetplayLink(void) {
	struct Option* const mode = optionFind("armgba_link_mode");
	if (!mode) {
		return;
	}
	addHeader("Link");
	addOption((int) (mode - options));
	struct Option* const shared = optionFind("armgba_shared_controls");
	if (shared) {
		addOption((int) (shared - options));
	}
	// The Link page, when it has more than these.
	bool more = false;
	for (int i = 0; i < optionCount; ++i) {
		struct Option* const o = &options[i];
		more |= o != mode && o != shared && o->visible && o->category && (!strcmp(o->category, "link") || isGroupOf(o->category, "link"));
	}
	for (int c = 0; c < categoryCount && more; ++c) {
		if (!strcmp(categories[c].key, "link")) {
			addItem("More Link Options...", openCategory, c);
			helpFor(categories[c].info);
		}
	}
}

// A host the Join list found.
static void joinFound(int which, int delta) {
	const struct NetHost* hosts;
	if (!delta && which < netScan(&hosts)) {
		joinDone(hosts[which].address);
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
	helpFor("How long each player's buttons wait, with Link Mode Off or Together, so fewer corrections show. More suits a slow network.");
	if (netMode == NET_OFF) {
		addNetplayLink();
		addHeader("Host");
		addItem("Create a Host Session", netAction, 1);
		helpFor("Hosts the loaded game, linked as Core's Link Mode says. Off: one console every player shares, as Shared "
		        "Controls says. Together: every player's console on every device. Separate: two devices, each its own.");
		addHeader("Join");
		addItem("Join a Host...", netAction, 3);
		helpFor("Type the host's name or address.");
		const struct NetHost* hosts;
		int const count = netScan(&hosts);
		for (int i = 0; i < count; ++i) {
			char label[192], value[64];
			snprintf(label, sizeof(label), "%s: %s", hosts[i].nickname, hosts[i].game);
			static const char* const shared[] = { "", ", Everyone", ", Pass Around" };
			snprintf(value, sizeof(value), "Link Mode %s%s, %d/%d", netLinkNames[hosts[i].how],
			         hosts[i].how == LINK_OFF ? shared[hosts[i].shared] : "", hosts[i].players, hosts[i].most);
			addChoice(label, value, joinFound, i);
			helpFor("A host on this network. Joins it.");
		}
		if (!count) {
			addInfo("Hosts", "Looking on this network...");
		}
	} else {
		addItem("Disconnect", netAction, 4);
		helpFor("Leaves netplay; the game carries on here.");
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
	openEntry("RetroAchievements Password", "Sent only to retroachievements.org; only a token is kept.", "", true, passwordDone);
}

static void cheevosAction(int which, int delta) {
	if (delta) {
		return;
	}
	if (which == 0) {
		openEntry("RetroAchievements User Name", NULL, settings.cheevosUser, false, userDone);
	} else {
		cheevosLogout();
	}
}

static void cheevosMenu(void) {
	addInfo("Status", cheevosStatus());
	if (settings.cheevosToken[0]) {
		addItem("Log Out", cheevosAction, 1);
		helpFor("Forgets your RetroAchievements login on this device.");
	} else {
		addItem("Log In...", cheevosAction, 0);
		helpFor("Your RetroAchievements name and password; only a token is kept.");
	}
	addValue("Hardcore Mode", onOff(settings.hardcore), changeSetting, SET_HARDCORE);
	helpFor("Earns achievements as hardcore: save states, cheats, rewind and run-ahead are off, and turning it on restarts the game.");
	static char lines[400][160];
	int const n = cheevosList(lines, 400);
	if (n) {
		addHeader("This Game");
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
	asGame();
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
			helpFor("Every game played lately.");
		}
	} else if (recentCount) {
		addItem("Recent Games...", openRecent, 0);
		helpFor("The games played lately, newest first.");
	}
	addItem("Load Game...", openBrowser, 0);
	helpFor("Finds a game in your folders; zip and 7z archives open too.");
	addHeader("Settings");
	addSettingsPages();
	addItem("Reset Config", resetConfig, 0);
	items[itemCount - 1].confirm = true;
	helpFor("Puts every setting and emulator option back as it first was. Your RetroAchievements login, netplay name, folders "
	        "and recent games stay.");
	addItem("Quit", misc, 2);
	items[itemCount - 1].confirm = app.loaded;
	helpFor("Closes armGBA; the game's save is written first.");
}

// Game: the game loaded, its states, and closing it.
static void gameTab(void) {
	char text[32];
	if (!app.loaded) {
		addInfo("No game loaded", "");
		addItem("Load Game...", openBrowser, 0);
		helpFor("Finds a game in your folders; zip and 7z archives open too.");
		return;
	}
	addHeader(app.gameName);
	asGame();
	addItem("Resume", resume, 0);
	helpFor("Back to the game.");
	snprintf(text, sizeof(text), stateExists(app.slot) ? "%d" : "%d \xC2\xB7 Empty", app.slot);
	addValue("State Slot", text, stateSlot, 0);
	asState();
	helpFor("The slot Save State and Load State use, 0 to 99; left and right change it, faster as they're held.");
	addItem("Save State", stateItem, 0);
	asState();
	helpFor("Saves the game as it is now in the slot.");
	addItem("Load State", stateItem, 1);
	asState();
	helpFor("Goes back to the state in the slot. Undo Load State brings back what you had.");
	if (canUndoLoad()) {
		addItem("Undo Load State", undoLoad, 0);
		helpFor("Goes back to the game as it was before the last state loaded.");
	}
	addItem("Restart", restart, 0);
	items[itemCount - 1].confirm = true;
	helpFor("Starts the game again from the beginning; the game's own save is kept.");
	addItem("Screenshot", misc, 0);
	helpFor("Saves the game's picture as it is now, in the screenshots folder.");
	addItem(recording() ? "Stop Recording" : "Start Recording", misc, 1);
	helpFor("Records the game's picture and sound to a video in the screenshots folder.");
	addItem("Close Game", closeGame, 0);
	items[itemCount - 1].confirm = true;
	helpFor("Ends the game, writing its save, and goes back to General.");
}

// Extras: cheats for the game loaded, netplay and RetroAchievements.
static void extrasTab(void) {
	if (app.loaded) {
		addItem("Cheats...", openCheats, 0);
		helpFor("This game's cheats: add codes, load RetroArch cheat files, turn them on or off.");
	}
	addItem("Netplay...", openNetplay, 0);
	helpFor("Play linked with another device over the network.");
	addItem("Achievements...", openCheevos, 0);
	helpFor("RetroAchievements: log in, hardcore mode, and this game's achievements.");
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
	chooseGameFolders();
	snprintf(browserPath, sizeof(browserPath), "%s", gamesDirectory);
	browserCheats = false;
}
