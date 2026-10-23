/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Each player's save: its memory, its files, the saves the core keeps itself, and their backups
// with the Back Up Save, Load Save, Load Into and Send Save options.

#include "core.h"
#include "gb.h"
#include "link.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
	char values[BACKUP_CHOICES][BACKUP_STAMP + 4];
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

// Save files: each player's save memory and where its file is.

void* saveMemory(int player, size_t* size) {
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
bool readFileInto(const char* path, void* data, size_t size) {
	FILE* f = data && size ? fopen(path, "rb") : NULL;
	if (!f) {
		return false;
	}
	size_t read = fread(data, 1, size, f);
	fclose(f);
	return read > 0;
}

// Flushes a folder's list of files to the card, so a file just put in place stays there.
static void syncFolder(const char* path) {
	char folder[SAVE_PATH_SIZE];
	snprintf(folder, sizeof(folder), "%s", path);
	char* const slash = strrchr(folder, '/');
	if (!slash) {
		return;
	}
	*slash = '\0';
	int const fd = open(*folder ? folder : "/", O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

// Writes a whole file. With Safe Saves on, it goes to a new file beside it first, flushed to the card,
// which then takes the old one's place in one step: a write cut off by power loss or a crash leaves
// the old file whole.
bool writeWholeFile(const char* path, const void* data, size_t size) {
	bool const safe = !optionIs("armgba_safe_saves", "disabled");
	char temporary[SAVE_PATH_SIZE + 8];
	bool const fits = snprintf(temporary, sizeof(temporary), "%s.tmp", path) < (int) sizeof(temporary);
	FILE* f = !safe || fits ? fopen(safe ? temporary : path, "wb") : NULL;
	bool written = f && fwrite(data, 1, size, f) == size && fflush(f) == 0 && (!safe || fsync(fileno(f)) == 0);
	if (f) {
		written &= fclose(f) == 0;
	}
	if (safe && written && rename(temporary, path) == 0) {
		syncFolder(path);
	} else if (safe) {
		written = false;
		if (fits) {
			remove(temporary);
		}
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
		writeWholeFile(to, data, size);
	}
	free(data);
	return data != NULL;
}

// RetroArch's save directory, or with a pair file the one its games' saves go in.
static const char* saveDirectoryFor(char unsorted[PATH_SIZE]) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (pairFile && saveDirectory) {
		pairFileSaveDirectory(saveDirectory, pairPath, unsorted, PATH_SIZE);
		saveDirectory = unsorted;
	}
	return saveDirectory;
}

void retroArchFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	saveFilePath(firstPath, "", "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	saveFilePath(firstPath, "", "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

static void folderFiles(int player, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	playerSaveFilePath(firstPath, player, "srm", saveDirectory, file, SAVE_PATH_SIZE);
	playerSaveFilePath(firstPath, player, "rtc", saveDirectory, rtcFile, SAVE_PATH_SIZE);
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
	remoteSaveFilePath(firstPath, player, "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	remoteSaveFilePath(firstPath, player, "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
}

static void receivedFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	receivedSaveFilePath(firstPath, "srm", firstPath, saveDirectory, file, SAVE_PATH_SIZE);
	receivedSaveFilePath(firstPath, "rtc", firstPath, saveDirectory, rtcFile, SAVE_PATH_SIZE);
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
bool readSave(int player, const char* game) {
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	char file[PATH_SIZE];
	char rtcFile[PATH_SIZE];
	saveFilePath(game, "", "srm", firstPath, saveDirectory, file, sizeof(file));
	saveFilePath(game, "", "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
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
	saveFilePath(firstPath, suffix, "srm", firstPath, saveDirectory, older, sizeof(older));
	saveFilePath(firstPath, suffix, "rtc", firstPath, saveDirectory, olderRtc, sizeof(olderRtc));
	bool const found = keepSave(player, older, olderRtc, file, rtcFile);
	// Written to the folder next time saves are written, though unchanged.
	keptSaves[player].hash ^= found;
	return found;
}

// Whether RetroArch reads and writes player 1's save. It doesn't when the core keeps that save (in
// armGBA/Player 1, or with a pair file), or on a device that joined another's netplay session,
// where RetroArch would write the host's save over its own as the game closes.
bool retroArchSaves(void) {
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
void readPlayerOneSave(void) {
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

void readNewerPlayerOne(void) {
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
void readOtherSaves(void) {
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
void writeCoreSaves(bool closing) {
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
		makeFolders(keptSaves[player].file);
		writeWholeFile(keptSaves[player].file, save, size);
		size_t rtcSize;
		void* rtc = rtcMemory(player, &rtcSize);
		if (rtc && rtcSize) {
			writeWholeFile(keptSaves[player].rtcFile, rtc, rtcSize);
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

void forgetSaves(void) {
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		keptSaves[player].kept = false;
	}
	copySaves = 0;
}

void copyFirstSave(void) {
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
                       char stamp[BACKUP_STAMP]) {
	char out[BACKUP_PATH_SIZE];
	char newest[1][BACKUP_STAMP];
	if (backupStamps(file, newest, 1)) {
		backupFilePath(file, newest[0], out, sizeof(out));
		if (fileHolds(out, save, size)) {
			memcpy(stamp, newest[0], BACKUP_STAMP);
			return 0;
		}
	}
	newBackupStamp(file, stamp);
	backupFilePath(file, stamp, out, sizeof(out));
	makeFolders(out);
	if (!writeWholeFile(out, save, size)) {
		return -1;
	}
	if (rtc && rtcSize) {
		backupFilePath(rtcFile, stamp, out, sizeof(out));
		writeWholeFile(out, rtc, rtcSize);
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
static int backUpPlayer(int player, char stamp[BACKUP_STAMP]) {
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
	char (*stamps)[BACKUP_STAMP] = keep ? malloc(MOST * BACKUP_STAMP) : NULL;
	if (!stamps) {
		return;
	}
	size_t const count = backupStamps(saveFile, stamps, MOST);
	for (size_t i = keep; i < count; ++i) {
		char file[BACKUP_PATH_SIZE];
		backupFilePath(saveFile, stamps[i], file, sizeof(file));
		remove(file);
		backupFilePath(rtcFile, stamps[i], file, sizeof(file));
		remove(file);
	}
	free(stamps);
}

// A device that joined another's netplay session keeps the session's saves, never over its own, in
// armGBA/Remote by the session's players, for Load Save.
void keepSession(void) {
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
		char stamp[BACKUP_STAMP];
		remoteSaveFilePath(gameOf(player), player, "srm", firstPath, saveDirectory, file, sizeof(file));
		remoteSaveFilePath(gameOf(player), player, "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
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
	char stamps[BACKUP_CHOICES][BACKUP_STAMP];
	size_t const count = backupStamps(file, stamps, most);
	for (size_t i = 0; i < count; ++i) {
		char value[BACKUP_STAMP + 4], label[80], when[32];
		stampLabel(stamps[i], when, sizeof(when));
		snprintf(value, sizeof(value), "%.3s %.*s", tag, BACKUP_STAMP - 1, stamps[i]);
		snprintf(label, sizeof(label), "%s: %s", name, when);
		addChoice(list, value, label);
	}
}

// Fills Back Up Save's and Load Into's choices with the players playing, and Load Save's and Send
// Save's with their saves.
void fillSaveChoices(void) {
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
	char stamp[1][BACKUP_STAMP];
	receivedFiles(received, rtcFile);
	bool const anyReceived = *firstPath && backupStamps(received, stamp, 1);
	int players = 0;
	int groups = anyReceived;
	for (int player = 0; player < MAX_PLAYERS; ++player) {
		playing[player] = backupFiles(player, files[player], rtcFile);
		remoteFiles(player, remotes[player], rtcFile);
		remote[player] = *firstPath && backupStamps(remotes[player], stamp, 1);
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
		char name[48];
		snprintf(name, sizeof(name), "Received into Player %d", receivedTarget() + 1);
		addBackupChoices(&loadChoices, received, each, "x", name);
		addBackupChoices(&sendChoices, received, each, "x", "Received");
	}
	backUpOption->values[shown + 1] = (struct retro_core_option_value) { NULL, NULL };
	loadIntoOption->values[shown + 1] = (struct retro_core_option_value) { NULL, NULL };
	listedInto = loadIntoPlayer();
}

void offerSaveChoices(void) {
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
	makeFolders(toFile);
	writeWholeFile(toFile, save, size);
	if (rtc && fromRtc && rtcSize == fromRtcSize) {
		memcpy(rtc, fromRtc, rtcSize);
		writeWholeFile(toRtc, rtc, rtcSize);
	}
}

// Makes a backup player to's save file, moving it when it is to's own and copying it otherwise, then
// reads it into to's memory.
static void loadBackup(int to, bool move, const char* file, const char* rtcFile, const char* toFile, const char* toRtc) {
	if (move) {
		rename(file, toFile);
		rename(rtcFile, toRtc);
	} else {
		makeFolders(toFile);
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
	char replaced[BACKUP_STAMP];
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
		backupFilePath(fromBase, stamp, file, sizeof(file));
		backupFilePath(fromRtcBase, stamp, rtcFile, sizeof(rtcFile));
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
	char stamp[BACKUP_STAMP];
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
	gameName(firstPath, game, sizeof(game));
	char const* problem = !save || !size || blankSave(save, size) ? "Send Save: nothing saved yet"
	                    : !linkSendSave(game, save, size, rtc, rtcSize) ? "Send Save: no one is linked"
	                                                                     : NULL;
	if (asked) {
		showMessage(problem ? problem : "Sent this save to the other player");
	}
}

void sendSaveOnPairing(void) {
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
	backupFilePath(base, stamp, file, sizeof(file));
	backupFilePath(rtcBase, stamp, rtcFile, sizeof(rtcFile));
	gameName(firstPath, game, sizeof(game));
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
void saveReceived(const char* game, const uint8_t* save, size_t size, const uint8_t* rtc, size_t rtcSize) {
	// The game's file, for where its saves are: player 1's, a game beside it, or else beside player 1's.
	char path[PATH_SIZE];
	char name[1024];
	gameName(firstPath, name, sizeof(name));
	snprintf(path, sizeof(path), "%s", firstPath);
	for (size_t i = 0; strcmp(name, game) && i < secondCount; ++i) {
		gameName(secondGames[i].path, name, sizeof(name));
		snprintf(path, sizeof(path), "%s", secondGames[i].path);
	}
	if (strcmp(name, game)) {
		char const* slash = strrchr(firstPath, '/');
		snprintf(path, sizeof(path), "%.*s%s.srm", slash ? (int) (slash - firstPath + 1) : 0, firstPath, game);
	}
	char unsorted[PATH_SIZE];
	const char* saveDirectory = saveDirectoryFor(unsorted);
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE], stamp[BACKUP_STAMP];
	receivedSaveFilePath(path, "srm", firstPath, saveDirectory, file, sizeof(file));
	receivedSaveFilePath(path, "rtc", firstPath, saveDirectory, rtcFile, sizeof(rtcFile));
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
void readSaveActions(bool act) {
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
		char send[BACKUP_STAMP + 4];
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
		char load[BACKUP_STAMP + 4];
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

// The Saves options whose choices are filled in here.
void findSaveOptions(void) {
	loadChoices.option = optionDefinition("armgba_load_save");
	sendChoices.option = optionDefinition("armgba_send_save");
	backUpOption = optionDefinition("armgba_back_up_save");
	loadIntoOption = optionDefinition("armgba_load_into");
}

// The other players' kept saves are forgotten as their consoles stop.
void forgetOtherSaves(void) {
	for (int player = 1; player < MAX_PLAYERS; ++player) {
		keptSaves[player].kept = false;
	}
}

// After an option changed: the Saves actions chosen, and the lists made again if Backups Listed or
// Load Into changed.
void saveOptionsChanged(void) {
	readSaveActions(true);
	const char* listed = option("armgba_list_saves");
	if ((listed && atoi(listed) != backupsListed) || loadIntoPlayer() != listedInto) {
		offerSaveChoices();
	}
}

// Each frame: the kept saves are written when they change, checked every few seconds.
void writeKeptSaves(void) {
	if (savesKept() && ++keptCheck >= 300) {
		keptCheck = 0;
		writeCoreSaves(false);
	}
}
