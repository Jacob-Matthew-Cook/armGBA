/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// The Game Boy Advance Wireless Adapter, as Nintendo's wireless library (pokeemerald's librfu)
// drives it, with the commands and replies documented by gba-link-connection (afska) and gpSP.
//
// The serial side: 32-bit transfers in normal mode. The game is clock master; the adapter's word for
// a transfer is ready before it starts, so it answers each word one transfer later. A command is
// 0x9966LLCC followed by LL words, then the game sends 0x80000000 and gets 0x9966LL(CC|0x80) and LL
// reply words. After a wait command the adapter becomes clock master: when something happens it
// clocks a notice to the game (data ready 0x28, timeout 0x27, disconnected 0x29), takes the game's
// acknowledgement, and hands the clock back. Between transfers the two sides signal readiness on SO
// and SI.
//
// The radio side: a host broadcasts and takes up to four clients. The host's packet reaches every
// client, and each client's packet reaches the host, in the exchanges the adapters keep up by
// themselves; each end holds one packet per sender, which a newer one replaces. Adapters talk
// through the air the frontend gives (struct RfuAir) in messages, and keep what they hear in their
// own console's state, so states and rollback hold the whole conversation.

enum {
	WIRE_ID,
	WIRE_COMMAND,
	WIRE_DATA,
	WIRE_ACK,
	WIRE_REPLY,
	WIRE_WAIT,
	WIRE_STOPPED,
};

enum {
	RADIO_IDLE,
	RADIO_HOST,
	RADIO_SEARCHING,
	RADIO_CONNECTING,
	RADIO_CLIENT,
};

enum {
	RFU_ID = 0x8001,
	FILLER = 0x80000000,
	// Errors in a command's reply: a command the adapter doesn't have, or one it can't do now.
	ERROR_STATE = 1,
	ERROR_COMMAND = 2,
	// Times in cycles: a host's packet reaching its clients, the clients' answers reaching the
	// host, a connection being made, and the adapter clocking a word to the game once it is ready.
	HOST_TO_CLIENT = 16384,
	EXCHANGE = 32768,
	CONNECTING = 65536,
	PUSH = 1024,
	FRAME = 280896,
	// How long a room stays listed after it was last heard: four seconds; and how often an adapter
	// listing rooms asks again, as one may not have heard the last answers (it was reset meanwhile).
	HEARD = 240 * FRAME,
	ASK = 60 * FRAME,
};

static const uint16_t NINTENDO[4] = { 0x494E, 0x544E, 0x4E45, 0x4F44 };

static int32_t now(struct GBA* gba) {
	return timingCurrentTime(gba);
}

static bool arrived(struct GBA* gba, int32_t when) {
	return (int32_t) (now(gba) - when) >= 0;
}

// Messages between adapters.
enum {
	AIR_ROOM = 1,
	AIR_SEARCH,
	AIR_JOIN,
	AIR_ACCEPT,
	AIR_REJECT,
	AIR_HOST_DATA,
	AIR_CLIENT_DATA,
	AIR_DROP,
	AIR_LEAVE,
};

static void put16(uint8_t* to, uint16_t value) {
	to[0] = value;
	to[1] = value >> 8;
}

static uint16_t get16(const uint8_t* from) {
	return from[0] | from[1] << 8;
}

static int self(struct GBA* gba) {
	return gba->sio.air ? gba->sio.air->self : 0;
}

static bool network(struct GBA* gba) {
	return gba->sio.air && gba->sio.air->network;
}

static void airSend(struct GBA* gba, int to, const uint8_t* message, size_t size) {
	const struct RfuAir* air = gba->sio.air;
	if (air && air->send) {
		air->send(air, to, message, size);
	}
}

// When something sent now reaches this console: in-core after the radio's delay, over a network
// when it arrives.
static int32_t arrival(struct GBA* gba, int32_t delay) {
	return now(gba) + (network(gba) ? 0 : delay);
}

// Device IDs are random on the real adapter; here they come from a generator seeded per console,
// so a replay makes the same ones.
static uint16_t newId(struct Rfu* rfu) {
	uint16_t id;
	do {
		rfu->seed = rfu->seed * 1664525 + 1013904223;
		id = rfu->seed >> 16;
	} while (!id);
	return id;
}

static void checkWait(struct GBA* gba);

// Has a waiting adapter look again at its wait; it then sets itself to look again when something
// next falls due.
static void wake(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->wire != WIRE_WAIT || rfu->pushLength) {
		return;
	}
	timingDeschedule(gba, &rfu->event);
	timingSchedule(gba, &rfu->event, 0);
}

// The slot a host gives its next client, or 0xFF when it is closed or full. Setup's bits 16-17 limit
// the players: 0 for five (four clients), down to 3 for two.
static int freeSlot(struct Rfu* host) {
	if (host->radio != RADIO_HOST || !host->open) {
		return 0xFF;
	}
	int slots = 4 - ((host->setup >> 16) & 3);
	for (int slot = 0; slot < slots; ++slot) {
		if (!host->clients[slot].active) {
			return slot;
		}
	}
	return 0xFF;
}

// Tells the consoles around what this adapter's room is now: open, its next slot and broadcast, or
// gone.
static void announce(struct GBA* gba, int to) {
	struct Rfu* rfu = &gba->sio.rfu;
	uint8_t message[5 + sizeof(rfu->broadcast)] = { AIR_ROOM, rfu->radio == RADIO_HOST && rfu->open, freeSlot(rfu) };
	put16(message + 3, rfu->id);
	for (unsigned i = 0; i < sizeof(rfu->broadcast); ++i) {
		message[5 + i] = rfu->broadcast[i / 4] >> (8 * (i & 3));
	}
	airSend(gba, to, message, sizeof(message));
}

// A room heard: an open one is listed, and one that closed stays listed for HEARD.
static void heard(struct GBA* gba, int from, const uint8_t* message) {
	struct Rfu* rfu = &gba->sio.rfu;
	int match = -1, free = -1;
	for (int i = 0; i < RFU_ROOMS && match < 0; ++i) {
		if (rfu->rooms[i].heard && rfu->rooms[i].console == from) {
			match = i;
		} else if (free < 0 && (!rfu->rooms[i].heard || (!rfu->rooms[i].live && arrived(gba, rfu->rooms[i].at + HEARD)))) {
			free = i;
		}
	}
	int i = match >= 0 ? match : message[1] ? free : -1;
	if (i < 0) {
		return;
	}
	rfu->rooms[i].heard = true;
	rfu->rooms[i].live = message[1];
	rfu->rooms[i].console = from;
	rfu->rooms[i].at = now(gba);
	if (message[1]) {
		rfu->rooms[i].slot = message[2];
		rfu->rooms[i].id = get16(message + 3);
		memset(rfu->rooms[i].broadcast, 0, sizeof(rfu->rooms[i].broadcast));
		for (unsigned b = 0; b < sizeof(rfu->rooms[i].broadcast); ++b) {
			rfu->rooms[i].broadcast[b / 4] |= (uint32_t) message[5 + b] << (8 * (b & 3));
		}
	}
}

// A client's link ends: the host dropped it (manual) or it was lost.
static void dropClient(struct GBA* gba, bool manual) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->radio == RADIO_CLIENT && rfu->number < RFU_SLOTS) {
		rfu->lost |= 1 << rfu->number;
		rfu->lostReason = manual ? 0 : 1;
	}
	rfu->radio = RADIO_IDLE;
	rfu->id = 0;
	rfu->rx.sent = false;
	rfu->tx.sent = false;
	wake(gba);
}

// A host loses a client: it left, or its console did.
static void loseClient(struct GBA* gba, int slot) {
	struct Rfu* rfu = &gba->sio.rfu;
	rfu->clients[slot].active = false;
	rfu->clients[slot].rx.sent = false;
	rfu->lost |= 1 << slot;
	rfu->lostReason = 1;
	wake(gba);
	announce(gba, -1);
}

static void sendDrop(struct GBA* gba, int slot) {
	struct Rfu* rfu = &gba->sio.rfu;
	uint8_t message[3] = { AIR_DROP };
	put16(message + 1, rfu->clients[slot].id);
	airSend(gba, rfu->clients[slot].console, message, sizeof(message));
}

static void sendLeave(struct GBA* gba, int host, int slot, uint16_t id) {
	uint8_t message[4] = { AIR_LEAVE, slot };
	put16(message + 2, id);
	airSend(gba, host, message, sizeof(message));
}

// Leaves whatever link the adapter is in, telling the other end.
static void leave(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	bool hosting = rfu->radio == RADIO_HOST;
	if (hosting) {
		for (int slot = 0; slot < RFU_SLOTS; ++slot) {
			if (rfu->clients[slot].active) {
				sendDrop(gba, slot);
			}
			rfu->clients[slot].active = false;
		}
	} else if ((rfu->radio == RADIO_CLIENT || rfu->radio == RADIO_CONNECTING) && rfu->number < RFU_SLOTS) {
		sendLeave(gba, rfu->hostConsole, rfu->number, rfu->id);
	}
	rfu->radio = RADIO_IDLE;
	rfu->open = false;
	rfu->id = 0;
	rfu->rx.sent = false;
	rfu->tx.sent = false;
	rfu->exchange = false;
	rfu->lost = 0;
	if (hosting) {
		announce(gba, -1);
	}
}

// Asks the rooms around to make themselves heard.
static void search(struct GBA* gba) {
	gba->sio.rfu.asked = now(gba);
	airSend(gba, -1, (const uint8_t[]) { AIR_SEARCH }, 1);
}

// The rooms in range: each one's ID and next slot, then its broadcast. A room stays listed for a
// while after it was last heard open, as a game switches between hosting and searching and so is
// only heard some of the time.
static int listHosts(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	if ((uint32_t) (now(gba) - rfu->asked) >= ASK) {
		search(gba);
	}
	int n = 0;
	for (int i = 0; i < RFU_ROOMS && n < 4 * 7; ++i) {
		if (!rfu->rooms[i].heard || (!rfu->rooms[i].live && arrived(gba, rfu->rooms[i].at + HEARD))) {
			continue;
		}
		rfu->words[n++] = rfu->rooms[i].id | rfu->rooms[i].slot << 16;
		memcpy(&rfu->words[n], rfu->rooms[i].broadcast, sizeof(rfu->rooms[i].broadcast));
		n += 6;
	}
	return n;
}

// A host's clients: each one's ID and slot.
static int listClients(struct Rfu* rfu) {
	int n = 0;
	for (int slot = 0; slot < RFU_SLOTS; ++slot) {
		if (rfu->clients[slot].active) {
			rfu->words[n++] = rfu->clients[slot].id | slot << 16;
		}
	}
	return n;
}

// Asks the room with this ID to take this console; the host's answer finishes the connection.
static void connect(struct GBA* gba, uint16_t hostId) {
	struct Rfu* rfu = &gba->sio.rfu;
	rfu->radio = RADIO_CONNECTING;
	rfu->connected = now(gba) + CONNECTING;
	rfu->number = 0xFF;
	rfu->hostId = hostId;
	rfu->answered = true;
	rfu->rx.sent = false;
	rfu->tx.sent = false;
	for (int i = 0; i < RFU_ROOMS; ++i) {
		if (rfu->rooms[i].heard && rfu->rooms[i].live && rfu->rooms[i].id == hostId) {
			rfu->hostConsole = rfu->rooms[i].console;
			rfu->answered = false;
			uint8_t message[3] = { AIR_JOIN };
			put16(message + 1, hostId);
			airSend(gba, rfu->hostConsole, message, sizeof(message));
			return;
		}
	}
}

// A connection being made: whether it has finished, and how it ended.
static uint32_t connectStatus(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->radio == RADIO_CONNECTING && rfu->answered && arrived(gba, rfu->connected)) {
		rfu->radio = rfu->number < RFU_SLOTS ? RADIO_CLIENT : RADIO_IDLE;
		if (rfu->radio == RADIO_IDLE) {
			rfu->id = 0;
		}
	}
	switch (rfu->radio) {
	case RADIO_CONNECTING:
		return 0x01000000;
	case RADIO_CLIENT:
		return rfu->id | rfu->number << 16;
	default:
		return 0x02000000;
	}
}

// A host's packet goes to each client.
static void hostSend(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	uint8_t message[4 + RFU_HOST_BYTES] = { AIR_HOST_DATA, rfu->tx.length };
	memcpy(message + 4, rfu->tx.data, rfu->tx.length);
	for (int slot = 0; slot < RFU_SLOTS; ++slot) {
		if (rfu->clients[slot].active) {
			put16(message + 2, rfu->clients[slot].id);
			airSend(gba, rfu->clients[slot].console, message, 4 + rfu->tx.length);
		}
	}
	rfu->exchange = true;
	rfu->exchanged = now(gba) + EXCHANGE;
}

// Takes a packet to send from a send command: a header word, then the data. A header with no data
// after it sends the last packet's bytes again.
static int send(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->radio != RADIO_HOST && rfu->radio != RADIO_CLIENT) {
		return -ERROR_STATE;
	}
	if (!rfu->length) {
		return 0;
	}
	bool host = rfu->radio == RADIO_HOST;
	uint32_t header = rfu->words[0];
	unsigned bytes = host ? header & 0x7F : (header >> (8 + rfu->number * 5)) & 0x1F;
	unsigned most = host ? RFU_HOST_BYTES : RFU_CLIENT_BYTES;
	bytes = bytes < most ? bytes : most;
	if (rfu->length > 1) {
		for (unsigned i = 0; i < bytes && i / 4 + 1 < rfu->length; ++i) {
			rfu->tx.data[i] = rfu->words[1 + i / 4] >> (8 * (i & 3));
		}
	}
	rfu->tx.length = bytes;
	if (host) {
		hostSend(gba);
		return 0;
	}
	// A client's packet goes up in the adapters' next radio exchange, which they make by themselves.
	uint8_t message[5 + RFU_CLIENT_BYTES] = { AIR_CLIENT_DATA, rfu->number, bytes };
	put16(message + 3, rfu->id);
	memcpy(message + 5, rfu->tx.data, bytes);
	airSend(gba, rfu->hostConsole, message, 5 + bytes);
	return 0;
}

// Hands over what has arrived: a header with the byte counts (the host's in bits 0-6, each client's
// in five bits from bit 8), then the bytes, packed.
static int receive(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	uint8_t bytes[RFU_HOST_BYTES + 4] = { 0 };
	unsigned total = 0;
	uint32_t header = 0;
	if (rfu->radio == RADIO_HOST) {
		for (int slot = 0; slot < RFU_SLOTS; ++slot) {
			struct RfuPacket* rx = &rfu->clients[slot].rx;
			if (rfu->clients[slot].active && rx->sent && arrived(gba, rx->arrival)) {
				memcpy(&bytes[total], rx->data, rx->length);
				total += rx->length;
				header |= rx->length << (8 + slot * 5);
				rx->sent = false;
			}
		}
	} else if (rfu->rx.sent && arrived(gba, rfu->rx.arrival)) {
		memcpy(bytes, rfu->rx.data, rfu->rx.length);
		total = rfu->rx.length;
		header = total;
		rfu->rx.sent = false;
	}
	rfu->words[0] = header;
	for (unsigned i = 0; i < (total + 3) / 4; ++i) {
		rfu->words[1 + i] = bytes[i * 4] | bytes[i * 4 + 1] << 8 | bytes[i * 4 + 2] << 16 | (uint32_t) bytes[i * 4 + 3] << 24;
	}
	return 1 + (total + 3) / 4;
}

static void disconnect(struct GBA* gba, uint32_t slots) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->radio == RADIO_HOST) {
		for (int slot = 0; slot < RFU_SLOTS; ++slot) {
			if (slots & (1 << slot) && rfu->clients[slot].active) {
				sendDrop(gba, slot);
				rfu->clients[slot].active = false;
				rfu->clients[slot].rx.sent = false;
			}
		}
		if (!rfu->open && !listClients(rfu)) {
			rfu->radio = RADIO_IDLE;
			rfu->id = 0;
		}
		announce(gba, -1);
	} else if (rfu->radio == RADIO_CLIENT || rfu->radio == RADIO_CONNECTING) {
		leave(gba);
	}
}

// Carries out a command; returns how many reply words it left in words, or minus an error.
static int execute(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	switch (rfu->command) {
	case 0x10:
		leave(gba);
		return 0;
	case 0x11: {
		// Link strength, a byte per slot: a host's clients, or a client's own slot.
		uint32_t strength = 0;
		if (rfu->radio == RADIO_HOST) {
			for (int slot = 0; slot < RFU_SLOTS; ++slot) {
				strength |= rfu->clients[slot].active ? 0xFFu << (slot * 8) : 0;
			}
		} else if (rfu->radio == RADIO_CLIENT) {
			strength = 0xFFu << (rfu->number * 8);
		}
		rfu->words[0] = strength;
		return 1;
	}
	case 0x12:
		rfu->words[0] = 0x00830117;
		return 1;
	case 0x13: {
		// The device ID, a client's slot bit, and the state: 1 host closed, 2 host open, 3
		// searching, 4 connecting, 5 connected.
		uint32_t state = 0;
		switch (rfu->radio) {
		case RADIO_HOST:
			state = (rfu->open ? 2u : 1u) << 24 | rfu->id;
			break;
		case RADIO_SEARCHING:
			state = 3u << 24;
			break;
		case RADIO_CONNECTING:
			state = 4u << 24;
			break;
		case RADIO_CLIENT:
			state = 5u << 24 | (1u << rfu->number) << 16 | rfu->id;
			break;
		}
		rfu->words[0] = state;
		return 1;
	}
	case 0x14: {
		// The slot the next client gets, then the clients.
		int n = rfu->radio == RADIO_HOST ? listClients(rfu) : 0;
		memmove(&rfu->words[1], &rfu->words[0], n * sizeof(rfu->words[0]));
		rfu->words[0] = freeSlot(rfu);
		return n + 1;
	}
	case 0x15:
		memcpy(rfu->words, rfu->broadcast, sizeof(rfu->broadcast));
		rfu->words[6] = rfu->setup;
		rfu->words[7] = 0;
		return rfu->radio == RADIO_HOST ? 8 : 7;
	case 0x16:
		if (rfu->length != 6) {
			return -ERROR_STATE;
		}
		memcpy(rfu->broadcast, rfu->words, sizeof(rfu->broadcast));
		if (rfu->radio == RADIO_HOST) {
			announce(gba, -1);
		}
		return 0;
	case 0x17:
		rfu->setup = rfu->length ? rfu->words[0] : 0;
		if (rfu->radio == RADIO_HOST) {
			announce(gba, -1);
		}
		return 0;
	case 0x18:
	case 0x31:
	case 0x36:
	case 0x38:
	case 0x39:
		return 0;
	case 0x19:
		if (rfu->radio == RADIO_CLIENT || rfu->radio == RADIO_CONNECTING) {
			return -ERROR_STATE;
		}
		if (rfu->radio != RADIO_HOST) {
			memset(rfu->clients, 0, sizeof(rfu->clients));
			rfu->id = newId(rfu);
			rfu->radio = RADIO_HOST;
			rfu->exchange = false;
		}
		rfu->open = true;
		announce(gba, -1);
		return 0;
	case 0x1A:
		if (rfu->radio != RADIO_HOST) {
			return -ERROR_STATE;
		}
		return listClients(rfu);
	case 0x1B: {
		// Closing the room keeps the clients; a host with none goes back to idle.
		if (rfu->radio != RADIO_HOST) {
			return -ERROR_STATE;
		}
		rfu->open = false;
		int n = listClients(rfu);
		if (!n) {
			rfu->radio = RADIO_IDLE;
			rfu->id = 0;
		}
		announce(gba, -1);
		return n;
	}
	case 0x1C:
		if ((rfu->radio == RADIO_HOST && listClients(rfu)) || rfu->radio == RADIO_CLIENT) {
			return -ERROR_STATE;
		}
		if (rfu->radio == RADIO_HOST) {
			rfu->radio = RADIO_IDLE;
			rfu->open = false;
			rfu->id = 0;
			announce(gba, -1);
		}
		rfu->radio = RADIO_SEARCHING;
		search(gba);
		return 0;
	case 0x1D:
		return listHosts(gba);
	case 0x1E: {
		int n = listHosts(gba);
		if (rfu->radio == RADIO_SEARCHING) {
			rfu->radio = RADIO_IDLE;
		}
		return n;
	}
	case 0x1F:
	case 0x32:
		if (rfu->radio == RADIO_HOST || rfu->radio == RADIO_CLIENT || !rfu->length) {
			return -ERROR_STATE;
		}
		connect(gba, rfu->words[0]);
		return 0;
	case 0x20:
		if (rfu->radio == RADIO_HOST) {
			return -ERROR_STATE;
		}
		rfu->words[0] = connectStatus(gba);
		return 1;
	case 0x21:
		if (rfu->radio == RADIO_HOST) {
			return -ERROR_STATE;
		}
		rfu->words[0] = connectStatus(gba);
		if (rfu->radio != RADIO_CLIENT) {
			// Finishing before the connection was made gives it up.
			leave(gba);
			rfu->words[0] = 0x01000000;
		}
		return 1;
	case 0x33:
		// A reconnection: 0 when made, 1 when it failed.
		if (rfu->radio == RADIO_HOST) {
			return -ERROR_STATE;
		}
		rfu->words[0] = connectStatus(gba) >> 24 == 2 ? 1 : 0;
		return 1;
	case 0x34:
		if (rfu->radio == RADIO_CONNECTING) {
			connectStatus(gba);
		}
		return 0;
	case 0x24:
	case 0x25:
		return send(gba);
	case 0x26:
		return receive(gba);
	case 0x27:
	case 0x35:
		return 0;
	case 0x37:
		if (rfu->radio == RADIO_HOST) {
			hostSend(gba);
		}
		return 0;
	case 0x30:
		disconnect(gba, rfu->length ? rfu->words[0] : 0xF);
		return 0;
	case 0x3D:
		leave(gba);
		return 0;
	default:
		return -ERROR_COMMAND;
	}
}

static bool waits(uint8_t command) {
	return command == 0x25 || command == 0x27 || command == 0x35 || command == 0x37;
}

// A whole command has arrived: the acknowledgement goes on the wire, and the reply after it.
static void respond(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	int result = execute(gba);
	if (result < 0) {
		rfu->words[0] = -result;
		rfu->length = 1;
		rfu->next = 0x996601EE;
		rfu->command = 0xEE;
	} else {
		rfu->length = result;
		rfu->next = 0x99660000 | result << 8 | (rfu->command | 0x80);
	}
	rfu->count = 0;
	rfu->wire = WIRE_ACK;
}

// After a wait command, the adapter waits for something to tell the game, or for the time Setup
// allows (in frames, bits 0-7; none when 0).
static void startWait(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	rfu->wire = WIRE_WAIT;
	rfu->next = FILLER;
	rfu->pushLength = 0;
	rfu->pushing = false;
	rfu->timed = rfu->setup & 0xFF;
	rfu->deadline = now(gba) + (int32_t) (rfu->setup & 0xFF) * FRAME;
	wake(gba);
}

// The game's half of the "NINTENDO" exchange, done from the adapter's side: each end sends a word
// of the string and the inverse of the other's last word, and moves on to the next word when the
// other end agrees; after four the adapter sends its ID.
static void identify(struct Rfu* rfu, uint32_t received) {
	uint16_t echo = received >> 16;
	uint16_t data = received;
	if (!rfu->idLast) {
		if (echo == rfu->idRecv) {
			if (rfu->idCount < 4) {
				if (rfu->idRecv == (uint16_t) ~rfu->idSend && data == (uint16_t) ~rfu->idRecv) {
					++rfu->idCount;
				}
			} else {
				rfu->idLast = data;
			}
		} else {
			rfu->idCount = 0;
		}
	}
	rfu->idSend = rfu->idCount < 4 ? NINTENDO[rfu->idCount] : RFU_ID;
	rfu->idRecv = ~data;
	rfu->next = (uint32_t) rfu->idSend << 16 | rfu->idRecv;
	if (rfu->idLast == RFU_ID) {
		rfu->wire = WIRE_COMMAND;
		rfu->next = FILLER;
	}
}

uint32_t rfuTransfer(struct GBA* gba, uint32_t out) {
	struct Rfu* rfu = &gba->sio.rfu;
	uint32_t in = rfu->next;
	switch (rfu->wire) {
	case WIRE_ID:
		identify(rfu, out);
		break;
	case WIRE_COMMAND:
		rfu->next = FILLER;
		if (out >> 16 == 0x9966) {
			rfu->command = out;
			rfu->length = out >> 8;
			rfu->count = 0;
			if (rfu->length) {
				rfu->wire = WIRE_DATA;
			} else {
				respond(gba);
			}
		}
		break;
	case WIRE_DATA:
		rfu->words[rfu->count++] = out;
		if (rfu->count == rfu->length || rfu->count == sizeof(rfu->words) / sizeof(rfu->words[0])) {
			respond(gba);
		}
		break;
	case WIRE_ACK:
	case WIRE_REPLY:
		if (rfu->count < rfu->length) {
			rfu->next = rfu->words[rfu->count++];
			rfu->wire = WIRE_REPLY;
		} else if (waits(rfu->command)) {
			startWait(gba);
		} else {
			rfu->wire = rfu->command == 0x3D ? WIRE_STOPPED : WIRE_COMMAND;
			rfu->next = FILLER;
		}
		break;
	case WIRE_WAIT:
		rfu->next = FILLER;
		break;
	case WIRE_STOPPED:
		in = 0;
		break;
	}
	return in;
}

// The game is ready for the adapter to clock a word to it: the external clock, the start bit set,
// SO low.
static bool gameReady(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	return (sio->siocnt & 0x3089) == 0x1080 && !(sio->rcnt & 0x8000);
}

static void setPush(struct Rfu* rfu, uint32_t notice, int length, uint32_t data) {
	rfu->push[0] = 0x99660000 | length << 8 | notice;
	rfu->push[1] = length ? data : FILLER;
	rfu->push[2] = FILLER;
	rfu->pushLength = length ? 3 : 2;
	rfu->pushCount = 0;
}

// Whether a client's packet has reached a host.
static bool clientData(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	for (int slot = 0; slot < RFU_SLOTS; ++slot) {
		struct RfuPacket* rx = &rfu->clients[slot].rx;
		if (rfu->clients[slot].active && rx->sent && arrived(gba, rx->arrival)) {
			return true;
		}
	}
	return false;
}

// Decides whether a wait is over, and clocks the notice and its acknowledgement through.
static void checkWait(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->wire != WIRE_WAIT) {
		return;
	}
	if (!rfu->pushLength) {
		int32_t soonest = rfu->deadline;
		bool due = rfu->timed;
		if (rfu->lost) {
			setPush(rfu, 0x29, 1, rfu->lost | rfu->lostReason << 8);
			rfu->lost = 0;
		} else if (rfu->radio == RADIO_CLIENT && rfu->rx.sent && arrived(gba, rfu->rx.arrival)) {
			setPush(rfu, 0x28, 0, 0);
		} else if (rfu->radio == RADIO_HOST && ((rfu->exchange && arrived(gba, rfu->exchanged)) || clientData(gba))) {
			rfu->exchange = false;
			setPush(rfu, 0x28, 0, 0);
		} else if (rfu->timed && arrived(gba, rfu->deadline)) {
			setPush(rfu, 0x27, 0, 0);
		} else {
			if (rfu->radio == RADIO_CLIENT && rfu->rx.sent && (!due || (int32_t) (rfu->rx.arrival - soonest) < 0)) {
				soonest = rfu->rx.arrival;
				due = true;
			}
			if (rfu->radio == RADIO_HOST && rfu->exchange && (!due || (int32_t) (rfu->exchanged - soonest) < 0)) {
				soonest = rfu->exchanged;
				due = true;
			}
			for (int slot = 0; rfu->radio == RADIO_HOST && slot < RFU_SLOTS; ++slot) {
				struct RfuPacket* rx = &rfu->clients[slot].rx;
				if (rfu->clients[slot].active && rx->sent && (!due || (int32_t) (rx->arrival - soonest) < 0)) {
					soonest = rx->arrival;
					due = true;
				}
			}
			if (due) {
				timingDeschedule(gba, &rfu->event);
				timingSchedule(gba, &rfu->event, soonest - now(gba) > 0 ? soonest - now(gba) : 0);
			}
			return;
		}
	}
	if (rfu->pushing || !gameReady(gba)) {
		return;
	}
	rfu->pushing = true;
	timingDeschedule(gba, &rfu->event);
	timingSchedule(gba, &rfu->event, PUSH);
}

// The adapter, as clock master, sends the game a word and takes the game's.
static void pushWord(struct GBA* gba, uint32_t late) {
	struct Rfu* rfu = &gba->sio.rfu;
	struct Sio* sio = &gba->sio;
	rfu->pushing = false;
	if (!gameReady(gba)) {
		return;
	}
	uint32_t word = rfu->push[rfu->pushCount++];
	gba->io[IO(REG_SIODATA32_LO)] = word;
	gba->io[IO(REG_SIODATA32_LO) + 1] = word >> 16;
	sio->siocnt &= ~0x0080;
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	if (sio->siocnt & 0x4000) {
		gbaRaiseIrq(gba, IRQ_SIO, late);
	}
	if (rfu->pushCount == rfu->pushLength) {
		rfu->pushLength = 0;
		rfu->wire = WIRE_COMMAND;
		rfu->next = FILLER;
	}
}

static void eventRun(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	struct Rfu* rfu = &gba->sio.rfu;
	if (rfu->pushing) {
		pushWord(gba, late);
	}
	checkWait(gba);
}

uint16_t rfuSiocnt(struct GBA* gba, uint16_t before, uint16_t after) {
	// SI answers SO: with the game as clock master, the adapter takes SI low once the game raises
	// SO; as clock slave, SI follows SO.
	bool soRise = (after & 0x0008) && !(before & 0x0008);
	bool soFall = !(after & 0x0008) && (before & 0x0008);
	uint16_t si = before & 0x0004;
	if (after & 0x0001) {
		si = soRise ? 0 : si;
	} else {
		si = soRise ? 0x0004 : soFall ? 0 : si;
	}
	return (after & ~0x0004) | si;
}

void rfuRcnt(struct GBA* gba, uint16_t before, uint16_t after) {
	// In general-purpose mode, SD driven from low to high resets the adapter.
	if ((after & 0xC000) == 0x8000 && (after & 0x0020) && (after & 0x0002) && !(before & 0x0002)) {
		rfuReset(gba);
	}
}

// Called after SIOCNT changes, for a wait ending on the game's readiness.
void rfuSiocntWritten(struct GBA* gba) {
	if (gba->sio.rfu.attached) {
		checkWait(gba);
	}
}

void rfuReset(struct GBA* gba) {
	struct Rfu* rfu = &gba->sio.rfu;
	leave(gba);
	timingDeschedule(gba, &rfu->event);
	bool attached = rfu->attached;
	uint32_t seed = rfu->seed;
	struct Event event = rfu->event;
	memset(rfu, 0, sizeof(*rfu));
	rfu->attached = attached;
	rfu->event = event;
	rfu->seed = seed ^ (uint32_t) now(gba) ^ (uint32_t) self(gba) * 0x9E3779B9u;
	rfu->wire = WIRE_ID;
	rfu->next = 0;
	rfu->setup = 0x003C0420;
	rfu->asked = now(gba) - ASK;
}

void rfuInit(struct GBA* gba) {
	gba->sio.rfu.event.callback = eventRun;
	gba->sio.rfu.event.context = NULL;
	gba->sio.rfu.event.priority = 0x81;
}

void rfuAttach(struct GBA* gba, bool attached, const struct RfuAir* air) {
	bool changed = attached != gba->sio.rfu.attached || air != gba->sio.air;
	gba->sio.air = air;
	gba->sio.rfu.attached = attached;
	if (changed) {
		rfuReset(gba);
	}
}

void rfuReceive(struct GBA* gba, int from, const uint8_t* message, size_t size) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (!rfu->attached || !size) {
		return;
	}
	bool client = (rfu->radio == RADIO_CLIENT || rfu->radio == RADIO_CONNECTING) && rfu->hostConsole == from;
	switch (message[0]) {
	case AIR_ROOM:
		if (size >= 5 + sizeof(rfu->broadcast)) {
			heard(gba, from, message);
		}
		break;
	case AIR_SEARCH:
		if (rfu->radio == RADIO_HOST && rfu->open) {
			announce(gba, from);
		}
		break;
	case AIR_JOIN: {
		if (size < 3 || rfu->radio != RADIO_HOST || get16(message + 1) != rfu->id) {
			break;
		}
		for (int slot = 0; slot < RFU_SLOTS; ++slot) {
			if (rfu->clients[slot].active && rfu->clients[slot].console == from) {
				rfu->clients[slot].active = false;
				rfu->clients[slot].rx.sent = false;
			}
		}
		int slot = freeSlot(rfu);
		uint8_t answer[6] = { AIR_REJECT, slot };
		put16(answer + 2, rfu->id);
		if (slot != 0xFF) {
			rfu->clients[slot].active = true;
			rfu->clients[slot].id = newId(rfu);
			rfu->clients[slot].console = from;
			rfu->clients[slot].rx.sent = false;
			answer[0] = AIR_ACCEPT;
			put16(answer + 4, rfu->clients[slot].id);
		}
		airSend(gba, from, answer, sizeof(answer));
		announce(gba, -1);
		break;
	}
	case AIR_ACCEPT:
	case AIR_REJECT:
		if (size < 6) {
			break;
		}
		if (client && rfu->radio == RADIO_CONNECTING && !rfu->answered && get16(message + 2) == rfu->hostId) {
			rfu->answered = true;
			if (message[0] == AIR_ACCEPT && message[1] < RFU_SLOTS) {
				rfu->number = message[1];
				rfu->id = get16(message + 4);
			}
		} else if (message[0] == AIR_ACCEPT) {
			// The connection was given up before the answer came.
			sendLeave(gba, from, message[1], get16(message + 4));
		}
		break;
	case AIR_HOST_DATA:
		if (size < 4u + message[1] || message[1] > RFU_HOST_BYTES || !client || rfu->number >= RFU_SLOTS || get16(message + 2) != rfu->id) {
			break;
		}
		rfu->rx.sent = true;
		rfu->rx.length = message[1];
		rfu->rx.arrival = arrival(gba, HOST_TO_CLIENT);
		memcpy(rfu->rx.data, message + 4, message[1]);
		wake(gba);
		break;
	case AIR_CLIENT_DATA: {
		if (size < 5u + message[2] || message[2] > RFU_CLIENT_BYTES || rfu->radio != RADIO_HOST || message[1] >= RFU_SLOTS) {
			break;
		}
		struct RfuPacket* rx = &rfu->clients[message[1]].rx;
		if (!rfu->clients[message[1]].active || rfu->clients[message[1]].console != from || rfu->clients[message[1]].id != get16(message + 3)) {
			break;
		}
		rx->sent = true;
		rx->length = message[2];
		rx->arrival = arrival(gba, EXCHANGE);
		memcpy(rx->data, message + 5, message[2]);
		wake(gba);
		break;
	}
	case AIR_DROP:
		if (size >= 3 && client && rfu->number < RFU_SLOTS && get16(message + 1) == rfu->id) {
			dropClient(gba, true);
		}
		break;
	case AIR_LEAVE:
		if (size >= 4 && rfu->radio == RADIO_HOST && message[1] < RFU_SLOTS && rfu->clients[message[1]].active
		    && rfu->clients[message[1]].console == from && rfu->clients[message[1]].id == get16(message + 2)) {
			loseClient(gba, message[1]);
		}
		break;
	}
}

void rfuPeerGone(struct GBA* gba, int console) {
	struct Rfu* rfu = &gba->sio.rfu;
	if (!rfu->attached) {
		return;
	}
	for (int i = 0; i < RFU_ROOMS; ++i) {
		if ((console < 0 || rfu->rooms[i].console == console)) {
			rfu->rooms[i].heard = false;
		}
	}
	if (rfu->radio == RADIO_HOST) {
		for (int slot = 0; slot < RFU_SLOTS; ++slot) {
			if (rfu->clients[slot].active && (console < 0 || rfu->clients[slot].console == console)) {
				loseClient(gba, slot);
			}
		}
	} else if ((rfu->radio == RADIO_CLIENT || rfu->radio == RADIO_CONNECTING) && (console < 0 || rfu->hostConsole == console)) {
		if (rfu->radio == RADIO_CONNECTING) {
			rfu->answered = true;
			rfu->number = 0xFF;
		} else {
			dropClient(gba, false);
		}
	}
}
