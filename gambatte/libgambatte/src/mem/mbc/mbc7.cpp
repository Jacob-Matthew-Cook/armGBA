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

// The frontend's reading of the tilt sensor, as an offset from lying flat.
int mbc7TiltX = 0;
int mbc7TiltY = 0;
// A flick of the console upwards, for the next reading the game takes: on the forward axis and
// towards the player, which Kirby Tilt 'n' Tumble takes as a flick; a single reading of it leaves
// Kirby where he was.
bool mbc7Flick = false;

// MBC7: a two-axis tilt sensor and a 93LC56 serial EEPROM of 128 words. The EEPROM is the first 256
// bytes of the cartridge RAM bank, with the state of its lines after them, so save states and the
// save file carry both.
namespace {

enum { words = 0, shift = 0x100, count = 0x102, mode = 0x103, address = 0x104, out = 0x105, lines = 0x107, writable = 0x108 };
enum { idle, command, reading, writing, writing_all, done };
enum { cs = 0x80, clk = 0x40, di = 0x02, dout = 0x01 };
unsigned const flat = 0x81D0;

}

Mbc7::Mbc7(MemPtrs &memptrs)
: memptrs_(memptrs)
, rombank_(1)
, enable1_(false)
, enable2_(false)
, x_(0x8000)
, y_(0x8000)
{
	// Before the first state is loaded, the bank holds nothing, so there are no line states to
	// keep.
	std::memset(memptrs_.rambankdata() + shift, 0, 16);
	setBanks();
}

bool Mbc7::disabledRam() const {
	return !(enable1_ && enable2_);
}

void Mbc7::romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/) {
	switch (p >> 13 & 3) {
	case 0: enable1_ = data == 0x0A; break;
	case 1: rombank_ = data & 0x7F; break;
	case 2: enable2_ = data == 0x40; break;
	}
	setBanks();
}

unsigned Mbc7::read(unsigned const p, unsigned long) const {
	if (p >= 0xB000)
		return 0xFF;
	unsigned char const *const s = memptrs_.rambankdata();
	switch (p >> 4 & 0xF) {
	case 2: return x_ & 0xFF;
	case 3: return x_ >> 8;
	case 4: return y_ & 0xFF;
	case 5: return y_ >> 8;
	case 6: return 0x00;
	case 8: return s[lines];
	}
	return 0xFF;
}

void Mbc7::write(unsigned const p, unsigned const data, unsigned long) {
	if (p >= 0xB000)
		return;
	switch (p >> 4 & 0xF) {
	case 0:
		if (data == 0x55)
			x_ = y_ = 0x8000;
		break;
	case 1:
		if (data == 0xAA) {
			x_ = flat + mbc7TiltX;
			y_ = flat + mbc7TiltY - (mbc7Flick ? 0xCD : 0);
			mbc7Flick = false;
		}
		break;
	case 8:
		eeprom(data);
		break;
	}
}

// The chip's lines: select, clock and data in; data out is kept in the low bit.
void Mbc7::eeprom(unsigned const data) {
	unsigned char *const s = memptrs_.rambankdata();
	unsigned const before = s[lines];
	unsigned bit = before & dout;
	if (!(data & cs)) {
		s[mode] = idle;
		s[count] = 0;
		bit = dout;
	} else if (data & clk && !(before & clk)) {
		unsigned const in = data >> 1 & 1;
		unsigned value = s[shift] | s[shift + 1] << 8;
		switch (s[mode]) {
		case idle:
			// A start bit, then two bits of command and eight of address.
			if (in) {
				s[mode] = command;
				s[count] = 0;
				value = 0;
			}
			break;
		case command:
			value = (value << 1 | in) & 0x3FF;
			if (++s[count] == 10) {
				s[address] = value & 0x7F;
				s[count] = 0;
				switch (value >> 8) {
				case 2:
					s[mode] = reading;
					s[out] = s[words + s[address] * 2];
					s[out + 1] = s[words + s[address] * 2 + 1];
					bit = 0;
					break;
				case 1:
					s[mode] = writing;
					break;
				case 3:
					if (s[writable])
						s[words + s[address] * 2] = s[words + s[address] * 2 + 1] = 0xFF;
					s[mode] = done;
					bit = dout;
					break;
				default:
					switch (value >> 6 & 3) {
					case 0: s[writable] = 0; s[mode] = done; break;
					case 3: s[writable] = 1; s[mode] = done; break;
					case 1: s[mode] = writing_all; break;
					case 2:
						if (s[writable])
							std::memset(s + words, 0xFF, 0x100);
						s[mode] = done;
						break;
					}
					bit = dout;
				}
				value = 0;
			}
			break;
		case reading: {
			unsigned word = s[out] | s[out + 1] << 8;
			bit = word >> 15 & 1;
			word <<= 1;
			if (++s[count] == 16) {
				// One word after another, for as long as the chip stays selected.
				s[count] = 0;
				s[address] = (s[address] + 1) & 0x7F;
				word = s[words + s[address] * 2] | s[words + s[address] * 2 + 1] << 8;
			}
			s[out] = word & 0xFF;
			s[out + 1] = word >> 8 & 0xFF;
			break;
		}
		case writing:
		case writing_all:
			value = (value << 1 | in) & 0xFFFF;
			if (++s[count] == 16) {
				if (s[writable]) {
					for (unsigned i = s[mode] == writing ? s[address] : 0; i < (s[mode] == writing ? s[address] + 1u : 0x80u); ++i) {
						s[words + i * 2] = value & 0xFF;
						s[words + i * 2 + 1] = value >> 8;
					}
				}
				s[mode] = done;
				bit = dout;
			}
			break;
		}
		s[shift] = value & 0xFF;
		s[shift + 1] = value >> 8;
	}
	s[lines] = (data & (cs | clk | di)) | bit;
}

void Mbc7::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_;
	ss.enableRam = enable1_;
	ss.rambankMode = enable2_;
	ss.mbcExtra = x_ | 1ul * y_ << 16;
}

void Mbc7::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank;
	enable1_ = ss.enableRam;
	enable2_ = ss.rambankMode;
	x_ = ss.mbcExtra & 0xFFFF;
	y_ = ss.mbcExtra >> 16 & 0xFFFF;
	setBanks();
}

bool Mbc7::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return (addr < rombank_size()) == (bank == 0);
}

void Mbc7::SyncState(NewState *ns, bool isReader) {
	NSS(rombank_);
	NSS(enable1_);
	NSS(enable2_);
	NSS(x_);
	NSS(y_);
}

void Mbc7::setBanks() const {
	memptrs_.setRambank(enable1_ && enable2_ ? MemPtrs::read_en | MemPtrs::write_en | MemPtrs::rtc_en : MemPtrs::disabled, 0);
	memptrs_.setRombank(rombank_ & (rombanks(memptrs_) - 1));
}

// MBC6 (Net de Get): the switchable area works as two 8 KiB halves, put side by side in a spare
// bank past the ROM's own. Its flash memory isn't emulated; a half switched to it reads as erased.
Mbc6::Mbc6(MemPtrs &memptrs, unsigned const banks)
: memptrs_(memptrs)
, banks_(banks)
, enableRam_(false)
, rambank_(0)
{
	half_[0] = 2;
	half_[1] = 3;
	flash_[0] = flash_[1] = false;
	setBanks();
}

bool Mbc6::disabledRam() const {
	return !enableRam_;
}

void Mbc6::romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/) {
	switch (p >> 10) {
	case 0x0: enableRam_ = (data & 0xF) == 0xA; break;
	case 0x1: rambank_ = data & 7; break;
	case 0x8:
	case 0x9: half_[0] = data; break;
	case 0xA:
	case 0xB: flash_[0] = data & 8; break;
	case 0xC:
	case 0xD: half_[1] = data; break;
	case 0xE:
	case 0xF: flash_[1] = data & 8; break;
	}
	setBanks();
}

void Mbc6::saveState(SaveState::Mem &ss) const {
	ss.rombank = half_[0] | half_[1] << 8;
	ss.rambank = rambank_;
	ss.enableRam = enableRam_;
	ss.mbcExtra = flash_[0] | flash_[1] << 1;
}

void Mbc6::loadState(SaveState::Mem const &ss) {
	half_[0] = ss.rombank & 0xFF;
	half_[1] = ss.rombank >> 8;
	rambank_ = ss.rambank;
	enableRam_ = ss.enableRam;
	flash_[0] = ss.mbcExtra & 1;
	flash_[1] = ss.mbcExtra >> 1 & 1;
	setBanks();
}

void Mbc6::SyncState(NewState *ns, bool isReader) {
	NSS(half_);
	NSS(flash_);
	NSS(rambank_);
	NSS(enableRam_);
}

void Mbc6::setBanks() const {
	unsigned char *const spare = memptrs_.romdata() + banks_ * rombank_size();
	for (unsigned i = 0; i < 2; ++i) {
		if (flash_[i])
			std::memset(spare + i * 0x2000, 0xFF, 0x2000);
		else
			std::memcpy(spare + i * 0x2000, memptrs_.romdata() + (half_[i] & (banks_ * 2 - 1)) * 0x2000ul, 0x2000);
	}
	memptrs_.setRombank(banks_);
	// The RAM switches in 4 KiB halves as well; the first half's number picks the 8 KiB here.
	memptrs_.setRambank(enableRam_ ? MemPtrs::read_en | MemPtrs::write_en : MemPtrs::disabled,
	                    (rambank_ >> 1) & (rambanks(memptrs_) - 1));
}

}
