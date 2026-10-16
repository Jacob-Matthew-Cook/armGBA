/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "../jit-internal.h"
#include "emit.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Register use in generated code:
//   x19  the GBA state     w20  cycles counted from the next event (cpu.cycles - cpu.nextEvent)
//   w21  guest NZCV in bits 31-28, as in the CPSR
//   w22-w29  guest r0-r7   w9-w15  guest r8-r14
//   w0-w8, w16, w17  scratch; memory routines take the address in w0 and a store value in w1 and
//   return a load in w0, keeping guest registers
// The guest flags may instead be live in the host NZCV, which uses the same layout and senses. jitBase
// holds the prefetch buffer's base counted like w20.

#define OFF(field) ((uint32_t) offsetof(struct GBA, field))
#define REG(n) (OFF(cpu.r) + 4 * (n))

enum {
	R_GBA = 19,
	R_CYC = 20,
	R_FLAGS = 21,
	LR = 30,
};

static const int8_t hostReg[16] = { 22, 23, 24, 25, 26, 27, 28, 29, 9, 10, 11, 12, 13, 14, 15, -1 };

// Tells the host about code that was written or changed; otherwise it may run what was there
// before.
static void flushCode(void* start, void* end) {
	__builtin___clear_cache((char*) start, (char*) end);
}

// Points the branch at site somewhere else. This is how blocks are linked and unlinked.
void backendPatchJump(uint8_t* site, const void* target) {
	patchBranch(site, target);
	flushCode(site, site + 4);
}

// Calls a C function anywhere in the address space.
static void callAbs(struct Emitter* e, const void* fn) {
	aMovImm64(e, 16, (uint64_t) (uintptr_t) fn);
	aBlr(e, 16);
}

// Loads a field of struct GBA, whatever its offset; a far one goes through x17.
static void loadField(struct Emitter* e, int rt, uint32_t offset, int size) {
	if (size == 4 && offset < 16384 && !(offset & 3)) {
		aLdr(e, rt, R_GBA, offset);
	} else if (size == 2 && offset < 8192 && !(offset & 1)) {
		aLdrh(e, rt, R_GBA, offset);
	} else if (size == 1 && offset < 4096) {
		aLdrb(e, rt, R_GBA, offset);
	} else if (size == 8 && offset < 32768 && !(offset & 7)) {
		aLdrX(e, rt, R_GBA, offset);
	} else {
		aMovImm(e, 17, offset);
		aLdst(e, size == 8 ? L_LDRX : size == 4 ? L_LDR : size == 2 ? L_LDRH : L_LDRB, rt, R_GBA, 17, L_LSL);
	}
}

// Stores to a field of struct GBA, with whichever addressing the offset allows.
static void storeField(struct Emitter* e, int rt, uint32_t offset, int size) {
	if (size == 4 && offset < 16384 && !(offset & 3)) {
		aStr(e, rt, R_GBA, offset);
	} else if (size == 2 && offset < 8192 && !(offset & 1)) {
		aStrh(e, rt, R_GBA, offset);
	} else if (size == 1 && offset < 4096) {
		aStrb(e, rt, R_GBA, offset);
	} else if (size == 8 && offset < 32768 && !(offset & 7)) {
		aStrX(e, rt, R_GBA, offset);
	} else {
		aMovImm(e, 17, offset);
		aLdst(e, size == 8 ? L_STRX : size == 4 ? L_STR : size == 2 ? L_STRH : L_STRB, rt, R_GBA, 17, L_LSL);
	}
}

// Stores a constant to a field, using w16 as scratch.
static void storeFieldImm(struct Emitter* e, uint32_t offset, uint32_t value, int size) {
	if (!value) {
		storeField(e, ZR, offset, size);
		return;
	}
	aMovImm(e, 16, value);
	storeField(e, 16, offset, size);
}

// Sets w to the time now plus cycles, minus the field at offset: negative while that time is still
// ahead.
static void timeUntil(struct Emitter* e, int w, int tmp, int32_t cycles, uint32_t offset) {
	aLdr(e, w, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_ADD, w, w, R_CYC, S_LSL, 0);
	aLdr(e, tmp, R_GBA, OFF(timing.masterCycles));
	aDp(e, A_ADD, w, w, tmp, S_LSL, 0);
	loadField(e, tmp, offset, 4);
	aDp(e, A_SUB, w, w, tmp, S_LSL, 0);
	if (cycles) {
		aAddImm(e, w, w, cycles, tmp);
	}
}

// Shared routines.

// Converts the prefetch buffer's base between jitBase and the form C code keeps; uses w16 and w17.
static void storeBase(struct Emitter* e) {
	aLdr(e, 16, R_GBA, OFF(jitBase));
	aLdr(e, 17, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_ADD, 16, 16, 17, S_LSL, 0);
	aLdr(e, 17, R_GBA, OFF(timing.masterCycles));
	aDp(e, A_ADD, 16, 16, 17, S_LSL, 0);
	aStr(e, 16, R_GBA, OFF(prefetcher.base));
}

// The prefetch buffer's start time, in the same terms as the cycle counter.
static void loadBase(struct Emitter* e) {
	aLdr(e, 16, R_GBA, OFF(prefetcher.base));
	aLdr(e, 17, R_GBA, OFF(timing.masterCycles));
	aDp(e, A_SUB, 16, 16, 17, S_LSL, 0);
	aLdr(e, 17, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_SUB, 16, 16, 17, S_LSL, 0);
	aStr(e, 16, R_GBA, OFF(jitBase));
}

// Preserves x0-x8.
static void emitSpill(struct Emitter* e) {
	for (int r = 0; r < 14; r += 2) {
		aStpW(e, hostReg[r], hostReg[r + 1], R_GBA, REG(r));
	}
	aStr(e, hostReg[14], R_GBA, REG(14));
	aLdr(e, 16, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_ADD, 16, R_CYC, 16, S_LSL, 0);
	aStr(e, 16, R_GBA, OFF(cpu.cycles));
	aLdr(e, 16, R_GBA, OFF(cpu.cpsr));
	aLsrImm(e, 17, R_FLAGS, 28);
	aBfi(e, 16, 17, 28, 4);
	aStr(e, 16, R_GBA, OFF(cpu.cpsr));
	storeBase(e);
	aRet(e);
}

// Preserves x0-x8.
static void emitReload(struct Emitter* e) {
	for (int r = 0; r < 14; r += 2) {
		aLdpW(e, hostReg[r], hostReg[r + 1], R_GBA, REG(r));
	}
	aLdr(e, hostReg[14], R_GBA, REG(14));
	aLdr(e, R_CYC, R_GBA, OFF(cpu.cycles));
	aLdr(e, 16, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_SUB, R_CYC, R_CYC, 16, S_LSL, 0);
	aLdr(e, R_FLAGS, R_GBA, OFF(cpu.cpsr));
	aLsrImm(e, R_FLAGS, R_FLAGS, 28);
	aLslImm(e, R_FLAGS, R_FLAGS, 28);
	loadBase(e);
	aRet(e);
}

// The routine C calls to run compiled code: saves the host's registers, loads the guest's, and
// jumps to the given block.
static void emitEnter(struct Jit* jit, struct Emitter* e) {
	aPush(e, 19, 20, 96);
	aStp(e, 21, 22, 16);
	aStp(e, 23, 24, 32);
	aStp(e, 25, 26, 48);
	aStp(e, 27, 28, 64);
	aStp(e, 29, LR, 80);
	aMovX(e, R_GBA, 0);
	aMovX(e, 2, SP);
	storeField(e, 2, OFF(hostStack), 8);
	aMovX(e, 0, 1);
	aBl(e, jit->reloadRoutine);
	aBr(e, 0);
}

// The way back out: stores the guest registers, restores the host's, and returns the reason.
static void emitExit(struct Jit* jit, struct Emitter* e) {
	loadField(e, 2, OFF(hostStack), 8);
	aMovX(e, SP, 2);
	aBl(e, jit->spillRoutine);
	loadField(e, 0, OFF(exitReason), 4);
	aLdp(e, 21, 22, 16);
	aLdp(e, 23, 24, 32);
	aLdp(e, 25, 26, 48);
	aLdp(e, 27, 28, 64);
	aLdp(e, 29, LR, 80);
	aPop(e, 19, 20, 96);
	aRet(e);
}

// The shared tail of the exit stubs of unlinked branches; x0 holds the link.
static void emitExitLink(struct Jit* jit, struct Emitter* e) {
	aLdr(e, 1, 0, offsetof(struct Link, targetKey));
	storeField(e, 1, OFF(exitPc), 4);
	storeField(e, 0, OFF(exitLink), 8);
	storeFieldImm(e, OFF(exitReason), EXIT_LINK, 4);
	aB(e, jit->exitRoutine);
}

// w0 holds the key.
static void emitDispatchKey(struct Jit* jit, struct Emitter* e) {
	aLsrImm(e, 2, 0, 13);
	aDp(e, A_EOR, 2, 2, 0, S_LSL, 0);
	aAndBits(e, 2, 2, 0, LOOKUP_BITS);
	loadField(e, 1, OFF(lookup), 8);
	aDp(e, A_ADD | W64, 1, 1, 2, S_LSL, 4);
	aLdr(e, 3, 1, 0);
	aDp(e, A_SUBS, ZR, 3, 0, S_LSL, 0);
	uint8_t* miss = aBcond(e, C_NE, NULL);
	aLdrX(e, 3, 1, 8);
	aBr(e, 3);
	patchHere(e, miss);
	storeField(e, 0, OFF(exitPc), 4);
	storeFieldImm(e, OFF(exitReason), EXIT_LOOKUP, 4);
	aB(e, jit->exitRoutine);
}

// memorySetActiveRegion's bookkeeping for a PC write into a region compiled code refills itself. w2
// holds the new region; uses w3.
static uint8_t* emitRegionUpdate(struct Emitter* e) {
	uint8_t* start = e->p;
	loadField(e, 3, OFF(activeRegion), 4);
	aDp(e, A_SUBS, ZR, 3, 2, S_LSL, 0);
	uint8_t* same = aBcond(e, C_EQ, NULL);
	uint8_t* notBios = aCbnz(e, 3, NULL);
	loadField(e, 3, OFF(biosPrefetchNext), 4);
	storeField(e, 3, OFF(biosPrefetch), 4);
	patchHere(e, notBios);
	storeField(e, 2, OFF(activeRegion), 4);
	storeField(e, 2, OFF(fetchRegion), 4);
	patchHere(e, same);
	aRet(e);
	return start;
}

// Puts the cycle that stopping the prefetch buffer costs, from jitStopPenalty, in w4: 0 when it is
// stopped or far ahead. Uses w6 and w7.
static void emitStopPenalty(struct Emitter* e) {
	aMov(e, 4, ZR);
	loadField(e, 6, OFF(cpu.prefetching), 1);
	uint8_t* idle = aCbz(e, 6, NULL);
	loadField(e, 6, OFF(jitBase), 4);
	aDp(e, A_SUB, 6, R_CYC, 6, S_LSL, 0);
	aDpImm(e, A_SUBS_IMM, ZR, 6, 127);
	uint8_t* far = aBcond(e, C_HI, NULL);
	loadField(e, 7, OFF(prefetcher.duty), 4);
	aDp(e, A_ADD, 6, 6, 7, S_LSL, 7);
	loadField(e, 7, OFF(prefetcher.width), 4);
	aDpImm(e, A_SUBS_IMM, ZR, 7, 4);
	uint8_t* half = aBcond(e, C_NE, NULL);
	aDpImm(e, A_ADD_IMM, 6, 6, 4096);
	patchHere(e, half);
	aAddImmX(e, 7, R_GBA, OFF(jitStopPenalty));
	aLdst(e, L_LDRB, 4, 7, 6, L_UXTW);
	patchHere(e, idle);
	patchHere(e, far);
}

// busRefillCartridge's two fetches, for w0 = target and w2 = its region (at least 8), when nothing
// can come between them; other refills go to the exact CPU through slow.
static void emitCartridgeRefill(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate, uint8_t** slow, int* slowCount) {
	int size = thumb ? 2 : 4;
	aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_ROM2_EX);
	slow[(*slowCount)++] = aBcond(e, C_HI, NULL);
	loadField(e, 3, OFF(prefetchDisabled), 1);
	slow[(*slowCount)++] = aCbnz(e, 3, NULL);
	// A new burst starts at the second fetch.
	aDpImm(e, A_ADD_IMM, 3, 0, size);
	aLogicImm(e, A_ANDS_IMM, ZR, 3, 0x1FFFF);
	slow[(*slowCount)++] = aBcond(e, C_EQ, NULL);
	// If the buffer is already reading there, the exact CPU handles it.
	loadField(e, 3, OFF(cpu.prefetching), 1);
	uint8_t* stopped = aCbz(e, 3, NULL);
	loadField(e, 3, OFF(prefetcher.head), 4);
	aDp(e, A_SUBS, ZR, 0, 3, S_LSL, 0);
	slow[(*slowCount)++] = aBcond(e, C_EQ, NULL);
	patchHere(e, stopped);
	emitStopPenalty(e);
	// Then the nonsequential fetch and the sequential one.
	aAddImmX(e, 3, R_GBA, thumb ? OFF(writePcCost16) : OFF(writePcCost32));
	aLdst(e, L_LDRB, 3, 3, 2, L_UXTW);
	aDp(e, A_ADD, 4, 4, 3, S_LSL, 0);
	aDp(e, A_ADDS, R_CYC, R_CYC, 4, S_LSL, 0);
	uint8_t* late = aBcond(e, C_PL, NULL);
	storeField(e, ZR, OFF(cpu.prefetching), 1);
	loadField(e, 3, OFF(prefetchEnabled), 1);
	uint8_t* off = aCbz(e, 3, NULL);
	// Started by the first fetch, which the second waits on.
	aMovImm(e, 3, 1);
	storeField(e, 3, OFF(cpu.prefetching), 1);
	aMovImm(e, 3, size);
	storeField(e, 3, OFF(prefetcher.width), 4);
	aAddImmX(e, 3, R_GBA, thumb ? OFF(waitS16) : OFF(waitS32));
	aLdst(e, L_LDRB, 3, 3, 2, L_UXTW);
	aDpImm(e, A_ADD_IMM, 3, 3, 1);
	storeField(e, 3, OFF(prefetcher.duty), 4);
	aLslImm(e, 3, 3, thumb ? 3 : 2);
	storeField(e, 3, OFF(prefetcher.full), 4);
	aDpImm(e, A_ADD_IMM, 3, 0, 2 * size);
	storeField(e, 3, OFF(prefetcher.head), 4);
	storeField(e, R_CYC, OFF(jitBase), 4);
	patchHere(e, off);
	aDpImm(e, A_ADD_IMM, 3, 0, 2 * size);
	aAndBits(e, 3, 3, 0, 25);
	storeField(e, 3, OFF(romNext), 4);
	storeField(e, 3, OFF(romLatch), 4);
	aMovImm(e, 3, 1);
	storeField(e, 3, OFF(cpu.fetchSeq), 1);
	storeField(e, ZR, OFF(lastAccessDma), 1);
	storeField(e, ZR, OFF(dmaIdleCredit), 4);
	// After a refill, the IRQ check uses the mask as it is now.
	loadField(e, 3, OFF(cpu.cpsr), 4);
	aUbfx(e, 3, 3, 7, 1);
	storeField(e, 3, OFF(cpu.irqMasked), 1);
	aBl(e, regionUpdate);
	if (thumb) {
		aLogicImm(e, A_ORR_IMM, 0, 0, 1);
	}
	aB(e, jit->dispatchKey);
	patchHere(e, late);
	aDp(e, A_SUB, R_CYC, R_CYC, 4, S_LSL, 0);
	slow[(*slowCount)++] = aB(e, NULL);
}

// Branches to w0, a target address in the given instruction set. Compiled code refills from BIOS,
// RAM, palette, VRAM and OAM itself; refills elsewhere, or ones an event would fall inside, go to
// the exact CPU. As in mGBA, an ARM-state PC keeps bit 1: fetches ignore it, but PC reads see it.
static void emitDispatchPc(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate) {
	uint8_t* slow[12];
	int slowCount = 0;
	aLogicImm(e, A_AND_IMM, 0, 0, ~1u);
	aLsrImm(e, 2, 0, 24);
	aDpImm(e, A_SUBS_IMM, ZR, 2, 8);
	uint8_t* far = aBcond(e, C_HS, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 2, 1);
	slow[slowCount++] = aBcond(e, C_EQ, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_IO);
	slow[slowCount++] = aBcond(e, C_EQ, NULL);
	aAddImmX(e, 3, R_GBA, thumb ? OFF(writePcCost16) : OFF(writePcCost32));
	aLdst(e, L_LDRB, 4, 3, 2, L_UXTW);
	aDp(e, A_ADDS, R_CYC, R_CYC, 4, S_LSL, 0);
	uint8_t* late = aBcond(e, C_PL, NULL);
	aBl(e, regionUpdate);
	if (thumb) {
		aLogicImm(e, A_ORR_IMM, 0, 0, 1);
	}
	aB(e, jit->dispatchKey);
	patchHere(e, late);
	aDp(e, A_SUB, R_CYC, R_CYC, 4, S_LSL, 0);
	slow[slowCount++] = aB(e, NULL);
	patchHere(e, far);
	emitCartridgeRefill(jit, e, thumb, regionUpdate, slow, &slowCount);
	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, slow[i]);
	}
	if (thumb) {
		aLogicImm(e, A_ORR_IMM, 0, 0, 1);
	}
	storeField(e, 0, OFF(exitPc), 4);
	aBl(e, jit->spillRoutine);
	aMovX(e, 0, R_GBA);
	loadField(e, 1, OFF(exitPc), 4);
	callAbs(e, jitHelperReload);
	aBl(e, jit->reloadRoutine);
	loadField(e, 0, OFF(exitPc), 4);
	aB(e, jit->dispatchKey);
}

// BX's target: bit 0 chooses the instruction set, the CPSR follows, then the block is looked up.
static void emitDispatchBx(struct Jit* jit, struct Emitter* e) {
	loadField(e, 1, OFF(cpu.cpsr), 4);
	uint8_t* toThumb = aTbnz(e, 0, 0, NULL);
	aTbz(e, 1, 5, jit->dispatchArm);
	aLogicImm(e, A_AND_IMM, 1, 1, ~(uint32_t) PSR_T);
	storeField(e, 1, OFF(cpu.cpsr), 4);
	aB(e, jit->dispatchArm);
	patchHere(e, toThumb);
	aTbnz(e, 1, 5, jit->dispatchThumb);
	aLogicImm(e, A_ORR_IMM, 1, 1, PSR_T);
	storeField(e, 1, OFF(cpu.cpsr), 4);
	aB(e, jit->dispatchThumb);
}

// Register-specified shifts take w1 = value and w2 = the shift register, and return w1 = result and
// w2 = carry. A zero amount keeps C, so the flags must be in w21.
static uint8_t* emitShiftRoutine(struct Emitter* e, int type) {
	uint8_t* start = e->p;
	aAndBits(e, 2, 2, 0, 8);
	uint8_t* zero = aCbz(e, 2, NULL);
	if (type == SHIFT_ROR) {
		aAndBits(e, 3, 2, 0, 5);
		uint8_t* rotate = aCbnz(e, 3, NULL);
		aLsrImm(e, 2, 1, 31);
		aRet(e);
		patchHere(e, rotate);
		aDp(e, A_RORV, 1, 1, 3, S_LSL, 0);
		aLsrImm(e, 2, 1, 31);
		aRet(e);
	} else {
		aDpImm(e, A_SUBS_IMM, ZR, 2, 32);
		uint8_t* big = aBcond(e, C_HS, NULL);
		if (type == SHIFT_LSL) {
			// carry = bit (32 - n)
			aDp(e, A_SUB, 3, ZR, 2, S_LSL, 0);
			aDp(e, A_LSRV, 3, 1, 3, S_LSL, 0);
			aDp(e, A_LSLV, 1, 1, 2, S_LSL, 0);
		} else {
			// carry = bit (n - 1)
			aDpImm(e, A_SUB_IMM, 3, 2, 1);
			aDp(e, A_LSRV, 3, 1, 3, S_LSL, 0);
			aDp(e, type == SHIFT_LSR ? A_LSRV : A_ASRV, 1, 1, 2, S_LSL, 0);
		}
		aAndBits(e, 2, 3, 0, 1);
		aRet(e);
		patchHere(e, big);
		switch (type) {
		case SHIFT_LSL: {
			uint8_t* over = aBcond(e, C_NE, NULL);
			aAndBits(e, 2, 1, 0, 1);
			aMov(e, 1, ZR);
			aRet(e);
			patchHere(e, over);
			aMov(e, 2, ZR);
			aMov(e, 1, ZR);
			aRet(e);
			break;
		}
		case SHIFT_LSR: {
			uint8_t* over = aBcond(e, C_NE, NULL);
			aLsrImm(e, 2, 1, 31);
			aMov(e, 1, ZR);
			aRet(e);
			patchHere(e, over);
			aMov(e, 2, ZR);
			aMov(e, 1, ZR);
			aRet(e);
			break;
		}
		case SHIFT_ASR:
			aLsrImm(e, 2, 1, 31);
			aAsrImm(e, 1, 1, 31);
			aRet(e);
			break;
		}
	}
	patchHere(e, zero);
	aUbfx(e, 2, R_FLAGS, 29, 1);
	aRet(e);
	return start;
}

// Calls fn(gba, w1) from a routine, preserving x0-x15 and the return address; the result is in w16.
static void emitPreserveCall(struct Emitter* e, const void* fn) {
	aPush(e, 0, 1, 160);
	aStp(e, 2, 3, 16);
	aStp(e, 4, 5, 32);
	aStp(e, 6, 7, 48);
	aStp(e, 8, 9, 64);
	aStp(e, 10, 11, 80);
	aStp(e, 12, 13, 96);
	aStp(e, 14, 15, 112);
	aStp(e, 17, LR, 128);
	aLdr(e, 16, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_ADD, 16, R_CYC, 16, S_LSL, 0);
	aStr(e, 16, R_GBA, OFF(cpu.cycles));
	storeBase(e);
	aMovX(e, 0, R_GBA);
	callAbs(e, fn);
	aStr(e, 0, SP, 144);
	aLdr(e, 0, R_GBA, OFF(cpu.cycles));
	aLdr(e, 1, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_SUB, R_CYC, 0, 1, S_LSL, 0);
	loadBase(e);
	aLdr(e, 16, SP, 144);
	aLdp(e, 2, 3, 16);
	aLdp(e, 4, 5, 32);
	aLdp(e, 6, 7, 48);
	aLdp(e, 8, 9, 64);
	aLdp(e, 10, 11, 80);
	aLdp(e, 12, 13, 96);
	aLdp(e, 14, 15, 112);
	aLdp(e, 17, LR, 128);
	aPop(e, 0, 1, 160);
}

// Memory access routines. Each inline path takes the bus's cycles for its region, including a
// load's internal cycle. An access an event would fall inside goes to the exact CPU's access
// instead.

// Goes through bus.c, events and all; w0 = w5 = address, w1 = a value to store.
static void emitSlowPath(struct Emitter* e, int kind) {
	// The return address identifies the guest instruction for open-bus reads.
	storeField(e, LR, OFF(siteReturn), 8);
	aLdr(e, 16, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_ADD, 16, R_CYC, 16, S_LSL, 0);
	aStr(e, 16, R_GBA, OFF(cpu.cycles));
	storeBase(e);
	aPush(e, 9, 10, 80);
	aStp(e, 11, 12, 16);
	aStp(e, 13, 14, 32);
	aStp(e, 15, LR, 48);
	// The address, for sign extension.
	aStr(e, 5, SP, 64);
	aMov(e, 3, 1);
	aMov(e, 2, 5);
	aMovImm(e, 1, kind);
	aMovX(e, 0, R_GBA);
	callAbs(e, jitSlowAccess);
	if (kind == MEM_LOADS8) {
		aSxtb(e, 0, 0);
	} else if (kind == MEM_LOADS16) {
		aLdr(e, 1, SP, 64);
		uint8_t* odd = aTbnz(e, 1, 0, NULL);
		aSxth(e, 0, 0);
		uint8_t* done = aB(e, NULL);
		patchHere(e, odd);
		aSxtb(e, 0, 0);
		patchHere(e, done);
	}
	aLdp(e, 11, 12, 16);
	aLdp(e, 13, 14, 32);
	aLdp(e, 15, LR, 48);
	aPop(e, 9, 10, 80);
	aLdr(e, R_CYC, R_GBA, OFF(cpu.cycles));
	aLdr(e, 3, R_GBA, OFF(cpu.nextEvent));
	aDp(e, A_SUB, R_CYC, R_CYC, 3, S_LSL, 0);
	loadBase(e);
	aRet(e);
}

// Loads data into w0 from [x3 + w4]; w5 holds the address.
static void emitLoadData(struct Emitter* e, int kind) {
	switch (kind) {
	case MEM_LOAD32:
		aLdst(e, L_LDR, 0, 3, 4, L_UXTW);
		aAndBits(e, 6, 5, 0, 2);
		aLslImm(e, 6, 6, 3);
		aDp(e, A_RORV, 0, 0, 6, S_LSL, 0);
		break;
	case MEM_LOAD16:
		aLdst(e, L_LDRH, 0, 3, 4, L_UXTW);
		aAndBits(e, 6, 5, 0, 1);
		aLslImm(e, 6, 6, 3);
		aDp(e, A_RORV, 0, 0, 6, S_LSL, 0);
		break;
	case MEM_LOADS16: {
		// An odd address gives the sign-extended high byte.
		uint8_t* odd = aTbnz(e, 5, 0, NULL);
		aLdst(e, L_LDRSH, 0, 3, 4, L_UXTW);
		uint8_t* done = aB(e, NULL);
		patchHere(e, odd);
		aDpImm(e, A_ADD_IMM, 4, 4, 1);
		aLdst(e, L_LDRSB, 0, 3, 4, L_UXTW);
		patchHere(e, done);
		break;
	}
	case MEM_LOAD8:
		aLdst(e, L_LDRB, 0, 3, 4, L_UXTW);
		break;
	case MEM_LOADS8:
		aLdst(e, L_LDRSB, 0, 3, 4, L_UXTW);
		break;
	}
}

// Goes to the slow path unless the access ends before the next event.
static void emitGuard(struct Emitter* e, int cycles, uint8_t** toSlow, int* slowCount) {
	aDpImm(e, A_ADDS_IMM, ZR, R_CYC, cycles);
	toSlow[(*slowCount)++] = aBcond(e, C_GE, NULL);
}

// Tells the renderer about VRAM stores by page: x3 + w4 was written.
static void emitVramPage(struct Emitter* e) {
	aLsrImm(e, 6, 4, 11);
	aAddImmX(e, 7, R_GBA, OFF(renderer.vramChanged));
	aMovImm(e, 16, 1);
	aLdst(e, L_STRB, 16, 7, 6, L_UXTW);
}

// VRAM below the mirrors; w0 = w5 = address, w1 = a value to store.
static void emitVramAccess(struct Emitter* e, int kind, uint8_t** toSlow, int* slowCount) {
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	int alignBits = size == 4 ? 2 : size == 2 ? 1 : 0;
	int cycles = (size == 4 ? 2 : 1) + (store ? 0 : 1);
	// While the display keeps the CPU out of VRAM, C works out the wait.
	loadField(e, 6, OFF(vramStall), 1);
	toSlow[(*slowCount)++] = aCbnz(e, 6, NULL);
	if (store) {
		// Once a line's H-blank has come, C draws the line first.
		timeUntil(e, 6, 7, cycles, OFF(video.drawDue));
		toSlow[(*slowCount)++] = aTbz(e, 6, 31, NULL);
	}
	aAndBits(e, 4, 0, 0, 17);
	aDpImm(e, A_SUBS_IMM, ZR, 4, SIZE_VRAM);
	toSlow[(*slowCount)++] = aBcond(e, C_HS, NULL);
	if (store) {
		loadField(e, 6, OFF(videoCode), 1);
		toSlow[(*slowCount)++] = aCbnz(e, 6, NULL);
	}
	emitGuard(e, cycles, toSlow, slowCount);
	aAndBits(e, 4, 0, alignBits, 17 - alignBits);
	aAddImmX(e, 3, R_GBA, OFF(vram));
	if (store) {
		// Stores that change something.
		aLdst(e, size == 4 ? L_LDR : L_LDRH, 6, 3, 4, L_UXTW);
		if (size == 4) {
			aDp(e, A_SUBS, ZR, 6, 1, S_LSL, 0);
		} else {
			aUxth(e, 7, 1);
			aDp(e, A_SUBS, ZR, 6, 7, S_LSL, 0);
		}
		uint8_t* same = aBcond(e, C_EQ, NULL);
		aLdst(e, size == 4 ? L_STR : L_STRH, 1, 3, 4, L_UXTW);
		emitVramPage(e);
		patchHere(e, same);
	} else {
		emitLoadData(e, kind);
	}
	aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, cycles);
	aRet(e);
}

// A byte store to VRAM, as memoryStore8 does it: into both halves of its halfword below the sprite
// tiles, and nowhere among them. w0 = w5 = address, w1 = value.
static void emitVramByteStore(struct Emitter* e, uint8_t** toSlow, int* slowCount) {
	loadField(e, 6, OFF(vramStall), 1);
	toSlow[(*slowCount)++] = aCbnz(e, 6, NULL);
	// Once a line's H-blank has come, C draws the line first.
	timeUntil(e, 6, 7, 1, OFF(video.drawDue));
	toSlow[(*slowCount)++] = aTbz(e, 6, 31, NULL);
	loadField(e, 6, OFF(videoCode), 1);
	toSlow[(*slowCount)++] = aCbnz(e, 6, NULL);
	emitGuard(e, 1, toSlow, slowCount);
	// The sprite tiles start at 0x10000, or 0x14000 in the bitmap modes.
	aAndBits(e, 4, 0, 0, 17);
	loadField(e, 6, OFF(io), 2);
	aAndBits(e, 6, 6, 0, 3);
	aDpImm(e, A_SUBS_IMM, ZR, 6, 3);
	uint8_t* tiles = aBcond(e, C_LO, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 4, 0x14000);
	uint8_t* ignoredBitmap = aBcond(e, C_HS, NULL);
	uint8_t* store = aB(e, NULL);
	patchHere(e, tiles);
	aDpImm(e, A_SUBS_IMM, ZR, 4, 0x10000);
	uint8_t* ignored = aBcond(e, C_HS, NULL);
	patchHere(e, store);
	aAndBits(e, 4, 4, 1, 16);
	aAndBits(e, 7, 1, 0, 8);
	aDp(e, A_ORR, 7, 7, 7, S_LSL, 8);
	aAddImmX(e, 3, R_GBA, OFF(vram));
	aLdst(e, L_LDRH, 6, 3, 4, L_UXTW);
	aDp(e, A_SUBS, ZR, 6, 7, S_LSL, 0);
	uint8_t* same = aBcond(e, C_EQ, NULL);
	aLdst(e, L_STRH, 7, 3, 4, L_UXTW);
	emitVramPage(e);
	patchHere(e, same);
	patchHere(e, ignored);
	patchHere(e, ignoredBitmap);
	aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, 1);
	aRet(e);
}

// romCode is set for cartridge code with the prefetch buffer, whose data accesses to the cartridge
// stop it.
static uint8_t* emitMemRoutine(struct Jit* jit, struct Emitter* e, int kind, bool romCode) {
	(void) jit;
	uint8_t* start = e->p;
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	uint32_t waitN = size == 4 ? OFF(waitN32) : OFF(waitN16);
	// The access and a load's internal cycle.
	int fixed = store ? 1 : 2;
	int alignBits = size == 4 ? 2 : size == 2 ? 1 : 0;
	uint8_t* toSlow[32];
	int slowCount = 0;

	aMov(e, 5, 0);
	aLsrImm(e, 2, 0, 24);
	aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_IWRAM);
	uint8_t* toIwram = aBcond(e, C_EQ, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_EWRAM);
	uint8_t* toEwram = aBcond(e, C_EQ, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_VRAM);
	uint8_t* toVram = aBcond(e, C_EQ, NULL);
	uint8_t* toIo = NULL;
	if (kind == MEM_LOAD16 || kind == MEM_LOADS16) {
		aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_IO);
		toIo = aBcond(e, C_EQ, NULL);
	}
	uint8_t* toRom = NULL;
	if (!store) {
		aDpImm(e, A_SUB_IMM, 3, 2, REGION_ROM0);
		aDpImm(e, A_SUBS_IMM, ZR, 3, REGION_ROM2_EX - REGION_ROM0);
		toRom = aBcond(e, C_LS, NULL);
	}
	toSlow[slowCount++] = aB(e, NULL);

	for (int r = 0; r < 2; ++r) {
		bool iwram = r == 0;
		patchHere(e, iwram ? toIwram : toEwram);
		int bits = iwram ? 15 : 18;
		if (iwram) {
			emitGuard(e, fixed, toSlow, &slowCount);
		} else {
			loadField(e, 3, waitN + REGION_EWRAM, 1);
			aDp(e, A_ADD, 3, 3, R_CYC, S_LSL, 0);
			aDpImm(e, A_ADDS_IMM, ZR, 3, fixed);
			toSlow[slowCount++] = aBcond(e, C_GE, NULL);
		}
		// w4 = offset in the region, aligned to the access.
		aAndBits(e, 4, 0, alignBits, bits - alignBits);
		aAddImmX(e, 3, R_GBA, iwram ? OFF(iwram) : OFF(ewram));
		if (store) {
			aLdst(e, size == 4 ? L_STR : size == 2 ? L_STRH : L_STRB, 1, 3, 4, L_UXTW);
			aLsrImm(e, 6, 4, 2);
			aAddImmX(e, 7, R_GBA, iwram ? OFF(iwramCode) : OFF(ewramCode));
			aLdst(e, L_LDRB, 7, 7, 6, L_UXTW);
			uint8_t* clean = aCbz(e, 7, NULL);
			// Code that checks the word when it runs only needs the helper for a store into the two
			// instructions after it (its r15 is in the state), which are already prefetched.
			aDpImm(e, A_SUBS_IMM, ZR, 7, CODE_MAP_CHECKED);
			uint8_t* other = aBcond(e, C_NE, NULL);
			aLdr(e, 6, R_GBA, REG(15));
			aDpImm(e, A_SUB_IMM, 6, 6, 4);
			aLogicImm(e, A_AND_IMM, 6, 6, ~3u);
			aLogicImm(e, A_AND_IMM, 7, 5, ~3u);
			aDp(e, A_SUB, 6, 7, 6, S_LSL, 0);
			aDpImm(e, A_SUBS_IMM, ZR, 6, 8);
			uint8_t* away = aBcond(e, C_HS, NULL);
			patchHere(e, other);
			storeField(e, LR, OFF(siteReturn), 8);
			aMov(e, 1, 5);
			emitPreserveCall(e, jitHelperCodeWrite);
			patchHere(e, clean);
			patchHere(e, away);
		} else {
			emitLoadData(e, kind);
		}
		if (iwram) {
			aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, fixed);
		} else {
			loadField(e, 3, waitN + REGION_EWRAM, 1);
			aDp(e, A_ADD, R_CYC, R_CYC, 3, S_LSL, 0);
			aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, fixed);
		}
		aRet(e);
	}

	if (toRom) {
		// Data from the cartridge stops the prefetch buffer. That costs nothing outside cartridge
		// code, and the buffer is never running in cartridge code compiled without it.
		patchHere(e, toRom);
		aAndBits(e, 3, 0, alignBits, 25 - alignBits);
		loadField(e, 6, OFF(romSize), 4);
		aDp(e, A_SUBS, ZR, 3, 6, S_LSL, 0);
		toSlow[slowCount++] = aBcond(e, C_HS, NULL);
		// An EEPROM answers in the cartridge's last region, whatever the cartridge's size.
		aDpImm(e, A_SUBS_IMM, ZR, 2, REGION_ROM2_EX);
		uint8_t* notTop = aBcond(e, C_NE, NULL);
		loadField(e, 6, OFF(savedata.type), 4);
		aDpImm(e, A_SUBS_IMM, ZR, 6, SAVEDATA_EEPROM);
		toSlow[slowCount++] = aBcond(e, C_EQ, NULL);
		aDpImm(e, A_SUBS_IMM, ZR, 6, SAVEDATA_EEPROM512);
		toSlow[slowCount++] = aBcond(e, C_EQ, NULL);
		patchHere(e, notTop);
		if (romCode) {
			emitStopPenalty(e);
		} else {
			aMov(e, 4, ZR);
		}
		aAddImmX(e, 6, R_GBA, waitN);
		aLdst(e, L_LDRB, 6, 6, 2, L_UXTW);
		aDp(e, A_ADD, 4, 4, 6, S_LSL, 0);
		aDpImm(e, A_ADD_IMM, 4, 4, fixed);
		aDp(e, A_ADD, 6, 4, R_CYC, S_LSL, 0);
		toSlow[slowCount++] = aTbz(e, 6, 31, NULL);
		aMov(e, R_CYC, 6);
		storeField(e, ZR, OFF(cpu.prefetching), 1);
		aMov(e, 4, 3);
		loadField(e, 3, OFF(rom), 8);
		emitLoadData(e, kind);
		aRet(e);
	}

	patchHere(e, toVram);
	if (kind == MEM_STORE8) {
		emitVramByteStore(e, toSlow, &slowCount);
	} else {
		emitVramAccess(e, kind, toSlow, &slowCount);
	}

	if (toIo) {
		// Registers games poll that ioRead returns as stored: KEYINPUT; VCOUNT and DISPSTAT until
		// the next line or H-blank; and IF until an H-blank IRQ request it works out, when ioRead
		// brings them up to date.
		patchHere(e, toIo);
		emitGuard(e, fixed, toSlow, &slowCount);
		aLogicImm(e, A_AND_IMM, 4, 0, 0xFFFFFE);
		uint8_t* toDispcnt = aCbz(e, 4, NULL);
		aDpImm(e, A_SUBS_IMM, ZR, 4, REG_DISPSTAT);
		uint8_t* toDispstat = aBcond(e, C_EQ, NULL);
		aDpImm(e, A_SUBS_IMM, ZR, 4, REG_VCOUNT);
		uint8_t* toVcount = aBcond(e, C_EQ, NULL);
		aDpImm(e, A_SUBS_IMM, ZR, 4, REG_KEYINPUT);
		uint8_t* toKeys = aBcond(e, C_EQ, NULL);
		aDpImm(e, A_SUBS_IMM, ZR, 4, REG_IF);
		toSlow[slowCount++] = aBcond(e, C_NE, NULL);
		timeUntil(e, 6, 7, fixed, OFF(irq.ifStable));
		toSlow[slowCount++] = aTbz(e, 6, 31, NULL);
		uint8_t* toLoad = aB(e, NULL);
		patchHere(e, toDispstat);
		patchHere(e, toVcount);
		timeUntil(e, 6, 7, fixed, OFF(video.stableUntil));
		toSlow[slowCount++] = aTbz(e, 6, 31, NULL);
		patchHere(e, toLoad);
		patchHere(e, toDispcnt);
		patchHere(e, toKeys);
		aAddImmX(e, 3, R_GBA, OFF(io));
		emitLoadData(e, kind);
		aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, fixed);
		aRet(e);
	}

	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, toSlow[i]);
	}
	aMov(e, 0, 5);
	emitSlowPath(e, kind);
	return start;
}

// Blocks' bodies are kept apart from their cold code (slow paths, exits, and whole careful copies),
// so the code a game spends its time in is dense in the caches. On a Cortex-A55 with 32 KiB of
// instruction cache and 512 KiB behind it, that decides most of a busy game's speed. The code
// memory is laid out in stretches, each with a part for bodies and a part for cold code, small
// enough for a conditional branch (1 MiB either way) to reach from anywhere in one part to anywhere
// in the other.
enum {
	HOT_PART = 352 << 10,
	COLD_PART = 640 << 10,
	STRETCH = HOT_PART + COLD_PART,
	// Room a block may need in each part.
	HOT_ROOM = 32 << 10,
	COLD_ROOM = 64 << 10,
};

// Lays out a stretch at base: block bodies fill its first part and cold code its second.
static void startStretch(struct Jit* jit, uint8_t* base) {
	jit->codeNext = base;
	jit->hotEnd = base + HOT_PART;
	jit->coldNext = jit->hotEnd;
	jit->coldEnd = base + STRETCH;
}

// After a flush, compiling starts in a new stretch.
void backendReset(struct Jit* jit) {
	startStretch(jit, jit->codeNext);
}

// Whether an address is in a stretch's cold part.
bool backendColdSide(const struct Jit* jit, const uint8_t* p) {
	return p >= jit->codeStart && (size_t) (p - jit->codeStart) % STRETCH >= HOT_PART;
}

// Emits everything shared: the entry and exit, the dispatchers, and the memory and shift routines.
// They are emitted once and sit before the first block.
bool backendInit(struct Jit* jit) {
	struct Emitter e = { .start = jit->codeNext, .p = jit->codeNext, .end = jit->codeEnd };
	jit->spillRoutine = e.p;
	emitSpill(&e);
	jit->reloadRoutine = e.p;
	emitReload(&e);
	jit->enter = (int (*)(struct GBA*, void*)) (void*) e.p;
	emitEnter(jit, &e);
	jit->exitRoutine = e.p;
	emitExit(jit, &e);
	jit->exitLinkRoutine = e.p;
	emitExitLink(jit, &e);
	jit->dispatchKey = e.p;
	emitDispatchKey(jit, &e);
	uint8_t* regionUpdate = emitRegionUpdate(&e);
	jit->dispatchArm = e.p;
	emitDispatchPc(jit, &e, false, regionUpdate);
	jit->dispatchThumb = e.p;
	emitDispatchPc(jit, &e, true, regionUpdate);
	jit->dispatchBx = e.p;
	emitDispatchBx(jit, &e);
	for (int type = 0; type < 4; ++type) {
		jit->shiftRoutines[type] = emitShiftRoutine(&e, type);
	}
	for (int kind = 0; kind < 8; ++kind) {
		jit->memRoutines[kind] = emitMemRoutine(jit, &e, kind, false);
		jit->romMemRoutines[kind] = emitMemRoutine(jit, &e, kind, true);
	}
	// w1 = 0 for MUL, 1 for UMULL and 2 for SMULL, with operands in jitScratch; the carry comes
	// back in w16.
	jit->multiplyCarryRoutine = e.p;
	emitPreserveCall(&e, jitMultiplyCarry);
	aRet(&e);
	// For ARMGBA_PERFMAP.
	static const char* const kinds[8] = { "load32", "load16", "load8", "loadS8", "loadS16", "store32", "store16", "store8" };
	jitNameCode(jit->spillRoutine, jit->reloadRoutine, "spill");
	jitNameCode(jit->reloadRoutine, (void*) jit->enter, "reload");
	jitNameCode((void*) jit->enter, jit->exitRoutine, "enter");
	jitNameCode(jit->exitRoutine, jit->exitLinkRoutine, "exit");
	jitNameCode(jit->exitLinkRoutine, jit->dispatchKey, "exitLink");
	jitNameCode(jit->dispatchKey, jit->dispatchArm, "dispatch");
	jitNameCode(jit->dispatchArm, jit->shiftRoutines[0], "dispatchPc");
	jitNameCode(jit->shiftRoutines[0], jit->memRoutines[0], "shift");
	for (int kind = 0; kind < 8; ++kind) {
		char name[32];
		snprintf(name, sizeof(name), "mem.%s", kinds[kind]);
		jitNameCode(jit->memRoutines[kind], jit->romMemRoutines[kind], name);
		snprintf(name, sizeof(name), "romMem.%s", kinds[kind]);
		jitNameCode(jit->romMemRoutines[kind], kind < 7 ? jit->memRoutines[kind + 1] : jit->multiplyCarryRoutine, name);
	}
	flushCode(jit->codeNext, e.p);
	jit->codeNext = e.p;
	return true;
}

// Block compiler.

// A constant, or a host register with a shift its consumer applies.
struct Operand2 {
	bool isImm;
	uint32_t imm;
	int reg;
	int shift;
	int amount;
	int carry;
};

struct Stub {
	uint8_t* site;
	uint32_t pc;
	int pending;
	// The instruction it comes before.
	int index;
	// In a careful copy: the instruction's maximum cycles, where its code continues after the
	// check, and whether events inside it can run after it.
	int cost;
	uint8_t* resume;
	bool deferrable;
};

struct Compiler {
	struct Jit* jit;
	struct GBA* gba;
	struct Block* block;
	struct Emitter e;
	struct Inst* insts;
	int count;
	const uint32_t* nextOps;
	bool thumb;
	int width;
	int region;
	int S;
	int N;
	int P;
	// For cartridge code with the prefetch buffer: its fetches' duty cycle, how far ahead it gets,
	// a nonsequential fetch's cycles, and its row of jitStopPenalty.
	bool prefetch;
	int D;
	int full;
	int Ncyc;
	uint32_t penaltyRow;
	bool careful;
	// Where a waiting loop's way back out of the block goes, or 0, and the loads it makes through
	// registers it doesn't change, which must be from RAM, DISPSTAT or VCOUNT for the loop to be
	// skipped.
	uint32_t idleExit;
	struct {
		uint8_t base;
		int32_t offset;
	} idleLoads[4];
	int idleLoadCount;
	uint8_t* fastDone[2];
	int fastDoneCount;

	// Where the guest flags are: host NZCV, w21, or both.
	bool inHost;
	bool inReg;

	uint8_t liveAfter[MAX_BLOCK_INSTS];
	bool checkpoint[MAX_BLOCK_INSTS + 1];
	int cur;
	// Cycles counted but not yet added to w20. In cartridge code with the prefetch buffer, they map
	// the time t and the buffer's base B in (max, +) algebra: t' = max(t + pending, B + pendB) and
	// B' = max(t + pendC, B + pendD), with NONE standing for no term.
	int pending;
	int pendB;
	int pendC;
	int pendD;
	struct Stub stubs[MAX_BLOCK_INSTS + 2];
	int stubCount;
	// In a careful copy, the checks after instructions whose events were put off.
	struct {
		uint8_t* site;
		int index;
	} deferred[MAX_BLOCK_INSTS];
	int deferredCount;
	// A conditional branch can need two; a rewritten word's forms count as instructions.
	struct Link links[3 * MAX_BLOCK_INSTS + 16];
	uint8_t* linkSites[3 * MAX_BLOCK_INSTS + 16];
	int linkCount;
	struct SlowAccess {
		const struct Inst* in;
		int index;
		uint8_t* sites[2];
		uint8_t* resume;
	} slow[MAX_BLOCK_INSTS];
	int slowCount;
	// Block transfers whose rarely run parts go with the cold code: the exact CPU's handling, and
	// each RAM region's words one at a time for a transfer that wraps around the region's end.
	struct SlowTransfer {
		const struct Inst* in;
		int index;
		uint8_t* sites[16];
		int siteCount;
		uint8_t* join;
		struct {
			uint8_t* site;
			int region;
			uint8_t* resume;
		} wraps[2];
		int wrapCount;
		// A transfer through the stack pointer keeps only IWRAM's copy inline, with a branch to
		// EWRAM's or the slow path.
		uint8_t* ewramSite;
		// Where a transfer outside work RAM leaves for the copies for palette RAM, VRAM and OAM.
		uint8_t* videoSite;
	} transfers[MAX_BLOCK_INSTS];
	int transferCount;
	struct SlowTransfer* transfer;
	// Tiny routines emitted in this block, forgotten again if its code doesn't fit.
	struct TinyRoutine* newTiny[8];
	int newTinyCount;
};

#include "../jit-compile.h"

// Flags.

// Makes w21 hold the guest flags.
static void needRegFlags(struct Compiler* c) {
	if (!c->inReg) {
		aMrsNzcv(&c->e, R_FLAGS);
		c->inReg = true;
	}
}

// Makes the host NZCV hold the guest flags.
static void needHostFlags(struct Compiler* c) {
	if (!c->inHost) {
		aMsrNzcv(&c->e, R_FLAGS);
		c->inHost = true;
	}
}

// Called before emitting anything that changes the host flags.
static void clobberFlags(struct Compiler* c) {
	needRegFlags(c);
	c->inHost = false;
}

// Called after a host instruction that set all four guest flags.
static void flagsSet(struct Compiler* c) {
	c->inHost = true;
	c->inReg = false;
}

// Called between instructions where anything can come in: puts the flags in w21 and leaves nothing
// pending.
static void canonical(struct Compiler* c) {
	c->inReg = true;
	c->inHost = false;
}

// A timing sequence that depends only on a few small numbers is emitted once, where it is first
// needed, and called from then on: on a core with a small cache, the code this saves is worth more
// than the call. Returns the slot for the key, or NULL when the table is full or a number doesn't
// fit.
static struct TinyRoutine* tinySlot(struct Compiler* c, int kind, int n0, int n1, int n2, int n3) {
	int numbers[4] = { n0, n1, n2, n3 };
	uint64_t key = (uint64_t) kind << 60;
	for (int i = 0; i < 4; ++i) {
		int n = numbers[i];
		if (n != NONE && (n < -0x3000 || n > 0x3000)) {
			return NULL;
		}
		key |= (uint64_t) (n == NONE ? 0x7FFF : n + 0x3000) << (i * 15);
	}
	for (unsigned probe = 0; probe < 8; ++probe) {
		struct TinyRoutine* slot = &c->jit->tiny[((key * 0x9E3779B97F4A7C15ull >> 54) + probe) & (TINY_ROUTINES - 1)];
		if (slot->code && slot->key == key) {
			return slot;
		}
		if (!slot->code) {
			if (c->newTinyCount == 8) {
				return NULL;
			}
			slot->key = key;
			return slot;
		}
	}
	return NULL;
}

// Starts a routine emitted inline: emits a jump over it, patched by tinyEnd.
static uint8_t* tinyBegin(struct Compiler* c, struct TinyRoutine* slot) {
	uint8_t* over = aB(&c->e, NULL);
	slot->code = c->e.p;
	c->newTiny[c->newTinyCount++] = slot;
	return over;
}

// Ends a tiny routine's body emitted in passing; the code around it jumps over it.
static void tinyEnd(struct Compiler* c, uint8_t* over) {
	aRet(&c->e);
	patchHere(&c->e, over);
}

// Uses w8, w16 and w17.
static void emitPendingTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	aLdr(e, 16, R_GBA, OFF(jitBase));
	if (c->pendC != NONE || c->pendD != 0) {
		if (c->pendC != NONE && c->pendD != NONE) {
			aAddImm(e, 17, R_CYC, c->pendC, 8);
			aAddImm(e, 8, 16, c->pendD, 8);
			aDp(e, A_SUBS, ZR, 8, 17, S_LSL, 0);
			aCsel(e, 8, 17, 8, C_LT);
		} else if (c->pendC != NONE) {
			aAddImm(e, 8, R_CYC, c->pendC, 8);
		} else {
			aAddImm(e, 8, 16, c->pendD, 8);
		}
		aStr(e, 8, R_GBA, OFF(jitBase));
	}
	if (c->pending) {
		aAddImm(e, R_CYC, R_CYC, c->pending, 17);
	}
	if (c->pendB != NONE) {
		aAddImm(e, 8, 16, c->pendB, 8);
		aDp(e, A_SUBS, ZR, R_CYC, 8, S_LSL, 0);
		aCsel(e, R_CYC, 8, R_CYC, C_LT);
	}
}

// Uses w8, w16, w17 and x30.
static void flushPending(struct Compiler* c) {
	struct Emitter* e = &c->e;
	if (pendingScalar(c)) {
		if (c->pending) {
			aAddImm(e, R_CYC, R_CYC, c->pending, 17);
		}
		resetPending(c);
		return;
	}
	clobberFlags(c);
	struct TinyRoutine* slot = tinySlot(c, 1, c->pending, c->pendB, c->pendC, c->pendD);
	if (!slot) {
		emitPendingTerms(c);
	} else {
		if (!slot->code) {
			uint8_t* over = tinyBegin(c, slot);
			emitPendingTerms(c);
			tinyEnd(c, over);
		}
		aBl(e, slot->code);
	}
	resetPending(c);
}

// Jumps when the ARM condition holds (or fails, if negate). Both paths have the flags in w21.
static uint8_t* emitCondJump(struct Compiler* c, int cond, bool negate) {
	struct Emitter* e = &c->e;
	if (negate) {
		cond ^= 1;
	}
	if (c->inHost) {
		needRegFlags(c);
	} else if (cond < 8) {
		static const int bit[4] = { 30, 29, 31, 28 };
		return (cond & 1) ? aTbz(e, R_FLAGS, bit[cond >> 1], NULL) : aTbnz(e, R_FLAGS, bit[cond >> 1], NULL);
	} else {
		needHostFlags(c);
	}
	// ARM and AArch64 condition codes are the same.
	return aBcond(e, cond, NULL);
}

// Values.

// Uses w16 as scratch.
static void storeField32(struct Compiler* c, uint32_t offset, uint32_t value) {
	storeFieldImm(&c->e, offset, value, 4);
}

// Returns the host register holding guest r, or puts r15's value in scratch and returns that.
static int guestReg(struct Compiler* c, int r, const struct Inst* in, int scratch) {
	if (r == 15) {
		aMovImm(&c->e, scratch, pcRead(c, in));
		return scratch;
	}
	return hostReg[r];
}

// Loads a guest register's value into a host register. r15 is a constant when compiling.
static void loadGuest(struct Compiler* c, int dst, int r, const struct Inst* in) {
	if (r == 15) {
		aMovImm(&c->e, dst, pcRead(c, in));
	} else {
		aMov(&c->e, dst, hostReg[r]);
	}
}

// Stores a host register's value into a guest register. r15 lives in the CPU state, not in a host
// register.
static void storeGuest(struct Compiler* c, int r, int src) {
	if (r == 15) {
		aStr(&c->e, src, R_GBA, REG(15));
	} else if (hostReg[r] != src) {
		aMov(&c->e, hostReg[r], src);
	}
}

// Stores a constant into a guest register.
static void storeGuestImm(struct Compiler* c, int r, uint32_t value) {
	if (r == 15) {
		storeField32(c, REG(15), value);
	} else {
		aMovImm(&c->e, hostReg[r], value);
	}
}

// Rotates right, as ARM's rotated constants do.
static uint32_t ror32(uint32_t value, int rotate) {
	rotate &= 31;
	return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

// Computes the shifter operand. Shifts by a register and rotations end up in w1; a computed carry
// lands in w2 as 0 or 1.
static struct Operand2 emitShifter(struct Compiler* c, const struct Inst* in, bool wantCarry) {
	struct Emitter* e = &c->e;
	const struct Operand* op = &in->op2;
	struct Operand2 out = { .carry = CARRY_SAME, .shift = S_LSL };
	if (op->isImm && in->patchable) {
		// The game rewrites this constant: rotate it from the instruction word at run time, passing
		// C through when it isn't rotated.
		clobberFlags(c);
		uint32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (in->address & (SIZE_EWRAM - 1));
		loadField(e, 1, word, 4);
		aUbfx(e, 3, 1, 8, 4);
		aDp(e, A_ADD, 3, 3, 3, S_LSL, 0);
		aAndBits(e, 1, 1, 0, 8);
		aDp(e, A_RORV, 1, 1, 3, S_LSL, 0);
		if (wantCarry) {
			aUbfx(e, 2, R_FLAGS, 29, 1);
			aLsrImm(e, 4, 1, 31);
			aDpImm(e, A_SUBS_IMM, ZR, 3, 0);
			aCsel(e, 2, 2, 4, C_EQ);
		}
		out.reg = 1;
		out.carry = CARRY_REG;
		return out;
	}
	if (op->isImm) {
		out.isImm = true;
		out.imm = op->imm;
		if (op->immRotated) {
			out.carry = (op->imm >> 31) ? CARRY_1 : CARRY_0;
		}
		return out;
	}
	if (op->byReg) {
		clobberFlags(c);
		if (op->rm == 15) {
			aMovImm(e, 1, pcRead(c, in) + 4);
		} else {
			aMov(e, 1, hostReg[op->rm]);
		}
		loadGuest(c, 2, op->rs, in);
		aBl(e, c->jit->shiftRoutines[op->type]);
		out.reg = 1;
		out.carry = CARRY_REG;
		return out;
	}
	if (in->patchable) {
		// The game rewrites this amount: shift LSL by the one in the instruction word, which passes
		// C through when it is 0.
		clobberFlags(c);
		uint32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (in->address & (SIZE_EWRAM - 1));
		loadField(e, 3, word, 4);
		aUbfx(e, 3, 3, 7, 5);
		int value = 1;
		if (op->rm == 15) {
			aMovImm(e, 1, pcRead(c, in));
		} else {
			value = hostReg[op->rm];
		}
		if (wantCarry) {
			aMovImm(e, 4, 32);
			aDp(e, A_SUB, 4, 4, 3, S_LSL, 0);
			aDp(e, A_LSRV, 4, value, 4, S_LSL, 0);
			aAndBits(e, 4, 4, 0, 1);
			aUbfx(e, 2, R_FLAGS, 29, 1);
			aDpImm(e, A_SUBS_IMM, ZR, 3, 0);
			aCsel(e, 2, 2, 4, C_EQ);
		}
		aDp(e, A_LSLV, 1, value, 3, S_LSL, 0);
		out.reg = 1;
		out.carry = CARRY_REG;
		return out;
	}
	int amount = op->amount;
	int type = op->type;
	if (op->rm == 15) {
		// A constant: fold the shift in when compiling.
		uint32_t v = pcRead(c, in);
		uint32_t result;
		int carry;
		switch (type) {
		case SHIFT_LSL:
			result = amount ? v << amount : v;
			carry = amount ? ((v >> (32 - amount)) & 1 ? CARRY_1 : CARRY_0) : CARRY_SAME;
			break;
		case SHIFT_LSR:
			result = amount ? v >> amount : 0;
			carry = ((amount ? v >> (amount - 1) : v >> 31) & 1) ? CARRY_1 : CARRY_0;
			break;
		case SHIFT_ASR:
			result = amount ? (uint32_t) ((int32_t) v >> amount) : (uint32_t) ((int32_t) v >> 31);
			carry = ((amount ? v >> (amount - 1) : v >> 31) & 1) ? CARRY_1 : CARRY_0;
			break;
		default:
			if (amount) {
				result = ror32(v, amount);
				carry = ((v >> (amount - 1)) & 1) ? CARRY_1 : CARRY_0;
			} else {
				// RRX needs the carry, so do it at run time.
				needRegFlags(c);
				aMovImm(e, 1, v);
				if (wantCarry) {
					aAndBits(e, 2, 1, 0, 1);
				}
				aUbfx(e, 3, R_FLAGS, 29, 1);
				emit32(e, 0x13800000 | (1 << 16) | (1 << 10) | (3 << 5) | 1);
				out.reg = 1;
				out.carry = CARRY_REG;
				return out;
			}
		}
		out.isImm = true;
		out.imm = result;
		out.carry = carry;
		return out;
	}
	int rm = hostReg[op->rm];
	out.reg = rm;
	if (type == SHIFT_LSL && !amount) {
		return out;
	}
	out.carry = CARRY_REG;
	switch (type) {
	case SHIFT_LSL:
		if (wantCarry) {
			aUbfx(e, 2, rm, 32 - amount, 1);
		}
		out.shift = S_LSL;
		out.amount = amount;
		break;
	case SHIFT_LSR:
		if (wantCarry) {
			aUbfx(e, 2, rm, amount ? amount - 1 : 31, 1);
		}
		if (amount) {
			out.shift = S_LSR;
			out.amount = amount;
		} else {
			out.isImm = true;
			out.imm = 0;
		}
		break;
	case SHIFT_ASR:
		if (wantCarry) {
			aUbfx(e, 2, rm, amount ? amount - 1 : 31, 1);
		}
		// ASR #32 gives the same value as ASR #31.
		out.shift = S_ASR;
		out.amount = amount ? amount : 31;
		break;
	case SHIFT_ROR:
		if (amount) {
			if (wantCarry) {
				aUbfx(e, 2, rm, amount - 1, 1);
			}
			aRorImm(e, 1, rm, amount);
		} else {
			// RRX: extr w1, wC, wm, #1
			needRegFlags(c);
			aUbfx(e, 3, R_FLAGS, 29, 1);
			if (wantCarry) {
				aAndBits(e, 2, rm, 0, 1);
			}
			emit32(e, 0x13800000 | (rm << 16) | (1 << 10) | (3 << 5) | 1);
		}
		out.reg = 1;
		break;
	}
	return out;
}

// Returns the operand as a plain register, using scratch if it needs work.
static int operandReg(struct Compiler* c, const struct Operand2* op, int scratch) {
	if (op->isImm) {
		aMovImm(&c->e, scratch, op->imm);
		return scratch;
	}
	if (op->amount) {
		aDp(&c->e, A_ORR, scratch, ZR, op->reg, op->shift, op->amount);
		return scratch;
	}
	return op->reg;
}

// Sets N and Z from a result the host flags were just set from, C from the shifter (or kept), and
// keeps V. The old flags must be in w21 when C or V is live.
static void logicalFlags(struct Compiler* c, int carry, uint8_t live) {
	struct Emitter* e = &c->e;
	if (!(live & (F_C | F_V))) {
		flagsSet(c);
		return;
	}
	// N and Z from the host, C and V (or just V) from w21.
	aMrsNzcv(e, 6);
	aBfxil(e, 6, R_FLAGS, 0, carry == CARRY_SAME ? 30 : 29);
	switch (carry) {
	case CARRY_SAME:
	case CARRY_0:
		aMov(e, R_FLAGS, 6);
		break;
	case CARRY_1:
		aLogicImm(e, A_ORR_IMM, R_FLAGS, 6, 0x20000000);
		break;
	case CARRY_REG:
		aDp(e, A_ORR, R_FLAGS, 6, 2, S_LSL, 29);
		break;
	}
	c->inReg = true;
	c->inHost = false;
}

// Leaves for the exact CPU unless the instructions up to the next check end before the next event.
static void emitCheck(struct Compiler* c, int index, int ahead) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	if (!pendingScalar(c)) {
		flushPending(c);
	}
	int total = c->pending + ahead;
	if (total < 0x1000) {
		aDpImm(e, A_ADDS_IMM, ZR, R_CYC, total);
	} else {
		aAddImm(e, 0, R_CYC, total, 17);
		aDpImm(e, A_SUBS_IMM, ZR, 0, 0);
	}
	struct Stub* stub = &c->stubs[c->stubCount++];
	stub->site = aBcond(e, C_GE, NULL);
	stub->pc = c->insts[0].address + index * c->width;
	stub->pending = c->pending;
	stub->index = index;
}

// Calls a runtime helper with the guest state in memory and arguments in w1 and w2; the helper's
// result stays in w0.
static void callHelper(struct Compiler* c, const struct Inst* in, const void* fn) {
	struct Emitter* e = &c->e;
	int index = (in->address - c->insts[0].address) / c->width;
	storeField32(c, REG(15), in->address + 2 * c->width);
	storeField32(c, OFF(cpu.prefetch), opcodeAt(c, index + 1));
	storeField32(c, OFF(cpu.prefetch) + 4, opcodeAt(c, index + 2));
	aMovX(e, 0, R_GBA);
	callAbs(e, fn);
	aBl(e, c->jit->reloadRoutine);
	canonical(c);
}

// Before calling C: brings the flags and cycles up to date and stores the guest registers.
static void spill(struct Compiler* c) {
	clobberFlags(c);
	flushPending(c);
	aBl(&c->e, c->jit->spillRoutine);
}

// Jumps to the block at a guest address. It is emitted as a jump to a stub that looks the block up,
// and is patched to go straight there once the block exists.
static void emitLinkTo(struct Compiler* c, uint32_t pc, bool thumb) {
	needRegFlags(c);
	struct Link* link = &c->links[c->linkCount];
	memset(link, 0, sizeof(*link));
	c->linkSites[c->linkCount++] = aB(&c->e, NULL);
	link->targetKey = blockKey(pc, thumb);
	link->from = c->block;
}

// Where the prefetch buffer reads next, for code that leaves the cartridge or C code that refills;
// uses w16.
static void storeHead(struct Compiler* c, uint32_t head) {
	if (c->prefetch) {
		storeField32(c, OFF(prefetcher.head), head);
	}
}

// Data or a refill from the cartridge stops the prefetch buffer: one cycle more if a read is about
// to finish.
static void emitStopTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	aLdr(e, 6, R_GBA, OFF(jitBase));
	aDp(e, A_SUB, 6, R_CYC, 6, S_LSL, 0);
	aDpImm(e, A_SUBS_IMM, ZR, 6, 127);
	uint8_t* far = aBcond(e, C_HI, NULL);
	aAddImmX(e, 7, R_GBA, c->penaltyRow);
	aLdst(e, L_LDRB, 6, 7, 6, L_UXTW);
	aDp(e, A_ADD, R_CYC, R_CYC, 6, S_LSL, 0);
	patchHere(e, far);
}

// A data access from the cartridge stops the prefetch buffer; returns what that costs the next
// fetch.
static void emitStop(struct Compiler* c) {
	clobberFlags(c);
	flushPending(c);
	struct TinyRoutine* slot = c->penaltyRow < 0x100000 ? tinySlot(c, 3, c->penaltyRow & 0xFFF, c->penaltyRow >> 12, 0, 0) : NULL;
	if (!slot) {
		emitStopTerms(c);
		return;
	}
	if (!slot->code) {
		uint8_t* over = tinyBegin(c, slot);
		emitStopTerms(c);
		tinyEnd(c, over);
	}
	aBl(&c->e, slot->code);
}

// Called after an access that may have stopped the buffer: the next fetch restarts it,
// nonsequential. Compiled code treats that as a fetch from the buffer whose read finishes then;
// until the fetch, the buffer stays stopped for the exact CPU.
static void emitMissTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	loadField(e, 6, OFF(cpu.prefetching), 1);
	uint8_t* running = aCbnz(e, 6, NULL);
	storeField(e, ZR, OFF(cpu.fetchSeq), 1);
	aAddImm(e, 6, R_CYC, c->Ncyc - c->D, 7);
	aStr(e, 6, R_GBA, OFF(jitBase));
	patchHere(e, running);
}

// Called after something that took a number of cycles not known when compiling: works out whether
// the prefetch buffer has the next instruction yet, and the wait if it doesn't.
static void emitMissCheck(struct Compiler* c) {
	if (!c->prefetch) {
		return;
	}
	clobberFlags(c);
	flushPending(c);
	struct TinyRoutine* slot = tinySlot(c, 2, c->Ncyc - c->D, 0, 0, 0);
	if (!slot) {
		emitMissTerms(c);
		return;
	}
	if (!slot->code) {
		uint8_t* over = tinyBegin(c, slot);
		emitMissTerms(c);
		tinyEnd(c, over);
	}
	aBl(&c->e, slot->code);
}

// The fetch after one that may have stopped the buffer restarts it.
static void emitRestarted(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_Static_assert(OFF(cpu.prefetching) == OFF(cpu.fetchSeq) + 1 && !(OFF(cpu.fetchSeq) & 1), "fetchSeq and prefetching are one halfword");
	aMovImm(e, 16, 0x0101);
	storeField(e, 16, OFF(cpu.fetchSeq), 2);
}

// In the BIOS, the word that will be on the bus after a jump out of it, which is what reads of the
// BIOS from outside return.
static void setBiosPrefetchNext(struct Compiler* c, const struct Inst* in) {
	if (c->region == REGION_BIOS) {
		storeField32(c, OFF(biosPrefetchNext), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
	}
}

// On a waiting loop's way back to its start.
static void emitIdleMark(struct Compiler* c, uint32_t to) {
	if (c->idleExit && c->cur == c->count - 1 && to == c->idleExit) {
		storeField32(c, OFF(idleMark), blockKey(c->insts[0].address, c->thumb));
	}
}

// Emits the cost and bookkeeping of a direct PC write, then a jump to the target block.
static void emitDirectBranch(struct Compiler* c, const struct Inst* in, uint32_t target) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	emitIdleMark(c, target);
	int region = target >> 24;
	uint32_t head = in->address + 3 * c->width;
	if (!inlineRefill(c, target)) {
		setBiosPrefetchNext(c, in);
		storeHead(c, head);
		flushPending(c);
		storeField32(c, OFF(exitPc), blockKey(target & ~1u, c->thumb));
		aBl(e, c->jit->spillRoutine);
		aMovX(e, 0, R_GBA);
		aMovImm(e, 1, target);
		callAbs(e, jitHelperReload);
		aBl(e, c->jit->reloadRoutine);
		loadField(e, 0, OFF(exitPc), 4);
		aB(e, c->jit->dispatchKey);
		return;
	}
	if (c->prefetch && inCartridge(region)) {
		// Through the buffer: from it if the target is where it reads next; otherwise it is stopped
		// and restarted there.
		if ((target & ~1u) == head) {
			pendFetch(c);
			pendFetch(c);
		} else {
			emitStop(c);
			pendAdd(c, c->Ncyc);
			pendSetBase(c, 0);
			pendFetch(c);
		}
	} else {
		// When leaving the cartridge, the buffer keeps reading on by itself.
		storeHead(c, head);
		pendAdd(c, directBranchCost(c, target));
	}
	flushPending(c);
	if (region != c->region) {
		if (c->region == REGION_BIOS) {
			storeField32(c, OFF(biosPrefetch), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
		}
		storeField32(c, OFF(activeRegion), region);
		storeField32(c, OFF(fetchRegion), region);
	}
	emitLinkTo(c, target & ~1u, c->thumb);
}

// w0 = target address; the instruction's cost is already pending.
static void emitIndirect(struct Compiler* c, const struct Inst* in, uint8_t* dispatch) {
	needRegFlags(c);
	storeHead(c, in->address + 3 * c->width);
	flushPending(c);
	setBiosPrefetchNext(c, in);
	aB(&c->e, dispatch);
}

// dst = rn op operand for the add/subtract family; rn may be ZR.
static void emitAddSub(struct Compiler* c, bool sub, bool flags, int dst, int rn, const struct Operand2* op) {
	struct Emitter* e = &c->e;
	if (op->isImm) {
		uint32_t imm = op->imm;
		if (rn != ZR && (imm < 0x1000 || (!(imm & 0xFFF) && imm < 0x1000000))) {
			aDpImm(e, (sub ? A_SUB_IMM : A_ADD_IMM) | (flags ? 0x20000000 : 0), dst, rn, imm);
			return;
		}
		uint32_t neg = -imm;
		if (rn != ZR && !flags && (neg < 0x1000 || (!(neg & 0xFFF) && neg < 0x1000000))) {
			aDpImm(e, sub ? A_ADD_IMM : A_SUB_IMM, dst, rn, neg);
			return;
		}
		aMovImm(e, 4, imm);
		aDp(e, (sub ? A_SUB : A_ADD) | (flags ? 0x20000000 : 0), dst, rn, 4, S_LSL, 0);
		return;
	}
	aDp(e, (sub ? A_SUB : A_ADD) | (flags ? 0x20000000 : 0), dst, rn, op->reg, op->shift, op->amount);
}

// dst = rn op operand for AND, ORR, EOR and BIC (as AND NOT).
static void emitLogic(struct Compiler* c, int alu, bool flags, int dst, int rn, const struct Operand2* op) {
	struct Emitter* e = &c->e;
	static const uint32_t immOps[] = { A_AND_IMM, A_ORR_IMM, A_EOR_IMM, A_ANDS_IMM };
	static const uint32_t regOps[] = { A_AND, A_ORR, A_EOR, A_ANDS };
	// alu: 0 AND, 1 ORR, 2 EOR; flags only with AND.
	int k = flags ? 3 : alu;
	if (op->isImm) {
		if (aLogicImm(e, immOps[k], dst, rn, op->imm)) {
			return;
		}
		aMovImm(e, 4, op->imm);
		aDp(e, regOps[k], dst, rn, 4, S_LSL, 0);
		return;
	}
	aDp(e, regOps[k], dst, rn, op->reg, op->shift, op->amount);
}

// Compiles a data-processing instruction. Flags are computed only if something later reads them,
// and the host's own flags are used where they come out the same.
static void emitAlu(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	int op = in->op;
	uint8_t live = c->liveAfter[c->cur];
	bool logical = isLogical(op);
	// A write to r15 that sets flags restores the CPSR; jitSystem handles it.
	bool writesPc = aluWritesPc(in);
	// Flags nothing reads before they are written again are left alone.
	bool flags = in->setFlags && (aluWrites(in) & live);
	bool wantCarry = flags && logical && (live & F_C);
	if (flags && logical && (live & (F_C | F_V))) {
		// The flags this instruction keeps.
		needRegFlags(c);
	}
	struct Operand2 op2 = emitShifter(c, in, wantCarry);
	int carry = op2.carry;
	if (carry == CARRY_REG && !wantCarry) {
		carry = CARRY_SAME;
	}

	int dst = writesPc ? 0 : isTest(op) ? ZR : hostReg[in->rd];
	int rn = 0;
	if (op != ALU_MOV && op != ALU_MVN) {
		if (in->rn == 15) {
			aMovImm(e, 3, pcRead(c, in) + (in->op2.byReg ? 4 : 0));
			rn = 3;
		} else {
			rn = hostReg[in->rn];
		}
	}
	// Logical results set N and Z with a test, unless the operation sets them itself.
	bool tested = false;
	switch (op) {
	case ALU_MOV:
	case ALU_MVN:
		if (op2.isImm) {
			aMovImm(e, dst, op == ALU_MVN ? ~op2.imm : op2.imm);
		} else {
			aDp(e, op == ALU_MVN ? A_ORN : A_ORR, dst, ZR, op2.reg, op2.shift, op2.amount);
		}
		break;
	case ALU_AND:
	case ALU_TST:
		if (op == ALU_TST && !flags) {
			break;
		}
		emitLogic(c, 0, flags, dst, rn, &op2);
		tested = flags;
		break;
	case ALU_BIC:
		if (op2.isImm) {
			struct Operand2 inverted = op2;
			inverted.imm = ~op2.imm;
			emitLogic(c, 0, flags, dst, rn, &inverted);
		} else {
			aDp(e, flags ? A_BICS : A_BIC, dst, rn, op2.reg, op2.shift, op2.amount);
		}
		tested = flags;
		break;
	case ALU_ORR:
		emitLogic(c, 1, false, dst, rn, &op2);
		break;
	case ALU_EOR:
		emitLogic(c, 2, false, dst, rn, &op2);
		break;
	case ALU_TEQ:
		if (!flags) {
			break;
		}
		dst = 0;
		emitLogic(c, 2, false, dst, rn, &op2);
		break;
	case ALU_ADD:
	case ALU_CMN:
		if (op == ALU_CMN && !flags) {
			break;
		}
		emitAddSub(c, false, flags, dst, rn, &op2);
		break;
	case ALU_SUB:
	case ALU_CMP:
		if (op == ALU_CMP && !flags) {
			break;
		}
		emitAddSub(c, true, flags, dst, rn, &op2);
		break;
	case ALU_RSB: {
		int value = operandReg(c, &op2, 4);
		aDp(e, flags ? A_SUBS : A_SUB, dst, value, rn, S_LSL, 0);
		break;
	}
	case ALU_ADC:
	case ALU_SBC:
	case ALU_RSC: {
		int value = operandReg(c, &op2, 4);
		needHostFlags(c);
		uint32_t opcode = op == ALU_ADC ? (flags ? A_ADCS : A_ADC) : (flags ? A_SBCS : A_SBC);
		if (op == ALU_RSC) {
			aDp(e, opcode, dst, value, rn, S_LSL, 0);
		} else {
			aDp(e, opcode, dst, rn, value, S_LSL, 0);
		}
		break;
	}
	}

	if (flags && logical && (op == ALU_MOV || op == ALU_MVN) && op2.isImm && (live & (F_C | F_V))) {
		// The result is a constant, and so are N and Z.
		uint32_t value = op == ALU_MVN ? ~op2.imm : op2.imm;
		uint32_t bits = (value & 0x80000000) | (value ? 0 : 0x40000000) | (carry == CARRY_1 ? 0x20000000 : 0);
		aLogicImm(e, A_AND_IMM, R_FLAGS, R_FLAGS, carry == CARRY_SAME ? 0x30000000 : 0x10000000);
		if (bits && !aLogicImm(e, A_ORR_IMM, R_FLAGS, R_FLAGS, bits)) {
			aMovImm(e, 6, bits);
			aDp(e, A_ORR, R_FLAGS, R_FLAGS, 6, S_LSL, 0);
		}
		if (carry == CARRY_REG) {
			aDp(e, A_ORR, R_FLAGS, R_FLAGS, 2, S_LSL, 29);
		}
		c->inReg = true;
		c->inHost = false;
	} else if (flags) {
		if (logical) {
			if (!tested) {
				aDp(e, A_ANDS, ZR, dst, dst, S_LSL, 0);
			}
			logicalFlags(c, carry, live);
		} else {
			flagsSet(c);
		}
	}

	pendAdd(c, aluExtra(c, in));
	if (writesPc) {
		// The register shift's internal cycle comes before the refill, not a nonsequential fetch.
		if (!in->op2.isImm && in->op2.byReg) {
			pendAdd(c, -nonseq(c));
		}
		emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
	}
}

// The multiplier's internal cycles, which stop early once the rest of the operand is all zeros (or
// all ones, when signed).
static void emitMulWait(struct Compiler* c, const struct Inst* in, int waitReg, bool sign, int extra) {
	struct Emitter* e = &c->e;
	// After the fetch, which the prefetch buffer can make wait.
	flushPending(c);
	int r = guestReg(c, waitReg, in, 0);
	if (sign) {
		aDp(e, A_EOR, 0, r, r, S_ASR, 31);
		r = 0;
	}
	aLogicImm(e, A_ORR_IMM, 0, r, 1);
	aClz(e, 0, 0);
	aLogicImm(e, A_EOR_IMM, 0, 0, 31);
	aLsrImm(e, 0, 0, 3);
	aDpImm(e, A_ADD_IMM, 0, 0, 1 + extra);
	aDp(e, A_ADD, R_CYC, R_CYC, 0, S_LSL, 0);
}

// The C flag the multiplier leaves, from operands put in jitScratch first: 0 for MUL, 1 for UMULL
// and 2 for SMULL; the carry comes back in w2.
static void emitMultiplyCarry(struct Compiler* c, int kind) {
	aMovImm(&c->e, 1, kind);
	aBl(&c->e, c->jit->multiplyCarryRoutine);
	aMov(&c->e, 2, 16);
}

// Puts the operands where the C helper that works out a multiply's carry looks for them.
static void storeScratch(struct Compiler* c, int slot, int r, const struct Inst* in) {
	int reg = guestReg(c, r, in, 0);
	aStr(&c->e, reg, R_GBA, OFF(jitScratch) + 4 * slot);
}

// Compiles MUL and MLA. The cycle count depends on the multiplier's value, so it is worked out at
// run time.
static void emitMul(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	pendAdd(c, nonseq(c));
	if (in->rd == 15 || (in->accumulate && in->rn == 15)) {
		return;
	}
	// Thumb's MUL multiplies by the destination's old value; ARM's by Rs.
	int multiplier = c->thumb ? in->rd : in->rs;
	int multiplicand = c->thumb ? in->rs : in->rm;
	emitMulWait(c, in, multiplier, true, in->accumulate ? 1 : 0);
	// The multiplier's carry is worked out only where something reads it before it is set again.
	uint8_t live = in->setFlags ? c->liveAfter[c->cur] : 0;
	bool carry = live & F_C;
	if (carry) {
		storeScratch(c, 0, multiplicand, in);
		storeScratch(c, 1, multiplier, in);
		if (in->accumulate) {
			storeScratch(c, 2, in->rn, in);
		} else {
			aStr(e, ZR, R_GBA, OFF(jitScratch) + 8);
		}
	}
	int rm = guestReg(c, in->rm, in, 0);
	int rs = guestReg(c, in->rs, in, 1);
	int dst = hostReg[in->rd];
	aMadd(e, dst, rm, rs, in->accumulate ? hostReg[in->rn] : ZR);
	if (in->setFlags) {
		if (carry) {
			emitMultiplyCarry(c, 0);
		}
		aDp(e, A_ANDS, ZR, dst, dst, S_LSL, 0);
		logicalFlags(c, carry ? CARRY_REG : CARRY_SAME, live);
	}
}

// Compiles the 64-bit multiplies.
static void emitMulLong(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	pendAdd(c, nonseq(c));
	if (in->rd == 15 || in->rdHi == 15) {
		return;
	}
	emitMulWait(c, in, in->rs, in->signedMul, in->accumulate ? 2 : 1);
	uint8_t live = in->setFlags ? c->liveAfter[c->cur] : 0;
	bool carry = live & F_C;
	if (carry) {
		storeScratch(c, 0, in->rm, in);
		storeScratch(c, 1, in->rs, in);
		if (in->accumulate) {
			storeScratch(c, 2, in->rd, in);
			storeScratch(c, 3, in->rdHi, in);
		} else {
			aStr(e, ZR, R_GBA, OFF(jitScratch) + 8);
			aStr(e, ZR, R_GBA, OFF(jitScratch) + 12);
		}
	}
	int rm = guestReg(c, in->rm, in, 0);
	int rs = guestReg(c, in->rs, in, 1);
	if (in->signedMul) {
		aSmull(e, 0, rm, rs);
	} else {
		aUmull(e, 0, rm, rs);
	}
	if (in->accumulate) {
		// Guest registers keep their upper halves clear.
		aDp(e, A_ADD | W64, 0, 0, hostReg[in->rd], S_LSL, 0);
		aAddHigh(e, 0, 0, hostReg[in->rdHi]);
	}
	aMov(e, hostReg[in->rd], 0);
	aLsrImmX(e, hostReg[in->rdHi], 0, 32);
	if (in->setFlags) {
		// N from bit 63, Z from all 64 bits, C from the multiplier; V is kept.
		if (carry) {
			emitMultiplyCarry(c, in->signedMul ? 2 : 1);
		}
		aDp(e, A_ANDS | W64, ZR, 0, 0, S_LSL, 0);
		logicalFlags(c, carry ? CARRY_REG : CARRY_SAME, live);
	}
}

// A word access through the stack pointer, which points into IWRAM: inline when it lands there
// aligned and misses compiled code, otherwise through the memory routine out of line. w0 holds the
// address and w1 a value to store. The check before it covers the inline path's cycles.
static void emitStackAccess(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	struct SlowAccess* slow = &c->slow[c->slowCount++];
	slow->in = in;
	slow->index = c->cur;
	slow->sites[1] = NULL;
	// (address & 0xFF000003) == 0x03000000
	aLogicImm(e, A_AND_IMM, 3, 0, 0xFF000003);
	aLogicImm(e, A_EOR_IMM, 3, 3, REGION_IWRAM << 24);
	slow->sites[0] = aCbnz(e, 3, NULL);
	aAndBits(e, 3, 0, 2, 13);
	aAddImmX(e, 4, R_GBA, OFF(iwram));
	if (!in->load) {
		aAddImmX(e, 5, R_GBA, OFF(iwramCode));
		aLsrImm(e, 6, 3, 2);
		aLdst(e, L_LDRB, 6, 5, 6, L_UXTW);
		slow->sites[1] = aCbnz(e, 6, NULL);
		aLdst(e, L_STR, 1, 4, 3, L_UXTW);
	} else {
		aLdst(e, L_LDR, hostReg[in->rd], 4, 3, L_UXTW);
	}
	aDpImm(e, A_ADD_IMM, R_CYC, R_CYC, in->load ? 2 : 1);
	slow->resume = e->p;
	pendAdd(c, nonseq(c));
}

// Compiles a load or store: works out the address, calls the shared routine and writes the base
// back. A load from the cartridge at a fixed address becomes a constant.
static void emitMem(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	uint32_t literal;
	int wait;
	if (jitRomLiteral(c->gba, in, &literal, &wait)) {
		storeGuestImm(c, in->rd, literal);
		if (c->prefetch) {
			emitStop(c);
			storeField(e, ZR, OFF(cpu.fetchSeq), 2);
			pendAdd(c, wait);
			pendSetBase(c, c->Ncyc - c->D);
		} else {
			pendAdd(c, nonseq(c) + wait);
		}
		return;
	}
	clobberFlags(c);
	// The fetch comes first, so an I/O access sees the time it happens at.
	flushPending(c);

	// The address goes in w0, the written-back base in w4, and a stored value in w1.
	struct Operand2 off = { .isImm = true, .shift = S_LSL };
	bool patched = false;
	if (!in->op2.isImm) {
		off = emitShifter(c, in, false);
	} else if (in->patchable) {
		// The game rewrites this offset and its direction, so read them from the instruction word.
		uint32_t word = in->address & ~3u;
		uint32_t base = (word >> 24) == REGION_IWRAM ? OFF(iwram) + (word & (SIZE_IWRAM - 1)) : OFF(ewram) + (word & (SIZE_EWRAM - 1));
		loadField(e, 2, base, 4);
		aUbfx(e, 3, 2, 23, 1);
		aDpImm(e, A_SUB_IMM, 3, 3, 1);
		aAndBits(e, 2, 2, 0, 12);
		aDp(e, A_EOR, 2, 2, 3, S_LSL, 0);
		aDp(e, A_SUB, 2, 2, 3, S_LSL, 0);
		off.isImm = false;
		off.reg = 2;
		patched = true;
	} else {
		off.imm = in->op2.imm;
	}
	bool subtract = !in->up && !patched;
	int base;
	if (in->rn == 15) {
		aMovImm(e, 5, c->thumb ? (in->address + 4) & ~3u : in->address + 8);
		base = 5;
	} else {
		base = hostReg[in->rn];
	}
	// base + offset into dst
	#define OFFSET_ADD(dst) do { \
		if (off.isImm) { \
			if (off.imm) { \
				aAddImm(e, dst, base, subtract ? -(int32_t) off.imm : (int32_t) off.imm, 17); \
			} else { \
				aMov(e, dst, base); \
			} \
		} else { \
			aDp(e, subtract ? A_SUB : A_ADD, dst, base, off.reg, off.shift, off.amount); \
		} \
	} while (0)
	if (in->pre) {
		OFFSET_ADD(0);
	} else {
		aMov(e, 0, base);
	}
	// The stored register is read before the base is written back.
	bool writeback = in->writeback && in->rn != 15;
	if (writeback && !in->pre) {
		OFFSET_ADD(4);
	}
	#undef OFFSET_ADD
	if (!in->load) {
		if (in->rd == 15) {
			aMovImm(e, 1, in->address + 12);
		} else {
			aMov(e, 1, hostReg[in->rd]);
		}
	}
	if (writeback) {
		storeGuest(c, in->rn, in->pre ? 0 : 4);
	}
	if (stackAccess(c, in)) {
		emitStackAccess(c, in);
		return;
	}
	if (!in->load && ramCode(c)) {
		storeField32(c, REG(15), in->address + 2 * c->width);
	}
	aBl(e, memRoutine(c, memKind(in)));
	jitRecordSite(c->block, e->p - c->block->entry, in->address, opcodeAt(c, c->cur + 1), opcodeAt(c, c->cur + 2));
	if (in->load) {
		if (in->rd == 15) {
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
			return;
		}
		storeGuest(c, in->rd, 0);
	}
	pendAdd(c, nonseq(c));
	emitMissCheck(c);
}

// A transfer that wraps around its RAM region's end, done word by word; w0 = first address. A store
// that would hit compiled code goes the slow way: to slowStart when that is known, or through slow.
static void emitWrappedTransfer(struct Compiler* c, const struct Inst* in, int region, uint8_t** slow, int* slowCount, uint8_t* slowStart) {
	struct Emitter* e = &c->e;
	bool iwram = region == REGION_IWRAM;
	int bits = iwram ? 15 : 18;
	int count = __builtin_popcount(in->list);
	int regs[16];
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (in->list & (1 << r)) {
			regs[k++] = r == 15 ? 1 : hostReg[r];
		}
	}
	aAddImmX(e, 5, R_GBA, iwram ? OFF(iwram) : OFF(ewram));
	if (!in->load) {
		aAddImmX(e, 6, R_GBA, iwram ? OFF(iwramCode) : OFF(ewramCode));
		aMov(e, 8, ZR);
		for (int k = 0; k < count; ++k) {
			if (k) {
				aDpImm(e, A_ADD_IMM, 3, 0, 4 * k);
				aUbfx(e, 3, 3, 2, bits - 2);
			} else {
				aUbfx(e, 3, 0, 2, bits - 2);
			}
			aLdst(e, L_LDRB, 3, 6, 3, L_UXTW);
			aDp(e, A_ORR, 8, 8, 3, S_LSL, 0);
		}
		if (slowStart) {
			aCbnz(e, 8, slowStart);
		} else {
			slow[(*slowCount)++] = aCbnz(e, 8, NULL);
		}
	}
	for (k = 0; k < count; ++k) {
		if (k) {
			aDpImm(e, A_ADD_IMM, 3, 0, 4 * k);
			aAndBits(e, 3, 3, 2, bits - 2);
		} else {
			aAndBits(e, 3, 0, 2, bits - 2);
		}
		aLdst(e, in->load ? L_LDR : L_STR, regs[k], 5, 3, L_UXTW);
	}
}

// One RAM region's copy of the inline transfer; w0 = first address. Jumps to slow when a store
// would hit compiled code, or an event would fall inside it. A loaded PC ends up in w1.
static void emitRamTransfer(struct Compiler* c, const struct Inst* in, int region, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	bool iwram = region == REGION_IWRAM;
	int bits = iwram ? 15 : 18;
	int count = __builtin_popcount(in->list);
	// The same wait for every word, nonsequential or not, and a load's internal cycle.
	struct GBA* gba = c->jit->gba;
	int cycles = count * (1 + gba->waitS32[region]) + (in->load ? 1 : 0);
	if (c->transfer && cycles < 0x1000) {
		// The slow path is with the cold code, further off than a bit test can reach.
		aDpImm(e, A_ADDS_IMM, 3, R_CYC, cycles);
		slow[(*slowCount)++] = aBcond(e, C_PL, NULL);
	} else {
		aAddImm(e, 3, R_CYC, cycles, 6);
		slow[(*slowCount)++] = aTbz(e, 3, 31, NULL);
	}
	aAddImmX(e, 5, R_GBA, iwram ? OFF(iwram) : OFF(ewram));
	// Words that don't wrap around the region: one base, accesses in pairs, and one check of the
	// code map for stores.
	int regs[16];
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (in->list & (1 << r)) {
			regs[k++] = r == 15 ? 1 : hostReg[r];
		}
	}
	aAndBits(e, 3, 0, 2, bits - 2);
	aDpImm(e, A_ADD_IMM, 6, 3, 4 * count);
	aDpImm(e, A_SUBS_IMM, ZR, 6, 1 << bits);
	uint8_t* wraps = aBcond(e, C_HI, NULL);
	if (!in->load) {
		aAddImmX(e, 6, R_GBA, iwram ? OFF(iwramCode) : OFF(ewramCode));
		aDp(e, A_ADD | W64, 6, 6, 3, S_LSR, 2);
		// Eight words at a time. Past the map's end, the byte read belongs to the next field of the
		// struct, and at worst sends the transfer the slow way.
		for (int part = 0; part < count; part += 8) {
			int bytes = count - part < 8 ? count - part : 8;
			aLdrX(e, 7, 6, part);
			if (bytes < 8) {
				aLslImmX(e, 7, 7, 64 - 8 * bytes);
			}
			slow[(*slowCount)++] = aCbnzX(e, 7, NULL);
		}
	}
	aDp(e, A_ADD | W64, 5, 5, 3, S_LSL, 0);
	for (k = 0; k + 1 < count; k += 2) {
		if (in->load) {
			aLdpW(e, regs[k], regs[k + 1], 5, 4 * k);
		} else {
			aStpW(e, regs[k], regs[k + 1], 5, 4 * k);
		}
	}
	if (k < count) {
		if (in->load) {
			aLdr(e, regs[k], 5, 4 * k);
		} else {
			aStr(e, regs[k], 5, 4 * k);
		}
	}
	uint8_t* contiguous = NULL;
	if (c->transfer && cycles < 0x1000) {
		struct SlowTransfer* t = c->transfer;
		t->wraps[t->wrapCount].site = wraps;
		t->wraps[t->wrapCount].region = region;
		t->wraps[t->wrapCount++].resume = e->p;
	} else {
		contiguous = aB(e, NULL);
		patchHere(e, wraps);
		emitWrappedTransfer(c, in, region, slow, slowCount, NULL);
		patchHere(e, contiguous);
	}
	if (in->writeback && (!in->load || !(in->list & (1 << in->rn)))) {
		int32_t start = in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
		aAddImm(e, hostReg[in->rn], 0, (in->up ? 4 * count : -4 * count) - start, 17);
	}
	aAddImm(e, R_CYC, R_CYC, cycles + (loadsPc(in) ? 0 : nonseq(c)), 17);
}

// Pushes, pops and copies within IWRAM or EWRAM run inline; anything else goes to the exact CPU.
static void emitFastTransfer(struct Compiler* c, const struct Inst* in, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	int count = __builtin_popcount(in->list);
	int32_t start = in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
	clobberFlags(c);
	if (start) {
		aAddImm(e, 0, hostReg[in->rn], start, 17);
	} else {
		aMov(e, 0, hostReg[in->rn]);
	}
	aAndBits(e, 3, 0, 0, 2);
	slow[(*slowCount)++] = aCbnz(e, 3, NULL);
	aDpImm(e, A_ADD_IMM, 3, 0, 4 * count - 4);
	aDp(e, A_EOR, 3, 3, 0, S_LSL, 0);
	aLsrImm(e, 3, 3, 24);
	slow[(*slowCount)++] = aCbnz(e, 3, NULL);
	aLsrImm(e, 3, 0, 24);
	bool toPc = in->load && (in->list & 0x8000);
	if (c->transfer && in->rn == 13) {
		// The stack is in IWRAM in all but a few games.
		aDpImm(e, A_SUBS_IMM, ZR, 3, REGION_IWRAM);
		c->transfer->ewramSite = aBcond(e, C_NE, NULL);
		emitRamTransfer(c, in, REGION_IWRAM, slow, slowCount);
		if (toPc) {
			aMov(e, 0, 1);
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
		}
		return;
	}
	aDpImm(e, A_SUBS_IMM, ZR, 3, REGION_EWRAM);
	uint8_t* ewram = aBcond(e, C_EQ, NULL);
	aDpImm(e, A_SUBS_IMM, ZR, 3, REGION_IWRAM);
	if (c->transfer && !toPc) {
		c->transfer->videoSite = aBcond(e, C_NE, NULL);
	} else {
		slow[(*slowCount)++] = aBcond(e, C_NE, NULL);
	}
	for (int region = REGION_IWRAM; region >= REGION_EWRAM; --region) {
		if (region == REGION_EWRAM) {
			patchHere(e, ewram);
		}
		emitRamTransfer(c, in, region, slow, slowCount);
		if (toPc) {
			aMov(e, 0, 1);
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
		} else if (!c->transfer || region != REGION_EWRAM) {
			c->fastDone[c->fastDoneCount++] = aB(e, NULL);
		}
	}
}

// Calls the exact CPU for the instruction, with its fetch counted; w0 = the next block key, or ~0.
static void emitExecute(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	aMovImm(e, 1, in->opcode);
	aMovImm(e, 2, in->address | c->thumb);
	callHelper(c, in, jitHelperExecute);
}

// Compiles LDM and STM. Transfers to work RAM are done inline; anything else goes to the exact CPU.
static void emitBlockTransfer(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	uint8_t* slow[16];
	int slowCount = 0;
	c->fastDoneCount = 0;
	// The fetch comes first, so I/O sees the time it happens at.
	clobberFlags(c);
	flushPending(c);
	if (!in->userBank && in->list && in->rn != 15 && (in->load || !(in->list & 0x8000)) && !storesMovedBase(in)) {
		if (!c->careful && c->transferCount < MAX_BLOCK_INSTS) {
			// The inline path stays here; the rest is emitted with the cold code by
			// emitSlowTransfer, from the state it would have had here and to the state this leaves.
			struct SlowTransfer* t = &c->transfers[c->transferCount++];
			t->in = in;
			t->index = c->cur;
			t->wrapCount = 0;
			t->siteCount = 0;
			t->ewramSite = NULL;
			t->videoSite = NULL;
			c->transfer = t;
			emitFastTransfer(c, in, t->sites, &t->siteCount);
			c->transfer = NULL;
			for (int i = 0; i < c->fastDoneCount; ++i) {
				patchHere(e, c->fastDone[i]);
			}
			t->join = e->p;
			canonical(c);
			resetPending(c);
			if (!loadsPc(in) && isTerminator(in)) {
				emitLinkTo(c, in->address + c->width, c->thumb);
			}
			return;
		}
		emitFastTransfer(c, in, slow, &slowCount);
		for (int i = 0; i < slowCount; ++i) {
			patchHere(e, slow[i]);
		}
		canonical(c);
	}
	emitExecute(c, in);
	if (loadsPc(in)) {
		setBiosPrefetchNext(c, in);
		aB(e, c->jit->dispatchKey);
		for (int i = 0; i < c->fastDoneCount; ++i) {
			patchHere(e, c->fastDone[i]);
		}
		return;
	}
	pendAdd(c, nonseq(c));
	flushPending(c);
	emitMissCheck(c);
	for (int i = 0; i < c->fastDoneCount; ++i) {
		patchHere(e, c->fastDone[i]);
	}
	if (isTerminator(in)) {
		emitLinkTo(c, in->address + c->width, c->thumb);
	}
}

// The copy of a block transfer for palette RAM, VRAM or OAM, in the cold code; w0 = first address,
// in the region. As the exact CPU's: every word costs the same, a store goes to the renderer only if
// it changes the word, and anything a store would make C do first (drawing a line whose H-blank
// has come, compiled code in video memory, the display holding VRAM) goes to the exact CPU at
// exact.
static void emitVideoTransfer(struct Compiler* c, const struct SlowTransfer* t, int region, uint8_t* exact) {
	struct Emitter* e = &c->e;
	const struct Inst* in = t->in;
	int count = __builtin_popcount(in->list);
	int cycles = count * (1 + c->gba->waitS32[region]) + (in->load ? 1 : 0);
	aAddImm(e, 3, R_CYC, cycles, 6);
	aDpImm(e, A_SUBS_IMM, ZR, 3, 0);
	aBcond(e, C_GE, exact);
	if (region == REGION_VRAM) {
		loadField(e, 4, OFF(vramStall), 1);
		aCbnz(e, 4, exact);
		// Below the mirrors of the sprite tiles.
		aAndBits(e, 4, 0, 0, 17);
		aMovImm(e, 5, SIZE_VRAM - 4 * count);
		aDp(e, A_SUBS, ZR, 4, 5, S_LSL, 0);
		aBcond(e, C_HI, exact);
	}
	if (!in->load) {
		loadField(e, 4, OFF(videoCode), 1);
		aCbnz(e, 4, exact);
		timeUntil(e, 4, 5, cycles, OFF(video.drawDue));
		aDpImm(e, A_SUBS_IMM, ZR, 4, 0);
		aBcond(e, C_GE, exact);
	}
	aAddImmX(e, 5, R_GBA, region == REGION_VRAM ? OFF(vram) : region == REGION_OAM ? OFF(oam) : OFF(palette));
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (!(in->list & (1 << r))) {
			continue;
		}
		aDpImm(e, A_ADD_IMM, 6, 0, 4 * k);
		aAndBits(e, 6, 6, 2, region == REGION_VRAM ? 15 : 8);
		if (in->load) {
			aLdst(e, L_LDR, hostReg[r], 5, 6, L_UXTW);
		} else {
			aLdst(e, L_LDR, 7, 5, 6, L_UXTW);
			aDp(e, A_SUBS, ZR, 7, hostReg[r], S_LSL, 0);
			uint8_t* same = aBcond(e, C_EQ, NULL);
			aLdst(e, L_STR, hostReg[r], 5, 6, L_UXTW);
			if (region == REGION_VRAM) {
				aLsrImm(e, 7, 6, 11);
				aAddImmX(e, 8, R_GBA, OFF(renderer.vramChanged));
				aMovImm(e, 16, 1);
				aLdst(e, L_STRB, 16, 8, 7, L_UXTW);
			} else {
				loadField(e, 7, OFF(renderer.memoryChanged), 1);
				aLogicImm(e, A_ORR_IMM, 7, 7, region == REGION_OAM ? RENDER_OAM : RENDER_PALETTE);
				storeField(e, 7, OFF(renderer.memoryChanged), 1);
			}
			patchHere(e, same);
		}
		++k;
	}
	if (in->writeback && (!in->load || !(in->list & (1 << in->rn)))) {
		int32_t start = in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
		aAddImm(e, hostReg[in->rn], 0, (in->up ? 4 * count : -4 * count) - start, 17);
	}
	aAddImm(e, R_CYC, R_CYC, cycles + nonseq(c), 17);
	aB(e, t->join);
}

// Emits the parts of a block transfer kept with the cold code (see emitBlockTransfer).
static void emitSlowTransfer(struct Compiler* c, struct SlowTransfer* t) {
	struct Emitter* e = &c->e;
	const struct Inst* in = t->in;
	uint8_t* start = e->p;
	for (int i = 0; i < t->siteCount; ++i) {
		patchHere(e, t->sites[i]);
	}
	c->cur = t->index;
	resetPending(c);
	canonical(c);
	emitExecute(c, in);
	if (loadsPc(in)) {
		setBiosPrefetchNext(c, in);
		aB(e, c->jit->dispatchKey);
	} else {
		pendAdd(c, nonseq(c));
		flushPending(c);
		emitMissCheck(c);
		aB(e, t->join);
	}
	if (t->ewramSite) {
		// w3 = the address's top byte, as at the test that led here.
		uint8_t* sites[16];
		int siteCount = 0;
		patchHere(e, t->ewramSite);
		aDpImm(e, A_SUBS_IMM, ZR, 3, REGION_EWRAM);
		aBcond(e, C_NE, start);
		c->transfer = t;
		emitRamTransfer(c, in, REGION_EWRAM, sites, &siteCount);
		c->transfer = NULL;
		for (int i = 0; i < siteCount; ++i) {
			if (sites[i] + 4 <= e->end) {
				patchBranch(sites[i], start);
			}
		}
		if (loadsPc(in)) {
			aMov(e, 0, 1);
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
		} else {
			aB(e, t->join);
		}
	}
	if (t->videoSite) {
		// w3 = the first address's region, w0 = the first address.
		patchHere(e, t->videoSite);
		for (int region = REGION_PALETTE; region <= REGION_OAM; ++region) {
			aDpImm(e, A_SUBS_IMM, ZR, 3, region);
			uint8_t* other = aBcond(e, C_NE, NULL);
			emitVideoTransfer(c, t, region, start);
			patchHere(e, other);
		}
		aB(e, start);
	}
	for (int i = 0; i < t->wrapCount; ++i) {
		patchHere(e, t->wraps[i].site);
		emitWrappedTransfer(c, in, t->wraps[i].region, NULL, NULL, start);
		aB(e, t->wraps[i].resume);
	}
}

// SWP, which the exact CPU runs.
static void emitSwap(struct Compiler* c, const struct Inst* in) {
	emitExecute(c, in);
	pendAdd(c, nonseq(c));
	emitMissCheck(c);
}

// MRS: the flags are kept apart from the CPSR while compiled code runs, so they are put back for
// the read.
static void emitMrs(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	if (in->spsr) {
		aLdr(e, 0, R_GBA, OFF(cpu.spsr));
	} else {
		needRegFlags(c);
		aLdr(e, 0, R_GBA, OFF(cpu.cpsr));
		aLsrImm(e, 1, R_FLAGS, 28);
		aBfi(e, 0, 1, 28, 4);
	}
	storeGuest(c, in->rd, 0);
}

// The exact CPU runs it from here, as a careful copy's stub does for the instruction an event falls
// in, and compiled code continues where it can.
static void emitStepExit(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	flushPending(c);
	storeHead(c, in->address + 2 * c->width);
	aBl(e, c->jit->spillRoutine);
	aMovX(e, 0, R_GBA);
	aMovImm(e, 1, in->address);
	callAbs(e, jitHelperStepOne);
	aBl(e, c->jit->reloadRoutine);
	aDpImm(e, A_ADDS_IMM, ZR, 0, 1);
	uint8_t* exact = aBcond(e, C_EQ, NULL);
	aB(e, c->jit->dispatchKey);
	patchHere(e, exact);
	storeField32(c, OFF(exitReason), EXIT_EXACT);
	aB(e, c->jit->exitRoutine);
}

// An instruction that writes the CPSR, run by the exact CPU. Compiled code continues after it, or
// wherever it went, unless the exact CPU has to take over.
static void emitSystem(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	bool writesPc = in->kind != I_MSR && !(in->kind == I_ALU && isTest(in->op));
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	aMovImm(e, 1, in->opcode);
	aMovImm(e, 2, in->address | c->thumb);
	callHelper(c, in, jitHelperSystem);
	aDpImm(e, A_ADDS_IMM, ZR, 0, 2);
	uint8_t* exact = aBcond(e, C_EQ, NULL);
	if (writesPc) {
		setBiosPrefetchNext(c, in);
		aB(e, c->jit->dispatchKey);
	} else {
		emitLinkTo(c, in->address + c->width, c->thumb);
	}
	patchHere(e, exact);
	storeField32(c, OFF(exitReason), EXIT_EXACT);
	aB(e, c->jit->exitRoutine);
}

// BX to a register.
static void emitBx(struct Compiler* c, const struct Inst* in) {
	loadGuest(c, 0, in->rm, in);
	emitIndirect(c, in, c->jit->dispatchBx);
}

// The branches: a known target is a linked jump, and BL also sets the return address.
static void emitBranch(struct Compiler* c, const struct Inst* in, int index) {
	switch (in->kind) {
	case I_B:
		if (in->link) {
			storeGuestImm(c, 14, in->address + 4);
		}
		emitDirectBranch(c, in, in->target);
		return;
	case I_BL2:
		if (index > 0 && c->insts[index - 1].kind == I_BL1) {
			uint32_t target = c->insts[index - 1].target + in->target;
			storeGuestImm(c, 14, (in->address + 2) | 1);
			emitDirectBranch(c, in, target);
			return;
		}
		aAddImm(&c->e, 0, hostReg[14], in->target, 17);
		storeGuestImm(c, 14, (in->address + 2) | 1);
		emitIndirect(c, in, c->jit->dispatchThumb);
		return;
	default:
		return;
	}
}

// Emits one instruction's code, by kind.
static void emitBody(struct Compiler* c, const struct Inst* in, int index) {
	if (jitStepOnly(in)) {
		emitStepExit(c, in);
		return;
	}
	if (jitSystem(in)) {
		emitSystem(c, in);
		return;
	}
	switch (in->kind) {
	case I_ALU:
		emitAlu(c, in);
		break;
	case I_MUL:
		emitMul(c, in);
		break;
	case I_MULL:
		emitMulLong(c, in);
		break;
	case I_MEM:
		emitMem(c, in);
		break;
	case I_BLOCK:
		emitBlockTransfer(c, in);
		break;
	case I_SWP:
		emitSwap(c, in);
		break;
	case I_MRS:
		emitMrs(c, in);
		break;
	case I_BL1:
		storeGuestImm(c, 14, in->target);
		break;
	case I_B:
	case I_BL2:
		emitBranch(c, in, index);
		break;
	case I_BX:
		emitBx(c, in);
		break;
	default:
		break;
	}
}

// Emits an instruction with its condition. Its fetch comes first, whether it runs or not; for an
// instruction left to the exact CPU, the exact CPU makes the fetch.
static void emitOne(struct Compiler* c, const struct Inst* in, int index) {
	struct Emitter* e = &c->e;
	bool step = jitStepOnly(in);
	if (in->cond == COND_AL) {
		if (!step) {
			pendFetch(c);
		}
		emitBody(c, in, index);
		return;
	}
	if (in->cond == 0xF) {
		pendFetch(c);
		if (isTerminator(in)) {
			clobberFlags(c);
			flushPending(c);
			emitLinkTo(c, in->address + c->width, c->thumb);
		}
		return;
	}
	if (!step) {
		pendFetch(c);
	}
	flushPending(c);
	uint8_t* skip = emitCondJump(c, in->cond, true);
	// Both paths continue from the flags in w21 (and the host's, if they were there).
	bool skipHost = c->inHost;
	emitBody(c, in, index);
	if (isTerminator(in) || step) {
		// When skipped, it costs its fetch and the block continues past it.
		patchHere(e, skip);
		c->inReg = true;
		c->inHost = skipHost;
		if (step) {
			pendFetch(c);
		}
		flushPending(c);
		emitIdleMark(c, in->address + c->width);
		emitLinkTo(c, in->address + c->width, c->thumb);
		return;
	}
	clobberFlags(c);
	flushPending(c);
	patchHere(e, skip);
	canonical(c);
}

// A word the game keeps rewriting: runs the code for the form it holds now, or leaves to compile a
// new form.
static void emitPoly(struct Compiler* c, const struct Inst* in, int index) {
	struct Emitter* e = &c->e;
	const struct Poly* poly = in->poly;
	clobberFlags(c);
	flushPending(c);
	uint32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (in->address & (SIZE_EWRAM - 1));
	loadField(e, 0, word, 4);
	uint8_t* to[MAX_FORMS];
	for (int i = 0; i < poly->count; ++i) {
		aMovImm(e, 1, poly->forms[i].opcode);
		aDp(e, A_EOR, 2, 0, 1, S_LSL, 0);
		// Clears the field read at run time: a load or store's offset and direction (bits 0-11 and
		// 23, two ANDs), a constant, or a shift amount.
		if (poly->masks[i] == ~0x800FFFu) {
			aLogicImm(e, A_AND_IMM, 2, 2, 0xFFFFF000);
			aLogicImm(e, A_AND_IMM, 2, 2, 0xFF7FFFFF);
		} else if (poly->masks[i] != ~0u && !aLogicImm(e, A_AND_IMM, 2, 2, poly->masks[i])) {
			aMovImm(e, 3, poly->masks[i]);
			aDp(e, A_AND, 2, 2, 3, S_LSL, 0);
		}
		to[i] = aCbz(e, 2, NULL);
	}
	emitLinkTo(c, in->address, false);
	c->links[c->linkCount - 1].formMiss = true;
	uint8_t* join[MAX_FORMS];
	int joins = 0;
	for (int i = 0; i < poly->count; ++i) {
		const struct Inst* form = &poly->forms[i];
		patchHere(e, to[i]);
		resetPending(c);
		canonical(c);
		emitOne(c, form, index);
		if (isTerminator(form)) {
			continue;
		}
		clobberFlags(c);
		flushPending(c);
		join[joins++] = aB(e, NULL);
	}
	for (int i = 0; i < joins; ++i) {
		patchHere(e, join[i]);
	}
	canonical(c);
}

// Emits one instruction with what surrounds it: the entry point a careful copy needs before it, its
// condition's test, and the cycle counting after it.
static void emitInst(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	const struct Inst* in = &c->insts[index];
	c->cur = index;
	if (c->careful) {
		// Enters here between instructions, with the flags in w21 and nothing pending.
		canonical(c);
		c->block->entries[index] = e->p;
		int cost = ownCost(c, index);
		emitCheck(c, index, cost);
		struct Stub* stub = &c->stubs[c->stubCount - 1];
		stub->cost = cost;
		stub->resume = e->p;
		stub->deferrable = deferrable(c, index);
	} else if (c->checkpoint[index]) {
		emitCheck(c, index, ahead(c, index));
	}
	if (c->prefetch && (c->careful || index == 0 || isDynamic(c, &c->insts[index - 1]) || jitRomLiteral(c->gba, &c->insts[index - 1], NULL, NULL))) {
		emitRestarted(c);
	}
	if (in->poly) {
		emitPoly(c, in, index);
	} else {
		emitOne(c, in, index);
	}
	if (c->careful) {
		clobberFlags(c);
		flushPending(c);
		if (deferrable(c, index) && fallsThrough(c, index)) {
			loadField(e, 16, OFF(jitDeferred), 1);
			c->deferred[c->deferredCount].site = aCbnz(e, 16, NULL);
			c->deferred[c->deferredCount++].index = index;
		}
	}
}

// At a waiting loop's start: when the last time round came back the same way, with no event run and
// the prefetch buffer as it was, the times round that end before the next event are skipped at
// once. For a loop reading DISPSTAT or VCOUNT, only those that end before they change, if they
// didn't change last time round.
static void emitIdleSkip(struct Compiler* c) {
	struct Emitter* e = &c->e;
	uint8_t* again[16];
	int count = 0;
	aLdr(e, 0, R_GBA, OFF(idleMark));
	aMovImm(e, 1, blockKey(c->insts[0].address, c->thumb));
	aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
	again[count++] = aBcond(e, C_NE, NULL);
	loadField(e, 0, OFF(eventRuns), 4);
	aLdr(e, 1, R_GBA, OFF(idleRuns));
	aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
	again[count++] = aBcond(e, C_NE, NULL);
	aLdr(e, 0, R_GBA, OFF(cpu.nextEvent));
	aLdr(e, 1, R_GBA, OFF(idleNext));
	aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
	again[count++] = aBcond(e, C_NE, NULL);
	if (c->prefetch) {
		aLdr(e, 0, R_GBA, OFF(jitBase));
		aDp(e, A_SUB, 0, 0, R_CYC, S_LSL, 0);
		aLdr(e, 1, R_GBA, OFF(idleLead));
		aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
		again[count++] = aBcond(e, C_NE, NULL);
	}
	// w2 = one time round, w0 = the cycles before the next event minus one.
	aLdr(e, 1, R_GBA, OFF(idleCycles));
	aDp(e, A_SUBS, 2, R_CYC, 1, S_LSL, 0);
	again[count++] = aBcond(e, C_LE, NULL);
	aMvn(e, 0, R_CYC);
	for (int i = 0; i < c->idleLoadCount; ++i) {
		aAddImm(e, 3, hostReg[c->idleLoads[i].base], c->idleLoads[i].offset, 4);
		aMovImm(e, 4, REGION_EWRAM << 24);
		aDp(e, A_SUBS, ZR, 3, 4, S_LSL, 0);
		uint8_t* low = aBcond(e, C_LO, NULL);
		aMovImm(e, 4, (REGION_IWRAM + 1) << 24);
		aDp(e, A_SUBS, ZR, 3, 4, S_LSL, 0);
		uint8_t* ram = aBcond(e, C_LO, NULL);
		patchHere(e, low);
		aLogicImm(e, A_AND_IMM, 3, 3, ~3u);
		aMovImm(e, 4, (REGION_IO << 24) | REG_DISPSTAT);
		aDp(e, A_SUBS, ZR, 3, 4, S_LSL, 0);
		again[count++] = aBcond(e, C_NE, NULL);
		aLdr(e, 3, R_GBA, OFF(video.stableUntil));
		aLdr(e, 4, R_GBA, OFF(idleStable));
		aDp(e, A_SUBS, ZR, 3, 4, S_LSL, 0);
		again[count++] = aBcond(e, C_NE, NULL);
		aLdr(e, 4, R_GBA, OFF(timing.masterCycles));
		aDp(e, A_SUB, 3, 3, 4, S_LSL, 0);
		aLdr(e, 4, R_GBA, OFF(cpu.nextEvent));
		aDp(e, A_SUB, 3, 3, 4, S_LSL, 0);
		aDp(e, A_SUB, 3, 3, R_CYC, S_LSL, 0);
		aDpImm(e, A_SUB_IMM, 3, 3, 1);
		aDp(e, A_SUBS, ZR, 3, 0, S_LSL, 0);
		aCsel(e, 0, 3, 0, C_LT);
		patchHere(e, ram);
	}
	again[count++] = aTbnz(e, 0, 31, NULL);
	aUdiv(e, 3, 0, 2);
	aMadd(e, 3, 3, 2, ZR);
	aDp(e, A_ADD, R_CYC, R_CYC, 3, S_LSL, 0);
	if (c->prefetch) {
		aLdr(e, 4, R_GBA, OFF(jitBase));
		aDp(e, A_ADD, 4, 4, 3, S_LSL, 0);
		aStr(e, 4, R_GBA, OFF(jitBase));
	}
	for (int i = 0; i < count; ++i) {
		patchHere(e, again[i]);
	}
	aStr(e, ZR, R_GBA, OFF(idleMark));
	aStr(e, R_CYC, R_GBA, OFF(idleCycles));
	loadField(e, 0, OFF(eventRuns), 4);
	aStr(e, 0, R_GBA, OFF(idleRuns));
	aLdr(e, 0, R_GBA, OFF(cpu.nextEvent));
	aStr(e, 0, R_GBA, OFF(idleNext));
	if (c->prefetch) {
		aLdr(e, 0, R_GBA, OFF(jitBase));
		aDp(e, A_SUB, 0, 0, R_CYC, S_LSL, 0);
		aStr(e, 0, R_GBA, OFF(idleLead));
	}
	if (c->idleLoadCount) {
		aLdr(e, 0, R_GBA, OFF(video.stableUntil));
		aStr(e, 0, R_GBA, OFF(idleStable));
	}
}

// An idle loop's head (jitIsIdleHead): once events have run since the run loop last looked at it,
// it leaves for the run loop to look again.
static void emitIdleHead(struct Compiler* c) {
	struct Emitter* e = &c->e;
	loadField(e, 0, OFF(eventRuns), 4);
	loadField(e, 1, OFF(idleHeadRuns), 4);
	aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
	uint8_t* same = aBcond(e, C_EQ, NULL);
	// As a deferred event's exit leaves things.
	uint32_t pc = c->insts[0].address;
	storeHead(c, pc + 2 * c->width);
	storeField32(c, OFF(exitPc), pc);
	storeField32(c, OFF(exitReason), EXIT_LOOKUP);
	aB(e, c->jit->exitRoutine);
	patchHere(e, same);
}

// Forgets what a compile whose code didn't fit left behind.
static void abandon(struct Compiler* c) {
	for (int i = 0; i < c->newTinyCount; ++i) {
		c->newTiny[i]->code = NULL;
	}
	c->block->siteCount = 0;
	free(c->block->out);
	c->block->out = NULL;
	c->block->outCount = 0;
}

// Compiles a block into the current stretch: the analysis, then each instruction, then the cold code
// the instructions left for the end (slow paths, exits and stubs). Returns false, having written
// nothing outside the free space there, if its code didn't fit.
static bool compileInStretch(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
	struct GBA* gba = jit->gba;
	struct Compiler c = {
		.jit = jit,
		.gba = gba,
		.block = block,
		.insts = insts,
		.count = count,
		.nextOps = nextOps,
		.thumb = block->thumb,
		.width = block->thumb ? 2 : 4,
		.region = block->pc >> 24,
		.inReg = true,
	};
	if (block->careful) {
		c.e = (struct Emitter) { .start = jit->coldNext, .p = jit->coldNext, .end = jit->coldEnd };
	} else {
		c.e = (struct Emitter) { .start = jit->codeNext, .p = jit->codeNext, .end = jit->hotEnd };
	}
	c.S = c.thumb ? gba->waitS16[c.region] : gba->waitS32[c.region];
	c.N = c.thumb ? gba->waitN16[c.region] : gba->waitN32[c.region];
	c.P = 1 + c.S;
	c.prefetch = inCartridge(c.region) && gba->prefetchEnabled;
	c.D = c.P;
	c.full = (c.thumb ? 8 : 4) * c.D;
	c.Ncyc = 1 + c.N;
	c.penaltyRow = OFF(jitStopPenalty) + (c.thumb ? 0 : 4096) + c.D * 128;
	c.careful = block->careful;
	block->precharged = inCartridge(c.region) && !c.prefetch;
	resetPending(&c);
	analyze(&c);
	c.idleExit = idleExit(&c);

	block->entry = c.e.p;
	if (jitIsIdleHead(jit, blockKey(block->pc, block->thumb))) {
		emitIdleHead(&c);
	}
	if (c.idleExit) {
		emitIdleSkip(&c);
	}
	for (int i = 0; i < count; ++i) {
		emitInst(&c, i);
	}
	// A rewritten word's other forms continue past the one it held when compiled.
	const struct Inst* last = &insts[count - 1];
	if ((!isTerminator(last) && !jitStepOnly(last)) || last->poly) {
		clobberFlags(&c);
		flushPending(&c);
		emitLinkTo(&c, last->address + c.width, c.thumb);
	}

	// Cold code: memory accesses that missed the inline path, cycle-check exits, and unlinked
	// branches.
	struct Emitter* e = &c.e;
	uint8_t* bodyEnd = e->p;
	if (e->p > e->end) {
		// The body ran past its part: nothing is patched, since the sites past the end were never
		// written.
		abandon(&c);
		return false;
	}
	if (!c.careful) {
		e->p = jit->coldNext;
		e->end = jit->coldEnd;
	}
	uint8_t* cold = e->p;
	block->cold = c.careful ? block->entry : cold;
	for (int i = 0; i < c.transferCount; ++i) {
		emitSlowTransfer(&c, &c.transfers[i]);
	}
	resetPending(&c);
	canonical(&c);
	for (int i = 0; i < c.slowCount; ++i) {
		struct SlowAccess* slow = &c.slow[i];
		const struct Inst* in = slow->in;
		patchHere(e, slow->sites[0]);
		if (slow->sites[1]) {
			patchHere(e, slow->sites[1]);
		}
		if (!in->load && ramCode(&c)) {
			storeField32(&c, REG(15), in->address + 2 * c.width);
		}
		aBl(e, memRoutine(&c, memKind(in)));
		jitRecordSite(block, e->p - block->entry, in->address, opcodeAt(&c, slow->index + 1), opcodeAt(&c, slow->index + 2));
		if (in->load) {
			storeGuest(&c, in->rd, 0);
		}
		emitMissCheck(&c);
		aB(e, slow->resume);
	}
	for (int i = 0; i < c.stubCount; ++i) {
		struct Stub* stub = &c.stubs[i];
		patchHere(e, stub->site);
		if (stub->pending) {
			aAddImm(e, R_CYC, R_CYC, stub->pending, 17);
		}
		if (!block->careful) {
			// Continues in the careful copy from the same instruction, once there is one.
			struct Link* link = &c.links[c.linkCount];
			memset(link, 0, sizeof(*link));
			c.linkSites[c.linkCount++] = aB(e, NULL);
			link->targetKey = blockKey(stub->pc, c.thumb);
			link->careful = true;
			link->index = stub->index;
			link->from = block;
			continue;
		}
		// Events inside the instruction run after it, or the exact CPU runs it.
		storeHead(&c, stub->pc + 2 * c.width);
		uint8_t* leaveHere = NULL;
		if (!block->precharged) {
			aBl(e, jit->spillRoutine);
			aMovX(e, 0, R_GBA);
			aMovImm(e, 1, stub->cost);
			aMovImm(e, 2, stub->deferrable);
			aMovImm64(e, 3, (uint64_t) (uintptr_t) block);
			callAbs(e, jitHelperCareful);
			aBl(e, jit->reloadRoutine);
			aDpImm(e, A_SUBS_IMM, ZR, 0, CAREFUL_RUN);
			aBcond(e, C_EQ, stub->resume);
			aDpImm(e, A_SUBS_IMM, ZR, 0, CAREFUL_LEAVE);
			leaveHere = aBcond(e, C_EQ, NULL);
		}
		// The exact CPU runs the instruction an event falls in; compiled code continues after it,
		// here if the copy is still there.
		aBl(e, jit->spillRoutine);
		aMovX(e, 0, R_GBA);
		aMovImm(e, 1, stub->pc);
		callAbs(e, jitHelperStepOne);
		aBl(e, jit->reloadRoutine);
		aDpImm(e, A_ADDS_IMM, ZR, 0, 1);
		uint8_t* leave = aBcond(e, C_EQ, NULL);
		if (stub->index + 1 < count) {
			aMovImm(e, 1, blockKey(stub->pc + c.width, c.thumb));
			aDp(e, A_SUBS, ZR, 0, 1, S_LSL, 0);
			uint8_t* elsewhere = aBcond(e, C_NE, NULL);
			aMovImm64(e, 1, (uint64_t) (uintptr_t) &block->valid);
			aLdrb(e, 1, 1, 0);
			uint8_t* gone = aCbz(e, 1, NULL);
			aB(e, block->entries[stub->index + 1]);
			patchHere(e, elsewhere);
			patchHere(e, gone);
		}
		aB(e, jit->dispatchKey);
		patchHere(e, leave);
		storeField32(&c, OFF(exitReason), EXIT_EXACT);
		aB(e, jit->exitRoutine);
		if (leaveHere) {
			// Leaves between instructions, for the run loop.
			patchHere(e, leaveHere);
			storeField32(&c, OFF(exitPc), stub->pc);
			storeField32(&c, OFF(exitReason), EXIT_LOOKUP);
			aB(e, jit->exitRoutine);
		}
	}
	for (int i = 0; i < c.deferredCount; ++i) {
		// The events inside the instruction just done.
		int index = c.deferred[i].index;
		uint32_t next = c.insts[0].address + (index + 1) * c.width;
		patchHere(e, c.deferred[i].site);
		storeHead(&c, next + 2 * c.width);
		aBl(e, jit->spillRoutine);
		aMovX(e, 0, R_GBA);
		aMovImm64(e, 1, (uint64_t) (uintptr_t) block);
		callAbs(e, jitHelperDeferred);
		aBl(e, jit->reloadRoutine);
		aCbz(e, 0, block->entries[index + 1]);
		storeField32(&c, OFF(exitPc), next);
		storeField32(&c, OFF(exitReason), EXIT_LOOKUP);
		aB(e, jit->exitRoutine);
	}
	block->outCount = c.linkCount;
	block->out = malloc(c.linkCount * sizeof(*block->out));
	memcpy(block->out, c.links, c.linkCount * sizeof(*block->out));
	for (int i = 0; i < block->outCount; ++i) {
		struct Link* link = &block->out[i];
		link->site = c.linkSites[i];
		link->stub = e->p;
		if (inCartridge(link->targetKey >> 24)) {
			storeHead(&c, (link->targetKey & ~1u) + 2 * c.width);
		}
		// ldr x0, =link
		emit32(e, 0x58000000 | (2 << 5));
		aB(e, jit->exitLinkRoutine);
		uint64_t pointer = (uintptr_t) link;
		emit32(e, (uint32_t) pointer);
		emit32(e, (uint32_t) (pointer >> 32));
	}
	if (e->p > e->end) {
		abandon(&c);
		return false;
	}
	for (int i = 0; i < block->outCount; ++i) {
		patchBranch(block->out[i].site, block->out[i].stub);
	}
	if (c.careful) {
		flushCode(c.e.start, e->p);
	} else {
		flushCode(c.e.start, bodyEnd);
		flushCode(cold, e->p);
		jit->codeNext = bodyEnd;
	}
	jitNameBlock(block, bodyEnd, cold, e->p);
	jit->coldNext = e->p;
	return true;
}

// A block's code goes in the stretch being filled while there is room for a large block there,
// otherwise in a new one; a block too large for the room left is compiled again in a new stretch.
// Fails if the code memory is full; the caller then flushes.
bool backendCompile(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
	if (jit->hotEnd - jit->codeNext < HOT_ROOM || jit->coldEnd - jit->coldNext < COLD_ROOM) {
		if (jit->codeEnd - jit->coldEnd < STRETCH) {
			return false;
		}
		startStretch(jit, jit->coldEnd);
	}
	if (compileInStretch(jit, block, insts, count, nextOps)) {
		return true;
	}
	if (jit->codeEnd - jit->coldEnd < STRETCH) {
		return false;
	}
	startStretch(jit, jit->coldEnd);
	return compileInStretch(jit, block, insts, count, nextOps);
}
