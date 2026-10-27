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
#include "link.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

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
	VERSION = 3,
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
	MSG_CANT_JOIN, // client: why it's leaving, a CANT_ reason, and for CANT_GAME the game's name
	MSG_SAVE,      // client, Together: its player, the save's size, its save, then its clock's
	MSG_SAVE_IN,   // host: whether it went in, or why not
};

// Why a client can't join a host's session.
enum {
	CANT_FIRMWARE,       // Together: no Nintendo BIOS or boot ROM, the host's being
	CANT_OTHER_FIRMWARE, // Together: another Nintendo BIOS or boot ROM file
	CANT_NO_CARTRIDGE,    // Together: joining with no cartridge
	CANT_GAME,           // Together: not the host's game
};

enum NetMode netMode;
bool netTogether;
char netStatus[256];

// A connection and what's waiting to be read from it and sent on it.
struct Connection {
	int fd;
	// Its players, id to id + count - 1: one for each controller on the device, Together.
	int id;
	int count;
	bool welcomed;
	// A player who can't join, leaving.
	bool turnedAway;
	bool wantsState;
	char nickname[32];
	uint8_t* in;
	size_t inSize, inCap;
	uint8_t* out;
	size_t outSize, outCap;
};

// The host's listening socket, and its socket for probes from devices looking for hosts.
static int listener = -1, answerer = -1;
// The host's connections to its players, or a client's to the host (connections[0]).
static struct Connection connections[PEERS];
// This device's players, self to self + selfCount - 1, its controllers in order.
static int self, selfCount = 1;
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
// The host's welcome, kept while the player chooses their copy of the host's game.
static uint8_t* heldWelcome;
static size_t heldWelcomeSize;
static char heldGame[256];

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

static bool isLocal(int player) {
	return player >= self && player < self + selfCount;
}

static bool owns(const struct Connection* c, int player) {
	return c->fd >= 0 && player >= c->id && player < c->id + c->count;
}

// This device's controllers, a player each: up to the last one connected, at least one, as the
// keyboard plays player 1.
static int localPlayers(void) {
	int count = 1;
	for (int port = 1; port < PORTS; ++port) {
		count = app.pads[port] ? port + 1 : count;
	}
	return count;
}

// "player 2", "players 2 and 3" or "players 2 to 4".
static const char* playersText(int first, int count) {
	static char text[32];
	if (count <= 1) {
		snprintf(text, sizeof(text), "player %d", first + 1);
	} else {
		snprintf(text, sizeof(text), count == 2 ? "players %d and %d" : "players %d to %d", first + 1, first + count);
	}
	return text;
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
static const char* optionValue(const char* key);

const char* const netLinkNames[] = { "Together", "Separate", "Off" };

static int sessionLink(void) {
	return !netTogether ? LINK_SEPARATE : !strcmp(optionValue("armgba_link_mode"), "off") ? LINK_OFF : LINK_TOGETHER;
}

static void setStatus(void) {
	if (netMode == NET_OFF) {
		netStatus[0] = '\0';
		return;
	}
	int const players = netPlayers();
	const char* const how = netLinkNames[sessionLink()];
	if (netMode == NET_HOSTING) {
		snprintf(netStatus, sizeof(netStatus), players > 1 ? "Hosting, Link Mode %s: %d players" : "Hosting, Link Mode %s, waiting for players", how, players);
	} else if (!connections[0].welcomed) {
		snprintf(netStatus, sizeof(netStatus), "Joining...");
	} else {
		snprintf(netStatus, sizeof(netStatus), "%s, Link Mode %s, %u ms", playersText(self, selfCount), how, (unsigned) roll.rtt);
		netStatus[0] = 'P';
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

// This device's own save settings and save actions, which a host doesn't choose: they don't change
// how the game runs, only this device's save files.
static bool ownOption(const char* key) {
	static const char* const own[] = {
		"armgba_back_up_save", "armgba_load_save", "armgba_load_into", "armgba_send_save", "armgba_receive_into", "armgba_keep_progress", "armgba_keep_playing", "armgba_player1_saves",
		"armgba_keep_saves", "armgba_list_saves", "armgba_safe_saves",
	};
	for (size_t i = 0; i < sizeof(own) / sizeof(*own); ++i) {
		if (!strcmp(key, own[i])) {
			return true;
		}
	}
	return false;
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

// The link mode the running game started with, as Link Mode applies on Restart.
static char linkStarted[16];

void netGameStarted(void) {
	snprintf(linkStarted, sizeof(linkStarted), "%s", optionValue("armgba_link_mode"));
}

// The link mode a session needs; the game starts again when it started with another.
static bool linkMode(const char* mode) {
	if (!strcmp(optionValue("armgba_link_mode"), mode) && !strcmp(linkStarted, mode)) {
		return false;
	}
	setOption("armgba_link_mode", mode);
	retro_reset();
	netGameStarted();
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
			if (!owns(c, p) && roll.joined[p] != INT_MAX && roll.known[p] >= first) {
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
		osd("Netplay: the host's state didn't load; its armGBA may be another build");
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
		// Their buttons from the frame on follow this message; this device's own are kept.
		if (!isLocal(q) || !roll.synced) {
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

// This device's players' buttons for a frame the input delay ahead, each from its controller, then
// the next frame.
static void advance(int avEnable) {
	for (int k = 0; k < selfCount; ++k) {
		int const player = self + k, from = roll.known[player] + 1;
		while (roll.known[player] < roll.frame + roll.delay) {
			int const f = ++roll.known[player];
			roll.buttons[player][f % RING] = f == roll.frame + roll.delay ? localButtons((unsigned) k) : 0;
		}
		if (from <= roll.known[player]) {
			sendInputs(player, from, roll.known[player], netMode == NET_JOINED ? &connections[0] : NULL);
		}
	}
	runFrame(avEnable);
}

// The host's check and this device's for a frame, when both are in: if they differ, the host's state.
static void checkAgainst(int frame) {
	struct Check const* host = &roll.hostChecks[frame / CHECK_FRAMES % CHECKS];
	struct Check const* own = &roll.ownChecks[frame / CHECK_FRAMES % CHECKS];
	if (host->frame == frame && own->frame == frame && host->crc != own->crc) {
		roll.hostChecks[frame / CHECK_FRAMES % CHECKS].frame = -1;
		osdResync();
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
		int const frame = SDL_min(confirmedFrame() + 1, roll.frame);
		for (int id = c->id; id < c->id + c->count; ++id) {
			if (roll.joined[id] == INT_MAX || roll.left[id] != INT_MAX) {
				roll.joined[id] = frame;
				roll.left[id] = INT_MAX;
				roll.known[id] = frame - 1;
				uint8_t message[9] = { (uint8_t) id };
				put32(message + 1, (uint32_t) frame);
				put32(message + 5, INT_MAX);
				sendOthers(c->id, MSG_PLAYER, message, sizeof(message));
			}
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

// Together takes up to the core's link players, Off four and Separate two.
static int playersMost(void) {
	switch (sessionLink()) {
	case LINK_TOGETHER:
		return SDL_max(2, SDL_min(PEERS, atoi(optionValue("armgba_link_players"))));
	case LINK_OFF:
		return PEERS;
	default:
		return 2;
	}
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

static int answerOn(void);
static void hearBroadcasts(bool on);

// Hosts the loaded game, linked as Link Mode says or as given: Together, Separate, or Off for one
// console the players share. The game restarts if it started with another link mode.
bool netHost(const char* link) {
	char mode[16];
	// With no cartridge, Save Manager and single-pak games link Separate.
	snprintf(mode, sizeof(mode), "%s", app.noCartridge ? "separate" : link ? link : optionValue("armgba_link_mode"));
	bool const separate = !strcmp(mode, "separate"), oneConsole = !strcmp(mode, "off");
	// Every device runs every console, Together's or Off's one; Separate, each device its own.
	bool const together = !separate;
	netStop();
	app.sessionEnded = false;
	if (!app.loaded) {
		return false;
	}
	if (!separate && !oneConsole && strcmp(mode, "together")) {
		osd("Netplay: Link Mode can be Off, Together or Separate");
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
	netScanStop();
	answerer = answerOn();
	hearBroadcasts(answerer >= 0);
	netTogether = together;
	netMode = NET_HOSTING;
	self = 0;
	libretroSetNetplayPlayer(self);
	resetSync();
	selfCount = together ? SDL_min(localPlayers(), playersMost()) : 1;
	// Off: one console the players share, as Shared Controls says, or everyone's buttons at once.
	if (oneConsole && !strcmp(optionValue("armgba_shared_controls"), "off")) {
		setOption("armgba_shared_controls", "everyone");
	}
	if (linkMode(mode) && !app.noCartridge) {
		osd(separate ? "Restarted for the link cable" : oneConsole ? "Restarted with one console, shared" : "Restarted with every player's console");
	}
	if (together) {
		for (int p = 0; p < selfCount; ++p) {
			roll.joined[p] = 0;
		}
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
	if (selfCount > 1) {
		osd("Hosting on port %d, this device's controllers as %s", settings.netPort, playersText(self, selfCount));
	} else {
		osd("Hosting on port %d", settings.netPort);
	}
	return true;
}

// Connects to a host, waiting up to five seconds for each address the name has, then says hello. The
// host's welcome does the rest. With no cartridge, the core starts with no game first, for a host on
// Separate to send a single-pak game over the cable.
bool netJoin(const char* address, bool noCartridge) {
	netStop();
	app.sessionEnded = false;
	writeSaves();
	if (noCartridge && !app.noCartridge && !loadNoCartridge()) {
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
	netScanStop();
	useSocket(&connections[0], fd);
	connections[0].id = 0;
	connections[0].count = 1;
	netMode = NET_JOINED;
	resetSync();
	struct retro_system_info info;
	retro_get_system_info(&info);
	// The version, name, core version, how many players this device's controllers make, and whether
	// it has a game, its own or its cartridge removed.
	uint8_t message[1 + 32 + 64 + 2] = { VERSION };
	snprintf((char*) message + 1, 32, "%s", settings.nickname);
	snprintf((char*) message + 33, 64, "%s", info.library_version);
	message[97] = (uint8_t) localPlayers();
	message[98] = app.loaded;
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
	if (netMode == NET_JOINED) {
		libretroSessionEnded();
	}
	if (netInHostsGame() && app.loaded) {
		app.hostsGame = true;
		app.sessionEnded = netClosesOnLeaving();
		if (!app.sessionEnded) {
			libretroPlayOn();
			osd("Netplay: the session ended; you can play on, but from here only armGBA/Remote keeps this game's saves");
		}
	}
	for (int i = 0; i < PEERS; ++i) {
		closeConnection(&connections[i]);
	}
	if (listener >= 0) {
		close(listener);
		listener = -1;
	}
	if (answerer >= 0) {
		close(answerer);
		answerer = -1;
		hearBroadcasts(false);
	}
	freeStates();
	restoreOptions();
	if (heldWelcome) {
		free(heldWelcome);
		heldWelcome = NULL;
		uiPickGame(NULL, NULL, NULL);
	}
	netMode = NET_OFF;
	netTogether = false;
	selfCount = 1;
	libretroSetNetplayPlayer(-1);
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
		count = selfCount;
		for (int i = 0; i < PEERS; ++i) {
			count += connections[i].fd >= 0 && connections[i].welcomed ? connections[i].count : 0;
		}
	}
	return count;
}

bool netRunning(void) {
	return netMode != NET_OFF;
}

bool netInHostsGame(void) {
	return netMode == NET_JOINED && netTogether && roll.synced;
}

bool netClosesOnLeaving(void) {
	return netInHostsGame() && strcmp(optionValue("armgba_keep_playing"), "enabled");
}

bool netRollingBack(void) {
	return netMode != NET_OFF && netTogether;
}

// The host.

// An option as the session has it: a host running armGBA's firmware, its system folder having no
// Nintendo BIOS or boot ROM, has the players run armGBA's too.
static const char* sessionValue(const char* key, const char* value) {
	const char* firmware;
	bool const own = !libretroFirmwareCrc(&firmware);
	if (own && ((!strcmp(key, "armgba_bios") && !strcmp(value, "nintendo")) || (!strcmp(key, "armgba_gb_boot_rom") && !strcmp(value, "enabled")))) {
		return "open-source";
	}
	return value;
}

// Tells a player why they can't join, then closes the connection.
static void refuse(struct Connection* c, const char* why) {
	sendTo(c, MSG_REFUSE, why, strlen(why) + 1);
	closeConnection(c);
}

// The lowest player number not taken, or -1 when the game is full.
static bool taken(int id) {
	for (int i = 0; i < PEERS; ++i) {
		if (owns(&connections[i], id)) {
			return true;
		}
	}
	return isLocal(id);
}

// The lowest player number not taken, and how many free ones run on from it, up to wanted; -1 when
// the game is full.
static int freePlayers(int wanted, int* count) {
	int const most = playersMost();
	for (int id = 0; id < most; ++id) {
		if (!taken(id)) {
			*count = 1;
			while (*count < wanted && id + *count < most && !taken(id + *count)) {
				++*count;
			}
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
	// Separate, each device plays its own game.
	if (!netTogether && size >= 99 && !p[98]) {
		osd("Netplay: %.31s can't join: on Link Mode Separate they need a game loaded", (const char*) p + 1);
		refuse(c, "The host is on Link Mode Separate: load your own game, or set Your Cartridge to Removed");
		return;
	}
	// Together, a player for each of the device's controllers, as many as there's room for.
	int const wanted = netTogether && size >= 98 ? SDL_max(1, SDL_min(PEERS, p[97])) : 1;
	int count = 0;
	int const id = freePlayers(wanted, &count);
	if (id < 0 || (!netTogether && packets && packets->connected && !packets->connected((uint16_t) id))) {
		refuse(c, "The game is full");
		return;
	}
	c->id = id;
	c->count = count;
	snprintf(c->nickname, sizeof(c->nickname), "%.31s", (const char*) p + 1);
	// Mode, first player, the number of options, the game's CRC, the firmware's CRC, how many players,
	// and the game's name, then each option's key and value.
	size_t length = 13 + strlen(baseName(app.gamePath)) + 1;
	for (int i = 0; i < optionCount; ++i) {
		length += strlen(options[i].key) + strlen(sessionValue(options[i].key, options[i].value)) + 2;
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
	const char* firmware;
	put32(message + 8, libretroFirmwareCrc(&firmware));
	message[12] = (uint8_t) count;
	char* at = (char*) message + 13;
	at += sprintf(at, "%s", baseName(app.gamePath)) + 1;
	for (int i = 0; i < optionCount; ++i) {
		at += sprintf(at, "%s", options[i].key) + 1;
		at += sprintf(at, "%s", sessionValue(options[i].key, options[i].value)) + 1;
	}
	c->welcomed = true;
	sendTo(c, MSG_WELCOME, message, length);
	free(message);
	// Together, a player has joined once their firmware is the host's and they're ready.
	if (!netTogether) {
		osd("%s joined", c->nickname);
	}
	setStatus();
}

// A player's connection closed: the others are told, Together from the frame after their last
// buttons.
static void playerGone(struct Connection* c) {
	int const id = c->id, count = c->count;
	bool const welcomed = c->welcomed;
	bool const turnedAway = c->turnedAway;
	char name[32];
	snprintf(name, sizeof(name), "%s", c->nickname);
	closeConnection(c);
	if (!welcomed) {
		return;
	}
	if (!turnedAway) {
		osd("%s left", name);
	}
	if (netTogether) {
		for (int q = id; q < id + count; ++q) {
			if (roll.joined[q] != INT_MAX && roll.left[q] == INT_MAX) {
				roll.left[q] = roll.known[q] + 1;
				uint8_t message[9] = { (uint8_t) q };
				put32(message + 1, (uint32_t) roll.joined[q]);
				put32(message + 5, (uint32_t) roll.left[q]);
				sendOthers(id, MSG_PLAYER, message, sizeof(message));
			}
		}
	} else if (packets && packets->disconnected) {
		packets->disconnected((uint16_t) id);
	}
	setStatus();
}

// A client.

// Tells the host why this device can't join, before it leaves.
static void cantJoin(int why, const char* game) {
	uint8_t message[1 + 256] = { (uint8_t) why };
	size_t const length = game ? strnlen(game, 255) : 0;
	memcpy(message + 1, game ? game : "", length);
	sendTo(&connections[0], MSG_CANT_JOIN, message, 1 + length);
}

// The host's game, beside this device's game or in the game list's folder. The host gives a file's
// name, never a path.
static bool tryHostGame(const char* path, uint32_t crc) {
	loadingForSession = true;
	bool const loaded = access(path, R_OK) == 0 && loadGame(path);
	loadingForSession = false;
	return loaded && gameCrc() == crc;
}

// The host's game by its name in a folder, or in its subfolders depth levels down.
static bool findHostGame(const char* folder, const char* name, uint32_t crc, int depth) {
	char path[FILE_SIZE];
	if (!folder[0]) {
		return false;
	}
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	if (tryHostGame(path, crc)) {
		return true;
	}
	DIR* const entries = depth > 0 ? opendir(folder) : NULL;
	struct dirent* entry;
	bool found = false;
	while (entries && !found && (entry = readdir(entries))) {
		struct stat info;
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		bool const folderHere = entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN && !stat(path, &info) && S_ISDIR(info.st_mode));
		found = entry->d_name[0] != '.' && folderHere && findHostGame(path, name, crc, depth - 1);
	}
	if (entries) {
		closedir(entries);
	}
	return found;
}

static void clientWelcome(const uint8_t* p, size_t size);

// A file chosen as the host's game: the welcome goes on if it is.
static bool pickedHostGame(const char* path) {
	if (!tryHostGame(path, get32(heldWelcome + 4))) {
		osd("Netplay: that isn't %s, the host's game", heldGame);
		return false;
	}
	uint8_t* const welcome = heldWelcome;
	heldWelcome = NULL;
	clientWelcome(welcome, heldWelcomeSize);
	free(welcome);
	return true;
}

// Leaving the game list without the host's game: the player can't join.
static void heldWelcomeLeft(void) {
	if (heldWelcome && !uiPickingGame()) {
		cantJoin(CANT_GAME, heldGame);
		osd("Netplay: you need %s to join", heldGame);
		netStop();
	}
}

// The host's game: the file of its name beside this device's game, in the game list's folder, the
// Game Boy, Game Boy Color and Game Boy Advance folders, or the Games folder and its subfolders, and
// only if its CRC is the host's game's. The host gives a file's name, never a path.
static bool loadHostGame(const char* name, uint32_t crc) {
	if (strchr(name, '/')) {
		return false;
	}
	char own[PATH_SIZE], beside[PATH_SIZE];
	snprintf(own, sizeof(own), "%s", app.loaded ? app.gamePath : "");
	snprintf(beside, sizeof(beside), "%s", app.gamePath);
	char* const slash = strrchr(beside, '/');
	*(slash ? slash : beside) = '\0';
	bool found = findHostGame(beside, name, crc, 0) || findHostGame(settings.directory, name, crc, 0);
	for (int c = 0; c < CONSOLES && !found; ++c) {
		found = findHostGame(consoleDirectories[c], name, crc, 1);
	}
	found = found || findHostGame(gamesDirectory, name, crc, 2);
	// None was the host's: this device's own game again, not a file of that name it tried.
	if (!found && app.loaded && strcmp(app.gamePath, own)) {
		loadingForSession = true;
		if (!*own || !loadGame(own)) {
			unloadGame();
		}
		loadingForSession = false;
	}
	return found;
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
	const char* at = (const char*) p + 13;
	const char* const game = size >= 13 ? takeText(&at, end) : NULL;
	if (!game || p[1] >= PEERS) {
		return;
	}
	netTogether = p[0];
	self = p[1];
	selfCount = netTogether ? SDL_max(1, SDL_min(PEERS - self, p[12])) : 1;
	libretroSetNetplayPlayer(self);
	// Every device runs every console in Together and Off, so each needs the game.
	if (netTogether && app.noCartridge) {
		cantJoin(CANT_NO_CARTRIDGE, NULL);
		osd("Netplay: joining with your cartridge removed needs a host on Link Mode Separate");
		netStop();
		return;
	}
	if (netTogether && gameCrc() != get32(p + 4) && !loadHostGame(game, get32(p + 4))) {
		// Not found by its name: the player can choose their copy, under another name.
		heldWelcome = malloc(size);
		if (heldWelcome) {
			memcpy(heldWelcome, p, size);
			heldWelcomeSize = size;
			snprintf(heldGame, sizeof(heldGame), "%s", game);
			osd("Netplay: %s isn't in your folders by that name: choose your copy", game);
			uiPickGame("Choose Your Copy", game, pickedHostGame);
			return;
		}
		cantJoin(CANT_GAME, game);
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
		if (ownOption(key)) {
			continue;
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
		netGameStarted();
		// Every device runs every console, so on the same firmware: the host's Nintendo BIOS or boot
		// ROM must be in this device's system folder too. It is never sent.
		const char* firmware;
		uint32_t const own = libretroFirmwareCrc(&firmware);
		if (own != get32(p + 8)) {
			bool const other = own != 0;
			cantJoin(other ? CANT_OTHER_FIRMWARE : CANT_FIRMWARE, NULL);
			if (other) {
				osd("Netplay: the host's Nintendo %s isn't the same file as yours. Both can switch to armGBA's in Core > System",
				    firmware);
			} else {
				osd("Netplay: the host plays with Nintendo's %s, which you don't have. The host can switch to armGBA's in Core > System",
				    firmware);
			}
			netStop();
			return;
		}
		sendTo(&connections[0], MSG_READY, NULL, 0);
		if (selfCount < localPlayers()) {
			osd("Joined as %s; the game had no room for your other controllers", playersText(self, selfCount));
		} else {
			osd("Joined as %s", playersText(self, selfCount));
		}
	} else {
		if (linkMode("separate") && !app.noCartridge) {
			osd("Restarted for the link cable");
		}
		if (!startPackets()) {
			osd("Netplay: this game can't use the link cable");
			netStop();
			return;
		}
		osd(app.noCartridge ? "Joined" : "Joined the link cable");
	}
	setStatus();
}

// Send My Save: a joining player's own save of the game, put in their console on the host, which
// restarts it; every device then takes the host's state, the states from before gone.
bool netCanSendSave(void) {
	return netMode == NET_JOINED && sessionLink() == LINK_TOGETHER && roll.synced;
}

void netSendSave(void) {
	char file[FILE_SIZE], rtcFile[FILE_SIZE];
	size_t size, rtcSize;
	savePath(file, "srm");
	savePath(rtcFile, "rtc");
	uint8_t* save = readWholeFile(file, &size);
	uint8_t* rtc = readWholeFile(rtcFile, &rtcSize);
	uint8_t* message = save ? malloc(5 + size + rtcSize) : NULL;
	if (!save) {
		osd("Netplay: you have no save of this game to send");
	} else if (message && netCanSendSave()) {
		message[0] = (uint8_t) self;
		put32(message + 1, (uint32_t) size);
		memcpy(message + 5, save, size);
		if (rtc) {
			memcpy(message + 5 + size, rtc, rtcSize);
		}
		sendTo(&connections[0], MSG_SAVE, message, 5 + size + rtcSize);
		osd("Netplay: sending your save...");
	}
	free(message);
	free(rtc);
	free(save);
}

static void hostTakeSave(struct Connection* c, const uint8_t* p, size_t size) {
	int const player = p[0];
	size_t const saveSize = get32(p + 1);
	const char* why = sessionLink() != LINK_TOGETHER ? "the host isn't on Link Mode Together"
	                                                 : libretroTakeSave(player, p + 5, saveSize, p + 5 + saveSize, size - 5 - saveSize);
	uint8_t answer[128] = { why == NULL };
	snprintf((char*) answer + 1, sizeof(answer) - 1, "%s", why ? why : "");
	sendTo(c, MSG_SAVE_IN, answer, 2 + strlen((const char*) answer + 1));
	if (why) {
		osd("Netplay: player %d's save couldn't be loaded: %s", player + 1, why);
		return;
	}
	for (int i = 0; i < STATES; ++i) {
		roll.stateFrames[i] = -1;
	}
	for (int i = 0; i < PEERS; ++i) {
		struct Connection* const other = &connections[i];
		other->wantsState |= other->fd >= 0 && roll.joined[other->id] != INT_MAX && roll.left[other->id] == INT_MAX;
	}
	osd("Netplay: player %d sent their save; their console restarted with it", player + 1);
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
	case MSG_CANT_JOIN:
		if (host && size >= 1) {
			const char* firmware;
			libretroFirmwareCrc(&firmware);
			c->turnedAway = true;
			if (p[0] == CANT_OTHER_FIRMWARE) {
				osd("Netplay: %s can't join: their Nintendo %s isn't the same as yours. Both can switch to armGBA's in Core > System",
				    c->nickname, firmware);
			} else if (p[0] == CANT_FIRMWARE) {
				osd("Netplay: %s can't join without your Nintendo %s. To play together, switch to armGBA's in Core > System", c->nickname,
				    firmware);
			} else if (p[0] == CANT_NO_CARTRIDGE) {
				osd("Netplay: %s can't join with their cartridge removed: Together and Off need the game on every device", c->nickname);
			} else {
				osd("Netplay: %s can't join: they don't have %.*s", c->nickname, (int) SDL_min(size - 1, 100), (const char*) p + 1);
			}
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
			if (type == MSG_READY) {
				osd("%s joined as %s", c->nickname, playersText(c->id, c->count));
			}
		}
		break;
	case MSG_STATE:
		if (!host && netTogether) {
			takeState(p, size);
		}
		break;
	case MSG_PLAYER:
		if (!host && size >= 9 && p[0] < PEERS && !isLocal(p[0]) && isFrame(p + 1) && isFrame(p + 5)) {
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
		if (size < 10 || !isFrame(p + 1) || get32(p + 1) > INT_MAX - 255u) {
			break;
		}
		int const player = p[0], first = (int) get32(p + 1), count = SDL_min(p[5], (int) (size - 10) / 2);
		if (host ? !owns(c, player) : isLocal(player)) {
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
	case MSG_SAVE:
		if (host && netTogether && size >= 5 && owns(c, p[0]) && get32(p + 1) <= size - 5) {
			hostTakeSave(c, p, size);
		}
		break;
	case MSG_SAVE_IN:
		if (!host && size >= 2) {
			if (p[0]) {
				osd("Netplay: your save is loaded; your console restarted with it");
			} else {
				osd("Netplay: your save couldn't be loaded: %.*s", (int) strnlen((const char*) p + 1, size - 1), (const char*) p + 1);
			}
		}
		break;
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
	// No more than the longest message at once, so a sender can't keep this reading. Messages that came
	// before the connection closed are still handled, as a refusal's reason.
	bool open = true;
	while (c->inSize < 4 + MESSAGE_MOST) {
		if (!grow(&c->in, &c->inCap, c->inSize + 65536)) {
			return false;
		}
		ssize_t const n = recv(c->fd, c->in + c->inSize, 65536, 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			open = false;
			break;
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
	return open;
}

// Takes new players, sends what's queued and reads what's come. A client whose host left stops.
static void answerProbes(void);

static void pump(void) {
	heldWelcomeLeft();
	if (netMode == NET_OFF) {
		return;
	}
	answerProbes();
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

// Finding hosts on the local network: a device looking broadcasts a probe to the netplay port each
// second, and hosts answer with who they are and what they're hosting.

static const char probeTag[8] = "armGBA?", answerTag[8] = "armGBA!";

enum {
	// The tag, version, how, players, most players, port, nickname, game and Shared Controls.
	ANSWER_SIZE = 8 + 4 + 2 + 32 + 128 + 1,
	FOUND_FOR = 3000,
};

static int scanner = -1;
static struct NetHost found[NET_HOSTS_MOST];
static int foundCount;
static Uint32 probedAt, scannedAt;

// Android drops broadcasts unless the app asks for them (Armgba.java), which a host does.
static void hearBroadcasts(bool on) {
#ifdef __ANDROID__
	JNIEnv* const env = SDL_AndroidGetJNIEnv();
	jclass const armgba = env ? (*env)->FindClass(env, "org/armgba/app/Armgba") : NULL;
	jmethodID const hear = armgba ? (*env)->GetStaticMethodID(env, armgba, "hearBroadcasts", "(Z)V") : NULL;
	if (hear) {
		(*env)->CallStaticVoidMethod(env, armgba, hear, (jboolean) on);
	}
	if (env && (*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	}
	if (armgba) {
		(*env)->DeleteLocalRef(env, armgba);
	}
#else
	(void) on;
#endif
}

static int udpSocket(void) {
	int const fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		fcntl(fd, F_SETFL, O_NONBLOCK);
	}
	return fd;
}

// A host's socket for probes, on the netplay port.
static int answerOn(void) {
	int const fd = udpSocket();
	int const yes = 1;
	struct sockaddr_in any = { .sin_family = AF_INET, .sin_port = htons((uint16_t) settings.netPort), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (fd >= 0 && (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) || bind(fd, (struct sockaddr*) &any, sizeof(any)))) {
		close(fd);
		return -1;
	}
	return fd;
}

static void answerProbes(void) {
	uint8_t probe[16];
	struct sockaddr_storage from;
	socklen_t length = sizeof(from);
	ssize_t n;
	while (answerer >= 0 && (n = recvfrom(answerer, probe, sizeof(probe), 0, (struct sockaddr*) &from, &length)) >= 0) {
		if (n == 9 && !memcmp(probe, probeTag, 8) && probe[8] == VERSION) {
			uint8_t answer[ANSWER_SIZE] = { 0 };
			memcpy(answer, answerTag, 8);
			answer[8] = VERSION;
			answer[9] = (uint8_t) sessionLink();
			answer[10] = (uint8_t) netPlayers();
			answer[11] = (uint8_t) playersMost();
			put16(answer + 12, (unsigned) settings.netPort);
			snprintf((char*) answer + 14, 32, "%.31s", settings.nickname);
			snprintf((char*) answer + 46, 128, "%.127s", app.gameName);
			const char* const shared = optionValue("armgba_shared_controls");
			answer[174] = !strcmp(shared, "everyone") ? 1 : !strcmp(shared, "pass around") ? 2 : 0;
			sendto(answerer, answer, sizeof(answer), 0, (struct sockaddr*) &from, length);
		}
		length = sizeof(from);
	}
}

// To every network's broadcast address, as 255.255.255.255 leaves by only one.
static void probe(void) {
	uint8_t message[9];
	memcpy(message, probeTag, 8);
	message[8] = VERSION;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t) settings.netPort), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
	sendto(scanner, message, sizeof(message), 0, (struct sockaddr*) &to, sizeof(to));
	struct ifaddrs* interfaces;
	if (getifaddrs(&interfaces)) {
		return;
	}
	for (struct ifaddrs* i = interfaces; i; i = i->ifa_next) {
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && i->ifa_broadaddr && (i->ifa_flags & IFF_BROADCAST) && (i->ifa_flags & IFF_UP)) {
			to.sin_addr = ((struct sockaddr_in*) i->ifa_broadaddr)->sin_addr;
			sendto(scanner, message, sizeof(message), 0, (struct sockaddr*) &to, sizeof(to));
		}
	}
	freeifaddrs(interfaces);
}

static void takeAnswers(Uint32 now) {
	uint8_t answer[ANSWER_SIZE + 1];
	struct sockaddr_in from;
	socklen_t length = sizeof(from);
	ssize_t n;
	while ((n = recvfrom(scanner, answer, sizeof(answer), 0, (struct sockaddr*) &from, &length)) >= 0) {
		length = sizeof(from);
		if (n != ANSWER_SIZE || memcmp(answer, answerTag, 8) || answer[8] != VERSION || answer[9] > LINK_OFF || from.sin_family != AF_INET) {
			continue;
		}
		char ip[INET_ADDRSTRLEN], address[sizeof(found[0].address)];
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
		snprintf(address, sizeof(address), "%s:%u", ip, get16(answer + 12));
		int i = 0;
		while (i < foundCount && strcmp(found[i].address, address)) {
			++i;
		}
		if (i == NET_HOSTS_MOST) {
			continue;
		}
		foundCount += i == foundCount;
		struct NetHost* h = &found[i];
		snprintf(h->address, sizeof(h->address), "%s", address);
		h->how = answer[9];
		h->players = answer[10];
		h->most = answer[11];
		snprintf(h->nickname, sizeof(h->nickname), "%.31s", (const char*) answer + 14);
		snprintf(h->game, sizeof(h->game), "%.127s", (const char*) answer + 46);
		h->shared = answer[174] % 3;
		h->seen = now;
	}
}

// Looks for hosts while it's called, as each frame the Join list is shown: a probe each second, and a
// host not heard from in three dropped.
int netScan(const struct NetHost** hosts) {
	Uint32 const now = SDL_GetTicks();
	scannedAt = now;
	if (scanner < 0) {
		int const yes = 1;
		scanner = udpSocket();
		if (scanner < 0) {
			return 0;
		}
		setsockopt(scanner, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
		probedAt = now - 1000;
	}
	if (now - probedAt >= 1000) {
		probedAt = now;
		probe();
	}
	takeAnswers(now);
	int kept = 0;
	for (int i = 0; i < foundCount; ++i) {
		if (now - found[i].seen < FOUND_FOR) {
			found[kept++] = found[i];
		}
	}
	foundCount = kept;
	*hosts = found;
	return foundCount;
}

// Stops looking once the Join list hasn't been shown for two seconds.
void netScanIdle(void) {
	if (scanner >= 0 && SDL_GetTicks() - scannedAt > 2000) {
		netScanStop();
	}
}

void netScanStop(void) {
	if (scanner >= 0) {
		close(scanner);
		scanner = -1;
	}
	foundCount = 0;
}

// With no game loaded, netplay's messages are still read, as a device that joined without one waits
// for the host's welcome to load its game.
void netIdle(void) {
	pump();
}

// No connections yet.
void netInit(void) {
	for (int i = 0; i < PEERS; ++i) {
		connections[i].fd = -1;
	}
}
