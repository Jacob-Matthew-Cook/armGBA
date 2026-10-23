/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The menus, messages and on-screen keyboard, drawn over the game. Every menu is built again each
// frame from the settings it shows, so it is never out of date; only a folder's listing is kept, read
// again when another folder is opened. The keyboard, a controller and the
// mouse all work: arrows or the D-pad or stick move, Enter or A or a click chooses, left and right
// change a value, Esc or B or a right-click goes back.

#include "frontend.h"
#include "ui.h"

#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lcd.h"
#include "text.h"

enum {
	// Rows a menu shows at once, at most.
	ROWS_MAX = 128,
	MESSAGES = 4,
	MESSAGE_MS = 3000,
	// Holding a direction on a controller repeats it after this long, then this often.
	REPEAT_DELAY_MS = 400,
	REPEAT_MS = 90,
	CIRCLES = 16,
	// How long a tab takes to slide in.
	SLIDE_MS = 180,
};

// The menu shown, built again each frame, and the menus opened to reach it.
struct Item* items;
static int itemCapacity;
int itemCount;
struct Menu stack[DEPTH_MAX];
int depth;
// Where each item was last drawn, for the mouse.
static SDL_Rect itemRects[ROWS_MAX];
static int firstShown, shownCount;
// Where a selected value's arrows were drawn, or -1.
static int arrowLeft[ROWS_MAX], arrowRight[ROWS_MAX];
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


// A message at the bottom left for a few seconds, the newest lowest, unless Show Messages is off;
// stderr gets it too.
void osd(const char* format, ...) {
	for (int i = MESSAGES - 1; i > 0; --i) {
		messages[i] = messages[i - 1];
	}
	va_list args;
	va_start(args, format);
	vsnprintf(messages[0].text, sizeof(messages[0].text), format, args);
	va_end(args);
	messages[0].until = settings.showMessages ? SDL_GetTicks() + MESSAGE_MS : 0;
	fprintf(stderr, "armgba: %s\n", messages[0].text);
}

// Shapes: rounded rectangles are a circle's quarters at the corners and plain rectangles between.

static struct {
	int radius;
	SDL_Texture* texture;
} circles[CIRCLES];
static int nextCircle;

// After the renderer is made again: the textures went with the old one.
void uiRecreateTextures(void) {
	textReset();
	for (int i = 0; i < CIRCLES; ++i) {
		if (circles[i].texture) {
			SDL_DestroyTexture(circles[i].texture);
		}
		circles[i].texture = NULL;
	}
}

// The game list starts in the games folder the settings name.
bool uiInit(void) {
	snprintf(browserPath, sizeof(browserPath), "%s", settings.directory);
	return textInit();
}

// A white circle of radius, its edge antialiased, as a texture to tint. The last few sizes are kept.
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

static void fillRect(int x, int y, int w, int h, SDL_Color color) {
	SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
	SDL_Rect const r = { x, y, w, h };
	SDL_RenderFillRect(app.renderer, &r);
}

// A plain rectangle if the circle can't be made.
static void fillRounded(SDL_Rect r, int radius, SDL_Color color) {
	radius = SDL_min(radius, SDL_min(r.w, r.h) / 2);
	SDL_Texture* texture = radius > 0 ? circle(radius) : NULL;
	if (!texture) {
		fillRect(r.x, r.y, r.w, r.h, color);
		return;
	}
	SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
	SDL_SetTextureAlphaMod(texture, color.a);
	for (int i = 0; i < 4; ++i) {
		int const right = i & 1, bottom = i >> 1;
		SDL_Rect const from = { right * radius, bottom * radius, radius, radius };
		SDL_Rect const to = { right ? r.x + r.w - radius : r.x, bottom ? r.y + r.h - radius : r.y, radius, radius };
		SDL_RenderCopy(app.renderer, texture, &from, &to);
	}
	fillRect(r.x + radius, r.y, r.w - 2 * radius, r.h, color);
	fillRect(r.x, r.y + radius, radius, r.h - 2 * radius, color);
	fillRect(r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius, color);
}

// A rounded rectangle with an edge of another color.
static void fillBordered(SDL_Rect r, int radius, SDL_Color fill, SDL_Color edge, int edgeWidth) {
	fillRounded(r, radius, edge);
	fillRounded((SDL_Rect) { r.x + edgeWidth, r.y + edgeWidth, r.w - 2 * edgeWidth, r.h - 2 * edgeWidth }, radius - edgeWidth, fill);
}

// The menus' building blocks.

// A row added to the menu being built. If the list can't grow, it goes to a spare that isn't shown.
struct Item* addItem(const char* label, ItemFunction function, int argument) {
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

// A section's title, not selectable.
void addHeader(const char* label) {
	addItem(label, NULL, 0)->header = true;
}

// A value that left and right step through.
void addValue(const char* label, const char* value, ItemFunction function, int argument) {
	struct Item* item = addItem(label, function, argument);
	snprintf(item->value, sizeof(item->value), "%s", value);
	item->adjustable = true;
}

// A value that is chosen, not stepped through.
void addChoice(const char* label, const char* value, ItemFunction function, int argument) {
	struct Item* item = addItem(label, function, argument);
	snprintf(item->value, sizeof(item->value), "%s", value);
}


// What the item just added does.
void helpFor(const char* help) {
	if (itemCount) {
		items[itemCount - 1].help = help;
	}
}

// The item chosen once of those that ask twice; choosing it again does it, anything else forgets it.
static struct {
	ItemFunction function;
	int argument;
} armed;

static void disarm(void) {
	armed.function = NULL;
}

// A row that only shows something, not selectable.
void addInfo(const char* label, const char* value) {
	struct Item* item = addItem(label, NULL, 0);
	snprintf(item->value, sizeof(item->value), "%s", value ? value : "");
}

// Opens a menu over the one shown; build makes its rows each frame.
void pushMenu(void (*build)(void), const char* title) {
	disarm();
	if (depth < DEPTH_MAX) {
		stack[depth].build = build;
		snprintf(stack[depth].title, sizeof(stack[depth].title), "%s", title);
		stack[depth].selected = -1;
		stack[depth].scroll = 0;
		++depth;
	}
}

// The tab shown, the row each was left on, and when and from which side the last one slid in.
static int tab;
static struct {
	int selected, scroll;
} tabSpots[TABS];
static Uint32 slideStart;
static int slideWay;
// Where each tab's name was drawn, for the mouse.
static SDL_Rect tabRects[TABS];

void showTab(int which, int way) {
	disarm();
	if (depth) {
		tabSpots[tab].selected = stack[0].selected;
		tabSpots[tab].scroll = stack[0].scroll;
	}
	tab = which;
	depth = 1;
	stack[0].build = tabBuilds[which];
	snprintf(stack[0].title, sizeof(stack[0].title), "%s", tabNames[which]);
	stack[0].selected = tabSpots[which].selected;
	stack[0].scroll = tabSpots[which].scroll;
	slideWay = way;
	slideStart = SDL_GetTicks();
}

static void moveTab(int way) {
	showTab(wrap(tab, way, TABS), way);
}

// Back to the menu before, or out of the menus from the first.
void popMenu(void) {
	disarm();
	if (depth > 1) {
		--depth;
	} else {
		uiCloseMenu();
	}
}

// The on-screen keyboard, to type text, starting from text: done gets what was typed if it's accepted.
// hidden shows stars for a password.
void openEntry(const char* title, const char* text, bool hidden, void (*done)(const char* text)) {
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

const char* onOff(bool on) {
	return on ? "On" : "Off";
}

const char* frames(int count) {
	return count == 1 ? "frame" : "frames";
}

// value + delta, wrapped round into 0 to count - 1.
int wrap(int value, int delta, int count) {
	return ((value + delta) % count + count) % count;
}

// The menus' look: each accent color for titles and headers, the selection's fill and a held key;
// the sizes, in percent; how dark the game is behind the menus.
const struct Accent accents[ACCENTS] = {
	{ "Blue", { 111, 168, 255, 255 }, { 58, 120, 232, 255 }, { 47, 74, 122, 255 } },
	{ "Purple", { 182, 146, 255, 255 }, { 120, 84, 220, 255 }, { 76, 58, 124, 255 } },
	{ "Pink", { 255, 136, 196, 255 }, { 206, 62, 138, 255 }, { 118, 46, 86, 255 } },
	{ "Red", { 255, 128, 118, 255 }, { 200, 58, 52, 255 }, { 116, 44, 42, 255 } },
	{ "Orange", { 255, 180, 96, 255 }, { 204, 106, 28, 255 }, { 118, 72, 34, 255 } },
	{ "Green", { 116, 222, 152, 255 }, { 36, 146, 88, 255 }, { 40, 94, 66, 255 } },
	{ "Teal", { 96, 214, 222, 255 }, { 22, 136, 148, 255 }, { 30, 90, 100, 255 } },
};
const struct MenuSize menuSizes[MENU_SIZES] = { { "Small", 100 }, { "Normal", 120 }, { "Large", 140 }, { "Larger", 160 } };
const char* const dims[3] = { "Light", "Medium", "Dark" };
// Dark enough, as the menus' own background, for their text to read over a bright game.
static const Uint8 dimAlphas[3] = { 175, 210, 240 };

// Back to the game, closing the keyboard and any binding waited for.
void uiCloseMenu(void) {
	if (!app.loaded) {
		// Nothing to go back to: the menu stays.
		depth = 1;
		return;
	}
	// The menus open again where they were left.
	app.menuOpen = false;
	entry.open = false;
	capture.open = false;
	SDL_StopTextInput();
}

// Waits for a key or a controller's button for button which, or hotkey which.
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

// Headers and rows that only show something can't be selected.
bool selectable(int i) {
	return i >= 0 && i < itemCount && !items[i].header && items[i].function;
}

// The next selectable row the way delta points, wrapping round.
static void moveSelection(int delta) {
	disarm();
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

// The selected row's function, chosen (delta 0) or changed by delta. A row that asks twice is armed
// the first time it's chosen.
static void activate(int delta) {
	struct Menu* m = &stack[depth - 1];
	if (!selectable(m->selected)) {
		return;
	}
	struct Item* item = &items[m->selected];
	if (delta && !item->adjustable) {
		return;
	}
	if (!delta && item->confirm && (armed.function != item->function || armed.argument != item->argument)) {
		armed.function = item->function;
		armed.argument = item->argument;
		return;
	}
	disarm();
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

// Whether left and right move between the tabs: on a tab, unless the row selected has a value.
static bool tabsMove(void) {
	struct Menu const* m = &stack[depth - 1];
	return depth == 1 && !(selectable(m->selected) && items[m->selected].adjustable);
}

// On the keyboard a direction moves between keys; in a menu up and down select, left and right change
// a value or the tab.
static void direction(int dx, int dy) {
	if (entry.open) {
		entryMove(dx, dy);
	} else if (dy) {
		moveSelection(dy);
	} else if (dx && tabsMove()) {
		moveTab(dx);
	} else if (dx) {
		activate(dx);
	}
}

// A or Enter: types the key, or chooses the row.
static void confirm(void) {
	if (entry.open) {
		entryType();
	} else {
		activate(0);
	}
}

// B or Esc: closes the keyboard, or goes back a menu.
static void back(void) {
	if (entry.open) {
		closeEntry(false);
	} else {
		popMenu();
	}
}

static bool inBrowser(void) {
	return depth && stack[depth - 1].build == browserMenu;
}

// The controller button bound to the game's button: B's or A's own if it's bound to a trigger or not
// at all.
static int menuButton(int button) {
	int const bound = settings.pad[button];
	if (bound >= 0 && bound < SDL_CONTROLLER_BUTTON_MAX) {
		return bound;
	}
	return button == BUTTON_A ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
}

// An event while the menus are open or a binding is waited for; returns whether it was used.
bool uiEvent(const SDL_Event* event) {
	if (event->type == SDL_CONTROLLERBUTTONDOWN || (event->type == SDL_CONTROLLERAXISMOTION && abs(event->caxis.value) > 20000)) {
		lastInputPad = true;
	} else if (event->type == SDL_KEYDOWN || event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEWHEEL) {
		lastInputPad = false;
	}
	if (capture.open) {
		// Waiting for a binding: Esc cancels, Delete clears it, and a key or button (a trigger too) binds.
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
	// Text typed into the keyboard, from the system's own.
	case SDL_TEXTINPUT:
		if (entry.open && strlen(entry.text) + strlen(event->text.text) < sizeof(entry.text)) {
			strcat(entry.text, event->text.text);
		}
		return true;
	case SDL_KEYDOWN: {
		SDL_Scancode const code = event->key.keysym.scancode;
		if (entry.open) {
			// The keyboard takes editing keys, arrows and Ctrl+V.
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
		// Ten rows at a time.
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
		// Tab, Shift+Tab, Q and E move between the tabs.
		case SDL_SCANCODE_TAB:
			if (depth == 1) {
				moveTab(event->key.keysym.mod & KMOD_SHIFT ? -1 : 1);
			}
			break;
		case SDL_SCANCODE_ESCAPE:
		case SDL_SCANCODE_BACKSPACE:
			back();
			break;
		// The Full Screen hotkey works in the menus too; in the game list, letters find a name.
		default: {
			SDL_Keycode const key = event->key.keysym.sym;
			if (code == settings.hotkeys[ACT_FULLSCREEN]) {
				settings.fullscreen = !settings.fullscreen;
				applyWindowSize();
			} else if (depth == 1 && (code == SDL_SCANCODE_Q || code == SDL_SCANCODE_E)) {
				moveTab(code == SDL_SCANCODE_Q ? -1 : 1);
			} else if (inBrowser() && key < 128 && isalnum((int) key)) {
				browserFind((char) key);
			}
			break;
		}
		}
		return true;
	}
	case SDL_CONTROLLERBUTTONDOWN:
		// The buttons bound to the game's A and B choose and go back, as in RetroArch.
		if (event->cbutton.button == menuButton(BUTTON_A)) {
			confirm();
			return true;
		}
		if (event->cbutton.button == menuButton(BUTTON_B)) {
			back();
			return true;
		}
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
		// On the keyboard, X deletes and Start is Done.
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
		// L and R move between the tabs, jump a letter in the game list, or ten rows elsewhere.
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: {
			int const way = event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? -1 : 1;
			if (depth == 1 && !entry.open) {
				moveTab(way);
			} else if (inBrowser()) {
				browserJump(way);
			} else {
				for (int i = 0; i < 10; ++i) {
					moveSelection(way);
				}
			}
			break;
		}
		default:
			// The menu hotkey closes the menus.
			if (event->cbutton.button == settings.padHotkeys[ACT_MENU]) {
				uiCloseMenu();
			}
			break;
		}
		return true;
	case SDL_CONTROLLERAXISMOTION:
		// The left stick moves as the D-pad does; uiDraw repeats it while it's held.
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
	// The mouse selects the row it's over.
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
		// A click on the keyboard types the key.
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
			for (int t = 0; t < TABS && depth == 1; ++t) {
				if (t != tab && SDL_PointInRect(&p, &tabRects[t])) {
					showTab(t, t > tab ? 1 : -1);
					return true;
				}
			}
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
	// The wheel scrolls three rows at a time.
	case SDL_MOUSEWHEEL:
		if (!entry.open) {
			stack[depth - 1].scroll -= event->wheel.y * 3;
		}
		return true;
	default:
		return false;
	}
}

// Drawing: the menus fill the window over the dimmed game, sized from a unit u that grows with the
// window.

static const SDL_Color white = { 255, 255, 255, 255 }, text = { 214, 220, 234, 255 }, muted = { 143, 155, 181, 255 },
                       panelFill = { 20, 26, 38, 240 }, edge = { 42, 53, 80, 255 }, keyFill = { 34, 43, 63, 255 };
// The accent color chosen, set as each frame is drawn.
static SDL_Color accent, pillFill, keyHeld;

static SDL_Color faded(SDL_Color c, int alpha) {
	c.a = (Uint8) (c.a * alpha / 255);
	return c;
}

// A 24th of the height or a 40th of the width, whichever is less, times the menu size.
static int unitFor(int width, int height) {
	return SDL_max(10, SDL_min(height / 24, width / 40) * settings.menuSize / 100);
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

// A badge and what it does, in the footer; returns where the next one goes.
static int drawHint(int x, int y, int u, const char* badge, bool round, const char* label) {
	int const size = u * 7 / 10, h = u + u / 8;
	x += drawBadge(x, y, u, badge, round) + u / 3;
	x += textDraw(FACE_BODY, size, x, y + (h - size) / 2, muted, label);
	return x + u;
}

// The header (ARMGBA and where you are) across the window, panel; returns where the content starts.
static int drawPanel(SDL_Rect panel, int u, const char* where) {
	int const headerH = u * 3, inset = u + u / 2;
	int const titleSize = u * 3 / 2, whereSize = u * 4 / 5;
	int const brand = textDraw(FACE_TITLE, titleSize, panel.x + inset, panel.y + (headerH - titleSize) / 2, accent, "ARMGBA");
	char fitted[512];
	textFit(FACE_BODY, whereSize, where, panel.w - 2 * inset - brand - u, true, fitted, sizeof(fitted));
	int const ww = textWidth(FACE_BODY, whereSize, fitted);
	textDraw(FACE_BODY, whereSize, panel.x + panel.w - inset - ww, panel.y + (headerH - whereSize) / 2, muted, fitted);
	fillRect(panel.x + u, panel.y + headerH, panel.w - 2 * u, 1, edge);
	return panel.y + headerH + u / 4;
}

// Help text in up to two lines, words wrapped, the second cut short with "..." if it runs on.
static void drawHelp(const char* help, int x, int y, int width, int size, int line) {
	char first[512] = "";
	const char* rest = help;
	const char* p = help;
	while (*p) {
		const char* end = p;
		while (*end && *end != ' ') {
			++end;
		}
		char trial[512];
		snprintf(trial, sizeof(trial), "%.*s", (int) (end - help), help);
		if (textWidth(FACE_BODY, size, trial) > width && first[0]) {
			break;
		}
		snprintf(first, sizeof(first), "%s", trial);
		rest = *end ? end + 1 : end;
		p = rest;
	}
	textDraw(FACE_BODY, size, x, y, muted, first);
	if (*rest) {
		char second[512];
		textFit(FACE_BODY, size, rest, width, false, second, sizeof(second));
		textDraw(FACE_BODY, size, x, y + line, muted, second);
	}
}

// The tabs, right of ARMGBA in the header: the one shown in the accent color and underlined.
static void drawTabs(SDL_Rect panel, int u) {
	int const size = u * 4 / 5, gap = u * 5 / 4, headerH = u * 3, inset = u + u / 2;
	int const y = panel.y + (headerH - size) / 2;
	int x = panel.x + panel.w - inset;
	for (int i = TABS - 1; i >= 0; --i) {
		int const w = textWidth(FACE_STRONG, size, tabNames[i]);
		x -= w;
		textDraw(FACE_STRONG, size, x, y, i == tab ? accent : muted, tabNames[i]);
		if (i == tab) {
			fillRect(x, y + size + u / 4, w, SDL_max(2, u / 8), accent);
		}
		tabRects[i] = (SDL_Rect) { x - gap / 2, panel.y, w + gap, headerH };
		x -= gap;
	}
}

static int footerTop(SDL_Rect panel, int u) {
	return panel.y + panel.h - u * 9 / 4;
}

// The footer's line; returns where its hints go.
static int drawFooter(SDL_Rect panel, int u) {
	int const top = footerTop(panel, u);
	fillRect(panel.x + u, top, panel.w - 2 * u, 1, edge);
	return top + (u * 9 / 4 - (u + u / 8)) / 2;
}

// The messages, newest lowest, each fading out over its last 0.4 seconds.
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

// The on-screen keyboard: the text typed, as stars for a password, with a blinking caret, then the
// keys. The last row's four keys are three wide.
static void drawEntry(int width, int height, int u) {
	int const fieldH = u * 2, keyH = u * 7 / 4, keySize = u * 4 / 5;
	SDL_Rect const panel = { 0, 0, width, height };
	int const top = drawPanel(panel, u, entry.title);
	// The field and keys, as wide as 28 units, in the middle of the room between header and footer.
	int const inner = SDL_min(width - 2 * u, u * 28), left = (width - inner) / 2;
	int y = top + SDL_max(u / 4, (footerTop(panel, u) - top - (fieldH + u / 2 + 5 * keyH)) / 2);
	fillBordered((SDL_Rect) { left, y, inner, fieldH }, u / 2, (SDL_Color) { 13, 18, 28, 255 }, accent, 2);
	// The text, as stars when hidden, its start cut off if it's too long.
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
	// The caret: half a second on, half off.
	if (SDL_GetTicks() / 500 % 2 == 0) {
		fillRect(left + u * 2 / 3 + tw + 1, y + (fieldH - keySize) / 2, 2, keySize, accent);
	}
	y += fieldH + u / 2;
	// Each key's rectangle is kept for the mouse, the last row's for each of the columns it spans.
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
			fillRounded(*rect, u / 4, on ? pillFill : shiftOn ? keyHeld : keyFill);
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

// Asks for the key or button to bind, over the menu.
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

// The menu on top: its rows, the help for the selected one and the footer's hints.
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
	int const size = u * 19 / 20, headerSize = u;
	int rowH = u * 13 / 8;
	// Two lines above the footer say what the selected item does, when this menu's items say.
	bool anyHelp = false;
	for (int i = 0; i < itemCount && !anyHelp; ++i) {
		anyHelp = items[i].help != NULL;
	}
	int const helpSize = u * 13 / 20, helpLine = u * 9 / 10, helpH = anyHelp ? helpLine * 2 + u / 2 : 0;
	int const chrome = u * 3 + u / 4 + u / 4 + u * 9 / 4 + helpH;
	// A list longer than the room takes the nearest whole number of rows, a little shorter or taller
	// to fill it to the help text.
	int const room = height - chrome, fit = SDL_max(1, (room + rowH / 2) / rowH);
	int const rows = SDL_min(SDL_min(itemCount, ROWS_MAX), fit);
	if (rows == fit) {
		rowH = room / rows;
	}
	SDL_Rect const panel = { 0, 0, width, height };
	// Where you are, the menu before and this one; on a tab, the tabs.
	char where[512] = "";
	if (depth > 1) {
		snprintf(where, sizeof(where), "%s \xE2\x80\xBA %s", stack[depth - 2].title, stack[depth - 1].title);
	}
	int const top = drawPanel(panel, u, where);
	if (depth == 1) {
		drawTabs(panel, u);
	}
	// Keep the selection in view; the mouse wheel moves the view too.
	if (m->selected >= 0 && (m->selected < m->scroll || m->selected >= m->scroll + rows)) {
		m->scroll = m->selected < m->scroll ? m->selected : m->selected - rows + 1;
	}
	m->scroll = SDL_max(0, SDL_min(m->scroll, itemCount - rows));
	// A scroll bar takes room on the right when the rows don't all fit.
	bool const scrolls = itemCount > rows;
	// A tab just moved to slides in from its side, slowing as it comes.
	int slide = 0;
	Uint32 const since = SDL_GetTicks() - slideStart;
	if (depth == 1 && slideWay && since < SLIDE_MS) {
		float const left = 1 - (float) since / SLIDE_MS;
		slide = (int) (slideWay * u * 6 * left * left);
	}
	int const pillL = panel.x + u + slide, pillR = panel.x + panel.w - u - (scrolls ? u / 2 : 0) + slide;
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
		// A header: bold, a little larger than the rows under it, toward the foot of its row.
		if (item->header) {
			char header[NAME_SIZE + 64];
			textFit(FACE_STRONG, headerSize, item->label, textR - textL, false, header, sizeof(header));
			textDraw(FACE_STRONG, headerSize, textL, y + rowH - headerSize - u / 4, accent, header);
			continue;
		}
		bool const on = i == m->selected;
		enum FaceId const face = on ? FACE_STRONG : FACE_BODY;
		int const ty = y + (rowH - size) / 2;
		int valueL = textR;
		// An item that asks twice says so once chosen.
		char again[32];
		const char* shownValue = item->value;
		if (on && item->confirm && armed.function == item->function && armed.argument == item->argument) {
			snprintf(again, sizeof(again), "Press %s again", lastInputPad ? "A" : "Enter");
			shownValue = again;
		}
		// A value is right-aligned; with arrows round it, kept for clicks, when it's selected and can change.
		if (shownValue[0]) {
			char value[NAME_SIZE + 64];
			int const arrows = on && item->adjustable ? textWidth(face, size, "\xE2\x80\xB9") + u / 2 : 0;
			textFit(face, size, shownValue, (textR - textL) / 2 - 2 * arrows, false, value, sizeof(value));
			int const vw = textWidth(face, size, value);
			SDL_Color const color = on ? white : item->function ? text : muted;
			if (arrows) {
				int const aw = textWidth(face, size, "\xE2\x80\xBA");
				textDraw(face, size, textR - aw, ty, white, "\xE2\x80\xBA");
				arrowRight[shownCount] = textR - aw - u / 4;
				valueL = textR - arrows - vw;
				textDraw(face, size, valueL, ty, color, value);
				valueL -= arrows;
				textDraw(face, size, valueL, ty, white, "\xE2\x80\xB9");
				arrowLeft[shownCount] = valueL + aw + u / 4;
			} else {
				valueL = textR - vw;
				textDraw(face, size, valueL, ty, item->adjustable && !on ? muted : color, value);
			}
		}
		char label[NAME_SIZE + 64];
		// The label, cut to fit before the value.
		textFit(face, size, item->label, valueL - textL - u, item->label[0] == '/', label, sizeof(label));
		textDraw(face, size, textL, ty, on ? white : item->function ? text : muted, label);
	}
	// The scroll bar.
	if (scrolls) {
		int const trackH = rows * rowH, barH = SDL_max(u, trackH * rows / itemCount);
		int const barY = top + (trackH - barH) * m->scroll / SDL_max(1, itemCount - rows);
		fillRounded((SDL_Rect) { panel.x + panel.w - u, barY, SDL_max(3, u / 6), barH }, u, faded(accent, 110));
	}
	// The selected row's help, above the footer.
	if (helpH && selectable(m->selected) && items[m->selected].help) {
		drawHelp(items[m->selected].help, textL, footerTop(panel, u) - helpH + u / 4, textR - textL, helpSize, helpLine);
	}
	int x = panel.x + u + u / 2;
	int const y = drawFooter(panel, u);
	// The footer's hints, for a controller or the keyboard, whichever was used last.
	bool const adjustable = selectable(m->selected) && items[m->selected].adjustable;
	const char* backLabel = depth > 1 ? "Back" : app.loaded ? "Close" : NULL;
	if (lastInputPad) {
		x = drawHint(x, y, u, "A", true, "Select");
		if (backLabel) {
			x = drawHint(x, y, u, "B", true, backLabel);
		}
		if (adjustable) {
			x = drawHint(x, y, u, "\xE2\x80\xB9 \xE2\x80\xBA", false, "Change");
		}
		if (inBrowser()) {
			drawHint(x, y, u, "L R", false, "Letter");
		} else if (depth == 1) {
			drawHint(x, y, u, "L R", false, "Tabs");
		}
	} else {
		x = drawHint(x, y, u, "Enter", false, "Select");
		if (backLabel) {
			x = drawHint(x, y, u, "Esc", false, backLabel);
		}
		if (adjustable) {
			x = drawHint(x, y, u, "\xE2\x86\x90 \xE2\x86\x92", false, "Change");
		}
		if (inBrowser()) {
			drawHint(x, y, u, "A-Z", false, "Find");
		} else if (depth == 1) {
			drawHint(x, y, u, "Q E", false, "Tabs");
		}
	}
}

// The menus, or the keyboard, over the dimmed game; then the frame rate and messages over everything.
void uiDraw(void) {
	int width, height;
	SDL_GetRendererOutputSize(app.renderer, &width, &height);
	int const u = unitFor(width, height);
	accent = accents[settings.accent].accent;
	pillFill = accents[settings.accent].fill;
	keyHeld = accents[settings.accent].held;
	// A held controller direction repeats.
	// A held direction repeats, but doesn't run on through the tabs.
	bool const tabbing = held.dx && !held.dy && !entry.open && depth && tabsMove();
	if ((held.dx || held.dy) && !tabbing && SDL_TICKS_PASSED(SDL_GetTicks(), held.next) && app.menuOpen) {
		direction(held.dx, held.dy);
		held.next = SDL_GetTicks() + REPEAT_MS;
	}
	if (app.menuOpen && depth) {
		fillRect(0, 0, width, height, app.loaded ? (SDL_Color) { panelFill.r, panelFill.g, panelFill.b, dimAlphas[settings.dim] } : (SDL_Color) { 11, 15, 24, 255 });
		if (entry.open) {
			drawEntry(width, height, u);
		} else {
			drawMenu(width, height, u);
			if (capture.open) {
				drawCapture(width, height, u);
			}
		}
	// A binding waited for ends with the menus.
	} else if (capture.open) {
		capture.open = false;
	}
	// The frame rate, at the top right.
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
