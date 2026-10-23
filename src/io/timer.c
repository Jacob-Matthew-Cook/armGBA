/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// The four timers: overflows as events, or counted later where only sound FIFOs and cascades see
// them.

static const int PRESCALE_BITS[4] = { 0, 6, 8, 10 };

// Counting resumes `offset` cycles into the current prescaler period; `now` is when that happens.
static void start(struct GBA* gba, struct Timer* timer, uint32_t now, int32_t offset, uint32_t late) {
	int32_t cycles = ((0x10000 - timer->counter) << timer->prescaleBits) - offset;
	timer->quiet = 0;
	timer->running = true;
	timer->started = now - offset;
	timingDeschedule(gba, &timer->event);
	timingSchedule(gba, &timer->event, cycles - (int32_t) late);
}

// Returns the cycles between overflows with the reload value as it stands.
static int32_t period(const struct Timer* timer) {
	return (0x10000 - timer->reload) << timer->prescaleBits;
}

// Whether timer id clocks a sound FIFO that is switched on.
static bool feeds(struct GBA* gba, int fifo, int id) {
	struct Audio* audio = &gba->audio;
	if (!audio->enable || id >= 2) {
		return false;
	}
	return fifo ? (audio->chBLeft || audio->chBRight) && audio->chBTimer == id : (audio->chALeft || audio->chARight) && audio->chATimer == id;
}

// An overflow at time: each FIFO it clocks puts out its next sample.
static void feedFifos(struct GBA* gba, int id, uint32_t time) {
	for (int fifo = 0; fifo < 2; ++fifo) {
		if (feeds(gba, fifo, id)) {
			audioSampleFifo(gba, fifo, time);
		}
	}
}

// Overflows a timer: it reloads, requests its interrupt, clocks the FIFOs, and ticks the next timer
// if that one counts overflows.
static void overflow(struct GBA* gba, int id, uint32_t late) {
	struct Timer* timer = &gba->timers[id];
	timer->counter = timer->reload;
	if (timer->doIrq) {
		gbaRaiseIrq(gba, IRQ_TIMER0 + id, late);
	}
	feedFifos(gba, id, timingCurrentTime(gba) - late);
	if (id < 3) {
		struct Timer* next = &gba->timers[id + 1];
		if (next->enable && next->countUp && ++next->counter == 0x10000) {
			overflow(gba, id + 1, late);
		}
	}
}

enum {
	QUIET_MAX = 64,
};

// Whether the timer after this one counts its overflows.
static bool cascades(struct GBA* gba, int id) {
	return id < 3 && gba->timers[id + 1].enable && gba->timers[id + 1].countUp;
}

// Returns how many overflows after the current one only feed the FIFOs, with none requesting DMA.
// It follows the FIFOs as they empty, since a refill means planning again.
static int quietOverflows(struct GBA* gba, int id) {
	if (gba->timers[id].doIrq) {
		return 0;
	}
	int limit = QUIET_MAX;
	if (cascades(gba, id)) {
		// Ticking the timer after it is quiet too, up to the tick that overflows it.
		int ticks = 0xFFFF - (int) gba->timers[id + 1].counter;
		if (ticks < limit) {
			limit = ticks;
		}
	}
	struct {
		int size;
		int remaining;
		bool asks;
	} fifos[2];
	int count = 0;
	for (int fifo = 0; fifo < 2; ++fifo) {
		if (!feeds(gba, fifo, id)) {
			continue;
		}
		struct Fifo* channel = fifo ? &gba->audio.chB : &gba->audio.chA;
		int size = channel->fifoWrite - channel->fifoRead;
		fifos[count].size = size < 0 ? size + AUDIO_FIFO_SIZE : size;
		fifos[count].remaining = channel->internalRemaining;
		fifos[count].asks = channel->dmaSource > 0 && dmaFifoArmed(gba, channel->dmaSource);
		++count;
	}
	for (int n = 1; n <= limit; ++n) {
		for (int i = 0; i < count; ++i) {
			if (fifos[i].asks && AUDIO_FIFO_SIZE - fifos[i].size > 4) {
				return n - 1;
			}
			if (!fifos[i].remaining && fifos[i].size) {
				--fifos[i].size;
				fifos[i].remaining = 4;
			}
			if (fifos[i].remaining) {
				--fifos[i].remaining;
			}
		}
	}
	return limit;
}

// Runs a timer's quiet overflows up to time; inclusive says whether one exactly at time counts.
static void catchUp(struct GBA* gba, int id, uint32_t time, bool inclusive) {
	struct Timer* timer = &gba->timers[id];
	while (timer->quiet) {
		uint32_t next = timer->started + period(timer);
		int32_t until = (int32_t) (next - time);
		if (inclusive ? until > 0 : until >= 0) {
			break;
		}
		--timer->quiet;
		timer->started = next;
		feedFifos(gba, id, next);
		if (cascades(gba, id)) {
			++gba->timers[id + 1].counter;
		}
	}
}

// Whether a timer event can run after the instruction it falls in instead of inside it: not if it
// can start DMA, which an overflow asking for more FIFO data does.
bool timerDeferrable(struct GBA* gba, const struct Event* event) {
	for (int id = 0; id < 4; ++id) {
		struct Timer* timer = &gba->timers[id];
		if (event != &timer->event && event != &timer->controlWrite && event != &timer->reloadWrite) {
			continue;
		}
		// An overflow can ask a FIFO's channel for more data.
		for (int fifo = 0; fifo < 2; ++fifo) {
			struct Fifo* channel = fifo ? &gba->audio.chB : &gba->audio.chA;
			for (int t = id; t < 4; ++t) {
				if (feeds(gba, fifo, t) && channel->dmaSource > 0 && dmaFifoArmed(gba, channel->dmaSource)) {
					return false;
				}
				// When cascaded, it ticks the timers after it.
				if (t + 1 < 4 && !(gba->timers[t + 1].enable && gba->timers[t + 1].countUp)) {
					break;
				}
			}
		}
		return true;
	}
	return true;
}

// Runs all four timers' quiet overflows up to time.
void timerCatchUp(struct GBA* gba, uint32_t time, bool inclusive) {
	for (int id = 0; id < 4; ++id) {
		catchUp(gba, id, time, inclusive);
	}
}

// Called when something the quiet overflows were planned around is about to change: they are caught
// up, and the timers go back to an event at their next overflow.
void timerReplan(struct GBA* gba, uint32_t time) {
	for (int id = 0; id < 4; ++id) {
		struct Timer* timer = &gba->timers[id];
		catchUp(gba, id, time, true);
		if (timer->quiet) {
			timer->quiet = 0;
			timingDeschedule(gba, &timer->event);
			timingSchedule(gba, &timer->event, (int32_t) (timer->started + period(timer) - timingCurrentTime(gba)));
		}
	}
}

// Stops a running timer, keeping its count so far.
static void stop(struct GBA* gba, int id, uint32_t now, uint32_t late) {
	struct Timer* timer = &gba->timers[id];
	timer->counter += (now - timer->started) >> timer->prescaleBits;
	if (timer->counter >= 0x10000) {
		overflow(gba, id, late);
	}
	timingDeschedule(gba, &timer->event);
	timer->running = false;
}

// The event at an overflow that isn't quiet. Afterwards, as many of the following ones as are quiet
// are skipped over: the next event is the first that isn't.
static void overflowEvent(struct GBA* gba, void* context, uint32_t late) {
	int id = (int) (intptr_t) context;
	struct Timer* timer = &gba->timers[id];
	uint32_t now = timingCurrentTime(gba) - late;
	catchUp(gba, id, now, false);
	overflow(gba, id, late);
	start(gba, timer, now, 0, late);
	int quiet = quietOverflows(gba, id);
	if (quiet) {
		timer->quiet = quiet;
		timingDeschedule(gba, &timer->event);
		timingSchedule(gba, &timer->event, (quiet + 1) * period(timer) - (int32_t) late);
	}
}

// A reload value reaches the timer one cycle after the write; it is used from the next overflow.
static void reloadWritten(struct GBA* gba, void* context, uint32_t late) {
	timerReplan(gba, timingCurrentTime(gba) - late);
	struct Timer* timer = &gba->timers[(intptr_t) context];
	timer->reload = timer->pendingReload;
}

// The control register, one cycle after the write: prescaler, cascade, interrupt, start and stop.
static void controlWritten(struct GBA* gba, void* context, uint32_t late) {
	int id = (int) (intptr_t) context;
	struct Timer* timer = &gba->timers[id];
	uint32_t now = timingCurrentTime(gba) - late;
	timerReplan(gba, now);
	uint16_t control = timer->pendingControl;
	bool wasEnabled = timer->enable;
	if (timer->running) {
		stop(gba, id, now, late);
	}
	timer->prescaleBits = PRESCALE_BITS[control & 3];
	timer->doIrq = control & 0x0040;
	timer->enable = control & 0x0080;
	if (id) {
		timer->countUp = control & 0x0004;
	}
	gba->io[IO(REG_TM0CNT_LO + 2 + (id << 2))] = control;
	if (!timer->enable) {
		return;
	}
	// The prescaler runs off the system clock, whatever the timer does.
	int32_t offset = now & ((1 << timer->prescaleBits) - 1);
	if (wasEnabled) {
		if (!timer->countUp) {
			start(gba, timer, now, offset, late);
		}
	} else if (timer->countUp) {
		timer->counter = timer->reload;
	} else if (timer->counter == 0xFFFF && !offset) {
		// Loading the reload value takes a cycle, in which the old count can still tick over.
		start(gba, timer, now, 0, late);
	} else {
		timer->counter = timer->reload;
		start(gba, timer, now, offset - 1, late);
	}
}

void timerInit(struct GBA* gba) {
	memset(gba->timers, 0, sizeof(gba->timers));
	for (int i = 0; i < 4; ++i) {
		struct Timer* timer = &gba->timers[i];
		timer->event = (struct Event) { .callback = overflowEvent, .context = (void*) (intptr_t) i, .priority = 0x20 + i };
		timer->reloadWrite = (struct Event) { .callback = reloadWritten, .context = (void*) (intptr_t) i, .priority = 0x28 + i };
		timer->controlWrite = (struct Event) { .callback = controlWritten, .context = (void*) (intptr_t) i, .priority = 0x2C + i };
	}
}

// Returns the counter as it reads now, worked out from when the timer was started rather than
// counted.
uint16_t timerReadCounter(struct GBA* gba, int id) {
	struct Timer* timer = &gba->timers[id];
	// The timers before it may have ticks waiting for it.
	for (int i = 0; i <= id; ++i) {
		catchUp(gba, i, timingCurrentTime(gba), true);
	}
	uint32_t counter = timer->counter;
	if (timer->running) {
		counter += (timingCurrentTime(gba) - timer->started) >> timer->prescaleBits;
	}
	return counter;
}

// Writes to a timer take effect one cycle later, so they are events.
void timerWriteReload(struct GBA* gba, int id, uint16_t reload) {
	struct Timer* timer = &gba->timers[id];
	timer->pendingReload = reload;
	if (!timingIsScheduled(gba, &timer->reloadWrite)) {
		timingSchedule(gba, &timer->reloadWrite, 1);
	}
}

void timerWriteControl(struct GBA* gba, int id, uint16_t control) {
	struct Timer* timer = &gba->timers[id];
	timer->pendingControl = control & 0x00C7;
	if (!timingIsScheduled(gba, &timer->controlWrite)) {
		timingSchedule(gba, &timer->controlWrite, 1);
	}
}
