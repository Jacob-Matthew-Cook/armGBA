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

// The unlicensed cartridges that are MBC5 with a lock on top: bank numbers with their bits out of
// order, and the switchable bank's data with its bits out of order (BBD, Hitek, GGB81) or XORed
// (Sintax). The tables are mGBA's and hhugboy's. A bank read through such a lock is unpacked into
// a spare bank past the ROM's own when it is switched in.
namespace {

typedef unsigned char Order[8];

Order const plain = { 0, 1, 2, 3, 4, 5, 6, 7 };

Order const bbdData[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 5, 1, 3, 4, 2, 6, 7 }, { 0, 4, 2, 3, 1, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 5, 3, 4, 2, 6, 7 },
};
Order const bbdBank[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 4, 2, 0, 1, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 1, 2, 3, 4, 0, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
Order const hitekData[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 6, 5, 3, 4, 1, 2, 7 }, { 0, 5, 6, 3, 4, 2, 1, 7 }, { 0, 6, 2, 3, 4, 5, 1, 7 },
	{ 0, 6, 1, 3, 4, 5, 2, 7 }, { 0, 1, 6, 3, 4, 5, 2, 7 }, { 0, 2, 6, 3, 4, 1, 5, 7 }, { 0, 6, 2, 3, 4, 1, 5, 7 },
};
Order const hitekBank[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 2, 1, 0, 4, 5, 6, 7 }, { 2, 1, 0, 3, 4, 5, 6, 7 }, { 1, 0, 3, 2, 4, 5, 6, 7 },
	{ 0, 3, 2, 1, 4, 5, 6, 7 }, { 2, 3, 0, 1, 4, 5, 6, 7 }, { 3, 0, 1, 2, 4, 5, 6, 7 }, { 2, 0, 3, 1, 4, 5, 6, 7 },
};
Order const ggb81Data[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 2, 1, 3, 4, 6, 5, 7 }, { 0, 6, 5, 3, 4, 2, 1, 7 }, { 0, 5, 1, 3, 4, 2, 6, 7 },
	{ 0, 5, 2, 3, 4, 1, 6, 7 }, { 0, 2, 6, 3, 4, 5, 1, 7 }, { 0, 1, 6, 3, 4, 2, 5, 7 }, { 0, 2, 5, 3, 4, 6, 1, 7 },
};
Order const sintaxBank[16] = {
	{ 2, 1, 4, 3, 6, 5, 0, 7 }, { 3, 2, 5, 4, 7, 6, 1, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 4, 5, 2, 3, 0, 1, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 6, 7, 4, 5, 1, 3, 0, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 7, 6, 1, 0, 3, 2, 5, 4 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 5, 4, 7, 6, 1, 0, 3, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 2, 3, 4, 5, 6, 7, 0, 1 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};

Order const ntOld1 = { 0, 2, 1, 4, 3, 5, 6, 7 };
Order const ntOld2 = { 1, 2, 0, 3, 4, 5, 6, 7 };

unsigned reorder(unsigned const value, Order const &order) {
	unsigned out = 0;
	for (unsigned bit = 0; bit < 8; ++bit)
		out |= (value >> order[bit] & 1) << bit;
	return out;
}

}

UnlMbc5::UnlMbc5(MemPtrs &memptrs, Kind const kind, unsigned const banks)
: memptrs_(memptrs)
, kind_(kind)
, banks_(banks)
, rombank_(1)
, rambank_(0)
, enableRam_(false)
, dataMode_(kind == hitek ? 7 : 0)
, bankMode_(kind == hitek ? 7 : 0)
, sintaxMode_(0xF)
, sintaxBank_(1)
, base_(0)
, count_(0)
, swapped_(false)
, split_(false)
{
	half_[0] = half_[1] = 2;
	xor_[0] = xor_[1] = xor_[2] = xor_[3] = 0;
	setRombank();
}

bool UnlMbc5::disabledRam() const {
	return !enableRam_;
}

void UnlMbc5::romWrite(unsigned const p, unsigned data, unsigned long const cc) {
	switch (kind_) {
	case bbd:
	case hitek:
	case ggb81:
		switch (p & 0xF0FF) {
		case 0x2000:
			if (kind_ != ggb81)
				data = reorder(data, kind_ == bbd ? bbdBank[bankMode_] : hitekBank[bankMode_]);
			break;
		case 0x2001:
			dataMode_ = data & 7;
			break;
		case 0x2080:
			if (kind_ != ggb81)
				bankMode_ = data & 7;
			break;
		case 0x0300:
			if (kind_ == hitek)
				return;
			break;
		}
		break;
	case licheng:
		if (p > 0x2100 && p < 0x3000)
			return;
		break;
	case sintax:
		if (p >= 0x2000 && p < 0x3000) {
			sintaxBank_ = data;
			data = reorder(data, sintaxBank[sintaxMode_]);
		} else if ((p & 0xF0F0) == 0x5010) {
			// The order of a bank number's bits; the bank last asked for is looked up again
			sintaxMode_ = data & 0xF;
			romWrite(0x2000, sintaxBank_, cc);
			return;
		} else if (p >= 0x7000) {
			unsigned const which = (p >> 4 & 0xF) - 2;
			if (which < 4)
				xor_[which] = data;
		}
		break;
	case ntold1:
	case ntold2:
		// MBC3's RAM switch, a bank number, and at 5000 the multicart's menu choosing a game
		if (p < 0x2000) {
			enableRam_ = (data & 0xF) == 0xA;
		} else if (p < 0x4000) {
			rombank_ = kind_ == ntold1 ? data & 0x1F : data;
		} else if ((p & 0xF000) == 0x5000) {
			switch (p & 3) {
			case 1:
				base_ = (data & 0x3F) * 2;
				break;
			case 2:
				switch (data & 0xF) {
				case 0x8: count_ = 16; break;
				case 0xC: count_ = 8; break;
				case 0xE: count_ = 4; break;
				case 0xF: count_ = 2; break;
				default: count_ = 32; break;
				}
				break;
			case 3:
				swapped_ = data & 0x10;
				break;
			}
		}
		setRambank();
		setRombank();
		return;
	case ntnew:
		// 55 written to 14xx splits the switchable area; the halves are then picked at 20xx and
		// 24xx (hhugboy's conditions, narrower than mGBA's)
		if (p >> 8 == 0x14 && data == 0x55) {
			split_ = true;
			return;
		}
		if (split_ && (p >> 8 == 0x20 || p >> 8 == 0x24)) {
			half_[p >> 10 & 1] = data < 2 ? 2 : data;
			setRombank();
			return;
		}
		break;
	}

	switch (p >> 13 & 3) {
	case 0:
		enableRam_ = (data & 0xF) == 0xA;
		break;
	case 1:
		rombank_ = p < 0x3000 ? (rombank_ & 0x100) | data : (data << 8 & 0x100) | (rombank_ & 0xFF);
		half_[0] = rombank_ * 2;
		half_[1] = rombank_ * 2 + 1;
		break;
	case 2:
		rambank_ = data & 0xF;
		break;
	}
	setRambank();
	setRombank();
}

void UnlMbc5::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_;
	ss.rambank = rambank_;
	ss.enableRam = enableRam_;
	if (kind_ == ntold1 || kind_ == ntold2) {
		ss.mbcExtra = base_ | count_ << 8 | swapped_ << 16;
		ss.mbcExtra2 = 0;
	} else if (kind_ == ntnew) {
		ss.mbcExtra = split_;
		ss.mbcExtra2 = half_[0] | 1ul * half_[1] << 16;
	} else {
		ss.mbcExtra = dataMode_ | bankMode_ << 4 | sintaxMode_ << 8 | 1ul * sintaxBank_ << 16;
		ss.mbcExtra2 = xor_[0] | xor_[1] << 8 | xor_[2] << 16 | 1ul * xor_[3] << 24;
	}
}

void UnlMbc5::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank;
	rambank_ = ss.rambank;
	enableRam_ = ss.enableRam;
	if (kind_ == ntold1 || kind_ == ntold2) {
		base_ = ss.mbcExtra & 0xFF;
		count_ = ss.mbcExtra >> 8 & 0xFF;
		swapped_ = ss.mbcExtra >> 16 & 1;
	} else if (kind_ == ntnew) {
		split_ = ss.mbcExtra & 1;
		half_[0] = ss.mbcExtra2 & 0xFFFF;
		half_[1] = ss.mbcExtra2 >> 16 & 0xFFFF;
	} else {
		dataMode_ = ss.mbcExtra & 7;
		bankMode_ = ss.mbcExtra >> 4 & 7;
		sintaxMode_ = ss.mbcExtra >> 8 & 0xF;
		sintaxBank_ = ss.mbcExtra >> 16 & 0xFF;
		for (unsigned i = 0; i < 4; ++i)
			xor_[i] = ss.mbcExtra2 >> i * 8 & 0xFF;
	}
	setRambank();
	setRombank();
}

void UnlMbc5::SyncState(NewState *ns, bool isReader) {
	NSS(rombank_);
	NSS(rambank_);
	NSS(enableRam_);
	NSS(dataMode_);
	NSS(bankMode_);
	NSS(sintaxMode_);
	NSS(sintaxBank_);
	NSS(xor_);
	NSS(base_);
	NSS(count_);
	NSS(swapped_);
	NSS(split_);
	NSS(half_);
}

void UnlMbc5::setRambank() const {
	memptrs_.setRambank(enableRam_ ? MemPtrs::read_en | MemPtrs::write_en : MemPtrs::disabled,
	                    rambank_ & (rambanks(memptrs_) - 1));
}

void UnlMbc5::setRombank() {
	unsigned char *const spare = memptrs_.romdata() + banks_ * rombank_size();
	if (kind_ == ntold1 || kind_ == ntold2) {
		unsigned bank = rombank_ ? rombank_ : 1;
		if (swapped_)
			bank = reorder(bank, kind_ == ntold1 ? ntOld1 : ntOld2);
		if (count_)
			bank &= count_ - 1;
		memptrs_.setRombank0(base_ & (banks_ - 1));
		memptrs_.setRombank((bank + base_) & (banks_ - 1));
		return;
	}
	if (kind_ == ntnew && split_) {
		// Two halves side by side in the spare bank
		for (unsigned i = 0; i < 2; ++i) {
			std::memcpy(spare + i * 0x2000, memptrs_.romdata() + (half_[i] & (banks_ * 2 - 1)) * 0x2000ul, 0x2000);
		}
		memptrs_.setRombank(banks_);
		return;
	}
	unsigned const bank = rombank_ & (banks_ - 1);
	Order const &order = kind_ == bbd ? bbdData[dataMode_] : kind_ == hitek ? hitekData[dataMode_]
	                   : kind_ == ggb81 ? ggb81Data[dataMode_] : plain;
	unsigned const mask = kind_ == sintax ? xor_[sintaxBank_ & 3] : 0;
	unsigned char table[256];
	bool same = true;
	for (unsigned i = 0; i < 256; ++i) {
		table[i] = reorder(i, order) ^ mask;
		same &= table[i] == i;
	}
	if (same) {
		memptrs_.setRombank(bank);
		return;
	}
	unsigned char const *const from = memptrs_.romdata() + bank * rombank_size();
	for (unsigned i = 0; i < rombank_size(); ++i)
		spare[i] = table[from[i]];
	memptrs_.setRombank(banks_);
}

}
