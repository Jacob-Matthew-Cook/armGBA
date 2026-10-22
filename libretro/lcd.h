/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LCD_H
#define ARMGBA_LIBRETRO_LCD_H

// The loaded console's screen, for the LCD grid: the gaps between its pixels and the shadow its pixels
// cast, which the standalone frontend draws on the GPU and the core can draw into a larger picture.

#include <math.h>
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

// Each screen's gaps: their width as a share of the pixel pitch, and how much of the gap color shows
// across (between columns) and down (between rows). From photos of the consoles: the first Game Boy's
// are a quarter of the pitch and brighter than its lightest shade; the Advance's darken as much as a
// black line a ninth of the pitch would; the Color's do so between columns, but hardly between rows.
// The Pocket's are thinner than the first Game Boy's, by an estimate.
struct LibretroLcdLook {
	float share, across, down;
};
static const struct LibretroLcdLook libretroLcdLooks[] = {
	[LIBRETRO_LCD_DMG] = { 0.25f, 1, 1 },
	[LIBRETRO_LCD_POCKET] = { 0.15f, 1, 1 },
	[LIBRETRO_LCD_COLOR] = { 0.15f, 0.6f, 0.1f },
	[LIBRETRO_LCD_ADVANCE] = { 0.15f, 0.75f, 0.75f },
};

// The first Game Boy's and the Pocket's pixels' shadow on the reflector behind them: a pixel as dark
// as the screen goes darkens it by a fifth (51 of 255), 1.4 dots away the way chosen (1 dot each way
// on a diagonal), from a Reddit photo of a first Game Boy's screen and Brick Boy's model of it (a
// shadow 1.35 dots away). Darkness counts from a sixteenth below the unlit screen's level, so the
// lightest shade, about a twentieth darker than the unlit screen, casts none. The ways, up and on
// clockwise.
enum { LIBRETRO_LCD_SHADOW = 51 };
static const int8_t libretroLcdWays[8][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };

static inline float libretroLcdReach(int way) {
	return libretroLcdWays[way][0] && libretroLcdWays[way][1] ? 1.0f : 1.4f;
}

// How much of output pixel at, from 0 to 1, falls in the gaps along one direction: each gap is gap
// wide, centered on the edge between two source pixels, which are pitch apart.
static inline float libretroLcdGapCoverage(int at, float pitch, float gap) {
	float covered = 0;
	for (int edge = (int) ceilf((at - gap / 2) / pitch); edge * pitch - gap / 2 < at + 1; ++edge) {
		float const from = fmaxf(edge * pitch - gap / 2, (float) at), to = fminf(edge * pitch + gap / 2, (float) at + 1);
		covered += to > from ? to - from : 0;
	}
	return fminf(covered, 1);
}

#endif
