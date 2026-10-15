/* Copyright (c) 2013-2015 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// Cartridge save memory: SRAM, Flash and EEPROM, detected on first use like mGBA does

enum {
	// How long an EEPROM reads busy after a write, in CPU cycles: about 7 ms
	EEPROM_SETTLE_CYCLES = 115000,
	// And Flash after a sector is erased, as mGBA has it: 1.8 ms. A programmed byte reads back at
	// once, as jsmolka's test has it on hardware; mGBA keeps the chip busy 39 us there.
	FLASH_ERASE_CYCLES = 30000,
};

enum {
	// The EEPROM's serial protocol: two command bits, an address, then 64 data bits either way. The
	// PENDING states are in the middle of the command or the address.
	EEPROM_COMMAND_NULL = 0,
	EEPROM_COMMAND_PENDING = 1,
	EEPROM_COMMAND_WRITE = 2,
	EEPROM_COMMAND_READ_PENDING = 3,
	EEPROM_COMMAND_READ = 4,
};

enum {
	// Flash command bytes. A command is AA to 5555, 55 to 2AAA, then the command byte to 5555.
	FLASH_COMMAND_START = 0xAA,
	FLASH_COMMAND_CONTINUE = 0x55,
	FLASH_COMMAND_ERASE_CHIP = 0x10,
	FLASH_COMMAND_ERASE_SECTOR = 0x30,
	FLASH_COMMAND_NONE = 0,
	FLASH_COMMAND_ERASE = 0x80,
	FLASH_COMMAND_ID = 0x90,
	FLASH_COMMAND_PROGRAM = 0xA0,
	FLASH_COMMAND_SWITCH_BANK = 0xB0,
	FLASH_COMMAND_TERMINATE = 0xF0,
};

enum {
	// How far into that three-write sequence the chip is
	FLASH_STATE_RAW = 0,
	FLASH_STATE_START = 1,
	FLASH_STATE_CONTINUE = 2,
};

enum {
	// The chip IDs games look for to tell a 64 KiB chip from a 128 KiB one, and the two command addresses
	FLASH_PANASONIC_MN63F805MNP = 0x1B32,
	FLASH_SANYO_LE26FV10N1TS = 0x1362,
	FLASH_BASE_HI = 0x5555,
	FLASH_BASE_LO = 0x2AAA,
};

static void settled(struct GBA* gba, void* context, uint32_t late) {
	(void) gba;
	(void) context;
	(void) late;
}

void savedataInit(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	savedata->type = SAVEDATA_AUTODETECT;
	savedata->data = NULL;
	savedata->command = EEPROM_COMMAND_NULL;
	savedata->flashState = FLASH_STATE_RAW;
	savedata->dust.callback = settled;
	savedata->dust.context = NULL;
	savedata->dust.priority = 0x70;
	memset(savedata->storage, 0xFF, sizeof(savedata->storage));
}

// A reset leaves the chip's contents and type alone and drops any command in progress
void savedataReset(struct GBA* gba) {
	gba->savedata.command = EEPROM_COMMAND_NULL;
	gba->savedata.flashState = FLASH_STATE_RAW;
}

// The save's size in bytes for the detected type; 0 until one is detected
size_t savedataSize(struct GBA* gba) {
	switch (gba->savedata.type) {
	case SAVEDATA_SRAM:
		return SIZE_SRAM;
	case SAVEDATA_SRAM512:
		return SIZE_SRAM512;
	case SAVEDATA_FLASH512:
		return SIZE_FLASH512;
	case SAVEDATA_FLASH1M:
		return SIZE_FLASH1M;
	case SAVEDATA_EEPROM:
		return SIZE_EEPROM;
	case SAVEDATA_EEPROM512:
		return SIZE_EEPROM512;
	default:
		return 0;
	}
}

// The first Flash access: the small chip until a game switches bank
void savedataInitFlash(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->type == SAVEDATA_AUTODETECT) {
		savedata->type = SAVEDATA_FLASH512;
	}
	if (savedata->type != SAVEDATA_FLASH512 && savedata->type != SAVEDATA_FLASH1M) {
		return;
	}
	savedata->data = savedata->storage;
	savedata->currentBank = savedata->data;
}

// The first EEPROM access: the small one until a game addresses past its end
void savedataInitEeprom(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->type == SAVEDATA_AUTODETECT) {
		savedata->type = SAVEDATA_EEPROM512;
	} else if (savedata->type != SAVEDATA_EEPROM512 && savedata->type != SAVEDATA_EEPROM) {
		return;
	}
	savedata->data = savedata->storage;
	// Without a save file, a fresh EEPROM reads erased for the first 512 bytes and zero past them, as in mGBA
	if (!savedata->backed) {
		memset(savedata->storage, 0, SIZE_EEPROM);
		memset(savedata->storage, 0xFF, SIZE_EEPROM512);
	}
}

// The first SRAM access
void savedataInitSram(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->type != SAVEDATA_AUTODETECT) {
		return;
	}
	savedata->type = SAVEDATA_SRAM;
	savedata->data = savedata->storage;
}

// For cartridges whose type the override list gives: set it up as that type from the start
void savedataForceType(struct GBA* gba, enum SavedataType type) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->type == type) {
		return;
	}
	savedata->type = SAVEDATA_AUTODETECT;
	switch (type) {
	case SAVEDATA_FLASH512:
	case SAVEDATA_FLASH1M:
		savedata->type = type;
		savedataInitFlash(gba);
		return;
	case SAVEDATA_EEPROM:
	case SAVEDATA_EEPROM512:
		savedata->type = type;
		savedataInitEeprom(gba);
		return;
	case SAVEDATA_SRAM:
		savedataInitSram(gba);
		return;
	case SAVEDATA_SRAM512:
		savedata->type = SAVEDATA_SRAM512;
		savedata->data = savedata->storage;
		return;
	case SAVEDATA_FORCE_NONE:
		savedata->type = SAVEDATA_FORCE_NONE;
		return;
	case SAVEDATA_AUTODETECT:
		return;
	}
}

// A byte from the current bank, or the chip's ID while a game has asked for it
uint8_t savedataReadFlash(struct GBA* gba, uint16_t address) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->command == FLASH_COMMAND_ID) {
		if (savedata->type == SAVEDATA_FLASH512 && address < 2) {
			return FLASH_PANASONIC_MN63F805MNP >> (address * 8);
		}
		if (savedata->type == SAVEDATA_FLASH1M && address < 2) {
			return FLASH_SANYO_LE26FV10N1TS >> (address * 8);
		}
	}
	// While the chip is busy the sector last erased reads as its status. Like mGBA, only the bit games
	// poll: the top one, the opposite of the data's.
	if (timingIsScheduled(gba, &savedata->dust) && (address >> 12) == savedata->settling) {
		return (savedata->currentBank[address] ^ 0x80) & 0x80;
	}
	return savedata->currentBank[address];
}

// Only the 128 KiB chip has a second bank, so asking for it shows which chip the game expects
static void flashSwitchBank(struct GBA* gba, int bank) {
	struct Savedata* savedata = &gba->savedata;
	if (bank > 0 && savedata->type == SAVEDATA_FLASH512) {
		savedata->type = SAVEDATA_FLASH1M;
	}
	savedata->currentBank = &savedata->data[bank << 16];
}

// Erased Flash reads FF. A whole-chip erase finishes at once.
static void flashErase(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	savedata->dirty = true;
	memset(savedata->data, 0xFF, savedata->type == SAVEDATA_FLASH1M ? SIZE_FLASH1M : SIZE_FLASH512);
}

// One 4 KiB sector of the current bank
static void flashEraseSector(struct GBA* gba, uint16_t sectorStart) {
	struct Savedata* savedata = &gba->savedata;
	savedata->dirty = true;
	savedata->settling = sectorStart >> 12;
	timingDeschedule(gba, &savedata->dust);
	timingSchedule(gba, &savedata->dust, FLASH_ERASE_CYCLES);
	memset(&savedata->currentBank[sectorStart & ~0xFFF], 0xFF, 0x1000);
}

// A write to the Flash chip: a step of the command sequence, or the byte a command was waiting for
void savedataWriteFlash(struct GBA* gba, uint16_t address, uint8_t value) {
	struct Savedata* savedata = &gba->savedata;
	switch (savedata->flashState) {
	case FLASH_STATE_RAW:
		switch (savedata->command) {
		// The byte to program, or the bank to switch to, follows its command directly
		case FLASH_COMMAND_PROGRAM:
			savedata->dirty = true;
			savedata->currentBank[address] = value;
			savedata->command = FLASH_COMMAND_NONE;
			break;
		case FLASH_COMMAND_SWITCH_BANK:
			if (address == 0 && value < 2) {
				flashSwitchBank(gba, value);
			}
			savedata->command = FLASH_COMMAND_NONE;
			break;
		default:
			if (address == FLASH_BASE_HI && value == FLASH_COMMAND_START) {
				savedata->flashState = FLASH_STATE_START;
			}
			break;
		}
		break;
	case FLASH_STATE_START:
		if (address == FLASH_BASE_LO && value == FLASH_COMMAND_CONTINUE) {
			savedata->flashState = FLASH_STATE_CONTINUE;
		} else {
			savedata->flashState = FLASH_STATE_RAW;
		}
		break;
	// The third write: a new command, or the second half of an erase
	case FLASH_STATE_CONTINUE:
		savedata->flashState = FLASH_STATE_RAW;
		if (address == FLASH_BASE_HI) {
			switch (savedata->command) {
			case FLASH_COMMAND_NONE:
				switch (value) {
				case FLASH_COMMAND_ERASE:
				case FLASH_COMMAND_ID:
				case FLASH_COMMAND_PROGRAM:
				case FLASH_COMMAND_SWITCH_BANK:
					savedata->command = value;
					break;
				}
				break;
			case FLASH_COMMAND_ERASE:
				if (value == FLASH_COMMAND_ERASE_CHIP) {
					flashErase(gba);
				}
				savedata->command = FLASH_COMMAND_NONE;
				break;
			case FLASH_COMMAND_ID:
				if (value == FLASH_COMMAND_TERMINATE) {
					savedata->command = FLASH_COMMAND_NONE;
				}
				break;
			default:
				savedata->command = FLASH_COMMAND_NONE;
				break;
			}
		} else if (savedata->command == FLASH_COMMAND_ERASE && value == FLASH_COMMAND_ERASE_SECTOR) {
			flashEraseSector(gba, address);
			savedata->command = FLASH_COMMAND_NONE;
		}
		break;
	}
}

// An address past the small EEPROM's 512 bytes means the 8 KiB one
static void ensureEeprom(struct GBA* gba, uint32_t size) {
	if (size < SIZE_EEPROM512 || gba->savedata.type == SAVEDATA_EEPROM) {
		return;
	}
	gba->savedata.type = SAVEDATA_EEPROM;
}

// One bit to the EEPROM, sent by DMA; writeSize is how many of the transfer's words are left, which is
// how the end of the address and the stop bit are told apart from data
void savedataWriteEeprom(struct GBA* gba, uint16_t value, uint32_t writeSize) {
	struct Savedata* savedata = &gba->savedata;
	switch (savedata->command) {
	case EEPROM_COMMAND_NULL:
	default:
		savedata->command = value & 0x1;
		break;
	case EEPROM_COMMAND_PENDING:
		savedata->command <<= 1;
		savedata->command |= value & 0x1;
		if (savedata->command == EEPROM_COMMAND_WRITE) {
			savedata->writeAddress = 0;
		} else {
			savedata->readAddress = 0;
		}
		break;
	case EEPROM_COMMAND_WRITE:
		// Address bits come before the 64 data bits and the stop bit; they end up as a bit offset
		if (writeSize > 65) {
			savedata->writeAddress <<= 1;
			savedata->writeAddress |= (value & 0x1) << 6;
		} else if (writeSize == 1) {
			savedata->command = EEPROM_COMMAND_NULL;
		} else if ((savedata->writeAddress >> 3) < SIZE_EEPROM) {
			ensureEeprom(gba, savedata->writeAddress >> 3);
			uint8_t current = savedata->data[savedata->writeAddress >> 3];
			current &= ~(1 << (0x7 - (savedata->writeAddress & 0x7)));
			current |= (value & 0x1) << (0x7 - (savedata->writeAddress & 0x7));
			savedata->dirty = true;
			savedata->data[savedata->writeAddress >> 3] = current;
			timingDeschedule(gba, &savedata->dust);
			timingSchedule(gba, &savedata->dust, EEPROM_SETTLE_CYCLES);
			++savedata->writeAddress;
		}
		break;
	case EEPROM_COMMAND_READ_PENDING:
		if (writeSize > 1) {
			savedata->readAddress <<= 1;
			if (value & 0x1) {
				savedata->readAddress |= 0x40;
			}
		} else {
			savedata->readBitsRemaining = 68;
			savedata->command = EEPROM_COMMAND_READ;
		}
		break;
	}
}

// One bit from the EEPROM. Outside a read it is the ready line: low while a write settles. A read is
// four dummy bits and then the 64 bits of the block addressed.
uint16_t savedataReadEeprom(struct GBA* gba) {
	struct Savedata* savedata = &gba->savedata;
	if (savedata->command != EEPROM_COMMAND_READ) {
		return timingIsScheduled(gba, &savedata->dust) ? 0 : 1;
	}
	--savedata->readBitsRemaining;
	if (savedata->readBitsRemaining < 64) {
		int step = 63 - savedata->readBitsRemaining;
		uint32_t address = (savedata->readAddress + step) >> 3;
		ensureEeprom(gba, address);
		if (address >= SIZE_EEPROM) {
			return 0xFF;
		}
		uint8_t data = savedata->data[address] >> (0x7 - (step & 0x7));
		if (!savedata->readBitsRemaining) {
			savedata->command = EEPROM_COMMAND_NULL;
		}
		return data & 0x1;
	}
	return 0;
}
