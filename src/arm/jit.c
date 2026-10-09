/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/jit.h>

#ifdef M_ARM_JIT

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/memory.h>

#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>

// Blocks run ARM and Thumb code from BIOS, EWRAM, IWRAM and the cartridge. Every guest
// instruction keeps the interpreter's
// exact state: PC, the two prefetched words, cycles, and an event check afterwards.

#define MAX_BLOCK 64
#define MAX_SPAN ARM_JIT_MAX_SPAN
#define HOT_THRESHOLD 2
#define PATCH_LIMIT 4
#define CODE_SIZE (32 * 1024 * 1024)
#define MAX_INSN_BYTES 640
#define EXIT_DIRECT -1
#define EXIT_TO_C -2
#define MAX_EXITS (MAX_BLOCK * 6)

struct ARMJitBlock {
	void* entry;
	uint32_t pc;
	uint32_t op0;
	uint32_t op1;
	bool thumb;
	int coverStart;
	unsigned coverWords;
	// Patched words are read at runtime, so writes to them leave the block alone
	uint32_t coverMask[3];
};

struct ARMJitPage {
	struct ARMJitBlock* blocks[0x800];
	uint8_t hits[0x800];
};

static const uint16_t _conditionLut[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

enum {
	OFF_PC = offsetof(struct ARMCore, gprs) + 4 * ARM_PC,
	OFF_CPSR = offsetof(struct ARMCore, cpsr),
	OFF_CYCLES = offsetof(struct ARMCore, cycles),
	OFF_NEXT_EVENT = offsetof(struct ARMCore, nextEvent),
	OFF_PREFETCH0 = offsetof(struct ARMCore, prefetch),
	OFF_PREFETCH1 = offsetof(struct ARMCore, prefetch) + 4,
	OFF_SHIFTER_CARRY = offsetof(struct ARMCore, shifterCarryOut),
	OFF_MEMORY = offsetof(struct ARMCore, memory),
	OFF_EXECUTION_MODE = offsetof(struct ARMCore, executionMode),
	JIT_CURRENT = offsetof(struct ARMJit, current),
	JIT_SMC_HIT = offsetof(struct ARMJit, smcHit),
	JIT_PAGES = offsetof(struct ARMJit, pages),
	JIT_FETCHED = offsetof(struct ARMJit, fetched),
	BLOCK_ENTRY = offsetof(struct ARMJitBlock, entry),
	BLOCK_PC = offsetof(struct ARMJitBlock, pc),
	BLOCK_OP0 = offsetof(struct ARMJitBlock, op0),
	BLOCK_OP1 = offsetof(struct ARMJitBlock, op1),
	BLOCK_THUMB = offsetof(struct ARMJitBlock, thumb),
};

enum {
	ALU_AND, ALU_EOR, ALU_SUB, ALU_RSB, ALU_ADD, ALU_ADC, ALU_SBC, ALU_RSC,
	ALU_TST, ALU_TEQ, ALU_CMP, ALU_CMN, ALU_ORR, ALU_MOV, ALU_BIC, ALU_MVN
};

struct Emitter {
	uint8_t* p;
};

struct Compiler {
	struct Emitter e;
	struct ARMJit* jit;
	struct ARMCore* cpu;
	struct GBA* gba;
	uint32_t pc;
	const uint32_t* ops;
	// Patched instructions and prefetch words, decoded at runtime from jit->fetched
	const bool* hot;
	unsigned count;
	bool thumb;
	unsigned width;
	// Data accesses from cartridge code go through the memory handlers for the prefetch buffer
	bool romCode;
	uint8_t* body;
	uint32_t aluCycles;
	uint32_t memCycles;
	struct {
		uint8_t* at;
		int index;
	} exits[MAX_EXITS];
	unsigned nExits;
};

static void _exitAt(struct Compiler* c, uint8_t* at, int index) {
	c->exits[c->nExits].at = at;
	c->exits[c->nExits].index = index;
	++c->nExits;
}

static bool _isLogical(unsigned opcode) {
	return opcode == ALU_AND || opcode == ALU_EOR || opcode == ALU_TST || opcode == ALU_TEQ ||
	       opcode == ALU_ORR || opcode == ALU_MOV || opcode == ALU_BIC || opcode == ALU_MVN;
}

// ARM and Thumb instructions decode to the same operations. A source is a guest register
// or a value known when compiling (the PC).
struct Source {
	bool constant;
	uint32_t value;
	unsigned reg;
};

struct AluOp {
	unsigned cond;
	unsigned opcode;
	bool s;
	unsigned rd;
	struct Source n;
	// Second operand: an immediate, or a register shifted by an immediate amount
	bool immediate;
	uint32_t imm;
	// Carry out of an immediate: -1 keeps C, otherwise 0 or 1
	int immCarry;
	struct Source m;
	unsigned shiftType;
	unsigned shiftAmount;
	// ARM keeps the shifter carry for MULS and MLAS; Thumb never reads it
	bool keepsShifterCarry;
	// Whether any flag this sets is read before another instruction overwrites it
	bool flagsLive;
	uint32_t cycles;
};

struct MemOp {
	unsigned cond;
	bool load;
	unsigned size;
	bool signExtend;
	unsigned rd;
	struct Source base;
	bool immediateOffset;
	uint32_t offset;
	struct Source m;
	unsigned shiftType;
	unsigned shiftAmount;
	bool up;
	bool pre;
	bool writeback;
};

static struct Source _reg(unsigned reg) {
	struct Source source = { false, 0, reg };
	return source;
}

static struct Source _const(uint32_t value) {
	struct Source source = { true, value, 0 };
	return source;
}

static bool _decodeArmAlu(uint32_t op, uint32_t address, uint32_t aluCycles, struct AluOp* alu) {
	if ((op & 0x0C000000) || (op >> 28) == 0xF) {
		return false;
	}
	if (!(op & 0x02000000) && (op & 0x10)) {
		return false; // Register-specified shift, multiply, halfword transfers, BX
	}
	unsigned opcode = (op >> 21) & 0xF;
	if (opcode >= ALU_TST && opcode <= ALU_CMN && !(op & 0x00100000)) {
		return false; // MRS, MSR
	}
	if (((op >> 12) & 0xF) == ARM_PC) {
		return false;
	}
	unsigned rn = (op >> 16) & 0xF;
	unsigned rm = op & 0xF;
	memset(alu, 0, sizeof(*alu));
	alu->cond = op >> 28;
	alu->opcode = opcode;
	alu->s = op & 0x00100000;
	alu->rd = (op >> 12) & 0xF;
	alu->n = rn == ARM_PC ? _const(address + 8) : _reg(rn);
	if (op & 0x02000000) {
		unsigned rotate = (op >> 7) & 0x1E;
		alu->immediate = true;
		alu->imm = op & 0xFF;
		alu->immCarry = -1;
		if (rotate) {
			alu->imm = (alu->imm >> rotate) | (alu->imm << (32 - rotate));
			alu->immCarry = alu->imm >> 31;
		}
	} else {
		alu->m = rm == ARM_PC ? _const(address + 8) : _reg(rm);
		alu->shiftType = (op >> 5) & 3;
		alu->shiftAmount = (op >> 7) & 0x1F;
	}
	alu->keepsShifterCarry = true;
	alu->cycles = aluCycles;
	return true;
}

static void _thumbAlu(struct AluOp* alu, unsigned opcode, bool s, unsigned rd, struct Source n, uint32_t cycles) {
	memset(alu, 0, sizeof(*alu));
	alu->cond = 0xE;
	alu->opcode = opcode;
	alu->s = s;
	alu->rd = rd;
	alu->n = n;
	alu->cycles = cycles;
}

static void _thumbImm(struct AluOp* alu, uint32_t imm) {
	alu->immediate = true;
	alu->imm = imm;
	alu->immCarry = -1;
}

static void _thumbReg(struct AluOp* alu, unsigned reg, unsigned shiftType, unsigned shiftAmount) {
	alu->m = _reg(reg);
	alu->shiftType = shiftType;
	alu->shiftAmount = shiftAmount;
}

static bool _decodeThumbAlu(uint32_t op, uint32_t address, uint32_t aluCycles, struct AluOp* alu) {
	unsigned rd = op & 7;
	unsigned rs = (op >> 3) & 7;
	switch (op >> 11) {
	case 0x00: // LSL, LSR, ASR #imm
	case 0x01:
	case 0x02:
		_thumbAlu(alu, ALU_MOV, true, rd, _reg(0), aluCycles);
		_thumbReg(alu, rs, op >> 11, (op >> 6) & 0x1F);
		return true;
	case 0x03: // ADD, SUB with a register or 3-bit immediate
		_thumbAlu(alu, (op & 0x0200) ? ALU_SUB : ALU_ADD, true, rd, _reg(rs), aluCycles);
		if (op & 0x0400) {
			_thumbImm(alu, (op >> 6) & 7);
		} else {
			_thumbReg(alu, (op >> 6) & 7, 0, 0);
		}
		return true;
	case 0x04: // MOV, CMP, ADD, SUB with an 8-bit immediate
	case 0x05:
	case 0x06:
	case 0x07: {
		static const unsigned opcodes[] = { ALU_MOV, ALU_CMP, ALU_ADD, ALU_SUB };
		rd = (op >> 8) & 7;
		_thumbAlu(alu, opcodes[(op >> 11) & 3], true, rd, _reg(rd), aluCycles);
		_thumbImm(alu, op & 0xFF);
		return true;
	}
	case 0x08:
		if (!(op & 0x0400)) {
			// AND EOR LSL LSR ASR ADC SBC ROR TST NEG CMP CMN ORR MUL BIC MVN
			static const int opcodes[] = {
				ALU_AND, ALU_EOR, -1, -1, -1, ALU_ADC, ALU_SBC, -1,
				ALU_TST, ALU_RSB, ALU_CMP, ALU_CMN, ALU_ORR, -1, ALU_BIC, ALU_MVN
			};
			int opcode = opcodes[(op >> 6) & 0xF];
			if (opcode < 0) {
				return false; // Register shifts and MUL
			}
			if (opcode == ALU_RSB) {
				_thumbAlu(alu, ALU_RSB, true, rd, _reg(rs), aluCycles); // NEG
				_thumbImm(alu, 0);
			} else {
				_thumbAlu(alu, opcode, true, rd, _reg(rd), aluCycles);
				_thumbReg(alu, rs, 0, 0);
			}
			return true;
		}
		if ((op & 0x0300) != 0x0300) {
			// ADD, CMP, MOV with high registers
			unsigned hd = rd | ((op >> 4) & 8);
			unsigned hm = (op >> 3) & 0xF;
			static const unsigned opcodes[] = { ALU_ADD, ALU_CMP, ALU_MOV };
			unsigned opcode = opcodes[(op >> 8) & 3];
			if (hd == ARM_PC && opcode != ALU_CMP) {
				return false;
			}
			_thumbAlu(alu, opcode, opcode == ALU_CMP, hd, hd == ARM_PC ? _const(address + 4) : _reg(hd), aluCycles);
			if (hm == ARM_PC) {
				_thumbImm(alu, address + 4);
			} else {
				_thumbReg(alu, hm, 0, 0);
			}
			return true;
		}
		return false;
	case 0x14: // ADD rd, PC, #imm
		_thumbAlu(alu, ALU_MOV, false, (op >> 8) & 7, _reg(0), aluCycles);
		_thumbImm(alu, ((address + 4) & ~3) + ((op & 0xFF) << 2));
		return true;
	case 0x15: // ADD rd, SP, #imm
		_thumbAlu(alu, ALU_ADD, false, (op >> 8) & 7, _reg(ARM_SP), aluCycles);
		_thumbImm(alu, (op & 0xFF) << 2);
		return true;
	case 0x16:
		if ((op & 0x0F00) == 0x0000) { // ADD SP, #+/-imm
			_thumbAlu(alu, (op & 0x80) ? ALU_SUB : ALU_ADD, false, ARM_SP, _reg(ARM_SP), aluCycles);
			_thumbImm(alu, (op & 0x7F) << 2);
			return true;
		}
		return false;
	case 0x1E: // BL prefix
		_thumbAlu(alu, ALU_MOV, false, ARM_LR, _reg(0), aluCycles);
		_thumbImm(alu, address + 4 + ((int32_t) (op << 21) >> 9));
		return true;
	default:
		return false;
	}
}

static bool _decodeArmMem(uint32_t op, uint32_t address, struct MemOp* mem) {
	unsigned cond = op >> 28;
	if (cond == 0xF) {
		return false;
	}
	bool p = op & (1 << 24);
	bool w = op & (1 << 21);
	unsigned rn = (op >> 16) & 0xF;
	unsigned rd = (op >> 12) & 0xF;
	if (rd == ARM_PC || (rn == ARM_PC && (!p || w))) {
		return false;
	}
	memset(mem, 0, sizeof(*mem));
	if ((op & 0x0C000000) == 0x04000000) {
		if ((op & 0x02000010) == 0x02000010 || (!p && w)) {
			return false; // Undefined, LDRT, STRT
		}
		mem->size = (op & (1 << 22)) ? 1 : 4;
		if (op & 0x02000000) {
			// Addressing mode 2 shifts: LSR and ASR #0 mean #32, ROR #0 is RRX
			mem->m = (op & 0xF) == ARM_PC ? _const(address + 8) : _reg(op & 0xF);
			mem->shiftType = (op >> 5) & 3;
			mem->shiftAmount = (op >> 7) & 0x1F;
		} else {
			mem->immediateOffset = true;
			mem->offset = op & 0xFFF;
		}
	} else if ((op & 0x0E000090) == 0x00000090 && (op & 0x60) == 0x20 && (p || !w)) {
		mem->size = 2; // LDRH, STRH
		if (op & (1 << 22)) {
			mem->immediateOffset = true;
			mem->offset = ((op >> 4) & 0xF0) | (op & 0xF);
		} else {
			mem->m = (op & 0xF) == ARM_PC ? _const(address + 8) : _reg(op & 0xF);
		}
	} else {
		return false;
	}
	mem->cond = cond;
	mem->load = op & (1 << 20);
	mem->rd = rd;
	mem->base = rn == ARM_PC ? _const(address + 8) : _reg(rn);
	mem->up = op & (1 << 23);
	mem->pre = p;
	mem->writeback = !p || w;
	return true;
}

static void _thumbMem(struct MemOp* mem, bool load, unsigned size, unsigned rd, struct Source base) {
	memset(mem, 0, sizeof(*mem));
	mem->cond = 0xE;
	mem->load = load;
	mem->size = size;
	mem->rd = rd;
	mem->base = base;
	mem->immediateOffset = true;
	mem->up = true;
	mem->pre = true;
}

static bool _decodeThumbMem(uint32_t op, uint32_t address, struct MemOp* mem) {
	unsigned rd = op & 7;
	unsigned rn = (op >> 3) & 7;
	switch (op >> 11) {
	case 0x09: // LDR rd, [PC, #imm]
		_thumbMem(mem, true, 4, (op >> 8) & 7, _const((address + 4) & ~3));
		mem->offset = (op & 0xFF) << 2;
		return true;
	case 0x0A: // Register offset
	case 0x0B: {
		// STR STRH STRB LDRSB LDR LDRH LDRB LDRSH
		static const unsigned sizes[] = { 4, 2, 1, 1, 4, 2, 1, 2 };
		unsigned kind = (op >> 9) & 7;
		_thumbMem(mem, kind >= 3, sizes[kind], rd, _reg(rn));
		mem->signExtend = kind == 3 || kind == 7;
		mem->immediateOffset = false;
		mem->m = _reg((op >> 6) & 7);
		return true;
	}
	case 0x0C: // STR, LDR #imm
	case 0x0D:
		_thumbMem(mem, op & 0x0800, 4, rd, _reg(rn));
		mem->offset = ((op >> 6) & 0x1F) << 2;
		return true;
	case 0x0E: // STRB, LDRB #imm
	case 0x0F:
		_thumbMem(mem, op & 0x0800, 1, rd, _reg(rn));
		mem->offset = (op >> 6) & 0x1F;
		return true;
	case 0x10: // STRH, LDRH #imm
	case 0x11:
		_thumbMem(mem, op & 0x0800, 2, rd, _reg(rn));
		mem->offset = ((op >> 6) & 0x1F) << 1;
		return true;
	case 0x12: // STR, LDR rd, [SP, #imm]
	case 0x13:
		_thumbMem(mem, op & 0x0800, 4, (op >> 8) & 7, _reg(ARM_SP));
		mem->offset = (op & 0xFF) << 2;
		return true;
	default:
		return false;
	}
}

// Flags as bits for liveness: N Z C V, and CPSR bits 24-27, which add and subtract clear
enum {
	FLAG_N = 1,
	FLAG_Z = 2,
	FLAG_C = 4,
	FLAG_V = 8,
	FLAG_LOW = 16,
	FLAG_ALL = 31,
};

static unsigned _condReads(unsigned cond) {
	static const unsigned reads[16] = {
		FLAG_Z, FLAG_Z, FLAG_C, FLAG_C, FLAG_N, FLAG_N, FLAG_V, FLAG_V,
		FLAG_C | FLAG_Z, FLAG_C | FLAG_Z, FLAG_N | FLAG_V, FLAG_N | FLAG_V,
		FLAG_N | FLAG_Z | FLAG_V, FLAG_N | FLAG_Z | FLAG_V, 0, 0
	};
	return reads[cond];
}

// Whether the carry out of the second operand is the old C (no shift, or an unrotated immediate)
static bool _keepsCarry(const struct AluOp* alu) {
	if (alu->immediate) {
		return alu->immCarry < 0;
	}
	return alu->shiftType == 0 && alu->shiftAmount == 0;
}

static unsigned _aluReads(const struct AluOp* alu) {
	unsigned reads = _condReads(alu->cond);
	if (alu->opcode == ALU_ADC || alu->opcode == ALU_SBC || alu->opcode == ALU_RSC) {
		reads |= FLAG_C;
	}
	if (!alu->immediate && alu->shiftType == 3 && alu->shiftAmount == 0) {
		reads |= FLAG_C; // RRX
	}
	return reads;
}

static unsigned _aluWrites(const struct AluOp* alu) {
	if (!alu->s) {
		return 0;
	}
	if (_isLogical(alu->opcode)) {
		return FLAG_N | FLAG_Z | (_keepsCarry(alu) ? 0 : FLAG_C);
	}
	if (alu->opcode == ALU_SBC || alu->opcode == ALU_RSC) {
		return FLAG_N | FLAG_Z | FLAG_C | FLAG_V;
	}
	return FLAG_ALL;
}

// Within a run of ALU instructions nothing else can see the flags; everything is live after it
static void _markLiveFlags(struct AluOp* alus, unsigned count) {
	unsigned live = FLAG_ALL;
	unsigned j = count;
	while (j--) {
		unsigned writes = _aluWrites(&alus[j]);
		alus[j].flagsLive = (writes & live) != 0;
		if (alus[j].cond == 0xE) {
			live &= ~writes;
		}
		live |= _aluReads(&alus[j]);
	}
}

static bool _decodeAlu(struct Compiler* c, unsigned i, struct AluOp* alu) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb) {
		return _decodeThumbAlu(c->ops[i], address, c->aluCycles, alu);
	}
	return _decodeArmAlu(c->ops[i], address, c->aluCycles, alu);
}

static bool _decodeMem(struct Compiler* c, unsigned i, struct MemOp* mem) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb) {
		return _decodeThumbMem(c->ops[i], address, mem);
	}
	return _decodeArmMem(c->ops[i], address, mem);
}

static void* _handler(struct Compiler* c, uint32_t op) {
	if (c->thumb) {
		return (void*) _thumbTable[op >> 6];
	}
	return (void*) _armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)];
}

// Whether a taken branch at ops[i] goes back to the start of this block
static bool _loopsToStart(struct Compiler* c, unsigned i) {
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + c->width * i;
	int32_t offset;
	if (c->thumb) {
		if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
			offset = (int8_t) op << 1;
		} else if ((op & 0xF800) == 0xE000) {
			offset = (int32_t) (op << 21) >> 20;
		} else {
			return false;
		}
	} else if ((op & 0x0E000000) == 0x0A000000) {
		offset = (int32_t) (op << 8) >> 6;
	} else {
		return false;
	}
	return address + 2 * c->width + offset == c->pc;
}

// Condition checked before calling a handler: ARM conditions and Thumb conditional branches
static unsigned _fallbackCond(struct Compiler* c, uint32_t op) {
	if (!c->thumb) {
		return op >> 28;
	}
	if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
		return (op >> 8) & 0xF;
	}
	return 0xE;
}

// Host address of a RAM word compiled from, for reading patched words at runtime
static void* _hostAddress(struct Compiler* c, uint32_t address) {
	if ((address >> 24) == GBA_REGION_IWRAM) {
		return (uint8_t*) c->gba->memory.iwram + (address & (GBA_SIZE_IWRAM - 1));
	}
	return (uint8_t*) c->gba->memory.wram + (address & (GBA_SIZE_EWRAM - 1));
}

// Each backend provides _prologue, _epilogue, _storeState, _addCycles, _segmentCheck,
// _eventCheck, _exitJump, _patch, _emitAlu, _emitMem and _emitFallback
#if defined(__aarch64__)
#include "jit-a64.h"
#else
#include "jit-x64.h"
#endif

static bool _endsBlock(uint32_t op, bool thumb) {
	if (thumb) {
		if ((op & 0xF000) == 0xD000 || (op & 0xF000) == 0xE000 || (op & 0xF800) == 0xF800) {
			return true; // B, conditional B, SWI, BL suffix, undefined
		}
		if ((op & 0xFF00) == 0x4700 || (op & 0xFF00) == 0xBD00 || (op & 0xFF00) == 0xBE00) {
			return true; // BX, POP with PC, BKPT
		}
		return ((op & 0xFF00) == 0x4400 || (op & 0xFF00) == 0x4600) && (op & 0x87) == 0x87; // ADD or MOV to PC
	}
	if ((op & 0x0E000000) == 0x0A000000) {
		return true; // B, BL
	}
	if ((op & 0x0FFFFFF0) == 0x012FFF10) {
		return true; // BX
	}
	if ((op & 0x0C000000) == 0x0C000000) {
		return true; // Coprocessor, SWI
	}
	if ((op & 0x0C000000) == 0x00000000 && ((op >> 12) & 0xF) == ARM_PC) {
		return true;
	}
	if ((op & 0x0C100000) == 0x04100000 && ((op >> 12) & 0xF) == ARM_PC) {
		return true;
	}
	if ((op & 0x0E108000) == 0x08108000) {
		return true; // LDM with PC
	}
	return false;
}

static struct ARMJitPage* _page(struct ARMJit* jit, uint32_t address, bool create) {
	struct ARMJitPage** page = &jit->pages[(address >> 12) & (ARM_JIT_PAGES - 1)];
	if (!*page && create) {
		*page = calloc(1, sizeof(**page));
	}
	return *page;
}

static bool _covers(const struct ARMJitBlock* block, unsigned word) {
	unsigned i = word - block->coverStart;
	return i < block->coverWords && (block->coverMask[i / 32] & (1u << (i & 31)));
}

static void _removeBlock(struct ARMJit* jit, struct ARMJitBlock* block) {
	struct ARMJitPage* page = _page(jit, block->pc, false);
	page->blocks[(block->pc & 0xFFF) >> 1] = NULL;
	unsigned i;
	for (i = 0; i < block->coverWords; ++i) {
		if (block->coverMask[i / 32] & (1u << (i & 31))) {
			--jit->cover[block->coverStart + i];
		}
	}
	if (block == jit->current) {
		jit->smcHit = 1;
		jit->current = NULL;
	}
	free(block);
}

static void _selfTestAlu(struct ARMJit* jit, unsigned iterations);

struct ARMJit* ARMJitCreate(void) {
	struct ARMJit* jit = calloc(1, sizeof(*jit));
	if (!jit) {
		return NULL;
	}
	jit->code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (jit->code == MAP_FAILED) {
		free(jit);
		return NULL;
	}
	jit->codeSize = CODE_SIZE;
	if (getenv("MGBA_JIT_SELFTEST")) {
		_selfTestAlu(jit, atoi(getenv("MGBA_JIT_SELFTEST")));
	}
	return jit;
}

void ARMJitDestroy(struct ARMJit* jit) {
	ARMJitFlush(jit);
	unsigned i;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		free(jit->pages[i]);
	}
	munmap(jit->code, jit->codeSize);
	free(jit);
}

// Waitstate changes and a full code buffer drop every block but keep the patch history
void ARMJitDropBlocks(struct ARMJit* jit) {
	unsigned i, j;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		struct ARMJitPage* page = jit->pages[i];
		if (!page) {
			continue;
		}
		for (j = 0; j < 0x800; ++j) {
			if (page->blocks[j]) {
				if (page->blocks[j] == jit->current) {
					jit->smcHit = 1;
					jit->current = NULL;
				}
				free(page->blocks[j]);
				page->blocks[j] = NULL;
			}
		}
	}
	memset(jit->cover, 0, sizeof(jit->cover));
	jit->codeUsed = jit->codeStart;
}

void ARMJitFlush(struct ARMJit* jit) {
	ARMJitDropBlocks(jit);
	unsigned i;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		if (jit->pages[i]) {
			memset(jit->pages[i]->hits, 0, sizeof(jit->pages[i]->hits));
		}
	}
	memset(jit->patched, 0, sizeof(jit->patched));
}

static uint32_t _ramWordAddress(unsigned word) {
	if (word < ARM_JIT_IWRAM_WORDS) {
		return GBA_BASE_IWRAM + 4 * word;
	}
	return GBA_BASE_EWRAM + 4 * (word - ARM_JIT_IWRAM_WORDS);
}

void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word) {
	if (jit->patched[word] < 255) {
		++jit->patched[word];
	}
	// A block can start up to MAX_SPAN ARM words before the word it covers
	uint32_t address = _ramWordAddress(word);
	unsigned back;
	for (back = 0; back < 4 * MAX_SPAN + 4 && jit->cover[word]; back += 2) {
		uint32_t start = address + 2 - back;
		struct ARMJitPage* page = _page(jit, start, false);
		struct ARMJitBlock* block = page ? page->blocks[(start & 0xFFF) >> 1] : NULL;
		if (block && block->pc == start && _covers(block, word)) {
			_removeBlock(jit, block);
		}
	}
}

// Words that keep getting patched are read when the pipeline fetches them instead of being
// compiled in
static bool _isPatched(struct ARMJit* jit, uint32_t address) {
	int word = ARMJitRamWord(address);
	return word >= 0 && jit->patched[word] >= PATCH_LIMIT;
}

// End of the region a block at this address may run through, or 0 if it can't be compiled
static uint32_t _regionEnd(struct ARMCore* cpu, uint32_t pc) {
	struct GBA* gba = (struct GBA*) cpu->master;
	switch (pc >> 24) {
	case GBA_REGION_BIOS:
		return pc < GBA_SIZE_BIOS ? GBA_SIZE_BIOS : 0;
	case GBA_REGION_EWRAM:
		return pc < GBA_BASE_EWRAM + GBA_SIZE_EWRAM ? GBA_BASE_EWRAM + GBA_SIZE_EWRAM : 0;
	case GBA_REGION_IWRAM:
		return pc < GBA_BASE_IWRAM + GBA_SIZE_IWRAM ? GBA_BASE_IWRAM + GBA_SIZE_IWRAM : 0;
	case GBA_REGION_ROM0:
	case GBA_REGION_ROM0_EX:
	case GBA_REGION_ROM1:
	case GBA_REGION_ROM1_EX:
	case GBA_REGION_ROM2:
	case GBA_REGION_ROM2_EX:
		if ((pc & (GBA_SIZE_ROM0 - 1)) < gba->memory.romSize) {
			return (pc & ~(GBA_SIZE_ROM0 - 1)) + gba->memory.romSize;
		}
		return 0;
	default:
		return 0;
	}
}

static struct ARMJitBlock* _compile(struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc, bool thumb) {
	const uint32_t* region = cpu->memory.activeRegion;
	uint32_t mask = cpu->memory.activeMask;
	unsigned width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	uint32_t end = _regionEnd(cpu, pc);
	uint32_t ops[MAX_SPAN];
	bool hot[MAX_SPAN];
	unsigned count = 0;
	// The dispatcher checks the first two words against the pipeline, so they can't be patched
	if (_isPatched(jit, pc) || _isPatched(jit, pc + width)) {
		return NULL;
	}
	while (count < MAX_BLOCK && pc + width * (count + 3) <= end) {
		uint32_t address = pc + width * count;
		hot[count] = _isPatched(jit, address);
		if (thumb) {
			LOAD_16(ops[count], address & mask, region);
		} else {
			LOAD_32(ops[count], address & mask, region);
		}
		++count;
		if (!hot[count - 1] && _endsBlock(ops[count - 1], thumb)) {
			break;
		}
	}
	if (!count) {
		return NULL;
	}
	hot[count] = _isPatched(jit, pc + width * count);
	hot[count + 1] = _isPatched(jit, pc + width * (count + 1));
	if (thumb) {
		LOAD_16(ops[count], (pc + width * count) & mask, region);
		LOAD_16(ops[count + 1], (pc + width * (count + 1)) & mask, region);
	} else {
		LOAD_32(ops[count], (pc + width * count) & mask, region);
		LOAD_32(ops[count + 1], (pc + width * (count + 1)) & mask, region);
	}
	if (ops[0] != cpu->prefetch[0] || ops[1] != cpu->prefetch[1]) {
		return NULL;
	}

	if (jit->codeUsed + count * MAX_INSN_BYTES + 1024 > jit->codeSize) {
		ARMJitDropBlocks(jit);
	}

	struct ARMJitBlock* block = malloc(sizeof(*block));
	if (!block) {
		return NULL;
	}
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint8_t* code = &jit->code[jit->codeUsed];
	c->e.p = code;
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	c->pc = pc;
	c->ops = ops;
	c->hot = hot;
	c->count = count;
	c->thumb = thumb;
	c->width = width;
	c->romCode = (pc >> 24) >= GBA_REGION_ROM0;
	c->nExits = 0;
	if (thumb) {
		c->aluCycles = 1 + cpu->memory.activeSeqCycles16;
		c->memCycles = 1 + cpu->memory.activeNonseqCycles16;
	} else {
		c->aluCycles = 1 + cpu->memory.activeSeqCycles32;
		c->memCycles = 1 + cpu->memory.activeNonseqCycles32;
	}

	c->body = c->e.p;

	struct AluOp alus[MAX_SPAN];
	struct MemOp mem;
	unsigned i = 0;
	while (i < count) {
		if (hot[i]) {
			_fetchAhead(c, i);
			_emitDynamic(c, i);
			++i;
		} else if (_decodeAlu(c, i, &alus[i])) {
			unsigned run = 1;
			while (i + run < count && !hot[i + run] && _decodeAlu(c, i + run, &alus[i + run])) {
				++run;
			}
			// The interpreter checks for events after every instruction, so only run the
			// whole segment when none can come due before its last instruction
			uint32_t cycles = 0;
			unsigned lastAlways = 0;
			unsigned j;
			for (j = 0; j < run; ++j) {
				if (j < run - 1) {
					cycles += alus[i + j].cycles;
				}
				if (alus[i + j].cond == 0xE) {
					lastAlways = j;
				}
			}
			if (run > 1) {
				_segmentCheck(c, i, cycles);
			}
			_markLiveFlags(&alus[i], run);
			for (j = 0; j < run; ++j) {
				_fetchAhead(c, i + j);
				_emitAlu(c, &alus[i + j], alus[i + j].keepsShifterCarry && j >= lastAlways);
			}
			_addCycles(c, cycles + alus[i + run - 1].cycles);
			i += run;
			_eventCheck(c, i);
		} else if (_decodeMem(c, i, &mem)) {
			_fetchAhead(c, i);
			_emitMem(c, i, &mem);
			++i;
		} else {
			_fetchAhead(c, i);
			_emitFallback(c, i);
			++i;
		}
	}
	_exitJump(c, count);

	// Exit stubs set the interpreter state for the next instruction, then go to the
	// dispatcher. Leaving before the first instruction goes back to C, which steps it.
	uint8_t* stubs[MAX_SPAN];
	memset(stubs, 0, sizeof(stubs));
	unsigned x;
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		if (index >= 0 && !stubs[index]) {
			stubs[index] = c->e.p;
			_storeState(c, index);
			_exitJump(c, index ? EXIT_DIRECT : EXIT_TO_C);
		}
	}
	uint8_t* direct = c->e.p;
	_jumpTo(c, jit->dispatch);
	uint8_t* toC = c->e.p;
	_jumpTo(c, jit->toC);
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		_patch(c->exits[x].at, index == EXIT_DIRECT ? direct : index == EXIT_TO_C ? toC : stubs[index]);
	}

	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeUsed += c->e.p - code;

	block->entry = code;
	block->pc = pc;
	block->op0 = ops[0];
	block->op1 = ops[1];
	block->thumb = thumb;
	block->coverStart = ARMJitRamWord(pc);
	block->coverWords = 0;
	memset(block->coverMask, 0, sizeof(block->coverMask));
	if (block->coverStart >= 0) {
		block->coverWords = ARMJitRamWord(pc + width * (count + 2) - 1) - block->coverStart + 1;
		for (i = 0; i < block->coverWords; ++i) {
			if (jit->patched[block->coverStart + i] < PATCH_LIMIT) {
				block->coverMask[i / 32] |= 1u << (i & 31);
				++jit->cover[block->coverStart + i];
			}
		}
	}
	_page(jit, pc, true)->blocks[(pc & 0xFFF) >> 1] = block;
	return block;
}

// Debug: compare inline ALU code with the interpreter on random inputs
static unsigned _selfTestAluMode(struct ARMJit* jit, unsigned iterations, bool thumb) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint32_t seed = thumb ? 54321 : 12345;
	unsigned failures = 0;
	unsigned n;
	for (n = 0; n < iterations && failures < 20; ++n) {
		uint32_t address = 0x03001000;
		uint32_t op;
		struct AluOp alu;
		do {
			seed = seed * 1103515245 + 12345;
			op = seed;
			seed = seed * 1103515245 + 12345;
			op ^= seed << 16;
			if (thumb) {
				op &= 0xFFFF;
			}
		} while (thumb ? !_decodeThumbAlu(op, address, 1, &alu) : !_decodeArmAlu(op, address, 1, &alu));
		struct ARMCore a;
		memset(&a, 0, sizeof(a));
		unsigned r;
		for (r = 0; r < 15; ++r) {
			seed = seed * 1103515245 + 12345;
			a.gprs[r] = (seed >> 3) ^ (seed << 29);
			if ((seed & 7) == 0) {
				a.gprs[r] = 0;
			} else if ((seed & 7) == 1) {
				a.gprs[r] = 0x80000000;
			} else if ((seed & 7) == 2) {
				a.gprs[r] = 0xFFFFFFFF;
			}
		}
		a.gprs[ARM_PC] = address + (thumb ? 4 : 8);
		seed = seed * 1103515245 + 12345;
		a.cpsr.packed = (seed & 0xFF000000) | (thumb ? 0x3F : 0x1F);
		struct ARMCore b = a;

		if (thumb) {
			_thumbTable[op >> 6](&a, op);
		} else if (_conditionLut[op >> 28] & (1 << ((uint32_t) a.cpsr.packed >> 28))) {
			_armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)](&a, op);
		} else {
			a.cycles += 1;
		}

		static const bool cold[3];
		uint32_t ops[3] = { op, 0, 0 };
		uint8_t* code = &jit->code[jit->codeUsed];
		c->e.p = code;
		c->jit = jit;
		c->gba = NULL;
		c->pc = address;
		c->ops = ops;
		c->hot = cold;
		c->count = 1;
		c->thumb = thumb;
		c->width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
		c->romCode = false;
		c->nExits = 0;
		c->aluCycles = 1;
		c->memCycles = 1;
		alu.flagsLive = true;
		_prologue(c);
		_emitAlu(c, &alu, alu.keepsShifterCarry);
		_addCycles(c, alu.cycles);
		_epilogue(c);
		__builtin___clear_cache((char*) code, (char*) c->e.p);
		((void (*)(struct ARMCore*)) code)(&b);

		bool bad = a.cpsr.packed != b.cpsr.packed || a.cycles != b.cycles || (a.shifterCarryOut & 1) != (b.shifterCarryOut & 1);
		for (r = 0; r < 16; ++r) {
			bad = bad || a.gprs[r] != b.gprs[r];
		}
		if (bad) {
			++failures;
			fprintf(stderr, "%s ALU mismatch op %08X: cpsr %08X/%08X cycles %d/%d", thumb ? "Thumb" : "ARM", op, a.cpsr.packed, b.cpsr.packed, a.cycles, b.cycles);
			for (r = 0; r < 16; ++r) {
				if (a.gprs[r] != b.gprs[r]) {
					fprintf(stderr, " r%u %08X/%08X", r, a.gprs[r], b.gprs[r]);
				}
			}
			fprintf(stderr, "\n");
		}
	}
	return failures;
}

static void _selfTestAlu(struct ARMJit* jit, unsigned iterations) {
	unsigned arm = _selfTestAluMode(jit, iterations, false);
	unsigned thumb = _selfTestAluMode(jit, iterations, true);
	fprintf(stderr, "ALU self-test: %u ARM and %u Thumb instructions, %u and %u failures\n", iterations, iterations, arm, thumb);
}

static void _buildTrampoline(struct ARMJit* jit, struct ARMCore* cpu) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint8_t* code = jit->code;
	c->e.p = code;
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	_emitTrampoline(c);
	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeStart = c->e.p - code;
	jit->codeUsed = jit->codeStart;
}

bool ARMJitRun(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	if (!jit->enter) {
		_buildTrampoline(jit, cpu);
	}
	bool thumb = cpu->executionMode == MODE_THUMB;
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
	struct ARMJitPage* page = _page(jit, pc, false);
	unsigned index = (pc & 0xFFF) >> 1;
	struct ARMJitBlock* block = page ? page->blocks[index] : NULL;
	if (!block || block->pc != pc || block->thumb != thumb) {
		if (!_regionEnd(cpu, pc)) {
			return false;
		}
		if (!page) {
			page = _page(jit, pc, true);
		}
		if (page->hits[index] < HOT_THRESHOLD) {
			++page->hits[index];
			return false;
		}
		if (block) {
			_removeBlock(jit, block);
		}
		block = _compile(jit, cpu, pc, thumb);
		if (!block) {
			page->hits[index] = 0;
			return false;
		}
	}
	if (cpu->prefetch[0] != block->op0 || cpu->prefetch[1] != block->op1) {
		return false;
	}
	jit->current = block;
	jit->smcHit = 0;
	int32_t cycles = cpu->cycles;
	jit->enter(cpu, block->entry);
	jit->current = NULL;
	// A block exits before its first segment when an event is due inside it
	return cpu->cycles != cycles;
}

#endif
