/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ref.h"

// The reference emulator for the test tools: game-armgba.c built around the exact CPU alone.

#define GAME refGame
#define GAME_NAME "REF"
#define RUN_LOOP refRunLoop
#define AFTER_RESET(gba) refReset(gba)
#define TRACE_ENV "REF_TRACE"

#include "game-armgba.c"
