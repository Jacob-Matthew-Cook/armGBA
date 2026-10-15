/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// Cartridges whose save type or extra hardware can't be detected, from mGBA's override list.

struct Override {
	char id[5];
	enum SavedataType savetype;
	int hardware;
};

static const struct Override overrides[] = {
	{ "AWRE", SAVEDATA_FLASH512, HW_NONE },
	{ "AWRP", SAVEDATA_FLASH512, HW_NONE },
	{ "AW2E", SAVEDATA_FLASH512, HW_NONE },
	{ "AW2P", SAVEDATA_FLASH512, HW_NONE },
	{ "U3IJ", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "U3IE", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "U3IP", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "U32J", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "U32E", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "U32P", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "AC8J", SAVEDATA_EEPROM, HW_NONE },
	{ "AC8E", SAVEDATA_EEPROM, HW_NONE },
	{ "AC8P", SAVEDATA_EEPROM, HW_NONE },
	{ "BDKJ", SAVEDATA_EEPROM, HW_NONE },
	{ "ALGP", SAVEDATA_EEPROM, HW_NONE },
	{ "ALFJ", SAVEDATA_EEPROM, HW_NONE },
	{ "ALFE", SAVEDATA_EEPROM, HW_NONE },
	{ "ALFP", SAVEDATA_EEPROM, HW_NONE },
	{ "BDBE", SAVEDATA_EEPROM, HW_NONE },
	{ "BDBP", SAVEDATA_EEPROM, HW_NONE },
	{ "V49J", SAVEDATA_SRAM, HW_RUMBLE },
	{ "V49E", SAVEDATA_SRAM, HW_RUMBLE },
	{ "V49P", SAVEDATA_SRAM, HW_RUMBLE },
	{ "PEAJ", SAVEDATA_FLASH1M, HW_EREADER },
	{ "PSAJ", SAVEDATA_FLASH1M, HW_EREADER },
	{ "PSAE", SAVEDATA_FLASH1M, HW_EREADER },
	{ "AFXE", SAVEDATA_FLASH512, HW_NONE },
	{ "BFTJ", SAVEDATA_FLASH1M, HW_NONE },
	{ "2GBP", SAVEDATA_SRAM, HW_RUMBLE },
	{ "AI2E", SAVEDATA_FORCE_NONE, HW_NONE },
	{ "AI2P", SAVEDATA_FORCE_NONE, HW_NONE },
	{ "BGWJ", SAVEDATA_FLASH1M, HW_NONE },
	{ "AGFE", SAVEDATA_FLASH512, HW_NONE },
	{ "KHPJ", SAVEDATA_EEPROM, HW_TILT },
	{ "BLJJ", SAVEDATA_FLASH512, HW_RTC },
	{ "BLJK", SAVEDATA_FLASH512, HW_RTC },
	{ "BLVJ", SAVEDATA_FLASH512, HW_RTC },
	{ "AREE", SAVEDATA_SRAM, HW_NONE },
	{ "AZCE", SAVEDATA_SRAM, HW_NONE },
	{ "BSME", SAVEDATA_EEPROM, HW_NONE },
	{ "AXVJ", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVE", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVP", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVI", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVS", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVD", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXVF", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPJ", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPE", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPP", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPI", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPS", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPD", SAVEDATA_FLASH1M, HW_RTC },
	{ "AXPF", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPEJ", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPEE", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPEP", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPEI", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPES", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPED", SAVEDATA_FLASH1M, HW_RTC },
	{ "BPEF", SAVEDATA_FLASH1M, HW_RTC },
	{ "B24E", SAVEDATA_FLASH1M, HW_NONE },
	{ "B24P", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRJ", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRE", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRP", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRI", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRS", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRD", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPRF", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGJ", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGE", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGP", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGI", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGS", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGD", SAVEDATA_FLASH1M, HW_NONE },
	{ "BPGF", SAVEDATA_FLASH1M, HW_NONE },
	{ "BR4J", SAVEDATA_FLASH512, HW_RTC },
	{ "AR8E", SAVEDATA_EEPROM, HW_NONE },
	{ "AROP", SAVEDATA_EEPROM, HW_NONE },
	{ "BKAJ", SAVEDATA_FLASH1M, HW_RTC },
	{ "U33J", SAVEDATA_EEPROM, HW_RTC | HW_LIGHT_SENSOR },
	{ "ASLE", SAVEDATA_FORCE_NONE, HW_NONE },
	{ "ASLF", SAVEDATA_FORCE_NONE, HW_NONE },
	{ "AA2J", SAVEDATA_EEPROM, HW_NONE },
	{ "AA2E", SAVEDATA_EEPROM, HW_NONE },
	{ "AA2P", SAVEDATA_AUTODETECT, HW_NONE },
	{ "A3AJ", SAVEDATA_EEPROM, HW_NONE },
	{ "A3AE", SAVEDATA_EEPROM, HW_NONE },
	{ "A3AP", SAVEDATA_EEPROM, HW_NONE },
	{ "AX4J", SAVEDATA_FLASH1M, HW_NONE },
	{ "AX4E", SAVEDATA_FLASH1M, HW_NONE },
	{ "AX4P", SAVEDATA_FLASH1M, HW_NONE },
	{ "ALUE", SAVEDATA_EEPROM, HW_NONE },
	{ "ALUP", SAVEDATA_EEPROM, HW_NONE },
	{ "A2YE", SAVEDATA_FORCE_NONE, HW_NONE },
	{ "BUHJ", SAVEDATA_EEPROM, HW_NONE },
	{ "RZWJ", SAVEDATA_SRAM, HW_RUMBLE | HW_GYRO },
	{ "RZWE", SAVEDATA_SRAM, HW_RUMBLE | HW_GYRO },
	{ "RZWP", SAVEDATA_SRAM, HW_RUMBLE | HW_GYRO },
	{ "KYGJ", SAVEDATA_EEPROM, HW_TILT },
	{ "KYGE", SAVEDATA_EEPROM, HW_TILT },
	{ "KYGP", SAVEDATA_EEPROM, HW_TILT },
	{ "TCHK", SAVEDATA_EEPROM, HW_NONE },
};

static const uint32_t pokemonTable[] = {
	0x4881F3F8, 0x8C4D3108, 0x1F1C08FB, 0x34C9DF89, 0xA3FDCCB1, 0xA0AEC80A,
	0x1A81EEDF, 0x3B2056E9, 0x5DC668F6, 0x73A72167, 0x84EE4776, 0x9F08064E, 0xBB640DF7, 0xDD88761C,
	0x61641576, 0xAEAC73E6, 0xF0815EE7,
};

// CRC-32, for checking the header's logo and for the cartridges the list knows by checksum.
static uint32_t crc32(const uint8_t* data, size_t size) {
	uint32_t crc = ~0u;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int b = 0; b < 8; ++b) {
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
		}
	}
	return ~crc;
}

// The Nintendo logo in the header, which the BIOS checks before it starts a cartridge.
bool overridesLogoValid(struct GBA* gba) {
	return gba->rom && gba->romSize >= 0xA0 && crc32(&gba->rom[4], 0x9C) == 0xD0BEB55E;
}

// The save chip, as named by the save library the game was built with. EEPROM is left to its first
// access, which also tells its size.
static void detectSaveLibrary(struct GBA* gba) {
	static const struct {
		const char* name;
		enum SavedataType type;
	} libraries[] = {
		{ "EEPROM_V", SAVEDATA_AUTODETECT },
		{ "SRAM_V", SAVEDATA_SRAM },
		{ "SRAM_F_V", SAVEDATA_SRAM },
		{ "FLASH_V", SAVEDATA_FLASH512 },
		{ "FLASH512_V", SAVEDATA_FLASH512 },
		{ "FLASH1M_V", SAVEDATA_FLASH1M },
	};
	const char* rom = (const char*) gba->rom;
	for (uint32_t i = 0; i + 12 <= gba->romSize; i += 4) {
		if (rom[i] != 'E' && rom[i] != 'S' && rom[i] != 'F') {
			continue;
		}
		for (size_t k = 0; k < sizeof(libraries) / sizeof(*libraries); ++k) {
			if (!memcmp(&rom[i], libraries[k].name, strlen(libraries[k].name))) {
				if (libraries[k].type != SAVEDATA_AUTODETECT) {
					savedataForceType(gba, libraries[k].type);
				}
				return;
			}
		}
	}
}

// Returns the hardware flags to emulate.
int overridesApply(struct GBA* gba) {
	const char* rom = (const char*) gba->rom;
	if (!rom || gba->romSize < 0xC0) {
		return HW_NONE;
	}
	if (gba->cart.multi) {
		// Its menu's header is another game's; the cartridge itself has plain save memory.
		savedataForceType(gba, SAVEDATA_SRAM);
		return HW_NONE;
	}
	bool isPokemon = !strncmp("pokemon red version", &rom[0x108], 20) || !strncmp("pokemon emerald version", &rom[0x108], 24) || !strncmp("AXVE", &rom[0xAC], 4);
	if (isPokemon) {
		uint32_t crc = crc32(gba->rom, gba->romSize);
		bool known = false;
		for (size_t i = 0; i < sizeof(pokemonTable) / sizeof(*pokemonTable); ++i) {
			known = known || crc == pokemonTable[i];
		}
		if (!known) {
			// ROM hacks of these get Flash 1M, the clock, and VBA's pin behavior (bit 16).
			savedataForceType(gba, SAVEDATA_FLASH1M);
			return HW_RTC | 0x10000;
		}
	}
	for (size_t i = 0; i < sizeof(overrides) / sizeof(*overrides); ++i) {
		if (!memcmp(overrides[i].id, &rom[0xAC], 4)) {
			if (overrides[i].savetype != SAVEDATA_AUTODETECT) {
				savedataForceType(gba, overrides[i].savetype);
			} else {
				detectSaveLibrary(gba);
			}
			return overrides[i].hardware;
		}
	}
	if (rom[0xAC] == 'F') {
		// Classic NES Series.
		savedataForceType(gba, SAVEDATA_EEPROM);
	} else {
		detectSaveLibrary(gba);
	}
	return HW_NONE;
}
