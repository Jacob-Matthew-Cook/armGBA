/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_PATCH_H
#define ARMGBA_LIBRETRO_PATCH_H

// Soft patches, as RetroArch applies them: a game's IPS, UPS or BPS patch beside it, named as the
// game is, with .ips, .ups or .bps in place of its extension ("Game.zip#Game.gba" takes the
// archive's name, "Game.bps"), applied to the game as it is read; the file stays as it is. Patches
// numbered on from 1, "Game.ips1", "Game.bps2" and so on, follow it in turn.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Applies the patches beside path's game to the game read from it, *data of *size bytes, which then
// holds the patched game. Returns how many applied, or -1 if one didn't (made for another release of
// the game, or damaged), leaving the game as it was read; name then has that patch's file name.
int patchGame(const char* path, void** data, size_t* size, char* name, size_t nameSize);

#ifdef __cplusplus
}
#endif

#endif
