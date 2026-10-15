/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

// The serial port with nothing attached. Which registers hold what is written depends on the mode.
// In multiplayer mode the open SI line makes the GBA a child, so it never starts a transfer.

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

// A transfer starts this long after the write that starts it
static const int TRANSFER_DELAY = 6;

// The mode is chosen by bits of RCNT and SIOCNT together
static void switchMode(struct GBA* gba) {
	struct Sio* sio = &gba->sio;
	unsigned mode = ((sio->rcnt & 0xC000) | (sio->siocnt & 0x3000)) >> 12;
	sio->mode = mode < 8 ? (int) (mode & 0x3) : (int) (mode & 0xC);
}

// A transfer's end: with nothing attached zeros were received, the busy bit clears, and the
// interrupt is asked for if enabled
static void sioFinish(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	struct Sio* sio = &gba->sio;
	switch (sio->mode) {
	case SIO_NORMAL_8:
		gba->io[IO(REG_SIODATA8)] = 0;
		break;
	case SIO_NORMAL_32:
		gba->io[IO(REG_SIODATA32_LO)] = 0;
		gba->io[IO(REG_SIODATA32_HI)] = 0;
		break;
	default:
		return;
	}
	sio->siocnt &= ~0x0080;
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	gba->io[IO(REG_RCNT)] = sio->rcnt;
	if (sio->siocnt & 0x4000) {
		gbaRaiseIrq(gba, IRQ_SIO, late);
	}
}

void sioInit(struct GBA* gba) {
	gba->sio.completeEvent.callback = sioFinish;
	gba->sio.completeEvent.context = NULL;
	gba->sio.completeEvent.priority = 0x80;
}

// General-purpose mode, as at power-on
void sioReset(struct GBA* gba) {
	gba->sio.rcnt = 0x8000;
	gba->sio.siocnt = 0;
	gba->io[IO(REG_RCNT)] = gba->sio.rcnt;
	switchMode(gba);
}

// How long a transfer takes in each mode at the baud rate or clock chosen
static int32_t transferCycles(int mode, uint16_t siocnt) {
	switch (mode) {
	case SIO_NORMAL_8:
		return 8 * CPU_FREQUENCY / (((siocnt & 2) ? 2048 : 256) * 1024);
	case SIO_NORMAL_32:
		return 32 * CPU_FREQUENCY / (((siocnt & 2) ? 2048 : 256) * 1024);
	default:
		return 0;
	}
}

// SIOCNT: setting the start bit with the internal clock begins a transfer
static void writeSiocnt(struct GBA* gba, uint16_t value) {
	struct Sio* sio = &gba->sio;
	bool busy = sio->siocnt & 0x0080;
	sio->siocnt = value & 0x3000;
	switchMode(gba);
	value &= sio->mode == SIO_UART ? 0x7FAF : 0x7F8F;
	bool start = !busy && (value & 0x0080);
	switch (sio->mode) {
	case SIO_NORMAL_8:
	case SIO_NORMAL_32:
		// With an external clock and nothing attached, the transfer never ends
		start &= value & 0x0001;
		break;
	default:
		start = false;
		break;
	}
	sio->siocnt = value;
	// Kept in the register file too, for byte writes to merge with
	gba->io[IO(REG_SIOCNT)] = sio->siocnt;
	gba->io[IO(REG_RCNT)] = sio->rcnt;
	if (start) {
		timingDeschedule(gba, &sio->completeEvent);
		timingSchedule(gba, &sio->completeEvent, TRANSFER_DELAY + transferCycles(sio->mode, sio->siocnt));
	}
}

// RCNT: the mode's top bits, and the pins in general-purpose mode
static void writeRcnt(struct GBA* gba, uint16_t value) {
	struct Sio* sio = &gba->sio;
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
	gba->io[IO(REG_RCNT)] = sio->rcnt;
}

// The serial registers
void sioWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	struct Sio* sio = &gba->sio;
	switch (address) {
	case REG_SIODATA32_LO:
	case REG_SIODATA32_HI:
		if (sio->mode == SIO_NORMAL_32) {
			gba->io[address >> 1] = value;
		}
		break;
	case REG_SIOCNT:
		writeSiocnt(gba, value);
		break;
	case REG_SIODATA8:
		if (sio->mode != SIO_UART) {
			gba->io[address >> 1] = value;
		}
		break;
	case REG_RCNT:
		writeRcnt(gba, value);
		break;
	case REG_JOYCNT: {
		// The three flags clear on writing 1; the IRQ enable keeps what is written
		uint16_t joycnt = gba->io[IO(REG_JOYCNT)];
		gba->io[IO(REG_JOYCNT)] = (joycnt & ~(value & 0x0007) & ~0x0040) | (value & 0x0040);
		break;
	}
	case REG_JOY_TRANS_LO:
	case REG_JOY_TRANS_HI:
		gba->io[IO(REG_JOYSTAT)] |= 0x0008;
		break;
	}
}

// The serial registers as they read
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
