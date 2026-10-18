/* Copyright (c) 2026 Magnominus
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
struct RfuAir;

// Offers the frontend the netpacket interface.
void linkSetEnvironment(retro_environment_t);
// The loaded console: a Game Boy Advance, a Game Boy (gba NULL), or none.
void linkSetGba(struct GBA*);
void linkSetGameBoy(bool loaded);
// The Wireless Adapter's air to the other player, and whether the adapter is on the link port
// instead of the cable.
const struct RfuAir* linkAir(void);
void linkSetWireless(bool);
// Whether a netplay session is on, during which time runs at the same speed for every player.
bool linkSession(void);
// Called before each Game Boy Advance frame: keeps the cable checked during it.
void linkGbaFrame(struct GBA*);
// The Link Timing option: whether exact timing is chosen, and whether it is on.
void linkSetExact(bool);
bool linkExactOn(void);
// Game Boy: whether another Game Boy is on the cable, a transfer this one clocked (returns the byte
// received), and a check for transfers the other one clocked.
bool linkGbConnected(void);
int linkGbTransfer(int out);
void linkGbPoll(void);
// Game Boy with exact timing: a transfer it clocks, told as soon as it is under way (sound samples
// until it ends, and its length); the samples it may run before it must stop; the samples it ran;
// and its stop, which takes the transfer due there or waits for the console leading, returning
// false if it is still to wait, for the frame to end early.
void linkGbClocking(int out, long left, long length);
long linkGbAllowance(void);
void linkGbRan(unsigned samples);
bool linkGbArrived(void);

#ifdef __cplusplus
}
#endif

#endif
