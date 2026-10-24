/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LCD_H
#define ARMGBA_LIBRETRO_LCD_H

// The loaded console's screen, for the LCD grid: the gaps between its pixels and the shadow its pixels
// cast, which the standalone frontend draws on the GPU and the core can draw into a larger picture.

#include <math.h>
#include <stdbool.h>
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

// Keeping the Game Boy screens' and the Color's brightness under the grid (lcd.c has why). The caller
// gives the shares of gap, 0 to 1, a square's pixels are blended with, and how many pixels take each,
// and sets stale when they change. The gap to draw instead, a table for each channel (red, green,
// blue) that darkens the picture, and one of each pixel's own gap color for the ones the gap can't
// keep, are solved again when stale or the screen or its colors change, counted in solved. Returns
// whether the screen's brightness is kept.
struct LibretroLcdKeep {
	float shares[256], weights[256];
	int count;
	bool stale;
	int lcd;
	uint32_t colors[5], gap;
	uint8_t table[3][256], gaps[3][256];
	unsigned solved;
};
bool libretroLcdKeep(struct LibretroLcdKeep* keep);

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
// shadow 1.35 dots away), so that is the screen's darkest shade's at the middle of its contrast wheel.
// A pixel's darkness is how far its luminance in linear light, the light it keeps from the reflector,
// is below a sixteenth under the unlit screen's: the first Game Boy's lightest shade, as bright as the
// unlit screen, casts none, and darker shades, as the wheel turns up, cast more. The ways, up and on
// clockwise.
enum { LIBRETRO_LCD_SHADOW = 51 };

// Each channel's share of an XRGB8888 color's luminance, in linear light, white 65535.
extern uint16_t libretroLcdLuminance[3][256];

static inline int libretroLcdLuminanceOf(uint32_t c) {
	return libretroLcdLuminance[0][c >> 16 & 0xFF] + libretroLcdLuminance[1][c >> 8 & 0xFF] + libretroLcdLuminance[2][c & 0xFF];
}

// Where the shadow of a screen unlit as unlit starts, in luminance, and its scale: a pixel of
// luminance y below start is (start - y) * scale >> 16 dark, LIBRETRO_LCD_SHADOW at the darkest shade.
// Fills libretroLcdLuminance.
void libretroLcdShadowFrom(uint32_t unlit, uint32_t darkest, int* start, int* scale);

// Whether the screen's pixels cast that shadow: the first Game Boy's and the Pocket's, lit from the
// front, but not the Light's, lit from behind.
bool libretroLcdShadow(void);
static const int8_t libretroLcdWays[8][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };

static inline float libretroLcdReach(int way) {
	return libretroLcdWays[way][0] && libretroLcdWays[way][1] ? 1.0f : 1.4f;
}

// How strongly a grid drawn at any size shows when a source pixel is pitch output pixels wide: a
// tenth of the photos' darkness for each, all of it from 10 on. Small, a gap is under a pixel wide
// and lands unevenly; fainter, it reads as the screen's texture rather than as lines.
static inline float libretroLcdFade(float pitch) {
	return fminf(fmaxf(pitch * 0.1f, 0), 1);
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

// The share of a tent two output pixels wide, centered on 0, that lies below u output pixels.
static inline float libretroLcdTentBelow(float u) {
	u = fminf(fmaxf(u, -1), 1);
	return u < 0 ? (u + 1) * (u + 1) / 2 : 1 - (1 - u) * (1 - u) / 2;
}

// libretroLcdGapCoverage seen through a tent two output pixels wide: each gap spreads over the same
// two pixels wherever it lands, so lines under a pixel wide stay even instead of some looking darker.
static inline float libretroLcdGapSpread(int at, float pitch, float gap) {
	float const center = at + 0.5f;
	float covered = 0;
	for (int edge = (int) floorf((center - 1 - gap / 2) / pitch); edge * pitch - gap / 2 < center + 1; ++edge) {
		covered += libretroLcdTentBelow(edge * pitch + gap / 2 - center) - libretroLcdTentBelow(edge * pitch - gap / 2 - center);
	}
	return fminf(covered, 1);
}

#endif
