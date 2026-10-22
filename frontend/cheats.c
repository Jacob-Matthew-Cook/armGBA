/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Cheats, a file for each game in ~/.local/share/armgba/cheats, in RetroArch's .cht form, so its
// cheat files work here too: cheats = N, then cheatI_desc, cheatI_code and cheatI_enable.

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Cheat* cheats;
int cheatCount;

// The game's cheat file, by its name, as RetroArch names it.
static void cheatPath(char out[FILE_SIZE]) {
	snprintf(out, FILE_SIZE, "%s/%s.cht", cheatDirectory, app.gameName);
}

// Room for count cheats, the new ones empty; returns the last, or NULL without the memory.
static struct Cheat* growTo(int count) {
	if (count > cheatCount) {
		struct Cheat* grown = realloc(cheats, count * sizeof(*cheats));
		if (!grown) {
			return NULL;
		}
		cheats = grown;
		memset(&cheats[cheatCount], 0, (count - cheatCount) * sizeof(*cheats));
		cheatCount = count;
	}
	return &cheats[count - 1];
}

// Reads a .cht file's cheats into the list after the first base. Returns false if it can't be read.
static bool readCheats(const char* path, int base) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		int index;
		char field[16];
		char* equals = strchr(line, '=');
		if (!equals || sscanf(line, "cheat%d_%15[a-z]", &index, field) != 2 || index < 0 || index > 999) {
			continue;
		}
		char* value = equals + 1 + strspn(equals + 1, " \t\"");
		value[strcspn(value, "\"\r\n")] = '\0';
		struct Cheat* const c = growTo(base + index + 1);
		if (!c) {
			break;
		}
		if (!strcmp(field, "desc")) {
			snprintf(c->description, sizeof(c->description), "%s", value);
		} else if (!strcmp(field, "code")) {
			snprintf(c->code, sizeof(c->code), "%s", value);
		} else if (!strcmp(field, "enable")) {
			c->enabled = !strcmp(value, "true");
		}
	}
	fclose(f);
	return true;
}

// The game's cheats, from its file, as it loads.
void cheatListLoad(void) {
	free(cheats);
	cheats = NULL;
	cheatCount = 0;
	char path[FILE_SIZE];
	cheatPath(path);
	readCheats(path, 0);
}

// A .cht file's cheats added after the game's; how many, or -1 if it can't be read.
int cheatListImport(const char* path) {
	int const before = cheatCount;
	if (!readCheats(path, before)) {
		return -1;
	}
	return cheatCount - before;
}

// The game's cheats to its file; with none, the file goes.
void cheatListSave(void) {
	char path[FILE_SIZE];
	cheatPath(path);
	if (!cheatCount) {
		remove(path);
		return;
	}
	FILE* f = fopen(path, "w");
	if (!f) {
		osd("Can't write %s", path);
		return;
	}
	fprintf(f, "cheats = %d\n\n", cheatCount);
	for (int i = 0; i < cheatCount; ++i) {
		fprintf(f, "cheat%d_desc = \"%s\"\ncheat%d_code = \"%s\"\ncheat%d_enable = %s\n\n", i, cheats[i].description, i, cheats[i].code, i,
		        cheats[i].enabled ? "true" : "false");
	}
	fclose(f);
}

// The core takes the enabled ones; none in hardcore mode.
void cheatListApply(void) {
	if (!app.loaded) {
		return;
	}
	retro_cheat_reset();
	for (int i = 0; i < cheatCount && !cheevosHardcore(); ++i) {
		if (cheats[i].enabled && cheats[i].code[0]) {
			retro_cheat_set(i, true, cheats[i].code);
		}
	}
}

// A new cheat, turned on.
void cheatListAdd(const char* description, const char* code) {
	struct Cheat* c = growTo(cheatCount + 1);
	if (c) {
		snprintf(c->description, sizeof(c->description), "%s", description);
		snprintf(c->code, sizeof(c->code), "%s", code);
		c->enabled = true;
	}
}

void cheatListRemove(int index) {
	if (index >= 0 && index < cheatCount) {
		memmove(&cheats[index], &cheats[index + 1], (cheatCount - index - 1) * sizeof(*cheats));
		--cheatCount;
	}
}
