/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Save states: the machine as it stands between two instructions, for the same game in the same
// build. The state is struct GBA as it lies in memory. The parts that belong to the host rather
// than the machine (the game, the BIOS, the recompiler's bookkeeping, pointers) stay as they are on
// loading, and the event queue is stored as the order of its events.

#include "gba.h"
#include "cpu/jit-internal.h"

#include <stdlib.h>
#include <string.h>

enum {
	STATE_MAGIC = 0x3953424A,
	// The machine's events, as events() lists them.
	EVENTS = 12 + 4 + 12 + 7 + 4,
};

struct StateHeader {
	uint32_t magic;
	uint32_t size;
	// Zero: pointers into the machine are kept as offsets from its start.
	uint64_t base;
	// The queue, soonest first, as indices into events(); queued is how many.
	uint8_t queue[EVENTS];
	uint8_t queued;
	// Whether the video event is an H-blank's or a line's.
	bool hblank;
};

// Lists every event inside the machine in a fixed order, so a state can name them by number.
static void events(struct GBA* gba, struct Event* all[EVENTS]) {
	int n = 0;
	for (int i = 0; i < 4; ++i) {
		all[n++] = &gba->timers[i].event;
		all[n++] = &gba->timers[i].reloadWrite;
		all[n++] = &gba->timers[i].controlWrite;
		all[n++] = &gba->dma[i].start;
		all[n++] = &gba->irq.apply[i].event;
		all[n++] = &gba->irq.availableChange[i].event;
		all[n++] = &gba->irq.lineChange[i].event;
	}
	all[n++] = &gba->video.event;
	all[n++] = &gba->video.matchEvent;
	for (int i = 0; i < 3; ++i) {
		all[n++] = &gba->video.irqEvent[i];
	}
	all[n++] = &gba->video.latchEvent;
	all[n++] = &gba->video.captureEvent;
	all[n++] = &gba->audio.psg.frameEvent;
	all[n++] = &gba->savedata.dust;
	all[n++] = &gba->sio.completeEvent;
	all[n++] = &gba->sio.rfu.event;
}

// A state is a header and the whole struct GBA.
size_t gbaStateSize(void) {
	return sizeof(struct StateHeader) + sizeof(struct GBA);
}

// With a render thread, the lines drawn so far are in its copy of the renderer.
static void takeRenderer(struct GBA* gba, struct Renderer* out) {
	if (!gba->renderThread) {
		return;
	}
	const struct Renderer* drawn = renderThreadRenderer(gba);
	uint8_t memoryChanged = out->memoryChanged;
	uint8_t vramChanged[sizeof(out->vramChanged)];
	memcpy(vramChanged, out->vramChanged, sizeof(vramChanged));
	memcpy(out, drawn, sizeof(*out));
	out->memoryChanged = memoryChanged;
	memcpy(out->vramChanged, vramChanged, sizeof(vramChanged));
	out->copy = false;
}

// Copies the machine as it is. The header records where the struct was, so pointers into it can be
// moved when it is loaded, and which events were queued, by number.
// A state holds the machine alone, so the same machine makes the same state in any process and on
// any CPU: what loading takes from the host (the game, the BIOS, the recompiler's bookkeeping,
// pointers, the frontend's settings) is left as zero, the line the renderer builds is scratch, and
// pointers into the machine are kept as offsets from its start.
static void leaveOutHost(const struct GBA* gba, struct GBA* saved) {
	saved->romSlot = 0;
	saved->multibootSlot = 0;
	saved->cart.fileSlot = 0;
	saved->cart.fileSize = 0;
	saved->exitLinkSlot = 0;
	saved->siteReturnSlot = 0;
	saved->siteValid = false;
	saved->lookupSlot = 0;
	saved->hostStackSlot = 0;
	saved->jitRunning = false;
	memset(saved->jitScratch, 0, sizeof(saved->jitScratch));
	saved->jitDeferred = false;
	saved->jitLeave = false;
	saved->idleMark = 0;
	saved->idleCycles = 0;
	saved->idleRuns = 0;
	saved->idleNext = 0;
	saved->idleLead = 0;
	saved->idleStable = 0;
	saved->eventRuns = 0;
	saved->idleHeadRuns = 0;
	memset(saved->jitStopPenalty, 0, sizeof(saved->jitStopPenalty));
	saved->traceFileSlot = 0;
	saved->traceFromPc = 0;
	saved->traceFromTime = 0;
	saved->traceLeft = 0;
	saved->tracing = false;
	saved->jitSlot = 0;
	saved->renderThreadSlot = 0;
	saved->sio.linkSlot = 0;
	saved->sio.airSlot = 0;
	saved->sio.rfu.attached = false;
	saved->earlyExit = false;
	saved->returnRequested = false;
	saved->biosLoaded = false;
	memset(saved->bios, 0, sizeof(saved->bios));
	saved->gpio.clockSlot = 0;
	saved->gpio.clockContextSlot = 0;
	saved->savedata.backed = false;
	saved->renderer.rgb565 = false;
	saved->renderer.copy = false;
	memset(saved->iwramCode, 0, sizeof(saved->iwramCode));
	memset(saved->ewramCode, 0, sizeof(saved->ewramCode));
	saved->videoCode = false;
	saved->timing.rootSlot = 0;
	saved->timing.rerootSlot = 0;
	struct Event* all[EVENTS];
	events(saved, all);
	for (int i = 0; i < EVENTS; ++i) {
		all[i]->callbackSlot = 0;
		all[i]->contextSlot = 0;
		all[i]->nextSlot = 0;
	}
#if defined(__ARM_NEON)
	memset(saved->renderer.rowColor, 0, sizeof(saved->renderer.rowColor));
	memset(saved->renderer.rowFlags, 0, sizeof(saved->renderer.rowFlags));
#else
	memset(saved->renderer.row, 0, sizeof(saved->renderer.row));
#endif
	const uint8_t* base = (const uint8_t*) gba;
	saved->savedata.dataSlot = gba->savedata.data ? (uint64_t) (gba->savedata.data - base) : 0;
	saved->savedata.currentBankSlot = gba->savedata.currentBank ? (uint64_t) (gba->savedata.currentBank - base) : 0;
}

void gbaSaveState(struct GBA* gba, void* state) {
	struct StateHeader header = { .magic = STATE_MAGIC, .size = sizeof(struct GBA), .base = 0 };
	struct Event* all[EVENTS];
	events(gba, all);
	// Events from outside the machine, such as a frontend's, aren't part of it.
	for (struct Event* event = gba->timing.root; event; event = event->next) {
		for (int i = 0; i < EVENTS; ++i) {
			if (all[i] == event) {
				header.queue[header.queued++] = i;
				break;
			}
		}
	}
	header.hblank = videoEventIsHblank(gba);
	struct GBA* saved = (struct GBA*) ((uint8_t*) state + sizeof(header));
	memcpy(state, &header, sizeof(header));
	memcpy(saved, gba, sizeof(*gba));
	takeRenderer(gba, &saved->renderer);
	leaveOutHost(gba, saved);
}

// Converts a pointer into the saved struct GBA, kept as an offset from its start, into one for this
// struct.
static uint8_t* moved(struct GBA* gba, uint64_t offset) {
	return offset ? (uint8_t*) gba + offset : NULL;
}

// Whether compiled code covers a word that the state holds differently, or was compiled for other
// wait states.
static bool codeChanged(const struct GBA* gba, const struct GBA* saved) {
	if (gba->videoCode || gba->prefetchEnabled != saved->prefetchEnabled || memcmp(gba->waitN16, saved->waitN16, sizeof(gba->waitN16)) ||
	    memcmp(gba->waitS16, saved->waitS16, sizeof(gba->waitS16)) || memcmp(gba->waitN32, saved->waitN32, sizeof(gba->waitN32)) ||
	    memcmp(gba->waitS32, saved->waitS32, sizeof(gba->waitS32))) {
		return true;
	}
	for (uint32_t word = 0; word < SIZE_IWRAM / 4; ++word) {
		if (gba->iwramCode[word] && memcmp(&gba->iwram[word * 4], &saved->iwram[word * 4], 4)) {
			return true;
		}
	}
	for (uint32_t word = 0; word < SIZE_EWRAM / 4; ++word) {
		if (gba->ewramCode[word] && memcmp(&gba->ewram[word * 4], &saved->ewram[word * 4], 4)) {
			return true;
		}
	}
	return false;
}

// Loads a state. It is refused if the header isn't this version's. What belongs to the host, such
// as the ROM, the compiled code and the frontend's callbacks, is kept from the running machine.
bool gbaLoadState(struct GBA* gba, const void* state, size_t size) {
	struct StateHeader header;
	if (size < gbaStateSize()) {
		return false;
	}
	memcpy(&header, state, sizeof(header));
	if (header.magic != STATE_MAGIC || header.size != sizeof(struct GBA) || header.queued > EVENTS) {
		return false;
	}
	const struct GBA* saved = (const struct GBA*) ((const uint8_t*) state + sizeof(header));
	// A multi-game cartridge's size is that of the game showing.
	if ((saved->romSize != gba->romSize && !gba->cart.multi) || saved->multibootSize != gba->multibootSize) {
		return false;
	}
	bool flush = codeChanged(gba, saved);
	struct GBA* host = malloc(sizeof(*host));
	if (!host) {
		return false;
	}
	memcpy(host, gba, sizeof(*host));
	struct Event* all[EVENTS];
	struct Event* was[EVENTS];
	events(gba, all);
	events(host, was);
	memcpy(gba, saved, sizeof(*gba));

	// The host's side of it.
	gba->rom = host->rom;
	gba->multiboot = host->multiboot;
	gba->cart.file = host->cart.file;
	gba->cart.fileSize = host->cart.fileSize;
	gba->exitLink = host->exitLink;
	gba->siteReturn = host->siteReturn;
	gba->siteValid = false;
	gba->lookup = host->lookup;
	gba->hostStack = host->hostStack;
	gba->jitRunning = false;
	gba->jitDeferred = false;
	gba->jitLeave = false;
	gba->idleMark = 0;
	gba->eventRuns = host->eventRuns;
	gba->idleHeadRuns = host->idleHeadRuns;
	memcpy(gba->jitStopPenalty, host->jitStopPenalty, sizeof(gba->jitStopPenalty));
	gba->traceFile = host->traceFile;
	gba->traceFromPc = host->traceFromPc;
	gba->traceFromTime = host->traceFromTime;
	gba->traceLeft = host->traceLeft;
	gba->tracing = host->tracing;
	gba->jit = host->jit;
	gba->renderThread = host->renderThread;
	gba->sio.link = host->sio.link;
	gba->sio.air = host->sio.air;
	gba->sio.rfu.attached = host->sio.rfu.attached;
	gba->earlyExit = false;
	gba->returnRequested = false;
	gba->biosLoaded = host->biosLoaded;
	memcpy(gba->bios, host->bios, sizeof(gba->bios));
	gba->gpio.clock = host->gpio.clock;
	gba->gpio.clockContext = host->gpio.clockContext;
	gba->savedata.backed = host->savedata.backed;
	gba->savedata.data = moved(gba, saved->savedata.dataSlot);
	gba->savedata.currentBank = moved(gba, saved->savedata.currentBankSlot);
	gba->renderer.rgb565 = host->renderer.rgb565;
	gba->renderer.copy = false;
	memcpy(gba->iwramCode, host->iwramCode, sizeof(gba->iwramCode));
	memcpy(gba->ewramCode, host->ewramCode, sizeof(gba->ewramCode));
	gba->videoCode = host->videoCode;

	// The events: what they call is the host's; when they come and in what order is the state's.
	for (int i = 0; i < EVENTS; ++i) {
		all[i]->callback = was[i]->callback;
		all[i]->context = was[i]->context;
		all[i]->next = NULL;
	}
	videoEventRestore(gba, header.hblank);
	gba->timing.root = NULL;
	gba->timing.reroot = NULL;
	struct Event** link = &gba->timing.root;
	for (int i = 0; i < header.queued; ++i) {
		*link = all[header.queue[i]];
		link = &(*link)->next;
	}
	timingResetNextEvent(gba);
	free(host);

	jitStateLoaded(gba, flush);
	cartRestore(gba);
	if (gba->renderThread) {
		renderThreadClone(gba);
	}
	return true;
}
