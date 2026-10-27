/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_CORE_H
#define ARMGBA_LIBRETRO_CORE_H

// What the libretro core's own files share, by the file that keeps it. Nothing here is seen
// outside the core.

#include "gb.h"
#include "gba.h"
#include "libretro.h"
#include "games.h"
#include "savefiles.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static inline int clampInt(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

enum {
	MAX_PLAYERS = 4,
	// Buffer sizes: a path RetroArch gives, a save's file made from one, and a backup's, which adds
	// a stamp.
	PATH_SIZE = 4096,
	SAVE_PATH_SIZE = 8192,
	BACKUP_PATH_SIZE = SAVE_PATH_SIZE + BACKUP_STAMP,
	// Player 2's Game's choices.
	SECOND_CHOICES = 125,
	// Turbo Button Period's range, in frames.
	TURBO_FEWEST = 4,
	TURBO_MOST = 120,
};

// libretro.c: the frontend's calls back, and the game and consoles running.
extern retro_environment_t environment;
extern retro_video_refresh_t videoRefresh;
// Player 1's Game Boy Advance, or NULL with a Game Boy game; each player's, when they run here.
extern struct GBA* gba;
extern struct GBA* gbas[MAX_PLAYERS];
extern int playersHere;
extern bool skipBios;
extern bool gameBoy;
extern enum LibretroGbModel gbModel;
// The original Game Boy's screen colors: 0 plain grays, 1 the Pocket's, 2 the first model's, 3 the
// Light's.
extern int gbShades;
// Game Boy Advance Panel: a Game Boy game on the Advance model shows the Color's panel.
extern bool gbColorPanel;
// Game Boy Advance consoles linked here, run in turns.
extern bool gbaTogether;
// On the Game Boy Advance model, a Game Boy game is shown as that console shows it.
extern bool gbOnAdvance;
// Whether the Game Boy cartridge has a tilt sensor (MBC7).
extern bool tiltCartridge;
enum { THREADED_OFF, THREADED_ON, THREADED_LATE };
extern int threadedVideo;
// Whether RetroArch is running this frame again, for a netplay rollback or runahead, rather than
// for the first time.
bool rerunning(void);

// The games: player 1's file, player 2's (empty for player 1's) and players 3 and 4's from a pair
// file; whether the others have no cartridge; the pair file; Player 2's Game's choices.
extern char firstPath[PATH_SIZE];
extern char secondPath[PATH_SIZE];
extern char otherPaths[MAX_PLAYERS][PATH_SIZE];
extern bool othersBare;
extern bool pairFile;
extern char pairPath[PATH_SIZE];
extern struct ListedGame secondGames[SECOND_CHOICES];
extern size_t secondCount;
// The game player runs, as a path.
const char* gameOf(int player);
// Restarts player's console, as its power switch would.
void restartConsole(int player);
// Players whose save starts as a copy of player 1's, made before the first frame.
extern uint8_t copySaves;
// Whether rollback netplay has been seen this session, and whether this device joined another's.
extern bool netplaySeen;
extern bool netplayJoined;
// A Game Boy Advance's save memory size, as large as the largest until the game shows its type.
size_t saveSize(struct GBA* machine);
// A message on screen for frames frames; showMessage logs it as an error too.
void notify(const char* message, unsigned frames);
void showMessage(const char* message);

// saves.c: each player's save memory and files, the saves the core keeps itself (the other
// players', and player 1's with a pair file or Player 1's Save Folder), and the Saves options.
void* saveMemory(int player, size_t* size);
// Reads a file into memory, as much as fits; returns whether there was a file.
bool readFileInto(const char* path, void* data, size_t size);
// Where RetroArch keeps player 1's save and clock, and whether the core keeps player 1's instead.
void retroArchFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]);
bool retroArchSaves(void);
// Reading saves as games start: player 1's, the newer of RetroArch's and the core's; a player's of
// a game of their own; the other players'. readNewerPlayerOne reads player 1's again each frame
// RetroArch wrote it.
void readPlayerOneSave(void);
void readNewerPlayerOne(void);
bool readSave(int player, const char* game);
void readOtherSaves(void);
void copyFirstSave(void);
// Writes the kept saves that changed (in netplay only as the game closes, on the host); forgets
// them; writeKeptSaves checks every few seconds.
void writeCoreSaves(bool closing);
void forgetSaves(void);
void forgetOtherSaves(void);
void writeKeptSaves(void);
// The Saves options: found as the frontend sets up, their choices filled and offered, and their
// actions, done once when chosen (or only reset when act is false, as a game starts).
void findSaveOptions(void);
void fillSaveChoices(void);
void offerSaveChoices(void);
void readSaveActions(bool act);
void saveOptionsChanged(void);
// Netplay's saves: a joined session's kept for Load Save, and Save Manager's received.
void keepSession(void);
// Started with no game: Save Manager.
bool noGameLoaded(void);
void saveReceived(const char* game, const uint8_t* save, size_t size, const uint8_t* rtc, size_t rtcSize);

// options.c: the options given to the frontend (NULL to an older frontend, which has plain
// variables), and their values. optionChoice gives the number of the option's value among choices,
// or fallback if it isn't one. The Wanted functions read Link Mode, Number of Players and GBA Link
// Port. showOptions shows the options that do something for the game and Link Mode, and returns
// whether that changed; setOptions gives the options again after a list of choices changed.
extern struct retro_core_options_v2* coreOptions;
struct Choice {
	const char* value;
	int number;
};
bool optionsGive(retro_environment_t callback);
struct retro_core_option_v2_definition* optionDefinition(const char* key);
const char* option(const char* key);
bool optionIs(const char* key, const char* wanted);
// Writes a whole file, the old one kept whole until the new one is (Safe Saves); false if it couldn't.
bool writeWholeFile(const char* path, const void* data, size_t size);
// Reads a whole file, to be freed; NULL if there is none, or it is empty.
void* readWholeFile(const char* path, size_t* size);
int optionChoice(const char* key, const struct Choice* choices, size_t count, int fallback);
bool togetherWanted(void);
bool separateWanted(void);
bool gamecubeWanted(void);
int playersWanted(void);
bool adapterWanted(int count);
bool showOptions(void);
void setOptions(void);

// lcd.c: LCD Ghosting (0 off, 1 half, 2 full) and Interlaced Rows; LCD Grid's scale (1 when off)
// and its shadow's way (0 none, then up and on clockwise); the threads they share rows among (0
// chooses). showPicture sends the picture on through them both, sendPicture through LCD Grid alone.
extern int ghosting;
extern bool interleaved;
extern int gridScale;
extern int gridWay;
extern int lcdThreads;
void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch);
void sendPicture(const void* picture, unsigned width, unsigned height, size_t pitch);
// Whether the screen is the first Game Boy's or the Pocket's, whose pixels cast a shadow.
bool lcdCastsShadow(void);

#endif
