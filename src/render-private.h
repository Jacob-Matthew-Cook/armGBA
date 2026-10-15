/* Copyright (c) 2013-2015 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_RENDER_PRIVATE_H
#define ARMGBA_RENDER_PRIVATE_H

#include "gba.h"

#include <string.h>

// What render.c shares with its SIMD versions (render-neon.c, render-x86.c): row pixel flags,
// register fields, blending and compositing.

enum {
	// Where the priority and the layer's number sit in a row pixel.
	OFFSET_PRIORITY = 30,
	OFFSET_INDEX = 28,
	// A background counts as on at this value.
	ENABLED_MAX = 4,
	// The sprite tiles start here in VRAM.
	BASE_TILE = 0x10000,
	// The cycles a line has for drawing sprites, with and without H-blank left free for the game.
	OBJ_HBLANK_FREE_LENGTH = 954,
	OBJ_LENGTH = 1210,
	TOTAL_LINES_ = 228,
};

enum {
	// BLDCNT's effects.
	BLEND_NONE = 0,
	BLEND_ALPHA = 1,
	BLEND_BRIGHTEN = 2,
	BLEND_DARKEN = 3,
};

enum {
	// A sprite's mode: ordinary, always a first blend target, or part of the object window's shape.
	OBJ_MODE_NORMAL = 0,
	OBJ_MODE_SEMITRANSPARENT = 1,
	OBJ_MODE_OBJWIN = 2,
};

// The top bits of a row pixel. Lower values are in front, so comparing two pixels' top bits says
// which is seen: priority first, then the layer's number, with sprites ahead of backgrounds of the
// same priority. A pixel nothing has drawn has them all set.
#define FLAG_PRIORITY 0xC0000000u
#define FLAG_INDEX 0x30000000u
#define FLAG_IS_BACKGROUND 0x08000000u
#define FLAG_UNWRITTEN 0xFC000000u
// A sprite pixel that blends with whatever second target ends up under it.
#define FLAG_REBLEND 0x04000000u
// Whether the pixel is a first or a second target of the blend. The sprite layer reuses the second
// target's bit for object window pixels.
#define FLAG_TARGET_1 0x02000000u
#define FLAG_TARGET_2 0x01000000u
#define FLAG_OBJWIN 0x01000000u
// The bits that order two pixels.
#define FLAG_ORDER_MASK 0xF8000000u

// Whether a pixel can still be drawn over or blended with.
#define IS_WRITABLE(PIXEL) ((PIXEL) & 0xFE000000u)

#define COLOR_WHITE 0x7FFF

#define UNLIKELY(X) __builtin_expect(!!(X), 0)
#define LIKELY(X) __builtin_expect(!!(X), 1)

// Width and height by shape and size: square, wide, tall.
static const int objSizes[16][2] = {
	{ 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
	{ 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 },
	{ 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
};

// Register fields
// DISPCNT
#define DISPCNT_MODE(D) ((D) & 7)
#define DISPCNT_FRAME_SELECT(D) ((D) & 0x0010)
#define DISPCNT_HBLANK_FREE(D) ((D) & 0x0020)
#define DISPCNT_OBJ_1D(D) ((D) & 0x0040)
#define DISPCNT_FORCED_BLANK(D) ((D) & 0x0080)
#define DISPCNT_BG_ENABLE(D, N) (((D) >> (8 + (N))) & 1)
#define DISPCNT_OBJ_ENABLE(D) ((D) & 0x1000)
#define DISPCNT_WIN0_ENABLE(D) ((D) & 0x2000)
#define DISPCNT_WIN1_ENABLE(D) ((D) & 0x4000)
#define DISPCNT_OBJWIN_ENABLE(D) ((D) & 0x8000)

// A window's control byte: the layers it shows, and whether blending happens inside it.
#define WIN_BG_ENABLE(W, N) (((W) >> (N)) & 1)
#define WIN_OBJ_ENABLE(W) (((W) >> 4) & 1)
#define WIN_BLEND_ENABLE(W) (((W) >> 5) & 1)

// MOSAIC: each size is one less than the block's.
#define MOSAIC_BG_H(M) ((M) & 0xF)
#define MOSAIC_BG_V(M) (((M) >> 4) & 0xF)
#define MOSAIC_OBJ_H(M) (((M) >> 8) & 0xF)
#define MOSAIC_OBJ_V(M) (((M) >> 12) & 0xF)

// A text background's map entry.
#define TEXT_MAP_TILE(MAP) ((MAP) & 0x03FF)
#define TEXT_MAP_HFLIP(MAP) ((MAP) & 0x0400)
#define TEXT_MAP_VFLIP(MAP) ((MAP) & 0x0800)
#define TEXT_MAP_PALETTE(MAP) (((MAP) & 0xF000) >> 12)

// A sprite's three attribute words. Bit 9 of the first means off for an ordinary sprite, and double
// size for a transformed one.
#define OBJ_A_Y(A) ((A) & 0xFF)
#define OBJ_A_TRANSFORMED(A) (((A) >> 8) & 1)
#define OBJ_A_DISABLE(A) (((A) >> 9) & 1)
#define OBJ_A_DOUBLE_SIZE(A) (((A) >> 9) & 1)
#define OBJ_A_MODE(A) (((A) >> 10) & 3)
#define OBJ_A_MOSAIC(A) (((A) >> 12) & 1)
#define OBJ_A_256(A) (((A) >> 13) & 1)
#define OBJ_A_SHAPE(A) (((A) >> 14) & 3)
#define OBJ_B_X(B) ((B) & 0x1FF)
#define OBJ_B_MAT_INDEX(B) (((B) >> 9) & 0x1F)
#define OBJ_B_HFLIP(B) (((B) >> 12) & 1)
#define OBJ_B_VFLIP(B) (((B) >> 13) & 1)
#define OBJ_B_SIZE(B) (((B) >> 14) & 3)
#define OBJ_C_TILE(C) ((C) & 0x3FF)
#define OBJ_C_PRIORITY(C) (((C) >> 10) & 3)
#define OBJ_C_PALETTE(C) (((C) >> 12) & 0xF)

// Unaligned little-endian loads from guest memory.
static inline uint32_t load16(const uint8_t* base, uint32_t offset) {
	uint16_t value;
	memcpy(&value, base + offset, 2);
	return value;
}

static inline uint32_t load32(const uint8_t* base, uint32_t offset) {
	uint32_t value;
	memcpy(&value, base + offset, 4);
	return value;
}

// Mixes two 15-bit colors with weights in sixteenths, clipping each channel. Green is moved clear
// of red and blue so all three are done in one multiply.
static inline unsigned mix5(int weightA, unsigned colorA, int weightB, unsigned colorB) {
	unsigned a = colorA & 0x7C1F;
	unsigned b = colorB & 0x7C1F;
	a |= (colorA & 0x3E0) << 16;
	b |= (colorB & 0x3E0) << 16;
	unsigned c = (a * weightA + b * weightB) / 16;
	if (c & 0x04000000) {
		c = (c & ~0x07E00000u) | 0x03E00000;
	}
	if (c & 0x0020) {
		c = (c & ~0x003Fu) | 0x001F;
	}
	if (c & 0x8000) {
		c = (c & ~0xF800u) | 0x7C00;
	}
	return (c & 0x7C1F) | ((c >> 16) & 0x03E0);
}

static inline unsigned brighten(unsigned color, int y) {
	unsigned c = 0;
	unsigned a = color & 0x1F;
	c |= (a + ((0x1F - a) * y) / 16) & 0x1F;
	a = color & 0x3E0;
	c |= (a + ((0x3E0 - a) * y) / 16) & 0x3E0;
	a = color & 0x7C00;
	c |= (a + ((0x7C00 - a) * y) / 16) & 0x7C00;
	return c;
}

static inline unsigned darken(unsigned color, int y) {
	unsigned c = 0;
	unsigned a = color & 0x1F;
	c |= (a - (a * y) / 16) & 0x1F;
	a = color & 0x3E0;
	c |= (a - (a * y) / 16) & 0x3E0;
	a = color & 0x7C00;
	c |= (a - (a * y) / 16) & 0x7C00;
	return c;
}

// A row pixel as one 32-bit word, with the color below the flags. On AArch64 the row is two planes,
// which these join and split.
static inline uint32_t rowGet(const struct Renderer* renderer, int x) {
#if defined(__aarch64__)
	return (uint32_t) renderer->rowFlags[x] << 16 | renderer->rowColor[x];
#else
	return renderer->row[x];
#endif
}

static inline void rowSet(struct Renderer* renderer, int x, uint32_t value) {
#if defined(__aarch64__)
	renderer->rowColor[x] = value;
	renderer->rowFlags[x] = value >> 16;
#else
	renderer->row[x] = value;
#endif
}

// Returns the color of a row pixel, as the frame takes it.
static inline uint16_t rowColorAt(const struct Renderer* renderer, int x) {
#if defined(__aarch64__)
	return renderer->rowColor[x];
#else
	return renderer->row[x];
#endif
}

// Compositing: the flags in the top bits order the layers, and target 1 pixels may blend.

static inline void compositeBlendObjwin(struct Renderer* renderer, int x, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			color = mix5(renderer->blda, current, renderer->bldb, color);
		} else {
			color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
		}
	} else {
		color = (color & ~FLAG_TARGET_2) | (current & FLAG_OBJWIN);
	}
	rowSet(renderer, x, color);
}

static inline void compositeBlendNoObjwin(struct Renderer* renderer, int x, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			color = mix5(renderer->blda, current, renderer->bldb, color);
		} else {
			color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
		}
	} else {
		color = color & ~FLAG_TARGET_2;
	}
	rowSet(renderer, x, color);
}

static inline void compositeNoBlendObjwin(struct Renderer* renderer, int x, uint32_t color, uint32_t current) {
	(void) renderer;
	if (color < current) {
		color |= (current & FLAG_OBJWIN);
	} else {
		color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	rowSet(renderer, x, color);
}

static inline void compositeNoBlendNoObjwin(struct Renderer* renderer, int x, uint32_t color, uint32_t current) {
	(void) renderer;
	if (color >= current) {
		color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	rowSet(renderer, x, color);
}

// SIMD versions of the hot paths: AVX2 (chosen at run time) on x86-64, NEON on AArch64.
#if defined(__x86_64__) || defined(__aarch64__)
#define RENDER_SIMD 1
#else
#define RENDER_SIMD 0
#endif
// Mode 4 with mosaic: only AVX2 beats the scalar loop; on the A55, the NEON path was 3-5% slower.
#if defined(__x86_64__)
#define RENDER_SIMD_MOSAIC 1
#else
#define RENDER_SIMD_MOSAIC 0
#endif

bool renderHasSimd(void);
bool renderTextSimd(struct GBA*, struct RenderBackground*, int y);

// One sprite's pixels on a line, after preprocessSprite has worked out where they come from.
struct SpriteRun {
	int outX;
	int condition;
	bool transformed;
	bool wide;
	// Untransformed: the column of the first pixel and its step, and the row.
	int inX;
	int xOffset;
	int inY;
	// Transformed: the accumulators before the first pixel, and their steps.
	int xAccum;
	int yAccum;
	int a;
	int c;
	int width;
	int height;
	int stride;
	unsigned charBase;
	unsigned maskLo;
	unsigned maskHi;
	uint32_t flags;
	// The 32-bit palette, and the offset of this sprite's colors in it.
	const uint32_t* palette;
	int paletteBase;
};
// One row of a sprite, by localX, as SPRITE_DRAW_PIXEL_*_NORMAL reads it: the texel and the bytes
// of its color.
static inline void spriteRow(struct GBA* gba, const struct SpriteRun* run, int localY, uint8_t* texels, uint8_t* colorLow, uint8_t* colorHigh) {
	const uint8_t* tiles = &gba->vram[BASE_TILE];
	int rowBytes = run->wide ? 8 : 4;
	unsigned yBase = (localY & ~7) * run->stride + (localY & 7) * rowBytes + run->maskHi;
	for (int localX = 0; localX < run->width; ++localX) {
		unsigned texel;
		if (!run->wide) {
			unsigned xBase = (localX & ~7) * 4 + ((localX >> 1) & 2);
			texel = (load16(tiles, (yBase + ((xBase + run->charBase) & run->maskLo)) & 0x7FFE) >> ((localX & 3) << 2)) & 0xF;
		} else {
			unsigned xBase = (localX & ~7) * 8 + (localX & 6);
			texel = (load16(tiles, (yBase + ((xBase + run->charBase) & run->maskLo)) & 0x7FFE) >> ((localX & 1) << 3)) & 0xFF;
		}
		uint32_t color = run->palette[run->paletteBase + texel];
		texels[localX] = texel;
		colorLow[localX] = color;
		colorHigh[localX] = color >> 8;
	}
}

void renderSpriteRunSimd(struct GBA*, const struct SpriteRun*);
void renderMode4Simd(struct GBA*, struct RenderBackground*, int32_t x, int32_t y, const uint8_t* picked);
void renderAffineSimd(struct GBA*, struct RenderBackground*, int32_t x, int32_t y);
void renderSpritesSimd(struct GBA*, unsigned priority, int from, int to);
void renderBlendBackdropSimd(struct GBA*, int x, int end, uint32_t backdrop);

#endif
