/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_GB_H
#define ARMGBA_LIBRETRO_GB_H

// The libretro core's Game Boy and Game Boy Color side (gb-gambatte.cpp).

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
	// The Game Boy Pocket; the Game Boy Light is the same with a backlight.
	LIBRETRO_GB_MGB,
};

// How games start: as a boot ROM would have left the machine, through Nintendo's boot ROM when the
// system directory has it (armGBA's otherwise), or through armGBA's own boot ROM built into the core.
enum LibretroGbBoot {
	LIBRETRO_GB_BOOT_OFF,
	LIBRETRO_GB_BOOT_NINTENDO,
	LIBRETRO_GB_BOOT_ARMGBA,
};

// Loads a game. Returns false, with a message for the user, if it can't be loaded.
bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize);
void libretroGbUnload(void);
// Takes effect when a game is loaded or reset.
void libretroGbSetBootRom(enum LibretroGbBoot);
// Resets the game. With a different model than it started as, or the boot ROM switched, it restarts
// that way.
void libretroGbReset(enum LibretroGbModel);
// Restarts the selected console alone, as its power switch would.
void libretroGbRestart(void);
void libretroGbSetColorCorrection(bool);
// On the Game Boy Advance model, shows the Color's panel, its colors, instead of the Advance's.
void libretroGbSetColorPanel(bool color);
// Sets the original Game Boy's screen: 0 plain grays, 1 the Pocket's, 2 the first model's green, 3
// the Light's backlight, 4 the first model measured, 5 the Pocket measured.
void libretroGbSetScreen(int);
// Sets that screen's contrast wheel, -5 to 5.
void libretroGbSetContrast(int);
// That screen's color while the game has it off, as XRGB8888.
uint32_t libretroGbOffColor(void);
// Whether the game runs as on a Game Boy Advance, which shows it on its own larger screen.
bool libretroGbOnAdvance(void);
// Runs a frame with the pressed buttons as KEYINPUT bits. Returns the picture in XRGB8888 and the
// stereo samples.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** samples, size_t* frames);
void* libretroGbSaveRam(size_t* size);
// An MBC3 or HuC3 cartridge's clock, which the frontend keeps as the .rtc file; NULL for other
// cartridges.
void* libretroGbRtc(size_t* size);
// Sets how far a tilt-sensor cartridge is tilted, each axis from -32768 to 32767. Right and towards
// the player are positive, like a stick's right and down.
void libretroGbSetTilt(int x, int y);
// Flicks the console upwards: the sensor's next reading has its top thrown towards the player.
void libretroGbFlick(void);
// The work RAM, for a frontend's cheats and achievements.
void* libretroGbSystemRam(size_t* size);
// Fills in the machine's memory areas as gambatte-libretro describes them to a frontend (room for
// ten), and returns how many there are. They stay put while the game is loaded, but a Game Boy
// Color has more.
size_t libretroGbMemoryMap(struct retro_memory_descriptor*);
bool libretroGbIsColor(void);
// Sets the cheat codes: Game Genie (HHH-HHH or HHH-HHH-HHH) and GameShark (01VVAAAA), each split by
// '+' or spaces. They stay in force, including after a restart. Returns false if a code is neither.
bool libretroGbSetCheats(const char* const* codes, size_t count);
size_t libretroGbStateSize(void);
bool libretroGbSaveState(void* data, size_t size);
bool libretroGbLoadState(const void* data, size_t size);
// The link cable: the byte this Game Boy shifts out, and a byte shifted in when the other Game Boy
// clocks a transfer.
int libretroGbLinkOut(void);
void libretroGbLinkIn(int byte);
// The serial control register, SC.
int libretroGbLinkControl(void);
// The consoles of a link here, two to four. The functions above work on the selected one (0 to 3).
// Setting how many there are joins their serial ports, by the cable (two) or the Four Player
// Adapter, and restarts their timing. States then hold them all.
void libretroGbSelect(int which);
void libretroGbSetPlayers(int count, bool adapter);
// Runs the consoles for a frame, with each one's buttons, returning each one's picture and sound.
void libretroGbRunPlayers(const uint16_t keys[], const uint32_t* pictures[], const int16_t* samples[], size_t frames[]);

#endif
