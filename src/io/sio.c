/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

// The serial port. With no link attached nothing is on the cable: in multiplayer mode the open SI
// line makes the GBA a child, so it never starts a transfer. With a link, the frontend carries
// normal-mode and multiplayer transfers to the other consoles.

enum {
	SIO_NORMAL_8 = 0,
	SIO_NORMAL_32 = 1,
	SIO_MULTI = 2,
	SIO_UART = 3,
	SIO_GPIO = 8,
	SIO_JOYBUS = 12,
};

enum {
	REG_SIODATA32_HI = 0x122,
	REG_SIODATA8 = 0x12A,
};

// A transfer starts this long after the write that starts it.
static const int TRANSFER_DELAY = 6;

// Multiplayer transfer times by baud rate (9600, 38400, 57600, 115200) and number of consoles, as
// measured on hardware.
static const int32_t MULTI_CYCLES[4][4] = {
	{ 31976, 63427, 94884, 125829 },
	{ 8378, 16241, 24104, 31457 },
	{ 5750, 10998, 16246, 20972 },
	{ 3140, 5755, 8376, 10486 },
};

// Returns the mode, chosen by bits of RCNT and SIOCNT together.
static void switchMode(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	unsigned mode = ((sio->rcnt & 0xC000) | (sio->siocnt & 0x3000)) >> 12;
	sio->mode = mode < 8 ? (int) (mode & 0x3) : (int) (mode & 0xC);
}

static bool linked(const struct Sio* sio) {
	return sio->link && sio->link->players > 1;
}

// In multiplayer mode SIOCNT shows the cable when a link is attached: SI low on the parent, and SD
// high while every console on it is in multiplayer mode. This console's number shows from its first
// transfer on. RCNT's low bits show the same lines, and SC, which is high but while the parent
// clocks a transfer; Mario Bros. waits for it before starting one.
static void multiStatus(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	if (sio->mode != SIO_MULTI) {
		return;
	}
	bool const clocking = (sio->siocnt & 0x0080) && linked(sio) && sio->link->id == 0;
	sio->rcnt = (sio->rcnt & ~0x0001) | !clocking;
	if (linked(sio)) {
		sio->siocnt = (sio->siocnt & ~0x004C) | (sio->link->ready ? 0x0008 : 0) | (sio->link->id ? 0x0004 : 0);
		sio->rcnt = (sio->rcnt & ~0x0006) | (sio->link->ready ? 0x0002 : 0) | (sio->link->id ? 0x0004 : 0);
	}
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	gba->io[IO(REG_RCNT)] = sio->rcnt;
}

// Ends a transfer with the data received: the busy bit clears and the interrupt is requested if
// enabled. Multiplayer mode has all four consoles' words, the others the other end's data.
static void finishWith(struct GBA* gba, const uint32_t values[4], uint32_t late) {
	struct Sio* sio = &gba->sio;
	switch (sio->mode) {
	case SIO_NORMAL_8:
		gba->io[IO(REG_SIODATA8)] = values[0] & 0xFF;
		break;
	case SIO_NORMAL_32:
		gba->io[IO(REG_SIODATA32_LO)] = values[0] & 0xFFFF;
		gba->io[IO(REG_SIODATA32_HI)] = values[0] >> 16;
		break;
	case SIO_MULTI:
		for (int i = 0; i < 4; ++i) {
			gba->io[IO(REG_SIOMULTI0) + i] = values[i];
		}
		if (linked(sio)) {
			sio->siocnt = (sio->siocnt & ~0x0030) | sio->link->id << 4;
		}
		break;
	default:
		return;
	}
	sio->siocnt &= ~0x0080;
	multiStatus(gba);
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	gba->io[IO(REG_RCNT)] = sio->rcnt;
	if (sio->siocnt & 0x4000) {
		gbaRaiseIrq(gba, IRQ_SIO, late);
	}
}

// The data this console puts on the cable for a transfer.
static uint32_t outgoing(struct GBA* gba) {
	switch (gba->sio.mode) {
	case SIO_NORMAL_8:
		return gba->io[IO(REG_SIODATA8)] & 0xFF;
	case SIO_NORMAL_32:
		return gba->io[IO(REG_SIODATA32_LO)] | (uint32_t) gba->io[IO(REG_SIODATA32_HI)] << 16;
	case SIO_MULTI:
		return gba->io[IO(REG_SIOMLT_SEND)];
	default:
		return 0xFFFFFFFF;
	}
}

// The end of a transfer this console clocked. With nothing attached, zeros were received. The
// Wireless Adapter answers a 32-bit transfer, then raises SI until the game is ready again.
static void sioFinish(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	struct Sio* sio = &gba->sio;
	uint32_t values[4] = { 0, 0xFFFF, 0xFFFF, 0xFFFF };
	if (sio->rfu.attached && sio->mode == SIO_NORMAL_32) {
		values[0] = rfuTransfer(gba, sio->rfu.out);
		finishWith(gba, values, late);
		sio->siocnt |= 0x0004;
		gba->io[IO(REG_SIOCNT)] = sio->siocnt;
		return;
	}
	if (linked(sio)) {
		uint32_t in[3] = { 0xFFFFFFFF, 0xFFFF, 0xFFFF };
		if (!sio->link->finish(sio->link->context, in)) {
			timingSchedule(gba, &sio->completeEvent, 0);
			return;
		}
		if (sio->mode == SIO_MULTI) {
			values[0] = outgoing(gba);
			for (int i = 0; i < 3; ++i) {
				values[i + 1] = in[i] & 0xFFFF;
			}
		} else {
			values[0] = in[0];
		}
	}
	finishWith(gba, values, late);
}

void sioInit(struct GBA* gba) {
	gba->sio.completeEvent.callback = sioFinish;
	gba->sio.completeEvent.context = NULL;
	gba->sio.completeEvent.priority = 0x80;
	rfuInit(gba);
}

// General-purpose mode, as at power-on.
void sioReset(struct GBA* gba) {
	gba->sio.rcnt = 0x8000;
	gba->sio.siocnt = 0;
	gba->io[IO(REG_RCNT)] = gba->sio.rcnt;
	switchMode(gba);
	// The adapter is powered by the console, so it starts over with it.
	rfuReset(gba);
}

// Attaches a link cable, or with NULL takes it away.
void sioAttach(struct GBA* gba, const struct SioLink* link) {
	gba->sio.link = link;
	multiStatus(gba);
}

void sioLinkChanged(struct GBA* gba) {
	multiStatus(gba);
}

void sioOthersTransfer(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	if (sio->mode == SIO_MULTI && linked(sio) && sio->link->id) {
		sio->siocnt |= 0x0080;
		gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	}
}

bool sioSdHigh(const struct GBA* gba) {
	const struct Sio* sio = &gba->sio;
	if ((sio->rcnt & 0xC000) == 0x8000) {
		return !(sio->rcnt & 0x0020) || (sio->rcnt & 0x0002);
	}
	return sio->mode == SIO_MULTI;
}

int32_t sioTransferLeft(struct GBA* gba) {
	return timingIsScheduled(gba, &gba->sio.completeEvent) ? timingUntil(gba, &gba->sio.completeEvent) : -1;
}

// Cycles per bit in normal mode, at 256 or 2048 KHz.
static int32_t bitCycles(const struct Sio* sio) {
	return CPU_FREQUENCY / (((sio->siocnt & 2) ? 2048 : 256) * 1024);
}

// Returns how long a transfer takes in each mode at the chosen baud rate or clock.
static int32_t transferCycles(const struct Sio* sio) {
	switch (sio->mode) {
	case SIO_NORMAL_8:
		return 8 * bitCycles(sio);
	case SIO_NORMAL_32:
		return 32 * bitCycles(sio);
	case SIO_MULTI:
		return MULTI_CYCLES[sio->siocnt & 3][linked(sio) ? sio->link->players - 1 : 0];
	default:
		return 0;
	}
}

// A normal-mode game can switch between 8 and 32 bits with its next write, so the shorter counts.
int32_t sioShortestTransfer(const struct GBA* gba) {
	const struct Sio* sio = &gba->sio;
	switch (sio->mode) {
	case SIO_NORMAL_8:
	case SIO_NORMAL_32:
		return TRANSFER_DELAY + 8 * bitCycles(sio);
	case SIO_MULTI:
		return TRANSFER_DELAY + transferCycles(sio);
	default:
		return INT32_MAX;
	}
}

// For another console's transfer: the mode and data this console answers with, and whether it
// takes part (a multiplayer child, or a normal-mode transfer waiting on the external clock).
uint32_t sioOutgoing(struct GBA* gba, int* mode, bool* ready) {
	struct Sio* sio = &gba->sio;
	*mode = sio->mode;
	*ready = sio->mode == SIO_MULTI || ((sio->mode == SIO_NORMAL_8 || sio->mode == SIO_NORMAL_32) && (sio->siocnt & 0x0081) == 0x0080);
	return outgoing(gba);
}

// A transfer another console clocked arrives, with the words of all four consoles in multiplayer
// mode or the other end's data in normal mode. Only a console in the same kind of mode takes part:
// a multiplayer transfer passes a normal-mode one by, and the other way round.
void sioReceive(struct GBA* gba, int transferMode, const uint32_t values[4]) {
	int mode;
	bool ready;
	sioOutgoing(gba, &mode, &ready);
	if (ready && (mode == SIO_MULTI) == (transferMode == SIO_MULTI)) {
		finishWith(gba, values, 0);
	}
}

// SIOCNT: setting the start bit with the internal clock begins a transfer.
static void writeSiocnt(struct GBA* gba, uint16_t value) {
	struct Sio* sio = &gba->sio;
	uint16_t before = sio->siocnt;
	// Busy while a transfer is being clocked. In normal mode, a start bit left set while waiting on
	// the external clock starts a transfer as soon as the internal clock is chosen, which is how
	// Nintendo's wireless library starts finding the Wireless Adapter.
	bool busy = sio->siocnt & 0x0080;
	if (sio->mode == SIO_NORMAL_8 || sio->mode == SIO_NORMAL_32) {
		busy = timingIsScheduled(gba, &sio->completeEvent);
	}
	sio->siocnt = value & 0x3000;
	switchMode(gba);
	value &= sio->mode == SIO_UART ? 0x7FAF : 0x7F8F;
	bool start = !busy && (value & 0x0080);
	switch (sio->mode) {
	case SIO_NORMAL_8:
	case SIO_NORMAL_32:
		// On the external clock, the transfer waits for the other end; with nothing there, for ever.
		start &= value & 0x0001;
		break;
	case SIO_MULTI:
		// Only the parent starts a transfer; a child's start bit waits for one.
		start &= linked(sio) && sio->link->id == 0;
		break;
	default:
		start = false;
		break;
	}
	if (sio->rfu.attached && sio->mode == SIO_NORMAL_32) {
		value = rfuSiocnt(gba, before, value);
	}
	// The multiplayer ID is read-only, kept from the last transfer.
	if (sio->mode == SIO_MULTI) {
		value = (value & ~0x0030) | (before & 0x0030);
	}
	sio->siocnt = value;
	multiStatus(gba);
	// Also kept in the register file, for byte writes to merge with.
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	gba->io[IO(REG_RCNT)] = sio->rcnt;
	if (start) {
		int32_t const length = TRANSFER_DELAY + transferCycles(sio);
		if (sio->rfu.attached) {
			sio->rfu.out = outgoing(gba);
		} else if (linked(sio)) {
			sio->link->begin(sio->link->context, sio->mode, outgoing(gba), length);
		}
		timingDeschedule(gba, &sio->completeEvent);
		timingSchedule(gba, &sio->completeEvent, length);
	}
	rfuSiocntWritten(gba);
}

// RCNT: the mode's top bits, and the pins in general-purpose mode.
static void writeRcnt(struct GBA* gba, uint16_t value) {
	struct Sio* sio = &gba->sio;
	uint16_t before = sio->rcnt;
	sio->rcnt = value & 0xC1FF;
	switchMode(gba);
	switch (sio->mode) {
	case SIO_NORMAL_8:
	case SIO_NORMAL_32:
		sio->rcnt &= ~0x000A;
		break;
	case SIO_JOYBUS:
		sio->rcnt &= ~0x0003;
		break;
	}
	multiStatus(gba);
	gba->io[IO(REG_RCNT)] = sio->rcnt;
	if (sio->rfu.attached) {
		rfuRcnt(gba, before, sio->rcnt);
	}
}

static void dataWritten(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	if (linked(sio) && sio->link->wrote) {
		sio->link->wrote(sio->link->context);
	}
}

// Handles writes to the serial registers.
void sioWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	struct Sio* sio = &gba->sio;
	switch (address) {
	case REG_SIODATA32_LO:
	case REG_SIODATA32_HI:
		if (sio->mode == SIO_NORMAL_32) {
			gba->io[address >> 1] = value;
			dataWritten(gba);
		}
		break;
	case REG_SIOCNT:
		writeSiocnt(gba, value);
		break;
	case REG_SIODATA8:
		if (sio->mode != SIO_UART) {
			gba->io[address >> 1] = value;
			dataWritten(gba);
		}
		break;
	case REG_RCNT:
		writeRcnt(gba, value);
		break;
	case REG_JOYCNT: {
		// The three flags clear on writing 1; the IRQ enable keeps what is written.
		uint16_t joycnt = gba->io[IO(REG_JOYCNT)];
		gba->io[IO(REG_JOYCNT)] = (joycnt & ~(value & 0x0007) & ~0x0040) | (value & 0x0040);
		break;
	}
	case REG_JOY_RECV_LO:
	case REG_JOY_RECV_HI:
		gba->io[address >> 1] = value;
		break;
	case REG_JOY_TRANS_LO:
	case REG_JOY_TRANS_HI:
		gba->io[address >> 1] = value;
		gba->io[IO(REG_JOYSTAT)] |= 0x0008;
		break;
	// Bits 4 and 5 are the game's; the rest show the transfers.
	case REG_JOYSTAT:
		gba->io[IO(REG_JOYSTAT)] = (gba->io[IO(REG_JOYSTAT)] & ~0x0030) | (value & 0x0030);
		break;
	}
}

// Returns the serial registers as they read.
uint16_t sioRead(struct GBA* gba, uint32_t address) {
	switch (address) {
	case REG_SIOCNT:
		return gba->sio.siocnt;
	case REG_RCNT:
		return gba->sio.rcnt;
	case REG_SIODATA32_LO:
	case REG_SIODATA32_HI:
	case REG_SIOMULTI2:
	case REG_SIOMULTI3:
	case REG_SIODATA8:
	case REG_JOYCNT:
	case REG_JOYSTAT:
		return gba->io[address >> 1];
	case REG_JOY_RECV_LO:
	case REG_JOY_RECV_HI:
		gba->io[IO(REG_JOYSTAT)] &= ~0x0002;
		return gba->io[address >> 1];
	default:
		return 0;
	}
}

enum {
	JOY_STATUS = 0x00,
	JOY_READ = 0x14,
	JOY_WRITE = 0x15,
	JOY_RESET = 0xFF,
};

int sioJoyCommand(struct GBA* gba, uint8_t command, uint8_t data[5]) {
	if (gba->sio.mode != SIO_JOYBUS) {
		return 0;
	}
	uint16_t* const io = gba->io;
	int length;
	// JOYCNT: bit 0 a reset came, 1 a write, 2 a read; each requests the interrupt if bit 6 is set.
	uint16_t flag;
	switch (command) {
	case JOY_RESET:
	case JOY_STATUS:
		// The device type, a GBA, then JOYSTAT.
		data[0] = 0x00;
		data[1] = 0x04;
		data[2] = io[IO(REG_JOYSTAT)];
		if (command == JOY_STATUS) {
			return 3;
		}
		flag = 0x0001;
		length = 3;
		break;
	case JOY_WRITE:
		io[IO(REG_JOY_RECV_LO)] = data[0] | data[1] << 8;
		io[IO(REG_JOY_RECV_HI)] = data[2] | data[3] << 8;
		io[IO(REG_JOYSTAT)] |= 0x0002;
		data[0] = io[IO(REG_JOYSTAT)];
		flag = 0x0002;
		length = 1;
		break;
	case JOY_READ:
		data[0] = io[IO(REG_JOY_TRANS_LO)];
		data[1] = io[IO(REG_JOY_TRANS_LO)] >> 8;
		data[2] = io[IO(REG_JOY_TRANS_HI)];
		data[3] = io[IO(REG_JOY_TRANS_HI)] >> 8;
		data[4] = io[IO(REG_JOYSTAT)];
		io[IO(REG_JOYSTAT)] &= ~0x0008;
		flag = 0x0004;
		length = 5;
		break;
	default:
		return 0;
	}
	io[IO(REG_JOYCNT)] |= flag;
	if (io[IO(REG_JOYCNT)] & 0x0040) {
		gbaRaiseIrq(gba, IRQ_SIO, 0);
	}
	return length;
}
