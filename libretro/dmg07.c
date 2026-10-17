/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "dmg07.h"

#include <math.h>
#include <string.h>

enum { PING, START, DATA, STOP };

static int32_t ms(double milliseconds) {
	return (int32_t) lround(milliseconds * 2097152 / 1000);
}

// A byte takes 128 us to shift. Pinging, a byte starts every 1.55 ms and a packet every 4.71 ms plus
// 12.2 ms and the rate's low nibble in ms (12.29 ms before a rate was asked). Playing, the rate's high
// nibble spaces the bytes, and a packet lasts at least 17 ms plus its low nibble in ms.
static int32_t shift(void) {
	return ms(0.128);
}

static int32_t pingStep(void) {
	return shift() + ms(1.42);
}

static int32_t dataStep(const struct Dmg07* a) {
	return shift() + ms((a->rate >> 4) * 0.106 + 0.887);
}

static int packetBytes(const struct Dmg07* a) {
	return a->phase == PING || a->phase == START ? 4 : 4 * a->size;
}

void dmg07Reset(struct Dmg07* a) {
	memset(a, 0, sizeof(*a));
	a->at = shift();
}

uint8_t dmg07Out(const struct Dmg07* a, int port) {
	switch (a->phase) {
	case PING:
		return a->index ? a->answering | (port + 1) : 0xFE;
	case START:
		return 0xCC;
	case DATA:
		return a->data[a->index];
	default:
		return 0xFF;
	}
}

// A port answers a ping with $88 $88, or $AA from a Game Boy starting the game. Player 1's answer
// carries the rate and packet size.
static void ping(struct Dmg07* a, const uint8_t in[DMG07_PORTS]) {
	for (int i = 0; i < DMG07_PORTS; ++i) {
		uint8_t const bit = 1 << i;
		bool const answered = in[i] == 0x88 || in[i] == 0xAA;
		a->aa[i] = in[i] == 0xAA ? (a->index ? a->aa[i] : 0) + 1 : 0;
		if (a->index == 0) {
			a->acked = (a->acked & ~bit) | (answered ? bit : 0);
		} else if (a->index == 1) {
			a->acked &= answered ? 0xFF : ~bit;
			a->answering = (a->answering & ~(0x10 << i)) | (a->acked & bit ? 0x10 << i : 0);
		}
	}
	if (a->acked & 1 && in[0] != 0xAA) {
		if (a->index == 2) {
			a->rate = in[0];
		} else if (a->index == 3) {
			a->size = in[0] < 1 ? 1 : in[0] > 4 ? 4 : in[0];
		}
	}
}

// Collects each answering port's first bytes of the packet, and watches for three $FF at its start.
static void collect(struct Dmg07* a, const uint8_t in[DMG07_PORTS]) {
	uint8_t quitting = 0;
	for (int i = 0; i < DMG07_PORTS; ++i) {
		bool const there = a->answering & (0x10 << i);
		if (a->index < a->size) {
			a->next[i * a->size + a->index] = there ? in[i] : 0;
		}
		quitting |= there && in[i] == 0xFF ? 1 << i : 0;
	}
	a->stopping = a->index == 0 ? quitting : a->stopping & quitting;
	if (a->index == 2 && a->stopping) {
		a->stop = true;
	}
}

// After a packet: the next one, and when it starts.
static void endPacket(struct Dmg07* a) {
	int const bytes = packetBytes(a);
	bool started = false;
	switch (a->phase) {
	case PING:
		for (int i = 0; i < DMG07_PORTS; ++i) {
			started |= a->aa[i] >= 4;
		}
		a->packetAt += started ? 4 * pingStep() : ms(4.71) + (a->rate ? ms(12.2 + (a->rate & 0x0F)) : ms(12.29));
		a->phase = started ? START : PING;
		a->size = a->size ? a->size : 1;
		break;
	case START:
		a->packetAt += 4 * pingStep();
		a->phase = DATA;
		memset(a->data, 0, sizeof(a->data));
		memset(a->next, 0, sizeof(a->next));
		a->stop = false;
		break;
	case DATA: {
		int32_t const least = ms(17 + (a->rate & 0x0F));
		int32_t const needed = bytes * dataStep(a) + ms(1);
		a->packetAt += least > needed ? least : needed;
		memcpy(a->data, a->next, sizeof(a->data));
		memset(a->next, 0, sizeof(a->next));
		a->phase = a->stop ? STOP : DATA;
		break;
	}
	case STOP:
		a->packetAt += bytes * dataStep(a);
		a->phase = PING;
		a->answering = 0;
		break;
	}
	a->index = 0;
	a->at = a->packetAt + shift();
}

void dmg07Transfer(struct Dmg07* a, const uint8_t in[DMG07_PORTS]) {
	if (a->phase == PING) {
		ping(a, in);
	} else if (a->phase == DATA) {
		collect(a, in);
	}
	if (++a->index < packetBytes(a)) {
		a->at = a->packetAt + shift() + a->index * (a->phase == DATA || a->phase == STOP ? dataStep(a) : pingStep());
	} else {
		endPacket(a);
	}
}

void dmg07EndFrame(struct Dmg07* a, int32_t samples) {
	a->at -= samples;
	a->packetAt -= samples;
}
