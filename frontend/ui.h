/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_FRONTEND_UI_H
#define ARMGBA_FRONTEND_UI_H

// What ui.c, which draws the menus and moves through them, and menus.c, which builds them, share.

#include "frontend.h"

enum {
	DEPTH_MAX = 8,
	MENU_SIZES = 4,
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
	// Asks to be chosen twice, as it ends or throws away the game's progress or the settings.
	bool confirm;
	// What it does, shown under the menu while it's selected.
	const char* help;
	// A game: indented from the menu's own rows, or as a header, panning across when it's too long.
	bool game;
	// A path, cut from its start when it doesn't fit.
	bool path;
	// Save State or Load State: selected, the slot's picture is shown.
	bool state;
};

struct Menu {
	void (*build)(void);
	char title[NAME_SIZE + 64];
	int selected;
	int scroll;
};

// ui.c: the menu being built and the menus opened to reach it; the rows' building blocks; the
// on-screen keyboard; and the menus' look.
extern struct Item* items;
extern int itemCount;
extern struct Menu stack[DEPTH_MAX];
extern int depth;
// A row added to the menu being built. A header is a section's title; a value is stepped through by
// left and right, a choice is chosen; an info row only shows something. helpFor says what the row
// just added does.
struct Item* addItem(const char* label, ItemFunction function, int argument);
void addHeader(const char* label);
void addValue(const char* label, const char* value, ItemFunction function, int argument);
void addChoice(const char* label, const char* value, ItemFunction function, int argument);
void addInfo(const char* label, const char* value);
void helpFor(const char* help);
void asGame(void);
void asState(void);
// Opens a menu over the one shown, build making its rows each frame; back to the one before.
void pushMenu(void (*build)(void), const char* title);
void popMenu(void);
// The first menu's tabs, side by side in the header: L and R move between them, and so do left and
// right on a row without a value. menus.c names and builds them; showTab shows one, back on the row it
// was left on, sliding in from way's side.
enum { TAB_GENERAL, TAB_GAME, TAB_CORE, TAB_EXTRAS, TABS };
extern const char* const tabNames[TABS];
extern void (*const tabBuilds[TABS])(void);
void showTab(int which, int way);
// The on-screen keyboard, to type text, starting from text: done gets it if it's accepted; hint,
// under the text, says what goes there (or NULL); hidden shows stars for a password.
void openEntry(const char* title, const char* hint, const char* text, bool hidden, void (*done)(const char* text));
// Headers and rows that only show something can't be selected.
bool selectable(int i);
// value + delta, wrapped round into 0 to count - 1; and words for values.
int wrap(int value, int delta, int count);
const char* onOff(bool on);
const char* frames(int count);
// The menus' look: each accent color for titles and headers, the selection's fill and a held key;
// the sizes, in percent; how dark the game is behind the menus.
struct Accent {
	const char* name;
	SDL_Color accent, fill, held;
};
struct MenuSize {
	const char* name;
	int percent;
};
extern const struct Accent accents[ACCENTS];
extern const struct MenuSize menuSizes[MENU_SIZES];
extern const char* const dims[3];

// menus.c: the game list, whose folder listing is kept until the folder changes, and its letter
// jumps and type-to-find.
extern char browserPath[PATH_SIZE];
extern char browserListed[PATH_SIZE];
void browserMenu(void);
#ifdef __ANDROID__
// Takes Android's folder picker's answer once it has one.
void folderPickerPoll(void);
#endif
void browserJump(int delta);
void browserFind(char c);

#endif
