/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The exact CPU: one instruction at a time, with its bus accesses in the order the ARM7TDMI makes
// them. Each access moves the clock on by its own cycles, and events due by then happen before it
// reads or writes, so I/O sees what the hardware would. IRQs are taken between instructions.
// Compiled code runs where no event can fall inside it; this CPU runs the instructions around
// events.

#include "decode.h"
#include "gba.h"
#include "jit-internal.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t ror32(uint32_t value, unsigned rotate) {
	rotate &= 31;
	return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

// After a PC write, the new PC's fetch is nonsequential and the next one sequential.
void stepReload(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	bool thumb = cpu->cpsr & PSR_T;
	int width = thumb ? 2 : 4;
	// As in mGBA, an ARM-state PC keeps bit 1: fetches ignore it, but PC reads see it.
	uint32_t pc = cpu->r[15] & ~1u;
	memorySetActiveRegion(gba, pc);
	cpu->fetchSeq = false;
	if ((pc >> 24) < REGION_ROM0 || (pc >> 24) > REGION_ROM2_EX || !busRefillCartridge(gba, pc, thumb)) {
		cpu->prefetch[0] = busFetch(gba, pc, thumb);
		cpu->prefetch[1] = busFetch(gba, pc + width, thumb);
	}
	cpu->r[15] = pc + width;
	cpu->irqMasked = cpu->cpsr & PSR_I;
}

// Refills the prefetch from memory as it stands, at no cost
static void refetch(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	if (cpu->cpsr & PSR_T) {
		cpu->prefetch[0] = codeFetch16(gba, cpu->r[15] - 2);
		cpu->prefetch[1] = codeFetch16(gba, cpu->r[15]);
	} else {
		cpu->prefetch[0] = codeFetch32(gba, cpu->r[15] - 4);
		cpu->prefetch[1] = codeFetch32(gba, cpu->r[15]);
	}
}

// Enters an exception: the mode's banked registers come in, the old CPSR goes to its SPSR, IRQs are
// masked, ARM state is entered, and the PC goes to the vector.
static void exception(struct GBA* gba, unsigned mode, uint32_t vector, uint32_t lr) {
	struct CPU* cpu = &gba->cpu;
	uint32_t cpsr = cpu->cpsr;
	cpuSetPrivilegeMode(gba, mode);
	cpu->cpsr = (cpsr & ~(0x1Fu | PSR_T)) | mode | PSR_I;
	cpu->spsr = cpsr;
	cpu->r[14] = lr;
	cpu->r[15] = vector;
	stepReload(gba);
}

// At an instruction boundary, r15 points past the next instruction.
static void takeIrq(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	bool thumb = cpu->cpsr & PSR_T;
	int width = thumb ? 2 : 4;
	// The fetch already under way is thrown away.
	busFetch(gba, cpu->r[15] + width, thumb);
	exception(gba, MODE_IRQ, 0x18, cpu->r[15] - width + 4);
}

// Whether the flags satisfy an instruction's condition field
static bool passes(uint32_t cpsr, int cond) {
	bool n = cpsr & PSR_N;
	bool z = cpsr & PSR_Z;
	bool c = cpsr & PSR_C;
	bool v = cpsr & PSR_V;
	switch (cond) {
	case 0x0:
		return z;
	case 0x1:
		return !z;
	case 0x2:
		return c;
	case 0x3:
		return !c;
	case 0x4:
		return n;
	case 0x5:
		return !n;
	case 0x6:
		return v;
	case 0x7:
		return !v;
	case 0x8:
		return c && !z;
	case 0x9:
		return !c || z;
	case 0xA:
		return n == v;
	case 0xB:
		return n != v;
	case 0xC:
		return !z && n == v;
	case 0xD:
		return z || n != v;
	case 0xE:
		return true;
	default:
		return false;
	}
}

// Addressing mode 1.
static uint32_t shifter(struct GBA* gba, const struct Operand* op, bool* carry) {
	struct CPU* cpu = &gba->cpu;
	bool c = cpu->cpsr & PSR_C;
	if (op->isImm) {
		*carry = op->immRotated ? op->imm >> 31 : c;
		return op->imm;
	}
	uint32_t v = cpu->r[op->rm];
	if (op->byReg) {
		// The shift takes a cycle, by which time the PC has moved on.
		if (op->rm == 15) {
			v += 4;
		}
		unsigned s = cpu->r[op->rs] & 0xFF;
		if (!s) {
			*carry = c;
			return v;
		}
		switch (op->type) {
		case SHIFT_LSL:
			if (s < 32) {
				*carry = (v >> (32 - s)) & 1;
				return v << s;
			}
			*carry = s == 32 ? v & 1 : 0;
			return 0;
		case SHIFT_LSR:
			if (s < 32) {
				*carry = (v >> (s - 1)) & 1;
				return v >> s;
			}
			*carry = s == 32 ? v >> 31 : 0;
			return 0;
		case SHIFT_ASR:
			if (s < 32) {
				*carry = (v >> (s - 1)) & 1;
				return (int32_t) v >> s;
			}
			*carry = v >> 31;
			return (int32_t) v >> 31;
		default:
			if (s & 31) {
				*carry = (v >> ((s & 31) - 1)) & 1;
				return ror32(v, s);
			}
			*carry = v >> 31;
			return v;
		}
	}
	unsigned a = op->amount;
	switch (op->type) {
	case SHIFT_LSL:
		if (!a) {
			*carry = c;
			return v;
		}
		*carry = (v >> (32 - a)) & 1;
		return v << a;
	case SHIFT_LSR:
		if (!a) {
			*carry = v >> 31;
			return 0;
		}
		*carry = (v >> (a - 1)) & 1;
		return v >> a;
	case SHIFT_ASR:
		if (!a) {
			*carry = v >> 31;
			return (int32_t) v >> 31;
		}
		*carry = (v >> (a - 1)) & 1;
		return (int32_t) v >> a;
	default:
		if (!a) {
			*carry = v & 1;
			return ((uint32_t) c << 31) | (v >> 1);
		}
		*carry = (v >> (a - 1)) & 1;
		return ror32(v, a);
	}
}

// Addressing mode 2: an offset register shifted by a constant.
static uint32_t offsetOf(struct GBA* gba, const struct Operand* op) {
	if (op->isImm) {
		return op->imm;
	}
	uint32_t v = gba->cpu.r[op->rm];
	unsigned a = op->amount;
	switch (op->type) {
	case SHIFT_LSL:
		return v << a;
	case SHIFT_LSR:
		return a ? v >> a : 0;
	case SHIFT_ASR:
		return (int32_t) v >> (a ? a : 31);
	default:
		return a ? ror32(v, a) : ((uint32_t) !!(gba->cpu.cpsr & PSR_C) << 31) | (v >> 1);
	}
}

// TST, TEQ, CMP and CMN set flags and write no register.
static bool isTest(int op) {
	return op >= ALU_TST && op <= ALU_CMN;
}

// Sets N and Z from a result; C and V are left for the operation to set.
static void setNz(struct CPU* cpu, uint32_t d) {
	cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z)) | (d & PSR_N) | (d ? 0 : PSR_Z);
}

// Runs a data-processing instruction. A shift by a register takes an internal cycle, and the fetch
// after it is then nonsequential.
static void alu(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	bool byReg = !in->op2.isImm && in->op2.byReg;
	if (byReg) {
		busIdle(gba);
		cpu->fetchSeq = false;
	}
	bool carry;
	uint32_t m = shifter(gba, &in->op2, &carry);
	uint32_t n = cpu->r[in->rn];
	// With a register shift, the PC is read a cycle later, so it is 12 ahead instead of 8.
	if (!in->thumb && in->rn == 15 && byReg) {
		n += 4;
	}
	uint32_t c = !!(cpu->cpsr & PSR_C);
	bool arithmetic = true;
	bool cf = false;
	bool vf = false;
	uint32_t d;
	switch (in->op) {
	case ALU_AND:
	case ALU_TST:
		d = n & m;
		arithmetic = false;
		break;
	case ALU_EOR:
	case ALU_TEQ:
		d = n ^ m;
		arithmetic = false;
		break;
	case ALU_SUB:
	case ALU_CMP:
		d = n - m;
		cf = n >= m;
		vf = ((n ^ m) & (n ^ d)) >> 31;
		break;
	case ALU_RSB:
		d = m - n;
		cf = m >= n;
		vf = ((m ^ n) & (m ^ d)) >> 31;
		break;
	case ALU_ADD:
	case ALU_CMN:
		d = n + m;
		cf = d < n;
		vf = (~(n ^ m) & (n ^ d)) >> 31;
		break;
	case ALU_ADC:
		d = n + m + c;
		cf = (uint64_t) n + m + c > 0xFFFFFFFFu;
		vf = (~(n ^ m) & (n ^ d)) >> 31;
		break;
	case ALU_SBC:
		d = n - m - !c;
		cf = (uint64_t) n >= (uint64_t) m + !c;
		vf = ((n ^ m) & (n ^ d)) >> 31;
		break;
	case ALU_RSC:
		d = m - n - !c;
		cf = (uint64_t) m >= (uint64_t) n + !c;
		vf = ((m ^ n) & (m ^ d)) >> 31;
		break;
	case ALU_ORR:
		d = n | m;
		arithmetic = false;
		break;
	case ALU_MOV:
		d = m;
		arithmetic = false;
		break;
	case ALU_BIC:
		d = n & ~m;
		arithmetic = false;
		break;
	default:
		d = ~m;
		arithmetic = false;
		break;
	}
	bool test = isTest(in->op);
	if (!test) {
		cpu->r[in->rd] = d;
	}
	if (in->setFlags) {
		if (in->rd == 15 && !in->thumb && cpuModeHasSpsr(cpu->cpsr & 0x1F)) {
			cpuWriteCpsr(gba, cpu->spsr);
		} else {
			setNz(cpu, d);
			if (arithmetic) {
				cpu->cpsr = (cpu->cpsr & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
			} else {
				cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
			}
		}
	}
	if (in->rd != 15) {
		return;
	}
	if (!test) {
		stepReload(gba);
	} else if (!in->thumb) {
		// The next instruction runs in whatever state the SPSR gave, without a refill.
		cpu->r[15] = in->address + 4 + ((cpu->cpsr & PSR_T) ? 2 : 4);
		refetch(gba);
	}
}

// The multiplier stops early once the rest of the operand is all zeros (or all ones, when signed).
static int multiplyCycles(uint32_t value, bool sign) {
	if (sign) {
		value ^= (uint32_t) ((int32_t) value >> 31);
	}
	return (31 - __builtin_clz(value | 1)) / 8 + 1;
}

// MUL and MLA: one internal cycle for each byte of the multiplier the early-out doesn't skip, and
// one more to accumulate. The carry flag comes out as the hardware's multiplier leaves it.
static void mul(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	cpu->fetchSeq = false;
	if (in->rd == 15 || (in->accumulate && in->rn == 15)) {
		return;
	}
	// Thumb's MUL multiplies by the destination's old value; ARM's by Rs.
	uint32_t multiplier = in->thumb ? cpu->r[in->rd] : cpu->r[in->rs];
	uint32_t multiplicand = in->thumb ? cpu->r[in->rs] : cpu->r[in->rm];
	uint32_t accum = in->accumulate ? cpu->r[in->rn] : 0;
	int stages = multiplyCycles(multiplier, true);
	uint32_t d = multiplicand * multiplier + accum;
	for (int i = 0; i < stages + in->accumulate; ++i) {
		busIdle(gba);
	}
	cpu->r[in->rd] = d;
	if (in->setFlags) {
		setNz(cpu, d);
		bool carry = stages == 4 ? cpuMultiplyCarryFull(multiplier) : cpuMultiplyCarryLow(multiplicand, multiplier, accum);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
	}
}

// The 64-bit multiplies, which take one cycle more than the 32-bit ones.
static void mulLong(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	cpu->fetchSeq = false;
	if (in->rd == 15 || in->rdHi == 15) {
		return;
	}
	uint32_t multiplicand = cpu->r[in->rm];
	uint32_t multiplier = cpu->r[in->rs];
	uint32_t accumLo = in->accumulate ? cpu->r[in->rd] : 0;
	uint32_t accumHi = in->accumulate ? cpu->r[in->rdHi] : 0;
	int stages = multiplyCycles(multiplier, in->signedMul);
	uint64_t d;
	if (in->signedMul) {
		d = (uint64_t) ((int64_t) (int32_t) multiplicand * (int32_t) multiplier);
	} else {
		d = (uint64_t) multiplicand * multiplier;
	}
	d += ((uint64_t) accumHi << 32) | accumLo;
	for (int i = 0; i < stages + 1 + in->accumulate; ++i) {
		busIdle(gba);
	}
	cpu->r[in->rd] = d;
	cpu->r[in->rdHi] = d >> 32;
	if (in->setFlags) {
		bool carry = stages == 4 ? cpuMultiplyCarryHigh(multiplicand, multiplier, accumHi, in->signedMul) : cpuMultiplyCarryLow(multiplicand, multiplier, accumLo);
		cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z | PSR_C)) | ((d >> 32) & PSR_N) | (d ? 0 : PSR_Z) | (carry ? PSR_C : 0);
	}
}

// Runs a single load or store. The base is written back after the access; a load then takes an
// internal cycle before the register is written, which is when a loaded PC takes effect.
static void mem(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	uint32_t base = cpu->r[in->rn];
	if (in->thumb && in->rn == 15) {
		base &= ~3u;
	}
	uint32_t offset = offsetOf(gba, &in->op2);
	uint32_t moved = in->up ? base + offset : base - offset;
	uint32_t address = in->pre ? moved : base;
	cpu->fetchSeq = false;
	if (in->load) {
		uint32_t value = busRead(gba, address, in->size, false);
		if (in->signExtend) {
			// A signed halfword load from an odd address reads the byte there, sign-extended.
			if (in->size == 1 || (address & 1)) {
				value = (uint32_t) (int8_t) value;
			} else {
				value = (uint32_t) (int16_t) value;
			}
		}
		if (in->writeback) {
			cpu->r[in->rn] = moved;
		}
		busIdle(gba);
		cpu->r[in->rd] = value;
		if (in->rd == 15) {
			stepReload(gba);
		}
		return;
	}
	uint32_t value = cpu->r[in->rd];
	if (in->rd == 15) {
		// A stored PC is 12 ahead of the instruction.
		value += 4;
	}
	busWrite(gba, address, value, in->size, false);
	if (in->writeback) {
		cpu->r[in->rn] = moved;
	}
}

// SWP: a read and a write of the same address with nothing able to come between them, then an
// internal cycle.
static void swap(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	uint32_t address = cpu->r[in->rn];
	cpu->fetchSeq = false;
	uint32_t value = busRead(gba, address, in->size, false);
	busWrite(gba, address, cpu->r[in->rm], in->size, false);
	busIdle(gba);
	cpu->r[in->rd] = value;
}

// LDM and STM transfer upward from the lowest address. The base is written back after the first
// register, so a later STM sees it moved and a loaded base wins.
static void block(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	uint16_t list = in->list;
	// An empty list moves the base as sixteen registers would, and transfers r15.
	int count = list ? __builtin_popcount(list) : 16;
	if (!list) {
		list = 0x8000;
	}
	uint32_t base = cpu->r[in->rn];
	uint32_t moved = in->up ? base + 4 * count : base - 4 * count;
	uint32_t address = in->up ? base : moved;
	bool pre = in->up ? in->pre : !in->pre;
	unsigned mode = cpu->mode;
	bool userBank = in->userBank && !(in->load && (list & 0x8000)) && cpuModeHasSpsr(mode);
	if (userBank) {
		cpuSetPrivilegeMode(gba, MODE_SYSTEM);
	}
	cpu->fetchSeq = false;
	bool seq = false;
	for (int i = 0; i < 16; ++i) {
		if (!(list & (1 << i))) {
			continue;
		}
		if (pre) {
			address += 4;
		}
		uint32_t at = (address >> 24) < REGION_SRAM ? address & ~3u : address;
		if (in->load) {
			uint32_t value = busRead(gba, at, 4, seq);
			if (in->writeback && !seq) {
				cpu->r[in->rn] = moved;
			}
			cpu->r[i] = value;
		} else {
			uint32_t value = cpu->r[i];
			if (i == 15) {
				value += in->thumb ? 2 : 4;
			}
			busWrite(gba, at, value, 4, seq);
			if (in->writeback && !seq) {
				cpu->r[in->rn] = moved;
			}
		}
		if (!pre) {
			address += 4;
		}
		seq = true;
	}
	if (in->load) {
		busIdle(gba);
	}
	if (userBank) {
		cpuSetPrivilegeMode(gba, mode);
	}
	if (in->load && (list & 0x8000)) {
		if (in->userBank && cpuModeHasSpsr(cpu->cpsr & 0x1F)) {
			cpuWriteCpsr(gba, cpu->spsr);
		}
		stepReload(gba);
	}
}

// BX: bit 0 of the target picks ARM or Thumb.
static void bx(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	uint32_t target = cpu->r[in->rm];
	if (target & 1) {
		cpu->cpsr |= PSR_T;
	} else {
		cpu->cpsr &= ~PSR_T;
	}
	cpu->r[15] = target & ~1u;
	stepReload(gba);
}

// MSR writes the CPSR or the SPSR, in the fields its mask names.
static void msr(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	uint32_t value = in->op2.isImm ? in->op2.imm : cpu->r[in->op2.rm];
	jitHelperMsr(gba, value, in->fieldMask | (in->spsr ? 0x100 : 0));
	if (in->spsr) {
		return;
	}
	// mGBA refetches after writing the CPSR; after switching to Thumb, a NOP fills the gap.
	if (cpu->cpsr & PSR_T) {
		cpu->prefetch[0] = 0x46C0;
		cpu->prefetch[1] &= 0xFFFF;
		cpu->r[15] += 2;
	} else {
		refetch(gba);
	}
}

// Runs one decoded instruction whose fetch has been done and whose condition passed. Also called
// straight from compiled code for the instructions it leaves to this CPU.
void stepExecute(struct GBA* gba, const struct Inst* in) {
	struct CPU* cpu = &gba->cpu;
	int width = in->thumb ? 2 : 4;
	switch (in->kind) {
	case I_ALU:
		alu(gba, in);
		break;
	case I_MUL:
		mul(gba, in);
		break;
	case I_MULL:
		mulLong(gba, in);
		break;
	case I_MEM:
		mem(gba, in);
		break;
	case I_BLOCK:
		block(gba, in);
		break;
	case I_SWP:
		swap(gba, in);
		break;
	case I_MRS:
		cpu->r[in->rd] = in->spsr ? cpu->spsr : cpu->cpsr;
		break;
	case I_MSR:
		msr(gba, in);
		break;
	case I_B:
		if (in->link) {
			cpu->r[14] = in->address + 4;
		}
		cpu->r[15] = in->target;
		stepReload(gba);
		break;
	case I_BL1:
		cpu->r[14] = in->target;
		break;
	case I_BL2: {
		uint32_t target = cpu->r[14] + in->target;
		cpu->r[14] = (in->address + 2) | 1;
		cpu->r[15] = target;
		stepReload(gba);
		break;
	}
	case I_BX:
		bx(gba, in);
		break;
	case I_SWI:
		exception(gba, MODE_SUPERVISOR, 0x08, in->address + width);
		break;
	default:
		exception(gba, MODE_UNDEFINED, 0x04, in->address + width);
		break;
	}
}

// For debugging: writes the PC and time of each instruction stepped, from a given PC or time
// onwards (TRACE_FROM, TRACE_TIME), for TRACE_COUNT instructions.
void stepTraceOpen(struct GBA* gba, const char* path) {
	gba->traceFile = fopen(path, "w");
	gba->traceFromPc = getenv("TRACE_FROM") ? strtoul(getenv("TRACE_FROM"), NULL, 16) : ~0u;
	gba->traceFromTime = getenv("TRACE_TIME") ? (int32_t) strtoul(getenv("TRACE_TIME"), NULL, 10) : 0;
	gba->traceLeft = getenv("TRACE_COUNT") ? atol(getenv("TRACE_COUNT")) : 1000000;
	gba->tracing = !getenv("TRACE_FROM") && !getenv("TRACE_TIME");
}

void stepTrace(struct GBA* gba) {
	if (!gba->traceFile || !gba->traceLeft) {
		return;
	}
	struct CPU* cpu = &gba->cpu;
	uint32_t pc = cpu->r[15] - ((cpu->cpsr & PSR_T) ? 2 : 4);
	int32_t now = timingCurrentTime(gba);
	if (pc == gba->traceFromPc || (gba->traceFromTime && now - gba->traceFromTime >= 0)) {
		gba->tracing = true;
	}
	if (gba->tracing) {
		fprintf(gba->traceFile, "%08X %u\n", pc, (uint32_t) now);
		if (!--gba->traceLeft) {
			fclose(gba->traceFile);
			gba->traceFile = NULL;
		}
	}
}

// Runs one instruction: an IRQ is taken first if one is waiting, then the pipeline moves on a place
// (the fetch for two instructions ahead goes out on the bus now), and the instruction at its head
// runs.
static void step(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	if (gba->irq.line && !cpu->irqMasked) {
		takeIrq(gba);
	}
	if (gba->traceFile) {
		stepTrace(gba);
	}
	bool thumb = cpu->cpsr & PSR_T;
	uint32_t opcode = cpu->prefetch[0];
	// The I flag as it is before the instruction decides whether an IRQ is taken after it.
	cpu->irqMasked = cpu->cpsr & PSR_I;
	cpu->prefetch[0] = cpu->prefetch[1];
	struct Inst in;
	if (thumb) {
		cpu->r[15] += 2;
		cpu->prefetch[1] = busFetch(gba, cpu->r[15], true);
		decodeThumb(&in, cpu->r[15] - 4, (uint16_t) opcode);
	} else {
		cpu->r[15] += 4;
		cpu->prefetch[1] = busFetch(gba, cpu->r[15], false);
		decodeArm(&in, cpu->r[15] - 8, opcode);
	}
	if (in.cond == COND_AL || passes(cpu->cpsr, in.cond)) {
		stepExecute(gba, &in);
	}
}

// After a reset or a loaded state, fills the pipeline from memory at no cost.
void stepReset(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	refetch(gba);
	cpu->fetchSeq = true;
	cpu->irqMasked = cpu->cpsr & PSR_I;
}

// Steps one instruction
void stepInstruction(struct GBA* gba) {
	step(gba);
}
