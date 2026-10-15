/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_H
#define ARMGBA_JIT_H

#include "gba.h"

// The recompiler as the rest of the emulator sees it

bool jitInit(struct GBA*);
void jitDeinit(struct GBA*);

// Runs a frame: compiled code, and the exact CPU around events
void jitRunLoop(struct GBA*);

// Guest memory changed under compiled code
void jitInvalidate(struct GBA*, uint32_t address, uint32_t size);
// After a store to IWRAM or EWRAM that the code maps flag
void jitCodeWritten(struct GBA*, uint32_t address, int size);
bool jitCovers(struct GBA*, uint32_t address);
// Brings r15 and the prefetch up to date when a slow memory access from compiled code needs them
void jitSyncPc(struct GBA*);
// The difference compiled code adds to a data access for its next fetch being nonsequential, or -1
// where it doesn't
int32_t jitFetchGap(struct GBA*);
// gap: jitFetchGap before the change
void jitWaitstatesChanged(struct GBA*, int32_t gap);
void jitFlush(struct GBA*);
void jitRomChanged(struct GBA*);
// Starts learning which code the game rewrites from scratch
void jitForgetRewrites(struct GBA*);
void jitStateLoaded(struct GBA*, bool codeChanged);
// While gba->idleProbing: the exact CPU's data reads and writes, before the write lands
void jitIdleRead(struct GBA*, uint32_t address, int size, uint32_t value);
void jitIdleWrite(struct GBA*, uint32_t address, uint32_t value, int size);

#endif
