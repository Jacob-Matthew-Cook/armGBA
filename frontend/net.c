/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Netplay over TCP, the host in the middle: every message from a player goes to the host, which
// passes it on to the others.
//
// Separate: each device runs its own console, and the core's link cable messages travel as
// RetroArch's netpacket interface carries them; the devices aren't kept in step.
//
// Together: every device runs every console. Each device sends its player's buttons for a frame a
// few frames ahead (the input delay); a frame whose other players' buttons haven't come yet runs
// with their last ones, and when the real ones differ the device goes back to the state saved
// before that frame and runs the frames again. The host sends its state to a player who joins, and
// a check of the state every few seconds; a player whose state differs is sent the host's again.

#include "frontend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

enum {
	PEERS = 4,
	// Frames of buttons kept, and of states for going back.
	RING = 128,
	STATES = 20,
	// How far a device may run past the last frame it has every player's buttons for.
	AHEAD_MOST = 10,
	CHECK_FRAMES = 180,
	CHECKS = 8,
	MESSAGE_MOST = 32 << 20,
	VERSION = 1,
};

enum Message {
	MSG_HELLO = 1, // client: version, nickname, core version
	MSG_WELCOME,   // host: mode, your id, the game's name and CRC, options
	MSG_REFUSE,    // host: why
	MSG_READY,     // client, Together: the game and options are the host's
	MSG_STATE,     // host: frame, players, the state; their buttons from then follow
	MSG_PLAYER,    // host: a player joined or left
	MSG_INPUT,     // a player's buttons for frames from one on, and the frame its sender is at
	MSG_CHECK,     // host: a frame and its state's CRC
	MSG_RESYNC,    // client: send me your state
	MSG_PACKET,    // Separate: from, to, data
	MSG_PING,
	MSG_PONG,
};

enum NetMode netMode;
bool netTogether;
char netStatus[256];

// A connection and what's waiting to be read from it and sent on it.
struct Connection {
	int fd;
	int id;
	bool welcomed;
	bool wantsState;
	char nickname[32];
	uint8_t* in;
	size_t inSize, inCap;
	uint8_t* out;
	size_t outSize, outCap;
};

static int listener = -1;
// The host's connections to its players, or a client's to the host (connections[0]).
static struct Connection connections[PEERS];
static int self;
static unsigned pings;

static const struct retro_netpacket_callback* packets;
static bool packetsStarted;

// The options this device had before a session changed them, put back when it ends.
static struct {
	char* key;
	char* value;
} kept[OPTIONS_MAX];
static int keptCount;
// Set while a client loads the host's game, which would otherwise end the session.
static bool loadingForSession;

struct Check {
	int frame;
	uint32_t crc;
};

static struct {
	bool synced;
	int delay;
	// The next frame to run, and the one running.
	int frame;
	int running;
	// The first frame run with buttons that turned out wrong.
	int redoFrom;
	uint16_t buttons[PEERS][RING];
	uint16_t used[PEERS][RING];
	// Each player's buttons are known up to a frame, from the frame they joined to the one they left.
	int known[PEERS];
	int joined[PEERS];
	int left[PEERS];
	void* states[STATES];
	int stateFrames[STATES];
	size_t stateSize;
	int nextCheck;
	struct Check hostChecks[CHECKS], ownChecks[CHECKS];
	int hostFrame;
	Uint32 hostFrameAt;
	Uint32 rtt;
	unsigned ticks;
} roll;

// Messages: a 32-bit length, the type, then what it carries.

static void put16(uint8_t* p, unsigned v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
	put16(p, v & 0xFFFF);
	put16(p + 2, v >> 16);
}

static unsigned get16(const uint8_t* p) {
	return p[0] | p[1] << 8;
}

static uint32_t get32(const uint8_t* p) {
	return get16(p) | (uint32_t) get16(p + 2) << 16;
}

// A frame number from the host. Frames index the rings, so one below 0 is refused.
static bool isFrame(const uint8_t* p) {
	return (int) get32(p) >= 0;
}

// Room for need bytes in a connection's buffer, doubling it as often as that takes.
static bool grow(uint8_t** data, size_t* cap, size_t need) {
	if (need <= *cap) {
		return true;
	}
	size_t size = *cap ? *cap : 4096;
	while (size < need) {
		size *= 2;
	}
	uint8_t* bigger = realloc(*data, size);
	if (!bigger) {
		return false;
	}
	*data = bigger;
	*cap = size;
	return true;
}

// Sends what the socket will take now; the rest waits for the next time.
static void flushOne(struct Connection* c) {
	while (c->fd >= 0 && c->outSize) {
		ssize_t const n = send(c->fd, c->out, c->outSize, MSG_NOSIGNAL);
		if (n <= 0) {
			return;
		}
		memmove(c->out, c->out + n, c->outSize - n);
		c->outSize -= n;
	}
}

// Queues a message and sends what it can. Nothing is sent to no connection, or one closed.
static void sendTo(struct Connection* c, int type, const void* payload, size_t size) {
	if (!c || c->fd < 0 || !grow(&c->out, &c->outCap, c->outSize + 5 + size)) {
		return;
	}
	uint8_t* p = c->out + c->outSize;
	put32(p, (uint32_t) size + 1);
	p[4] = (uint8_t) type;
	if (size) {
		memcpy(p + 5, payload, size);
	}
	c->outSize += 5 + size;
	flushOne(c);
}

// The host's message to every player who has been welcomed but except.
static void sendOthers(int except, int type, const void* payload, size_t size) {
	for (int i = 0; i < PEERS; ++i) {
		if (connections[i].fd >= 0 && connections[i].welcomed && connections[i].id != except) {
			sendTo(&connections[i], type, payload, size);
		}
	}
}

static struct Connection* connectionFor(int id) {
	for (int i = 0; i < PEERS; ++i) {
		if (connections[i].fd >= 0 && connections[i].id == id) {
			return &connections[i];
		}
	}
	return NULL;
}

static void closeConnection(struct Connection* c) {
	if (c->fd >= 0) {
		close(c->fd);
	}
	free(c->in);
	free(c->out);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

// The netplay menu's status line.
static void setStatus(void) {
	if (netMode == NET_OFF) {
		netStatus[0] = '\0';
		return;
	}
	int const players = netPlayers();
	const char* how = netTogether ? "Together" : "Separate";
	if (netMode == NET_HOSTING) {
		snprintf(netStatus, sizeof(netStatus), players > 1 ? "Hosting %s: %d players" : "Hosting %s, waiting for players", how, players);
	} else if (!connections[0].welcomed) {
		snprintf(netStatus, sizeof(netStatus), "Joining...");
	} else {
		snprintf(netStatus, sizeof(netStatus), "Player %d, %s, %u ms", self + 1, how, (unsigned) roll.rtt);
	}
}

// Options.

// A core option the host's session needs, the device's own value kept to put back after it.
static void setOption(const char* key, const char* value) {
	struct Option* o = optionFind(key);
	if (!o || !strcmp(o->value, value)) {
		return;
	}
	bool keptAlready = false;
	for (int i = 0; i < keptCount; ++i) {
		keptAlready |= !strcmp(kept[i].key, key);
	}
	if (!keptAlready && keptCount < OPTIONS_MAX) {
		kept[keptCount].key = strdup(key);
		kept[keptCount].value = strdup(o->value);
		++keptCount;
	}
	optionSet(o, value);
}

static void restoreOptions(void) {
	for (int i = 0; i < keptCount; ++i) {
		struct Option* o = optionFind(kept[i].key);
		if (o) {
			optionSet(o, kept[i].value);
		}
		free(kept[i].key);
		free(kept[i].value);
	}
	keptCount = 0;
}

static const char* optionValue(const char* key) {
	struct Option* o = optionFind(key);
	return o ? o->value : "";
}

static const char* baseName(const char* path) {
	const char* slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

// Player 2's game is a path on the host; here it is the game of the same name beside this device's.
static bool localSecondGame(const char* hostValue, const char** value) {
	struct Option* o = optionFind("armgba_player2_game");
	*value = hostValue;
	if (!o || hostValue[0] != '/') {
		return true;
	}
	for (int i = 0; i < o->valueCount; ++i) {
		if (!strcmp(baseName(o->values[i]), baseName(hostValue))) {
			*value = o->values[i];
			return true;
		}
	}
	return false;
}

// The link mode a session needs; the game starts again when it changes.
static bool linkMode(const char* mode) {
	if (!strcmp(optionValue("armgba_link_mode"), mode)) {
		return false;
	}
	setOption("armgba_link_mode", mode);
	retro_reset();
	return true;
}

// The core's link cable (Separate).

// The core's link cable data for player to, or every player: a client's goes through the host. Each
// message carries who it's from and who it's for.
static void packetSend(int flags, const void* buf, size_t len, uint16_t to) {
	static uint8_t message[4 + 65536];
	(void) flags;
	if (len > 65536) {
		return;
	}
	put16(message, (unsigned) self);
	put16(message + 2, to);
	memcpy(message + 4, buf, len);
	if (netMode == NET_JOINED) {
		sendTo(&connections[0], MSG_PACKET, message, 4 + len);
	} else if (to == RETRO_NETPACKET_BROADCAST) {
		sendOthers(-1, MSG_PACKET, message, 4 + len);
	} else {
		sendTo(connectionFor(to), MSG_PACKET, message, 4 + len);
	}
}

static void pump(void);

// The core asks for what has come: everything read now.
static void packetPoll(void) {
	pump();
}

// The core's netpacket interface; a new one stops the old one's session.
bool netPacketInterface(const struct retro_netpacket_callback* callback) {
	if (packetsStarted && callback != packets) {
		packetsStarted = false;
		if (packets->stop) {
			packets->stop();
		}
	}
	packets = callback;
	return true;
}

// Starts the core's link cable session as this player; false if the core has no link cable.
static bool startPackets(void) {
	if (!packets) {
		return false;
	}
	if (!packetsStarted) {
		packetsStarted = true;
		packets->start((uint16_t) self, packetSend, packetPoll);
	}
	return true;
}

// Together: buttons and states.

// Whether player is in the game at frame.
static bool present(int player, int frame) {
	return frame >= roll.joined[player] && frame < roll.left[player];
}

// A player's buttons for a frame: as known, or the last known ones as a guess, or none while the
// player isn't in the game.
static uint16_t inputFor(int player, int frame) {
	if (!present(player, frame)) {
		return 0;
	}
	if (frame <= roll.known[player]) {
		return roll.buttons[player][frame % RING];
	}
	return roll.known[player] >= roll.joined[player] ? roll.buttons[player][roll.known[player] % RING] : 0;
}

// The last frame every player's buttons are known for.
static int confirmedFrame(void) {
	int frame = INT_MAX;
	for (int p = 0; p < PEERS; ++p) {
		if (roll.joined[p] != INT_MAX && roll.left[p] == INT_MAX) {
			frame = SDL_min(frame, roll.known[p]);
		}
	}
	return frame;
}

// A player's buttons for the frame after the last known. If a frame already run guessed otherwise,
// the frames from it are run again.
static void takeInput(int player, int frame, uint16_t buttons) {
	if (player < 0 || player >= PEERS || frame < 0 || frame - 1 != roll.known[player]) {
		return;
	}
	roll.buttons[player][frame % RING] = buttons;
	roll.known[player] = frame;
	if (frame < roll.frame && present(player, frame) && roll.used[player][frame % RING] != buttons) {
		roll.redoFrom = SDL_min(roll.redoFrom, frame);
	}
}

// The states kept for going back, at the core's state size.
static bool ensureStates(void) {
	size_t const size = retro_serialize_size();
	if (size == roll.stateSize && roll.states[0]) {
		return true;
	}
	for (int i = 0; i < STATES; ++i) {
		free(roll.states[i]);
		roll.states[i] = malloc(size);
		roll.stateFrames[i] = -1;
		if (!roll.states[i]) {
			return false;
		}
	}
	roll.stateSize = size;
	return true;
}

static void freeStates(void) {
	for (int i = 0; i < STATES; ++i) {
		free(roll.states[i]);
		roll.states[i] = NULL;
	}
	roll.stateSize = 0;
}

// A player's buttons for frames from through to, 64 frames to a message, to c or every player, with the
// frame this device is at.
static void sendInputs(int player, int from, int to, struct Connection* c) {
	for (int f = from; f <= to; f += 64) {
		int const count = SDL_min(64, to - f + 1);
		uint8_t message[10 + 64 * 2];
		message[0] = (uint8_t) player;
		put32(message + 1, (uint32_t) f);
		message[5] = (uint8_t) count;
		put32(message + 6, (uint32_t) roll.frame);
		for (int i = 0; i < count; ++i) {
			put16(message + 10 + i * 2, roll.buttons[player][(f + i) % RING]);
		}
		if (c) {
			sendTo(c, MSG_INPUT, message, 10 + count * 2);
		} else {
			sendOthers(-1, MSG_INPUT, message, 10 + count * 2);
		}
	}
}

// The state at the start of the first frame not every player's buttons are known for, with every
// player's buttons from then on. Made after any frames run again, so the state is right.
static void sendState(struct Connection* c) {
	int frame = SDL_min(confirmedFrame() + 1, roll.frame);
	void* state = roll.states[frame % STATES];
	bool made = false;
	if (frame == roll.frame || roll.stateFrames[frame % STATES] != frame) {
		frame = roll.frame;
		state = malloc(roll.stateSize);
		made = true;
		if (!state || !retro_serialize(state, roll.stateSize)) {
			free(state);
			return;
		}
	}
	size_t const header = 8 + PEERS * 12;
	uLongf packed = compressBound(roll.stateSize);
	uint8_t* message = malloc(header + packed);
	if (message && compress2(message + header, &packed, state, roll.stateSize, 1) == Z_OK) {
		put32(message, (uint32_t) frame);
		put32(message + 4, (uint32_t) roll.stateSize);
		for (int p = 0; p < PEERS; ++p) {
			put32(message + 8 + p * 12, (uint32_t) roll.joined[p]);
			put32(message + 12 + p * 12, (uint32_t) roll.left[p]);
			put32(message + 16 + p * 12, (uint32_t) roll.known[p]);
		}
		sendTo(c, MSG_STATE, message, header + packed);
		for (int p = 0; p < PEERS; ++p) {
			int const first = SDL_max(frame, roll.joined[p]);
			if (p != c->id && roll.joined[p] != INT_MAX && roll.known[p] >= first) {
				sendInputs(p, first, roll.known[p], c);
			}
		}
	}
	free(message);
	if (made) {
		free(state);
	}
}

// The host's state for a frame, and each player's frames in the game: this device starts from there.
static void takeState(const uint8_t* p, size_t size) {
	size_t const header = 8 + PEERS * 12;
	if (size < header || !isFrame(p)) {
		return;
	}
	for (int q = 0; q < PEERS; ++q) {
		if (!isFrame(p + 8 + q * 12) || !isFrame(p + 12 + q * 12)) {
			return;
		}
	}
	int const frame = (int) get32(p);
	uLongf length = get32(p + 4);
	if (!ensureStates() || length != roll.stateSize) {
		osd("Netplay: the host's game differs from this one");
		netStop();
		return;
	}
	void* state = roll.states[frame % STATES];
	if (uncompress(state, &length, p + header, size - header) != Z_OK || !retro_unserialize(state, roll.stateSize)) {
		osd("Netplay: the host's state didn't load");
		netStop();
		return;
	}
	for (int i = 0; i < STATES; ++i) {
		roll.stateFrames[i] = -1;
	}
	roll.stateFrames[frame % STATES] = frame;
	for (int q = 0; q < PEERS; ++q) {
		roll.joined[q] = (int) get32(p + 8 + q * 12);
		roll.left[q] = (int) get32(p + 12 + q * 12);
		// Their buttons from the frame on follow this message; this player's own are kept.
		if (q != self || !roll.synced) {
			roll.known[q] = SDL_max(frame, roll.joined[q]) - 1;
		}
	}
	roll.frame = frame;
	roll.redoFrom = INT_MAX;
	roll.nextCheck = (frame / CHECK_FRAMES + 1) * CHECK_FRAMES;
	memset(roll.hostChecks, 0xFF, sizeof(roll.hostChecks));
	memset(roll.ownChecks, 0xFF, sizeof(roll.ownChecks));
	roll.synced = true;
	setStatus();
}

// Runs the next frame with every player's buttons as known or guessed now.
static void runFrame(int avEnable) {
	int const frame = roll.frame;
	if (retro_serialize(roll.states[frame % STATES], roll.stateSize)) {
		roll.stateFrames[frame % STATES] = frame;
	}
	for (int p = 0; p < PEERS; ++p) {
		roll.used[p][frame % RING] = inputFor(p, frame);
	}
	roll.running = frame;
	int const was = app.avEnable;
	app.avEnable = avEnable;
	retro_run();
	app.avEnable = was;
	++roll.frame;
}

// This player's buttons for a frame the input delay ahead, then the next frame.
static void advance(int avEnable) {
	int const from = roll.known[self] + 1;
	while (roll.known[self] < roll.frame + roll.delay) {
		int const f = ++roll.known[self];
		roll.buttons[self][f % RING] = f == roll.frame + roll.delay ? localButtons(0) : 0;
	}
	if (from <= roll.known[self]) {
		sendInputs(self, from, roll.known[self], netMode == NET_JOINED ? &connections[0] : NULL);
	}
	runFrame(avEnable);
}

// The host's check and this device's for a frame, when both are in: if they differ, the host's state.
static void checkAgainst(int frame) {
	struct Check const* host = &roll.hostChecks[frame / CHECK_FRAMES % CHECKS];
	struct Check const* own = &roll.ownChecks[frame / CHECK_FRAMES % CHECKS];
	if (host->frame == frame && own->frame == frame && host->crc != own->crc) {
		roll.hostChecks[frame / CHECK_FRAMES % CHECKS].frame = -1;
		osd("Netplay: out of step, taking the host's state");
		sendTo(&connections[0], MSG_RESYNC, NULL, 0);
	}
}

// Every few seconds, the state at a frame every player's buttons before are known for.
static void checkStates(void) {
	int const frame = roll.nextCheck;
	if (frame >= roll.frame || confirmedFrame() < frame - 1) {
		return;
	}
	roll.nextCheck += CHECK_FRAMES;
	if (roll.stateFrames[frame % STATES] != frame) {
		return;
	}
	uint32_t const crc = (uint32_t) crc32(0, roll.states[frame % STATES], (uInt) roll.stateSize);
	if (netMode == NET_HOSTING) {
		uint8_t message[8];
		put32(message, (uint32_t) frame);
		put32(message + 4, crc);
		sendOthers(-1, MSG_CHECK, message, sizeof(message));
		return;
	}
	roll.ownChecks[frame / CHECK_FRAMES % CHECKS] = (struct Check) { frame, crc };
	checkAgainst(frame);
}

// A player joining, or out of step, gets the host's state.
static void hostSendStates(void) {
	for (int i = 0; i < PEERS; ++i) {
		struct Connection* c = &connections[i];
		if (c->fd < 0 || !c->wantsState) {
			continue;
		}
		c->wantsState = false;
		if (roll.joined[c->id] == INT_MAX || roll.left[c->id] != INT_MAX) {
			int const frame = SDL_min(confirmedFrame() + 1, roll.frame);
			roll.joined[c->id] = frame;
			roll.left[c->id] = INT_MAX;
			roll.known[c->id] = frame - 1;
			uint8_t message[9] = { (uint8_t) c->id };
			put32(message + 1, (uint32_t) frame);
			put32(message + 5, INT_MAX);
			sendOthers(c->id, MSG_PLAYER, message, sizeof(message));
		}
		sendState(c);
	}
}

// A frame of Together. The frames run with wrong buttons are run again first, from the state before
// them; with that state gone, a client asks for the host's. Nothing runs while waiting for the host's
// state or too far ahead of every player's buttons.
static void togetherFrame(void) {
	if (!roll.synced) {
		SDL_Delay(2);
		return;
	}
	if (roll.redoFrom < roll.frame) {
		int const to = roll.frame, from = roll.redoFrom;
		roll.redoFrom = INT_MAX;
		if (to - from >= STATES || roll.stateFrames[from % STATES] != from || !retro_unserialize(roll.states[from % STATES], roll.stateSize)) {
			if (netMode == NET_JOINED) {
				sendTo(&connections[0], MSG_RESYNC, NULL, 0);
			}
			return;
		}
		roll.frame = from;
		while (roll.frame < to) {
			runFrame(0);
		}
	}
	if (netMode == NET_HOSTING) {
		hostSendStates();
	}
	if (roll.frame - confirmedFrame() > AHEAD_MOST) {
		SDL_Delay(2);
		return;
	}
	// A player keeps in step with the host: a frame more when behind, none every other time when ahead.
	double behind = 0;
	if (netMode == NET_JOINED && roll.hostFrameAt) {
		behind = roll.hostFrame + (SDL_GetTicks() - roll.hostFrameAt + roll.rtt / 2.0) * FRAME_RATE / 1000 - roll.frame;
	}
	if (behind <= -2 && ++roll.ticks % 2) {
		return;
	}
	advance(RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO);
	if (behind >= 2 && roll.frame - confirmedFrame() <= AHEAD_MOST) {
		advance(RETRO_AV_ENABLE_AUDIO);
	}
	checkStates();
}

// What a frame's run reads.
uint16_t netInput(unsigned port, unsigned id) {
	(void) id;
	if (!netTogether) {
		return localButtons(port);
	}
	return port < PEERS ? roll.used[port][roll.running % RING] : 0;
}

// The session.

// A new session: no players yet, nothing known.
static void resetSync(void) {
	freeStates();
	memset(&roll, 0, sizeof(roll));
	for (int p = 0; p < PEERS; ++p) {
		roll.joined[p] = INT_MAX;
		roll.left[p] = INT_MAX;
		roll.known[p] = -1;
	}
	roll.redoFrom = INT_MAX;
	roll.delay = settings.inputDelay;
	roll.nextCheck = CHECK_FRAMES;
	memset(roll.hostChecks, 0xFF, sizeof(roll.hostChecks));
	memset(roll.ownChecks, 0xFF, sizeof(roll.ownChecks));
}

// The loaded game's CRC, to see that every device has the same game.
static uint32_t gameCrc(void) {
	return app.gameData ? (uint32_t) crc32(0, app.gameData, (uInt) app.gameSize) : 0;
}

// Together takes up to the core's link players; Separate is two.
static int playersMost(void) {
	return netTogether ? SDL_max(2, SDL_min(PEERS, atoi(optionValue("armgba_link_players")))) : 2;
}

// A listening socket on the netplay port, for IPv4 and IPv6 both when family is AF_INET6.
static int listenOn(int family) {
	int const fd = socket(family, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	int const yes = 1, no = 0;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in6 any6 = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t) settings.netPort), .sin6_addr = IN6ADDR_ANY_INIT };
	struct sockaddr_in any4 = { .sin_family = AF_INET, .sin_port = htons((uint16_t) settings.netPort), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (family == AF_INET6) {
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
	}
	bool const bound = family == AF_INET6 ? !bind(fd, (struct sockaddr*) &any6, sizeof(any6)) : !bind(fd, (struct sockaddr*) &any4, sizeof(any4));
	if (!bound || listen(fd, PEERS)) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

// Nonblocking, and without Nagle's delay, as messages are small and wanted at once.
static void useSocket(struct Connection* c, int fd) {
	int const yes = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
	fcntl(fd, F_SETFL, O_NONBLOCK);
	c->fd = fd;
}

// Hosts the loaded game, as Together or Separate; the game restarts if the link mode must change.
bool netHost(bool together) {
	netStop();
	if (!app.loaded) {
		return false;
	}
	listener = listenOn(AF_INET6);
	if (listener < 0) {
		listener = listenOn(AF_INET);
	}
	if (listener < 0) {
		osd("Netplay: can't use port %d", settings.netPort);
		return false;
	}
	netTogether = together;
	netMode = NET_HOSTING;
	self = 0;
	resetSync();
	if (linkMode(together ? "together" : "separate")) {
		osd(together ? "Restarted with every player's console" : "Restarted for the link cable");
	}
	if (together) {
		roll.joined[0] = 0;
		if (!ensureStates()) {
			netStop();
			return false;
		}
		roll.synced = true;
	} else if (!startPackets()) {
		osd("Netplay: this game can't use the link cable");
		netStop();
		return false;
	}
	setStatus();
	osd("Hosting on port %d", settings.netPort);
	return true;
}

// Connects to a host, waiting up to five seconds for each address the name has, then says hello. The
// host's welcome does the rest.
bool netJoin(const char* address) {
	netStop();
	if (!app.loaded) {
		osd("Load your game first");
		return false;
	}
	// name, name:port, [IPv6 address]:port, or an IPv6 address alone.
	char name[256], port[16];
	snprintf(name, sizeof(name), "%s", address);
	snprintf(port, sizeof(port), "%d", settings.netPort);
	char* host = name;
	char* colon = strrchr(name, ':');
	if (name[0] == '[') {
		host = name + 1;
		char* end = strchr(name, ']');
		if (end) {
			*end = '\0';
			if (end[1] == ':') {
				snprintf(port, sizeof(port), "%s", end + 2);
			}
		}
	} else if (colon && colon == strchr(name, ':')) {
		*colon = '\0';
		snprintf(port, sizeof(port), "%s", colon + 1);
	}
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *found = NULL;
	if (getaddrinfo(host, port, &hints, &found)) {
		osd("Netplay: can't find %s", host);
		return false;
	}
	int fd = -1;
	for (struct addrinfo* a = found; a && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) {
			continue;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		int error = 0;
		socklen_t length = sizeof(error);
		struct pollfd wait = { fd, POLLOUT, 0 };
		if ((connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) || poll(&wait, 1, 5000) != 1 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(found);
	if (fd < 0) {
		osd("Netplay: no answer from %s", address);
		return false;
	}
	useSocket(&connections[0], fd);
	connections[0].id = 0;
	netMode = NET_JOINED;
	resetSync();
	struct retro_system_info info;
	retro_get_system_info(&info);
	uint8_t message[1 + 32 + 64] = { VERSION };
	snprintf((char*) message + 1, 32, "%s", settings.nickname);
	snprintf((char*) message + 33, 64, "%s", info.library_version);
	sendTo(&connections[0], MSG_HELLO, message, sizeof(message));
	setStatus();
	return true;
}

// Ends the session: the connections close and the options the session changed are put back. Not
// while a client loads the host's game.
void netStop(void) {
	if (loadingForSession || netMode == NET_OFF) {
		return;
	}
	if (packetsStarted) {
		packetsStarted = false;
		if (packets && packets->stop) {
			packets->stop();
		}
	}
	for (int i = 0; i < PEERS; ++i) {
		closeConnection(&connections[i]);
	}
	if (listener >= 0) {
		close(listener);
		listener = -1;
	}
	freeStates();
	restoreOptions();
	netMode = NET_OFF;
	netTogether = false;
	setStatus();
}

// The players in the session, this device's included.
int netPlayers(void) {
	if (netMode == NET_OFF) {
		return 0;
	}
	int count = 1;
	if (netMode == NET_JOINED && netTogether) {
		count = 0;
		for (int p = 0; p < PEERS; ++p) {
			count += roll.joined[p] != INT_MAX && roll.left[p] == INT_MAX;
		}
	} else if (netMode == NET_HOSTING) {
		for (int i = 0; i < PEERS; ++i) {
			count += connections[i].fd >= 0 && connections[i].welcomed;
		}
	}
	return count;
}

bool netRunning(void) {
	return netMode != NET_OFF;
}

bool netRollingBack(void) {
	return netMode != NET_OFF && netTogether;
}

// The host.

// Tells a player why they can't join, then closes the connection.
static void refuse(struct Connection* c, const char* why) {
	sendTo(c, MSG_REFUSE, why, strlen(why) + 1);
	closeConnection(c);
}

// The lowest player number not taken, or -1 when the game is full.
static int freeId(void) {
	for (int id = 1; id < playersMost(); ++id) {
		if (!connectionFor(id)) {
			return id;
		}
	}
	return -1;
}

// A player's hello: the same armGBA version is needed, and room in the game. The welcome carries
// everything the player needs to play the host's game as the host has it.
static void hostHello(struct Connection* c, const uint8_t* p, size_t size) {
	struct retro_system_info info;
	retro_get_system_info(&info);
	if (size < 97 || p[0] != VERSION || strncmp((const char*) p + 33, info.library_version, 63)) {
		refuse(c, "The host runs another version of armGBA");
		return;
	}
	int const id = freeId();
	if (id < 0 || (!netTogether && packets && packets->connected && !packets->connected((uint16_t) id))) {
		refuse(c, "The game is full");
		return;
	}
	c->id = id;
	snprintf(c->nickname, sizeof(c->nickname), "%.31s", (const char*) p + 1);
	// Mode, id, the number of options, the game's CRC and name, then each option's key and value.
	size_t length = 8 + strlen(baseName(app.gamePath)) + 1;
	for (int i = 0; i < optionCount; ++i) {
		length += strlen(options[i].key) + strlen(options[i].value) + 2;
	}
	uint8_t* message = malloc(length);
	if (!message) {
		refuse(c, "The host is out of memory");
		return;
	}
	message[0] = netTogether;
	message[1] = (uint8_t) id;
	put16(message + 2, (unsigned) optionCount);
	put32(message + 4, gameCrc());
	char* at = (char*) message + 8;
	at += sprintf(at, "%s", baseName(app.gamePath)) + 1;
	for (int i = 0; i < optionCount; ++i) {
		at += sprintf(at, "%s", options[i].key) + 1;
		at += sprintf(at, "%s", options[i].value) + 1;
	}
	c->welcomed = true;
	sendTo(c, MSG_WELCOME, message, length);
	free(message);
	osd("%s joined", c->nickname);
	setStatus();
}

// A player's connection closed: the others are told, Together from the frame after their last
// buttons.
static void playerGone(struct Connection* c) {
	int const id = c->id;
	bool const welcomed = c->welcomed;
	char name[32];
	snprintf(name, sizeof(name), "%s", c->nickname);
	closeConnection(c);
	if (!welcomed) {
		return;
	}
	osd("%s left", name);
	if (netTogether) {
		if (roll.joined[id] != INT_MAX && roll.left[id] == INT_MAX) {
			roll.left[id] = roll.known[id] + 1;
			uint8_t message[9] = { (uint8_t) id };
			put32(message + 1, (uint32_t) roll.joined[id]);
			put32(message + 5, (uint32_t) roll.left[id]);
			sendOthers(id, MSG_PLAYER, message, sizeof(message));
		}
	} else if (packets && packets->disconnected) {
		packets->disconnected((uint16_t) id);
	}
	setStatus();
}

// A client.

// The host's game, beside this device's game or in the game list's folder. The host gives a file's
// name, never a path.
static bool loadHostGame(const char* name, uint32_t crc) {
	const char* folders[2] = { app.gamePath, settings.directory };
	if (strchr(name, '/')) {
		return false;
	}
	for (int i = 0; i < 2; ++i) {
		char path[FILE_SIZE];
		snprintf(path, sizeof(path), "%s", folders[i]);
		char* end = i == 0 ? strrchr(path, '/') : path + strlen(path);
		if (!end) {
			continue;
		}
		snprintf(end, sizeof(path) - (end - path), "/%s", name);
		loadingForSession = true;
		bool const loaded = access(path, R_OK) == 0 && loadGame(path);
		loadingForSession = false;
		if (loaded && gameCrc() == crc) {
			return true;
		}
	}
	return false;
}

// The text at *at, moving *at past it; NULL if it doesn't end before end.
static const char* takeText(const char** at, const char* end) {
	const char* const text = *at;
	size_t const length = strnlen(text, (size_t) (end - text));
	if (length == (size_t) (end - text)) {
		return NULL;
	}
	*at = text + length + 1;
	return text;
}

// The host's welcome. Together, the host's game is loaded if this device's isn't the same, and the
// host's options are taken; the host's state follows. Separate, the link cable starts.
static void clientWelcome(const uint8_t* p, size_t size) {
	const char* const end = (const char*) p + size;
	const char* at = (const char*) p + 8;
	const char* const game = size >= 8 ? takeText(&at, end) : NULL;
	if (!game || p[1] >= PEERS) {
		return;
	}
	netTogether = p[0];
	self = p[1];
	if (netTogether && gameCrc() != get32(p + 4) && !loadHostGame(game, get32(p + 4))) {
		osd("Netplay: you need %s to join", game);
		netStop();
		return;
	}
	for (unsigned i = 0; i < get16(p + 2) && netTogether; ++i) {
		const char* const key = takeText(&at, end);
		const char* value = key ? takeText(&at, end) : NULL;
		if (!value) {
			break;
		}
		if (!strcmp(key, "armgba_player2_game") && !localSecondGame(value, &value)) {
			osd("Netplay: you need %s beside your game", baseName(value));
			netStop();
			return;
		}
		setOption(key, value);
	}
	connections[0].welcomed = true;
	if (netTogether) {
		retro_reset();
		sendTo(&connections[0], MSG_READY, NULL, 0);
		osd("Joined as player %d", self + 1);
	} else {
		if (linkMode("separate")) {
			osd("Restarted for the link cable");
		}
		if (!startPackets()) {
			osd("Netplay: this game can't use the link cable");
			netStop();
			return;
		}
		osd("Joined the link cable");
	}
	setStatus();
}

// Messages as they come.

// One message. Everything from the network is checked before it is used. The host takes nothing but
// a hello from a player before welcoming them.
static void handle(struct Connection* c, int type, const uint8_t* p, size_t size) {
	bool const host = netMode == NET_HOSTING;
	if (host && !c->welcomed && type != MSG_HELLO) {
		return;
	}
	switch (type) {
	case MSG_HELLO:
		if (host && !c->welcomed) {
			hostHello(c, p, size);
		}
		break;
	case MSG_WELCOME:
		if (!host && !c->welcomed) {
			clientWelcome(p, size);
		}
		break;
	case MSG_REFUSE:
		if (!host) {
			osd("Netplay: %.*s", (int) strnlen((const char*) p, size), (const char*) p);
			netStop();
		}
		break;
	case MSG_READY:
	case MSG_RESYNC:
		if (host && netTogether) {
			c->wantsState = true;
		}
		break;
	case MSG_STATE:
		if (!host && netTogether) {
			takeState(p, size);
		}
		break;
	case MSG_PLAYER:
		if (!host && size >= 9 && p[0] < PEERS && p[0] != self && isFrame(p + 1) && isFrame(p + 5)) {
			int const q = p[0];
			roll.joined[q] = (int) get32(p + 1);
			roll.left[q] = (int) get32(p + 5);
			if (roll.left[q] == INT_MAX) {
				roll.known[q] = roll.joined[q] - 1;
			}
			setStatus();
		}
		break;
	case MSG_INPUT: {
		if (size < 10) {
			break;
		}
		int const player = p[0], first = (int) get32(p + 1), count = SDL_min(p[5], (int) (size - 10) / 2);
		if (host && player != c->id) {
			break;
		}
		for (int i = 0; i < count; ++i) {
			takeInput(player, first + i, (uint16_t) get16(p + 10 + i * 2));
		}
		if (host) {
			sendOthers(c->id, MSG_INPUT, p, size);
		} else if (player == 0) {
			roll.hostFrame = (int) get32(p + 6);
			roll.hostFrameAt = SDL_GetTicks();
		}
		break;
	}
	case MSG_CHECK:
		if (!host && size >= 8 && isFrame(p)) {
			int const frame = (int) get32(p);
			roll.hostChecks[frame / CHECK_FRAMES % CHECKS] = (struct Check) { frame, get32(p + 4) };
			checkAgainst(frame);
		}
		break;
	// The host passes link data for another player on, and keeps its own and a broadcast's.
	case MSG_PACKET: {
		if (size < 4 || !packetsStarted) {
			break;
		}
		int const from = host ? c->id : (int) get16(p);
		unsigned const to = get16(p + 2);
		if (host && to != 0) {
			uint8_t* relay = (uint8_t*) p;
			put16(relay, (unsigned) from);
			if (to == RETRO_NETPACKET_BROADCAST) {
				sendOthers(from, MSG_PACKET, relay, size);
			} else {
				sendTo(connectionFor((int) to), MSG_PACKET, relay, size);
			}
		}
		if (!host || to == 0 || to == RETRO_NETPACKET_BROADCAST) {
			packets->receive(p + 4, size - 4, (uint16_t) from);
		}
		break;
	}
	case MSG_PING:
		sendTo(c, MSG_PONG, p, size);
		break;
	case MSG_PONG:
		if (size >= 4) {
			roll.rtt = SDL_GetTicks() - get32(p);
			setStatus();
		}
		break;
	}
}

// Reads what has come, and handles each whole message; false when the connection is gone.
static bool readFrom(struct Connection* c) {
	// No more than the longest message at once, so a sender can't keep this reading.
	while (c->inSize < 4 + MESSAGE_MOST) {
		if (!grow(&c->in, &c->inCap, c->inSize + 65536)) {
			return false;
		}
		ssize_t const n = recv(c->fd, c->in + c->inSize, 65536, 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			return false;
		}
		if (n < 0) {
			break;
		}
		c->inSize += n;
	}
	size_t at = 0;
	while (c->inSize - at >= 5) {
		uint32_t const length = get32(c->in + at);
		if (!length || length > MESSAGE_MOST) {
			return false;
		}
		if (c->inSize - at < 4 + length) {
			break;
		}
		handle(c, c->in[at + 4], c->in + at + 5, length - 1);
		if (netMode == NET_OFF || c->fd < 0) {
			return true;
		}
		at += 4 + length;
	}
	memmove(c->in, c->in + at, c->inSize - at);
	c->inSize -= at;
	return true;
}

// Takes new players, sends what's queued and reads what's come. A client whose host left stops.
static void pump(void) {
	int fd;
	while (listener >= 0 && (fd = accept(listener, NULL, NULL)) >= 0) {
		struct Connection* c = NULL;
		for (int i = 0; i < PEERS && !c; ++i) {
			c = connections[i].fd < 0 ? &connections[i] : NULL;
		}
		if (c) {
			useSocket(c, fd);
			c->id = -1;
		} else {
			close(fd);
		}
	}
	for (int i = 0; i < PEERS && netMode != NET_OFF; ++i) {
		struct Connection* c = &connections[i];
		if (c->fd < 0) {
			continue;
		}
		flushOne(c);
		if (c->fd >= 0 && !readFrom(c) && netMode != NET_OFF) {
			if (netMode == NET_JOINED) {
				osd("Netplay: the host left");
				netStop();
				return;
			}
			playerGone(c);
		}
	}
}

// Runs a frame, as the session has it: a client pings the host every second for the round trip.
void netFrame(void) {
	pump();
	if (netMode == NET_OFF) {
		retro_run();
		return;
	}
	if (netMode == NET_JOINED && ++pings % 60 == 0) {
		uint8_t message[4];
		put32(message, SDL_GetTicks());
		sendTo(&connections[0], MSG_PING, message, sizeof(message));
	}
	if (netTogether) {
		togetherFrame();
		return;
	}
	if (packetsStarted && packets->poll) {
		packets->poll();
	}
	retro_run();
	for (int i = 0; i < PEERS; ++i) {
		flushOne(&connections[i]);
	}
}

// No connections yet.
void netInit(void) {
	for (int i = 0; i < PEERS; ++i) {
		connections[i].fd = -1;
	}
}
