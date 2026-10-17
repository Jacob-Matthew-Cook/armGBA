/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_SECOND_H
#define ARMGBA_LIBRETRO_SECOND_H

// Player 2's game when both consoles of a link run here: the games to pick it from, reading it, and
// where its save file is.

#include <stdbool.h>
#include <stddef.h>

struct SecondGame {
	// The file, and its name as shown: the file name without its extension.
	char* path;
	char* label;
};

// Lists the games in the folder the loaded game came from, the likeliest partners for it first: other
// releases of the same game, then games sharing words with its name (Red with Blue and Yellow), then
// the rest by name. A Game Boy game's list takes in the gb and gbc folders beside each other. The
// loaded game itself is left out. Returns how many, at most max; free with secondFree.
size_t secondList(const char* loadedPath, bool gameBoy, struct SecondGame* out, size_t max);
void secondFree(struct SecondGame* games, size_t count);
// Reads a game file, taking the game out of a .zip or .7z. Returns NULL if it can't.
void* secondRead(const char* path, size_t* size);
// The file where a game's save (extension "srm" or "rtc") is kept: where RetroArch keeps it, by
// looking for it beside the game, in the save directory, and in the save directory's per-core and
// per-folder subfolders; a new one goes where player 1's game keeps its save. A suffix such as
// " (Player 2)" names a second save for the same game.
void secondSavePath(const char* gamePath, const char* suffix, const char* extension, const char* firstPath, const char* saveDirectory,
                    char* out, size_t outSize);

// Each game's choice of player 2's game, kept in a text file of "game<TAB>choice" lines: RetroArch
// keeps one value for every game, and forgets a value its list didn't have before the game loaded.
bool secondRecall(const char* memoryFile, const char* firstPath, char* out, size_t outSize);
void secondRemember(const char* memoryFile, const char* firstPath, const char* choice);

// A pair file names the games of a link, two to four, one per line, as Batocera's .gb2 files do: "gb:Game.zip"
// for a game in the gb folder beside the pair file's folder (gbc: and gba: likewise), a path from the
// pair file's folder, or a full path. Lines starting with # are left out. Returns how many games, or 0
// for fewer than two or more than four.
int secondPairFile(const char* path, const void* data, size_t size, char games[4][4096]);
// The save directory for the games of a pair file: RetroArch's, unless it was sorted into a folder
// named for the pair file's folder, when it is the one above.
void secondPairSaveDirectory(const char* saveDirectory, const char* pairPath, char* out, size_t outSize);

#endif
