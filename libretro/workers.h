/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_WORKERS_H
#define ARMGBA_LIBRETRO_WORKERS_H

// Threads that run a job for each of a few indices at once: index 0 on the caller's thread, the
// others on threads of their own, which wait between runs spinning briefly, then asleep.

#ifdef __cplusplus
extern "C" {
#endif

// Starts threads for indices 1 to count - 1, as many as there are other CPU cores for.
void workersStart(int count);
void workersStop(void);
// The count workersStart was last asked for, 0 once stopped.
int workersAsked(void);
// Runs job(i) for each index below count, and returns when all have finished.
void workersRun(void (*job)(int index), int count);

#ifdef __cplusplus
}
#endif

#endif
