/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_TESTS_REF_H
#define ARMGBA_TESTS_REF_H

// The reference run loop: the exact CPU alone, for the test tools.

struct GBA;

// After a reset, fills the prefetch from where the PC points.
void refReset(struct GBA*);
// Runs instructions one at a time until an event is due, then processes events.
void refRunLoop(struct GBA*);

#endif
