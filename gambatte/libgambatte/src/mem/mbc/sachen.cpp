//
//   Copyright (c) 2026 Magnominus
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

// Sachen's multicart mapper, as it is once its logo check has let the game start. The ROM is held
// twice: the lower half with the header page as the cartridge's scrambled address lines show it,
// for the fixed area; the upper half as dumped, for the switchable one.
Sachen::Sachen(MemPtrs &memptrs)
: memptrs_(memptrs)
, bank_(1)
, base_(0)
, mask_(0)
{
	setBanks();
}

unsigned Sachen::unscramble(unsigned const p) {
	return (p & 0xFFAC) | (p & 0x40) >> 6 | (p & 0x10) >> 3 | (p & 0x02) << 3 | (p & 0x01) << 6;
}

bool Sachen::disabledRam() const {
	return true;
}

void Sachen::romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/) {
	switch (p >> 13 & 3) {
	case 0:
		if ((bank_ & 0x30) == 0x30)
			base_ = data;
		break;
	case 1:
		bank_ = data ? data : 1;
		break;
	case 2:
		if ((bank_ & 0x30) == 0x30)
			mask_ = data;
		break;
	}
	setBanks();
}

void Sachen::saveState(SaveState::Mem &ss) const {
	ss.rombank = bank_ | base_ << 8;
	ss.rambank = mask_;
}

void Sachen::loadState(SaveState::Mem const &ss) {
	bank_ = ss.rombank & 0xFF;
	base_ = ss.rombank >> 8;
	mask_ = ss.rambank;
	setBanks();
}

bool Sachen::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return (addr < rombank_size()) == (bank < rombanks(memptrs_) / 2);
}

void Sachen::SyncState(NewState *ns, bool isReader) {
	NSS(bank_);
	NSS(base_);
	NSS(mask_);
}

void Sachen::setBanks() const {
	unsigned const half = rombanks(memptrs_) / 2;
	unsigned base = base_ & mask_;
	// A 512 KiB cartridge of two games (Rocman X Gold + 4 in 1) has the second at its upper half and
	// picks it with the base's bit 6, as its own start-up code shows by comparing the two headers
	if (half == 32 && base & 0x40)
		base = (base & 0x1F) | 0x10;
	memptrs_.setRombank0(base & (half - 1));
	memptrs_.setRombank(half + (((bank_ & ~mask_) | base) & (half - 1)));
}

}
