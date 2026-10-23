/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "savefiles.h"
#include "games.h"

// Paths are cut at 4096 bytes, longer than any folder RetroArch uses.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static bool exists(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISREG(info.st_mode);
}

static bool isFolder(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISDIR(info.st_mode);
}

// The folders RetroArch may keep a game's saves in, in the order it would use them.
static int saveFolders(const char* gamePath, const char* saveDirectory, char out[5][4096]) {
	char folder[4096];
	gameFolder(gamePath, folder, sizeof(folder));
	int count = 0;
	snprintf(out[count++], 4096, "%s", folder);
	if (saveDirectory && *saveDirectory) {
		const char* last = strrchr(folder, '/');
		const char* folderName = last ? last + 1 : folder;
		snprintf(out[count++], 4096, "%s/armGBA", saveDirectory);
		snprintf(out[count++], 4096, "%s/%s/armGBA", saveDirectory, folderName);
		snprintf(out[count++], 4096, "%s/%s", saveDirectory, folderName);
		snprintf(out[count++], 4096, "%s", saveDirectory);
	}
	return count;
}

// Which of the folders holds a game's save, or -1.
static int saveFolderOf(const char* gamePath, const char* suffix, const char* extension, const char* saveDirectory) {
	char folders[5][4096];
	int count = saveFolders(gamePath, saveDirectory, folders);
	char name[1024];
	gameName(gamePath, name, sizeof(name));
	for (int i = 0; i < count; ++i) {
		char path[8192];
		snprintf(path, sizeof(path), "%s/%s%s.%s", folders[i], name, suffix, extension);
		if (exists(path)) {
			return i;
		}
	}
	return -1;
}

void saveFilePath(const char* gamePath, const char* suffix, const char* extension, const char* firstPath, const char* saveDirectory,
                    char* out, size_t outSize) {
	char folders[5][4096];
	int count = saveFolders(gamePath, saveDirectory, folders);
	int found = saveFolderOf(gamePath, suffix, extension, saveDirectory);
	if (found < 0) {
		// Where player 1's game keeps its save, else the save directory (beside the game without
		// one).
		found = saveFolderOf(firstPath, "", "srm", saveDirectory);
		found = found >= 0 ? found : count - 1;
	}
	char name[1024];
	gameName(gamePath, name, sizeof(name));
	snprintf(out, outSize, "%s/%s%s.%s", folders[found], name, suffix, extension);
}

static void armgbaSavePath(const char* gamePath, const char* within, const char* extension, const char* saveDirectory, char* out,
                           size_t outSize) {
	char folder[4096];
	if (saveDirectory && *saveDirectory) {
		snprintf(folder, sizeof(folder), "%s", saveDirectory);
	} else {
		gameFolder(gamePath, folder, sizeof(folder));
	}
	char name[1024];
	gameName(gamePath, name, sizeof(name));
	snprintf(out, outSize, "%s/armGBA/%s%s.%s", folder, within, name, extension);
}

void playerSaveFilePath(const char* gamePath, int player, const char* extension, const char* saveDirectory, char* out,
                          size_t outSize) {
	char within[32];
	snprintf(within, sizeof(within), "Player %d/", player + 1);
	armgbaSavePath(gamePath, within, extension, saveDirectory, out, outSize);
}

// The folder a game's saves go in, following where player 1's game keeps its own:
// - beside the game, when saves are kept with the games;
// - the folder named for the game's folder, when RetroArch sorts saves by content folder;
// - otherwise the save directory.
static void gameSaveFolder(const char* gamePath, const char* firstPath, const char* saveDirectory, char* out, size_t outSize) {
	char firstFolder[4096];
	char ownFolder[4096];
	gameFolder(firstPath, firstFolder, sizeof(firstFolder));
	gameFolder(gamePath, ownFolder, sizeof(ownFolder));
	char directory[4096];
	snprintf(directory, sizeof(directory), "%s", saveDirectory ? saveDirectory : "");
	size_t length = strlen(directory);
	while (length > 1 && directory[length - 1] == '/') {
		directory[--length] = '\0';
	}
	const char* firstName = strrchr(firstFolder, '/');
	const char* directoryName = strrchr(directory, '/');
	const char* folderName = strrchr(ownFolder, '/');
	if (!length || !strcmp(directory, firstFolder)) {
		snprintf(out, outSize, "%s", ownFolder);
	} else if (firstName && directoryName && folderName && !strcmp(firstName, directoryName)) {
		snprintf(out, outSize, "%.*s%s", (int) (directoryName - directory), directory, folderName);
	} else {
		snprintf(out, outSize, "%s", directory);
	}
}

void remoteSaveFilePath(const char* gamePath, int player, const char* extension, const char* firstPath, const char* saveDirectory,
                          char* out, size_t outSize) {
	char within[32];
	snprintf(within, sizeof(within), "Remote/Player %d/", player + 1);
	char folder[4096];
	gameSaveFolder(gamePath, firstPath, saveDirectory, folder, sizeof(folder));
	armgbaSavePath(gamePath, within, extension, folder, out, outSize);
}

void receivedSaveFilePath(const char* gamePath, const char* extension, const char* firstPath, const char* saveDirectory, char* out,
                            size_t outSize) {
	char folder[4096];
	gameSaveFolder(gamePath, firstPath, saveDirectory, folder, sizeof(folder));
	armgbaSavePath(gamePath, "Received/", extension, folder, out, outSize);
}

void makeFolders(const char* filePath) {
	char path[4096];
	snprintf(path, sizeof(path), "%s", filePath);
	for (char* slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (!isFolder(path)) {
			mkdir(path, 0755);
		}
		*slash = '/';
	}
}

// Splits a save's path at its name's last dot: the part before, and the extension with its dot.
static void splitSave(const char* savePath, char* stem, size_t stemSize, const char** extension) {
	const char* slash = strrchr(savePath, '/');
	const char* dot = strrchr(savePath, '.');
	if (!dot || (slash && dot < slash)) {
		dot = savePath + strlen(savePath);
	}
	snprintf(stem, stemSize, "%.*s", (int) (dot - savePath), savePath);
	*extension = dot;
}

void backupFilePath(const char* savePath, const char* stamp, char* out, size_t outSize) {
	char stem[4096];
	const char* extension;
	splitSave(savePath, stem, sizeof(stem), &extension);
	snprintf(out, outSize, "%s.%s%s", stem, stamp, extension);
}

// The length of a stamp's time, "20261018-153012".
enum { STAMP_TIME = 15 };

// Whether a name is a stamp: eight digits, a dash, six digits, then maybe a dash and a count.
static bool isStamp(const char* name, size_t length) {
	if (length < STAMP_TIME || length == STAMP_TIME + 1 || length >= BACKUP_STAMP || name[8] != '-'
	    || (length > STAMP_TIME && name[STAMP_TIME] != '-')) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		if (i != 8 && i != STAMP_TIME && !isdigit((unsigned char) name[i])) {
			return false;
		}
	}
	return true;
}

// A stamp's count among backups from the same second: 1 for the first, which has none.
static int stampCount(const char* stamp) {
	return stamp[STAMP_TIME] ? atoi(stamp + STAMP_TIME + 1) : 1;
}

static int newerFirst(const void* a, const void* b) {
	const char* x = a;
	const char* y = b;
	int const time = strncmp(y, x, STAMP_TIME);
	return time ? time : stampCount(y) - stampCount(x);
}

size_t backupStamps(const char* savePath, char (*stamps)[BACKUP_STAMP], size_t max) {
	char stem[4096];
	const char* extension;
	splitSave(savePath, stem, sizeof(stem), &extension);
	char* slash = strrchr(stem, '/');
	if (!slash) {
		return 0;
	}
	*slash = '\0';
	const char* name = slash + 1;
	size_t const nameLength = strlen(name);
	size_t const extensionLength = strlen(extension);
	DIR* dir = opendir(*stem ? stem : "/");
	if (!dir) {
		return 0;
	}
	size_t count = 0;
	struct dirent* entry;
	while (max && (entry = readdir(dir))) {
		const char* file = entry->d_name;
		size_t const length = strlen(file);
		if (length <= nameLength + 1 + extensionLength || strncmp(file, name, nameLength) || file[nameLength] != '.'
		    || strcmp(file + length - extensionLength, extension)) {
			continue;
		}
		size_t const stampLength = length - nameLength - 1 - extensionLength;
		if (!isStamp(file + nameLength + 1, stampLength)) {
			continue;
		}
		char stamp[BACKUP_STAMP];
		snprintf(stamp, sizeof(stamp), "%.*s", (int) stampLength, file + nameLength + 1);
		if (count < max) {
			memcpy(stamps[count++], stamp, BACKUP_STAMP);
			continue;
		}
		// The list is full: a newer one takes the oldest one's place.
		size_t oldest = 0;
		for (size_t i = 1; i < count; ++i) {
			oldest = newerFirst(stamps[i], stamps[oldest]) > 0 ? i : oldest;
		}
		if (newerFirst(stamp, stamps[oldest]) < 0) {
			memcpy(stamps[oldest], stamp, BACKUP_STAMP);
		}
	}
	closedir(dir);
	qsort(stamps, count, sizeof(stamps[0]), newerFirst);
	return count;
}

void newBackupStamp(const char* savePath, char stamp[BACKUP_STAMP]) {
	time_t const now = time(NULL);
	char base[STAMP_TIME + 1];
	strftime(base, sizeof(base), "%Y%m%d-%H%M%S", localtime(&now));
	// After every backup from the same second, so it lists as the newest.
	enum { MOST = 1000 };
	char (*stamps)[BACKUP_STAMP] = malloc(MOST * BACKUP_STAMP);
	size_t const count = stamps ? backupStamps(savePath, stamps, MOST) : 0;
	int last = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!strncmp(stamps[i], base, STAMP_TIME)) {
			int const n = stampCount(stamps[i]);
			last = n > last ? n : last;
		}
	}
	free(stamps);
	if (last) {
		snprintf(stamp, BACKUP_STAMP, "%s-%d", base, last + 1);
	} else {
		snprintf(stamp, BACKUP_STAMP, "%s", base);
	}
}
