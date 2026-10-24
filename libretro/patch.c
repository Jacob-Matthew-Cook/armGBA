/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "patch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// No patched game is larger: a 32 MiB Game Boy Advance cartridge, or an 8 MiB Game Boy one, with room.
enum { PATCHED_MOST = 64 << 20 };

// A patch being read, and the patched game being made.
struct Patch {
	const uint8_t* at;
	const uint8_t* end;
	const uint8_t* source;
	size_t sourceSize;
	uint8_t* out;
	size_t outSize;
};

// The next byte, or -1 past the end.
static int nextByte(struct Patch* p) {
	return p->at < p->end ? *p->at++ : -1;
}

// UPS's and BPS's numbers: seven bits a byte, the last with its top bit set, each byte after the first
// counting one more. False if it runs past the end or is too large.
static bool number(struct Patch* p, uint64_t* value) {
	uint64_t total = 0, shift = 1;
	for (int i = 0; i < 9; ++i) {
		int const b = nextByte(p);
		if (b < 0) {
			return false;
		}
		total += (uint64_t) (b & 0x7F) * shift;
		if (b & 0x80) {
			*value = total;
			return true;
		}
		shift <<= 7;
		total += shift;
	}
	return false;
}

static uint32_t read32(const uint8_t* at) {
	return at[0] | at[1] << 8 | at[2] << 16 | (uint32_t) at[3] << 24;
}

// IPS: "PATCH", then records of a 3-byte offset and 2-byte length, big-endian, and that many bytes,
// or with a length of 0 a 2-byte count and one byte repeated; "EOF", then perhaps a 3-byte size to
// cut the game to. A record may run past the game's end, which grows it.
static bool applyIps(struct Patch* p) {
	if (p->end - p->at < 8 || memcmp(p->at, "PATCH", 5)) {
		return false;
	}
	p->at += 5;
	size_t size = p->sourceSize;
	// The size the records make, first, so the game is copied once.
	size_t const left = (size_t) (p->end - p->at);
	for (size_t i = 0; i + 3 <= left && memcmp(p->at + i, "EOF", 3);) {
		const uint8_t* const r = p->at + i;
		if (left - i < 5) {
			return false;
		}
		size_t const offset = (size_t) r[0] << 16 | r[1] << 8 | r[2], length = r[3] << 8 | r[4];
		size_t const span = length ? length : (left - i >= 7 ? (size_t) (r[5] << 8 | r[6]) : 0);
		size = offset + span > size ? offset + span : size;
		i += length ? 5 + length : 8;
	}
	if (size > PATCHED_MOST || !(p->out = calloc(size ? size : 1, 1))) {
		return false;
	}
	memcpy(p->out, p->source, p->sourceSize);
	p->outSize = size;
	for (;;) {
		if (p->end - p->at < 3) {
			return false;
		}
		if (!memcmp(p->at, "EOF", 3)) {
			p->at += 3;
			break;
		}
		if (p->end - p->at < 5) {
			return false;
		}
		size_t const offset = (size_t) p->at[0] << 16 | p->at[1] << 8 | p->at[2], length = p->at[3] << 8 | p->at[4];
		p->at += 5;
		if (length) {
			if ((size_t) (p->end - p->at) < length) {
				return false;
			}
			memcpy(p->out + offset, p->at, length);
			p->at += length;
		} else {
			if (p->end - p->at < 3) {
				return false;
			}
			memset(p->out + offset, p->at[2], (size_t) (p->at[0] << 8 | p->at[1]));
			p->at += 3;
		}
	}
	if (p->end - p->at >= 3) {
		size_t const cut = (size_t) p->at[0] << 16 | p->at[1] << 8 | p->at[2];
		p->outSize = cut < p->outSize ? cut : p->outSize;
	}
	return true;
}

// UPS: "UPS1", the game's size and the patched size, then runs: how many bytes to keep, then bytes
// XORed in until one of 0. The game's, the patched game's and the patch's CRC32 end it.
static bool applyUps(struct Patch* p) {
	uint64_t sourceSize, targetSize;
	if (p->end - p->at < 16 || memcmp(p->at, "UPS1", 4)) {
		return false;
	}
	const uint8_t* const footer = p->end - 12;
	p->at += 4;
	p->end = footer;
	if (!number(p, &sourceSize) || !number(p, &targetSize) || sourceSize != p->sourceSize || targetSize > PATCHED_MOST ||
	    read32(footer) != (uint32_t) crc32(0, p->source, (uInt) p->sourceSize) || !(p->out = calloc(targetSize ? targetSize : 1, 1))) {
		return false;
	}
	p->outSize = targetSize;
	memcpy(p->out, p->source, sourceSize < targetSize ? sourceSize : targetSize);
	uint64_t at = 0;
	while (p->at < p->end) {
		uint64_t skip;
		if (!number(p, &skip)) {
			return false;
		}
		at += skip;
		for (;;) {
			int const b = nextByte(p);
			if (b < 0) {
				return false;
			}
			if (at < targetSize) {
				p->out[at] ^= (uint8_t) b;
			}
			++at;
			if (!b) {
				break;
			}
		}
	}
	return read32(footer + 4) == (uint32_t) crc32(0, p->out, (uInt) targetSize);
}

// BPS: "BPS1", the game's size, the patched size and some metadata, then commands that make the
// patched game in order: copy the game's bytes at the same place, copy bytes from the patch, or copy
// from elsewhere in the game or in what's made so far, at an offset moved by each copy. The game's,
// the patched game's and the patch's CRC32 end it.
static bool applyBps(struct Patch* p) {
	uint64_t sourceSize, targetSize, metadata;
	if (p->end - p->at < 16 || memcmp(p->at, "BPS1", 4)) {
		return false;
	}
	const uint8_t* const footer = p->end - 12;
	p->at += 4;
	p->end = footer;
	if (!number(p, &sourceSize) || !number(p, &targetSize) || !number(p, &metadata) || sourceSize != p->sourceSize ||
	    targetSize > PATCHED_MOST || metadata > (uint64_t) (p->end - p->at) ||
	    read32(footer) != (uint32_t) crc32(0, p->source, (uInt) p->sourceSize) || !(p->out = malloc(targetSize ? targetSize : 1))) {
		return false;
	}
	p->at += metadata;
	p->outSize = targetSize;
	uint64_t made = 0;
	int64_t sourceAt = 0, targetAt = 0;
	while (p->at < p->end) {
		uint64_t command;
		if (!number(p, &command)) {
			return false;
		}
		uint64_t const length = (command >> 2) + 1;
		if (length > targetSize - made) {
			return false;
		}
		switch (command & 3) {
		case 0:
			if (made + length > sourceSize) {
				return false;
			}
			memcpy(p->out + made, p->source + made, length);
			break;
		case 1:
			if (length > (uint64_t) (p->end - p->at)) {
				return false;
			}
			memcpy(p->out + made, p->at, length);
			p->at += length;
			break;
		default: {
			uint64_t offset;
			if (!number(p, &offset) || offset >> 1 > PATCHED_MOST) {
				return false;
			}
			int64_t const move = (offset & 1 ? -1 : 1) * (int64_t) (offset >> 1);
			if ((command & 3) == 2) {
				sourceAt += move;
				if (sourceAt < 0 || (uint64_t) sourceAt + length > sourceSize) {
					return false;
				}
				memcpy(p->out + made, p->source + sourceAt, length);
				sourceAt += length;
			} else {
				targetAt += move;
				if (targetAt < 0 || (uint64_t) targetAt >= made) {
					return false;
				}
				// A byte at a time: the copy may read what it has just made.
				for (uint64_t i = 0; i < length; ++i) {
					p->out[made + i] = p->out[targetAt++];
				}
			}
			break;
		}
		}
		made += length;
	}
	return made == targetSize && read32(footer + 4) == (uint32_t) crc32(0, p->out, (uInt) targetSize);
}

static uint8_t* readFile(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long const length = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = length > 0 && length <= PATCHED_MOST ? malloc((size_t) length) : NULL;
	bool const read = data && fread(data, 1, (size_t) length, f) == (size_t) length;
	fclose(f);
	if (!read) {
		free(data);
		return NULL;
	}
	*size = (size_t) length;
	return data;
}

int patchGame(const char* path, void** data, size_t* size, char* name, size_t nameSize) {
	// The game's path without the part in an archive or the extension.
	char base[4096];
	snprintf(base, sizeof(base), "%s", path);
	char* const hash = strchr(base, '#');
	if (hash) {
		*hash = '\0';
	}
	char* const dot = strrchr(base, '.');
	const char* const slash = strrchr(base, '/');
	if (dot && (!slash || dot > slash)) {
		*dot = '\0';
	}
	static const char* const kinds[] = { "bps", "ups", "ips" };
	static bool (*const appliers[])(struct Patch*) = { applyBps, applyUps, applyIps };
	uint8_t* game = *data;
	size_t gameSize = *size;
	int applied = 0;
	for (int number = 0; number < 100; ++number) {
		char file[4200];
		uint8_t* patch = NULL;
		size_t patchSize = 0;
		int kind = 0;
		for (; kind < 3 && !patch; ++kind) {
			snprintf(file, sizeof(file), number ? "%s.%s%d" : "%s.%s", base, kinds[kind], number);
			patch = readFile(file, &patchSize);
		}
		if (!patch) {
			// The numbered ones go on from 1 without a gap.
			if (number) {
				break;
			}
			continue;
		}
		struct Patch p = { patch, patch + patchSize, game, gameSize, NULL, 0 };
		bool const ok = appliers[kind - 1](&p);
		free(patch);
		if (!ok) {
			free(p.out);
			if (game != *data) {
				free(game);
			}
			const char* const fileName = strrchr(file, '/');
			snprintf(name, nameSize, "%s", fileName ? fileName + 1 : file);
			return -1;
		}
		if (game != *data) {
			free(game);
		}
		game = p.out;
		gameSize = p.outSize;
		++applied;
	}
	if (applied) {
		free(*data);
		*data = game;
		*size = gameSize;
	}
	return applied;
}
