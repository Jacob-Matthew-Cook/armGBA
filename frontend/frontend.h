/* Copyright (c) 2026 Magnominus
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
	// Game Boy, Game Boy Color, Game Boy Advance.
	CONSOLES = 3,
	BUTTONS = 16,
	// In libretro's order: B is the first button, A the ninth.
	BUTTON_B = 0,
	BUTTON_A = 8,
	RECENT_MAX = 15,
	SLOTS = 100,
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
	ACT_QUIT,
	ACT_FPS,
	ACTIONS,
};

enum Filter {
	FILTER_SHARP,
	FILTER_SMOOTH,
	FILTERS,
};

// The menus' accent colors.
enum { ACCENTS = 7 };

// A controller binding: a button, or a trigger past half way.
enum {
	PAD_NONE = -1,
	// The Hotkey on Auto: Home on a controller that has one, otherwise Select.
	PAD_AUTO = -2,
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
	// Where the GPU grid's Game Boy pixels cast their shadow: 0 nowhere, then up and on clockwise, then
	// SHADOW_TILT, the way the device tilts.
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
	// A controller's hotkeys: the Hotkey held with each action's own button, as on ROCKNIX.
	int padHotkey;
	int padHotkeys[ACTIONS];
	char nickname[32];
	char host[128];
	int netPort;
	int inputDelay;
	char cheevosUser[64];
	char cheevosToken[64];
	bool hardcore;
	// The last game's folder.
	char directory[PATH_SIZE];
	// The Games folder and its consoles' (Game Boy, Game Boy Color, Game Boy Advance): "" for Auto, found
	// by name, or a folder chosen.
	char gamesFolder[PATH_SIZE];
	char consoleFolders[CONSOLES][PATH_SIZE];
	// The Folders: "" for Auto (armGBA's for states and screenshots), "armgba" or "retroarch" for
	// theirs, or a folder typed in.
	char biosFolder[PATH_SIZE];
	char saveFolder[PATH_SIZE];
	char stateFolder[PATH_SIZE];
	char shotFolder[PATH_SIZE];
	// Fast-Forward Speed: times as fast, 0 for as fast as it goes (up to 12).
	int fastForward;
	bool showMessages;
	// On-Screen Controls (Android), shown while no controller is connected.
	bool touchControls;
	// Swap A/B and X/Y: off, A is the right face button, as on a Game Boy and in RetroArch; on, A is
	// the bottom one, on every controller.
	bool swapFaceButtons;
	// Each player's controller, as SDL's GUID, put back in its place when it connects; "" for any.
	char playerPads[PORTS][33];
	// The menus' look: the accent color, their size as a percentage, and how far the game behind them
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
	// What it holds, a sentence, shown under the menu.
	char* info;
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
// The BIOS, save, state and screenshot folders in use, chosen by the Folders settings (the save folder
// as each game loads), and armGBA's own.
extern char systemDirectory[PATH_SIZE];
// The Games folder and its consoles' as chosen, "" for a console's not found.
extern char gamesDirectory[PATH_SIZE];
extern char consoleDirectories[CONSOLES][PATH_SIZE];
extern const char* const consoleNames[CONSOLES];
extern char ownSystemDirectory[PATH_SIZE];
extern char saveDirectory[PATH_SIZE];
extern char ownSaveDirectory[PATH_SIZE];
extern char stateDirectory[PATH_SIZE];
extern char ownStateDirectory[PATH_SIZE];
extern char shotDirectory[PATH_SIZE];
extern char ownShotDirectory[PATH_SIZE];
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
	// The core started with no game, a Game Boy Advance with its cartridge out.
	bool noCartridge;
	// The game ran a session this device joined in Off or Together, the host's game, so no state of it
	// is kept on exit; and that session ended, so it closes, unless Keep Playing on Disconnect keeps it
	// going.
	bool hostsGame;
	bool sessionEnded;
	char gamePath[PATH_SIZE];
	char gameName[NAME_SIZE];
	const void* gameData;
	size_t gameSize;
	bool paused;
	bool menuOpen;
	bool running;
	bool fastForward;
	// Fast-forward turned on by the controller's hotkey, until it's pressed again.
	bool fastForwardOn;
	bool rewinding;
	int slot;
	// What the core may put out now: RETRO_AV_ENABLE_* bits, cleared while a frame is run again.
	int avEnable;
	// What the states saved now are for, as the core is told: kept in a file, or only for this session
	// (rewind, run-ahead and Undo Load State), which the core may save exactly.
	int stateContext;
	double fps;
	SDL_GameController* pads[PORTS];
};
extern struct App app;

bool loadGame(const char* path);
bool loadNoCartridge(void);
void unloadGame(void);
void recreateRenderer(void);
void applyWindowSize(void);
void refreshShownOptions(void);
void chooseFolders(void);
void chooseGameFolders(void);
const char* retroarchFolder(const char* which);
void writeSaves(void);
// The loaded game's save file, game.srm, or its clock's, game.rtc.
void savePath(char out[FILE_SIZE], const char* extension);
void runFrames(void);
void setPadForPort(void);
void swapPlayers(int a, int b);
const char* playerPadName(int port);
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
const char* faceName(int binding);
uint32_t padBit(int binding);
// The core's (libretro/saves.c): writes a whole file, the old one kept whole until the new one is,
// with Safe Saves on; false if it couldn't.
bool writeWholeFile(const char* path, const void* data, size_t size);
// Reads a whole file, to be freed; NULL if there is none, or it is empty.
void* readWholeFile(const char* path, size_t* size);
// Whether a button or trigger pressed on a controller, with its Hotkey held, is action's; both are
// then kept from the game until let go.
bool padHotkeyPressed(SDL_JoystickID which, int binding, int action);
const char* padHotkeyName(void);
// Whether there's an accelerometer for the Game Boy Pixel Shadow's Tilt (Android's only).
bool tiltAvailable(void);
#define SHADOW_TILT 9
// The On-Screen Controls (touch.c): attached or detached as wanted when the controllers change, a
// touch, and drawn over the game.
void touchUpdate(void);
// Whether the device is played by touch alone: Android with no controller connected.
bool touchOnly(void);
bool touchEvent(const SDL_Event* event);
void touchDraw(void);
void touchHideMenuButton(void);
// Quits on the Quit hotkey's second press within three seconds; the first says so.
void requestQuit(bool controller);

// ui.c
bool uiInit(void);
void uiRecreateTextures(void);
void uiOpenMenu(void);
void uiPickGame(const char* title, const char* wanted, bool (*picked)(const char* path));
bool uiPickingGame(void);
void uiCloseMenu(void);
// Closes the game, the menu open on General.
void uiCloseGame(void);
bool uiEvent(const SDL_Event* event);
void uiDraw(void);
void osd(const char* format, ...);
// The messages, the press a row asked for and the menu button forgotten, as the app starts or comes
// back: on Android its clock stops while it's away, and starts again with the app in the same process.
void osdClear(void);
// A small sync arrow at the top left for a moment: netplay took the host's state.
void osdResync(void);
void uiCaptureBinding(int which, bool controller, bool hotkey);

// play.c
bool saveState(int slot);
bool loadState(int slot);
bool stateExists(int slot);
// The picture saved with a state, and how many states have been saved.
void stateThumbnailPath(char out[FILE_SIZE + 8], int slot);
extern int statesSaved;
uint8_t* readPng(const char* path, int* width, int* height);
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
// Link Mode in netplay: every player's console on every device, each device its own, or one console every player shares.
enum { LINK_TOGETHER, LINK_SEPARATE, LINK_OFF };
bool netHost(const char* link);
void netGameStarted(void);
extern const char* const netLinkNames[];
// A host found on the local network.
enum { NET_HOSTS_MOST = 8 };
struct NetHost {
	char address[64];
	char nickname[32];
	char game[128];
	// Shared Controls: 0 off, 1 Everyone, 2 Pass Around.
	int how, players, most, shared;
	Uint32 seen;
};
int netScan(const struct NetHost** hosts);
void netScanIdle(void);
void netScanStop(void);
bool netJoin(const char* address, bool noCartridge);
void netStop(void);
// Send My Save: whether it can be sent now, and sending it.
bool netCanSendSave(void);
void netSendSave(void);
void netFrame(void);
void netIdle(void);
bool netPacketInterface(const struct retro_netpacket_callback* callback);
int netPlayers(void);
bool netRunning(void);
// Whether this device joined a session in Off or Together, running the host's game.
bool netInHostsGame(void);
// Whether leaving that session closes the game, as Keep Playing on Disconnect says.
bool netClosesOnLeaving(void);
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
