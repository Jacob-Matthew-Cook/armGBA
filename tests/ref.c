/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The reference: the exact CPU alone, without compiled code, for checking the JIT against

#include "ref.h"

#include "gba.h"

void refReset(struct GBA* gba) {
	stepReset(gba);
}

void refRunLoop(struct GBA* gba) {
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
