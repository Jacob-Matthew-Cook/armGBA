/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "second.h"

// Paths are cut at 4096 bytes, longer than any folder RetroArch uses.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>

// The file a frontend names for content in an archive is the archive: "game.zip#game.gb".
static void filePart(const char* path, char* out, size_t outSize) {
	snprintf(out, outSize, "%s", path);
	char* hash = strchr(out, '#');
	if (hash) {
		*hash = '\0';
	}
}

static void folderOf(const char* path, char* out, size_t outSize) {
	filePart(path, out, outSize);
	char* slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(out, outSize, ".");
	}
}

// The file name without its folder and last extension.
static void baseName(const char* path, char* out, size_t outSize) {
	char file[4096];
	filePart(path, file, sizeof(file));
	const char* slash = strrchr(file, '/');
	snprintf(out, outSize, "%s", slash ? slash + 1 : file);
	char* dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

static bool exists(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISREG(info.st_mode);
}

static bool isFolder(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISDIR(info.st_mode);
}

static bool hasExtension(const char* name, const char* const* extensions) {
	const char* dot = strrchr(name, '.');
	for (int i = 0; dot && extensions[i]; ++i) {
		if (!strcasecmp(dot + 1, extensions[i])) {
			return true;
		}
	}
	return false;
}

// A name's words, lower case, for comparing: the part before any "(", without punctuation.
static int words(const char* name, char out[16][32]) {
	int count = 0;
	int length = 0;
	for (const char* c = name;; ++c) {
		bool letter = *c && *c != '(' && *c != '[' && isalnum((unsigned char) *c);
		if (letter && length < 31) {
			out[count][length++] = (char) tolower((unsigned char) *c);
		} else if (!letter && length) {
			out[count][length] = '\0';
			length = 0;
			if (++count == 16) {
				break;
			}
		}
		if (!*c || *c == '(' || *c == '[') {
			break;
		}
	}
	return count;
}

// Words that say nothing about which game it is, so sharing them doesn't make two games partners:
// articles and joining words, in the languages of the releases. ("Version" stays: it marks the
// paired releases, as Pokemon's.)
static bool filler(const char* word) {
	static const char* const fillers[] = {
		"a", "an", "and", "of", "the", "de", "del", "des", "du", "la", "le", "les", "el", "los", "et", "y", "der", "die",
		"das", "und", "von", NULL,
	};
	for (int i = 0; fillers[i]; ++i) {
		if (!strcmp(word, fillers[i])) {
			return true;
		}
	}
	return false;
}

// How likely a game is to link with the loaded one: every release of the same game first, then by
// the words their names share, filler words aside.
static int likeness(const char* name, char loaded[16][32], int loadedCount) {
	char theirs[16][32];
	int count = words(name, theirs);
	bool same = count == loadedCount;
	int shared = 0;
	for (int i = 0; i < count; ++i) {
		same &= !strcmp(theirs[i], loaded[i]);
		if (filler(theirs[i])) {
			continue;
		}
		for (int k = 0; k < loadedCount; ++k) {
			if (!strcmp(theirs[i], loaded[k])) {
				++shared;
				break;
			}
		}
	}
	return same ? 1000 : shared;
}

struct Ranked {
	struct SecondGame game;
	int likeness;
};

static int byLikeness(const void* a, const void* b) {
	const struct Ranked* x = a;
	const struct Ranked* y = b;
	if (x->likeness != y->likeness) {
		return y->likeness - x->likeness;
	}
	return strcasecmp(x->game.label, y->game.label);
}

static void addFolder(const char* folder, const char* skip, const char* const* extensions, char loaded[16][32], int loadedCount,
                      struct Ranked** list, size_t* count, size_t* room) {
	DIR* dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent* entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.' || !hasExtension(entry->d_name, extensions)) {
			continue;
		}
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (!strcmp(path, skip) || !exists(path)) {
			continue;
		}
		if (*count == *room) {
			*room = *room ? *room * 2 : 256;
			struct Ranked* grown = realloc(*list, *room * sizeof(**list));
			if (!grown) {
				break;
			}
			*list = grown;
		}
		char label[1024];
		baseName(path, label, sizeof(label));
		struct Ranked* ranked = &(*list)[(*count)++];
		ranked->game.path = strdup(path);
		ranked->game.label = strdup(label);
		ranked->likeness = likeness(entry->d_name, loaded, loadedCount);
	}
	closedir(dir);
}

size_t secondList(const char* loadedPath, bool gameBoy, struct SecondGame* out, size_t max) {
	static const char* const gameBoyFiles[] = { "gb", "gbc", "zip", "7z", NULL };
	static const char* const advanceFiles[] = { "gba", "zip", "7z", NULL };
	char loadedFile[4096];
	char folder[4096];
	filePart(loadedPath, loadedFile, sizeof(loadedFile));
	folderOf(loadedPath, folder, sizeof(folder));
	char name[1024];
	baseName(loadedPath, name, sizeof(name));
	char loaded[16][32];
	int loadedCount = words(name, loaded);
	struct Ranked* list = NULL;
	size_t count = 0;
	size_t room = 0;
	const char* const* extensions = gameBoy ? gameBoyFiles : advanceFiles;
	addFolder(folder, loadedFile, extensions, loaded, loadedCount, &list, &count, &room);
	// Game Boy and Game Boy Color games link with each other, and usually sit in folders beside
	// each other.
	const char* last = strrchr(folder, '/');
	if (gameBoy && last && (!strcasecmp(last + 1, "gb") || !strcasecmp(last + 1, "gbc"))) {
		char other[4096];
		snprintf(other, sizeof(other), "%.*s/%s", (int) (last - folder), folder, strcasecmp(last + 1, "gb") ? "gb" : "gbc");
		if (isFolder(other)) {
			addFolder(other, loadedFile, extensions, loaded, loadedCount, &list, &count, &room);
		}
	}
	qsort(list, count, sizeof(*list), byLikeness);
	size_t kept = count < max ? count : max;
	for (size_t i = 0; i < count; ++i) {
		if (i < kept) {
			out[i] = list[i].game;
		} else {
			free(list[i].game.path);
			free(list[i].game.label);
		}
	}
	free(list);
	return kept;
}

void secondFree(struct SecondGame* games, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		free(games[i].path);
		free(games[i].label);
	}
}

void* secondRead(const char* path, size_t* size) {
	char file[4096];
	filePart(path, file, sizeof(file));
	if (path_is_compressed_file(file)) {
		// The first game in the archive, read by RetroArch's own archive code.
		struct string_list* entries = file_archive_get_file_list(file, "gb|gbc|gba|bin");
		if (!entries || !entries->size) {
			string_list_free(entries);
			return NULL;
		}
		char inner[8192];
		snprintf(inner, sizeof(inner), "%s#%s", file, entries->elems[0].data);
		string_list_free(entries);
		void* data = NULL;
		int64_t length = 0;
		if (!file_archive_compressed_read(inner, &data, NULL, &length) || length <= 0) {
			free(data);
			return NULL;
		}
		*size = (size_t) length;
		return data;
	}
	FILE* f = fopen(file, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = length > 0 ? malloc(length) : NULL;
	bool read = data && fread(data, 1, length, f) == (size_t) length;
	fclose(f);
	if (!read) {
		free(data);
		return NULL;
	}
	*size = length;
	return data;
}

// The folders RetroArch may keep a game's saves in, in the order it would use them.
static int saveFolders(const char* gamePath, const char* saveDirectory, char out[5][4096]) {
	char folder[4096];
	folderOf(gamePath, folder, sizeof(folder));
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
	baseName(gamePath, name, sizeof(name));
	for (int i = 0; i < count; ++i) {
		char path[8192];
		snprintf(path, sizeof(path), "%s/%s%s.%s", folders[i], name, suffix, extension);
		if (exists(path)) {
			return i;
		}
	}
	return -1;
}

void secondSavePath(const char* gamePath, const char* suffix, const char* extension, const char* firstPath, const char* saveDirectory,
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
	baseName(gamePath, name, sizeof(name));
	snprintf(out, outSize, "%s/%s%s.%s", folders[found], name, suffix, extension);
}

static void armgbaSavePath(const char* gamePath, const char* within, int player, const char* extension, const char* saveDirectory,
                           char* out, size_t outSize) {
	char folder[4096];
	if (saveDirectory && *saveDirectory) {
		snprintf(folder, sizeof(folder), "%s", saveDirectory);
	} else {
		folderOf(gamePath, folder, sizeof(folder));
	}
	char name[1024];
	baseName(gamePath, name, sizeof(name));
	snprintf(out, outSize, "%s/armGBA/%sPlayer %d/%s.%s", folder, within, player + 1, name, extension);
}

void secondPlayerSavePath(const char* gamePath, int player, const char* extension, const char* saveDirectory, char* out,
                          size_t outSize) {
	armgbaSavePath(gamePath, "", player, extension, saveDirectory, out, outSize);
}

void secondRemoteSavePath(const char* gamePath, int player, const char* extension, const char* saveDirectory, char* out,
                          size_t outSize) {
	armgbaSavePath(gamePath, "Remote/", player, extension, saveDirectory, out, outSize);
}

void secondMakeFolders(const char* filePath) {
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

void secondBackupPath(const char* savePath, const char* stamp, char* out, size_t outSize) {
	char stem[4096];
	const char* extension;
	splitSave(savePath, stem, sizeof(stem), &extension);
	snprintf(out, outSize, "%s.%s%s", stem, stamp, extension);
}

// Whether a name is a stamp: eight digits, a dash, six digits, then maybe a dash and a count.
static bool isStamp(const char* name, size_t length) {
	if (length < 15 || length == 16 || length >= SECOND_STAMP || name[8] != '-' || (length > 15 && name[15] != '-')) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		if (i != 8 && i != 15 && !isdigit((unsigned char) name[i])) {
			return false;
		}
	}
	return true;
}

static int newerFirst(const void* a, const void* b) {
	const char* x = a;
	const char* y = b;
	int const time = strncmp(y, x, 15);
	return time ? time : atoi(y[15] ? y + 16 : "1") - atoi(x[15] ? x + 16 : "1");
}

size_t secondBackups(const char* savePath, char (*stamps)[SECOND_STAMP], size_t max) {
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
		char stamp[SECOND_STAMP];
		snprintf(stamp, sizeof(stamp), "%.*s", (int) stampLength, file + nameLength + 1);
		if (count < max) {
			memcpy(stamps[count++], stamp, SECOND_STAMP);
			continue;
		}
		// The list is full: a newer one takes the oldest one's place.
		size_t oldest = 0;
		for (size_t i = 1; i < count; ++i) {
			oldest = newerFirst(stamps[i], stamps[oldest]) > 0 ? i : oldest;
		}
		if (newerFirst(stamp, stamps[oldest]) < 0) {
			memcpy(stamps[oldest], stamp, SECOND_STAMP);
		}
	}
	closedir(dir);
	qsort(stamps, count, sizeof(stamps[0]), newerFirst);
	return count;
}

void secondNewStamp(const char* savePath, char stamp[SECOND_STAMP]) {
	time_t const now = time(NULL);
	char base[16];
	strftime(base, sizeof(base), "%Y%m%d-%H%M%S", localtime(&now));
	// After every backup from the same second, so it lists as the newest.
	enum { MOST = 1000 };
	char (*stamps)[SECOND_STAMP] = malloc(MOST * SECOND_STAMP);
	size_t const count = stamps ? secondBackups(savePath, stamps, MOST) : 0;
	int last = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!strncmp(stamps[i], base, 15)) {
			int const n = stamps[i][15] ? atoi(stamps[i] + 16) : 1;
			last = n > last ? n : last;
		}
	}
	free(stamps);
	if (last) {
		snprintf(stamp, SECOND_STAMP, "%s-%d", base, last + 1);
	} else {
		snprintf(stamp, SECOND_STAMP, "%s", base);
	}
}

bool secondRecall(const char* memoryFile, const char* firstPath, char* out, size_t outSize) {
	FILE* f = fopen(memoryFile, "r");
	if (!f) {
		return false;
	}
	char line[8192];
	bool found = false;
	size_t length = strlen(firstPath);
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, firstPath, length) && line[length] == '\t') {
			snprintf(out, outSize, "%s", line + length + 1);
			found = true;
		}
	}
	fclose(f);
	return found;
}

void secondRemember(const char* memoryFile, const char* firstPath, const char* choice) {
	char kept[8192];
	if (secondRecall(memoryFile, firstPath, kept, sizeof(kept)) ? !strcmp(kept, choice) : !strcmp(choice, "same")) {
		return;
	}
	// The other games' lines stay as they were.
	char* others = NULL;
	size_t othersSize = 0;
	FILE* f = fopen(memoryFile, "r");
	if (f) {
		FILE* copy = open_memstream(&others, &othersSize);
		char line[8192];
		size_t length = strlen(firstPath);
		while (copy && fgets(line, sizeof(line), f)) {
			if (strncmp(line, firstPath, length) || line[length] != '\t') {
				fputs(line, copy);
			}
		}
		if (copy) {
			fclose(copy);
		}
		fclose(f);
	}
	f = fopen(memoryFile, "w");
	if (f) {
		if (others) {
			fputs(others, f);
		}
		fprintf(f, "%s\t%s\n", firstPath, choice);
		fclose(f);
	}
	free(others);
}

int secondPairFile(const char* path, const void* data, size_t size, char games[4][4096]) {
	char folder[4096];
	char roms[4096];
	folderOf(path, folder, sizeof(folder));
	folderOf(folder, roms, sizeof(roms));
	int count = 0;
	const char* text = data;
	const char* end = text + size;
	while (text < end) {
		const char* next = memchr(text, '\n', end - text);
		size_t length = (next ? next : end) - text;
		char line[4096];
		snprintf(line, sizeof(line), "%.*s", (int) length, text);
		text = next ? next + 1 : end;
		line[strcspn(line, "\r")] = '\0';
		char* start = line + strspn(line, " \t");
		if (!*start || *start == '#') {
			continue;
		}
		if (count == 4) {
			return 0;
		}
		const char* colon = strchr(start, ':');
		const char* slash = strchr(start, '/');
		bool system = colon && colon > start && (!slash || slash > colon);
		if (*start == '/') {
			snprintf(games[count++], 4096, "%s", start);
		} else if (system) {
			snprintf(games[count++], 4096, "%s/%.*s/%s", roms, (int) (colon - start), start, colon + 1);
		} else {
			snprintf(games[count++], 4096, "%s/%s", folder, start);
		}
	}
	return count >= 2 ? count : 0;
}

void secondPairSaveDirectory(const char* saveDirectory, const char* pairPath, char* out, size_t outSize) {
	char folder[4096];
	folderOf(pairPath, folder, sizeof(folder));
	const char* folderName = strrchr(folder, '/');
	folderName = folderName ? folderName + 1 : folder;
	const char* last = strrchr(saveDirectory, '/');
	if (last && *folderName && !strcmp(last + 1, folderName)) {
		snprintf(out, outSize, "%.*s", (int) (last - saveDirectory), saveDirectory);
	} else {
		snprintf(out, outSize, "%s", saveDirectory);
	}
}
