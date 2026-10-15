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

Pkjd::Pkjd(MemPtrs &memptrs)
: memptrs_(memptrs)
, rombank_(1)
, select_(0)
, enableRam_(false)
{
	counters_[0] = counters_[1] = 0;
	setBanks();
}

bool Pkjd::disabledRam() const {
	return !enableRam_;
}

void Pkjd::romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/) {
	switch (p >> 13 & 3) {
	case 0:
		enableRam_ = (data & 0xF) == 0xA;
		break;
	case 1:
		rombank_ = data & 0x7F;
		break;
	case 2:
		// A RAM bank, or one of the counters (D, E), or their command register (F).
		if (data < 8 || (data >= 0xD && data <= 0xF))
			select_ = data;
		break;
	}
	setBanks();
}

unsigned Pkjd::read(unsigned, unsigned long) const {
	return select_ == 0xD || select_ == 0xE ? counters_[select_ - 0xD] : 0;
}

void Pkjd::write(unsigned, unsigned const data, unsigned long) {
	if (select_ == 0xD || select_ == 0xE) {
		counters_[select_ - 0xD] = data;
	} else if (select_ == 0xF) {
		switch (data) {
		case 0x11: --counters_[0]; break;
		case 0x12: --counters_[1]; break;
		case 0x41: counters_[0] += counters_[1]; break;
		case 0x42: counters_[1] += counters_[0]; break;
		case 0x51: ++counters_[0]; break;
		case 0x52: --counters_[1]; break;
		}
	}
}

void Pkjd::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_ | counters_[0] << 8;
	ss.rambank = select_;
	ss.ramflag = counters_[1];
	ss.enableRam = enableRam_;
}

void Pkjd::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank & 0xFF;
	counters_[0] = ss.rombank >> 8;
	select_ = ss.rambank;
	counters_[1] = ss.ramflag;
	enableRam_ = ss.enableRam;
	setBanks();
}

bool Pkjd::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return (addr < rombank_size()) == (bank == 0);
}

void Pkjd::SyncState(NewState *ns, bool isReader) {
	NSS(rombank_);
	NSS(select_);
	NSS(counters_);
	NSS(enableRam_);
}

void Pkjd::setBanks() const {
	unsigned flags = enableRam_ ? MemPtrs::read_en | MemPtrs::write_en : MemPtrs::disabled;
	if (enableRam_ && select_ >= 8)
		flags |= MemPtrs::rtc_en;
	memptrs_.setRambank(flags, select_ & (rambanks(memptrs_) - 1));
	memptrs_.setRombank((rombank_ ? rombank_ : 1) & (rombanks(memptrs_) - 1));
}

}
