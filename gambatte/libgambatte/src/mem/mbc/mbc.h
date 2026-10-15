//
//   Copyright (C) 2007-2010 by sinamas <sinamas at users.sourceforge.net>
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
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

#ifndef MBC_H
#define MBC_H

#include <cstring>
#include "../memptrs.h"
#include "../time.h"
#include "../rtc.h"
#include "../infrared.h"
#include "../huc3_chip.h"
#include "../camera.h"
#include "savestate.h"
#include "newstate.h"

#include <string>
#include <vector>

namespace gambatte {

inline unsigned rambanks(MemPtrs const &memptrs) {
	return (memptrs.rambankdataend() - memptrs.rambankdata()) / rambank_size();
}

inline unsigned rombanks(MemPtrs const &memptrs) {
	return (memptrs.romdataend() - memptrs.romdata()) / rombank_size();
}

class Mbc {
public:
	virtual ~Mbc() {}
	virtual bool disabledRam() const = 0;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cycleCounter) = 0;
	virtual void saveState(SaveState::Mem &ss) const = 0;
	virtual void loadState(SaveState::Mem const &ss) = 0;
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned address, unsigned rombank) const = 0;
	template<bool isReader>void SyncState(NewState *ns) {
		// can't have virtual templates, so..
		SyncState(ns, isReader);
	}
	virtual void SyncState(NewState *ns, bool isReader) = 0;
};

class DefaultMbc : public Mbc {
public:
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
		return (addr < rombank_size()) == (bank == 0);
	}

	virtual void SyncState(NewState */*ns*/, bool /*isReader*/)
	{
	}
};

class Mbc0 : public DefaultMbc {
public:
	explicit Mbc0(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	bool enableRam_;
};

class Mbc1 : public Mbc {
public:
	explicit Mbc1(MemPtrs &memptrs, unsigned char rombankMask = 0x1Fu, unsigned char rombankShift = 0x05u);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	bool enableRam_;
	bool rambankMode_;
	unsigned char bankReg1_;
	unsigned char bankReg2_;
	unsigned char rombank0_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char rombankMask_;
	unsigned char rombankShift_;

	void updateBanking();
	void setRambank() const;
	void setRombank() const;
};

class Mbc1Multi64 : public Mbc1 {
public:
	explicit Mbc1Multi64(MemPtrs &memptrs)
	: Mbc1(memptrs, 0x0Fu, 0x04u)
	{
	}
};

class Mbc2 : public DefaultMbc {
public:
	explicit Mbc2(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool enableRam_;
};

class Mbc3 : public DefaultMbc {
public:
	Mbc3(MemPtrs &memptrs, Rtc *const rtc, unsigned char rombankMask = 0x7Fu, unsigned char rambankMask = 0x03u);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Rtc *const rtc_;
	unsigned char rombank_;
	unsigned char rambank_;
	bool enableRam_;
	unsigned char rombankMask_;
	unsigned char rambankMask_;

	void setRambank(unsigned flags = MemPtrs::read_en | MemPtrs::write_en) const;
	void setRombank() const;
};

class Mbc30 : public Mbc3 {
public:
	Mbc30(MemPtrs &memptrs, Rtc *const rtc)
	: Mbc3(memptrs, rtc, 0xFFu, 0x07u)
	{
	}
};

class Mbc5 : public DefaultMbc {
public:
	explicit Mbc5(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;

	void setRambank() const;
	void setRombank() const;
};

class M161 : public Mbc {
public:
	explicit M161(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const /*p*/, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState* ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool mapped_;

	void setRombank() const;
};

class Mmm01 : public Mbc {
public:
	explicit Mmm01(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char bankReg0_;
	unsigned char bankReg1_;
	unsigned char bankReg2_;
	unsigned char bankReg3_;

	bool isMapped() const;
	bool enableRam() const;
	unsigned char rombankLow(bool upper) const;
	unsigned char rombankMid() const;
	unsigned char rombankHigh() const;
	unsigned char rombankMask() const;
	unsigned char rambankLow(bool noMode) const;
	unsigned char rambankHigh() const;
	unsigned char rambankMask() const;
	bool isMuxed() const;
	void updateBanking() const;
};

class HuC1 : public DefaultMbc {
public:
	explicit HuC1(MemPtrs &memptrs, Infrared *const ir_);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Infrared *const ir_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char ramflag_;

	void setRambank(bool loadingState = false) const;
	void setRombank() const;
};

class HuC3 : public DefaultMbc {
public:
	explicit HuC3(MemPtrs &memptrs, HuC3Chip *const huc3);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	HuC3Chip *const huc3_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char ramflag_;

	void setRambank(bool loadingState = false) const;
	void setRombank() const;
};

class PocketCamera : public DefaultMbc {
public:
	explicit PocketCamera(MemPtrs &memptrs, Camera *const camera);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Camera *const camera_;
	unsigned char rombank_;
	unsigned char rambank_;
	bool enableRam_;

	void setRambank() const;
	void setRombank() const;
};

class WisdomTree : public Mbc {
public:
	explicit WisdomTree(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const /*data*/, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;

	void setRombank() const;
};

// armGBA: unlicensed cartridges that are MBC5 behind a lock (see unl.cpp)
class UnlMbc5 : public DefaultMbc {
public:
	enum Kind { bbd, hitek, ggb81, licheng, sintax, ntold1, ntold2, ntnew };
	// banks: the ROM's own; one more follows them in memory for a bank unpacked through the lock
	UnlMbc5(MemPtrs &memptrs, Kind kind, unsigned banks);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Kind kind_;
	unsigned banks_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;
	unsigned char dataMode_;
	unsigned char bankMode_;
	unsigned char sintaxMode_;
	unsigned char sintaxBank_;
	unsigned char xor_[4];
	// NT's multicarts: where the game picked starts and how many banks it has, and whether bank
	// numbers come with their bits out of order
	unsigned char base_;
	unsigned char count_;
	bool swapped_;
	// NT's later cartridges: the switchable area as two halves of 8 KiB each
	bool split_;
	unsigned short half_[2];

	void setRambank() const;
	void setRombank();
};

// armGBA: a cartridge chip that answers in the RAM area itself
class CartIo {
public:
	virtual ~CartIo() {}
	virtual unsigned read(unsigned p, unsigned long cc) const = 0;
	virtual void write(unsigned p, unsigned data, unsigned long cc) = 0;
	virtual void resetCc(unsigned long, unsigned long) {}
};

// armGBA: Bandai's TAMA5 (Game de Hakken!! Tamagotchi - Osutchi to Mesutchi)
class Tama5 : public Mbc, public CartIo {
public:
	explicit Tama5(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, unsigned long cc) const;
	virtual void write(unsigned p, unsigned data, unsigned long cc);
	virtual void resetCc(unsigned long oldCc, unsigned long newCc);

private:
	MemPtrs &memptrs_;

	unsigned char * chip() const;
	void tick(unsigned long cc) const;
	void setRombank() const;
};

// armGBA: a cartridge that answers some reads of its ROM itself
class RomHook {
public:
	virtual ~RomHook() {}
	virtual unsigned romRead(unsigned p) = 0;
};

// armGBA: Vast Fame's protected cartridge (see vf001.cpp)
class Vf001 : public DefaultMbc, public RomHook {
public:
	// banks: the ROM's own, with two spare ones after them; maybeGgb81: the logo is one a GGB81
	// cartridge's game carries too
	Vf001(MemPtrs &memptrs, unsigned banks, bool maybeGgb81);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned romRead(unsigned p);

private:
	MemPtrs &memptrs_;
	unsigned banks_;
	unsigned char start_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;
	bool config_;
	unsigned char running_;
	unsigned char reg6000_;
	unsigned char reg700x_[11];
	unsigned char left_;
	// How many times the running value has turned since the lock was opened, and at which turn
	// each register was written: for telling the two kinds of cartridge apart (not kept in states)
	unsigned char turns_;
	unsigned char turnOf_[12];
	bool decided_;
	// Mojie Chuanshuo has the logo of Vast Fame's Fellowship of the Rings on a GGB81 cartridge:
	// until the lock is first opened, a write to 2001 picks GGB81's order of data bits
	bool opened_;
	bool maybeGgb81_;
	unsigned char dataMode_;

	unsigned length() const;
	void otherKind();
	void setBanks() const;
};

// armGBA: MBC7 (Kirby Tilt 'n' Tumble, Command Master) and MBC6 (Net de Get); see mbc7.cpp
class Mbc7 : public Mbc, public CartIo {
public:
	explicit Mbc7(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, unsigned long cc) const;
	virtual void write(unsigned p, unsigned data, unsigned long cc);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool enable1_;
	bool enable2_;
	unsigned short x_;
	unsigned short y_;

	void eeprom(unsigned data);
	void setBanks() const;
};

class Mbc6 : public DefaultMbc {
public:
	Mbc6(MemPtrs &memptrs, unsigned banks);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned banks_;
	bool enableRam_;
	unsigned char rambank_;
	unsigned char half_[2];
	bool flash_[2];

	void setBanks() const;
};

// armGBA: the cartridge of the Telefang pirates sold as Pokemon Jade and Diamond: MBC3's banking,
// with two counters where the clock would be that the games use as a lock
class Pkjd : public Mbc, public CartIo {
public:
	explicit Pkjd(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, unsigned long cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, unsigned long cc) const;
	virtual void write(unsigned p, unsigned data, unsigned long cc);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	unsigned char select_;
	unsigned char counters_[2];
	bool enableRam_;

	void setBanks() const;
};

class Sachen : public Mbc {
public:
	explicit Sachen(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, unsigned long const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	// Where the cartridge's scrambled address lines take a read of the header page
	static unsigned unscramble(unsigned p);

private:
	MemPtrs &memptrs_;
	unsigned char bank_;
	unsigned char base_;
	unsigned char mask_;

	void setBanks() const;
};

}

#endif
