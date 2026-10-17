/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/gba.h"
#include "gb.h"

// The host keeps the cable: the players running the same console as it, in the order they joined,
// the host first. The console that clocks a transfer broadcasts a request with its data; each other
// console on the cable answers with its own and takes the transfer; in multiplayer mode with more
// than two consoles the parent then sends everyone all four words. The clocking console waits for
// the answers inside the transfer, so emulated time stays exact on its side.

enum {
	MAX_PLAYERS = 4,
	MAX_PEERS = 8,
	QUEUE = 16,
	// How often a console checks the cable for transfers the others clocked, in GBA cycles.
	POLL_CYCLES = 4096,
};

// A console that doesn't answer for this long has left; the transfer gets nothing.
static const int64_t TIMEOUT_NS = 3000000000LL;

enum { CONSOLE_NONE, CONSOLE_GB, CONSOLE_GBA };
enum { MSG_HELLO = 1, MSG_ROSTER, MSG_REQUEST, MSG_REPLY, MSG_DONE };
enum { MODE_MULTI = 2 };

static retro_environment_t environment;
static retro_netpacket_send_t sendPacket;
static retro_netpacket_poll_receive_t pollPackets;
static bool session;
static uint16_t self;
static int console;
static struct GBA* gba;

static uint16_t cable[MAX_PLAYERS];
static int cableCount;
// The players the host knows, and the console each runs.
static struct {
	uint16_t id;
	uint8_t console;
} peers[MAX_PEERS];
static int peerCount;

// Transfers other consoles clocked, waiting to be taken.
static struct {
	uint16_t from;
	uint16_t seq;
	uint32_t data;
} requests[QUEUE];
static int requestCount;

// A transfer this console clocked, waiting for the others' data.
static struct {
	bool waiting;
	uint16_t seq;
	uint8_t mode;
	uint32_t out;
	uint32_t in[3];
	unsigned answered;
	unsigned expected;
} own;
static uint16_t nextSeq;

// A multiplayer child's wait for the parent's four words.
static struct {
	bool waiting;
	bool arrived;
	uint16_t seq;
	uint32_t values[4];
} done;

static struct SioLink sioLink = { .players = 1 };
static struct Event pollEvent;

static void put16(uint8_t* to, uint16_t value) {
	to[0] = value;
	to[1] = value >> 8;
}

static void put32(uint8_t* to, uint32_t value) {
	put16(to, value);
	put16(to + 2, value >> 16);
}

static uint16_t get16(const uint8_t* from) {
	return from[0] | from[1] << 8;
}

static uint32_t get32(const uint8_t* from) {
	return get16(from) | (uint32_t) get16(from + 2) << 16;
}

static void send(const uint8_t* message, size_t size, uint16_t to) {
	if (session && sendPacket) {
		sendPacket(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, message, size, to);
	}
}

// A player's place on the cable, or -1.
static int slotOf(uint16_t id) {
	for (int i = 0; i < cableCount; ++i) {
		if (cable[i] == id) {
			return i;
		}
	}
	return -1;
}

// Tells the player when the cable changes: their place on it, or why they aren't on it.
static void announce(int slot) {
	static int shownCount = -1, shownSlot = -1;
	if (!session || (cableCount == shownCount && slot == shownSlot)) {
		return;
	}
	char text[96];
	if (slot >= 0 && cableCount > 1) {
		snprintf(text, sizeof(text), "Link cable: player %d of %d", slot + 1, cableCount);
	} else if (slot < 0 && cableCount > 0) {
		snprintf(text, sizeof(text), "Link cable: the host is playing a different console, or the cable is full");
	} else if (shownCount > 1) {
		snprintf(text, sizeof(text), "Link cable: the other players left");
	} else {
		text[0] = 0;
	}
	shownCount = cableCount;
	shownSlot = slot;
	if (text[0] && environment) {
		struct retro_message message = { text, 240 };
		environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
}

static void cableChanged(void) {
	int slot = slotOf(self);
	announce(slot);
	if (gba && sioLink.players == (slot < 0 ? 1 : cableCount) && sioLink.id == (slot < 0 ? 0 : slot) && gba->sio.link == &sioLink) {
		return;
	}
	sioLink.players = slot < 0 ? 1 : cableCount;
	sioLink.id = slot < 0 ? 0 : slot;
	if (gba) {
		sioAttach(gba, &sioLink);
	}
}

static int maxPlayers(void) {
	return console == CONSOLE_GBA ? MAX_PLAYERS : 2;
}

// The host's view: everyone who joined with the same console, in order, up to what the cable takes.
static void hostRebuild(void) {
	cableCount = 0;
	if (console != CONSOLE_NONE) {
		cable[cableCount++] = self;
		for (int i = 0; i < peerCount && cableCount < maxPlayers(); ++i) {
			if (peers[i].console == console) {
				cable[cableCount++] = peers[i].id;
			}
		}
	}
	uint8_t message[2 + 2 * MAX_PLAYERS] = { MSG_ROSTER, (uint8_t) cableCount };
	for (int i = 0; i < cableCount; ++i) {
		put16(message + 2 + 2 * i, cable[i]);
	}
	send(message, 2 + 2 * cableCount, RETRO_NETPACKET_BROADCAST);
	cableChanged();
}

static void hello(void) {
	if (self == 0) {
		hostRebuild();
	} else {
		uint8_t message[2] = { MSG_HELLO, (uint8_t) console };
		send(message, sizeof(message), 0);
	}
}

static void reply(uint16_t to, uint16_t seq, uint32_t data, bool ready) {
	uint8_t message[10] = { MSG_REPLY, ready };
	put16(message + 2, seq);
	put32(message + 4, data);
	put16(message + 8, to);
	send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
}

static void answered(uint16_t from, uint32_t data) {
	int slot = slotOf(from);
	if (slot < 0 || !(own.expected & (1u << slot))) {
		return;
	}
	own.answered |= 1u << slot;
	if (own.mode == MODE_MULTI && console == CONSOLE_GBA) {
		own.in[slot - 1] = data & 0xFFFF;
	} else {
		own.in[0] = data;
	}
}

static void receive(const void* buffer, size_t size, uint16_t from) {
	const uint8_t* message = buffer;
	if (size < 2) {
		return;
	}
	switch (message[0]) {
	case MSG_HELLO:
		if (self == 0) {
			for (int i = 0; i < peerCount; ++i) {
				if (peers[i].id == from) {
					peers[i].console = message[1];
					hostRebuild();
					return;
				}
			}
			if (peerCount < MAX_PEERS) {
				peers[peerCount].id = from;
				peers[peerCount++].console = message[1];
				hostRebuild();
			}
		}
		break;
	case MSG_ROSTER:
		if (from == 0 && size >= 2u + 2u * message[1] && message[1] <= MAX_PLAYERS) {
			cableCount = message[1];
			for (int i = 0; i < cableCount; ++i) {
				cable[i] = get16(message + 2 + 2 * i);
			}
			cableChanged();
		}
		break;
	case MSG_REQUEST:
		if (size < 8 || slotOf(from) < 0) {
			break;
		}
		// Two consoles clocking at once swap their data, as the wires would.
		if (own.waiting && own.mode != MODE_MULTI) {
			answered(from, get32(message + 4));
			reply(from, get16(message + 2), own.out, true);
		} else if (requestCount < QUEUE) {
			requests[requestCount].from = from;
			requests[requestCount].seq = get16(message + 2);
			requests[requestCount++].data = get32(message + 4);
		}
		break;
	case MSG_REPLY:
		if (size >= 10 && own.waiting && get16(message + 8) == self && get16(message + 2) == own.seq) {
			answered(from, message[1] ? get32(message + 4) : 0xFFFFFFFF);
		}
		break;
	case MSG_DONE:
		if (size >= 12 && done.waiting && get16(message + 2) == done.seq) {
			for (int i = 0; i < 4; ++i) {
				done.values[i] = get16(message + 4 + 2 * i);
			}
			done.arrived = true;
		}
		break;
	}
}

static int64_t now(void) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000000000LL + time.tv_nsec;
}

// Reads the network until the condition holds, the session ends or the other end is gone.
static void waitFor(bool (*ready)(void)) {
	int64_t start = now();
	while (session && !ready() && now() - start < TIMEOUT_NS) {
		if (pollPackets) {
			pollPackets();
		}
		if (ready()) {
			break;
		}
		struct timespec pause = { 0, 100000 };
		nanosleep(&pause, NULL);
	}
}

static bool allAnswered(void) {
	return (own.answered & own.expected) == own.expected;
}

static bool doneArrived(void) {
	return done.arrived;
}

static void begin(void* context, int mode, uint32_t out) {
	(void) context;
	own.waiting = true;
	own.seq = ++nextSeq;
	own.mode = mode;
	own.out = out;
	own.answered = 0;
	own.expected = 0;
	for (int i = 0; i < cableCount; ++i) {
		if (cable[i] != self && (mode == MODE_MULTI || !own.expected)) {
			own.expected |= 1u << i;
		}
	}
	own.in[0] = 0xFFFFFFFF;
	own.in[1] = own.in[2] = 0xFFFF;
	uint8_t message[8] = { MSG_REQUEST, (uint8_t) mode };
	put16(message + 2, own.seq);
	put32(message + 4, out);
	send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
}

static void finish(void* context, uint32_t in[3]) {
	(void) context;
	waitFor(allAnswered);
	memcpy(in, own.in, sizeof(own.in));
	own.waiting = false;
	if (own.mode == MODE_MULTI && cableCount > 2) {
		uint8_t message[12] = { MSG_DONE, 0 };
		put16(message + 2, own.seq);
		put16(message + 4, own.out);
		for (int i = 0; i < 3; ++i) {
			put16(message + 6 + 2 * i, own.in[i]);
		}
		send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
	}
}

// Takes the transfers other consoles clocked: answers each with this console's data, then receives.
static void takeRequests(void) {
	for (int i = 0; i < requestCount; ++i) {
		uint16_t from = requests[i].from, seq = requests[i].seq;
		uint32_t data = requests[i].data;
#ifdef ARMGBA_GB
		if (console == CONSOLE_GB) {
			reply(from, seq, libretroGbLinkOut(), true);
			libretroGbLinkIn(data & 0xFF);
			continue;
		}
#endif
		if (console != CONSOLE_GBA || !gba) {
			continue;
		}
		int mode;
		bool ready;
		uint32_t mine = sioOutgoing(gba, &mode, &ready);
		reply(from, seq, mine, ready);
		uint32_t values[4] = { data, 0xFFFF, 0xFFFF, 0xFFFF };
		if (mode == MODE_MULTI) {
			values[0] = data & 0xFFFF;
			if (cableCount > 2) {
				done.waiting = true;
				done.arrived = false;
				done.seq = seq;
				waitFor(doneArrived);
				done.waiting = false;
				if (done.arrived) {
					memcpy(values, done.values, sizeof(values));
				}
			} else {
				values[1] = mine & 0xFFFF;
			}
		}
		sioReceive(gba, values);
	}
	requestCount = 0;
}

static void poll(void) {
	if (pollPackets) {
		pollPackets();
	}
	takeRequests();
}

static void pollEventRun(struct GBA* machine, void* context, uint32_t late) {
	(void) context;
	(void) late;
	poll();
	if (cableCount > 1) {
		timingSchedule(machine, &pollEvent, POLL_CYCLES);
	}
}

static void start(uint16_t id, retro_netpacket_send_t sendFunction, retro_netpacket_poll_receive_t pollFunction) {
	sendPacket = sendFunction;
	pollPackets = pollFunction;
	session = true;
	self = id;
	peerCount = 0;
	requestCount = 0;
	own.waiting = false;
	hello();
}

static void stop(void) {
	session = false;
	sendPacket = NULL;
	pollPackets = NULL;
	cableCount = 0;
	requestCount = 0;
	own.waiting = false;
	cableChanged();
}

// Every half second: the host repeats the cable and a client not on it says hello again, since a
// packet sent while a player is still joining can be lost.
static void sessionPoll(void) {
	static unsigned frames;
	if (!session || ++frames % 30) {
		return;
	}
	if (self == 0) {
		hostRebuild();
	} else if (slotOf(self) < 0) {
		hello();
	}
}

static bool connected(uint16_t id) {
	(void) id;
	return peerCount < MAX_PEERS;
}

static void disconnected(uint16_t id) {
	for (int i = 0; i < peerCount; ++i) {
		if (peers[i].id == id) {
			memmove(&peers[i], &peers[i + 1], (peerCount - i - 1) * sizeof(peers[0]));
			--peerCount;
			hostRebuild();
			return;
		}
	}
}

void linkSetEnvironment(retro_environment_t callback) {
	environment = callback;
	static const struct retro_netpacket_callback callbacks = {
		start, receive, stop, sessionPoll, connected, disconnected, "armGBA link 1",
	};
	callback(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*) &callbacks);
	sioLink.begin = begin;
	sioLink.finish = finish;
	pollEvent.callback = pollEventRun;
	pollEvent.priority = 0x90;
}

static void consoleChanged(int now) {
	console = now;
	if (session) {
		hello();
	}
}

void linkSetGba(struct GBA* machine) {
	gba = machine;
	consoleChanged(machine ? CONSOLE_GBA : CONSOLE_NONE);
	if (machine) {
		cableChanged();
	}
}

void linkSetGameBoy(bool loaded) {
	gba = NULL;
	consoleChanged(loaded ? CONSOLE_GB : CONSOLE_NONE);
}

void linkGbaFrame(struct GBA* machine) {
	timingDeschedule(machine, &pollEvent);
	if (session && cableCount > 1) {
		takeRequests();
		timingSchedule(machine, &pollEvent, POLL_CYCLES);
	}
}

bool linkGbConnected(void) {
	return session && console == CONSOLE_GB && cableCount > 1 && slotOf(self) >= 0;
}

int linkGbTransfer(int out) {
	uint32_t in[3];
	begin(NULL, 0, out & 0xFF);
	finish(NULL, in);
	return in[0] & 0xFF;
}

void linkGbPoll(void) {
	if (session) {
		poll();
	}
}
