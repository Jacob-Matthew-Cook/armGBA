/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_CORE_H
#define ARMGBA_LIBRETRO_CORE_H

// What the libretro core's own files share, by the file that keeps it. Nothing here is seen
// outside the core.

#include "gb.h"
#include "libretro.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// libretro.c: the frontend's calls back, and the game and consoles running.
extern retro_video_refresh_t videoRefresh;
extern bool gameBoy;
extern enum LibretroGbModel gbModel;
// The original Game Boy's screen colors: 0 plain grays, 1 the Pocket's, 2 the first model's, 3 the
// Light's.
extern int gbShades;
// Game Boy Advance Panel: a Game Boy game on the Advance model shows the Color's panel.
extern bool gbColorPanel;
// Game Boy Advance consoles linked here, run in turns.
extern bool gbaTogether;
enum { THREADED_OFF, THREADED_ON, THREADED_LATE };
extern int threadedVideo;
// Whether RetroArch is running this frame again, for a netplay rollback or runahead, rather than
// for the first time.
bool rerunning(void);

// lcd.c: LCD Ghosting (0 off, 1 half, 2 full) and Interleaved Rows; LCD Grid's scale (1 when off)
// and its shadow's way (0 none, then up and on clockwise); the threads they share rows among (0
// chooses). showPicture sends the picture on through them both, sendPicture through LCD Grid alone.
extern int ghosting;
extern bool interleaved;
extern int gridScale;
extern int gridWay;
extern int lcdThreads;
void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch);
void sendPicture(const void* picture, unsigned width, unsigned height, size_t pitch);

#endif
