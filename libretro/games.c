/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "games.h"

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

void gameFolder(const char* path, char* out, size_t outSize) {
	filePart(path, out, outSize);
	char* slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(out, outSize, ".");
	}
}

void gameName(const char* path, char* out, size_t outSize) {
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

enum { MAX_WORDS = 16, WORD_SIZE = 32 };

// A name's words, lower case, for comparing: the part before any "(", without punctuation.
static int words(const char* name, char out[MAX_WORDS][WORD_SIZE]) {
	int count = 0;
	int length = 0;
	for (const char* c = name;; ++c) {
		bool letter = *c && *c != '(' && *c != '[' && isalnum((unsigned char) *c);
		if (letter && length < WORD_SIZE - 1) {
			out[count][length++] = (char) tolower((unsigned char) *c);
		} else if (!letter && length) {
			out[count][length] = '\0';
			length = 0;
			if (++count == MAX_WORDS) {
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
static int likeness(const char* name, char loaded[MAX_WORDS][WORD_SIZE], int loadedCount) {
	char theirs[MAX_WORDS][WORD_SIZE];
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
	struct ListedGame game;
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

static void addFolder(const char* folder, const char* skip, const char* const* extensions, char loaded[MAX_WORDS][WORD_SIZE], int loadedCount,
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
		gameName(path, label, sizeof(label));
		struct Ranked* ranked = &(*list)[(*count)++];
		ranked->game.path = strdup(path);
		ranked->game.label = strdup(label);
		ranked->likeness = likeness(entry->d_name, loaded, loadedCount);
	}
	closedir(dir);
}

size_t gamesList(const char* loadedPath, bool gameBoy, struct ListedGame* out, size_t max) {
	static const char* const gameBoyFiles[] = { "gb", "gbc", "zip", "7z", NULL };
	static const char* const advanceFiles[] = { "gba", "zip", "7z", NULL };
	char loadedFile[4096];
	char folder[4096];
	filePart(loadedPath, loadedFile, sizeof(loadedFile));
	gameFolder(loadedPath, folder, sizeof(folder));
	char name[1024];
	gameName(loadedPath, name, sizeof(name));
	char loaded[MAX_WORDS][WORD_SIZE];
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

void gamesFree(struct ListedGame* games, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		free(games[i].path);
		free(games[i].label);
	}
}

void* gameRead(const char* path, size_t* size) {
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

bool choiceRecall(const char* memoryFile, const char* firstPath, char* out, size_t outSize) {
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

void choiceRemember(const char* memoryFile, const char* firstPath, const char* choice) {
	char kept[8192];
	if (choiceRecall(memoryFile, firstPath, kept, sizeof(kept)) ? !strcmp(kept, choice) : !strcmp(choice, "same")) {
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

int pairFileGames(const char* path, const void* data, size_t size, char games[4][4096]) {
	char folder[4096];
	char roms[4096];
	gameFolder(path, folder, sizeof(folder));
	gameFolder(folder, roms, sizeof(roms));
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

void pairFileSaveDirectory(const char* saveDirectory, const char* pairPath, char* out, size_t outSize) {
	char folder[4096];
	gameFolder(pairPath, folder, sizeof(folder));
	const char* folderName = strrchr(folder, '/');
	folderName = folderName ? folderName + 1 : folder;
	const char* last = strrchr(saveDirectory, '/');
	if (last && *folderName && !strcmp(last + 1, folderName)) {
		snprintf(out, outSize, "%.*s", (int) (last - saveDirectory), saveDirectory);
	} else {
		snprintf(out, outSize, "%s", saveDirectory);
	}
}
