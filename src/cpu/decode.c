/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "decode.h"

#include <string.h>

// ARM and Thumb instructions decoded into struct Inst (decode.h), which the exact CPU and the
// recompiler both work from.

static uint32_t ror32(uint32_t value, unsigned rotate) {
	rotate &= 31;
	return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

// Makes an operand that is a constant.
static void immOperand(struct Operand* op, uint32_t value) {
	op->isImm = true;
	op->immRotated = false;
	op->imm = value;
}

// Makes an operand that is a register shifted by a constant.
static void regOperand(struct Operand* op, int rm, int type, int amount) {
	op->isImm = false;
	op->rm = rm;
	op->type = type;
	op->amount = amount;
	op->byReg = false;
}

// Decodes ARM's second operand: an 8-bit constant rotated right by an even amount, or a register
// shifted by a constant or by another register's low byte.
static void decodeShifter(struct Operand* op, uint32_t opcode) {
	if (opcode & 0x02000000) {
		int rotate = (opcode >> 7) & 0x1E;
		immOperand(op, ror32(opcode & 0xFF, rotate));
		op->immRotated = rotate != 0;
		return;
	}
	regOperand(op, opcode & 0xF, (opcode >> 5) & 3, (opcode >> 7) & 0x1F);
	if (opcode & 0x10) {
		op->byReg = true;
		op->rs = (opcode >> 8) & 0xF;
	}
}

// The Wii U Virtual Console's calls to its emulator, which are undefined on a GBA. Its dumps keep
// them, and mGBA lets them go by: an instruction that never runs.
static void vcCall(struct Inst* in) {
	in->kind = I_ALU;
	in->op = ALU_MOV;
	in->cond = 0xF;
	in->rd = 0;
	in->op2.isImm = false;
	in->op2.rm = 0;
	in->op2.type = SHIFT_LSL;
	in->op2.amount = 0;
}

// Decodes the ARM instruction opcode at address into in. Bits 27-25 pick the class; everything the
// CPUs and the recompiler need afterwards is in the fields, so none of them looks at the opcode
// again.
void decodeArm(struct Inst* in, uint32_t address, uint32_t opcode) {
	memset(in, 0, sizeof(*in));
	in->address = address;
	in->opcode = opcode;
	in->cond = opcode >> 28;
	in->rd = (opcode >> 12) & 0xF;
	in->rn = (opcode >> 16) & 0xF;
	in->rm = opcode & 0xF;
	in->rs = (opcode >> 8) & 0xF;
	in->kind = I_UNDEFINED;

	switch ((opcode >> 25) & 7) {
	// Data processing with a register operand, and the instructions that share its encoding space:
	// multiplies, SWP, the halfword and signed loads (bits 7 and 4 set), MRS, MSR and BX.
	case 0:
		if ((opcode & 0x90) == 0x90) {
			if (!(opcode & 0x60)) {
				if ((opcode & 0x01800000) == 0) {
					in->kind = I_MUL;
					in->accumulate = opcode & 0x00200000;
					in->setFlags = opcode & 0x00100000;
					in->rd = (opcode >> 16) & 0xF;
					in->rn = (opcode >> 12) & 0xF;
				} else if ((opcode & 0x01800000) == 0x00800000) {
					in->kind = I_MULL;
					in->signedMul = opcode & 0x00400000;
					in->accumulate = opcode & 0x00200000;
					in->setFlags = opcode & 0x00100000;
					in->rdHi = (opcode >> 16) & 0xF;
				} else if ((opcode & 0x01B00F00) == 0x01000000) {
					in->kind = I_SWP;
					in->size = (opcode & 0x00400000) ? 1 : 4;
				}
				return;
			}
			// Halfword and signed transfers.
			in->kind = I_MEM;
			in->load = opcode & 0x00100000;
			in->pre = opcode & 0x01000000;
			in->up = opcode & 0x00800000;
			in->writeback = !in->pre || (opcode & 0x00200000);
			switch ((opcode >> 5) & 3) {
			// Data processing with a constant, and MSR with one.
			case 1:
				in->size = 2;
				break;
			// LDR, STR, LDRB and STRB, with a constant or a shifted register as the offset.
			case 2:
				in->size = 1;
				in->signExtend = true;
				break;
			case 3:
				in->size = 2;
				in->signExtend = true;
				break;
			}
			if (!in->load && in->signExtend) {
				in->kind = I_UNDEFINED;
				return;
			}
			if (opcode & 0x00400000) {
				immOperand(&in->op2, ((opcode >> 4) & 0xF0) | (opcode & 0xF));
			} else {
				regOperand(&in->op2, opcode & 0xF, SHIFT_LSL, 0);
			}
			return;
		}
		if ((opcode & 0x01900000) == 0x01000000) {
			if ((opcode & 0x0FFFFFF0) == 0x012FFF10) {
				in->kind = I_BX;
				return;
			}
			if (opcode & 0x10) {
				return;
			}
			if (opcode & 0x00200000) {
				in->kind = I_MSR;
				in->spsr = opcode & 0x00400000;
				in->fieldMask = ((opcode & 0x00010000) ? 0xFF : 0) | ((opcode & 0x00080000) ? 0xFF000000 : 0);
				regOperand(&in->op2, opcode & 0xF, SHIFT_LSL, 0);
			} else {
				in->kind = I_MRS;
				in->spsr = opcode & 0x00400000;
			}
			return;
		}
		// Fall through.
	case 1:
		if ((opcode & 0x01900000) == 0x01000000) {
			if (opcode & 0x00200000) {
				in->kind = I_MSR;
				in->spsr = opcode & 0x00400000;
				in->fieldMask = ((opcode & 0x00010000) ? 0xFF : 0) | ((opcode & 0x00080000) ? 0xFF000000 : 0);
				decodeShifter(&in->op2, opcode);
			}
			return;
		}
		in->kind = I_ALU;
		in->op = (opcode >> 21) & 0xF;
		in->setFlags = opcode & 0x00100000;
		decodeShifter(&in->op2, opcode);
		return;
	case 2:
	case 3:
		if ((opcode & 0x02000010) == 0x02000010) {
			return;
		}
		in->kind = I_MEM;
		in->load = opcode & 0x00100000;
		in->size = (opcode & 0x00400000) ? 1 : 4;
		in->pre = opcode & 0x01000000;
		in->up = opcode & 0x00800000;
		in->writeback = !in->pre || (opcode & 0x00200000);
		if (opcode & 0x02000000) {
			regOperand(&in->op2, opcode & 0xF, (opcode >> 5) & 3, (opcode >> 7) & 0x1F);
		} else {
			immOperand(&in->op2, opcode & 0xFFF);
		}
		return;
	// LDM and STM.
	case 4:
		in->kind = I_BLOCK;
		in->load = opcode & 0x00100000;
		in->pre = opcode & 0x01000000;
		in->up = opcode & 0x00800000;
		in->userBank = opcode & 0x00400000;
		in->writeback = opcode & 0x00200000;
		in->list = opcode & 0xFFFF;
		return;
	// B and BL: a signed 24-bit word offset from the PC, which is 8 ahead.
	case 5: {
		in->kind = I_B;
		in->link = opcode & 0x01000000;
		int32_t offset = (int32_t) (opcode << 8) >> 6;
		in->target = address + 8 + offset;
		return;
	}
	// The coprocessor instructions. A GBA has no coprocessor, so they are undefined.
	case 6:
		in->kind = I_STUB;
		return;
	case 7:
		if (opcode & 0x01000000) {
			in->kind = I_SWI;
		} else if (!(opcode & 0x00000F10)) {
			// CDP p0, a Wii U Virtual Console call.
			vcCall(in);
		}
		return;
	}
}

static void thumbAlu(struct Inst* in, int op, int rd, int rn, bool setFlags) {
	in->kind = I_ALU;
	in->op = op;
	in->rd = rd;
	in->rn = rn;
	in->setFlags = setFlags;
}

// Decodes the Thumb instruction opcode at address. Its top five bits pick the format; each becomes
// the ARM instruction it stands for, so the rest of the emulator handles one instruction set.
void decodeThumb(struct Inst* in, uint32_t address, uint16_t opcode) {
	memset(in, 0, sizeof(*in));
	in->address = address;
	in->opcode = opcode;
	in->thumb = true;
	in->cond = COND_AL;
	in->kind = I_UNDEFINED;

	int rd = opcode & 7;
	int rs3 = (opcode >> 3) & 7;
	int r6 = (opcode >> 6) & 7;
	int r8 = (opcode >> 8) & 7;

	switch (opcode >> 11) {
	// LSL, LSR and ASR by a constant.
	case 0x00:
	case 0x01:
	case 0x02:
		thumbAlu(in, ALU_MOV, rd, 0, true);
		regOperand(&in->op2, rs3, opcode >> 11, (opcode >> 6) & 0x1F);
		return;
	// ADD and SUB of a register or a 3-bit constant.
	case 0x03:
		thumbAlu(in, (opcode & 0x0200) ? ALU_SUB : ALU_ADD, rd, rs3, true);
		if (opcode & 0x0400) {
			immOperand(&in->op2, r6);
		} else {
			regOperand(&in->op2, r6, SHIFT_LSL, 0);
		}
		return;
	// MOV, CMP, ADD and SUB with an 8-bit constant.
	case 0x04:
		thumbAlu(in, ALU_MOV, r8, 0, true);
		immOperand(&in->op2, opcode & 0xFF);
		return;
	case 0x05:
		thumbAlu(in, ALU_CMP, 0, r8, true);
		immOperand(&in->op2, opcode & 0xFF);
		return;
	case 0x06:
		thumbAlu(in, ALU_ADD, r8, r8, true);
		immOperand(&in->op2, opcode & 0xFF);
		return;
	case 0x07:
		thumbAlu(in, ALU_SUB, r8, r8, true);
		immOperand(&in->op2, opcode & 0xFF);
		return;
	// The register-to-register operations, and the ones that reach r8-r15: ADD, CMP, MOV and BX.
	case 0x08:
		if (!(opcode & 0x0400)) {
			static const int8_t ops[16] = {
				ALU_AND, ALU_EOR, -SHIFT_LSL - 1, -SHIFT_LSR - 1, -SHIFT_ASR - 1, ALU_ADC, ALU_SBC, -SHIFT_ROR - 1,
				ALU_TST, ALU_RSB, ALU_CMP, ALU_CMN, ALU_ORR, -16, ALU_BIC, ALU_MVN,
			};
			int op = ops[(opcode >> 6) & 0xF];
			if (op == -16) {
				in->kind = I_MUL;
				in->setFlags = true;
				in->rd = rd;
				in->rm = rd;
				in->rs = rs3;
				return;
			}
			if (op < 0) {
				thumbAlu(in, ALU_MOV, rd, 0, true);
				regOperand(&in->op2, rd, -op - 1, 0);
				in->op2.byReg = true;
				in->op2.rs = rs3;
				return;
			}
			if (op == ALU_RSB) {
				thumbAlu(in, ALU_RSB, rd, rs3, true);
				immOperand(&in->op2, 0);
				return;
			}
			thumbAlu(in, op, rd, rd, true);
			regOperand(&in->op2, rs3, SHIFT_LSL, 0);
			return;
		}
		{
			int hd = rd | ((opcode >> 4) & 8);
			int hm = (opcode >> 3) & 0xF;
			switch ((opcode >> 8) & 3) {
			case 0:
				thumbAlu(in, ALU_ADD, hd, hd, false);
				regOperand(&in->op2, hm, SHIFT_LSL, 0);
				return;
			case 1:
				thumbAlu(in, ALU_CMP, 0, hd, true);
				regOperand(&in->op2, hm, SHIFT_LSL, 0);
				return;
			case 2:
				thumbAlu(in, ALU_MOV, hd, 0, false);
				regOperand(&in->op2, hm, SHIFT_LSL, 0);
				return;
			case 3:
				in->kind = I_BX;
				in->rm = hm;
				return;
			}
		}
		return;
	// LDR from the literal pool: the word-aligned PC plus a constant.
	case 0x09:
		in->kind = I_MEM;
		in->load = true;
		in->size = 4;
		in->rd = r8;
		in->rn = 15;
		in->pre = true;
		in->up = true;
		immOperand(&in->op2, (opcode & 0xFF) << 2);
		return;
	// Loads and stores with a register offset, including the signed loads.
	case 0x0A:
	case 0x0B: {
		static const struct {
			bool load;
			uint8_t size;
			bool signExtend;
		} forms[8] = {
			{ false, 4, false }, { false, 2, false }, { false, 1, false }, { true, 1, true },
			{ true, 4, false }, { true, 2, false }, { true, 1, false }, { true, 2, true },
		};
		int form = (opcode >> 9) & 7;
		in->kind = I_MEM;
		in->load = forms[form].load;
		in->size = forms[form].size;
		in->signExtend = forms[form].signExtend;
		in->rd = rd;
		in->rn = rs3;
		in->pre = true;
		in->up = true;
		regOperand(&in->op2, r6, SHIFT_LSL, 0);
		return;
	}
	// Loads and stores of words and bytes with a 5-bit offset, then of halfwords.
	case 0x0C:
	case 0x0D:
	case 0x0E:
	case 0x0F:
	case 0x10:
	case 0x11: {
		static const uint8_t sizes[6] = { 4, 4, 1, 1, 2, 2 };
		int form = (opcode >> 11) - 0x0C;
		in->kind = I_MEM;
		in->load = form & 1;
		in->size = sizes[form];
		in->rd = rd;
		in->rn = rs3;
		in->pre = true;
		in->up = true;
		immOperand(&in->op2, ((opcode >> 6) & 0x1F) * in->size);
		return;
	}
	// Loads and stores relative to the stack pointer.
	case 0x12:
	case 0x13:
		in->kind = I_MEM;
		in->load = opcode & 0x0800;
		in->size = 4;
		in->rd = r8;
		in->rn = 13;
		in->pre = true;
		in->up = true;
		immOperand(&in->op2, (opcode & 0xFF) << 2);
		return;
	// ADD of a constant to the PC or the stack pointer, into a low register.
	case 0x14:
		// The base is the word-aligned PC, which is a constant.
		thumbAlu(in, ALU_MOV, r8, 0, false);
		immOperand(&in->op2, ((address + 4) & ~3u) + ((opcode & 0xFF) << 2));
		return;
	case 0x15:
		thumbAlu(in, ALU_ADD, r8, 13, false);
		immOperand(&in->op2, (opcode & 0xFF) << 2);
		return;
	// The stack pointer moved by a constant, PUSH and POP.
	case 0x16:
	case 0x17:
		switch ((opcode >> 8) & 0xF) {
		case 0x0:
			thumbAlu(in, (opcode & 0x80) ? ALU_SUB : ALU_ADD, 13, 13, false);
			immOperand(&in->op2, (opcode & 0x7F) << 2);
			return;
		case 0x4:
		case 0x5:
			in->kind = I_BLOCK;
			in->rn = 13;
			in->pre = true;
			in->up = false;
			in->writeback = true;
			in->list = (opcode & 0xFF) | ((opcode & 0x100) ? 0x4000 : 0);
			return;
		case 0xC:
		case 0xD:
			in->kind = I_BLOCK;
			in->load = true;
			in->rn = 13;
			in->up = true;
			in->writeback = true;
			in->list = (opcode & 0xFF) | ((opcode & 0x100) ? 0x8000 : 0);
			return;
		}
		return;
	// STMIA and LDMIA.
	case 0x18:
	case 0x19:
		in->kind = I_BLOCK;
		in->load = opcode & 0x0800;
		in->rn = r8;
		in->up = true;
		in->writeback = true;
		in->list = opcode & 0xFF;
		return;
	// Conditional branches, and SWI where the condition field is all ones.
	case 0x1A:
	case 0x1B: {
		int cond = (opcode >> 8) & 0xF;
		if (cond == 0xF) {
			in->kind = I_SWI;
			return;
		}
		if (cond == 0xE) {
			return;
		}
		in->kind = I_B;
		in->cond = cond;
		in->target = address + 4 + (int32_t) (int8_t) opcode * 2;
		return;
	}
	// The unconditional branch.
	case 0x1C:
		in->kind = I_B;
		in->target = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 20);
		return;
	case 0x1D:
		if ((opcode & 0xFFC0) == 0xE800) {
			vcCall(in);
		}
		return;
	// BL is two instructions: the first puts the target's upper bits in LR, the second adds the
	// rest and swaps LR with the return address.
	case 0x1E:
		in->kind = I_BL1;
		in->target = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 9);
		return;
	case 0x1F:
		in->kind = I_BL2;
		in->target = (opcode & 0x7FF) << 1;
		return;
	}
}

// Whether nothing can be compiled after this instruction in the same block: it leaves, or changes
// the CPU's state in a way the instructions after it weren't compiled for.
bool instEndsBlock(const struct Inst* in) {
	switch (in->kind) {
	case I_B:
	case I_BL2:
	case I_BX:
	case I_SWI:
	case I_UNDEFINED:
	case I_STUB:
	case I_MSR:
		return true;
	case I_ALU:
		// An ARM compare with r15 as its destination copies the SPSR to the CPSR.
		return in->rd == 15 && (!in->thumb || (in->op != ALU_TST && in->op != ALU_TEQ && in->op != ALU_CMP && in->op != ALU_CMN));
	case I_MEM:
		return in->load && (in->rd == 15 || (in->writeback && in->rn == 15));
	case I_BLOCK:
		return (in->load && (in->list & 0x8000)) || (in->load && !in->list) || in->userBank;
	default:
		return false;
	}
}
