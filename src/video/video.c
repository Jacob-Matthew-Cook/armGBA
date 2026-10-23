/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// Scanline timing and the registers that depend on it; drawing lives elsewhere.

enum {
	HBLANK_START = 1007,
	LINE_LENGTH = 1232,
	VISIBLE_LINES = 160,
	TOTAL_LINES = 228,
};

static void startHblank(struct GBA*, void*, uint32_t);
static void lineEvent(struct GBA*, void*, uint32_t);

static const int IRQ_OF_EVENT[3] = { IRQ_VBLANK, IRQ_HBLANK, IRQ_VCOUNTER };

// The V-blank, H-blank and line-match interrupts are requested one cycle after what causes them.
static void requestIrq(struct GBA* gba, void* context, uint32_t late) {
	gbaRaiseIrq(gba, IRQ_OF_EVENT[(intptr_t) context], late);
}

static void scheduleIrq(struct GBA* gba, int which, uint32_t late) {
	timingDeschedule(gba, &gba->video.irqEvent[which]);
	timingSchedule(gba, &gba->video.irqEvent[which], 1 - (int32_t) late);
}

// DISPSTAT's match flag: set while VCOUNT equals the line a game chose, with an interrupt when it
// sets.
static void updateMatch(struct GBA* gba, uint32_t late) {
	uint16_t dispstat = gba->io[IO(REG_DISPSTAT)];
	bool match = gba->video.vcount == (dispstat >> 8);
	if (match && !(dispstat & 0x0004) && (dispstat & 0x0020)) {
		scheduleIrq(gba, 2, late);
	}
	gba->io[IO(REG_DISPSTAT)] = match ? dispstat | 0x0004 : dispstat & ~0x0004;
}

// Lines start when something looks at them: the line, VCOUNT and DISPSTAT are brought up to date
// then. Only the frame's first line (for the keypad IRQ test) and V-blank's first start as events,
// and H-blank is one on the visible lines, which are drawn then, and on every line while its IRQ is
// on. What happens a few cycles into some lines is scheduled ahead from the line starts before it.

static uint32_t nextLineStart(struct GBA* gba) {
	return gba->video.lineStart + LINE_LENGTH;
}

// A visible line is drawn as at its H-blank: then, or later but before anything it shows changes.
static void drawLine(struct GBA* gba) {
	struct Video* video = &gba->video;
	if (!video->drawn && video->vcount < VISIBLE_LINES) {
		video->drawn = true;
		if (gba->renderThread) {
			renderThreadScanline(gba, video->vcount);
		} else {
			renderScanline(gba, video->vcount);
		}
	}
}

// Returns when the next line to draw is due. Compiled code's video stores check this, so a line
// whose H-blank has passed is drawn before what it shows changes.
static void planDraw(struct GBA* gba) {
	struct Video* video = &gba->video;
	int vcount = video->vcount;
	if (vcount < VISIBLE_LINES && !video->drawn) {
		video->drawDue = video->lineStart + HBLANK_START;
	} else if (vcount + 1 < VISIBLE_LINES) {
		video->drawDue = video->lineStart + LINE_LENGTH + HBLANK_START;
	} else {
		video->drawDue = video->lineStart + (TOTAL_LINES - vcount) * LINE_LENGTH + HBLANK_START;
	}
}

// Starts a line, worked out when something first needs it: VCOUNT, the flags, V-blank's interrupt
// and DMA, and the frame's end.
static void startLine(struct GBA* gba, uint32_t late) {
	struct Video* video = &gba->video;
	drawLine(gba);
	video->drawn = false;
	video->lineStart += LINE_LENGTH;
	++video->vcount;
	if (video->vcount == TOTAL_LINES) {
		video->vcount = 0;
	}
	int vcount = video->vcount;
	gba->io[IO(REG_VCOUNT)] = vcount;
	renderStartLine(gba, vcount);
	if (gba->renderThread) {
		renderThreadStartLine(gba, vcount);
	}

	uint16_t dispstat = gba->io[IO(REG_DISPSTAT)] & ~0x0002;
	switch (vcount) {
	case 0:
		gbaFrameStarted(gba, late);
		break;
	case VISIBLE_LINES:
		dispstat |= 0x0001;
		renderFinishFrame(gba);
		if (gba->renderThread) {
			renderThreadFinishFrame(gba);
		}
		dmaRequestVblank(gba, late);
		if (dispstat & 0x0008) {
			scheduleIrq(gba, 0, late);
		}
		gba->io[IO(REG_DISPSTAT)] = dispstat;
		gbaFrameEnded(gba);
		++video->frameCounter;
		gbaInterrupt(gba);
		return;
	case TOTAL_LINES - 1:
		dispstat &= ~0x0001;
		break;
	}
	gba->io[IO(REG_DISPSTAT)] = dispstat;
}

// Brings the line, VCOUNT and DISPSTAT up to now. stableUntil is the time until which compiled code
// can read the two registers as they stand.
void videoSync(struct GBA* gba) {
	struct Video* video = &gba->video;
	uint32_t now = timingCurrentTime(gba);
	while ((int32_t) (now - nextLineStart(gba)) >= 0) {
		startLine(gba, now - nextLineStart(gba));
	}
	if ((int32_t) (now - video->lineStart) >= HBLANK_START) {
		gba->io[IO(REG_DISPSTAT)] |= 0x0002;
		video->stableUntil = nextLineStart(gba);
		drawLine(gba);
	} else {
		video->stableUntil = video->lineStart + HBLANK_START;
	}
	planDraw(gba);
}

// Returns the line the display is on now.
int videoLine(struct GBA* gba) {
	videoSync(gba);
	return gba->video.vcount;
}

// H-blank is an event only for what can't wait: H-blank DMA on the visible lines, and its IRQ where
// IE lets it through.
static bool hblankEvent(struct GBA* gba, int line) {
	return (line < VISIBLE_LINES && dmaTimingArmed(gba, 2)) || ((gba->io[IO(REG_DISPSTAT)] & 0x0010) && (gba->irq.ie & (1 << IRQ_HBLANK)));
}

// Returns the first H-blank at from or later, before or after the line video last came up to.
uint32_t videoNextHblank(struct GBA* gba, uint32_t from) {
	uint32_t hblank = gba->video.lineStart + HBLANK_START;
	int32_t ahead = from - hblank;
	if (ahead > 0) {
		hblank += (ahead + LINE_LENGTH - 1) / LINE_LENGTH * LINE_LENGTH;
	} else {
		hblank -= -ahead / LINE_LENGTH * LINE_LENGTH;
	}
	return hblank;
}

// The two line starts that are always events: the frame's first and V-blank's.
static bool lineStartEvent(int line) {
	return line == 0 || line == VISIBLE_LINES;
}

// Returns the video event after time from (synced): the next H-blank or line start that does
// something then. What H-blank does is the same on every visible line and on every V-blank line, so
// the event is found without going line by line.
static void scheduleVideo(struct GBA* gba, uint32_t from) {
	struct Video* video = &gba->video;
	int line = video->vcount;
	uint32_t start = video->lineStart;
	timingDeschedule(gba, &video->event);
	bool visibleHblank = hblankEvent(gba, 0);
	bool blankHblank = hblankEvent(gba, VISIBLE_LINES);
	if ((int32_t) (from - start) < HBLANK_START && (line < VISIBLE_LINES ? visibleHblank : blankHblank)) {
		video->event.callback = startHblank;
		video->event.endsRun = false;
		video->eventLine = line;
		timingScheduleAbsolute(gba, &video->event, start + HBLANK_START);
		return;
	}
	// The next line, then on to the next line start or H-blank that is an event.
	int next = line + 1 == TOTAL_LINES ? 0 : line + 1;
	start += LINE_LENGTH;
	int target;
	bool hblank;
	if (lineStartEvent(next)) {
		target = next;
		hblank = false;
	} else if (next < VISIBLE_LINES ? visibleHblank : blankHblank) {
		target = next;
		hblank = true;
	} else {
		// Nothing until V-blank's line or the frame's first.
		target = next < VISIBLE_LINES ? VISIBLE_LINES : 0;
		hblank = false;
	}
	int lines = target - next;
	if (lines < 0) {
		lines += TOTAL_LINES;
	}
	start += lines * LINE_LENGTH;
	video->eventLine = target;
	if (hblank) {
		video->event.callback = startHblank;
		video->event.endsRun = false;
		timingScheduleAbsolute(gba, &video->event, start + HBLANK_START);
	} else {
		video->event.callback = lineEvent;
		// The line that starts V-blank ends the frame.
		video->event.endsRun = target == VISIBLE_LINES;
		timingScheduleAbsolute(gba, &video->event, start);
	}
}

// Returns the next time, after now (or from the next line), that a line in lines starts offset
// cycles in.
static void scheduleAhead(struct GBA* gba, struct Event* event, int32_t offset, bool (*lines)(struct GBA*, int), bool thisLine) {
	struct Video* video = &gba->video;
	uint32_t now = timingCurrentTime(gba);
	int line = video->vcount;
	uint32_t start = video->lineStart;
	timingDeschedule(gba, event);
	for (int i = 0; i <= TOTAL_LINES; ++i) {
		if ((i || thisLine) && (int32_t) (start + offset - now) > 0 && lines(gba, line)) {
			timingScheduleAbsolute(gba, event, start + offset);
			return;
		}
		start += LINE_LENGTH;
		line = line + 1 == TOTAL_LINES ? 0 : line + 1;
	}
}

// The VCOUNT flag changes one cycle into a line, only on the line LYC names and the one after.
static bool matchLine(struct GBA* gba, int line) {
	int lyc = gba->io[IO(REG_DISPSTAT)] >> 8;
	return line == lyc || line == (lyc + 1) % TOTAL_LINES;
}

// One cycle into a line, the match flag follows the new VCOUNT.
static void matchEvent(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	videoSync(gba);
	updateMatch(gba, late);
	scheduleAhead(gba, &gba->video.matchEvent, 1, matchLine, false);
}

// The latch and capture events run only where they can change something: a latch while DISPCNT
// differs from what was latched, a capture while DMA3 waits for one.
static bool latchLine(struct GBA* gba, int vcount) {
	(void) gba;
	return vcount <= VISIBLE_LINES || vcount >= TOTAL_LINES - 3;
}

// Whether latching DISPCNT again would change nothing.
static bool latchSettled(struct GBA* gba) {
	const uint16_t* latch = gba->renderer.dispcntLatch;
	uint16_t dispcnt = gba->renderer.dispcnt;
	return latch[0] == dispcnt && latch[1] == dispcnt && latch[2] == dispcnt;
}

// The renderer's DISPCNT latch, 40 cycles into a line, for as long as it has something to take in.
static void scheduleLatch(struct GBA* gba, bool thisLine) {
	if (latchSettled(gba)) {
		return;
	}
	if (!thisLine || !timingIsScheduled(gba, &gba->video.latchEvent)) {
		scheduleAhead(gba, &gba->video.latchEvent, 40, latchLine, thisLine);
	}
}

// Runs a latch, on one thread or both.
static void latchEvent(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	(void) late;
	videoSync(gba);
	renderLatchDispcnt(gba);
	if (gba->renderThread) {
		renderThreadLatch(gba);
	}
	scheduleLatch(gba, false);
}

// Video capture DMA runs on lines 2 to 161, three cycles in.
static bool captureLine(struct GBA* gba, int vcount) {
	(void) gba;
	return vcount >= 2 && vcount < VISIBLE_LINES + 2;
}

// Whether channel 3 is on and waiting for capture.
static bool captureArmed(struct GBA* gba) {
	uint16_t reg = gba->dma[3].reg;
	return (reg & 0x8000) && ((reg >> 12) & 3) == 3;
}

static void scheduleCapture(struct GBA* gba, bool thisLine) {
	if (!captureArmed(gba)) {
		return;
	}
	if (!thisLine || !timingIsScheduled(gba, &gba->video.captureEvent)) {
		scheduleAhead(gba, &gba->video.captureEvent, 3, captureLine, thisLine);
	}
}

static void captureEvent(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	videoSync(gba);
	dmaRequestDisplayStart(gba, late);
	scheduleCapture(gba, false);
}

// Whether a video event can run after the instruction it falls in: not one that starts DMA.
bool videoDeferrable(struct GBA* gba, const struct Event* event) {
	struct Video* video = &gba->video;
	if (event == &video->captureEvent) {
		return false;
	}
	// H-blank asks H-blank DMA to start.
	return event != &video->event || event->callback != startHblank || video->eventLine >= VISIBLE_LINES || !dmaTimingArmed(gba, 2);
}

// For save states: which of its two callbacks the video event has.
bool videoEventIsHblank(struct GBA* gba) {
	return gba->video.event.callback == startHblank;
}

// A loaded state says which of its two callbacks the video event had.
void videoEventRestore(struct GBA* gba, bool hblank) {
	gba->video.event.callback = hblank ? startHblank : lineEvent;
}

// Called when DISPCNT changes, so there is something to latch.
void videoDispcntWritten(struct GBA* gba) {
	scheduleLatch(gba, true);
}

// In mode 2 with both backgrounds on, the display fetches from VRAM all through a visible line's
// drawing, and the CPU's access waits for H-blank, as in mGBA. Returns the cycles to add to an
// access that has already waited `wait`.
int32_t videoVramStall(struct GBA* gba, int32_t wait) {
	struct Video* video = &gba->video;
	videoSync(gba);
	if (video->vcount >= VISIBLE_LINES) {
		return 0;
	}
	int32_t until = (int32_t) (video->lineStart + HBLANK_START - (uint32_t) timingCurrentTime(gba)) - wait;
	return until > 0 ? until : 0;
}

// Called when channel 3 is set up for capture.
void videoCaptureArmed(struct GBA* gba) {
	videoSync(gba);
	scheduleCapture(gba, true);
}

// The video events and their priorities.
void videoInit(struct GBA* gba) {
	struct Video* video = &gba->video;
	video->event = (struct Event) { .priority = 8 };
	video->matchEvent = (struct Event) { .callback = matchEvent, .priority = 8 };
	for (int i = 0; i < 3; ++i) {
		video->irqEvent[i] = (struct Event) { .callback = requestIrq, .context = (void*) (intptr_t) i, .priority = 8 };
	}
	video->latchEvent = (struct Event) { .callback = latchEvent, .priority = 8 };
	video->captureEvent = (struct Event) { .callback = captureEvent, .priority = 8 };
}

// Partway through line vcount, with its H-blank this many cycles away.
void videoStartAt(struct GBA* gba, int vcount, int32_t untilHblank) {
	struct Video* video = &gba->video;
	video->vcount = vcount;
	gba->io[IO(REG_VCOUNT)] = vcount;
	video->lineStart = timingCurrentTime(gba) + untilHblank - HBLANK_START;
	video->stableUntil = video->lineStart + HBLANK_START;
	video->drawn = false;
	planDraw(gba);
	scheduleVideo(gba, timingCurrentTime(gba));
	scheduleAhead(gba, &video->matchEvent, 1, matchLine, false);
	// The renderer starts with DISPCNT not yet latched.
	timingDeschedule(gba, &video->latchEvent);
	scheduleLatch(gba, false);
}

// Clears video memory and puts the display at the point of the frame the hardware, or the BIOS,
// leaves it.
void videoReset(struct GBA* gba) {
	gba->vramStall = false;
	struct Video* video = &gba->video;
	timingDeschedule(gba, &video->captureEvent);
	video->frameCounter = 0;
	memset(gba->palette, 0, sizeof(gba->palette));
	memset(gba->oam, 0, sizeof(gba->oam));
	memset(gba->vram, 0, sizeof(gba->vram));
	renderReset(gba);
	if (gba->renderThread) {
		renderThreadClone(gba);
	}
	if (gba->biosLoaded) {
		videoStartAt(gba, 0, HBLANK_START);
	} else {
		videoStartAt(gba, 0x7E, 117);
	}
}

// A line start that is an event.
static void lineEvent(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	videoSync(gba);
	scheduleVideo(gba, timingCurrentTime(gba) - late);
}

// H-blank as an event: the flag, H-blank DMA on the visible lines, and the interrupt.
static void startHblank(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	struct Video* video = &gba->video;
	videoSync(gba);
	audioCatchUp(gba, timingCurrentTime(gba) - late, false);
	uint16_t dispstat = gba->io[IO(REG_DISPSTAT)] | 0x0002;
	gba->io[IO(REG_DISPSTAT)] = dispstat;
	if (video->vcount < VISIBLE_LINES) {
		dmaRequestHblank(gba, late);
	}
	if ((dispstat & 0x0010) && (gba->irq.ie & (1 << IRQ_HBLANK))) {
		scheduleIrq(gba, 1, late);
	}
	scheduleVideo(gba, timingCurrentTime(gba) - late);
}

// Called when what makes H-blank an event changed: the next video event is planned again.
void videoHblankDmaArmed(struct GBA* gba) {
	videoSync(gba);
	scheduleVideo(gba, timingCurrentTime(gba));
}

// DISPSTAT: the interrupt enables and the match line; the three status bits are read-only.
uint16_t videoWriteDispstat(struct GBA* gba, uint16_t value) {
	videoSync(gba);
	gbaIrqSync(gba);
	bool wasQuiet = gbaQuietHblank(gba);
	uint16_t old = gba->io[IO(REG_DISPSTAT)];
	gba->io[IO(REG_DISPSTAT)] = (old & 0x7) | (value & 0xFFF8);
	gbaIrqQuietChanged(gba, wasQuiet);
	updateMatch(gba, 0);
	if ((old ^ value) & 0xFF00) {
		scheduleAhead(gba, &gba->video.matchEvent, 1, matchLine, true);
	}
	if ((old ^ value) & 0x0010) {
		// H-blank events on every line, or only on the visible ones.
		scheduleVideo(gba, timingCurrentTime(gba));
	}
	return gba->io[IO(REG_DISPSTAT)];
}
