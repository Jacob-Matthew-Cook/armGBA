/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit.h"

// I/O register reads and writes, with mGBA's masking and side effects.

// Sets the registers as the hardware leaves them at power-on, or, without a BIOS to run, as the
// BIOS leaves them.
void ioInit(struct GBA* gba) {
	gba->io[IO(REG_DISPCNT)] = 0x0080;
	gba->io[IO(REG_RCNT)] = 0x8000;
	gba->io[IO(REG_KEYINPUT)] = 0x3FF ^ gba->keysActive;
	gba->io[IO(REG_SOUNDBIAS)] = 0x200;
	// The affine backgrounds' matrices start as the identity: 1.0 in 8.8 fixed point.
	gba->io[IO(0x020)] = 0x100;
	gba->io[IO(0x026)] = 0x100;
	gba->io[IO(0x030)] = 0x100;
	gba->io[IO(0x036)] = 0x100;
	gba->io[IO(REG_INTERNAL_EXWAITCNT_LO)] = 0x20;
	gba->io[IO(REG_INTERNAL_EXWAITCNT_HI)] = 0xD00;
	if (!gba->biosLoaded) {
		gba->io[IO(REG_VCOUNT)] = 0x7E;
		gba->io[IO(REG_POSTFLG)] = 1;
	}
}

// Only the BIOS can set POSTFLG or halt the CPU, and POSTFLG can't be cleared.
static void writePostflg(struct GBA* gba, uint8_t value) {
	if (gba->activeRegion == REGION_BIOS) {
		gba->io[IO(REG_POSTFLG)] |= value & 1;
	}
}

// HALTCNT: sleeps until an interrupt or, with the top bit set, until a key is pressed.
static void writeHaltcnt(struct GBA* gba, uint8_t value) {
	if (gba->activeRegion != REGION_BIOS) {
		return;
	}
	if (value & 0x80) {
		gbaStop(gba);
	} else {
		gbaHalt(gba);
	}
}

// Handles a write to one of the four tone and noise channels. The sound so far is mixed first,
// since it was made with the old value.
static void writeSound(struct GBA* gba, uint32_t address, uint16_t value) {
	audioSync(gba);
	switch (address) {
	case REG_SOUND1CNT_LO:
		psgWrite(gba, 10, value);
		break;
	case REG_SOUND1CNT_HI:
		psgWrite(gba, 11, value);
		psgWrite(gba, 12, value >> 8);
		break;
	case REG_SOUND1CNT_X:
		psgWrite(gba, 13, value);
		psgWrite(gba, 14, value >> 8);
		break;
	case REG_SOUND2CNT_LO:
		psgWrite(gba, 21, value);
		psgWrite(gba, 22, value >> 8);
		break;
	case REG_SOUND2CNT_HI:
		psgWrite(gba, 23, value);
		psgWrite(gba, 24, value >> 8);
		break;
	case REG_SOUND3CNT_LO:
		gba->audio.psg.ch3.size = (value >> 5) & 1;
		gba->audio.psg.ch3.bank = (value >> 6) & 1;
		psgWrite(gba, 30, value);
		break;
	case REG_SOUND3CNT_HI:
		psgWrite(gba, 31, value);
		gba->audio.psg.ch3.volume = (value >> 13) & 7;
		break;
	case REG_SOUND3CNT_X:
		psgWrite(gba, 33, value);
		psgWrite(gba, 34, value >> 8);
		break;
	case REG_SOUND4CNT_LO:
		psgWrite(gba, 41, value);
		psgWrite(gba, 42, value >> 8);
		break;
	case REG_SOUND4CNT_HI:
		psgWrite(gba, 43, value);
		psgWrite(gba, 44, value >> 8);
		break;
	case REG_SOUNDCNT_LO:
		psgWrite(gba, 50, value);
		psgWrite(gba, 51, value >> 8);
		break;
	}
}

// Handles a 16-bit write to an I/O register at address (an offset into the I/O space). Each case
// does what the write does and masks value down to the bits that read back; a case that returns has
// stored its own.
void ioWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	// The video registers, apart from DISPSTAT and VCOUNT, belong to the renderer.
	if (address < REG_SOUND1CNT_LO && (address > REG_VCOUNT || address < REG_DISPSTAT)) {
		// Lines that have started have taken what they latch at their start.
		videoSync(gba);
		gba->io[address >> 1] = renderWriteRegister(gba, address, value);
		if (gba->renderThread) {
			renderThreadRegister(gba, address, value);
		}
		return;
	}
	// With the master switch off, the sound registers can't be written.
	if (address >= REG_SOUND1CNT_LO && address <= REG_SOUNDCNT_LO && !gba->audio.enable) {
		return;
	}

	switch (address) {
	case REG_DISPSTAT:
		value = videoWriteDispstat(gba, value);
		break;
	// Read-only.
	case REG_VCOUNT:
		return;

	case REG_SOUND1CNT_LO:
		writeSound(gba, address, value);
		value &= 0x007F;
		break;
	case REG_SOUND1CNT_HI:
		writeSound(gba, address, value);
		value &= 0xFFC0;
		break;
	case REG_SOUND1CNT_X:
		writeSound(gba, address, value);
		value &= 0x4000;
		break;
	case REG_SOUND2CNT_LO:
		writeSound(gba, address, value);
		value &= 0xFFC0;
		break;
	case REG_SOUND2CNT_HI:
		writeSound(gba, address, value);
		value &= 0x4000;
		break;
	case REG_SOUND3CNT_LO:
		writeSound(gba, address, value);
		value &= 0x00E0;
		break;
	case REG_SOUND3CNT_HI:
		writeSound(gba, address, value);
		value &= 0xE000;
		break;
	case REG_SOUND3CNT_X:
		writeSound(gba, address, value);
		value &= 0x4000;
		break;
	case REG_SOUND4CNT_LO:
		writeSound(gba, address, value);
		value &= 0xFF00;
		break;
	case REG_SOUND4CNT_HI:
		writeSound(gba, address, value);
		value &= 0x40FF;
		break;
	case REG_SOUNDCNT_LO:
		writeSound(gba, address, value);
		value &= 0xFF77;
		break;
	case REG_SOUNDCNT_HI:
		audioWriteSoundcntHi(gba, value);
		value &= 0x770F;
		break;
	case REG_SOUNDCNT_X:
		audioWriteSoundcntX(gba, value);
		value &= 0x0080;
		value |= gba->io[IO(REG_SOUNDCNT_X)] & 0xF;
		break;
	case REG_SOUNDBIAS:
		value &= 0xC3FE;
		audioWriteSoundbias(gba, value);
		break;

	// The wave RAM and the DMA addresses are 32-bit registers: a write to either half becomes a
	// write of the whole, with the other half as it stands.
	case 0x090:
	case 0x094:
	case 0x098:
	case 0x09C:
	case 0x0B0:
	case 0x0B4:
	case 0x0BC:
	case 0x0C0:
	case 0x0C8:
	case 0x0CC:
	case 0x0D4:
	case 0x0D8:
		ioWrite32(gba, address, ((uint32_t) gba->io[(address >> 1) + 1] << 16) | value);
		break;
	case 0x092:
	case 0x096:
	case 0x09A:
	case 0x09E:
	case 0x0B2:
	case 0x0B6:
	case 0x0BE:
	case 0x0C2:
	case 0x0CA:
	case 0x0CE:
	case 0x0D6:
	case 0x0DA:
		ioWrite32(gba, address - 2, gba->io[(address >> 1) - 1] | ((uint32_t) value << 16));
		break;
	case REG_FIFO_A_LO:
	case REG_FIFO_B_LO:
		ioWrite32(gba, address, ((uint32_t) gba->io[(address >> 1) + 1] << 16) | value);
		return;
	case REG_FIFO_A_HI:
	case REG_FIFO_B_HI:
		ioWrite32(gba, address - 2, gba->io[(address >> 1) - 1] | ((uint32_t) value << 16));
		return;

	// The DMA counts, which are write-only and need nothing done until the control word is written.
	case 0x0B8:
	case 0x0C4:
	case 0x0D0:
	case 0x0DC:
		break;
	case 0x0BA:
		value = dmaWriteControl(gba, 0, value);
		break;
	case 0x0C6:
		value = dmaWriteControl(gba, 1, value);
		break;
	case 0x0D2:
		value = dmaWriteControl(gba, 2, value);
		break;
	case 0x0DE:
		value = dmaWriteControl(gba, 3, value);
		break;

	// A timer's reload and control each reach the timer one cycle later, which the timer handles.
	case 0x100:
	case 0x104:
	case 0x108:
	case 0x10C:
		timerWriteReload(gba, (address - 0x100) >> 2, value);
		return;
	case 0x102:
	case 0x106:
	case 0x10A:
	case 0x10E:
		timerWriteControl(gba, (address - 0x102) >> 2, value);
		return;

	// The serial port.
	case REG_SIODATA32_LO ... 0x12E:
	case REG_RCNT ... REG_JOYSTAT:
		sioWrite(gba, address, value);
		return;

	case REG_KEYINPUT:
		// Read-only, and kept up to date with the keys.
		return;
	// The key interrupt's mask and condition.
	case REG_KEYCNT:
		value &= 0xC3FF;
		if (gba->keysLast < 0x400) {
			gba->keysLast &= gba->io[address >> 1] | ~value;
		}
		gba->io[address >> 1] = value;
		gbaTestKeypadIrq(gba, 0);
		return;
	// The cartridge's wait states. Compiled code has them built in, so a change is passed on with
	// how far the fetch in progress has got.
	case REG_WAITCNT: {
		value &= 0x5FFF;
		int32_t gap = jitFetchGap(gba);
		memoryAdjustWaitstates(gba, value);
		if (value != gba->io[IO(REG_WAITCNT)]) {
			jitWaitstatesChanged(gba, gap);
		}
		break;
	}
	case REG_IE:
		gbaWriteIe(gba, value);
		return;
	case REG_IF:
		gbaWriteIf(gba, value);
		return;
	case REG_IME:
		gbaWriteIme(gba, value);
		return;
	case REG_MAX:
		break;
	// POSTFLG and HALTCNT are the two bytes of this halfword.
	case REG_POSTFLG:
		writePostflg(gba, value);
		writeHaltcnt(gba, value >> 8);
		return;
	// The undocumented work RAM wait control at 0x800, kept past the end of the ordinary registers.
	case REG_EXWAITCNT_HI:
		address = REG_INTERNAL_EXWAITCNT_HI;
		value &= 0xFF00;
		memoryAdjustEwramWaitstates(gba, value);
		if (value != gba->io[IO(REG_INTERNAL_EXWAITCNT_HI)]) {
			jitWaitstatesChanged(gba, -1);
		}
		break;
	default:
		if (address >= REG_MAX) {
			return;
		}
		break;
	}
	gba->io[address >> 1] = value;
}

// Handles a byte write: a few registers act on a single byte, and the rest become a write of the
// halfword with its other byte as it stands.
void ioWrite8(struct GBA* gba, uint32_t address, uint8_t value) {
	if (address > SIZE_IO) {
		return;
	}
	struct Psg* psg = &gba->audio.psg;
	switch (address) {
	case REG_SOUND1CNT_HI:
		audioSync(gba);
		psgWrite(gba, 11, value);
		gba->io[IO(REG_SOUND1CNT_HI)] &= 0xFF00;
		gba->io[IO(REG_SOUND1CNT_HI)] |= value & 0xC0;
		return;
	case REG_SOUND1CNT_HI + 1:
		audioSync(gba);
		psgWrite(gba, 12, value);
		gba->io[IO(REG_SOUND1CNT_HI)] &= 0x00C0;
		gba->io[IO(REG_SOUND1CNT_HI)] |= value << 8;
		return;
	case REG_SOUND1CNT_X:
		audioSync(gba);
		psgWrite(gba, 13, value);
		return;
	case REG_SOUND1CNT_X + 1:
		audioSync(gba);
		psgWrite(gba, 14, value);
		gba->io[IO(REG_SOUND1CNT_X)] = (value & 0x40) << 8;
		return;
	case REG_SOUND2CNT_LO:
		audioSync(gba);
		psgWrite(gba, 21, value);
		gba->io[IO(REG_SOUND2CNT_LO)] &= 0xFF00;
		gba->io[IO(REG_SOUND2CNT_LO)] |= value & 0xC0;
		return;
	case REG_SOUND2CNT_LO + 1:
		audioSync(gba);
		psgWrite(gba, 22, value);
		gba->io[IO(REG_SOUND2CNT_LO)] &= 0x00C0;
		gba->io[IO(REG_SOUND2CNT_LO)] |= value << 8;
		return;
	case REG_SOUND2CNT_HI:
		audioSync(gba);
		psgWrite(gba, 23, value);
		return;
	case REG_SOUND2CNT_HI + 1:
		audioSync(gba);
		psgWrite(gba, 24, value);
		gba->io[IO(REG_SOUND2CNT_HI)] = (value & 0x40) << 8;
		return;
	case REG_SOUND3CNT_HI:
		audioSync(gba);
		psgWrite(gba, 31, value);
		return;
	case REG_SOUND3CNT_HI + 1:
		audioSync(gba);
		psg->ch3.volume = (value >> 5) & 7;
		gba->io[IO(REG_SOUND3CNT_HI)] = (value & 0xE0) << 8;
		return;
	case REG_SOUND3CNT_X:
		audioSync(gba);
		psgWrite(gba, 33, value);
		return;
	case REG_SOUND3CNT_X + 1:
		audioSync(gba);
		psgWrite(gba, 34, value);
		gba->io[IO(REG_SOUND3CNT_X)] = (value & 0x40) << 8;
		return;
	case REG_SOUND4CNT_LO:
		audioSync(gba);
		psgWrite(gba, 41, value);
		return;
	case REG_SOUND4CNT_LO + 1:
		audioSync(gba);
		psgWrite(gba, 42, value);
		gba->io[IO(REG_SOUND4CNT_LO)] = value << 8;
		return;
	case REG_SOUND4CNT_HI:
		audioSync(gba);
		psgWrite(gba, 43, value);
		gba->io[IO(REG_SOUND4CNT_HI)] &= 0x4000;
		gba->io[IO(REG_SOUND4CNT_HI)] |= value;
		return;
	case REG_SOUND4CNT_HI + 1:
		audioSync(gba);
		psgWrite(gba, 44, value);
		gba->io[IO(REG_SOUND4CNT_HI)] &= 0x00FF;
		gba->io[IO(REG_SOUND4CNT_HI)] |= (value & 0x40) << 8;
		return;
	case REG_IE:
	case REG_IE + 1:
		gbaWriteIe(gba, (gba->irq.ie & ~(0xFF << (8 * (address & 1)))) | value << (8 * (address & 1)));
		return;
	case REG_IF:
	case REG_IF + 1:
		gbaWriteIf(gba, value << (8 * (address & 1)));
		return;
	case REG_IME:
		gbaWriteIme(gba, value);
		return;
	case REG_IME + 1:
		return;
	case REG_POSTFLG:
		writePostflg(gba, value);
		return;
	case REG_POSTFLG + 1:
		writeHaltcnt(gba, value);
		return;
	// A timer's reload can be written a byte at a time; its control is one byte.
	case 0x100 ... 0x10F: {
		struct Timer* timer = &gba->timers[(address - 0x100) >> 2];
		switch (address & 3) {
		case 0:
			timerWriteReload(gba, (address - 0x100) >> 2, (timer->pendingReload & 0xFF00) | value);
			break;
		case 1:
			timerWriteReload(gba, (address - 0x100) >> 2, (timer->pendingReload & 0x00FF) | value << 8);
			break;
		case 2:
			timerWriteControl(gba, (address - 0x100) >> 2, value);
			break;
		}
		return;
	}
	default: {
		uint16_t value16 = value << (8 * (address & 1));
		value16 |= gba->io[(address & (SIZE_IO - 1)) >> 1] & ~(0xFF << (8 * (address & 1)));
		ioWrite(gba, address & ~1u, value16);
		return;
	}
	}
}

// Handles a 32-bit write: the registers that are 32 bits wide, otherwise two 16-bit writes, low
// half first.
void ioWrite32(struct GBA* gba, uint32_t address, uint32_t value) {
	switch (address) {
	case 0x090:
	case 0x094:
	case 0x098:
	case 0x09C:
		audioWriteWaveRam(gba, (address - 0x090) >> 2, value);
		break;
	case REG_FIFO_A_LO:
	case REG_FIFO_B_LO:
		value = audioWriteFifo(gba, address, value);
		break;
	case 0x0B0:
		value = dmaWriteSource(gba, 0, value);
		break;
	case 0x0B4:
		value = dmaWriteDest(gba, 0, value);
		break;
	case 0x0BC:
		value = dmaWriteSource(gba, 1, value);
		break;
	case 0x0C0:
		value = dmaWriteDest(gba, 1, value);
		break;
	case 0x0C8:
		value = dmaWriteSource(gba, 2, value);
		break;
	case 0x0CC:
		value = dmaWriteDest(gba, 2, value);
		break;
	case 0x0D4:
		value = dmaWriteSource(gba, 3, value);
		break;
	case 0x0D8:
		value = dmaWriteDest(gba, 3, value);
		break;
	default:
		ioWrite(gba, address, value & 0xFFFF);
		ioWrite(gba, address | 2, value >> 16);
		return;
	}
	gba->io[address >> 1] = value;
	gba->io[(address >> 1) + 1] = value >> 16;
}

// Handles a 16-bit read. Registers that keep time are brought up to date; write-only ones read as
// open bus; the rest read back what a write stored.
uint16_t ioRead(struct GBA* gba, uint32_t address) {
	switch (address) {
	case 0x100:
	case 0x104:
	case 0x108:
	case 0x10C:
		return timerReadCounter(gba, (address - 0x100) >> 2);
	case REG_SIODATA32_LO ... 0x12E:
	case REG_RCNT ... REG_JOYSTAT + 2:
		return sioRead(gba, address);

	case 0x010 ... 0x01E:
	case 0x020 ... 0x03E:
	case 0x040:
	case 0x042:
	case 0x044:
	case 0x046:
	case 0x04C:
	case 0x054:
	case REG_FIFO_A_LO:
	case REG_FIFO_A_HI:
	case REG_FIFO_B_LO:
	case REG_FIFO_B_HI:
	case 0x0B0 ... 0x0B6:
	case 0x0BC ... 0x0C2:
	case 0x0C8 ... 0x0CE:
	case 0x0D4 ... 0x0DA:
		// Write-only.
		return memoryLoadBad(gba);

	// The DMA counts and the unused register after IME read as 0.
	case 0x0B8:
	case 0x0C4:
	case 0x0D0:
	case 0x0DC:
	case REG_MAX:
		return 0;


	case 0x090:
		return audioReadWaveRam(gba, 0) & 0xFFFF;
	case 0x092:
		return audioReadWaveRam(gba, 0) >> 16;
	case 0x094:
		return audioReadWaveRam(gba, 1) & 0xFFFF;
	case 0x096:
		return audioReadWaveRam(gba, 1) >> 16;
	case 0x098:
		return audioReadWaveRam(gba, 2) & 0xFFFF;
	case 0x09A:
		return audioReadWaveRam(gba, 2) >> 16;
	case 0x09C:
		return audioReadWaveRam(gba, 3) & 0xFFFF;
	case 0x09E:
		return audioReadWaveRam(gba, 3) >> 16;

	case REG_DISPSTAT:
	case REG_VCOUNT:
		videoSync(gba);
		return gba->io[address >> 1];
	case REG_IF:
		gbaIrqSync(gba);
		return gba->io[address >> 1];

	case REG_SOUND1CNT_LO:
	case REG_SOUND1CNT_HI:
	case REG_SOUND1CNT_X:
	case REG_SOUND2CNT_LO:
	case REG_SOUND2CNT_HI:
	case REG_SOUND3CNT_LO:
	case REG_SOUND3CNT_HI:
	case REG_SOUND3CNT_X:
	case REG_SOUND4CNT_LO:
	case REG_SOUND4CNT_HI:
	case REG_SOUNDCNT_LO:
		if (!(gba->io[IO(REG_SOUNDCNT_X)] & 0x80)) {
			return 0;
		}
		// Fall through.
	case REG_DISPCNT:
	case REG_STEREOCNT:
	case REG_BG0CNT:
	case REG_BG1CNT:
	case REG_BG2CNT:
	case REG_BG3CNT:
	case 0x048:
	case 0x04A:
	case 0x050:
	case 0x052:
	case REG_SOUNDCNT_HI:
	case REG_SOUNDCNT_X:
	case REG_SOUNDBIAS:
	case 0x0BA:
	case 0x0C6:
	case 0x0D2:
	case 0x0DE:
	case 0x102:
	case 0x106:
	case 0x10A:
	case 0x10E:
	case REG_KEYINPUT:
	case REG_KEYCNT:
	case REG_IE:
	case REG_WAITCNT:
	case REG_IME:
	case REG_POSTFLG:
		break;
	// Unused halves of the sound registers, which read as 0 instead of open bus.
	case 0x066:
	case 0x06A:
	case 0x06E:
	case 0x076:
	case 0x07A:
	case 0x07E:
	case 0x086:
	case 0x08A:
	case 0x206:
	case 0x302:
		return 0;
	// Stored out of the way, at the end of the array.
	case REG_EXWAITCNT_LO:
	case REG_EXWAITCNT_HI:
		address += REG_INTERNAL_EXWAITCNT_LO - REG_EXWAITCNT_LO;
		break;
	default:
		return memoryLoadBad(gba);
	}
	return gba->io[address >> 1];
}
