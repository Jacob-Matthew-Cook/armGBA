//
//   Copyright (c) 2026 Jacob Cook
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//

#include "mbc.h"

namespace gambatte {

// Vast Fame's protected cartridge, after taizou's findings in hhugboy (MbcUnlVf001): MBC5 with a
// lock set up by writes to 6000-7FFF. Each such write turns a running value and XORs the byte into
// it, and the resulting value is stored by address. With those values the game arms two things. One
// is a read of a particular ROM address that makes the next one to four ROM reads return bytes the
// game supplied. The other is bank 0, from some address up, read from another bank. The second is
// assembled in a spare bank past the ROM's own; for the first, ROM reads take the slow path while
// it is armed.
Vf001::Vf001(MemPtrs &memptrs, unsigned const banks, bool const maybeGgb81)
: memptrs_(memptrs)
, banks_(banks)
, start_(0)
, rombank_(1)
, rambank_(0)
, enableRam_(false)
, config_(false)
, running_(0)
, reg6000_(0)
, left_(0)
, turns_(0)
, decided_(false)
, opened_(!maybeGgb81)
, maybeGgb81_(maybeGgb81)
, dataMode_(0)
{
	std::memset(reg700x_, 0, sizeof reg700x_);
	std::memset(turnOf_, 0, sizeof turnOf_);
	setBanks();
}

bool Vf001::disabledRam() const {
	return !enableRam_;
}

// How many reads the armed sequence answers.
unsigned Vf001::length() const {
	return (reg700x_[0] & 7) >= 4 ? (reg700x_[0] & 7) - 3 : 0;
}

namespace {

// GGB81's orders of data bits, as in unl.cpp.
unsigned char const ggb81Data[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 2, 1, 3, 4, 6, 5, 7 }, { 0, 6, 5, 3, 4, 2, 1, 7 }, { 0, 5, 1, 3, 4, 2, 6, 7 },
	{ 0, 5, 2, 3, 4, 1, 6, 7 }, { 0, 2, 6, 3, 4, 5, 1, 7 }, { 0, 1, 6, 3, 4, 2, 5, 7 }, { 0, 2, 5, 3, 4, 6, 1, 7 },
};

// Where the difference between the two kinds' starting values is after so many turns.
unsigned turned(unsigned const turns) {
	return (0x10 >> (turns & 7) | 0x10 << (8 - (turns & 7))) & 0xFF;
}

}

// The registers as the other kind of cartridge would have them.
void Vf001::otherKind() {
	start_ ^= 0x10;
	running_ ^= turned(turns_);
	reg6000_ ^= turned(turnOf_[11]);
	for (unsigned i = 0; i < sizeof reg700x_; ++i)
		reg700x_[i] ^= turned(turnOf_[i]);
	decided_ = true;
}

unsigned Vf001::romRead(unsigned const p) {
	if (!decided_ && !left_) {
		// Whichever kind's trigger address the game reads first is the kind this cartridge is.
		unsigned const otherBank = reg700x_[3] ^ turned(turnOf_[3]);
		unsigned const other = (reg700x_[2] ^ turned(turnOf_[2])) << 8 | (reg700x_[1] ^ turned(turnOf_[1]));
		unsigned const mine = reg700x_[2] << 8 | reg700x_[1];
		if (p == mine) {
			decided_ = true;
		} else if (p == other && ((otherBank == 0 && p < 0x3FFF) || (otherBank == rombank_ && p >= 4000))) {
			otherKind();
			setBanks();
		}
	}
	unsigned const bank = reg700x_[3];
	unsigned const address = reg700x_[2] << 8 | reg700x_[1];
	// As hhugboy has it, bounds included.
	bool const inBank = (bank == 0 && p < 0x3FFF) || (bank == rombank_ && p >= 4000);
	if (inBank && p == address && !left_)
		left_ = length();
	if (left_) {
		--left_;
		return reg700x_[4 + length() - left_ - 1];
	}
	return memptrs_.romdata(p >> 14)[p];
}

void Vf001::romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/) {
	if (p >= 0x6000) {
		unsigned const reg = p & 0xF00F;
		if (reg == 0x7000 && data == 0x96) {
			opened_ = true;
			dataMode_ = 0;
			config_ = true;
			running_ = start_;
			turns_ = 0;
		} else if (reg == 0x700F && data == 0x96) {
			config_ = false;
		} else if (config_ && (reg == 0x6000 || (reg >= 0x7000 && reg < 0x700B))) {
			running_ = ((running_ & 1) << 7 | running_ >> 1) ^ data;
			turns_ = (turns_ + 1) & 7;
			unsigned const index = reg == 0x6000 ? 11 : reg & 0xF;
			turnOf_[index] = turns_;
			// The running value starts at 00 on Vast Fame's own cartridges and at 10 on the
			// mainland releases of the same games. Nothing in the ROM says which; a command that
			// only makes sense the other way does.
			if (reg == 0x6000)
				reg6000_ = running_;
			else
				reg700x_[reg & 0xF] = running_;
			unsigned const other = running_ ^ turned(turns_);
			if (!decided_ && ((reg == 0x7000 && (running_ & 7) < 4 && (other & 7) >= 4)
					|| (reg == 0x7008 && (running_ & 0xF) != 0xF && (other & 0xF) == 0xF)))
				otherKind();
			if (reg == 0x7000)
				left_ = 0;
			setBanks();
		}
		return;
	}
	if (!opened_ && (p & 0xF0FF) == 0x2001)
		dataMode_ = data & 7;
	switch (p >> 13 & 3) {
	case 0:
		enableRam_ = (data & 0xF) == 0xA;
		break;
	case 1:
		rombank_ = p < 0x3000 ? (rombank_ & 0x100) | data : (data << 8 & 0x100) | (rombank_ & 0xFF);
		break;
	case 2:
		rambank_ = data & 0xF;
		break;
	}
	setBanks();
}

void Vf001::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_;
	ss.rambank = rambank_;
	ss.enableRam = enableRam_;
	ss.mbcExtra = running_ | reg6000_ << 8 | left_ << 16 | 1ul * config_ << 24 | 1ul * decided_ << 25;
	ss.mbcExtra2 = reg700x_[0] | reg700x_[1] << 8 | reg700x_[2] << 16 | 1ul * reg700x_[3] << 24;
	ss.mbcExtra3 = reg700x_[4] | reg700x_[5] << 8 | reg700x_[6] << 16 | 1ul * reg700x_[7] << 24;
	ss.mbcExtra4 = reg700x_[8] | reg700x_[9] << 8 | reg700x_[10] << 16 | 1ul * (start_ | dataMode_ | opened_ << 5) << 24;
}

void Vf001::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank;
	rambank_ = ss.rambank;
	enableRam_ = ss.enableRam;
	running_ = ss.mbcExtra & 0xFF;
	reg6000_ = ss.mbcExtra >> 8 & 0xFF;
	left_ = ss.mbcExtra >> 16 & 0xFF;
	config_ = ss.mbcExtra >> 24 & 1;
	decided_ = ss.mbcExtra >> 25 & 1;
	unsigned long const words[3] = { ss.mbcExtra2, ss.mbcExtra3, ss.mbcExtra4 };
	for (unsigned i = 0; i < sizeof reg700x_; ++i)
		reg700x_[i] = words[i / 4] >> i % 4 * 8 & 0xFF;
	start_ = ss.mbcExtra4 >> 24 & 0x10;
	dataMode_ = ss.mbcExtra4 >> 24 & 7;
	opened_ = !maybeGgb81_ || (ss.mbcExtra4 >> 29 & 1);
	setBanks();
}

void Vf001::SyncState(NewState *ns, bool isReader) {
	NSS(rombank_);
	NSS(rambank_);
	NSS(enableRam_);
	NSS(config_);
	NSS(running_);
	NSS(reg6000_);
	NSS(reg700x_);
	NSS(left_);
	NSS(start_);
	NSS(decided_);
	NSS(opened_);
	NSS(dataMode_);
}

void Vf001::setBanks() const {
	memptrs_.setRambank(enableRam_ ? MemPtrs::read_en | MemPtrs::write_en : MemPtrs::disabled,
	                    rambank_ & (rambanks(memptrs_) - 1));
	unsigned const bank = rombank_ & (banks_ - 1);
	if (dataMode_) {
		unsigned char table[256];
		for (unsigned i = 0; i < 256; ++i) {
			table[i] = 0;
			for (unsigned bit = 0; bit < 8; ++bit)
				table[i] |= (i >> ggb81Data[dataMode_][bit] & 1) << bit;
		}
		unsigned char const *const from = memptrs_.romdata() + bank * rombank_size();
		unsigned char *const to = memptrs_.romdata() + (banks_ + 1) * rombank_size();
		for (unsigned i = 0; i < rombank_size(); ++i)
			to[i] = table[from[i]];
		memptrs_.setRombank(banks_ + 1);
	} else
		memptrs_.setRombank(bank);
	// Bank 0 with its upper part from another bank, when 7008's value ends in F.
	unsigned const from = reg700x_[10] << 8 | reg700x_[9];
	if ((reg700x_[8] & 0xF) == 0xF && from < rombank_size()) {
		unsigned char *const spare = memptrs_.romdata() + banks_ * rombank_size();
		std::memcpy(spare, memptrs_.romdata(), from);
		std::memcpy(spare + from, memptrs_.romdata() + (reg6000_ & (banks_ - 1)) * rombank_size() + from, rombank_size() - from);
		memptrs_.setRombank0(banks_);
	} else
		memptrs_.setRombank0(0);
	memptrs_.setRomHooked(length());
}

}
