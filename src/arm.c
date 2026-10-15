/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// The ARM7TDMI's modes and banked registers, CPSR and PC writes, reset, and the multiplier's carry.

enum {
	BANK_NONE,
	BANK_FIQ,
	BANK_IRQ,
	BANK_SUPERVISOR,
	BANK_ABORT,
	BANK_UNDEFINED,
};

// Returns which set of banked r13 and r14 a mode uses.
static int selectBank(unsigned mode) {
	switch (mode) {
	case MODE_FIQ:
		return BANK_FIQ;
	case MODE_IRQ:
		return BANK_IRQ;
	case MODE_SUPERVISOR:
		return BANK_SUPERVISOR;
	case MODE_ABORT:
		return BANK_ABORT;
	case MODE_UNDEFINED:
		return BANK_UNDEFINED;
	default:
		return BANK_NONE;
	}
}

// User and System mode have no saved status register.
bool cpuModeHasSpsr(unsigned mode) {
	return mode != MODE_SYSTEM && mode != MODE_USER;
}

// Changes mode: the registers the old mode banks are put away and the new mode's are brought in.
// FIQ also banks r8 to r12.
void cpuSetPrivilegeMode(struct GBA* gba, unsigned mode) {
	struct CPU* cpu = &gba->cpu;
	if (mode == cpu->mode) {
		return;
	}
	int newBank = selectBank(mode);
	int oldBank = selectBank(cpu->mode);
	if (newBank != oldBank) {
		if (mode == MODE_FIQ || cpu->mode == MODE_FIQ) {
			int oldFiq = oldBank == BANK_FIQ;
			int newFiq = newBank == BANK_FIQ;
			for (int i = 0; i < 5; ++i) {
				cpu->bank[oldFiq][2 + i] = cpu->r[8 + i];
				cpu->r[8 + i] = cpu->bank[newFiq][2 + i];
			}
		}
		cpu->bank[oldBank][0] = cpu->r[13];
		cpu->bank[oldBank][1] = cpu->r[14];
		cpu->r[13] = cpu->bank[newBank][0];
		cpu->r[14] = cpu->bank[newBank][1];
		cpu->bankSpsr[oldBank] = cpu->spsr;
		cpu->spsr = cpu->bankSpsr[newBank];
	}
	cpu->mode = mode;
}

// After the whole CPSR changes, switches the instruction set and register bank.
void cpuWriteCpsr(struct GBA* gba, uint32_t cpsr) {
	gba->cpu.cpsr = cpsr;
	cpuSetPrivilegeMode(gba, cpsr & 0x1F);
}

// After a write to the PC, for the paths that count a branch as a whole: sets the region code runs
// from, and returns what refilling the pipeline costs.
int32_t cpuWritePc(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	bool thumb = cpu->cpsr & PSR_T;
	uint32_t pc = cpu->r[15] & ~1u;
	memorySetActiveRegion(gba, pc);
	// Code that can't be fetched keeps the wait states of the region before it.
	int region = gba->activeRegion < 0 ? gba->fetchRegion : gba->activeRegion;
	if (thumb) {
		cpu->r[15] = pc + 2;
		return 2 + gba->waitN16[region] + gba->waitS16[region];
	}
	cpu->r[15] = pc + 4;
	return 2 + gba->waitN32[region] + gba->waitS32[region];
}

// Clears the CPU, in System mode at address 0; gbaReset puts it where the BIOS or a game starts.
void cpuReset(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	memset(cpu, 0, sizeof(*cpu));
	cpu->mode = MODE_SYSTEM;
	cpu->cpsr = MODE_SYSTEM;
	gba->activeRegion = -1;
	cpuWritePc(gba);
	cpu->cycles = 0;
	cpu->nextEvent = 0;
}

// Returns the carry a flag-setting multiply leaves, from the multiplier's Booth stages and
// carry-save adder. After zaydlang and calc84maniac's analysis (2024, zlib licence), as
// NanoBoyAdvance applies it.

// The multiplier ran all four stages: the last Booth addend is negative only for top bits 10.
bool cpuMultiplyCarryFull(uint32_t multiplier) {
	return (multiplier >> 30) == 2;
}

// It stopped early, or this is a long multiply's low half.
bool cpuMultiplyCarryLow(uint32_t multiplicand, uint32_t multiplier, uint32_t accum) {
	// A set low bit makes negating invert the upper bits; it never reaches the carry.
	multiplicand |= 1;
	uint32_t booth = (int32_t) (multiplier << 31) >> 31;
	uint32_t carry = multiplicand * booth;
	uint32_t sum = carry + accum;
	int shift = 29;
	do {
		// Eight multiplier bits per stage, two per Booth step.
		for (int i = 0; i < 4; ++i, shift -= 2) {
			uint32_t next = (int32_t) (multiplier << shift) >> shift;
			uint32_t addend = multiplicand * (next - booth);
			booth = next;
			accum ^= carry ^ addend;
			sum += addend;
			carry = sum - accum;
		}
	} while (booth != multiplier);
	return carry >> 31;
}

// A long multiply that ran all four stages: only the last three Booth steps reach bit 63.
bool cpuMultiplyCarryHigh(uint32_t multiplicand, uint32_t multiplier, uint32_t accumHi, bool sign) {
	if (sign) {
		multiplicand = (int32_t) multiplicand >> 6;
		multiplier = (int32_t) multiplier >> 26;
	} else {
		multiplicand >>= 6;
		multiplier >>= 26;
	}
	multiplicand |= 1;
	uint32_t carry = ~accumHi & 0x20000000;
	uint32_t accum = accumHi - 0x08000000;
	uint32_t booth0 = (int32_t) (multiplier << 27) >> 27;
	uint32_t booth1 = (int32_t) (multiplier << 29) >> 29;
	uint32_t booth2 = (int32_t) (multiplier << 31) >> 31;
	uint32_t addend = multiplicand * (booth1 - booth2);
	accum -= addend & 0x10000000;
	addend = multiplicand * (booth0 - booth1);
	accum -= addend & 0x40000000;
	uint32_t sum = accum + (addend & 0x20000000);
	accum -= carry;
	addend = multiplicand * (multiplier - booth0);
	sum += addend & 0x40000000;
	return (sum ^ accum) >> 31;
}
