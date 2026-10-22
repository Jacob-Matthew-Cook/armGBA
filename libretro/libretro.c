/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core. Game Boy Advance games run on armGBA; Game Boy and Game Boy Color games run on
// the Gambatte core in gambatte/.

#include "libretro.h"

#include "cheats.h"
#include "gba.h"
#include "gb.h"
#include "dolphin.h"
#include "link.h"
#include "second.h"
#include "workers.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

enum {
	AUDIO_RATE = 32768,
};

#define FRAME_RATE (16777216.0 / 280896.0)

static retro_environment_t environment;
static retro_video_refresh_t videoRefresh;
static retro_audio_sample_batch_t audioBatch;
static retro_input_poll_t inputPoll;
static retro_input_state_t inputState;
static retro_log_printf_t logPrintf;

static struct GBA* gba;
static bool skipBios;
enum { TILT_LEFT_STICK, TILT_RIGHT_STICK, TILT_DPAD };
static int tiltControl;
enum { RESTART_L_R_START, RESTART_L_R_A_B, RESTART_STICKS, RESTART_SELECT_START_A_B, RESTART_OFF };
static int restartButtons;
// Turbo timing: each press-and-release cycle lasts turboPeriod frames, and the button is held for
// the first turboPulse of them.
enum { TURBO_FEWEST = 4, TURBO_MOST = 120 };
static unsigned turboPeriod = TURBO_FEWEST;
static unsigned turboPulse = 2;
// Boktai's sunlight, 0 to 10.
static int solarLevel = 5;
static retro_set_rumble_state_t setRumble;
static bool rumbling;
enum { THREADED_OFF, THREADED_ON, THREADED_LATE };
static int threadedVideo;
// Color correction: each RGB565 color, green's low bit ignored, to its corrected color.
static bool colorCorrection;
static uint16_t* correctedColors;
static uint16_t correctedFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
static int ghosting;
static int32_t lowPass;
static bool gameBoy;
static enum LibretroGbModel gbModel;
static bool gbColorCorrection;
// The original Game Boy's screen colors: 0 plain grays, 1 the Pocket's, 2 the first model's, 3 the
// Light's.
static int gbShades;
// On the Game Boy Advance model, a Game Boy game is shown as that console shows it: centered on its
// screen, or stretched across it. When cropped, the black border is left out.
static bool gbOnAdvance;
static bool gbStretched;
static bool gbCropped;
static uint32_t gbAdvanceFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
// Game Boys linked here, a controller each, by the cable (two) or the Four Player Adapter.
static bool gbTogether;
static bool gbAdapter;
static uint32_t gbTogetherFrame[LIBRETRO_GB_WIDTH * 2 * LIBRETRO_GB_HEIGHT * 2];
enum { MAX_PLAYERS = 4 };
// Buffer sizes: a path RetroArch gives, a save's file made from one, and a backup's, which adds a
// stamp.
enum { PATH_SIZE = 4096, SAVE_PATH_SIZE = 8192, BACKUP_PATH_SIZE = SAVE_PATH_SIZE + SECOND_STAMP };
// The console's buttons, as the Game Boy Advance's KEYINPUT register has them.
enum {
	KEY_A = 0x1,
	KEY_B = 0x2,
	KEY_SELECT = 0x4,
	KEY_START = 0x8,
	KEY_RIGHT = 0x10,
	KEY_LEFT = 0x20,
	KEY_UP = 0x40,
	KEY_DOWN = 0x80,
	KEY_R = 0x100,
	KEY_L = 0x200,
};
static int playersHere = 1;
// Players whose save starts as a copy of player 1's, made before the first frame.
static uint8_t copySaves;

// The consoles of a Game Boy Advance link can also run here: up to four on one cable, each on its
// own thread.
//
// They run in rounds that all end at the same moment. Between rounds, with every console stopped,
// the link data is passed in a fixed order, so thread timing never changes the result.
//
// A round ends where a transfer ends, so the consoles swap data on its last cycle, as the wires
// would. No round is longer than the shortest transfer a console could start, so none is missed.
//
// Each console starts part of a frame after the one before, because real consoles are never in
// step.
enum {
	GBA_FRAME_CYCLES = 280896,
	ROUND_LONGEST = GBA_FRAME_CYCLES,
};
static struct GBA* gbas[MAX_PLAYERS];
static bool gbaTogether;
static int32_t gbaAhead[MAX_PLAYERS];
static struct Event sliceEvents[MAX_PLAYERS];
static bool sliceOver[MAX_PLAYERS];
static int32_t roundDone[MAX_PLAYERS];
// The cable: each console's transfer, and the others' data for it, handed over between rounds. A
// transfer can end in the frame after it starts, so the transfers are part of a state.
enum { MULTIPLAYER = 2 };
static struct SioLink cables[MAX_PLAYERS];
static struct Transfer {
	int32_t mode;
	uint32_t out;
	bool waiting;
	bool answered;
	uint32_t in[3];
	// The other consoles' data, and when to take it: as the transfer starts in normal mode, since
	// both ends shift out from the first bit, or as it ends in multiplayer mode, which Kirby & The
	// Amazing Mirror needs with four players. pending has a bit for each console whose data is
	// still to be taken.
	uint32_t data[MAX_PLAYERS];
	int32_t takeAt[MAX_PLAYERS];
	uint8_t pending;
} transfers[MAX_PLAYERS];
// Each console's writes to the data it sends in a round are kept, by when, with its data as the
// round began, so its data at a moment inside the round can be told after it.
enum { DATA_WRITES = 8 };
static struct {
	uint32_t before;
	int count;
	int32_t at[DATA_WRITES];
	uint32_t value[DATA_WRITES];
} dataWrites[MAX_PLAYERS];
static uint32_t roundStartTime[MAX_PLAYERS];
// The Wireless Adapters of the consoles here: their messages are carried between rounds.
enum { AIR_QUEUE = 16, AIR_BYTES = 100 };
static struct RfuAir airs[MAX_PLAYERS];
static struct {
	int8_t to;
	uint8_t size;
	uint8_t data[AIR_BYTES];
} airQueue[MAX_PLAYERS][AIR_QUEUE];
static int airQueued[MAX_PLAYERS];
static uint16_t gbaShown[MAX_PLAYERS][SCREEN_WIDTH * SCREEN_HEIGHT];
static uint16_t gbaTogetherFrame[SCREEN_WIDTH * 2 * SCREEN_HEIGHT * 2];
static uint16_t gbaCorrected[MAX_PLAYERS][SCREEN_WIDTH * SCREEN_HEIGHT];

// How the screens are shown: side by side, stacked, a grid, or one player's alone (SCREENS_ONE +
// player).
enum { SCREENS_SIDE, SCREENS_STACKED, SCREENS_ONE, SCREENS_COUNT = SCREENS_ONE + MAX_PLAYERS };
static int screens;
static int screensOption = -1;
// The L2 Button option: fast-forward, or the previous screens.
static bool l2FastForward = true;
static int16_t mixedSamples[2 * 4096];

// Player 2's game: the choices offered, and the game running (empty for player 1's).
enum { SECOND_CHOICES = 125 };
static struct retro_core_options_v2* coreOptions;
static struct retro_core_option_v2_definition* secondOption;
static struct SecondGame secondGames[SECOND_CHOICES];
static size_t secondCount;
static char firstPath[PATH_SIZE];
static char secondPath[PATH_SIZE];
static bool secondFromFrontend;
static bool othersBare;
static bool pairFile;
static char pairPath[PATH_SIZE];
// Players 3 and 4's games, empty for player 1's: from a pair file.
static char otherPaths[MAX_PLAYERS][PATH_SIZE];
static int pairCount;
static bool netplaySeen;
// The Switch Controller buttons move controller 1 to the next console here; controllerFocus is the
// console it plays, the other controllers moving along with it.
enum { SWITCH_OFF, SWITCH_L_R_A_B, SWITCH_START_L2_R2 };
static int switchController = SWITCH_L_R_A_B;
static int controllerFocus;
// In rollback netplay every device runs the host's session. Each device tags its session, and the
// tag goes in every state. A device that loads a state with another device's tag has joined that
// session and takes its tag, so the states on every device stay the same.
static uint32_t sessionTag;
static bool netplayJoined;

// Saves the core reads and writes itself: the other players' saves (where RetroArch keeps their
// game's save, or in armGBA/Player N), and player 1's save with a pair file or Player 1's Save
// Folder.
//
// In rollback netplay the host writes them as the game closes. A device that joined keeps the
// session's saves in armGBA/Remote.
static struct {
	bool kept;
	char file[SAVE_PATH_SIZE];
	char rtcFile[SAVE_PATH_SIZE];
	uint64_t hash;
} keptSaves[MAX_PLAYERS];
static unsigned keptCheck;

// The Saves options' lists. An option holds Off, 126 choices and the end of the list.
enum { BACKUP_CHOICES = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2 };
struct SaveChoices {
	struct retro_core_option_v2_definition* option;
	char values[BACKUP_CHOICES][SECOND_STAMP + 4];
	char labels[BACKUP_CHOICES][80];
	int count;
};
static struct SaveChoices loadChoices;
static struct SaveChoices sendChoices;
static struct retro_core_option_v2_definition* backUpOption;
static struct retro_core_option_v2_definition* loadIntoOption;
static char playerValues[MAX_PLAYERS][2];
static char playerLabels[MAX_PLAYERS][12];
static int backupsListed;
// The player Load Into named when the remote saves' labels were made.
static int listedInto = -1;

// Which options were last shown, once set; and whether the Game Boy cartridge has a tilt sensor
// (MBC7).
static bool optionsShown[32];
static bool optionsShownSet;
static bool tiltCartridge;
// Whether the GameCube link was last shown as connected.
static bool gamecubeShown;

static bool showOptions(void);
static bool togetherWanted(void);
static bool separateWanted(void);
static bool gamecubeWanted(void);
static void fillSaveChoices(void);
static void offerSaveChoices(void);
static void keepSession(void);
static void sendSaveOnPairing(void);
static void saveReceived(const char* game, const uint8_t* save, size_t size, const uint8_t* rtc, size_t rtcSize);
static void airSend(const struct RfuAir* air, int to, const uint8_t* message, size_t size);
static void cableBegin(void* context, int mode, uint32_t out, int32_t cycles);
static bool cableFinish(void* context, uint32_t in[3]);
static void cableWrote(void* context);

// Cheat codes from the frontend, by index, and the Game Boy Advance operations they decode to. All
// codes are decoded again, in order, whenever one changes, because a master code changes how the
// codes after it decode.
static char** cheatCodes;
static size_t cheatCodeCount;
static struct CheatSet cheats;
// Whether the memory map given to the frontend is a Game Boy Color's, which has more work RAM.
static bool gbMapColor;

// Two games loaded together, one per console of a link, each with its own save (and clock) files.
enum {
	SUBSYSTEM_LINK = 1,
	SUBSYSTEM_ADVANCE_LINK = 2,
	MEMORY_SECOND_SAVE = 0x100,
	MEMORY_SECOND_RTC = 0x101,
};

// Helpers.

// A Game Boy Advance's save memory: until the game shows its save type, as large as the largest, as
// in mGBA's core, so a save file from either core loads.
static size_t saveSize(struct GBA* machine) {
	return machine->savedata.type == SAVEDATA_AUTODETECT ? SIZE_FLASH1M : savedataSize(machine);
}

// Whether the consoles of a link run here: Together, a pair file, or two games from the frontend.
static bool togetherRunning(void) {
	return gbTogether || gbaTogether;
}

static void logMessage(enum retro_log_level level, const char* message) {
	if (logPrintf) {
		logPrintf(level, "%s\n", message);
	} else {
		fprintf(stderr, "%s\n", message);
	}
}

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static int clampInt(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

static void notify(const char* message, unsigned frames) {
	struct retro_message shown = { message, frames };
	environment(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
}

static void showMessage(const char* message) {
	notify(message, 180);
	logMessage(RETRO_LOG_ERROR, message);
}

static void updateRumble(bool on) {
	if (on != rumbling && setRumble) {
		setRumble(0, RETRO_RUMBLE_STRONG, on ? 0xFFFF : 0);
		setRumble(0, RETRO_RUMBLE_WEAK, on ? 0xFFFF : 0);
	}
	rumbling = on;
}

// The frontend's calls to set the core up, and what the core tells it.

static void fillTurboPeriods(struct retro_core_option_v2_definition* definitions) {
	static char periods[TURBO_MOST - TURBO_FEWEST + 1][4];
	for (; definitions->key && strcmp(definitions->key, "armgba_turbo_period"); ++definitions) {
	}
	for (size_t i = 0; definitions->key && i < COUNT(periods); ++i) {
		snprintf(periods[i], sizeof(periods[i]), "%d", TURBO_FEWEST + (int) i);
		definitions->values[i] = (struct retro_core_option_value) { periods[i], NULL };
	}
}

// Older frontends take each option as "Label; value|value", the default first, made here from the
// definitions.
static void setVariables(retro_environment_t callback, const struct retro_core_option_v2_definition* definitions) {
	static struct retro_variable variables[64];
	static char text[64][1024];
	int n = 0;
	for (; definitions->key && n < 63; ++definitions, ++n) {
		size_t at = snprintf(text[n], sizeof(text[n]), "%s; %s", definitions->desc, definitions->default_value);
		for (int i = 0; definitions->values[i].value && at < sizeof(text[n]); ++i) {
			if (strcmp(definitions->values[i].value, definitions->default_value)) {
				at += snprintf(text[n] + at, sizeof(text[n]) - at, "|%s", definitions->values[i].value);
			}
		}
		variables[n] = (struct retro_variable) { definitions->key, text[n] };
	}
	variables[n] = (struct retro_variable) { NULL, NULL };
	callback(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

// Keys and values match saved configurations; labels are what the menu shows. showOptions hides
// the options that do nothing for the game and Link Mode.
static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armgba_skip_bios", "Skip BIOS Intro", NULL, "Start games without the BIOS's intro: the Game Boy Advance logo, or armGBA's splash.", NULL, "system",
	  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
	{ "armgba_bios", "BIOS", NULL,
	  "Nintendo's gba_bios.bin from the system folder, or armGBA's open-source BIOS built into the core. Nintendo's falls back to armGBA's when the file isn't there. Applies on Restart.",
	  NULL, "system",
	  { { "nintendo", "Nintendo's (System Folder)" }, { "open-source", "armGBA's (Built In)" }, { NULL, NULL } }, "nintendo" },
	{ "armgba_gb_model", "Game Boy Model", NULL,
	  "The console Game Boy and Game Boy Color games run on. On the Game Boy Advance, L or R stretches the picture. Applies on Restart.",
	  NULL, "system",
	  { { "Auto", NULL }, { "Game Boy", NULL }, { "Game Boy Pocket", "Game Boy Pocket / Light" }, { "Game Boy Color", NULL }, { "Game Boy Advance", NULL }, { NULL, NULL } }, "Auto" },
	{ "armgba_gb_boot_rom", "Boot ROM", NULL,
	  "Start with the console's logo: Nintendo's boot ROM from the system folder (armGBA's when it isn't there), or armGBA's open-source one built into the core. Applies on Restart.",
	  NULL, "system",
	  { { "enabled", "Nintendo's (System Folder)" }, { "disabled", "Off" }, { "open-source", "armGBA's (Built In)" }, { NULL, NULL } }, "enabled" },
	{ "armgba_threaded_video", "Threaded Video", NULL,
	  "Draw the picture on a second CPU core, which helps fast-forward and runahead on slower devices. One Frame Late is faster still, but shows each frame a frame late.", NULL, "video",
	  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { "one frame late", "One Frame Late" }, { NULL, NULL } }, "disabled" },
	{ "armgba_color_correction", "Color Correction", NULL, "Darker, softer colors, as the Game Boy Advance's screen showed them. Costs a little speed.", NULL, "video",
	  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_color_correction", "Color Correction", NULL,
	  "Colors as the Game Boy Color's screen showed them. Not used for games running on an original Game Boy.", NULL, "video",
	  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_screen", "Game Boy Screen", NULL, "The colors original Game Boy games are shown in: plain grays, the first model's green, the Pocket's olive gray, or the Light's backlit teal.", NULL, "video",
	  { { "grayscale", "Grayscale" }, { "original", "Game Boy (Green)" }, { "pocket", "Game Boy Pocket (Olive)" }, { "light", "Game Boy Light (Teal)" }, { NULL, NULL } }, "grayscale" },
	{ "armgba_gb_advance_border", "Game Boy Advance Border", NULL,
	  "On the Game Boy Advance model: Cropped shows only the game's picture, Shown the black around it as that console's screen has.", NULL, "video",
	  { { "cropped", "Cropped" }, { "shown", "Shown" }, { NULL, NULL } }, "cropped" },
	{ "armgba_lcd_ghosting", "LCD Ghosting", NULL,
	  "The console's slow screen, which some games flicker sprites on to make them see-through: each change fades in over a few frames, darkening faster than it lightens, the first Game Boy's slowest. Half fades twice as fast.",
	  NULL, "video", { { "disabled", "Off" }, { "half", "Half" }, { "enabled", "Full" }, { NULL, NULL } }, "disabled" },
	{ "armgba_low_pass_filter", "Low-Pass Filter", NULL,
	  "Softens the sound's high notes, as the consoles' small speakers did. Higher is softer.", NULL, "audio",
	  { { "disabled", "Disabled" }, { "20", "20%" }, { "40", "40%" }, { "60", "60%" }, { "80", "80%" }, { NULL, NULL } }, "disabled" },
	{ "armgba_turbo_period", "Turbo Button Period", NULL,
	  "Frames per press while a turbo button is held: X and Y for A and B, and L3 and R3 for L and R on the Game Boy Advance.", NULL, "input",
	  { { NULL, NULL } }, "4" },
	{ "armgba_gb_tilt", "Tilt Sensor", NULL,
	  "What tilts the cartridge in motion sensor games, such as Kirby Tilt 'n' Tumble and WarioWare: Twisted!.",
	  NULL, "input",
	  { { "left stick", "Left Stick" }, { "right stick", "Right Stick" }, { "d-pad", "D-Pad" }, { NULL, NULL } }, "left stick" },
	{ "armgba_solar_sensor", "Solar Sensor", NULL,
	  "The sunlight Boktai's sensor sees. Pushing the right stick up or down changes it while playing.", NULL, "input",
	  { { "0", "0 (Dark)" }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", "10 (Full Sun)" }, { NULL, NULL } }, "5" },
	{ "armgba_restart_buttons", "Restart Buttons", NULL,
	  "Hold for a second to restart that player's console. On many handhelds Select + Start also quits RetroArch.",
	  NULL, "input",
	  { { "l r start", "L + R + Start" }, { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "select start a b", "Select + Start + A + B" }, { "off", "Off" }, { NULL, NULL } },
	  "l r start" },
	{ "armgba_link_mode", "Link Mode", NULL,
	  "Together: two to four consoles run on each device, for multiplayer on one device (a controller each) or smooth rollback netplay. Separate: two players in netplay, each device running only its own player's console and game. GameCube: this Game Boy Advance is plugged into a GameCube in Dolphin. Applies on Restart.",
	  NULL, "link",
	  { { "off", "Off" }, { "together", "Together (one device, or smooth netplay)" }, { "separate", "Separate (each device its own console)" }, { "gamecube", "GameCube (Dolphin)" }, { NULL, NULL } },
	  "off" },
	{ "armgba_link_players", "Players", NULL,
	  "How many consoles run. Four Game Boy Advances need a fast device. Applies on Restart.",
	  NULL, "link", { { "2", NULL }, { "3", NULL }, { "4", NULL }, { NULL, NULL } }, "2" },
	{ "armgba_player2_game", "Player 2's Game", NULL,
	  "The game on player 2's console, from the games beside player 1's, likeliest partners first. Players 3 and 4 play player 1's. No Cartridge leaves the other consoles empty, for games that send themselves over the cable. Applies on Restart.",
	  NULL, "link", { { "same", "Same Game" }, { NULL, NULL } }, "same" },
	{ "armgba_link_cartridge", "This Console's Cartridge", NULL,
	  "Removed starts this console with no cartridge, waiting for a game the other player's console sends over the cable, as single-pak games do. The game's save is left as it is. Applies on Restart.",
	  NULL, "link", { { "inserted", "Inserted" }, { "removed", "Removed" }, { NULL, NULL } }, "inserted" },
	{ "armgba_link_port", "Link Port", NULL,
	  "What is plugged in: the Link Cable, or the Wireless Adapter that Pokemon FireRed, LeafGreen and Emerald and other games use. Applies on Restart.",
	  NULL, "link", { { "cable", "Link Cable" }, { "wireless", "Wireless Adapter (smoothest over Wi-Fi)" }, { NULL, NULL } }, "cable" },
	{ "armgba_gamecube_address", "GameCube Address", NULL,
	  "Where Dolphin runs, with a controller port set to GBA (TCP): found on this device or the local network, or this device only.",
	  NULL, "link", { { "auto", "Find on the Network" }, { "127.0.0.1", "This Device" }, { NULL, NULL } }, "auto" },
	{ "armgba_gb_link_port", "Link Port", NULL,
	  "What joins the Game Boys: the Link Cable, for two, or the Four Player Adapter that Wario Blast, F-1 Race and other four-player games use. Three or four always use the adapter. Applies on Restart.",
	  NULL, "link", { { "cable", "Link Cable" }, { "adapter", "Four Player Adapter" }, { NULL, NULL } }, "cable" },
	{ "armgba_gb_link_timing", "Game Boy Link Timing", NULL,
	  "Standard suits Wi-Fi and games that link in turns, such as trades and battles. Exact keeps both Game Boys in step, as a real cable does, for games that link all the time; it slows with a long ping. Game Boy Advances are always exact.",
	  NULL, "link", { { "standard", "Standard (smooth on Wi-Fi)" }, { "exact", "Exact (if a game errors or desyncs)" }, { NULL, NULL } }, "standard" },
	{ "armgba_link_screens", "Screens", NULL,
	  "How the screens are arranged. R2 changes it while playing.", NULL, "link",
	  { { "side by side", "Side by Side" }, { "top and bottom", "Top and Bottom" }, { "player 1", "Player 1 Only" }, { "player 2", "Player 2 Only" }, { "player 3", "Player 3 Only" }, { "player 4", "Player 4 Only" }, { NULL, NULL } },
	  "side by side" },
	{ "armgba_link_l2", "L2 Button", NULL,
	  "What L2 does outside netplay: fast-forward while held, or go back to the previous screens. In netplay it always goes back.",
	  NULL, "link", { { "fast forward", "Fast Forward" }, { "previous screens", "Previous Screens" }, { NULL, NULL } }, "fast forward" },
	{ "armgba_switch_controller", "Switch Controller", NULL,
	  "Pressed on controller 1, moves it to the next console, the other controllers moving along with it: one controller can play every console. Not in netplay.",
	  NULL, "link", { { "l r a b", "L + R + A + B" }, { "start l2 r2", "Start + L2 + R2" }, { "off", "Off" }, { NULL, NULL } }, "l r a b" },
	{ "armgba_back_up_save", "Back Up Save", NULL,
	  "Copies a player's save, as it is now, to a file beside it named for the time: Game.20261018-153012.srm. The game carries on.",
	  NULL, "saves", { { "off", "Off" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "off" },
	{ "armgba_load_save", "Load Save", NULL,
	  "Makes a backed-up save, or another player's current one, a player's save, and restarts their console. Load Into picks the player. The save it replaces is backed up first. Not during netplay.",
	  NULL, "saves", { { "off", "Off" }, { NULL, NULL } }, "off" },
	{ "armgba_load_into", "Load Into", NULL,
	  "The player Load Save gives a save to: the one it belongs to, or the player named here.",
	  NULL, "saves", { { "same", "Same Player" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "same" },
	{ "armgba_send_save", "Send Save", NULL,
	  "With Separate, while linked: sends the chosen save to the other player, who finds it under Received in Load Save. Linked devices also swap their current saves when they connect.",
	  NULL, "saves", { { "off", "Off" }, { "1 now", "Player 1: Now" }, { NULL, NULL } }, "off" },
	{ "armgba_new_save", "New Players' Saves", NULL,
	  "What a player on player 1's game starts with when they have no save yet: a new save, or a copy of player 1's. Their saves are kept in the save folder's armGBA/Player 2 to 4 folders. Applies on Restart.",
	  NULL, "saves", { { "new", "Create New Save" }, { "copy", "Copy Player 1's Save" }, { NULL, NULL } }, "new" },
	{ "armgba_player1_saves", "Player 1's Save Folder", NULL,
	  "Where player 1's save is kept: where RetroArch keeps saves, or in the save folder's armGBA/Player 1 folder, beside the other players' saves and player 1's backups. When it changes, the newer of the two is used. Applies when a game starts.",
	  NULL, "saves", { { "retroarch", "RetroArch's Save Folder" }, { "armgba", "armGBA/Player 1 Folder" }, { NULL, NULL } }, "retroarch" },
	{ "armgba_keep_saves", "Backups Kept", NULL,
	  "How many backups each player keeps. When there are more, the oldest are deleted. A save the same as its newest backup isn't backed up again.",
	  NULL, "saves", { { "5", "5" }, { "10", "10" }, { "20", "20" }, { "all", "All" }, { NULL, NULL } }, "10" },
	{ "armgba_list_saves", "Backups Listed", NULL,
	  "How many of each player's newest backups Load Save lists, up to 126 in all.",
	  NULL, "saves", { { "10", "10" }, { "20", "20" }, { "40", "40" }, { "80", "80" }, { "126", "126" }, { NULL, NULL } }, "10" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};
static struct retro_core_option_v2_category optionCategories[] = {
	{ "system", "System", "The console the game runs on and how it starts." },
	{ "video", "Video", "Colors, the picture and how it is drawn." },
	{ "audio", "Audio", "How the sound is filtered." },
	{ "input", "Input", "Turbo, restart buttons and cartridge sensors." },
	{ "link", "Link", "Playing together: the link cable, the Wireless Adapter and the Four Player Adapter." },
	{ "saves", "Saves", "Backing up, loading and sending saves." },
	{ NULL, NULL, NULL },
};
static struct retro_core_options_v2 optionsV2 = { optionCategories, optionDefinitions };

void retro_set_environment(retro_environment_t callback) {
	environment = callback;
	static const struct retro_subsystem_memory_info firstMemory[] = { { "srm", RETRO_MEMORY_SAVE_RAM }, { "rtc", RETRO_MEMORY_RTC } };
	static const struct retro_subsystem_memory_info secondMemory[] = { { "srm", MEMORY_SECOND_SAVE }, { "rtc", MEMORY_SECOND_RTC } };
	static const struct retro_subsystem_rom_info roms[] = {
		{ "Player 1's Game", "gb|gbc", false, false, true, firstMemory, 2 },
		{ "Player 2's Game", "gb|gbc", false, false, true, secondMemory, 2 },
	};
	static const struct retro_subsystem_rom_info advanceRoms[] = {
		{ "Player 1's Game", "gba|bin", false, false, true, firstMemory, 1 },
		{ "Player 2's Game", "gba|bin", false, false, true, secondMemory, 1 },
	};
	static const struct retro_subsystem_info subsystems[] = {
		{ "Two Game Boys (Link Cable)", "gblink", roms, 2, SUBSYSTEM_LINK },
		{ "Two Game Boy Advances (Link Cable)", "gbalink", advanceRoms, 2, SUBSYSTEM_ADVANCE_LINK },
		{ NULL, NULL, NULL, 0, 0 },
	};
	callback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*) subsystems);
	struct retro_log_callback logging;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
		logPrintf = logging.log;
	}
	fillTurboPeriods(optionDefinitions);
	unsigned version = 0;
	if (callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &optionsV2);
		coreOptions = &optionsV2;
		for (struct retro_core_option_v2_definition* definition = optionDefinitions; definition->key; ++definition) {
			if (!strcmp(definition->key, "armgba_player2_game")) {
				secondOption = definition;
			} else if (!strcmp(definition->key, "armgba_load_save")) {
				loadChoices.option = definition;
			} else if (!strcmp(definition->key, "armgba_send_save")) {
				sendChoices.option = definition;
			} else if (!strcmp(definition->key, "armgba_back_up_save")) {
				backUpOption = definition;
			} else if (!strcmp(definition->key, "armgba_load_into")) {
				loadIntoOption = definition;
			}
		}
		struct retro_core_options_update_display_callback display = { showOptions };
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &display);
		return;
	}
	setVariables(callback, optionDefinitions);
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
	videoRefresh = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
	audioBatch = callback;
}

void retro_set_input_poll(retro_input_poll_t callback) {
	inputPoll = callback;
}

void retro_set_input_state(retro_input_state_t callback) {
	inputState = callback;
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armGBA";
	info->library_version = "0.1";
	info->valid_extensions = "gba|bin|gb|gbc|gb2|gba2";
	info->need_fullpath = false;
	info->block_extract = false;
}

// The picture size: 240x160, or 160x144 for a Game Boy game, or the linked consoles' screens. On the Game
// Boy Advance model, a Game Boy game is 240x160 with the border shown, or 160x144 cropped (240x144
// when stretched).
static void geometry(struct retro_game_geometry* geometry) {
	geometry->base_width = SCREEN_WIDTH;
	geometry->base_height = SCREEN_HEIGHT;
	// Room for two by two consoles' screens, which a Restart can bring in.
	geometry->max_width = SCREEN_WIDTH * 2;
	geometry->max_height = SCREEN_HEIGHT * 2;
	geometry->aspect_ratio = 3.0f / 2.0f;
	if (gameBoy && !gbOnAdvance) {
		geometry->base_width = LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = 10.0f / 9.0f;
	}
	if (gameBoy && gbOnAdvance && gbCropped) {
		geometry->base_width = gbStretched ? SCREEN_WIDTH : LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = gbStretched ? 5.0f / 3.0f : 10.0f / 9.0f;
	}
	if (gameBoy && gbTogether) {
		geometry->base_width = LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
	}
	if (gbaTogether) {
		geometry->base_width = SCREEN_WIDTH;
		geometry->base_height = SCREEN_HEIGHT;
	}
	if (togetherRunning()) {
		bool const grid = playersHere > 2 && screens < SCREENS_ONE;
		geometry->base_width *= screens == SCREENS_SIDE || grid ? 2 : 1;
		geometry->base_height *= screens == SCREENS_STACKED || grid ? 2 : 1;
		geometry->aspect_ratio = (float) geometry->base_width / geometry->base_height;
	}
}

static void announceGeometry(void) {
	struct retro_game_geometry changed;
	geometry(&changed);
	environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &changed);
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	gbOnAdvance = gameBoy && !gbTogether && libretroGbOnAdvance();
	geometry(&info->geometry);
	// A Game Boy frame lasts as long as a Game Boy Advance frame.
	info->timing.fps = FRAME_RATE;
	info->timing.sample_rate = AUDIO_RATE;
}

void retro_init(void) {
	cheatsInit(&cheats);
	linkSetEnvironment(environment);
	linkSetSaveHooks(sendSaveOnPairing, saveReceived);
}

void retro_deinit(void) {
	free(correctedColors);
	correctedColors = NULL;
	colorCorrection = false;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
}

// Options.

// Builds the color correction table from Pokefan531's measurements of the screen: its primaries
// mixed in linear light, at a gamma of 2.2.
static uint16_t corrected(uint16_t color) {
	return correctedColors[(color & 0x1F) | ((color >> 1) & 0x7FE0)];
}

static bool buildCorrectedColors(void) {
	if (correctedColors) {
		return true;
	}
	correctedColors = malloc(0x8000 * sizeof(*correctedColors));
	if (!correctedColors) {
		return false;
	}
	static const float mix[3][3] = {
		{ 0.80f, 0.275f, -0.075f },
		{ 0.135f, 0.64f, 0.225f },
		{ 0.195f, 0.155f, 0.65f },
	};
	float linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = powf(i / 31.0f, 2.2f);
	}
	for (int i = 0; i < 0x8000; ++i) {
		float in[3] = { linear[i >> 10], linear[(i >> 5) & 0x1F], linear[i & 0x1F] };
		int out[3];
		for (int c = 0; c < 3; ++c) {
			float value = mix[c][0] * in[0] + mix[c][1] * in[1] + mix[c][2] * in[2];
			value = value < 0 ? 0 : value > 1 ? 1 : value;
			out[c] = (int) (powf(value, 1 / 2.2f) * (c == 1 ? 63 : 31) + 0.5f);
		}
		correctedColors[i] = out[0] << 11 | out[1] << 5 | out[2];
	}
	return true;
}

static const char* option(const char* key) {
	struct retro_variable variable = { key, NULL };
	return environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : NULL;
}

static bool optionIs(const char* key, const char* wanted) {
	const char* const value = option(key);
	return value && !strcmp(value, wanted);
}

struct Choice {
	const char* value;
	int number;
};

// The number for the option's value, or fallback if the value isn't one of the choices.
static int optionChoice(const char* key, const struct Choice* choices, size_t count, int fallback) {
	const char* const value = option(key);
	for (size_t i = 0; value && i < count; ++i) {
		if (!strcmp(value, choices[i].value)) {
			return choices[i].number;
		}
	}
	return fallback;
}

// Names each button for the loaded console, including the tilt sensor's controls.
static void showButtons(void) {
	bool const forGameBoy = gameBoy;
	static struct retro_input_descriptor shown[80];
	size_t n = 0;
#define BUTTON(device, index, id, name) shown[n++] = (struct retro_input_descriptor) { port, device, index, id, name }
	unsigned port = 0;
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B");
	if (togetherRunning()) {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, l2FastForward ? "Fast Forward (Previous Screens in Netplay)" : "Previous Screens");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Next Screens");
	} else {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Fast Forward");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Fast Forward");
	}
	if (forGameBoy) {
		// A Game Boy has no L or R; on the Game Boy Advance model they stretch the picture. With
		// the tilt sensor on the D-pad, L is the flick.
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, tiltControl == TILT_DPAD ? "Flick (Motion Sensor)" : "Stretch (Game Boy Advance Model)");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Stretch (Game Boy Advance Model)");
	} else {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Turbo L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Turbo R");
	}
	if (tiltControl != TILT_DPAD) {
		unsigned const stick = tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X, "Tilt / Turn (Motion Sensor)");
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y,
		       !forGameBoy && tiltControl == TILT_RIGHT_STICK ? "Tilt Up / Down, Sunlight" : "Tilt Up / Down (Motion Sensor)");
		if (forGameBoy) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_ID_JOYPAD_R3 : RETRO_DEVICE_ID_JOYPAD_L3, "Flick (Motion Sensor)");
		}
	}
	if (!forGameBoy && tiltControl != TILT_RIGHT_STICK) {
		BUTTON(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Sunlight (Solar Sensor)");
	}
	for (port = 1; port < (unsigned) playersHere; ++port) {
		static const unsigned ids[12] = {
			RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_RIGHT,
			RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
			RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
		};
		for (int i = 0; i < 12; ++i) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, ids[i], shown[i].description);
		}
		if (!forGameBoy) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L");
			BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R");
		}
	}
#undef BUTTON
	shown[n] = (struct retro_input_descriptor) { 0, 0, 0, 0, NULL };
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, shown);
}

static void readOptions(void) {
	static const struct Choice threadedChoices[] = { { "enabled", THREADED_ON }, { "one frame late", THREADED_LATE } };
	static const struct Choice modelChoices[] = {
		{ "Game Boy", LIBRETRO_GB_DMG },
		{ "Game Boy Pocket", LIBRETRO_GB_MGB },
		{ "Game Boy Color", LIBRETRO_GB_CGB },
		{ "Game Boy Advance", LIBRETRO_GB_AGB },
	};
	static const struct Choice bootChoices[] = { { "disabled", LIBRETRO_GB_BOOT_OFF }, { "open-source", LIBRETRO_GB_BOOT_ARMGBA } };
	static const struct Choice shadeChoices[] = { { "pocket", 1 }, { "original", 2 }, { "light", 3 } };
	static const struct Choice screensChoices[] = {
		{ "top and bottom", SCREENS_STACKED },
		{ "player 1", SCREENS_ONE },
		{ "player 2", SCREENS_ONE + 1 },
		{ "player 3", SCREENS_ONE + 2 },
		{ "player 4", SCREENS_ONE + 3 },
	};
	static const struct Choice restartChoices[] = {
		{ "l r a b", RESTART_L_R_A_B },
		{ "l3 r3", RESTART_STICKS },
		{ "select start a b", RESTART_SELECT_START_A_B },
		{ "off", RESTART_OFF },
	};
	static const struct Choice tiltChoices[] = { { "right stick", TILT_RIGHT_STICK }, { "d-pad", TILT_DPAD } };

	skipBios = optionIs("armgba_skip_bios", "enabled");
	const char* address = option("armgba_gamecube_address");
	dolphinSetHost(gamecubeWanted() && !gameBoy ? (address ? address : "auto") : NULL);
	int threaded = optionChoice("armgba_threaded_video", threadedChoices, COUNT(threadedChoices), THREADED_OFF);
	// Linked consoles run in turns, which a render thread would only slow.
	threaded = gbaTogether ? THREADED_OFF : threaded;
	if (gba && threaded != threadedVideo) {
		renderThreadStop(gba);
		if (threaded) {
			renderThreadStart(gba, threaded == THREADED_LATE);
		}
	}
	threadedVideo = threaded;
	colorCorrection = optionIs("armgba_color_correction", "enabled") && buildCorrectedColors();
	static const struct Choice ghostingChoices[] = { { "half", 1 }, { "enabled", 2 } };
	ghosting = optionChoice("armgba_lcd_ghosting", ghostingChoices, COUNT(ghostingChoices), 0);
	const char* const filter = option("armgba_low_pass_filter");
	lowPass = filter ? atoi(filter) * 0x10000 / 100 : 0;
	gbModel = optionChoice("armgba_gb_model", modelChoices, COUNT(modelChoices), LIBRETRO_GB_AUTO);
	libretroGbSetBootRom(optionChoice("armgba_gb_boot_rom", bootChoices, COUNT(bootChoices), LIBRETRO_GB_BOOT_NINTENDO));
	int const shades = optionChoice("armgba_gb_screen", shadeChoices, COUNT(shadeChoices), 0);
	gbShades = shades;
	gbColorCorrection = optionIs("armgba_gb_color_correction", "enabled");
	for (int i = gameBoy ? playersHere - 1 : 0; i >= 0; --i) {
		libretroGbSelect(i);
		libretroGbSetScreen(shades);
		if (gameBoy) {
			libretroGbSetColorCorrection(gbColorCorrection);
		}
	}
	int const chosenScreens = optionChoice("armgba_link_screens", screensChoices, COUNT(screensChoices), SCREENS_SIDE);
	// R2 (and L2 in netplay) changes the screens too; the option wins only when it changes.
	if (chosenScreens != screensOption) {
		screensOption = screens = chosenScreens;
		if (togetherRunning()) {
			announceGeometry();
		}
	}
	static const struct Choice switchChoices[] = {
		{ "start l2 r2", SWITCH_START_L2_R2 },
		{ "off", SWITCH_OFF },
	};
	switchController = optionChoice("armgba_switch_controller", switchChoices, COUNT(switchChoices), SWITCH_L_R_A_B);
	bool const fastForward = !optionIs("armgba_link_l2", "previous screens");
	if (fastForward != l2FastForward) {
		l2FastForward = fastForward;
		if (togetherRunning()) {
			showButtons();
		}
	}
	bool const cropped = !optionIs("armgba_gb_advance_border", "shown");
	if (cropped != gbCropped) {
		gbCropped = cropped;
		if (gameBoy && gbOnAdvance) {
			announceGeometry();
		}
	}
	const char* value = option("armgba_turbo_period");
	turboPeriod = clampInt(value ? atoi(value) : TURBO_FEWEST, TURBO_FEWEST, TURBO_MOST);
	turboPulse = clampInt(turboPeriod / 2, 2, 15);
	linkSetGbExact(optionIs("armgba_gb_link_timing", "exact"));
	restartButtons = optionChoice("armgba_restart_buttons", restartChoices, COUNT(restartChoices), RESTART_L_R_START);
	int const tilt = optionChoice("armgba_gb_tilt", tiltChoices, COUNT(tiltChoices), TILT_LEFT_STICK);
	if (tilt != tiltControl) {
		tiltControl = tilt;
		if (gba || gameBoy) {
			showButtons();
		}
	}
	value = option("armgba_solar_sensor");
	if (value) {
		solarLevel = atoi(value);
	}
}

// Hides the options that do nothing for the game and Link Mode. RetroArch calls it when an option
// changes; returns whether that changed which are shown.
static bool showOptions(void) {
	bool const advance = gba && !gameBoy;
	int const devices = advance ? gba->gpio.devices : 0;
	bool const together = togetherWanted();
	bool const linked = together || separateWanted();
	const struct {
		const char* key;
		bool shown;
	} options[] = {
		{ "armgba_skip_bios", !gameBoy },
		{ "armgba_bios", !gameBoy },
		{ "armgba_threaded_video", !gameBoy },
		{ "armgba_color_correction", !gameBoy },
		{ "armgba_solar_sensor", !gameBoy && (!advance || devices & HW_LIGHT_SENSOR) },
		{ "armgba_gb_tilt", (!gameBoy && !advance) || devices & (HW_TILT | HW_GYRO) || (gameBoy && tiltCartridge) },
		{ "armgba_link_port", !gameBoy && linked },
		{ "armgba_gb_model", !advance },
		{ "armgba_gb_boot_rom", !advance },
		{ "armgba_gb_color_correction", !advance },
		{ "armgba_gb_screen", !advance },
		{ "armgba_gb_advance_border", !advance },
		{ "armgba_gb_link_port", !advance && together },
		{ "armgba_gb_link_timing", !advance && separateWanted() },
		{ "armgba_link_players", together },
		{ "armgba_link_screens", together },
		{ "armgba_link_l2", together },
		{ "armgba_player2_game", together },
		{ "armgba_new_save", together },
		{ "armgba_load_into", together },
		{ "armgba_send_save", separateWanted() },
		{ "armgba_link_cartridge", advance && (separateWanted() || gamecubeWanted()) },
		{ "armgba_gamecube_address", advance && gamecubeWanted() },
	};
	enum { OPTIONS = COUNT(options) };
	bool changed = false;
	for (int i = 0; i < OPTIONS; ++i) {
		if (!optionsShownSet || optionsShown[i] != options[i].shown) {
			optionsShown[i] = options[i].shown;
			struct retro_core_option_display display = { options[i].key, options[i].shown };
			environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
			changed = true;
		}
	}
	optionsShownSet = true;
	return changed;
}

// Sets the options again after a list of choices changed. That shows every option, so they are
// hidden again.
static void setOptions(void) {
	environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, coreOptions);
	optionsShownSet = false;
	showOptions();
}

static bool togetherWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "together");
}

static bool separateWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "separate");
}

static bool gamecubeWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "gamecube");
}

static int playersWanted(void) {
	const char* value = option("armgba_link_players");
	return clampInt(value ? atoi(value) : 2, 2, MAX_PLAYERS);
}

// Three or four Game Boys always use the Four Player Adapter.
static bool adapterWanted(int count) {
	const char* value = option("armgba_gb_link_port");
	return count > 2 || (value && !strcmp(value, "adapter"));
}

// Player 2's Game.

// Lists the games beside the loaded one as Player 2's Game's choices.
static void offerSecondGames(const char* path, bool forGameBoy) {
	secondFree(secondGames, secondCount);
	secondCount = path ? secondList(path, forGameBoy, secondGames, SECOND_CHOICES) : 0;
	// A game in an archive comes as "game.zip#game.gb"; the file is the archive.
	snprintf(firstPath, sizeof(firstPath), "%s", path ? path : "");
	firstPath[strcspn(firstPath, "#")] = '\0';
	if (!coreOptions || !secondOption) {
		return;
	}
	size_t n = 0;
	secondOption->values[n++] = (struct retro_core_option_value) { "same", "Same Game" };
	if (!forGameBoy) {
		secondOption->values[n++] = (struct retro_core_option_value) { "none", "No Cartridge" };
	}
	for (size_t i = 0; i < secondCount; ++i) {
		secondOption->values[n++] = (struct retro_core_option_value) { secondGames[i].path, secondGames[i].label };
	}
	secondOption->values[n] = (struct retro_core_option_value) { NULL, NULL };
	fillSaveChoices();
	setOptions();
}

// The file remembering each game's Player 2's Game: in the save directory, or beside player 1's
// game.
static void secondMemoryFile(char* out, size_t outSize) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (saveDirectory && *saveDirectory) {
		snprintf(out, outSize, "%s/armGBA player 2 games.txt", saveDirectory);
		return;
	}
	snprintf(out, outSize, "%s", firstPath);
	char* slash = strrchr(out, '/');
	snprintf(slash ? slash + 1 : out, outSize - (slash ? (size_t) (slash + 1 - out) : 0), "armGBA player 2 games.txt");
}

static bool offered(const char* value) {
	for (size_t i = 0; value && i < secondCount; ++i) {
		if (!strcmp(value, secondGames[i].path)) {
			return true;
		}
	}
	return value && (!strcmp(value, "same") || (!gameBoy && !strcmp(value, "none")));
}

// Whether the other consoles are to start with no cartridge, waiting for player 1's game to send
// itself over the cable.
static bool bareWanted(void) {
	const char* value = option("armgba_player2_game");
	return !pairFile && !gameBoy && value && !strcmp(value, "none");
}

static void recallSecondChoice(void) {
	char memory[PATH_SIZE];
	char choice[PATH_SIZE];
	secondMemoryFile(memory, sizeof(memory));
	if (secondRecall(memory, firstPath, choice, sizeof(choice)) && offered(choice)) {
		struct retro_variable variable = { "armgba_player2_game", choice };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
	}
}

static void rememberSecondChoice(void) {
	const char* value = option("armgba_player2_game");
	if (*firstPath && offered(value)) {
		char memory[PATH_SIZE];
		secondMemoryFile(memory, sizeof(memory));
		secondRemember(memory, firstPath, value);
	}
}

// The game chosen for player 2, or NULL for the same game.
static const char* chosenSecond(void) {
	const char* value = pairFile ? NULL : option("armgba_player2_game");
	for (size_t i = 0; value && i < secondCount; ++i) {
		if (!strcmp(value, secondGames[i].path)) {
			return secondGames[i].path;
		}
	}
	return NULL;
}

static const char* gameOf(int player) {
	const char* chosen = player == 1 ? secondPath : player > 1 ? otherPaths[player] : "";
	return *chosen ? chosen : firstPath;
}

// Save files: each player's save memory and where its file is.

static void* saveMemory(int player, size_t* size) {
	*size = 0;
	if (gameBoy && player < playersHere) {
		libretroGbSelect(player);
		void* data = libretroGbSaveRam(size);
		libretroGbSelect(0);
		return data;
	}
	if (!gameBoy && gbas[player]) {
		*size = saveSize(gbas[player]);
		return gbas[player]->savedata.storage;
	}
	return NULL;
}

static void* rtcMemory(int player, size_t* size) {
	*size = 0;
	if (gameBoy && player < playersHere) {
		libretroGbSelect(player);
		void* data = libretroGbRtc(size);
		libretroGbSelect(0);
		return data;
	}
	return NULL;
}

static uint64_t hashOf(const void* data, size_t size) {
	uint64_t hash = 0xCBF29CE484222325ull;
	for (size_t i = 0; i < size; ++i) {
		hash = (hash ^ ((const uint8_t*) data)[i]) * 0x100000001B3ull;
	}
	return hash;
}

static bool fileExists(const char* path) {
	FILE* f = fopen(path, "rb");
	if (f) {
		fclose(f);
	}
	return f != NULL;
}

// Reads a file into memory, as much as fits; returns whether there was a file.
static bool readFileInto(const char* path, void* data, size_t size) {
	FILE* f = data && size ? fopen(path, "rb") : NULL;
	if (!f) {
		return false;
	}
	size_t read = fread(data, 1, size, f);
	fclose(f);
	return read > 0;
}

static bool writeFile(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	bool const written = f && fwrite(data, 1, size, f) == size;
	if (f) {
		fclose(f);
	}
	if (!written) {
		char message[160];
		snprintf(message, sizeof(message), "armGBA can't write %.120s", path);
		showMessage(message);
	}
	return written;
}

// Reads a whole file; NULL if there is none, or it is empty.
static void* readWholeFile(const char* path, size_t* size) {
	*size = 0;
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long const length = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = length > 0 ? malloc(length) : NULL;
	if (data && fread(data, 1, length, f) == (size_t) length) {
		*size = length;
	} else {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static bool copyFile(const char* from, const char* to) {
	size_t size;
	void* data = readWholeFile(from, &size);
	if (data) {
		writeFile(to, data, size);
	}
	free(data);
	return data != NULL;
}

// RetroArch's save directory, or with a pair file the one its games' saves go in.
static const char* saveDirectoryFor(char unsorted[PATH_SIZE]) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (pairFile && saveDirectory) {
		secondPairSaveDirectory(saveDirectory, pairPath, unsorted, PATH_SIZE);
		saveDirectory = unsorted;
	}
	return saveDirectory;
}

static void retroArchFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	secondSavePath(firstPath, "", "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	secondSavePath(firstPath, "", "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

static void folderFiles(int player, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	secondPlayerSavePath(firstPath, player, "srm", saveDirectory, file, SAVE_PATH_SIZE);
	secondPlayerSavePath(firstPath, player, "rtc", saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

// Where a player's save and clock are kept: the core's file for the save it keeps, or for player 1
// the one RetroArch keeps. Returns false for a player who isn't playing.
static bool saveFiles(int player, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	if (player < playersHere && keptSaves[player].kept) {
		snprintf(file, SAVE_PATH_SIZE, "%s", keptSaves[player].file);
		snprintf(rtcFile, SAVE_PATH_SIZE, "%s", keptSaves[player].rtcFile);
		return true;
	}
	if (player) {
		return false;
	}
	retroArchFiles(file, rtcFile);
	return true;
}

static void* liveSave(int player, size_t* size, void** rtc, size_t* rtcSize) {
	*rtc = NULL;
	*rtcSize = *size = 0;
	if (player && (player >= playersHere || !keptSaves[player].kept)) {
		return NULL;
	}
	*rtc = rtcMemory(player, rtcSize);
	return saveMemory(player, size);
}

// Where the saves of player 1's game from another device's netplay session are, by that session's
// players.
static void remoteFiles(int player, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	secondRemoteSavePath(firstPath, player, "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	secondRemoteSavePath(firstPath, player, "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

static void receivedFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	secondReceivedSavePath(firstPath, "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	secondReceivedSavePath(firstPath, "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

// Where a player's backups go: beside their save, and player 1's in the save folder's armGBA/Player
// 1 folder.
static bool backupFiles(int player, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	if (player || pairFile) {
		return saveFiles(player, file, rtcFile);
	}
	folderFiles(0, file, rtcFile);
	return true;
}

// Reads a player's save and clock from the files given, and keeps the save to be written to file
// and rtcFile; returns whether there was a save.
static bool keepSave(int player, const char* readFrom, const char* rtcFrom, const char* file, const char* rtcFile) {
	snprintf(keptSaves[player].file, sizeof(keptSaves[player].file), "%s", file);
	snprintf(keptSaves[player].rtcFile, sizeof(keptSaves[player].rtcFile), "%s", rtcFile);
	size_t size;
	void* save = saveMemory(player, &size);
	bool found = readFileInto(readFrom, save, size);
	size_t rtcSize;
	void* rtc = rtcMemory(player, &rtcSize);
	readFileInto(rtcFrom, rtc, rtcSize);
	keptSaves[player].hash = save ? hashOf(save, size) : 0;
	keptSaves[player].kept = true;
	keptCheck = 0;
	return found;
}

// A player's save of a game of their own, such as player 2's from Player 2's Game or a pair file:
// where RetroArch keeps that game's save.
static bool readSave(int player, const char* game) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	char file[PATH_SIZE];
	char rtcFile[PATH_SIZE];
	secondSavePath(game, "", "srm", firstPath, saveDirectory, file, sizeof(file));
	secondSavePath(game, "", "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
	return keepSave(player, file, rtcFile, file, rtcFile);
}

// A player's own save of player 1's game, in their own folder. One kept beside player 1's under the
// older " (Player N)" name is read until the folder has one.
static bool readPlayerSave(int player) {
	char file[SAVE_PATH_SIZE];
	char rtcFile[SAVE_PATH_SIZE];
	folderFiles(player, file, rtcFile);
	if (fileExists(file)) {
		return keepSave(player, file, rtcFile, file, rtcFile);
	}
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	char suffix[24];
	snprintf(suffix, sizeof(suffix), " (Player %d)", player + 1);
	char older[PATH_SIZE];
	char olderRtc[PATH_SIZE];
	secondSavePath(firstPath, suffix, "srm", firstPath, saveDirectory, older, sizeof(older));
	secondSavePath(firstPath, suffix, "rtc", firstPath, saveDirectory, olderRtc, sizeof(olderRtc));
	bool const found = keepSave(player, older, olderRtc, file, rtcFile);
	// Written to the folder next time saves are written, though unchanged.
	keptSaves[player].hash ^= found;
	return found;
}

// Whether RetroArch reads and writes player 1's save. It doesn't when the core keeps that save (in
// armGBA/Player 1, or with a pair file), or on a device that joined another's netplay session,
// where RetroArch would write the host's save over its own as the game closes.
static bool retroArchSaves(void) {
	return !keptSaves[0].kept && !netplayJoined;
}

static bool newerFile(const char* a, const char* b) {
	struct stat sa, sb;
	return !stat(a, &sa) && (stat(b, &sb) || sa.st_mtime > sb.st_mtime);
}

// Player 1's save when RetroArch keeps it in its folder but armGBA/Player 1 has a newer one, which
// takes its place on the first frame, after RetroArch has read its own.
static bool playerOneNewer;
static char playerOneFile[SAVE_PATH_SIZE];
static char playerOneRtc[SAVE_PATH_SIZE];

// Player 1's save: kept by the core in armGBA/Player 1 when Player 1's Save Folder says so, or left to
// RetroArch. The newer of the two files is read, so changing the option keeps the latest save.
static void readPlayerOneSave(void) {
	playerOneNewer = false;
	if (pairFile) {
		return;
	}
	char retroArchFile[SAVE_PATH_SIZE], retroArchRtc[SAVE_PATH_SIZE];
	retroArchFiles(retroArchFile, retroArchRtc);
	folderFiles(0, playerOneFile, playerOneRtc);
	const char* value = option("armgba_player1_saves");
	if (value && !strcmp(value, "armgba")) {
		bool const older = newerFile(retroArchFile, playerOneFile);
		keepSave(0, older ? retroArchFile : playerOneFile, older ? retroArchRtc : playerOneRtc, playerOneFile, playerOneRtc);
		// Written to the folder next time saves are written, though unchanged.
		keptSaves[0].hash ^= older;
	} else {
		playerOneNewer = newerFile(playerOneFile, retroArchFile);
	}
}

static void readNewerPlayerOne(void) {
	if (!playerOneNewer) {
		return;
	}
	playerOneNewer = false;
	size_t size, rtcSize;
	void* save = saveMemory(0, &size);
	void* rtc = rtcMemory(0, &rtcSize);
	readFileInto(playerOneFile, save, size);
	readFileInto(playerOneRtc, rtc, rtcSize);
	showMessage("Player 1's save: the newer one, from armGBA/Player 1");
}

// The other players' saves. A player on player 1's game gets a save of their own; until they have
// one, it starts new, or as a copy of player 1's, as the New Players' Saves option says.
static void readOtherSaves(void) {
	const char* value = option("armgba_new_save");
	bool const copy = value && !strcmp(value, "copy");
	for (int player = 1; player < playersHere && !othersBare; ++player) {
		const char* chosen = player == 1 ? secondPath : otherPaths[player];
		if (*chosen) {
			readSave(player, chosen);
		} else if (!readPlayerSave(player) && copy) {
			copySaves |= 1 << player;
		}
	}
	offerSaveChoices();
}

// Writes the kept saves and clocks that have changed; in netplay only as the game closes, on the
// device hosting it. A device that joined keeps the session's saves in armGBA/Remote.
static void writeSaves(bool closing) {
	if (netplaySeen && closing && netplayJoined) {
		keepSession();
	}
	if (netplaySeen && (!closing || netplayJoined)) {
		return;
	}
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		size_t size;
		void* save = saveMemory(player, &size);
		if (!keptSaves[player].kept || !save || !size) {
			continue;
		}
		uint64_t hash = hashOf(save, size);
		if (hash == keptSaves[player].hash) {
			continue;
		}
		keptSaves[player].hash = hash;
		secondMakeFolders(keptSaves[player].file);
		writeFile(keptSaves[player].file, save, size);
		size_t rtcSize;
		void* rtc = rtcMemory(player, &rtcSize);
		if (rtc && rtcSize) {
			writeFile(keptSaves[player].rtcFile, rtc, rtcSize);
		}
	}
}

static bool savesKept(void) {
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		if (keptSaves[player].kept) {
			return true;
		}
	}
	return false;
}

static void forgetSaves(void) {
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		keptSaves[player].kept = false;
	}
	copySaves = 0;
}

static void copyFirstSave(void) {
	for (int player = 1; player < playersHere; ++player) {
		if (!(copySaves & 1 << player)) {
			continue;
		}
		if (!gameBoy) {
			memcpy(gbas[player]->savedata.storage, gba->savedata.storage, sizeof(gba->savedata.storage));
			continue;
		}
		size_t size;
		size_t otherSize;
		const void* const first = saveMemory(0, &size);
		void* const other = saveMemory(player, &otherSize);
		if (first && other && size == otherSize) {
			memcpy(other, first, size);
		}
	}
	copySaves = 0;
}

// Backups, and the Back Up Save, Load Save and Send Save options.

// "20261018-153012-2" as "2026-10-18 15:30:12 (2)".
static void stampLabel(const char* stamp, char* out, size_t outSize) {
	snprintf(out, outSize, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s%s%s%s", stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11, stamp + 13,
	         stamp[15] ? " (" : "", stamp[15] ? stamp + 16 : "", stamp[15] ? ")" : "");
}

static bool fileHolds(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	uint8_t* held = malloc(size + 1);
	bool const same = held && fread(held, 1, size + 1, f) == size && !memcmp(held, data, size);
	free(held);
	fclose(f);
	return same;
}

// Writes a save, and its clock, as backups of the given save file, stamped with the current time.
// Skips the write if the newest backup already holds that save.
// Returns 1 if it wrote them, 0 if the newest backup already holds the save (stamp is then that
// backup's), or -1 if the write failed.
static int writeBackup(const char* file, const char* rtcFile, const void* save, size_t size, const void* rtc, size_t rtcSize,
                       char stamp[SECOND_STAMP]) {
	char out[BACKUP_PATH_SIZE];
	char newest[1][SECOND_STAMP];
	if (secondBackups(file, newest, 1)) {
		secondBackupPath(file, newest[0], out, sizeof(out));
		if (fileHolds(out, save, size)) {
			memcpy(stamp, newest[0], SECOND_STAMP);
			return 0;
		}
	}
	secondNewStamp(file, stamp);
	secondBackupPath(file, stamp, out, sizeof(out));
	secondMakeFolders(out);
	if (!writeFile(out, save, size)) {
		return -1;
	}
	if (rtc && rtcSize) {
		secondBackupPath(rtcFile, stamp, out, sizeof(out));
		writeFile(out, rtc, rtcSize);
	}
	return 1;
}

static bool blankSave(const void* save, size_t size) {
	for (size_t i = 1; i < size; ++i) {
		if (((const uint8_t*) save)[i] != ((const uint8_t*) save)[0]) {
			return false;
		}
	}
	return true;
}

// Backs up a player's save, and its clock, as they are in memory, which is newer than the file.
// Returns 1 if it did, 2 if the newest backup already holds the save, 0 if there was nothing to
// back up (no file and nothing saved yet), or -1 if the write failed.
static int backUpPlayer(int player, char stamp[SECOND_STAMP]) {
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE], base[SAVE_PATH_SIZE], rtcBase[SAVE_PATH_SIZE];
	size_t size, rtcSize;
	void* rtc;
	void* save = liveSave(player, &size, &rtc, &rtcSize);
	if (!saveFiles(player, file, rtcFile) || !backupFiles(player, base, rtcBase) || !save || !size) {
		return 0;
	}
	if (!fileExists(file) && blankSave(save, size)) {
		return 0;
	}
	int const written = writeBackup(base, rtcBase, save, size, rtc, rtcSize, stamp);
	return written < 0 ? -1 : written ? 1 : 2;
}

static void pruneBackups(const char* saveFile, const char* rtcFile) {
	const char* value = option("armgba_keep_saves");
	size_t const keep = value ? (size_t) atoi(value) : 10;
	enum { MOST = 1000 };
	char (*stamps)[SECOND_STAMP] = keep ? malloc(MOST * SECOND_STAMP) : NULL;
	if (!stamps) {
		return;
	}
	size_t const count = secondBackups(saveFile, stamps, MOST);
	for (size_t i = keep; i < count; ++i) {
		char file[BACKUP_PATH_SIZE];
		secondBackupPath(saveFile, stamps[i], file, sizeof(file));
		remove(file);
		secondBackupPath(rtcFile, stamps[i], file, sizeof(file));
		remove(file);
	}
	free(stamps);
}

// A device that joined another's netplay session keeps the session's saves, never over its own, in
// armGBA/Remote by the session's players, for Load Save.
static void keepSession(void) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	for (int player = 0; player < playersHere; ++player) {
		size_t size, rtcSize;
		void* rtc;
		void* save = liveSave(player, &size, &rtc, &rtcSize);
		if (!save || !size || blankSave(save, size)) {
			continue;
		}
		char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
		char stamp[SECOND_STAMP];
		secondRemoteSavePath(gameOf(player), player, "srm", firstPath, saveDirectory, file, sizeof(file));
		secondRemoteSavePath(gameOf(player), player, "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
		if (writeBackup(file, rtcFile, save, size, rtc, rtcSize, stamp) > 0) {
			pruneBackups(file, rtcFile);
		}
	}
}

// The player Load Into names, when they are playing here, or -1.
static int loadIntoPlayer(void) {
	const char* into = option("armgba_load_into");
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
	int const player = into && into[0] >= '1' && into[0] <= '4' ? into[0] - '1' : -1;
	return player >= 0 && saveFiles(player, file, rtcFile) ? player : -1;
}

// Where a remote save goes. Load Into picks the player when it is set; otherwise the save goes to
// the other person:
// - the session's player 1 (the host) to player 2 here, if there is one, or else player 1;
// - its player 2 to player 1 here;
// - players 3 and 4 to the same players here, or else player 1.
static int remoteTarget(int from) {
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
	int const named = loadIntoPlayer();
	if (named >= 0) {
		return named;
	}
	if (from == 0) {
		return saveFiles(1, file, rtcFile) ? 1 : 0;
	}
	return from > 1 && saveFiles(from, file, rtcFile) ? from : 0;
}

// Where a received save goes: where Load Into says, or else to player 2, the other person's place,
// when there is one, or player 1.
static int receivedTarget(void) {
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
	int const named = loadIntoPlayer();
	return named >= 0 ? named : saveFiles(1, file, rtcFile) ? 1 : 0;
}

static void startChoices(struct SaveChoices* list) {
	list->count = 0;
	list->option->values[0] = (struct retro_core_option_value) { "off", "Off" };
	list->option->values[1] = (struct retro_core_option_value) { NULL, NULL };
}

static void addChoice(struct SaveChoices* list, const char* value, const char* label) {
	if (list->count >= BACKUP_CHOICES) {
		return;
	}
	int const n = list->count++;
	snprintf(list->values[n], sizeof(list->values[0]), "%s", value);
	snprintf(list->labels[n], sizeof(list->labels[0]), "%s", label);
	list->option->values[n + 1] = (struct retro_core_option_value) { list->values[n], list->labels[n] };
	list->option->values[n + 2] = (struct retro_core_option_value) { NULL, NULL };
}

// Adds a save's newest backups to a list, as "<tag> <stamp>" named "<name>: <time>".
static void addBackupChoices(struct SaveChoices* list, const char* file, int most, const char* tag, const char* name) {
	char stamps[BACKUP_CHOICES][SECOND_STAMP];
	size_t const count = secondBackups(file, stamps, most);
	for (size_t i = 0; i < count; ++i) {
		char value[SECOND_STAMP + 4], label[80], when[32];
		stampLabel(stamps[i], when, sizeof(when));
		snprintf(value, sizeof(value), "%.3s %.*s", tag, SECOND_STAMP - 1, stamps[i]);
		snprintf(label, sizeof(label), "%s: %s", name, when);
		addChoice(list, value, label);
	}
}

// Fills Back Up Save's and Load Into's choices with the players playing, and Load Save's and Send
// Save's with their saves.
static void fillSaveChoices(void) {
	if (!loadChoices.option || !sendChoices.option || !backUpOption || !loadIntoOption) {
		return;
	}
	const char* value = option("armgba_list_saves");
	backupsListed = value ? atoi(value) : 0;
	backupsListed = backupsListed > 0 && backupsListed <= BACKUP_CHOICES ? backupsListed : 10;
	char files[MAX_PLAYERS][SAVE_PATH_SIZE];
	char remotes[MAX_PLAYERS][SAVE_PATH_SIZE];
	char rtcFile[SAVE_PATH_SIZE];
	bool playing[MAX_PLAYERS];
	bool remote[MAX_PLAYERS];
	char received[SAVE_PATH_SIZE];
	char stamp[1][SECOND_STAMP];
	receivedFiles(received, rtcFile);
	bool const anyReceived = *firstPath && secondBackups(received, stamp, 1);
	int players = 0;
	int groups = anyReceived;
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		playing[player] = backupFiles(player, files[player], rtcFile);
		remoteFiles(player, remotes[player], rtcFile);
		remote[player] = *firstPath && secondBackups(remotes[player], stamp, 1);
		players += playing[player];
		groups += playing[player] + remote[player];
	}
	// Each player's current save, then the backups, shared out.
	int const current = players > 1 ? players : 0;
	int const room = BACKUP_CHOICES - current;
	int const each = groups && backupsListed > room / groups ? room / groups : backupsListed;
	int shown = 0;
	backUpOption->values[0] = (struct retro_core_option_value) { "off", "Off" };
	loadIntoOption->values[0] = (struct retro_core_option_value) { "same", "Same Player" };
	startChoices(&loadChoices);
	startChoices(&sendChoices);
	addChoice(&sendChoices, "1 now", "Player 1: Now");
	addBackupChoices(&sendChoices, files[0], each, "1", "Player 1");
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		if (!playing[player]) {
			continue;
		}
		snprintf(playerValues[player], sizeof(playerValues[0]), "%d", player + 1);
		snprintf(playerLabels[player], sizeof(playerLabels[0]), "Player %d", player + 1);
		++shown;
		backUpOption->values[shown] = (struct retro_core_option_value) { playerValues[player], playerLabels[player] };
		loadIntoOption->values[shown] = (struct retro_core_option_value) { playerValues[player], playerLabels[player] };
		char tag[4], now[8], nowLabel[24];
		snprintf(tag, sizeof(tag), "%d", player + 1);
		if (current) {
			snprintf(now, sizeof(now), "%s now", tag);
			snprintf(nowLabel, sizeof(nowLabel), "Player %d: Now", player + 1);
			addChoice(&loadChoices, now, nowLabel);
		}
		addBackupChoices(&loadChoices, files[player], each, tag, playerLabels[player]);
	}
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		if (remote[player]) {
			char tag[4], name[48];
			snprintf(tag, sizeof(tag), "r%d", player + 1);
			snprintf(name, sizeof(name), "Remote Player %d into Player %d", player + 1, remoteTarget(player) + 1);
			addBackupChoices(&loadChoices, remotes[player], each, tag, name);
			snprintf(name, sizeof(name), "Remote Player %d", player + 1);
			addBackupChoices(&sendChoices, remotes[player], each, tag, name);
		}
	}
	if (anyReceived) {
		char name[32];
		snprintf(name, sizeof(name), "Received into Player %d", receivedTarget() + 1);
		addBackupChoices(&loadChoices, received, each, "x", name);
		addBackupChoices(&sendChoices, received, each, "x", "Received");
	}
	backUpOption->values[shown + 1] = (struct retro_core_option_value) { NULL, NULL };
	loadIntoOption->values[shown + 1] = (struct retro_core_option_value) { NULL, NULL };
	listedInto = loadIntoPlayer();
}

static void offerSaveChoices(void) {
	if (coreOptions && loadChoices.option) {
		fillSaveChoices();
		setOptions();
	}
}

static bool sameSize(int from, int to) {
	size_t size, toSize, rtcSize, toRtcSize;
	void* rtc;
	void* toRtc;
	void* save = liveSave(from, &size, &rtc, &rtcSize);
	void* toSave = liveSave(to, &toSize, &toRtc, &toRtcSize);
	return save && toSave && size == toSize;
}

// Holding the Restart Buttons for a second restarts that player's console, as its power switch
// would. Games that reset on A, B, Select and Start do so at once by themselves.
enum { RESTART_FRAMES = 60 };
static uint8_t restartHeld[MAX_PLAYERS];

static void restartConsole(int player) {
	if (gameBoy) {
		libretroGbSelect(player);
		libretroGbRestart();
		libretroGbSelect(0);
	} else {
		gbaReset(gbas[player], skipBios);
	}
	char message[48];
	snprintf(message, sizeof(message), playersHere > 1 ? "Player %d's console restarted" : "Restarted", player + 1);
	notify(message, 120);
}

// Where Load Save takes a save from: a player's own backups or current save, a session of another
// device's that this one joined, or the other player of a Separate link.
enum { FROM_PLAYER, FROM_REMOTE, FROM_RECEIVED };

// Copies player from's current save, and clock, over player to's, in memory and in to's files.
static void copyCurrentSave(int from, int to, const char* toFile, const char* toRtc) {
	size_t size, rtcSize, fromSize, fromRtcSize;
	void* rtc;
	void* fromRtc;
	void* save = liveSave(to, &size, &rtc, &rtcSize);
	memcpy(save, liveSave(from, &fromSize, &fromRtc, &fromRtcSize), size);
	secondMakeFolders(toFile);
	writeFile(toFile, save, size);
	if (rtc && fromRtc && rtcSize == fromRtcSize) {
		memcpy(rtc, fromRtc, rtcSize);
		writeFile(toRtc, rtc, rtcSize);
	}
}

// Makes a backup player to's save file, moving it when it is to's own and copying it otherwise, then
// reads it into to's memory.
static void loadBackup(int to, bool move, const char* file, const char* rtcFile, const char* toFile, const char* toRtc) {
	if (move) {
		rename(file, toFile);
		rename(rtcFile, toRtc);
	} else {
		secondMakeFolders(toFile);
		copyFile(file, toFile);
		copyFile(rtcFile, toRtc);
	}
	size_t size, rtcSize;
	void* rtc;
	void* save = liveSave(to, &size, &rtc, &rtcSize);
	readFileInto(toFile, save, size);
	readFileInto(toRtc, rtc, rtcSize);
}

static void loadedMessage(char* message, size_t size, int source, int from, int to, const char* stamp) {
	char when[32];
	if (!strcmp(stamp, "now")) {
		snprintf(message, size, "Player %d's save is now a copy of player %d's", to + 1, from + 1);
		return;
	}
	stampLabel(stamp, when, sizeof(when));
	if (source == FROM_RECEIVED) {
		snprintf(message, size, "Player %d's save is now the one received %s", to + 1, when);
	} else if (source == FROM_REMOTE) {
		snprintf(message, size, "Player %d's save is now remote player %d's from %s", to + 1, from + 1, when);
	} else if (from == to) {
		snprintf(message, size, "Player %d's save is now the one from %s", to + 1, when);
	} else {
		snprintf(message, size, "Player %d's save is now player %d's from %s", to + 1, from + 1, when);
	}
}

// Load Save: makes a backup, a player's current save ("now"), a remote or a received save player
// to's save, backing up the one it replaces, and restarts that console. A save given to another
// player is copied.
static void loadSave(int source, int from, const char* stamp, int to) {
	char message[160];
	char fromBase[SAVE_PATH_SIZE], fromRtcBase[SAVE_PATH_SIZE];
	char toFile[SAVE_PATH_SIZE], toRtc[SAVE_PATH_SIZE], toBase[SAVE_PATH_SIZE], toRtcBase[SAVE_PATH_SIZE];
	char file[BACKUP_PATH_SIZE], rtcFile[BACKUP_PATH_SIZE];
	char replaced[SECOND_STAMP];
	bool const current = !strcmp(stamp, "now");
	bool fromPlaying = true;
	if (source == FROM_REMOTE) {
		remoteFiles(from, fromBase, fromRtcBase);
	} else if (source == FROM_RECEIVED) {
		receivedFiles(fromBase, fromRtcBase);
	} else {
		fromPlaying = backupFiles(from, fromBase, fromRtcBase);
	}
	bool const remote = source != FROM_PLAYER;
	bool const toPlaying = saveFiles(to, toFile, toRtc) && backupFiles(to, toBase, toRtcBase);
	const char* fromGame = remote ? firstPath : gameOf(from);
	if (fromPlaying) {
		secondBackupPath(fromBase, stamp, file, sizeof(file));
		secondBackupPath(fromRtcBase, stamp, rtcFile, sizeof(rtcFile));
	}
	if (!fromPlaying || !toPlaying) {
		snprintf(message, sizeof(message), "Load Save: player %d isn't playing", (fromPlaying ? to : from) + 1);
	} else if (netplaySeen || linkSession()) {
		snprintf(message, sizeof(message), "Load Save: not in netplay");
	} else if (strcmp(fromGame, gameOf(to))) {
		snprintf(message, sizeof(message), "Load Save: that save is for another game than player %d's", to + 1);
	} else if (current && from == to) {
		snprintf(message, sizeof(message), "Load Save: that is player %d's save already", to + 1);
	} else if (current && !sameSize(from, to)) {
		snprintf(message, sizeof(message), "Load Save: players %d and %d's saves differ in size", from + 1, to + 1);
	} else if (!current && !fileExists(file)) {
		snprintf(message, sizeof(message), "Load Save: that save is gone");
	} else if (backUpPlayer(to, replaced) < 0) {
		snprintf(message, sizeof(message), "Load Save: player %d's save can't be backed up", to + 1);
	} else {
		if (current) {
			copyCurrentSave(from, to, toFile, toRtc);
		} else {
			loadBackup(to, from == to && !remote, file, rtcFile, toFile, toRtc);
		}
		if (to < playersHere && keptSaves[to].kept) {
			size_t size, rtcSize;
			void* rtc;
			void* save = liveSave(to, &size, &rtc, &rtcSize);
			keptSaves[to].hash = save ? hashOf(save, size) : 0;
		}
		restartConsole(to);
		pruneBackups(toBase, toRtcBase);
		offerSaveChoices();
		loadedMessage(message, sizeof(message), source, from, to, stamp);
	}
	notify(message, 240);
}

static void backUpSave(int player) {
	char message[160];
	char file[SAVE_PATH_SIZE];
	char rtcFile[SAVE_PATH_SIZE];
	char stamp[SECOND_STAMP];
	size_t size, rtcSize;
	void* rtc;
	if (!backupFiles(player, file, rtcFile)) {
		snprintf(message, sizeof(message), "Back Up Save: player %d isn't playing", player + 1);
	} else if (!liveSave(player, &size, &rtc, &rtcSize) || !size) {
		snprintf(message, sizeof(message), "Back Up Save: the game has no save");
	} else {
		int const done = backUpPlayer(player, stamp);
		char when[32];
		if (done > 0) {
			stampLabel(stamp, when, sizeof(when));
		}
		if (done == 1) {
			pruneBackups(file, rtcFile);
			offerSaveChoices();
			snprintf(message, sizeof(message), "Backed up player %d's save: %s", player + 1, when);
		} else if (done == 2) {
			snprintf(message, sizeof(message), "Player %d's save is unchanged since its backup of %s", player + 1, when);
		} else {
			snprintf(message, sizeof(message), done ? "Back Up Save: player %d's save can't be written" : "Back Up Save: player %d has nothing saved yet",
			         player + 1);
		}
	}
	notify(message, 240);
}

// Sends player 1's save of this game to the other player of a Separate link: when chosen (asked),
// and as the other player pairs.
static void sendSave(bool asked) {
	size_t size, rtcSize;
	void* rtc;
	void* save = liveSave(0, &size, &rtc, &rtcSize);
	char game[1024];
	secondGameName(firstPath, game, sizeof(game));
	char const* problem = !save || !size || blankSave(save, size) ? "Send Save: nothing saved yet"
	                    : !linkSendSave(game, save, size, rtc, rtcSize) ? "Send Save: no one is linked"
	                                                                     : NULL;
	if (asked) {
		showMessage(problem ? problem : "Sent this save to the other player");
	}
}

static void sendSaveOnPairing(void) {
	sendSave(false);
}

static void sendBackup(int source, int from, const char* stamp) {
	char base[SAVE_PATH_SIZE], rtcBase[SAVE_PATH_SIZE], file[BACKUP_PATH_SIZE], rtcFile[BACKUP_PATH_SIZE], game[1024];
	if (source == FROM_REMOTE) {
		remoteFiles(from, base, rtcBase);
	} else if (source == FROM_RECEIVED) {
		receivedFiles(base, rtcBase);
	} else {
		backupFiles(0, base, rtcBase);
	}
	secondBackupPath(base, stamp, file, sizeof(file));
	secondBackupPath(rtcBase, stamp, rtcFile, sizeof(rtcFile));
	secondGameName(firstPath, game, sizeof(game));
	size_t size, rtcSize;
	void* save = readWholeFile(file, &size);
	void* rtc = readWholeFile(rtcFile, &rtcSize);
	showMessage(!save                                         ? "Send Save: that save is gone"
	            : !linkSendSave(game, save, size, rtc, rtcSize) ? "Send Save: no one is linked"
	                                                            : "Sent that save to the other player");
	free(save);
	free(rtc);
}

// A save the other player sent, of the game named, kept as a received save of that game, unless the
// newest one received already holds it.
static void saveReceived(const char* game, const uint8_t* save, size_t size, const uint8_t* rtc, size_t rtcSize) {
	// The game's file, for where its saves are: player 1's, a game beside it, or else beside player 1's.
	char path[PATH_SIZE];
	char name[1024];
	secondGameName(firstPath, name, sizeof(name));
	snprintf(path, sizeof(path), "%s", firstPath);
	for (size_t i = 0; strcmp(name, game) && i < secondCount; ++i) {
		secondGameName(secondGames[i].path, name, sizeof(name));
		snprintf(path, sizeof(path), "%s", secondGames[i].path);
	}
	if (strcmp(name, game)) {
		char const* slash = strrchr(firstPath, '/');
		snprintf(path, sizeof(path), "%.*s%s.srm", slash ? (int) (slash - firstPath + 1) : 0, firstPath, game);
	}
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE], stamp[SECOND_STAMP];
	secondReceivedSavePath(path, "srm", firstPath, saveDirectory, file, sizeof(file));
	secondReceivedSavePath(path, "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
	if (writeBackup(file, rtcFile, save, size, rtc, rtcSize, stamp) > 0) {
		pruneBackups(file, rtcFile);
		offerSaveChoices();
		char message[1200];
		snprintf(message, sizeof(message), "Received the other player's save of %s", game);
		showMessage(message);
	}
}

// Back Up Save and Load Save act once when chosen, then go back to Off. Chosen when a game starts,
// as RetroArch may have kept them, they only go back to Off.
static void readSaveActions(bool act) {
	const char* value = option("armgba_back_up_save");
	if (value && strcmp(value, "off")) {
		int const player = value[0] - '1';
		struct retro_variable off = { "armgba_back_up_save", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		if (act && player >= 0 && player < MAX_PLAYERS) {
			backUpSave(player);
		}
	}
	value = option("armgba_send_save");
	if (value && strcmp(value, "off")) {
		char send[SECOND_STAMP + 4];
		snprintf(send, sizeof(send), "%s", value);
		struct retro_variable off = { "armgba_send_save", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		if (act && !strcmp(send, "1 now")) {
			sendSave(true);
		} else if (act && !strncmp(send, "1 ", 2)) {
			sendBackup(FROM_PLAYER, 0, send + 2);
		} else if (act && send[0] == 'r' && send[1] >= '1' && send[1] <= '4' && send[2] == ' ') {
			sendBackup(FROM_REMOTE, send[1] - '1', send + 3);
		} else if (act && send[0] == 'x' && send[1] == ' ') {
			sendBackup(FROM_RECEIVED, 0, send + 2);
		}
	}
	value = option("armgba_load_save");
	if (value && strcmp(value, "off")) {
		char load[SECOND_STAMP + 4];
		snprintf(load, sizeof(load), "%s", value);
		struct retro_variable off = { "armgba_load_save", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		int const named = loadIntoPlayer();
		if (act && load[0] >= '1' && load[0] <= '4' && load[1] == ' ') {
			int const from = load[0] - '1';
			loadSave(FROM_PLAYER, from, load + 2, named >= 0 ? named : from);
		} else if (act && load[0] == 'r' && load[1] >= '1' && load[1] <= '4' && load[2] == ' ') {
			loadSave(FROM_REMOTE, load[1] - '1', load + 3, remoteTarget(load[1] - '1'));
		} else if (act && load[0] == 'x' && load[1] == ' ') {
			loadSave(FROM_RECEIVED, 0, load + 2, receivedTarget());
		}
	}
}

// Starting and stopping the consoles: loading a game, the other players' consoles, Restart.

// Nintendo's gba_bios.bin from the system directory, or else armGBA's own BIOS, as BIOS picks.
static bool loadBios(struct GBA* machine) {
	const char* directory = NULL;
	uint8_t bios[SIZE_BIOS];
	bool read = false;
	if (!optionIs("armgba_bios", "open-source") && environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) && directory) {
		char path[PATH_SIZE];
		snprintf(path, sizeof(path), "%s/gba_bios.bin", directory);
		FILE* f = fopen(path, "rb");
		read = f && fread(bios, 1, sizeof(bios), f) == sizeof(bios);
		if (f) {
			fclose(f);
		}
	}
	if (!read) {
		logMessage(RETRO_LOG_INFO, "armGBA: using armGBA's BIOS");
		return gbaLoadBios(machine, armgbaBios, SIZE_BIOS);
	}
	return gbaLoadBios(machine, bios, sizeof(bios));
}

static struct GBA* startAdvance(const struct retro_game_info* game) {
	struct GBA* machine = gbaCreate();
	if (!machine) {
		return NULL;
	}
	// RetroArch loads the save file into save memory after this, and writes it back from there.
	machine->savedata.backed = true;
	machine->renderer.rgb565 = true;
	if (!loadBios(machine) || !gbaLoadRom(machine, game->data, game->size)) {
		gbaDestroy(machine);
		return NULL;
	}
	return machine;
}

// Starts the other players' Game Boys; RetroArch's own netplay runs them all.
static bool startOtherGameBoys(int count, const void* const data[], const size_t sizes[]) {
	const char* directory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
	char error[256];
	bool const adapter = adapterWanted(count);
	libretroGbSetPlayers(count, adapter);
	int loaded = 1;
	while (loaded < count) {
		libretroGbSelect(loaded);
		if (!libretroGbLoad(data[loaded], sizes[loaded], directory, gbModel, gbColorCorrection, AUDIO_RATE, error, sizeof(error))) {
			break;
		}
		++loaded;
	}
	if (loaded < count) {
		for (int i = loaded; i > 0; --i) {
			libretroGbSelect(i);
			libretroGbUnload();
		}
		libretroGbSelect(0);
		libretroGbSetPlayers(1, false);
		showMessage(error);
		return false;
	}
	libretroGbSelect(0);
	gbTogether = true;
	gbAdapter = adapter;
	playersHere = count;
	linkSetGameBoy(false);
	return true;
}

static void stopOtherGameBoys(void) {
	for (int i = playersHere - 1; i > 0; --i) {
		libretroGbSelect(i);
		libretroGbUnload();
	}
	libretroGbSelect(0);
	libretroGbSetPlayers(1, false);
	gbTogether = false;
	gbAdapter = false;
	playersHere = 1;
	linkSetGameBoy(true);
}

// Plugs the Link Port option's choice into each console: the Wireless Adapter, or the cable between
// the consoles here.
static void attachPorts(void) {
	const char* value = option("armgba_link_port");
	bool wireless = value && !strcmp(value, "wireless") && (togetherWanted() || separateWanted());
	linkSetWireless(wireless && !gbaTogether);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (!gbas[i]) {
			continue;
		}
		airs[i] = (struct RfuAir) { i, false, airSend };
		rfuAttach(gbas[i], wireless, gbaTogether ? &airs[i] : linkAir());
		if (gbaTogether) {
			sioAttach(gbas[i], wireless ? NULL : &cables[i]);
		}
	}
}

static void setAdvanceOffsets(void) {
	static const int32_t behind[MAX_PLAYERS] = { 0, GBA_FRAME_CYCLES / 2, GBA_FRAME_CYCLES / 4, GBA_FRAME_CYCLES * 3 / 4 };
	memcpy(gbaAhead, behind, sizeof(gbaAhead));
}

static bool startOtherAdvances(int count, const void* const data[], const size_t sizes[]) {
	for (int i = 1; i < count; ++i) {
		struct retro_game_info game = { NULL, data[i], sizes[i], NULL };
		gbas[i] = startAdvance(&game);
		if (!gbas[i]) {
			for (int k = i - 1; k > 0; --k) {
				gbaDestroy(gbas[k]);
				gbas[k] = NULL;
			}
			return false;
		}
	}
	if (threadedVideo) {
		renderThreadStop(gba);
		threadedVideo = 0;
	}
	gbaTogether = true;
	playersHere = count;
	linkSetGba(NULL);
	for (int i = 0; i < count; ++i) {
		if (i) {
			gbaReset(gbas[i], skipBios);
		}
		cables[i] = (struct SioLink) { .context = (void*) (intptr_t) i, .players = count, .id = i, .begin = cableBegin, .finish = cableFinish, .wrote = cableWrote };
	}
	attachPorts();
	setAdvanceOffsets();
	workersStart(count);
	return true;
}

static void stopOtherAdvances(void) {
	workersStop();
	for (int i = 1; i < MAX_PLAYERS; ++i) {
		if (gbas[i]) {
			gbaDestroy(gbas[i]);
		}
		gbas[i] = NULL;
	}
	gbaTogether = false;
	playersHere = 1;
	sioAttach(gba, NULL);
	linkSetGba(gba);
	attachPorts();
}

// The game each other player's console runs: player 2's from the frontend, the menu or a pair file,
// players 3 and 4's from a pair file, or else player 1's. Returns false if one can't be read.
static bool readOtherGames(const struct retro_game_info* second, const void* sameData, size_t sameSize, int count,
                           const void* data[], size_t sizes[], void* read[]) {
	for (int i = 1; i < count; ++i) {
		const char* path = i == 1 ? secondPath : otherPaths[i];
		if (i == 1 && second) {
			data[i] = second->data;
			sizes[i] = second->size;
		} else if (othersBare) {
			data[i] = "";
			sizes[i] = 0;
		} else if (!*path && sameData) {
			data[i] = sameData;
			sizes[i] = sameSize;
		} else {
			data[i] = read[i] = secondRead(*path ? path : firstPath, &sizes[i]);
		}
		if (!data[i]) {
			char message[300];
			snprintf(message, sizeof(message), "armGBA can't read player %d's game", i + 1);
			showMessage(message);
			return false;
		}
	}
	return true;
}

static void announcePlayers(void) {
	char message[300];
	const char* name = strrchr(secondPath, '/');
	if (othersBare) {
		snprintf(message, sizeof(message), "%d players, %s with no cartridge", playersHere, playersHere > 2 ? "the others" : "player 2");
	} else if (playersHere > 2) {
		snprintf(message, sizeof(message), "%d players", playersHere);
	} else {
		snprintf(message, sizeof(message), "Player 2: %.240s", *secondPath ? (name ? name + 1 : secondPath) : "the same game, with its own save");
	}
	notify(message, 240);
}

// Starts the other players' consoles beside player 1's: two to four in all with Together, as
// many as a pair file names, or two with the game the frontend loaded for player 2.
static void startOthers(const struct retro_game_info* second, const void* sameData, size_t sameSize) {
	secondFromFrontend = second && !pairFile;
	othersBare = !second && bareWanted();
	int const count = pairFile ? pairCount : second ? 2 : playersWanted();
	if (!pairFile) {
		const char* choice = second ? NULL : chosenSecond();
		snprintf(secondPath, sizeof(secondPath), "%s", second && second->path ? second->path : choice ? choice : "");
	}
	const void* data[MAX_PLAYERS] = { NULL };
	size_t sizes[MAX_PLAYERS] = { 0 };
	void* read[MAX_PLAYERS] = { NULL };
	bool started = readOtherGames(pairFile ? NULL : second, sameData, sameSize, count, data, sizes, read);
	if (started) {
		started = gameBoy ? startOtherGameBoys(count, data, sizes) : startOtherAdvances(count, data, sizes);
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		free(read[i]);
	}
	if (!started) {
		secondPath[0] = '\0';
		return;
	}
	if (!secondFromFrontend) {
		readOtherSaves();
	}
	readOptions();
	announcePlayers();
}

static void stopOthers(void) {
	writeSaves(false);
	for (int player = 1; player < MAX_PLAYERS; ++player) {
		keptSaves[player].kept = false;
	}
	if (gbTogether) {
		stopOtherGameBoys();
	}
	if (gbaTogether) {
		stopOtherAdvances();
	}
	secondPath[0] = '\0';
	secondFromFrontend = false;
	othersBare = false;
}

// Offers RetroArch the netplay that carries the cable when Link Mode is Separate, and its usual
// netplay otherwise.
static void offerLinkNetplay(void) {
	linkSetNetplay(separateWanted() && !togetherRunning());
}

static bool runningAsWanted(void) {
	bool const want = togetherWanted();
	if (togetherRunning() != want) {
		return false;
	}
	if (!want) {
		return true;
	}
	const char* choice = chosenSecond();
	bool same = !strcmp(choice ? choice : "", secondPath) && playersHere == playersWanted();
	same &= !gameBoy || gbAdapter == adapterWanted(playersHere);
	same &= othersBare == bareWanted();
	return same;
}

// Restart takes in a change of Link Mode, Players, a Link Port or Player 2's Game.
static void applyLinkOptions(void) {
	if (secondFromFrontend || pairFile) {
		return;
	}
	rememberSecondChoice();
	if (runningAsWanted()) {
		return;
	}
	if (togetherRunning()) {
		stopOthers();
	}
	if (togetherWanted()) {
		startOthers(NULL, NULL, 0);
	}
	announceGeometry();
	showOptions();
	showButtons();
}

// Describes memory for the frontend's achievements and cheat search, the same way mGBA's and
// gambatte-libretro's cores do.
static void announceMemory(void) {
	struct retro_memory_descriptor descriptors[11];
	size_t count = 0;
	if (gameBoy) {
		count = libretroGbMemoryMap(descriptors);
		gbMapColor = libretroGbIsColor();
	}
	if (!count && gba) {
		size_t romSize = gba->romSize + (gba->romSize & 1);
		const struct retro_memory_descriptor advance[] = {
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->iwram, 0, 0x03000000, 0xFF000000, 0, SIZE_IWRAM, NULL },
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->ewram, 0, 0x02000000, 0xFF000000, 0, SIZE_EWRAM, NULL },
			{ 0, gba->savedata.storage, 0, 0x0E000000, 0, 0, saveSize(gba), NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x08000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0A000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0C000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->bios, 0, 0x00000000, 0, 0, SIZE_BIOS, NULL },
			{ 0, gba->vram, 0, 0x06000000, 0xFF000000, 0, SIZE_VRAM, NULL },
			{ 0, gba->palette, 0, 0x05000000, 0xFF000000, 0, SIZE_PALETTE, NULL },
			{ 0, gba->oam, 0, 0x07000000, 0xFF000000, 0, SIZE_OAM, NULL },
			{ 0, gba->io, 0, 0x04000000, 0, 0, SIZE_IO, NULL },
		};
		count = COUNT(advance);
		memcpy(descriptors, advance, sizeof(advance));
	}
	struct retro_memory_map map = { descriptors, count };
	environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool yes = true;
	environment(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
}

// A restart or a loaded state can switch the machine to or from a Game Boy Color, which changes the
// memory map.
static void checkGameBoyMemory(void) {
	if (gbMapColor != libretroGbIsColor()) {
		announceMemory();
	}
}

// Game Boy and Game Boy Color games are told apart from Game Boy Advance games by file extension.
static bool hasGameBoyExtension(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

static bool isPairFile(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && (!strcasecmp(dot, ".gb2") || !strcasecmp(dot, ".gba2"));
}

static bool isGameBoyRom(const void* data, size_t size) {
	// A Game Boy cartridge has the Nintendo logo at 0x104.
	return size >= 0x150 && ((const uint8_t*) data)[0x104] == 0xCE && ((const uint8_t*) data)[0x105] == 0xED;
}

static bool loadGameBoy(const struct retro_game_info* game, const struct retro_game_info* second) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no XRGB8888");
		return false;
	}
	const char* directory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
	offerSecondGames(game->path, true);
	recallSecondChoice();
	gbTogether = false;
	playersHere = 1;
	libretroGbSetPlayers(1, false);
	readOptions();
	char error[256];
	libretroGbSelect(0);
	if (!libretroGbLoad(game->data, game->size, directory, gbModel, gbColorCorrection, AUDIO_RATE, error, sizeof(error))) {
		showMessage(error);
		libretroGbUnload();
		return false;
	}
	gameBoy = true;
	gbStretched = false;
	readPlayerOneSave();
	tiltCartridge = game->size > 0x147 && ((const uint8_t*) game->data)[0x147] == 0x22;
	linkSetGameBoy(true);
	if (second || togetherWanted() || pairFile) {
		startOthers(second, game->data, game->size);
	}
	offerLinkNetplay();
	showOptions();
	showButtons();
	announceMemory();
	return true;
}

// This Console's Cartridge: with Separate, player 1's console may start with its cartridge out,
// waiting for a game sent over the cable. The game comes back from its file when put back in, its
// save memory as it was; if it was out from the start, RetroArch read no save into it, so the save
// comes from RetroArch's save file once the console has been reset. Returns whether it went back in.
static bool cartridgeOut;
static bool cartridgeSaveUnread;

static bool applyCartridge(void) {
	const char* value = option("armgba_link_cartridge");
	bool const out = !gameBoy && (separateWanted() || gamecubeWanted()) && value && !strcmp(value, "removed");
	if (out == cartridgeOut || !gba) {
		return false;
	}
	if (out) {
		gbaLoadRom(gba, "", 0);
		notify("No cartridge: waiting for a game over the cable", 240);
	} else {
		size_t size;
		void* data = secondRead(firstPath, &size);
		if (!data || !gbaLoadRom(gba, data, size)) {
			free(data);
			showMessage("armGBA can't read the game to put it back in");
			return false;
		}
		free(data);
		savedataForceType(gba, SAVEDATA_AUTODETECT);
	}
	cartridgeOut = out;
	return !out;
}

static void readCartridgeSave(void) {
	if (!cartridgeSaveUnread) {
		return;
	}
	cartridgeSaveUnread = false;
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
	retroArchFiles(file, rtcFile);
	size_t size;
	readFileInto(file, saveMemory(0, &size), size);
}

static bool loadAdvance(const struct retro_game_info* game, const struct retro_game_info* second) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no RGB565");
		return false;
	}
	offerSecondGames(game->path, false);
	recallSecondChoice();
	gbas[0] = gba = startAdvance(game);
	gbaTogether = false;
	playersHere = 1;
	if (!gba) {
		return false;
	}
	showButtons();
	threadedVideo = 0;
	readOptions();
	cartridgeOut = false;
	applyCartridge();
	cartridgeSaveUnread = cartridgeOut;
	gbaReset(gba, skipBios);
	readPlayerOneSave();
	linkSetGba(gba);
	attachPorts();
	if (second || togetherWanted() || pairFile) {
		startOthers(second, game->data, game->size);
	}
	offerLinkNetplay();
	showOptions();
	announceMemory();
	struct retro_rumble_interface rumble = { NULL };
	setRumble = gba->gpio.devices & HW_RUMBLE && environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) ? rumble.set_rumble_state : NULL;
	rumbling = false;
	return true;
}

static bool loadPairFile(const struct retro_game_info* file) {
	char games[MAX_PLAYERS][PATH_SIZE];
	pairCount = secondPairFile(file->path, file->data, file->size, games);
	if (!pairCount) {
		showMessage("armGBA: a pair file needs two to four lines, one game each");
		return false;
	}
	struct retro_game_info first = { games[0], NULL, 0, NULL };
	void* data = secondRead(games[0], &first.size);
	first.data = data;
	if (!data) {
		char message[300];
		snprintf(message, sizeof(message), "armGBA can't read %.250s", games[0]);
		showMessage(message);
		return false;
	}
	// The other players' games, empty where player 1's game is named again.
	for (int i = 1; i < MAX_PLAYERS; ++i) {
		snprintf(i == 1 ? secondPath : otherPaths[i], PATH_SIZE, "%s", i < pairCount && strcmp(games[i], games[0]) ? games[i] : "");
	}
	pairFile = true;
	snprintf(pairPath, sizeof(pairPath), "%s", file->path);
	bool loaded = false;
	if (isGameBoyRom(data, first.size)) {
		gameBoy = false;
		loaded = loadGameBoy(&first, NULL);
	} else {
		loaded = loadAdvance(&first, NULL);
	}
	if (loaded) {
		readSave(0, firstPath);
	} else {
		pairFile = false;
	}
	free(data);
	return loaded;
}

bool retro_load_game(const struct retro_game_info* game) {
	readSaveActions(false);
	if (!game || !game->data) {
		return false;
	}
	if (isPairFile(game->path)) {
		return loadPairFile(game);
	}
	gameBoy = false;
	if (hasGameBoyExtension(game->path)) {
		return loadGameBoy(game, NULL);
	}
	return loadAdvance(game, NULL);
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
	readSaveActions(false);
	if (type == SUBSYSTEM_ADVANCE_LINK && count == 2 && info[0].data && info[1].data) {
		gameBoy = false;
		return loadAdvance(&info[0], &info[1]);
	}
	if (type == SUBSYSTEM_LINK && count == 2 && info[0].data && info[1].data && hasGameBoyExtension(info[0].path) && hasGameBoyExtension(info[1].path)) {
		gameBoy = false;
		return loadGameBoy(&info[0], &info[1]);
	}
	return false;
}

void retro_unload_game(void) {
	if (!secondFromFrontend) {
		rememberSecondChoice();
	}
	writeSaves(true);
	forgetSaves();
	pairFile = false;
	secondFromFrontend = false;
	secondPath[0] = '\0';
	netplaySeen = false;
	netplayJoined = false;
	sessionTag = 0;
	updateRumble(false);
	cheatsClear(&cheats, NULL);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
	if (gameBoy) {
		for (int i = playersHere - 1; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbUnload();
		}
		gameBoy = false;
		gbTogether = false;
		gbAdapter = false;
		libretroGbSetPlayers(1, false);
	}
	playersHere = 1;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		otherPaths[i][0] = '\0';
	}
	linkSetGba(NULL);
	dolphinSetHost(NULL);
	gamecubeShown = false;
	workersStop();
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (gbas[i]) {
			gbaDestroy(gbas[i]);
		}
		gbas[i] = NULL;
	}
	gba = NULL;
	gbaTogether = false;
}

void retro_reset(void) {
	// Restart can come straight after an option change, before a frame has read it, so read the
	// options first.
	readOptions();
	applyLinkOptions();
	offerLinkNetplay();
	if (gameBoy) {
		for (int i = playersHere - 1; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbReset(gbModel);
		}
		libretroGbSetPlayers(playersHere, gbAdapter);
		checkGameBoyMemory();
		return;
	}
	attachPorts();
	bool const putBack = applyCartridge();
	for (int i = 0; i < playersHere; ++i) {
		loadBios(gbas[i]);
		gbaReset(gbas[i], skipBios);
	}
	if (putBack) {
		readCartridgeSave();
	}
	setAdvanceOffsets();
}

// Input.

// Reads the motion sensor control: a stick, or the D-pad pushed all the way, giving -0x7FFF to
// 0x7FFF on each axis. Also reports whether the stick is clicked, or L is held when using the
// D-pad.
static void readMotion(uint16_t keys, int* x, int* y, bool* click) {
	if (tiltControl == TILT_DPAD) {
		*x = keys & KEY_RIGHT ? 0x7FFF : keys & KEY_LEFT ? -0x7FFF : 0;
		*y = keys & KEY_DOWN ? 0x7FFF : keys & KEY_UP ? -0x7FFF : 0;
		*click = keys & KEY_L;
	} else {
		unsigned stick = tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		*x = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X);
		*y = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y);
		*click = inputState(0, RETRO_DEVICE_JOYPAD, 0, tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_ID_JOYPAD_R3 : RETRO_DEVICE_ID_JOYPAD_L3);
	}
}

// Feeds the tilt sensor of a Game Boy cartridge that has one (Kirby Tilt 'n' Tumble). Clicking the
// stick flicks the console upwards, which is how Kirby jumps.
static void readTilt(uint16_t keys) {
	static bool held;
	int x;
	int y;
	bool click;
	readMotion(keys, &x, &y, &click);
	if (click && !held) {
		libretroGbFlick();
	}
	held = click;
	libretroGbSetTilt(x, y);
}

// Feeds a Game Boy Advance cartridge's sensors before each frame. The tilt sensor and the gyro are
// scaled as mGBA's SDL frontend scales a stick; the gyro's turn comes from the control's left and
// right. The light sensor uses mGBA's levels, and pushing the right stick up or down changes the
// level by one step.
static void readSensors(uint16_t keys) {
	int devices = gba->gpio.devices;
	if (devices & (HW_TILT | HW_GYRO)) {
		int x;
		int y;
		bool click;
		readMotion(keys, &x, &y, &click);
		gba->gpio.tiltInputX = x * 0x3800;
		gba->gpio.tiltInputY = y * 0x3800;
		gba->gpio.gyroZ = x * 11000;
	}
	if (devices & HW_LIGHT_SENSOR) {
		static const int levels[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };
		static bool held;
		int const push = inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		bool brighter = push < -0x4000;
		bool darker = push > 0x4000;
		if ((brighter || darker) && !held) {
			solarLevel += brighter ? (solarLevel < 10) : -(solarLevel > 0);
			char message[32];
			snprintf(message, sizeof(message), "Sunlight %d of 10", solarLevel);
			notify(message, 120);
		}
		held = brighter || darker;
		gba->gpio.luminance = 0xFF - (0x16 + (solarLevel ? levels[solarLevel - 1] : 0));
	}
}

// Returns whether a held turbo button presses its button this frame: pressed for the first frames
// of every turbo period, as in gambatte's core.
static bool turbo(unsigned port, unsigned button, unsigned* counter) {
	if (!inputState(port, RETRO_DEVICE_JOYPAD, 0, button)) {
		*counter = 0;
		return false;
	}
	bool const down = *counter < turboPulse;
	*counter = *counter + 1 >= turboPeriod ? 0 : *counter + 1;
	return down;
}

// Turbo L and R: L3 and R3 held press a Game Boy Advance game's L and R.
static uint16_t readTurboShoulders(void) {
	static unsigned turboL, turboR;
	return (turbo(0, RETRO_DEVICE_ID_JOYPAD_L3, &turboL) ? KEY_L : 0) | (turbo(0, RETRO_DEVICE_ID_JOYPAD_R3, &turboR) ? KEY_R : 0);
}

static bool allHeld(uint16_t keys, uint16_t wanted) {
	return (keys & wanted) == wanted;
}

// Returns the buttons held on a controller, as KEY_ bits.
static uint16_t readKeys(unsigned port) {
	static const unsigned buttons[10] = {
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_SELECT,
		RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_RIGHT,
		RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_UP,
		RETRO_DEVICE_ID_JOYPAD_DOWN,
		RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_L,
	};
	uint16_t keys = 0;
	for (int i = 0; i < 10; ++i) {
		if (inputState(port, RETRO_DEVICE_JOYPAD, 0, buttons[i])) {
			keys |= 1 << i;
		}
	}
	// Turbo A and B: X and Y held press A and B.
	static unsigned turboA[MAX_PLAYERS], turboB[MAX_PLAYERS];
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_X, &turboA[port]) ? KEY_A : 0;
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_Y, &turboB[port]) ? KEY_B : 0;
	// As in mGBA's core by default, pressing opposite directions together presses neither.
	if (allHeld(keys, KEY_LEFT | KEY_RIGHT)) {
		keys &= ~(KEY_LEFT | KEY_RIGHT);
	}
	if (allHeld(keys, KEY_UP | KEY_DOWN)) {
		keys &= ~(KEY_UP | KEY_DOWN);
	}
	return keys;
}

// On the Game Boy Advance model, L or R switches between the game's own size and stretched across
// the screen, like that console's shoulder buttons. With the tilt sensor on the D-pad, only R does,
// since L is the flick.
static void readStretch(uint16_t keys) {
	static bool held;
	bool now = keys & (tiltControl == TILT_DPAD ? KEY_R : KEY_L | KEY_R);
	if (now && !held) {
		gbStretched = !gbStretched;
		if (gbCropped) {
			announceGeometry();
		}
	}
	held = now;
}

// Whether RetroArch is running this frame again, for a netplay rollback or runahead, rather than
// for the first time.
static bool rerunning(void) {
	int enabled = RETRO_AV_ENABLE_VIDEO;
	return environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &enabled) && !(enabled & RETRO_AV_ENABLE_VIDEO);
}

// Whether controller 1 holds the Switch Controller buttons.
static bool switchHeld(int port) {
	static unsigned const lrab[] = { RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B };
	static unsigned const startL2R2[] = { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2 };
	if (switchController == SWITCH_OFF || port != 0 || !togetherRunning() || netplaySeen || playersHere < 2) {
		return false;
	}
	bool const isLrab = switchController == SWITCH_L_R_A_B;
	for (size_t k = 0; k < (isLrab ? COUNT(lrab) : COUNT(startL2R2)); ++k) {
		if (!inputState(0, RETRO_DEVICE_JOYPAD, 0, isLrab ? lrab[k] : startL2R2[k])) {
			return false;
		}
	}
	return true;
}

// The controller that plays console i: controller 1 plays controllerFocus's.
static int portFor(int i) {
	if (netplaySeen || controllerFocus >= playersHere) {
		controllerFocus = 0;
	}
	return (i - controllerFocus + playersHere) % playersHere;
}

// Whether controller 1 holds Start with Switch Controller on Start + L2 + R2, so its L2 and R2 only switch.
static bool switchStart(int port) {
	return switchController == SWITCH_START_L2_R2 && port == 0 && togetherRunning() && !netplaySeen && playersHere > 1 &&
	       inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
}

// The Switch Controller buttons on controller 1 move it to the next console.
static void readControllerSwitch(void) {
	static bool held;
	if (rerunning()) {
		return;
	}
	bool const now = switchHeld(0);
	bool const pressed = now && !held;
	held = now;
	if (!pressed) {
		return;
	}
	controllerFocus = (controllerFocus + 1) % playersHere;
	char name[48];
	snprintf(name, sizeof(name), "Controller 1: Player %d's Console", controllerFocus + 1);
	notify(name, 120);
}

// L2 or R2 held fast-forwards at RetroArch's maximum run speed; with the consoles here only L2, as
// the L2 Button option says. Not in netplay, where the other consoles would wait.
static void readFastForward(void) {
	static bool held;
	bool now = false;
	for (int port = 0; port < (togetherRunning() ? playersHere : 1); ++port) {
		now |= (!togetherRunning() || l2FastForward) && !switchStart(port) && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
		now |= !togetherRunning() && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
	}
	now &= !linkSession() && !netplaySeen;
	if (now == held) {
		return;
	}
	held = now;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = now, .notification = true, .inhibit_toggle = now };
	environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

// Whether a player holds the Restart Buttons.
static bool restartHeldDown(int port, uint16_t keys) {
	switch (restartButtons) {
	case RESTART_L_R_START:
		return allHeld(keys, KEY_L | KEY_R | KEY_START);
	case RESTART_L_R_A_B:
		return allHeld(keys, KEY_L | KEY_R | KEY_A | KEY_B);
	case RESTART_STICKS:
		return inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
	case RESTART_SELECT_START_A_B:
		return allHeld(keys, KEY_SELECT | KEY_START | KEY_A | KEY_B);
	default:
		return false;
	}
}

static void readRestarts(const uint16_t keys[]) {
	for (int i = 0; i < playersHere; ++i) {
		if (!restartHeldDown(portFor(i), keys[i]) || switchHeld(portFor(i))) {
			restartHeld[i] = 0;
		} else if (restartHeld[i] < RESTART_FRAMES && ++restartHeld[i] == RESTART_FRAMES) {
			restartConsole(i);
		}
	}
}

// R2 moves to the next way of showing the screens, and L2 back unless it fast-forwards. Each device
// shows the screens its own way: in netplay only this device's player changes them, the host's
// player 1 or a joined device's player 2, as RetroArch doesn't say which player is here.
static void readScreensKeys(void) {
	static bool held[MAX_PLAYERS][2];
	if (rerunning()) {
		return;
	}
	int ways[SCREENS_COUNT];
	int count = 0;
	ways[count++] = SCREENS_SIDE;
	if (playersHere == 2) {
		ways[count++] = SCREENS_STACKED;
	}
	int at = 0;
	for (int i = 0; i < playersHere; ++i) {
		ways[count++] = SCREENS_ONE + i;
	}
	for (int i = 0; i < count; ++i) {
		at = ways[i] == screens ? i : at;
	}
	int const here = netplayJoined ? 1 : 0;
	int step = 0;
	for (int port = 0; port < playersHere; ++port) {
		if (netplaySeen && port != here) {
			continue;
		}
		for (int k = netplaySeen || !l2FastForward ? 0 : 1; k < 2; ++k) {
			bool const now = !switchStart(port) && inputState(port, RETRO_DEVICE_JOYPAD, 0, k ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_L2);
			if (now && !held[port][k]) {
				step += k ? 1 : -1;
			}
			held[port][k] = now;
		}
	}
	if (!step) {
		return;
	}
	screens = ways[((at + step) % count + count) % count];
	announceGeometry();
	char name[32];
	if (screens >= SCREENS_ONE) {
		snprintf(name, sizeof(name), "Player %d Only", screens - SCREENS_ONE + 1);
	} else {
		snprintf(name, sizeof(name), "%s", playersHere > 2 ? "Grid" : screens == SCREENS_SIDE ? "Side by Side" : "Top and Bottom");
	}
	notify(name, 120);
}

// Running a frame.

// LCD Ghosting: the screens' liquid crystal takes time to change, darkening, as it is driven,
// faster than it lightens, as it relaxes. Each subpixel moves in linear light from where it was
// towards the new frame by a share each frame. The time constants, in milliseconds to darken and to
// lighten, are estimates for each kind of panel: the first Game Boy's STN, the Pocket's and Light's
// FSTN, and the Color's and Advance's reflective TFTs; Half halves them. A frame RetroArch runs again, and doesn't
// show, leaves the screen as it is.
enum { SCREEN_DMG, SCREEN_POCKET, SCREEN_TFT, SCREEN_KINDS };
static const float screenTimes[SCREEN_KINDS][2] = { { 50, 110 }, { 25, 50 }, { 15, 30 } };

struct Cell {
	uint32_t input;
	uint16_t level[3];
	bool settled;
};

static int screenKind(void) {
	if (!gameBoy || libretroGbOnAdvance() || libretroGbIsColor()) {
		return SCREEN_TFT;
	}
	return gbModel == LIBRETRO_GB_MGB || gbShades == 1 || gbShades == 3 ? SCREEN_POCKET : SCREEN_DMG;
}

static void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch) {
	static uint16_t toLinear[256];
	static uint8_t toEncoded[4096];
	static uint16_t shares[2][SCREEN_KINDS][2];
	static struct Cell* cells;
	static void* shown;
	static unsigned lastWidth, lastHeight;
	static size_t lastPitch;
	if (!ghosting || !picture || rerunning()) {
		videoRefresh(picture, width, height, pitch);
		return;
	}
	if (!toLinear[255]) {
		for (int v = 0; v < 256; ++v) {
			toLinear[v] = (uint16_t) lround(pow(v / 255.0, 2.2) * 65535);
		}
		for (int i = 0; i < 4096; ++i) {
			toEncoded[i] = (uint8_t) lround(pow((i * 16 + 8) / 65535.0, 1 / 2.2) * 255);
		}
		// A frame is 280896 cycles of 16.78 MHz on either console.
		for (int l = 0; l < 2; ++l) {
			for (int k = 0; k < SCREEN_KINDS; ++k) {
				for (int d = 0; d < 2; ++d) {
					shares[l][k][d] = (uint16_t) lround(4096 * (1 - exp(-16.743 * 2 / (l + 1) / screenTimes[k][d])));
				}
			}
		}
	}
	size_t const bytes = pitch / width;
	size_t const pixels = pitch / bytes * height;
	bool const fresh = !cells || width != lastWidth || height != lastHeight || pitch != lastPitch;
	if (fresh) {
		struct Cell* const newCells = realloc(cells, pixels * sizeof(*cells));
		void* const newShown = realloc(shown, pitch * height);
		if (!newCells || !newShown) {
			cells = newCells ? newCells : cells;
			shown = newShown ? newShown : shown;
			lastWidth = 0;
			videoRefresh(picture, width, height, pitch);
			return;
		}
		cells = newCells;
		shown = newShown;
		lastWidth = width;
		lastHeight = height;
		lastPitch = pitch;
	}
	const uint16_t* const share = shares[ghosting - 1][screenKind()];
	// Each channel's place and width: RGB565 or XRGB8888.
	static const int shifts[2][3] = { { 11, 5, 0 }, { 16, 8, 0 } };
	static const int widths[2][3] = { { 5, 6, 5 }, { 8, 8, 8 } };
	int const wide = bytes == sizeof(uint32_t);
	for (size_t i = 0; i < pixels; ++i) {
		uint32_t const input = wide ? ((const uint32_t*) picture)[i] : ((const uint16_t*) picture)[i];
		struct Cell* const cell = &cells[i];
		uint32_t output = input;
		if (fresh || !cell->settled || input != cell->input) {
			cell->input = input;
			cell->settled = true;
			output = wide ? input & 0xFF000000 : 0;
			for (int c = 0; c < 3; ++c) {
				int const bits = widths[wide][c];
				uint32_t const value = input >> shifts[wide][c] & ((1u << bits) - 1);
				uint32_t const full = value << (8 - bits) | value >> (2 * bits - 8);
				int32_t const target = toLinear[full];
				// A fresh cell's level isn't set yet: it takes the target.
				int32_t const diff = fresh ? 0 : target - cell->level[c];
				uint32_t out = value;
				if (fresh) {
					cell->level[c] = (uint16_t) target;
				} else if (diff) {
					int32_t step = diff < 0 ? -((-diff * share[0]) >> 12) : (diff * share[1]) >> 12;
					step = step ? step : diff < 0 ? -1 : 1;
					cell->level[c] = (uint16_t) (cell->level[c] + step);
					if (cell->level[c] != target) {
						cell->settled = false;
						out = toEncoded[cell->level[c] >> 4] >> (8 - bits);
					}
				}
				output |= out << shifts[wide][c];
			}
		}
		if (wide) {
			((uint32_t*) shown)[i] = output;
		} else {
			((uint16_t*) shown)[i] = (uint16_t) output;
		}
	}
	videoRefresh(shown, width, height, pitch);
}

// The low-pass filter: each sample out, per side, is the last one out weighted by lowPass, out of
// 0x10000, and the new one by the rest.
static void sendSound(const int16_t* samples, size_t frames) {
	static int16_t filtered[2 * 4096];
	static int32_t last[2];
	if (!lowPass || rerunning()) {
		audioBatch(samples, frames);
		return;
	}
	while (frames) {
		size_t const n = frames < COUNT(filtered) / 2 ? frames : COUNT(filtered) / 2;
		for (size_t k = 0; k < n * 2; ++k) {
			last[k & 1] = (last[k & 1] * lowPass + samples[k] * (0x10000 - lowPass)) >> 16;
			filtered[k] = (int16_t) last[k & 1];
		}
		audioBatch(filtered, n);
		samples += n * 2;
		frames -= n;
	}
}

// Returns the picture as a Game Boy Advance shows it, on black: centered on its 240x160 screen, or
// stretched to 240x144 by doubling every other column.
static const uint32_t* advanceFrame(const uint32_t* picture) {
	uint32_t* row = gbAdvanceFrame + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH;
	int const border = (SCREEN_WIDTH - LIBRETRO_GB_WIDTH) / 2;
	for (int y = 0; y < LIBRETRO_GB_HEIGHT; ++y, row += SCREEN_WIDTH, picture += LIBRETRO_GB_WIDTH) {
		if (gbStretched) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				row[x] = picture[x * 2 / 3];
			}
		} else {
			memset(row, 0, border * sizeof(*row));
			memcpy(row + border, picture, LIBRETRO_GB_WIDTH * sizeof(*row));
			memset(row + border + LIBRETRO_GB_WIDTH, 0, border * sizeof(*row));
		}
	}
	return gbAdvanceFrame;
}

// Puts the consoles' pictures side by side, one above the other, in a grid, or one alone, as chosen,
// and hands over their sound mixed, or the shown one's.
static void showConsoles(const void* const pictures[], size_t pixelBytes, int width, int height, void* frame, const int16_t* const samples[],
                         const size_t frames[]) {
	if (screens >= SCREENS_ONE) {
		int const shown = screens - SCREENS_ONE < playersHere ? screens - SCREENS_ONE : 0;
		showPicture(pictures[shown], width, height, width * pixelBytes);
		sendSound(samples[shown], frames[shown]);
		return;
	}
	int const columns = playersHere == 2 && screens == SCREENS_STACKED ? 1 : 2;
	int const rows = (playersHere + columns - 1) / columns;
	size_t const row = width * pixelBytes;
	uint8_t* const out = frame;
	memset(out, 0, row * columns * height * rows);
	for (int i = 0; i < playersHere; ++i) {
		uint8_t* const corner = out + (i / columns) * height * row * columns + (i % columns) * row;
		for (int y = 0; y < height; ++y) {
			memcpy(corner + y * row * columns, (const uint8_t*) pictures[i] + y * row, row);
		}
	}
	showPicture(frame, width * columns, height * rows, row * columns);
	size_t count = COUNT(mixedSamples) / 2;
	for (int i = 0; i < playersHere; ++i) {
		count = frames[i] < count ? frames[i] : count;
	}
	for (size_t k = 0; k < count * 2; ++k) {
		int sum = 0;
		for (int i = 0; i < playersHere; ++i) {
			sum += samples[i][k];
		}
		mixedSamples[k] = sum / playersHere;
	}
	sendSound(mixedSamples, count);
}

static void runGameBoys(void) {
	uint16_t keys[MAX_PLAYERS];
	readControllerSwitch();
	for (int i = 0; i < playersHere; ++i) {
		keys[i] = readKeys(portFor(i));
	}
	readTilt(keys[0]);
	readScreensKeys();
	readRestarts(keys);
	if (copySaves) {
		copyFirstSave();
	}
	const uint32_t* pictures[MAX_PLAYERS];
	const int16_t* samples[MAX_PLAYERS];
	size_t frames[MAX_PLAYERS];
	libretroGbRunPlayers(keys, pictures, samples, frames);
	const void* shown[MAX_PLAYERS];
	for (int i = 0; i < playersHere; ++i) {
		shown[i] = pictures[i];
	}
	showConsoles(shown, sizeof(uint32_t), LIBRETRO_GB_WIDTH, LIBRETRO_GB_HEIGHT, gbTogetherFrame, samples, frames);
}

static void airSend(const struct RfuAir* air, int to, const uint8_t* message, size_t size) {
	int const from = air->self;
	if (airQueued[from] == AIR_QUEUE || size > AIR_BYTES) {
		return;
	}
	airQueue[from][airQueued[from]].to = to;
	airQueue[from][airQueued[from]].size = size;
	memcpy(airQueue[from][airQueued[from]].data, message, size);
	++airQueued[from];
}

// Between rounds: each console's messages reach the others, in the order the consoles come, and the
// answers they bring go out the same way.
static void carryAir(void) {
	for (bool any = true; any;) {
		any = false;
		for (int from = 0; from < MAX_PLAYERS; ++from) {
			int const count = airQueued[from];
			airQueued[from] = 0;
			for (int k = 0; k < count; ++k) {
				any = true;
				for (int i = 0; i < MAX_PLAYERS; ++i) {
					if (i != from && gbas[i] && (airQueue[from][k].to < 0 || airQueue[from][k].to == i)) {
						rfuReceive(gbas[i], from, airQueue[from][k].data, airQueue[from][k].size);
					}
				}
			}
		}
	}
}

// The cable between the Game Boy Advances here. When a transfer ends:
// - in normal mode, players 1 and 2 swap the data each had as it started; the console that didn't
//   start it takes the data only if it is waiting for a transfer. With three or four, the parent's
//   data shifts along the cable instead (see exchange);
// - in multiplayer mode, the parent's word and each child's go to every child.

// Where a console is in the frame while it runs in a round.
static int32_t roundNow(int i) {
	return roundDone[i] + (int32_t) ((uint32_t) timingCurrentTime(gbas[i]) - roundStartTime[i]);
}

static uint32_t dataOf(int i) {
	int mode;
	bool ready;
	return sioOutgoing(gbas[i], &mode, &ready);
}

static void cableBegin(void* context, int mode, uint32_t out, int32_t cycles) {
	int const i = (int) (intptr_t) context;
	struct Transfer* const t = &transfers[i];
	memset(t, 0, sizeof(*t));
	t->mode = mode;
	t->out = out;
	int32_t const start = roundNow(i);
	for (int k = 0; k < playersHere; ++k) {
		if (k != i) {
			t->takeAt[k] = start + (mode == MULTIPLAYER ? cycles : 0);
			t->pending |= 1 << k;
		}
	}
}

static void cableWrote(void* context) {
	int const i = (int) (intptr_t) context;
	int const n = dataWrites[i].count;
	if (n < DATA_WRITES) {
		dataWrites[i].at[n] = roundNow(i);
		dataWrites[i].value[n] = dataOf(i);
		dataWrites[i].count = n + 1;
	}
}

// A console's data at a moment of the round just run.
static uint32_t dataAt(int i, int32_t when) {
	uint32_t value = dataWrites[i].before;
	for (int k = 0; k < dataWrites[i].count && dataWrites[i].at[k] <= when; ++k) {
		value = dataWrites[i].value[k];
	}
	return value;
}

// Between rounds: the data due in the round, for each transfer under way, or a waiting console's,
// which can't change; then the writes are forgotten, and each console's data is noted as the next
// round begins.
static void takeData(void) {
	for (int i = 0; i < playersHere; ++i) {
		struct Transfer* const t = &transfers[i];
		for (int k = 0; t->pending && k < playersHere; ++k) {
			if ((t->pending & 1 << k) && (t->takeAt[k] <= roundDone[k] || transfers[k].waiting)) {
				t->data[k] = dataAt(k, t->takeAt[k]);
				t->pending &= ~(1 << k);
			}
		}
	}
	for (int i = 0; i < playersHere; ++i) {
		dataWrites[i].count = 0;
		dataWrites[i].before = dataOf(i);
	}
}

static bool cableFinish(void* context, uint32_t in[3]) {
	int const i = (int) (intptr_t) context;
	struct Transfer* const t = &transfers[i];
	if (!t->answered) {
		t->waiting = true;
		gbas[i]->returnRequested = true;
		timingInterrupt(gbas[i]);
		return false;
	}
	t->answered = false;
	memcpy(in, t->in, sizeof(t->in));
	return true;
}

// Between rounds: the children see a multiplayer transfer the parent has under way.
static void carryBusy(void) {
	for (int i = 0; i < playersHere; ++i) {
		if (transfers[i].mode != MULTIPLAYER || sioTransferLeft(gbas[i]) <= 0) {
			continue;
		}
		for (int k = 0; k < playersHere; ++k) {
			if (k != i) {
				sioOthersTransfer(gbas[k]);
			}
		}
	}
}

// Between rounds: each console's SD line is high while every other console leaves it high.
static void carryReady(void) {
	for (int i = 0; i < playersHere; ++i) {
		bool ready = true;
		for (int k = 0; k < playersHere; ++k) {
			ready &= k == i || sioSdHigh(gbas[k]);
		}
		if (ready != cables[i].ready) {
			cables[i].ready = ready;
			sioLinkChanged(gbas[i]);
		}
	}
}

// A console's transfer ending: it takes the others' data, and they take its.
static void exchange(int i) {
	struct Transfer* const t = &transfers[i];
	t->answered = true;
	t->in[0] = 0xFFFFFFFF;
	t->in[1] = t->in[2] = 0xFFFF;
	int mode;
	bool ready;
	if (t->mode == MULTIPLAYER) {
		uint32_t values[4] = { t->out & 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
		for (int k = 1; k < playersHere; ++k) {
			sioOutgoing(gbas[k], &mode, &ready);
			values[k] = t->in[k - 1] = mode == MULTIPLAYER ? t->data[k] & 0xFFFF : 0xFFFF;
		}
		for (int k = 1; k < playersHere; ++k) {
			sioReceive(gbas[k], t->mode, values);
		}
	} else if (i == 0 && playersHere > 2) {
		// Normal mode with three or four on the cable: the data shifts along from the parent, each
		// console taking the one before's as the transfer started, and the parent takes zero.
		t->in[0] = 0;
		for (int k = 1; k < playersHere; ++k) {
			uint32_t const values[4] = { k == 1 ? t->out : t->data[k - 1], 0xFFFF, 0xFFFF, 0xFFFF };
			sioReceive(gbas[k], t->mode, values);
		}
	} else if (i < 2) {
		struct GBA* const other = gbas[1 - i];
		sioOutgoing(other, &mode, &ready);
		t->in[0] = ready && mode != MULTIPLAYER ? t->data[1 - i] : 0xFFFFFFFF;
		uint32_t const values[4] = { t->out, 0xFFFF, 0xFFFF, 0xFFFF };
		sioReceive(other, t->mode, values);
	}
}

// Between rounds: the transfers ending on this cycle, answered before they end; and any that ended
// inside a round, which waited for it to end. Consoles stop a few cycles past a round's end, so a
// transfer can end before another console has got to where its data is taken: it waits for it.
static void carryCable(void) {
	for (int i = 0; i < playersHere; ++i) {
		if (transfers[i].pending) {
			continue;
		}
		if (transfers[i].waiting || (!transfers[i].answered && sioTransferLeft(gbas[i]) == 0)) {
			transfers[i].waiting = false;
			exchange(i);
		}
	}
}

// The end of a console's part of a round, before anything else due on that cycle, such as a
// transfer ending.
static void sliceEnd(struct GBA* machine, void* context, uint32_t late) {
	(void) late;
	sliceOver[(intptr_t) context] = true;
	machine->returnRequested = true;
	timingInterrupt(machine);
}

// One console's part of a round: it runs to the round's end, or until a transfer it clocked waits for
// the others' data. A frame it finishes on the way is kept as its picture.
static int32_t roundEnd;

static void runRound(int i) {
	struct GBA* const machine = gbas[i];
	int32_t const left = roundEnd - roundDone[i];
	if (left <= 0 || transfers[i].waiting) {
		return;
	}
	uint32_t const start = timingCurrentTime(machine);
	roundStartTime[i] = start;
	sliceOver[i] = false;
	timingSchedule(machine, &sliceEvents[i], left);
	while (!sliceOver[i] && !transfers[i].waiting) {
		uint32_t const frame = machine->video.frameCounter;
		gbaRunPart(machine);
		if (machine->video.frameCounter != frame) {
			memcpy(gbaShown[i], machine->renderer.frame, sizeof(gbaShown[i]));
		}
	}
	timingDeschedule(machine, &sliceEvents[i]);
	roundDone[i] += (int32_t) ((uint32_t) timingCurrentTime(machine) - start);
}

// Where the next round ends: at the frame's end, at the next transfer's end, and no later than the
// shortest transfer any console could start from now.
static int32_t nextRoundEnd(void) {
	int32_t now = ROUND_LONGEST;
	for (int i = 0; i < playersHere; ++i) {
		now = roundDone[i] < now ? roundDone[i] : now;
	}
	int64_t end = ROUND_LONGEST;
	for (int i = 0; i < playersHere; ++i) {
		int64_t const shortest = (int64_t) now + sioShortestTransfer(gbas[i]);
		int32_t const left = sioTransferLeft(gbas[i]);
		end = shortest < end ? shortest : end;
		if (left > 0 && roundDone[i] + left < end) {
			end = roundDone[i] + left;
		}
	}
	return (int32_t) end;
}

static void runAdvances(void) {
	for (int i = 0; i < playersHere; ++i) {
		roundDone[i] = gbaAhead[i];
		sliceEvents[i] = (struct Event) { .callback = sliceEnd, .context = (void*) (intptr_t) i, .priority = 0x7F, .endsRun = true };
		dataWrites[i].count = 0;
		dataWrites[i].before = dataOf(i);
	}
	for (;;) {
		bool unfinished = false;
		for (int i = 0; i < playersHere; ++i) {
			unfinished |= roundDone[i] < GBA_FRAME_CYCLES;
		}
		if (!unfinished) {
			break;
		}
		roundEnd = nextRoundEnd();
		workersRun(runRound, playersHere);
		takeData();
		carryCable();
		carryBusy();
		carryAir();
		carryReady();
	}
	for (int i = 0; i < playersHere; ++i) {
		gbaAhead[i] = roundDone[i] - GBA_FRAME_CYCLES;
		for (int k = 0; k < playersHere; ++k) {
			transfers[i].takeAt[k] -= GBA_FRAME_CYCLES;
		}
	}
}

static void runAdvancesFrame(void) {
	uint16_t keys[MAX_PLAYERS];
	readControllerSwitch();
	for (int i = 0; i < playersHere; ++i) {
		int const port = portFor(i);
		keys[i] = readKeys(port) | (port ? 0 : readTurboShoulders());
		gbaSetKeys(gbas[i], keys[i]);
	}
	readScreensKeys();
	readRestarts(keys);
	if (copySaves) {
		copyFirstSave();
	}
	readSensors(keys[0]);
	runAdvances();
	updateRumble(gba->gpio.rumble);
	if (cheats.count || cheats.patchCount) {
		cheatsApply(&cheats, gba);
	}
	const void* pictures[MAX_PLAYERS];
	const int16_t* samples[MAX_PLAYERS];
	size_t frames[MAX_PLAYERS];
	for (int i = 0; i < playersHere; ++i) {
		pictures[i] = gbaShown[i];
		if (colorCorrection) {
			for (int k = 0; k < SCREEN_WIDTH * SCREEN_HEIGHT; ++k) {
				gbaCorrected[i][k] = corrected(gbaShown[i][k]);
			}
			pictures[i] = gbaCorrected[i];
		}
		samples[i] = (const int16_t*) gbas[i]->audio.out;
		frames[i] = gbas[i]->audio.outCount;
	}
	showConsoles(pictures, sizeof(uint16_t), SCREEN_WIDTH, SCREEN_HEIGHT, gbaTogetherFrame, samples, frames);
	for (int i = 0; i < playersHere; ++i) {
		gbas[i]->audio.outCount = 0;
	}
}

static void runOneGameBoy(void) {
	const int16_t* samples;
	size_t frames;
	uint16_t keys = readKeys(0);
	readTilt(keys);
	readRestarts(&keys);
	const uint32_t* picture = libretroGbRunFrame(keys, &samples, &frames);
	// A restart or a loaded state can change the model.
	if (gbOnAdvance != libretroGbOnAdvance()) {
		gbOnAdvance = !gbOnAdvance;
		announceGeometry();
	}
	if (gbOnAdvance) {
		readStretch(keys);
	}
	if (gbOnAdvance && !gbCropped) {
		showPicture(advanceFrame(picture), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));
	} else if (gbOnAdvance && gbStretched) {
		// The stretched picture, without the black above and below it.
		showPicture(advanceFrame(picture) + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH, SCREEN_WIDTH, LIBRETRO_GB_HEIGHT,
		             SCREEN_WIDTH * sizeof(uint32_t));
	} else {
		showPicture(picture, LIBRETRO_GB_WIDTH, LIBRETRO_GB_HEIGHT, LIBRETRO_GB_WIDTH * sizeof(uint32_t));
	}
	sendSound(samples, frames);
}

static void sendOneAdvanceSound(void) {
	sendSound((const int16_t*) gba->audio.out, gba->audio.outCount);
	gba->audio.outCount = 0;
}

static void runOneAdvance(void) {
	uint16_t keys = readKeys(0) | readTurboShoulders();
	readRestarts(&keys);
	gbaSetKeys(gba, keys);
	readSensors(keys);
	linkGbaFrame(gba);
	dolphinGbaFrame(gba);
	if (dolphinConnected() != gamecubeShown) {
		gamecubeShown = dolphinConnected();
		char message[64] = "GameCube: Dolphin has gone";
		if (gamecubeShown) {
			snprintf(message, sizeof(message), "GameCube: Dolphin at %s", dolphinAddress());
		}
		notify(message, 180);
	}
	uint32_t const frame = gba->video.frameCounter;
	do {
		gbaRunPart(gba);
	} while (gba->video.frameCounter == frame && !linkGbaHeld());
	if (gba->video.frameCounter == frame) {
		// Held for the link: the picture is half drawn, so the last one shows again.
		videoRefresh(NULL, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
		sendOneAdvanceSound();
		return;
	}
	updateRumble(gba->gpio.rumble);
	if (cheats.count || cheats.patchCount) {
		cheatsApply(&cheats, gba);
	}
	if (colorCorrection) {
		const uint16_t* frame = (const uint16_t*) gba->renderer.frame;
		for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
			correctedFrame[i] = corrected(frame[i]);
		}
		showPicture(correctedFrame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	} else {
		showPicture(gba->renderer.frame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	}
	sendOneAdvanceSound();
}

void retro_run(void) {
	readNewerPlayerOne();
	bool updated = false;
	if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readOptions();
		readSaveActions(true);
		const char* listed = option("armgba_list_saves");
		if ((listed && atoi(listed) != backupsListed) || loadIntoPlayer() != listedInto) {
			offerSaveChoices();
		}
	}
	inputPoll();
	readFastForward();
	// The kept saves are written when they change, checked every few seconds.
	if (savesKept() && ++keptCheck >= 300) {
		keptCheck = 0;
		writeSaves(false);
	}
	if (gbTogether) {
		runGameBoys();
	} else if (gameBoy) {
		runOneGameBoy();
	} else if (gbaTogether) {
		runAdvancesFrame();
	} else {
		runOneAdvance();
	}
}

// States.

// Save states: the size, then saving and loading. A Game Boy Advance state is the machine's whole
// state; a Game Boy state is Gambatte's.
static size_t consolesStateSize(void) {
	if (gameBoy) {
		return libretroGbStateSize();
	}
	if (gbaTogether) {
		return playersHere * (sizeof(gbaAhead[0]) + gbaStateSize() + sizeof(transfers[0]) + sizeof(cables[0].ready) + sizeof(gbaShown[0]));
	}
	return gbaStateSize();
}

static void checkNetplay(void) {
	int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	netplaySeen |= environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
	// A tag of this device's own: the time, the processor time used and where this core sits in
	// memory, which differ from one device to the next.
	while (netplaySeen && !sessionTag) {
		sessionTag = (uint32_t) time(NULL) * 2654435761u ^ (uint32_t) clock() ^ (uint32_t) (uintptr_t) &sessionTag;
	}
}

// A state's session tag: one from another device's session, in netplay, means this device joined it.
static void loadedTag(uint32_t tag) {
	if (netplaySeen && tag != sessionTag && !netplayJoined) {
		notify("Netplay: this device's saves stay as they were; the session's are kept as backups when it ends", 240);
	}
	netplayJoined |= netplaySeen && tag != sessionTag;
	sessionTag = tag;
}

static bool saveConsoles(void* data, size_t size) {
	if (gameBoy) {
		return libretroGbSaveState(data, size);
	}
	if (!gba || size < consolesStateSize()) {
		return false;
	}
	if (!gbaTogether) {
		gbaSaveState(gba, data);
		return true;
	}
	// The consoles here: how far each ran past the last frame, their states, the cable between them,
	// then their last pictures.
	uint8_t* at = data;
	memcpy(at, gbaAhead, playersHere * sizeof(gbaAhead[0]));
	at += playersHere * sizeof(gbaAhead[0]);
	for (int i = 0; i < playersHere; ++i) {
		gbaSaveState(gbas[i], at);
		at += gbaStateSize();
	}
	memcpy(at, transfers, playersHere * sizeof(transfers[0]));
	at += playersHere * sizeof(transfers[0]);
	for (int i = 0; i < playersHere; ++i) {
		*at++ = cables[i].ready;
	}
	memcpy(at, gbaShown, playersHere * sizeof(gbaShown[0]));
	return true;
}

static bool loadConsoles(const void* data, size_t size) {
	if (gameBoy) {
		bool loaded = libretroGbLoadState(data, size);
		checkGameBoyMemory();
		return loaded;
	}
	if (!gbaTogether) {
		return gba && gbaLoadState(gba, data, size);
	}
	if (size < consolesStateSize()) {
		return false;
	}
	const uint8_t* at = data;
	int32_t ahead[MAX_PLAYERS];
	memcpy(ahead, at, playersHere * sizeof(ahead[0]));
	at += playersHere * sizeof(ahead[0]);
	for (int i = 0; i < playersHere; ++i) {
		if (!gbaLoadState(gbas[i], at, gbaStateSize())) {
			return false;
		}
		at += gbaStateSize();
	}
	memcpy(gbaAhead, ahead, playersHere * sizeof(gbaAhead[0]));
	memcpy(transfers, at, playersHere * sizeof(transfers[0]));
	at += playersHere * sizeof(transfers[0]);
	for (int i = 0; i < playersHere; ++i) {
		cables[i].ready = *at++;
	}
	memcpy(gbaShown, at, playersHere * sizeof(gbaShown[0]));
	return true;
}

// A state holds, in order:
// - the consoles' states;
// - with the consoles of a link here, how long each player has held the restart buttons, so a
//   rollback restarts a console on the same frame;
// - the session's tag, which tells a device in netplay that it joined another's session.
// A single-console state from before the tag still loads.
static size_t extraStateSize(void) {
	return togetherRunning() ? sizeof(restartHeld) : 0;
}

size_t retro_serialize_size(void) {
	return consolesStateSize() + extraStateSize() + sizeof(sessionTag);
}

bool retro_serialize(void* data, size_t size) {
	checkNetplay();
	if (size < retro_serialize_size() || !saveConsoles(data, consolesStateSize())) {
		return false;
	}
	uint8_t* const at = (uint8_t*) data + consolesStateSize();
	memcpy(at, restartHeld, extraStateSize());
	memcpy(at + extraStateSize(), &sessionTag, sizeof(sessionTag));
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	checkNetplay();
	bool const tagged = size >= retro_serialize_size();
	if (!(tagged || (size == consolesStateSize() && !togetherRunning())) || !loadConsoles(data, consolesStateSize())) {
		return false;
	}
	// A loaded state has every console's save as it was; player 1's isn't copied over them after.
	copySaves = 0;
	if (tagged) {
		const uint8_t* const at = (const uint8_t*) data + consolesStateSize();
		memcpy(restartHeld, at, extraStateSize());
		uint32_t tag;
		memcpy(&tag, at + extraStateSize(), sizeof(tag));
		loadedTag(tag);
	}
	return true;
}

// Cheats, and the memory the frontend sees.

static void applyCheatCodes(void) {
	if (gameBoy) {
		if (!libretroGbSetCheats((const char* const*) cheatCodes, cheatCodeCount)) {
			logMessage(RETRO_LOG_WARN, "armGBA: a cheat code is neither Game Genie nor GameShark");
		}
		return;
	}
	cheatsClear(&cheats, gba);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		if (cheatCodes[i] && !cheatsAdd(&cheats, cheatCodes[i])) {
			char message[160];
			snprintf(message, sizeof(message), "armGBA: cheat code %.100s isn't one armGBA reads", cheatCodes[i]);
			logMessage(RETRO_LOG_WARN, message);
		}
	}
}

// Cheat codes: GameShark, Action Replay and CodeBreaker for the Game Boy Advance; Game Genie and
// GameShark for the Game Boy.
void retro_cheat_reset(void) {
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
	applyCheatCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	if (index >= cheatCodeCount) {
		char** grown = realloc(cheatCodes, (index + 1) * sizeof(*cheatCodes));
		if (!grown) {
			return;
		}
		memset(&grown[cheatCodeCount], 0, (index + 1 - cheatCodeCount) * sizeof(*grown));
		cheatCodes = grown;
		cheatCodeCount = index + 1;
	}
	free(cheatCodes[index]);
	cheatCodes[index] = enabled && code ? strdup(code) : NULL;
	applyCheatCodes();
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

// Memory the frontend may read or keep, and its size:
// - the cartridge's save memory, loaded and saved as the save file;
// - a Game Boy cartridge's clock, as the .rtc file;
// - work RAM, for achievements and cheats.
// RetroArch gets no save memory when the core keeps player 1's save itself.
static void* memory(unsigned id, size_t* size) {
	*size = 0;
	if (pairFile && id != RETRO_MEMORY_SYSTEM_RAM) {
		return NULL;
	}
	if (!retroArchSaves() && (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC)) {
		return NULL;
	}
	if (gbTogether && (id == MEMORY_SECOND_SAVE || id == MEMORY_SECOND_RTC)) {
		libretroGbSelect(1);
		void* const data = id == MEMORY_SECOND_SAVE ? libretroGbSaveRam(size) : libretroGbRtc(size);
		libretroGbSelect(0);
		return data;
	}
	if (gameBoy) {
		return id == RETRO_MEMORY_SAVE_RAM ? libretroGbSaveRam(size) : id == RETRO_MEMORY_SYSTEM_RAM ? libretroGbSystemRam(size)
		     : id == RETRO_MEMORY_RTC                                ? libretroGbRtc(size)
		                                                             : NULL;
	}
	if (!gba) {
		return NULL;
	}
	switch (id) {
	case MEMORY_SECOND_SAVE:
		if (!gbaTogether) {
			return NULL;
		}
		*size = saveSize(gbas[1]);
		return gbas[1]->savedata.storage;
	case RETRO_MEMORY_SAVE_RAM:
		*size = saveSize(gba);
		return gba->savedata.storage;
	case RETRO_MEMORY_SYSTEM_RAM:
		*size = SIZE_EWRAM;
		return gba->ewram;
	default:
		return NULL;
	}
}

void* retro_get_memory_data(unsigned id) {
	size_t size;
	return memory(id, &size);
}

size_t retro_get_memory_size(unsigned id) {
	size_t size;
	memory(id, &size);
	return size;
}
