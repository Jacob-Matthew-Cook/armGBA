/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_DOLPHIN_H
#define ARMGBA_LIBRETRO_DOLPHIN_H

// The GameCube link cable: this Game Boy Advance on a controller port of Dolphin, set to GBA (TCP).
// Dolphin listens for it, and sends each command with the time it may run before the next.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GBA;

// Where Dolphin runs: an address, "auto" to look for it on this device and its network, or NULL to
// unplug.
void dolphinSetHost(const char* host);
// Called before each frame: keeps looking for Dolphin until it answers, then serves it during the
// frame.
void dolphinGbaFrame(struct GBA*);
// Whether Dolphin is connected, and where.
bool dolphinConnected(void);
const char* dolphinAddress(void);

#ifdef __cplusplus
}
#endif

#endif
