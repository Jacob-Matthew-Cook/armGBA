/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "link.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/gba.h"
#include "gb.h"

// The link between two players' consoles in a netplay session, each running their own game: the
// cable, or the Game Boy Advance Wireless Adapter. Three or four players link only with all their
// consoles on one device (libretro.c), as waiting on several devices costs too much.
//
// The host keeps the cable: itself and the first player who joined with the same console. A
// transfer's data leaves the console clocking it as the transfer starts and is answered by the
// other's; the clocking console waits for the answer as the transfer ends, so emulated time stays
// exact on its side. How the other console's side is timed is the Link Timing option:
//
// - Standard: the other console runs at its own pace and takes the transfer when it next looks.
// - Exact: the console that clocked last leads, and the other stays just behind it, as far as the
//   leader says it has got. A transfer names the cycles it starts and ends on: the follower runs to
//   the start, answers with its data then, and takes the leader's data at the end, as on a cable.

enum {
	MAX_PEERS = 8,
	QUEUE = 16,
	// How often a Game Boy Advance looks for transfers the other clocked, in cycles.
	POLL_CYCLES = 4096,
	// How often the leader tells how far it has got: Game Boy Advance cycles, Game Boy samples.
	GRANT_CYCLES = 16384,
	GRANT_SAMPLES = 2048,
	// The furthest ahead a Game Boy Advance follower's stop is scheduled, well inside the event
	// timing's 32 bits; a later grant moves it.
	STOP_HORIZON = 0x10000000,
	// The longest Wireless Adapter message.
	AIR_BYTES = 100,
};

// A console that doesn't answer for this long has left; the transfer gets nothing.
static const int64_t TIMEOUT_NS = 3000000000LL;
// A follower waits this long for the leader before handing the frontend its frame.
static const int64_t FOLLOW_WAIT_NS = 50000000LL;

enum { CONSOLE_NONE, CONSOLE_GB, CONSOLE_GBA };
enum {
	MSG_HELLO = 1,
	MSG_ROSTER,
	MSG_REQUEST,
	MSG_REPLY,
	MSG_OFFER,
	MSG_MODE,
	MSG_GRANT,
	MSG_RFU,
	MSG_GONE,
};
// A request's mode: a Game Boy Advance serial mode, or a Game Boy transfer that took the other's
// offered byte.
enum { MODE_MULTI = 2, MODE_TOOK_OFFER = 0x80 };

static retro_environment_t environment;
static retro_netpacket_send_t sendPacket;
static retro_netpacket_poll_receive_t pollPackets;
static bool session;
static uint16_t self;
static int console;
static struct GBA* gba;
static bool wireless;

// The cable: this console and the other's, in the host's order, the host first.
static uint16_t cable[2];
static int cableCount;
// The players the host knows, and the console each runs.
static struct {
	uint16_t id;
	uint8_t console;
} peers[MAX_PEERS];
static int peerCount;

static struct SioLink sioLink = { .players = 1 };
static struct Event pollEvent;

// Transfers the other console clocked: taken when this one next looks, or with exact timing,
// answered at their start and taken at their end.
static struct Request {
	uint16_t seq;
	uint8_t mode;
	uint32_t data;
	bool timed;
	bool answered;
	// This console's data, as it answered.
	uint32_t mine;
	uint64_t start;
	uint64_t end;
} requests[QUEUE];
static int requestCount;

// A transfer this console clocked, waiting for the other's data.
static struct {
	bool waiting;
	bool answered;
	uint16_t seq;
	uint8_t mode;
	uint32_t out;
	uint32_t in;
} own;
static uint16_t nextSeq;

// Game Boy send-ahead, with standard timing: a Game Boy waiting for the other's clock offers the byte
// it will send, so the other can end its transfer at once instead of waiting a round trip. An offer
// names the last transfer its sender took from the other, so one made before a later one isn't used.
static struct {
	bool sent;
	bool ready;
	uint16_t after;
	uint8_t byte;
} offer, offered;
static uint16_t taken;

// Exact timing. Times are on a clock each console keeps for the link, Game Boy Advance cycles or
// Game Boy samples, which a follower sets from the leader's when it first hears it.
static struct {
	bool wanted;
	uint16_t leader;
	bool aligned;
	uint64_t origin;
	uint64_t granted;
	uint64_t told;
	// A Game Boy's samples run so far.
	uint64_t samples;
	struct Event stop;
} exact;

// The SD line, high while the other console is in multiplayer mode.
static bool otherMulti;
static bool ownMulti;
static bool ownMultiTold;

// The Wireless Adapter's air: messages to and from the other player.
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

static void put64(uint8_t* to, uint64_t value) {
	put32(to, value);
	put32(to + 4, value >> 32);
}

static uint16_t get16(const uint8_t* from) {
	return from[0] | from[1] << 8;
}

static uint32_t get32(const uint8_t* from) {
	return get16(from) | (uint32_t) get16(from + 2) << 16;
}

static uint64_t get64(const uint8_t* from) {
	return get32(from) | (uint64_t) get32(from + 4) << 32;
}

static void send(const uint8_t* message, size_t size) {
	if (session && sendPacket) {
		sendPacket(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, message, size, RETRO_NETPACKET_BROADCAST);
	}
}

static int64_t now(void) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000000000LL + time.tv_nsec;
}

// Reads the network until the condition holds, the session ends, or the time runs out. Returns
// whether the condition holds.
static bool waitUntil(bool (*ready)(void), int64_t limit) {
	int64_t const start = now();
	while (session && !ready() && now() - start < limit) {
		if (pollPackets) {
			pollPackets();
		}
		if (ready()) {
			break;
		}
		struct timespec pause = { 0, 100000 };
		nanosleep(&pause, NULL);
	}
	return ready();
}

// The cable.

static bool onCable(void) {
	return session && cableCount == 2 && (cable[0] == self || cable[1] == self);
}

static bool isPartner(uint16_t id) {
	return onCable() && id != self && (cable[0] == id || cable[1] == id);
}

// Tells the player when the cable changes.
static void announce(void) {
	static int shown = -1;
	int const state = !session ? -1 : onCable() ? 2 : cableCount;
	if (state == shown) {
		return;
	}
	char const* text = state == 2 ? (cable[0] == self ? "Link cable: player 1 of 2" : "Link cable: player 2 of 2")
	                 : shown == 2 ? "Link cable: the other player left"
	                 : state == 1 && self != cable[0] ? "Link cable: the host is playing a different console, or the cable is taken"
	                              : NULL;
	shown = state;
	if (text && environment) {
		struct retro_message message = { text, 240 };
		environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
}

static void setReady(bool ready) {
	if (ready != sioLink.ready) {
		sioLink.ready = ready;
		if (gba && gba->sio.link == &sioLink) {
			sioLinkChanged(gba);
		}
	}
}

static void cableChanged(void) {
	announce();
	setReady(onCable() && otherMulti);
	int const players = onCable() ? 2 : 1;
	int const id = onCable() && cable[1] == self ? 1 : 0;
	if (!gba || (sioLink.players == players && sioLink.id == id && gba->sio.link == (wireless ? NULL : &sioLink))) {
		return;
	}
	sioLink.players = players;
	sioLink.id = id;
	sioAttach(gba, wireless ? NULL : &sioLink);
}

// The host's cable: itself and the first player who joined with the same console.
static void hostRebuild(void) {
	cableCount = 0;
	if (console != CONSOLE_NONE) {
		cable[cableCount++] = self;
		for (int i = 0; i < peerCount && cableCount < 2; ++i) {
			if (peers[i].console == console) {
				cable[cableCount++] = peers[i].id;
			}
		}
	}
	uint8_t message[6] = { MSG_ROSTER, (uint8_t) cableCount };
	for (int i = 0; i < cableCount; ++i) {
		put16(message + 2 + 2 * i, cable[i]);
	}
	send(message, 2 + 2 * cableCount);
	cableChanged();
}

static void hello(void) {
	if (self == 0) {
		hostRebuild();
	} else {
		uint8_t message[2] = { MSG_HELLO, (uint8_t) console };
		send(message, sizeof(message));
	}
}

// The SD line: each console tells the other when it goes in or out of multiplayer mode, and again
// every so often for a player who joined since.
static void tellMode(bool again) {
	bool const multi = gba && sioMultiplayer(gba);
	if (session && (multi != ownMulti || !ownMultiTold || again)) {
		ownMulti = multi;
		ownMultiTold = true;
		uint8_t const message[2] = { MSG_MODE, multi };
		send(message, sizeof(message));
	}
}

// The Wireless Adapter's air.

static void airSend(const struct RfuAir* from, int to, const uint8_t* message, size_t size) {
	(void) from;
	(void) to;
	uint8_t packet[1 + AIR_BYTES] = { MSG_RFU };
	if (size <= AIR_BYTES) {
		memcpy(packet + 1, message, size);
		send(packet, size + 1);
	}
}

// Exact timing.

static bool exactOn(void) {
	return exact.wanted && onCable() && !wireless && (console == CONSOLE_GB || (console == CONSOLE_GBA && gba));
}

static bool leading(void) {
	return exact.leader == self;
}

// The console's own clock: a Game Boy Advance's cycles, or a Game Boy's samples.
static uint64_t consoleTime(void) {
	return console == CONSOLE_GBA && gba ? gba->timing.globalCycles + (uint32_t) gba->cpu.cycles : exact.samples;
}

static uint64_t linkTime(void) {
	return consoleTime() - exact.origin;
}

// A console sets its link clock the first time it leads, or hears the leader.
static void align(uint64_t leaderTime) {
	if (!exact.aligned) {
		exact.origin = consoleTime() - leaderTime;
		exact.aligned = true;
	}
}

// Where a request is next due: its start, to answer it, then its end, to take it.
static uint64_t dueAt(int i) {
	return requests[i].answered ? requests[i].end : requests[i].start;
}

// Where a follower stops next: where the leader has got to, or a transfer's start or end before.
static uint64_t stopAt(void) {
	uint64_t at = exact.granted;
	if (requestCount && requests[0].timed && dueAt(0) < at) {
		at = dueAt(0);
	}
	return at;
}

static bool following(void) {
	return exactOn() && !leading() && exact.aligned;
}

static bool mayRunOn(void) {
	return !following() || stopAt() > linkTime();
}

static uint64_t untilStop(void) {
	uint64_t const at = stopAt();
	uint64_t const time = linkTime();
	return at > time ? at - time : 0;
}

static void scheduleStop(void) {
	if (console != CONSOLE_GBA || !gba) {
		return;
	}
	timingDeschedule(gba, &exact.stop);
	if (following()) {
		uint64_t const wait = untilStop();
		timingSchedule(gba, &exact.stop, wait < STOP_HORIZON ? (int32_t) wait : STOP_HORIZON);
	}
}

static void heardGrant(uint64_t at) {
	align(at);
	exact.granted = at > exact.granted ? at : exact.granted;
	scheduleStop();
}

// The leader tells how far it has got, every so often.
static void grant(void) {
	if (!exactOn() || !leading()) {
		return;
	}
	align(0);
	uint64_t const at = linkTime();
	if (at - exact.told >= (uint64_t) (console == CONSOLE_GBA ? GRANT_CYCLES : GRANT_SAMPLES)) {
		exact.told = at;
		uint8_t message[9] = { MSG_GRANT };
		put64(message + 1, at);
		send(message, sizeof(message));
	}
}

// Transfers.

static void reply(uint16_t seq, uint32_t data, bool ready) {
	uint8_t message[8] = { MSG_REPLY, ready };
	put16(message + 2, seq);
	put32(message + 4, data);
	send(message, sizeof(message));
}

// This console's data for the other's transfer, and whether it takes part (waiting on the external
// clock, or a multiplayer child).
static uint32_t ownData(bool* ready) {
#ifdef ARMGBA_GB
	if (console == CONSOLE_GB) {
		*ready = true;
		return libretroGbLinkOut();
	}
#endif
	int mode;
	*ready = false;
	return gba ? sioOutgoing(gba, &mode, ready) : 0xFFFFFFFF;
}

// The other console's transfer ends here with its data, and this console's own as sent.
static void receiveData(uint8_t mode, uint32_t data, uint32_t mine) {
#ifdef ARMGBA_GB
	if (console == CONSOLE_GB) {
		libretroGbLinkIn(data & 0xFF);
		return;
	}
#endif
	if (!gba) {
		return;
	}
	uint32_t values[4] = { data, 0xFFFF, 0xFFFF, 0xFFFF };
	if (mode == MODE_MULTI) {
		values[0] = data & 0xFFFF;
		values[1] = mine & 0xFFFF;
	}
	sioReceive(gba, values);
}

static void dropRequest(void) {
	memmove(&requests[0], &requests[1], (requestCount - 1) * sizeof(requests[0]));
	--requestCount;
}

// Takes the transfers the other console clocked: with standard timing all of them; with exact
// timing those due, answering each at its start and taking it at its end. A Game Boy takes one
// transfer at a time, so its game can ready the next byte in between.
static void takeRequests(void) {
	while (requestCount) {
		if (requests[0].timed && dueAt(0) > linkTime()) {
			return;
		}
		if (!requests[0].answered) {
			bool ready;
			requests[0].mine = ownData(&ready);
			if (!(requests[0].mode & MODE_TOOK_OFFER)) {
				reply(requests[0].seq, requests[0].mine, ready);
			}
			requests[0].answered = true;
			if (requests[0].timed && requests[0].end > linkTime()) {
				continue;
			}
		}
		receiveData(requests[0].mode, requests[0].data, requests[0].mine);
		taken = requests[0].seq;
		dropRequest();
		if (console == CONSOLE_GB) {
			return;
		}
	}
}

// A follower at its stop: takes the transfers due there, and waits a little for the leader to get
// further. Returns whether it may run on; if not, the frame ends early and it stops again next frame.
static bool arrive(void) {
	takeRequests();
	if (!mayRunOn()) {
		waitUntil(mayRunOn, FOLLOW_WAIT_NS);
		takeRequests();
	}
	return mayRunOn();
}

static void stopReached(struct GBA* machine, void* context, uint32_t late) {
	(void) context;
	(void) late;
	if (!arrive()) {
		machine->returnRequested = true;
		timingInterrupt(machine);
		timingSchedule(machine, &exact.stop, 0);
		return;
	}
	scheduleStop();
}

// A transfer this console clocks starts: its data goes out, with exact timing naming the cycles the
// transfer starts and ends on, which makes this console the leader.
static void requestStart(int mode, uint32_t out, uint64_t start, uint64_t end) {
	own.waiting = true;
	own.answered = false;
	own.seq = ++nextSeq;
	own.mode = mode;
	own.out = out;
	own.in = 0xFFFFFFFF;
	uint8_t message[24] = { MSG_REQUEST, (uint8_t) mode };
	put16(message + 2, own.seq);
	put32(message + 4, out);
	if (!exactOn()) {
		send(message, 8);
		return;
	}
	exact.leader = self;
	align(0);
	put64(message + 8, start);
	put64(message + 16, end);
	send(message, sizeof(message));
	scheduleStop();
}

static bool ownAnswered(void) {
	return own.answered;
}

// The transfer this console clocked ends: the other's answer, waited for if it hasn't come.
static uint32_t requestEnd(void) {
	waitUntil(ownAnswered, TIMEOUT_NS);
	own.waiting = false;
	return own.in;
}

static void begin(void* context, int mode, uint32_t out, int32_t cycles) {
	(void) context;
	uint64_t const start = exactOn() ? linkTime() : 0;
	requestStart(mode, out, start, start + cycles);
}

static bool finish(void* context, uint32_t in[3]) {
	(void) context;
	uint32_t const data = requestEnd();
	in[0] = own.mode == MODE_MULTI ? data & 0xFFFF : data;
	return true;
}

// The other console's request arrives. Two consoles clocking at once swap their data, as the wires
// would. A request naming its cycles makes its sender the leader; one starting before where this
// console has got, as the lead passes to a console behind it, is taken at once.
static void heardRequest(uint16_t from, const uint8_t* message, size_t size) {
	offered.ready = false;
	if (own.waiting && own.mode != MODE_MULTI) {
		taken = get16(message + 2);
		own.in = get32(message + 4);
		own.answered = true;
		reply(get16(message + 2), own.out, true);
		return;
	}
	if (requestCount == QUEUE) {
		return;
	}
	struct Request* const request = &requests[requestCount++];
	*request = (struct Request) { .seq = get16(message + 2), .mode = message[1], .data = get32(message + 4) };
	if (size >= 24 && exactOn()) {
		request->start = get64(message + 8);
		request->end = get64(message + 16);
		exact.leader = from;
		align(request->start);
		request->timed = request->start >= linkTime();
		exact.granted = request->end > exact.granted ? request->end : exact.granted;
	}
	scheduleStop();
}

static void poll(void) {
	if (pollPackets) {
		pollPackets();
	}
	takeRequests();
}

// A Game Boy Advance's regular look at the cable.
static void gbaLook(void) {
	takeRequests();
	tellMode(false);
	grant();
}

static void pollEventRun(struct GBA* machine, void* context, uint32_t late) {
	(void) context;
	(void) late;
	if (pollPackets) {
		pollPackets();
	}
	gbaLook();
	if (onCable()) {
		timingSchedule(machine, &pollEvent, POLL_CYCLES);
	}
}

// Game Boy send-ahead: offers this Game Boy's byte while it waits for the other's clock, and says when
// it stops waiting.
static void offerByte(void) {
#ifdef ARMGBA_GB
	bool const ready = (libretroGbLinkControl() & 0x81) == 0x80;
	uint8_t const byte = libretroGbLinkOut();
	if (offer.sent && offer.ready == ready && (!ready || (offer.after == taken && offer.byte == byte))) {
		return;
	}
	offer.sent = true;
	offer.ready = ready;
	offer.after = taken;
	offer.byte = byte;
	uint8_t message[5] = { MSG_OFFER, ready, byte };
	put16(message + 3, taken);
	send(message, sizeof(message));
#endif
}

// The netplay session.

static void receive(const void* buffer, size_t size, uint16_t from) {
	const uint8_t* message = buffer;
	if (size < 2) {
		return;
	}
	switch (message[0]) {
	case MSG_HELLO:
		if (self == 0) {
			int i = 0;
			while (i < peerCount && peers[i].id != from) {
				++i;
			}
			if (i == peerCount && peerCount < MAX_PEERS) {
				peers[peerCount++].id = from;
			}
			if (i < peerCount) {
				peers[i].console = message[1];
				hostRebuild();
			}
		}
		break;
	case MSG_ROSTER:
		if (from == 0 && message[1] <= 2 && size >= 2u + 2u * message[1]) {
			cableCount = message[1];
			for (int i = 0; i < cableCount; ++i) {
				cable[i] = get16(message + 2 + 2 * i);
			}
			cableChanged();
		}
		break;
	case MSG_REQUEST:
		if (size >= 8 && isPartner(from)) {
			heardRequest(from, message, size);
		}
		break;
	case MSG_REPLY:
		if (size >= 8 && isPartner(from) && own.waiting && get16(message + 2) == own.seq) {
			own.in = message[1] ? get32(message + 4) : 0xFFFFFFFF;
			own.answered = true;
		}
		break;
	case MSG_OFFER:
		if (size >= 5 && isPartner(from)) {
			offered.ready = message[1];
			offered.byte = message[2];
			offered.after = get16(message + 3);
		}
		break;
	case MSG_MODE:
		if (isPartner(from)) {
			otherMulti = message[1];
			setReady(otherMulti);
		}
		break;
	case MSG_GRANT:
		if (size >= 9 && isPartner(from) && from == exact.leader) {
			heardGrant(get64(message + 1));
		}
		break;
	case MSG_RFU:
		if (gba && isPartner(from)) {
			rfuReceive(gba, from, message + 1, size - 1);
		}
		break;
	case MSG_GONE:
		if (from == 0 && size >= 3 && gba) {
			rfuPeerGone(gba, get16(message + 1));
		}
		break;
	}
}

static void resetExact(void) {
	struct Event const stop = exact.stop;
	bool const wanted = exact.wanted;
	if (gba) {
		timingDeschedule(gba, &exact.stop);
	}
	memset(&exact, 0, sizeof(exact));
	exact.stop = stop;
	exact.wanted = wanted;
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
	resetExact();
	otherMulti = false;
	ownMultiTold = false;
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
	resetExact();
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
	} else if (!onCable()) {
		hello();
	}
	tellMode(true);
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
			send(message, sizeof(message));
			if (gba) {
				rfuPeerGone(gba, id);
			}
			hostRebuild();
			return;
		}
	}
}

// What the libretro core asks of the link.

void linkSetEnvironment(retro_environment_t callback) {
	environment = callback;
	static const struct retro_netpacket_callback callbacks = {
		start, receive, stop, sessionPoll, connected, disconnected, "armGBA link 3",
	};
	callback(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*) &callbacks);
	sioLink.begin = begin;
	sioLink.finish = finish;
	pollEvent.callback = pollEventRun;
	pollEvent.priority = 0x90;
	exact.stop.callback = stopReached;
	exact.stop.priority = 0x7E;
}

static void consoleChanged(int kind) {
	console = kind;
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

void linkSetWireless(bool on) {
	if (wireless != on) {
		wireless = on;
		cableChanged();
	}
}

void linkSetExact(bool on) {
	if (exact.wanted != on) {
		exact.wanted = on;
		resetExact();
	}
}

const struct RfuAir* linkAir(void) {
	return &air;
}

bool linkSession(void) {
	return session;
}

bool linkExactOn(void) {
	return exactOn();
}

void linkGbaFrame(struct GBA* machine) {
	timingDeschedule(machine, &pollEvent);
	if (onCable()) {
		gbaLook();
		scheduleStop();
		timingSchedule(machine, &pollEvent, POLL_CYCLES);
	}
}

bool linkGbConnected(void) {
	return console == CONSOLE_GB && onCable();
}

int linkGbTransfer(int out) {
	if (exactOn() && own.waiting) {
		return requestEnd() & 0xFF;
	}
	if (offered.ready && offered.after == nextSeq && !exactOn()) {
		offered.ready = false;
		uint8_t message[8] = { MSG_REQUEST, MODE_TOOK_OFFER };
		put16(message + 2, ++nextSeq);
		put32(message + 4, out & 0xFF);
		send(message, sizeof(message));
		return offered.byte;
	}
	uint64_t const time = exactOn() ? linkTime() : 0;
	requestStart(0, out & 0xFF, time, time);
	return requestEnd() & 0xFF;
}

void linkGbClocking(int out, long left, long length) {
	if (exactOn() && !own.waiting) {
		uint64_t const end = linkTime() + left;
		requestStart(0, out & 0xFF, end - length, end);
	}
}

void linkGbPoll(void) {
	if (session) {
		poll();
		grant();
		if (linkGbConnected() && !exactOn()) {
			offerByte();
		}
	}
}

long linkGbAllowance(void) {
	if (!following()) {
		return LONG_MAX;
	}
	uint64_t const wait = untilStop();
	return wait < LONG_MAX ? (long) wait : LONG_MAX;
}

void linkGbRan(unsigned samples) {
	exact.samples += samples;
}

bool linkGbArrived(void) {
	return arrive();
}
