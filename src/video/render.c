/* Copyright (c) 2013-2015 Jeffrey Pfau
 * Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mGBA's software renderer (video-software.c, software-bg.c, software-mode0.c, software-obj.c,
// common.c), kept line for line in BGR555 so its frames are the reference the faster paths must
// match.

#include "render-private.h"

#include <stdio.h>
#include <stdlib.h>

// A line is built in the row, one 32-bit word per pixel: the color in the low bits, and above it
// the flags that decide what may draw over it (priority, which layer, whether it is a blend
// target). The words are stored as two planes, colors and top halves, which rowGet and rowSet join
// and split. Layers are drawn front to back, so a pixel is written only where nothing in front has
// been drawn.
//
// Draws one background pixel of a 16-color tile into the row where the object window applies: the
// window picks the palette and flags, and may hide the pixel.
#define COMPOSITE_16_OBJWIN(BLEND, IDX) \
	if (background->objwinForceEnable || (!(current & FLAG_OBJWIN)) == background->objwinOnly) { \
		unsigned color; \
		unsigned mergedFlags = flags; \
		if (current & FLAG_OBJWIN) { \
			mergedFlags = objwinFlags; \
			color = objwinPalette[paletteData | pixelData]; \
		} else if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = normalPalette[paletteData | pixelData]; \
		} else { \
			color = palette[paletteData | pixelData]; \
		} \
		composite##BLEND##Objwin(renderer, pixel + IDX, color | mergedFlags, current); \
	}

// The same, with no object window to consult.
#define COMPOSITE_16_NO_OBJWIN(BLEND, IDX) \
	{ \
		unsigned color; \
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = normalPalette[paletteData | pixelData]; \
		} else { \
			color = palette[paletteData | pixelData]; \
		} \
		composite##BLEND##NoObjwin(renderer, pixel + IDX, color | flags, current); \
	}

// And the two for 256-color tiles, whose pixel is the palette index itself.
#define COMPOSITE_256_OBJWIN(BLEND, IDX) \
	if (background->objwinForceEnable || (!(current & FLAG_OBJWIN)) == background->objwinOnly) { \
		unsigned color; \
		unsigned mergedFlags = flags; \
		if (current & FLAG_OBJWIN) { \
			mergedFlags = objwinFlags; \
			color = objwinPalette[pixelData]; \
		} else if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = normalPalette[pixelData]; \
		} else { \
			color = palette[pixelData]; \
		} \
		composite##BLEND##Objwin(renderer, pixel + IDX, color | mergedFlags, current); \
	}

#define COMPOSITE_256_NO_OBJWIN(BLEND, IDX) \
	{ \
		unsigned color; \
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = normalPalette[pixelData]; \
		} else { \
			color = palette[pixelData]; \
		} \
		composite##BLEND##NoObjwin(renderer, pixel + IDX, color | flags, current); \
	}

// Takes the next pixel of the tile row in tileData, lowest bits first, and draws it if it isn't
// color 0 and the row's pixel is still free.
#define BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, IDX) \
	pixelData = tileData & 0xF; \
	current = rowGet(renderer, pixel + IDX); \
	if (pixelData && IS_WRITABLE(current)) { \
		COMPOSITE_16_##OBJWIN(BLEND, IDX); \
	} \
	tileData >>= 4;

#define BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, IDX) \
	pixelData = tileData & 0xFF; \
	current = rowGet(renderer, pixel + IDX); \
	if (pixelData && IS_WRITABLE(current)) { \
		COMPOSITE_256_##OBJWIN(BLEND, IDX); \
	} \
	tileData >>= 8;

// Whether pixels have to consult the object window, and the palette pixels inside it use: the
// brightened or darkened one when the window allows the effect on this background.
#define PREPARE_OBJWIN \
	int objwinSlowPath = DISPCNT_OBJWIN_ENABLE(renderer->dispcnt); \
	const uint16_t* objwinPalette = normalPalette; \
	(void) objwinPalette; \
	if (objwinSlowPath) { \
		if (background->target1 && WIN_BLEND_ENABLE(renderer->objwin.packed) && \
		    (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN)) { \
			objwinPalette = renderer->variantPalette; \
		} \
	}

// Sets up the start of an affine or bitmap background's line: its texture coordinates at the left
// edge in 8.8 fixed point, pulled back to the top-left of the mosaic block when mosaic is on, and
// its palettes.
#define BACKGROUND_BITMAP_INIT \
	int32_t x = background->sx + (renderer->start - 1) * background->dx; \
	int32_t y = background->sy + (renderer->start - 1) * background->dy; \
	int mosaicH = 0; \
	int mosaicWait = 0; \
	int32_t localX = 0; \
	int32_t localY = 0; \
	if (background->mosaic) { \
		int mosaicV = MOSAIC_BG_V(renderer->mosaic) + 1; \
		mosaicH = MOSAIC_BG_H(renderer->mosaic) + 1; \
		mosaicWait = (mosaicH - renderer->start + SCREEN_WIDTH * mosaicH) % mosaicH; \
		int32_t startX = renderer->start - (renderer->start % mosaicH); \
		--mosaicH; \
		localX = -(inY % mosaicV) * background->dmx; \
		localY = -(inY % mosaicV) * background->dmy; \
		x += localX; \
		y += localY; \
		localX += background->sx + startX * background->dx; \
		localY += background->sy + startX * background->dy; \
	} \
	uint32_t flags = background->flags; \
	uint32_t objwinFlags = background->objwinFlags; \
	bool variant = background->variant; \
	const uint16_t* normalPalette = (const uint16_t*) gba->palette; \
	const uint16_t* palette = normalPalette; \
	if (variant) { \
		palette = renderer->variantPalette; \
	} \
	(void) palette; \
	(void) objwinFlags; \
	PREPARE_OBJWIN;

// Text backgrounds.

// Returns the map entry for the tile at localX; the map is one, two or four 32x32 screens.
#define BACKGROUND_TEXT_SELECT_CHARACTER \
	xBase = localX & 0xF8; \
	if (background->size & 1) { \
		xBase += (localX & 0x100) << 5; \
	} \
	screenBase = yBase + (xBase >> 3); \
	mapData = load16(vram, screenBase << 1);

// A text background's line is drawn as a partial tile, whole tiles, then another partial tile. This
// draws the left-hand part of a tile cut off by the right edge, for 16 colors.
#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_16(BLEND, OBJWIN) \
	paletteData = TEXT_MAP_PALETTE(mapData) << 4; \
	charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	tileData = load32(vram, charBase); \
	if (!TEXT_MAP_HFLIP(mapData)) { \
		tileData >>= 4 * mod8; \
		for (; outX < end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		for (outX = end - 1; outX >= renderer->start; --outX) { \
			int pixel = outX; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	}

// The right-hand part of a tile cut off by the left edge.
#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_16(BLEND, OBJWIN) \
	charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	tileData = load32(vram, charBase); \
	paletteData = TEXT_MAP_PALETTE(mapData) << 4; \
	pixel = outX; \
	if (!TEXT_MAP_HFLIP(mapData)) { \
		if (outX < renderer->start) { \
			tileData >>= 4 * (renderer->start - outX); \
			outX = renderer->start; \
			pixel = outX; \
		} \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		tileData >>= 4 * (0x8 - mod8); \
		int end = renderer->end - 8; \
		if (end < -1) { \
			end = -1; \
		} \
		outX = renderer->end - 1; \
		pixel = outX; \
		for (; outX > end; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	}

// The whole line with horizontal mosaic: each block of pixels repeats its first pixel.
#define DRAW_BACKGROUND_MODE_0_MOSAIC_16(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			paletteData = TEXT_MAP_PALETTE(mapData) << 4; \
			tileData = load32(vram, charBase); \
			if (!TEXT_MAP_HFLIP(mapData)) { \
				tileData >>= 4 * baseX; \
			} else { \
				tileData >>= 4 * (7 - baseX); \
			} \
			tileData &= 0xF; \
			tileData |= tileData << 4; \
			tileData |= tileData << 8; \
			tileData |= tileData << 16; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					paletteData = TEXT_MAP_PALETTE(mapData) << 4; \
					tileData = load32(vram, charBase); \
					if (!TEXT_MAP_HFLIP(mapData)) { \
						tileData >>= x * 4; \
					} else { \
						tileData >>= (7 - x) * 4; \
					} \
					tileData &= 0xF; \
					tileData |= tileData << 4; \
					tileData |= tileData << 8; \
					tileData |= tileData << 16; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

// The whole tiles in between, eight pixels at a time; a flipped tile is read from its other end.
#define DRAW_BACKGROUND_MODE_0_TILES_16(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		paletteData = TEXT_MAP_PALETTE(mapData) << 4; \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		if (!TEXT_MAP_HFLIP(mapData)) { \
			tileData = load32(vram, charBase); \
		} else { \
			tileData = __builtin_bswap32(load32(vram, charBase)); \
			tileData = ((tileData & 0xF0F0F0F0) >> 4) | ((tileData & 0x0F0F0F0F) << 4); \
		} \
		if (tileData) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 1); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 2); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 3); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 4); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 5); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 6); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 7); \
		} \
		pixel += 8; \
	}

// The same four for 256-color tiles, which take two words per row.
#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	int end2 = end - 4; \
	if (!TEXT_MAP_HFLIP(mapData)) { \
		int shift = inX & 0x3; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > outX) { \
				tileData = load32(vram, charBase); \
				tileData >>= 8 * shift; \
				shift = 0; \
				for (; outX < end2; ++outX, ++pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
			} \
		} \
		if (LIKELY(charBase < 0x10000)) { \
			tileData = load32(vram, charBase + 4); \
			tileData >>= 8 * shift; \
			for (; outX < end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
	} else { \
		int start = outX; \
		outX = end - 1; \
		pixel = outX; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > start) { \
				tileData = load32(vram, charBase); \
				for (; outX >= end2; --outX, --pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
				charBase += 4; \
			} \
		} \
		if (LIKELY(charBase < 0x10000)) { \
			tileData = load32(vram, charBase); \
			for (; outX >= renderer->start; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
		outX = end; \
		pixel = outX; \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	if (UNLIKELY(charBase >= 0x10000)) { \
		return; \
	} \
	int end = mod8 - 4; \
	pixel = outX; \
	if (!TEXT_MAP_HFLIP(mapData)) { \
		if (end > 0) { \
			tileData = load32(vram, charBase); \
			for (; outX < renderer->end - end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			charBase += 4; \
		} \
		tileData = load32(vram, charBase); \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
	} else { \
		int shift = (8 - mod8) & 0x3; \
		int start = outX; \
		outX = renderer->end - 1; \
		pixel = outX; \
		if (end > 0) { \
			tileData = load32(vram, charBase); \
			tileData >>= 8 * shift; \
			for (; outX >= start + 4; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			shift = 0; \
		} \
		tileData = load32(vram, charBase + 4); \
		tileData >>= 8 * shift; \
		for (; outX >= start; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_TILES_256(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		if (!TEXT_MAP_HFLIP(mapData)) { \
			tileData = load32(vram, charBase); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
			tileData = load32(vram, charBase + 4); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
		} else { \
			tileData = load32(vram, charBase + 4); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
			tileData = load32(vram, charBase); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_MOSAIC_256(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			tileData = load32(vram, charBase); \
			if (!TEXT_MAP_HFLIP(mapData)) { \
				if (x >= 4) { \
					tileData = load32(vram, charBase + 4); \
					tileData >>= (x - 4) * 8; \
				} else { \
					tileData = load32(vram, charBase); \
					tileData >>= x * 8; \
				} \
			} else { \
				if (x >= 4) { \
					tileData = load32(vram, charBase); \
					tileData >>= (7 - x) * 8; \
				} else { \
					tileData = load32(vram, charBase + 4); \
					tileData >>= (3 - x) * 8; \
				} \
			} \
			tileData &= 0xFF; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					if (!TEXT_MAP_HFLIP(mapData)) { \
						if (x >= 4) { \
							tileData = load32(vram, charBase + 4); \
							tileData >>= (x - 4) * 8; \
						} else { \
							tileData = load32(vram, charBase); \
							tileData >>= x * 8; \
						} \
					} else { \
						if (x >= 4) { \
							tileData = load32(vram, charBase); \
							tileData >>= (7 - x) * 8; \
						} else { \
							tileData = load32(vram, charBase + 4); \
							tileData >>= (3 - x) * 8; \
						} \
					} \
					tileData &= 0xFF; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			tileData |= tileData << 8; \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

// Draws a text background's line for one color depth, one blend setting and one object window
// setting. The compiler makes a loop for each combination, so none of them tests those per pixel.
#define DRAW_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN) \
	int pixel = outX; \
	if (background->mosaic && MOSAIC_BG_H(renderer->mosaic)) { \
		int mosaicH = MOSAIC_BG_H(renderer->mosaic) + 1; \
		int x; \
		int mosaicWait = (mosaicH - outX + SCREEN_WIDTH * mosaicH) % mosaicH; \
		int carryData = 0; \
		paletteData = 0; \
		DRAW_BACKGROUND_MODE_0_MOSAIC_##BPP(BLEND, OBJWIN) \
		return; \
	} \
	if (inX & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = inX & 0x7; \
		int end = outX + 0x8 - mod8; \
		if (end > renderer->end) { \
			end = renderer->end; \
		} \
		if (UNLIKELY(end == outX)) { \
			return; \
		} \
		if (UNLIKELY(end < outX)) { \
			return; \
		} \
		DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_##BPP(BLEND, OBJWIN) \
		outX = end; \
		if (tileX < tileEnd) { \
			++tileX; \
		} \
		length -= end - renderer->start; \
	} \
	pixel = outX; \
	outX += (tileEnd - tileX) * 8; \
	localX = (tileX * 8 + inX) & 0x1FF; \
	DRAW_BACKGROUND_MODE_0_TILES_##BPP(BLEND, OBJWIN) \
	if (length & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = length & 0x7; \
		DRAW_BACKGROUND_MODE_0_TILE_PREFIX_##BPP(BLEND, OBJWIN) \
	}

#if RENDER_SIMD
static bool useSimd;
static bool checkFast;
#endif

// ARMGBA_RENDER=reference draws with mGBA's loops alone; =check draws both ways and compares.
void renderInit(void) {
#if RENDER_SIMD
	const char* mode = getenv("ARMGBA_RENDER");
	bool reference = mode && !strcmp(mode, "reference");
	checkFast = mode && !strcmp(mode, "check");
	useSimd = !reference && renderHasSimd();
#endif
}

static void drawBackgroundMode0Reference(struct GBA* gba, struct RenderBackground* background, int y);

#if RENDER_SIMD
// For ARMGBA_RENDER=check: saves the row as 32-bit pixels, restores it, and compares it over [from,
// to).
static void rowSave(const struct Renderer* renderer, uint32_t* out) {
	for (int x = 0; x < SCREEN_WIDTH; ++x) {
		out[x] = rowGet(renderer, x);
	}
}

static void rowRestore(struct Renderer* renderer, const uint32_t* in) {
	for (int x = 0; x < SCREEN_WIDTH; ++x) {
		rowSet(renderer, x, in[x]);
	}
}

static bool rowDiffers(const struct Renderer* renderer, const uint32_t* row, int from, int to) {
	for (int x = from; x < to; ++x) {
		if (row[x] != rowGet(renderer, x)) {
			return true;
		}
	}
	return false;
}
#endif

static void drawBackgroundMode4Reference(struct GBA* gba, struct RenderBackground* background, int inY);

// Draws a text background's line: the SIMD path where it applies, otherwise the reference loops.
static void drawBackgroundMode0(struct GBA* gba, struct RenderBackground* background, int y) {
#if RENDER_SIMD
	struct Renderer* renderer = &gba->renderer;
	// Horizontal mosaic needs mGBA's own loop; the 256-color fast path has no object window
	// support.
	bool fast = !(background->mosaic && MOSAIC_BG_H(renderer->mosaic)) && (!DISPCNT_OBJWIN_ENABLE(renderer->dispcnt) || !background->multipalette);
	int fastY = y;
	if (background->mosaic) {
		fastY -= y % (MOSAIC_BG_V(renderer->mosaic) + 1);
	}
	if (fast && useSimd) {
		if (!checkFast) {
			if (renderTextSimd(gba, background, fastY)) {
				return;
			}
		} else {
			uint32_t before[SCREEN_WIDTH];
			uint32_t fastRow[SCREEN_WIDTH];
			rowSave(renderer, before);
			bool handled = renderTextSimd(gba, background, fastY);
			rowSave(renderer, fastRow);
			rowRestore(renderer, before);
			drawBackgroundMode0Reference(gba, background, y);
			if (handled && rowDiffers(renderer, fastRow, 0, SCREEN_WIDTH)) {
				for (int x = 0; x < SCREEN_WIDTH; ++x) {
					if (fastRow[x] != rowGet(renderer, x)) {
						fprintf(stderr, "text bg %d line %d x %d [%d,%d): fast %08X reference %08X (was %08X), bg x %d size %d 256 %d flags %08X variant %d\n",
						        background->index, y, x, renderer->start, renderer->end, fastRow[x], rowGet(renderer, x), before[x],
						        background->x, background->size, background->multipalette, background->flags, background->variant);
						abort();
					}
				}
			}
			return;
		}
	}
#endif
	drawBackgroundMode0Reference(gba, background, y);
}

// mGBA's loops for a text background: finds the map row and picks the specialized loop.
static void drawBackgroundMode0Reference(struct GBA* gba, struct RenderBackground* background, int y) {
	struct Renderer* renderer = &gba->renderer;
	int inX = (renderer->start + background->x) & 0x1FF;
	int length = renderer->end - renderer->start;
	if (background->mosaic) {
		int mosaicV = MOSAIC_BG_V(renderer->mosaic) + 1;
		y -= y % mosaicV;
	}
	int inY = y + background->y;
	uint16_t mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);

	int localX;
	int localY;
	unsigned xBase;

	uint32_t flags = background->flags;
	uint32_t objwinFlags = background->objwinFlags;
	bool variant = background->variant;
	(void) objwinFlags;

	uint32_t screenBase;
	uint32_t charBase;
	const uint16_t* normalPalette = (const uint16_t*) gba->palette;
	const uint16_t* palette = normalPalette;
	if (variant) {
		palette = renderer->variantPalette;
	}
	PREPARE_OBJWIN;

	int outX = renderer->start;

	uint32_t tileData;
	uint32_t current;
	int pixelData;
	int paletteData;
	int tileX;
	int tileEnd = ((length + inX) >> 3) - (inX >> 3);
	const uint8_t* vram = gba->vram;

	if (background->yCache != inY >> 3) {
		localX = 0;
		for (tileX = 0; tileX < 64; ++tileX, localX += 8) {
			BACKGROUND_TEXT_SELECT_CHARACTER;
			background->mapCache[tileX] = mapData;
		}
		background->yCache = inY >> 3;
	}

	tileX = 0;
	if (!objwinSlowPath) {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, NoBlend, NO_OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, NoBlend, NO_OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, Blend, NO_OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, Blend, NO_OBJWIN);
			}
		}
	} else {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, NoBlend, OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, NoBlend, OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, Blend, OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, Blend, OBJWIN);
			}
		}
	}
}

// Affine and bitmap backgrounds.

// One step along an affine line: moves on by the matrix's dx and dy, and skips the pixel where the
// background doesn't wrap and the coordinates fall outside it.
#define BACKGROUND_BITMAP_ITERATE(W, H) \
	x += background->dx; \
	y += background->dy; \
	if ((x < 0 || y < 0 || (x >> 8) >= W || (y >> 8) >= H) && !mosaicWait) { \
		continue; \
	} \
	localX = x; \
	localY = y;

#define MODE_2_COORD_OVERFLOW \
	localX = x & (sizeAdjusted - 1); \
	localY = y & (sizeAdjusted - 1);

#define MODE_2_COORD_NO_OVERFLOW \
	if ((x | y) & ~(sizeAdjusted - 1)) { \
		continue; \
	} \
	localX = x; \
	localY = y;

// Modes 1 and 2's affine tile backgrounds: the map is bytes, and the tiles are 256 colors.
#define MODE_2_NO_MOSAIC(COORD) \
	COORD \
	mapData = screenBase[(localX >> 11) + (((localY >> 7) & 0x7F0) << background->size)]; \
	pixelData = charBase[(mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8)];

// With mosaic, the pixel is looked up only at the start of each block.
#define MODE_2_MOSAIC(COORD) \
	if (!mosaicWait) { \
		MODE_2_NO_MOSAIC(COORD) \
		mosaicWait = mosaicH; \
	} else { \
		--mosaicWait; \
	}

#define MODE_2_LOOP(MOSAIC, COORD, BLEND, OBJWIN) \
	for (outX = renderer->start, pixel = outX; outX < renderer->end; ++outX, ++pixel) { \
		x += background->dx; \
		y += background->dy; \
		uint32_t current = rowGet(renderer, pixel); \
		MOSAIC(COORD) \
		if (pixelData && IS_WRITABLE(current)) { \
			COMPOSITE_256_##OBJWIN(BLEND, 0); \
		} \
	}

// The line for one blend setting and one object window setting.
#define DRAW_BACKGROUND_MODE_2(BLEND, OBJWIN) \
	if (background->overflow) { \
		if (mosaicH > 1) { \
			localX &= sizeAdjusted - 1; \
			localY &= sizeAdjusted - 1; \
			MODE_2_NO_MOSAIC(); \
			MODE_2_LOOP(MODE_2_MOSAIC, MODE_2_COORD_OVERFLOW, BLEND, OBJWIN); \
		} else { \
			MODE_2_LOOP(MODE_2_NO_MOSAIC, MODE_2_COORD_OVERFLOW, BLEND, OBJWIN); \
		} \
	} else { \
		if (mosaicH > 1) { \
			if (!((x | y) & ~(sizeAdjusted - 1))) { \
				localX &= sizeAdjusted - 1; \
				localY &= sizeAdjusted - 1; \
				MODE_2_NO_MOSAIC(); \
			} \
			MODE_2_LOOP(MODE_2_MOSAIC, MODE_2_COORD_NO_OVERFLOW, BLEND, OBJWIN); \
		} else { \
			MODE_2_LOOP(MODE_2_NO_MOSAIC, MODE_2_COORD_NO_OVERFLOW, BLEND, OBJWIN); \
		} \
	}

static void drawBackgroundMode2Reference(struct GBA*, struct RenderBackground*, int inY);

// Draws an affine tile background's line: the SIMD path where it applies, otherwise the reference
// loop.
static void drawBackgroundMode2(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	BACKGROUND_BITMAP_INIT;
	(void) flags;
	(void) objwinFlags;
	(void) variant;

#if RENDER_SIMD
	if (useSimd && !objwinSlowPath && mosaicH <= 1) {
		if (!checkFast) {
			renderAffineSimd(gba, background, x, y);
			return;
		}
		uint32_t before[SCREEN_WIDTH];
		uint32_t fastRow[SCREEN_WIDTH];
		rowSave(renderer, before);
		renderAffineSimd(gba, background, x, y);
		rowSave(renderer, fastRow);
		rowRestore(renderer, before);
		drawBackgroundMode2Reference(gba, background, inY);
		if (rowDiffers(renderer, fastRow, 0, SCREEN_WIDTH)) {
			fprintf(stderr, "affine bg %d differs on line %d\n", background->index, inY);
			abort();
		}
		return;
	}
#endif
	(void) mosaicWait;
	(void) localX;
	(void) localY;
	(void) x;
	(void) y;
	drawBackgroundMode2Reference(gba, background, inY);
}

// mGBA's loop for an affine tile background.
static void drawBackgroundMode2Reference(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	int sizeAdjusted = 0x8000 << background->size;

	BACKGROUND_BITMAP_INIT;

	const uint8_t* screenBase = &gba->vram[background->screenBase];
	const uint8_t* charBase = &gba->vram[background->charBase];
	uint8_t mapData;
	uint8_t pixelData = 0;
	int paletteData = 0;
	(void) paletteData;

	int outX;
	int pixel;

	if (!objwinSlowPath) {
		if (!(flags & FLAG_TARGET_2)) {
			DRAW_BACKGROUND_MODE_2(NoBlend, NO_OBJWIN);
		} else {
			DRAW_BACKGROUND_MODE_2(Blend, NO_OBJWIN);
		}
	} else {
		if (!(flags & FLAG_TARGET_2)) {
			DRAW_BACKGROUND_MODE_2(NoBlend, OBJWIN);
		} else {
			DRAW_BACKGROUND_MODE_2(Blend, OBJWIN);
		}
	}
}

// Mode 3: a 240x160 bitmap of 15-bit colors.
static void drawBackgroundMode3(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	BACKGROUND_BITMAP_INIT;

	uint32_t color = normalPalette[0];
	if (mosaicWait && localX >= 0 && localY >= 0 && (localX >> 8) < SCREEN_WIDTH && (localY >> 8) < SCREEN_HEIGHT) {
		color = load16(gba->vram, ((localX >> 8) + (localY >> 8) * SCREEN_WIDTH) << 1);
	}

	int outX;
	int pixel;
	for (outX = renderer->start, pixel = outX; outX < renderer->end; ++outX, ++pixel) {
		BACKGROUND_BITMAP_ITERATE(SCREEN_WIDTH, SCREEN_HEIGHT);

		if (!mosaicWait) {
			color = load16(gba->vram, ((localX >> 8) + (localY >> 8) * SCREEN_WIDTH) << 1);
			mosaicWait = mosaicH;
		} else {
			--mosaicWait;
		}

		uint32_t current = rowGet(renderer, pixel);
		if (!objwinSlowPath || (!(current & FLAG_OBJWIN)) != background->objwinOnly) {
			unsigned mergedFlags = flags;
			if (current & FLAG_OBJWIN) {
				mergedFlags = objwinFlags;
			}
			if (!variant) {
				compositeBlendObjwin(renderer, pixel, color | mergedFlags, current);
			} else if (renderer->blendEffect == BLEND_BRIGHTEN) {
				compositeBlendObjwin(renderer, pixel, brighten(color, renderer->bldy) | mergedFlags, current);
			} else if (renderer->blendEffect == BLEND_DARKEN) {
				compositeBlendObjwin(renderer, pixel, darken(color, renderer->bldy) | mergedFlags, current);
			}
		}
	}
}

// Mode 4: a 240x160 bitmap of palette indices, in one of two pages.
static void drawBackgroundMode4(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	BACKGROUND_BITMAP_INIT;
#if RENDER_SIMD
	if (useSimd && !objwinSlowPath && (!mosaicH || RENDER_SIMD_MOSAIC)) {
		uint8_t picked[SCREEN_WIDTH + 8];
		if (mosaicH) {
			// The colors drawBackgroundMode4Reference's mosaic picks, before compositing them all
			// at once.
			uint32_t offset = DISPCNT_FRAME_SELECT(renderer->dispcnt) ? 0xA000 : 0;
			uint8_t color = 0;
			if (mosaicWait && localX >= 0 && localY >= 0 && (localX >> 8) < SCREEN_WIDTH && (localY >> 8) < SCREEN_HEIGHT) {
				color = gba->vram[offset + (localX >> 8) + (localY >> 8) * SCREEN_WIDTH];
			}
			int32_t px = x;
			int32_t py = y;
			for (int i = 0; i < renderer->end - renderer->start; ++i) {
				px += background->dx;
				py += background->dy;
				picked[i] = 0;
				if ((px < 0 || py < 0 || (px >> 8) >= SCREEN_WIDTH || (py >> 8) >= SCREEN_HEIGHT) && !mosaicWait) {
					continue;
				}
				if (!mosaicWait) {
					color = gba->vram[offset + (px >> 8) + (py >> 8) * SCREEN_WIDTH];
					mosaicWait = mosaicH;
				} else {
					--mosaicWait;
				}
				picked[i] = color;
			}
		}
		const uint8_t* pick = mosaicH ? picked : NULL;
		if (!checkFast) {
			renderMode4Simd(gba, background, x, y, pick);
			return;
		}
		uint32_t before[SCREEN_WIDTH];
		uint32_t fastRow[SCREEN_WIDTH];
		rowSave(renderer, before);
		renderMode4Simd(gba, background, x, y, pick);
		rowSave(renderer, fastRow);
		rowRestore(renderer, before);
		drawBackgroundMode4Reference(gba, background, inY);
		if (rowDiffers(renderer, fastRow, 0, SCREEN_WIDTH)) {
			fprintf(stderr, "mode 4 differs on line %d\n", inY);
			abort();
		}
		return;
	}
#endif
	(void) x;
	(void) y;
	(void) flags;
	(void) mosaicWait;
	(void) localX;
	(void) localY;
	(void) variant;
	drawBackgroundMode4Reference(gba, background, inY);
}

// mGBA's loop for mode 4.
static void drawBackgroundMode4Reference(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	BACKGROUND_BITMAP_INIT;

	uint16_t color = 0;
	uint32_t offset = 0;
	if (DISPCNT_FRAME_SELECT(renderer->dispcnt)) {
		offset = 0xA000;
	}
	if (mosaicWait && localX >= 0 && localY >= 0 && (localX >> 8) < SCREEN_WIDTH && (localY >> 8) < SCREEN_HEIGHT) {
		color = gba->vram[offset + (localX >> 8) + (localY >> 8) * SCREEN_WIDTH];
	}

	int outX;
	int pixel;
	for (outX = renderer->start, pixel = outX; outX < renderer->end; ++outX, ++pixel) {
		BACKGROUND_BITMAP_ITERATE(SCREEN_WIDTH, SCREEN_HEIGHT);

		if (!mosaicWait) {
			color = gba->vram[offset + (localX >> 8) + (localY >> 8) * SCREEN_WIDTH];
			mosaicWait = mosaicH;
		} else {
			--mosaicWait;
		}

		uint32_t current = rowGet(renderer, pixel);
		if (color && IS_WRITABLE(current)) {
			if (!objwinSlowPath) {
				compositeBlendNoObjwin(renderer, pixel, palette[color] | flags, current);
			} else if (background->objwinForceEnable || (!(current & FLAG_OBJWIN)) == background->objwinOnly) {
				const uint16_t* currentPalette = (current & FLAG_OBJWIN) ? objwinPalette : palette;
				unsigned mergedFlags = flags;
				if (current & FLAG_OBJWIN) {
					mergedFlags = objwinFlags;
				}
				compositeBlendObjwin(renderer, pixel, currentPalette[color] | mergedFlags, current);
			}
		}
	}
}

// Mode 5: a 160x128 bitmap of 15-bit colors, in one of two pages.
static void drawBackgroundMode5(struct GBA* gba, struct RenderBackground* background, int inY) {
	struct Renderer* renderer = &gba->renderer;
	BACKGROUND_BITMAP_INIT;

	uint32_t color = normalPalette[0];
	uint32_t offset = 0;
	if (DISPCNT_FRAME_SELECT(renderer->dispcnt)) {
		offset = 0xA000;
	}
	if (mosaicWait && localX >= 0 && localY >= 0 && (localX >> 8) < 160 && (localY >> 8) < 128) {
		color = load16(gba->vram, offset + (localX >> 8) * 2 + (localY >> 8) * 320);
	}

	int outX;
	int pixel;
	for (outX = renderer->start, pixel = outX; outX < renderer->end; ++outX, ++pixel) {
		BACKGROUND_BITMAP_ITERATE(160, 128);

		if (!mosaicWait) {
			color = load16(gba->vram, offset + (localX >> 8) * 2 + (localY >> 8) * 320);
			mosaicWait = mosaicH;
		} else {
			--mosaicWait;
		}

		uint32_t current = rowGet(renderer, pixel);
		if (!objwinSlowPath || (!(current & FLAG_OBJWIN)) != background->objwinOnly) {
			unsigned mergedFlags = flags;
			if (current & FLAG_OBJWIN) {
				mergedFlags = objwinFlags;
			}
			if (!variant) {
				compositeBlendObjwin(renderer, pixel, color | mergedFlags, current);
			} else if (renderer->blendEffect == BLEND_BRIGHTEN) {
				compositeBlendObjwin(renderer, pixel, brighten(color, renderer->bldy) | mergedFlags, current);
			} else if (renderer->blendEffect == BLEND_DARKEN) {
				compositeBlendObjwin(renderer, pixel, darken(color, renderer->bldy) | mergedFlags, current);
			}
		}
	}
}

// Sprites.

// Sprites are drawn into their own layer first, then merged one priority at a time. These loops
// walk a sprite's pixels on the line; DEPTH is its color depth and TYPE says whether it's an
// ordinary sprite or one that defines the object window.
//
// An unscaled sprite, left to right or flipped.
#define SPRITE_NORMAL_LOOP(DEPTH, TYPE) \
	SPRITE_YBASE_##DEPTH(inY); \
	unsigned tileData; \
	for (; outX < condition; ++outX, inX += xOffset) { \
		SPRITE_XBASE_##DEPTH(inX); \
		SPRITE_DRAW_PIXEL_##DEPTH##_##TYPE(inX); \
	}

// A rotated or scaled sprite: each screen pixel is mapped back through the sprite's matrix.
#define SPRITE_TRANSFORMED_LOOP(DEPTH, TYPE) \
	unsigned tileData; \
	unsigned widthMask = ~(width - 1); \
	unsigned heightMask = ~(height - 1); \
	for (; outX < condition; ++outX, ++inX) { \
		xAccum += mat.a; \
		yAccum += mat.c; \
		int localX = xAccum >> 8; \
		int localY = yAccum >> 8; \
		if (localX & widthMask || localY & heightMask) { \
			break; \
		} \
		SPRITE_YBASE_##DEPTH(localY); \
		SPRITE_XBASE_##DEPTH(localX); \
		SPRITE_DRAW_PIXEL_##DEPTH##_##TYPE(localX); \
	}

// Where in VRAM a 16-color sprite's pixel is: tiles are 32 bytes, four bits per pixel.
#define SPRITE_XBASE_16(localX) unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
#define SPRITE_YBASE_16(localY) unsigned yBase = (localY & ~0x7) * stride + (localY & 0x7) * 4 + maskHi;

#define SPRITE_OTHER_FLAGS (FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1)

// Writes one pixel of a 16-color sprite where it comes before what's already there. A transparent
// pixel still passes some of its flags to the pixel under it.
#define SPRITE_DRAW_PIXEL_16_NORMAL(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 3) << 2)) & 0xF; \
	current = renderer->spriteLayer[outX]; \
	if ((current & FLAG_ORDER_MASK) > flags) { \
		if (tileData) { \
			renderer->spriteLayer[outX] = palette[tileData] | flags; \
		} else if (current != FLAG_UNWRITTEN) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

// The same, where the object window decides whether sprites show.
#define SPRITE_DRAW_PIXEL_16_NORMAL_OBJWIN(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 3) << 2)) & 0xF; \
	current = renderer->spriteLayer[outX]; \
	if ((current & FLAG_ORDER_MASK) > flags) { \
		if (tileData) { \
			unsigned color = (rowGet(renderer, outX) & FLAG_OBJWIN) ? objwinPalette[tileData] : palette[tileData]; \
			renderer->spriteLayer[outX] = color | flags; \
		} else if (current != FLAG_UNWRITTEN) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

// A window sprite draws nothing: its pixels mark where the object window is.
#define SPRITE_DRAW_PIXEL_16_OBJWIN(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 3) << 2)) & 0xF; \
	if (tileData) { \
		rowSet(renderer, outX, rowGet(renderer, outX) | FLAG_OBJWIN); \
	} else { \
		current = renderer->spriteLayer[outX]; \
		if (current != FLAG_UNWRITTEN && (current & FLAG_ORDER_MASK) > flags) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

// And the same for 256-color sprites: 64 bytes per tile, one byte per pixel.
#define SPRITE_XBASE_256(localX) unsigned xBase = (localX & ~0x7) * 8 + (localX & 6);
#define SPRITE_YBASE_256(localY) unsigned yBase = (localY & ~0x7) * stride + (localY & 0x7) * 8 + maskHi;

#define SPRITE_DRAW_PIXEL_256_NORMAL(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 1) << 3)) & 0xFF; \
	current = renderer->spriteLayer[outX]; \
	if ((current & FLAG_ORDER_MASK) > flags) { \
		if (tileData) { \
			renderer->spriteLayer[outX] = palette[tileData] | flags; \
		} else if (current != FLAG_UNWRITTEN) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

#define SPRITE_DRAW_PIXEL_256_NORMAL_OBJWIN(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 1) << 3)) & 0xFF; \
	current = renderer->spriteLayer[outX]; \
	if ((current & FLAG_ORDER_MASK) > flags) { \
		if (tileData) { \
			unsigned color = (rowGet(renderer, outX) & FLAG_OBJWIN) ? objwinPalette[tileData] : palette[tileData]; \
			renderer->spriteLayer[outX] = color | flags; \
		} else if (current != FLAG_UNWRITTEN) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

#define SPRITE_DRAW_PIXEL_256_OBJWIN(localX) \
	tileData = load16(vramBase, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE); \
	tileData = (tileData >> ((localX & 1) << 3)) & 0xFF; \
	if (tileData) { \
		rowSet(renderer, outX, rowGet(renderer, outX) | FLAG_OBJWIN); \
	} else { \
		current = renderer->spriteLayer[outX]; \
		if (current != FLAG_UNWRITTEN && (current & FLAG_ORDER_MASK) > flags) { \
			renderer->spriteLayer[outX] = (current & ~SPRITE_OTHER_FLAGS) | (flags & SPRITE_OTHER_FLAGS); \
		} \
	}

#if RENDER_SIMD
// Draws a run of pixels with the SIMD path where there is one, otherwise with the given loop.
#define SPRITE_RUN(RUN, REFERENCE) \
	if (!checkFast) { \
		renderSpriteRunSimd(gba, &RUN); \
	} else { \
		uint32_t before[SCREEN_WIDTH]; \
		uint32_t fastLayer[SCREEN_WIDTH]; \
		memcpy(before, renderer->spriteLayer, sizeof(before)); \
		renderSpriteRunSimd(gba, &RUN); \
		memcpy(fastLayer, renderer->spriteLayer, sizeof(fastLayer)); \
		memcpy(renderer->spriteLayer, before, sizeof(before)); \
		{ REFERENCE } \
		if (memcmp(fastLayer, renderer->spriteLayer, sizeof(fastLayer))) { \
			fprintf(stderr, "sprite run differs: line %d, attributes %04X %04X %04X\n", y, a, b, c); \
			abort(); \
		} \
	}
#endif

struct Matrix {
	int16_t a;
	int16_t b;
	int16_t c;
	int16_t d;
};

// The columns sprites wrote on this line, so only those are cleared for the next.
static void noteSpriteSpan(struct Renderer* renderer, int from, int to) {
	if (from < renderer->spriteFrom) {
		renderer->spriteFrom = from;
	}
	if (to > renderer->spriteTo) {
		renderer->spriteTo = to;
	}
}

static void coverMosaic(struct Renderer* renderer, int from, int to, unsigned priority);

// Draws one sprite's pixels on line y into the sprite layer, as far as the line's budget of sprite
// cycles allows. Returns whether the sprite is on the line.
static int preprocessSprite(struct GBA* gba, const struct RenderSprite* sprite, int y) {
	struct Renderer* renderer = &gba->renderer;
	uint16_t a = sprite->a;
	uint16_t b = sprite->b;
	uint16_t c = sprite->c;
	// A sprite's shape and size fields pick one of twelve sizes, from 8x8 to 64x64.
	int width = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][0];
	int height = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][1];
	int start = renderer->start;
	int end = renderer->end;
	// The flags its pixels carry: priority, whether it is a first blend target (a semi-transparent
	// sprite always is), and whether it is an object window sprite.
	uint32_t flags = (uint32_t) OBJ_C_PRIORITY(c) << OFFSET_PRIORITY;
	flags |= FLAG_TARGET_1 * ((WIN_BLEND_ENABLE(renderer->currentWindow.packed) && renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || OBJ_A_MODE(a) == OBJ_MODE_SEMITRANSPARENT);
	flags |= FLAG_OBJWIN * (OBJ_A_MODE(a) == OBJ_MODE_OBJWIN);
	// Inside one of the rectangular windows, which take precedence, the object window doesn't
	// apply.
	if ((flags & FLAG_OBJWIN) && renderer->currentWindow.priority < renderer->objwin.priority) {
		return 0;
	}
	// X is nine bits, signed.
	int32_t x = (uint32_t) OBJ_B_X(b) << 23;
	x >>= 23;
	const uint8_t* vramBase = &gba->vram[BASE_TILE];
	// Where its tiles are. With two-dimensional mapping, tiles are 32 across, and a 256-color
	// sprite's tile number has its low bit ignored.
	unsigned align = OBJ_A_256(a) && !DISPCNT_OBJ_1D(renderer->dispcnt);
	unsigned charBase = (OBJ_C_TILE(c) & ~align) * 0x20;
	unsigned maskLo = DISPCNT_OBJ_1D(renderer->dispcnt) ? 0x7FFE : 0x3FE;
	unsigned maskHi = DISPCNT_OBJ_1D(renderer->dispcnt) ? 0 : charBase & 0x7C00;
	// The bitmap modes use the first half of the sprite tiles for the bitmap.
	if (DISPCNT_MODE(renderer->dispcnt) >= 3 && OBJ_C_TILE(c) < 512) {
		return 0;
	}

	// Which palette: the brightened or darkened copy if sprites are a target of that effect here.
	int objwinSlowPath = DISPCNT_OBJWIN_ENABLE(renderer->dispcnt) && WIN_BLEND_ENABLE(renderer->objwin.packed) != WIN_BLEND_ENABLE(renderer->currentWindow.packed);
	int variant = renderer->target1Obj &&
	              WIN_BLEND_ENABLE(renderer->currentWindow.packed) &&
	              (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
	// A sprite that may alpha-blend only does so if there is a second target to blend with.
	if (OBJ_A_MODE(a) == OBJ_MODE_SEMITRANSPARENT || (renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || objwinSlowPath) {
		int target2 = renderer->target2Bd;
		target2 |= renderer->bg[0].target2 && renderer->bg[0].enabled;
		target2 |= renderer->bg[1].target2 && renderer->bg[1].enabled;
		target2 |= renderer->bg[2].target2 && renderer->bg[2].enabled;
		target2 |= renderer->bg[3].target2 && renderer->bg[3].enabled;
		if (target2) {
			renderer->forceTarget1 = true;
			flags |= FLAG_REBLEND;
			variant = 0;
		} else {
			flags &= ~FLAG_TARGET_1;
		}
	}

	const uint16_t* palette = &((const uint16_t*) gba->palette)[0x100];
	const uint16_t* objwinPalette = palette;

	if (variant) {
		palette = &renderer->variantPalette[0x100];
		if (WIN_BLEND_ENABLE(renderer->objwin.packed)) {
			objwinPalette = palette;
		}
	}

	// The sprite's row on this line, and the bytes from one tile row to the next.
	int inY = y - (int) OBJ_A_Y(a);
	int stride = DISPCNT_OBJ_1D(renderer->dispcnt) ? (width >> !OBJ_A_256(a)) : 0x80;

	uint32_t current;
	// A rotated or scaled sprite: its matrix is in OAM, shared among sprites, and it may be drawn
	// in a box twice its size.
	if (OBJ_A_TRANSFORMED(a)) {
		int totalWidth = width << OBJ_A_DOUBLE_SIZE(a);
		int totalHeight = height << OBJ_A_DOUBLE_SIZE(a);
		struct Matrix mat;
		int matIndex = OBJ_B_MAT_INDEX(b);
		mat.a = (int16_t) load16(renderer->oamSeen, matIndex * 32 + 6);
		mat.b = (int16_t) load16(renderer->oamSeen, matIndex * 32 + 14);
		mat.c = (int16_t) load16(renderer->oamSeen, matIndex * 32 + 22);
		mat.d = (int16_t) load16(renderer->oamSeen, matIndex * 32 + 30);

		if (inY < 0) {
			inY += 256;
		}
		// The sprite's columns inside the span being drawn.
		int outX = x >= start ? x : start;
		int condition = x + totalWidth;
		int inX = outX - x;
		if (end < condition) {
			condition = end;
		}
		// Horizontal mosaic is applied to the whole line of sprites once they are all drawn.
		if (OBJ_A_MOSAIC(a) && MOSAIC_OBJ_H(renderer->mosaic) && !(flags & FLAG_OBJWIN)) {
			coverMosaic(renderer, outX, condition, OBJ_C_PRIORITY(c));
		}
		noteSpriteSpan(renderer, outX, condition);

		// The texture coordinate of the first column, in 8.8 fixed point, measured from the
		// sprite's center.
		int xAccum = mat.a * (inX - 1 - (totalWidth >> 1)) + mat.b * (inY - (totalHeight >> 1)) + (width << 7);
		int yAccum = mat.c * (inX - 1 - (totalWidth >> 1)) + mat.d * (inY - (totalHeight >> 1)) + (height << 7);

		// Clips off pixels before the span.
		if (mat.a) {
			if ((xAccum >> 8) < 0) {
				int32_t diffX = -xAccum - 1;
				int32_t skip = mat.a ? diffX / mat.a : 0;
				xAccum += mat.a * skip;
				yAccum += mat.c * skip;
				outX += skip;
				inX += skip;
			} else if ((xAccum >> 8) >= width) {
				int32_t diffX = (width << 8) - xAccum;
				int32_t skip = mat.a ? diffX / mat.a : 0;
				xAccum += mat.a * skip;
				yAccum += mat.c * skip;
				outX += skip;
				inX += skip;
			}
		}
		if (mat.c) {
			if ((yAccum >> 8) < 0) {
				int32_t diffY = -yAccum - 1;
				int32_t skip = mat.c ? diffY / mat.c : 0;
				xAccum += mat.a * skip;
				yAccum += mat.c * skip;
				outX += skip;
				inX += skip;
			} else if ((yAccum >> 8) >= height) {
				int32_t diffY = (height << 8) - yAccum;
				int32_t skip = mat.c ? diffY / mat.c : 0;
				xAccum += mat.a * skip;
				yAccum += mat.c * skip;
				outX += skip;
				inX += skip;
			}
		}

		if (outX < start || outX >= condition) {
			return 0;
		}

		if (!OBJ_A_256(a)) {
			palette = &palette[OBJ_C_PALETTE(c) << 4];
			if (flags & FLAG_OBJWIN) {
				SPRITE_TRANSFORMED_LOOP(16, OBJWIN);
			} else if (objwinSlowPath) {
				objwinPalette = &objwinPalette[OBJ_C_PALETTE(c) << 4];
				SPRITE_TRANSFORMED_LOOP(16, NORMAL_OBJWIN);
			} else {
#if RENDER_SIMD
				if (useSimd) {
					struct SpriteRun run = { .outX = outX, .condition = condition, .transformed = true, .wide = false, .xAccum = xAccum, .yAccum = yAccum, .a = mat.a, .c = mat.c, .width = width, .height = height, .stride = stride, .charBase = charBase, .maskLo = maskLo, .maskHi = maskHi, .flags = flags, .palette = variant ? renderer->variant32 : renderer->normal32, .paletteBase = 0x100 + (OBJ_C_PALETTE(c) << 4) };
					SPRITE_RUN(run, SPRITE_TRANSFORMED_LOOP(16, NORMAL););
				} else
#endif
				{
					SPRITE_TRANSFORMED_LOOP(16, NORMAL);
				}
			}
		} else {
			if (flags & FLAG_OBJWIN) {
				SPRITE_TRANSFORMED_LOOP(256, OBJWIN);
			} else if (objwinSlowPath) {
				SPRITE_TRANSFORMED_LOOP(256, NORMAL_OBJWIN);
			} else {
#if RENDER_SIMD
				if (useSimd) {
					struct SpriteRun run = { .outX = outX, .condition = condition, .transformed = true, .wide = true, .xAccum = xAccum, .yAccum = yAccum, .a = mat.a, .c = mat.c, .width = width, .height = height, .stride = stride, .charBase = charBase, .maskLo = maskLo, .maskHi = maskHi, .flags = flags, .palette = variant ? renderer->variant32 : renderer->normal32, .paletteBase = 0x100 };
					SPRITE_RUN(run, SPRITE_TRANSFORMED_LOOP(256, NORMAL););
				} else
#endif
				{
					SPRITE_TRANSFORMED_LOOP(256, NORMAL);
				}
			}
		}
	} else {
		int outX = x >= start ? x : start;
		int condition = x + width;
		// Horizontal mosaic is applied to the whole line of sprites once they are all drawn.
		if (OBJ_A_MOSAIC(a) && MOSAIC_OBJ_H(renderer->mosaic) && !(flags & FLAG_OBJWIN)) {
			coverMosaic(renderer, outX, condition < end ? condition : end, OBJ_C_PRIORITY(c));
		}
		if ((int) OBJ_A_Y(a) + height - 256 >= 0) {
			inY += 256;
		}
		if (OBJ_B_VFLIP(b)) {
			inY = height - inY - 1;
		}
		if (end < condition) {
			condition = end;
		}
		noteSpriteSpan(renderer, outX, condition);
		int inX = outX - x;
		int xOffset = 1;
		if (OBJ_B_HFLIP(b)) {
			inX = width - inX - 1;
			xOffset = -1;
		}
		if (!OBJ_A_256(a)) {
			palette = &palette[OBJ_C_PALETTE(c) << 4];
			if (flags & FLAG_OBJWIN) {
				SPRITE_NORMAL_LOOP(16, OBJWIN);
			} else if (objwinSlowPath) {
				objwinPalette = &objwinPalette[OBJ_C_PALETTE(c) << 4];
				SPRITE_NORMAL_LOOP(16, NORMAL_OBJWIN);
			} else {
#if RENDER_SIMD
				if (useSimd) {
					struct SpriteRun run = { .outX = outX, .condition = condition, .transformed = false, .wide = false, .inX = inX, .xOffset = xOffset, .inY = inY, .stride = stride, .charBase = charBase, .maskLo = maskLo, .maskHi = maskHi, .flags = flags, .palette = variant ? renderer->variant32 : renderer->normal32, .paletteBase = 0x100 + (OBJ_C_PALETTE(c) << 4) };
					SPRITE_RUN(run, SPRITE_NORMAL_LOOP(16, NORMAL););
				} else
#endif
				{
					SPRITE_NORMAL_LOOP(16, NORMAL);
				}
			}
		} else {
			if (flags & FLAG_OBJWIN) {
				SPRITE_NORMAL_LOOP(256, OBJWIN);
			} else if (objwinSlowPath) {
				SPRITE_NORMAL_LOOP(256, NORMAL_OBJWIN);
			} else {
#if RENDER_SIMD
				if (useSimd) {
					struct SpriteRun run = { .outX = outX, .condition = condition, .transformed = false, .wide = true, .inX = inX, .xOffset = xOffset, .inY = inY, .stride = stride, .charBase = charBase, .maskLo = maskLo, .maskHi = maskHi, .flags = flags, .palette = variant ? renderer->variant32 : renderer->normal32, .paletteBase = 0x100 };
					SPRITE_RUN(run, SPRITE_NORMAL_LOOP(256, NORMAL););
				} else
#endif
				{
					SPRITE_NORMAL_LOOP(256, NORMAL);
				}
			}
		}
	}
	return 1;
}

// Merges the sprite layer's pixels of one priority into the row, under the window's rules.
static void postprocessSprite(struct GBA* gba, unsigned priority) {
	struct Renderer* renderer = &gba->renderer;
	int x;
	int pixel = renderer->start;
	uint32_t flags = FLAG_TARGET_2 * renderer->target2Obj;

	int objwinSlowPath = DISPCNT_OBJWIN_ENABLE(renderer->dispcnt);
	bool objwinDisable = false;
	bool objwinOnly = false;
	if (objwinSlowPath) {
		objwinDisable = !WIN_OBJ_ENABLE(renderer->objwin.packed);
		objwinOnly = !objwinDisable && !WIN_OBJ_ENABLE(renderer->currentWindow.packed);
		if (objwinDisable && !WIN_OBJ_ENABLE(renderer->currentWindow.packed)) {
			return;
		}

		if (objwinDisable) {
			for (x = renderer->start; x < renderer->end; ++x, ++pixel) {
				uint32_t color = renderer->spriteLayer[x] & ~FLAG_OBJWIN;
				uint32_t current = rowGet(renderer, pixel);
				if ((color & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && !(current & FLAG_OBJWIN) && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
					compositeBlendObjwin(renderer, pixel, color | flags, current);
				}
			}
			return;
		} else if (objwinOnly) {
			for (x = renderer->start; x < renderer->end; ++x, ++pixel) {
				uint32_t color = renderer->spriteLayer[x] & ~FLAG_OBJWIN;
				uint32_t current = rowGet(renderer, pixel);
				if ((color & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (current & FLAG_OBJWIN) && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
					compositeBlendObjwin(renderer, pixel, color | flags, current);
				}
			}
			return;
		} else {
			for (x = renderer->start; x < renderer->end; ++x, ++pixel) {
				uint32_t color = renderer->spriteLayer[x] & ~FLAG_OBJWIN;
				uint32_t current = rowGet(renderer, pixel);
				if ((color & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
					compositeBlendObjwin(renderer, pixel, color | flags, current);
				}
			}
			return;
		}
	} else if (!WIN_OBJ_ENABLE(renderer->currentWindow.packed)) {
		return;
	}
#if RENDER_SIMD
	if (useSimd) {
		if (!checkFast) {
			renderSpritesSimd(gba, priority, renderer->spriteFrom, renderer->spriteTo);
			return;
		}
		uint32_t before[SCREEN_WIDTH];
		uint32_t fastRow[SCREEN_WIDTH];
		rowSave(renderer, before);
		renderSpritesSimd(gba, priority, renderer->spriteFrom, renderer->spriteTo);
		rowSave(renderer, fastRow);
		rowRestore(renderer, before);
		for (x = renderer->start; x < renderer->end; ++x, ++pixel) {
			uint32_t color = renderer->spriteLayer[x] & ~FLAG_OBJWIN;
			uint32_t current = rowGet(renderer, pixel);
			if ((color & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
				compositeBlendNoObjwin(renderer, pixel, color | flags, current);
			}
		}
		if (rowDiffers(renderer, fastRow, 0, SCREEN_WIDTH)) {
			fprintf(stderr, "sprite merge differs, priority %u [%d,%d)\n", priority, renderer->start, renderer->end);
			abort();
		}
		return;
	}
#endif
	for (x = renderer->start; x < renderer->end; ++x, ++pixel) {
		uint32_t color = renderer->spriteLayer[x] & ~FLAG_OBJWIN;
		uint32_t current = rowGet(renderer, pixel);
		if ((color & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
			compositeBlendNoObjwin(renderer, pixel, color | flags, current);
		}
	}
}

// The sprites OAM describes, as common.c's GBAVideoRendererCleanOAM lists them.
static uint64_t pageRange(uint32_t from, uint32_t size);

// Where a sprite's tiles lie with either mapping: rows of 32 tiles, or one after another.
static uint64_t spritePages(uint16_t a, uint16_t b, uint16_t c) {
	int width = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][0];
	int height = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][1];
	uint32_t base = OBJ_C_TILE(c) * 0x20;
	uint32_t from = base & ~0x3FFu;
	uint32_t to = from + height / 8 * 0x400;
	uint32_t oneD = base + ((width * height) >> !OBJ_A_256(a));
	if (oneD > to) {
		to = oneD;
	}
	if (to > 0x8000) {
		// It wraps around the sprite tiles.
		from = 0;
		to = 0x8000;
	}
	return pageRange(BASE_TILE + from, to - from);
}

// Lists the sprites that are switched on, in OAM order, with the lines each covers.
static int cleanOam(struct GBA* gba, struct RenderSprite* sprites) {
	int oamMax = 0;
	for (int i = 0; i < 128; ++i) {
		uint16_t a = load16(gba->renderer.oamSeen, i * 8);
		uint16_t b = load16(gba->renderer.oamSeen, i * 8 + 2);
		uint16_t c = load16(gba->renderer.oamSeen, i * 8 + 4);
		if (OBJ_A_TRANSFORMED(a) || !OBJ_A_DISABLE(a)) {
			int width = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][0];
			int height = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][1];
			int32_t x = (uint32_t) OBJ_B_X(b) << 23;
			x >>= 23;
			int cycles;
			if (OBJ_A_TRANSFORMED(a)) {
				height <<= OBJ_A_DOUBLE_SIZE(a);
				width <<= OBJ_A_DOUBLE_SIZE(a);
				cycles = 8 + width * 2;
				if (x < 0) {
					cycles += x;
				}
			} else {
				cycles = width - 2;
				if (x < 0) {
					if (x + width < 0) {
						continue;
					}
					cycles += x >> 1;
				}
			}
			if (OBJ_A_Y(a) >= SCREEN_HEIGHT && OBJ_A_Y(a) + height < TOTAL_LINES_) {
				continue;
			}
			if (OBJ_B_X(b) >= SCREEN_WIDTH && OBJ_B_X(b) + width < 512) {
				continue;
			}
			int y = OBJ_A_Y(a);
			if (y + height > 256) {
				y -= 256;
			}
			sprites[oamMax].y = y;
			sprites[oamMax].endY = y + height;
			sprites[oamMax].cycles = cycles;
			sprites[oamMax].a = a;
			sprites[oamMax].b = b;
			sprites[oamMax].c = c;
			sprites[oamMax].index = i;
			sprites[oamMax].pages = spritePages(a, b, c);
			++oamMax;
		}
	}
	return oamMax;
}

// Draws all the sprites on line y into the sprite layer. Returns the priorities that have pixels,
// one bit each. Also records the columns a mosaic sprite covers, its transparent pixels included,
// with the best priority covering each plus one.
static void coverMosaic(struct Renderer* renderer, int from, int to, unsigned priority) {
	for (int x = from; x < to; ++x) {
		uint8_t* cover = &renderer->spriteMosaicCover[x];
		if (!*cover || *cover > priority + 1) {
			*cover = priority + 1;
		}
	}
	if (from < to) {
		renderer->spriteMosaic = true;
	}
}

// Applies sprites' horizontal mosaic to the finished line of sprite pixels, as the hardware does.
// In a column a mosaic sprite covers, the pixel is the one latched at the start of its block, with
// blocks counted from the left of the screen. The latch carries on into a neighbouring mosaic
// sprite unless that one's priority is higher, and starts afresh where mosaic sprites begin after
// other pixels. A transparent pixel is latched too, with its sprite's priority.
static void spriteMosaicLine(struct Renderer* renderer) {
	renderer->spriteMosaic = false;
	int size = MOSAIC_OBJ_H(renderer->mosaic) + 1;
	uint32_t* layer = renderer->spriteLayer;
	uint8_t* cover = renderer->spriteMosaicCover;
	uint32_t latch = 0;
	unsigned latchPriority = 0;
	bool inside = false;
	for (int x = 0; x < SCREEN_WIDTH; ++x) {
		if (!cover[x]) {
			inside = false;
			continue;
		}
		uint32_t pixel = layer[x];
		unsigned priority = pixel != FLAG_UNWRITTEN ? pixel >> OFFSET_PRIORITY : cover[x] - 1u;
		if (!inside || x % size == 0 || priority < latchPriority) {
			latch = pixel;
			latchPriority = priority;
		} else {
			layer[x] = latch;
		}
		inside = true;
	}
	memset(cover, 0, SCREEN_WIDTH);
}

static int preprocessSpriteLayer(struct GBA* gba, int y) {
	struct Renderer* renderer = &gba->renderer;
	int spriteLayers = 0;
	if (DISPCNT_OBJ_ENABLE(renderer->lineDispcnt)) {
		if (renderer->oamDirty) {
			renderer->oamMax = cleanOam(gba, renderer->sprites);
			renderer->oamDirty = false;
			memset(renderer->lineSpriteCount, 0, sizeof(renderer->lineSpriteCount));
			for (int i = 0; i < renderer->oamMax; ++i) {
				int from = renderer->sprites[i].y > 0 ? renderer->sprites[i].y : 0;
				int to = renderer->sprites[i].endY < SCREEN_HEIGHT ? renderer->sprites[i].endY : SCREEN_HEIGHT;
				for (int line = from; line < to; ++line) {
					renderer->lineSprites[line][renderer->lineSpriteCount[line]++] = i;
				}
			}
		}
		int mosaicV = MOSAIC_OBJ_V(renderer->mosaic) + 1;
		int mosaicY = y - (y % mosaicV);
		int lastIndex = 0;
		// Only the sprites on this line. The cycles the others took come from the gaps between OAM
		// indices, so the limit can only be reached sooner, never skipped.
		for (int j = 0; j < renderer->lineSpriteCount[y]; ++j) {
			struct RenderSprite* sprite = &renderer->sprites[renderer->lineSprites[y][j]];
			int localY = y;
			renderer->end = 0;
			renderer->spriteCyclesRemaining -= 2 * (sprite->index - lastIndex);
			lastIndex = sprite->index;
			if (renderer->spriteCyclesRemaining <= 0) {
				break;
			}
			if (OBJ_A_MOSAIC(sprite->a) && mosaicV > 1) {
				localY = mosaicY;
				if (localY < sprite->y && sprite->y < SCREEN_HEIGHT) {
					localY = sprite->y;
				}
				if (localY >= (sprite->endY & 0xFF)) {
					localY = sprite->endY - 1;
				}
			}
			for (int w = 0; w < renderer->nWindows; ++w) {
				renderer->currentWindow = renderer->windows[w].control;
				renderer->start = renderer->end;
				renderer->end = renderer->windows[w].endX;
				if (!WIN_OBJ_ENABLE(renderer->currentWindow.packed) && !DISPCNT_OBJWIN_ENABLE(renderer->dispcnt)) {
					continue;
				}
				int drawn = preprocessSprite(gba, sprite, localY);
				spriteLayers |= drawn << OBJ_C_PRIORITY(sprite->c);
			}
			renderer->spriteCyclesRemaining -= sprite->cycles;
		}
		if (renderer->spriteMosaic) {
			spriteMosaicLine(renderer);
		}
	}
	return spriteLayers;
}

// Windows.

// The line is cut into spans in which the same layers show. This cuts the spans at one of the two
// rectangular windows' edges, and gives the part inside it that window's settings.
static void breakWindowInner(struct Renderer* renderer, const struct RenderWindowN* win) {
	int activeWindow;
	int startX = 0;
	if (win->hEnd > 0) {
		for (activeWindow = 0; activeWindow < renderer->nWindows; ++activeWindow) {
			if (win->hStart < renderer->windows[activeWindow].endX) {
				// Inserts a window before the end of the active window.
				struct RenderWindow oldWindow = renderer->windows[activeWindow];
				if (win->hStart > startX) {
					// And after the start of the active window.
					int nextWindow = renderer->nWindows;
					++renderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						renderer->windows[nextWindow] = renderer->windows[nextWindow - 1];
					}
					renderer->windows[activeWindow].endX = win->hStart;
					++activeWindow;
				}
				renderer->windows[activeWindow].control = win->control;
				renderer->windows[activeWindow].endX = win->hEnd;
				if (win->hEnd >= oldWindow.endX) {
					// Trims off extra windows that were overwritten.
					for (++activeWindow; renderer->nWindows > activeWindow + 1 && win->hEnd >= renderer->windows[activeWindow].endX; ++activeWindow) {
						renderer->windows[activeWindow] = renderer->windows[activeWindow + 1];
						--renderer->nWindows;
					}
				} else {
					++activeWindow;
					int nextWindow = renderer->nWindows;
					++renderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						renderer->windows[nextWindow] = renderer->windows[nextWindow - 1];
					}
					renderer->windows[activeWindow] = oldWindow;
				}
				break;
			}
			startX = renderer->windows[activeWindow].endX;
		}
	}
}

// A window that wraps around the right edge becomes two.
static void breakWindow(struct Renderer* renderer, const struct RenderWindowN* win) {
	if (win->hEnd > SCREEN_WIDTH || win->hEnd < win->hStart) {
		struct RenderWindowN splits[2] = { *win, *win };
		splits[0].hStart = 0;
		splits[1].hEnd = SCREEN_WIDTH;
		breakWindowInner(renderer, &splits[0]);
		breakWindowInner(renderer, &splits[1]);
	} else {
		breakWindowInner(renderer, win);
	}
}

// Works out the flags a background's pixels carry: its priority and index, whether it is a blend
// target, and which palette it uses. Done when a register changes instead of for every pixel.
static void updateFlags(struct Renderer* renderer, struct RenderBackground* background) {
	uint32_t flags = ((uint32_t) background->priority << OFFSET_PRIORITY) | (background->index << OFFSET_INDEX) | FLAG_IS_BACKGROUND;
	if (background->target2) {
		flags |= FLAG_TARGET_2;
	}
	uint32_t objwinFlags = flags;
	if (renderer->blendEffect == BLEND_ALPHA) {
		if (renderer->blda == 0x10 && renderer->bldb == 0) {
			flags &= ~FLAG_TARGET_2;
			objwinFlags &= ~FLAG_TARGET_2;
		} else if (background->target1) {
			if (WIN_BLEND_ENABLE(renderer->currentWindow.packed)) {
				flags |= FLAG_TARGET_1;
			}
			if (WIN_BLEND_ENABLE(renderer->objwin.packed)) {
				objwinFlags |= FLAG_TARGET_1;
			}
		}
	}
	background->flags = flags;
	background->objwinFlags = objwinFlags;
	background->variant = background->target1 && WIN_BLEND_ENABLE(renderer->currentWindow.packed) && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
}

// Before a span is drawn, works out what the current window and the object window allow each
// background.
static void prepareWindow(struct Renderer* renderer) {
	if (DISPCNT_OBJWIN_ENABLE(renderer->dispcnt)) {
		for (int i = 0; i < 4; ++i) {
			renderer->bg[i].objwinForceEnable = WIN_BG_ENABLE(renderer->objwin.packed, i) && WIN_BG_ENABLE(renderer->currentWindow.packed, i);
			renderer->bg[i].objwinOnly = !WIN_BG_ENABLE(renderer->objwin.packed, i);
		}
	}

	switch (DISPCNT_MODE(renderer->dispcnt)) {
	case 0:
		if (renderer->bg[0].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[0]);
		}
		if (renderer->bg[1].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[1]);
		}
		// Fall through.
	case 2:
		if (renderer->bg[3].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[3]);
		}
		// Fall through.
	case 3:
	case 4:
	case 5:
		if (renderer->bg[2].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[2]);
		}
		break;
	case 1:
		if (renderer->bg[0].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[0]);
		}
		if (renderer->bg[1].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[1]);
		}
		if (renderer->bg[2].enabled == ENABLED_MAX) {
			updateFlags(renderer, &renderer->bg[2]);
		}
		break;
	}
}

// A window switches on at the line its top is on and off at its bottom, whatever happens in
// between.
static void stepWindow(struct Renderer* renderer, int y) {
	for (int i = 0; i < 2; ++i) {
		struct RenderWindowN* win = &renderer->winN[i];
		if (y == win->vStart) {
			win->on = true;
		}
		if (y == win->vEnd) {
			win->on = false;
		}
	}
}

// Which backgrounds this line shows.
static void updateDispcnt(struct Renderer* renderer) {
	for (int i = 0; i < 4; ++i) {
		renderer->bg[i].enabled = DISPCNT_BG_ENABLE(renderer->lineDispcnt, i) ? ENABLED_MAX : 0;
	}
}

// The 16-color banks the palette has changed in since variantPalette was made, one bit each.
static uint32_t changedBanks(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	uint32_t banks = 0;
	for (int bank = 0; bank < 32; ++bank) {
		if (memcmp(&gba->palette[bank * 32], &renderer->paletteSeen[bank * 16], 32)) {
			banks |= 1u << bank;
		}
	}
	return banks;
}

// Updates the brightened or darkened copy of the palette in the banks that changed.
static void updatePalettes(struct GBA* gba, uint32_t banks) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* normalPalette = (const uint16_t*) gba->palette;
	for (; banks; banks &= banks - 1) {
		int from = __builtin_ctz(banks) * 16;
		memcpy(&renderer->paletteSeen[from], &normalPalette[from], 32);
		if (renderer->blendEffect == BLEND_BRIGHTEN) {
			for (int i = from; i < from + 16; ++i) {
				renderer->variantPalette[i] = brighten(normalPalette[i], renderer->bldy);
			}
		} else if (renderer->blendEffect == BLEND_DARKEN) {
			for (int i = from; i < from + 16; ++i) {
				renderer->variantPalette[i] = darken(normalPalette[i], renderer->bldy);
			}
		} else {
			memcpy(&renderer->variantPalette[from], &normalPalette[from], 32);
		}
	}
}

// These are separate functions so the compiler knows nothing else is written, and uses wide stores.
#if defined(__ARM_NEON)
static void fill16(uint16_t* restrict p, uint16_t value, int n) {
	for (int i = 0; i < n; ++i) {
		p[i] = value;
	}
}
#endif

static void fill32(uint32_t* restrict p, uint32_t value, int n) {
	for (int i = 0; i < n; ++i) {
		p[i] = value;
	}
}

// Before a line: empties the row and the sprite layer, and brings the palette copy and the spans up
// to date.
static void preprocessBuffer(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	// Sprites wrote only the span noted on the last line drawn.
	int spriteTo = renderer->spriteTo < SCREEN_WIDTH ? renderer->spriteTo : SCREEN_WIDTH;
	if (renderer->spriteFrom < spriteTo) {
		fill32(&renderer->spriteLayer[renderer->spriteFrom], FLAG_UNWRITTEN, spriteTo - renderer->spriteFrom);
	}
	renderer->spriteFrom = SCREEN_WIDTH;
	renderer->spriteTo = 0;

	renderer->windows[0].endX = SCREEN_WIDTH;
	renderer->nWindows = 1;
	if (DISPCNT_WIN0_ENABLE(renderer->dispcnt) || DISPCNT_WIN1_ENABLE(renderer->dispcnt) || DISPCNT_OBJWIN_ENABLE(renderer->dispcnt)) {
		renderer->windows[0].control = renderer->winout;
		if (DISPCNT_WIN1_ENABLE(renderer->dispcnt) && renderer->winN[1].on) {
			breakWindow(renderer, &renderer->winN[1]);
		}
		if (DISPCNT_WIN0_ENABLE(renderer->dispcnt) && renderer->winN[0].on) {
			breakWindow(renderer, &renderer->winN[0]);
		}
	} else {
		renderer->windows[0].control.packed = 0xFF;
	}

	updateDispcnt(renderer);

	// mGBA keeps the brightened or darkened palette current as the palette changes.
	if (renderer->blendDirty || renderer->paletteDirty) {
		uint32_t banks = renderer->blendDirty ? 0xFFFFFFFF : changedBanks(gba);
		updatePalettes(gba, banks);
		renderer->palette32Stale |= banks;
		renderer->blendDirty = false;
		renderer->paletteDirty = false;
	}
	renderer->forceTarget1 = false;

	const uint16_t* normalPalette = (const uint16_t*) gba->palette;
	int x = 0;
	for (int w = 0; w < renderer->nWindows; ++w) {
		uint32_t backdrop = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND;
		if (!renderer->target1Bd || renderer->blendEffect == BLEND_NONE || renderer->blendEffect == BLEND_ALPHA || !WIN_BLEND_ENABLE(renderer->windows[w].control.packed)) {
			backdrop |= normalPalette[0];
		} else {
			backdrop |= renderer->variantPalette[0];
		}
		int end = renderer->windows[w].endX;
		if (end > x) {
#if defined(__ARM_NEON)
			fill16(&renderer->rowColor[x], backdrop, end - x);
			fill16(&renderer->rowFlags[x], backdrop >> 16, end - x);
#else
			fill32(&renderer->row[x], backdrop, end - x);
#endif
			x = end;
		}
	}
}

// Blends the backdrop under the first-target pixels of [from, end), as mGBA's loop does.
static void blendBackdropReference(struct Renderer* renderer, int from, int end, uint32_t backdrop) {
	for (int x = from; x < end; ++x) {
		uint32_t color = rowGet(renderer, x);
		if (color & FLAG_TARGET_1) {
			rowSet(renderer, x, mix5(renderer->bldb, backdrop, renderer->blda, color));
		}
	}
}

// After the layers are drawn: fills the backdrop where nothing drew, blends a top layer with the one
// under it, and turns the row into the frame's colors.
static void postprocessBuffer(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* normalPalette = (const uint16_t*) gba->palette;
	int x;
	int w;
	if ((renderer->forceTarget1 || renderer->bg[0].target1 || renderer->bg[1].target1 || renderer->bg[2].target1 || renderer->bg[3].target1) && renderer->target2Bd) {
		x = 0;
		for (w = 0; w < renderer->nWindows; ++w) {
			uint32_t backdrop = 0;
			if (!renderer->target1Bd || renderer->blendEffect == BLEND_NONE || renderer->blendEffect == BLEND_ALPHA || !WIN_BLEND_ENABLE(renderer->windows[w].control.packed)) {
				backdrop |= normalPalette[0];
			} else {
				backdrop |= renderer->variantPalette[0];
			}
			int end = renderer->windows[w].endX;
#if RENDER_SIMD
			if (useSimd && !checkFast) {
				renderBlendBackdropSimd(gba, x, end, backdrop);
				x = end;
				continue;
			}
			if (useSimd) {
				uint32_t before[SCREEN_WIDTH];
				uint32_t fastRow[SCREEN_WIDTH];
				rowSave(renderer, before);
				renderBlendBackdropSimd(gba, x, end, backdrop);
				rowSave(renderer, fastRow);
				rowRestore(renderer, before);
				blendBackdropReference(renderer, x, end, backdrop);
				if (rowDiffers(renderer, fastRow, x, end)) {
					fprintf(stderr, "backdrop blend differs [%d,%d)\n", x, end);
					abort();
				}
				x = end;
				continue;
			}
#endif
			blendBackdropReference(renderer, x, end, backdrop);
			x = end;
		}
	}
	if (renderer->forceTarget1 && (renderer->blendEffect == BLEND_DARKEN || renderer->blendEffect == BLEND_BRIGHTEN)) {
		x = 0;
		for (w = 0; w < renderer->nWindows; ++w) {
			int end = renderer->windows[w].endX;
			uint32_t mask = FLAG_REBLEND | FLAG_IS_BACKGROUND;
			uint32_t match = FLAG_REBLEND;
			bool objBlend = WIN_BLEND_ENABLE(renderer->objwin.packed);
			bool winBlend = WIN_BLEND_ENABLE(renderer->windows[w].control.packed);
			if (DISPCNT_OBJWIN_ENABLE(renderer->dispcnt) && objBlend != winBlend) {
				mask |= FLAG_OBJWIN;
				if (objBlend) {
					match |= FLAG_OBJWIN;
				}
			} else if (!winBlend) {
				x = end;
				continue;
			}
			if (renderer->blendEffect == BLEND_DARKEN) {
				for (; x < end; ++x) {
					uint32_t color = rowGet(renderer, x);
					if ((color & mask) == match) {
						rowSet(renderer, x, darken(color, renderer->bldy));
					}
				}
			} else if (renderer->blendEffect == BLEND_BRIGHTEN) {
				for (; x < end; ++x) {
					uint32_t color = rowGet(renderer, x);
					if ((color & mask) == match) {
						rowSet(renderer, x, brighten(color, renderer->bldy));
					}
				}
			}
		}
	}
}

// A background draws at its own priority, if it is on and the window, or the object window, shows
// it.
#define TEST_LAYER_ENABLED(X) \
	(renderer->bg[X].enabled == ENABLED_MAX && \
	 (WIN_BG_ENABLE(renderer->currentWindow.packed, X) || \
	  (DISPCNT_OBJWIN_ENABLE(renderer->dispcnt) && WIN_BG_ENABLE(renderer->objwin.packed, X))) && \
	 renderer->bg[X].priority == priority)

#define DIRTY_SCANLINE(R, Y) (R)->scanlineDirty[(Y) >> 5] |= (1U << ((Y) & 0x1F))
#define CLEAN_SCANLINE(R, Y) (R)->scanlineDirty[(Y) >> 5] &= ~(1U << ((Y) & 0x1F))

// RGB565 from BGR555, with green's top bit repeated below it.
static uint16_t toRgb565(uint32_t color) {
	uint32_t g = (color >> 5) & 0x1F;
	return ((color & 0x1F) << 11) | (g << 6) | ((g >> 4) << 5) | ((color >> 10) & 0x1F);
}

// Returns the lines a sprite covers, as cleanOam places it.
static void dirtySpriteLines(struct Renderer* renderer, uint16_t a, uint16_t b) {
	if (!OBJ_A_TRANSFORMED(a) && OBJ_A_DISABLE(a)) {
		return;
	}
	int height = objSizes[OBJ_A_SHAPE(a) * 4 + OBJ_B_SIZE(b)][1];
	if (OBJ_A_TRANSFORMED(a)) {
		height <<= OBJ_A_DOUBLE_SIZE(a);
	}
	int y = OBJ_A_Y(a);
	if (y + height > 256) {
		y -= 256;
	}
	int to = y + height < SCREEN_HEIGHT ? y + height : SCREEN_HEIGHT;
	for (int line = y > 0 ? y : 0; line < to; ++line) {
		DIRTY_SCANLINE(renderer, line);
	}
}

// [from, from + size) in 2 KiB pages.
static uint64_t pageRange(uint32_t from, uint32_t size) {
	uint32_t first = from >> 11;
	uint32_t last = (from + size - 1) >> 11;
	if (last > 63) {
		last = 63;
	}
	if (first > last) {
		return 0;
	}
	return (~0ull >> (63 - last)) & (~0ull << first);
}

// Marks the VRAM pages a line drawn now reads, or more where that is simpler: the map row and the
// characters it names for a text background, whole maps and character blocks for the others, and
// each sprite's tiles.
static uint64_t linePages(struct GBA* gba, int y) {
	struct Renderer* renderer = &gba->renderer;
	const uint8_t* vram = gba->vram;
	int mode = DISPCNT_MODE(renderer->dispcnt);
	uint64_t pages = 0;
	for (int i = 0; i < 4; ++i) {
		struct RenderBackground* bg = &renderer->bg[i];
		if (bg->enabled != ENABLED_MAX || (mode == 1 && i == 3) || (mode >= 2 && i < 2) || (mode >= 3 && i == 3)) {
			continue;
		}
		if (mode >= 3) {
			pages |= pageRange(0, 0x14000);
		} else if (mode == 0 || (mode == 1 && i < 2)) {
			int inY = y;
			if (bg->mosaic) {
				inY -= y % (MOSAIC_BG_V(renderer->mosaic) + 1);
			}
			inY += bg->y;
			// The same row of tiles as last time, with the same layout.
			int32_t key = ((inY & 0x1F8) << 4) | ((bg->x >> 3) & 0x3F);
			if (key == bg->rowKey) {
				pages |= bg->rowPages;
				continue;
			}
			uint64_t row = 0;
			unsigned yBase = inY & 0xF8;
			if (bg->size == 2) {
				yBase += inY & 0x100;
			} else if (bg->size == 3) {
				yBase += (inY & 0x100) << 1;
			}
			yBase = (bg->screenBase >> 1) + (yBase << 2);
			int shift = bg->multipalette ? 6 : 5;
			for (int t = 0; t < 31; ++t) {
				int localX = ((bg->x & ~7) + t * 8) & 0x1FF;
				unsigned xBase = localX & 0xF8;
				if (bg->size & 1) {
					xBase += (localX & 0x100) << 5;
				}
				uint32_t map = (yBase + (xBase >> 3)) << 1;
				uint32_t character = bg->charBase + ((load16(vram, map) & 0x3FF) << shift);
				row |= 1ull << ((map >> 11) & 63);
				row |= 1ull << ((character >> 11) & 63);
			}
			bg->rowKey = key;
			bg->rowPages = row;
			bg->mapPages = pageRange(bg->screenBase, 0x2000);
			pages |= row;
		} else {
			uint32_t side = 16 << bg->size;
			pages |= pageRange(bg->screenBase, side * side) | pageRange(bg->charBase, 0x4000);
		}
	}
	if (DISPCNT_OBJ_ENABLE(renderer->lineDispcnt)) {
		for (int j = 0; j < renderer->lineSpriteCount[y]; ++j) {
			pages |= renderer->sprites[renderer->lineSprites[y][j]].pages;
		}
	}
	return pages;
}

// Called after line y was drawn.
static void notePages(struct GBA* gba, int y, uint64_t pages) {
	struct Renderer* renderer = &gba->renderer;
	uint64_t moved = renderer->linePages[y] ^ pages;
	renderer->linePages[y] = pages;
	while (moved) {
		int page = __builtin_ctzll(moved);
		moved &= moved - 1;
		renderer->pageLines[page][y >> 5] ^= 1U << (y & 0x1F);
	}
}

// The pages VRAM stores changed since last asked.
static uint64_t takeVramChanges(struct Renderer* renderer) {
	uint64_t words[6];
	memcpy(words, renderer->vramChanged, sizeof(words));
	if (!(words[0] | words[1] | words[2] | words[3] | words[4] | words[5])) {
		return 0;
	}
	memset(renderer->vramChanged, 0, sizeof(renderer->vramChanged));
	uint64_t changed = 0;
	for (int w = 0; w < 6; ++w) {
		// Turns eight bytes of 0 or 1 into eight bits.
		changed |= ((words[w] * 0x0102040810204080ull) >> 56) << (w * 8);
	}
	return changed;
}

// Marks dirty the lines that read a changed page, and the row caches whose map may have changed.
static void dirtyVramLines(struct Renderer* renderer, uint64_t changed) {
	for (uint64_t bits = changed; bits; bits &= bits - 1) {
		int page = __builtin_ctzll(bits);
		for (int i = 0; i < 5; ++i) {
			renderer->scanlineDirty[i] |= renderer->pageLines[page][i];
		}
	}
	for (int i = 0; i < 4; ++i) {
		if (renderer->bg[i].mapPages & changed) {
			renderer->bg[i].rowKey = -1;
		}
	}
}

// After only OAM changed, marks dirty the lines of the sprites that changed, both where they were
// and where they are now, and of those using a changed matrix.
static void dirtyChangedSprites(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	uint32_t matrices = 0;
	for (int i = 0; i < 128; ++i) {
		int at = i * 8;
		if (!memcmp(&renderer->oamNext[at], &renderer->oamSeen[at], 8)) {
			continue;
		}
		if (memcmp(&renderer->oamNext[at], &renderer->oamSeen[at], 6)) {
			dirtySpriteLines(renderer, load16(renderer->oamSeen, at), load16(renderer->oamSeen, at + 2));
			dirtySpriteLines(renderer, load16(renderer->oamNext, at), load16(renderer->oamNext, at + 2));
		}
		if (load16(renderer->oamNext, at + 6) != load16(renderer->oamSeen, at + 6)) {
			matrices |= 1u << (i >> 2);
		}
	}
	for (int i = 0; matrices && i < 128; ++i) {
		uint16_t a = load16(renderer->oamNext, i * 8);
		uint16_t b = load16(renderer->oamNext, i * 8 + 2);
		if (OBJ_A_TRANSFORMED(a) && (matrices >> OBJ_B_MAT_INDEX(b) & 1)) {
			dirtySpriteLines(renderer, a, b);
		}
	}
}

// Draws line y, unless nothing it shows has changed since it was last drawn.
void renderScanline(struct GBA* gba, int y) {
	struct Renderer* renderer = &gba->renderer;
	renderer->nextY = y == SCREEN_HEIGHT - 1 ? 0 : y + 1;
	uint16_t latched = renderer->dispcntLatch[0];
	renderer->lineDispcnt = (renderer->dispcnt & ~0x1F80) | (renderer->dispcnt & latched & 0x1F00) | ((renderer->dispcnt | latched) & 0x0080);
	// What changed since the last line: any of it makes the lines that read it dirty.
	uint64_t vramChanged = takeVramChanges(renderer);
	if (vramChanged) {
		dirtyVramLines(renderer, vramChanged);
	}
	// OAM as the previous line left it is this line's; as it is now, it is the next line's. The
	// frame's first line takes OAM as it is, since nothing was drawn above it to delay it.
	for (int pass = 0; pass < 2; ++pass) {
		if (pass) {
			if (!(renderer->memoryChanged & RENDER_OAM)) {
				break;
			}
			memcpy(renderer->oamNext, gba->oam, sizeof(renderer->oamNext));
			renderer->oamPending = true;
			if (y) {
				break;
			}
		}
		if (renderer->oamPending) {
			dirtyChangedSprites(gba);
			renderer->oamDirty = true;
			memcpy(renderer->oamSeen, renderer->oamNext, sizeof(renderer->oamSeen));
			renderer->oamPending = false;
		}
	}
	if (renderer->memoryChanged & RENDER_PALETTE) {
		memset(renderer->scanlineDirty, 0xFF, sizeof(renderer->scanlineDirty));
		renderer->paletteDirty = true;
	}
	renderer->memoryChanged = 0;
	// mGBA drops the map rows whenever VRAM changes; rebuilding them every line gives the same
	// result.
	for (int i = 0; i < 4; ++i) {
		renderer->bg[i].yCache = -1;
	}

	// The line is also dirty if its registers, affine coordinates or windows differ from last time.
	bool dirty = renderer->scanlineDirty[y >> 5] & (1U << (y & 0x1F));
	if (memcmp(renderer->nextIo, renderer->cache[y].io, sizeof(renderer->nextIo))) {
		memcpy(renderer->cache[y].io, renderer->nextIo, sizeof(renderer->nextIo));
		dirty = true;
	}
	if (DISPCNT_MODE(renderer->dispcnt) != 0) {
		if (renderer->cache[y].scale[0][0] != renderer->bg[2].sx ||
		    renderer->cache[y].scale[0][1] != renderer->bg[2].sy ||
		    renderer->cache[y].scale[1][0] != renderer->bg[3].sx ||
		    renderer->cache[y].scale[1][1] != renderer->bg[3].sy) {
			dirty = true;
		}
	}
	renderer->cache[y].scale[0][0] = renderer->bg[2].sx;
	renderer->cache[y].scale[0][1] = renderer->bg[2].sy;
	renderer->cache[y].scale[1][0] = renderer->bg[3].sx;
	renderer->cache[y].scale[1][1] = renderer->bg[3].sy;
	if (renderer->cache[y].lineDispcnt != renderer->lineDispcnt) {
		renderer->cache[y].lineDispcnt = renderer->lineDispcnt;
		dirty = true;
	}

	stepWindow(renderer, y);
	if (renderer->cache[y].windowOn[0] != renderer->winN[0].on || renderer->cache[y].windowOn[1] != renderer->winN[1].on) {
		dirty = true;
	}
	renderer->cache[y].windowOn[0] = renderer->winN[0].on;
	renderer->cache[y].windowOn[1] = renderer->winN[1].on;

	if (!dirty && !renderer->stereoChanges) {
		return;
	}
	CLEAN_SCANLINE(renderer, y);

	uint16_t* out = renderer->frame[y];
	if (DISPCNT_FORCED_BLANK(renderer->lineDispcnt)) {
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			out[x] = renderer->rgb565 ? toRgb565(COLOR_WHITE) : COLOR_WHITE;
		}
		notePages(gba, y, 0);
		return;
	}

	// Sprites first, into their own layer, then each priority from the front: sprites, then
	// backgrounds.
	preprocessBuffer(gba);
	renderer->spriteCyclesRemaining = DISPCNT_HBLANK_FREE(renderer->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
	int spriteLayers = preprocessSpriteLayer(gba, y);

	int mode = DISPCNT_MODE(renderer->dispcnt);
	renderer->end = 0;
	for (int w = 0; w < renderer->nWindows; ++w) {
		renderer->start = renderer->end;
		renderer->end = renderer->windows[w].endX;
		renderer->currentWindow = renderer->windows[w].control;
		prepareWindow(renderer);
		for (unsigned priority = 0; priority < 4; ++priority) {
			if (spriteLayers & (1 << priority)) {
				postprocessSprite(gba, priority);
			}
			if (TEST_LAYER_ENABLED(0) && mode < 2) {
				drawBackgroundMode0(gba, &renderer->bg[0], y);
			}
			if (TEST_LAYER_ENABLED(1) && mode < 2) {
				drawBackgroundMode0(gba, &renderer->bg[1], y);
			}
			if (TEST_LAYER_ENABLED(2)) {
				switch (mode) {
				case 0:
					drawBackgroundMode0(gba, &renderer->bg[2], y);
					break;
				case 1:
				case 2:
					drawBackgroundMode2(gba, &renderer->bg[2], y);
					break;
				case 3:
					drawBackgroundMode3(gba, &renderer->bg[2], y);
					break;
				case 4:
					drawBackgroundMode4(gba, &renderer->bg[2], y);
					break;
				case 5:
					drawBackgroundMode5(gba, &renderer->bg[2], y);
					break;
				}
			}
			if (TEST_LAYER_ENABLED(3)) {
				switch (mode) {
				case 0:
					drawBackgroundMode0(gba, &renderer->bg[3], y);
					break;
				case 2:
					drawBackgroundMode2(gba, &renderer->bg[3], y);
					break;
				}
			}
		}
	}

	postprocessBuffer(gba);
	notePages(gba, y, linePages(gba, y));

	if (renderer->stereoChanges) {
		// Each pixel pair goes out at line cycle 52 + 4x, with green swap as it is at that moment.
		bool stereo = renderer->lineStereo;
		int change = 0;
		for (int x = 0; x < SCREEN_WIDTH; x += 2) {
			while (change < renderer->stereoChanges && renderer->stereoCycle[change] <= 52 + 4 * x) {
				stereo = renderer->stereoValue[change++];
			}
			if (stereo) {
				out[x] = (rowColorAt(renderer, x) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x + 1) & 0x03E0);
				out[x + 1] = (rowColorAt(renderer, x + 1) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x) & 0x03E0);
			} else {
				out[x] = rowColorAt(renderer, x);
				out[x + 1] = rowColorAt(renderer, x + 1);
			}
		}
		if (renderer->rgb565) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				out[x] = toRgb565(out[x]);
			}
		}
	} else if (renderer->stereo) {
		for (int x = 0; x < SCREEN_WIDTH; x += 4) {
			out[x] = (rowColorAt(renderer, x) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x + 1) & 0x03E0);
			out[x + 1] = (rowColorAt(renderer, x + 1) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x) & 0x03E0);
			out[x + 2] = (rowColorAt(renderer, x + 2) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x + 3) & 0x03E0);
			out[x + 3] = (rowColorAt(renderer, x + 3) & (0x001F | 0x7C00)) | (rowColorAt(renderer, x + 2) & 0x03E0);
		}
		if (renderer->rgb565) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				out[x] = toRgb565(out[x]);
			}
		}
	} else if (renderer->rgb565) {
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			out[x] = toRgb565(rowColorAt(renderer, x));
		}
	} else {
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			out[x] = rowColorAt(renderer, x);
		}
	}
}

// At the end of the frame, a window whose top or bottom lies below the screen is switched as if the
// lines down there had been stepped through.
void renderFinishFrame(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	renderer->nextY = 0;
	for (int i = 0; i < 2; ++i) {
		struct RenderWindowN* win = &renderer->winN[i];
		if (win->vEnd >= SCREEN_HEIGHT && win->vEnd < TOTAL_LINES_) {
			win->on = false;
		}
		if (win->vStart >= SCREEN_HEIGHT && win->vStart < TOTAL_LINES_ && win->vStart > win->vEnd) {
			win->on = true;
		}
	}
}

// Registers.

// The register writes that set up the renderer's own copies of the state, masked as they read back.
static void writeBgcnt(struct Renderer* renderer, struct RenderBackground* bg, uint16_t value) {
	bg->priority = value & 3;
	bg->charBase = ((value >> 2) & 3) << 14;
	bg->mosaic = (value >> 6) & 1;
	bg->multipalette = (value >> 7) & 1;
	bg->screenBase = ((value >> 8) & 0x1F) << 11;
	bg->overflow = (value >> 13) & 1;
	bg->size = (value >> 14) & 3;
	bg->yCache = -1;
	bg->rowKey = -1;
	updateFlags(renderer, bg);
}

// An affine reference point is 28 bits, signed, written in halves; the write is noted for the line
// start.
static void writeRefLo(int32_t* ref, bool* written, uint16_t value) {
	*ref = (*ref & 0xFFFF0000) | value;
	*written = true;
}

// The high half, sign-extended from 28 bits.
static void writeRefHi(int32_t* ref, bool* written, uint16_t value) {
	*ref = (*ref & 0x0000FFFF) | ((uint32_t) value << 16);
	*ref = (int32_t) ((uint32_t) *ref << 4) >> 4;
	*written = true;
}

// Which layers blend, and how. A change of effect means the palette copy has to be made again.
static void writeBldcnt(struct Renderer* renderer, uint16_t value) {
	int oldEffect = renderer->blendEffect;
	for (int i = 0; i < 4; ++i) {
		renderer->bg[i].target1 = (value >> i) & 1;
		renderer->bg[i].target2 = (value >> (8 + i)) & 1;
	}
	renderer->blendEffect = (value >> 6) & 3;
	renderer->target1Obj = (value >> 4) & 1;
	renderer->target1Bd = (value >> 5) & 1;
	renderer->target2Obj = (value >> 12) & 1;
	renderer->target2Bd = (value >> 13) & 1;
	if (oldEffect != renderer->blendEffect) {
		renderer->blendDirty = true;
	}
}

// A window's left and right edges, with the hardware's handling of edges past the screen.
static void writeWindowH(struct RenderWindowN* win, uint16_t value) {
	win->hEnd = value;
	win->hStart = value >> 8;
	if (win->hStart > SCREEN_WIDTH && win->hStart > win->hEnd) {
		win->hStart = 0;
	}
	if (win->hEnd > SCREEN_WIDTH) {
		win->hEnd = SCREEN_WIDTH;
		if (win->hStart > SCREEN_WIDTH) {
			win->hStart = SCREEN_WIDTH;
		}
	}
}

// Handles a write to a video register, and returns the value as it reads back. The cases are the
// registers by their offset in the I/O space.
uint16_t renderWriteRegister(struct GBA* gba, uint32_t address, uint16_t value) {
	struct Renderer* renderer = &gba->renderer;
	switch (address) {
	// DISPCNT: the mode, which layers are on, and the window switches.
	case 0x000:
		value &= 0xFFF7;
		renderer->dispcnt = value;
		updateDispcnt(renderer);
		if (!renderer->copy) {
			gba->vramStall = (value & 7) == 2 && (value & 0x0C00) == 0x0C00;
			videoDispcntWritten(gba);
		}
		break;
	// The undocumented green swap register.
	case 0x002:
		if ((value & 1) != renderer->stereo && renderer->stereoChanges < 8) {
			renderer->stereoCycle[renderer->stereoChanges] = renderer->copy ? renderer->copyCycle : (int32_t) (timingCurrentTime(gba) - gba->video.lineStart);
			renderer->stereoValue[renderer->stereoChanges] = value & 1;
			++renderer->stereoChanges;
		}
		renderer->stereo = value & 1;
		break;
	// BG0CNT to BG3CNT: priority, where the tiles and the map are, colors and size. Only the affine
	// backgrounds have the wraparound bit.
	case 0x008:
	case 0x00A:
		value &= 0xDFFF;
		writeBgcnt(renderer, &renderer->bg[(address - 0x008) >> 1], value);
		break;
	case 0x00C:
	case 0x00E:
		writeBgcnt(renderer, &renderer->bg[(address - 0x008) >> 1], value);
		break;
	// The text backgrounds' horizontal scroll, nine bits each.
	case 0x010:
	case 0x014:
	case 0x018:
	case 0x01C:
		value &= 0x01FF;
		renderer->bg[(address - 0x010) >> 2].x = value;
		break;
	// And vertical.
	case 0x012:
	case 0x016:
	case 0x01A:
	case 0x01E:
		value &= 0x01FF;
		renderer->bg[(address - 0x012) >> 2].y = value;
		break;
	// The two affine backgrounds' matrices: PA, PB, PC and PD, in 8.8 fixed point.
	case 0x020:
	case 0x030:
		renderer->bg[2 + ((address - 0x020) >> 4)].dx = value;
		break;
	case 0x022:
	case 0x032:
		renderer->bg[2 + ((address - 0x022) >> 4)].dmx = value;
		break;
	case 0x024:
	case 0x034:
		renderer->bg[2 + ((address - 0x024) >> 4)].dy = value;
		break;
	case 0x026:
	case 0x036:
		renderer->bg[2 + ((address - 0x026) >> 4)].dmy = value;
		break;
	// And their reference points, X then Y, each written in two halves.
	case 0x028:
	case 0x038: {
		int i = (address - 0x028) >> 4;
		struct RenderBackground* bg = &renderer->bg[2 + i];
		writeRefLo(&bg->refx, &bg->refxWritten, value);
		if (bg->sx != renderer->cache[renderer->nextY].scale[i][0]) {
			DIRTY_SCANLINE(renderer, renderer->nextY);
		}
		break;
	}
	case 0x02A:
	case 0x03A: {
		int i = (address - 0x02A) >> 4;
		struct RenderBackground* bg = &renderer->bg[2 + i];
		writeRefHi(&bg->refx, &bg->refxWritten, value);
		if (bg->sx != renderer->cache[renderer->nextY].scale[i][0]) {
			DIRTY_SCANLINE(renderer, renderer->nextY);
		}
		break;
	}
	case 0x02C:
	case 0x03C: {
		int i = (address - 0x02C) >> 4;
		struct RenderBackground* bg = &renderer->bg[2 + i];
		writeRefLo(&bg->refy, &bg->refyWritten, value);
		if (bg->sy != renderer->cache[renderer->nextY].scale[i][1]) {
			DIRTY_SCANLINE(renderer, renderer->nextY);
		}
		break;
	}
	case 0x02E:
	case 0x03E: {
		int i = (address - 0x02E) >> 4;
		struct RenderBackground* bg = &renderer->bg[2 + i];
		writeRefHi(&bg->refy, &bg->refyWritten, value);
		if (bg->sy != renderer->cache[renderer->nextY].scale[i][1]) {
			DIRTY_SCANLINE(renderer, renderer->nextY);
		}
		break;
	}
	// BLDCNT, BLDALPHA and BLDY: what blends with what, and the coefficients, each at most 16/16.
	case 0x050:
		writeBldcnt(renderer, value);
		value &= 0x3FFF;
		break;
	case 0x052:
		renderer->blda = value & 0x1F;
		if (renderer->blda > 0x10) {
			renderer->blda = 0x10;
		}
		renderer->bldb = (value >> 8) & 0x1F;
		if (renderer->bldb > 0x10) {
			renderer->bldb = 0x10;
		}
		value &= 0x1F1F;
		break;
	case 0x054:
		value &= 0x1F;
		if (value > 0x10) {
			value = 0x10;
		}
		if (renderer->bldy != value) {
			renderer->bldy = value;
			renderer->blendDirty = true;
		}
		break;
	// The two windows' horizontal and vertical edges.
	case 0x040:
		writeWindowH(&renderer->winN[0], value);
		break;
	case 0x042:
		writeWindowH(&renderer->winN[1], value);
		break;
	case 0x044:
		renderer->winN[0].vEnd = value;
		renderer->winN[0].vStart = value >> 8;
		break;
	case 0x046:
		renderer->winN[1].vEnd = value;
		renderer->winN[1].vStart = value >> 8;
		break;
	// WININ and WINOUT: which layers show inside each window, outside both, and in the object
	// window.
	case 0x048:
		value &= 0x3F3F;
		renderer->winN[0].control.packed = value;
		renderer->winN[1].control.packed = value >> 8;
		break;
	case 0x04A:
		value &= 0x3F3F;
		renderer->winout.packed = value;
		renderer->objwin.packed = value >> 8;
		break;
	// MOSAIC: block sizes for backgrounds and for sprites.
	case 0x04C:
		renderer->mosaic = value;
		break;
	default:
		break;
	}
	renderer->nextIo[address >> 1] = value;
	if (renderer->cache[renderer->nextY].io[address >> 1] != value) {
		renderer->cache[renderer->nextY].io[address >> 1] = value;
		DIRTY_SCANLINE(renderer, renderer->nextY);
	}
	return value;
}

// The start of line y, where the hardware latches what it uses for the line.
void renderStartLine(struct GBA* gba, int y) {
	struct Renderer* renderer = &gba->renderer;
	int last = y ? y - 1 : TOTAL_LINES_ - 1;
	int mode = DISPCNT_MODE(renderer->dispcnt);
	uint16_t enabled = renderer->dispcntLatch[0] & renderer->dispcnt;
	// When the last line ends, an affine background moves on by PB and PD as they are now, if it
	// was showing.
	if (last < SCREEN_HEIGHT && mode >= 1 && mode <= 5) {
		for (int i = 2; i < 4; ++i) {
			if ((i == 2 || mode == 2) && DISPCNT_BG_ENABLE(enabled, i)) {
				renderer->bg[i].sx += renderer->bg[i].dmx;
				renderer->bg[i].sy += renderer->bg[i].dmy;
			}
		}
	}
	if (y >= SCREEN_HEIGHT) {
		return;
	}
	renderer->lineStereo = renderer->stereo;
	renderer->stereoChanges = 0;
	for (int i = 2; i < 4; ++i) {
		struct RenderBackground* bg = &renderer->bg[i];
		if (bg->refxWritten || !y) {
			bg->sx = bg->refx;
			bg->refxWritten = false;
		}
		if (bg->refyWritten || !y) {
			bg->sy = bg->refy;
			bg->refyWritten = false;
		}
	}
}

// A layer switched on in DISPCNT shows only a few lines later: the values at the last three latches
// are kept, and a line shows a layer that is on now and was on at the oldest of them.
void renderLatchDispcnt(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	renderer->dispcntLatch[0] = renderer->dispcntLatch[1];
	renderer->dispcntLatch[1] = renderer->dispcntLatch[2];
	renderer->dispcntLatch[2] = renderer->dispcnt;
}

// Sets a white screen, every line to be drawn, and the registers at their reset values.
void renderReset(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	memset(renderer->dispcntLatch, 0, sizeof(renderer->dispcntLatch));
	renderer->lineDispcnt = 0x0080;
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			renderer->frame[y][x] = renderer->rgb565 ? toRgb565(COLOR_WHITE) : COLOR_WHITE;
		}
	}
	renderer->dispcnt = 0x0080;
	renderer->target1Obj = 0;
	renderer->target1Bd = 0;
	renderer->target2Obj = 0;
	renderer->target2Bd = 0;
	renderer->blendEffect = BLEND_NONE;
	renderer->paletteDirty = true;
	renderer->blendDirty = false;
	updatePalettes(gba, 0xFFFFFFFF);
	renderer->palette32Stale = 0xFFFFFFFF;
	renderer->blda = 0;
	renderer->bldb = 0;
	renderer->bldy = 0;
	memset(renderer->winN, 0, sizeof(renderer->winN));
	renderer->winN[0].control.priority = 0;
	renderer->winN[1].control.priority = 1;
	renderer->objwin = (struct RenderWindowControl) { .priority = 2 };
	renderer->winout = (struct RenderWindowControl) { .priority = 3 };
	renderer->oamDirty = true;
	renderer->oamMax = 0;
	// The first line clears the whole sprite layer.
	renderer->spriteFrom = 0;
	renderer->spriteTo = SCREEN_WIDTH;
	renderer->mosaic = 0;
	renderer->stereo = false;
	renderer->nextY = 0;
	renderer->memoryChanged = 0;
	memset(renderer->scanlineDirty, 0xFF, sizeof(renderer->scanlineDirty));
	memcpy(renderer->oamSeen, gba->oam, sizeof(renderer->oamSeen));
	memcpy(renderer->oamNext, gba->oam, sizeof(renderer->oamNext));
	renderer->oamPending = false;
	memset(renderer->vramChanged, 0, sizeof(renderer->vramChanged));
	memset(renderer->linePages, 0, sizeof(renderer->linePages));
	memset(renderer->pageLines, 0, sizeof(renderer->pageLines));
	memset(renderer->cache, 0, sizeof(renderer->cache));
	memset(renderer->nextIo, 0, sizeof(renderer->nextIo));
	for (int i = 0; i < 4; ++i) {
		struct RenderBackground* bg = &renderer->bg[i];
		memset(bg, 0, sizeof(*bg));
		bg->index = i;
		bg->dx = 256;
		bg->dmy = 256;
		bg->yCache = -1;
		bg->rowKey = -1;
	}
}
