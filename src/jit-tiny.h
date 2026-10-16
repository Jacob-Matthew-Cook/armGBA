/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_TINY_H
#define ARMGBA_JIT_TINY_H

// Tiny routines, for the ARM backends: on x86-64 the calls cost more than the code they save.
// Included after jit-compile.h; the backend defines tinyBegin, tinyEnd and tinyCall, and calls
// forgetTiny before abandoning a compile.

static uint8_t* tinyBegin(struct Compiler*, struct TinyRoutine*);
static void tinyEnd(struct Compiler*, uint8_t* over);
static void tinyCall(struct Compiler*, const uint8_t* routine);

// A timing sequence that depends only on a few small numbers is emitted once, where it is first
// needed, and called from then on: on a core with a small cache, the code this saves is worth more
// than the call. Returns the slot for the key, or NULL when the table is full or a number doesn't
// fit.
static struct TinyRoutine* tinySlot(struct Compiler* c, int kind, int n0, int n1, int n2, int n3) {
	int numbers[4] = { n0, n1, n2, n3 };
	uint64_t key = (uint64_t) kind << 60;
	for (int i = 0; i < 4; ++i) {
		int n = numbers[i];
		if (n != NONE && (n < -0x3000 || n > 0x3000)) {
			return NULL;
		}
		key |= (uint64_t) (n == NONE ? 0x7FFF : n + 0x3000) << (i * 15);
	}
	for (unsigned probe = 0; probe < 8; ++probe) {
		struct TinyRoutine* slot = &c->jit->tiny[((key * 0x9E3779B97F4A7C15ull >> 54) + probe) & (TINY_ROUTINES - 1)];
		if (slot->code && slot->key == key) {
			return slot;
		}
		if (!slot->code) {
			if (c->newTinyCount == (int) (sizeof(c->newTiny) / sizeof(*c->newTiny))) {
				return NULL;
			}
			slot->key = key;
			return slot;
		}
	}
	return NULL;
}

// Calls the tiny routine for a slot, emitting it first if it's new; with no slot, emits the
// sequence inline.
static void callTiny(struct Compiler* c, struct TinyRoutine* slot, void (*body)(struct Compiler*)) {
	if (!slot) {
		body(c);
		return;
	}
	if (!slot->code) {
		uint8_t* over = tinyBegin(c, slot);
		body(c);
		tinyEnd(c, over);
	}
	tinyCall(c, slot->code);
}

// Forgets the tiny routines a compile whose code didn't fit emitted.
static void forgetTiny(struct Compiler* c) {
	for (int i = 0; i < c->newTinyCount; ++i) {
		c->newTiny[i]->code = NULL;
	}
}

#endif
