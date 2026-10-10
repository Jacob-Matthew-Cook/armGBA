/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/renderers/video-batch.h>

#include "gba/renderers/software-private.h"

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/renderers/common.h>

// One scanline as the pixel function reads it
enum {
	L_HEAD = 0,
	L_BLEND,
	L_WINDOWS,
	L_SEGMENT,
	L_ROWS = L_SEGMENT + MAX_WINDOW,
	L_BG,
};

enum {
	BG_CONTROL = 0,
	BG_OFFSET,
	BG_SX,
	BG_SY,
	BG_D,
	BG_DM,
	BG_WORDS
};

#define HEAD_EFFECT(H) (((H) >> 16) & 3)
#define HEAD_FORCE_TARGET_1 0x00040000
#define HEAD_TARGET_1_OBJ 0x00080000
#define HEAD_TARGET_1_BD 0x00100000
#define HEAD_TARGET_2_OBJ 0x00200000
#define HEAD_TARGET_2_BD 0x00400000
#define HEAD_BLANK 0x00800000
#define HEAD_SPRITES(H) ((H) >> 24)

#define CONTROL_ENABLED 0x1
#define CONTROL_PRESENT 0x2
#define CONTROL_PRIORITY(C) (((C) >> 2) & 3)
#define CONTROL_CHAR_BASE(C) ((((C) >> 4) & 3) << 14)
#define CONTROL_SCREEN_BASE(C) ((((C) >> 6) & 0x1F) << 11)
#define CONTROL_SIZE(C) (((C) >> 11) & 3)
#define CONTROL_256 0x2000
#define CONTROL_MOSAIC 0x4000
#define CONTROL_OVERFLOW 0x8000
#define CONTROL_TARGET_1 0x10000
#define CONTROL_TARGET_2 0x20000

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

static inline unsigned _color(uint16_t value) {
	return mColorFrom555(value);
}

static inline unsigned _variant(unsigned color, int effect, int bldy) {
	if (effect == BLEND_BRIGHTEN) {
		return _brighten(color, bldy);
	}
	if (effect == BLEND_DARKEN) {
		return _darken(color, bldy);
	}
	return color;
}

static inline uint32_t _batchBlendObjwin(unsigned blda, unsigned bldb, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			return mColorMix5Bit(blda, current, bldb, color);
		}
		return current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return (color & ~FLAG_TARGET_2) | (current & FLAG_OBJWIN);
}

static inline uint32_t _batchBlendNoObjwin(unsigned blda, unsigned bldb, uint32_t color, uint32_t current) {
	if (color >= current) {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			return mColorMix5Bit(blda, current, bldb, color);
		}
		return current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return color & ~FLAG_TARGET_2;
}

static inline uint32_t _batchNoBlendObjwin(uint32_t color, uint32_t current) {
	if (color < current) {
		return color | (current & FLAG_OBJWIN);
	}
	return current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
}

static inline uint32_t _batchNoBlendNoObjwin(uint32_t color, uint32_t current) {
	if (color >= current) {
		return current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return color;
}

#define VRAM16(B, A) ((B)->vram[(A) >> 1])
#define VRAM8(B, A) ((uint8_t) (VRAM16(B, A) >> (((A) & 1) << 3)))

// Everything the software renderer works out for one pixel, in the order it draws
static mColor _pixel(const struct GBAVideoBatchRenderer* batch, int x, int y) {
	const uint32_t* line = batch->lines[y];
	uint32_t head = line[L_HEAD];
	if (head & HEAD_BLANK) {
		return M_COLOR_WHITE;
	}
	GBARegisterDISPCNT dispcnt = head;
	int mode = GBARegisterDISPCNTGetMode(dispcnt);
	int effect = HEAD_EFFECT(head);
	unsigned blda = line[L_BLEND] & 0x1F;
	unsigned bldb = (line[L_BLEND] >> 5) & 0x1F;
	int bldy = (line[L_BLEND] >> 10) & 0x1F;
	GBAMosaicControl mosaic = line[L_BLEND] >> 16;
	const uint16_t* palette = batch->palettes[line[L_ROWS] & 0xFFFF];
	bool target1Obj = head & HEAD_TARGET_1_OBJ;
	bool brightness = effect == BLEND_BRIGHTEN || effect == BLEND_DARKEN;

	int nWindows = line[L_WINDOWS] & 0xFF;
	GBAWindowControl objwin = line[L_WINDOWS] >> 8;
	int objwinPriority = (int8_t) (line[L_WINDOWS] >> 16);
	int start = 0;
	int w;
	uint32_t segment = line[L_SEGMENT];
	for (w = 0; w < nWindows - 1 && x >= (int) (segment & 0xFF); ++w) {
		start = segment & 0xFF;
		segment = line[L_SEGMENT + w + 1];
	}
	int end = segment & 0xFF;
	GBAWindowControl control = segment >> 8;
	int controlPriority = (int8_t) (segment >> 16);
	bool objwinEnable = GBARegisterDISPCNTIsObjwinEnable(dispcnt);
	bool winBlend = GBAWindowControlIsBlendEnable(control);
	bool objwinBlend = GBAWindowControlIsBlendEnable(objwin);

	bool target2 = head & HEAD_TARGET_2_BD;
	bool anyTarget1 = false;
	int i;
	for (i = 0; i < 4; ++i) {
		uint32_t bg = line[L_BG + i * BG_WORDS + BG_CONTROL];
		if ((bg & CONTROL_TARGET_2) && (bg & CONTROL_PRESENT)) {
			target2 = true;
		}
		if (bg & CONTROL_TARGET_1) {
			anyTarget1 = true;
		}
	}

	unsigned backdrop = _color(palette[0]);
	if ((head & HEAD_TARGET_1_BD) && brightness && winBlend) {
		backdrop = _variant(backdrop, effect, bldy);
	}
	uint32_t row = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND | backdrop;
	uint32_t spriteLayer = FLAG_UNWRITTEN;

	if (GBARegisterDISPCNTIsObjEnable(dispcnt) && (GBAWindowControlIsObjEnable(control) || objwinEnable)) {
		const uint8_t* list = batch->spriteLists[y];
		const uint32_t (*sprites)[BATCH_SPRITE_WORDS] = batch->sprites[line[L_ROWS] >> 16];
		int mosaicV = GBAMosaicControlGetObjV(mosaic) + 1;
		int mosaicY = y - (y % mosaicV);
		int n = HEAD_SPRITES(head);
		for (i = 0; i < n; ++i) {
			const uint32_t* sprite = sprites[list[i]];
			GBAObjAttributesA a = sprite[0];
			GBAObjAttributesB b = sprite[0] >> 16;
			GBAObjAttributesC c = sprite[1];
			int spriteY = (int16_t) sprite[2];
			int spriteEndY = (int16_t) (sprite[2] >> 16);
			int localY = y;
			if (GBAObjAttributesAIsMosaic(a) && mosaicV > 1) {
				localY = mosaicY;
				if (localY < spriteY && spriteY < GBA_VIDEO_VERTICAL_PIXELS) {
					localY = spriteY;
				}
				if (localY >= (spriteEndY & 0xFF)) {
					localY = spriteEndY - 1;
				}
			}

			int width = GBAVideoObjSizes[GBAObjAttributesAGetShape(a) * 4 + GBAObjAttributesBGetSize(b)][0];
			int height = GBAVideoObjSizes[GBAObjAttributesAGetShape(a) * 4 + GBAObjAttributesBGetSize(b)][1];
			uint32_t flags = GBAObjAttributesCGetPriority(c) << OFFSET_PRIORITY;
			flags |= FLAG_TARGET_1 * ((winBlend && target1Obj && effect == BLEND_ALPHA) || GBAObjAttributesAGetMode(a) == OBJ_MODE_SEMITRANSPARENT);
			flags |= FLAG_OBJWIN * (GBAObjAttributesAGetMode(a) == OBJ_MODE_OBJWIN);
			if ((flags & FLAG_OBJWIN) && controlPriority < objwinPriority) {
				continue;
			}
			int32_t spriteX = (uint32_t) GBAObjAttributesBGetX(b) << 23;
			spriteX >>= 23;
			unsigned align = GBAObjAttributesAIs256Color(a) && !GBARegisterDISPCNTIsObjCharacterMapping(dispcnt);
			unsigned charBase = (GBAObjAttributesCGetTile(c) & ~align) * 0x20;
			unsigned maskLo = GBARegisterDISPCNTIsObjCharacterMapping(dispcnt) ? 0x7FFE : 0x3FE;
			unsigned maskHi = GBARegisterDISPCNTIsObjCharacterMapping(dispcnt) ? 0 : charBase & 0x7C00;
			if (mode >= 3 && GBAObjAttributesCGetTile(c) < 512) {
				continue;
			}
			bool objwinSlowPath = objwinEnable && objwinBlend != winBlend;
			bool variant = target1Obj && winBlend && brightness;
			if (GBAObjAttributesAGetMode(a) == OBJ_MODE_SEMITRANSPARENT || (target1Obj && effect == BLEND_ALPHA) || objwinSlowPath) {
				if (target2) {
					flags |= FLAG_REBLEND;
					variant = false;
				} else {
					flags &= ~FLAG_TARGET_1;
				}
			}
			bool objwinVariant = variant && objwinBlend;
			unsigned paletteBase = 0x100;
			if (!GBAObjAttributesAIs256Color(a)) {
				paletteBase += GBAObjAttributesCGetPalette(c) << 4;
			}
			int inY = localY - (int) GBAObjAttributesAGetY(a);
			int stride = GBARegisterDISPCNTIsObjCharacterMapping(dispcnt) ? (width >> !GBAObjAttributesAIs256Color(a)) : 0x80;

			int localX;
			int tileY;
			if (GBAObjAttributesAIsTransformed(a)) {
				int totalWidth = width << GBAObjAttributesAGetDoubleSize(a);
				int totalHeight = height << GBAObjAttributesAGetDoubleSize(a);
				int pa = (int16_t) sprite[3];
				int pb = (int16_t) (sprite[3] >> 16);
				int pc = (int16_t) sprite[4];
				int pd = (int16_t) (sprite[4] >> 16);
				if (inY < 0) {
					inY += 256;
				}
				int outX = spriteX >= start ? spriteX : start;
				int condition = spriteX + totalWidth;
				int inX = outX - spriteX;
				if (end < condition) {
					condition = end;
				}
				int mosaicH = 1;
				if (GBAObjAttributesAIsMosaic(a)) {
					mosaicH = GBAMosaicControlGetObjH(mosaic) + 1;
					if (condition != end && condition % mosaicH) {
						condition += mosaicH - (condition % mosaicH);
					}
				}
				int xAccum = pa * (inX - 1 - (totalWidth >> 1)) + pb * (inY - (totalHeight >> 1)) + (width << 7);
				int yAccum = pc * (inX - 1 - (totalWidth >> 1)) + pd * (inY - (totalHeight >> 1)) + (height << 7);
				if (pa) {
					int32_t skip = 0;
					if ((xAccum >> 8) < 0) {
						skip = (-xAccum - 1) / pa;
					} else if ((xAccum >> 8) >= width) {
						skip = ((width << 8) - xAccum) / pa;
					}
					xAccum += pa * skip;
					yAccum += pc * skip;
					outX += skip;
				}
				if (pc) {
					int32_t skip = 0;
					if ((yAccum >> 8) < 0) {
						skip = (-yAccum - 1) / pc;
					} else if ((yAccum >> 8) >= height) {
						skip = ((height << 8) - yAccum) / pc;
					}
					xAccum += pa * skip;
					yAccum += pc * skip;
					outX += skip;
				}
				if (outX < start || outX >= condition || x < outX || x >= condition) {
					continue;
				}
				unsigned widthMask = ~(width - 1);
				unsigned heightMask = ~(height - 1);
				int step;
				if ((flags & FLAG_OBJWIN) || mosaicH <= 1) {
					// Drawing stops at the first pixel outside the sprite
					if (((xAccum + pa) >> 8) & widthMask || ((yAccum + pc) >> 8) & heightMask) {
						continue;
					}
					step = x - outX + 1;
				} else {
					int sampleX = x - (x % mosaicH);
					step = sampleX >= outX ? sampleX - outX + 1 : 0;
				}
				localX = (xAccum + pa * step) >> 8;
				tileY = (yAccum + pc * step) >> 8;
				if (localX & widthMask || tileY & heightMask) {
					continue;
				}
			} else {
				int outX = spriteX >= start ? spriteX : start;
				int condition = spriteX + width;
				int mosaicH = 1;
				if (GBAObjAttributesAIsMosaic(a)) {
					mosaicH = GBAMosaicControlGetObjH(mosaic) + 1;
					if (condition % mosaicH) {
						condition += mosaicH - (condition % mosaicH);
					}
				}
				if ((int) GBAObjAttributesAGetY(a) + height - 256 >= 0) {
					inY += 256;
				}
				if (GBAObjAttributesBIsVFlip(b)) {
					inY = height - inY - 1;
				}
				if (end < condition) {
					condition = end;
				}
				if (x < outX || x >= condition) {
					continue;
				}
				int inX = x - spriteX;
				int xOffset = 1;
				if (GBAObjAttributesBIsHFlip(b)) {
					inX = width - inX - 1;
					xOffset = -1;
				}
				localX = inX;
				if (!(flags & FLAG_OBJWIN) && mosaicH > 1) {
					localX = inX - xOffset * (x % mosaicH);
					if (localX < 0) {
						localX = 0;
					} else if (localX > width - 1) {
						localX = width - 1;
					}
				}
				tileY = inY;
			}

			unsigned tileData;
			if (!GBAObjAttributesAIs256Color(a)) {
				unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
				unsigned yBase = (tileY & ~0x7) * stride + (tileY & 0x7) * 4 + maskHi;
				tileData = VRAM16(batch, BASE_TILE + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFE));
				tileData = (tileData >> ((localX & 3) << 2)) & 0xF;
			} else {
				unsigned xBase = (localX & ~0x7) * 8 + (localX & 6);
				unsigned yBase = (tileY & ~0x7) * stride + (tileY & 0x7) * 8 + maskHi;
				tileData = VRAM16(batch, BASE_TILE + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFE));
				tileData = (tileData >> ((localX & 1) << 3)) & 0xFF;
			}

			uint32_t current = spriteLayer;
			uint32_t reordered = (current & ~(FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1)) | (flags & (FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1));
			if (flags & FLAG_OBJWIN) {
				if (tileData) {
					row |= FLAG_OBJWIN;
				} else if (current != FLAG_UNWRITTEN && (current & FLAG_ORDER_MASK) > flags) {
					spriteLayer = reordered;
				}
			} else if ((current & FLAG_ORDER_MASK) > flags) {
				if (tileData) {
					unsigned color = _color(palette[paletteBase + tileData]);
					if ((objwinSlowPath && (row & FLAG_OBJWIN)) ? objwinVariant : variant) {
						color = _variant(color, effect, bldy);
					}
					spriteLayer = color | flags;
				} else if (current != FLAG_UNWRITTEN) {
					spriteLayer = reordered;
				}
			}
		}
	}

	unsigned priority;
	for (priority = 0; priority < 4; ++priority) {
		uint32_t sprite = spriteLayer & ~FLAG_OBJWIN;
		if ((sprite & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (sprite & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority) {
			sprite |= FLAG_TARGET_2 * !!(head & HEAD_TARGET_2_OBJ);
			if (objwinEnable) {
				bool objwinDisable = !GBAWindowControlIsObjEnable(objwin);
				bool objwinOnly = !objwinDisable && !GBAWindowControlIsObjEnable(control);
				if (objwinDisable && !GBAWindowControlIsObjEnable(control)) {
					// Not drawn in this window
				} else if (objwinDisable ? !(row & FLAG_OBJWIN) : (!objwinOnly || (row & FLAG_OBJWIN))) {
					row = _batchBlendObjwin(blda, bldb, sprite, row);
				}
			} else if (GBAWindowControlIsObjEnable(control)) {
				row = _batchBlendNoObjwin(blda, bldb, sprite, row);
			}
		}

		int index;
		for (index = 0; index < 4; ++index) {
			const uint32_t* bg = &line[L_BG + index * BG_WORDS];
			uint32_t bgControl = bg[BG_CONTROL];
			if (!(bgControl & CONTROL_ENABLED) || CONTROL_PRIORITY(bgControl) != priority) {
				continue;
			}
			bool windowed = control & (1 << index);
			bool objwinBg = objwin & (1 << index);
			if (!windowed && !(objwinEnable && objwinBg)) {
				continue;
			}
			int bgMode;
			if (index < 2) {
				if (mode >= 2) {
					continue;
				}
				bgMode = 0;
			} else if (index == 2) {
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

			bool target1 = bgControl & CONTROL_TARGET_1;
			uint32_t flags = (priority << OFFSET_PRIORITY) | (index << OFFSET_INDEX) | FLAG_IS_BACKGROUND;
			if (bgControl & CONTROL_TARGET_2) {
				flags |= FLAG_TARGET_2;
			}
			uint32_t objwinFlags = flags;
			if (effect == BLEND_ALPHA) {
				if (blda == 0x10 && bldb == 0) {
					flags &= ~FLAG_TARGET_2;
					objwinFlags &= ~FLAG_TARGET_2;
				} else if (target1) {
					if (winBlend) {
						flags |= FLAG_TARGET_1;
					}
					if (objwinBlend) {
						objwinFlags |= FLAG_TARGET_1;
					}
				}
			}
			bool variant = target1 && winBlend && brightness;
			bool objwinVariant = objwinEnable && target1 && objwinBlend && brightness;
			bool objwinForceEnable = objwinBg && windowed;
			bool objwinOnly = !objwinBg;

			if (bgMode == 3 || bgMode == 5) {
				// Direct color: no transparency, always composited
				int32_t sx = bg[BG_SX];
				int32_t sy = bg[BG_SY];
				int dx = (int16_t) bg[BG_D];
				int dy = (int16_t) (bg[BG_D] >> 16);
				int sampleX = x;
				if (bgControl & CONTROL_MOSAIC) {
					int mosaicV = GBAMosaicControlGetBgV(mosaic) + 1;
					int mosaicH = GBAMosaicControlGetBgH(mosaic) + 1;
					sx -= (y % mosaicV) * (int16_t) bg[BG_DM];
					sy -= (y % mosaicV) * (int16_t) (bg[BG_DM] >> 16);
					sampleX = x - (x % mosaicH);
				}
				int32_t localX = sx + sampleX * dx;
				int32_t localY = sy + sampleX * dy;
				int width = bgMode == 3 ? GBA_VIDEO_HORIZONTAL_PIXELS : 160;
				int height = bgMode == 3 ? GBA_VIDEO_VERTICAL_PIXELS : 128;
				if (localX < 0 || localY < 0 || (localX >> 8) >= width || (localY >> 8) >= height) {
					continue;
				}
				unsigned color;
				if (bgMode == 3) {
					color = _color(VRAM16(batch, ((localX >> 8) + (localY >> 8) * GBA_VIDEO_HORIZONTAL_PIXELS) << 1));
				} else {
					uint32_t offset = GBARegisterDISPCNTIsFrameSelect(dispcnt) ? 0xA000 : 0;
					color = _color(VRAM16(batch, offset + (localX >> 8) * 2 + (localY >> 8) * 320));
				}
				if (!objwinEnable || (!(row & FLAG_OBJWIN)) != objwinOnly) {
					uint32_t mergedFlags = (row & FLAG_OBJWIN) ? objwinFlags : flags;
					if (variant) {
						color = _variant(color, effect, bldy);
					}
					row = _batchBlendObjwin(blda, bldb, color | mergedFlags, row);
				}
				continue;
			}

			unsigned pixelData;
			unsigned paletteData = 0;
			if (bgMode == 0) {
				int sampleX = x;
				int inY = y;
				if (bgControl & CONTROL_MOSAIC) {
					if (GBAMosaicControlGetBgH(mosaic)) {
						sampleX = x - (x % (GBAMosaicControlGetBgH(mosaic) + 1));
					}
					inY -= inY % (GBAMosaicControlGetBgV(mosaic) + 1);
				}
				int inX = (sampleX + (bg[BG_OFFSET] & 0xFFFF)) & 0x1FF;
				inY += bg[BG_OFFSET] >> 16;
				int size = CONTROL_SIZE(bgControl);
				unsigned yBase = inY & 0xF8;
				if (size == 2) {
					yBase += inY & 0x100;
				} else if (size == 3) {
					yBase += (inY & 0x100) << 1;
				}
				yBase = (CONTROL_SCREEN_BASE(bgControl) >> 1) + (yBase << 2);
				unsigned xBase = inX & 0xF8;
				if (size & 1) {
					xBase += (inX & 0x100) << 5;
				}
				uint16_t mapData = batch->vram[yBase + (xBase >> 3)];
				int localY = inY & 0x7;
				if (GBA_TEXT_MAP_VFLIP(mapData)) {
					localY = 7 - localY;
				}
				int column = inX & 0x7;
				if (GBA_TEXT_MAP_HFLIP(mapData)) {
					column = 7 - column;
				}
				uint32_t charBase;
				if (!(bgControl & CONTROL_256)) {
					charBase = CONTROL_CHAR_BASE(bgControl) + (GBA_TEXT_MAP_TILE(mapData) << 5) + (localY << 2);
					if (charBase >= 0x10000) {
						continue;
					}
					pixelData = VRAM8(batch, charBase + (column >> 1));
					pixelData = (column & 1) ? pixelData >> 4 : pixelData & 0xF;
					paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4;
				} else {
					charBase = CONTROL_CHAR_BASE(bgControl) + (GBA_TEXT_MAP_TILE(mapData) << 6) + (localY << 3);
					if (charBase >= 0x10000) {
						continue;
					}
					pixelData = VRAM8(batch, charBase + column);
				}
			} else {
				int32_t sx = bg[BG_SX];
				int32_t sy = bg[BG_SY];
				int dx = (int16_t) bg[BG_D];
				int dy = (int16_t) (bg[BG_D] >> 16);
				int sampleX = x;
				if (bgControl & CONTROL_MOSAIC) {
					int mosaicV = GBAMosaicControlGetBgV(mosaic) + 1;
					int mosaicH = GBAMosaicControlGetBgH(mosaic) + 1;
					sx -= (y % mosaicV) * (int16_t) bg[BG_DM];
					sy -= (y % mosaicV) * (int16_t) (bg[BG_DM] >> 16);
					sampleX = x - (x % mosaicH);
				}
				int32_t localX = sx + sampleX * dx;
				int32_t localY = sy + sampleX * dy;
				if (bgMode == 4) {
					if (localX < 0 || localY < 0 || (localX >> 8) >= GBA_VIDEO_HORIZONTAL_PIXELS || (localY >> 8) >= GBA_VIDEO_VERTICAL_PIXELS) {
						continue;
					}
					uint32_t offset = GBARegisterDISPCNTIsFrameSelect(dispcnt) ? 0xA000 : 0;
					pixelData = VRAM8(batch, offset + (localX >> 8) + (localY >> 8) * GBA_VIDEO_HORIZONTAL_PIXELS);
					if (!pixelData || !IS_WRITABLE(row)) {
						continue;
					}
					if (!objwinEnable) {
						unsigned color = _color(palette[pixelData]);
						if (variant) {
							color = _variant(color, effect, bldy);
						}
						row = _batchBlendNoObjwin(blda, bldb, color | flags, row);
					} else if (objwinForceEnable || (!(row & FLAG_OBJWIN)) == objwinOnly) {
						unsigned color = _color(palette[pixelData]);
						if ((row & FLAG_OBJWIN) ? objwinVariant : variant) {
							color = _variant(color, effect, bldy);
						}
						row = _batchBlendObjwin(blda, bldb, color | ((row & FLAG_OBJWIN) ? objwinFlags : flags), row);
					}
					continue;
				}
				int32_t sizeAdjusted = 0x8000 << CONTROL_SIZE(bgControl);
				if (bgControl & CONTROL_OVERFLOW) {
					localX &= sizeAdjusted - 1;
					localY &= sizeAdjusted - 1;
				} else if ((localX | localY) & ~(sizeAdjusted - 1)) {
					continue;
				}
				uint8_t mapData = VRAM8(batch, CONTROL_SCREEN_BASE(bgControl) + (localX >> 11) + (((localY >> 7) & 0x7F0) << CONTROL_SIZE(bgControl)));
				pixelData = VRAM8(batch, CONTROL_CHAR_BASE(bgControl) + (mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8));
			}

			if (!pixelData || !IS_WRITABLE(row)) {
				continue;
			}
			unsigned entry = paletteData | pixelData;
			bool reblend = (row & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND;
			if (!objwinEnable) {
				unsigned color = _color(palette[entry]);
				if (variant && !reblend) {
					color = _variant(color, effect, bldy);
				}
				if (flags & FLAG_TARGET_2) {
					row = _batchBlendNoObjwin(blda, bldb, color | flags, row);
				} else {
					row = _batchNoBlendNoObjwin(color | flags, row);
				}
			} else if (objwinForceEnable || (!(row & FLAG_OBJWIN)) == objwinOnly) {
				unsigned color = _color(palette[entry]);
				uint32_t mergedFlags = flags;
				if (row & FLAG_OBJWIN) {
					mergedFlags = objwinFlags;
					if (objwinVariant) {
						color = _variant(color, effect, bldy);
					}
				} else if (variant && !reblend) {
					color = _variant(color, effect, bldy);
				}
				if (flags & FLAG_TARGET_2) {
					row = _batchBlendObjwin(blda, bldb, color | mergedFlags, row);
				} else {
					row = _batchNoBlendObjwin(color | mergedFlags, row);
				}
			}
		}
	}

	if (((head & HEAD_FORCE_TARGET_1) || anyTarget1) && (head & HEAD_TARGET_2_BD)) {
		if (row & FLAG_TARGET_1) {
			unsigned color = _color(palette[0]);
			if ((head & HEAD_TARGET_1_BD) && brightness && winBlend) {
				color = _variant(color, effect, bldy);
			}
			row = mColorMix5Bit(bldb, color, blda, row);
		}
	}
	if ((head & HEAD_FORCE_TARGET_1) && brightness) {
		uint32_t mask = FLAG_REBLEND | FLAG_IS_BACKGROUND;
		uint32_t match = FLAG_REBLEND;
		bool apply = true;
		if (objwinEnable && objwinBlend != winBlend) {
			mask |= FLAG_OBJWIN;
			if (objwinBlend) {
				match |= FLAG_OBJWIN;
			}
		} else if (!winBlend) {
			apply = false;
		}
		if (apply && (row & mask) == match) {
			row = _variant(row, effect, bldy);
		}
	}
	return row;
}

static unsigned _stats[8];

static void _draw(struct GBAVideoBatchRenderer* batch, int endY) {
	if (endY > batch->drawnY) {
		++_stats[0];
	}
	int y;
	for (y = batch->drawnY; y < endY; ++y) {
		mColor* out = &batch->outputBuffer[batch->outputBufferStride * y];
		int x;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			out[x] = _pixel(batch, x, y);
		}
	}
	batch->drawnY = endY;
}

static void _uploadVRAM(struct GBAVideoBatchRenderer* batch, uint32_t pages) {
	batch->vramDirty &= ~pages;
	while (pages) {
		unsigned page = __builtin_ctz(pages);
		pages &= pages - 1;
		++_stats[1];
		memcpy(&batch->vram[page << 11], &batch->d.vram[page << 11], 4096);
	}
}

static uint32_t _pages(uint32_t start, uint32_t size) {
	uint32_t end = (start + size - 1) >> 12;
	if (end >= BATCH_VRAM_PAGES) {
		end = BATCH_VRAM_PAGES - 1;
	}
	return (2U << end) - (1U << (start >> 12));
}

static uint32_t _rowPages(const uint32_t* bg, int y, GBAMosaicControl mosaic, uint32_t base, int rowBytes, int rows) {
	int32_t sy = bg[BG_SY];
	int dy = (int16_t) (bg[BG_D] >> 16);
	int dmy = (int16_t) (bg[BG_DM] >> 16);
	int32_t first = sy;
	int32_t last = sy + (GBA_VIDEO_HORIZONTAL_PIXELS - 1) * dy;
	if (bg[BG_CONTROL] & CONTROL_MOSAIC) {
		first -= (y % (GBAMosaicControlGetBgV(mosaic) + 1)) * dmy;
		last -= (y % (GBAMosaicControlGetBgV(mosaic) + 1)) * dmy;
	}
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
static uint32_t _textPages(const struct GBAVideoBatchRenderer* batch, const uint32_t* bg, int y, GBAMosaicControl mosaic) {
	uint32_t control = bg[BG_CONTROL];
	int inY = y;
	if (control & CONTROL_MOSAIC) {
		inY -= inY % (GBAMosaicControlGetBgV(mosaic) + 1);
	}
	inY += bg[BG_OFFSET] >> 16;
	int size = CONTROL_SIZE(control);
	unsigned yBase = inY & 0xF8;
	if (size == 2) {
		yBase += inY & 0x100;
	} else if (size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (CONTROL_SCREEN_BASE(control) >> 1) + (yBase << 2);
	uint32_t pages = 0;
	unsigned minTile = 0x3FF;
	unsigned maxTile = 0;
	int tile;
	int inX = bg[BG_OFFSET] & 0x1F8;
	for (tile = 0; tile < 31; ++tile, inX += 8) {
		unsigned xBase = inX & 0xF8;
		if (size & 1) {
			xBase += (inX & 0x100) << 5;
		}
		unsigned address = yBase + (xBase >> 3);
		pages |= 1U << (address >> 11);
		unsigned index = GBA_TEXT_MAP_TILE(batch->d.vram[address]);
		if (index < minTile) {
			minTile = index;
		}
		if (index > maxTile) {
			maxTile = index;
		}
	}
	unsigned shift = control & CONTROL_256 ? 6 : 5;
	uint32_t start = CONTROL_CHAR_BASE(control) + (minTile << shift);
	uint32_t end = CONTROL_CHAR_BASE(control) + ((maxTile + 1) << shift);
	if (end > 0x10000) {
		end = 0x10000;
	}
	if (start < end) {
		pages |= _pages(start, end - start);
	}
	return pages;
}

// The pages a sprite on this line reads
static uint32_t _spritePages(const struct GBAVideoBatchRenderer* batch, const uint32_t* sprite) {
	GBAObjAttributesA a = sprite[0];
	GBAObjAttributesB b = sprite[0] >> 16;
	GBAObjAttributesC c = sprite[1];
	int width = GBAVideoObjSizes[GBAObjAttributesAGetShape(a) * 4 + GBAObjAttributesBGetSize(b)][0];
	int height = GBAVideoObjSizes[GBAObjAttributesAGetShape(a) * 4 + GBAObjAttributesBGetSize(b)][1];
	unsigned rowBytes = (width / 8) * (GBAObjAttributesAIs256Color(a) ? 64 : 32);
	unsigned rows = height / 8;
	unsigned bytes;
	if (GBARegisterDISPCNTIsObjCharacterMapping(batch->sw.dispcnt)) {
		bytes = rowBytes * rows;
	} else {
		bytes = (rows - 1) * 0x400 + rowBytes;
	}
	uint32_t start = GBAObjAttributesCGetTile(c) * 32;
	if (start + bytes > 0x8000) {
		return _pages(BASE_TILE, 0x8000);
	}
	return _pages(BASE_TILE + start, bytes);
}

// The 4 KiB pages of VRAM a recorded line can read, roughly when nothing it might read is dirty
static uint32_t _readPages(const struct GBAVideoBatchRenderer* batch, const uint32_t* line, int y, bool exact) {
	uint32_t head = line[L_HEAD];
	if (head & HEAD_BLANK) {
		return 0;
	}
	int mode = GBARegisterDISPCNTGetMode(head);
	GBAMosaicControl mosaic = line[L_BLEND] >> 16;
	uint32_t pages = 0;
	if (HEAD_SPRITES(head)) {
		if (!exact) {
			pages |= mode >= 3 ? _pages(0x14000, 0x4000) : _pages(BASE_TILE, 0x8000);
		} else {
			const uint32_t (*sprites)[BATCH_SPRITE_WORDS] = batch->sprites[line[L_ROWS] >> 16];
			int i;
			for (i = 0; i < (int) HEAD_SPRITES(head); ++i) {
				pages |= _spritePages(batch, sprites[batch->spriteLists[y][i]]);
			}
		}
	}
	int i;
	for (i = 0; i < 4; ++i) {
		const uint32_t* bg = &line[L_BG + i * BG_WORDS];
		uint32_t control = bg[BG_CONTROL];
		if (!(control & CONTROL_ENABLED)) {
			continue;
		}
		if (mode >= 3) {
			if (i != 2) {
				continue;
			}
			uint32_t frame = mode != 3 && GBARegisterDISPCNTIsFrameSelect(head) ? 0xA000 : 0;
			if (!exact) {
				pages |= _pages(frame, mode == 3 ? 0x12C00 : mode == 4 ? 0x9600 : 0xA000);
			} else if (mode == 3) {
				pages |= _rowPages(bg, y, mosaic, 0, 480, GBA_VIDEO_VERTICAL_PIXELS);
			} else if (mode == 4) {
				pages |= _rowPages(bg, y, mosaic, frame, 240, GBA_VIDEO_VERTICAL_PIXELS);
			} else {
				pages |= _rowPages(bg, y, mosaic, frame, 320, 128);
			}
		} else if ((i < 2 && mode != 2) || mode == 0) {
			if (exact) {
				pages |= _textPages(batch, bg, y, mosaic);
				continue;
			}
			static const uint32_t screenSize[4] = { 0x800, 0x1000, 0x1000, 0x2000 };
			pages |= _pages(CONTROL_SCREEN_BASE(control), screenSize[CONTROL_SIZE(control)]);
			uint32_t charSize = control & CONTROL_256 ? 0x10000 : 0x8000;
			if (CONTROL_CHAR_BASE(control) + charSize > 0x10000) {
				charSize = 0x10000 - CONTROL_CHAR_BASE(control);
			}
			pages |= _pages(CONTROL_CHAR_BASE(control), charSize);
		} else if (i >= 2) {
			pages |= _pages(CONTROL_SCREEN_BASE(control), 0x100 << (2 * CONTROL_SIZE(control)));
			pages |= _pages(CONTROL_CHAR_BASE(control), 0x4000);
		}
	}
	return pages;
}

// The sprites the software renderer would draw on this line, and whether any of them blends again afterwards
static int _spriteList(struct GBAVideoBatchRenderer* batch, int y, bool* forceTarget1) {
	struct GBAVideoSoftwareRenderer* sw = &batch->sw;
	uint8_t* list = batch->spriteLists[y];
	int cycles = GBARegisterDISPCNTIsHblankIntervalFree(sw->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
	bool objwinEnable = GBARegisterDISPCNTIsObjwinEnable(sw->dispcnt);
	bool target2 = sw->target2Bd;
	int i;
	for (i = 0; i < 4; ++i) {
		target2 |= sw->bg[i].target2 && sw->bg[i].enabled;
	}
	int n = 0;
	int lastIndex = 0;
	for (i = 0; i < sw->oamMax; ++i) {
		struct GBAVideoRendererSprite* sprite = &sw->sprites[i];
		cycles -= 2 * (sprite->index - lastIndex);
		lastIndex = sprite->index;
		if (cycles <= 0) {
			break;
		}
		if (y < sprite->y || y >= sprite->endY) {
			continue;
		}
		list[n] = i;
		++n;
		cycles -= sprite->cycles;

		if (*forceTarget1 || !target2) {
			continue;
		}
		GBAObjAttributesA a = sprite->obj.a;
		if (GBARegisterDISPCNTGetMode(sw->dispcnt) >= 3 && GBAObjAttributesCGetTile(sprite->obj.c) < 512) {
			continue;
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
				*forceTarget1 = true;
				break;
			}
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
	int nSprites = 0;
	bool forceTarget1 = false;
	if (GBARegisterDISPCNTIsObjEnable(sw->dispcnt)) {
		if (sw->oamDirty) {
			sw->oamMax = GBAVideoRendererCleanOAM(sw->d.oam->obj, sw->sprites, sw->objOffsetY);
			sw->oamDirty = false;
			batch->oamDirty = true;
		}
		if (batch->oamDirty) {
			uint32_t (*sprites)[BATCH_SPRITE_WORDS] = batch->sprites[batch->nSprites];
			int i;
			for (i = 0; i < sw->oamMax; ++i) {
				struct GBAVideoRendererSprite* sprite = &sw->sprites[i];
				const struct GBAOAMMatrix* mat = &sw->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->obj.b)];
				sprites[i][0] = sprite->obj.a | (sprite->obj.b << 16);
				sprites[i][1] = sprite->obj.c;
				sprites[i][2] = (uint16_t) sprite->y | ((uint32_t) (uint16_t) sprite->endY << 16);
				sprites[i][3] = (uint16_t) mat->a | ((uint32_t) (uint16_t) mat->b << 16);
				sprites[i][4] = (uint16_t) mat->c | ((uint32_t) (uint16_t) mat->d << 16);
				sprites[i][5] = sprite->index;
			}
			++batch->nSprites;
			batch->oamDirty = false;
		}
		nSprites = _spriteList(batch, y, &forceTarget1);
	}

	line[L_HEAD] = sw->dispcnt | (sw->blendEffect << 16) | (forceTarget1 ? HEAD_FORCE_TARGET_1 : 0) |
	               (sw->target1Obj ? HEAD_TARGET_1_OBJ : 0) | (sw->target1Bd ? HEAD_TARGET_1_BD : 0) |
	               (sw->target2Obj ? HEAD_TARGET_2_OBJ : 0) | (sw->target2Bd ? HEAD_TARGET_2_BD : 0) |
	               ((uint32_t) nSprites << 24);
	line[L_BLEND] = sw->blda | (sw->bldb << 5) | (sw->bldy << 10) | ((uint32_t) sw->mosaic << 16);
	line[L_WINDOWS] = sw->nWindows | (sw->objwin.packed << 8) | ((uint8_t) sw->objwin.priority << 16);
	int w;
	for (w = 0; w < sw->nWindows; ++w) {
		line[L_SEGMENT + w] = sw->windows[w].endX | (sw->windows[w].control.packed << 8) | ((uint8_t) sw->windows[w].control.priority << 16);
	}
	line[L_ROWS] = (batch->nPalettes - 1) | ((uint32_t) (batch->nSprites ? batch->nSprites - 1 : 0) << 16);
	int i;
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &sw->bg[i];
		uint32_t* out = &line[L_BG + i * BG_WORDS];
		out[BG_CONTROL] = (bg->enabled == ENABLED_MAX ? CONTROL_ENABLED : 0) | (bg->enabled ? CONTROL_PRESENT : 0) |
		                  (bg->priority << 2) | ((bg->charBase >> 14) << 4) | ((bg->screenBase >> 11) << 6) |
		                  (bg->size << 11) | (bg->multipalette ? CONTROL_256 : 0) | (bg->mosaic ? CONTROL_MOSAIC : 0) |
		                  (bg->overflow ? CONTROL_OVERFLOW : 0) | (bg->target1 ? CONTROL_TARGET_1 : 0) |
		                  (bg->target2 ? CONTROL_TARGET_2 : 0);
		out[BG_OFFSET] = bg->x | ((uint32_t) bg->y << 16);
		out[BG_SX] = bg->sx;
		out[BG_SY] = bg->sy;
		out[BG_D] = (uint16_t) bg->dx | ((uint32_t) (uint16_t) bg->dy << 16);
		out[BG_DM] = (uint16_t) bg->dmx | ((uint32_t) (uint16_t) bg->dmy << 16);
	}
}

static void GBAVideoBatchRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
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
		return;
	}
	GBAVideoSoftwareRendererPreprocessBuffer(sw);
	_recordLine(batch, y);

	// Lines waiting to be drawn saw VRAM as it was; this one may need newer pages
	uint32_t stale = batch->vramDirty & _readPages(batch, batch->lines[y], y, false);
	if (stale && !(batch->vramDirty & _readPages(batch, batch->lines[y], y, true))) {
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
