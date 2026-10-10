/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef VIDEO_BATCH_H
#define VIDEO_BATCH_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/renderers/video-software.h>

// Scanlines are recorded as they come and drawn together, one pixel at a time, when VRAM changes or the frame ends
#define BATCH_LINE_WORDS 44
#define BATCH_SPRITE_WORDS 8
#define BATCH_VRAM_PAGES (GBA_SIZE_VRAM >> 12)
#define BATCH_MAGIC 0x68637462

struct GBAVideoBatchGL;

struct GBAVideoBatchRenderer {
	struct GBAVideoRenderer d;

	// Draws with OpenGL ES 3 when set, on the CPU otherwise
	struct GBAVideoBatchGL* gl;

	// Keeps the windows, affine steps, layer enables and sprite list; draws nothing
	struct GBAVideoSoftwareRenderer sw;

	mColor* outputBuffer;
	int outputBufferStride;

	uint32_t lines[GBA_VIDEO_VERTICAL_PIXELS][BATCH_LINE_WORDS];
	uint8_t spriteLists[GBA_VIDEO_VERTICAL_PIXELS][128];
	uint16_t palettes[GBA_VIDEO_VERTICAL_PIXELS][512];
	uint32_t sprites[GBA_VIDEO_VERTICAL_PIXELS][128][BATCH_SPRITE_WORDS];
	uint16_t vram[GBA_SIZE_VRAM / 2];

	int nPalettes;
	int nSprites;
	bool paletteDirty;
	bool oamDirty;
	uint32_t vramDirty;
	int drawnY;
	int nextY;

	// Optional: a full software renderer fed the same writes, compared every frame
	struct GBAVideoSoftwareRenderer* check;
	mColor* checkBuffer;
	FILE* checkLog;
	unsigned frame;
};

void GBAVideoBatchRendererCreate(struct GBAVideoBatchRenderer* renderer);
void GBAVideoBatchRendererReloadVRAM(struct GBAVideoBatchRenderer* renderer);

bool GBAVideoBatchRendererInitGL(struct GBAVideoBatchRenderer* renderer, void* (*getProc)(const char*), uintptr_t (*getFramebuffer)(void));
void GBAVideoBatchRendererDeinitGL(struct GBAVideoBatchRenderer* renderer);
void GBAVideoBatchGLUploadVRAM(struct GBAVideoBatchRenderer* renderer, uint32_t pages);
void GBAVideoBatchGLDraw(struct GBAVideoBatchRenderer* renderer, int startY, int endY);
void GBAVideoBatchGLStartFrame(struct GBAVideoBatchRenderer* renderer);

CXX_GUARD_END

#endif
