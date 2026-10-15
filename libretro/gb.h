/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_GB_H
#define ARMGBA_LIBRETRO_GB_H

// The libretro core's Game Boy and Game Boy Color side (gb-gambatte.cpp)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct retro_memory_descriptor;

enum {
	LIBRETRO_GB_WIDTH = 160,
	LIBRETRO_GB_HEIGHT = 144,
};

enum LibretroGbModel {
	LIBRETRO_GB_AUTO,
	LIBRETRO_GB_DMG,
	LIBRETRO_GB_CGB,
	LIBRETRO_GB_AGB,
	// The Game Boy Pocket, and the Game Boy Light, which is one with a backlight
	LIBRETRO_GB_MGB,
};

// Where games start from: as a boot ROM would have left the machine, through Nintendo's boot ROM when
// the system directory has it, or through SameBoy's free one built in
enum LibretroGbBoot {
	LIBRETRO_GB_BOOT_OFF,
	LIBRETRO_GB_BOOT_NINTENDO,
	LIBRETRO_GB_BOOT_SAMEBOY,
};

// False with a message for the user when the game can't be loaded
bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize);
void libretroGbUnload(void);
// Takes effect when a game is loaded or reset
void libretroGbSetBootRom(enum LibretroGbBoot);
// With another model than the game was started as, or the boot ROM switched, it starts again that way
void libretroGbReset(enum LibretroGbModel);
void libretroGbSetColorCorrection(bool);
// The original Game Boy's screen: 0 plain grays, 1 the Pocket's, 2 the first model's green, 3 the Light's backlight
void libretroGbSetScreen(int);
// Whether a screen the game has turned off shows the game's background color instead of the console's
void libretroGbSetScreenOffBackground(bool);
// Whether the game runs as on a Game Boy Advance, which shows it on its own larger screen
bool libretroGbOnAdvance(void);
// Runs a frame with the pressed buttons as KEYINPUT bits; returns the picture in XRGB8888 and the
// stereo samples
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** samples, size_t* frames);
void* libretroGbSaveRam(size_t* size);
// An MBC3 or HuC3 cartridge's clock, which the frontend keeps as the .rtc file; NULL for other cartridges
void* libretroGbRtc(size_t* size);
// How far a tilt-sensor cartridge is tilted, each from -32768 to 32767: right and towards the
// player are positive, as a stick's right and down are
void libretroGbSetTilt(int x, int y);
// A flick of the console upwards: the sensor's next reading has its top thrown towards the player
void libretroGbFlick(void);
// The work RAM, for a frontend's cheats and achievements
void* libretroGbSystemRam(size_t* size);
// The machine's memory as gambatte-libretro describes it to a frontend, into room for ten areas; the
// number of areas. They stay where they are while the game is loaded, but a Game Boy Color has more.
size_t libretroGbMemoryMap(struct retro_memory_descriptor*);
bool libretroGbIsColor(void);
// Game Genie (HHH-HHH or HHH-HHH-HHH) and GameShark (01VVAAAA) codes, each split by '+' or spaces,
// in force from now on and again after a restart; false if a code is neither
bool libretroGbSetCheats(const char* const* codes, size_t count);
size_t libretroGbStateSize(void);
bool libretroGbSaveState(void* data, size_t size);
bool libretroGbLoadState(const void* data, size_t size);

#endif
