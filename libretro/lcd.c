/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's LCD effects on the picture it sends: LCD Ghosting, then LCD Grid with the Game Boy's
// pixel shadow (lcd.h has the screens' values). Both share their rows out among the CPU's cores.

#include "core.h"
#include "gb.h"
#include "lcd.h"
#include "workers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

int ghosting;
bool interleaved;
int gridScale = 1;
int gridWay;
int lcdThreads;

// LCD Ghosting: the screens' liquid crystal takes time to change, darkening, as it is driven,
// faster than it lightens, as it relaxes. Each subpixel moves in linear light from where it was
// towards the new frame by a share each frame. The time constants, in milliseconds to darken and to
// lighten, are estimates for each kind of panel: the first Game Boy's STN, the Pocket's and Light's
// FSTN, and the Color's and Advance's reflective TFTs; Half halves them. With Interleaved Rows on, the
// Advance's also drives alternate rows harder each frame, the other rows the next, the weaker rows
// moving half as far, so a picture that alternates each frame shows in stripes, as it does on the
// console (as SameBoy's accurate frame blending also has it). Each subpixel's level is kept 8-bit,
// gamma encoded, and its next level read from a table built for the screen and the picture's channel
// widths, the rows shared out among the CPU's cores. A frame RetroArch runs again, and doesn't show,
// leaves the screen as it is.
enum { SCREEN_DMG, SCREEN_POCKET, SCREEN_TFT, SCREEN_KINDS };
static const float screenTimes[SCREEN_KINDS][2] = { { 50, 110 }, { 25, 50 }, { 15, 30 } };

static int screenKind(void) {
	if (!gameBoy || libretroGbOnAdvance() || libretroGbIsColor()) {
		return SCREEN_TFT;
	}
	return gbModel == LIBRETRO_GB_MGB || gbShades == 1 || gbShades == 3 ? SCREEN_POCKET : SCREEN_DMG;
}

// Whether the picture is on the Game Boy Advance's panel: a Game Boy Advance game's, or a Game Boy
// game's on the Advance model unless the Color's panel was chosen.
static bool advancePanel(void) {
	return !gameBoy || (libretroGbOnAdvance() && !gbColorPanel);
}

enum LibretroLcd libretroLcd(uint32_t* gap) {
	int const kind = screenKind();
	*gap = kind == SCREEN_TFT ? 0 : libretroGbOffColor();
	if (kind != SCREEN_TFT) {
		return kind == SCREEN_POCKET ? LIBRETRO_LCD_POCKET : LIBRETRO_LCD_DMG;
	}
	return advancePanel() ? LIBRETRO_LCD_ADVANCE : LIBRETRO_LCD_COLOR;
}

// LCD Grid: the picture gridScale times larger, with the gaps between the console screen's pixels drawn
// in and, on the first Game Boy and the Pocket, the shadow its pixels cast, as the standalone frontend
// draws them on the GPU (lcd.h has the values), for RetroArch without its shader presets. The scale
// is whole, so every pixel's square has the same gaps: each source row is blended toward the gap color
// once for each share of gap the square uses, those rows woven into the larger one, and rows alike
// copied. The shadow is the picture's darkness below the unlit screen, moved and sampled smoothly
// once for each square, which darkens the square's blends, gaps and all: much cheaper than shading
// every pixel of the larger picture, as the GPU does, and at the size shown hardly different. The
// loops work on whole rows in vectors, NEON on ARM, SSE or AVX on x86, the rows shared out among the
// CPU's cores.

#if defined(__AVX2__)
#define GRID_VECTOR 32
#else
#define GRID_VECTOR 16
#endif
typedef uint16_t GridU16 __attribute__((vector_size(GRID_VECTOR)));
typedef uint32_t GridU32 __attribute__((vector_size(GRID_VECTOR)));
enum { GRID_LANES16 = GRID_VECTOR / 2, GRID_LANES32 = GRID_VECTOR / 4 };
// Bytes, as many as a vector's 16- or 32-bit lanes, to widen into them.
typedef uint8_t GridBytes16 __attribute__((vector_size(GRID_LANES16)));
typedef uint8_t GridBytes32 __attribute__((vector_size(GRID_LANES32)));

// x / 255, rounded, for x up to 255 * 255.
#define GRID_DIV255(x) (((x) + 128 + (((x) + 128) >> 8)) >> 8)

// A row of RGB565 pixels blended toward the gap color by a of 255.
static void gridBlend565(const uint16_t* from, uint16_t* to, unsigned count, unsigned a, unsigned gapR, unsigned gapG, unsigned gapB) {
	unsigned const keep = 255 - a;
	uint16_t const keep16 = (uint16_t) keep, addR = (uint16_t) (gapR * a), addG = (uint16_t) (gapG * a), addB = (uint16_t) (gapB * a);
	unsigned i = 0;
	for (; i + GRID_LANES16 <= count; i += GRID_LANES16) {
		GridU16 c;
		memcpy(&c, from + i, sizeof(c));
		GridU16 r = c >> 11, g = c >> 5 & 0x3F, b = c & 0x1F;
		r = (GridU16) (r << 3 | r >> 2);
		g = (GridU16) (g << 2 | g >> 4);
		b = (GridU16) (b << 3 | b >> 2);
		r = GRID_DIV255(r * keep16 + addR);
		g = GRID_DIV255(g * keep16 + addG);
		b = GRID_DIV255(b * keep16 + addB);
		GridU16 const out = (GridU16) ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		unsigned const c = from[i];
		unsigned r = (c >> 11) << 3 | c >> 13, g = (c >> 5 & 0x3F) << 2 | (c >> 9 & 3), b = (c & 0x1F) << 3 | (c >> 2 & 7);
		r = GRID_DIV255(r * keep + gapR * a);
		g = GRID_DIV255(g * keep + gapG * a);
		b = GRID_DIV255(b * keep + gapB * a);
		to[i] = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
	}
}

// Each 8-bit channel of XRGB8888 pixels times k, a share of 256 for each pixel, in 16-bit multiplies,
// which SSE2 and NEON have: red and blue together in one lane pair, green in another.
static inline GridU32 gridScale8888(GridU32 c, GridU32 k) {
	GridU16 const both = (GridU16) (k | k << 16);
	GridU16 const rb = (GridU16) (c & 0xFF00FF) * both >> 8;
	GridU16 const g = (GridU16) (c >> 8 & 0xFF) * both >> 8;
	return (c & 0xFF000000) | (GridU32) rb | (GridU32) g << 8;
}

static inline uint32_t gridScale8888One(uint32_t c, uint32_t k) {
	return (c & 0xFF000000) | ((c & 0xFF00FF) * k >> 8 & 0xFF00FF) | ((c & 0xFF00) * k >> 8 & 0xFF00);
}

static void gridBlend8888(const uint32_t* from, uint32_t* to, unsigned count, unsigned a, unsigned gapR, unsigned gapG, unsigned gapB) {
	// Shares of 256: a of 255 rounds to within one level.
	uint32_t const toward = a + (a >> 7), keep = 256 - toward;
	uint32_t const gap = gridScale8888One(gapR << 16 | gapG << 8 | gapB, toward) & 0xFFFFFF;
	GridU32 const keeps = (GridU32) { 0 } + keep;
	unsigned i = 0;
	for (; i + GRID_LANES32 <= count; i += GRID_LANES32) {
		GridU32 c;
		memcpy(&c, from + i, sizeof(c));
		GridU32 const out = gridScale8888(c, keeps) + gap;
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		to[i] = gridScale8888One(from[i], keep) + gap;
	}
}

#if defined(__AVX2__) && !defined(__aarch64__)
// Rows woven with AVX2, 32 bytes of each at a time. Elements of 2, 4 or 8 bytes are interleaved
// within each 128-bit half by unpacking, the halves then put in order; three rows of 4- or 8-byte
// elements by permuting each across the register and blending. Six and eight rows are three and
// four rows of pairs.

// a and b interleaved, size-byte elements, in order: first holds the first half of the pairs.
static inline void gridAvxPairs(__m256i a, __m256i b, int size, __m256i* first, __m256i* second) {
	__m256i low, high;
	if (size == 2) {
		low = _mm256_unpacklo_epi16(a, b);
		high = _mm256_unpackhi_epi16(a, b);
	} else if (size == 4) {
		low = _mm256_unpacklo_epi32(a, b);
		high = _mm256_unpackhi_epi32(a, b);
	} else {
		low = _mm256_unpacklo_epi64(a, b);
		high = _mm256_unpackhi_epi64(a, b);
	}
	*first = _mm256_permute2x128_si256(low, high, 0x20);
	*second = _mm256_permute2x128_si256(low, high, 0x31);
}

// Four rows: two levels of pairs. 64 bytes of elements of 2 * size bytes, in order, per pair level.
static inline void gridAvxFour(__m256i a, __m256i b, __m256i c, __m256i d, int size, __m256i* to) {
	__m256i ab0, ab1, cd0, cd1;
	gridAvxPairs(a, b, size, &ab0, &ab1);
	gridAvxPairs(c, d, size, &cd0, &cd1);
	gridAvxPairs(ab0, cd0, size * 2, &to[0], &to[1]);
	gridAvxPairs(ab1, cd1, size * 2, &to[2], &to[3]);
}

// Three rows of 4-byte elements, 8 each: a0 b0 c0 a1 b1 c1 ...
static inline void gridAvxThree32(__m256i a, __m256i b, __m256i c, __m256i* to) {
	__m256i const ia0 = _mm256_setr_epi32(0, 0, 0, 1, 0, 0, 2, 0), ib0 = _mm256_setr_epi32(0, 0, 0, 0, 1, 0, 0, 2), ic0 = _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0);
	__m256i const ia1 = _mm256_setr_epi32(0, 3, 0, 0, 4, 0, 0, 5), ib1 = _mm256_setr_epi32(0, 0, 3, 0, 0, 4, 0, 0), ic1 = _mm256_setr_epi32(2, 0, 0, 3, 0, 0, 4, 0);
	__m256i const ia2 = _mm256_setr_epi32(0, 0, 6, 0, 0, 7, 0, 0), ib2 = _mm256_setr_epi32(5, 0, 0, 6, 0, 0, 7, 0), ic2 = _mm256_setr_epi32(0, 5, 0, 0, 6, 0, 0, 7);
	__m256i const b0 = _mm256_permutevar8x32_epi32(b, ib0), c0 = _mm256_permutevar8x32_epi32(c, ic0);
	to[0] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia0), b0, 0x92), c0, 0x24);
	to[1] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia1), _mm256_permutevar8x32_epi32(b, ib1), 0x24),
	                           _mm256_permutevar8x32_epi32(c, ic1), 0x49);
	to[2] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia2), _mm256_permutevar8x32_epi32(b, ib2), 0x49),
	                           _mm256_permutevar8x32_epi32(c, ic2), 0x92);
}

// Three rows of 8-byte elements, 4 each: a0 b0 c0 a1 | b1 c1 a2 b2 | c2 a3 b3 c3.
static inline void gridAvxThree64(__m256i a, __m256i b, __m256i c, __m256i* to) {
	__m256i const ia0 = _mm256_setr_epi32(0, 1, 0, 0, 0, 0, 2, 3), ib0 = _mm256_setr_epi32(0, 0, 0, 1, 0, 0, 0, 0), ic0 = _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0);
	__m256i const ia1 = _mm256_setr_epi32(0, 0, 0, 0, 4, 5, 0, 0), ib1 = _mm256_setr_epi32(2, 3, 0, 0, 0, 0, 4, 5), ic1 = _mm256_setr_epi32(0, 0, 2, 3, 0, 0, 0, 0);
	__m256i const ia2 = _mm256_setr_epi32(0, 0, 6, 7, 0, 0, 0, 0), ib2 = _mm256_setr_epi32(0, 0, 0, 0, 6, 7, 0, 0), ic2 = _mm256_setr_epi32(4, 5, 0, 0, 0, 0, 6, 7);
	to[0] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia0), _mm256_permutevar8x32_epi32(b, ib0), 0x0C),
	                           _mm256_permutevar8x32_epi32(c, ic0), 0x30);
	to[1] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia1), _mm256_permutevar8x32_epi32(b, ib1), 0xC3),
	                           _mm256_permutevar8x32_epi32(c, ic1), 0x0C);
	to[2] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia2), _mm256_permutevar8x32_epi32(b, ib2), 0x30),
	                           _mm256_permutevar8x32_epi32(c, ic2), 0xC3);
}

static inline __m256i gridAvxLoad(const uint8_t* row, unsigned at, int size) {
	return _mm256_loadu_si256((const __m256i*) (row + (size_t) at * size));
}

// Weaves what it can; returns how many of each row it did.
static unsigned gridWeaveAvx2(uint8_t* to, const uint8_t* const* from, int s, unsigned count, int size) {
	unsigned const step = 32 / size;
	unsigned done = 0;
	for (; done + step <= count; done += step) {
		__m256i* const t = (__m256i*) (to + (size_t) done * s * size);
		__m256i r[8];
		if (s == 2) {
			gridAvxPairs(gridAvxLoad(from[0], done, size), gridAvxLoad(from[1], done, size), size, &r[0], &r[1]);
			_mm256_storeu_si256(t, r[0]);
			_mm256_storeu_si256(t + 1, r[1]);
		} else if (s == 4) {
			gridAvxFour(gridAvxLoad(from[0], done, size), gridAvxLoad(from[1], done, size), gridAvxLoad(from[2], done, size),
			            gridAvxLoad(from[3], done, size), size, r);
			for (int k = 0; k < 4; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 3 && size == 4) {
			gridAvxThree32(gridAvxLoad(from[0], done, 4), gridAvxLoad(from[1], done, 4), gridAvxLoad(from[2], done, 4), r);
			for (int k = 0; k < 3; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 6 && (size == 2 || size == 4)) {
			// Three rows of pairs, each pair row two registers long.
			__m256i p[3][2];
			for (int k = 0; k < 3; ++k) {
				gridAvxPairs(gridAvxLoad(from[k * 2], done, size), gridAvxLoad(from[k * 2 + 1], done, size), size, &p[k][0], &p[k][1]);
			}
			for (int h = 0; h < 2; ++h) {
				if (size == 2) {
					gridAvxThree32(p[0][h], p[1][h], p[2][h], r + h * 3);
				} else {
					gridAvxThree64(p[0][h], p[1][h], p[2][h], r + h * 3);
				}
			}
			for (int k = 0; k < 6; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 8 && size == 2) {
			// Four rows of pairs.
			__m256i p[4][2];
			for (int k = 0; k < 4; ++k) {
				gridAvxPairs(gridAvxLoad(from[k * 2], done, size), gridAvxLoad(from[k * 2 + 1], done, size), size, &p[k][0], &p[k][1]);
			}
			for (int h = 0; h < 2; ++h) {
				gridAvxFour(p[0][h], p[1][h], p[2][h], p[3][h], size * 2, r + h * 4);
			}
			for (int k = 0; k < 8; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else {
			break;
		}
	}
	return done;
}
#endif

// gridWeave16 and 32: rows woven together, to[x * s + k] = from[k][x], for s rows of count
// elements of that many bits. On 64-bit ARM the interleaving stores do it, two to four rows at once,
// and six or eight as three or four rows of pairs zipped into elements twice as wide; with AVX2,
// gridWeaveAvx2; what is left, one element at a time. bits, lanes: the element and how many fill a
// NEON register; wide, wideLanes: the same for a pair.
#if defined(__aarch64__)
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes)                                                                       \
	if (s == 2 || s == 3 || s == 4) {                                                                                         \
		for (; done + lanes <= count; done += lanes) {                                                                        \
			uint##bits##x##lanes##_t const a = vld1q_u##bits(from[0] + done), b = vld1q_u##bits(from[1] + done);             \
			if (s == 2) {                                                                                                     \
				vst2q_u##bits(to + done * 2, ((uint##bits##x##lanes##x2_t) { { a, b } }));                                    \
			} else if (s == 3) {                                                                                              \
				vst3q_u##bits(to + done * 3, ((uint##bits##x##lanes##x3_t) { { a, b, vld1q_u##bits(from[2] + done) } }));     \
			} else {                                                                                                          \
				vst4q_u##bits(to + done * 4,                                                                                  \
				              ((uint##bits##x##lanes##x4_t) { { a, b, vld1q_u##bits(from[2] + done), vld1q_u##bits(from[3] + done) } })); \
			}                                                                                                                 \
		}                                                                                                                     \
	} else if (s == 6 || s == 8) {                                                                                            \
		for (; done + lanes <= count; done += lanes) {                                                                        \
			uint##wide##x##wideLanes##_t lows[4], highs[4];                                                                   \
			for (int k = 0; k < s / 2; ++k) {                                                                                 \
				uint##bits##x##lanes##_t const a = vld1q_u##bits(from[k * 2] + done), b = vld1q_u##bits(from[k * 2 + 1] + done); \
				lows[k] = vreinterpretq_u##wide##_u##bits(vzip1q_u##bits(a, b));                                              \
				highs[k] = vreinterpretq_u##wide##_u##bits(vzip2q_u##bits(a, b));                                             \
			}                                                                                                                 \
			uint##wide##_t* const t = (uint##wide##_t*) (to + done * s);                                                      \
			if (s == 6) {                                                                                                     \
				vst3q_u##wide(t, ((uint##wide##x##wideLanes##x3_t) { { lows[0], lows[1], lows[2] } }));                       \
				vst3q_u##wide(t + 3 * wideLanes, ((uint##wide##x##wideLanes##x3_t) { { highs[0], highs[1], highs[2] } }));    \
			} else {                                                                                                          \
				vst4q_u##wide(t, ((uint##wide##x##wideLanes##x4_t) { { lows[0], lows[1], lows[2], lows[3] } }));              \
				vst4q_u##wide(t + 4 * wideLanes, ((uint##wide##x##wideLanes##x4_t) { { highs[0], highs[1], highs[2], highs[3] } })); \
			}                                                                                                                 \
		}                                                                                                                     \
	}
#elif defined(__AVX2__)
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes) done = gridWeaveAvx2((uint8_t*) to, (const uint8_t* const*) from, s, count, bits / 8)
#else
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes)
#endif

#define GRID_WEAVE(bits, lanes, wide, wideLanes)                                                                     \
	static void gridWeave##bits(uint##bits##_t* to, const uint##bits##_t* const* from, int s, unsigned count) { \
		unsigned done = 0;                                                                                       \
		GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes);                                                         \
		for (int k = 0; k < s; ++k) {                                                                            \
			for (unsigned x = done; x < count; ++x) {                                                            \
				to[x * s + k] = from[k][x];                                                                      \
			}                                                                                                    \
		}                                                                                                        \
	}
GRID_WEAVE(16, 8, 32, 4)
GRID_WEAVE(32, 4, 64, 2)

// A buffer kept from frame to frame, grown when a frame needs more.
struct GridBuffer {
	uint8_t* data;
	size_t size;
};

// The rows are shared out among up to this many threads, the core's own and workers (workers.h)
// started for them unless linked consoles have them already; each has its own scratch rows. Auto
// leaves a core for the frontend's own threads, and another with Threaded Video, whose render thread
// waits on a core of its own as the frame ends: on an RK3566, 3 threads, or 2, were the fastest.
enum { GRID_JOBS = 4 };
static int postJobs = 1;

// The larger picture; for each thread, the source row blended for each share of gap and each row of a
// square unlike those above it; the shadow's darkness at the source's size, with 8 more on each side
// and 2 more rows above and below; for each thread, a row of each square's share kept from the shadow.
static struct GridBuffer gridOut, gridBlends, gridTemplates, gridDark, gridKeeps;

// Whether b holds wanted bytes, growing it if it didn't.
static bool gridReserve(struct GridBuffer* b, size_t wanted) {
	if (wanted > b->size) {
		uint8_t* const grown = realloc(b->data, wanted);
		if (!grown) {
			return false;
		}
		b->data = grown;
		b->size = wanted;
	}
	return true;
}

enum { GRID_SHARES_MAX = 64 };

// A square's layout for a scale and screen: the different shares of gap, 0 to 255, its pixels have;
// each pixel's, as an index into them; and each row's first row alike, as rows alike share a template.
static struct {
	int scale, lcd, shareCount;
	uint8_t shares[GRID_SHARES_MAX];
	uint8_t shareOf[8][8];
	int firstAlike[8];
} gridLayout = { .lcd = -1 };

static void gridLay(int s, enum LibretroLcd lcd) {
	if (gridLayout.scale == s && gridLayout.lcd == (int) lcd) {
		return;
	}
	const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
	int across[8], down[8];
	for (int i = 0; i < s; ++i) {
		float const covered = libretroLcdGapCoverage(i, (float) s, look->share * s);
		across[i] = (int) lroundf(covered * look->across * 255);
		down[i] = (int) lroundf(covered * look->down * 255);
	}
	gridLayout.shareCount = 0;
	for (int y = 0; y < s; ++y) {
		for (int x = 0; x < s; ++x) {
			int const a = across[x] + down[y] - (across[x] * down[y] + 127) / 255;
			int k = 0;
			while (k < gridLayout.shareCount && gridLayout.shares[k] != a) {
				++k;
			}
			if (k == gridLayout.shareCount) {
				gridLayout.shares[gridLayout.shareCount++] = (uint8_t) a;
			}
			gridLayout.shareOf[y][x] = (uint8_t) k;
		}
	}
	for (int y = 0; y < s; ++y) {
		gridLayout.firstAlike[y] = y;
		for (int k = 0; k < y && gridLayout.firstAlike[y] == y; ++k) {
			gridLayout.firstAlike[y] = !memcmp(gridLayout.shareOf[k], gridLayout.shareOf[y], (size_t) s) ? k : y;
		}
	}
	gridLayout.scale = s;
	gridLayout.lcd = lcd;
}

// The frame the threads draw.
static struct GridFrame {
	const uint8_t* picture;
	unsigned width, height;
	size_t pitch, bytes;
	int s;
	uint32_t gap;
	bool shadow;
	// Where the shadow on a square comes from: the source pixel above and left of that point, as
	// offsets, and the weights of the ones after; the darkness from start down, by scale / 65536.
	int fromX, fromY, weightX, weightY, start, scale;
} gridFrame;

// The rows of height a thread draws: an even share of them.
static void rowsOf(int job, unsigned height, unsigned* from, unsigned* to) {
	*from = height * (unsigned) job / (unsigned) postJobs;
	*to = height * (unsigned) (job + 1) / (unsigned) postJobs;
}

// The threads for this frame's post-processing, and workers for them unless linked consoles' are
// running.
static void startJobs(void) {
	static long cores;
	if (!cores) {
		cores = sysconf(_SC_NPROCESSORS_ONLN);
	}
	int const spare = (int) cores - 1 - (threadedVideo != 0);
	postJobs = lcdThreads ? lcdThreads : spare > GRID_JOBS ? GRID_JOBS : spare < 1 ? 1 : spare;
	if (!gbaTogether && workersAsked() != (postJobs > 1 ? postJobs : 0)) {
		if (postJobs > 1) {
			workersStart(postJobs);
		} else {
			workersStop();
		}
	}
}

// Runs job for each post-processing thread.
static void runJobs(void (*job)(int index)) {
	if (postJobs > 1) {
		workersRun(job, postJobs);
	} else {
		job(0);
	}
}

// Each pixel's darkness, in a row of count XRGB8888 pixels: 0 down to start, then rising by scale
// / 65536 a level; returns whether any is dark.
static bool gridDarkRow(const uint32_t* row, uint8_t* to, unsigned count, int start, int scale) {
	GridU32 const starts = (GridU32) { 0 } + (uint32_t) start, scales = (GridU32) { 0 } + (uint32_t) scale;
	GridU32 seen = { 0 };
	unsigned x = 0;
	for (; x + GRID_LANES32 <= count; x += GRID_LANES32) {
		GridU32 c;
		memcpy(&c, row + x, sizeof(c));
		GridU32 const level = (c >> 16 & 0xFF) * 2 + (c >> 8 & 0xFF) * 5 + (c & 0xFF);
		GridU32 const dark = (GridU32) (level < starts) & ((starts - level) * scales >> 16);
		GridBytes32 const bytes = __builtin_convertvector(dark, GridBytes32);
		memcpy(to + x, &bytes, sizeof(bytes));
		seen |= dark;
	}
	bool any = false;
	for (int i = 0; i < GRID_LANES32; ++i) {
		any |= seen[i] != 0;
	}
	for (; x < count; ++x) {
		uint32_t const c = row[x];
		int const level = 2 * (int) (c >> 16 & 0xFF) + 5 * (int) (c >> 8 & 0xFF) + (int) (c & 0xFF);
		to[x] = (uint8_t) (level < start ? (start - level) * scale >> 16 : 0);
		any |= to[x] != 0;
	}
	return any;
}

// A thread's rows of the shadow's darkness map.
static void gridDarkJob(int job) {
	unsigned from, to;
	rowsOf(job, gridFrame.height, &from, &to);
	for (unsigned y = from; y < to; ++y) {
		uint8_t* const d = gridDark.data + (size_t) (y + 2) * (gridFrame.width + 16) + 8;
		gridDarkRow((const uint32_t*) (gridFrame.picture + y * gridFrame.pitch), d, gridFrame.width, gridFrame.start, gridFrame.scale);
	}
}

// How much of each square of source row y the shadow leaves, 255 for all: the darkness map sampled
// smoothly where the shadow over the square's middle comes from, beyond the picture none.
static void gridKeepRow(unsigned y, uint8_t* keep) {
	unsigned const width = gridFrame.width;
	size_t const stride = width + 16;
	const uint8_t* const top = gridDark.data + (size_t) (y + 2 + gridFrame.fromY) * stride + 8 + gridFrame.fromX;
	const uint8_t* const bottom = top + stride;
	uint16_t const right = (uint16_t) gridFrame.weightX, left = (uint16_t) (256 - right);
	uint16_t const below = (uint16_t) gridFrame.weightY, above = (uint16_t) (256 - below);
	unsigned x = 0;
	for (; x + GRID_LANES16 <= width; x += GRID_LANES16) {
		GridBytes16 a, b, c, d;
		memcpy(&a, top + x, sizeof(a));
		memcpy(&b, top + x + 1, sizeof(b));
		memcpy(&c, bottom + x, sizeof(c));
		memcpy(&d, bottom + x + 1, sizeof(d));
		GridU16 const upper = (__builtin_convertvector(a, GridU16) * left + __builtin_convertvector(b, GridU16) * right) >> 8;
		GridU16 const lower = (__builtin_convertvector(c, GridU16) * left + __builtin_convertvector(d, GridU16) * right) >> 8;
		GridBytes16 const kept = __builtin_convertvector(255 - ((upper * above + lower * below) >> 8), GridBytes16);
		memcpy(keep + x, &kept, sizeof(kept));
	}
	for (; x < width; ++x) {
		unsigned const upper = (top[x] * left + top[x + 1] * right) >> 8, lower = (bottom[x] * left + bottom[x + 1] * right) >> 8;
		keep[x] = (uint8_t) (255 - ((upper * above + lower * below) >> 8));
	}
}

// A blended XRGB8888 row darkened by the shadow: each byte times keep + 1, of 256. Runs the shadow
// leaves alone are passed over.
static void gridShadeRow(uint32_t* row, const uint8_t* keep, unsigned width) {
	unsigned x = 0;
#if defined(__aarch64__)
	// 16 pixels at a time: each pixel's keep copied to its four bytes by a table lookup, then 8-bit
	// multiplies that widen to 16 bits.
	static const uint8_t spreads[4][16] = {
		{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 },
		{ 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7 },
		{ 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11 },
		{ 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15 },
	};
	for (; x + 16 <= width; x += 16) {
		uint8x16_t const kept = vld1q_u8(keep + x);
		if (vminvq_u8(kept) == 255) {
			continue;
		}
		for (int q = 0; q < 4; ++q) {
			uint8_t* const at = (uint8_t*) (row + x + q * 4);
			uint8x16_t const c = vld1q_u8(at), k = vqtbl1q_u8(kept, vld1q_u8(spreads[q]));
			uint16x8_t const low = vaddw_u8(vmull_u8(vget_low_u8(c), vget_low_u8(k)), vget_low_u8(c));
			uint16x8_t const high = vaddw_high_u8(vmull_high_u8(c, k), c);
			vst1q_u8(at, vshrn_high_n_u16(vshrn_n_u16(low, 8), high, 8));
		}
	}
#else
	for (; x + GRID_LANES32 <= width; x += GRID_LANES32) {
		uint64_t all = 0;
		memcpy(&all, keep + x, GRID_LANES32);
		if (all == ~0ull >> (64 - 8 * GRID_LANES32)) {
			continue;
		}
		GridBytes32 k;
		memcpy(&k, keep + x, sizeof(k));
		GridU32 const keeps = __builtin_convertvector(k, GridU32);
		GridU32 c;
		memcpy(&c, row + x, sizeof(c));
		GridU32 const out = gridScale8888(c, keeps + 1);
		memcpy(row + x, &out, sizeof(out));
	}
#endif
	for (; x < width; ++x) {
		row[x] = gridScale8888One(row[x], keep[x] + 1u);
	}
}

// One source row's templates: the row blended for each share of gap, darkened by the shadow when
// there is one, then each row of a square unlike those above it woven from the blends its columns use.
static void gridMakeTemplates(const uint8_t* row, uint8_t* blends, uint8_t* templates, const uint8_t* keep) {
	unsigned const width = gridFrame.width, gap = gridFrame.gap;
	unsigned const gapR = gap >> 16 & 0xFF, gapG = gap >> 8 & 0xFF, gapB = gap & 0xFF;
	size_t const bytes = gridFrame.bytes, rowBytes = width * bytes, outPitch = rowBytes * gridFrame.s;
	for (int k = 0; k < gridLayout.shareCount; ++k) {
		void* const blended = blends + k * rowBytes;
		if (!gridLayout.shares[k]) {
			memcpy(blended, row, rowBytes);
		} else if (bytes == 4) {
			gridBlend8888((const uint32_t*) row, blended, width, gridLayout.shares[k], gapR, gapG, gapB);
		} else {
			gridBlend565((const uint16_t*) row, blended, width, gridLayout.shares[k], gapR, gapG, gapB);
		}
		if (keep) {
			gridShadeRow(blended, keep, width);
		}
	}
	for (int sy = 0; sy < gridFrame.s; ++sy) {
		if (gridLayout.firstAlike[sy] != sy) {
			continue;
		}
		uint8_t* const line = templates + sy * outPitch;
		const void* from[8];
		for (int sx = 0; sx < gridFrame.s; ++sx) {
			from[sx] = blends + gridLayout.shareOf[sy][sx] * rowBytes;
		}
		if (bytes == 4) {
			gridWeave32((uint32_t*) line, (const uint32_t* const*) from, gridFrame.s, width);
		} else {
			gridWeave16((uint16_t*) line, (const uint16_t* const*) from, gridFrame.s, width);
		}
	}
}

// A thread's rows of the larger picture, each from its template.
static void gridDrawJob(int job) {
	unsigned from, to;
	rowsOf(job, gridFrame.height, &from, &to);
	int const s = gridFrame.s;
	size_t const rowBytes = gridFrame.width * gridFrame.bytes, outPitch = rowBytes * s;
	uint8_t* const blends = gridBlends.data + (size_t) job * GRID_SHARES_MAX * rowBytes;
	uint8_t* const templates = gridTemplates.data + (size_t) job * 8 * outPitch;
	uint8_t* const keep = gridFrame.shadow ? gridKeeps.data + (size_t) job * gridFrame.width : NULL;
	for (unsigned y = from; y < to; ++y) {
		if (keep) {
			gridKeepRow(y, keep);
		}
		gridMakeTemplates(gridFrame.picture + y * gridFrame.pitch, blends, templates, keep);
		for (int sy = 0; sy < s; ++sy) {
			memcpy(gridOut.data + (size_t) (y * s + sy) * outPitch, templates + (size_t) gridLayout.firstAlike[sy] * outPitch, outPitch);
		}
	}
}

void sendPicture(const void* picture, unsigned width, unsigned height, size_t pitch) {
	int const s = gridScale;
	if (s <= 1) {
		videoRefresh(picture, width, height, pitch);
		return;
	}
	// A frame RetroArch runs again and doesn't show isn't drawn.
	if (!picture || rerunning()) {
		videoRefresh(NULL, width * s, height * s, pitch * s);
		return;
	}
	size_t const bytes = pitch / width;
	unsigned const outWidth = width * s, outHeight = height * s;
	size_t const outPitch = (size_t) outWidth * bytes;
	uint32_t gap;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	bool const shadow = gridWay && bytes == 4 && (lcd == LIBRETRO_LCD_DMG || lcd == LIBRETRO_LCD_POCKET);
	bool const room = gridReserve(&gridOut, outPitch * outHeight) && gridReserve(&gridBlends, (size_t) GRID_JOBS * GRID_SHARES_MAX * width * bytes) &&
	                  gridReserve(&gridTemplates, (size_t) GRID_JOBS * 8 * outPitch) &&
	                  (!shadow || (gridReserve(&gridDark, (size_t) (width + 16) * (height + 4)) &&
	                               gridReserve(&gridKeeps, (size_t) GRID_JOBS * width)));
	// Without the memory, the last picture stays.
	if (!room) {
		videoRefresh(NULL, outWidth, outHeight, outPitch);
		return;
	}
	gridLay(s, lcd);
	startJobs();
	gridFrame = (struct GridFrame) { .picture = picture, .width = width, .height = height, .pitch = pitch, .bytes = bytes, .s = s, .gap = gap, .shadow = shadow };
	if (shadow) {
		// Darkness counts from a sixteenth below the unlit screen; it is a fifth (51 of 255) at black.
		int const lit = 2 * (int) (gap >> 16 & 0xFF) + 5 * (int) (gap >> 8 & 0xFF) + (int) (gap & 0xFF);
		gridFrame.start = lit - lit / 16;
		gridFrame.scale = gridFrame.start ? (LIBRETRO_LCD_SHADOW << 16) / gridFrame.start : 0;
		int const way = gridWay - 1;
		float const reach = libretroLcdReach(way);
		float const x = -libretroLcdWays[way][0] * reach, y = -libretroLcdWays[way][1] * reach;
		gridFrame.fromX = (int) floorf(x);
		gridFrame.fromY = (int) floorf(y);
		gridFrame.weightX = (int) lroundf((x - gridFrame.fromX) * 256);
		gridFrame.weightY = (int) lroundf((y - gridFrame.fromY) * 256);
		// Beside and above and below the picture, no darkness.
		size_t const stride = width + 16;
		memset(gridDark.data, 0, 2 * stride);
		memset(gridDark.data + (height + 2) * stride, 0, 2 * stride);
		for (unsigned row = 0; row < height; ++row) {
			memset(gridDark.data + (row + 2) * stride, 0, 8);
			memset(gridDark.data + (row + 2) * stride + 8 + width, 0, 8);
		}
		runJobs(gridDarkJob);
	}
	runJobs(gridDrawJob);
	videoRefresh(gridOut.data, outWidth, outHeight, outPitch);
}

// Each channel's next level, from its level now and the channel's value in the new picture: tables for
// 5- and 6-bit channels (RGB565) and 8-bit ones (XRGB8888), for strong and weak drive.
static uint8_t next5[2][256 * 32], next6[2][256 * 64], next8[2][256 * 256];
static int builtFor = -1;

static void buildNext(int kind, int level, int bits, uint8_t* table, float drive) {
	// Each level in linear light, and where in linear light each level's rounding gives way to the next.
	static float linear[256], bounds[255];
	if (!linear[255]) {
		for (int v = 0; v < 256; ++v) {
			linear[v] = (float) pow(v / 255.0, 2.2);
			if (v < 255) {
				bounds[v] = (float) pow((v + 0.5) / 255, 2.2);
			}
		}
	}
	float const dark = (float) (1 - exp(-16.743 * 2 / (level + 1) / screenTimes[kind][0])) * drive;
	float const light = (float) (1 - exp(-16.743 * 2 / (level + 1) / screenTimes[kind][1])) * drive;
	int const codes = 1 << bits;
	for (int now = 0; now < 256; ++now) {
		for (int code = 0; code < codes; ++code) {
			int const full = bits == 8 ? code : bits == 6 ? code << 2 | code >> 4 : code << 3 | code >> 2;
			float const diff = linear[full] - linear[now];
			float const moved = linear[now] + diff * (diff < 0 ? dark : light);
			int next = 0;
			for (int step = 128; step; step >>= 1) {
				next += next + step <= 255 && bounds[next + step - 1] < moved ? step : 0;
			}
			if (next == now && now != full) {
				next += full > now ? 1 : -1;
			}
			table[now * codes + code] = (uint8_t) next;
		}
	}
}

// The screen between frames: each subpixel's level, whether each pixel is still moving and how many
// of each row's are, the last picture in and the picture shown; and this frame's picture in, its
// pixels' size and whether its rows alternate.
static struct {
	uint8_t* levels;
	uint8_t* busy;
	uint16_t* rowBusy;
	uint8_t* last;
	uint8_t* shown;
	unsigned width, height;
	size_t pitch;
	unsigned frames;
	const uint8_t* picture;
	size_t bytes;
	bool alternate;
} ghost;

// A thread's rows of the screen, a frame on. A row as it was, settled, is copied.
static void ghostJob(int job) {
	unsigned from, to;
	rowsOf(job, ghost.height, &from, &to);
	size_t const pitch = ghost.pitch, rowPixels = pitch / ghost.bytes;
	for (unsigned y = from; y < to; ++y) {
		const uint8_t* const in = ghost.picture + y * pitch;
		uint8_t* const before = ghost.last + y * pitch;
		uint8_t* const out = ghost.shown + y * pitch;
		if (!ghost.rowBusy[y] && !memcmp(in, before, pitch)) {
			memcpy(out, in, pitch);
			continue;
		}
		int const weak = ghost.alternate && (y + ghost.frames) % 2;
		uint8_t* level = ghost.levels + y * rowPixels * 3;
		uint8_t* moving = ghost.busy + y * rowPixels;
		unsigned count = 0;
		if (ghost.bytes == 4) {
			const uint8_t* const t = next8[weak];
			for (size_t x = 0; x < rowPixels; ++x, level += 3) {
				uint32_t const input = ((const uint32_t*) in)[x];
				if (input == ((const uint32_t*) before)[x] && !moving[x]) {
					((uint32_t*) out)[x] = input;
					continue;
				}
				uint32_t const r = t[level[0] << 8 | (input >> 16 & 0xFF)];
				uint32_t const g = t[level[1] << 8 | (input >> 8 & 0xFF)];
				uint32_t const b = t[level[2] << 8 | (input & 0xFF)];
				level[0] = (uint8_t) r;
				level[1] = (uint8_t) g;
				level[2] = (uint8_t) b;
				uint32_t const output = (input & 0xFF000000) | r << 16 | g << 8 | b;
				bool const still = output != input;
				moving[x] = still;
				count += still;
				((uint32_t*) out)[x] = output;
			}
		} else {
			const uint8_t* const t5 = next5[weak];
			const uint8_t* const t6 = next6[weak];
			for (size_t x = 0; x < rowPixels; ++x, level += 3) {
				uint32_t const input = ((const uint16_t*) in)[x];
				if (input == ((const uint16_t*) before)[x] && !moving[x]) {
					((uint16_t*) out)[x] = (uint16_t) input;
					continue;
				}
				uint32_t const r = t5[level[0] << 5 | input >> 11];
				uint32_t const g = t6[level[1] << 6 | (input >> 5 & 0x3F)];
				uint32_t const b = t5[level[2] << 5 | (input & 0x1F)];
				level[0] = (uint8_t) r;
				level[1] = (uint8_t) g;
				level[2] = (uint8_t) b;
				// Settled when each level is its channel's value widened to 8 bits.
				uint32_t const output = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
				bool const still = output != input || (r & 7) != r >> 5 || (g & 3) != g >> 6 || (b & 7) != b >> 5;
				moving[x] = still;
				count += still;
				((uint16_t*) out)[x] = (uint16_t) output;
			}
		}
		memcpy(before, in, pitch);
		ghost.rowBusy[y] = (uint16_t) count;
	}
}

void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch) {
	if (!ghosting || !picture || rerunning()) {
		// Turned off, the screen starts again from the picture when it's turned on.
		ghost.width = ghosting ? ghost.width : 0;
		sendPicture(picture, width, height, pitch);
		return;
	}
	size_t const bytes = pitch / width;
	size_t const pixels = pitch / bytes * height;
	int const kind = screenKind();
	int const wanted = (kind * 2 + ghosting - 1) * 2 + (bytes == 4);
	if (builtFor != wanted) {
		builtFor = wanted;
		for (int d = 0; d < (kind == SCREEN_TFT ? 2 : 1); ++d) {
			float const drive = d ? 0.5f : 1;
			if (bytes == 4) {
				buildNext(kind, ghosting - 1, 8, next8[d], drive);
			} else {
				buildNext(kind, ghosting - 1, 5, next5[d], drive);
				buildNext(kind, ghosting - 1, 6, next6[d], drive);
			}
		}
	}
	++ghost.frames;
	if (!ghost.levels || width != ghost.width || height != ghost.height || pitch != ghost.pitch) {
		uint8_t* const newLevels = realloc(ghost.levels, pixels * 3);
		ghost.levels = newLevels ? newLevels : ghost.levels;
		uint8_t* const newBusy = realloc(ghost.busy, pixels);
		ghost.busy = newBusy ? newBusy : ghost.busy;
		uint16_t* const newRowBusy = realloc(ghost.rowBusy, height * sizeof(*ghost.rowBusy));
		ghost.rowBusy = newRowBusy ? newRowBusy : ghost.rowBusy;
		uint8_t* const newLast = realloc(ghost.last, pitch * height);
		ghost.last = newLast ? newLast : ghost.last;
		uint8_t* const newShown = realloc(ghost.shown, pitch * height);
		ghost.shown = newShown ? newShown : ghost.shown;
		ghost.width = 0;
		if (newLevels && newBusy && newRowBusy && newLast && newShown) {
			ghost.width = width;
			ghost.height = height;
			ghost.pitch = pitch;
			memcpy(ghost.last, picture, pitch * height);
			memset(ghost.busy, 0, pixels);
			memset(ghost.rowBusy, 0, height * sizeof(*ghost.rowBusy));
			for (size_t i = 0; i < pixels; ++i) {
				if (bytes == 4) {
					uint32_t const c = ((const uint32_t*) picture)[i];
					ghost.levels[i * 3] = c >> 16 & 0xFF;
					ghost.levels[i * 3 + 1] = c >> 8 & 0xFF;
					ghost.levels[i * 3 + 2] = c & 0xFF;
				} else {
					uint16_t const c = ((const uint16_t*) picture)[i];
					ghost.levels[i * 3] = (uint8_t) ((c >> 11) << 3 | (c >> 13));
					ghost.levels[i * 3 + 1] = (uint8_t) ((c >> 5 & 0x3F) << 2 | (c >> 9 & 3));
					ghost.levels[i * 3 + 2] = (uint8_t) ((c & 0x1F) << 3 | (c >> 2 & 7));
				}
			}
		}
		sendPicture(picture, width, height, pitch);
		return;
	}
	ghost.picture = picture;
	ghost.bytes = bytes;
	ghost.alternate = kind == SCREEN_TFT && interleaved && advancePanel();
	startJobs();
	runJobs(ghostJob);
	sendPicture(ghost.shown, width, height, pitch);
}
