/* Copyright (c) 2026 Magnominus
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
#include "dmg07.h"
#include "link.h"
#include "armgba-boot.h"

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
	MAX_PLAYERS = 4,
	// With exact link timing, the most samples run between looks at the network.
	EXACT_PART = 2048,
};

// Gambatte's linkStatus calls; a value below 256 shifts that byte in.
enum {
	LINK_CLOCKED = 256, // whether this Game Boy's own clock finished a transfer
	LINK_ACK = 257,     // clears that
	LINK_OUT = 258,     // the byte being sent (SB)
	LINK_ON = 264,
	LINK_OFF = 265,
	LINK_STOP = 266,    // ends the transfer under way, stopping the console's own clock
	LINK_CONTROL = 267, // SC
	LINK_LEFT = 268,    // cycles until the transfer under way ends, or -1
};

void linkClocked();
// How many native samples make one frontend sample.
unsigned ratio;
unsigned cgbColors[32768];
// The boot ROM folder, kept so a game can be restarted as another model.
std::string systemPath;
bool hasSystemPath;
// The boot ROM choice from the option.
LibretroGbBoot bootWanted = LIBRETRO_GB_BOOT_NINTENDO;

// An MBC3 clock, kept by the frontend as the .rtc file:
// - 8 bytes: the Unix time the clock would have started from, as gambatte-libretro stores it, so
//   either core reads the other's file;
// - the registers, in rtc.h's order, and when they were saved, which carry more detail.
// A HuC3 chip's file stores the chip's whole state in place of the registers.
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

// One Game Boy. There are two to four when the consoles of a link run here; s is the one being worked
// on.
struct Console {
	gambatte::GB* gb;
	// Whether Gambatte's serial port goes to another Game Boy.
	bool linked;
	unsigned loadFlags;
	unsigned pressed;
	gambatte::uint_least32_t pixels[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	// The last whole picture, when both consoles run here and a frame can end mid-picture.
	gambatte::uint_least32_t shown[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	gambatte::uint_least32_t native[FRAME_SAMPLES + OVERRUN];
	// A frame's sound for the frontend: at most two frames' worth of native samples, two channels
	// each, and the running sums towards the next sample.
	int16_t samples[(FRAME_SAMPLES + OVERRUN) * 2 / 16 * 2 + 8];
	size_t sampleCount;
	long sumLeft;
	long sumRight;
	unsigned summed;
	size_t stateSize;
	// The game, kept so it can be restarted as another model.
	std::vector<unsigned char> game;
	LibretroGbModel chosen;
	// The model the Game Boy Model option asks for. Loading a state from another model may have
	// switched the machine away from it.
	LibretroGbModel wanted;
	// The boot ROM this game actually started with.
	LibretroGbBoot bootUsed;
	// The cheat codes in force, in Gambatte's format. Loading the game again clears them, so
	// they're reapplied.
	std::string gameGenie;
	std::string gameShark;
	unsigned char rtcFile[HUC3_SIZE];
	// The frontend fills rtcFile after the game is loaded, so the file is read just before the
	// first frame.
	bool rtcPending;
	// Native samples run past the end of the last frame, when the consoles of a link run here.
	long ahead;
};
Console consoles[MAX_PLAYERS];
Console* s = &consoles[0];
// How many consoles of a link run here (1 without one), and whether the Four Player Adapter joins
// them; two without it share the cable.
int players = 1;
bool adapterOn;

Dmg07 adapter;

// One transfer on the Four Player Adapter: each Game Boy waiting on the external clock takes the
// adapter's byte and gives its own.
void adapterTransfer() {
	uint8_t in[DMG07_PORTS];
	for (int i = 0; i < DMG07_PORTS; ++i) {
		in[i] = 0xFF;
		gambatte::GB* const gb = i < players ? consoles[i].gb : 0;
		if (gb && (gb->linkStatus(LINK_CONTROL) & 0x81) == 0x80) {
			in[i] = gb->linkStatus(LINK_OUT);
			gb->linkStatus(dmg07Out(&adapter, i));
		}
	}
	dmg07Transfer(&adapter, in);
}

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
	return !s->gb ? 0 : s->gb->hasMbc3Rtc() ? RTC_SIZE : s->gb->hasHuC3() ? HUC3_SIZE : 0;
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
	s->rtcPending = false;
	unsigned long long const now = std::time(0);
	if (s->gb->hasHuC3()) {
		if (std::memcmp(s->rtcFile + RTC_MAGIC, "aGBh", 4)) {
			return;
		}
		unsigned char state[HUC3_STATE];
		std::memcpy(state, s->rtcFile + RTC_REGS, sizeof(state));
		unsigned long long const saved = get64(s->rtcFile + RTC_SAVED);
		if (saved >= SANE_TIME && now >= saved) {
			runHuC3(state, now - saved);
		}
		s->gb->setHuC3Regs(state);
		return;
	}
	unsigned long r[11] = { 0 };
	if (!std::memcmp(s->rtcFile + RTC_MAGIC, "aGBc", 4)) {
		const unsigned char* const regs = s->rtcFile + RTC_REGS;
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
		unsigned long long const saved = get64(s->rtcFile + RTC_SAVED);
		if (saved >= SANE_TIME && now >= saved) {
			runClock(r, now - saved);
		}
	} else {
		// gambatte-libretro's format: the clock shows the time since its start, unless that start
		// is in the future.
		unsigned long long const base = get64(s->rtcFile + RTC_BASE);
		if (!base || base < SANE_TIME / 2 || now < SANE_TIME || base > now) {
			return;
		}
		runClock(r, now - base);
		for (int i = 0; i < 5; ++i) {
			r[i + RTC_LATCHED] = r[i];
		}
	}
	s->gb->setRtcRegs(r);
}

void writeRtcFile() {
	if (s->gb->hasHuC3()) {
		unsigned long long const now = std::time(0);
		put64(s->rtcFile + RTC_BASE, now);
		std::memcpy(s->rtcFile + RTC_MAGIC, "aGBh", 4);
		put64(s->rtcFile + RTC_SAVED, now);
		s->gb->getHuC3Regs(s->rtcFile + RTC_REGS);
		return;
	}
	unsigned long r[11];
	s->gb->getRtcRegs(r);
	unsigned long long const now = std::time(0);
	unsigned long long const shown = (((((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 60
	                                  + r[RTC_M]) * 60 + r[RTC_S]);
	put64(s->rtcFile + RTC_BASE, now - shown);
	std::memcpy(s->rtcFile + RTC_MAGIC, "aGBc", 4);
	put64(s->rtcFile + RTC_SAVED, now);
	unsigned char* const regs = s->rtcFile + RTC_REGS;
	for (int i = 0; i < 5; ++i) {
		regs[i] = r[i];
		regs[9 + i] = r[i + RTC_LATCHED];
	}
	for (int i = 0; i < 4; ++i) {
		regs[5 + i] = r[RTC_C] >> i * 8;
	}
}

unsigned input(void* console) {
	return static_cast<Console*>(console)->pressed;
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

// Finds the boot ROM for the model in the system directory, or returns none. It looks for
// Nintendo's boot ROM under every name that ROCKNIX's BIOS check, Gambatte, gbdev's archive,
// SameBoy and Gearboy use for it, then for a SameBoy boot ROM under the same names.
//
// For a Game Boy Advance, the Advance's own comes first; a Game Boy Color boot ROM is used only if
// it is Nintendo's: Gambatte can convert that one, but not SameBoy's, and the conversion's timing
// is not quite the Advance's.
std::string bootRom(const char* systemDirectory, LibretroGbModel model) {
	static const char* const names[4][5] = {
		{ "gb_bios.bin", "dmg_boot.bin" },
		{ "gbc_bios.bin", "cgb_boot.bin" },
		// The later Game Boy Advances', the first ones', the name SameBoy also uses, then the Game Boy
		// Color's.
		{ "cgb_agb_boot.bin", "cgb_agb0_boot.bin", "agb_boot.bin", "gbc_bios.bin", "cgb_boot.bin" },
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
		bool const converted = model == LIBRETRO_GB_AGB && (i == 3 || i == 4);
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

// Returns armGBA's boot ROM for the model (gbboot/).
std::vector<unsigned char> armgbaBoot(LibretroGbModel model) {
	switch (model) {
	case LIBRETRO_GB_CGB:
		return std::vector<unsigned char>(armgba_cgb_boot, armgba_cgb_boot + sizeof(armgba_cgb_boot));
	case LIBRETRO_GB_AGB:
		return std::vector<unsigned char>(armgba_agb_boot, armgba_agb_boot + sizeof(armgba_agb_boot));
	case LIBRETRO_GB_MGB:
		return std::vector<unsigned char>(armgba_mgb_boot, armgba_mgb_boot + sizeof(armgba_mgb_boot));
	default:
		return std::vector<unsigned char>(armgba_dmg_boot, armgba_dmg_boot + sizeof(armgba_dmg_boot));
	}
}

// How long the Game Boy Advance takes before handing over to the Game Boy Color hardware inside it.
size_t stall() {
	return (s->loadFlags & gambatte::GB::GBA_FLAG) && !(s->loadFlags & gambatte::GB::NO_BIOS) ? 485808 : 0;
}

// The original Game Boy screen chosen: 0 plain grays, 1 the first model's, 2 the Pocket's, 3 the
// Light's with its backlight on.
int screen;
bool correction;
// On the Game Boy Advance model, the Color's panel shown instead of the Advance's.
bool colorPanel;
// The contrast wheel of the first Game Boy, the Pocket and the Light, -5 to 5.
int contrast;
// The four shades shown now, the contrast wheel's included.
unsigned long shown[4];
// What the color table was last built for: 0 uncorrected, 1 a Game Boy Color's screen, 2 a Game Boy
// Advance's.
int built = -1;

// Builds the Game Boy Color's colors as its screen shows them, the way SameBoy does (Lior Halphon,
// MIT license; its "modern balanced" correction). Each channel goes through a response curve, then
// some blue is mixed into green. The Game Boy Advance's screen has its own curves and mixes less.
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
// Each screen: its color while the game has it off, its four shades, and the color its pixels go
// toward driven harder than the darkest shade. The plain grays are gambatte-libretro's. The first
// model's and the Pocket's are BGB's (bgb.bircd.org/reality): real ones photographed under
// color-accurate light and corrected with a ColorChecker. A first Game Boy's pixels go toward dark
// blue at the most contrast, as in BGB's contrast wheel; the Pocket's only darker. The Light is the
// Pocket's screen lit from behind, in the backlight's cyan (hue 180; photographed Lights run from
// about 157 to 191). A Light photographed with its light off and on, the contrast wheel unchanged,
// shows each shade's share of the lightest when lit as about the unlit share to the power 0.85, so
// each color here is the Pocket's share of its lightest shade to that power. The lightest shade
// passes the square root of the Pocket's share of the screen switched off: light crosses the screen
// once rather than twice.
struct Screen {
	unsigned long off;
	unsigned long shades[4];
	unsigned long full;
};
const Screen screens[4] = {
	{ 0xFFFFFF, { 0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000 }, 0x000000 },
	{ 0x948A04, { 0x759833, 0x588F51, 0x3B7560, 0x2E615A }, 0x00005E },
	{ 0xC2CFA8, { 0xADBF92, 0x96A67C, 0x727E64, 0x5A635C }, 0x2C322E },
	{ 0x60F1F1, { 0x59DFDF, 0x4EC6C6, 0x3C9E9E, 0x308383 }, 0x194B4B },
};

void setColors() {
	Screen const& now = screens[screen];
	s->gb->setDmgPaletteColor(0, 4, now.off);
	// The wheel sets the drive voltages of the whole STN screen together (DMG-LCD-06: its 30k pot sets
	// the IR3E02's bias levels), from about -11 V to -14 V end to end, so 12% either side of the
	// middle, each step a tenth of the turn. An STN pixel's darkness is an S-curve of its voltage, as
	// steep as BGB's contrast wheel shows: all the way down the screen is blank, and all the way up
	// even the lightest shade is nearly as dark as it goes, 10% to 90% dark over about 7% of the
	// voltage. A shade's darkness places it on its
	// screen's own path, from the screen switched off through the four shades to its darkest, so
	// its color changes along the way as a real screen's does: the lightest a little past the screen
	// switched off, the others by their brightness, the darkest nine tenths of the way.
	auto const contrasted = [&now](int shade) {
		auto const linear = [](unsigned long color, int bits) {
			double const c = (color >> bits & 0xFF) / 255.0;
			return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
		};
		auto const luminance = [&](unsigned long color) {
			return 0.2126 * linear(color, 16) + 0.7152 * linear(color, 8) + 0.0722 * linear(color, 0);
		};
		double const lightest = 0.03, darkest = 0.95, width = 0.12 / (std::log((1 - lightest) / lightest) * 2.12);
		unsigned long const path[6] = { now.off, now.shades[0], now.shades[1], now.shades[2], now.shades[3], now.full };
		double places[6] = { 0, lightest, 0, 0, darkest, 1 };
		double const top = luminance(now.shades[0]), bottom = luminance(now.shades[3]);
		for (int k = 2; k <= 3; ++k) {
			places[k] = lightest + (darkest - lightest) * (top - luminance(now.shades[k - 1])) / (top - bottom);
		}
		double const before = places[shade + 1];
		double const voltage = (1 + width * std::log(before / (1 - before))) * (1 + 0.12 * contrast / 5);
		double const after = 1 / (1 + std::exp(-(voltage - 1) / width));
		int k = 0;
		while (k < 4 && after > places[k + 1]) {
			++k;
		}
		double const t = std::min(std::max((after - places[k]) / (places[k + 1] - places[k]), 0.0), 1.0);
		unsigned long result = 0;
		for (int bits = 0; bits < 24; bits += 8) {
			double const l = linear(path[k], bits) + (linear(path[k + 1], bits) - linear(path[k], bits)) * t;
			double const c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
			result |= (unsigned long) std::lround(c * 255) << bits;
		}
		return result;
	};
	// A Game Boy game started through a Game Boy Color's boot ROM keeps the colors the boot ROM
	// gave it (a state stores them too), so they aren't replaced here.
	if (!s->gb->isCgb() || (s->loadFlags & gambatte::GB::NO_BIOS)) {
		for (int i = 0; i < 4; ++i) {
			shown[i] = contrast ? contrasted(i) : now.shades[i];
		}
		for (int i = 0; i < 12; ++i) {
			s->gb->setDmgPaletteColor(i / 4, i % 4, shown[i % 4]);
		}
	}
	// Game Boy screen shades are shown as they are; color correction is only for a Game Boy Color's
	// screen.
	int const table = correction && s->gb->isCgb() ? (s->loadFlags & gambatte::GB::GBA_FLAG && !colorPanel ? 2 : 1) : 0;
	if (table != built) {
		built = table;
		if (table) {
			correctedColors(table == 2);
		} else {
			for (unsigned i = 0; i < 32768; ++i) {
				unsigned const r = i & 0x1F;
				unsigned const g = i >> 5 & 0x1F;
				unsigned const b = i >> 10 & 0x1F;
				cgbColors[i] = 0xFF000000 | ((r * 255 + 15) / 31) << 16 | ((g * 255 + 15) / 31) << 8 | ((b * 255 + 15) / 31);
			}
		}
	}
	s->gb->setCgbPalette(cgbColors);
}

void restartAs(LibretroGbModel model);

// Sachen's multicarts show the boot ROM their own logo through a lock that isn't emulated, and keep
// their header where their scrambled address lines put it.
bool isSachen(const unsigned char* bytes, size_t size) {
	return size >= 0x200 && ((bytes[0x104] == 0xCE && bytes[0x144] == 0xED && bytes[0x114] == 0x66)
	                         || (bytes[0x184] == 0xCE && bytes[0x1C4] == 0xED && bytes[0x194] == 0x66));
}

// The model a game's header asks for.
LibretroGbModel headerModel(const unsigned char* header, size_t size, bool sachen) {
	if (sachen) {
		return (header[0x151] & 0xBF) == 0x80 ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
	}
	// The header's CGB flag: 0x80 for color games that also run on a Game Boy, 0xC0 for color-only
	// games. Any other value is part of an older game's title. So is 0x80 in an unlicensed game
	// whose title runs right up to it and whose header names no known cartridge type.
	unsigned const flag = size > 0x14B ? header[0x143] : 0;
	unsigned const type = size > 0x14B ? header[0x147] : 0;
	bool const knownType = type <= 0x22 || type >= 0xFC;
	bool const title = !knownType && header[0x14B] != 0x33 && header[0x142] > 0x20 && header[0x142] < 0x7F;
	return flag == 0xC0 || (flag == 0x80 && !title) ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
}

// Starts the game as the chosen model, or as its header asks for, reusing the existing machine if
// there is one.
bool start(LibretroGbModel model, char* error, size_t errorSize) {
	s->chosen = model;
	const unsigned char* const bytes = &s->game[0];
	size_t const size = s->game.size();
	const char* const systemDirectory = hasSystemPath ? systemPath.c_str() : 0;
	bool const sachen = isSachen(bytes, size);
	if (model == LIBRETRO_GB_AUTO) {
		model = headerModel(bytes, size, sachen);
	}
	// Starts through Nintendo's boot ROM when the system directory has it, else armGBA's; with Boot
	// ROM off, in the state a boot ROM would have left the machine.
	std::string const boot = !sachen && bootWanted == LIBRETRO_GB_BOOT_NINTENDO ? bootRom(systemDirectory, model) : std::string();
	std::vector<unsigned char> const builtIn = !sachen && bootWanted != LIBRETRO_GB_BOOT_OFF && boot.empty() ? armgbaBoot(model)
	                                                                                                       : std::vector<unsigned char>();
	s->bootUsed = bootWanted;
	s->loadFlags = gambatte::GB::READONLY_SAV;
	if (model == LIBRETRO_GB_MGB) {
		s->loadFlags |= gambatte::GB::MGB_FLAG;
	} else if (model != LIBRETRO_GB_DMG) {
		s->loadFlags |= gambatte::GB::CGB_MODE;
	}
	if (model == LIBRETRO_GB_AGB) {
		s->loadFlags |= gambatte::GB::GBA_FLAG;
	}
	if (boot.empty() && builtIn.empty()) {
		s->loadFlags |= gambatte::GB::NO_BIOS;
	}
	if (!s->gb) {
		s->gb = new gambatte::GB;
	}
	s->gb->setLinkCallback(players > 1 ? 0 : linkClocked);
	s->linked = false;
	if (s->gb->load(reinterpret_cast<const char*>(bytes), size, s->loadFlags) != gambatte::LOADRES_OK) {
		std::snprintf(error, errorSize, "armGBA can't run this Game Boy game");
		return false;
	}
	s->gb->linkStatus(players > 1 ? LINK_ON : LINK_OFF);
	if (!boot.empty()) {
		if (s->gb->loadBios(boot) < 0) {
			std::snprintf(error, errorSize, "armGBA can't read %s", boot.c_str());
			return false;
		}
		s->gb->reset(stall());
	} else if (!builtIn.empty()) {
		s->gb->loadBios(reinterpret_cast<const char*>(&builtIn[0]), builtIn.size());
		s->gb->reset(stall());
	}
	s->gb->setInputGetter(input, s);
	s->gb->setGameGenie(s->gameGenie);
	s->gb->setGameShark(s->gameShark);
	setColors();
	s->sumLeft = s->sumRight = 0;
	s->summed = 0;
	s->pressed = 0;
	std::memset(s->pixels, 0, sizeof(s->pixels));
	size_t const needed = s->gb->saveState(0, 0, static_cast<char*>(0)) + sizeof(gambatte::uint_least32_t);
	if (!s->stateSize) {
		s->stateSize = needed + STATE_SLACK;
	} else if (needed > s->stateSize) {
		s->stateSize = needed;
	}
	return true;
}

// The eight bits of a transfer this Game Boy clocked are out: the other one's byte comes back.
void linkClocked() {
	s->gb->linkStatus(LINK_ACK);
	s->gb->linkStatus(linkGbTransfer(s->gb->linkStatus(LINK_OUT)));
}

void updateLink() {
	if (s->linked != linkGbConnected()) {
		s->linked = !s->linked;
		s->gb->linkStatus(s->linked ? LINK_ON : LINK_OFF);
	}
}

}

extern "C" {

bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel model, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize) {
	libretroGbUnload();
	s->game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	systemPath = systemDirectory ? systemDirectory : "";
	hasSystemPath = systemDirectory != 0;
	correction = colorCorrection;
	ratio = NATIVE_RATE / audioRate;
	s->stateSize = 0;
	s->wanted = model;
	if (!start(model, error, errorSize)) {
		libretroGbUnload();
		return false;
	}
	std::memset(s->rtcFile, 0, sizeof(s->rtcFile));
	s->rtcPending = rtcSize();
	return true;
}

void libretroGbUnload(void) {
	delete s->gb;
	s->gb = 0;
	s->gameGenie.clear();
	s->gameShark.clear();
}

void libretroGbReset(enum LibretroGbModel model) {
	s->wanted = model;
	if (model == s->chosen && s->bootUsed == bootWanted) {
		s->gb->reset(stall());
		return;
	}
	restartAs(model);
}

// Restarts the selected console as its power switch would, keeping it on the link.
void libretroGbRestart(void) {
	libretroGbReset(s->wanted);
	if (players > 1) {
		s->gb->setLinkCallback(0);
		s->gb->linkStatus(LINK_ON);
	}
	s->linked = false;
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
	if (s->gb->hasMbc3Rtc()) {
		s->gb->getRtcRegs(clock);
	} else if (s->gb->hasHuC3()) {
		s->gb->getHuC3Regs(huc3);
	}
	LibretroGbModel const before = s->chosen;
	char error[64];
	if (!start(model, error, sizeof(error))) {
		start(before, error, sizeof(error));
	}
	void* const now = libretroGbSaveRam(&size);
	if (now && size == kept.size() && size) {
		std::memcpy(now, &kept[0], size);
	}
	if (s->gb->hasMbc3Rtc() && !s->rtcPending) {
		s->gb->setRtcRegs(clock);
	} else if (s->gb->hasHuC3() && !s->rtcPending) {
		s->gb->setHuC3Regs(huc3);
	}
}

}

extern "C" {

void libretroGbSetColorCorrection(bool on) {
	correction = on;
	setColors();
}

void libretroGbSetColorPanel(bool color) {
	colorPanel = color;
	if (s->gb) {
		setColors();
	}
}

void libretroGbSetBootRom(enum LibretroGbBoot boot) {
	bootWanted = boot;
}

void libretroGbSetScreen(int which) {
	screen = which;
	if (s->gb) {
		setColors();
	}
}

void libretroGbSetContrast(int steps) {
	contrast = steps;
	if (s->gb) {
		setColors();
	}
}

uint32_t libretroGbShade(int shade) {
	return (uint32_t) shown[shade];
}

uint32_t libretroGbDarkestColor(void) {
	return screens[screen].shades[3];
}

uint32_t libretroGbOffColor(void) {
	return screens[screen].off;
}

bool libretroGbOnAdvance(void) {
	return s->loadFlags & gambatte::GB::GBA_FLAG;
}

}

namespace {

// Averages a run's 2 MHz stereo samples down to the frontend's rate: each frontend sample is the
// mean of the native samples it covers.
void mixDown(std::size_t count) {
	for (std::size_t i = 0; i < count;) {
		std::size_t const run = ratio - s->summed < count - i ? ratio - s->summed : count - i;
		int left = 0;
		int right = 0;
		for (std::size_t k = 0; k < run; ++k) {
			left += static_cast<int16_t>(s->native[i + k] & 0xFFFF);
			right += static_cast<int16_t>(s->native[i + k] >> 16);
		}
		s->sumLeft += left;
		s->sumRight += right;
		s->summed += run;
		i += run;
		if (s->summed == ratio) {
			if (s->sampleCount + 2 <= sizeof(s->samples) / sizeof(s->samples[0])) {
				s->samples[s->sampleCount++] = s->sumLeft / static_cast<long>(ratio);
				s->samples[s->sampleCount++] = s->sumRight / static_cast<long>(ratio);
			}
			s->sumLeft = s->sumRight = 0;
			s->summed = 0;
		}
	}
}

// Starts a frame on the selected console: its buttons, and its clock file once loaded.
void beginFrame(uint16_t keys) {
	s->pressed = keys & 0xFF;
	s->sampleCount = 0;
	if (s->rtcPending) {
		readRtcFile();
	}
}

void endFrame() {
	if (rtcSize()) {
		writeRtcFile();
	}
}

// The cable between the two consoles here. When one has clocked out its eight bits, it takes the
// other's byte. The other takes this one's only if its serial port is waiting for a clock or
// clocking too. Both then stop their own clocks, since the transfer is done.
void exchange(Console& clocked, Console& other) {
	clocked.gb->linkStatus(LINK_ACK);
	int const out = clocked.gb->linkStatus(LINK_OUT);
	clocked.gb->linkStatus(other.gb->linkStatus(LINK_OUT));
	clocked.gb->linkStatus(LINK_STOP);
	other.gb->linkStatus(out);
	other.gb->linkStatus(LINK_STOP);
}

// Sound samples until a Game Boy's transfer on its own clock ends: -1 for none, 0 when due.
long serialLeft(Console& c) {
	if (c.gb->linkStatus(LINK_CLOCKED)) {
		return 0;
	}
	int const cycles = c.gb->linkStatus(LINK_LEFT);
	return cycles < 0 ? -1 : cycles / 2;
}

// The fewest samples a transfer a Game Boy could start now takes: eight bits at 8192 Hz, or on a
// Game Boy Color with SC's fast bit, at 262144 Hz.
long shortestTransfer(Console& c) {
	return c.gb->isCgb() && c.gb->linkStatus(LINK_CONTROL) & 0x02 ? 64 : 2048;
}

// Where the consoles next meet: at the frame's end, at the next transfer's end on the cable or the
// adapter, and no later than the shortest transfer any console could start from now.
long nextMeeting(long const done[]) {
	long now = done[0];
	for (int i = 1; i < players; ++i) {
		now = std::min(now, done[i]);
	}
	long meet = FRAME_SAMPLES;
	if (adapterOn) {
		meet = std::min<long>(meet, std::max<long>(adapter.at, now));
	}
	for (int i = 0; i < players; ++i) {
		long const left = serialLeft(consoles[i]);
		if (left >= 0) {
			meet = std::min(meet, done[i] + left);
		}
		if (!adapterOn) {
			meet = std::min(meet, now + shortestTransfer(consoles[i]));
		}
	}
	return meet;
}

// With every console at the meeting point: the transfers ending there.
void meet(long now) {
	if (adapterOn) {
		// On the adapter, a Game Boy's own clock reaches nothing.
		for (int i = 0; i < players; ++i) {
			if (serialLeft(consoles[i]) == 0) {
				consoles[i].gb->linkStatus(LINK_ACK);
				consoles[i].gb->linkStatus(0xFF);
				consoles[i].gb->linkStatus(LINK_STOP);
			}
		}
		while (adapter.at <= now) {
			adapterTransfer();
		}
		return;
	}
	for (int i = 0; i < players; ++i) {
		if (serialLeft(consoles[i]) == 0) {
			exchange(consoles[i], consoles[1 - i]);
		}
	}
}

// With exact link timing: this Game Boy's transfer ends, and the other's byte comes back.
void clockedExactly() {
	s->gb->linkStatus(LINK_ACK);
	int const in = linkGbTransfer(s->gb->linkStatus(LINK_OUT));
	s->gb->linkStatus(in & 0xFF);
	s->gb->linkStatus(LINK_STOP);
}

// With exact link timing, a frame runs in parts. Each part ends at this Game Boy's own transfer
// end, or where the leading console has got to. Its own transfer is told to the other as soon as it
// starts. A long wait at a stop ends the frame early.
void runExactlyParts() {
	long done = 0;
	while (done < FRAME_SAMPLES) {
		long const own = serialLeft(*s);
		if (own > 0) {
			linkGbClocking(s->gb->linkStatus(LINK_OUT), own, shortestTransfer(*s));
		}
		linkGbPoll();
		if (own == 0) {
			clockedExactly();
			continue;
		}
		long const allowed = linkGbAllowance();
		if (allowed == 0) {
			if (!linkGbArrived()) {
				return;
			}
			continue;
		}
		long count = std::min<long>(FRAME_SAMPLES - done, EXACT_PART);
		count = std::min(count, allowed);
		count = own > 0 ? std::min(count, own) : count;
		std::size_t samples = count;
		if (s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, samples) >= 0) {
			std::memcpy(s->shown, s->pixels, sizeof(s->shown));
		}
		mixDown(samples);
		done += samples;
		linkGbRan(samples);
	}
}

// Runs until a picture is drawn; linked, in sixteen parts with the cable checked between them.
void runUntilDrawn() {
	int const parts = s->linked ? 16 : 1;
	bool drawn = false;
	for (int runs = 0; !drawn && runs < 4 * parts; ++runs) {
		if (s->linked) {
			linkGbPoll();
		}
		std::size_t count = FRAME_SAMPLES / parts;
		drawn = s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count) >= 0;
		mixDown(count);
	}
}

void runExactly() {
	s->gb->setLinkCallback(0);
	runExactlyParts();
	s->gb->setLinkCallback(linkClocked);
}

}

extern "C" {

// Runs one frame. Gambatte runs by audio samples, so it is asked for a frame's worth at a time
// until it has drawn a frame.
//
// Turning the screen off can delay a frame by up to three frames' worth. A game that turns the
// screen off and on faster than a frame never finishes one, so after four frames' worth the
// frontend gets the last picture again.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** out, size_t* frames) {
	beginFrame(keys);
	updateLink();
	bool const exactly = s->linked && linkExactOn();
	if (exactly) {
		runExactly();
	} else {
		runUntilDrawn();
	}
	endFrame();
	*out = s->samples;
	*frames = s->sampleCount / 2;
	return reinterpret_cast<const uint32_t*>(exactly ? s->shown : s->pixels);
}

void libretroGbRunPlayers(const uint16_t keys[], const uint32_t* pictures[], const int16_t* out[], size_t frames[]) {
	Console* const selected = s;
	long done[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		beginFrame(keys[i]);
		done[i] = s->ahead;
	}
	// The consoles run in turns, the one furthest behind next, each up to where they next meet; there
	// they all stand on the same sample as a transfer ends.
	for (;;) {
		int i = 0;
		for (int j = 1; j < players; ++j) {
			i = done[j] < done[i] ? j : i;
		}
		if (done[i] >= FRAME_SAMPLES) {
			break;
		}
		long const meeting = nextMeeting(done);
		if (meeting <= done[i]) {
			meet(meeting);
			if (nextMeeting(done) <= done[i]) {
				// Nothing ended after all, as a game stopped its transfer: run on a little.
				s = &consoles[i];
				std::size_t count = 1;
				s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count);
				mixDown(count);
				done[i] += count;
			}
			continue;
		}
		s = &consoles[i];
		std::size_t count = meeting - done[i];
		if (s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count) >= 0) {
			std::memcpy(s->shown, s->pixels, sizeof(s->shown));
		}
		mixDown(count);
		done[i] += count;
	}
	dmg07EndFrame(&adapter, FRAME_SAMPLES);
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		endFrame();
		s->ahead = done[i] - FRAME_SAMPLES;
		pictures[i] = reinterpret_cast<const uint32_t*>(s->shown);
		out[i] = s->samples;
		frames[i] = s->sampleCount / 2;
	}
	s = selected;
}

// The cartridge's battery RAM, which the frontend loads and saves as the save file.
void* libretroGbSaveRam(size_t* size) {
	unsigned char* data = 0;
	int length = 0;
	*size = 0;
	if (!s->gb || !s->gb->getMemoryArea(3, &data, &length)) {
		return 0;
	}
	*size = length;
	return data;
}

// The cartridge's clock, MBC3 or HuC3, which the frontend keeps as the .rtc file.
void* libretroGbRtc(size_t* size) {
	*size = rtcSize();
	return *size ? s->rtcFile : 0;
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
	if (!s->gb || !s->gb->getMemoryArea(2, &data, &length)) {
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
	if (!s->gb || !s->gb->getMemoryArea(0, &vram, &vramLength) || !s->gb->getMemoryArea(1, &rom, &romLength)
	    || !s->gb->getMemoryArea(2, &wram, &wramLength) || !s->gb->getMemoryArea(3, &cartRam, &cartRamLength)
	    || !s->gb->getMemoryArea(4, &oam, &oamLength)) {
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
	if (s->gb->isCgb()) {
		struct retro_memory_descriptor const area = { RETRO_MEMDESC_SYSTEM_RAM, wram, 0x2000, 0x10000, 0xFFFFA000, 0, 0x6000, 0 };
		out[count++] = area;
	}
	return count;
}

bool libretroGbIsColor(void) {
	return s->gb && s->gb->isCgb();
}

bool libretroGbSetCheats(const char* const* codes, size_t count) {
	bool understood = true;
	s->gameGenie.clear();
	s->gameShark.clear();
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
			std::string& list = genie ? s->gameGenie : s->gameShark;
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
	if (s->gb) {
		s->gb->setGameGenie(s->gameGenie);
		s->gb->setGameShark(s->gameShark);
	}
	return understood;
}

}

namespace {

// Save states: the size reported to the frontend leaves room for a state to grow.
size_t stateSizeOne() {
	return s->gb ? s->stateSize : 0;
}

// Saves the state's length, then the state.
bool saveOne(void* data, size_t size) {
	if (!s->gb || size < s->stateSize) {
		return false;
	}
	gambatte::uint_least32_t const length = s->gb->saveState(0, 0, static_cast<char*>(0));
	if (length + sizeof(length) > size) {
		return false;
	}
	std::memcpy(data, &length, sizeof(length));
	s->gb->saveState(0, 0, static_cast<char*>(data) + sizeof(length));
	// The room left for the state to grow is zero, so the same machine always makes the same state.
	std::memset(static_cast<char*>(data) + sizeof(length) + length, 0, s->stateSize - sizeof(length) - length);
	return true;
}

bool loadOne(const void* data, size_t size) {
	gambatte::uint_least32_t length;
	if (!s->gb || size < sizeof(length)) {
		return false;
	}
	std::memcpy(&length, data, sizeof(length));
	const char* const state = static_cast<const char*>(data) + sizeof(length);
	if (length > size - sizeof(length)) {
		return false;
	}
	// A state from another model switches the machine to that model until the next restart. The
	// state's third byte says whether it ran as a Game Boy Color, and in states that record it
	// (0x80), whether it ran as a Game Boy Advance. An older Color state counts as Game Boy Advance
	// when the option asks for one.
	//
	// A second instance that a frontend runs ahead with is given each state the first one makes, so
	// it follows along.
	bool const header = length >= 3 && static_cast<unsigned char>(state[0]) == 0xFF;
	unsigned const flags = header ? static_cast<unsigned char>(state[2]) : 0;
	bool const color = flags & gambatte::GB::CGB_MODE;
	bool const recorded = flags & 0x80;
	bool const advance = recorded ? (flags & gambatte::GB::GBA_FLAG) != 0 : s->wanted == LIBRETRO_GB_AGB;
	bool const machineAdvance = s->loadFlags & gambatte::GB::GBA_FLAG;
	if (header && (color != s->gb->isCgb() || (color && recorded && advance != machineAdvance))) {
		LibretroGbModel const was = s->chosen;
		restartAs(color ? (advance ? LIBRETRO_GB_AGB : LIBRETRO_GB_CGB) : LIBRETRO_GB_DMG);
		if (!s->gb->loadState(state, length)) {
			restartAs(was);
			return false;
		}
	} else if (!s->gb->loadState(state, length)) {
		return false;
	}
	// A state holds the shades it was saved with, but the screen should be the one chosen now. It
	// also holds the clock, so a state loaded before the first frame wins over the .rtc file.
	setColors();
	s->rtcPending = false;
	return true;
}

}

extern "C" {

// Link cable: the byte this Game Boy shifts out, and one shifted in from the other's transfer.
int libretroGbLinkOut(void) {
	return s->gb ? s->gb->linkStatus(LINK_OUT) : 0xFF;
}

void libretroGbLinkIn(int byte) {
	if (s->gb) {
		s->gb->linkStatus(byte & 0xFF);
	}
}

int libretroGbLinkControl(void) {
	return s->gb ? s->gb->linkStatus(LINK_CONTROL) : 0;
}

void libretroGbSelect(int which) {
	s = &consoles[which];
}

// Real consoles are never in step, and games rely on it: when two try to lead the link at the same
// moment, Pokemon's handshake never settles. So each console runs a part of a frame behind the one
// before.
void libretroGbSetPlayers(int count, bool withAdapter) {
	players = count;
	adapterOn = count > 1 && withAdapter;
	dmg07Reset(&adapter);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		static long const behind[MAX_PLAYERS] = { 0, FRAME_SAMPLES / 2, FRAME_SAMPLES / 4, FRAME_SAMPLES * 3 / 4 };
		consoles[i].ahead = behind[i];
		if (consoles[i].gb) {
			consoles[i].gb->setLinkCallback(count > 1 ? 0 : linkClocked);
			consoles[i].gb->linkStatus(count > 1 ? LINK_ON : LINK_OFF);
			consoles[i].linked = false;
		}
	}
}

// With the consoles of a link here, a state holds, in order:
// - how far each console ran past the last frame;
// - the adapter;
// - each console's state, in its own fixed-size part;
// - their pictures, since a frame ends partway through drawing one and a state loaded for a
//   rollback has to carry on with it.
size_t libretroGbStateSize(void) {
	if (players < 2) {
		return stateSizeOne();
	}
	size_t size = players * sizeof(int32_t) + sizeof(adapter);
	for (int i = 0; i < players; ++i) {
		size += consoles[i].stateSize + sizeof(consoles[i].pixels) + sizeof(consoles[i].shown);
	}
	return size;
}

bool libretroGbSaveState(void* data, size_t size) {
	if (players < 2) {
		return saveOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	char* at = static_cast<char*>(data);
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		int32_t const ahead = consoles[i].ahead;
		std::memcpy(at, &ahead, sizeof(ahead));
		at += sizeof(ahead);
	}
	std::memcpy(at, &adapter, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool saved = true;
	for (int i = 0; i < players && saved; ++i) {
		s = &consoles[i];
		saved = saveOne(at, s->stateSize);
		at += s->stateSize;
	}
	for (int i = 0; i < players && saved; ++i) {
		std::memcpy(at, consoles[i].pixels, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(at, consoles[i].shown, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return saved;
}

bool libretroGbLoadState(const void* data, size_t size) {
	if (players < 2) {
		return loadOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	const char* at = static_cast<const char*>(data);
	int32_t ahead[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		std::memcpy(&ahead[i], at, sizeof(ahead[i]));
		at += sizeof(ahead[i]);
	}
	std::memcpy(&adapter, at, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool loaded = true;
	for (int i = 0; i < players && loaded; ++i) {
		s = &consoles[i];
		loaded = loadOne(at, s->stateSize);
		s->ahead = ahead[i];
		at += s->stateSize;
	}
	for (int i = 0; i < players && loaded; ++i) {
		std::memcpy(consoles[i].pixels, at, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(consoles[i].shown, at, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return loaded;
}

}
