/* Copyright (c) 2013-2015 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_RENDER_H
#define ARMGBA_RENDER_H

#include <stdbool.h>
#include <stdint.h>

// The state of mGBA's software renderer, drawing BGR555 the way its 16-bit color builds do

enum {
	SCREEN_WIDTH = 240,
	SCREEN_HEIGHT = 160,
};

// What stores changed since the last line was drawn, as mGBA's writeVRAM/writeOAM/writePalette hear it
enum {
	RENDER_VRAM = 1,
	RENDER_OAM = 2,
	RENDER_PALETTE = 4,
};

struct RenderBackground {
	int index;
	int enabled;
	unsigned priority;
	uint32_t charBase;
	int mosaic;
	int multipalette;
	uint32_t screenBase;
	int overflow;
	int size;
	int target1;
	int target2;
	uint16_t x;
	uint16_t y;
	// The reference point as written; the BG takes it on at the start of the next line
	int32_t refx;
	int32_t refy;
	bool refxWritten;
	bool refyWritten;
	int16_t dx;
	int16_t dmx;
	int16_t dy;
	int16_t dmy;
	int32_t sx;
	int32_t sy;
	int yCache;
	uint16_t mapCache[64];
	// The pages a row of tiles reads, and where that row was; cleared when its map changes
	uint64_t rowPages;
	uint64_t mapPages;
	int32_t rowKey;
	uint32_t flags;
	uint32_t objwinFlags;
	int objwinForceEnable;
	bool objwinOnly;
	bool variant;
};

struct RenderWindowControl {
	uint8_t packed;
	int8_t priority;
};

struct RenderWindow {
	uint8_t endX;
	struct RenderWindowControl control;
};

struct RenderWindowN {
	uint8_t hEnd;
	uint8_t hStart;
	uint8_t vEnd;
	uint8_t vStart;
	struct RenderWindowControl control;
	bool on;
};

struct RenderSprite {
	uint16_t a;
	uint16_t b;
	uint16_t c;
	int16_t y;
	int16_t endY;
	int16_t cycles;
	int8_t index;
	// The VRAM pages its tiles lie in
	uint64_t pages;
};

struct Renderer {
	uint16_t dispcnt;
	// DISPCNT as latched 40 cycles into each of the last three lines: a layer shows only once enabled
	// in both the oldest and the current, and forced blank in either blanks
	uint16_t dispcntLatch[3];
	// The DISPCNT this line draws with
	uint16_t lineDispcnt;
#if defined(__aarch64__)
	// The line being built: each pixel's color, and its flags (render.c's 32-bit pixel's top half)
	// apart, for NEON's 16-bit lanes
	uint16_t rowColor[SCREEN_WIDTH];
	uint16_t rowFlags[SCREEN_WIDTH];
#else
	// The line being built, a 32-bit pixel each (render.c), for AVX2's 32-bit lanes
	uint32_t row[SCREEN_WIDTH];
#endif
	uint32_t spriteLayer[SCREEN_WIDTH];
	int32_t spriteCyclesRemaining;
	// The columns sprites may have written on this line
	int spriteFrom;
	int spriteTo;
	// A sprite with horizontal mosaic is on this line, and the columns such sprites cover
	bool spriteMosaic;
	uint8_t spriteMosaicCover[SCREEN_WIDTH];

	unsigned target1Obj;
	unsigned target1Bd;
	unsigned target2Obj;
	unsigned target2Bd;
	bool blendDirty;
	bool paletteDirty;
	int blendEffect;
	uint16_t variantPalette[512];
	// The palette variantPalette was made from, to find the 16-color banks a change touched
	uint16_t paletteSeen[512];
	// The same, 32 bits wide for SIMD gathers
	uint32_t normal32[512];
	uint32_t variant32[512];
	// Low and high bytes of each 16-color bank, for byte shuffles: [normal/variant][bank][low/high][16]
	uint8_t bankBytes[2][32][2][16];
	// The banks of these three made before the palette last changed, one bit each
	uint32_t palette32Stale;

	uint16_t blda;
	uint16_t bldb;
	uint16_t bldy;

	uint16_t mosaic;
	bool stereo;
	// Green swap as the line started, and its changes since with the line cycle of each
	bool lineStereo;
	int stereoChanges;
	uint16_t stereoCycle[8];
	bool stereoValue[8];

	struct RenderWindowN winN[2];
	struct RenderWindowControl winout;
	struct RenderWindowControl objwin;
	struct RenderWindowControl currentWindow;
	int nWindows;
	struct RenderWindow windows[5];

	struct RenderBackground bg[4];

	bool forceTarget1;
	bool oamDirty;
	int oamMax;
	struct RenderSprite sprites[128];
	// The sprites on each line, in OAM order
	uint8_t lineSpriteCount[SCREEN_HEIGHT];
	uint8_t lineSprites[SCREEN_HEIGHT][128];

	int nextY;
	int start;
	int end;

	// mGBA redraws a line only if something it saw changed; the frames depend on that
	uint8_t memoryChanged;
	uint32_t scanlineDirty[5];
	// OAM as the dirty lines last took it in
	// The OAM lines are drawn from. Sprites are fetched a line ahead of the one they show on, so a
	// change reaches the picture a line late: it waits in oamNext while the line under way is drawn.
	uint8_t oamSeen[0x400];
	uint8_t oamNext[0x400];
	bool oamPending;
	// The 2 KiB VRAM pages stores have changed (one byte each), those each line read when last drawn,
	// and the lines that read each page
	uint8_t vramChanged[48];
	uint64_t linePages[SCREEN_HEIGHT];
	uint32_t pageLines[48][5];
	uint16_t nextIo[0x30];
	struct {
		uint16_t io[0x30];
		int32_t scale[2][2];
		bool windowOn[2];
		uint16_t lineDispcnt;
	} cache[SCREEN_HEIGHT];

	uint16_t frame[SCREEN_HEIGHT][SCREEN_WIDTH];
	// Set by a frontend that takes frame in RGB565 instead of BGR555
	bool rgb565;
	// The render thread's copy, fed from a log: it has no say in the video timing, and a register
	// write comes with how far into the line it was made
	bool copy;
	int32_t copyCycle;
};

struct GBA;

void renderReset(struct GBA*);
// Picks the SIMD paths if the CPU has them; ARMGBA_RENDER=reference keeps to the scalar ones
void renderInit(void);
uint16_t renderWriteRegister(struct GBA*, uint32_t address, uint16_t value);
void renderScanline(struct GBA*, int y);
// At the start of each line: the affine BGs finish the last line and take on new reference points
void renderStartLine(struct GBA*, int y);
void renderLatchDispcnt(struct GBA*);
void renderFinishFrame(struct GBA*);

// render-thread.c: drawing on a second thread. Started, each call above is logged for it after it is
// made here, but for renderScanline, which only it makes; the frame is complete after renderFinishFrame.
// late: renderFinishFrame leaves the frame before the one it ends, so that drawing overlaps emulation
bool renderThreadStart(struct GBA*, bool late);
void renderThreadStop(struct GBA*);
void renderThreadClone(struct GBA*);
// The copy's renderer, with all that is logged drawn
const struct Renderer* renderThreadRenderer(struct GBA*);
void renderThreadRegister(struct GBA*, uint32_t address, uint16_t value);
void renderThreadScanline(struct GBA*, int y);
void renderThreadStartLine(struct GBA*, int y);
void renderThreadLatch(struct GBA*);
void renderThreadFinishFrame(struct GBA*);

#endif
