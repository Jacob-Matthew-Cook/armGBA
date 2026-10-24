/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AVX2 versions of the renderer's hot paths. They compute what render.c computes, eight pixels at a
// time, with the same compositing order, flags and 5-bit blending.

#include "render-private.h"

#include <immintrin.h>

#define AVX2 __attribute__((target("avx2")))

// The fast paths need AVX2; without it, the reference loops draw. A build for machines that all
// have it doesn't ask the CPU, which also avoids linking libgcc's CPU check into the core.
bool renderHasSimd(void) {
#ifdef __AVX2__
	return true;
#else
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2");
#endif
}

// mix5 on eight pixels.
AVX2 static inline __m256i mix5x8(__m256i weightA, __m256i colorA, __m256i weightB, __m256i colorB) {
	const __m256i rb = _mm256_set1_epi32(0x7C1F);
	const __m256i g = _mm256_set1_epi32(0x3E0);
	__m256i a = _mm256_or_si256(_mm256_and_si256(colorA, rb), _mm256_slli_epi32(_mm256_and_si256(colorA, g), 16));
	__m256i b = _mm256_or_si256(_mm256_and_si256(colorB, rb), _mm256_slli_epi32(_mm256_and_si256(colorB, g), 16));
	__m256i c = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(a, weightA), _mm256_mullo_epi32(b, weightB)), 4);
	__m256i zero = _mm256_setzero_si256();
	__m256i over = _mm256_cmpeq_epi32(_mm256_and_si256(c, _mm256_set1_epi32(0x04000000)), zero);
	c = _mm256_blendv_epi8(_mm256_or_si256(_mm256_andnot_si256(_mm256_set1_epi32(0x07E00000), c), _mm256_set1_epi32(0x03E00000)), c, over);
	over = _mm256_cmpeq_epi32(_mm256_and_si256(c, _mm256_set1_epi32(0x0020)), zero);
	c = _mm256_blendv_epi8(_mm256_or_si256(_mm256_andnot_si256(_mm256_set1_epi32(0x003F), c), _mm256_set1_epi32(0x001F)), c, over);
	over = _mm256_cmpeq_epi32(_mm256_and_si256(c, _mm256_set1_epi32(0x8000)), zero);
	c = _mm256_blendv_epi8(_mm256_or_si256(_mm256_andnot_si256(_mm256_set1_epi32(0xF800), c), _mm256_set1_epi32(0x7C00)), c, over);
	return _mm256_or_si256(_mm256_and_si256(c, rb), _mm256_and_si256(_mm256_srli_epi32(c, 16), g));
}

// Loads eight entries of a row where the first count are real: plain loads and stores when all
// eight are, since masked stores are slow on some CPUs.
AVX2 static inline __m256i loadLanes(const uint32_t* p, int count, __m256i lanes) {
	return count >= 8 ? _mm256_loadu_si256((const __m256i*) p) : _mm256_maskload_epi32((const int*) p, lanes);
}

// Stores the first count of eight lanes, for the end of a line.
AVX2 static inline void storeLanes(uint32_t* p, int count, __m256i lanes, __m256i value) {
	if (count >= 8) {
		_mm256_storeu_si256((__m256i*) p, value);
	} else {
		_mm256_maskstore_epi32((int*) p, lanes, value);
	}
}

// 32-bit copies of the palettes for gathers, and each bank split into bytes for shuffles.
static void updatePalette32(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* normal = (const uint16_t*) gba->palette;
	for (uint32_t banks = renderer->palette32Stale; banks; banks &= banks - 1) {
		int bank = __builtin_ctz(banks);
		for (int i = bank * 16; i < bank * 16 + 16; ++i) {
			renderer->normal32[i] = normal[i];
			renderer->variant32[i] = renderer->variantPalette[i];
			renderer->bankBytes[0][bank][0][i & 15] = normal[i];
			renderer->bankBytes[0][bank][1][i & 15] = normal[i] >> 8;
			renderer->bankBytes[1][bank][0][i & 15] = renderer->variantPalette[i];
			renderer->bankBytes[1][bank][1][i & 15] = renderer->variantPalette[i] >> 8;
		}
	}
	renderer->palette32Stale = 0;
}

// Eight pixels' colors from one 16-color bank: index bytes in, 32-bit colors out.
AVX2 static inline __m256i bankColors(const uint8_t (*bank)[16], __m128i indices) {
	__m128i lo = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*) bank[0]), indices);
	__m128i hi = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*) bank[1]), indices);
	return _mm256_cvtepu16_epi32(_mm_unpacklo_epi8(lo, hi));
}

// One composite step for eight pixels: render.c's COMPOSITE_*_NO_OBJWIN where drawn is set.
AVX2 static inline __m256i compositeEight(struct Renderer* renderer, __m256i current, __m256i color, __m256i drawn, bool blend) {
	const __m256i keep = _mm256_set1_epi32(0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	const __m256i zero = _mm256_setzero_si256();
	__m256i notBelow = _mm256_cmpeq_epi32(_mm256_max_epu32(color, current), color);
	__m256i kept = _mm256_and_si256(current, keep);
	__m256i result;
	if (!blend) {
		result = _mm256_blendv_epi8(color, kept, notBelow);
	} else {
		const __m256i target1 = _mm256_set1_epi32(FLAG_TARGET_1);
		const __m256i target2 = _mm256_set1_epi32(FLAG_TARGET_2);
		__m256i mixes = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, target1), zero),
		                                    _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(color, target2), zero), _mm256_set1_epi32(-1)));
		__m256i above = kept;
		if (!_mm256_testz_si256(mixes, mixes)) {
			above = _mm256_blendv_epi8(kept, mix5x8(_mm256_set1_epi32(renderer->blda), current, _mm256_set1_epi32(renderer->bldb), color), mixes);
		}
		result = _mm256_blendv_epi8(_mm256_andnot_si256(target2, color), above, notBelow);
	}
	return _mm256_blendv_epi8(current, result, drawn);
}

// render.c's COMPOSITE_*_OBJWIN for eight pixels, once the color for each lane is chosen.
AVX2 static inline __m256i compositeEightObjwin(struct Renderer* renderer, __m256i current, __m256i color, __m256i drawn, bool blend) {
	const __m256i keep = _mm256_set1_epi32(0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	const __m256i objwin = _mm256_set1_epi32(FLAG_OBJWIN);
	const __m256i zero = _mm256_setzero_si256();
	__m256i notBelow = _mm256_cmpeq_epi32(_mm256_max_epu32(color, current), color);
	__m256i kept = _mm256_and_si256(current, keep);
	__m256i currentObjwin = _mm256_and_si256(current, objwin);
	__m256i result;
	if (!blend) {
		result = _mm256_blendv_epi8(_mm256_or_si256(color, currentObjwin), kept, notBelow);
	} else {
		const __m256i target1 = _mm256_set1_epi32(FLAG_TARGET_1);
		const __m256i target2 = _mm256_set1_epi32(FLAG_TARGET_2);
		__m256i mixes = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, target1), zero),
		                                    _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(color, target2), zero), _mm256_set1_epi32(-1)));
		__m256i above = kept;
		if (!_mm256_testz_si256(mixes, mixes)) {
			above = _mm256_blendv_epi8(kept, mix5x8(_mm256_set1_epi32(renderer->blda), current, _mm256_set1_epi32(renderer->bldb), color), mixes);
		}
		result = _mm256_blendv_epi8(_mm256_or_si256(_mm256_andnot_si256(target2, color), currentObjwin), above, notBelow);
	}
	return _mm256_blendv_epi8(current, result, drawn);
}

// Composites palette indices onto the row: render.c's BACKGROUND_DRAW_PIXEL with
// COMPOSITE_*_NO_OBJWIN. index[i] holds pixel start + i; an index of 0 within its tile (opaque[i]
// zero) draws nothing.
AVX2 static void compositeIndices(struct GBA* gba, const uint32_t* index, const uint32_t* opaque, uint32_t flags, bool variant) {
	struct Renderer* renderer = &gba->renderer;
	const int* normal = (const int*) renderer->normal32;
	const int* palette = variant ? (const int*) renderer->variant32 : normal;
	const __m256i flagsV = _mm256_set1_epi32(flags);
	const __m256i writableMask = _mm256_set1_epi32(0xFE000000);
	const __m256i reblendMask = _mm256_set1_epi32(FLAG_IS_BACKGROUND | FLAG_REBLEND);
	const __m256i reblend = _mm256_set1_epi32(FLAG_REBLEND);
	const __m256i keep = _mm256_set1_epi32(0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	const __m256i target1 = _mm256_set1_epi32(FLAG_TARGET_1);
	const __m256i target2 = _mm256_set1_epi32(FLAG_TARGET_2);
	const __m256i blda = _mm256_set1_epi32(renderer->blda);
	const __m256i bldb = _mm256_set1_epi32(renderer->bldb);
	const __m256i zero = _mm256_setzero_si256();
	bool blend = flags & FLAG_TARGET_2;
	int length = renderer->end - renderer->start;
	uint32_t* row = &renderer->row[renderer->start];
	for (int i = 0; i < length; i += 8) {
		int left = length - i;
		__m256i lanes = _mm256_cmpgt_epi32(_mm256_set1_epi32(left), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
		__m256i current = loadLanes(&row[i], left, lanes);
		__m256i idx = _mm256_and_si256(_mm256_loadu_si256((const __m256i*) &index[i]), _mm256_set1_epi32(0xFF));
		__m256i drawn = _mm256_and_si256(lanes, _mm256_loadu_si256((const __m256i*) &opaque[i]));
		drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, writableMask), zero), drawn);
		if (_mm256_testz_si256(drawn, drawn)) {
			continue;
		}
		__m256i color = _mm256_i32gather_epi32(palette, idx, 4);
		if (variant) {
			__m256i useNormal = _mm256_cmpeq_epi32(_mm256_and_si256(current, reblendMask), reblend);
			color = _mm256_blendv_epi8(color, _mm256_i32gather_epi32(normal, idx, 4), useNormal);
		}
		color = _mm256_or_si256(color, flagsV);
		// color >= current, unsigned
		__m256i notBelow = _mm256_cmpeq_epi32(_mm256_max_epu32(color, current), color);
		__m256i kept = _mm256_and_si256(current, keep);
		__m256i result;
		if (!blend) {
			result = _mm256_blendv_epi8(color, kept, notBelow);
		} else {
			__m256i mixes = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, target1), zero),
			                                    _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(color, target2), zero), _mm256_set1_epi32(-1)));
			__m256i above = kept;
			if (!_mm256_testz_si256(mixes, mixes)) {
				above = _mm256_blendv_epi8(kept, mix5x8(blda, current, bldb, color), mixes);
			}
			result = _mm256_blendv_epi8(_mm256_andnot_si256(target2, color), above, notBelow);
		}
		result = _mm256_blendv_epi8(current, result, drawn);
		storeLanes(&row[i], left, lanes, result);
	}
}

// Draws a text background without mosaic or the object window, as render.c's drawBackgroundMode0
// does.
AVX2 bool renderTextSimd(struct GBA* gba, struct RenderBackground* background, int y) {
	struct Renderer* renderer = &gba->renderer;
	int length = renderer->end - renderer->start;
	if (length <= 0) {
		return true;
	}
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	int inX = (renderer->start + background->x) & 0x1FF;
	int inY = y + background->y;
	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);
	const uint8_t* vram = gba->vram;

	// Indices from the start of the first tile; its lead pixels lie before the window.
	int lead = inX & 7;
	int count = lead + length;
	// When the window ends inside the first, partial tile, mGBA lines its pixels up differently.
	if (lead && count < 8) {
		return false;
	}
	int tiles = (count + 7) >> 3;
	uint32_t index[SCREEN_WIDTH + 16] __attribute__((aligned(32)));
	uint32_t opaque[SCREEN_WIDTH + 16] __attribute__((aligned(32)));
	const __m256i nibbleShifts = _mm256_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28);
	const __m256i nibbleShiftsFlipped = _mm256_setr_epi32(28, 24, 20, 16, 12, 8, 4, 0);
	const __m256i reverse = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
	const __m256i zero = _mm256_setzero_si256();
	int localY0 = inY & 0x7;
	if (!background->multipalette) {
		// Each tile's eight colors come from one 16-color bank: two byte shuffles, composited in
		// place.
		const __m128i nibbleMask = _mm_set1_epi8(0x0F);
		const __m128i flip = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15);
		const __m256i laneIndex = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
		const __m256i flags = _mm256_set1_epi32(background->flags);
		const __m256i writableMask = _mm256_set1_epi32(0xFE000000);
		const __m256i reblendMask = _mm256_set1_epi32(FLAG_IS_BACKGROUND | FLAG_REBLEND);
		const __m256i reblend = _mm256_set1_epi32(FLAG_REBLEND);
		const __m256i first = _mm256_set1_epi32(renderer->start - 1);
		const __m256i last = _mm256_set1_epi32(renderer->end);
		bool blend = background->flags & FLAG_TARGET_2;
		bool variant = background->variant;
		// With the object window on, pixels under it take objwinFlags and the object window
		// palette.
		bool objwinSlowPath = DISPCNT_OBJWIN_ENABLE(renderer->dispcnt);
		bool objwinVariant = objwinSlowPath && background->target1 && WIN_BLEND_ENABLE(renderer->objwin.packed) &&
		                     (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
		const __m256i objwinFlags = _mm256_set1_epi32(background->objwinFlags);
		const __m256i objwinBit = _mm256_set1_epi32(FLAG_OBJWIN);
		const __m256i objwinOnly = _mm256_set1_epi32(background->objwinOnly ? -1 : 0);
		bool forceEnable = background->objwinForceEnable;
		for (int t = 0; t < tiles; ++t) {
			int localX = (inX - lead + t * 8) & 0x1FF;
			unsigned xBase = localX & 0xF8;
			if (background->size & 1) {
				xBase += (localX & 0x100) << 5;
			}
			uint16_t mapData = load16(vram, (yBase + (xBase >> 3)) << 1);
			int localY = TEXT_MAP_VFLIP(mapData) ? 7 - localY0 : localY0;
			uint32_t charBase = background->charBase + (TEXT_MAP_TILE(mapData) << 5) + (localY << 2);
			// mGBA skips whole tiles past 64 KiB, but draws the partial ones at the ends of the
			// run.
			bool partial = (t == 0 && lead) || (t == tiles - 1 && (count & 7));
			if (!partial && charBase >= 0x10000) {
				continue;
			}
			uint32_t data = load32(vram, charBase);
			if (!data) {
				continue;
			}
			__m128i bytes = _mm_cvtsi32_si128(data);
			__m128i nibbles = _mm_unpacklo_epi8(_mm_and_si128(bytes, nibbleMask), _mm_and_si128(_mm_srli_epi16(bytes, 4), nibbleMask));
			if (TEXT_MAP_HFLIP(mapData)) {
				nibbles = _mm_shuffle_epi8(nibbles, flip);
			}
			int x0 = renderer->start - lead + t * 8;
			int* row = (int*) &renderer->row[x0];
			bool edge = x0 < renderer->start || x0 + 8 > renderer->end;
			__m256i inside = _mm256_set1_epi32(-1);
			__m256i current;
			if (edge) {
				__m256i position = _mm256_add_epi32(_mm256_set1_epi32(x0), laneIndex);
				inside = _mm256_and_si256(_mm256_cmpgt_epi32(position, first), _mm256_cmpgt_epi32(last, position));
				current = _mm256_maskload_epi32(row, inside);
			} else {
				current = _mm256_loadu_si256((const __m256i*) row);
			}
			__m256i drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_cvtepu8_epi32(nibbles), zero), inside);
			drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, writableMask), zero), drawn);
			if (_mm256_testz_si256(drawn, drawn)) {
				continue;
			}
			int bank = TEXT_MAP_PALETTE(mapData);
			__m256i color = bankColors(renderer->bankBytes[variant][bank], nibbles);
			__m256i normal = color;
			if (variant || objwinSlowPath) {
				normal = bankColors(renderer->bankBytes[0][bank], nibbles);
			}
			if (variant) {
				__m256i useNormal = _mm256_cmpeq_epi32(_mm256_and_si256(current, reblendMask), reblend);
				color = _mm256_blendv_epi8(color, normal, useNormal);
			}
			__m256i result;
			if (!objwinSlowPath) {
				color = _mm256_or_si256(color, flags);
				result = compositeEight(renderer, current, color, drawn, blend);
			} else {
				__m256i underObjwin = _mm256_cmpeq_epi32(_mm256_and_si256(current, objwinBit), objwinBit);
				if (!forceEnable) {
					// (!(current & FLAG_OBJWIN)) == objwinOnly: drawn where being under the object
					// window differs from objwinOnly.
					drawn = _mm256_and_si256(drawn, _mm256_xor_si256(underObjwin, objwinOnly));
				}
				__m256i objwinColor = objwinVariant ? bankColors(renderer->bankBytes[1][bank], nibbles) : normal;
				color = _mm256_blendv_epi8(_mm256_or_si256(color, flags), _mm256_or_si256(objwinColor, objwinFlags), underObjwin);
				result = compositeEightObjwin(renderer, current, color, drawn, blend);
			}
			if (edge) {
				_mm256_maskstore_epi32(row, inside, result);
			} else {
				_mm256_storeu_si256((__m256i*) row, result);
			}
		}
		return true;
	}
	for (int t = 0; t < tiles; ++t) {
		int localX = (inX - lead + t * 8) & 0x1FF;
		unsigned xBase = localX & 0xF8;
		if (background->size & 1) {
			xBase += (localX & 0x100) << 5;
		}
		uint16_t mapData = load16(vram, (yBase + (xBase >> 3)) << 1);
		int localY = TEXT_MAP_VFLIP(mapData) ? 7 - localY0 : localY0;
		// mGBA skips whole tiles past 64 KiB, but draws the partial ones at the ends of a 16-color
		// run.
		bool partial = (t == 0 && lead) || (t == tiles - 1 && (count & 7));
		__m256i idx;
		if (!background->multipalette) {
			uint32_t charBase = background->charBase + (TEXT_MAP_TILE(mapData) << 5) + (localY << 2);
			if (!partial && charBase >= 0x10000) {
				_mm256_storeu_si256((__m256i*) &opaque[t * 8], zero);
				_mm256_storeu_si256((__m256i*) &index[t * 8], zero);
				continue;
			}
			__m256i data = _mm256_set1_epi32(load32(vram, charBase));
			idx = _mm256_and_si256(_mm256_srlv_epi32(data, TEXT_MAP_HFLIP(mapData) ? nibbleShiftsFlipped : nibbleShifts), _mm256_set1_epi32(0xF));
			__m256i transparent = _mm256_cmpeq_epi32(idx, zero);
			_mm256_storeu_si256((__m256i*) &opaque[t * 8], _mm256_xor_si256(transparent, _mm256_set1_epi32(-1)));
			idx = _mm256_or_si256(idx, _mm256_set1_epi32(TEXT_MAP_PALETTE(mapData) << 4));
		} else {
			uint32_t charBase = background->charBase + (TEXT_MAP_TILE(mapData) << 6) + (localY << 3);
			if (charBase >= 0x10000) {
				_mm256_storeu_si256((__m256i*) &opaque[t * 8], zero);
				_mm256_storeu_si256((__m256i*) &index[t * 8], zero);
				continue;
			}
			idx = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*) &vram[charBase]));
			if (TEXT_MAP_HFLIP(mapData)) {
				idx = _mm256_permutevar8x32_epi32(idx, reverse);
			}
			__m256i transparent = _mm256_cmpeq_epi32(idx, zero);
			_mm256_storeu_si256((__m256i*) &opaque[t * 8], _mm256_xor_si256(transparent, _mm256_set1_epi32(-1)));
		}
		_mm256_storeu_si256((__m256i*) &index[t * 8], idx);
	}
	compositeIndices(gba, &index[lead], &opaque[lead], background->flags, background->variant);
	return true;
}

// Merges the sprites of one priority onto the row: render.c's postprocessSprite without the object
// window. Sprites wrote only columns [from, to) on this line.
AVX2 void renderSpritesSimd(struct GBA* gba, unsigned priority, int from, int to) {
	struct Renderer* renderer = &gba->renderer;
	const __m256i unwritten = _mm256_set1_epi32(FLAG_UNWRITTEN);
	const __m256i priorityMask = _mm256_set1_epi32(FLAG_PRIORITY);
	const __m256i wanted = _mm256_set1_epi32((int) ((uint32_t) priority << OFFSET_PRIORITY));
	const __m256i objwin = _mm256_set1_epi32(FLAG_OBJWIN);
	const __m256i flags = _mm256_set1_epi32(FLAG_TARGET_2 * renderer->target2Obj);
	const __m256i laneIndex = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	int start = renderer->start > from ? renderer->start : from;
	int length = (renderer->end < to ? renderer->end : to) - start;
	for (int i = 0; i < length; i += 8) {
		int x = start + i;
		__m256i inside = _mm256_cmpgt_epi32(_mm256_set1_epi32(length - i), laneIndex);
		bool edge = length - i < 8;
		__m256i sprite = edge ? _mm256_maskload_epi32((const int*) &renderer->spriteLayer[x], inside) : _mm256_loadu_si256((const __m256i*) &renderer->spriteLayer[x]);
		sprite = _mm256_andnot_si256(objwin, sprite);
		__m256i drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(sprite, unwritten), unwritten), inside);
		drawn = _mm256_and_si256(drawn, _mm256_cmpeq_epi32(_mm256_and_si256(sprite, priorityMask), wanted));
		if (_mm256_testz_si256(drawn, drawn)) {
			continue;
		}
		__m256i current = edge ? _mm256_maskload_epi32((const int*) &renderer->row[x], inside) : _mm256_loadu_si256((const __m256i*) &renderer->row[x]);
		__m256i color = _mm256_or_si256(sprite, flags);
		// compositeBlendNoObjwin: blends whenever target 1 meets target 2.
		__m256i result = compositeEight(renderer, current, color, drawn, true);
		if (edge) {
			_mm256_maskstore_epi32((int*) &renderer->row[x], inside, result);
		} else {
			_mm256_storeu_si256((__m256i*) &renderer->row[x], result);
		}
	}
}

// The first part of render.c's postprocessBuffer: blends target 1 pixels with the backdrop.
AVX2 void renderBlendBackdropSimd(struct GBA* gba, int x, int end, uint32_t backdrop) {
	struct Renderer* renderer = &gba->renderer;
	const __m256i target1 = _mm256_set1_epi32(FLAG_TARGET_1);
	const __m256i zero = _mm256_setzero_si256();
	const __m256i laneIndex = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i backdropV = _mm256_set1_epi32(backdrop);
	const __m256i blda = _mm256_set1_epi32(renderer->blda);
	const __m256i bldb = _mm256_set1_epi32(renderer->bldb);
	for (; x < end; x += 8) {
		__m256i inside = _mm256_cmpgt_epi32(_mm256_set1_epi32(end - x), laneIndex);
		__m256i color = loadLanes(&renderer->row[x], end - x, inside);
		__m256i hit = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(color, target1), zero), inside);
		if (_mm256_testz_si256(hit, hit)) {
			continue;
		}
		__m256i mixed = mix5x8(bldb, backdropV, blda, color);
		storeLanes(&renderer->row[x], end - x, inside, _mm256_blendv_epi8(color, mixed, hit));
	}
}

// Byte i of a 64-byte table for each index byte.
AVX2 static inline __m128i lookup64(const __m128i* table, __m128i index) {
	__m128i low = _mm_blendv_epi8(_mm_shuffle_epi8(table[0], index), _mm_shuffle_epi8(table[1], index), _mm_cmpgt_epi8(index, _mm_set1_epi8(15)));
	__m128i high = _mm_blendv_epi8(_mm_shuffle_epi8(table[2], index), _mm_shuffle_epi8(table[3], index), _mm_cmpgt_epi8(index, _mm_set1_epi8(47)));
	return _mm_blendv_epi8(low, high, _mm_cmpgt_epi8(index, _mm_set1_epi8(31)));
}

// Eight 32-bit lanes of small values narrowed to bytes.
AVX2 static inline __m128i packBytes(__m256i lanes) {
	__m128i words = _mm_packus_epi32(_mm256_castsi256_si128(lanes), _mm256_extracti128_si256(lanes, 1));
	return _mm_packus_epi16(words, words);
}

// Draws a run of a sprite's pixels eight at a time: render.c's SPRITE_NORMAL_LOOP and
// SPRITE_TRANSFORMED_LOOP with SPRITE_DRAW_PIXEL_*_NORMAL.
AVX2 void renderSpriteRunSimd(struct GBA* gba, const struct SpriteRun* run) {
	struct Renderer* renderer = &gba->renderer;
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	const int* vramBase = (const int*) &gba->vram[BASE_TILE];
	const __m256i laneIndex = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i zero = _mm256_setzero_si256();
	const __m256i order = _mm256_set1_epi32(FLAG_ORDER_MASK);
	const __m256i other = _mm256_set1_epi32(FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1);
	const __m256i unwritten = _mm256_set1_epi32(FLAG_UNWRITTEN);
	const __m256i flags = _mm256_set1_epi32(run->flags);
	const __m256i flagsOther = _mm256_and_si256(flags, other);
	const __m256i charBase = _mm256_set1_epi32(run->charBase);
	const __m256i maskLo = _mm256_set1_epi32(run->maskLo);
	const __m256i maskHi = _mm256_set1_epi32(run->maskHi);
	const __m256i stride = _mm256_set1_epi32(run->stride);
	const __m256i halfword = _mm256_set1_epi32(0x7FFE);
	const __m256i paletteBase = _mm256_set1_epi32(run->paletteBase);
	const __m128i nibbleMask = _mm_set1_epi8(0x0F);
	const __m128i flip = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15);
	const int rowBytes = run->wide ? 8 : 4;
	const uint8_t (*bank)[16] = renderer->bankBytes[run->palette == renderer->variant32][run->paletteBase >> 4];
	int outX = run->outX;
	int inX = run->inX;
	int xAccum = run->xAccum;
	int yAccum = run->yAccum;
	// An affine sprite whose rows don't turn (c == 0) reads a single row of itself, so its texels
	// and their colors go into tables once.
	int rowY = yAccum >> 8;
	bool rowTable = run->transformed && !run->c && run->width <= 64 && !(rowY & ~(run->height - 1));
	__m128i rowTexels[4];
	__m128i rowLow[4];
	__m128i rowHigh[4];
	if (rowTable) {
		uint8_t texels[64] = { 0 };
		uint8_t low[64] = { 0 };
		uint8_t high[64] = { 0 };
		spriteRow(gba, run, rowY, texels, low, high);
		for (int i = 0; i < 4; ++i) {
			rowTexels[i] = _mm_loadu_si128((const __m128i*) &texels[i * 16]);
			rowLow[i] = _mm_loadu_si128((const __m128i*) &low[i * 16]);
			rowHigh[i] = _mm_loadu_si128((const __m128i*) &high[i * 16]);
		}
	}
	while (outX < run->condition) {
		int left = run->condition - outX;
		__m256i lanes = _mm256_cmpgt_epi32(_mm256_set1_epi32(left), laneIndex);
		int valid = left < 8 ? left : 8;
		bool stop = false;
		__m256i texel;
		__m256i color;
		if (!run->transformed && !run->wide && left >= 8 && (run->xOffset == 1 ? !(inX & 7) : run->xOffset == -1 && (inX & 7) == 7)) {
			// One whole row of a 16-color tile: its two halfwords, as SPRITE_DRAW_PIXEL_16_NORMAL
			// reads them.
			unsigned xBase = (inX & ~7) * 4;
			unsigned yBase = (run->inY & ~7) * run->stride + (run->inY & 7) * 4 + run->maskHi;
			const uint8_t* tiles = &gba->vram[BASE_TILE];
			uint32_t data = load16(tiles, (yBase + ((xBase + run->charBase) & run->maskLo)) & 0x7FFE);
			data |= load16(tiles, (yBase + ((xBase + 2 + run->charBase) & run->maskLo)) & 0x7FFE) << 16;
			__m128i bytes = _mm_cvtsi32_si128(data);
			__m128i nibbles = _mm_unpacklo_epi8(_mm_and_si128(bytes, nibbleMask), _mm_and_si128(_mm_srli_epi16(bytes, 4), nibbleMask));
			if (run->xOffset < 0) {
				nibbles = _mm_shuffle_epi8(nibbles, flip);
			}
			texel = _mm256_cvtepu8_epi32(nibbles);
			color = bankColors(bank, nibbles);
		} else {
			__m256i localX;
			__m256i localY;
			if (!run->transformed) {
				localX = _mm256_add_epi32(_mm256_set1_epi32(inX), _mm256_mullo_epi32(laneIndex, _mm256_set1_epi32(run->xOffset)));
				localY = _mm256_set1_epi32(run->inY);
			} else {
				__m256i step = _mm256_add_epi32(laneIndex, _mm256_set1_epi32(1));
				__m256i xs = _mm256_add_epi32(_mm256_set1_epi32(xAccum), _mm256_mullo_epi32(step, _mm256_set1_epi32(run->a)));
				__m256i ys = _mm256_add_epi32(_mm256_set1_epi32(yAccum), _mm256_mullo_epi32(step, _mm256_set1_epi32(run->c)));
				localX = _mm256_srai_epi32(xs, 8);
				localY = _mm256_srai_epi32(ys, 8);
				// The loop stops at the first pixel outside the sprite.
				__m256i outside = _mm256_or_si256(_mm256_and_si256(localX, _mm256_set1_epi32(~(run->width - 1))),
				                                  _mm256_and_si256(localY, _mm256_set1_epi32(~(run->height - 1))));
				outside = _mm256_and_si256(_mm256_xor_si256(_mm256_cmpeq_epi32(outside, zero), _mm256_set1_epi32(-1)), lanes);
				int bits = _mm256_movemask_ps(_mm256_castsi256_ps(outside));
				if (bits) {
					int firstOut = __builtin_ctz(bits);
					lanes = _mm256_cmpgt_epi32(_mm256_set1_epi32(firstOut), laneIndex);
					valid = firstOut;
					stop = true;
				}
			}
			if (rowTable) {
				__m128i index = packBytes(_mm256_and_si256(localX, _mm256_set1_epi32(63)));
				texel = _mm256_cvtepu8_epi32(lookup64(rowTexels, index));
				color = _mm256_cvtepu16_epi32(_mm_unpacklo_epi8(lookup64(rowLow, index), lookup64(rowHigh, index)));
			} else {
				__m256i yBase = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_andnot_si256(_mm256_set1_epi32(7), localY), stride),
				                                 _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(localY, _mm256_set1_epi32(7)), _mm256_set1_epi32(rowBytes)), maskHi));
				__m256i xBase;
				if (!run->wide) {
					xBase = _mm256_add_epi32(_mm256_slli_epi32(_mm256_andnot_si256(_mm256_set1_epi32(7), localX), 2),
					                         _mm256_and_si256(_mm256_srli_epi32(localX, 1), _mm256_set1_epi32(2)));
				} else {
					xBase = _mm256_add_epi32(_mm256_slli_epi32(_mm256_andnot_si256(_mm256_set1_epi32(7), localX), 3),
					                         _mm256_and_si256(localX, _mm256_set1_epi32(6)));
				}
				__m256i address = _mm256_and_si256(_mm256_add_epi32(yBase, _mm256_and_si256(_mm256_add_epi32(xBase, charBase), maskLo)), halfword);
				texel = _mm256_mask_i32gather_epi32(zero, (const int*) ((const uint8_t*) vramBase), address, lanes, 1);
				if (!run->wide) {
					texel = _mm256_and_si256(_mm256_srlv_epi32(texel, _mm256_slli_epi32(_mm256_and_si256(localX, _mm256_set1_epi32(3)), 2)), _mm256_set1_epi32(0xF));
				} else {
					texel = _mm256_and_si256(_mm256_srlv_epi32(texel, _mm256_slli_epi32(_mm256_and_si256(localX, _mm256_set1_epi32(1)), 3)), _mm256_set1_epi32(0xFF));
				}
				color = _mm256_i32gather_epi32((const int*) run->palette, _mm256_add_epi32(paletteBase, texel), 4);
			}
		}
		uint32_t* layer = &renderer->spriteLayer[outX];
		__m256i current = loadLanes(layer, valid, lanes);
		// (current & FLAG_ORDER_MASK) > flags, unsigned
		__m256i currentOrder = _mm256_and_si256(current, order);
		__m256i before = _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_max_epu32(currentOrder, flags), flags), _mm256_set1_epi32(-1));
		before = _mm256_and_si256(before, lanes);
		__m256i transparent = _mm256_cmpeq_epi32(texel, zero);
		__m256i opaqueResult = _mm256_or_si256(color, flags);
		__m256i clearResult = _mm256_or_si256(_mm256_andnot_si256(other, current), flagsOther);
		__m256i written = _mm256_xor_si256(_mm256_cmpeq_epi32(current, unwritten), _mm256_set1_epi32(-1));
		__m256i result = _mm256_blendv_epi8(opaqueResult, clearResult, transparent);
		__m256i store = _mm256_and_si256(before, _mm256_or_si256(_mm256_andnot_si256(transparent, _mm256_set1_epi32(-1)), written));
		storeLanes(layer, valid, lanes, _mm256_blendv_epi8(current, result, store));
		if (stop) {
			return;
		}
		int done = left < 8 ? left : 8;
		outX += done;
		inX += done * run->xOffset;
		xAccum += done * run->a;
		yAccum += done * run->c;
	}
}

// render.c's drawBackgroundMode2 without mosaic or the object window: eight pixels' map and
// character bytes gathered at once, then composited as a 256-color text background is. x and y are
// the coordinates of the pixel before the window's first, as BACKGROUND_BITMAP_INIT leaves them.
AVX2 void renderAffineSimd(struct GBA* gba, struct RenderBackground* background, int32_t x, int32_t y) {
	struct Renderer* renderer = &gba->renderer;
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	int length = renderer->end - renderer->start;
	if (length <= 0) {
		return;
	}
	uint32_t index[SCREEN_WIDTH + 8] __attribute__((aligned(32)));
	uint32_t opaque[SCREEN_WIDTH + 8] __attribute__((aligned(32)));
	const int* screenBase = (const int*) &gba->vram[background->screenBase];
	const int* charBase = (const int*) &gba->vram[background->charBase];
	int32_t mask = (0x8000 << background->size) - 1;
	const __m256i step = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8);
	const __m256i dx = _mm256_set1_epi32(background->dx);
	const __m256i dy = _mm256_set1_epi32(background->dy);
	const __m256i coordMask = _mm256_set1_epi32(mask);
	const __m256i bytes = _mm256_set1_epi32(0xFF);
	const __m256i zero = _mm256_setzero_si256();
	const __m128i size = _mm_cvtsi32_si128(background->size);
	for (int i = 0; i < length; i += 8) {
		// The loop adds dx and dy before each pixel.
		__m256i xs = _mm256_add_epi32(_mm256_set1_epi32(x + i * background->dx), _mm256_mullo_epi32(step, dx));
		__m256i ys = _mm256_add_epi32(_mm256_set1_epi32(y + i * background->dy), _mm256_mullo_epi32(step, dy));
		__m256i inside = _mm256_set1_epi32(-1);
		if (background->overflow) {
			xs = _mm256_and_si256(xs, coordMask);
			ys = _mm256_and_si256(ys, coordMask);
		} else {
			inside = _mm256_cmpeq_epi32(_mm256_andnot_si256(coordMask, _mm256_or_si256(xs, ys)), zero);
		}
		// screenBase[(localX >> 11) + (((localY >> 7) & 0x7F0) << size)]
		__m256i map = _mm256_add_epi32(_mm256_srli_epi32(xs, 11), _mm256_sll_epi32(_mm256_and_si256(_mm256_srli_epi32(ys, 7), _mm256_set1_epi32(0x7F0)), size));
		map = _mm256_and_si256(_mm256_mask_i32gather_epi32(zero, screenBase, map, inside, 1), bytes);
		// charBase[(mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8)]
		__m256i at = _mm256_add_epi32(_mm256_slli_epi32(map, 6), _mm256_srli_epi32(_mm256_and_si256(ys, _mm256_set1_epi32(0x700)), 5));
		at = _mm256_add_epi32(at, _mm256_srli_epi32(_mm256_and_si256(xs, _mm256_set1_epi32(0x700)), 8));
		__m256i pixels = _mm256_and_si256(_mm256_mask_i32gather_epi32(zero, charBase, at, inside, 1), bytes);
		_mm256_store_si256((__m256i*) &index[i], pixels);
		_mm256_store_si256((__m256i*) &opaque[i], _mm256_andnot_si256(_mm256_cmpeq_epi32(pixels, zero), inside));
	}
	compositeIndices(gba, index, opaque, background->flags, background->variant);
}

// render.c's drawBackgroundMode4 without the object window. x and y are the coordinates of the
// pixel before the window's first, as BACKGROUND_BITMAP_INIT leaves them; with mosaic, picked holds
// the color index each pixel took instead, or 0 for none.
AVX2 void renderMode4Simd(struct GBA* gba, struct RenderBackground* background, int32_t x, int32_t y, const uint8_t* picked) {
	struct Renderer* renderer = &gba->renderer;
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	const int* palette = (const int*) (background->variant ? renderer->variant32 : renderer->normal32);
	uint32_t offset = DISPCNT_FRAME_SELECT(renderer->dispcnt) ? 0xA000 : 0;
	const uint8_t* frame = &gba->vram[offset];
	const __m256i laneIndex = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i zero = _mm256_setzero_si256();
	const __m256i writableMask = _mm256_set1_epi32(0xFE000000);
	const __m256i flags = _mm256_set1_epi32(background->flags);
	const __m256i width = _mm256_set1_epi32(SCREEN_WIDTH);
	const __m256i height = _mm256_set1_epi32(SCREEN_HEIGHT);
	const __m256i dx = _mm256_set1_epi32(background->dx);
	const __m256i dy = _mm256_set1_epi32(background->dy);
	const __m256i step = _mm256_add_epi32(laneIndex, _mm256_set1_epi32(1));
	// Without rotation, a row reads one line; shrunk at most by half, eight pixels lie within 16
	// bytes of it.
	bool flat = background->dy == 0 && background->dx > 0 && background->dx <= 0x200;
	int length = renderer->end - renderer->start;
	for (int i = 0; i < length; i += 8) {
		__m256i lanes = _mm256_cmpgt_epi32(_mm256_set1_epi32(length - i), laneIndex);
		// The loop adds dx and dy before each pixel.
		__m256i xs = _mm256_add_epi32(_mm256_set1_epi32(x + i * background->dx), _mm256_mullo_epi32(step, dx));
		__m256i column = _mm256_srai_epi32(xs, 8);
		__m256i inside;
		__m256i color;
		int line0 = y >> 8;
		int first = (x + (i + 1) * background->dx) >> 8;
		int last = (x + (i + 8) * background->dx) >> 8;
		if (picked) {
			inside = lanes;
			color = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*) &picked[i]));
		} else if (flat && length - i >= 8 && (unsigned) line0 < SCREEN_HEIGHT && first >= 0 && last < SCREEN_WIDTH) {
			inside = _mm256_set1_epi32(-1);
			__m256i offsets = _mm256_sub_epi32(column, _mm256_set1_epi32(first));
			__m128i offsets16 = _mm_packus_epi32(_mm256_castsi256_si128(offsets), _mm256_extracti128_si256(offsets, 1));
			__m128i window = _mm_loadu_si128((const __m128i*) &frame[line0 * SCREEN_WIDTH + first]);
			color = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(window, _mm_packus_epi16(offsets16, offsets16)));
		} else {
			__m256i ys = _mm256_add_epi32(_mm256_set1_epi32(y + i * background->dy), _mm256_mullo_epi32(step, dy));
			__m256i line = _mm256_srai_epi32(ys, 8);
			// x and y in range: (unsigned) column < 240 and line < 160.
			inside = _mm256_and_si256(_mm256_cmpgt_epi32(width, column), _mm256_cmpgt_epi32(height, line));
			inside = _mm256_and_si256(inside, _mm256_cmpgt_epi32(_mm256_or_si256(column, line), _mm256_set1_epi32(-1)));
			inside = _mm256_and_si256(inside, lanes);
			if (_mm256_testz_si256(inside, inside)) {
				continue;
			}
			__m256i address = _mm256_add_epi32(column, _mm256_mullo_epi32(line, width));
			color = _mm256_and_si256(_mm256_mask_i32gather_epi32(zero, (const int*) frame, address, inside, 1), _mm256_set1_epi32(0xFF));
		}
		int* row = (int*) &renderer->row[renderer->start + i];
		__m256i current = loadLanes((const uint32_t*) row, length - i, lanes);
		__m256i drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(color, zero), inside);
		drawn = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(current, writableMask), zero), drawn);
		if (_mm256_testz_si256(drawn, drawn)) {
			continue;
		}
		__m256i colors = _mm256_or_si256(_mm256_mask_i32gather_epi32(zero, palette, color, drawn, 4), flags);
		storeLanes((uint32_t*) row, length - i, lanes, compositeEight(renderer, current, colors, drawn, true));
	}
}
