/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_DECODE_H
#define ARMGBA_DECODE_H

#include <stdbool.h>
#include <stdint.h>

// What an instruction is after decoding; ARM and Thumb both come down to these.
enum InstKind {
	// Data processing: the sixteen operations of enum AluOp.
	I_ALU,
	// The 32-bit and the 64-bit multiplies.
	I_MUL,
	I_MULL,
	// A single load or store, and LDM/STM.
	I_MEM,
	I_BLOCK,
	// A branch, the two halves of Thumb's BL, and BX.
	I_B,
	I_BL1,
	I_BL2,
	I_BX,
	// SWP, and the status register moves.
	I_SWP,
	I_MRS,
	I_MSR,
	// A software interrupt, an undefined instruction, and one that does nothing and is never run.
	I_SWI,
	I_UNDEFINED,
	I_STUB,
};

// In ARM's opcode order, so the field is the value.
enum AluOp {
	ALU_AND,
	ALU_EOR,
	ALU_SUB,
	ALU_RSB,
	ALU_ADD,
	ALU_ADC,
	ALU_SBC,
	ALU_RSC,
	ALU_TST,
	ALU_TEQ,
	ALU_CMP,
	ALU_CMN,
	ALU_ORR,
	ALU_MOV,
	ALU_BIC,
	ALU_MVN,
};

// Likewise the shift types.
enum ShiftType {
	SHIFT_LSL,
	SHIFT_LSR,
	SHIFT_ASR,
	SHIFT_ROR,
};

enum {
	// Always.
	COND_AL = 0xE,
};

// A shifter operand: either a constant (with its carry-out if rotated) or a register shifted by a
// constant or by another register. For loads and stores, it is the offset.
struct Operand {
	// A constant, and whether it came from a rotation (which sets the carry).
	bool isImm;
	bool immRotated;
	uint32_t imm;
	// Or a register, how it is shifted, and by how much.
	uint8_t rm;
	uint8_t type;
	uint8_t amount;
	// The amount is another register's low byte.
	bool byReg;
	uint8_t rs;
};

struct Poly;

// One decoded instruction. Each kind uses the fields in its group and leaves the rest alone.
struct Inst {
	uint32_t address;
	uint32_t opcode;
	// enum InstKind, the condition field, and which instruction set it was decoded from.
	uint8_t kind;
	uint8_t cond;
	bool thumb;

	// Data processing and multiplies: the operation, whether flags are set, and the registers. rn
	// is the first operand or a load's base; op2 the second operand or a load's offset.
	uint8_t op;
	bool setFlags;
	uint8_t rd;
	uint8_t rn;
	uint8_t rm;
	uint8_t rs;
	uint8_t rdHi;
	struct Operand op2;

	// Multiplies: MLA-style, and signed for the long ones.
	bool accumulate;
	bool signedMul;

	// Loads and stores: direction, bytes moved, and the addressing mode (offset applied before or
	// after, added or subtracted, base written back).
	bool load;
	uint8_t size;
	bool signExtend;
	bool pre;
	bool up;
	bool writeback;

	// LDM and STM: one bit per register, and the S bit, which moves the user-mode registers.
	uint16_t list;
	bool userBank;

	// Branches: the address, and whether LR is set.
	uint32_t target;
	bool link;

	// MRS and MSR: which status register, and for MSR the bits its field mask lets through.
	bool spsr;
	uint32_t fieldMask;

	// The 12-bit offset is read from the instruction word at run time, since the game rewrites it.
	bool patchable;
	// The forms the game has written to this word, picked from when it runs; NULL for a word that
	// stays the same.
	const struct Poly* poly;
};

void decodeArm(struct Inst*, uint32_t address, uint32_t opcode);
void decodeThumb(struct Inst*, uint32_t address, uint16_t opcode);

bool instEndsBlock(const struct Inst*);

#endif
