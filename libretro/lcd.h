/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LCD_H
#define ARMGBA_LIBRETRO_LCD_H

// The loaded console's screen, for the standalone frontend's LCD grid.

#include <stdint.h>

enum LibretroLcd {
	// The first Game Boy's, and the Pocket's and Light's: dark dots, with the liquid crystal between
	// them never driven, so it is the screen's color while off there.
	LIBRETRO_LCD_DMG,
	LIBRETRO_LCD_POCKET,
	// The Game Boy Color's and Advance's: a black matrix between the pixels.
	LIBRETRO_LCD_COLOR,
	LIBRETRO_LCD_ADVANCE,
};

// The screen, and the color between its pixels as XRGB8888.
enum LibretroLcd libretroLcd(uint32_t* gap);

#endif
