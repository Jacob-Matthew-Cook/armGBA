/* Copyright (c) 2026 Magnominus
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
//   r11  the GBA state          r10  cycles counted from the next event (cpu.cycles - cpu.nextEvent)
//   r9   guest NZCV in bits 31-28, when the host's flags don't hold them
//   r4-r8  guest r0-r4
//   r0-r3, lr  scratch; memory routines take the address in r1 and a store value in r2, and return
//   a load in r0
//   r12  the offset of a field too far from r11 for an instruction's own offset
// The host's flags and shifter work as the guest's do, so data processing compiles to the same
// instruction.

#define OFF(field) ((int32_t) offsetof(struct GBA, field))
#define REG(n) (OFF(cpu.r) + 4 * (n))

enum {
	R_FLAGS = R9,
	R_CYC = R10,
	R_GBA = R11,
};

enum {
	F_V = 1,
	F_C = 2,
	F_Z = 4,
	F_N = 8,
	F_ALL = 15,
};

static const int8_t hostReg[16] = { R4, R5, R6, R7, R8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const int pinned[] = { R4, R5, R6, R7, R8 };
static const int pinnedGuest[] = { 0, 1, 2, 3, 4 };

_Static_assert(sizeof(struct LookupEntry) == 12 && offsetof(struct LookupEntry, entry) == 8, "dispatchKey's table layout");

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
	aMovI(e, R12, (uint32_t) (uintptr_t) fn);
	aBlx(e, R12);
}

// Loads or stores a field of struct GBA of 1, 2 or 4 bytes; a far one goes through r12.
static void fieldAccess(struct Emitter* e, bool load, int size, int rt, uint32_t offset) {
	if (size == 2) {
		if (offset < 256) {
			aMemH(e, load, H_HALF, rt, R_GBA, (int32_t) offset);
		} else {
			aMovI(e, R12, offset);
			aMemHR(e, load, H_HALF, rt, R_GBA, R12);
		}
		return;
	}
	if (offset < 4096) {
		aMem(e, C_AL, load, size == 1, rt, R_GBA, (int32_t) offset);
	} else {
		aMovI(e, R12, offset);
		aMemR(e, load, size == 1, rt, R_GBA, R12, 0);
	}
}

static void ldrF(struct Emitter* e, int rt, uint32_t offset) {
	fieldAccess(e, true, 4, rt, offset);
}

static void strF(struct Emitter* e, int rt, uint32_t offset) {
	fieldAccess(e, false, 4, rt, offset);
}

static void ldrbF(struct Emitter* e, int rt, uint32_t offset) {
	fieldAccess(e, true, 1, rt, offset);
}

static void strbF(struct Emitter* e, int rt, uint32_t offset) {
	fieldAccess(e, false, 1, rt, offset);
}

// Stores a constant to a field, through tmp.
static void strFImm(struct Emitter* e, uint32_t offset, uint32_t value, int size, int tmp) {
	aMovI(e, tmp, value);
	fieldAccess(e, false, size, tmp, offset);
}

// rd = the address of a field, for indexing into it.
static void fieldAddr(struct Emitter* e, int rd, uint32_t offset) {
	aAddI(e, rd, R_GBA, (int32_t) offset, R12);
}

// Compares rn with a constant, through tmp if it doesn't encode.
static void aCmpI(struct Emitter* e, int rn, int32_t value, int tmp) {
	if (immEncode((uint32_t) value) >= 0) {
		aDpI(e, C_AL, D_CMP, true, 0, rn, (uint32_t) value);
	} else if (immEncode((uint32_t) -value) >= 0) {
		aDpI(e, C_AL, D_CMN, true, 0, rn, (uint32_t) -value);
	} else {
		aMovI(e, tmp, (uint32_t) value);
		aDpR(e, C_AL, D_CMP, true, 0, rn, tmp, S_LSL, 0);
	}
}

// Sets rd to the time now plus cycles, minus the field at offset, and the flags from that: negative
// while that time is still ahead.
static void timeUntil(struct Emitter* e, int rd, int tmp, int32_t cycles, uint32_t offset) {
	ldrF(e, rd, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_ADD, false, rd, rd, R_CYC, S_LSL, 0);
	ldrF(e, tmp, OFF(timing.masterCycles));
	aDpR(e, C_AL, D_ADD, false, rd, rd, tmp, S_LSL, 0);
	if (cycles) {
		aAddI(e, rd, rd, cycles, R12);
	}
	ldrF(e, tmp, offset);
	aDpR(e, C_AL, D_SUB, true, rd, rd, tmp, S_LSL, 0);
}

// Shared routines.

// Converts the prefetch buffer's base between jitBase and the form C code keeps.
static void storeBase(struct Emitter* e, int ra, int rb) {
	ldrF(e, ra, OFF(jitBase));
	ldrF(e, rb, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_ADD, false, ra, ra, rb, S_LSL, 0);
	ldrF(e, rb, OFF(timing.masterCycles));
	aDpR(e, C_AL, D_ADD, false, ra, ra, rb, S_LSL, 0);
	strF(e, ra, OFF(prefetcher.base));
}

// The prefetch buffer's start time, in the same terms as the cycle counter.
static void loadBase(struct Emitter* e, int ra, int rb) {
	ldrF(e, ra, OFF(prefetcher.base));
	ldrF(e, rb, OFF(timing.masterCycles));
	aDpR(e, C_AL, D_SUB, false, ra, ra, rb, S_LSL, 0);
	ldrF(e, rb, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_SUB, false, ra, ra, rb, S_LSL, 0);
	strF(e, ra, OFF(jitBase));
}

// The routine that stores the guest registers held in host registers, the cycle counter and the
// flags in r9 to the CPU state. Preserves r0.
static void emitSpill(struct Emitter* e) {
	for (int i = 0; i < 5; ++i) {
		strF(e, pinned[i], REG(pinnedGuest[i]));
	}
	ldrF(e, R1, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_ADD, false, R1, R1, R_CYC, S_LSL, 0);
	strF(e, R1, OFF(cpu.cycles));
	ldrF(e, R1, OFF(cpu.cpsr));
	aDpI(e, C_AL, D_BIC, false, R1, R1, 0xF0000000);
	aDpI(e, C_AL, D_AND, false, R2, R_FLAGS, 0xF0000000);
	aDpR(e, C_AL, D_ORR, false, R1, R1, R2, S_LSL, 0);
	strF(e, R1, OFF(cpu.cpsr));
	storeBase(e, R1, R2);
	aBx(e, LR);
}

// The reverse; also preserves r0.
static void emitReload(struct Emitter* e) {
	for (int i = 0; i < 5; ++i) {
		ldrF(e, pinned[i], REG(pinnedGuest[i]));
	}
	ldrF(e, R_CYC, OFF(cpu.cycles));
	ldrF(e, R1, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_SUB, false, R_CYC, R_CYC, R1, S_LSL, 0);
	loadBase(e, R1, R2);
	ldrF(e, R_FLAGS, OFF(cpu.cpsr));
	aBx(e, LR);
}

// The routine C calls to run compiled code: saves the host's registers, loads the guest's, and
// jumps to the given block. Ten registers keep the stack 8-byte aligned.
static void emitEnter(struct Jit* jit, struct Emitter* e) {
	aPush(e, 0x5FF0);
	aMov(e, R_GBA, R0);
	strF(e, SP, OFF(hostStack));
	aMov(e, R0, R1);
	aBl(e, jit->reloadRoutine);
	aBx(e, R0);
}

// The way back out: stores the guest registers, restores the host's, and returns the reason.
static void emitExit(struct Jit* jit, struct Emitter* e) {
	ldrF(e, SP, OFF(hostStack));
	aBl(e, jit->spillRoutine);
	ldrF(e, R0, OFF(exitReason));
	aPop(e, 0x9FF0);
}

// Looks up the block for the key in r0 and jumps to it, compiling it first if needed.
static void emitDispatchKey(struct Jit* jit, struct Emitter* e) {
	aDpR(e, C_AL, D_EOR, false, R1, R0, R0, S_LSR, 13);
	aUbfx(e, R1, R1, 0, LOOKUP_BITS);
	aDpR(e, C_AL, D_ADD, false, R1, R1, R1, S_LSL, 1);
	ldrF(e, R2, OFF(lookup));
	aDpR(e, C_AL, D_ADD, false, R2, R2, R1, S_LSL, 2);
	aLdr(e, R3, R2, 0);
	aDpR(e, C_AL, D_CMP, true, 0, R3, R0, S_LSL, 0);
	aMem(e, C_EQ, true, false, PC, R2, 8);
	strF(e, R0, OFF(exitPc));
	strFImm(e, OFF(exitReason), EXIT_LOOKUP, 4, R1);
	aB(e, C_AL, jit->exitRoutine);
}

// memorySetActiveRegion's bookkeeping for a PC write into a region compiled code refills itself;
// r2 holds the new region. Uses r1.
static uint8_t* emitRegionUpdate(struct Emitter* e) {
	uint8_t* start = e->p;
	ldrF(e, R1, OFF(activeRegion));
	aDpR(e, C_AL, D_CMP, true, 0, R1, R2, S_LSL, 0);
	uint8_t* same = aB(e, C_EQ, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R1, 0);
	uint8_t* notBios = aB(e, C_NE, NULL);
	ldrF(e, R1, OFF(biosPrefetchNext));
	strF(e, R1, OFF(biosPrefetch));
	patchHere(e, notBios);
	strF(e, R2, OFF(activeRegion));
	strF(e, R2, OFF(fetchRegion));
	patchHere(e, same);
	aBx(e, LR);
	return start;
}

// busRefillCartridge's two fetches, for r0 = target and r2 = its region (at least 8), when nothing
// can come between them; other refills go to the exact CPU through slow.
static void emitCartridgeRefill(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate, uint8_t** slow, int* slowCount) {
	int size = thumb ? 2 : 4;
	aDpI(e, C_AL, D_CMP, true, 0, R2, REGION_ROM2_EX);
	slow[(*slowCount)++] = aB(e, C_HI, NULL);
	ldrbF(e, R1, OFF(prefetchDisabled));
	aDpI(e, C_AL, D_CMP, true, 0, R1, 0);
	slow[(*slowCount)++] = aB(e, C_NE, NULL);
	// A new burst starts at the second fetch.
	aDpI(e, C_AL, D_ADD, false, R1, R0, size);
	aDpR(e, C_AL, D_MOV, true, R1, 0, R1, S_LSL, 15);
	slow[(*slowCount)++] = aB(e, C_EQ, NULL);
	// The cost of stopping a read in flight: prefetcherStop's table, indexed by how far into the
	// read it is.
	aMovI(e, R1, 0);
	ldrbF(e, R3, OFF(cpu.prefetching));
	aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
	uint8_t* stopped = aB(e, C_EQ, NULL);
	ldrF(e, R3, OFF(prefetcher.head));
	aDpR(e, C_AL, D_CMP, true, 0, R0, R3, S_LSL, 0);
	slow[(*slowCount)++] = aB(e, C_EQ, NULL);
	ldrF(e, R3, OFF(jitBase));
	aDpR(e, C_AL, D_SUB, false, R2, R_CYC, R3, S_LSL, 0);
	aDpI(e, C_AL, D_CMP, true, 0, R2, 127);
	uint8_t* far = aB(e, C_HI, NULL);
	ldrF(e, R1, OFF(prefetcher.duty));
	aDpR(e, C_AL, D_ADD, false, R1, R2, R1, S_LSL, 7);
	ldrF(e, R3, OFF(prefetcher.width));
	aDpI(e, C_AL, D_CMP, true, 0, R3, 4);
	aDpI(e, C_EQ, D_ADD, false, R1, R1, 4096);
	fieldAddr(e, R3, OFF(jitStopPenalty));
	aMemR(e, true, true, R1, R3, R1, 0);
	uint8_t* counted = aB(e, C_AL, NULL);
	patchHere(e, far);
	aMovI(e, R1, 0);
	patchHere(e, stopped);
	patchHere(e, counted);
	// Then the nonsequential fetch and the sequential one.
	aDpR(e, C_AL, D_MOV, false, R2, 0, R0, S_LSR, 24);
	fieldAddr(e, R3, thumb ? OFF(writePcCost16) : OFF(writePcCost32));
	aMemR(e, true, true, R2, R3, R2, 0);
	aDpR(e, C_AL, D_ADD, false, R1, R1, R2, S_LSL, 0);
	aDpR(e, C_AL, D_ADD, true, R_CYC, R_CYC, R1, S_LSL, 0);
	uint8_t* late = aB(e, C_PL, NULL);
	aMovI(e, R3, 0);
	strbF(e, R3, OFF(cpu.prefetching));
	ldrbF(e, R3, OFF(prefetchEnabled));
	aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
	uint8_t* off = aB(e, C_EQ, NULL);
	// Started by the first fetch, which the second waits on.
	aMovI(e, R3, 1);
	strbF(e, R3, OFF(cpu.prefetching));
	strFImm(e, OFF(prefetcher.width), size, 4, R3);
	aDpR(e, C_AL, D_MOV, false, R2, 0, R0, S_LSR, 24);
	fieldAddr(e, R3, thumb ? OFF(waitS16) : OFF(waitS32));
	aMemR(e, true, true, R2, R3, R2, 0);
	aDpI(e, C_AL, D_ADD, false, R2, R2, 1);
	strF(e, R2, OFF(prefetcher.duty));
	aDpR(e, C_AL, D_MOV, false, R2, 0, R2, S_LSL, thumb ? 3 : 2);
	strF(e, R2, OFF(prefetcher.full));
	aDpI(e, C_AL, D_ADD, false, R2, R0, 2 * size);
	strF(e, R2, OFF(prefetcher.head));
	strF(e, R_CYC, OFF(jitBase));
	patchHere(e, off);
	aDpI(e, C_AL, D_ADD, false, R2, R0, 2 * size);
	aUbfx(e, R2, R2, 0, 25);
	strF(e, R2, OFF(romNext));
	strF(e, R2, OFF(romLatch));
	aMovI(e, R3, 1);
	strbF(e, R3, OFF(cpu.fetchSeq));
	aMovI(e, R3, 0);
	strbF(e, R3, OFF(lastAccessDma));
	strF(e, R3, OFF(dmaIdleCredit));
	// After a refill, the IRQ check uses the mask as it is now.
	ldrF(e, R3, OFF(cpu.cpsr));
	aDpI(e, C_AL, D_AND, false, R3, R3, PSR_I);
	aDpR(e, C_AL, D_MOV, false, R3, 0, R3, S_LSR, 7);
	strbF(e, R3, OFF(cpu.irqMasked));
	aDpR(e, C_AL, D_MOV, false, R2, 0, R0, S_LSR, 24);
	aBl(e, regionUpdate);
	if (thumb) {
		aDpI(e, C_AL, D_ORR, false, R0, R0, 1);
	}
	aB(e, C_AL, jit->dispatchKey);
	patchHere(e, late);
	aDpR(e, C_AL, D_SUB, false, R_CYC, R_CYC, R1, S_LSL, 0);
	slow[(*slowCount)++] = aB(e, C_AL, NULL);
}

// Branches to r0, a target address in the given instruction set. Compiled code refills from BIOS,
// RAM, palette, VRAM and OAM itself; refills elsewhere, or ones an event would fall inside, go to
// the exact CPU. As in mGBA, an ARM-state PC keeps bit 1: fetches ignore it, but PC reads see it.
static void emitDispatchPc(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate) {
	uint8_t* slow[8];
	int slowCount = 0;
	aDpI(e, C_AL, D_BIC, false, R0, R0, 1);
	aDpR(e, C_AL, D_MOV, false, R2, 0, R0, S_LSR, 24);
	aDpI(e, C_AL, D_CMP, true, 0, R2, 8);
	uint8_t* far = aB(e, C_HS, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R2, 1);
	slow[slowCount++] = aB(e, C_EQ, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R2, REGION_IO);
	slow[slowCount++] = aB(e, C_EQ, NULL);
	fieldAddr(e, R3, thumb ? OFF(writePcCost16) : OFF(writePcCost32));
	aMemR(e, true, true, R1, R3, R2, 0);
	aDpR(e, C_AL, D_ADD, true, R_CYC, R_CYC, R1, S_LSL, 0);
	uint8_t* late = aB(e, C_PL, NULL);
	aBl(e, regionUpdate);
	if (thumb) {
		aDpI(e, C_AL, D_ORR, false, R0, R0, 1);
	}
	aB(e, C_AL, jit->dispatchKey);
	patchHere(e, late);
	aDpR(e, C_AL, D_SUB, false, R_CYC, R_CYC, R1, S_LSL, 0);
	slow[slowCount++] = aB(e, C_AL, NULL);
	patchHere(e, far);
	emitCartridgeRefill(jit, e, thumb, regionUpdate, slow, &slowCount);
	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, slow[i]);
	}
	if (thumb) {
		aDpI(e, C_AL, D_ORR, false, R0, R0, 1);
	}
	strF(e, R0, OFF(exitPc));
	aBl(e, jit->spillRoutine);
	aMov(e, R0, R_GBA);
	ldrF(e, R1, OFF(exitPc));
	callAbs(e, jitHelperReload);
	aBl(e, jit->reloadRoutine);
	ldrF(e, R0, OFF(exitPc));
	aB(e, C_AL, jit->dispatchKey);
}

// BX's target: bit 0 chooses the instruction set, the CPSR follows, then the block is looked up.
static void emitDispatchBx(struct Jit* jit, struct Emitter* e) {
	ldrF(e, R1, OFF(cpu.cpsr));
	aDpI(e, C_AL, D_TST, true, 0, R0, 1);
	uint8_t* toThumb = aB(e, C_NE, NULL);
	aDpI(e, C_AL, D_TST, true, 0, R1, PSR_T);
	aB(e, C_EQ, jit->dispatchArm);
	aDpI(e, C_AL, D_BIC, false, R1, R1, PSR_T);
	strF(e, R1, OFF(cpu.cpsr));
	aB(e, C_AL, jit->dispatchArm);
	patchHere(e, toThumb);
	aDpI(e, C_AL, D_TST, true, 0, R1, PSR_T);
	aB(e, C_NE, jit->dispatchThumb);
	aDpI(e, C_AL, D_ORR, false, R1, R1, PSR_T);
	strF(e, R1, OFF(cpu.cpsr));
	aB(e, C_AL, jit->dispatchThumb);
}

// Calls a C function from inside a routine, preserving r0, r2, r3 and lr: r1 in, r1 out.
static void emitPreserveCall(struct Emitter* e, const void* fn) {
	aPush(e, (1 << R0) | (1 << R2) | (1 << R3) | (1 << LR));
	ldrF(e, R0, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_ADD, false, R0, R0, R_CYC, S_LSL, 0);
	strF(e, R0, OFF(cpu.cycles));
	storeBase(e, R0, R2);
	aMov(e, R0, R_GBA);
	callAbs(e, fn);
	aMov(e, R1, R0);
	ldrF(e, R_CYC, OFF(cpu.cycles));
	ldrF(e, R0, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_SUB, false, R_CYC, R_CYC, R0, S_LSL, 0);
	loadBase(e, R0, R2);
	aPop(e, (1 << R0) | (1 << R2) | (1 << R3) | (1 << LR));
}

// Memory access routines. Each inline path takes the bus's cycles for its region, including a
// load's internal cycle. An access an event would fall inside goes to the exact CPU's access
// instead.

// Goes through bus.c, events and all; r1 = address, r2 = a value to store.
static void emitSlowPath(struct Emitter* e, int kind) {
	// The return address identifies the guest instruction for open-bus reads.
	strF(e, LR, OFF(siteReturn));
	ldrF(e, R0, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_ADD, false, R0, R0, R_CYC, S_LSL, 0);
	strF(e, R0, OFF(cpu.cycles));
	storeBase(e, R0, R3);
	// The address, for sign extension.
	aPush(e, (1 << R1) | (1 << LR));
	aMov(e, R3, R2);
	aMov(e, R2, R1);
	aMovI(e, R1, kind);
	aMov(e, R0, R_GBA);
	callAbs(e, jitSlowAccess);
	if (kind == MEM_LOADS8) {
		aExtend(e, X_SXTB, R0, R0);
	} else if (kind == MEM_LOADS16) {
		aLdr(e, R1, SP, 0);
		aDpI(e, C_AL, D_TST, true, 0, R1, 1);
		uint8_t* odd = aB(e, C_NE, NULL);
		aExtend(e, X_SXTH, R0, R0);
		uint8_t* done = aB(e, C_AL, NULL);
		patchHere(e, odd);
		aExtend(e, X_SXTB, R0, R0);
		patchHere(e, done);
	}
	aPop(e, (1 << R1) | (1 << LR));
	ldrF(e, R_CYC, OFF(cpu.cycles));
	ldrF(e, R2, OFF(cpu.nextEvent));
	aDpR(e, C_AL, D_SUB, false, R_CYC, R_CYC, R2, S_LSL, 0);
	loadBase(e, R2, R3);
	aBx(e, LR);
}

// Bytes moved by each kind of memory routine.
static int accessSize(int kind) {
	switch (kind) {
	case MEM_LOAD32:
	case MEM_STORE32:
		return 4;
	case MEM_LOAD16:
	case MEM_LOADS16:
	case MEM_STORE16:
		return 2;
	default:
		return 1;
	}
}

// Loads data into r0 from [base + r0]; r1 still holds the address. Uses r2.
static void emitLoadData(struct Emitter* e, int kind, int base) {
	switch (kind) {
	case MEM_LOAD32:
		aMemR(e, true, false, R0, base, R0, 0);
		aDpR(e, C_AL, D_MOV, false, R2, 0, R1, S_LSL, 30);
		aDpR(e, C_AL, D_MOV, false, R2, 0, R2, S_LSR, 27);
		aDpRR(e, C_AL, D_MOV, false, R0, 0, R0, S_ROR, R2);
		break;
	case MEM_LOAD16:
		aMemHR(e, true, H_HALF, R0, base, R0);
		aDpI(e, C_AL, D_TST, true, 0, R1, 1);
		aDpR(e, C_NE, D_MOV, false, R0, 0, R0, S_ROR, 8);
		break;
	case MEM_LOADS16: {
		aDpI(e, C_AL, D_TST, true, 0, R1, 1);
		uint8_t* odd = aB(e, C_NE, NULL);
		aMemHR(e, true, H_SHALF, R0, base, R0);
		uint8_t* done = aB(e, C_AL, NULL);
		patchHere(e, odd);
		aDpI(e, C_AL, D_ADD, false, R0, R0, 1);
		aMemHR(e, true, H_SBYTE, R0, base, R0);
		patchHere(e, done);
		break;
	}
	case MEM_LOAD8:
		aMemR(e, true, true, R0, base, R0, 0);
		break;
	case MEM_LOADS8:
		aMemHR(e, true, H_SBYTE, R0, base, R0);
		break;
	}
}

// Compares the time after cycles more with the next event, for a branch to the slow path on GE.
static void cmnCycles(struct Emitter* e, int32_t cycles) {
	aCmpI(e, R_CYC, -cycles, R12);
}

// Goes to the slow path unless the access ends before the next event: cycles is fixed, plus the
// wait table entry at tableOffset when there is one. Uses r0.
static void emitGuard(struct Emitter* e, int cycles, int32_t tableOffset, uint8_t** toSlow, int* slowCount) {
	if (tableOffset < 0) {
		cmnCycles(e, cycles);
		toSlow[(*slowCount)++] = aB(e, C_GE, NULL);
		return;
	}
	ldrbF(e, R0, tableOffset);
	aDpR(e, C_AL, D_ADD, false, R0, R0, R_CYC, S_LSL, 0);
	aDpI(e, C_AL, D_ADD, true, R0, R0, cycles);
	toSlow[(*slowCount)++] = aB(e, C_PL, NULL);
}

// Tells the renderer about a store into VRAM at offset r0 that changes something, by page: compares
// the size bytes there with r2 and stores them if they differ. Uses r1, r3 and r12.
static void emitVramStore(struct Emitter* e, int size) {
	fieldAddr(e, R3, OFF(vram));
	if (size == 4) {
		aMemR(e, true, false, R12, R3, R0, 0);
		aDpR(e, C_AL, D_CMP, true, 0, R12, R2, S_LSL, 0);
	} else {
		aMemHR(e, true, H_HALF, R12, R3, R0);
		aDpR(e, C_AL, D_EOR, false, R1, R12, R2, S_LSL, 0);
		aDpR(e, C_AL, D_MOV, true, R1, 0, R1, S_LSL, 16);
	}
	uint8_t* same = aB(e, C_EQ, NULL);
	if (size == 4) {
		aMemR(e, false, false, R2, R3, R0, 0);
	} else {
		aMemHR(e, false, H_HALF, R2, R3, R0);
	}
	aDpR(e, C_AL, D_MOV, false, R0, 0, R0, S_LSR, 11);
	fieldAddr(e, R3, OFF(renderer.vramChanged));
	aMovI(e, R1, 1);
	aMemR(e, false, true, R1, R3, R0, 0);
	patchHere(e, same);
}

// VRAM below the mirrors; r1 = address.
static void emitVramAccess(struct Emitter* e, int kind, uint8_t** toSlow, int* slowCount) {
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	int cycles = (size == 4 ? 2 : 1) + (store ? 0 : 1);
	// While the display keeps the CPU out of VRAM, C works out the wait.
	ldrbF(e, R0, OFF(vramStall));
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
	toSlow[(*slowCount)++] = aB(e, C_NE, NULL);
	if (store) {
		// Once a line's H-blank has come, C draws the line first.
		timeUntil(e, R0, R3, cycles, OFF(video.drawDue));
		toSlow[(*slowCount)++] = aB(e, C_PL, NULL);
	}
	aUbfx(e, R0, R1, 0, 17);
	aDpI(e, C_AL, D_CMP, true, 0, R0, SIZE_VRAM);
	toSlow[(*slowCount)++] = aB(e, C_HS, NULL);
	if (store) {
		ldrbF(e, R3, OFF(videoCode));
		aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
		toSlow[(*slowCount)++] = aB(e, C_NE, NULL);
	}
	cmnCycles(e, cycles);
	toSlow[(*slowCount)++] = aB(e, C_GE, NULL);
	if (size > 1) {
		aDpI(e, C_AL, D_BIC, false, R0, R0, size - 1);
	}
	if (store) {
		emitVramStore(e, size);
	} else {
		fieldAddr(e, R3, OFF(vram));
		emitLoadData(e, kind, R3);
	}
	aAddI(e, R_CYC, R_CYC, cycles, R12);
	aBx(e, LR);
}

// A byte store to VRAM, as memoryStore8 does it: into both halves of its halfword below the sprite
// tiles, and nowhere among them. r1 = address, r2 = value.
static void emitVramByteStore(struct Emitter* e, uint8_t** toSlow, int* slowCount) {
	ldrbF(e, R0, OFF(vramStall));
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
	toSlow[(*slowCount)++] = aB(e, C_NE, NULL);
	// Once a line's H-blank has come, C draws the line first.
	timeUntil(e, R0, R3, 1, OFF(video.drawDue));
	toSlow[(*slowCount)++] = aB(e, C_PL, NULL);
	ldrbF(e, R0, OFF(videoCode));
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
	toSlow[(*slowCount)++] = aB(e, C_NE, NULL);
	cmnCycles(e, 1);
	toSlow[(*slowCount)++] = aB(e, C_GE, NULL);
	// The sprite tiles start at 0x10000, or 0x14000 in the bitmap modes.
	aUbfx(e, R0, R1, 0, 17);
	fieldAccess(e, true, 2, R1, OFF(io));
	aDpI(e, C_AL, D_AND, false, R1, R1, 7);
	aDpI(e, C_AL, D_CMP, true, 0, R1, 3);
	uint8_t* tiles = aB(e, C_LO, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0x14000);
	uint8_t* ignoredBitmap = aB(e, C_HS, NULL);
	uint8_t* store = aB(e, C_AL, NULL);
	patchHere(e, tiles);
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0x10000);
	uint8_t* ignored = aB(e, C_HS, NULL);
	patchHere(e, store);
	aDpI(e, C_AL, D_BIC, false, R0, R0, 1);
	aExtend(e, X_UXTB, R2, R2);
	aDpR(e, C_AL, D_ORR, false, R2, R2, R2, S_LSL, 8);
	emitVramStore(e, 2);
	patchHere(e, ignored);
	patchHere(e, ignoredBitmap);
	aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, 1);
	aBx(e, LR);
}

// The offset of an access in a memory of 1 << bits bytes, aligned to its size: r0 from r1.
static void emitOffset(struct Emitter* e, int bits, int size) {
	aUbfx(e, R0, R1, 0, bits);
	if (size > 1) {
		aDpI(e, C_AL, D_BIC, false, R0, R0, size - 1);
	}
}

// romCode is set for cartridge code with the prefetch buffer, whose data accesses to the cartridge
// stop it.
static uint8_t* emitMemRoutine(struct Jit* jit, struct Emitter* e, int kind, bool romCode) {
	(void) jit;
	uint8_t* start = e->p;
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	int32_t waitN = size == 4 ? OFF(waitN32) : OFF(waitN16);
	// The access and a load's internal cycle.
	int fixed = store ? 1 : 2;
	uint8_t* toSlow[24];
	int slowCount = 0;

	aDpR(e, C_AL, D_MOV, false, R0, 0, R1, S_LSR, 24);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_IWRAM);
	uint8_t* toIwram = aB(e, C_EQ, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_EWRAM);
	uint8_t* toEwram = aB(e, C_EQ, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_VRAM);
	uint8_t* toVram = aB(e, C_EQ, NULL);
	uint8_t* toIo = NULL;
	if (kind == MEM_LOAD16 || kind == MEM_LOADS16) {
		aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_IO);
		toIo = aB(e, C_EQ, NULL);
	}
	uint8_t* toRom = NULL;
	if (!store) {
		aDpI(e, C_AL, D_SUB, false, R0, R0, REGION_ROM0);
		aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_ROM2_EX - REGION_ROM0);
		toRom = aB(e, C_LS, NULL);
	}
	toSlow[slowCount++] = aB(e, C_AL, NULL);

	for (int r = 0; r < 2; ++r) {
		bool iwram = r == 0;
		patchHere(e, iwram ? toIwram : toEwram);
		int bits = iwram ? 15 : 18;
		int32_t data = iwram ? OFF(iwram) : OFF(ewram);
		int32_t map = iwram ? OFF(iwramCode) : OFF(ewramCode);
		int32_t table = iwram ? -1 : waitN + REGION_EWRAM;
		emitGuard(e, fixed, table, toSlow, &slowCount);
		emitOffset(e, bits, size);
		fieldAddr(e, R3, data);
		if (store) {
			switch (size) {
			case 4:
				aMemR(e, false, false, R2, R3, R0, 0);
				break;
			case 2:
				aMemHR(e, false, H_HALF, R2, R3, R0);
				break;
			default:
				aMemR(e, false, true, R2, R3, R0, 0);
				break;
			}
			aDpR(e, C_AL, D_MOV, false, R0, 0, R0, S_LSR, 2);
			fieldAddr(e, R3, map);
			aMemR(e, true, true, R3, R3, R0, 0);
			aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
			uint8_t* clean = aB(e, C_EQ, NULL);
			// Code that checks the word when it runs only needs the helper for a store into the two
			// instructions after it (its r15 is in the state), which are already prefetched.
			aDpI(e, C_AL, D_CMP, true, 0, R3, CODE_MAP_CHECKED);
			uint8_t* other = aB(e, C_NE, NULL);
			ldrF(e, R0, REG(15));
			aDpI(e, C_AL, D_SUB, false, R0, R0, 4);
			aDpI(e, C_AL, D_BIC, false, R0, R0, 3);
			aDpI(e, C_AL, D_BIC, false, R3, R1, 3);
			aDpR(e, C_AL, D_SUB, false, R3, R3, R0, S_LSL, 0);
			aDpI(e, C_AL, D_CMP, true, 0, R3, 8);
			uint8_t* away = aB(e, C_HS, NULL);
			patchHere(e, other);
			strF(e, LR, OFF(siteReturn));
			emitPreserveCall(e, jitHelperCodeWrite);
			patchHere(e, clean);
			patchHere(e, away);
		} else {
			emitLoadData(e, kind, R3);
		}
		if (iwram) {
			aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, fixed);
		} else {
			ldrbF(e, R2, table);
			aDpR(e, C_AL, D_ADD, false, R_CYC, R_CYC, R2, S_LSL, 0);
			aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, fixed);
		}
		aBx(e, LR);
	}

	if (toRom) {
		// Data from the cartridge stops the prefetch buffer. That costs nothing outside cartridge
		// code, and the buffer is never running in cartridge code compiled without it.
		patchHere(e, toRom);
		emitOffset(e, 25, size);
		ldrF(e, R3, OFF(romSize));
		aDpR(e, C_AL, D_CMP, true, 0, R0, R3, S_LSL, 0);
		toSlow[slowCount++] = aB(e, C_HS, NULL);
		// An EEPROM answers in the cartridge's last region, whatever the cartridge's size.
		aDpR(e, C_AL, D_MOV, false, R0, 0, R1, S_LSR, 24);
		aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_ROM2_EX);
		uint8_t* notTop = aB(e, C_NE, NULL);
		ldrF(e, R3, OFF(savedata.type));
		aDpI(e, C_AL, D_CMP, true, 0, R3, SAVEDATA_EEPROM);
		toSlow[slowCount++] = aB(e, C_EQ, NULL);
		aDpI(e, C_AL, D_CMP, true, 0, R3, SAVEDATA_EEPROM512);
		toSlow[slowCount++] = aB(e, C_EQ, NULL);
		patchHere(e, notTop);
		// The cycle a stop can cost, in r2.
		aMovI(e, R2, 0);
		if (romCode) {
			ldrbF(e, R3, OFF(cpu.prefetching));
			aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
			uint8_t* idle = aB(e, C_EQ, NULL);
			ldrF(e, R3, OFF(jitBase));
			aDpR(e, C_AL, D_SUB, false, R0, R_CYC, R3, S_LSL, 0);
			aDpI(e, C_AL, D_CMP, true, 0, R0, 127);
			uint8_t* far = aB(e, C_HI, NULL);
			ldrF(e, R3, OFF(prefetcher.duty));
			aDpR(e, C_AL, D_ADD, false, R0, R0, R3, S_LSL, 7);
			ldrF(e, R3, OFF(prefetcher.width));
			aDpI(e, C_AL, D_CMP, true, 0, R3, 4);
			aDpI(e, C_EQ, D_ADD, false, R0, R0, 4096);
			fieldAddr(e, R3, OFF(jitStopPenalty));
			aMemR(e, true, true, R2, R3, R0, 0);
			patchHere(e, far);
			patchHere(e, idle);
		}
		aDpR(e, C_AL, D_MOV, false, R0, 0, R1, S_LSR, 24);
		fieldAddr(e, R3, waitN);
		aMemR(e, true, true, R0, R3, R0, 0);
		aDpR(e, C_AL, D_ADD, false, R2, R2, R0, S_LSL, 0);
		aDpI(e, C_AL, D_ADD, false, R2, R2, fixed);
		aDpR(e, C_AL, D_ADD, true, R0, R2, R_CYC, S_LSL, 0);
		toSlow[slowCount++] = aB(e, C_PL, NULL);
		aMov(e, R_CYC, R0);
		aMovI(e, R3, 0);
		strbF(e, R3, OFF(cpu.prefetching));
		emitOffset(e, 25, size);
		ldrF(e, R3, OFF(rom));
		emitLoadData(e, kind, R3);
		aBx(e, LR);
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
		static const uint32_t plain[] = { REG_DISPCNT, REG_DISPSTAT, REG_VCOUNT, REG_IF, REG_KEYINPUT };
		patchHere(e, toIo);
		emitGuard(e, fixed, -1, toSlow, &slowCount);
		aUbfx(e, R0, R1, 0, 24);
		aDpI(e, C_AL, D_BIC, false, R0, R0, 1);
		uint8_t* toPlain[5];
		for (int i = 0; i < 5; ++i) {
			aCmpI(e, R0, plain[i], R3);
			toPlain[i] = aB(e, C_EQ, NULL);
		}
		toSlow[slowCount++] = aB(e, C_AL, NULL);
		patchHere(e, toPlain[3]);
		timeUntil(e, R2, R3, fixed, OFF(irq.ifStable));
		toSlow[slowCount++] = aB(e, C_PL, NULL);
		uint8_t* toLoad = aB(e, C_AL, NULL);
		patchHere(e, toPlain[1]);
		patchHere(e, toPlain[2]);
		timeUntil(e, R2, R3, fixed, OFF(video.stableUntil));
		toSlow[slowCount++] = aB(e, C_PL, NULL);
		patchHere(e, toLoad);
		patchHere(e, toPlain[0]);
		patchHere(e, toPlain[4]);
		fieldAddr(e, R3, OFF(io));
		emitLoadData(e, kind, R3);
		aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, fixed);
		aBx(e, LR);
	}

	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, toSlow[i]);
	}
	emitSlowPath(e, kind);
	return start;
}

// Blocks' bodies are kept apart from their cold code (slow paths, exits, and whole careful copies),
// so the code a game spends its time in is dense in the caches; on a Cortex-A7 or A55 with 32 KiB of
// instruction cache, that decides much of a busy game's speed. The code memory is laid out in
// stretches, each with a part for bodies and a part for cold code, so a block's cold code stays near
// its body.
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

// Emits everything shared: the entry and exit, the dispatchers, and the memory routines. They are
// emitted once and sit before the first block. Shifts by a register need no routine here: the
// host's follow ARM's rules.
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
	jit->dispatchKey = e.p;
	emitDispatchKey(jit, &e);
	uint8_t* regionUpdate = emitRegionUpdate(&e);
	jit->dispatchArm = e.p;
	emitDispatchPc(jit, &e, false, regionUpdate);
	jit->dispatchThumb = e.p;
	emitDispatchPc(jit, &e, true, regionUpdate);
	jit->dispatchBx = e.p;
	emitDispatchBx(jit, &e);
	for (int kind = 0; kind < 8; ++kind) {
		jit->memRoutines[kind] = emitMemRoutine(jit, &e, kind, false);
		jit->romMemRoutines[kind] = emitMemRoutine(jit, &e, kind, true);
	}
	jit->multiplyCarryRoutine = e.p;
	emitPreserveCall(&e, jitMultiplyCarry);
	aBx(&e, LR);
	// For ARMGBA_PERFMAP.
	static const char* const kinds[8] = { "load32", "load16", "load8", "loadS8", "loadS16", "store32", "store16", "store8" };
	jitNameCode(jit->spillRoutine, jit->reloadRoutine, "spill");
	jitNameCode(jit->reloadRoutine, (void*) jit->enter, "reload");
	jitNameCode((void*) jit->enter, jit->exitRoutine, "enter");
	jitNameCode(jit->exitRoutine, jit->dispatchKey, "exit");
	jitNameCode(jit->dispatchKey, jit->dispatchArm, "dispatch");
	jitNameCode(jit->dispatchArm, jit->memRoutines[0], "dispatchPc");
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

enum {
	CARRY_SAME,
	CARRY_REG,
};

// A shifter operand in host form: a constant, or a host register shifted by a constant or by
// another host register, as the host's own data-processing instructions take it.
enum {
	OP_IMM,
	OP_SHIFT_IMM,
	OP_SHIFT_REG,
};

struct Operand2 {
	int kind;
	uint32_t imm;
	// The guest's own rotate:imm8 field, for a constant whose rotation sets the carry; -1 if none.
	int32_t enc;
	int rm;
	int type;
	int amount;
	int rs;
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
	int32_t penaltyRow;
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

	// Where the guest flags are: the host's, r9, or both.
	bool inHost;
	bool inReg;

	uint8_t liveAfter[MAX_BLOCK_INSTS];
	bool checkpoint[MAX_BLOCK_INSTS + 1];
	int cur;
	// Cycles counted but not yet added to r10. In cartridge code with the prefetch buffer, they map
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
};

enum {
	NONE = INT32_MIN / 4,
};

// Adds two terms, passing NONE (not known when compiling) through.
static int plus(int a, int b) {
	return a == NONE || b == NONE ? NONE : a + b;
}

// Returns the larger of two terms.
static int most(int a, int b) {
	return a > b ? a : b;
}

// Whether code can store into the instructions after a store; if so, memory routines get its r15.
static bool ramCode(const struct Compiler* c) {
	return c->region == REGION_IWRAM || c->region == REGION_EWRAM;
}

// The operations whose carry comes from the shifter, not from an addition.
static bool isLogical(int op) {
	switch (op) {
	case ALU_AND:
	case ALU_EOR:
	case ALU_TST:
	case ALU_TEQ:
	case ALU_ORR:
	case ALU_MOV:
	case ALU_BIC:
	case ALU_MVN:
		return true;
	default:
		return false;
	}
}

// The four that only set flags.
static bool isTest(int op) {
	return op >= ALU_TST && op <= ALU_CMN;
}

// Whether a data-processing instruction is a jump.
static bool aluWritesPc(const struct Inst* in) {
	return in->kind == I_ALU && !isTest(in->op) && in->rd == 15;
}

// Whether this is the last instruction a block can hold.
static bool isTerminator(const struct Inst* in) {
	return instEndsBlock(in);
}

// Costs that don't depend on run-time values; dynamic instructions are followed by a cycle check.
static bool isDynamic(struct Compiler* c, const struct Inst* in) {
	if (in->poly) {
		return true;
	}
	if (in->cond == 0xF || jitRomLiteral(c->gba, in, NULL, NULL)) {
		return false;
	}
	switch (in->kind) {
	case I_MEM:
	case I_MUL:
	case I_MULL:
	case I_SWP:
	case I_BLOCK:
		return true;
	default:
		return isTerminator(in) || jitStepOnly(in);
	}
}

// After a data access, the next fetch is nonsequential: compiled code adds the difference to the
// access and counts every fetch as sequential.
static int nonseq(const struct Compiler* c) {
	return c->prefetch ? 0 : c->N - c->S;
}

// An internal cycle to shift by a register.
static int aluExtra(struct Compiler* c, const struct Inst* in) {
	if (in->cond != 0xF && !in->op2.isImm && in->op2.byReg) {
		return 1 + nonseq(c);
	}
	return 0;
}

// Whether a word is moved to or from the stack, which is in internal RAM unless a game is doing
// something unusual; such an access is done inline, with a check.
static bool stackAccess(const struct Compiler* c, const struct Inst* in) {
	return in->rn == 13 && in->size == 4 && in->rd != 15 && c->slowCount < MAX_BLOCK_INSTS;
}

// Whether a load or block load writes r15, and so is a jump.
static bool loadsPc(const struct Inst* in) {
	if (in->kind == I_MEM) {
		return in->load && in->rd == 15;
	}
	return in->kind == I_BLOCK && in->load && (!in->list || (in->list & 0x8000));
}

static int directBranchCost(const struct Compiler* c, uint32_t target);

// Returns the cycles of an instruction that a check before it has to cover: all except those of
// accesses and refills that check for events themselves.
static int checkedCost(struct Compiler* c, const struct Inst* in, int index) {
	if (in->cond == 0xF) {
		return c->P;
	}
	int wait;
	if (jitRomLiteral(c->gba, in, NULL, &wait)) {
		if (c->prefetch) {
			// A cycle to stop the prefetch buffer, and the next fetch is nonsequential.
			return c->P + 1 + wait + most(0, c->Ncyc - c->D);
		}
		return c->P + nonseq(c) + wait;
	}
	if (jitStepOnly(in)) {
		// It only runs in compiled code when skipped.
		return in->cond == COND_AL ? 0 : c->P;
	}
	if (jitSystem(in)) {
		// Its refill checks for events itself.
		return c->P;
	}
	switch (in->kind) {
	case I_ALU:
		return c->P + aluExtra(c, in);
	case I_MEM:
		if (stackAccess(c, in)) {
			return c->P + nonseq(c) + (in->load ? 2 : 1);
		}
		return c->P + (loadsPc(in) ? 0 : nonseq(c));
	case I_BLOCK:
		return c->P + (loadsPc(in) ? 0 : nonseq(c));
	case I_SWP:
		return c->P + nonseq(c);
	case I_MUL:
		// The maximum number of multiplier stages.
		return c->P + nonseq(c) + 4 + in->accumulate;
	case I_MULL:
		return c->P + nonseq(c) + 5 + in->accumulate;
	case I_B:
		return c->P + directBranchCost(c, in->target);
	case I_BL2:
		if (index > 0 && c->insts[index - 1].kind == I_BL1) {
			return c->P + directBranchCost(c, c->insts[index - 1].target + in->target);
		}
		return c->P;
	default:
		return c->P;
	}
}

// The flags each condition code reads.
static uint8_t condReads(int cond) {
	switch (cond) {
	case 0x0:
	case 0x1:
		return F_Z;
	case 0x2:
	case 0x3:
		return F_C;
	case 0x4:
	case 0x5:
		return F_N;
	case 0x6:
	case 0x7:
		return F_V;
	case 0x8:
	case 0x9:
		return F_C | F_Z;
	case 0xA:
	case 0xB:
		return F_N | F_V;
	case 0xC:
	case 0xD:
		return F_N | F_Z | F_V;
	default:
		return 0;
	}
}

// Whether an operand's shifter passes C through or sets it, for the flag analysis.
static int shifterCarry(const struct Inst* in) {
	const struct Operand* op = &in->op2;
	if (op->isImm) {
		// A constant read at run time may be unrotated, which passes C through.
		return op->immRotated && !in->patchable ? CARRY_REG : CARRY_SAME;
	}
	if (op->byReg) {
		return CARRY_SAME;
	}
	if (op->type == SHIFT_LSL && (!op->amount || in->patchable)) {
		return CARRY_SAME;
	}
	return CARRY_REG;
}

// The flags an instruction reads, including its condition's.
static uint8_t flagReads(const struct Inst* in) {
	uint8_t reads = in->cond != COND_AL ? condReads(in->cond) : 0;
	switch (in->kind) {
	case I_ALU:
		if (in->op == ALU_ADC || in->op == ALU_SBC || in->op == ALU_RSC) {
			reads |= F_C;
		}
		if (!in->op2.isImm && !in->op2.byReg && in->op2.type == SHIFT_ROR && !in->op2.amount) {
			reads |= F_C;
		}
		if (in->op2.byReg && in->setFlags && isLogical(in->op)) {
			reads |= F_C;
		}
		if (aluWritesPc(in)) {
			reads = F_ALL;
		}
		break;
	case I_MEM:
		if (!in->op2.isImm && in->op2.type == SHIFT_ROR && !in->op2.amount) {
			reads |= F_C;
		}
		break;
	case I_MUL:
	case I_MULL:
	case I_BL1:
		break;
	default:
		reads = F_ALL;
		break;
	}
	return reads;
}

// The flags it is sure to write. A conditional instruction may not run, so it is sure of none.
static uint8_t flagWrites(const struct Inst* in) {
	if (in->cond != COND_AL || !in->setFlags) {
		return 0;
	}
	switch (in->kind) {
	case I_ALU:
		if (aluWritesPc(in)) {
			return 0;
		}
		if (!isLogical(in->op)) {
			return F_ALL;
		}
		return F_N | F_Z | (shifterCarry(in) == CARRY_REG ? F_C : 0);
	case I_MUL:
	case I_MULL:
		return F_N | F_Z | F_C;
	default:
		return 0;
	}
}

// The flags a data-processing instruction may set when it runs: C for a logical one unless its
// operand is a plain register or an unrotated constant compiled in.
static uint8_t aluWrites(const struct Inst* in) {
	if (!isLogical(in->op)) {
		return F_ALL;
	}
	const struct Operand* op = &in->op2;
	bool keepsC = !in->patchable && (op->isImm ? !op->immRotated : !op->byReg && op->type == SHIFT_LSL && !op->amount);
	return F_N | F_Z | (keepsC ? 0 : F_C);
}

// Returns the opcode i instructions into the block, or of those prefetched past its end.
static uint32_t opcodeAt(struct Compiler* c, int i) {
	return i < c->count ? c->insts[i].opcode : c->nextOps[i - c->count];
}

// What r15 reads as in an instruction: two instructions ahead.
static uint32_t pcRead(struct Compiler* c, const struct Inst* in) {
	return in->address + (c->thumb ? 4 : 8);
}

// Where in struct GBA the word of RAM code at address is.
static int32_t codeWord(const struct Compiler* c, uint32_t address) {
	(void) c;
	return (address >> 24) == REGION_IWRAM ? OFF(iwram) + (int32_t) (address & (SIZE_IWRAM - 4)) : OFF(ewram) + (int32_t) (address & (SIZE_EWRAM - 4));
}

// Flags.

// Makes r9 hold the guest flags.
static void needRegFlags(struct Compiler* c) {
	if (!c->inReg) {
		aMrs(&c->e, R_FLAGS);
		c->inReg = true;
	}
}

// Makes the host's flags hold the guest's.
static void needHostFlags(struct Compiler* c) {
	if (!c->inHost) {
		aMsr(&c->e, R_FLAGS);
		c->inHost = true;
	}
}

// Called before emitting anything that changes the host flags.
static void clobberFlags(struct Compiler* c) {
	needRegFlags(c);
	c->inHost = false;
}

// Called after a host instruction set the guest flags.
static void flagsSet(struct Compiler* c) {
	c->inHost = true;
	c->inReg = false;
}

// Called where code joins or is entered from elsewhere: the flags are in r9.
static void canonical(struct Compiler* c) {
	c->inReg = true;
	c->inHost = false;
}

// Cycles are counted up while compiling and added to the counter in one go; pending is what has
// been counted since the last add. The other terms track what the prefetch buffer has to be told.
static void resetPending(struct Compiler* c) {
	c->pending = 0;
	c->pendB = NONE;
	c->pendC = NONE;
	c->pendD = 0;
}

// Whether only the plain count is outstanding.
static bool pendingScalar(const struct Compiler* c) {
	return c->pendB == NONE && c->pendC == NONE && c->pendD == 0;
}

// Counts cycles to add later.
static void pendAdd(struct Compiler* c, int cycles) {
	c->pending += cycles;
	c->pendB = plus(c->pendB, cycles);
}

// Sets the buffer's base to the time plus cycles.
static void pendSetBase(struct Compiler* c, int cycles) {
	c->pendC = c->pending + cycles;
	c->pendD = plus(c->pendB, cycles);
}

// A fetch from the buffer costs at least a cycle, once the read in flight is done. The buffer fills
// at most full ahead.
static void pendFetch(struct Compiler* c) {
	if (!c->prefetch) {
		pendAdd(c, c->P);
		return;
	}
	int d = c->D;
	int a = most(1 + c->pending, plus(d, c->pendC));
	int b = most(plus(1, c->pendB), plus(d, c->pendD));
	int cc = most(d - c->full + c->pending, plus(d, c->pendC));
	int dd = most(plus(d - c->full, c->pendB), plus(d, c->pendD));
	c->pending = a;
	c->pendB = b;
	c->pendC = cc;
	c->pendD = dd;
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
		if (!slot->code || slot->key == key) {
			slot->key = key;
			return slot;
		}
	}
	return NULL;
}

// Starts a routine emitted inline: emits a jump over it, patched by tinyEnd.
static uint8_t* tinyBegin(struct Compiler* c, struct TinyRoutine* slot) {
	uint8_t* over = aB(&c->e, C_AL, NULL);
	slot->code = c->e.p;
	return over;
}

// Ends a tiny routine's body emitted in passing; the code around it jumps over it.
static void tinyEnd(struct Compiler* c, uint8_t* over) {
	aBx(&c->e, LR);
	patchHere(&c->e, over);
}

// Calls the tiny routine for a slot, emitting it first if it's new; with no slot, emits the
// sequence inline.
static void callTiny(struct Compiler* c, struct TinyRoutine* slot, void (*body)(struct Compiler*)) {
	if (!slot) {
		body(c);
		return;
	}
	if (!slot->code) {
		uint8_t* over = tinyBegin(c, slot);
		body(c);
		tinyEnd(c, over);
	}
	aBl(&c->e, slot->code);
}

// Adds the prefetch buffer's terms of the pending cycles; uses r1-r3 and r12.
static void emitPendingTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	ldrF(e, R1, OFF(jitBase));
	if (c->pendC != NONE || c->pendD != 0) {
		if (c->pendC != NONE && c->pendD != NONE) {
			aAddI(e, R3, R_CYC, c->pendC, R12);
			aAddI(e, R2, R1, c->pendD, R12);
			aDpR(e, C_AL, D_CMP, true, 0, R2, R3, S_LSL, 0);
			aDpR(e, C_LT, D_MOV, false, R2, 0, R3, S_LSL, 0);
		} else if (c->pendC != NONE) {
			aAddI(e, R2, R_CYC, c->pendC, R12);
		} else {
			aAddI(e, R2, R1, c->pendD, R12);
		}
		strF(e, R2, OFF(jitBase));
	}
	if (c->pending) {
		aAddI(e, R_CYC, R_CYC, c->pending, R12);
	}
	if (c->pendB != NONE) {
		aAddI(e, R2, R1, c->pendB, R12);
		aDpR(e, C_AL, D_CMP, true, 0, R_CYC, R2, S_LSL, 0);
		aDpR(e, C_LT, D_MOV, false, R_CYC, 0, R2, S_LSL, 0);
	}
}

// Adds the cycles counted so far to the cycle counter. Uses r1-r3, r12 and lr; only the scalar case
// leaves the host flags alone.
static void flushPending(struct Compiler* c) {
	if (pendingScalar(c)) {
		if (c->pending) {
			aAddI(&c->e, R_CYC, R_CYC, c->pending, R12);
		}
		resetPending(c);
		return;
	}
	clobberFlags(c);
	callTiny(c, tinySlot(c, 1, c->pending, c->pendB, c->pendC, c->pendD), emitPendingTerms);
	resetPending(c);
}

// Jumps when the ARM condition holds (or fails, if negate). Both paths have the flags in r9.
static uint8_t* emitCondJump(struct Compiler* c, int cond, bool negate) {
	struct Emitter* e = &c->e;
	if (negate) {
		cond ^= 1;
	}
	if (c->inHost) {
		needRegFlags(c);
	} else if (cond < 8) {
		static const int bit[4] = { 30, 29, 31, 28 };
		aDpI(e, C_AL, D_TST, true, 0, R_FLAGS, 1u << bit[cond >> 1]);
		return aB(e, (cond & 1) ? C_EQ : C_NE, NULL);
	} else {
		needHostFlags(c);
	}
	// The host's condition codes are the guest's.
	return aB(e, cond, NULL);
}

// Values.

// Stores a constant to a field, using lr as scratch.
static void storeConst(struct Compiler* c, uint32_t offset, uint32_t value) {
	strFImm(&c->e, offset, value, 4, LR);
}

// Returns the host register holding guest r: its own, or scratch with the value loaded. r15 is a
// constant when compiling.
static int guestReg(struct Compiler* c, int r, const struct Inst* in, int scratch) {
	if (r == 15) {
		aMovI(&c->e, scratch, pcRead(c, in));
		return scratch;
	}
	if (hostReg[r] >= 0) {
		return hostReg[r];
	}
	ldrF(&c->e, scratch, REG(r));
	return scratch;
}

// Loads a guest register's value into a host register.
static void loadGuest(struct Compiler* c, int dst, int r, const struct Inst* in) {
	aMov(&c->e, dst, guestReg(c, r, in, dst));
}

// Stores a host register's value into a guest register. r15 lives in the CPU state, not in a host
// register.
static void storeGuest(struct Compiler* c, int r, int src) {
	if (hostReg[r] >= 0) {
		aMov(&c->e, hostReg[r], src);
	} else {
		strF(&c->e, src, REG(r));
	}
}

// Stores a constant into a guest register.
static void storeGuestImm(struct Compiler* c, int r, uint32_t value) {
	if (hostReg[r] >= 0) {
		aMovI(&c->e, hostReg[r], value);
	} else {
		storeConst(c, REG(r), value);
	}
}

// Works out the shifter operand in host form. A register not held in a host register is loaded
// into r1, and a shift register into r2; a constant or amount the game rewrites is read from the
// instruction word into r1 and r3, and becomes a shift by a register, which treats a zero amount
// as the guest does.
static struct Operand2 emitShifter(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	const struct Operand* op = &in->op2;
	struct Operand2 out = { .kind = OP_SHIFT_IMM, .enc = -1, .type = S_LSL };
	if (op->isImm && in->patchable) {
		ldrF(e, R1, codeWord(c, in->address));
		aDpR(e, C_AL, D_MOV, false, R3, 0, R1, S_LSR, 7);
		aDpI(e, C_AL, D_AND, false, R3, R3, 0x1E);
		aDpI(e, C_AL, D_AND, false, R1, R1, 0xFF);
		out.kind = OP_SHIFT_REG;
		out.rm = R1;
		out.type = S_ROR;
		out.rs = R3;
		return out;
	}
	if (op->isImm) {
		out.kind = OP_IMM;
		out.imm = op->imm;
		if (op->immRotated && !c->thumb && in->kind == I_ALU) {
			out.enc = in->opcode & 0xFFF;
		}
		return out;
	}
	if (op->byReg) {
		if (op->rm == 15) {
			aMovI(e, R1, pcRead(c, in) + 4);
			out.rm = R1;
		} else {
			out.rm = guestReg(c, op->rm, in, R1);
		}
		out.kind = OP_SHIFT_REG;
		out.type = op->type;
		out.rs = guestReg(c, op->rs, in, R2);
		return out;
	}
	out.rm = guestReg(c, op->rm, in, R1);
	if (in->patchable) {
		ldrF(e, R3, codeWord(c, in->address));
		aUbfx(e, R3, R3, 7, 5);
		out.kind = OP_SHIFT_REG;
		out.rs = R3;
		return out;
	}
	// The amount is as encoded: LSR and ASR by 0 are by 32, and ROR by 0 is RRX.
	out.type = op->type;
	out.amount = op->amount;
	return out;
}

// Whether the operand reads the carry: RRX.
static bool operandReadsCarry(const struct Operand2* op) {
	return op->kind == OP_SHIFT_IMM && op->type == S_ROR && !op->amount;
}

// rd = rn op operand, as a host instruction, setting the flags if s. A constant that doesn't
// encode, or whose encoding would set C where the guest's doesn't, goes through r3.
static void emitDp(struct Compiler* c, int op, bool s, int rd, int rn, const struct Operand2* o) {
	struct Emitter* e = &c->e;
	switch (o->kind) {
	case OP_IMM: {
		int32_t enc = o->enc;
		if (enc < 0 && (!s || !isLogical(op) || o->imm < 256)) {
			enc = immEncode(o->imm);
		}
		if (enc >= 0) {
			aDpEnc(e, C_AL, op, s, rd, rn, (uint32_t) enc);
		} else if (!s && (op == D_ADD || op == D_SUB) && immEncode(-o->imm) >= 0) {
			aDpI(e, C_AL, op == D_ADD ? D_SUB : D_ADD, false, rd, rn, -o->imm);
		} else {
			aMovI(e, R3, o->imm);
			aDpR(e, C_AL, op, s, rd, rn, R3, S_LSL, 0);
		}
		break;
	}
	case OP_SHIFT_IMM:
		aDpR(e, C_AL, op, s, rd, rn, o->rm, o->type, o->amount);
		break;
	default:
		aDpRR(e, C_AL, op, s, rd, rn, o->rm, o->type, o->rs);
		break;
	}
}

// Calls a shared routine, which changes the host flags.
static void callRoutine(struct Compiler* c, const void* routine) {
	clobberFlags(c);
	aBl(&c->e, routine);
}

// Leaves for the exact CPU unless the instructions up to the next check end before the next event.
static void emitCheck(struct Compiler* c, int index, int ahead) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	if (!pendingScalar(c)) {
		flushPending(c);
	}
	cmnCycles(e, c->pending + ahead);
	struct Stub* stub = &c->stubs[c->stubCount++];
	stub->site = aB(e, C_GE, NULL);
	stub->pc = c->insts[0].address + index * c->width;
	stub->pending = c->pending;
	stub->index = index;
}

// Calls a runtime helper with the guest state in memory, its arguments after the first in r1 and
// r2; the helper's result stays in r0.
static void callHelper(struct Compiler* c, const struct Inst* in, const void* fn) {
	struct Emitter* e = &c->e;
	int index = (in->address - c->insts[0].address) / c->width;
	storeConst(c, REG(15), in->address + 2 * c->width);
	storeConst(c, OFF(cpu.prefetch), opcodeAt(c, index + 1));
	storeConst(c, OFF(cpu.prefetch) + 4, opcodeAt(c, index + 2));
	aMov(e, R0, R_GBA);
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
	struct Link* link = &c->links[c->linkCount];
	c->linkSites[c->linkCount++] = aB(&c->e, C_AL, NULL);
	link->targetKey = blockKey(pc, thumb);
	link->from = c->block;
}

// The cartridge's six regions.
static bool inCartridge(int region) {
	return region >= REGION_ROM0 && region <= REGION_ROM2_EX;
}

// Whether compiled code refills from a branch target itself: for cartridge code, only from
// cartridge code compiled without the prefetch buffer; for other code, from anywhere it can run.
// The exact CPU refills from the rest.
static bool inlineRefill(const struct Compiler* c, uint32_t target) {
	int region = target >> 24;
	if (codeKindOf(c->gba, target) == CODE_NONE) {
		return false;
	}
	if (!inCartridge(region)) {
		return true;
	}
	if (!inCartridge(c->region)) {
		return false;
	}
	// A refill's second fetch at a 128 KiB boundary starts a new burst, which only the exact CPU
	// counts.
	if (!c->prefetch && !((target + c->width) & 0x1FFFF)) {
		return false;
	}
	// With the buffer, only to the same wait states.
	return !c->prefetch || (region - REGION_ROM0) / 2 == (c->region - REGION_ROM0) / 2;
}

// Returns a direct PC write's refill cost when compiled code counts it; through the prefetch
// buffer, the most it can take.
static int directBranchCost(const struct Compiler* c, uint32_t target) {
	if (!inlineRefill(c, target)) {
		return 0;
	}
	int region = target >> 24;
	if (c->prefetch && inCartridge(region)) {
		return most(1 + c->Ncyc + c->D, 2 * c->D);
	}
	return c->thumb ? c->gba->writePcCost16[region] : c->gba->writePcCost32[region];
}

// Where the prefetch buffer reads next, for code that leaves the cartridge or C code that refills.
static void storeHead(struct Compiler* c, uint32_t head) {
	if (c->prefetch) {
		storeConst(c, OFF(prefetcher.head), head);
	}
}

// Data or a refill from the cartridge stops the prefetch buffer: one cycle more if a read is about
// to finish. Uses r1, r2 and r12.
static void emitStopTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	ldrF(e, R1, OFF(jitBase));
	aDpR(e, C_AL, D_SUB, false, R1, R_CYC, R1, S_LSL, 0);
	aDpI(e, C_AL, D_CMP, true, 0, R1, 127);
	uint8_t* far = aB(e, C_HI, NULL);
	fieldAddr(e, R2, c->penaltyRow);
	aMemR(e, true, true, R1, R2, R1, 0);
	aDpR(e, C_AL, D_ADD, false, R_CYC, R_CYC, R1, S_LSL, 0);
	patchHere(e, far);
}

static void emitStop(struct Compiler* c) {
	clobberFlags(c);
	flushPending(c);
	callTiny(c, tinySlot(c, 3, c->penaltyRow, 0, 0, 0), emitStopTerms);
}

// Called after an access that may have stopped the buffer: the next fetch restarts it,
// nonsequential. Compiled code treats that as a fetch from the buffer whose read finishes then;
// until the fetch, the buffer stays stopped for the exact CPU. Uses r1 and r12.
static void emitMissTerms(struct Compiler* c) {
	struct Emitter* e = &c->e;
	ldrbF(e, R1, OFF(cpu.prefetching));
	aDpI(e, C_AL, D_CMP, true, 0, R1, 0);
	uint8_t* running = aB(e, C_NE, NULL);
	aMovI(e, R1, 0);
	strbF(e, R1, OFF(cpu.fetchSeq));
	aAddI(e, R1, R_CYC, c->Ncyc - c->D, R12);
	strF(e, R1, OFF(jitBase));
	patchHere(e, running);
}

static void emitMissCheck(struct Compiler* c) {
	if (!c->prefetch) {
		return;
	}
	clobberFlags(c);
	flushPending(c);
	callTiny(c, tinySlot(c, 2, c->Ncyc - c->D, 0, 0, 0), emitMissTerms);
}

// The fetch after one that may have stopped the buffer restarts it.
static void emitRestarted(struct Compiler* c) {
	aMovI(&c->e, LR, 1);
	strbF(&c->e, LR, OFF(cpu.prefetching));
	strbF(&c->e, LR, OFF(cpu.fetchSeq));
}

// In the BIOS, the word that will be on the bus after a jump out of it, which is what reads of the
// BIOS from outside return.
static void setBiosPrefetchNext(struct Compiler* c, const struct Inst* in) {
	if (c->region == REGION_BIOS) {
		storeConst(c, OFF(biosPrefetchNext), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
	}
}

// On a waiting loop's way back to its start.
static void emitIdleMark(struct Compiler* c, uint32_t to) {
	if (c->idleExit && c->cur == c->count - 1 && to == c->idleExit) {
		storeConst(c, OFF(idleMark), blockKey(c->insts[0].address, c->thumb));
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
		storeConst(c, OFF(exitPc), blockKey(target & ~1u, c->thumb));
		aBl(e, c->jit->spillRoutine);
		aMov(e, R0, R_GBA);
		aMovI(e, R1, target);
		callAbs(e, jitHelperReload);
		aBl(e, c->jit->reloadRoutine);
		ldrF(e, R0, OFF(exitPc));
		aB(e, C_AL, c->jit->dispatchKey);
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
			storeConst(c, OFF(biosPrefetch), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
		}
		storeConst(c, OFF(activeRegion), region);
		storeConst(c, OFF(fetchRegion), region);
	}
	emitLinkTo(c, target & ~1u, c->thumb);
}

// r0 = target address; the instruction's cost is already pending.
static void emitIndirect(struct Compiler* c, const struct Inst* in, uint8_t* dispatch) {
	clobberFlags(c);
	storeHead(c, in->address + 3 * c->width);
	flushPending(c);
	setBiosPrefetchNext(c, in);
	aB(&c->e, C_AL, dispatch);
}

// Compiles a data-processing instruction to the same host instruction. Flags are set only if
// something later reads them.
static void emitAlu(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	int op = in->op;
	uint8_t live = c->liveAfter[c->cur];
	bool logical = isLogical(op);
	// A write to r15 that sets flags restores the CPSR; jitSystem handles it.
	bool writesPc = aluWritesPc(in);
	bool flags = in->setFlags && (aluWrites(in) & live);
	if (!isTest(op) || flags) {
		struct Operand2 op2 = emitShifter(c, in);
		int rn = 0;
		if (op != ALU_MOV && op != ALU_MVN) {
			if (in->rn == 15) {
				aMovI(e, R0, pcRead(c, in) + (in->op2.byReg ? 4 : 0));
				rn = R0;
			} else {
				rn = guestReg(c, in->rn, in, R0);
			}
		}
		int dst = writesPc || isTest(op) || hostReg[in->rd] < 0 ? R0 : hostReg[in->rd];
		// The carry going in, and the flags a logical operation keeps.
		if (op == ALU_ADC || op == ALU_SBC || op == ALU_RSC || operandReadsCarry(&op2) || (flags && logical && (live & (F_C | F_V)))) {
			needHostFlags(c);
		}
		if (!flags && (op == ALU_MOV || op == ALU_MVN) && op2.kind == OP_IMM) {
			aMovI(e, dst, op == ALU_MVN ? ~op2.imm : op2.imm);
		} else {
			emitDp(c, op, flags, isTest(op) ? 0 : dst, rn, &op2);
		}
		if (flags) {
			flagsSet(c);
		}
		if (dst == R0 && !isTest(op) && !writesPc) {
			storeGuest(c, in->rd, R0);
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
	loadGuest(c, R0, waitReg, in);
	if (sign) {
		aDpR(e, C_AL, D_EOR, false, R0, R0, R0, S_ASR, 31);
	}
	aDpI(e, C_AL, D_ORR, false, R0, R0, 1);
	aClz(e, R0, R0);
	aDpI(e, C_AL, D_RSB, false, R0, R0, 31);
	aDpR(e, C_AL, D_ADD, false, R_CYC, R_CYC, R0, S_LSR, 3);
	aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, 1 + extra);
}

// Puts the operands where the C helper that works out a multiply's carry looks for them.
static void storeScratch(struct Compiler* c, int slot, int r, const struct Inst* in) {
	strF(&c->e, guestReg(c, r, in, R0), OFF(jitScratch) + 4 * slot);
}

// Sets the guest flags from a multiply: C from the multiplier, worked out from the operands in
// jitScratch (kind 0 for MUL, 1 for UMULL, 2 for SMULL), N from bit 31 of hi, Z from lo and hi
// together (hi alone for MUL); V is kept.
static void emitMultiplyFlags(struct Compiler* c, int kind, int lo, int hi) {
	struct Emitter* e = &c->e;
	aMovI(e, R1, kind);
	callRoutine(c, c->jit->multiplyCarryRoutine);
	aDpI(e, C_AL, D_BIC, false, R_FLAGS, R_FLAGS, 0xE0000000);
	aDpR(e, C_AL, D_ORR, false, R_FLAGS, R_FLAGS, R1, S_LSL, 29);
	aDpI(e, C_AL, D_AND, false, R1, hi, 0x80000000);
	aDpR(e, C_AL, D_ORR, false, R_FLAGS, R_FLAGS, R1, S_LSL, 0);
	if (lo >= 0) {
		aDpR(e, C_AL, D_ORR, true, R1, lo, hi, S_LSL, 0);
	} else {
		aDpI(e, C_AL, D_CMP, true, 0, hi, 0);
	}
	aDpI(e, C_EQ, D_ORR, false, R_FLAGS, R_FLAGS, 0x40000000);
	canonical(c);
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
	if (in->setFlags) {
		storeScratch(c, 0, multiplicand, in);
		storeScratch(c, 1, multiplier, in);
		if (in->accumulate) {
			storeScratch(c, 2, in->rn, in);
		} else {
			storeConst(c, OFF(jitScratch) + 8, 0);
		}
	}
	int rm = guestReg(c, in->rm, in, R1);
	int rs = guestReg(c, in->rs, in, R2);
	int dst = hostReg[in->rd] >= 0 ? hostReg[in->rd] : R0;
	if (in->accumulate) {
		aMla(e, false, dst, rm, rs, guestReg(c, in->rn, in, R3));
	} else {
		aMul(e, false, dst, rm, rs);
	}
	if (dst == R0) {
		storeGuest(c, in->rd, R0);
	}
	if (in->setFlags) {
		emitMultiplyFlags(c, 0, -1, dst);
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
	if (in->setFlags) {
		storeScratch(c, 0, in->rm, in);
		storeScratch(c, 1, in->rs, in);
		if (in->accumulate) {
			storeScratch(c, 2, in->rd, in);
			storeScratch(c, 3, in->rdHi, in);
		} else {
			storeConst(c, OFF(jitScratch) + 8, 0);
			storeConst(c, OFF(jitScratch) + 12, 0);
		}
	}
	if (in->accumulate) {
		loadGuest(c, R0, in->rd, in);
		loadGuest(c, R2, in->rdHi, in);
	}
	int rm = guestReg(c, in->rm, in, R1);
	int rs = guestReg(c, in->rs, in, R3);
	uint32_t op = in->accumulate ? (in->signedMul ? M_SMLAL : M_UMLAL) : (in->signedMul ? M_SMULL : M_UMULL);
	aMull(e, op, false, R0, R2, rm, rs);
	storeGuest(c, in->rd, R0);
	storeGuest(c, in->rdHi, R2);
	if (in->setFlags) {
		emitMultiplyFlags(c, in->signedMul ? 2 : 1, R0, R2);
	}
}

// Returns the shared routine for a kind of access. Cartridge code has its own set, which stop the
// prefetch buffer on a data access.
static uint8_t* memRoutine(const struct Compiler* c, int kind) {
	return c->prefetch ? c->jit->romMemRoutines[kind] : c->jit->memRoutines[kind];
}

// Returns the kind of memory routine an instruction needs.
static int memKind(const struct Inst* in) {
	if (!in->load) {
		return in->size == 4 ? MEM_STORE32 : in->size == 2 ? MEM_STORE16 : MEM_STORE8;
	}
	if (in->size == 4) {
		return MEM_LOAD32;
	}
	if (in->size == 2) {
		return in->signExtend ? MEM_LOADS16 : MEM_LOAD16;
	}
	return in->signExtend ? MEM_LOADS8 : MEM_LOAD8;
}

// A word access through the stack pointer, which points into IWRAM: inline when it lands there
// aligned and misses compiled code, otherwise through the memory routine out of line. r1 holds the
// address and r2 a value to store. The check before it covers the inline path's cycles.
static void emitStackAccess(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	struct SlowAccess* slow = &c->slow[c->slowCount++];
	slow->in = in;
	slow->index = c->cur;
	slow->sites[1] = NULL;
	aDpR(e, C_AL, D_MOV, false, R0, 0, R1, S_LSR, 24);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_IWRAM);
	aDpI(e, C_EQ, D_TST, true, 0, R1, 3);
	slow->sites[0] = aB(e, C_NE, NULL);
	aUbfx(e, R0, R1, 2, 13);
	if (!in->load) {
		fieldAddr(e, LR, OFF(iwramCode));
		aMemR(e, true, true, R3, LR, R0, 0);
		aDpI(e, C_AL, D_CMP, true, 0, R3, 0);
		slow->sites[1] = aB(e, C_NE, NULL);
		fieldAddr(e, R3, OFF(iwram));
		aMemR(e, false, false, R2, R3, R0, 2);
	} else {
		int dst = hostReg[in->rd] >= 0 ? hostReg[in->rd] : R0;
		fieldAddr(e, R3, OFF(iwram));
		aMemR(e, true, false, dst, R3, R0, 2);
		if (dst == R0) {
			storeGuest(c, in->rd, R0);
		}
	}
	aDpI(e, C_AL, D_ADD, false, R_CYC, R_CYC, in->load ? 2 : 1);
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
			aMovI(e, LR, 0);
			strbF(e, LR, OFF(cpu.prefetching));
			strbF(e, LR, OFF(cpu.fetchSeq));
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

	// Base in r1, offset as a constant or in r2.
	bool regOffset = !in->op2.isImm;
	int32_t offset = 0;
	if (regOffset) {
		struct Operand2 off = emitShifter(c, in);
		if (operandReadsCarry(&off)) {
			needHostFlags(c);
		}
		emitDp(c, D_MOV, false, R2, 0, &off);
	} else if (in->patchable) {
		// The game rewrites this offset and its direction, so read them from the instruction word.
		ldrF(e, R2, codeWord(c, in->address));
		aUbfx(e, R0, R2, 23, 1);
		aDpI(e, C_AL, D_SUB, false, R0, R0, 1);
		aUbfx(e, R2, R2, 0, 12);
		aDpR(e, C_AL, D_EOR, false, R2, R2, R0, S_LSL, 0);
		aDpR(e, C_AL, D_SUB, false, R2, R2, R0, S_LSL, 0);
		regOffset = true;
	} else {
		offset = in->op2.imm;
	}
	if (!in->up) {
		offset = -offset;
	}
	if (in->rn == 15) {
		uint32_t base = c->thumb ? (in->address + 4) & ~3u : in->address + 8;
		aMovI(e, R1, base);
	} else {
		loadGuest(c, R1, in->rn, in);
	}
	if (regOffset && !in->up && !in->patchable) {
		aDpI(e, C_AL, D_RSB, false, R2, R2, 0);
	}
	if (in->pre) {
		if (regOffset) {
			aDpR(e, C_AL, D_ADD, false, R1, R1, R2, S_LSL, 0);
		} else if (offset) {
			aAddI(e, R1, R1, offset, R12);
		}
	}
	// The stored register is read before the base is written back.
	bool writeback = in->writeback && in->rn != 15;
	if (writeback) {
		if (in->pre) {
			aMov(e, R0, R1);
		} else if (regOffset) {
			aDpR(e, C_AL, D_ADD, false, R0, R1, R2, S_LSL, 0);
		} else {
			aAddI(e, R0, R1, offset, R12);
		}
	}
	if (!in->load) {
		if (in->rd == 15) {
			aMovI(e, R2, in->address + 12);
		} else {
			loadGuest(c, R2, in->rd, in);
		}
	}
	if (writeback) {
		storeGuest(c, in->rn, R0);
	}
	clobberFlags(c);
	if (stackAccess(c, in)) {
		emitStackAccess(c, in);
		return;
	}
	if (!in->load && ramCode(c)) {
		storeConst(c, REG(15), in->address + 2 * c->width);
	}
	aBl(e, memRoutine(c, memKind(in)));
	jitRecordSite(c->block, e->p - c->block->entry, in->address, opcodeAt(c, c->cur + 1), opcodeAt(c, c->cur + 2));
	if (in->load) {
		if (in->rd == 15) {
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
			return;
		}
		storeGuest(c, in->rd, R0);
	}
	pendAdd(c, nonseq(c));
	emitMissCheck(c);
}

// One RAM region's copy of the inline transfer; r1 = first address. Jumps to slow when a store
// would hit compiled code, or an event would fall inside it.
static void emitRamTransfer(struct Compiler* c, const struct Inst* in, int region, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	bool iwram = region == REGION_IWRAM;
	int bits = iwram ? 15 : 18;
	int32_t data = iwram ? OFF(iwram) : OFF(ewram);
	int32_t map = iwram ? OFF(iwramCode) : OFF(ewramCode);
	int count = __builtin_popcount(in->list);
	// The same wait for every word, nonsequential or not, and a load's internal cycle.
	struct GBA* gba = c->jit->gba;
	int cycles = count * (1 + gba->waitS32[region]) + (in->load ? 1 : 0);
	cmnCycles(e, cycles);
	slow[(*slowCount)++] = aB(e, C_GE, NULL);
	if (!in->load) {
		fieldAddr(e, R3, map);
		for (int k = 0; k < count; ++k) {
			aAddI(e, R0, R1, 4 * k, R12);
			aUbfx(e, R0, R0, 2, bits - 2);
			aMemR(e, true, true, R0, R3, R0, 0);
			aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
			slow[(*slowCount)++] = aB(e, C_NE, NULL);
		}
	}
	fieldAddr(e, R3, data);
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (!(in->list & (1 << r))) {
			continue;
		}
		aAddI(e, R0, R1, 4 * k, R12);
		aUbfx(e, R0, R0, 2, bits - 2);
		if (!in->load) {
			aMemR(e, false, false, guestReg(c, r, in, R2), R3, R0, 2);
		} else if (r == 15) {
			aMemR(e, true, false, R0, R3, R0, 2);
		} else if (hostReg[r] >= 0) {
			aMemR(e, true, false, hostReg[r], R3, R0, 2);
		} else {
			aMemR(e, true, false, R2, R3, R0, 2);
			storeGuest(c, r, R2);
		}
		++k;
	}
	if (in->writeback && (!in->load || !(in->list & (1 << in->rn)))) {
		int32_t start = in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
		aAddI(e, R2, R1, (in->up ? 4 * count : -4 * count) - start, R12);
		storeGuest(c, in->rn, R2);
	}
	aAddI(e, R_CYC, R_CYC, cycles + (loadsPc(in) ? 0 : nonseq(c)), R12);
}

// Pushes, pops and copies within IWRAM or EWRAM run inline; anything else goes to the exact CPU.
static void emitFastTransfer(struct Compiler* c, const struct Inst* in, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	int count = __builtin_popcount(in->list);
	int32_t start = in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
	clobberFlags(c);
	loadGuest(c, R1, in->rn, in);
	if (start) {
		aAddI(e, R1, R1, start, R12);
	}
	aDpI(e, C_AL, D_TST, true, 0, R1, 3);
	slow[(*slowCount)++] = aB(e, C_NE, NULL);
	aDpI(e, C_AL, D_ADD, false, R2, R1, 4 * count - 4);
	aDpR(e, C_AL, D_EOR, false, R2, R2, R1, S_LSL, 0);
	aDpR(e, C_AL, D_MOV, true, R2, 0, R2, S_LSR, 24);
	slow[(*slowCount)++] = aB(e, C_NE, NULL);
	aDpR(e, C_AL, D_MOV, false, R0, 0, R1, S_LSR, 24);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_EWRAM);
	uint8_t* ewram = aB(e, C_EQ, NULL);
	aDpI(e, C_AL, D_CMP, true, 0, R0, REGION_IWRAM);
	slow[(*slowCount)++] = aB(e, C_NE, NULL);
	bool toPc = in->load && (in->list & 0x8000);
	for (int region = REGION_IWRAM; region >= REGION_EWRAM; --region) {
		if (region == REGION_EWRAM) {
			patchHere(e, ewram);
		}
		emitRamTransfer(c, in, region, slow, slowCount);
		if (toPc) {
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
		} else {
			c->fastDone[c->fastDoneCount++] = aB(e, C_AL, NULL);
		}
	}
}

// Calls the exact CPU for the instruction, with its fetch counted; r0 = the next block key, or ~0.
static void emitExecute(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	aMovI(e, R1, in->opcode);
	aMovI(e, R2, in->address | c->thumb);
	callHelper(c, in, jitHelperExecute);
}

// Compiles LDM and STM. Transfers to work RAM are done inline; anything else goes to the exact CPU.
static void emitBlockTransfer(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	uint8_t* slow[40];
	int slowCount = 0;
	c->fastDoneCount = 0;
	// The fetch comes first, so I/O sees the time it happens at.
	clobberFlags(c);
	flushPending(c);
	if (!in->userBank && in->list && in->rn != 15 && (in->load || !(in->list & 0x8000)) && !storesMovedBase(in)) {
		emitFastTransfer(c, in, slow, &slowCount);
		for (int i = 0; i < slowCount; ++i) {
			patchHere(e, slow[i]);
		}
	}
	emitExecute(c, in);
	if (loadsPc(in)) {
		setBiosPrefetchNext(c, in);
		aB(e, C_AL, c->jit->dispatchKey);
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
	clobberFlags(c);
	if (in->spsr) {
		ldrF(e, R0, OFF(cpu.spsr));
	} else {
		aBl(e, c->jit->spillRoutine);
		ldrF(e, R0, OFF(cpu.cpsr));
	}
	storeGuest(c, in->rd, R0);
}

// Leaves for the exact CPU to run the instruction from here, as a careful copy's stub does for the
// instruction an event falls in; compiled code continues where it can.
static void emitStepExit(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	flushPending(c);
	storeHead(c, in->address + 2 * c->width);
	aBl(e, c->jit->spillRoutine);
	aMov(e, R0, R_GBA);
	aMovI(e, R1, in->address);
	callAbs(e, jitHelperStepOne);
	aBl(e, c->jit->reloadRoutine);
	aDpI(e, C_AL, D_CMN, true, 0, R0, 1);
	uint8_t* leave = aB(e, C_EQ, NULL);
	aB(e, C_AL, c->jit->dispatchKey);
	patchHere(e, leave);
	storeConst(c, OFF(exitReason), EXIT_EXACT);
	aB(e, C_AL, c->jit->exitRoutine);
	canonical(c);
}

// An instruction that writes the CPSR, run by the exact CPU. Compiled code continues after it, or
// wherever it went, unless the exact CPU has to take over.
static void emitSystem(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	bool writesPc = in->kind != I_MSR && !(in->kind == I_ALU && isTest(in->op));
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	aMovI(e, R1, in->opcode);
	aMovI(e, R2, in->address | c->thumb);
	callHelper(c, in, jitHelperSystem);
	aCmpI(e, R0, (int32_t) JIT_EXACT, R3);
	uint8_t* exact = aB(e, C_EQ, NULL);
	if (writesPc) {
		setBiosPrefetchNext(c, in);
		aB(e, C_AL, c->jit->dispatchKey);
	} else {
		emitLinkTo(c, in->address + c->width, c->thumb);
	}
	patchHere(e, exact);
	storeConst(c, OFF(exitReason), EXIT_EXACT);
	aB(e, C_AL, c->jit->exitRoutine);
}

// BX to a register.
static void emitBx(struct Compiler* c, const struct Inst* in) {
	loadGuest(c, R0, in->rm, in);
	emitIndirect(c, in, c->jit->dispatchBx);
}

// The branches: a known target is a linked jump, and BL also sets the return address.
static void emitBranch(struct Compiler* c, const struct Inst* in, int index) {
	struct Emitter* e = &c->e;
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
		loadGuest(c, R0, 14, in);
		aAddI(e, R0, R0, (int32_t) in->target, R12);
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
	// Both paths continue from the flags in r9 (and the host's, if they were there).
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
	ldrF(e, R0, codeWord(c, in->address));
	uint8_t* to[MAX_FORMS];
	for (int i = 0; i < poly->count; ++i) {
		aMovI(e, R1, poly->forms[i].opcode);
		if (poly->masks[i] == ~0u) {
			aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
		} else {
			aDpR(e, C_AL, D_EOR, false, R2, R0, R1, S_LSL, 0);
			aMovI(e, R3, poly->masks[i]);
			aDpR(e, C_AL, D_TST, true, 0, R2, R3, S_LSL, 0);
		}
		to[i] = aB(e, C_EQ, NULL);
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
		join[joins++] = aB(e, C_AL, NULL);
	}
	for (int i = 0; i < joins; ++i) {
		patchHere(e, join[i]);
	}
	canonical(c);
}

// Returns what a check before instruction index covers: up to the next check, which comes after the
// next dynamic instruction.
static int ahead(struct Compiler* c, int index) {
	// After an access that stopped the prefetch buffer, the first fetch is nonsequential.
	int cycles = c->prefetch ? most(0, c->Ncyc - c->D) : 0;
	for (int j = index; j < c->count; ++j) {
		const struct Inst* in = &c->insts[j];
		if (in->poly) {
			int most = 0;
			for (int i = 0; i < in->poly->count; ++i) {
				int cost = checkedCost(c, &in->poly->forms[i], j);
				most = cost > most ? cost : most;
			}
			cycles += most;
		} else {
			cycles += checkedCost(c, in, j);
		}
		if (isDynamic(c, in)) {
			break;
		}
	}
	return cycles;
}

// One instruction's cycles that a check before it covers.
static int ownCost(struct Compiler* c, int index) {
	const struct Inst* in = &c->insts[index];
	int cycles = c->prefetch ? most(0, c->Ncyc - c->D) : 0;
	if (!in->poly) {
		return cycles + checkedCost(c, in, index);
	}
	int most_ = 0;
	for (int i = 0; i < in->poly->count; ++i) {
		most_ = most(most_, checkedCost(c, &in->poly->forms[i], index));
	}
	return cycles + most_;
}

// Whether events inside an instruction can run once it is done: after it in the copy, or where it
// goes, at the check there (or in the run loop) before anything else runs.
static bool deferrable(const struct Compiler* c, int index) {
	const struct Inst* in = &c->insts[index];
	// Not past a CPSR write: an IRQ an event asks for would then see the mask from after it, not
	// before.
	return !in->poly && !jitStepOnly(in) && (!jitSystem(in) || (in->kind == I_MSR && in->spsr)) && in->cond != 0xF;
}

// Whether a deferral is taken up straight after the instruction, in the copy.
static bool fallsThrough(const struct Compiler* c, int index) {
	return index + 1 < c->count && !isTerminator(&c->insts[index]);
}

// Emits one instruction with what surrounds it: the entry point a careful copy needs before it, its
// condition's test, and the cycle counting after it.
static void emitInst(struct Compiler* c, int index) {
	const struct Inst* in = &c->insts[index];
	c->cur = index;
	if (c->careful) {
		// Enters here between instructions, with the flags in r9 and nothing pending.
		c->block->entries[index] = c->e.p;
		int cost = ownCost(c, index);
		emitCheck(c, index, cost);
		struct Stub* stub = &c->stubs[c->stubCount - 1];
		stub->cost = cost;
		stub->resume = c->e.p;
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
			ldrbF(&c->e, R3, OFF(jitDeferred));
			aDpI(&c->e, C_AL, D_CMP, true, 0, R3, 0);
			c->deferred[c->deferredCount].site = aB(&c->e, C_NE, NULL);
			c->deferred[c->deferredCount++].index = index;
		}
	}
}

// What any of a rewritten word's forms read.
static uint8_t polyReads(const struct Inst* in) {
	uint8_t reads = 0;
	for (int i = 0; i < in->poly->count; ++i) {
		reads |= flagReads(&in->poly->forms[i]);
	}
	return reads;
}

// The flags every form of a rewritten instruction writes.
static uint8_t polyWrites(const struct Inst* in) {
	uint8_t writes = F_ALL;
	for (int i = 0; i < in->poly->count; ++i) {
		writes &= flagWrites(&in->poly->forms[i]);
	}
	return writes;
}

// A pass over the block before emitting: works out which flags are live after each instruction, so
// only those are computed, and where the state has to be complete.
static void analyze(struct Compiler* c) {
	// A careful copy can hand any instruction to the exact CPU, which sees all the flags.
	for (int i = 0; i < c->count; ++i) {
		c->checkpoint[i] = c->careful || i == 0 || isDynamic(c, &c->insts[i - 1]);
	}
	uint8_t live = F_ALL;
	for (int i = c->count - 1; i >= 0; --i) {
		c->liveAfter[i] = live;
		const struct Inst* in = &c->insts[i];
		uint8_t reads = flagReads(in);
		uint8_t writes = flagWrites(in);
		if (in->poly) {
			reads = polyReads(in);
			writes = polyWrites(in);
		}
		live = reads | (live & ~writes);
		if (c->checkpoint[i]) {
			live = F_ALL;
		}
	}
}

// Whether there is a lone branch at address back to start, in the cartridge, which doesn't change.
static bool branchesBack(struct Compiler* c, uint32_t address, uint32_t start) {
	if (codeKindOf(c->gba, address) != CODE_ROM) {
		return false;
	}
	struct Inst in;
	if (c->thumb) {
		decodeThumb(&in, address, codeFetch16(c->gba, address));
	} else {
		decodeArm(&in, address, codeFetch32(c->gba, address));
	}
	return in.kind == I_B && !in.link && in.cond == COND_AL && in.target == start;
}

// A known register shifted left or right by a constant, or a constant.
static bool knownOperand(const struct Operand* op, uint16_t known, const uint32_t* value, uint32_t* out) {
	if (op->isImm) {
		*out = op->imm;
		return true;
	}
	if (op->byReg || !((known >> op->rm) & 1)) {
		return false;
	}
	if (op->type == SHIFT_LSL) {
		*out = value[op->rm] << op->amount;
		return true;
	}
	if (op->type == SHIFT_LSR && op->amount) {
		*out = value[op->rm] >> op->amount;
		return true;
	}
	return false;
}

// Finds a loop waiting on memory: it goes from the block's start round to it again, through the
// last instruction to the start or to a lone branch back there. It stores nothing, loads only from
// fixed addresses (in IWRAM and EWRAM, the cartridge's literals, or through a register it never
// changes), and each register and flag it reads is either set first that time round or never set.
// Each time round then goes like the one before, as long as no event comes. Returns where the way
// back leaves the block, or 0.
static uint32_t idleExit(struct Compiler* c) {
	const struct Inst* last = &c->insts[c->count - 1];
	uint32_t start = c->insts[0].address;
	if (c->careful || c->block->polymorphic || c->block->patchable || c->region == REGION_BIOS || last->kind != I_B || last->link) {
		return 0;
	}
	uint32_t exit;
	if (last->target == start || branchesBack(c, last->target, start)) {
		exit = last->target;
	} else if (last->cond != COND_AL && branchesBack(c, last->address + c->width, start)) {
		exit = last->address + c->width;
	} else {
		return 0;
	}
	uint16_t written = 0;
	uint8_t flagsWritten = 0;
	for (int i = 0; i < c->count - 1; ++i) {
		const struct Inst* in = &c->insts[i];
		if (in->kind == I_MEM || (in->kind == I_ALU && !isTest(in->op))) {
			written |= 1 << in->rd;
		}
		if (in->setFlags) {
			flagsWritten = F_ALL;
		}
	}
	uint16_t set = 0;
	uint16_t known = 0;
	uint32_t value[16];
	uint8_t flagsSet = 0;
	for (int i = 0; i < c->count; ++i) {
		const struct Inst* in = &c->insts[i];
		if (flagReads(in) & flagsWritten & ~flagsSet) {
			return 0;
		}
		if (i == c->count - 1) {
			break;
		}
		uint16_t reads = 0;
		bool isKnown = false;
		uint32_t result = 0;
		uint32_t operand;
		if (in->kind == I_ALU && !aluWritesPc(in)) {
			if (in->op != ALU_MOV && in->op != ALU_MVN) {
				reads |= 1 << in->rn;
			}
			if (!in->op2.isImm) {
				reads |= 1 << in->op2.rm;
				if (in->op2.byReg) {
					reads |= 1 << in->op2.rs;
				}
			}
			if (knownOperand(&in->op2, known, value, &operand)) {
				bool base = (known >> in->rn) & 1;
				isKnown = true;
				switch (in->op) {
				case ALU_MOV:
					result = operand;
					break;
				case ALU_ADD:
					result = value[in->rn] + operand;
					isKnown = base;
					break;
				case ALU_SUB:
					result = value[in->rn] - operand;
					isKnown = base;
					break;
				case ALU_ORR:
					result = value[in->rn] | operand;
					isKnown = base;
					break;
				default:
					isKnown = false;
					break;
				}
			}
		} else if (in->kind == I_MEM && in->load && !in->writeback && in->pre && in->rd != 15) {
			if (jitRomLiteral(c->gba, in, &result, NULL)) {
				isKnown = true;
			} else {
				reads |= 1 << in->rn;
				if (!in->op2.isImm) {
					reads |= 1 << in->op2.rm;
				}
				if (!knownOperand(&in->op2, known, value, &operand)) {
					return 0;
				}
				int32_t offset = in->up ? operand : -operand;
				if ((known >> in->rn) & 1) {
					uint32_t address = value[in->rn] + offset;
					if ((address >> 24) != REGION_IWRAM && (address >> 24) != REGION_EWRAM) {
						return 0;
					}
				} else if (!((written >> in->rn) & 1) && in->rn != 15 && c->idleLoadCount < 4) {
					c->idleLoads[c->idleLoadCount].base = in->rn;
					c->idleLoads[c->idleLoadCount++].offset = offset;
				} else {
					return 0;
				}
			}
		} else {
			return 0;
		}
		if ((reads & (1 << 15)) || (reads & written & ~set)) {
			return 0;
		}
		if (in->kind == I_MEM || !isTest(in->op)) {
			known &= ~(1 << in->rd);
			if (in->cond == COND_AL) {
				set |= 1 << in->rd;
				known |= isKnown << in->rd;
				value[in->rd] = result;
			}
		}
		flagsSet |= flagWrites(in);
	}
	return exit;
}

// The whole times round of lap cycles that fit in budget, in cycles.
static uint32_t wholeLaps(uint32_t budget, uint32_t lap) {
	return budget / lap * lap;
}

// At a waiting loop's start: when the last time round came back the same way, with no event run and
// the prefetch buffer as it was, the times round that end before the next event are skipped at
// once. For a loop reading DISPSTAT or VCOUNT, only those that end before they change, if they
// didn't change last time round.
static void emitIdleSkip(struct Compiler* c) {
	struct Emitter* e = &c->e;
	uint8_t* again[16];
	int count = 0;
	ldrF(e, R0, OFF(idleMark));
	aMovI(e, R1, blockKey(c->insts[0].address, c->thumb));
	aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
	again[count++] = aB(e, C_NE, NULL);
	ldrF(e, R0, OFF(eventRuns));
	ldrF(e, R1, OFF(idleRuns));
	aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
	again[count++] = aB(e, C_NE, NULL);
	ldrF(e, R0, OFF(cpu.nextEvent));
	ldrF(e, R1, OFF(idleNext));
	aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
	again[count++] = aB(e, C_NE, NULL);
	if (c->prefetch) {
		ldrF(e, R0, OFF(jitBase));
		aDpR(e, C_AL, D_SUB, false, R0, R0, R_CYC, S_LSL, 0);
		ldrF(e, R1, OFF(idleLead));
		aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
		again[count++] = aB(e, C_NE, NULL);
	}
	// r1 = one time round, r0 = the cycles before the next event minus one.
	ldrF(e, R1, OFF(idleCycles));
	aDpR(e, C_AL, D_SUB, true, R1, R_CYC, R1, S_LSL, 0);
	again[count++] = aB(e, C_LE, NULL);
	aDpR(e, C_AL, D_MVN, false, R0, 0, R_CYC, S_LSL, 0);
	for (int i = 0; i < c->idleLoadCount; ++i) {
		int base = c->idleLoads[i].base;
		loadGuest(c, R2, base, c->insts);
		aAddI(e, R2, R2, c->idleLoads[i].offset, R12);
		aDpI(e, C_AL, D_CMP, true, 0, R2, REGION_EWRAM << 24);
		uint8_t* low = aB(e, C_LO, NULL);
		aDpI(e, C_AL, D_CMP, true, 0, R2, (REGION_IWRAM + 1) << 24);
		uint8_t* ram = aB(e, C_LO, NULL);
		patchHere(e, low);
		aDpI(e, C_AL, D_BIC, false, R2, R2, 3);
		aCmpI(e, R2, (REGION_IO << 24) | REG_DISPSTAT, R3);
		again[count++] = aB(e, C_NE, NULL);
		ldrF(e, R2, OFF(video.stableUntil));
		ldrF(e, R3, OFF(idleStable));
		aDpR(e, C_AL, D_CMP, true, 0, R2, R3, S_LSL, 0);
		again[count++] = aB(e, C_NE, NULL);
		ldrF(e, R3, OFF(timing.masterCycles));
		aDpR(e, C_AL, D_SUB, false, R2, R2, R3, S_LSL, 0);
		ldrF(e, R3, OFF(cpu.nextEvent));
		aDpR(e, C_AL, D_SUB, false, R2, R2, R3, S_LSL, 0);
		aDpR(e, C_AL, D_SUB, false, R2, R2, R_CYC, S_LSL, 0);
		aDpI(e, C_AL, D_SUB, false, R2, R2, 1);
		aDpR(e, C_AL, D_CMP, true, 0, R2, R0, S_LSL, 0);
		aDpR(e, C_LT, D_MOV, false, R0, 0, R2, S_LSL, 0);
		patchHere(e, ram);
	}
	aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
	again[count++] = aB(e, C_LT, NULL);
	callAbs(e, wholeLaps);
	aDpR(e, C_AL, D_ADD, false, R_CYC, R_CYC, R0, S_LSL, 0);
	if (c->prefetch) {
		ldrF(e, R1, OFF(jitBase));
		aDpR(e, C_AL, D_ADD, false, R1, R1, R0, S_LSL, 0);
		strF(e, R1, OFF(jitBase));
	}
	for (int i = 0; i < count; ++i) {
		patchHere(e, again[i]);
	}
	storeConst(c, OFF(idleMark), 0);
	strF(e, R_CYC, OFF(idleCycles));
	ldrF(e, R0, OFF(eventRuns));
	strF(e, R0, OFF(idleRuns));
	ldrF(e, R0, OFF(cpu.nextEvent));
	strF(e, R0, OFF(idleNext));
	if (c->prefetch) {
		ldrF(e, R0, OFF(jitBase));
		aDpR(e, C_AL, D_SUB, false, R0, R0, R_CYC, S_LSL, 0);
		strF(e, R0, OFF(idleLead));
	}
	if (c->idleLoadCount) {
		ldrF(e, R0, OFF(video.stableUntil));
		strF(e, R0, OFF(idleStable));
	}
}

// An idle loop's head (jitIsIdleHead): once events have run since the run loop last looked at it,
// it leaves for the run loop to look again.
static void emitIdleHead(struct Compiler* c) {
	struct Emitter* e = &c->e;
	ldrF(e, R0, OFF(eventRuns));
	ldrF(e, R1, OFF(idleHeadRuns));
	aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
	uint8_t* same = aB(e, C_EQ, NULL);
	// As a deferred event's exit leaves things.
	uint32_t pc = c->insts[0].address;
	storeHead(c, pc + 2 * c->width);
	storeConst(c, OFF(exitPc), pc);
	storeConst(c, OFF(exitReason), EXIT_LOOKUP);
	aB(e, C_AL, c->jit->exitRoutine);
	patchHere(e, same);
}

// Compiles a block: the analysis, then each instruction, then the cold code the instructions left for
// the end (slow paths, exits and stubs). Fails if the code memory is full; the caller then flushes.
bool backendCompile(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
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
	};
	if (jit->hotEnd - jit->codeNext < HOT_ROOM || jit->coldEnd - jit->coldNext < COLD_ROOM) {
		if (jit->codeEnd - jit->coldEnd < STRETCH) {
			return false;
		}
		startStretch(jit, jit->coldEnd);
	}
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
	canonical(&c);
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
	if (!c.careful) {
		e->p = jit->coldNext;
		e->end = jit->coldEnd;
	}
	uint8_t* cold = e->p;
	block->cold = c.careful ? block->entry : cold;
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
			storeConst(&c, REG(15), in->address + 2 * c.width);
		}
		aBl(e, memRoutine(&c, memKind(in)));
		jitRecordSite(block, e->p - block->entry, in->address, opcodeAt(&c, slow->index + 1), opcodeAt(&c, slow->index + 2));
		if (in->load) {
			storeGuest(&c, in->rd, R0);
		}
		emitMissCheck(&c);
		aB(e, C_AL, slow->resume);
	}
	for (int i = 0; i < c.stubCount; ++i) {
		struct Stub* stub = &c.stubs[i];
		patchHere(e, stub->site);
		if (stub->pending) {
			aAddI(e, R_CYC, R_CYC, stub->pending, R12);
		}
		if (!block->careful) {
			// Continues in the careful copy from the same instruction, once there is one.
			struct Link* link = &c.links[c.linkCount];
			c.linkSites[c.linkCount++] = aB(e, C_AL, NULL);
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
			aMov(e, R0, R_GBA);
			aMovI(e, R1, stub->cost);
			aMovI(e, R2, stub->deferrable);
			aMovI(e, R3, (uint32_t) (uintptr_t) block);
			callAbs(e, jitHelperCareful);
			aBl(e, jit->reloadRoutine);
			aDpI(e, C_AL, D_CMP, true, 0, R0, CAREFUL_RUN);
			aB(e, C_EQ, stub->resume);
			aDpI(e, C_AL, D_CMP, true, 0, R0, CAREFUL_LEAVE);
			leaveHere = aB(e, C_EQ, NULL);
		}
		// The exact CPU runs the instruction an event falls in; compiled code continues after it,
		// here if the copy is still there.
		aBl(e, jit->spillRoutine);
		aMov(e, R0, R_GBA);
		aMovI(e, R1, stub->pc);
		callAbs(e, jitHelperStepOne);
		aBl(e, jit->reloadRoutine);
		aDpI(e, C_AL, D_CMN, true, 0, R0, 1);
		uint8_t* leave = aB(e, C_EQ, NULL);
		if (stub->index + 1 < count) {
			aMovI(e, R1, blockKey(stub->pc + c.width, c.thumb));
			aDpR(e, C_AL, D_CMP, true, 0, R0, R1, S_LSL, 0);
			uint8_t* elsewhere = aB(e, C_NE, NULL);
			aMovI(e, R1, (uint32_t) (uintptr_t) &block->valid);
			aLdrb(e, R1, R1, 0);
			aDpI(e, C_AL, D_CMP, true, 0, R1, 0);
			uint8_t* gone = aB(e, C_EQ, NULL);
			aB(e, C_AL, block->entries[stub->index + 1]);
			patchHere(e, elsewhere);
			patchHere(e, gone);
		}
		aB(e, C_AL, jit->dispatchKey);
		patchHere(e, leave);
		storeConst(&c, OFF(exitReason), EXIT_EXACT);
		aB(e, C_AL, jit->exitRoutine);
		if (leaveHere) {
			// Leaves between instructions, for the run loop.
			patchHere(e, leaveHere);
			storeConst(&c, OFF(exitPc), stub->pc);
			storeConst(&c, OFF(exitReason), EXIT_LOOKUP);
			aB(e, C_AL, jit->exitRoutine);
		}
	}
	for (int i = 0; i < c.deferredCount; ++i) {
		// The events inside the instruction just done.
		int index = c.deferred[i].index;
		uint32_t next = c.insts[0].address + (index + 1) * c.width;
		patchHere(e, c.deferred[i].site);
		storeHead(&c, next + 2 * c.width);
		aBl(e, jit->spillRoutine);
		aMov(e, R0, R_GBA);
		aMovI(e, R1, (uint32_t) (uintptr_t) block);
		callAbs(e, jitHelperDeferred);
		aBl(e, jit->reloadRoutine);
		aDpI(e, C_AL, D_CMP, true, 0, R0, 0);
		aB(e, C_EQ, block->entries[index + 1]);
		storeConst(&c, OFF(exitPc), next);
		storeConst(&c, OFF(exitReason), EXIT_LOOKUP);
		aB(e, C_AL, jit->exitRoutine);
	}
	block->outCount = c.linkCount;
	block->out = malloc(c.linkCount * sizeof(*block->out));
	memcpy(block->out, c.links, c.linkCount * sizeof(*block->out));
	for (int i = 0; i < block->outCount; ++i) {
		struct Link* link = &block->out[i];
		link->site = c.linkSites[i];
		link->stub = e->p;
		patchBranch(link->site, link->stub);
		if (inCartridge(link->targetKey >> 24)) {
			storeHead(&c, (link->targetKey & ~1u) + 2 * c.width);
		}
		storeConst(&c, OFF(exitPc), link->targetKey);
		aMovI(e, R0, (uint32_t) (uintptr_t) link);
		strF(e, R0, OFF(exitLink));
		storeConst(&c, OFF(exitReason), EXIT_LINK);
		aB(e, C_AL, jit->exitRoutine);
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
