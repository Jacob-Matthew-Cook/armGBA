/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LINK_H
#define ARMGBA_LIBRETRO_LINK_H

// The link cable between players' consoles in a netplay session, carried over RetroArch's netpacket
// interface. Each player runs their own game and save; the host is player 1. A transfer waits for
// the other end's data, so a link is smooth on a local network and slow over a long one.

#include <stdbool.h>
#include <stdint.h>

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GBA;

// Offers the frontend the netpacket interface.
void linkSetEnvironment(retro_environment_t);
// The loaded console: a Game Boy Advance, a Game Boy (gba NULL), or none.
void linkSetGba(struct GBA*);
void linkSetGameBoy(bool loaded);
// Called before each Game Boy Advance frame: keeps the cable checked during it.
void linkGbaFrame(struct GBA*);
// Game Boy: whether another Game Boy is on the cable, a transfer this one clocked (returns the byte
// received), and a check for transfers the other one clocked.
bool linkGbConnected(void);
int linkGbTransfer(int out);
void linkGbPoll(void);

#ifdef __cplusplus
}
#endif

#endif
