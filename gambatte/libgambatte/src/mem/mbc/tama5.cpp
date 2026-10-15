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

// Bandai's TAMA5 with its TAMA6 clock, as mGBA documents them. All of it is kept in the cartridge
// RAM bank past the chip's own 32 bytes, so save states and the save file carry it. The clock runs
// on emulated time.
namespace {

enum {
	ram_size = 0x20,
	regs = 0x20,      // the eight nibble registers
	reg = 0x28,       // the one selected
	stopped = 0x29,
	marker = 0x2F,
	timer_page = 0x30,
	alarm_page = 0x40,
	free_page0 = 0x50,
	free_page1 = 0x60,
	last_cc = 0x70,   // the cycle the clock is counted up to, four bytes
	marked = 0x5A,
};

enum { bank_lo, bank_hi, write_lo = 4, write_hi, addr_hi, addr_lo, active = 0xA, read_lo = 0xC, read_hi };
enum { second_1, second_10, minute_1, minute_10, hour_1, hour_10, week, day_1, day_10, month_1, month_10,
       year_1, year_10, page };
enum { is_24_hour = 0xA, leap_year = 0xB };

unsigned char const rtcMask[32] = {
	0xF, 0x7, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0xF, 0x1, 0xF, 0xF, 0x0, 0x0, 0x0,
	0x0, 0x0, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0x0, 0x1, 0x3, 0x0, 0x0, 0x0, 0x0,
};

// Adds one to a two-digit count kept as two nibbles; returns true when it wraps to `first`.
bool step(unsigned char *const ones, unsigned const first, unsigned const count) {
	unsigned const value = ones[0] + ones[1] * 10 + 1;
	bool const wrapped = value >= first + count;
	ones[0] = (wrapped ? first : value) % 10;
	ones[1] = (wrapped ? first : value) / 10;
	return wrapped;
}

}

Tama5::Tama5(MemPtrs &memptrs)
: memptrs_(memptrs)
{
	memptrs_.setRambank(MemPtrs::read_en | MemPtrs::write_en | MemPtrs::rtc_en, 0);
	memptrs_.setRombank(0);
}

unsigned char * Tama5::chip() const {
	unsigned char *const s = memptrs_.rambankdata();
	if (s[marker] != marked) {
		// New memory, or a save from elsewhere with only the chip's 32 bytes.
		std::memset(s + ram_size, 0, 0x80 - ram_size);
		s[alarm_page + page] = 1;
		s[free_page0 + page] = 2;
		s[free_page1 + page] = 3;
		s[marker] = marked;
	}
	return s;
}

bool Tama5::disabledRam() const {
	return false;
}

void Tama5::romWrite(unsigned, unsigned, unsigned long) {
}

void Tama5::resetCc(unsigned long const oldCc, unsigned long const newCc) {
	unsigned char *const s = chip();
	unsigned long last = s[last_cc] | s[last_cc + 1] << 8 | s[last_cc + 2] << 16 | 1ul * s[last_cc + 3] << 24;
	last = last > oldCc - newCc ? last - (oldCc - newCc) : 0;
	for (unsigned i = 0; i < 4; ++i)
		s[last_cc + i] = last >> i * 8;
}

// Brings the clock up to now: 2^22 cycles to the second.
void Tama5::tick(unsigned long const cc) const {
	unsigned char *const s = chip();
	unsigned long last = s[last_cc] | s[last_cc + 1] << 8 | s[last_cc + 2] << 16 | 1ul * s[last_cc + 3] << 24;
	if (last > cc)
		last = cc;
	unsigned char *const t = s + timer_page;
	for (; cc - last >= 1ul << 22; last += 1ul << 22) {
		if (s[stopped] || !step(t + second_1, 0, 60) || !step(t + minute_1, 0, 60))
			continue;
		unsigned hour = t[hour_1];
		if (s[alarm_page + is_24_hour])
			hour += t[hour_10] * 10;
		else
			hour += (t[hour_10] & 1) * 10 + (t[hour_10] & 2) * 6;
		hour = (hour + 1) % 24;
		if (s[alarm_page + is_24_hour]) {
			t[hour_1] = hour % 10;
			t[hour_10] = hour / 10;
		} else {
			t[hour_1] = hour % 12 % 10;
			t[hour_10] = hour % 12 / 10 + hour / 12 * 2;
		}
		if (hour)
			continue;
		t[week] = (t[week] + 1) % 7;
		static unsigned char const days[13] = { 31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		unsigned const month = t[month_1] + t[month_10] * 10;
		unsigned const length = month == 2 && !(s[alarm_page + leap_year] & 3) ? 29 : days[month <= 12 ? month : 0];
		if (step(t + day_1, 1, length) && step(t + month_1, 1, 12)) {
			step(t + year_1, 0, 100);
			s[alarm_page + leap_year] = (s[alarm_page + leap_year] + 1) & 3;
		}
	}
	for (unsigned i = 0; i < 4; ++i)
		s[last_cc + i] = last >> i * 8;
}

void Tama5::write(unsigned const p, unsigned data, unsigned long const cc) {
	unsigned char *const s = chip();
	if (p & 1) {
		s[reg] = data;
		return;
	}
	if (s[reg] >= 8)
		return;
	tick(cc);
	unsigned char *const r = s + regs;
	r[s[reg]] = data & 0xF;
	unsigned const address = (r[addr_hi] << 4 & 0x10) | r[addr_lo];
	unsigned const out = r[write_hi] << 4 | r[write_lo];
	if (s[reg] == bank_lo || s[reg] == bank_hi) {
		setRombank();
	} else if (s[reg] == addr_lo) {
		switch (r[addr_hi] >> 1) {
		case 0:
			s[address] = out;
			break;
		case 2:
			switch (address) {
			case 0x00:
			case 0x01:
				s[stopped] = !address;
				if (address)
					s[timer_page + second_1] = s[timer_page + second_10] = 0;
				for (unsigned i = timer_page; i <= free_page1; i += 0x10)
					s[i + page] = (s[i + page] & 0x7) | address << 3;
				break;
			case 0x04:
				s[timer_page + minute_1] = out & 0xF;
				s[timer_page + minute_10] = out >> 4;
				break;
			case 0x05:
				s[timer_page + hour_1] = out & 0xF;
				s[timer_page + hour_10] = out >> 4;
				break;
			case 0x10:
			case 0x11:
				for (unsigned i = timer_page; i <= free_page1; i += 0x10)
					s[i + page] = (s[i + page] & 0xB) | (address & 1) << 2;
				break;
			}
			break;
		case 4:
			if (r[write_lo] >= page)
				break;
			switch (r[addr_lo]) {
			case 0: s[timer_page + r[write_lo]] = r[write_hi] & rtcMask[r[write_lo]]; break;
			case 2: s[alarm_page + r[write_lo]] = r[write_hi] & rtcMask[r[write_lo] | 0x10]; break;
			case 4: s[free_page0 + r[write_lo]] = r[write_hi]; break;
			case 6: s[free_page1 + r[write_lo]] = r[write_hi]; break;
			}
			break;
		}
	}
}

unsigned Tama5::read(unsigned const p, unsigned long const cc) const {
	if (p & 1)
		return 0xFF;
	unsigned char *const s = chip();
	unsigned char const *const r = s + regs;
	if (s[reg] != read_lo && s[reg] != read_hi)
		return 0xF1;
	unsigned const address = (r[addr_hi] << 4 & 0x10) | r[addr_lo];
	unsigned value = 0xF0;
	switch (r[addr_hi] >> 1) {
	case 1:
		value = s[address];
		break;
	case 2:
		tick(cc);
		if (address == 0x06)
			value = s[timer_page + minute_10] << 4 | s[timer_page + minute_1];
		else if (address == 0x07)
			value = s[timer_page + hour_10] << 4 | s[timer_page + hour_1];
		else
			value = address;
		break;
	case 4:
		if (s[reg] == read_hi)
			break;
		tick(cc);
		if (r[write_lo] > page) {
			value = 0;
			break;
		}
		switch (r[addr_lo]) {
		case 1: value = s[timer_page + r[write_lo]]; break;
		case 3: value = s[alarm_page + r[write_lo]]; break;
		case 5: value = s[free_page0 + r[write_lo]]; break;
		case 7: value = s[free_page1 + r[write_lo]]; break;
		}
		break;
	}
	if (s[reg] == read_hi)
		value >>= 4;
	return value | 0xF0;
}

void Tama5::saveState(SaveState::Mem &ss) const {
	ss.rombank = chip()[regs + bank_lo] | chip()[regs + bank_hi] << 4;
}

void Tama5::loadState(SaveState::Mem const &) {
	memptrs_.setRambank(MemPtrs::read_en | MemPtrs::write_en | MemPtrs::rtc_en, 0);
	setRombank();
}

bool Tama5::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return (addr < rombank_size()) == (bank == 0);
}

void Tama5::SyncState(NewState *, bool) {
}

void Tama5::setRombank() const {
	unsigned char const *const r = chip() + regs;
	memptrs_.setRombank((r[bank_lo] | r[bank_hi] << 4) & (rombanks(memptrs_) - 1));
}

}
