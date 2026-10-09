/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Guest instructions decoded into the operations both backends emit
static const uint16_t _conditionLut[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

enum {
	SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR
};

enum {
	ALU_AND, ALU_EOR, ALU_SUB, ALU_RSB, ALU_ADD, ALU_ADC, ALU_SBC, ALU_RSC,
	ALU_TST, ALU_TEQ, ALU_CMP, ALU_CMN, ALU_ORR, ALU_MOV, ALU_BIC, ALU_MVN
};

// A source is a guest register, or a value known when compiling (the PC)
struct Source {
	bool constant;
	uint32_t value;
	unsigned reg;
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
	// A patched instruction: the 12-bit offset and the up bit come from the fetched opcode
	bool runtimeOffset;
	unsigned fetchedIndex;
};

static bool _isLogical(unsigned opcode) {
	return opcode == ALU_AND || opcode == ALU_EOR || opcode == ALU_TST || opcode == ALU_TEQ ||
	       opcode == ALU_ORR || opcode == ALU_MOV || opcode == ALU_BIC || opcode == ALU_MVN;
}

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
	} else if ((op & 0x0E000090) == 0x00000090 && (op & 0x60) && ((op & 0x60) == 0x20 || (op & (1 << 20))) && (p || !w)) {
		// LDRH, STRH, LDRSB, LDRSH
		mem->size = (op & 0x60) == 0x40 ? 1 : 2;
		mem->signExtend = op & 0x40;
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
	// Bits 24-27, which add and subtract clear
	FLAG_LOW = 16,
	FLAG_ALL = FLAG_N | FLAG_Z | FLAG_C | FLAG_V | FLAG_LOW,
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
	if (!alu->immediate && alu->shiftType == SHIFT_ROR && alu->shiftAmount == 0) {
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

// MUL, MLA and Thumb MUL, which take a wait by the multiplier's significant bytes
struct MulOp {
	unsigned cond;
	unsigned rd;
	unsigned rm;
	unsigned rs;
	int rn;
	bool s;
};

static bool _decodeMul(uint32_t op, bool thumb, struct MulOp* m) {
	if (thumb) {
		if ((op & 0xFFC0) != 0x4340) {
			return false;
		}
		// rd *= rm, waiting by rd
		m->cond = 0xE;
		m->rd = op & 7;
		m->rs = op & 7;
		m->rm = (op >> 3) & 7;
		m->rn = -1;
		m->s = true;
		return true;
	}
	if ((op & 0x0FC000F0) != 0x00000090) {
		return false;
	}
	m->cond = op >> 28;
	m->rd = (op >> 16) & 0xF;
	m->rs = (op >> 8) & 0xF;
	m->rm = op & 0xF;
	m->rn = (op & 0x00200000) ? (int) ((op >> 12) & 0xF) : -1;
	m->s = op & 0x00100000;
	return m->cond != 0xF && m->rd != ARM_PC && m->rs != ARM_PC && m->rm != ARM_PC && m->rn != ARM_PC;
}

// LDM and STM without PC, the S bit or an empty list; Thumb PUSH, POP, LDMIA and STMIA
struct MultiOp {
	unsigned cond;
	bool load;
	unsigned rn;
	unsigned list;
	bool up;
	bool pre;
	bool writeback;
};

static bool _decodeMulti(uint32_t op, bool thumb, struct MultiOp* m) {
	m->cond = 0xE;
	if (thumb) {
		if ((op & 0xF000) == 0xC000) {
			m->load = op & 0x0800;
			m->rn = (op >> 8) & 7;
			m->list = op & 0xFF;
			m->up = true;
			m->pre = false;
		} else if ((op & 0xFE00) == 0xB400 || (op & 0xFF00) == 0xBC00) {
			m->load = op & 0x0800;
			m->rn = ARM_SP;
			m->list = op & 0xFF;
			if (op & 0x0100) {
				m->list |= 1 << ARM_LR;
			}
			m->up = m->load;
			m->pre = !m->load;
		} else {
			return false;
		}
		m->writeback = !(m->load && (m->list & (1 << m->rn)));
	} else {
		if ((op & 0x0E400000) != 0x08000000) {
			return false;
		}
		m->cond = op >> 28;
		m->load = op & 0x00100000;
		m->rn = (op >> 16) & 0xF;
		m->list = op & 0xFFFF;
		m->up = op & 0x00800000;
		m->pre = op & 0x01000000;
		m->writeback = (op & 0x00200000) && !(m->load && (m->list & (1 << m->rn)));
	}
	return m->list && !(m->list & (1 << ARM_PC)) && m->rn != ARM_PC && m->cond != 0xF;
}

// ARM conditions, and Thumb conditional branches
static unsigned _condition(uint32_t op, bool thumb) {
	if (!thumb) {
		return op >> 28;
	}
	if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
		return (op >> 8) & 0xF;
	}
	return 0xE;
}

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

static void* _handler(uint32_t op, bool thumb) {
	if (thumb) {
		return (void*) _thumbTable[op >> 6];
	}
	return (void*) _armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)];
}
