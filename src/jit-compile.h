/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_COMPILE_H
#define ARMGBA_JIT_COMPILE_H

// The parts of compiling a block that don't depend on the host CPU: what instructions cost, which
// flags are live, how the prefetch buffer's timing is counted, which branch targets compiled code
// refills from, and which loops wait on memory. Each backend includes this after its struct
// Compiler, which has the fields these use (gba, jit, block, insts, count, nextOps, thumb, width,
// region, S, N, P, prefetch, D, full, Ncyc, careful, idleLoads, idleLoadCount, slowCount, liveAfter,
// checkpoint, pending, pendB, pendC, pendD).

// The flags, one bit each.
enum {
	F_V = 1,
	F_C = 2,
	F_Z = 4,
	F_N = 8,
	F_ALL = 15,
};

// Where a shifter's carry comes from: kept, known when compiling, or worked out when it runs.
enum {
	CARRY_SAME,
	CARRY_0,
	CARRY_1,
	CARRY_REG,
};

// A term not known when compiling.
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

// A block transfer's first address, counted from its base register.
static int32_t transferStart(const struct Inst* in) {
	int count = __builtin_popcount(in->list);
	return in->up ? (in->pre ? 4 : 0) : (in->pre ? -4 * count : -4 * count + 4);
}

// Where writeback leaves the base register, counted from the first address.
static int32_t transferWriteback(const struct Inst* in) {
	int count = __builtin_popcount(in->list);
	return (in->up ? 4 * count : -4 * count) - transferStart(in);
}

// The cycles of a block transfer that RAM or video memory serves inline: the same wait for every
// word, nonsequential or not, and a load's internal cycle.
static int transferCycles(const struct Compiler* c, const struct Inst* in, int region) {
	return __builtin_popcount(in->list) * (1 + c->gba->waitS32[region]) + (in->load ? 1 : 0);
}

// Where palette RAM, VRAM or OAM is in struct GBA.
static int32_t videoMemory(int region) {
	return region == REGION_VRAM ? OFF(vram) : region == REGION_OAM ? OFF(oam) : OFF(palette);
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
static inline uint8_t aluWrites(const struct Inst* in) {
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

// Returns what a check before instruction index covers: up to the next check, which comes after the
// next dynamic instruction.
static int ahead(struct Compiler* c, int index) {
	// After an access that stopped the prefetch buffer, the first fetch is nonsequential.
	int cycles = c->prefetch ? most(0, c->Ncyc - c->D) : 0;
	for (int j = index; j < c->count; ++j) {
		const struct Inst* in = &c->insts[j];
		if (in->poly) {
			int highest = 0;
			for (int i = 0; i < in->poly->count; ++i) {
				highest = most(highest, checkedCost(c, &in->poly->forms[i], j));
			}
			cycles += highest;
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
	int highest = 0;
	for (int i = 0; i < in->poly->count; ++i) {
		highest = most(highest, checkedCost(c, &in->poly->forms[i], index));
	}
	return cycles + highest;
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

#endif
