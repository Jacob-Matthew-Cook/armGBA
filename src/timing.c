/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <limits.h>

// The same ordering rules as mGBA's mTiming: events due at the same time run in priority order.

// No events, and the clock at zero.
void timingClear(struct GBA* gba) {
	gba->timing.root = NULL;
	gba->timing.reroot = NULL;
	gba->timing.masterCycles = 0;
	gba->timing.globalCycles = 0;
}

// Events are a list in time order, and by priority where times are equal. when is in cycles from
// now.
void timingSchedule(struct GBA* gba, struct Event* event, int32_t when) {
	struct Timing* timing = &gba->timing;
	int32_t nextEvent = when + gba->cpu.cycles;
	event->when = nextEvent + timing->masterCycles;
	if (nextEvent < gba->cpu.nextEvent) {
		gba->cpu.nextEvent = nextEvent;
	}
	struct Event** previous = timing->reroot ? &timing->reroot : &timing->root;
	struct Event* next = *previous;
	while (next) {
		int32_t nextWhen = next->when - timing->masterCycles;
		if (nextWhen > nextEvent || (nextWhen == nextEvent && next->priority > event->priority)) {
			break;
		}
		previous = &next->next;
		next = next->next;
	}
	event->next = next;
	*previous = event;
}

// The same, with when on the clock itself.
void timingScheduleAbsolute(struct GBA* gba, struct Event* event, int32_t when) {
	timingSchedule(gba, event, when - timingCurrentTime(gba));
}

// Takes an event off the list if it is on it.
void timingDeschedule(struct GBA* gba, struct Event* event) {
	struct Timing* timing = &gba->timing;
	struct Event** previous = timing->reroot ? &timing->reroot : &timing->root;
	struct Event* next = *previous;
	while (next) {
		if (next == event) {
			*previous = next->next;
			return;
		}
		previous = &next->next;
		next = next->next;
	}
}

// Whether an event is waiting.
bool timingIsScheduled(struct GBA* gba, struct Event* event) {
	struct Event* next = gba->timing.root ? gba->timing.root : gba->timing.reroot;
	for (; next; next = next->next) {
		if (next == event) {
			return true;
		}
	}
	return false;
}

// Returns the cycles until the first event, as the CPU's cycle counter counts them.
int32_t timingNextEvent(struct GBA* gba) {
	struct Event* next = gba->timing.root;
	if (!next) {
		return INT_MAX;
	}
	return next->when - gba->timing.masterCycles - gba->cpu.cycles;
}

// Returns the cycles from now until a scheduled event.
int32_t timingUntil(struct GBA* gba, struct Event* event) {
	return event->when - gba->timing.masterCycles - gba->cpu.cycles;
}

// Makes the event loop return after the event running now, keeping the rest for the next call.
void timingInterrupt(struct GBA* gba) {
	if (!gba->timing.root) {
		return;
	}
	gba->timing.reroot = gba->timing.root;
	gba->timing.root = NULL;
}

// Moves the clock on by cycles and runs every event due, each told how late it is. Returns the
// cycles until the next one.
int32_t timingTick(struct GBA* gba, int32_t cycles) {
	struct Timing* timing = &gba->timing;
	timing->masterCycles += cycles;
	timing->globalCycles += cycles;
	uint32_t masterCycles = timing->masterCycles;
	while (timing->root) {
		struct Event* next = timing->root;
		int32_t nextWhen = next->when - masterCycles;
		if (nextWhen > 0) {
			return nextWhen;
		}
		timing->root = next->next;
		next->callback(gba, next->context, -nextWhen);
	}
	if (timing->reroot) {
		timing->root = timing->reroot;
		timing->reroot = NULL;
		gba->cpu.nextEvent = timingNextEvent(gba);
	}
	return gba->cpu.nextEvent;
}

bool timingDeferrable(struct GBA* gba, int32_t time) {
	for (struct Event* event = gba->timing.root; event; event = event->next) {
		if ((int32_t) (event->when - gba->timing.masterCycles) > time) {
			break;
		}
		if (event->endsRun || dmaStartEvent(event) || !videoDeferrable(gba, event) || !timerDeferrable(gba, event)) {
			return false;
		}
	}
	return true;
}

// After compiled code moved nextEvent up to leave early.
void timingResetNextEvent(struct GBA* gba) {
	struct Event* next = gba->timing.root;
	gba->cpu.nextEvent = next ? (int32_t) (next->when - gba->timing.masterCycles) : INT_MAX;
}

// Called when the CPU's cycle counter reaches nextEvent: the counter is folded into the clock, the
// due events run, and a halted CPU is moved straight to the next event.
void processEvents(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	++gba->eventRuns;
	int32_t nextEvent = cpu->nextEvent;
	while (cpu->cycles >= nextEvent) {
		cpu->nextEvent = INT_MAX;
		nextEvent = 0;
		int32_t cycles = cpu->cycles;
		cpu->cycles = 0;
		nextEvent = timingTick(gba, cycles < nextEvent ? nextEvent : cycles);
		cpu->nextEvent = nextEvent;
		// While halted, the CPU sleeps until IE & IF, whatever IME and the I flag say; waking takes
		// a cycle. DMA still runs. A sleeping CPU moves on to the next event, unless an event asked
		// for control back: the console then stops where that event ran.
		if (cpu->halted && !gba->performingDma) {
			if (gba->irq.available) {
				cpu->halted = 0;
				cpu->cycles += 1;
			} else if (gba->dmaRunnable) {
				dmaRun(gba);
			} else if (!gba->returnRequested) {
				cpu->cycles = nextEvent;
			}
		}
		if (gba->earlyExit || gba->returnRequested) {
			// Asked for control back: compiled code looks at events again at once, and leaves.
			if (gba->returnRequested) {
				cpu->nextEvent = cpu->cycles;
			}
			break;
		}
	}
	gba->earlyExit = false;
}
