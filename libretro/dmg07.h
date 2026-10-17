/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_DMG07_H
#define ARMGBA_LIBRETRO_DMG07_H

// The Four Player Adapter (DMG-07), as Pan Docs describes it from GBE+'s research. It clocks every
// Game Boy on it itself. It pings each port with $FE and three status bytes (the ports answering in
// bits 4-7, the port's number in bits 0-2), and a Game Boy answers $88, $88, the rate it wants and the
// packet size. Four $AA from a Game Boy start the game: four $CC, then packets of four times the size,
// in which each Game Boy's first bytes are collected and the next packet hands every port all four
// players' bytes in order, zeros for a port nobody answers on. Three $FF at the start of a packet end
// it: a packet of $FF, then pinging again.
//
// Times are in Game Boy sound samples (2^21 a second), counted by whoever runs the adapter.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DMG07_PORTS = 4 };
// What it is doing: pinging, sending the $CC that start a game, passing the game's packets, or
// sending the $FF packet that ends it.
enum { DMG07_PING, DMG07_START, DMG07_DATA, DMG07_STOP };

struct Dmg07 {
	// The next transfer, and the start of the packet under way.
	int32_t at;
	int32_t packetAt;
	uint8_t phase;
	uint8_t index;
	// Bits 4-7: the ports a Game Boy answers pings on.
	uint8_t answering;
	uint8_t acked;
	// The rate and packet size player 1 asked for.
	uint8_t rate;
	uint8_t size;
	uint8_t stopping;
	bool stop;
	uint8_t aa[DMG07_PORTS];
	// The packet being handed out, and the one being collected.
	uint8_t data[4 * DMG07_PORTS];
	uint8_t next[4 * DMG07_PORTS];
};

void dmg07Reset(struct Dmg07*);
// The byte the adapter sends on a port in the transfer due at `at`.
uint8_t dmg07Out(const struct Dmg07*, int port);
// Takes each port's byte from that transfer (0xFF where no Game Boy took part) and moves on to the
// next one.
void dmg07Transfer(struct Dmg07*, const uint8_t in[DMG07_PORTS]);
// Moves its times back as the frame they count from ends.
void dmg07EndFrame(struct Dmg07*, int32_t samples);

#ifdef __cplusplus
}
#endif

#endif
