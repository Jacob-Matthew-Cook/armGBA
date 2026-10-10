/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SOFTWARE_PRIVATE_H
#define SOFTWARE_PRIVATE_H

#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/renderers/video-software.h>

#ifdef NDEBUG
#define VIDEO_CHECKS false
#else
#define VIDEO_CHECKS true
#endif

#define ENABLED_MAX 4

void GBAVideoSoftwareRendererDrawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer,
                                                 struct GBAVideoSoftwareBackground* background, int y);
void GBAVideoSoftwareRendererDrawBackgroundMode2(struct GBAVideoSoftwareRenderer* renderer,
                                                 struct GBAVideoSoftwareBackground* background, int y);
void GBAVideoSoftwareRendererDrawBackgroundMode3(struct GBAVideoSoftwareRenderer* renderer,
                                                 struct GBAVideoSoftwareBackground* background, int y);
void GBAVideoSoftwareRendererDrawBackgroundMode4(struct GBAVideoSoftwareRenderer* renderer,
                                                 struct GBAVideoSoftwareBackground* background, int y);
void GBAVideoSoftwareRendererDrawBackgroundMode5(struct GBAVideoSoftwareRenderer* renderer,
                                                 struct GBAVideoSoftwareBackground* background, int y);

void GBAVideoSoftwareRendererStepWindow(struct GBAVideoSoftwareRenderer* renderer, int y);
void GBAVideoSoftwareRendererPreprocessBuffer(struct GBAVideoSoftwareRenderer* renderer);

int GBAVideoSoftwareRendererPreprocessSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAObj* sprite, int index, int y);
void GBAVideoSoftwareRendererPostprocessSprite(struct GBAVideoSoftwareRenderer* renderer, unsigned priority);

static inline unsigned _brighten(unsigned color, int y);
static inline unsigned _darken(unsigned color, int y);

// We stash the priority on the top bits so we can do a one-operator comparison
// The lower the number, the higher the priority, and sprites take precedence over backgrounds
// We want to do special processing if the color pixel is target 1, however

static inline void _compositeBlendObjwin(struct GBAVideoSoftwareRenderer* renderer, uint32_t* pixel, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			color = mColorMix5Bit(renderer->blda, current, renderer->bldb, color);
		} else {
			color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
		}
	} else {
		color = (color & ~FLAG_TARGET_2) | (current & FLAG_OBJWIN);
	}
	*pixel = color;
}

static inline void _compositeBlendNoObjwin(struct GBAVideoSoftwareRenderer* renderer, uint32_t* pixel, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			color = mColorMix5Bit(renderer->blda, current, renderer->bldb, color);
		} else {
			color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
		}
	} else {
		color = color & ~FLAG_TARGET_2;
	}
	*pixel = color;
}

static inline void _compositeNoBlendObjwin(struct GBAVideoSoftwareRenderer* renderer, uint32_t* pixel, uint32_t color,
                                           uint32_t current) {
	UNUSED(renderer);
	if (color < current) {
		color |= (current & FLAG_OBJWIN);
	} else {
		color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	*pixel = color;
}

static inline void _compositeNoBlendNoObjwin(struct GBAVideoSoftwareRenderer* renderer, uint32_t* pixel, uint32_t color,
                                             uint32_t current) {
	UNUSED(renderer);
	if (color >= current) {
		color = current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	*pixel = color;
}

// Eight pixels at a time where there is NEON or SSE4.1
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5) && ((defined(__ARM_NEON) && defined(__aarch64__)) || defined(__SSE4_1__))
#define VIDEO_SIMD 1
#ifdef __ARM_NEON
#include <arm_neon.h>
typedef uint32x4_t v32;
typedef uint16x8_t v16;
typedef uint8x8_t vidx;

static inline v32 _v32Load(const uint32_t* p) { return vld1q_u32(p); }
static inline void _v32Store(uint32_t* p, v32 v) { vst1q_u32(p, v); }
static inline v32 _v32(uint32_t x) { return vdupq_n_u32(x); }
static inline v32 _v32And(v32 a, v32 b) { return vandq_u32(a, b); }
static inline v32 _v32Or(v32 a, v32 b) { return vorrq_u32(a, b); }
static inline v32 _v32Bic(v32 a, v32 b) { return vbicq_u32(a, b); }
static inline v32 _v32Eq(v32 a, v32 b) { return vceqq_u32(a, b); }
static inline v32 _v32Ge(v32 a, v32 b) { return vcgeq_u32(a, b); }
static inline v32 _v32Test(v32 a, v32 b) { return vtstq_u32(a, b); }
static inline v32 _v32Select(v32 mask, v32 a, v32 b) { return vbslq_u32(mask, a, b); }
static inline bool _v32Any(v32 a, v32 b) { return vmaxvq_u32(vorrq_u32(a, b)); }
static inline v16 _v16(uint16_t x) { return vdupq_n_u16(x); }
static inline void _v16Store(uint16_t* p, v16 v) { vst1q_u16(p, v); }
static inline v16 _v16Narrow(v32 low, v32 high) { return vcombine_u16(vmovn_u32(low), vmovn_u32(high)); }
static inline v32 _v16Widen(v16 v, unsigned half) { return vmovl_u16(half ? vget_high_u16(v) : vget_low_u16(v)); }
static inline v16 _v16Select(v16 mask, v16 a, v16 b) { return vbslq_u16(mask, a, b); }

// The eight 4-bit pixels of a tile row, in drawing order
static inline vidx _nibbles(uint32_t tileData) {
	uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(tileData));
	return vzip_u8(vand_u8(bytes, vdup_n_u8(0xF)), vshr_n_u8(bytes, 4)).val[0];
}

static inline v32 _nonzero(vidx index, unsigned half) {
	v32 wide = vmovl_u16(half ? vget_high_u16(vmovl_u8(index)) : vget_low_u16(vmovl_u8(index)));
	return vtstq_u32(wide, wide);
}

static inline v16 _lookup16(const mColor* palette, vidx index) {
	uint8x16x2_t bytes = vld2q_u8((const uint8_t*) palette);
	return vorrq_u16(vmovl_u8(vqtbl1_u8(bytes.val[0], index)), vshlq_n_u16(vmovl_u8(vqtbl1_u8(bytes.val[1], index)), 8));
}

// mColorMix5Bit for eight colors
static inline v16 _mix5Bit8(v16 a, v16 b, v16 weightA, v16 weightB) {
	uint16x8_t mask = vdupq_n_u16(0x1F);
	uint16x8_t red = vminq_u16(vshrq_n_u16(vmlaq_u16(vmulq_u16(vshrq_n_u16(a, 11), weightA), vshrq_n_u16(b, 11), weightB), 4), mask);
	uint16x8_t green = vminq_u16(vshrq_n_u16(vmlaq_u16(vmulq_u16(vandq_u16(vshrq_n_u16(a, 6), mask), weightA), vandq_u16(vshrq_n_u16(b, 6), mask), weightB), 4), mask);
	uint16x8_t blue = vminq_u16(vshrq_n_u16(vmlaq_u16(vmulq_u16(vandq_u16(a, mask), weightA), vandq_u16(b, mask), weightB), 4), mask);
	return vorrq_u16(vorrq_u16(vshlq_n_u16(red, 11), vshlq_n_u16(green, 6)), blue);
}
#else
#include <immintrin.h>
typedef __m128i v32;
typedef __m128i v16;
typedef __m128i vidx;

static inline v32 _v32Load(const uint32_t* p) { return _mm_loadu_si128((const __m128i*) p); }
static inline void _v32Store(uint32_t* p, v32 v) { _mm_storeu_si128((__m128i*) p, v); }
static inline v32 _v32(uint32_t x) { return _mm_set1_epi32(x); }
static inline v32 _v32And(v32 a, v32 b) { return _mm_and_si128(a, b); }
static inline v32 _v32Or(v32 a, v32 b) { return _mm_or_si128(a, b); }
static inline v32 _v32Bic(v32 a, v32 b) { return _mm_andnot_si128(b, a); }
static inline v32 _v32Eq(v32 a, v32 b) { return _mm_cmpeq_epi32(a, b); }
static inline v32 _v32Ge(v32 a, v32 b) { return _mm_cmpeq_epi32(_mm_max_epu32(a, b), a); }
static inline v32 _v32Test(v32 a, v32 b) { return _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(a, b), _mm_setzero_si128()), _mm_set1_epi32(-1)); }
static inline v32 _v32Select(v32 mask, v32 a, v32 b) { return _mm_blendv_epi8(b, a, mask); }
static inline bool _v32Any(v32 a, v32 b) { v32 mask = _mm_or_si128(a, b); return !_mm_testz_si128(mask, mask); }
static inline v16 _v16(uint16_t x) { return _mm_set1_epi16(x); }
static inline void _v16Store(uint16_t* p, v16 v) { _mm_storeu_si128((__m128i*) p, v); }
static inline v16 _v16Narrow(v32 low, v32 high) { return _mm_packus_epi32(_mm_and_si128(low, _mm_set1_epi32(0xFFFF)), _mm_and_si128(high, _mm_set1_epi32(0xFFFF))); }
static inline v32 _v16Widen(v16 v, unsigned half) { return _mm_cvtepu16_epi32(half ? _mm_srli_si128(v, 8) : v); }
static inline v16 _v16Select(v16 mask, v16 a, v16 b) { return _mm_blendv_epi8(b, a, mask); }

static inline vidx _nibbles(uint32_t tileData) {
	__m128i bytes = _mm_cvtsi32_si128(tileData);
	__m128i low = _mm_and_si128(bytes, _mm_set1_epi8(0xF));
	__m128i high = _mm_and_si128(_mm_srli_epi16(bytes, 4), _mm_set1_epi8(0xF));
	return _mm_unpacklo_epi8(low, high);
}

static inline v32 _nonzero(vidx index, unsigned half) {
	v32 wide = _mm_cvtepu8_epi32(half ? _mm_srli_si128(index, 4) : index);
	return _v32Test(wide, wide);
}

static inline v16 _lookup16(const mColor* palette, vidx index) {
	const __m128i even = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i odd = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
	__m128i first = _mm_loadu_si128((const __m128i*) palette);
	__m128i second = _mm_loadu_si128((const __m128i*) &palette[8]);
	__m128i low = _mm_unpacklo_epi64(_mm_shuffle_epi8(first, even), _mm_shuffle_epi8(second, even));
	__m128i high = _mm_unpacklo_epi64(_mm_shuffle_epi8(first, odd), _mm_shuffle_epi8(second, odd));
	return _mm_unpacklo_epi8(_mm_shuffle_epi8(low, index), _mm_shuffle_epi8(high, index));
}

static inline v16 _mixChannel(v16 a, v16 b, v16 weightA, v16 weightB) {
	return _mm_min_epu16(_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(a, weightA), _mm_mullo_epi16(b, weightB)), 4), _mm_set1_epi16(0x1F));
}

static inline v16 _mix5Bit8(v16 a, v16 b, v16 weightA, v16 weightB) {
	__m128i mask = _mm_set1_epi16(0x1F);
	__m128i red = _mixChannel(_mm_srli_epi16(a, 11), _mm_srli_epi16(b, 11), weightA, weightB);
	__m128i green = _mixChannel(_mm_and_si128(_mm_srli_epi16(a, 6), mask), _mm_and_si128(_mm_srli_epi16(b, 6), mask), weightA, weightB);
	__m128i blue = _mixChannel(_mm_and_si128(a, mask), _mm_and_si128(b, mask), weightA, weightB);
	return _mm_or_si128(_mm_or_si128(_mm_slli_epi16(red, 11), _mm_slli_epi16(green, 6)), blue);
}
#endif

// Eight 32-bit pixels: one AVX2 register, or two NEON or SSE registers
#ifdef __AVX2__
typedef __m256i p8;

static inline p8 _p8Load(const uint32_t* p) { return _mm256_loadu_si256((const __m256i*) p); }
static inline void _p8Store(uint32_t* p, p8 v) { _mm256_storeu_si256((__m256i*) p, v); }
static inline p8 _p8(uint32_t x) { return _mm256_set1_epi32(x); }
static inline p8 _p8And(p8 a, p8 b) { return _mm256_and_si256(a, b); }
static inline p8 _p8Or(p8 a, p8 b) { return _mm256_or_si256(a, b); }
static inline p8 _p8Bic(p8 a, p8 b) { return _mm256_andnot_si256(b, a); }
static inline p8 _p8Eq(p8 a, p8 b) { return _mm256_cmpeq_epi32(a, b); }
static inline p8 _p8Ge(p8 a, p8 b) { return _mm256_cmpeq_epi32(_mm256_max_epu32(a, b), a); }
static inline p8 _p8Test(p8 a, p8 b) { return _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_and_si256(a, b), _mm256_setzero_si256()), _mm256_set1_epi32(-1)); }
static inline p8 _p8Select(p8 mask, p8 a, p8 b) { return _mm256_blendv_epi8(b, a, mask); }
static inline bool _p8Any(p8 a) { return !_mm256_testz_si256(a, a); }
static inline p8 _p8Widen(v16 v) { return _mm256_cvtepu16_epi32(v); }
static inline p8 _p8Nonzero(vidx index) { p8 wide = _mm256_cvtepu8_epi32(index); return _p8Test(wide, wide); }

static inline v16 _p8Narrow(p8 v) {
	p8 low = _mm256_and_si256(v, _mm256_set1_epi32(0xFFFF));
	return _mm_packus_epi32(_mm256_castsi256_si128(low), _mm256_extracti128_si256(low, 1));
}
#else
typedef struct { v32 low, high; } p8;

static inline p8 _p8Load(const uint32_t* p) { return (p8) { _v32Load(p), _v32Load(p + 4) }; }
static inline void _p8Store(uint32_t* p, p8 v) { _v32Store(p, v.low); _v32Store(p + 4, v.high); }
static inline p8 _p8(uint32_t x) { return (p8) { _v32(x), _v32(x) }; }
static inline p8 _p8And(p8 a, p8 b) { return (p8) { _v32And(a.low, b.low), _v32And(a.high, b.high) }; }
static inline p8 _p8Or(p8 a, p8 b) { return (p8) { _v32Or(a.low, b.low), _v32Or(a.high, b.high) }; }
static inline p8 _p8Bic(p8 a, p8 b) { return (p8) { _v32Bic(a.low, b.low), _v32Bic(a.high, b.high) }; }
static inline p8 _p8Eq(p8 a, p8 b) { return (p8) { _v32Eq(a.low, b.low), _v32Eq(a.high, b.high) }; }
static inline p8 _p8Ge(p8 a, p8 b) { return (p8) { _v32Ge(a.low, b.low), _v32Ge(a.high, b.high) }; }
static inline p8 _p8Test(p8 a, p8 b) { return (p8) { _v32Test(a.low, b.low), _v32Test(a.high, b.high) }; }
static inline p8 _p8Select(p8 mask, p8 a, p8 b) { return (p8) { _v32Select(mask.low, a.low, b.low), _v32Select(mask.high, a.high, b.high) }; }
static inline bool _p8Any(p8 a) { return _v32Any(a.low, a.high); }
static inline p8 _p8Widen(v16 v) { return (p8) { _v16Widen(v, 0), _v16Widen(v, 1) }; }
static inline p8 _p8Nonzero(vidx index) { return (p8) { _nonzero(index, 0), _nonzero(index, 1) }; }
static inline v16 _p8Narrow(p8 v) { return _v16Narrow(v.low, v.high); }
#endif

// _compositeBlendNoObjwin, or _compositeNoBlendNoObjwin, for the eight pixels where write is set
static inline void _composite8(struct GBAVideoSoftwareRenderer* renderer, uint32_t* pixel, p8 current, p8 color, p8 write, bool blend) {
	p8 behind = _p8Ge(color, current);
	p8 below = _p8And(current, _p8(0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN));
	p8 out;
	if (blend) {
		p8 mix = _p8And(_p8And(write, behind), _p8And(_p8Test(current, _p8(FLAG_TARGET_1)), _p8Test(color, _p8(FLAG_TARGET_2))));
		// Most blending rows mix no pixel
		if (_p8Any(mix)) {
			v16 mixed = _mix5Bit8(_p8Narrow(current), _p8Narrow(color), _v16(renderer->blda), _v16(renderer->bldb));
			below = _p8Select(mix, _p8Widen(mixed), below);
		}
		out = _p8Select(behind, below, _p8Bic(color, _p8(FLAG_TARGET_2)));
	} else {
		out = _p8Select(behind, below, color);
	}
	_p8Store(pixel, _p8Select(write, out, current));
}
#endif

#define COMPOSITE_16_OBJWIN(BLEND, IDX)  \
	if (background->objwinForceEnable || (!(current & FLAG_OBJWIN)) == background->objwinOnly) { \
		unsigned color; \
		unsigned mergedFlags = flags; \
		if (current & FLAG_OBJWIN) { \
			mergedFlags = objwinFlags; \
			color = objwinPalette[paletteData | pixelData]; \
		} else if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = renderer->normalPalette[paletteData | pixelData]; \
		} else { \
			color = palette[paletteData | pixelData]; \
		} \
		_composite ## BLEND ## Objwin(renderer, &pixel[IDX], color | mergedFlags, current); \
	}

#define COMPOSITE_16_NO_OBJWIN(BLEND, IDX) \
	{ \
		unsigned color; \
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = renderer->normalPalette[paletteData | pixelData]; \
		} else { \
			color = palette[paletteData | pixelData]; \
		} \
		_composite ## BLEND ## NoObjwin(renderer, &pixel[IDX], color | flags, current); \
	}

#define COMPOSITE_256_OBJWIN(BLEND, IDX) \
	if (background->objwinForceEnable || (!(current & FLAG_OBJWIN)) == background->objwinOnly) { \
		unsigned color; \
		unsigned mergedFlags = flags; \
		if (current & FLAG_OBJWIN) { \
			mergedFlags = objwinFlags; \
			color = objwinPalette[pixelData]; \
		} else if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = renderer->normalPalette[pixelData]; \
		} else { \
			color = palette[pixelData]; \
		} \
		_composite ## BLEND ## Objwin(renderer, &pixel[IDX], color | mergedFlags, current); \
	}

#define COMPOSITE_256_NO_OBJWIN(BLEND, IDX) \
	{ \
		unsigned color; \
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND) { \
			color = renderer->normalPalette[pixelData]; \
		} else { \
			color = palette[pixelData]; \
		} \
		_composite ## BLEND ## NoObjwin(renderer, &pixel[IDX], color | flags, current); \
	}

#define BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, IDX) \
	pixelData = tileData & 0xF; \
	current = pixel[IDX]; \
	if (pixelData && IS_WRITABLE(current)) { \
		COMPOSITE_16_ ## OBJWIN (BLEND, IDX); \
	} \
	tileData >>= 4;

#define BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, IDX) \
	pixelData = tileData & 0xFF; \
	current = pixel[IDX]; \
	if (pixelData && IS_WRITABLE(current)) { \
		COMPOSITE_256_ ## OBJWIN (BLEND, IDX); \
	} \
	tileData >>= 8;

// TODO: Remove UNUSEDs after implementing OBJWIN for modes 3 - 5
#define PREPARE_OBJWIN                                                                            \
	int objwinSlowPath = GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt);                     \
	mColor* objwinPalette = renderer->normalPalette;                                             \
	if (renderer->d.highlightAmount && background->highlight) {                                   \
		objwinPalette = renderer->highlightPalette;                                               \
	}                                                                                             \
	UNUSED(objwinPalette);                                                                        \
	if (objwinSlowPath) {                                                                         \
		if (background->target1 && GBAWindowControlIsBlendEnable(renderer->objwin.packed) &&      \
		    (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN)) { \
			objwinPalette = renderer->variantPalette;                                             \
			if (renderer->d.highlightAmount && background->highlight) {                           \
				palette = renderer->highlightVariantPalette;                                      \
			}                                                                                     \
		}                                                                                         \
	}

#define BACKGROUND_BITMAP_INIT                                                                                        \
	int32_t x = background->sx + (renderer->start - 1) * background->dx;                                              \
	int32_t y = background->sy + (renderer->start - 1) * background->dy;                                              \
	int mosaicH = 0;                                                                                                  \
	int mosaicWait = 0;                                                                                               \
	int32_t localX;                                                                                                   \
	int32_t localY;                                                                                                   \
	if (background->mosaic) {                                                                                         \
		int mosaicV = GBAMosaicControlGetBgV(renderer->mosaic) + 1;                                                   \
		mosaicH = GBAMosaicControlGetBgH(renderer->mosaic) + 1;                                                       \
		mosaicWait = (mosaicH - renderer->start + GBA_VIDEO_HORIZONTAL_PIXELS * mosaicH) % mosaicH;                   \
		int32_t startX = renderer->start - (renderer->start % mosaicH);                                               \
		--mosaicH;                                                                                                    \
		localX = -(inY % mosaicV) * background->dmx;                                                                  \
		localY = -(inY % mosaicV) * background->dmy;                                                                  \
		x += localX;                                                                                                  \
		y += localY;                                                                                                  \
		localX += background->sx + startX * background->dx;                                                           \
		localY += background->sy + startX * background->dy;                                                           \
	}                                                                                                                 \
                                                                                                                      \
	uint32_t flags = background->flags;                                                                               \
	uint32_t objwinFlags = background->objwinFlags;                                                                   \
	bool variant = background->variant;                                                                               \
	mColor* palette = renderer->normalPalette;                                                                       \
	if (renderer->d.highlightAmount && background->highlight) {                                                       \
		palette = renderer->highlightPalette;                                                                         \
	}                                                                                                                 \
	if (variant) {                                                                                                    \
		palette = renderer->variantPalette;                                                                           \
		if (renderer->d.highlightAmount && background->highlight) {                                                   \
			palette = renderer->highlightVariantPalette;                                                              \
		}                                                                                                             \
	}                                                                                                                 \
	UNUSED(palette);                                                                                                  \
	PREPARE_OBJWIN;

#define TEST_LAYER_ENABLED(X) \
	!softwareRenderer->d.disableBG[X] && \
	(softwareRenderer->bg[X].enabled == ENABLED_MAX && \
	(GBAWindowControlIsBg ## X ## Enable(softwareRenderer->currentWindow.packed) || \
	(GBARegisterDISPCNTIsObjwinEnable(softwareRenderer->dispcnt) && GBAWindowControlIsBg ## X ## Enable (softwareRenderer->objwin.packed))) && \
	softwareRenderer->bg[X].priority == priority)

static inline unsigned _brighten(unsigned color, int y) {
	unsigned c = 0;
	unsigned a;
#ifdef COLOR_16_BIT
	a = color & 0x1F;
	c |= (a + ((0x1F - a) * y) / 16) & 0x1F;

#ifdef COLOR_5_6_5
	a = color & 0x7C0;
	c |= (a + ((0x7C0 - a) * y) / 16) & 0x7C0;

	a = color & 0xF800;
	c |= (a + ((0xF800 - a) * y) / 16) & 0xF800;
#else
	a = color & 0x3E0;
	c |= (a + ((0x3E0 - a) * y) / 16) & 0x3E0;

	a = color & 0x7C00;
	c |= (a + ((0x7C00 - a) * y) / 16) & 0x7C00;
#endif
#else
	a = color & 0xFF;
	c |= (a + ((0xFF - a) * y) / 16) & 0xFF;

	a = color & 0xFF00;
	c |= (a + ((0xFF00 - a) * y) / 16) & 0xFF00;

	a = color & 0xFF0000;
	c |= (a + ((0xFF0000 - a) * y) / 16) & 0xFF0000;
#endif
	return c;
}

static inline unsigned _darken(unsigned color, int y) {
	unsigned c = 0;
	unsigned a;
#ifdef COLOR_16_BIT
	a = color & 0x1F;
	c |= (a - (a * y) / 16) & 0x1F;

#ifdef COLOR_5_6_5
	a = color & 0x7C0;
	c |= (a - (a * y) / 16) & 0x7C0;

	a = color & 0xF800;
	c |= (a - (a * y) / 16) & 0xF800;
#else
	a = color & 0x3E0;
	c |= (a - (a * y) / 16) & 0x3E0;

	a = color & 0x7C00;
	c |= (a - (a * y) / 16) & 0x7C00;
#endif
#else
	a = color & 0xFF;
	c |= (a - (a * y) / 16) & 0xFF;

	a = color & 0xFF00;
	c |= (a - (a * y) / 16) & 0xFF00;

	a = color & 0xFF0000;
	c |= (a - (a * y) / 16) & 0xFF0000;
#endif
	return c;
}

#endif
