/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core's Game Boy and Game Boy Color side, running on Gambatte-Speedrun's core
// (gambatte/, GPL-2.0).

extern "C" {
#include "gb.h"
#include "libretro.h"
}
#include "link.h"
#include "sameboy-boot.h"

#include "gambatte.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace gambatte {
// MBC7's tilt sensor, as an offset from lying flat, and its flick (defined in mbc7.cpp).
extern int mbc7TiltX;
extern int mbc7TiltY;
extern bool mbc7Flick;
unsigned long crc32(unsigned long crc, void const* buf, std::size_t size);
}

namespace {

enum {
	// Gambatte produces sound at 2^21 Hz, 35112 samples per frame, and may overrun by up to 2064.
	NATIVE_RATE = 2097152,
	FRAME_SAMPLES = 35112,
	OVERRUN = 2064,
	// Extra room in the state size reported to the frontend: for the state to grow, and for the
	// extra work RAM a Game Boy Color has if the game restarts as one.
	STATE_SLACK = 4096 + 6 * 4096,
};

gambatte::GB* gb;
// Whether Gambatte's serial port goes to another Game Boy over the network.
bool linked;
void linkClocked();
unsigned loadFlags;
unsigned pressed;
gambatte::uint_least32_t pixels[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
gambatte::uint_least32_t native[FRAME_SAMPLES + OVERRUN];
// A frame's sound for the frontend: at most two frames' worth of native samples, two channels each.
int16_t samples[(FRAME_SAMPLES + OVERRUN) * 2 / 16 * 2 + 8];
size_t sampleCount;
// How many native samples make one frontend sample, and the running sums towards the next one.
unsigned ratio;
long sumLeft;
long sumRight;
unsigned summed;
size_t stateSize;
unsigned cgbColors[32768];
// The game and the boot ROM folder, kept so the game can be restarted as another model.
std::vector<unsigned char> game;
std::string systemPath;
bool hasSystemPath;
LibretroGbModel chosen;
// The model the Game Boy Model option asks for. Loading a state from another model may have
// switched the machine away from it.
LibretroGbModel wanted;
// The boot ROM choice from the option, and the one this game actually started with.
LibretroGbBoot bootWanted = LIBRETRO_GB_BOOT_NINTENDO;
LibretroGbBoot bootUsed;
// The cheat codes in force, in Gambatte's format. Loading the game again clears them, so they're
// reapplied.
std::string gameGenie;
std::string gameShark;

// An MBC3 clock, kept by the frontend as the .rtc file. The first 8 bytes are the Unix time the
// clock would have started from, as gambatte-libretro stores it, so either core reads the other's
// file. Then come the registers and when they were saved, which carry more detail (in rtc.h's
// order). A HuC3 chip's file stores the chip's whole state in place of the registers.
enum {
	RTC_BASE = 0,
	RTC_MAGIC = 8,
	RTC_SAVED = 12,
	RTC_REGS = 20,
	RTC_SIZE = RTC_REGS + 14,
	HUC3_STATE = 0x104,
	HUC3_SIZE = RTC_REGS + HUC3_STATE,
	// In the HuC3 state: the cycles into the current minute, then I/O with the minutes and days as
	// 4-bit fields.
	HUC3_MINUTES = 4 + 0x10,
	HUC3_DAYS = 4 + 0x13,
	// A host clock before 2024 was never set, so it says nothing about the time the game was off.
	SANE_TIME = 1704067200,
};
// The registers in the order Gambatte keeps them (mem/rtc.h), and the length of a second in rtc_c's
// cycles.
enum { RTC_DH, RTC_DL, RTC_H, RTC_M, RTC_S, RTC_C, RTC_LATCHED };
unsigned long const RTC_SECOND = 0x400000;
unsigned char rtcFile[HUC3_SIZE];
// The frontend fills rtcFile after the game is loaded, so the file is read just before the first
// frame.
bool rtcPending;

void put64(unsigned char* to, unsigned long long value) {
	for (int i = 0; i < 8; ++i) {
		to[i] = value >> i * 8;
	}
}

unsigned long long get64(const unsigned char* from) {
	unsigned long long value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | from[i];
	}
	return value;
}

// Advances a clock by some seconds the way the chip counts: a field past its range counts up to its
// last value and wraps to 0 without a carry; past 511 days, the day counter wraps and sets its
// carry bit.
void runClock(unsigned long r[11], unsigned long long seconds) {
	if (r[RTC_DH] & 0x40) {
		return;
	}
	static const unsigned limits[3] = { 60, 60, 24 };
	static const unsigned ends[3] = { 64, 64, 32 };
	static const int fields[3] = { RTC_S, RTC_M, RTC_H };
	unsigned long long carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		unsigned long& value = r[fields[i]];
		if (value >= limits[i]) {
			if (carry < ends[i] - value) {
				value += carry;
				return;
			}
			carry -= ends[i] - value;
			value = 0;
		}
		unsigned long long const total = value + carry;
		value = total % limits[i];
		carry = total / limits[i];
	}
	unsigned long long days = ((r[RTC_DH] & 1) << 8 | r[RTC_DL]) + carry;
	if (days > 511) {
		r[RTC_DH] |= 0x80;
	}
	r[RTC_DL] = days & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (days >> 8 & 1);
}

// How much of rtcFile the cartridge's clock uses, or 0 if it has none.
size_t rtcSize() {
	return !gb ? 0 : gb->hasMbc3Rtc() ? RTC_SIZE : gb->hasHuC3() ? HUC3_SIZE : 0;
}

// Reads a 12-bit count stored as three 4-bit fields, lowest first.
unsigned long getNibbles(const unsigned char* at) {
	return (at[0] & 0xF) | (at[1] & 0xF) << 4 | (at[2] & 0xF) << 8;
}

void putNibbles(unsigned char* at, unsigned long value) {
	for (int i = 0; i < 3; ++i) {
		at[i] = value >> i * 4 & 0xF;
	}
}

// Advances a HuC3 clock by some seconds the way the chip counts: minutes up to 1440, then days up
// to 4096.
void runHuC3(unsigned char* state, unsigned long long seconds) {
	unsigned long long const minute = RTC_SECOND * 60;
	unsigned long long cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
	cycles = cycles % minute + seconds % 60 * RTC_SECOND;
	unsigned long long minutes = getNibbles(state + HUC3_MINUTES) + seconds / 60 + cycles / minute;
	cycles %= minute;
	unsigned long long const days = getNibbles(state + HUC3_DAYS) + minutes / 1440;
	putNibbles(state + HUC3_MINUTES, minutes % 1440);
	putNibbles(state + HUC3_DAYS, days & 0xFFF);
	for (int i = 0; i < 4; ++i) {
		state[i] = cycles >> (24 - i * 8);
	}
}

// Loads the clock from the .rtc file and advances it by the time the game was off, when the host
// clock can tell.
void readRtcFile() {
	rtcPending = false;
	unsigned long long const now = std::time(0);
	if (gb->hasHuC3()) {
		if (std::memcmp(rtcFile + RTC_MAGIC, "aGBh", 4)) {
			return;
		}
		unsigned char state[HUC3_STATE];
		std::memcpy(state, rtcFile + RTC_REGS, sizeof(state));
		unsigned long long const saved = get64(rtcFile + RTC_SAVED);
		if (saved >= SANE_TIME && now >= saved) {
			runHuC3(state, now - saved);
		}
		gb->setHuC3Regs(state);
		return;
	}
	unsigned long r[11] = { 0 };
	if (!std::memcmp(rtcFile + RTC_MAGIC, "aGBc", 4)) {
		const unsigned char* const regs = rtcFile + RTC_REGS;
		r[RTC_DH] = regs[0] & 0xC1;
		r[RTC_DL] = regs[1];
		r[RTC_H] = regs[2] & 0x1F;
		r[RTC_M] = regs[3] & 0x3F;
		r[RTC_S] = regs[4] & 0x3F;
		r[RTC_C] = (regs[5] | regs[6] << 8 | regs[7] << 16 | (unsigned long) regs[8] << 24) % RTC_SECOND;
		r[RTC_DH + RTC_LATCHED] = regs[9] & 0xC1;
		r[RTC_DL + RTC_LATCHED] = regs[10];
		r[RTC_H + RTC_LATCHED] = regs[11] & 0x1F;
		r[RTC_M + RTC_LATCHED] = regs[12] & 0x3F;
		r[RTC_S + RTC_LATCHED] = regs[13] & 0x3F;
		unsigned long long const saved = get64(rtcFile + RTC_SAVED);
		if (saved >= SANE_TIME && now >= saved) {
			runClock(r, now - saved);
		}
	} else {
		// gambatte-libretro's format: the clock shows the time since its start, unless that start
		// is in the future.
		unsigned long long const base = get64(rtcFile + RTC_BASE);
		if (!base || base < SANE_TIME / 2 || now < SANE_TIME || base > now) {
			return;
		}
		runClock(r, now - base);
		for (int i = 0; i < 5; ++i) {
			r[i + RTC_LATCHED] = r[i];
		}
	}
	gb->setRtcRegs(r);
}

// Writes the clock's current state into the .rtc file.
void writeRtcFile() {
	if (gb->hasHuC3()) {
		unsigned long long const now = std::time(0);
		put64(rtcFile + RTC_BASE, now);
		std::memcpy(rtcFile + RTC_MAGIC, "aGBh", 4);
		put64(rtcFile + RTC_SAVED, now);
		gb->getHuC3Regs(rtcFile + RTC_REGS);
		return;
	}
	unsigned long r[11];
	gb->getRtcRegs(r);
	unsigned long long const now = std::time(0);
	unsigned long long const shown = (((((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 60
	                                  + r[RTC_M]) * 60 + r[RTC_S]);
	put64(rtcFile + RTC_BASE, now - shown);
	std::memcpy(rtcFile + RTC_MAGIC, "aGBc", 4);
	put64(rtcFile + RTC_SAVED, now);
	unsigned char* const regs = rtcFile + RTC_REGS;
	for (int i = 0; i < 5; ++i) {
		regs[i] = r[i];
		regs[9 + i] = r[i + RTC_LATCHED];
	}
	for (int i = 0; i < 4; ++i) {
		regs[5 + i] = r[RTC_C] >> i * 8;
	}
}

// Gambatte asks for the buttons through a callback.
unsigned input(void*) {
	return pressed;
}

// Reads the first 2304 bytes of a boot ROM file (the size of the largest boot ROM), or nothing if
// there is no file.
std::vector<unsigned char> readBoot(std::string const& path) {
	std::vector<unsigned char> bytes(2305);
	FILE* const f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return std::vector<unsigned char>();
	}
	bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f));
	std::fclose(f);
	return bytes;
}

// Whether a boot ROM is Nintendo's: each of Nintendo's holds the logo it checks cartridges against,
// while SameBoy's draw it from the cartridge.
bool nintendoBoot(std::vector<unsigned char> const& bytes) {
	static const unsigned char logo[48] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
		0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
		0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
	};
	return std::search(bytes.begin(), bytes.end(), logo, logo + sizeof(logo)) != bytes.end();
}

// Finds the boot ROM in the system directory for the model, or none. Nintendo's is looked for under
// every name ROCKNIX's BIOS check, Gambatte, gbdev's archive, SameBoy and Gearboy use for it;
// failing that, a SameBoy boot ROM under one of those names. Gambatte can convert Nintendo's Game
// Boy Color boot ROM into the Game Boy Advance's, but not SameBoy's.
std::string bootRom(const char* systemDirectory, LibretroGbModel model) {
	static const char* const names[4][5] = {
		{ "gb_bios.bin", "dmg_boot.bin" },
		{ "gbc_bios.bin", "cgb_boot.bin" },
		// The later Game Boy Advances', the first ones', the Game Boy Color's, then SameBoy's name.
		{ "cgb_agb_boot.bin", "cgb_agb0_boot.bin", "gbc_bios.bin", "cgb_boot.bin", "agb_boot.bin" },
		// The Game Boy's boot ROM would leave the wrong value in A for a Pocket, so without the
		// Pocket's there is none.
		{ "mgb_boot.bin" },
	};
	std::string other;
	for (int i = 0; systemDirectory && i < 5 && names[model - 1][i]; ++i) {
		std::string const path = std::string(systemDirectory) + "/" + names[model - 1][i];
		std::vector<unsigned char> const bytes = readBoot(path);
		if (bytes.empty()) {
			continue;
		}
		bool const converted = model == LIBRETRO_GB_AGB && (i == 2 || i == 3);
		if (converted ? bytes.size() == 2304 && gambatte::crc32(0, &bytes[0], bytes.size()) == 0x41884E46
		              : nintendoBoot(bytes)) {
			return path;
		}
		if (!converted && other.empty()) {
			other = path;
		}
	}
	return other;
}

// Returns SameBoy's boot ROM for the model. The Pocket's is the Game Boy's with 0xFF left in A:
// that byte is the high byte of its "ld hl, $01B0" before "push hl; pop af".
std::vector<unsigned char> sameboyBoot(LibretroGbModel model) {
	switch (model) {
	case LIBRETRO_GB_CGB:
		return std::vector<unsigned char>(sameboy_cgb_boot, sameboy_cgb_boot + sizeof(sameboy_cgb_boot));
	case LIBRETRO_GB_AGB:
		return std::vector<unsigned char>(sameboy_agb_boot, sameboy_agb_boot + sizeof(sameboy_agb_boot));
	default: {
		std::vector<unsigned char> boot(sameboy_dmg_boot, sameboy_dmg_boot + sizeof(sameboy_dmg_boot));
		if (model == LIBRETRO_GB_MGB) {
			boot[0x93] = 0xFF;
		}
		return boot;
	}
	}
}

// How long the Game Boy Advance takes before handing over to the Game Boy Color hardware inside it.
size_t stall() {
	return (loadFlags & gambatte::GB::GBA_FLAG) && !(loadFlags & gambatte::GB::NO_BIOS) ? 485808 : 0;
}

// The original Game Boy screen chosen: 0 plain grays, 1 the Pocket's, 2 the first model's green, 3
// the Light's with its backlight on.
int screen;
bool correction;
// Whether a screen the game has turned off shows its background color, instead of white or the
// screen's own off shade.
bool offBackground;
// What the color table was last built for: 0 uncorrected, 1 a Game Boy Color's screen, 2 a Game Boy
// Advance's.
int built = -1;

// Builds the Game Boy Color's colors as its screen shows them, as SameBoy does (Lior Halphon, MIT
// license; its "modern balanced" correction): each channel's response curve, then green takes in
// some blue. The Game Boy Advance's screen has its own curves and mixes less.
void correctedColors(bool advance) {
	static const unsigned char cgbCurve[32] = { 0, 6, 12, 20, 28, 36, 45, 56, 66, 76, 88, 100, 113, 125, 137, 149,
	                                            161, 172, 182, 192, 202, 210, 218, 225, 232, 238, 243, 247, 250, 252, 254, 255 };
	static const unsigned char agbCurve[32] = { 0, 3, 8, 14, 20, 26, 33, 40, 47, 54, 62, 70, 78, 86, 94, 103,
	                                            112, 120, 129, 138, 147, 157, 166, 176, 185, 195, 205, 215, 225, 235, 245, 255 };
	const unsigned char* const curve = advance ? agbCurve : cgbCurve;
	double linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = std::pow(curve[i] / 255.0, 1.6);
	}
	double const greenShare = advance ? 5 / 6.0 : 3 / 4.0;
	for (unsigned i = 0; i < 32768; ++i) {
		unsigned const r = curve[i & 0x1F];
		unsigned const g = i >> 5 & 0x1F;
		unsigned const b = i >> 10 & 0x1F;
		unsigned const mixed = std::lround(std::pow(linear[g] * greenShare + linear[b] * (1 - greenShare), 1 / 1.6) * 255);
		cgbColors[i] = 0xFF000000 | r << 16 | mixed << 8 | curve[b];
	}
}

// Installs the colors: the chosen screen's four shades for original Game Boy games, and the table
// that turns Game Boy Color 15-bit colors into the frontend's, corrected or not.
void setColors() {
	// The plain grays are those gambatte-libretro shows Game Boy games in; the Pocket's, the first
	// model's and the Light's are SameBoy's.
	static const unsigned long shades[4][4] = {
		{ 0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000 },
		{ 0xC2CE93, 0x818D66, 0x3A4C3A, 0x07100E },
		{ 0xC6DE8C, 0x84A563, 0x396139, 0x081810 },
		{ 0x7FE2C3, 0x56B495, 0x357862, 0x0A1C15 },
	};
	// Each screen's color while the game has it off: slightly lighter than its lightest shade, not
	// white.
	static const unsigned long off[4] = { 0xFFFFFF, 0xCFDAAC, 0xD2E6A6, 0x91EAD0 };
	gb->setDmgPaletteColor(0, 4, off[screen]);
	gb->setScreenOffBackground(offBackground);
	// A Game Boy game started through a Game Boy Color's boot ROM keeps the colors the boot ROM
	// gave it (a state stores them too), so they aren't replaced here.
	if (!gb->isCgb() || (loadFlags & gambatte::GB::NO_BIOS)) {
		for (int i = 0; i < 12; ++i) {
			gb->setDmgPaletteColor(i / 4, i % 4, shades[screen][i % 4]);
		}
	}
	// Game Boy screen shades are shown as they are; color correction is only for a Game Boy Color's
	// screen.
	int const wanted = correction && gb->isCgb() ? (loadFlags & gambatte::GB::GBA_FLAG ? 2 : 1) : 0;
	if (wanted != built) {
		built = wanted;
		if (wanted) {
			correctedColors(wanted == 2);
		} else {
			for (unsigned i = 0; i < 32768; ++i) {
				unsigned const r = i & 0x1F;
				unsigned const g = i >> 5 & 0x1F;
				unsigned const b = i >> 10 & 0x1F;
				cgbColors[i] = 0xFF000000 | ((r * 255 + 15) / 31) << 16 | ((g * 255 + 15) / 31) << 8 | ((b * 255 + 15) / 31);
			}
		}
	}
	gb->setCgbPalette(cgbColors);
}

void restartAs(LibretroGbModel model);

// Starts the game as the chosen model, or as its header asks for, reusing the existing machine if
// there is one.
bool start(LibretroGbModel model, char* error, size_t errorSize) {
	chosen = model;
	const unsigned char* const bytes = &game[0];
	size_t const size = game.size();
	const char* const systemDirectory = hasSystemPath ? systemPath.c_str() : 0;
	// Sachen's multicarts show the boot ROM their own logo through a lock that isn't emulated, and
	// keep their header where their scrambled address lines put it.
	bool const sachen = size >= 0x200 && ((bytes[0x104] == 0xCE && bytes[0x144] == 0xED && bytes[0x114] == 0x66)
	                                      || (bytes[0x184] == 0xCE && bytes[0x1C4] == 0xED && bytes[0x194] == 0x66));
	if (model == LIBRETRO_GB_AUTO && sachen) {
		model = (bytes[0x151] & 0xBF) == 0x80 ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
	}
	if (model == LIBRETRO_GB_AUTO) {
		// The header's CGB flag: 0x80 for color games that also run on a Game Boy, 0xC0 for color
		// only. Any other value is the end of an older game's title. So is 0x80 when a title runs
		// right up to it in an unlicensed game whose header names no known cartridge type.
		const unsigned char* const header = bytes;
		unsigned const flag = size > 0x14B ? header[0x143] : 0;
		unsigned const type = size > 0x14B ? header[0x147] : 0;
		bool const knownType = type <= 0x22 || type >= 0xFC;
		bool const title = !knownType && header[0x14B] != 0x33 && header[0x142] > 0x20 && header[0x142] < 0x7F;
		model = flag == 0xC0 || (flag == 0x80 && !title) ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
	}
	// Starts through a boot ROM when there is one; otherwise in the state the boot ROM would have
	// left the machine.
	std::string const boot = !sachen && bootWanted == LIBRETRO_GB_BOOT_NINTENDO ? bootRom(systemDirectory, model) : std::string();
	std::vector<unsigned char> const builtIn = !sachen && bootWanted == LIBRETRO_GB_BOOT_SAMEBOY ? sameboyBoot(model)
	                                                                                             : std::vector<unsigned char>();
	bootUsed = bootWanted;
	loadFlags = gambatte::GB::READONLY_SAV;
	if (model == LIBRETRO_GB_MGB) {
		loadFlags |= gambatte::GB::MGB_FLAG;
	} else if (model != LIBRETRO_GB_DMG) {
		loadFlags |= gambatte::GB::CGB_MODE;
	}
	if (model == LIBRETRO_GB_AGB) {
		loadFlags |= gambatte::GB::GBA_FLAG;
	}
	if (boot.empty() && builtIn.empty()) {
		loadFlags |= gambatte::GB::NO_BIOS;
	}
	if (!gb) {
		gb = new gambatte::GB;
		gb->setLinkCallback(linkClocked);
	}
	linked = false;
	if (gb->load(reinterpret_cast<const char*>(bytes), size, loadFlags) != gambatte::LOADRES_OK) {
		std::snprintf(error, errorSize, "armGBA can't run this Game Boy game");
		return false;
	}
	gb->linkStatus(265);
	if (!boot.empty()) {
		if (gb->loadBios(boot) < 0) {
			std::snprintf(error, errorSize, "armGBA can't read %s", boot.c_str());
			return false;
		}
		gb->reset(stall());
	} else if (!builtIn.empty()) {
		gb->loadBios(reinterpret_cast<const char*>(&builtIn[0]), builtIn.size());
		gb->reset(stall());
	}
	gb->setInputGetter(input, 0);
	gb->setGameGenie(gameGenie);
	gb->setGameShark(gameShark);
	setColors();
	sumLeft = sumRight = 0;
	summed = 0;
	pressed = 0;
	std::memset(pixels, 0, sizeof(pixels));
	size_t const needed = gb->saveState(0, 0, static_cast<char*>(0)) + sizeof(gambatte::uint_least32_t);
	if (!stateSize) {
		stateSize = needed + STATE_SLACK;
	} else if (needed > stateSize) {
		stateSize = needed;
	}
	return true;
}

// The eight bits of a transfer this Game Boy clocked are out: the other one's byte comes back.
void linkClocked() {
	gb->linkStatus(linkGbTransfer(gb->linkStatus(258)));
}

void updateLink() {
	if (linked != linkGbConnected()) {
		linked = !linked;
		gb->linkStatus(linked ? 264 : 265);
	}
}

}

extern "C" {

bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel model, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize) {
	libretroGbUnload();
	game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	systemPath = systemDirectory ? systemDirectory : "";
	hasSystemPath = systemDirectory != 0;
	correction = colorCorrection;
	ratio = NATIVE_RATE / audioRate;
	stateSize = 0;
	wanted = model;
	if (!start(model, error, errorSize)) {
		libretroGbUnload();
		return false;
	}
	std::memset(rtcFile, 0, sizeof(rtcFile));
	rtcPending = rtcSize();
	return true;
}

// Frees the machine.
void libretroGbUnload(void) {
	delete gb;
	gb = 0;
	gameGenie.clear();
	gameShark.clear();
}

void libretroGbReset(enum LibretroGbModel model) {
	wanted = model;
	if (model == chosen && bootUsed == bootWanted) {
		gb->reset(stall());
		return;
	}
	restartAs(model);
}

}

namespace {

// Restarts the game in the same machine as another model, or with the boot ROM switched on or off.
// Its cartridge RAM stays as it was and where it was, since the frontend may hold a pointer to it,
// and so does its clock.
void restartAs(LibretroGbModel model) {
	size_t size;
	const unsigned char* const ram = static_cast<const unsigned char*>(libretroGbSaveRam(&size));
	std::vector<unsigned char> const kept(ram, ram + size);
	unsigned long clock[11];
	unsigned char huc3[HUC3_STATE];
	if (gb->hasMbc3Rtc()) {
		gb->getRtcRegs(clock);
	} else if (gb->hasHuC3()) {
		gb->getHuC3Regs(huc3);
	}
	LibretroGbModel const before = chosen;
	char error[64];
	if (!start(model, error, sizeof(error))) {
		start(before, error, sizeof(error));
	}
	void* const now = libretroGbSaveRam(&size);
	if (now && size == kept.size() && size) {
		std::memcpy(now, &kept[0], size);
	}
	if (gb->hasMbc3Rtc() && !rtcPending) {
		gb->setRtcRegs(clock);
	} else if (gb->hasHuC3() && !rtcPending) {
		gb->setHuC3Regs(huc3);
	}
}

}

extern "C" {

// Called when the Screen Off Behavior option changes, or is read before a game is loaded.
void libretroGbSetScreenOffBackground(bool on) {
	offBackground = on;
	if (gb) {
		gb->setScreenOffBackground(on);
	}
}

// Called when the Color Correction option changes.
void libretroGbSetColorCorrection(bool on) {
	correction = on;
	setColors();
}

// Called when the Game Boy Screen option changes, or is read before a game is loaded.
void libretroGbSetBootRom(enum LibretroGbBoot boot) {
	bootWanted = boot;
}

void libretroGbSetScreen(int which) {
	screen = which;
	if (gb) {
		setColors();
	}
}

bool libretroGbOnAdvance(void) {
	return loadFlags & gambatte::GB::GBA_FLAG;
}

// Runs one frame. Gambatte runs by audio samples, so it is asked for a frame's worth at a time
// until it has drawn a frame, and its 2 MHz stereo samples are averaged down to the frontend's
// rate. Turning the screen off can delay a frame by up to three frames' worth. A game that turns
// the screen off and on faster than a frame never finishes one; after four frames' worth, the
// frontend gets the last picture again.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** out, size_t* frames) {
	pressed = keys & 0xFF;
	sampleCount = 0;
	if (rtcPending) {
		readRtcFile();
	}
	updateLink();
	// Linked, a frame runs in sixteen parts with the cable checked between them.
	int const parts = linked ? 16 : 1;
	bool drawn = false;
	for (int runs = 0; !drawn && runs < 4 * parts; ++runs) {
		if (linked) {
			linkGbPoll();
		}
		std::size_t count = FRAME_SAMPLES / parts;
		drawn = gb->runFor(pixels, LIBRETRO_GB_WIDTH, native, count) >= 0;
		// Each frontend sample is the mean of the native samples it covers.
		for (std::size_t i = 0; i < count;) {
			std::size_t const run = ratio - summed < count - i ? ratio - summed : count - i;
			int left = 0;
			int right = 0;
			for (std::size_t k = 0; k < run; ++k) {
				left += static_cast<int16_t>(native[i + k] & 0xFFFF);
				right += static_cast<int16_t>(native[i + k] >> 16);
			}
			sumLeft += left;
			sumRight += right;
			summed += run;
			i += run;
			if (summed == ratio) {
				if (sampleCount + 2 <= sizeof(samples) / sizeof(samples[0])) {
					samples[sampleCount++] = sumLeft / static_cast<long>(ratio);
					samples[sampleCount++] = sumRight / static_cast<long>(ratio);
				}
				sumLeft = sumRight = 0;
				summed = 0;
			}
		}
	}
	if (rtcSize()) {
		writeRtcFile();
	}
	*out = samples;
	*frames = sampleCount / 2;
	return reinterpret_cast<const uint32_t*>(pixels);
}

// The cartridge's battery RAM, which the frontend loads and saves as the save file.
void* libretroGbSaveRam(size_t* size) {
	unsigned char* data = 0;
	int length = 0;
	*size = 0;
	if (!gb || !gb->getMemoryArea(3, &data, &length)) {
		return 0;
	}
	*size = length;
	return data;
}

// The cartridge's clock, MBC3 or HuC3, which the frontend keeps as the .rtc file.
void* libretroGbRtc(size_t* size) {
	*size = rtcSize();
	return *size ? rtcFile : 0;
}

// Converts a stick position into the tilt sensor's reading.
void libretroGbSetTilt(int x, int y) {
	// The sensor reads lower to the right and towards the player; 0x70 is the cartridge tipped onto
	// its side.
	gambatte::mbc7TiltX = -x * 0x70 / 32768;
	gambatte::mbc7TiltY = -y * 0x70 / 32768;
}

// Flicks the console, for the one sensor reading the flick lasts.
void libretroGbFlick(void) {
	gambatte::mbc7Flick = true;
}

// Work RAM, for achievements and cheats.
void* libretroGbSystemRam(size_t* size) {
	unsigned char* data = 0;
	int length = 0;
	*size = 0;
	if (!gb || !gb->getMemoryArea(2, &data, &length)) {
		return 0;
	}
	*size = length;
	return data;
}

size_t libretroGbMemoryMap(struct retro_memory_descriptor* out) {
	unsigned char* vram;
	unsigned char* rom;
	unsigned char* wram;
	unsigned char* cartRam;
	unsigned char* oam;
	int vramLength, romLength, wramLength, cartRamLength, oamLength;
	if (!gb || !gb->getMemoryArea(0, &vram, &vramLength) || !gb->getMemoryArea(1, &rom, &romLength)
	    || !gb->getMemoryArea(2, &wram, &wramLength) || !gb->getMemoryArea(3, &cartRam, &cartRamLength)
	    || !gb->getMemoryArea(4, &oam, &oamLength)) {
		return 0;
	}
	size_t count = 0;
	// OAM, then the I/O registers and high RAM, are one block.
	struct retro_memory_descriptor const areas[] = {
		{ RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0xC000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, wram, 0x1000, 0xD000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, oam, 0x180, 0xFF80, 0, 0, 0x80, 0 },
		{ 0, vram, 0, 0x8000, 0, 0, 0x2000, 0 },
		{ 0, oam, 0, 0xFE00, 0xFFFFFFE0, 0, 0xA0, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0, 0x0000, 0, 0, 0x4000, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0x4000, 0x4000, 0, 0, 0x4000, 0 },
		{ 0, oam, 0x100, 0xFF00, 0, 0, 0x80, 0 },
	};
	for (std::size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i) {
		out[count++] = areas[i];
	}
	if (cartRamLength) {
		struct retro_memory_descriptor const area = { 0, cartRam, 0, 0xA000, ~(std::size_t) 0x1FFF, 0, (std::size_t) cartRamLength, 0 };
		out[count++] = area;
	}
	// A Game Boy Color's work RAM banks 2 to 7, at the addresses achievements expect.
	if (gb->isCgb()) {
		struct retro_memory_descriptor const area = { RETRO_MEMDESC_SYSTEM_RAM, wram, 0x2000, 0x10000, 0xFFFFA000, 0, 0x6000, 0 };
		out[count++] = area;
	}
	return count;
}

bool libretroGbIsColor(void) {
	return gb && gb->isCgb();
}

bool libretroGbSetCheats(const char* const* codes, size_t count) {
	bool understood = true;
	gameGenie.clear();
	gameShark.clear();
	for (size_t i = 0; i < count; ++i) {
		std::string const all = codes[i] ? codes[i] : "";
		std::size_t pos = 0;
		while (pos < all.size()) {
			std::size_t const end = all.find_first_of("+ \t\r\n;", pos);
			std::string const code = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
			pos = end == std::string::npos ? all.size() : end + 1;
			if (code.empty()) {
				continue;
			}
			bool const genie = (code.size() == 7 && code[3] == '-') || (code.size() == 11 && code[3] == '-' && code[7] == '-');
			bool const shark = code.size() == 8 && code.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos;
			std::string& list = genie ? gameGenie : gameShark;
			if (!genie && !shark) {
				understood = false;
				continue;
			}
			if (!list.empty()) {
				list += ';';
			}
			list += code;
		}
	}
	if (gb) {
		gb->setGameGenie(gameGenie);
		gb->setGameShark(gameShark);
	}
	return understood;
}

// Save states: the size reported to the frontend leaves room for a state to grow.
size_t libretroGbStateSize(void) {
	return gb ? stateSize : 0;
}

// Saves the state's length, then the state.
bool libretroGbSaveState(void* data, size_t size) {
	if (!gb || size < stateSize) {
		return false;
	}
	gambatte::uint_least32_t const length = gb->saveState(0, 0, static_cast<char*>(0));
	if (length + sizeof(length) > size) {
		return false;
	}
	std::memcpy(data, &length, sizeof(length));
	gb->saveState(0, 0, static_cast<char*>(data) + sizeof(length));
	return true;
}

// Loads a state saved by libretroGbSaveState.
// Link cable: the byte this Game Boy shifts out, and one shifted in from the other's transfer.
int libretroGbLinkOut(void) {
	return gb ? gb->linkStatus(258) : 0xFF;
}

void libretroGbLinkIn(int byte) {
	if (gb) {
		gb->linkStatus(byte & 0xFF);
	}
}

bool libretroGbLoadState(const void* data, size_t size) {
	gambatte::uint_least32_t length;
	if (!gb || size < sizeof(length)) {
		return false;
	}
	std::memcpy(&length, data, sizeof(length));
	const char* const state = static_cast<const char*>(data) + sizeof(length);
	if (length > size - sizeof(length)) {
		return false;
	}
	// A state from another model switches the machine to that model until the next restart. The
	// state's third byte says whether it ran as a Game Boy Color and, in states that record it
	// (0x80), whether as a Game Boy Advance. An older Color state counts as Game Boy Advance when
	// the option asks for one. A second instance that a frontend runs ahead with is handed each
	// state the first one makes, so it follows along.
	bool const header = length >= 3 && static_cast<unsigned char>(state[0]) == 0xFF;
	unsigned const flags = header ? static_cast<unsigned char>(state[2]) : 0;
	bool const color = flags & gambatte::GB::CGB_MODE;
	bool const recorded = flags & 0x80;
	bool const advance = recorded ? (flags & gambatte::GB::GBA_FLAG) != 0 : wanted == LIBRETRO_GB_AGB;
	bool const machineAdvance = loadFlags & gambatte::GB::GBA_FLAG;
	if (header && (color != gb->isCgb() || (color && recorded && advance != machineAdvance))) {
		LibretroGbModel const was = chosen;
		restartAs(color ? (advance ? LIBRETRO_GB_AGB : LIBRETRO_GB_CGB) : LIBRETRO_GB_DMG);
		if (!gb->loadState(state, length)) {
			restartAs(was);
			return false;
		}
	} else if (!gb->loadState(state, length)) {
		return false;
	}
	// A state holds the shades it was saved with, but the screen should be the one chosen now. It
	// also holds the clock, so a state loaded before the first frame wins over the .rtc file.
	setColors();
	rtcPending = false;
	return true;
}

}
