/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_GAMES_H
#define ARMGBA_LIBRETRO_GAMES_H

// Game files: listing the games to pick player 2's from, reading one (out of an archive too),
// remembering each game's choice, and pair files naming a link's games.

#include <stdbool.h>
#include <stddef.h>

struct ListedGame {
	// The file, and its name as shown: the file name without its extension.
	char* path;
	char* label;
};

// Lists the games in the folder the loaded game came from, the likeliest partners first:
// - other releases of the same game;
// - games sharing words with its name (Red with Blue and Yellow), not counting words such as "the"
//   and "of";
// - the rest, by name.
// A Game Boy game's list takes in both the gb and gbc folders beside each other. The loaded game
// itself is left out. Returns how many, at most max; free with gamesFree.
size_t gamesList(const char* loadedPath, bool gameBoy, struct ListedGame* out, size_t max);
void gamesFree(struct ListedGame* games, size_t count);
// Reads a game file, taking the game out of a .zip or .7z. Returns NULL if it can't.
void* gameRead(const char* path, size_t* size);
// A game's file name without its folder or extension, as its save is named; and its folder. A
// frontend names a game in an archive "game.zip#game.gb": the archive is the file.
void gameName(const char* gamePath, char* out, size_t outSize);
void gameFolder(const char* gamePath, char* out, size_t outSize);

// Each game's choice of player 2's game, kept in a text file of "game<TAB>choice" lines: RetroArch
// keeps one value for every game, and forgets a value its list didn't have before the game loaded.
bool choiceRecall(const char* memoryFile, const char* firstPath, char* out, size_t outSize);
void choiceRemember(const char* memoryFile, const char* firstPath, const char* choice);

// A pair file names the games of a link, two to four, one per line, as Batocera's .gb2 files do.
// Each line is one of:
// - "gb:Game.zip" for a game in the gb folder beside the pair file's folder (gbc: and gba:
//   likewise);
// - a path from the pair file's folder;
// - a full path.
// Lines starting with # are skipped. Returns how many games, or 0 for fewer than two or more than
// four.
int pairFileGames(const char* path, const void* data, size_t size, char games[4][4096]);
// The save directory for the games of a pair file: RetroArch's, unless it was sorted into a folder
// named for the pair file's folder, when it is the one above.
void pairFileSaveDirectory(const char* saveDirectory, const char* pairPath, char* out, size_t outSize);

#endif
