/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/gba.h"
#include "dmg07.h"
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
	// The longest Wireless Adapter message.
	AIR_BYTES = 100,
};

// A console that doesn't answer for this long has left; the transfer gets nothing.
static const int64_t TIMEOUT_NS = 3000000000LL;

enum { CONSOLE_NONE, CONSOLE_GB, CONSOLE_GBA };
enum {
	MSG_HELLO = 1,
	MSG_ROSTER,
	MSG_REQUEST,
	MSG_REPLY,
	MSG_DONE,
	MSG_RFU,
	MSG_GONE,
	MSG_OFFER,
	MSG_ADAPTER_STATUS,
	MSG_ADAPTER_PACKET,
	MSG_ADAPTER_QUIT,
};
// A request's mode: a GBA serial mode, or a Game Boy transfer that took the other's offered byte.
enum { MODE_MULTI = 2, MODE_TOOK_OFFER = 0x80 };

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
	uint8_t mode;
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

// Game Boy send-ahead: a Game Boy waiting for the other's clock offers the byte it will send, so the
// other can finish its transfer at once instead of waiting a round trip. An offer names the last
// transfer its sender took from the other, so one made before a later transfer isn't used.
static struct {
	bool sent;
	bool ready;
	uint16_t after;
	uint8_t byte;
} offer, offered;
static uint16_t taken;

// The Four Player Adapter over netplay. Each device runs an adapter for its own Game Boy, on the
// port of its place on the cable. The other ports are the other players' Game Boys as they tell it:
// whether they answer pings, player 1's rate and packet size, a game starting or ending, and each
// packet's bytes, which an adapter hands out a packet later, so they have that long to arrive.
struct AdapterPort {
	bool answering;
	bool starting;
	bool quitting;
	uint8_t rate;
	uint8_t size;
	// The last two packets' bytes, by packet number.
	uint16_t packets[2];
	bool has[2];
	uint8_t bytes[2][4];
};
static struct {
	bool wanted;
	struct Dmg07 machine;
	uint16_t packet;
	uint8_t phase;
	struct AdapterPort ports[DMG07_PORTS];
	// What this Game Boy answered, and what the others were last told.
	struct AdapterPort own;
	struct AdapterPort told;
	bool toldAny;
	bool toldQuit;
} adapter;

static void resetAdapter(void) {
	bool const wanted = adapter.wanted;
	memset(&adapter, 0, sizeof(adapter));
	adapter.wanted = wanted;
	dmg07Reset(&adapter.machine);
}

static struct SioLink sioLink = { .players = 1 };
static struct Event pollEvent;

// The Wireless Adapter's air: its messages go to the other players, whatever console they run.
static bool wireless;
static void airSend(const struct RfuAir*, int to, const uint8_t* message, size_t size);
static struct RfuAir air = { 0, true, airSend };

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
	if (gba && sioLink.players == (slot < 0 ? 1 : cableCount) && sioLink.id == (slot < 0 ? 0 : slot) && gba->sio.link == (wireless ? NULL : &sioLink)) {
		return;
	}
	sioLink.players = slot < 0 ? 1 : cableCount;
	sioLink.id = slot < 0 ? 0 : slot;
	if (gba) {
		sioAttach(gba, wireless ? NULL : &sioLink);
	}
}

static void airSend(const struct RfuAir* from, int to, const uint8_t* message, size_t size) {
	(void) from;
	uint8_t packet[1 + AIR_BYTES] = { MSG_RFU };
	if (size > AIR_BYTES) {
		return;
	}
	memcpy(packet + 1, message, size);
	send(packet, size + 1, to < 0 ? RETRO_NETPACKET_BROADCAST : (uint16_t) to);
}

static void peerGone(uint16_t id) {
	if (gba) {
		rfuPeerGone(gba, id);
	}
}

static int maxPlayers(void) {
	return console == CONSOLE_GBA || adapter.wanted ? MAX_PLAYERS : 2;
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

static void adapterReceive(const uint8_t* message, size_t size, int port) {
	if (port < 0 || port >= DMG07_PORTS) {
		return;
	}
	struct AdapterPort* const p = &adapter.ports[port];
	switch (message[0]) {
	case MSG_ADAPTER_STATUS:
		if (size >= 5) {
			p->answering = message[1];
			// Kept until this adapter starts the game too, however briefly the other was starting.
			p->starting |= message[2];
			p->rate = message[3];
			p->size = message[4];
		}
		break;
	case MSG_ADAPTER_PACKET: {
		unsigned const bytes = size >= 4 ? message[3] : 0;
		if (bytes <= 4 && size >= 4 + bytes) {
			uint16_t const packet = get16(message + 1);
			p->packets[packet & 1] = packet;
			p->has[packet & 1] = true;
			memcpy(p->bytes[packet & 1], message + 4, bytes);
		}
		break;
	}
	case MSG_ADAPTER_QUIT:
		p->quitting = true;
		break;
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
		offered.ready = false;
		// Two consoles clocking at once swap their data, as the wires would.
		if (own.waiting && own.mode != MODE_MULTI) {
			taken = get16(message + 2);
			answered(from, get32(message + 4));
			reply(from, get16(message + 2), own.out, true);
		} else if (requestCount < QUEUE) {
			requests[requestCount].from = from;
			requests[requestCount].seq = get16(message + 2);
			requests[requestCount].mode = message[1];
			requests[requestCount++].data = get32(message + 4);
		}
		break;
	case MSG_OFFER:
		if (size >= 5 && slotOf(from) >= 0) {
			offered.ready = message[1];
			offered.byte = message[2];
			offered.after = get16(message + 3);
		}
		break;
	case MSG_REPLY:
		if (size >= 10 && own.waiting && get16(message + 8) == self && get16(message + 2) == own.seq) {
			answered(from, message[1] ? get32(message + 4) : 0xFFFFFFFF);
		}
		break;
	case MSG_RFU:
		if (gba) {
			rfuReceive(gba, from, message + 1, size - 1);
		}
		break;
	case MSG_GONE:
		if (from == 0 && size >= 3) {
			peerGone(get16(message + 1));
		}
		break;
	case MSG_ADAPTER_STATUS:
	case MSG_ADAPTER_PACKET:
	case MSG_ADAPTER_QUIT:
		adapterReceive(message, size, slotOf(from));
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

static bool finish(void* context, uint32_t in[3]) {
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
	return true;
}

// Takes the transfers other consoles clocked: answers each with this console's data, then receives.
static void takeRequests(void) {
	for (int i = 0; i < requestCount; ++i) {
		uint16_t from = requests[i].from, seq = requests[i].seq;
		uint32_t data = requests[i].data;
#ifdef ARMGBA_GB
		// A Game Boy takes one transfer at a time, so its game can ready the next byte in between.
		if (console == CONSOLE_GB) {
			if (!(requests[i].mode & MODE_TOOK_OFFER)) {
				reply(from, seq, libretroGbLinkOut(), true);
			}
			libretroGbLinkIn(data & 0xFF);
			taken = seq;
			memmove(&requests[0], &requests[i + 1], (requestCount - i - 1) * sizeof(requests[0]));
			requestCount -= i + 1;
			return;
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
	air.self = id;
	nextSeq = 0;
	taken = 0;
	offer.sent = false;
	offered.ready = false;
	resetAdapter();
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
	if (gba) {
		rfuPeerGone(gba, -1);
	}
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
			uint8_t message[3] = { MSG_GONE };
			put16(message + 1, id);
			send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
			peerGone(id);
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

bool linkSession(void) {
	return session;
}

bool linkGbConnected(void) {
	return session && console == CONSOLE_GB && cableCount > 1 && slotOf(self) >= 0;
}

int linkGbTransfer(int out) {
	if (offered.ready && offered.after == nextSeq) {
		offered.ready = false;
		uint8_t message[8] = { MSG_REQUEST, MODE_TOOK_OFFER };
		put16(message + 2, ++nextSeq);
		put32(message + 4, out & 0xFF);
		send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
		return offered.byte;
	}
	uint32_t in[3];
	begin(NULL, 0, out & 0xFF);
	finish(NULL, in);
	return in[0] & 0xFF;
}

// Offers this Game Boy's byte while it waits for the other's clock, and says when it stops waiting.
static void offerByte(void) {
#ifdef ARMGBA_GB
	bool ready = (libretroGbLinkControl() & 0x81) == 0x80;
	uint8_t byte = libretroGbLinkOut();
	if (offer.sent && offer.ready == ready && (!ready || (offer.after == taken && offer.byte == byte))) {
		return;
	}
	offer.sent = true;
	offer.ready = ready;
	offer.after = taken;
	offer.byte = byte;
	uint8_t message[5] = { MSG_OFFER, ready, byte };
	put16(message + 3, taken);
	send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
#endif
}

void linkGbPoll(void) {
	if (session) {
		poll();
		if (linkGbConnected()) {
			offerByte();
		}
	}
}

const struct RfuAir* linkAir(void) {
	return &air;
}

void linkSetWireless(bool on) {
	if (wireless != on) {
		wireless = on;
		cableChanged();
	}
}

void linkSetGbAdapter(bool on) {
	if (adapter.wanted != on) {
		adapter.wanted = on;
		resetAdapter();
		if (session) {
			hello();
		}
	}
}

bool linkAdapterOn(void) {
	return session && console == CONSOLE_GB && adapter.wanted && slotOf(self) >= 0;
}

// What another player's port gives in this transfer: the answers to a ping its Game Boy gave, $AA
// while it starts a game, $FF while it ends one; its bytes of a packet come in their own message.
static uint8_t remoteIn(struct AdapterPort* p) {
	struct Dmg07 const* const m = &adapter.machine;
	switch (m->phase) {
	case DMG07_PING:
		if (p->starting) {
			return 0xAA;
		}
		if (!p->answering) {
			return 0xFF;
		}
		return m->index < 2 ? 0x88 : m->index == 2 ? p->rate : p->size;
	case DMG07_DATA:
		if (p->quitting && m->index < 3) {
			p->quitting = m->index < 2;
			return 0xFF;
		}
		return 0;
	default:
		return 0xFF;
	}
}

static uint8_t waitingPort;

static bool packetArrived(void) {
	struct AdapterPort const* const p = &adapter.ports[waitingPort];
	return p->has[adapter.packet & 1] && p->packets[adapter.packet & 1] == adapter.packet;
}

// Before a packet ends, the other players' bytes of it go in, waited for if they haven't come.
static void fillPacket(int me) {
	struct Dmg07* const m = &adapter.machine;
	for (int port = 0; port < cableCount && port < DMG07_PORTS; ++port) {
		if (port == me || !(m->answering & (0x10 << port))) {
			continue;
		}
		waitingPort = port;
		waitFor(packetArrived);
		struct AdapterPort const* const p = &adapter.ports[port];
		bool const arrived = packetArrived();
		for (int k = 0; k < m->size; ++k) {
			m->next[port * m->size + k] = arrived ? p->bytes[adapter.packet & 1][k] : 0;
		}
	}
}

// Tells the others what this Game Boy answered, when that changes; its bytes of each packet once
// it has given them; and the end of a game it asked for.
static void tellOthers(int me) {
	struct Dmg07 const* const m = &adapter.machine;
	struct AdapterPort* const own = &adapter.own;
	own->answering = m->answering & (0x10 << me);
	own->starting = m->phase == DMG07_START || (m->phase == DMG07_PING && m->aa[me]);
	struct AdapterPort* const told = &adapter.told;
	if (!adapter.toldAny || own->answering != told->answering || own->starting != told->starting || own->rate != told->rate || own->size != told->size) {
		*told = *own;
		adapter.toldAny = true;
		uint8_t const message[5] = { MSG_ADAPTER_STATUS, own->answering, own->starting, own->rate, own->size };
		send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
	}
	if (m->phase == DMG07_DATA && m->index == m->size) {
		uint8_t message[8] = { MSG_ADAPTER_PACKET, 0, 0, m->size };
		put16(message + 1, adapter.packet);
		memcpy(message + 4, &m->next[me * m->size], m->size);
		send(message, 4 + m->size, RETRO_NETPACKET_BROADCAST);
	}
	if (m->stop && !adapter.toldQuit) {
		adapter.toldQuit = true;
		uint8_t const message[1] = { MSG_ADAPTER_QUIT };
		send(message, sizeof(message), RETRO_NETPACKET_BROADCAST);
	}
}

// One transfer: this Game Boy, if waiting on the external clock, takes the adapter's byte and gives
// its own; the other players' ports give what they told.
static void adapterTransfer(void) {
#ifdef ARMGBA_GB
	struct Dmg07* const m = &adapter.machine;
	int const me = slotOf(self);
	uint8_t in[DMG07_PORTS] = { 0xFF, 0xFF, 0xFF, 0xFF };
	if ((libretroGbLinkControl() & 0x81) == 0x80) {
		in[me] = libretroGbLinkOut();
		libretroGbLinkIn(dmg07Out(m, me));
	}
	if (m->phase == DMG07_PING && (m->index == 2 || m->index == 3) && in[me] != 0xAA) {
		*(m->index == 2 ? &adapter.own.rate : &adapter.own.size) = in[me];
	}
	for (int port = 0; port < cableCount && port < DMG07_PORTS; ++port) {
		if (port != me) {
			in[port] = remoteIn(&adapter.ports[port]);
		}
	}
	if (m->phase == DMG07_DATA && m->index == 4 * m->size - 1) {
		fillPacket(me);
	}
	uint8_t const phase = m->phase;
	dmg07Transfer(m, in);
	// Packets are numbered from the start of each game.
	if (m->phase == DMG07_DATA && phase != DMG07_DATA) {
		adapter.packet = 0;
		for (int port = 0; port < DMG07_PORTS; ++port) {
			adapter.ports[port].starting = false;
		}
	} else if (phase == DMG07_DATA && m->index == 0) {
		++adapter.packet;
	}
	if (m->phase == DMG07_PING) {
		adapter.toldQuit = false;
	}
	tellOthers(me);
#endif
}

void linkAdapterRun(unsigned samples) {
	if (!linkAdapterOn()) {
		return;
	}
	dmg07EndFrame(&adapter.machine, (int32_t) samples);
	while (adapter.machine.at <= 0) {
		adapterTransfer();
	}
}
