/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_TESTS_MGBA_STATE_H
#define ARMGBA_TESTS_MGBA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Starting armGBA from an mGBA save state, so both can run on from the same point.

struct GBA;

// Continues from an mGBA save state of the loaded game, as closely as armGBA can.
bool mgbaStateLoad(struct GBA*, const uint8_t* state, size_t size);
// Returns the state MGBA_STATE names, read once, or NULL without one.
const uint8_t* mgbaStateFile(size_t* size);

#endif
