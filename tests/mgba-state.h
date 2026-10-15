/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_TESTS_MGBA_STATE_H
#define ARMGBA_TESTS_MGBA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Starting armGBA from an mGBA save state, so both can run on from the same point

struct GBA;

// Goes on from an mGBA save state of the game loaded, as near as armGBA can take it
bool mgbaStateLoad(struct GBA*, const uint8_t* state, size_t size);
// The state MGBA_STATE names, read once; NULL without one
const uint8_t* mgbaStateFile(size_t* size);

#endif
