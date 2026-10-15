/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "decode.h"
#include "gba.h"
#include "jit.h"

#include <stdlib.h>
#include <string.h>

enum {
	SP_BASE_SYSTEM = 0x03007F00,
	SP_BASE_IRQ = 0x03007FA0,
	SP_BASE_SUPERVISOR = 0x03007FE0,
};

// The interrupt controller is a short pipeline: a write to IE, IF or IME, or a request, shows in the
// registers a cycle later; whether an enabled interrupt is waiting (which wakes a halted CPU) a cycle
// after that; and the CPU's IRQ line a cycle after that. Each stage is an event, from a small pool
// since several can be in flight.
static void freeChange(void* context) {
	((struct IrqChange*) context)->busy = false;
}

// Schedules a stage's change with a free slot of its pool
static void scheduleChange(struct GBA* gba, struct IrqChange* slots, bool value, int32_t when) {
	struct IrqChange* change = &slots[0];
	while (change->busy && change < &slots[3]) {
		++change;
	}
	change->value = value;
	change->busy = true;
	timingSchedule(gba, &change->event, when);
}

// The second stage: IE & IF as the halt logic sees it
static void availableChanged(struct GBA* gba, void* context, uint32_t late) {
	(void) late;
	freeChange(context);
	gba->irq.available = ((struct IrqChange*) context)->value;
}

// The third: the line the CPU samples between instructions
static void lineChanged(struct GBA* gba, void* context, uint32_t late) {
	(void) late;
	freeChange(context);
	gba->irq.line = ((struct IrqChange*) context)->value;
}

// The first: the registers take their new values, and the later stages are scheduled if they change
static void applyIrq(struct GBA* gba, void* context, uint32_t late) {
	struct Irq* irq = &gba->irq;
	gbaIrqSync(gba);
	freeChange(context);
	gba->io[IO(REG_IE)] = irq->ie;
	gba->io[IO(REG_IF)] = irq->iflags;
	gba->io[IO(REG_IME)] = irq->ime;
	bool available = irq->ie & irq->iflags;
	if (available != irq->available) {
		scheduleChange(gba, irq->availableChange, available, 1 - (int32_t) late);
	}
	bool line = irq->ime && available;
	if (line != irq->lineDriven) {
		irq->lineDriven = line;
		scheduleChange(gba, irq->lineChange, line, 2 - (int32_t) late);
	}
}

// A pool's four events, all with the same callback
static void initChanges(struct IrqChange* slots, EventCallback callback, unsigned priority) {
	for (int i = 0; i < 4; ++i) {
		slots[i].event.callback = callback;
		slots[i].event.context = &slots[i];
		slots[i].event.priority = priority;
	}
}

// Nothing enabled, nothing requested, nothing in flight
static void irqReset(struct GBA* gba) {
	struct Irq* irq = &gba->irq;
	irq->ie = 0;
	irq->iflags = 0;
	irq->ime = false;
	irq->available = false;
	irq->line = false;
	irq->lineDriven = false;
	irq->quietPending = false;
	irq->ifStable = timingCurrentTime(gba) + INT32_MAX;
	for (int i = 0; i < 4; ++i) {
		irq->apply[i].busy = false;
		irq->availableChange[i].busy = false;
		irq->lineChange[i].busy = false;
	}
}

// A machine with no BIOS or cartridge yet
struct GBA* gbaCreate(void) {
	struct GBA* gba = calloc(1, sizeof(*gba));
	if (!gba) {
		return NULL;
	}
	renderInit();
	memoryInit(gba);
	savedataInit(gba);
	videoInit(gba);
	audioInit(gba);
	dmaInit(gba);
	timerInit(gba);
	sioInit(gba);
	initChanges(gba->irq.lineChange, lineChanged, 0);
	initChanges(gba->irq.availableChange, availableChanged, 1);
	initChanges(gba->irq.apply, applyIrq, 2);
	gba->keysLast = 0x400;
	gba->cpu.nextEvent = 0x7FFFFFFF;
	if (!jitInit(gba)) {
		free(gba);
		return NULL;
	}
	return gba;
}

void gbaDestroy(struct GBA* gba) {
	renderThreadStop(gba);
	jitDeinit(gba);
	cartFree(gba);
	free(gba->rom);
	free(gba->multiboot);
	free(gba);
}

// The 16 KiB BIOS image
bool gbaLoadBios(struct GBA* gba, const void* data, size_t size) {
	if (size != SIZE_BIOS) {
		return false;
	}
	memcpy(gba->bios, data, SIZE_BIOS);
	gba->biosLoaded = true;
	return true;
}

// The next power of two at or above value
static uint32_t toPow2(uint32_t value) {
	uint32_t p = 1;
	while (p < value) {
		p <<= 1;
	}
	return p;
}

// Whether a word could be an address in a region's first 128 KiB
static bool addressIn(uint32_t word, int region) {
	return (word & ~0x1FFFFu) == (uint32_t) region << 24;
}

// A multiboot image, as mGBA tells one from a cartridge: an entry at 0xC0 that branches on, or code
// after it that points into EWRAM rather than the cartridge
static bool multibootImage(const uint8_t* data, size_t size) {
	if (size > SIZE_EWRAM || size < 0xC4 || data[3] != 0xEA || data[0xB2] != 0x96) {
		return false;
	}
	struct Inst in;
	uint32_t word;
	memcpy(&word, &data[0xC0], 4);
	decodeArm(&in, 0xC0, word);
	if (in.kind == I_B && !in.link) {
		int32_t offset = in.target - 0xC8;
		// 28 is an old toolchain's loop
		if (offset <= 0 || offset == 28) {
			return false;
		}
		if (offset != 24) {
			return true;
		}
	}
	int ewram = 0;
	int cartridge = 0;
	for (uint32_t pc = 0xC4; pc < 0xC4 + 128 * 4 && pc + 4 <= size; pc += 4) {
		memcpy(&word, &data[pc], 4);
		ewram += addressIn(word, REGION_EWRAM);
		cartridge += addressIn(word, REGION_ROM0);
		decodeArm(&in, pc, word);
		if (in.kind != I_MEM || !in.load || in.rn != 15 || !in.op2.isImm) {
			continue;
		}
		uint32_t at = pc + 8 + (in.up ? in.op2.imm : -in.op2.imm);
		if (at + 4 > size) {
			break;
		}
		memcpy(&word, &data[at], 4);
		ewram += addressIn(word, REGION_EWRAM);
		cartridge += addressIn(word, REGION_ROM0);
	}
	return cartridge < 2 && ewram;
}

// The cartridge image, copied. A multiboot image, which runs from work RAM, is kept apart and the
// cartridge left empty.
bool gbaLoadRom(struct GBA* gba, const void* data, size_t size) {
	if (!cartLoad(gba, data, size)) {
		return false;
	}
	// Like mGBA, the first 32 MiB of a bigger file, as ROM hacks that run past it need
	if (size > SIZE_ROM) {
		size = SIZE_ROM;
	}
	free(gba->rom);
	free(gba->multiboot);
	gba->multiboot = NULL;
	gba->multibootSize = 0;
	if (multibootImage(data, size)) {
		// No cartridge
		gba->multiboot = malloc(size);
		if (!gba->multiboot) {
			return false;
		}
		memcpy(gba->multiboot, data, size);
		gba->multibootSize = size;
		size = 0;
	}
	// The full 32 MiB is mapped so out-of-range reads can't leave the buffer
	gba->rom = calloc(1, SIZE_ROM);
	if (!gba->rom) {
		return false;
	}
	memcpy(gba->rom, data, size);
	// A Classic NES Series cartridge repeats its 1 MiB all through the cartridge space; the games read
	// it there, and drop button presses without it
	if (size >= 0xB0 && gba->rom[0xAC] == 'F' && !(size & (size - 1))) {
		for (size_t at = size; at < SIZE_ROM; at += size) {
			memcpy(&gba->rom[at], gba->rom, size);
		}
		size = SIZE_ROM;
	}
	gba->romSize = size;
	gba->romMask = toPow2(size) - 1;
	return true;
}

// Power-on. With skipBios the machine starts as the BIOS would have left it, at the cartridge's
// entry point; otherwise at the BIOS's reset vector.
void gbaReset(struct GBA* gba, bool skipBios) {
	struct CPU* cpu = &gba->cpu;
	gpioSetDevices(gba, overridesApply(gba));
	cartReset(gba);
	cpuReset(gba);
	// The BIOS hangs on a cartridge whose logo is wrong; like mGBA, start it straight away
	if (!overridesLogoValid(gba)) {
		skipBios = true;
	}

	cpuSetPrivilegeMode(gba, MODE_IRQ);
	cpu->r[13] = SP_BASE_IRQ;
	cpuSetPrivilegeMode(gba, MODE_SUPERVISOR);
	cpu->r[13] = SP_BASE_SUPERVISOR;
	cpuSetPrivilegeMode(gba, MODE_SYSTEM);
	cpu->r[13] = SP_BASE_SYSTEM;

	gba->earlyExit = false;
	gba->keysLast = 0x400;
	timingClear(gba);
	irqReset(gba);
	cpu->cycles = 0;
	cpu->nextEvent = 0;
	memoryReset(gba);
	gpioReset(gba);
	savedataReset(gba);
	dmaReset(gba);
	videoReset(gba);
	audioReset(gba);
	ioInit(gba);
	timerInit(gba);
	sioReset(gba);
	jitFlush(gba);
	if (gba->multiboot) {
		memcpy(gba->ewram, gba->multiboot, gba->multibootSize);
	}

	if (skipBios && cpu->r[15] == 4) {
		// A multiboot image at its header's branch, as mGBA starts one: kiosk images have no entry at
		// 0xC0 and fall into 0xE0's wait for a GameCube
		cpu->r[15] = gba->multiboot ? 0x02000000 : 0x08000000;
		videoStartAt(gba, 0x7E, 117);
		gba->io[IO(REG_POSTFLG)] = 1;
		cpuWritePc(gba);
	}
	stepReset(gba);
}

// Runs until the next frame is complete
void gbaRunFrame(struct GBA* gba) {
	uint32_t frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame) {
		jitRunLoop(gba);
	}
}

// The buttons held, a bit each, set as 1; the register has them the other way up
void gbaSetKeys(struct GBA* gba, uint16_t keys) {
	gba->keysActive = keys & 0x3FF;
	// Kept as read, for compiled code's loads
	gba->io[IO(REG_KEYINPUT)] = 0x3FF ^ gba->keysActive;
	gbaTestKeypadIrq(gba, 0);
}

// A request from a device: its IF bit, by way of the pipeline
void gbaRaiseIrq(struct GBA* gba, int irq, uint32_t late) {
	gba->irq.iflags |= 1 << irq;
	scheduleChange(gba, gba->irq.apply, false, 1 - (int32_t) late);
}

// An HBlank IRQ request that IE leaves out only sets IF, a cycle after H-blank, and the register shows
// it a cycle later. With nothing else to do, those requests aren't events: IF is worked out when read,
// written, or changed by the controller.
bool gbaQuietHblank(struct GBA* gba) {
	return (gba->io[IO(REG_DISPSTAT)] & 0x0010) && !(gba->irq.ie & (1 << IRQ_HBLANK));
}

// Brings IF up to date with H-blank requests that were left to be worked out
void gbaIrqSync(struct GBA* gba) {
	struct Irq* irq = &gba->irq;
	if (!irq->quietPending) {
		return;
	}
	uint32_t now = timingCurrentTime(gba);
	uint32_t hblank = videoNextHblank(gba, irq->quietFrom);
	irq->ifStable = hblank + 2;
	if ((int32_t) (now - (hblank + 1)) < 0) {
		return;
	}
	irq->iflags |= 1 << IRQ_HBLANK;
	if ((int32_t) (now - (hblank + 2)) < 0) {
		irq->quietFrom = hblank;
		return;
	}
	// Set until IF is written
	gba->io[IO(REG_IF)] |= 1 << IRQ_HBLANK;
	irq->quietPending = false;
	irq->ifStable = now + INT32_MAX;
}

// Requests from H-blanks at from or later
static void startQuiet(struct GBA* gba, uint32_t from) {
	struct Irq* irq = &gba->irq;
	irq->quietPending = true;
	irq->quietFrom = from;
	irq->ifStable = videoNextHblank(gba, from) + 2;
}

// After DISPSTAT's HBlank IRQ enable or IE changed. Leaving quiet, a request under way goes on as
// events: the IF update a cycle after an H-blank just now, or the register's a cycle after that.
void gbaIrqQuietChanged(struct GBA* gba, bool wasQuiet) {
	struct Irq* irq = &gba->irq;
	bool quiet = gbaQuietHblank(gba);
	if (quiet == wasQuiet) {
		return;
	}
	if (quiet) {
		// An H-blank just now went by as an event, before the change
		startQuiet(gba, timingCurrentTime(gba) + 1);
		return;
	}
	if (irq->quietPending) {
		uint32_t now = timingCurrentTime(gba);
		uint32_t hblank = videoNextHblank(gba, irq->quietFrom);
		if (hblank == now) {
			timingDeschedule(gba, &gba->video.irqEvent[1]);
			timingSchedule(gba, &gba->video.irqEvent[1], 1);
		} else if ((int32_t) (now - hblank) > 0 && (int32_t) (now - (hblank + 2)) < 0) {
			scheduleChange(gba, irq->apply, false, hblank + 2 - now);
		}
	}
	irq->quietPending = false;
	irq->ifStable = timingCurrentTime(gba) + INT32_MAX;
}

// The three interrupt registers. Each write goes through the pipeline's first stage a cycle later.
void gbaWriteIe(struct GBA* gba, uint16_t value) {
	gbaIrqSync(gba);
	bool wasQuiet = gbaQuietHblank(gba);
	uint16_t old = gba->irq.ie;
	gba->irq.ie = value & 0x3FFF;
	scheduleChange(gba, gba->irq.apply, false, 1);
	if ((old ^ gba->irq.ie) & (1 << IRQ_HBLANK)) {
		gbaIrqQuietChanged(gba, wasQuiet);
		// H-blank events on every line, or not
		videoHblankDmaArmed(gba);
	}
}

// Writing a 1 to an IF bit acknowledges that interrupt
void gbaWriteIf(struct GBA* gba, uint16_t value) {
	gbaIrqSync(gba);
	gba->irq.iflags &= ~value;
	scheduleChange(gba, gba->irq.apply, false, 1);
	if ((value & (1 << IRQ_HBLANK)) && gbaQuietHblank(gba)) {
		// An H-blank just now asks after the write
		startQuiet(gba, timingCurrentTime(gba));
	}
}

void gbaWriteIme(struct GBA* gba, uint16_t value) {
	gba->irq.ime = value & 1;
	scheduleChange(gba, gba->irq.apply, false, 1);
}

// The key interrupt: any of the chosen keys, or all of them at once, as KEYCNT says. It is asked
// for again only when the keys change.
void gbaTestKeypadIrq(struct GBA* gba, uint32_t late) {
	uint16_t keysLast = gba->keysLast;
	uint16_t keysActive = gba->keysActive;
	uint16_t keycnt = gba->io[IO(REG_KEYCNT)];
	if (!(keycnt & 0x4000)) {
		return;
	}
	gba->keysLast = keysActive;
	bool isAnd = keycnt & 0x8000;
	keycnt &= 0x3FF;
	if (isAnd && keycnt == (keysActive & keycnt)) {
		if (keysLast == keysActive) {
			return;
		}
		gbaRaiseIrq(gba, IRQ_KEYPAD, late);
	} else if (!isAnd && (keysActive & keycnt)) {
		gbaRaiseIrq(gba, IRQ_KEYPAD, late);
	} else {
		gba->keysLast = 0x400;
	}
}

// Halting takes a cycle on top of the write
void gbaHalt(struct GBA* gba) {
	gba->cpu.cycles += 1;
	gba->cpu.nextEvent = gba->cpu.cycles;
	gba->cpu.halted = 1;
}

// STOP is not emulated beyond making compiled code look at events
void gbaStop(struct GBA* gba) {
	gba->cpu.nextEvent = gba->cpu.cycles;
}

// Asks the run loop to return to its caller as soon as it can
void gbaInterrupt(struct GBA* gba) {
	gba->earlyExit = true;
	timingInterrupt(gba);
}

// Once a frame, in case the keys changed
void gbaFrameStarted(struct GBA* gba, uint32_t late) {
	gbaTestKeypadIrq(gba, late);
}

void gbaFrameEnded(struct GBA* gba) {
	(void) gba;
}
