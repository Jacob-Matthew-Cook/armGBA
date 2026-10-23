/* Copyright (c) 2016 taizou
 * Copyright (c) 2013-2024 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit.h"

#include <stdlib.h>
#include <string.h>

// Cartridge hardware beyond a ROM and a save chip, after mGBA's cart/matrix.c and cart/vfame.c: the
// Matrix mapper of the 64 MiB Game Boy Advance Video cartridges, Vast Fame's protection, and the
// bank register of multi-game cartridges.

enum {
	MATRIX_PAGE = 0x200,
	MATRIX_WINDOW = MATRIX_PAGES * MATRIX_PAGE,
};

// Matrix: the first 8 KiB of the cartridge's address space is sixteen 512-byte pages, each showing
// any 512 bytes of the 64 MiB chip. The rest of the space shows the chip's first 32 MiB as is.
static void matrixRemap(struct GBA* gba) {
	struct Cart* cart = &gba->cart;
	uint32_t vaddr = cart->matrixVaddr;
	uint32_t size = cart->matrixSize;
	// Whole pages inside the window, from inside the chip.
	if ((vaddr & ~(MATRIX_WINDOW - MATRIX_PAGE)) || (size & ~(MATRIX_WINDOW - MATRIX_PAGE)) || !size || vaddr + size > MATRIX_WINDOW ||
	    cart->matrixPaddr > cart->fileSize || size > cart->fileSize - cart->matrixPaddr) {
		return;
	}
	for (uint32_t i = 0; i < size / MATRIX_PAGE; ++i) {
		cart->matrixMap[vaddr / MATRIX_PAGE + i] = cart->matrixPaddr + i * MATRIX_PAGE;
	}
	memcpy(&gba->rom[vaddr], &cart->file[cart->matrixPaddr], size);
	// Code compiled from what the pages showed before is no longer what's there.
	jitInvalidate(gba, 0x08000000 + vaddr, size);
}

// Handles the mapper's four registers at 0x08800100: command, chip address, window address, and
// size in pages. Command 0x01 or 0x11 maps what the other three describe.
void cartMatrixWrite(struct GBA* gba, uint32_t reg, uint32_t value) {
	struct Cart* cart = &gba->cart;
	switch (reg) {
	case 0x0:
		cart->matrixCmd = value;
		if (value == 0x01 || value == 0x11) {
			matrixRemap(gba);
		}
		break;
	case 0x4:
		cart->matrixPaddr = value & 0x03FFFFFF;
		break;
	case 0x8:
		cart->matrixVaddr = value & 0x007FFFFF;
		break;
	case 0xC:
		if (value) {
			cart->matrixSize = value << 9;
		}
		break;
	}
}

// A 16-bit write sets a register's low half.
void cartMatrixWrite16(struct GBA* gba, uint32_t reg, uint16_t value) {
	struct Cart* cart = &gba->cart;
	static const size_t fields[4] = { offsetof(struct Cart, matrixCmd), offsetof(struct Cart, matrixPaddr), offsetof(struct Cart, matrixVaddr),
		                          offsetof(struct Cart, matrixSize) };
	if (reg & 3 || reg > 0xC) {
		return;
	}
	uint32_t old;
	memcpy(&old, (uint8_t*) cart + fields[reg >> 2], 4);
	cartMatrixWrite(gba, reg, value | (old & 0xFFFF0000));
}

// Vast Fame: reads past the end of the ROM return a value worked out from the address, which the
// games check, and writes to SRAM go to a scrambled address with scrambled bits, in a mode the game
// picks by writing a sequence to the top of SRAM.

enum {
	VFAME_STANDARD = 1,
	VFAME_GEORGE,
	VFAME_ALTERNATE,
};

static const uint8_t ADDRESS_REORDERING[3][3][16] = {
	{
		{ 15, 14, 9, 1, 8, 10, 7, 3, 5, 11, 4, 0, 13, 12, 2, 6 },
		{ 15, 7, 13, 5, 11, 6, 0, 9, 12, 2, 10, 14, 3, 1, 8, 4 },
		{ 15, 0, 3, 12, 2, 4, 14, 13, 1, 8, 6, 7, 9, 5, 11, 10 },
	},
	{
		{ 15, 7, 13, 1, 11, 10, 14, 9, 12, 2, 4, 0, 3, 5, 8, 6 },
		{ 15, 14, 3, 12, 8, 4, 0, 13, 5, 11, 6, 7, 9, 1, 2, 10 },
		{ 15, 0, 9, 5, 2, 6, 7, 3, 1, 8, 10, 14, 13, 12, 11, 4 },
	},
	{
		{ 15, 0, 13, 5, 8, 4, 7, 3, 1, 2, 10, 14, 9, 12, 11, 6 },
		{ 15, 7, 9, 1, 2, 6, 14, 13, 12, 11, 4, 0, 3, 5, 8, 10 },
		{ 15, 14, 3, 12, 11, 10, 0, 9, 5, 8, 6, 7, 13, 1, 2, 4 },
	},
};

static const uint8_t VALUE_REORDERING[3][3][8] = {
	{
		{ 5, 4, 3, 2, 1, 0, 7, 6 },
		{ 3, 2, 1, 0, 7, 6, 5, 4 },
		{ 1, 0, 7, 6, 5, 4, 3, 2 },
	},
	{
		{ 3, 0, 7, 2, 1, 4, 5, 6 },
		{ 1, 4, 3, 0, 5, 6, 7, 2 },
		{ 5, 2, 1, 6, 7, 0, 3, 4 },
	},
	{
		{ 5, 4, 7, 2, 1, 0, 3, 6 },
		{ 1, 2, 3, 0, 5, 6, 7, 4 },
		{ 3, 0, 1, 6, 7, 4, 5, 2 },
	},
};

static const uint8_t MODE_CHANGE_START[5] = { 0x99, 0x02, 0x05, 0x02, 0x03 };
static const uint8_t MODE_CHANGE_END[5] = { 0x99, 0x03, 0x62, 0x02, 0x56 };
// Part of the start-up routine these games copy to RAM, at 0x15C in all but one of them.
static const uint8_t INIT_SEQUENCE[16] = { 0xB4, 0x00, 0x9F, 0xE5, 0x99, 0x10, 0xA0, 0xE3, 0x00, 0x10, 0xC0, 0xE5, 0xAC, 0x00, 0x9F, 0xE5 };

static uint32_t crc32(const uint8_t* data, size_t size) {
	uint32_t crc = ~0u;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
		}
	}
	return ~crc;
}

static int vfameDetect(const uint8_t* rom, size_t size) {
	// Dumps of these games from reprints with the protection removed are 32 MiB, which no real one
	// is.
	if (size == SIZE_ROM || size < 0x200) {
		return 0;
	}
	int type = 0;
	// The Lord of the Rings game uses another engine and has no such routine, so it's found by its
	// title.
	if (!memcmp(INIT_SEQUENCE, &rom[0x15C], sizeof(INIT_SEQUENCE)) || !memcmp("\0LORD\0WORD\0\0AKIJ", &rom[0xA0], 16)) {
		type = VFAME_STANDARD;
	}
	// Two cartridges whose chips scramble differently.
	if (!memcmp("George Sango", &rom[0xA0], 12)) {
		type = VFAME_GEORGE;
	} else if (type && crc32(rom, size) == 0x793A328F) {
		// Digimon Sapphire in Chinese, whose header is the English one's.
		type = VFAME_ALTERNATE;
	}
	return type;
}

static uint32_t patternShift2(uint32_t address) {
	uint32_t value = (address & 0xFFFF) >> 2;
	value += (address & 3) == 2 ? 0x8000 : 0;
	value += (address & 0x10000) ? 0x4000 : 0;
	return value;
}

// Returns the 16-bit value at an address past the ROM: one of a set of transformations of the
// address, chosen by which 64 KiB of the 2 MiB pattern it falls in.
static uint32_t pattern(uint32_t address) {
	address &= 0x1FFFFF;
	uint32_t low = address & 0xFFFF;
	uint32_t half = (address >> 1) & 0xFFFF;
	uint32_t value = 0;
	switch (address >> 16) {
	case 0x00:
	case 0x01:
		value = half;
		break;
	case 0x02:
		value = low;
		break;
	case 0x03:
		value = low + 1;
		break;
	case 0x04:
		value = 0xFFFF - low;
		break;
	case 0x05:
		value = 0xFFFF - low - 1;
		break;
	case 0x06:
		value = low ^ 0xAAAA;
		break;
	case 0x07:
		value = (low ^ 0xAAAA) + 1;
		break;
	case 0x08:
		value = low ^ 0x5555;
		break;
	case 0x09:
		value = (low ^ 0x5555) - 1;
		break;
	case 0x0A:
	case 0x0B:
		value = patternShift2(address);
		break;
	case 0x0C:
	case 0x0D:
		value = 0xFFFF - patternShift2(address);
		break;
	case 0x0E:
	case 0x0F:
		value = patternShift2(address) ^ 0xAAAA;
		break;
	case 0x10:
	case 0x11:
		value = patternShift2(address) ^ 0x5555;
		break;
	case 0x12:
		value = 0xFFFF - (low >> 1);
		break;
	case 0x13:
		value = 0xFFFF - (low >> 1) - 0x8000;
		break;
	case 0x14:
	case 0x15:
		value = half ^ 0xAAAA;
		break;
	case 0x16:
	case 0x17:
		value = half ^ 0x5555;
		break;
	case 0x18:
	case 0x19:
		value = half ^ 0xF0F0;
		break;
	case 0x1A:
	case 0x1B:
		value = half ^ 0x0F0F;
		break;
	case 0x1C:
	case 0x1D:
		value = half ^ 0xFF00;
		break;
	case 0x1E:
	case 0x1F:
		value = half ^ 0x00FF;
		break;
	}
	return value & 0xFFFF;
}

// Returns what a read of size bytes past the end of a Vast Fame ROM gives.
uint32_t cartVfameRead(uint32_t address, int size) {
	switch (size) {
	case 1:
		return address & 1 ? pattern(address) & 0xFF : pattern(address) >> 8;
	case 2:
		return pattern(address);
	default:
		return (pattern(address) << 2) + pattern(address + 2);
	}
}

// Bit i of the result, counting from the top of length bits, is the bit of value that order[i]
// names.
static uint32_t reorder(uint32_t value, const uint8_t* order, int length) {
	uint32_t result = value;
	for (int i = 0; i < length; ++i) {
		uint32_t bit = (value >> order[i]) & 1;
		int place = length - 1 - i;
		result = (result & ~(1u << place)) | (bit << place);
	}
	return result;
}

// Handles a write to SRAM on a Vast Fame cartridge.
void cartVfameSramWrite(struct GBA* gba, uint32_t address, uint8_t value) {
	struct Cart* cart = &gba->cart;
	// Writes to FFF8-FFFC spell out the sequences that open and close mode changes.
	if (address >= 0xFFF8 && address <= 0xFFFC) {
		cart->vfameSequence[address - 0xFFF8] = value;
		if (address == 0xFFFC) {
			if (!memcmp(MODE_CHANGE_START, cart->vfameSequence, 5)) {
				cart->vfameChanging = true;
			}
			if (!memcmp(MODE_CHANGE_END, cart->vfameSequence, 5)) {
				cart->vfameChanging = false;
			}
		}
	}
	if (cart->vfameChanging) {
		if (address == 0xFFFE) {
			cart->vfameSramMode = value;
		} else if (address == 0xFFFD) {
			cart->vfameRomMode = value;
		}
	}
	// SRAM can't be written until a mode is set.
	if (cart->vfameSramMode < 0) {
		return;
	}
	int mode = cart->vfameSramMode;
	int type = cart->vfame - 1;
	if (mode & 3) {
		address = reorder(address, ADDRESS_REORDERING[type][(mode & 3) - 1], 16);
	}
	if ((mode & 0xF) >> 2) {
		value = reorder(value, VALUE_REORDERING[type][((mode & 0xF) >> 2) - 1], 8);
	}
	if (mode & 0x80) {
		value ^= 0xAA;
	}
	gba->savedata.data[address & (SIZE_SRAM - 1)] = value;
	gba->savedata.dirty = true;
}

// AGBPrint: games built for Nintendo's development cartridges write text to a buffer near the top
// of the cartridge space and call a routine there to send it. Prototypes that still do would run
// off into nothing on a plain cartridge. The text goes nowhere here, but the registers and the
// routine are there, so the game carries on.
enum {
	PRINT_TOP = 0x00FE0000,
	PRINT_STRUCT = 0x00FE20F8,
	PRINT_FLUSH = 0x00FE209C,
	PRINT_PROTECT = 0x00FE2FFE,
	PRINT_UNLOCKED = 0x20,
};

// Reads of the lock and the four words, where the cartridge has no ROM there. Everything written
// counts as already sent: get reads as put.
bool cartPrintRead(struct GBA* gba, uint32_t address, uint16_t* value) {
	struct Cart* cart = &gba->cart;
	uint32_t at = address & 0x00FFFFFE;
	if (at == PRINT_PROTECT) {
		*value = cart->printProtect;
		return true;
	}
	if ((at & 0x00FFFFF8) == PRINT_STRUCT) {
		int field = (at & 7) >> 1;
		*value = cart->printCtx[field == 2 ? 3 : field];
		return true;
	}
	return false;
}

// Writes to the lock, and, with it open, to the four words and the text buffer.
bool cartPrintWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	struct Cart* cart = &gba->cart;
	uint32_t at = address & 0x00FFFFFE;
	if (at == PRINT_PROTECT) {
		cart->printProtect = value;
		return true;
	}
	if (cart->printProtect != PRINT_UNLOCKED) {
		return false;
	}
	if ((at & 0x00FFFFF8) == PRINT_STRUCT) {
		cart->printCtx[(at & 7) >> 1] = value;
		return true;
	}
	return at >= 0x00FD0000 && at < PRINT_TOP;
}

// The routine a game calls to send the buffer, where the cartridge has no ROM; it just returns.
const uint8_t* cartPrintCode(struct GBA* gba, uint32_t address) {
	// bx lr, twice
	static const uint8_t code[4] = { 0x70, 0x47, 0x70, 0x47 };
	if (gba->cart.printProtect != PRINT_UNLOCKED || (address & 0x00FFFFFC) != PRINT_FLUSH) {
		return NULL;
	}
	return &code[address & 2];
}

enum {
	MULTI_BLOCK = 0x80000,
};

// A multi-game cartridge shows up to 32 MiB of its chip from a 512 KiB block onwards: bank * 64 +
// offset blocks in, size blocks long.
static void multiMap(struct GBA* gba) {
	struct Cart* cart = &gba->cart;
	size_t at = (size_t) (cart->multiBank * 0x40 + (cart->multiOffset & 0x3F)) * MULTI_BLOCK;
	size_t length = (size_t) cart->multiSize * MULTI_BLOCK;
	if (at >= cart->fileSize) {
		at = 0;
	}
	if (length > cart->fileSize - at) {
		length = cart->fileSize - at;
	}
	if (at == cart->multiAt && length == cart->multiLength) {
		return;
	}
	cart->multiAt = at;
	cart->multiLength = length;
	memcpy(gba->rom, &cart->file[at], length);
	memset(&gba->rom[length], 0, SIZE_ROM - length);
	gba->romSize = length;

	jitRomChanged(gba);
}

// Its registers are in the save memory's space, at 2 to 6: the bank in the high four bits of 2; the
// offset at 3, with bit 7 locking them all until power-off; and the size at 4, as 64 minus the
// number of blocks. Returns true for a write they took.
bool cartMultiWrite(struct GBA* gba, uint32_t address, uint8_t value) {
	struct Cart* cart = &gba->cart;
	if (!cart->multi || cart->multiLocked || address < 2 || address > 6) {
		return false;
	}
	switch (address) {
	case 2:
		cart->multiBank = value >> 4;
		break;
	case 3:
		cart->multiOffset = value;
		cart->multiLocked = value & 0x80;
		break;
	case 4:
		cart->multiSize = 0x40 - (value & 0x3F);
		break;
	}
	multiMap(gba);
	return true;
}

// Scans a newly loaded cartridge image, all of it, for the hardware it needs. The Matrix mapper is
// on images over 32 MiB whose game code starts with M; it needs the whole image kept.
bool cartLoad(struct GBA* gba, const uint8_t* data, size_t size) {
	struct Cart* cart = &gba->cart;
	cartFree(gba);
	memset(cart, 0, sizeof(*cart));
	if (size > SIZE_ROM && data[0xAC] == 'M') {
		cart->file = malloc(size);
		if (!cart->file) {
			return false;
		}
		memcpy(cart->file, data, size);
		cart->fileSize = size;
		cart->matrix = true;
	} else if (size > SIZE_ROM + 0xC0 && data[SIZE_ROM + 3] == 0xEA && data[SIZE_ROM + 0xB2] == 0x96) {
		// Another game's header 32 MiB in, found by its entry branch and fixed byte: several games
		// behind a bank register.
		cart->file = malloc(size);
		if (!cart->file) {
			return false;
		}
		memcpy(cart->file, data, size);
		cart->fileSize = size;
		cart->multi = true;
	}
	cart->vfame = vfameDetect(data, size);
	return true;
}

void cartFree(struct GBA* gba) {
	free(gba->cart.file);
	gba->cart.file = NULL;
	gba->cart.fileSize = 0;
}

// At power-on, the mapper shows the chip's first 4 KiB, then the 4 KiB from 0x200; Vast Fame has no
// mode.
void cartReset(struct GBA* gba) {
	struct Cart* cart = &gba->cart;
	cart->vfameSramMode = -1;
	cart->vfameRomMode = -1;
	cart->vfameChanging = false;
	memset(cart->vfameSequence, 0, sizeof(cart->vfameSequence));
	cart->printProtect = 0;
	memset(cart->printCtx, 0, sizeof(cart->printCtx));
	if (cart->multi) {
		// The first game, which is the menu.
		cart->multiLocked = false;
		cart->multiBank = 0;
		cart->multiOffset = 0;
		cart->multiSize = 0x40;
		multiMap(gba);
	}
	if (!cart->matrix) {
		return;
	}
	memset(cart->matrixMap, 0, sizeof(cart->matrixMap));
	cart->matrixCmd = 0;
	cart->matrixSize = 0x1000;
	cart->matrixPaddr = 0;
	cart->matrixVaddr = 0;
	matrixRemap(gba);
	cart->matrixPaddr = 0x200;
	cart->matrixVaddr = 0x1000;
	matrixRemap(gba);
}

// After a state is loaded, the window shows what the state's map says.
void cartRestore(struct GBA* gba) {
	struct Cart* cart = &gba->cart;
	if (cart->multi && cart->multiAt <= cart->fileSize && cart->multiLength <= cart->fileSize - cart->multiAt && cart->multiLength <= SIZE_ROM) {
		memcpy(gba->rom, &cart->file[cart->multiAt], cart->multiLength);
		memset(&gba->rom[cart->multiLength], 0, SIZE_ROM - cart->multiLength);
		jitRomChanged(gba);
	}
	if (!cart->matrix) {
		return;
	}
	for (int i = 0; i < MATRIX_PAGES; ++i) {
		if (cart->matrixMap[i] <= cart->fileSize - MATRIX_PAGE) {
			memcpy(&gba->rom[i * MATRIX_PAGE], &cart->file[cart->matrixMap[i]], MATRIX_PAGE);
		}
	}
	jitInvalidate(gba, 0x08000000, MATRIX_WINDOW);
}
