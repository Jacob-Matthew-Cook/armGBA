/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// armgba, armGBA's own frontend: what its files share. main.c runs the core; settings.c keeps the
// frontend's settings and the core's options; ui.c draws the menus and messages; play.c has save
// states, rewind, run-ahead, screenshots and recording; cheats.c, net.c and cheevos.c the rest.

#pragma once

#include "libretro.h"

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PATH_SIZE = 4096,
	NAME_SIZE = 256,
	// A directory, a file name and its extension.
	FILE_SIZE = PATH_SIZE + NAME_SIZE + 16,
	PORTS = 4,
	BUTTONS = 16,
	// In libretro's order: B is the first button, A the ninth.
	BUTTON_B = 0,
	BUTTON_A = 8,
	RECENT_MAX = 15,
	SLOTS = 10,
	OPTIONS_MAX = 160,
	AUDIO_RATE = 32768,
};

#define FRAME_RATE (16777216.0 / 280896.0)

// What the user can do outside the game, each on a key and, if bound, a controller button.
enum Action {
	ACT_MENU,
	ACT_FAST_FORWARD,
	ACT_REWIND,
	ACT_PAUSE,
	ACT_SAVE_STATE,
	ACT_LOAD_STATE,
	ACT_SLOT_PREVIOUS,
	ACT_SLOT_NEXT,
	ACT_SCREENSHOT,
	ACT_RECORD,
	ACT_FULLSCREEN,
	ACT_RESET,
	ACTIONS,
};

enum Filter {
	FILTER_SHARP,
	FILTER_SMOOTH,
	FILTERS,
};

// The menus' accent colours.
enum { ACCENTS = 7 };

// A controller binding: a button, or a trigger past half way.
enum {
	PAD_NONE = -1,
	PAD_LEFT_TRIGGER = 100,
	PAD_RIGHT_TRIGGER = 101,
};

struct Settings {
	int scale;
	bool fullscreen;
	bool integerScale;
	bool stretch;
	bool vsync;
	int filter;
	// LCD Rendering: the LCD grid and Game Boy pixel shadow drawn here on the GPU, or by the core on the
	// CPU; never both.
	bool lcdOnGpu;
	bool gpuGrid;
	// Where the GPU grid's Game Boy pixels cast their shadow: 0 nowhere, then up and on clockwise.
	int shadow;
	bool showFps;
	int volume;
	int latency;
	int runahead;
	bool rewind;
	int rewindMegabytes;
	bool autoState;
	SDL_Scancode keys[BUTTONS];
	int pad[BUTTONS];
	SDL_Scancode hotkeys[ACTIONS];
	int padHotkeys[ACTIONS];
	char nickname[32];
	char host[128];
	int netPort;
	int inputDelay;
	char cheevosUser[64];
	char cheevosToken[64];
	bool hardcore;
	char directory[PATH_SIZE];
	// The menus' look: the accent colour, their size as a percentage, and how far the game behind them
	// is dimmed (0 to 2).
	int accent;
	int menuSize;
	int dim;
};

struct Option {
	char* key;
	char* value;
	char* defaultValue;
	char* description;
	// What it does, a sentence or two, shown under the menu.
	char* info;
	char* category;
	char** values;
	char** labels;
	int valueCount;
	bool visible;
};

struct Category {
	char* key;
	char* description;
};

extern struct Settings settings;
extern struct Option options[OPTIONS_MAX];
extern int optionCount;
extern struct Category categories[32];
extern int categoryCount;
extern bool optionsChanged;

extern const char* actionNames[ACTIONS];
extern const char* buttonNames[BUTTONS];

// Directories, made at start.
extern char configDirectory[PATH_SIZE];
extern char systemDirectory[PATH_SIZE];
extern char saveDirectory[PATH_SIZE];
extern char stateDirectory[PATH_SIZE];
extern char shotDirectory[PATH_SIZE];
extern char cheatDirectory[PATH_SIZE];

// main.c
struct App {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* game;
	int gameWidth, gameHeight;
	enum retro_pixel_format format;
	struct retro_game_geometry geometry;
	// The last picture, as the core gave it, for screenshots and recording.
	uint8_t* picture;
	size_t picturePitch;
	bool loaded;
	char gamePath[PATH_SIZE];
	char gameName[NAME_SIZE];
	const void* gameData;
	size_t gameSize;
	bool paused;
	bool menuOpen;
	bool running;
	bool fastForward;
	bool rewinding;
	int slot;
	// What the core may put out now: RETRO_AV_ENABLE_* bits, cleared while a frame is run again.
	int avEnable;
	double fps;
	SDL_GameController* pads[PORTS];
};
extern struct App app;

bool loadGame(const char* path);
void unloadGame(void);
void recreateRenderer(void);
void applyWindowSize(void);
void refreshShownOptions(void);
void writeSaves(void);
void runFrames(void);
void setPadForPort(void);
uint16_t localButtons(unsigned port);

// settings.c
void settingsDefaults(void);
void settingsRead(void);
void settingsWrite(void);
// The games loaded last, newest first, kept in recent.txt.
void recentAdd(const char* path);
int recentList(char (*out)[PATH_SIZE], int max);
void optionsReadSaved(void);
void optionsWrite(void);
void optionsDefine(const struct retro_core_options_v2* definitions);
void optionsDisplay(const struct retro_core_option_display* display);
struct Option* optionFind(const char* key);
void optionSet(struct Option* option, const char* value);
const char* scancodeName(SDL_Scancode code);
const char* padName(int binding);

// ui.c
bool uiInit(void);
void uiRecreateTextures(void);
void uiOpenMenu(void);
void uiCloseMenu(void);
bool uiEvent(const SDL_Event* event);
void uiDraw(void);
void osd(const char* format, ...);
void uiCaptureBinding(int which, bool controller, bool hotkey);

// play.c
bool saveState(int slot);
bool loadState(int slot);
bool stateExists(int slot);
// Saves or loads the chosen slot from a hotkey or the menu, saying how it went.
bool stateCommand(bool load);
// The game as it was before the last state was loaded, until the game closes.
bool canUndoLoad(void);
bool undoLoadState(void);
void undoForget(void);
void rewindReset(void);
void rewindPush(void);
bool rewindStep(void);
void runAheadFrame(void);
void takeScreenshot(void);
bool recordToggle(void);
bool recording(void);
void recordVideo(const void* data, unsigned width, unsigned height, size_t pitch);
void recordAudio(const int16_t* data, size_t frames);
void recordStop(void);

// cheats.c
struct Cheat {
	char description[128];
	char code[256];
	bool enabled;
};
extern struct Cheat* cheats;
extern int cheatCount;
void cheatListLoad(void);
void cheatListSave(void);
void cheatListApply(void);
void cheatListAdd(const char* description, const char* code);
void cheatListRemove(int index);
// Adds a RetroArch .cht file's cheats to the game's; returns how many, or -1 if it can't be read.
int cheatListImport(const char* path);

// net.c
enum NetMode {
	NET_OFF,
	NET_HOSTING,
	NET_JOINED,
};
extern enum NetMode netMode;
extern bool netTogether;
extern char netStatus[256];
bool netHost(bool together);
bool netJoin(const char* address);
void netStop(void);
void netFrame(void);
bool netPacketInterface(const struct retro_netpacket_callback* callback);
int netPlayers(void);
bool netRunning(void);
uint16_t netInput(unsigned port, unsigned id);
bool netRollingBack(void);
void netInit(void);

// cheevos.c
void cheevosInit(void);
void cheevosLogin(const char* user, const char* password);
void cheevosLogout(void);
void cheevosLoadGame(void);
void cheevosUnloadGame(void);
void cheevosFrame(void);
void cheevosReset(void);
void cheevosSetHardcore(bool on);
void cheevosIdle(void);
void cheevosSetMemoryMaps(const struct retro_memory_map* maps);
bool cheevosHardcore(void);
int cheevosList(char lines[][160], int most);
const char* cheevosStatus(void);
