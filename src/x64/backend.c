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
//   r15  the GBA state          r14d  cycles counted from the next event (cpu.cycles - cpu.nextEvent)
//   ebp  guest NZCV in lahf/seto form: N bit 15, Z bit 14, C bit 8 (ARM sense), V bit 0
//   rbx rsi rdi r8-r12  guest r0-r7     r13d  guest r13
//   rax rcx rdx  scratch; memory routines take the address in ecx and a store value in edx,
//   return a load in eax and clobber only these three

#define OFF(field) ((int32_t) offsetof(struct GBA, field))
#define REG(n) (OFF(cpu.r) + 4 * (n))

static const int8_t hostReg[16] = { RBX, RSI, RDI, R8, R9, R10, R11, R12, -1, -1, -1, -1, -1, R13, -1, -1 };
static const int pinned[] = { RBX, RSI, RDI, R8, R9, R10, R11, R12, R13 };
static const int pinnedGuest[] = { 0, 1, 2, 3, 4, 5, 6, 7, 13 };

// Where a guest register lives: its host register, or its slot in the CPU state.
static struct Opnd guest(int r) {
	return hostReg[r] >= 0 ? R(hostReg[r]) : M(R15, REG(r));
}

// Calls a C function anywhere in the address space.
static void callAbs(struct Emitter* e, const void* fn) {
	xMovI64(e, RAX, (uint64_t) (uintptr_t) fn);
	xCallR(e, RAX);
}

// Points the branch at site somewhere else. This is how blocks are linked and unlinked.
void backendPatchJump(uint8_t* site, const void* target) {
	patchRel32(site, target);
}

// Shared routines.

// Converts the prefetch buffer's base between jitBase and the form C code keeps.
static void storeBase(struct Emitter* e, int scratch) {
	xLoad(e, 4, scratch, M(R15, OFF(jitBase)));
	xAluRM(e, X_ADD, 4, scratch, M(R15, OFF(cpu.nextEvent)));
	xAluRM(e, X_ADD, 4, scratch, M(R15, OFF(timing.masterCycles)));
	xStore(e, 4, M(R15, OFF(prefetcher.base)), scratch);
}

// The prefetch buffer's start time, in the same terms as the cycle counter.
static void loadBase(struct Emitter* e, int scratch) {
	xLoad(e, 4, scratch, M(R15, OFF(prefetcher.base)));
	xAluRM(e, X_SUB, 4, scratch, M(R15, OFF(timing.masterCycles)));
	xAluRM(e, X_SUB, 4, scratch, M(R15, OFF(cpu.nextEvent)));
	xStore(e, 4, M(R15, OFF(jitBase)), scratch);
}

// The routine that stores the guest registers held in host registers, and the cycle counter, to the
// CPU state.
static void emitSpill(struct Emitter* e) {
	for (int i = 0; i < 9; ++i) {
		xStore(e, 4, M(R15, REG(pinnedGuest[i])), pinned[i]);
	}
	xLoad(e, 4, RDX, M(R15, OFF(cpu.nextEvent)));
	xAluRR(e, X_ADD, 4, R(RDX), R14);
	xStore(e, 4, M(R15, OFF(cpu.cycles)), RDX);
	xMovRR(e, 4, RAX, RBP);
	xAluI(e, X_AND, 4, R(RAX), 0xC000);
	xShiftI(e, X_SHL, 4, R(RAX), 16);
	xMovRR(e, 4, RDX, RBP);
	xAluI(e, X_AND, 4, R(RDX), 0x100);
	xShiftI(e, X_SHL, 4, R(RDX), 21);
	xAluRR(e, X_OR, 4, R(RAX), RDX);
	xMovRR(e, 4, RDX, RBP);
	xAluI(e, X_AND, 4, R(RDX), 1);
	xShiftI(e, X_SHL, 4, R(RDX), 28);
	xAluRR(e, X_OR, 4, R(RAX), RDX);
	xLoad(e, 4, RDX, M(R15, OFF(cpu.cpsr)));
	xAluI(e, X_AND, 4, R(RDX), 0x0FFFFFFF);
	xAluRR(e, X_OR, 4, R(RDX), RAX);
	xStore(e, 4, M(R15, OFF(cpu.cpsr)), RDX);
	storeBase(e, RAX);
	xRet(e);
}

// Preserves rax.
static void emitReload(struct Emitter* e) {
	for (int i = 0; i < 9; ++i) {
		xLoad(e, 4, pinned[i], M(R15, REG(pinnedGuest[i])));
	}
	xLoad(e, 4, R14, M(R15, OFF(cpu.cycles)));
	xAluRM(e, X_SUB, 4, R14, M(R15, OFF(cpu.nextEvent)));
	loadBase(e, RDX);
	xLoad(e, 4, RDX, M(R15, OFF(cpu.cpsr)));
	xMovRR(e, 4, RBP, RDX);
	xShiftI(e, X_SHR, 4, R(RBP), 16);
	xAluI(e, X_AND, 4, R(RBP), 0xC000);
	xMovRR(e, 4, RCX, RDX);
	xShiftI(e, X_SHR, 4, R(RCX), 21);
	xAluI(e, X_AND, 4, R(RCX), 0x100);
	xAluRR(e, X_OR, 4, R(RBP), RCX);
	xShiftI(e, X_SHR, 4, R(RDX), 28);
	xAluI(e, X_AND, 4, R(RDX), 1);
	xAluRR(e, X_OR, 4, R(RBP), RDX);
	xRet(e);
}

// The routine C calls to run compiled code: saves the host's registers, loads the guest's, and
// jumps to the given block.
static void emitEnter(struct Jit* jit, struct Emitter* e) {
	xPush(e, RBX);
	xPush(e, RBP);
	xPush(e, R12);
	xPush(e, R13);
	xPush(e, R14);
	xPush(e, R15);
	xAluI(e, X_SUB, 8, R(RSP), 8);
	xMovRR(e, 8, R15, RDI);
	xStore(e, 8, M(R15, OFF(hostStack)), RSP);
	xMovRR(e, 8, RAX, RSI);
	xCall(e, jit->reloadRoutine);
	xJmpR(e, RAX);
}

// The way back out: stores the guest registers, restores the host's, and returns the reason.
static void emitExit(struct Jit* jit, struct Emitter* e) {
	xLoad(e, 8, RSP, M(R15, OFF(hostStack)));
	xCall(e, jit->spillRoutine);
	xLoad(e, 4, RAX, M(R15, OFF(exitReason)));
	xAluI(e, X_ADD, 8, R(RSP), 8);
	xPop(e, R15);
	xPop(e, R14);
	xPop(e, R13);
	xPop(e, R12);
	xPop(e, RBP);
	xPop(e, RBX);
	xRet(e);
}

// Looks up the block for a key and jumps to it, compiling it first if needed.
static void emitDispatchKey(struct Jit* jit, struct Emitter* e) {
	xMovRR(e, 4, RDX, RAX);
	xShiftI(e, X_SHR, 4, R(RDX), 13);
	xAluRR(e, X_XOR, 4, R(RDX), RAX);
	xAluI(e, X_AND, 4, R(RDX), LOOKUP_SIZE - 1);
	xShiftI(e, X_SHL, 4, R(RDX), 4);
	xLoad(e, 8, RCX, M(R15, OFF(lookup)));
	xAluRM(e, X_CMP, 4, RAX, MI(RCX, RDX, 1, 0));
	uint8_t* miss = xJcc(e, CC_NE, NULL);
	xJmpM(e, MI(RCX, RDX, 1, 8));
	patchHere(e, miss);
	xStore(e, 4, M(R15, OFF(exitPc)), RAX);
	xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_LOOKUP);
	xJmp(e, jit->exitRoutine);
}

// memorySetActiveRegion's bookkeeping for a PC write into a region compiled code refills itself;
// edx holds the new region.
static uint8_t* emitRegionUpdate(struct Emitter* e) {
	uint8_t* start = e->p;
	xLoad(e, 4, RCX, M(R15, OFF(activeRegion)));
	xAluRR(e, X_CMP, 4, R(RCX), RDX);
	uint8_t* same = xJcc(e, CC_E, NULL);
	xTest(e, 4, R(RCX), RCX);
	uint8_t* notBios = xJcc(e, CC_NE, NULL);
	xLoad(e, 4, RCX, M(R15, OFF(biosPrefetchNext)));
	xStore(e, 4, M(R15, OFF(biosPrefetch)), RCX);
	patchHere(e, notBios);
	xStore(e, 4, M(R15, OFF(activeRegion)), RDX);
	xStore(e, 4, M(R15, OFF(fetchRegion)), RDX);
	patchHere(e, same);
	xRet(e);
	return start;
}

// busRefillCartridge's two fetches, for eax = target and edx = its region (at least 8), when
// nothing can come between them; other refills go to the exact CPU through slow.
static void emitCartridgeRefill(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate, uint8_t** slow, int* slowCount) {
	int size = thumb ? 2 : 4;
	xAluI(e, X_CMP, 4, R(RDX), REGION_ROM2_EX);
	slow[(*slowCount)++] = xJcc(e, CC_A, NULL);
	xAluI(e, X_CMP, 1, M(R15, OFF(prefetchDisabled)), 0);
	slow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	// A new burst starts at the second fetch.
	xLea(e, 4, RCX, M(RAX, size));
	xTestI(e, 4, R(RCX), 0x1FFFF);
	slow[(*slowCount)++] = xJcc(e, CC_E, NULL);
	// The cost of stopping a read in flight: prefetcherStop's table, indexed by how far into the
	// read it is.
	xMovI(e, RCX, 0);
	xAluI(e, X_CMP, 1, M(R15, OFF(cpu.prefetching)), 0);
	uint8_t* stopped = xJcc(e, CC_E, NULL);
	xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(prefetcher.head)));
	slow[(*slowCount)++] = xJcc(e, CC_E, NULL);
	xMovRR(e, 4, RDX, R14);
	xAluRM(e, X_SUB, 4, RDX, M(R15, OFF(jitBase)));
	xAluI(e, X_CMP, 4, R(RDX), 127);
	uint8_t* far = xJcc(e, CC_A, NULL);
	xLoad(e, 4, RCX, M(R15, OFF(prefetcher.duty)));
	xShiftI(e, X_SHL, 4, R(RCX), 7);
	xAluRR(e, X_ADD, 4, R(RCX), RDX);
	xAluI(e, X_CMP, 4, M(R15, OFF(prefetcher.width)), 4);
	uint8_t* narrow = xJcc(e, CC_NE, NULL);
	xAluI(e, X_ADD, 4, R(RCX), 4096);
	patchHere(e, narrow);
	xMovzx(e, RCX, 1, MI(R15, RCX, 1, OFF(jitStopPenalty)));
	uint8_t* counted = xJmp(e, NULL);
	patchHere(e, far);
	xMovI(e, RCX, 0);
	patchHere(e, stopped);
	patchHere(e, counted);
	// Then the nonsequential fetch and the sequential one.
	xMovRR(e, 4, RDX, RAX);
	xShiftI(e, X_SHR, 4, R(RDX), 24);
	xMovzx(e, RDX, 1, MI(R15, RDX, 1, thumb ? OFF(writePcCost16) : OFF(writePcCost32)));
	xAluRR(e, X_ADD, 4, R(RCX), RDX);
	xAluRR(e, X_ADD, 4, R(R14), RCX);
	uint8_t* late = xJcc(e, CC_NS, NULL);
	xStoreI(e, 1, M(R15, OFF(cpu.prefetching)), 0);
	xAluI(e, X_CMP, 1, M(R15, OFF(prefetchEnabled)), 0);
	uint8_t* off = xJcc(e, CC_E, NULL);
	// Started by the first fetch, which the second waits on.
	xStoreI(e, 1, M(R15, OFF(cpu.prefetching)), 1);
	xStoreI(e, 4, M(R15, OFF(prefetcher.width)), size);
	xMovRR(e, 4, RDX, RAX);
	xShiftI(e, X_SHR, 4, R(RDX), 24);
	xMovzx(e, RDX, 1, MI(R15, RDX, 1, thumb ? OFF(waitS16) : OFF(waitS32)));
	xLea(e, 4, RDX, M(RDX, 1));
	xStore(e, 4, M(R15, OFF(prefetcher.duty)), RDX);
	xShiftI(e, X_SHL, 4, R(RDX), thumb ? 3 : 2);
	xStore(e, 4, M(R15, OFF(prefetcher.full)), RDX);
	xLea(e, 4, RDX, M(RAX, 2 * size));
	xStore(e, 4, M(R15, OFF(prefetcher.head)), RDX);
	xStore(e, 4, M(R15, OFF(jitBase)), R14);
	patchHere(e, off);
	xLea(e, 4, RDX, M(RAX, 2 * size));
	xAluI(e, X_AND, 4, R(RDX), SIZE_ROM - 1);
	xStore(e, 4, M(R15, OFF(romNext)), RDX);
	xStore(e, 4, M(R15, OFF(romLatch)), RDX);
	xStoreI(e, 1, M(R15, OFF(cpu.fetchSeq)), 1);
	xStoreI(e, 1, M(R15, OFF(lastAccessDma)), 0);
	xStoreI(e, 4, M(R15, OFF(dmaIdleCredit)), 0);
	// After a refill, the IRQ check uses the mask as it is now.
	xTestI(e, 4, M(R15, OFF(cpu.cpsr)), PSR_I);
	xSetcc(e, CC_NE, M(R15, OFF(cpu.irqMasked)));
	xMovRR(e, 4, RDX, RAX);
	xShiftI(e, X_SHR, 4, R(RDX), 24);
	xCall(e, regionUpdate);
	if (thumb) {
		xAluI(e, X_OR, 4, R(RAX), 1);
	}
	xJmp(e, jit->dispatchKey);
	patchHere(e, late);
	xAluRR(e, X_SUB, 4, R(R14), RCX);
	slow[(*slowCount)++] = xJmp(e, NULL);
}

// Branches to eax, a target address in the given instruction set. Compiled code refills from BIOS,
// RAM, palette, VRAM and OAM itself; refills elsewhere, or ones an event would fall inside, go to
// the exact CPU. As in mGBA, an ARM-state PC keeps bit 1: fetches ignore it, but PC reads see it.
static void emitDispatchPc(struct Jit* jit, struct Emitter* e, bool thumb, uint8_t* regionUpdate) {
	xAluI(e, X_AND, 4, R(RAX), ~1);
	xMovRR(e, 4, RDX, RAX);
	xShiftI(e, X_SHR, 4, R(RDX), 24);
	xAluI(e, X_CMP, 4, R(RDX), 8);
	uint8_t* far = xJcc(e, CC_AE, NULL);
	xAluI(e, X_CMP, 4, R(RDX), 1);
	uint8_t* other = xJcc(e, CC_E, NULL);
	xAluI(e, X_CMP, 4, R(RDX), REGION_IO);
	uint8_t* io = xJcc(e, CC_E, NULL);
	xMovzx(e, RCX, 1, MI(R15, RDX, 1, thumb ? OFF(writePcCost16) : OFF(writePcCost32)));
	xAluRR(e, X_ADD, 4, R(R14), RCX);
	uint8_t* late = xJcc(e, CC_NS, NULL);
	xCall(e, regionUpdate);
	if (thumb) {
		xAluI(e, X_OR, 4, R(RAX), 1);
	}
	xJmp(e, jit->dispatchKey);
	patchHere(e, late);
	xAluRR(e, X_SUB, 4, R(R14), RCX);
	patchHere(e, far);
	uint8_t* slow[6];
	int slowCount = 0;
	emitCartridgeRefill(jit, e, thumb, regionUpdate, slow, &slowCount);
	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, slow[i]);
	}
	patchHere(e, other);
	patchHere(e, io);
	if (thumb) {
		xAluI(e, X_OR, 4, R(RAX), 1);
	}
	xStore(e, 4, M(R15, OFF(exitPc)), RAX);
	xCall(e, jit->spillRoutine);
	xMovRR(e, 8, RDI, R15);
	xLoad(e, 4, RSI, M(R15, OFF(exitPc)));
	callAbs(e, jitHelperReload);
	xCall(e, jit->reloadRoutine);
	xLoad(e, 4, RAX, M(R15, OFF(exitPc)));
	xJmp(e, jit->dispatchKey);
}

static void emitPreserveCall(struct Emitter* e, const void* fn, void (*args)(struct Emitter*), bool aligned);

// BX's target: bit 0 chooses the instruction set, the CPSR follows, then the block is looked up.
static void emitDispatchBx(struct Jit* jit, struct Emitter* e) {
	xTestI(e, 1, R(RAX), 1);
	uint8_t* toThumb = xJcc(e, CC_NE, NULL);
	xTestI(e, 4, M(R15, OFF(cpu.cpsr)), PSR_T);
	xJcc(e, CC_E, jit->dispatchArm);
	xAluI(e, X_AND, 4, M(R15, OFF(cpu.cpsr)), ~PSR_T);
	xJmp(e, jit->dispatchArm);
	patchHere(e, toThumb);
	xTestI(e, 4, M(R15, OFF(cpu.cpsr)), PSR_T);
	xJcc(e, CC_NE, jit->dispatchThumb);
	xAluI(e, X_OR, 4, M(R15, OFF(cpu.cpsr)), PSR_T);
	xJmp(e, jit->dispatchThumb);
}

// Register-specified shifts take edx = value and ecx = the shift register, and return edx = result
// and ecx = carry.
static void carryFromFlags(struct Emitter* e) {
	xMovRR(e, 4, RCX, RBP);
	xShiftI(e, X_SHR, 4, R(RCX), 8);
	xAluI(e, X_AND, 4, R(RCX), 1);
}

// Copies the host's carry flag into a register.
static void setcEcx(struct Emitter* e) {
	xSetcc(e, CC_B, R(RCX));
	xMovzx(e, RCX, 1, R(RCX));
}

// The shared routines for shifts by a register, which follow ARM's rules for amounts of 32 and
// over.
static uint8_t* emitShiftRoutine(struct Emitter* e, int type) {
	uint8_t* start = e->p;
	xAluI(e, X_AND, 4, R(RCX), 0xFF);
	uint8_t* zero = xJcc(e, CC_E, NULL);
	uint8_t* big = NULL;
	if (type == SHIFT_ROR) {
		xAluI(e, X_AND, 4, R(RCX), 0x1F);
		uint8_t* rotate = xJcc(e, CC_NE, NULL);
		xMovRR(e, 4, RCX, RDX);
		xShiftI(e, X_SHR, 4, R(RCX), 31);
		xRet(e);
		patchHere(e, rotate);
		xShiftCl(e, X_ROR, 4, R(RDX));
		setcEcx(e);
		xRet(e);
	} else {
		xAluI(e, X_CMP, 4, R(RCX), 32);
		big = xJcc(e, CC_AE, NULL);
		xShiftCl(e, type == SHIFT_LSL ? X_SHL : type == SHIFT_LSR ? X_SHR : X_SAR, 4, R(RDX));
		setcEcx(e);
		xRet(e);
		patchHere(e, big);
		switch (type) {
		case SHIFT_LSL: {
			uint8_t* over = xJcc(e, CC_NE, NULL);
			xMovRR(e, 4, RCX, RDX);
			xAluI(e, X_AND, 4, R(RCX), 1);
			xMovI(e, RDX, 0);
			xRet(e);
			patchHere(e, over);
			xMovI(e, RCX, 0);
			xMovI(e, RDX, 0);
			xRet(e);
			break;
		}
		case SHIFT_LSR: {
			uint8_t* over = xJcc(e, CC_NE, NULL);
			xMovRR(e, 4, RCX, RDX);
			xShiftI(e, X_SHR, 4, R(RCX), 31);
			xMovI(e, RDX, 0);
			xRet(e);
			patchHere(e, over);
			xMovI(e, RCX, 0);
			xMovI(e, RDX, 0);
			xRet(e);
			break;
		}
		case SHIFT_ASR:
			xMovRR(e, 4, RCX, RDX);
			xShiftI(e, X_SHR, 4, R(RCX), 31);
			xShiftI(e, X_SAR, 4, R(RDX), 31);
			xRet(e);
			break;
		}
	}
	patchHere(e, zero);
	carryFromFlags(e);
	xRet(e);
	return start;
}

// Calls a C function from inside a routine, preserving guest registers, rax and rdx: ecx in, ecx
// out. The stack is 16-byte aligned on entry when aligned is set, otherwise 8 bytes off.
static void emitPreserveCall(struct Emitter* e, const void* fn, void (*args)(struct Emitter*), bool aligned) {
	static const int saved[] = { RAX, RDX, RSI, RDI, R8, R9, R10, R11 };
	for (int i = 0; i < 8; ++i) {
		xPush(e, saved[i]);
	}
	if (!aligned) {
		xAluI(e, X_SUB, 8, R(RSP), 8);
	}
	xLoad(e, 4, RAX, M(R15, OFF(cpu.nextEvent)));
	xAluRR(e, X_ADD, 4, R(RAX), R14);
	xStore(e, 4, M(R15, OFF(cpu.cycles)), RAX);
	storeBase(e, RAX);
	xMovRR(e, 8, RDI, R15);
	args(e);
	callAbs(e, fn);
	xMovRR(e, 4, RCX, RAX);
	xLoad(e, 4, R14, M(R15, OFF(cpu.cycles)));
	xAluRM(e, X_SUB, 4, R14, M(R15, OFF(cpu.nextEvent)));
	loadBase(e, RAX);
	if (!aligned) {
		xAluI(e, X_ADD, 8, R(RSP), 8);
	}
	for (int i = 7; i >= 0; --i) {
		xPop(e, saved[i]);
	}
}

// Moves the helper's second argument into the register the C calling convention passes it in.
static void multiplyCarryArgs(struct Emitter* e) {
	xMovRR(e, 4, RSI, RCX);
}

// The same for the helper called when a store lands on compiled code.
static void codeWriteArgs(struct Emitter* e) {
	xMovRR(e, 4, RSI, RCX);
}

// Memory access routines. Each inline path takes the bus's cycles for its region, including a
// load's internal cycle. An access an event would fall inside goes to the exact CPU's access
// instead.

// Goes through bus.c, events and all; ecx = address, edx = a value to store.
static void emitSlowPath(struct Emitter* e, int kind) {
	// The return address identifies the guest instruction for open-bus reads.
	xLoad(e, 8, RAX, M(RSP, 0));
	xStore(e, 8, M(R15, OFF(siteReturn)), RAX);
	xLoad(e, 4, RAX, M(R15, OFF(cpu.nextEvent)));
	xAluRR(e, X_ADD, 4, R(RAX), R14);
	xStore(e, 4, M(R15, OFF(cpu.cycles)), RAX);
	storeBase(e, RAX);
	static const int saved[] = { RSI, RDI, R8, R9, R10, R11 };
	for (int i = 0; i < 6; ++i) {
		xPush(e, saved[i]);
	}
	// The address, for sign extension; this also aligns the stack.
	xPush(e, RCX);
	xMovRR(e, 8, RDI, R15);
	xMovI(e, RSI, kind);
	xMovRR(e, 4, RAX, RDX);
	xMovRR(e, 4, RDX, RCX);
	xMovRR(e, 4, RCX, RAX);
	callAbs(e, jitSlowAccess);
	if (kind == MEM_LOADS8) {
		xMovsx(e, RAX, 1, R(RAX));
	} else if (kind == MEM_LOADS16) {
		xTestI(e, 1, M(RSP, 0), 1);
		uint8_t* odd = xJcc(e, CC_NE, NULL);
		xMovsx(e, RAX, 2, R(RAX));
		uint8_t* done = xJmp(e, NULL);
		patchHere(e, odd);
		xMovsx(e, RAX, 1, R(RAX));
		patchHere(e, done);
	}
	xPop(e, RCX);
	for (int i = 5; i >= 0; --i) {
		xPop(e, saved[i]);
	}
	xLoad(e, 4, R14, M(R15, OFF(cpu.cycles)));
	xAluRM(e, X_SUB, 4, R14, M(R15, OFF(cpu.nextEvent)));
	loadBase(e, RDX);
	xRet(e);
}

// Loads data into eax from [base + rax]; ecx still holds the address.
static void emitLoadData(struct Emitter* e, int kind, int base, int32_t disp) {
	switch (kind) {
	case MEM_LOAD32:
		xLoad(e, 4, RAX, MI(base, RAX, 1, disp));
		xTestI(e, 1, R(RCX), 3);
		{
			uint8_t* aligned = xJcc(e, CC_E, NULL);
			xMovRR(e, 4, RDX, RCX);
			xAluI(e, X_AND, 4, R(RCX), 3);
			xShiftI(e, X_SHL, 4, R(RCX), 3);
			xShiftCl(e, X_ROR, 4, R(RAX));
			xMovRR(e, 4, RCX, RDX);
			patchHere(e, aligned);
		}
		break;
	case MEM_LOAD16:
		xMovzx(e, RAX, 2, MI(base, RAX, 1, disp));
		xTestI(e, 1, R(RCX), 1);
		{
			uint8_t* aligned = xJcc(e, CC_E, NULL);
			xShiftI(e, X_ROR, 4, R(RAX), 8);
			patchHere(e, aligned);
		}
		break;
	case MEM_LOADS16:
		xTestI(e, 1, R(RCX), 1);
		{
			uint8_t* odd = xJcc(e, CC_NE, NULL);
			xMovsx(e, RAX, 2, MI(base, RAX, 1, disp));
			uint8_t* done = xJmp(e, NULL);
			patchHere(e, odd);
			xMovsx(e, RAX, 1, MI(base, RAX, 1, disp + 1));
			patchHere(e, done);
		}
		break;
	case MEM_LOAD8:
		xMovzx(e, RAX, 1, MI(base, RAX, 1, disp));
		break;
	case MEM_LOADS8:
		xMovsx(e, RAX, 1, MI(base, RAX, 1, disp));
		break;
	}
}

// Goes to the slow path unless the access ends before the next event: cycles is fixed, plus the
// wait table entry at tableOffset when there is one. Uses eax.
static void emitGuard(struct Emitter* e, int cycles, int32_t tableOffset, uint8_t** toSlow, int* slowCount) {
	if (tableOffset < 0) {
		xAluI(e, X_CMP, 4, R(R14), -cycles);
		toSlow[(*slowCount)++] = xJcc(e, CC_GE, NULL);
		return;
	}
	xMovzx(e, RAX, 1, M(R15, tableOffset));
	xLea(e, 4, RAX, MI(RAX, R14, 1, cycles));
	xTest(e, 4, R(RAX), RAX);
	toSlow[(*slowCount)++] = xJcc(e, CC_NS, NULL);
}

// VRAM below the mirrors; ecx = address.
static void emitVramAccess(struct Emitter* e, int kind, uint8_t** toSlow, int* slowCount) {
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	int cycles = (size == 4 ? 2 : 1) + (store ? 0 : 1);
	// While the display keeps the CPU out of VRAM, C works out the wait.
	xAluI(e, X_CMP, 1, M(R15, OFF(vramStall)), 0);
	toSlow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	if (store) {
		// Once a line's H-blank has come, C draws the line first.
		xLea(e, 4, RAX, M(R14, cycles));
		xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(cpu.nextEvent)));
		xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(timing.masterCycles)));
		xAluRM(e, X_SUB, 4, RAX, M(R15, OFF(video.drawDue)));
		toSlow[(*slowCount)++] = xJcc(e, CC_NS, NULL);
	}
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), 0x1FFFF);
	xAluI(e, X_CMP, 4, R(RAX), SIZE_VRAM);
	toSlow[(*slowCount)++] = xJcc(e, CC_AE, NULL);
	if (store) {
		xAluI(e, X_CMP, 1, M(R15, OFF(videoCode)), 0);
		toSlow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	}
	xAluI(e, X_CMP, 4, R(R14), -cycles);
	toSlow[(*slowCount)++] = xJcc(e, CC_GE, NULL);
	xAluI(e, X_AND, 4, R(RAX), 0x1FFFF & ~(size - 1));
	if (store) {
		// Tells the renderer about stores that change something, by page.
		xAluRM(e, X_CMP, size, RDX, MI(R15, RAX, 1, OFF(vram)));
		uint8_t* same = xJcc(e, CC_E, NULL);
		xStore(e, size, MI(R15, RAX, 1, OFF(vram)), RDX);
		xShiftI(e, X_SHR, 4, R(RAX), 11);
		xStoreI(e, 1, MI(R15, RAX, 1, OFF(renderer.vramChanged)), 1);
		patchHere(e, same);
	} else {
		emitLoadData(e, kind, R15, OFF(vram));
	}
	xAluI(e, X_ADD, 4, R(R14), cycles);
	xRet(e);
}

// A byte store to VRAM, as memoryStore8 does it: into both halves of its halfword below the sprite
// tiles, and nowhere among them. ecx = address, dl = value.
static void emitVramByteStore(struct Emitter* e, uint8_t** toSlow, int* slowCount) {
	xAluI(e, X_CMP, 1, M(R15, OFF(vramStall)), 0);
	toSlow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	// Once a line's H-blank has come, C draws the line first.
	xLea(e, 4, RAX, M(R14, 1));
	xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(cpu.nextEvent)));
	xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(timing.masterCycles)));
	xAluRM(e, X_SUB, 4, RAX, M(R15, OFF(video.drawDue)));
	toSlow[(*slowCount)++] = xJcc(e, CC_NS, NULL);
	xAluI(e, X_CMP, 1, M(R15, OFF(videoCode)), 0);
	toSlow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	xAluI(e, X_CMP, 4, R(R14), -1);
	toSlow[(*slowCount)++] = xJcc(e, CC_GE, NULL);
	// The sprite tiles start at 0x10000, or 0x14000 in the bitmap modes.
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), 0x1FFFF);
	xMovzx(e, RCX, 2, M(R15, OFF(io)));
	xAluI(e, X_AND, 4, R(RCX), 7);
	xAluI(e, X_CMP, 4, R(RCX), 3);
	uint8_t* tiles = xJcc(e, CC_B, NULL);
	xAluI(e, X_CMP, 4, R(RAX), 0x14000);
	uint8_t* ignoredBitmap = xJcc(e, CC_AE, NULL);
	uint8_t* store = xJmp(e, NULL);
	patchHere(e, tiles);
	xAluI(e, X_CMP, 4, R(RAX), 0x10000);
	uint8_t* ignored = xJcc(e, CC_AE, NULL);
	patchHere(e, store);
	xAluI(e, X_AND, 4, R(RAX), 0x1FFFE);
	xMovzx(e, RDX, 1, R(RDX));
	xMovRR(e, 4, RCX, RDX);
	xShiftI(e, X_SHL, 4, R(RDX), 8);
	xAluRR(e, X_OR, 4, R(RDX), RCX);
	// Tells the renderer about stores that change something, by page.
	xAluRM(e, X_CMP, 2, RDX, MI(R15, RAX, 1, OFF(vram)));
	uint8_t* same = xJcc(e, CC_E, NULL);
	xStore(e, 2, MI(R15, RAX, 1, OFF(vram)), RDX);
	xShiftI(e, X_SHR, 4, R(RAX), 11);
	xStoreI(e, 1, MI(R15, RAX, 1, OFF(renderer.vramChanged)), 1);
	patchHere(e, same);
	patchHere(e, ignored);
	patchHere(e, ignoredBitmap);
	xAluI(e, X_ADD, 4, R(R14), 1);
	xRet(e);
}

// romCode is set for cartridge code with the prefetch buffer, whose data accesses to the cartridge
// stop it.
static uint8_t* emitMemRoutine(struct Jit* jit, struct Emitter* e, int kind, bool romCode) {
	uint8_t* start = e->p;
	bool store = kind >= MEM_STORE32;
	int size = accessSize(kind);
	int32_t waitN = size == 4 ? OFF(waitN32) : OFF(waitN16);
	// The access and a load's internal cycle.
	int fixed = store ? 1 : 2;
	uint8_t* toSlow[24];
	int slowCount = 0;

	xMovRR(e, 4, RAX, RCX);
	xShiftI(e, X_SHR, 4, R(RAX), 24);
	xAluI(e, X_CMP, 4, R(RAX), REGION_IWRAM);
	uint8_t* toIwram = xJcc(e, CC_E, NULL);
	xAluI(e, X_CMP, 4, R(RAX), REGION_EWRAM);
	uint8_t* toEwram = xJcc(e, CC_E, NULL);
	uint8_t* toVram = NULL;
	xAluI(e, X_CMP, 4, R(RAX), REGION_VRAM);
	toVram = xJcc(e, CC_E, NULL);
	uint8_t* toIo = NULL;
	if (kind == MEM_LOAD16 || kind == MEM_LOADS16) {
		xAluI(e, X_CMP, 4, R(RAX), REGION_IO);
		toIo = xJcc(e, CC_E, NULL);
	}
	uint8_t* toRom = NULL;
	if (!store) {
		xAluI(e, X_SUB, 4, R(RAX), REGION_ROM0);
		xAluI(e, X_CMP, 4, R(RAX), REGION_ROM2_EX - REGION_ROM0);
		toRom = xJcc(e, CC_BE, NULL);
	}
	toSlow[slowCount++] = xJmp(e, NULL);

	for (int r = 0; r < 2; ++r) {
		bool iwram = r == 0;
		patchHere(e, iwram ? toIwram : toEwram);
		uint32_t mask = (iwram ? SIZE_IWRAM : SIZE_EWRAM) - size;
		int32_t data = iwram ? OFF(iwram) : OFF(ewram);
		int32_t map = iwram ? OFF(iwramCode) : OFF(ewramCode);
		int32_t table = iwram ? -1 : waitN + REGION_EWRAM;
		emitGuard(e, fixed, table, toSlow, &slowCount);
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), mask);
		if (store) {
			xStore(e, size, MI(R15, RAX, 1, data), RDX);
			xShiftI(e, X_SHR, 4, R(RAX), 2);
			xAluI(e, X_CMP, 1, MI(R15, RAX, 1, map), 0);
			uint8_t* clean = xJcc(e, CC_E, NULL);
			// Code that checks the word when it runs only needs the helper for a store into the two
			// instructions after it (its r15 is in the state), which are already prefetched.
			xAluI(e, X_CMP, 1, MI(R15, RAX, 1, map), CODE_MAP_CHECKED);
			uint8_t* other = xJcc(e, CC_NE, NULL);
			xLoad(e, 4, RDX, M(R15, REG(15)));
			xAluI(e, X_SUB, 4, R(RDX), 4);
			xAluI(e, X_AND, 4, R(RDX), ~3);
			xMovRR(e, 4, RAX, RCX);
			xAluI(e, X_AND, 4, R(RAX), ~3);
			xAluRR(e, X_SUB, 4, R(RAX), RDX);
			xAluI(e, X_CMP, 4, R(RAX), 8);
			uint8_t* away = xJcc(e, CC_AE, NULL);
			patchHere(e, other);
			xLoad(e, 8, RAX, M(RSP, 0));
			xStore(e, 8, M(R15, OFF(siteReturn)), RAX);
			emitPreserveCall(e, jitHelperCodeWrite, codeWriteArgs, false);
			patchHere(e, clean);
			patchHere(e, away);
		} else {
			emitLoadData(e, kind, R15, data);
		}
		if (iwram) {
			xAluI(e, X_ADD, 4, R(R14), fixed);
		} else {
			xMovzx(e, RDX, 1, M(R15, table));
			xLea(e, 4, R14, MI(R14, RDX, 1, fixed));
		}
		xRet(e);
	}

	if (toRom) {
		// Data from the cartridge stops the prefetch buffer. That costs nothing outside cartridge
		// code, and the buffer is never running in cartridge code compiled without it.
		patchHere(e, toRom);
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), SIZE_ROM - size);
		xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(romSize)));
		toSlow[slowCount++] = xJcc(e, CC_AE, NULL);
		// An EEPROM answers in the cartridge's last region, whatever the cartridge's size.
		xMovRR(e, 4, RAX, RCX);
		xShiftI(e, X_SHR, 4, R(RAX), 24);
		xAluI(e, X_CMP, 4, R(RAX), REGION_ROM2_EX);
		uint8_t* notTop = xJcc(e, CC_NE, NULL);
		xAluI(e, X_CMP, 4, M(R15, OFF(savedata.type)), SAVEDATA_EEPROM);
		toSlow[slowCount++] = xJcc(e, CC_E, NULL);
		xAluI(e, X_CMP, 4, M(R15, OFF(savedata.type)), SAVEDATA_EEPROM512);
		toSlow[slowCount++] = xJcc(e, CC_E, NULL);
		patchHere(e, notTop);
		// The cycle a stop can cost, in edx.
		xAluRR(e, X_XOR, 4, R(RDX), RDX);
		if (romCode) {
			xAluI(e, X_CMP, 1, M(R15, OFF(cpu.prefetching)), 0);
			uint8_t* idle = xJcc(e, CC_E, NULL);
			xMovRR(e, 4, RAX, R14);
			xAluRM(e, X_SUB, 4, RAX, M(R15, OFF(jitBase)));
			xAluI(e, X_CMP, 4, R(RAX), 127);
			uint8_t* far = xJcc(e, CC_A, NULL);
			xLoad(e, 4, RDX, M(R15, OFF(prefetcher.duty)));
			xShiftI(e, X_SHL, 4, R(RDX), 7);
			xAluRR(e, X_ADD, 4, R(RAX), RDX);
			xAluI(e, X_CMP, 4, M(R15, OFF(prefetcher.width)), 4);
			uint8_t* half = xJcc(e, CC_NE, NULL);
			xAluI(e, X_ADD, 4, R(RAX), 4096);
			patchHere(e, half);
			xMovzx(e, RDX, 1, MI(R15, RAX, 1, OFF(jitStopPenalty)));
			uint8_t* done = xJmp(e, NULL);
			patchHere(e, far);
			xAluRR(e, X_XOR, 4, R(RDX), RDX);
			patchHere(e, idle);
			patchHere(e, done);
		}
		xMovRR(e, 4, RAX, RCX);
		xShiftI(e, X_SHR, 4, R(RAX), 24);
		xMovzx(e, RAX, 1, MI(R15, RAX, 1, waitN));
		xLea(e, 4, RDX, MI(RDX, RAX, 1, fixed));
		xLea(e, 4, RAX, MI(RDX, R14, 1, 0));
		xTest(e, 4, R(RAX), RAX);
		toSlow[slowCount++] = xJcc(e, CC_NS, NULL);
		xMovRR(e, 4, R14, RAX);
		xStoreI(e, 1, M(R15, OFF(cpu.prefetching)), 0);
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), SIZE_ROM - size);
		xLoad(e, 8, RDX, M(R15, OFF(rom)));
		emitLoadData(e, kind, RDX, 0);
		xRet(e);
	}

	if (toVram) {
		patchHere(e, toVram);
		if (kind == MEM_STORE8) {
			emitVramByteStore(e, toSlow, &slowCount);
		} else {
			emitVramAccess(e, kind, toSlow, &slowCount);
		}
	}

	if (toIo) {
		// Registers games poll that ioRead returns as stored: KEYINPUT; VCOUNT and DISPSTAT until
		// the next line or H-blank; and IF until an H-blank IRQ request it works out, when ioRead
		// brings them up to date.
		static const uint32_t plain[] = { REG_DISPCNT, REG_DISPSTAT, REG_VCOUNT, REG_IF, REG_KEYINPUT };
		patchHere(e, toIo);
		emitGuard(e, fixed, -1, toSlow, &slowCount);
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), 0xFFFFFE);
		uint8_t* toPlain[5];
		for (int i = 0; i < 5; ++i) {
			xAluI(e, X_CMP, 4, R(RAX), plain[i]);
			toPlain[i] = xJcc(e, CC_E, NULL);
		}
		toSlow[slowCount++] = xJmp(e, NULL);
		patchHere(e, toPlain[3]);
		xLea(e, 4, RDX, M(R14, fixed));
		xAluRM(e, X_ADD, 4, RDX, M(R15, OFF(cpu.nextEvent)));
		xAluRM(e, X_ADD, 4, RDX, M(R15, OFF(timing.masterCycles)));
		xAluRM(e, X_SUB, 4, RDX, M(R15, OFF(irq.ifStable)));
		toSlow[slowCount++] = xJcc(e, CC_NS, NULL);
		uint8_t* toLoad = xJmp(e, NULL);
		patchHere(e, toPlain[1]);
		patchHere(e, toPlain[2]);
		xLea(e, 4, RDX, M(R14, fixed));
		xAluRM(e, X_ADD, 4, RDX, M(R15, OFF(cpu.nextEvent)));
		xAluRM(e, X_ADD, 4, RDX, M(R15, OFF(timing.masterCycles)));
		xAluRM(e, X_SUB, 4, RDX, M(R15, OFF(video.stableUntil)));
		toSlow[slowCount++] = xJcc(e, CC_NS, NULL);
		patchHere(e, toLoad);
		patchHere(e, toPlain[0]);
		patchHere(e, toPlain[4]);
		emitLoadData(e, kind, R15, OFF(io));
		xAluI(e, X_ADD, 4, R(R14), fixed);
		xRet(e);
	}

	for (int i = 0; i < slowCount; ++i) {
		patchHere(e, toSlow[i]);
	}
	emitSlowPath(e, kind);
	return start;
}

// Bodies and cold code share one run of memory here.
void backendReset(struct Jit* jit) {
	(void) jit;
}

// This backend keeps a block's cold code with its body, so nothing is on a cold side.
bool backendColdSide(const struct Jit* jit, const uint8_t* p) {
	(void) jit;
	(void) p;
	return false;
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
	jit->multiplyCarryRoutine = e.p;
	emitPreserveCall(&e, jitMultiplyCarry, multiplyCarryArgs, false);
	xRet(&e);
	// For ARMGBA_PERFMAP.
	static const char* const kinds[8] = { "load32", "load16", "load8", "loadS8", "loadS16", "store32", "store16", "store8" };
	jitNameCode(jit->spillRoutine, jit->reloadRoutine, "spill");
	jitNameCode(jit->reloadRoutine, (void*) jit->enter, "reload");
	jitNameCode((void*) jit->enter, jit->exitRoutine, "enter");
	jitNameCode(jit->exitRoutine, jit->dispatchKey, "exit");
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
	jit->codeNext = e.p;
	return true;
}

// Block compiler.

struct Operand2 {
	bool isImm;
	uint32_t imm;
	struct Opnd reg;
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
	// Block transfers that may move words to or from palette RAM, VRAM or OAM, whose copies for those
	// go with the cold code: the jump there from the work RAM checks, and where the exact CPU's
	// handling and the code after the transfer start.
	struct VideoTransfer {
		const struct Inst* in;
		uint8_t* site;
		uint8_t* exact;
		uint8_t* join;
	} videoTransfers[MAX_BLOCK_INSTS];
	int videoCount;
	struct VideoTransfer* video;

	// Where the guest flags are: in the host EFLAGS (C inverted after subtraction), in ebp, or
	// both.
	bool hostFlags;
	bool hostInverted;

	uint8_t liveAfter[MAX_BLOCK_INSTS];
	bool checkpoint[MAX_BLOCK_INSTS + 1];
	int cur;
	// Cycles counted but not yet added to r14d. In cartridge code with the prefetch buffer, they
	// map the time t and the buffer's base B in (max, +) algebra: t' = max(t + pending, B + pendB)
	// and B' = max(t + pendC, B + pendD), with NONE standing for no term.
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

#include "../jit-compile.h"

// Flags.

// Moves the host's flags after an operation to where the guest's are kept.
static void captureFlags(struct Compiler* c) {
	struct Emitter* e = &c->e;
	if (c->hostInverted) {
		xCmc(e);
		c->hostInverted = false;
	}
	xSetcc(e, CC_O, R(RAX));
	xLahf(e);
	xMovzx(e, RBP, 2, R(RAX));
}

// Called before emitting anything that changes the host flags.
static void clobberFlags(struct Compiler* c) {
	if (c->hostFlags) {
		captureFlags(c);
		c->hostFlags = false;
	}
}

// Adds the cycles counted so far to the cycle counter.
static void flushPending(struct Compiler* c) {
	struct Emitter* e = &c->e;
	if (pendingScalar(c)) {
		if (c->pending) {
			xLea(e, 4, R14, M(R14, c->pending));
		}
		resetPending(c);
		return;
	}
	clobberFlags(c);
	xLoad(e, 4, RAX, M(R15, OFF(jitBase)));
	if (c->pendC != NONE || c->pendD != 0) {
		if (c->pendC != NONE && c->pendD != NONE) {
			xLea(e, 4, RCX, M(R14, c->pendC));
			xLea(e, 4, RDX, M(RAX, c->pendD));
			xAluRR(e, X_CMP, 4, R(RDX), RCX);
			xCmov(e, CC_L, 4, RDX, R(RCX));
		} else if (c->pendC != NONE) {
			xLea(e, 4, RDX, M(R14, c->pendC));
		} else {
			xLea(e, 4, RDX, M(RAX, c->pendD));
		}
		xStore(e, 4, M(R15, OFF(jitBase)), RDX);
	}
	if (c->pending) {
		xLea(e, 4, R14, M(R14, c->pending));
	}
	if (c->pendB != NONE) {
		xLea(e, 4, RDX, M(RAX, c->pendB));
		xAluRR(e, X_CMP, 4, R(R14), RDX);
		xCmov(e, CC_L, 4, R14, R(RDX));
	}
	resetPending(c);
}

// Jumps when the ARM condition holds (or fails, if negate). Leaves the canonical flags valid.
static uint8_t* emitCondJump(struct Compiler* c, int cond, bool negate) {
	struct Emitter* e = &c->e;
	if (negate) {
		cond ^= 1;
	}
	static const int simpleMask[8] = { 0x4000, 0x100, 0x8000, 1 };
	if (c->hostFlags) {
		captureFlags(c);
	} else if (cond < 8) {
		xTestI(e, 4, R(RBP), simpleMask[cond >> 1]);
		return xJcc(e, (cond & 1) ? CC_E : CC_NE, NULL);
	} else {
		xMovRR(e, 4, RAX, RBP);
		xAluI(e, X_ADD, 1, R(RAX), 0x7F);
		xSahf(e);
		c->hostFlags = true;
		c->hostInverted = false;
	}
	// The host flags hold N=SF, Z=ZF, V=OF, and C=CF unless inverted.
	int cc;
	switch (cond) {
	case 0x0: cc = CC_E; break;
	case 0x1: cc = CC_NE; break;
	case 0x2: cc = c->hostInverted ? CC_AE : CC_B; break;
	case 0x3: cc = c->hostInverted ? CC_B : CC_AE; break;
	case 0x4: cc = CC_S; break;
	case 0x5: cc = CC_NS; break;
	case 0x6: cc = CC_O; break;
	case 0x7: cc = CC_NO; break;
	case 0x8:
	case 0x9:
		if (!c->hostInverted) {
			xCmc(e);
			c->hostInverted = true;
		}
		cc = cond == 0x8 ? CC_A : CC_BE;
		break;
	case 0xA: cc = CC_GE; break;
	case 0xB: cc = CC_L; break;
	case 0xC: cc = CC_G; break;
	default: cc = CC_LE; break;
	}
	return xJcc(e, cc, NULL);
}

// Values.

// Loads a guest register's value into a host register. r15 is a constant when compiling.
static void loadGuest(struct Compiler* c, int dst, int r, const struct Inst* in) {
	if (r == 15) {
		xMovIKeepFlags(&c->e, dst, pcRead(c, in));
	} else if (hostReg[r] >= 0) {
		xMovRR(&c->e, 4, dst, hostReg[r]);
	} else {
		xLoad(&c->e, 4, dst, guest(r));
	}
}

// Stores a host register's value into a guest register. r15 lives in the CPU state, not in a host
// register.
static void storeGuest(struct Compiler* c, int r, int src) {
	if (hostReg[r] >= 0) {
		xMovRR(&c->e, 4, hostReg[r], src);
	} else {
		xStore(&c->e, 4, guest(r), src);
	}
}

// Stores a constant into a guest register.
static void storeGuestImm(struct Compiler* c, int r, uint32_t value) {
	if (hostReg[r] >= 0) {
		xMovIKeepFlags(&c->e, hostReg[r], value);
	} else {
		xStoreI(&c->e, 4, guest(r), value);
	}
}

// Rotates right, as ARM's rotated constants do.
static uint32_t ror32(uint32_t value, int rotate) {
	rotate &= 31;
	return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

// Computes the shifter operand, in edx unless it's a constant or an unshifted register; a computed
// carry lands in ecx as 0 or 1.
static struct Operand2 emitShifter(struct Compiler* c, const struct Inst* in, bool wantCarry) {
	struct Emitter* e = &c->e;
	const struct Operand* op = &in->op2;
	struct Operand2 out = { .carry = CARRY_SAME };
	if (op->isImm && in->patchable) {
		// The game rewrites this constant: rotate it from the instruction word at run time, passing
		// C through when it isn't rotated.
		clobberFlags(c);
		int32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (int32_t) (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (int32_t) (in->address & (SIZE_EWRAM - 1));
		xLoad(e, 4, RDX, M(R15, word));
		xMovRR(e, 4, RCX, RDX);
		xShiftI(e, X_SHR, 4, R(RCX), 7);
		xAluI(e, X_AND, 4, R(RCX), 0x1E);
		xAluI(e, X_AND, 4, R(RDX), 0xFF);
		xShiftCl(e, X_ROR, 4, R(RDX));
		if (wantCarry) {
			xTest(e, 4, R(RCX), RCX);
			uint8_t* same = xJcc(e, CC_E, NULL);
			xMovRR(e, 4, RCX, RDX);
			xShiftI(e, X_SHR, 4, R(RCX), 31);
			uint8_t* done = xJmp(e, NULL);
			patchHere(e, same);
			xMovRR(e, 4, RCX, RBP);
			xShiftI(e, X_SHR, 4, R(RCX), 8);
			xAluI(e, X_AND, 4, R(RCX), 1);
			patchHere(e, done);
		}
		out.reg = R(RDX);
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
			xMovI(e, RDX, pcRead(c, in) + 4);
		} else {
			loadGuest(c, RDX, op->rm, in);
		}
		loadGuest(c, RCX, op->rs, in);
		xCall(e, c->jit->shiftRoutines[op->type]);
		out.reg = R(RDX);
		out.carry = CARRY_REG;
		return out;
	}
	if (in->patchable) {
		// The game rewrites this amount: shift LSL by the one in the instruction word, which passes
		// C through when it is 0.
		clobberFlags(c);
		int32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (int32_t) (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (int32_t) (in->address & (SIZE_EWRAM - 1));
		xLoad(e, 4, RCX, M(R15, word));
		xShiftI(e, X_SHR, 4, R(RCX), 7);
		xAluI(e, X_AND, 4, R(RCX), 31);
		if (op->rm == 15) {
			xMovI(e, RDX, pcRead(c, in));
		} else {
			loadGuest(c, RDX, op->rm, in);
		}
		if (wantCarry) {
			// SHL leaves the last bit shifted out in CF, and leaves the flags alone for an amount
			// of 0.
			xTest(e, 4, R(RCX), RCX);
			uint8_t* zero = xJcc(e, CC_E, NULL);
			xShiftCl(e, X_SHL, 4, R(RDX));
			xSetcc(e, CC_B, R(RCX));
			xMovzx(e, RCX, 1, R(RCX));
			uint8_t* done = xJmp(e, NULL);
			patchHere(e, zero);
			xMovRR(e, 4, RCX, RBP);
			xShiftI(e, X_SHR, 4, R(RCX), 8);
			xAluI(e, X_AND, 4, R(RCX), 1);
			patchHere(e, done);
		} else {
			xShiftCl(e, X_SHL, 4, R(RDX));
		}
		out.reg = R(RDX);
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
				clobberFlags(c);
				xMovI(e, RDX, v);
				xBtI(e, 4, R(RBP), 8);
				xShiftI(e, X_RCR, 4, R(RDX), 1);
				if (wantCarry) {
					setcEcx(e);
				}
				out.reg = R(RDX);
				out.carry = CARRY_REG;
				return out;
			}
		}
		out.isImm = true;
		out.imm = result;
		out.carry = carry;
		return out;
	}
	if (type == SHIFT_LSL && !amount) {
		out.reg = guest(op->rm);
		return out;
	}
	clobberFlags(c);
	loadGuest(c, RDX, op->rm, in);
	out.reg = R(RDX);
	out.carry = CARRY_REG;
	switch (type) {
	case SHIFT_LSL:
		xShiftI(e, X_SHL, 4, R(RDX), amount);
		if (wantCarry) {
			setcEcx(e);
		}
		break;
	case SHIFT_LSR:
		if (amount) {
			xShiftI(e, X_SHR, 4, R(RDX), amount);
			if (wantCarry) {
				setcEcx(e);
			}
		} else {
			if (wantCarry) {
				xMovRR(e, 4, RCX, RDX);
				xShiftI(e, X_SHR, 4, R(RCX), 31);
			}
			xMovI(e, RDX, 0);
		}
		break;
	case SHIFT_ASR:
		xShiftI(e, X_SAR, 4, R(RDX), amount ? amount : 31);
		if (wantCarry) {
			if (amount) {
				setcEcx(e);
			} else {
				xMovRR(e, 4, RCX, RDX);
				xAluI(e, X_AND, 4, R(RCX), 1);
			}
		}
		break;
	case SHIFT_ROR:
		if (amount) {
			xShiftI(e, X_ROR, 4, R(RDX), amount);
		} else {
			xBtI(e, 4, R(RBP), 8);
			xShiftI(e, X_RCR, 4, R(RDX), 1);
		}
		if (wantCarry) {
			setcEcx(e);
		}
		break;
	}
	return out;
}

// alu dst, operand
static void aluWith(struct Compiler* c, int alu, int dst, const struct Operand2* op) {
	if (op->isImm) {
		xAluI(&c->e, alu, 4, R(dst), op->imm);
	} else if (op->reg.mem) {
		xAluRM(&c->e, alu, 4, dst, op->reg);
	} else {
		xAluRR(&c->e, alu, 4, R(dst), op->reg.reg);
	}
}

// Loads an operand (a constant, a host register or a slot in the CPU state) into a host register.
static void movOperand(struct Compiler* c, int dst, const struct Operand2* op) {
	if (op->isImm) {
		xMovIKeepFlags(&c->e, dst, op->imm);
	} else if (op->reg.mem) {
		xLoad(&c->e, 4, dst, op->reg);
	} else {
		xMovRR(&c->e, 4, dst, op->reg.reg);
	}
}

// Sets N and Z from a result, C from the shifter (or kept), and keeps V.
static void logicalFlags(struct Compiler* c, int result, int carry, uint8_t live) {
	struct Emitter* e = &c->e;
	xTest(e, 4, R(result), result);
	if (!(live & (F_C | F_V))) {
		c->hostFlags = true;
		c->hostInverted = false;
		return;
	}
	xLahf(e);
	xAluI(e, X_AND, 4, R(RAX), 0xC000);
	switch (carry) {
	case CARRY_SAME:
		xAluI(e, X_AND, 4, R(RBP), 0x0101);
		break;
	case CARRY_0:
		xAluI(e, X_AND, 4, R(RBP), 0x0001);
		break;
	case CARRY_1:
		xAluI(e, X_AND, 4, R(RBP), 0x0001);
		xAluI(e, X_OR, 4, R(RBP), 0x0100);
		break;
	case CARRY_REG:
		xAluI(e, X_AND, 4, R(RBP), 0x0001);
		xShiftI(e, X_SHL, 4, R(RCX), 8);
		xAluRR(e, X_OR, 4, R(RBP), RCX);
		break;
	}
	xAluRR(e, X_OR, 4, R(RBP), RAX);
	c->hostFlags = false;
}

// Leaves for the exact CPU unless the instructions up to the next check end before the next event.
static void emitCheck(struct Compiler* c, int index, int ahead) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	if (!pendingScalar(c)) {
		flushPending(c);
	}
	xAluI(e, X_CMP, 4, R(R14), -(c->pending + ahead));
	struct Stub* stub = &c->stubs[c->stubCount++];
	stub->site = xJcc(e, CC_GE, NULL);
	stub->pc = c->insts[0].address + index * c->width;
	stub->pending = c->pending;
	stub->index = index;
}

// Calls a runtime helper with the guest state in memory; the helper's result stays in eax.
static void callHelper(struct Compiler* c, const struct Inst* in, const void* fn) {
	struct Emitter* e = &c->e;
	int index = (in->address - c->insts[0].address) / c->width;
	xStoreI(e, 4, M(R15, REG(15)), in->address + 2 * c->width);
	xStoreI(e, 4, M(R15, OFF(cpu.prefetch)), opcodeAt(c, index + 1));
	xStoreI(e, 4, M(R15, OFF(cpu.prefetch) + 4), opcodeAt(c, index + 2));
	xMovRR(e, 8, RDI, R15);
	callAbs(e, fn);
	xCall(e, c->jit->reloadRoutine);
}

// Before calling C: brings the flags and cycles up to date and stores the guest registers.
static void spill(struct Compiler* c) {
	clobberFlags(c);
	flushPending(c);
	xCall(&c->e, c->jit->spillRoutine);
}

// Jumps to the block at a guest address. It is emitted as a jump to a stub that looks the block up,
// and is patched to go straight there once the block exists.
static void emitLinkTo(struct Compiler* c, uint32_t pc, bool thumb) {
	struct Link* link = &c->links[c->linkCount];
	c->linkSites[c->linkCount++] = xJmp(&c->e, NULL);
	link->targetKey = blockKey(pc, thumb);
	link->from = c->block;
}

// Where the prefetch buffer reads next, for code that leaves the cartridge or C code that refills.
static void storeHead(struct Compiler* c, uint32_t head) {
	if (c->prefetch) {
		xStoreI(&c->e, 4, M(R15, OFF(prefetcher.head)), head);
	}
}

// Data or a refill from the cartridge stops the prefetch buffer: one cycle more if a read is about
// to finish.
static void emitStop(struct Compiler* c) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	flushPending(c);
	xMovRR(e, 4, RAX, R14);
	xAluRM(e, X_SUB, 4, RAX, M(R15, OFF(jitBase)));
	xAluI(e, X_CMP, 4, R(RAX), 127);
	uint8_t* far = xJcc(e, CC_A, NULL);
	xMovzx(e, RAX, 1, MI(R15, RAX, 1, c->penaltyRow));
	xAluRR(e, X_ADD, 4, R(R14), RAX);
	patchHere(e, far);
}

// Called after an access that may have stopped the buffer: the next fetch restarts it,
// nonsequential. Compiled code treats that as a fetch from the buffer whose read finishes then;
// until the fetch, the buffer stays stopped for the exact CPU.
static void emitMissCheck(struct Compiler* c) {
	if (!c->prefetch) {
		return;
	}
	struct Emitter* e = &c->e;
	clobberFlags(c);
	flushPending(c);
	xAluI(e, X_CMP, 1, M(R15, OFF(cpu.prefetching)), 0);
	uint8_t* running = xJcc(e, CC_NE, NULL);
	xStoreI(e, 1, M(R15, OFF(cpu.fetchSeq)), 0);
	xLea(e, 4, RAX, M(R14, c->Ncyc - c->D));
	xStore(e, 4, M(R15, OFF(jitBase)), RAX);
	patchHere(e, running);
}

// The fetch after one that may have stopped the buffer restarts it.
static void emitRestarted(struct Compiler* c) {
	xStoreI(&c->e, 1, M(R15, OFF(cpu.prefetching)), 1);
	xStoreI(&c->e, 1, M(R15, OFF(cpu.fetchSeq)), 1);
}

// In the BIOS, the word that will be on the bus after a jump out of it, which is what reads of the
// BIOS from outside return.
static void setBiosPrefetchNext(struct Compiler* c, const struct Inst* in) {
	if (c->region == REGION_BIOS) {
		xStoreI(&c->e, 4, M(R15, OFF(biosPrefetchNext)), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
	}
}

// On a waiting loop's way back to its start.
static void emitIdleMark(struct Compiler* c, uint32_t to) {
	if (c->idleExit && c->cur == c->count - 1 && to == c->idleExit) {
		xStoreI(&c->e, 4, M(R15, OFF(idleMark)), blockKey(c->insts[0].address, c->thumb));
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
		xStoreI(e, 4, M(R15, OFF(exitPc)), blockKey(target & ~1u, c->thumb));
		xCall(e, c->jit->spillRoutine);
		xMovRR(e, 8, RDI, R15);
		xMovI(e, RSI, target);
		callAbs(e, jitHelperReload);
		xCall(e, c->jit->reloadRoutine);
		xLoad(e, 4, RAX, M(R15, OFF(exitPc)));
		xJmp(e, c->jit->dispatchKey);
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
			xStoreI(e, 4, M(R15, OFF(biosPrefetch)), opcodeAt(c, (in->address - c->insts[0].address) / c->width + 2));
		}
		xStoreI(e, 4, M(R15, OFF(activeRegion)), region);
		xStoreI(e, 4, M(R15, OFF(fetchRegion)), region);
	}
	emitLinkTo(c, target & ~1u, c->thumb);
}

// eax = target address; the instruction's cost is already pending.
static void emitIndirect(struct Compiler* c, const struct Inst* in, uint8_t* dispatch) {
	struct Emitter* e = &c->e;
	if (c->hostFlags || !pendingScalar(c)) {
		xStore(e, 4, M(R15, OFF(jitScratch)), RAX);
		clobberFlags(c);
		flushPending(c);
		xLoad(e, 4, RAX, M(R15, OFF(jitScratch)));
	}
	storeHead(c, in->address + 3 * c->width);
	flushPending(c);
	setBiosPrefetchNext(c, in);
	xJmp(&c->e, dispatch);
}

// Compiles a data-processing instruction. Flags are computed only if something later reads them,
// and the host's own flags are used where they come out the same.
static void emitAlu(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	int op = in->op;
	uint8_t live = c->liveAfter[c->cur];
	bool setFlags = in->setFlags;
	bool logical = isLogical(op);
	// A write to r15 that sets flags is left to the exact CPU.
	bool writesPc = aluWritesPc(in);
	bool wantCarry = setFlags && logical && (live & F_C);
	struct Operand2 op2 = emitShifter(c, in, wantCarry);

	if (!writesPc && !setFlags && (op == ALU_MOV || op == ALU_MVN)) {
		// Doesn't need to touch the host flags.
		if (hostReg[in->rd] >= 0) {
			movOperand(c, hostReg[in->rd], &op2);
			if (op == ALU_MVN) {
				xNot(e, 4, R(hostReg[in->rd]));
			}
		} else {
			movOperand(c, RAX, &op2);
			if (op == ALU_MVN) {
				xNot(e, 4, R(RAX));
			}
			xStore(e, 4, guest(in->rd), RAX);
		}
		pendAdd(c, aluExtra(c, in));
		return;
	}

	clobberFlags(c);
	int rn = in->rn;
	bool needsRn = op != ALU_MOV && op != ALU_MVN;
	int dst = (!writesPc && !isTest(op) && hostReg[in->rd] >= 0) ? hostReg[in->rd] : RAX;
	// Works in place only when the destination isn't also the operand register.
	bool inPlace = needsRn && rn != 15 && dst != RAX && hostReg[rn] == dst && op != ALU_RSB && op != ALU_RSC;
	if (!inPlace) {
		dst = RAX;
	}

	switch (op) {
	case ALU_MOV:
	case ALU_MVN:
		movOperand(c, RAX, &op2);
		if (op == ALU_MVN) {
			xNot(e, 4, R(RAX));
		}
		break;
	case ALU_RSB:
	case ALU_RSC:
		movOperand(c, RAX, &op2);
		if (rn == 15) {
			xMovIKeepFlags(e, RCX, pcRead(c, in) + (in->op2.byReg ? 4 : 0));
		}
		if (op == ALU_RSC) {
			xBtI(e, 4, R(RBP), 8);
			xCmc(e);
		}
		if (rn == 15) {
			xAluRR(e, op == ALU_RSB ? X_SUB : X_SBB, 4, R(RAX), RCX);
		} else if (hostReg[rn] >= 0) {
			xAluRR(e, op == ALU_RSB ? X_SUB : X_SBB, 4, R(RAX), hostReg[rn]);
		} else {
			xAluRM(e, op == ALU_RSB ? X_SUB : X_SBB, 4, RAX, guest(rn));
		}
		break;
	default: {
		static const int hostAlu[16] = {
			[ALU_AND] = X_AND, [ALU_EOR] = X_XOR, [ALU_SUB] = X_SUB, [ALU_ADD] = X_ADD,
			[ALU_ADC] = X_ADC, [ALU_SBC] = X_SBB, [ALU_TST] = X_AND, [ALU_TEQ] = X_XOR,
			[ALU_CMP] = X_SUB, [ALU_CMN] = X_ADD, [ALU_ORR] = X_OR, [ALU_BIC] = X_AND,
		};
		if (op == ALU_BIC) {
			if (op2.isImm) {
				op2.imm = ~op2.imm;
			} else {
				movOperand(c, RDX, &op2);
				xNot(e, 4, R(RDX));
				op2.reg = R(RDX);
			}
		}
		if (!inPlace) {
			if (rn == 15) {
				xMovIKeepFlags(e, RAX, pcRead(c, in) + (in->op2.byReg ? 4 : 0));
			} else {
				loadGuest(c, RAX, rn, in);
			}
		}
		if (op == ALU_ADC) {
			xBtI(e, 4, R(RBP), 8);
		} else if (op == ALU_SBC) {
			xBtI(e, 4, R(RBP), 8);
			xCmc(e);
		}
		aluWith(c, hostAlu[op], dst, &op2);
		break;
	}
	}

	bool subtract = op == ALU_SUB || op == ALU_SBC || op == ALU_RSB || op == ALU_RSC || op == ALU_CMP;
	if (dst == RAX && !isTest(op) && !writesPc) {
		storeGuest(c, in->rd, RAX);
	}
	if (setFlags) {
		if (logical) {
			int result = dst;
			if (dst == RAX) {
				xMovRR(e, 4, RDX, RAX);
				result = RDX;
			}
			int carry = op2.carry;
			if (carry == CARRY_REG && !wantCarry) {
				carry = CARRY_SAME;
			}
			logicalFlags(c, result, carry, live);
			if (writesPc) {
				xMovRR(e, 4, RAX, RDX);
			}
		} else {
			c->hostFlags = true;
			c->hostInverted = subtract;
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
	loadGuest(c, RAX, waitReg, in);
	if (sign) {
		xMovRR(e, 4, RDX, RAX);
		xShiftI(e, X_SAR, 4, R(RDX), 31);
		xAluRR(e, X_XOR, 4, R(RAX), RDX);
	}
	xAluI(e, X_OR, 4, R(RAX), 1);
	xBsr(e, 4, RAX, R(RAX));
	xShiftI(e, X_SHR, 4, R(RAX), 3);
	xLea(e, 4, R14, MI(R14, RAX, 1, 1 + extra));
}

// The C flag the multiplier leaves, from operands put in jitScratch first: ecx = 0 for MUL, 1 for
// UMULL and 2 for SMULL; the carry comes back in ecx.
static void emitMultiplyCarry(struct Compiler* c, int kind) {
	xMovI(&c->e, RCX, kind);
	xCall(&c->e, c->jit->multiplyCarryRoutine);
}

// Puts the operands where the C helper that works out a multiply's carry looks for them.
static void storeScratch(struct Compiler* c, int slot, int r, const struct Inst* in) {
	loadGuest(c, RAX, r, in);
	xStore(&c->e, 4, M(R15, OFF(jitScratch) + 4 * slot), RAX);
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
			xStoreI(e, 4, M(R15, OFF(jitScratch) + 8), 0);
		}
	}
	loadGuest(c, RAX, in->rm, in);
	if (hostReg[in->rs] >= 0) {
		xImul(e, 4, RAX, R(hostReg[in->rs]));
	} else {
		xImul(e, 4, RAX, guest(in->rs));
	}
	if (in->accumulate) {
		if (hostReg[in->rn] >= 0) {
			xAluRR(e, X_ADD, 4, R(RAX), hostReg[in->rn]);
		} else {
			xAluRM(e, X_ADD, 4, RAX, guest(in->rn));
		}
	}
	storeGuest(c, in->rd, RAX);
	if (in->setFlags) {
		xMovRR(e, 4, RDX, RAX);
		emitMultiplyCarry(c, 0);
		logicalFlags(c, RDX, CARRY_REG, F_ALL);
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
			xStoreI(e, 4, M(R15, OFF(jitScratch) + 8), 0);
			xStoreI(e, 4, M(R15, OFF(jitScratch) + 12), 0);
		}
	}
	loadGuest(c, RAX, in->rm, in);
	xMulWide(e, 4, guest(in->rs), in->signedMul);
	if (in->accumulate) {
		// The low halves add as unsigned, and the high halves take the carry.
		if (hostReg[in->rd] >= 0) {
			xAluRR(e, X_ADD, 4, R(RAX), hostReg[in->rd]);
		} else {
			xAluRM(e, X_ADD, 4, RAX, guest(in->rd));
		}
		if (hostReg[in->rdHi] >= 0) {
			xAluRR(e, X_ADC, 4, R(RDX), hostReg[in->rdHi]);
		} else {
			xAluRM(e, X_ADC, 4, RDX, guest(in->rdHi));
		}
	}
	storeGuest(c, in->rd, RAX);
	storeGuest(c, in->rdHi, RDX);
	if (in->setFlags) {
		emitMultiplyCarry(c, in->signedMul ? 2 : 1);
		// N from bit 63, Z from all 64 bits, C from the multiplier; V is kept.
		xShiftI(e, X_SHL, 4, R(RCX), 8);
		xAluI(e, X_AND, 4, R(RBP), 0x0001);
		xAluRR(e, X_OR, 4, R(RBP), RCX);
		xMovRR(e, 4, RCX, RAX);
		xAluRR(e, X_OR, 4, R(RCX), RDX);
		xSetcc(e, CC_E, R(RCX));
		xMovzx(e, RCX, 1, R(RCX));
		xShiftI(e, X_SHL, 4, R(RCX), 14);
		xShiftI(e, X_SHR, 4, R(RDX), 31);
		xShiftI(e, X_SHL, 4, R(RDX), 15);
		xAluRR(e, X_OR, 4, R(RCX), RDX);
		xAluRR(e, X_OR, 4, R(RBP), RCX);
	}
}

// A word access through the stack pointer, which points into IWRAM: inline when it lands there
// aligned and misses compiled code, otherwise through the memory routine out of line. ecx holds the
// address and edx a value to store. The check before it covers the inline path's cycles.
static void emitStackAccess(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	struct SlowAccess* slow = &c->slow[c->slowCount++];
	slow->in = in;
	slow->index = c->cur;
	slow->sites[1] = NULL;
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), 0xFF000003);
	xAluI(e, X_CMP, 4, R(RAX), REGION_IWRAM << 24);
	slow->sites[0] = xJcc(e, CC_NE, NULL);
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), SIZE_IWRAM - 4);
	if (!in->load) {
		xShiftI(e, X_SHR, 4, R(RAX), 2);
		xAluI(e, X_CMP, 1, MI(R15, RAX, 1, OFF(iwramCode)), 0);
		slow->sites[1] = xJcc(e, CC_NE, NULL);
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), SIZE_IWRAM - 4);
		xStore(e, 4, MI(R15, RAX, 1, OFF(iwram)), RDX);
	} else if (hostReg[in->rd] >= 0) {
		xLoad(e, 4, hostReg[in->rd], MI(R15, RAX, 1, OFF(iwram)));
	} else {
		xLoad(e, 4, RAX, MI(R15, RAX, 1, OFF(iwram)));
		storeGuest(c, in->rd, RAX);
	}
	xLea(e, 4, R14, M(R14, in->load ? 2 : 1));
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
			xStoreI(e, 1, M(R15, OFF(cpu.prefetching)), 0);
			xStoreI(e, 1, M(R15, OFF(cpu.fetchSeq)), 0);
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

	// Base in ecx, offset as a constant or in edx.
	bool regOffset = !in->op2.isImm;
	int32_t offset = 0;
	if (regOffset) {
		struct Operand2 off = emitShifter(c, in, false);
		if (off.isImm) {
			regOffset = false;
			offset = off.imm;
		} else {
			movOperand(c, RDX, &off);
		}
	} else if (in->patchable) {
		// The game rewrites this offset and its direction, so read them from the instruction word.
		uint32_t word = in->address & ~3u;
		int32_t base = (word >> 24) == REGION_IWRAM ? OFF(iwram) + (word & (SIZE_IWRAM - 1)) : OFF(ewram) + (word & (SIZE_EWRAM - 1));
		xLoad(e, 4, RDX, M(R15, base));
		xMovRR(e, 4, RAX, RDX);
		xShiftI(e, X_SHR, 4, R(RAX), 23);
		xAluI(e, X_AND, 4, R(RAX), 1);
		xAluI(e, X_SUB, 4, R(RAX), 1);
		xAluI(e, X_AND, 4, R(RDX), 0xFFF);
		xAluRR(e, X_XOR, 4, R(RDX), RAX);
		xAluRR(e, X_SUB, 4, R(RDX), RAX);
		regOffset = true;
	} else {
		offset = in->op2.imm;
	}
	if (!in->up) {
		offset = -offset;
	}
	if (in->rn == 15) {
		uint32_t base = c->thumb ? (in->address + 4) & ~3u : in->address + 8;
		xMovI(e, RCX, base);
	} else {
		loadGuest(c, RCX, in->rn, in);
	}
	if (regOffset && !in->up && !in->patchable) {
		xNeg(e, 4, R(RDX));
	}
	if (in->pre) {
		if (regOffset) {
			xAluRR(e, X_ADD, 4, R(RCX), RDX);
		} else if (offset) {
			xLea(e, 4, RCX, M(RCX, offset));
		}
	}
	// The stored register is read before the base is written back.
	bool writeback = in->writeback && in->rn != 15;
	if (writeback) {
		if (in->pre) {
			xMovRR(e, 4, RAX, RCX);
		} else if (regOffset) {
			xLea(e, 4, RAX, MI(RCX, RDX, 1, 0));
		} else {
			xLea(e, 4, RAX, M(RCX, offset));
		}
	}
	if (!in->load) {
		if (in->rd == 15) {
			xMovI(e, RDX, in->address + 12);
		} else {
			loadGuest(c, RDX, in->rd, in);
		}
	}
	if (writeback) {
		storeGuest(c, in->rn, RAX);
	}
	if (stackAccess(c, in)) {
		emitStackAccess(c, in);
		return;
	}
	if (!in->load && ramCode(c)) {
		xStoreI(e, 4, M(R15, REG(15)), in->address + 2 * c->width);
	}
	xCall(e, memRoutine(c, memKind(in)));
	jitRecordSite(c->block, e->p - c->block->entry, in->address, opcodeAt(c, c->cur + 1), opcodeAt(c, c->cur + 2));
	if (in->load) {
		if (in->rd == 15) {
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
			return;
		}
		storeGuest(c, in->rd, RAX);
	}
	pendAdd(c, nonseq(c));
	emitMissCheck(c);
}

enum {
	BLOCK_LOAD = 1 << 20,
	BLOCK_PRE = 1 << 21,
	BLOCK_UP = 1 << 22,
	BLOCK_WRITEBACK = 1 << 23,
	BLOCK_USER = 1 << 24,
	BLOCK_THUMB = 1 << 25,
};

// One RAM region's copy of the inline transfer; ecx = first address. Jumps to slow when a store
// would hit compiled code, or an event would fall inside it.
static void emitRamTransfer(struct Compiler* c, const struct Inst* in, int region, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	bool iwram = region == REGION_IWRAM;
	uint32_t mask = (iwram ? SIZE_IWRAM : SIZE_EWRAM) - 4;
	int32_t data = iwram ? OFF(iwram) : OFF(ewram);
	int32_t map = iwram ? OFF(iwramCode) : OFF(ewramCode);
	int count = __builtin_popcount(in->list);
	int cycles = transferCycles(c, in, region);
	xAluI(e, X_CMP, 4, R(R14), -cycles);
	slow[(*slowCount)++] = xJcc(e, CC_GE, NULL);
	if (!in->load) {
		for (int k = 0; k < count; ++k) {
			xLea(e, 4, RAX, M(RCX, 4 * k));
			xAluI(e, X_AND, 4, R(RAX), mask);
			xShiftI(e, X_SHR, 4, R(RAX), 2);
			xAluI(e, X_CMP, 1, MI(R15, RAX, 1, map), 0);
			slow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
		}
	}
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (!(in->list & (1 << r))) {
			continue;
		}
		xLea(e, 4, RAX, M(RCX, 4 * k));
		xAluI(e, X_AND, 4, R(RAX), mask);
		if (!in->load) {
			loadGuest(c, RDX, r, in);
			xStore(e, 4, MI(R15, RAX, 1, data), RDX);
		} else if (r == 15) {
			xLoad(e, 4, RAX, MI(R15, RAX, 1, data));
		} else if (hostReg[r] >= 0) {
			xLoad(e, 4, hostReg[r], MI(R15, RAX, 1, data));
		} else {
			xLoad(e, 4, RDX, MI(R15, RAX, 1, data));
			storeGuest(c, r, RDX);
		}
		++k;
	}
	if (in->writeback && (!in->load || !(in->list & (1 << in->rn)))) {
		xLea(e, 4, RDX, M(RCX, transferWriteback(in)));
		storeGuest(c, in->rn, RDX);
	}
	xLea(e, 4, R14, M(R14, cycles + (loadsPc(in) ? 0 : nonseq(c))));
}

// Pushes, pops and copies within IWRAM or EWRAM run inline; anything else goes to the exact CPU.
static void emitFastTransfer(struct Compiler* c, const struct Inst* in, uint8_t** slow, int* slowCount) {
	struct Emitter* e = &c->e;
	int count = __builtin_popcount(in->list);
	int32_t start = transferStart(in);
	clobberFlags(c);
	loadGuest(c, RCX, in->rn, in);
	if (start) {
		xLea(e, 4, RCX, M(RCX, start));
	}
	xTestI(e, 1, R(RCX), 3);
	slow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	xLea(e, 4, RDX, M(RCX, 4 * count - 4));
	xAluRR(e, X_XOR, 4, R(RDX), RCX);
	xShiftI(e, X_SHR, 4, R(RDX), 24);
	slow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	xMovRR(e, 4, RAX, RCX);
	xShiftI(e, X_SHR, 4, R(RAX), 24);
	xAluI(e, X_CMP, 4, R(RAX), REGION_EWRAM);
	uint8_t* ewram = xJcc(e, CC_E, NULL);
	bool toPc = in->load && (in->list & 0x8000);
	xAluI(e, X_CMP, 4, R(RAX), REGION_IWRAM);
	if (!toPc) {
		c->video = &c->videoTransfers[c->videoCount++];
		c->video->in = in;
		c->video->site = xJcc(e, CC_NE, NULL);
	} else {
		slow[(*slowCount)++] = xJcc(e, CC_NE, NULL);
	}
	for (int region = REGION_IWRAM; region >= REGION_EWRAM; --region) {
		if (region == REGION_EWRAM) {
			patchHere(e, ewram);
		}
		emitRamTransfer(c, in, region, slow, slowCount);
		if (toPc) {
			emitIndirect(c, in, c->thumb ? c->jit->dispatchThumb : c->jit->dispatchArm);
		} else {
			c->fastDone[c->fastDoneCount++] = xJmp(e, NULL);
		}
	}
}

// The copy of a block transfer for palette RAM, VRAM or OAM, in the cold code; ecx = first address,
// in the region. As the exact CPU's: every word costs the same, a store goes to the renderer only if
// it changes the word, and anything a store would make C do first (drawing a line whose H-blank
// has come, compiled code in video memory, the display holding VRAM) goes to the exact CPU.
static void emitVideoTransfer(struct Compiler* c, const struct VideoTransfer* t, int region) {
	struct Emitter* e = &c->e;
	const struct Inst* in = t->in;
	int count = __builtin_popcount(in->list);
	int cycles = transferCycles(c, in, region);
	int32_t data = videoMemory(region);
	uint32_t mask = region == REGION_VRAM ? 0x1FFFF : (region == REGION_OAM ? SIZE_OAM : SIZE_PALETTE) - 4;
	xAluI(e, X_CMP, 4, R(R14), -cycles);
	xJcc(e, CC_GE, t->exact);
	if (region == REGION_VRAM) {
		xAluI(e, X_CMP, 1, M(R15, OFF(vramStall)), 0);
		xJcc(e, CC_NE, t->exact);
		// Below the mirrors of the sprite tiles.
		xMovRR(e, 4, RAX, RCX);
		xAluI(e, X_AND, 4, R(RAX), 0x1FFFF);
		xAluI(e, X_CMP, 4, R(RAX), SIZE_VRAM - 4 * count);
		xJcc(e, CC_A, t->exact);
	}
	if (!in->load) {
		xAluI(e, X_CMP, 1, M(R15, OFF(videoCode)), 0);
		xJcc(e, CC_NE, t->exact);
		xLea(e, 4, RAX, M(R14, cycles));
		xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(cpu.nextEvent)));
		xAluRM(e, X_ADD, 4, RAX, M(R15, OFF(timing.masterCycles)));
		xAluRM(e, X_SUB, 4, RAX, M(R15, OFF(video.drawDue)));
		xJcc(e, CC_NS, t->exact);
	}
	int k = 0;
	for (int r = 0; r < 16; ++r) {
		if (!(in->list & (1 << r))) {
			continue;
		}
		xLea(e, 4, RAX, M(RCX, 4 * k));
		xAluI(e, X_AND, 4, R(RAX), mask);
		if (in->load) {
			if (hostReg[r] >= 0) {
				xLoad(e, 4, hostReg[r], MI(R15, RAX, 1, data));
			} else {
				xLoad(e, 4, RDX, MI(R15, RAX, 1, data));
				storeGuest(c, r, RDX);
			}
		} else {
			loadGuest(c, RDX, r, in);
			xAluRM(e, X_CMP, 4, RDX, MI(R15, RAX, 1, data));
			uint8_t* same = xJcc(e, CC_E, NULL);
			xStore(e, 4, MI(R15, RAX, 1, data), RDX);
			if (region == REGION_VRAM) {
				xShiftI(e, X_SHR, 4, R(RAX), 11);
				xStoreI(e, 1, MI(R15, RAX, 1, OFF(renderer.vramChanged)), 1);
			} else {
				xAluI(e, X_OR, 1, M(R15, OFF(renderer.memoryChanged)), region == REGION_OAM ? RENDER_OAM : RENDER_PALETTE);
			}
			patchHere(e, same);
		}
		++k;
	}
	if (in->writeback && (!in->load || !(in->list & (1 << in->rn)))) {
		xLea(e, 4, RDX, M(RCX, transferWriteback(in)));
		storeGuest(c, in->rn, RDX);
	}
	xLea(e, 4, R14, M(R14, cycles + nonseq(c)));
	xJmp(e, t->join);
}

// Calls the exact CPU for the instruction, with its fetch counted; eax = the next block key, or ~0.
static void emitExecute(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	xMovI(e, RSI, in->opcode);
	xMovI(e, RDX, in->address | c->thumb);
	callHelper(c, in, jitHelperExecute);
}

// Compiles LDM and STM. Transfers to work RAM are done inline; anything else goes to the exact CPU.
static void emitBlockTransfer(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	uint8_t* slow[40];
	int slowCount = 0;
	c->fastDoneCount = 0;
	c->video = NULL;
	// The fetch comes first, so I/O sees the time it happens at.
	clobberFlags(c);
	flushPending(c);
	if (!in->userBank && in->list && in->rn != 15 && (in->load || !(in->list & 0x8000)) && !storesMovedBase(in)) {
		emitFastTransfer(c, in, slow, &slowCount);
		for (int i = 0; i < slowCount; ++i) {
			patchHere(e, slow[i]);
		}
	}
	if (c->video) {
		c->video->exact = e->p;
	}
	emitExecute(c, in);
	if (loadsPc(in)) {
		setBiosPrefetchNext(c, in);
		xJmp(e, c->jit->dispatchKey);
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
	if (c->video) {
		c->video->join = e->p;
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
		xLoad(e, 4, RAX, M(R15, OFF(cpu.spsr)));
	} else {
		xCall(e, c->jit->spillRoutine);
		xLoad(e, 4, RAX, M(R15, OFF(cpu.cpsr)));
	}
	storeGuest(c, in->rd, RAX);
}

// Leaves for the exact CPU to run the instruction from here, as a careful copy's stub does for the
// instruction an event falls in; compiled code continues where it can.
static void emitStepExit(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	clobberFlags(c);
	flushPending(c);
	storeHead(c, in->address + 2 * c->width);
	xCall(e, c->jit->spillRoutine);
	xMovRR(e, 8, RDI, R15);
	xMovI(e, RSI, in->address);
	callAbs(e, jitHelperStepOne);
	xCall(e, c->jit->reloadRoutine);
	xAluI(e, X_CMP, 4, R(RAX), -1);
	uint8_t* leave = xJcc(e, CC_E, NULL);
	xJmp(e, c->jit->dispatchKey);
	patchHere(e, leave);
	xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_EXACT);
	xJmp(e, c->jit->exitRoutine);
}

// An instruction that writes the CPSR, run by the exact CPU. Compiled code continues after it, or
// wherever it went, unless the exact CPU has to take over.
static void emitSystem(struct Compiler* c, const struct Inst* in) {
	struct Emitter* e = &c->e;
	bool writesPc = in->kind != I_MSR && !(in->kind == I_ALU && isTest(in->op));
	storeHead(c, in->address + 3 * c->width);
	spill(c);
	xMovI(e, RSI, in->opcode);
	xMovI(e, RDX, in->address | c->thumb);
	callHelper(c, in, jitHelperSystem);
	xAluI(e, X_CMP, 4, R(RAX), (int32_t) JIT_EXACT);
	uint8_t* exact = xJcc(e, CC_E, NULL);
	if (writesPc) {
		setBiosPrefetchNext(c, in);
		xJmp(e, c->jit->dispatchKey);
	} else {
		emitLinkTo(c, in->address + c->width, c->thumb);
	}
	patchHere(e, exact);
	xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_EXACT);
	xJmp(e, c->jit->exitRoutine);
}

// BX to a register.
static void emitBx(struct Compiler* c, const struct Inst* in) {
	loadGuest(c, RAX, in->rm, in);
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
		loadGuest(c, RAX, 14, in);
		xLea(e, 4, RAX, M(RAX, in->target));
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
			flushPending(c);
			clobberFlags(c);
			emitLinkTo(c, in->address + c->width, c->thumb);
		}
		return;
	}
	if (!step) {
		pendFetch(c);
	}
	flushPending(c);
	uint8_t* skip = emitCondJump(c, in->cond, true);
	// Both paths continue from the flags in ebp.
	c->hostFlags = false;
	emitBody(c, in, index);
	if (isTerminator(in) || step) {
		// When skipped, it costs its fetch and the block continues past it.
		patchHere(e, skip);
		c->hostFlags = false;
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
	c->hostFlags = false;
}

// A word the game keeps rewriting: runs the code for the form it holds now, or leaves to compile a
// new form.
static void emitPoly(struct Compiler* c, const struct Inst* in, int index) {
	struct Emitter* e = &c->e;
	const struct Poly* poly = in->poly;
	clobberFlags(c);
	flushPending(c);
	int32_t word = c->region == REGION_IWRAM ? OFF(iwram) + (int32_t) (in->address & (SIZE_IWRAM - 1)) : OFF(ewram) + (int32_t) (in->address & (SIZE_EWRAM - 1));
	xLoad(e, 4, RAX, M(R15, word));
	uint8_t* to[MAX_FORMS];
	for (int i = 0; i < poly->count; ++i) {
		if (poly->masks[i] == ~0u) {
			xAluI(e, X_CMP, 4, R(RAX), (int32_t) poly->forms[i].opcode);
		} else {
			xMovRR(e, 4, RCX, RAX);
			xAluI(e, X_AND, 4, R(RCX), (int32_t) poly->masks[i]);
			xAluI(e, X_CMP, 4, R(RCX), (int32_t) (poly->forms[i].opcode & poly->masks[i]));
		}
		to[i] = xJcc(e, CC_E, NULL);
	}
	emitLinkTo(c, in->address, false);
	c->links[c->linkCount - 1].formMiss = true;
	uint8_t* join[MAX_FORMS];
	int joins = 0;
	for (int i = 0; i < poly->count; ++i) {
		const struct Inst* form = &poly->forms[i];
		patchHere(e, to[i]);
		resetPending(c);
		c->hostFlags = false;
		emitOne(c, form, index);
		if (isTerminator(form)) {
			continue;
		}
		clobberFlags(c);
		flushPending(c);
		join[joins++] = xJmp(e, NULL);
	}
	for (int i = 0; i < joins; ++i) {
		patchHere(e, join[i]);
	}
	c->hostFlags = false;
}

// Emits one instruction with what surrounds it: the entry point a careful copy needs before it, its
// condition's test, and the cycle counting after it.
static void emitInst(struct Compiler* c, int index) {
	const struct Inst* in = &c->insts[index];
	c->cur = index;
	if (c->careful) {
		// Enters here between instructions, with the flags in ebp and nothing pending.
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
			xAluI(&c->e, X_CMP, 1, M(R15, OFF(jitDeferred)), 0);
			c->deferred[c->deferredCount].site = xJcc(&c->e, CC_NE, NULL);
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
	xAluI(e, X_CMP, 4, M(R15, OFF(idleMark)), blockKey(c->insts[0].address, c->thumb));
	again[count++] = xJcc(e, CC_NE, NULL);
	xLoad(e, 4, RAX, M(R15, OFF(eventRuns)));
	xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(idleRuns)));
	again[count++] = xJcc(e, CC_NE, NULL);
	xLoad(e, 4, RAX, M(R15, OFF(cpu.nextEvent)));
	xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(idleNext)));
	again[count++] = xJcc(e, CC_NE, NULL);
	if (c->prefetch) {
		xLoad(e, 4, RAX, M(R15, OFF(jitBase)));
		xAluRR(e, X_SUB, 4, R(RAX), R14);
		xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(idleLead)));
		again[count++] = xJcc(e, CC_NE, NULL);
	}
	// ecx = one time round, eax = the cycles before the next event minus one.
	xMovRR(e, 4, RCX, R14);
	xAluRM(e, X_SUB, 4, RCX, M(R15, OFF(idleCycles)));
	again[count++] = xJcc(e, CC_LE, NULL);
	xMovRR(e, 4, RAX, R14);
	xNot(e, 4, R(RAX));
	for (int i = 0; i < c->idleLoadCount; ++i) {
		int base = c->idleLoads[i].base;
		if (hostReg[base] >= 0) {
			xLea(e, 4, RDX, M(hostReg[base], c->idleLoads[i].offset));
		} else {
			xLoad(e, 4, RDX, M(R15, REG(base)));
			xAluI(e, X_ADD, 4, R(RDX), c->idleLoads[i].offset);
		}
		xAluI(e, X_CMP, 4, R(RDX), REGION_EWRAM << 24);
		uint8_t* low = xJcc(e, CC_B, NULL);
		xAluI(e, X_CMP, 4, R(RDX), (REGION_IWRAM + 1) << 24);
		uint8_t* ram = xJcc(e, CC_B, NULL);
		patchHere(e, low);
		xAluI(e, X_AND, 4, R(RDX), ~3);
		xAluI(e, X_CMP, 4, R(RDX), (REGION_IO << 24) | REG_DISPSTAT);
		again[count++] = xJcc(e, CC_NE, NULL);
		xLoad(e, 4, RDX, M(R15, OFF(video.stableUntil)));
		xAluRM(e, X_CMP, 4, RDX, M(R15, OFF(idleStable)));
		again[count++] = xJcc(e, CC_NE, NULL);
		xAluRM(e, X_SUB, 4, RDX, M(R15, OFF(timing.masterCycles)));
		xAluRM(e, X_SUB, 4, RDX, M(R15, OFF(cpu.nextEvent)));
		xAluRR(e, X_SUB, 4, R(RDX), R14);
		xAluI(e, X_SUB, 4, R(RDX), 1);
		xAluRR(e, X_CMP, 4, R(RDX), RAX);
		uint8_t* later = xJcc(e, CC_GE, NULL);
		xMovRR(e, 4, RAX, RDX);
		patchHere(e, later);
		patchHere(e, ram);
	}
	xTest(e, 4, R(RAX), RAX);
	again[count++] = xJcc(e, CC_S, NULL);
	xAluRR(e, X_XOR, 4, R(RDX), RDX);
	xDiv(e, 4, R(RCX));
	xImul(e, 4, RAX, R(RCX));
	xAluRR(e, X_ADD, 4, R(R14), RAX);
	if (c->prefetch) {
		xAluRR(e, X_ADD, 4, M(R15, OFF(jitBase)), RAX);
	}
	for (int i = 0; i < count; ++i) {
		patchHere(e, again[i]);
	}
	xStoreI(e, 4, M(R15, OFF(idleMark)), 0);
	xStore(e, 4, M(R15, OFF(idleCycles)), R14);
	xLoad(e, 4, RAX, M(R15, OFF(eventRuns)));
	xStore(e, 4, M(R15, OFF(idleRuns)), RAX);
	xLoad(e, 4, RAX, M(R15, OFF(cpu.nextEvent)));
	xStore(e, 4, M(R15, OFF(idleNext)), RAX);
	if (c->prefetch) {
		xLoad(e, 4, RAX, M(R15, OFF(jitBase)));
		xAluRR(e, X_SUB, 4, R(RAX), R14);
		xStore(e, 4, M(R15, OFF(idleLead)), RAX);
	}
	if (c->idleLoadCount) {
		xLoad(e, 4, RAX, M(R15, OFF(video.stableUntil)));
		xStore(e, 4, M(R15, OFF(idleStable)), RAX);
	}
}

// An idle loop's head (jitIsIdleHead): once events have run since the run loop last looked at it,
// it leaves for the run loop to look again.
static void emitIdleHead(struct Compiler* c) {
	struct Emitter* e = &c->e;
	xLoad(e, 4, RAX, M(R15, OFF(eventRuns)));
	xAluRM(e, X_CMP, 4, RAX, M(R15, OFF(idleHeadRuns)));
	uint8_t* same = xJcc(e, CC_E, NULL);
	// As a deferred event's exit leaves things.
	uint32_t pc = c->insts[0].address;
	storeHead(c, pc + 2 * c->width);
	xStoreI(e, 4, M(R15, OFF(exitPc)), pc);
	xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_LOOKUP);
	xJmp(e, c->jit->exitRoutine);
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
	if (jit->codeEnd - jit->codeNext < 64 * 1024) {
		return false;
	}
	c.e = (struct Emitter) { .start = jit->codeNext, .p = jit->codeNext, .end = jit->codeEnd };
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
	uint8_t* cold = e->p;
	resetPending(&c);
	c.hostFlags = false;
	for (int i = 0; i < c.slowCount; ++i) {
		struct SlowAccess* slow = &c.slow[i];
		const struct Inst* in = slow->in;
		patchHere(e, slow->sites[0]);
		if (slow->sites[1]) {
			patchHere(e, slow->sites[1]);
		}
		if (!in->load && ramCode(&c)) {
			xStoreI(e, 4, M(R15, REG(15)), in->address + 2 * c.width);
		}
		xCall(e, memRoutine(&c, memKind(in)));
		jitRecordSite(block, e->p - block->entry, in->address, opcodeAt(&c, slow->index + 1), opcodeAt(&c, slow->index + 2));
		if (in->load) {
			storeGuest(&c, in->rd, RAX);
		}
		emitMissCheck(&c);
		xJmp(e, slow->resume);
	}
	for (int i = 0; i < c.videoCount; ++i) {
		// eax = the first address's region, ecx = the first address.
		struct VideoTransfer* t = &c.videoTransfers[i];
		patchHere(e, t->site);
		for (int region = REGION_PALETTE; region <= REGION_OAM; ++region) {
			xAluI(e, X_CMP, 4, R(RAX), region);
			uint8_t* other = xJcc(e, CC_NE, NULL);
			emitVideoTransfer(&c, t, region);
			patchHere(e, other);
		}
		xJmp(e, t->exact);
	}
	for (int i = 0; i < c.stubCount; ++i) {
		struct Stub* stub = &c.stubs[i];
		patchHere(e, stub->site);
		if (stub->pending) {
			xLea(e, 4, R14, M(R14, stub->pending));
		}
		if (!block->careful) {
			// Continues in the careful copy from the same instruction, once there is one.
			struct Link* link = &c.links[c.linkCount];
			c.linkSites[c.linkCount++] = xJmp(e, NULL);
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
			xCall(e, jit->spillRoutine);
			xMovRR(e, 8, RDI, R15);
			xMovI(e, RSI, stub->cost);
			xMovI(e, RDX, stub->deferrable);
			xMovI64(e, RCX, (uint64_t) (uintptr_t) block);
			callAbs(e, jitHelperCareful);
			xCall(e, jit->reloadRoutine);
			xAluI(e, X_CMP, 4, R(RAX), CAREFUL_RUN);
			xJcc(e, CC_E, stub->resume);
			xAluI(e, X_CMP, 4, R(RAX), CAREFUL_LEAVE);
			leaveHere = xJcc(e, CC_E, NULL);
		}
		// The exact CPU runs the instruction an event falls in; compiled code continues after it,
		// here if the copy is still there.
		xCall(e, jit->spillRoutine);
		xMovRR(e, 8, RDI, R15);
		xMovI(e, RSI, stub->pc);
		callAbs(e, jitHelperStepOne);
		xCall(e, jit->reloadRoutine);
		xAluI(e, X_CMP, 4, R(RAX), -1);
		uint8_t* leave = xJcc(e, CC_E, NULL);
		if (stub->index + 1 < count) {
			xAluI(e, X_CMP, 4, R(RAX), (int32_t) blockKey(stub->pc + c.width, c.thumb));
			uint8_t* elsewhere = xJcc(e, CC_NE, NULL);
			xMovI64(e, RCX, (uint64_t) (uintptr_t) &block->valid);
			xAluI(e, X_CMP, 1, M(RCX, 0), 0);
			uint8_t* gone = xJcc(e, CC_E, NULL);
			xJmp(e, block->entries[stub->index + 1]);
			patchHere(e, elsewhere);
			patchHere(e, gone);
		}
		xJmp(e, jit->dispatchKey);
		patchHere(e, leave);
		xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_EXACT);
		xJmp(e, jit->exitRoutine);
		if (leaveHere) {
			// Leaves between instructions, for the run loop.
			patchHere(e, leaveHere);
			xStoreI(e, 4, M(R15, OFF(exitPc)), stub->pc);
			xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_LOOKUP);
			xJmp(e, jit->exitRoutine);
		}
	}
	for (int i = 0; i < c.deferredCount; ++i) {
		// The events inside the instruction just done.
		int index = c.deferred[i].index;
		uint32_t next = c.insts[0].address + (index + 1) * c.width;
		patchHere(e, c.deferred[i].site);
		storeHead(&c, next + 2 * c.width);
		xCall(e, jit->spillRoutine);
		xMovRR(e, 8, RDI, R15);
		xMovI64(e, RSI, (uint64_t) (uintptr_t) block);
		callAbs(e, jitHelperDeferred);
		xCall(e, jit->reloadRoutine);
		xTest(e, 4, R(RAX), RAX);
		xJcc(e, CC_E, block->entries[index + 1]);
		xStoreI(e, 4, M(R15, OFF(exitPc)), next);
		xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_LOOKUP);
		xJmp(e, jit->exitRoutine);
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
		xStoreI(e, 4, M(R15, OFF(exitPc)), link->targetKey);
		xMovI64(e, RAX, (uint64_t) (uintptr_t) link);
		xStore(e, 8, M(R15, OFF(exitLink)), RAX);
		xStoreI(e, 4, M(R15, OFF(exitReason)), EXIT_LINK);
		xJmp(e, jit->exitRoutine);
	}
	if (e->p > e->end) {
		// It ran past the end of the code memory, writing nothing there; the caller flushes.
		block->siteCount = 0;
		free(block->out);
		block->out = NULL;
		block->outCount = 0;
		return false;
	}
	for (int i = 0; i < block->outCount; ++i) {
		patchRel32(block->out[i].site, block->out[i].stub);
	}
	jitNameBlock(block, cold, cold, e->p);
	jit->codeNext = e->p;
	return true;
}
