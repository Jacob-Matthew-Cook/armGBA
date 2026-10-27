/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_SAVEFILES_H
#define ARMGBA_LIBRETRO_SAVEFILES_H

// Where saves' files are: each game's, as RetroArch keeps them; the other players' and a joined
// netplay session's, in armGBA's folders; and each save's backups.

#include <stdbool.h>
#include <stddef.h>

// The file where a game's save (extension "srm" or "rtc") is kept. It looks where RetroArch would
// keep it: beside the game, in the save directory, and in the save directory's per-core and
// per-folder subfolders. A new one goes where player 1's game keeps its save. A suffix such as "
// (Player 2)" names a second save for the same game.
void saveFilePath(const char* gamePath, const char* suffix, const char* extension, const char* firstPath, const char* saveDirectory, char* out,
                  size_t outSize);
// Where another player keeps their own save of player 1's game: in the save directory's
// armGBA/Player N folder (beside the game without one), under the game's own file name.
void playerSaveFilePath(const char* gamePath, int player, const char* extension, const char* saveDirectory, char* out, size_t outSize);
// Where a device that joined another's netplay session keeps that session's saves:
// armGBA/Remote/Player N in the folder that game's own saves go in, so a Game Boy Color game's are
// with gbc's though player 1 played from gb.
void remoteSaveFilePath(const char* gamePath, int player, const char* extension, const char* firstPath, const char* saveDirectory, char* out,
                        size_t outSize);
// Makes the folders a file is to go in.
void makeFolders(const char* filePath);

// A save's backups, beside it, named for when they were made: "Game.srm" keeps
// "Game.20261018-153012.srm", with "-2" and on after the time when two fall in the same second.
// The stamp is the part between the dots.
enum { BACKUP_STAMP = 24 };
// Lists the stamps of a save's newest backups, newest first. Returns how many, at most max.
size_t backupStamps(const char* savePath, char (*stamps)[BACKUP_STAMP], size_t max);
// A stamp for now that sorts after every backup the save has.
void newBackupStamp(const char* savePath, char stamp[BACKUP_STAMP]);
// The file of the save's backup with this stamp.
void backupFilePath(const char* savePath, const char* stamp, char* out, size_t outSize);
// Whether a file's name is a backup's, Game.<stamp>.srm.
bool isBackupName(const char* name);

#endif
