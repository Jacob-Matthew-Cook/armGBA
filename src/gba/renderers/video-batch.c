/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/renderers/video-batch.h>

#include "gba/renderers/software-private.h"

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/renderers/common.h>

// One scanline as the pixel function reads it, in texels of four words:
// 0: segment ends, head, blend and rows, sprite count, mosaic and object window
// 1 + i: where background i reads this line
// 5, 6: the first window segment and the backgrounds it draws, front to back
// 7 to 22: the sprites, two to a word
// 21 + 2s, 22 + 2s: window segment s from the second on
enum {
	L_ENDS = 0,
	L_HEAD,
	L_MISC,
	L_OBJ,
	L_PARAMS = 4,
	L_PROGRAM = 20,
	L_SPRITES = 28,
	L_PROGRAM_MORE = 84,
};

#define HEAD_FORCE_TARGET_1 0x00040000
#define HEAD_TARGET_1_OBJ 0x00080000
#define HEAD_TARGET_1_BD 0x00100000
#define HEAD_TARGET_2_OBJ 0x00200000
#define HEAD_TARGET_2_BD 0x00400000
#define HEAD_BLANK 0x00800000
#define HEAD_TARGET_2_ANY 0x01000000
#define HEAD_TARGET_1_ANY 0x02000000

static unsigned _stats[8];

#include <time.h>
static double _timing[4];
static bool _timed;

static inline double _now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void GBAVideoBatchRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoBatchRendererReset(struct GBAVideoRenderer* renderer);
static void GBAVideoBatchRendererDeinit(struct GBAVideoRenderer* renderer);
static uint32_t GBAVideoBatchRendererId(const struct GBAVideoRenderer* renderer);
static bool GBAVideoBatchRendererLoadState(struct GBAVideoRenderer* renderer, const void* state, size_t size);
static void GBAVideoBatchRendererSaveState(struct GBAVideoRenderer* renderer, void** state, size_t* size);
static uint16_t GBAVideoBatchRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoBatchRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address);
static void GBAVideoBatchRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam);
static void GBAVideoBatchRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoBatchRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoBatchRendererFinishFrame(struct GBAVideoRenderer* renderer);
static void GBAVideoBatchRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels);
static void GBAVideoBatchRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels);

void GBAVideoBatchRendererCreate(struct GBAVideoBatchRenderer* renderer) {
	memset(renderer, 0, sizeof(*renderer));
	_timed = getenv("ARMGBA_BATCH_TIME");
	renderer->d.init = GBAVideoBatchRendererInit;
	renderer->d.reset = GBAVideoBatchRendererReset;
	renderer->d.deinit = GBAVideoBatchRendererDeinit;
	renderer->d.rendererId = GBAVideoBatchRendererId;
	renderer->d.loadState = GBAVideoBatchRendererLoadState;
	renderer->d.saveState = GBAVideoBatchRendererSaveState;
	renderer->d.writeVideoRegister = GBAVideoBatchRendererWriteVideoRegister;
	renderer->d.writeVRAM = GBAVideoBatchRendererWriteVRAM;
	renderer->d.writeOAM = GBAVideoBatchRendererWriteOAM;
	renderer->d.writePalette = GBAVideoBatchRendererWritePalette;
	renderer->d.drawScanline = GBAVideoBatchRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoBatchRendererFinishFrame;
	renderer->d.getPixels = GBAVideoBatchRendererGetPixels;
	renderer->d.putPixels = GBAVideoBatchRendererPutPixels;
	renderer->d.highlightColor = M_COLOR_WHITE;
	GBAVideoSoftwareRendererCreate(&renderer->sw);
}

// For VRAM replaced without stores, as by loading a state
void GBAVideoBatchRendererReloadVRAM(struct GBAVideoBatchRenderer* batch) {
	batch->vramDirty = (1U << BATCH_VRAM_PAGES) - 1;
}

static void _shareMemory(struct GBAVideoBatchRenderer* batch, struct GBAVideoSoftwareRenderer* sw) {
	sw->d.palette = batch->d.palette;
	sw->d.vram = batch->d.vram;
	sw->d.oam = batch->d.oam;
	sw->d.cache = NULL;
}

static void _startFrame(struct GBAVideoBatchRenderer* batch) {
	batch->nPalettes = 0;
	batch->nSprites = 0;
	batch->paletteDirty = true;
	batch->oamDirty = true;
	batch->drawnY = 0;
	batch->nextY = 0;
	if (batch->gl) {
		GBAVideoBatchGLStartFrame(batch);
	}
}

static void GBAVideoBatchRendererInit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	_shareMemory(batch, &batch->sw);
	batch->sw.outputBuffer = batch->outputBuffer;
	batch->sw.outputBufferStride = batch->outputBufferStride;
	batch->sw.d.init(&batch->sw.d);
	if (batch->check) {
		_shareMemory(batch, batch->check);
		batch->check->outputBuffer = batch->checkBuffer;
		batch->check->outputBufferStride = GBA_VIDEO_HORIZONTAL_PIXELS;
		batch->check->d.init(&batch->check->d);
	}
	GBAVideoBatchRendererReset(renderer);
}

static void GBAVideoBatchRendererReset(struct GBAVideoRenderer* renderer) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	_shareMemory(batch, &batch->sw);
	batch->sw.d.reset(&batch->sw.d);
	if (batch->check) {
		_shareMemory(batch, batch->check);
		batch->check->d.reset(&batch->check->d);
	}
	batch->vramDirty = (1U << BATCH_VRAM_PAGES) - 1;
	_startFrame(batch);
}

static void GBAVideoBatchRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	batch->sw.d.deinit(&batch->sw.d);
	if (batch->check) {
		batch->check->d.deinit(&batch->check->d);
	}
}

static uint32_t GBAVideoBatchRendererId(const struct GBAVideoRenderer* renderer) {
	UNUSED(renderer);
	return BATCH_MAGIC;
}

static bool GBAVideoBatchRendererLoadState(struct GBAVideoRenderer* renderer, const void* state, size_t size) {
	UNUSED(renderer);
	UNUSED(state);
	UNUSED(size);
	return false;
}

static void GBAVideoBatchRendererSaveState(struct GBAVideoRenderer* renderer, void** state, size_t* size) {
	UNUSED(renderer);
	*state = NULL;
	*size = 0;
}

static uint16_t GBAVideoBatchRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	if (batch->check) {
		batch->check->d.writeVideoRegister(&batch->check->d, address, value);
	}
	return batch->sw.d.writeVideoRegister(&batch->sw.d, address, value);
}

static void GBAVideoBatchRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	unsigned page = (address >> 12) % BATCH_VRAM_PAGES;
	batch->vramDirty |= 1U << page;
	if (batch->check) {
		batch->check->d.writeVRAM(&batch->check->d, address);
	}
}

static void GBAVideoBatchRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	batch->sw.oamDirty = true;
	batch->oamDirty = true;
	if (batch->check) {
		batch->check->d.writeOAM(&batch->check->d, oam);
	}
}

static void GBAVideoBatchRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	batch->paletteDirty = true;
	if (batch->check) {
		batch->check->d.writePalette(&batch->check->d, address, value);
	}
}

// The pixel function the shader runs, compiled as C
typedef unsigned int uint;
typedef struct {
	uint x, y, z, w;
} uvec4;

static const struct GBAVideoBatchRenderer* _frame;

static inline uvec4 _fetch4(const uint32_t* words) {
	uvec4 v = { words[0], words[1], words[2], words[3] };
	return v;
}

#define BATCH_SHARED(...) __VA_ARGS__
#define BATCH_FN static inline
#define FEATURE_OBJWIN true
#define FEATURE_MOSAIC true
#define FEATURE_AFFINE_SPRITES true
#define FEATURE_AFFINE_BG true
#define FEATURE_BRIGHTNESS true
#define FEATURE_BLEND true
#define FEATURE_WINDOWS true
#define U(X) ((uint) (X))
#define I(X) ((int) (X))
#define iabs(X) abs(X)
#define FETCH_LINE(Y, T) _fetch4(&_frame->lines[Y][(T) * 4])
#define FETCH_SPRITE(R, T) _fetch4(&_frame->sprites[R][(T) >> 1][((T) & 1) * 4])
#define FETCH_PAL(R, E) ((uint) _frame->palettes[R][E])
#define FETCH_VRAM16(H) ((uint) _frame->vram[(H) < 0xC000u ? (H) : 0xBFFFu])
#include "gba/renderers/video-batch-pixel.h"
#undef BATCH_SHARED
#undef BATCH_FN
#undef FEATURE_OBJWIN
#undef FEATURE_MOSAIC
#undef FEATURE_AFFINE_SPRITES
#undef FEATURE_AFFINE_BG
#undef FEATURE_BRIGHTNESS
#undef FEATURE_BLEND
#undef FEATURE_WINDOWS
#undef U
#undef I
#undef iabs
#undef FETCH_LINE
#undef FETCH_SPRITE
#undef FETCH_PAL
#undef FETCH_VRAM16

static void _draw(struct GBAVideoBatchRenderer* batch, int endY) {
	if (endY > batch->drawnY) {
		++_stats[0];
	}
	if (batch->gl) {
		if (endY > batch->drawnY) {
			double t = _timed ? _now() : 0;
			GBAVideoBatchGLDraw(batch, batch->drawnY, endY);
			if (_timed) {
				_timing[1] += _now() - t;
			}
		}
		batch->drawnY = endY;
		return;
	}
	_frame = batch;
	int y;
	for (y = batch->drawnY; y < endY; ++y) {
		mColor* out = &batch->outputBuffer[batch->outputBufferStride * y];
		int x;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			out[x] = batchPixel(x, y);
		}
	}
	batch->drawnY = endY;
}

static void _uploadVRAM(struct GBAVideoBatchRenderer* batch, uint32_t pages) {
	batch->vramDirty &= ~pages;
	double t = _timed ? _now() : 0;
	if (batch->gl) {
		GBAVideoBatchGLUploadVRAM(batch, pages);
	}
	while (pages) {
		unsigned page = __builtin_ctz(pages);
		pages &= pages - 1;
		++_stats[1];
		memcpy(&batch->vram[page << 11], &batch->d.vram[page << 11], 4096);
	}
	if (_timed) {
		_timing[2] += _now() - t;
	}
}

static inline uint16_t* _spriteEntries(const struct GBAVideoBatchRenderer* batch, int y) {
	return (uint16_t*) &batch->lines[y][L_SPRITES];
}

static uint32_t _pages(uint32_t start, uint32_t size) {
	uint32_t end = (start + size - 1) >> 12;
	if (end >= BATCH_VRAM_PAGES) {
		end = BATCH_VRAM_PAGES - 1;
	}
	return (2U << end) - (1U << (start >> 12));
}

// The rows of a bitmap background this line reads
static uint32_t _rowPages(const uint32_t* params, uint32_t base, int rowBytes, int rows) {
	int32_t first = params[1];
	int32_t last = first + (GBA_VIDEO_HORIZONTAL_PIXELS - 1) * ((int32_t) params[2] >> 16);
	if (first > last) {
		int32_t swap = first;
		first = last;
		last = swap;
	}
	first >>= 8;
	last >>= 8;
	if (first < 0) {
		first = 0;
	}
	if (last >= rows) {
		last = rows - 1;
	}
	if (first > last) {
		return 0;
	}
	return _pages(base + first * rowBytes, (last - first + 1) * rowBytes);
}

// The pages a text background line reads: its map row and the span of tiles that row names
static uint32_t _textPages(const struct GBAVideoBatchRenderer* batch, const uint32_t* params) {
	uint32_t pages = 0;
	unsigned minTile = 0x3FF;
	unsigned maxTile = 0;
	int inX = params[1] & 0x1F8;
	int tile;
	for (tile = 0; tile < 31; ++tile, inX += 8) {
		unsigned xBase = inX & 0xF8;
		if (params[1] & 0x200) {
			xBase += (inX & 0x100) << 5;
		}
		unsigned address = params[0] + (xBase >> 3);
		pages |= 1U << (address >> 11);
		unsigned index = GBA_TEXT_MAP_TILE(batch->d.vram[address]);
		if (index < minTile) {
			minTile = index;
		}
		if (index > maxTile) {
			maxTile = index;
		}
	}
	unsigned shift = params[1] & 0x400 ? 6 : 5;
	uint32_t start = params[2] + (minTile << shift);
	uint32_t end = params[2] + ((maxTile + 1) << shift);
	if (end > 0x10000) {
		end = 0x10000;
	}
	if (start < end) {
		pages |= _pages(start, end - start);
	}
	return pages;
}

// The pages a sprite reads
static uint32_t _spritePages(const struct GBAVideoBatchRenderer* batch, const uint32_t* sprite) {
	GBAObjAttributesA a = sprite[0];
	GBAObjAttributesC c = sprite[1];
	int width = sprite[5] & 0xFF;
	int height = (sprite[5] >> 8) & 0xFF;
	unsigned rowBytes = (width / 8) * (GBAObjAttributesAIs256Color(a) ? 64 : 32);
	unsigned rows = height / 8;
	unsigned bytes;
	if (GBARegisterDISPCNTIsObjCharacterMapping(batch->sw.dispcnt)) {
		bytes = rowBytes * rows;
	} else {
		bytes = (rows - 1) * 0x400 + rowBytes;
	}
	uint32_t start = GBAObjAttributesCGetTile(c) * 32;
	if (!bytes || start + bytes > 0x8000) {
		return _pages(BASE_TILE, 0x8000);
	}
	return _pages(BASE_TILE + start, bytes);
}

// The 4 KiB pages of VRAM a recorded line can read, roughly or exactly
static uint32_t _readPages(const struct GBAVideoBatchRenderer* batch, int y, bool exact) {
	const struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	const uint32_t* line = batch->lines[y];
	if (line[L_HEAD] & HEAD_BLANK) {
		return 0;
	}
	int mode = GBARegisterDISPCNTGetMode(sw->dispcnt);
	uint32_t pages = 0;
	int nSprites = line[L_OBJ] & 0xFF;
	if (nSprites) {
		if (!exact) {
			pages |= mode >= 3 ? _pages(0x14000, 0x4000) : _pages(BASE_TILE, 0x8000);
		} else {
			const uint32_t (*sprites)[BATCH_SPRITE_WORDS] = batch->sprites[line[L_MISC] >> 24];
			int i;
			for (i = 0; i < nSprites; ++i) {
				pages |= _spritePages(batch, sprites[_spriteEntries(batch, y)[i] & 0xFF]);
			}
		}
	}
	int i;
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &sw->bg[i];
		const uint32_t* params = &line[L_PARAMS + i * 4];
		if (bg->enabled != ENABLED_MAX) {
			continue;
		}
		if (mode >= 3) {
			if (i != 2) {
				continue;
			}
			uint32_t frame = mode != 3 && GBARegisterDISPCNTIsFrameSelect(sw->dispcnt) ? 0xA000 : 0;
			if (!exact) {
				pages |= _pages(frame, mode == 3 ? 0x12C00 : mode == 4 ? 0x9600 : 0xA000);
			} else if (mode == 3) {
				pages |= _rowPages(params, 0, 480, GBA_VIDEO_VERTICAL_PIXELS);
			} else if (mode == 4) {
				pages |= _rowPages(params, frame, 240, GBA_VIDEO_VERTICAL_PIXELS);
			} else {
				pages |= _rowPages(params, frame, 320, 128);
			}
		} else if (i < 2 ? mode != 2 : mode == 0) {
			if (exact) {
				pages |= _textPages(batch, params);
				continue;
			}
			static const uint32_t screenSize[4] = { 0x800, 0x1000, 0x1000, 0x2000 };
			pages |= _pages(bg->screenBase, screenSize[bg->size]);
			uint32_t charSize = bg->multipalette ? 0x10000 : 0x8000;
			if (bg->charBase + charSize > 0x10000) {
				charSize = 0x10000 - bg->charBase;
			}
			pages |= _pages(bg->charBase, charSize);
		} else if (i >= 2) {
			pages |= _pages(bg->screenBase, 0x100 << (2 * bg->size));
			pages |= _pages(bg->charBase, 0x4000);
		}
	}
	return pages;
}

// Which sprites cover each line, and what each adds to a line's list, worked out when OAM changes
static void _spriteCoverage(struct GBAVideoBatchRenderer* batch) {
	struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	memset(batch->spriteCoverage, 0, sizeof(batch->spriteCoverage));
	int i;
	for (i = 0; i < sw->oamMax; ++i) {
		struct GBAVideoRendererSprite* sprite = &sw->sprites[i];
		struct GBAVideoBatchSprite* info = &batch->spriteInfo[i];
		GBAObjAttributesA a = sprite->obj.a;
		int width = GBAVideoObjSizes[GBAObjAttributesAGetShape(a) * 4 + GBAObjAttributesBGetSize(sprite->obj.b)][0];
		int32_t x = (uint32_t) GBAObjAttributesBGetX(sprite->obj.b) << 23;
		x >>= 23;
		int end = x + (width << (GBAObjAttributesAIsTransformed(a) && GBAObjAttributesAIsDoubleSize(a)));
		if (GBAObjAttributesAIsMosaic(a)) {
			end += 15;
		}
		if (x < 0) {
			x = 0;
		}
		if (end > GBA_VIDEO_HORIZONTAL_PIXELS) {
			end = GBA_VIDEO_HORIZONTAL_PIXELS;
		}
		info->entry = x < end ? i | ((((2U << ((end - 1) >> 5)) - (1U << (x >> 5))) & 0xFF) << 8) : 0;
		info->cycles = sprite->cycles;
		info->index = sprite->index;
		info->features = 0;
		if (GBAObjAttributesAIsTransformed(a)) {
			info->features |= BATCH_FEATURE_AFFINE_SPRITES;
		}
		if (GBAObjAttributesAGetMode(a) == OBJ_MODE_SEMITRANSPARENT) {
			info->features |= BATCH_FEATURE_BLEND;
		}
		int y = sprite->y < 0 ? 0 : sprite->y;
		int endY = sprite->endY > GBA_VIDEO_VERTICAL_PIXELS ? GBA_VIDEO_VERTICAL_PIXELS : sprite->endY;
		for (; y < endY; ++y) {
			batch->spriteCoverage[y][i >> 6] |= 1ULL << (i & 63);
		}
	}
}

// Whether a sprite drawn on this line makes the line blend again afterwards
static bool _forcesTarget1(const struct GBAVideoSoftwareRenderer* sw, const struct GBAVideoRendererSprite* sprite) {
	bool objwinEnable = GBARegisterDISPCNTIsObjwinEnable(sw->dispcnt);
	GBAObjAttributesA a = sprite->obj.a;
	if (GBARegisterDISPCNTGetMode(sw->dispcnt) >= 3 && GBAObjAttributesCGetTile(sprite->obj.c) < 512) {
		return false;
	}
	int w;
	for (w = 0; w < sw->nWindows; ++w) {
		struct WindowControl control = sw->windows[w].control;
		if (!GBAWindowControlIsObjEnable(control.packed) && !objwinEnable) {
			continue;
		}
		if (GBAObjAttributesAGetMode(a) == OBJ_MODE_OBJWIN && control.priority < sw->objwin.priority) {
			continue;
		}
		bool objwinSlowPath = objwinEnable && GBAWindowControlIsBlendEnable(sw->objwin.packed) != GBAWindowControlIsBlendEnable(control.packed);
		if (GBAObjAttributesAGetMode(a) == OBJ_MODE_SEMITRANSPARENT || (sw->target1Obj && sw->blendEffect == BLEND_ALPHA) || objwinSlowPath) {
			return true;
		}
	}
	return false;
}

// The sprites the software renderer would draw on this line, with the 32-pixel columns each can reach.
// Its cycle budget only drops at drawn sprites and by twice the OAM index reached, so only the sprites on the line matter.
static int _spriteList(struct GBAVideoBatchRenderer* batch, int y, bool target2, bool* forceTarget1, unsigned* features) {
	struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	uint16_t* list = _spriteEntries(batch, y);
	int budget = GBARegisterDISPCNTIsHblankIntervalFree(sw->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
	int n = 0;
	int half;
	for (half = 0; half < 2; ++half) {
		uint64_t covering = batch->spriteCoverage[y][half];
		while (covering) {
			int i = half * 64 + __builtin_ctzll(covering);
			covering &= covering - 1;
			const struct GBAVideoBatchSprite* info = &batch->spriteInfo[i];
			if (budget - 2 * info->index <= 0) {
				return n;
			}
			budget -= info->cycles;
			if (info->entry) {
				list[n] = info->entry;
				++n;
			}
			*features |= info->features;
			if (target2 && !*forceTarget1) {
				*forceTarget1 = _forcesTarget1(sw, &sw->sprites[i]);
			}
		}
	}
	return n;
}

// The backgrounds a window segment draws, front to back, with how each blends there
static int _program(const struct GBAVideoSoftwareRenderer* sw, struct WindowControl control, uint32_t* slots) {
	int mode = GBARegisterDISPCNTGetMode(sw->dispcnt);
	bool objwinEnable = GBARegisterDISPCNTIsObjwinEnable(sw->dispcnt);
	bool winBlend = GBAWindowControlIsBlendEnable(control.packed);
	bool objwinBlend = GBAWindowControlIsBlendEnable(sw->objwin.packed);
	bool brightness = sw->blendEffect == BLEND_BRIGHTEN || sw->blendEffect == BLEND_DARKEN;
	int n = 0;
	unsigned priority;
	for (priority = 0; priority < 4; ++priority) {
		int i;
		for (i = 0; i < 4; ++i) {
			const struct GBAVideoSoftwareBackground* bg = &sw->bg[i];
			if (bg->enabled != ENABLED_MAX || bg->priority != priority) {
				continue;
			}
			bool windowed = control.packed & (1 << i);
			bool objwinBg = sw->objwin.packed & (1 << i);
			if (!windowed && !(objwinEnable && objwinBg)) {
				continue;
			}
			int bgMode;
			if (i < 2) {
				if (mode >= 2) {
					continue;
				}
				bgMode = 0;
			} else if (i == 2) {
				if (mode > 5) {
					continue;
				}
				bgMode = mode == 1 ? 2 : mode;
			} else {
				if (mode != 0 && mode != 2) {
					continue;
				}
				bgMode = mode;
			}
			bool target1 = false;
			bool target2 = bg->target2;
			bool objwinTarget1 = false;
			bool objwinTarget2 = bg->target2;
			if (sw->blendEffect == BLEND_ALPHA) {
				if (sw->blda == 0x10 && sw->bldb == 0) {
					target2 = false;
					objwinTarget2 = false;
				} else if (bg->target1) {
					target1 = winBlend;
					objwinTarget1 = objwinBlend;
				}
			}
			slots[n] = i | (priority << 2) | (bgMode << 4) | (target1 << 7) | (target2 << 8) | (objwinTarget1 << 9) |
			           (objwinTarget2 << 10) | ((bg->target1 && winBlend && brightness) << 11) |
			           ((objwinEnable && bg->target1 && objwinBlend && brightness) << 12) | ((objwinBg && windowed) << 13) |
			           (!objwinBg << 14);
			++n;
		}
	}
	return n;
}

static void _recordLine(struct GBAVideoBatchRenderer* batch, int y) {
	struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	uint32_t* line = batch->lines[y];
	if (batch->paletteDirty) {
		memcpy(batch->palettes[batch->nPalettes], batch->d.palette, sizeof(batch->palettes[0]));
		++batch->nPalettes;
		batch->paletteDirty = false;
	}
	bool target2 = sw->target2Bd;
	bool anyTarget1 = false;
	int i;
	for (i = 0; i < 4; ++i) {
		target2 |= sw->bg[i].target2 && sw->bg[i].enabled;
		anyTarget1 |= sw->bg[i].target1;
	}
	int nSprites = 0;
	bool forceTarget1 = false;
	unsigned features = 0;
	if (GBARegisterDISPCNTIsObjEnable(sw->dispcnt)) {
		if (sw->oamDirty) {
			sw->oamMax = GBAVideoRendererCleanOAM(sw->d.oam->obj, sw->sprites, sw->objOffsetY);
			sw->oamDirty = false;
			batch->oamDirty = true;
		}
		if (batch->oamDirty) {
			uint32_t (*sprites)[BATCH_SPRITE_WORDS] = batch->sprites[batch->nSprites];
			for (i = 0; i < sw->oamMax; ++i) {
				struct GBAVideoRendererSprite* sprite = &sw->sprites[i];
				const struct GBAOAMMatrix* mat = &sw->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->obj.b)];
				const int* size = GBAVideoObjSizes[GBAObjAttributesAGetShape(sprite->obj.a) * 4 + GBAObjAttributesBGetSize(sprite->obj.b)];
				sprites[i][0] = sprite->obj.a | (sprite->obj.b << 16);
				sprites[i][1] = sprite->obj.c;
				sprites[i][2] = (uint16_t) sprite->y | ((uint32_t) (uint16_t) sprite->endY << 16);
				sprites[i][3] = (uint16_t) mat->a | ((uint32_t) (uint16_t) mat->b << 16);
				sprites[i][4] = (uint16_t) mat->c | ((uint32_t) (uint16_t) mat->d << 16);
				sprites[i][5] = size[0] | (size[1] << 8);
			}
			++batch->nSprites;
			batch->oamDirty = false;
			_spriteCoverage(batch);
		}
		nSprites = _spriteList(batch, y, target2, &forceTarget1, &features);
	}

	uint32_t ends = 0xFFFFFFFF;
	int w;
	for (w = 0; w < sw->nWindows - 1; ++w) {
		ends &= ~(0xFF << (8 * w));
		ends |= sw->windows[w].endX << (8 * w);
	}
	line[L_ENDS] = ends;
	line[L_HEAD] = sw->dispcnt | (sw->blendEffect << 16) | (forceTarget1 ? HEAD_FORCE_TARGET_1 : 0) |
	               (sw->target1Obj ? HEAD_TARGET_1_OBJ : 0) | (sw->target1Bd ? HEAD_TARGET_1_BD : 0) |
	               (sw->target2Obj ? HEAD_TARGET_2_OBJ : 0) | (sw->target2Bd ? HEAD_TARGET_2_BD : 0) |
	               (target2 ? HEAD_TARGET_2_ANY : 0) | (anyTarget1 ? HEAD_TARGET_1_ANY : 0);
	line[L_MISC] = sw->blda | (sw->bldb << 5) | (sw->bldy << 10) | ((batch->nPalettes - 1) << 16) | ((uint32_t) (batch->nSprites ? batch->nSprites - 1 : 0) << 24);
	line[L_OBJ] = nSprites | ((uint32_t) sw->mosaic << 8) | ((uint32_t) sw->objwin.packed << 24);

	if (GBARegisterDISPCNTIsObjwinEnable(sw->dispcnt)) {
		features |= BATCH_FEATURE_OBJWIN;
	}
	if (sw->mosaic) {
		features |= BATCH_FEATURE_MOSAIC;
	}
	if (GBARegisterDISPCNTGetMode(sw->dispcnt) != 0) {
		features |= BATCH_FEATURE_AFFINE_BG;
	}
	if (sw->blendEffect == BLEND_BRIGHTEN || sw->blendEffect == BLEND_DARKEN) {
		features |= BATCH_FEATURE_BRIGHTNESS;
	}
	if (sw->blendEffect == BLEND_ALPHA || forceTarget1) {
		features |= BATCH_FEATURE_BLEND;
	}
	if (sw->nWindows > 1) {
		features |= BATCH_FEATURE_WINDOWS;
	}
	batch->features[y] = features;
	// The programs only change with the registers and enables they come from
	uint32_t key[2] = { sw->dispcnt | (sw->blendEffect << 16) | ((sw->blda == 0x10 && sw->bldb == 0) << 18) | (sw->objwin.packed << 24), 0 };
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &sw->bg[i];
		key[1] |= ((bg->enabled == ENABLED_MAX) | (bg->priority << 1) | (bg->target1 << 3) | (bg->target2 << 4)) << (i * 8);
	}
	if (key[0] != batch->programKey[0] || key[1] != batch->programKey[1]) {
		batch->programKey[0] = key[0];
		batch->programKey[1] = key[1];
		batch->nPrograms = 0;
	}
	for (w = 0; w < sw->nWindows; ++w) {
		uint32_t* program = &line[w ? L_PROGRAM_MORE + w * 8 : L_PROGRAM];
		int cached;
		for (cached = 0; cached < batch->nPrograms; ++cached) {
			if ((batch->programs[cached][0] & 0xFFFF) == (uint32_t) (sw->windows[w].control.packed | ((uint8_t) sw->windows[w].control.priority << 8))) {
				break;
			}
		}
		if (cached < batch->nPrograms) {
			memcpy(program, batch->programs[cached], sizeof(batch->programs[cached]));
			continue;
		}
		int slots = _program(sw, sw->windows[w].control, &program[1]);
		program[0] = sw->windows[w].control.packed | ((uint8_t) sw->windows[w].control.priority << 8) | ((uint8_t) sw->objwin.priority << 16) | (slots << 24);
		if (batch->nPrograms < MAX_WINDOW) {
			memcpy(batch->programs[batch->nPrograms], program, sizeof(batch->programs[0]));
			++batch->nPrograms;
		}
	}

	int mode = GBARegisterDISPCNTGetMode(sw->dispcnt);
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &sw->bg[i];
		uint32_t* params = &line[L_PARAMS + i * 4];
		if (bg->enabled != ENABLED_MAX) {
			continue;
		}
		int mosaicH = bg->mosaic ? GBAMosaicControlGetBgH(sw->mosaic) : 0;
		if (i < 2 ? mode < 2 : mode == 0) {
			int inY = y;
			if (bg->mosaic) {
				inY -= inY % (GBAMosaicControlGetBgV(sw->mosaic) + 1);
			}
			inY += bg->y;
			unsigned yBase = inY & 0xF8;
			if (bg->size == 2) {
				yBase += inY & 0x100;
			} else if (bg->size == 3) {
				yBase += (inY & 0x100) << 1;
			}
			params[0] = (bg->screenBase >> 1) + (yBase << 2);
			params[1] = bg->x | ((bg->size & 1) << 9) | (bg->multipalette ? 0x400 : 0) | (mosaicH << 12) | ((inY & 7) << 16);
			params[2] = bg->charBase;
		} else {
			int32_t sx = bg->sx;
			int32_t sy = bg->sy;
			if (bg->mosaic) {
				sx -= (y % (GBAMosaicControlGetBgV(sw->mosaic) + 1)) * bg->dmx;
				sy -= (y % (GBAMosaicControlGetBgV(sw->mosaic) + 1)) * bg->dmy;
			}
			params[0] = sx;
			params[1] = sy;
			params[2] = (uint16_t) bg->dx | ((uint32_t) (uint16_t) bg->dy << 16);
			params[3] = (bg->screenBase >> 11) | ((bg->charBase >> 14) << 5) | (bg->size << 7) | (bg->overflow << 9) | (mosaicH << 12) |
			            (GBARegisterDISPCNTIsFrameSelect(sw->dispcnt) ? 0x10000 : 0);
		}
	}
}


static void _drawScanline(struct GBAVideoRenderer* renderer, int y);

static void GBAVideoBatchRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	if (!_timed) {
		_drawScanline(renderer, y);
		return;
	}
	double t = _now();
	double inner = _timing[1] + _timing[2];
	_drawScanline(renderer, y);
	_timing[0] += _now() - t - (_timing[1] + _timing[2] - inner);
}

static void _drawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	if (batch->check) {
		batch->check->d.drawScanline(&batch->check->d, y);
	}

	unsigned i;
	sw->nextY = y == GBA_VIDEO_VERTICAL_PIXELS - 1 ? 0 : y + 1;
	batch->nextY = y + 1;
	GBAVideoSoftwareRendererStepWindow(sw, y);
	if (GBARegisterDISPCNTIsForcedBlank(sw->dispcnt)) {
		batch->lines[y][L_HEAD] = HEAD_BLANK;
		batch->lines[y][L_OBJ] = 0;
		batch->features[y] = 0;
		return;
	}
	GBAVideoSoftwareRendererPrepareScanline(sw);
	_recordLine(batch, y);

	if (getenv("ARMGBA_BATCH_TRACE") && batch->frame == (unsigned) atoi(getenv("ARMGBA_BATCH_TRACE")) && (y == 0 || y == 100)) {
		const uint32_t* l = batch->lines[y];
		fprintf(stderr, "line %d:", y);
		int k;
		for (k = 0; k < BATCH_LINE_WORDS; ++k) {
			fprintf(stderr, " %08X", l[k]);
		}
		fprintf(stderr, "\n  dispcnt %04X effect %d bg2 enabled %d prio %d sx %08X sy %08X dx %d dy %d nWindows %d win0 %02X ctl %02X\n", sw->dispcnt, sw->blendEffect, sw->bg[2].enabled, sw->bg[2].priority, sw->bg[2].sx, sw->bg[2].sy, sw->bg[2].dx, sw->bg[2].dy, sw->nWindows, sw->windows[0].endX, sw->windows[0].control.packed);
	}
	// Lines waiting to be drawn saw VRAM as it was; this one may need newer pages
	uint32_t stale = batch->vramDirty & _readPages(batch, y, false);
	if (stale && !(batch->vramDirty & _readPages(batch, y, true))) {
		stale = 0;
	}
	if (stale) {
		_draw(batch, y);
		_uploadVRAM(batch, stale);
	}

	if (GBARegisterDISPCNTGetMode(sw->dispcnt) != 0) {
		for (i = 2; i < 4; ++i) {
			if (sw->bg[i].enabled == ENABLED_MAX) {
				sw->bg[i].sx += sw->bg[i].dmx;
				sw->bg[i].sy += sw->bg[i].dmy;
			}
		}
	}
	for (i = 0; i < 4; ++i) {
		if (sw->bg[i].enabled != 0 && sw->bg[i].enabled < ENABLED_MAX) {
			++sw->bg[i].enabled;
		}
	}
}

static void _compare(struct GBAVideoBatchRenderer* batch) {
	unsigned differ = 0;
	unsigned near = 0;
	int firstX = -1;
	int firstY = -1;
	int y;
	for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
		const mColor* ours = &batch->outputBuffer[batch->outputBufferStride * y];
		const mColor* theirs = &batch->checkBuffer[GBA_VIDEO_HORIZONTAL_PIXELS * y];
		int x;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			if (ours[x] == theirs[x]) {
				continue;
			}
			++differ;
			// Close enough: one step per channel, or the right color one pixel away
			int dr = (int) (ours[x] >> 11) - (int) (theirs[x] >> 11);
			int dg = (int) ((ours[x] >> 6) & 0x1F) - (int) ((theirs[x] >> 6) & 0x1F);
			int db = (int) (ours[x] & 0x1F) - (int) (theirs[x] & 0x1F);
			bool ok = dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1 && db >= -1 && db <= 1;
			int ny;
			for (ny = y - 1; !ok && ny <= y + 1; ++ny) {
				int nx;
				for (nx = x - 1; !ok && nx <= x + 1; ++nx) {
					if (nx >= 0 && ny >= 0 && nx < GBA_VIDEO_HORIZONTAL_PIXELS && ny < GBA_VIDEO_VERTICAL_PIXELS) {
						ok = batch->checkBuffer[GBA_VIDEO_HORIZONTAL_PIXELS * ny + nx] == ours[x];
					}
				}
			}
			if (ok) {
				++near;
			} else if (firstX < 0) {
				firstX = x;
				firstY = y;
			}
		}
	}
	const char* dump = getenv("ARMGBA_BATCH_DUMP");
	if (dump && (unsigned) atoi(dump) == batch->frame) {
		int i;
		for (i = 0; i < 2; ++i) {
			char path[64];
			snprintf(path, sizeof(path), "/tmp/batch-%s.ppm", i ? "theirs" : "ours");
			FILE* f = fopen(path, "wb");
			fprintf(f, "P6 240 160 255\n");
			for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
				int x;
				for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
					mColor c = i ? batch->checkBuffer[GBA_VIDEO_HORIZONTAL_PIXELS * y + x] : batch->outputBuffer[batch->outputBufferStride * y + x];
					uint8_t rgb[3] = { (c >> 11) << 3, ((c >> 6) & 0x1F) << 3, (c & 0x1F) << 3 };
					fwrite(rgb, 1, 3, f);
				}
			}
			fclose(f);
		}
	}
	if (differ && batch->checkLog) {
		fprintf(batch->checkLog, "frame %u: %u differ, %u near", batch->frame, differ, near);
		if (firstX >= 0) {
			fprintf(batch->checkLog, ", first far at %d,%d ours %04X theirs %04X", firstX, firstY,
			        batch->outputBuffer[batch->outputBufferStride * firstY + firstX], batch->checkBuffer[GBA_VIDEO_HORIZONTAL_PIXELS * firstY + firstX]);
		}
		fputc('\n', batch->checkLog);
		fflush(batch->checkLog);
	}
}

static void GBAVideoBatchRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	_draw(batch, batch->nextY);
	batch->sw.d.finishFrame(&batch->sw.d);
	if (batch->check) {
		batch->check->d.finishFrame(&batch->check->d);
		if (batch->nextY == GBA_VIDEO_VERTICAL_PIXELS) {
			_compare(batch);
		}
	}
	++batch->frame;
	if (_timed && !(batch->frame % 1200)) {
		fprintf(stderr, "batch timing per frame: record %.1f us, draw %.1f us, upload %.1f us, %.2f draws, %.2f pages\n", _timing[0] * 1e6 / 1200, _timing[1] * 1e6 / 1200, _timing[2] * 1e6 / 1200, (_stats[0] - _stats[6]) / 1200.0, (_stats[1] - _stats[7]) / 1200.0);
		memset(_timing, 0, sizeof(_timing));
		_stats[6] = _stats[0];
		_stats[7] = _stats[1];
	}
	_stats[2] += batch->nPalettes;
	_stats[3] += batch->nSprites;
	if (_stats[4] < _stats[0] - _stats[5]) {
		_stats[4] = _stats[0] - _stats[5];
	}
	_stats[5] = _stats[0];
	if (batch->checkLog && !(batch->frame % 600)) {
		fprintf(batch->checkLog, "stats %u frames: %.2f draws/frame (max %u), %.2f pages/frame, %.2f palettes/frame, %.2f sprite tables/frame\n", batch->frame,
		        _stats[0] / (double) batch->frame, _stats[4], _stats[1] / (double) batch->frame, _stats[2] / (double) batch->frame, _stats[3] / (double) batch->frame);
	}
	_startFrame(batch);
}

static void GBAVideoBatchRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	*stride = batch->outputBufferStride;
	*pixels = batch->outputBuffer;
}

static void GBAVideoBatchRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels) {
	struct GBAVideoBatchRenderer* batch = (struct GBAVideoBatchRenderer*) renderer;
	const mColor* colorPixels = pixels;
	unsigned i;
	for (i = 0; i < GBA_VIDEO_VERTICAL_PIXELS; ++i) {
		memmove(&batch->outputBuffer[batch->outputBufferStride * i], &colorPixels[stride * i], GBA_VIDEO_HORIZONTAL_PIXELS * BYTES_PER_PIXEL);
	}
}
