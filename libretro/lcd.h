/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LCD_H
#define ARMGBA_LIBRETRO_LCD_H

// The loaded console's screen, for the standalone frontend's LCD grid.

#include <stdint.h>

enum LibretroLcd {
	// The first Game Boy's and the Pocket's and Light's: dark dots, with the screen's own unlit color
	// between them.
	LIBRETRO_LCD_STN,
	// The Game Boy Color's and Advance's: a black matrix between the pixels.
	LIBRETRO_LCD_TFT,
};

// The screen, and the color between its pixels as XRGB8888.
enum LibretroLcd libretroLcd(uint32_t* gap);

#endif
