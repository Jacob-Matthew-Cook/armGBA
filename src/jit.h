/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_H
#define ARMGBA_JIT_H

#include "gba.h"

// The recompiler as the rest of the emulator sees it.

bool jitInit(struct GBA*);
void jitDeinit(struct GBA*);

// Runs a frame: compiled code, with the exact CPU around events.
void jitRunLoop(struct GBA*);

// Called when guest memory changed under compiled code.
void jitInvalidate(struct GBA*, uint32_t address, uint32_t size);
// Called after a store to IWRAM or EWRAM that the code maps flag.
void jitCodeWritten(struct GBA*, uint32_t address, int size);
bool jitCovers(struct GBA*, uint32_t address);
// Brings r15 and the prefetch up to date when a slow memory access from compiled code needs them.
void jitSyncPc(struct GBA*);
// Returns the cycles compiled code adds to a data access because its next fetch is nonsequential,
// or -1 where it doesn't.
int32_t jitFetchGap(struct GBA*);
// gap: jitFetchGap from before the change.
void jitWaitstatesChanged(struct GBA*, int32_t gap);
void jitFlush(struct GBA*);
void jitRomChanged(struct GBA*);
// Forgets which code the game rewrites, and starts learning again.
void jitForgetRewrites(struct GBA*);
void jitStateLoaded(struct GBA*, bool codeChanged);
// While gba->idleProbing, receives the exact CPU's data reads and writes, before a write lands.
void jitIdleRead(struct GBA*, uint32_t address, int size, uint32_t value);
void jitIdleWrite(struct GBA*, uint32_t address, uint32_t value, int size);

#endif
