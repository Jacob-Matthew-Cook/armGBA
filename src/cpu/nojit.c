/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The emulator without a recompiler, for CPUs that have no backend: every instruction runs on the exact
// CPU, and the recompiler's hooks do nothing.

#include "gba.h"

bool jitInit(struct GBA* gba) {
	return true;
}

void jitDeinit(struct GBA* gba) {
}

// Runs the exact CPU until the frame ends or a frontend asks for control back.
void jitRunLoop(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	uint32_t frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame && !gba->returnRequested) {
		if (cpu->halted) {
			if (cpu->cycles < cpu->nextEvent) {
				cpu->cycles = cpu->nextEvent;
			}
			processEvents(gba);
			continue;
		}
		stepInstruction(gba);
	}
	gba->returnRequested = false;
}

void jitInvalidate(struct GBA* gba, uint32_t address, uint32_t size) {
}

void jitCodeWritten(struct GBA* gba, uint32_t address, int size) {
}

bool jitCovers(struct GBA* gba, uint32_t address) {
	return false;
}

void jitSyncPc(struct GBA* gba) {
}

int32_t jitFetchGap(struct GBA* gba) {
	return -1;
}

void jitWaitstatesChanged(struct GBA* gba, int32_t gap) {
}

void jitFlush(struct GBA* gba) {
}

void jitRomChanged(struct GBA* gba) {
}

void jitForgetRewrites(struct GBA* gba) {
}

void jitStateLoaded(struct GBA* gba, bool codeChanged) {
}

void jitIdleRead(struct GBA* gba, uint32_t address, int size, uint32_t value) {
}

void jitIdleWrite(struct GBA* gba, uint32_t address, uint32_t value, int size) {
}
