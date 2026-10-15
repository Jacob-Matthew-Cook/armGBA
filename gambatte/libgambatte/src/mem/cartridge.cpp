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

#include "cartridge.h"
#include "file/file.h"
#include "../savestate.h"
#include "pakinfo_internal.h"
#include "file/crc32.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

using namespace gambatte;

namespace {

std::string stripExtension(std::string const &str) {
	std::string::size_type const lastDot = str.find_last_of('.');
	std::string::size_type const lastSlash = str.find_last_of('/');

	if (lastDot != std::string::npos && (lastSlash == std::string::npos || lastSlash < lastDot))
		return str.substr(0, lastDot);

	return str;
}

std::string stripDir(std::string const &str) {
	std::string::size_type const lastSlash = str.find_last_of('/');
	if (lastSlash != std::string::npos)
		return str.substr(lastSlash + 1);

	return str;
}

void enforce8bit(unsigned char *data, std::size_t size) {
	if (static_cast<unsigned char>(0x100))
		while (size--)
			*data++ &= 0xFF;
}

unsigned pow2ceil(unsigned n) {
	--n;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	++n;

	return n;
}

bool presumedMbc1m(unsigned char const romdata[], unsigned rombanks) {
	// armGBA: the second game's logo is enough; its header checksum needn't hold (mooneye-gb's multicart test)
	return rombanks == 64 && isHeaderChecksumOk(romdata)
	&& !std::memcmp(&romdata[0x104], &romdata[16 * rombank_size() + 0x104], 0x134 - 0x104);
}

// armGBA: Sachen's multicarts keep the logo where their scrambled address lines put it: 1 for the
// first mapper, 2 for the second, whose Nintendo logo is a half page up
unsigned presumedSachen(unsigned char const romdata[], unsigned size) {
	if (size < 0x200)
		return 0;
	if (romdata[0x104] == 0xCE && romdata[0x144] == 0xED && romdata[0x114] == 0x66)
		return 1;
	return romdata[0x184] == 0xCE && romdata[0x1C4] == 0xED && romdata[0x194] == 0x66 ? 2 : 0;
}

// armGBA: Vast Fame's protected cartridge by its second logo, as hhugboy tells it by the sum of the
// logo's bytes: 0, 1 where the logo is one a GGB81 cartridge's game carries too, or -1 for none
int presumedVf001(unsigned char const romdata[], unsigned size) {
	if (size < 0x8000)
		return -1;
	unsigned sum = 0;
	for (unsigned i = 0x184; i < 0x1B4; ++i)
		sum += romdata[i];
	// Vast Fame's logo, Soul Falchion's, an italic DIGI, and two that Vast Fame's games carry
	// elsewhere (Fellowship of the Rings, Sanguozhi - Aoshi Tianxia)
	return sum == 2981 ? 1 : sum == 4844 || sum == 6127 || sum == 4406 || sum == 3209 ? 0 : -1;
}

// armGBA: Sintax's games end each bank in its number, with the bits in the lock's order. Where banks 2,
// 4 and 8 end in just those the dump runs without the lock (Emo Cheng DX, Luke Yingxiong Chuan 3).
static bool sintaxLocked(unsigned char const romdata[], unsigned size) {
	if (romdata[0x7FFF] <= 0x01)
		return false;
	return size < 9 * 0x4000u || romdata[3 * 0x4000 - 1] != 2 || romdata[5 * 0x4000 - 1] != 4 || romdata[9 * 0x4000 - 1] != 8;
}

// armGBA: the unlicensed MBC5 look-alikes by the second logo their games carry, as mGBA tells them;
// -1 for none. Dumps patched to run on plain MBC5 end their first bank in 01.
int presumedUnl(unsigned char const romdata[], unsigned size) {
	if (size < 0x8000)
		return -1;
	// NT's cartridges have no mark of their own; these are their games' headers, from mGBA's list
	switch (crc32(0, romdata + 0x100, 0x50)) {
	case 0xE1147E75: // Rockman 8
	case 0xEFF88FAA: // True Color 25 in 1
		return UnlMbc5::ntold1;
	case 0x811925D9: // 23 in 1
	case 0x62A8016A: // 29 in 1
	case 0x5758D6D9: // Caise Gedou 24 in 1
	case 0x80265A64: // Rockman X4
	case 0x805459DE: // Sonic Adventure 8
	case 0x0B1B808A: // Super Donkey Kong 5
	case 0x4650EB9A: // Super Mario Special 3
		return UnlMbc5::ntold2;
	case 0xB289D95A: // Capcom vs SNK - Millennium Fight 2001
	case 0x688D6713: // Digimon 02 4
	case 0x8931A272: // Digimon 2
	case 0x79083C6B: // Digimon Pocket
	case 0x0C5047EE: // Harry Potter 3
	case 0x8AC634B7: // Pokemon Diamond (Special Pikachu Edition)
	case 0x8628A287: // Pokemon Jade (Special Pikachu Edition)
	case 0xBC75D7B8: // Pokemon - Mewtwo Strikes Back
	case 0xFF0B60CC: // Shuma Baolong 02 4
	case 0x14A992A6: // Street Fighter Zero 4
		return UnlMbc5::ntnew;
	case 0x3EF5AFB2: // Pokemon Jade Version, a Telefang pirate
		return UnlMbc5::licheng;
	}
	switch (crc32(0, romdata + 0x184, 0x30)) {
	case 0x4fdab691: return UnlMbc5::hitek;
	case 0xc7d8c1df:
	case 0x6d1ea662: return romdata[0x7FFF] != 0x01 ? UnlMbc5::bbd : -1;
	case 0x79f34594:
	case 0x7e8c539b: return UnlMbc5::ggb81;
	case 0x20d092e2:
	case 0xd2b57657: return romdata[0x147] == 0x01 || (0x8000u << romdata[0x148]) != size ? UnlMbc5::licheng : -1;
	case 0x6c1dcf2d:
	case 0x99e3449d: return sintaxLocked(romdata, size) ? UnlMbc5::sintax : -1;
	}
	// Other logos of the same makers, by the sum of their bytes as hhugboy tells them. Their games
	// end each bank in its number; a dump where that holds is one patched to run without the lock.
	unsigned sum = 0;
	for (unsigned i = 0x184; i < 0x1B4; ++i)
		sum += romdata[i];
	bool const locked = romdata[0x7FFF] != 0x01 || romdata[0xBFFF] != 0x02;
	switch (sum) {
	case 4639: // BBD
	case 3334: // "S-GBC"
	case 5092: // Fiver Firm
		return locked ? UnlMbc5::bbd : -1;
	case 4125:
	case 4138:
		return sintaxLocked(romdata, size) ? UnlMbc5::sintax : -1;
	}
	// Makon's games, by the licensee code they made up, are on NT's cartridges: the first few on
	// the older kind, with a header that says 256 KiB, the rest on the newer
	if ((romdata[0x144] == 'M' && romdata[0x145] == 'K') || (romdata[0x144] == 'G' && romdata[0x145] == 'C')) {
		static char const *const early[] = { "SONIC 7", "SUPER MARIO 3", "DONKEY\x09KONG 5", "ROCKMAN 99" };
		for (unsigned i = 0; i < sizeof early / sizeof early[0]; ++i) {
			if (!std::strcmp(reinterpret_cast<char const *>(romdata) + 0x134, early[i]))
				return romdata[0x148] == 3 ? UnlMbc5::ntold2 : -1;
		}
		// Two dumps of such games are patched to run on the cartridge their headers name
		// (Pokemon Adventure, Shuma Baolong 2); nothing but their headers tells them apart
		unsigned long const header = crc32(0, romdata + 0x100, 0x50);
		return header == 0x72EDBF3E || header == 0x212ECA2F ? -1 : UnlMbc5::ntnew;
	}
	return -1;
}

bool presumedM161(unsigned char const romdata[], unsigned size) {
	if (size != rombank_size() * 16u || romdata[0x147] != 0x10)
		return false;

	static char const tetrisSetStr[10] { 0x54, 0x45, 0x54, 0x52, 0x49, 0x53, 0x20, 0x53, 0x45, 0x54, };
	return !std::memcmp(&romdata[0x134], tetrisSetStr, sizeof tetrisSetStr);
}

bool presumedMmm01(unsigned char const romdata[], unsigned size) {
	if (!isHeaderChecksumOk(romdata) || (size / rombank_size()) != std::max(pow2ceil(size / rombank_size()), 2u))
		return false;

	if (isHeaderChecksumOk(&romdata[size + -2 * rombank_size()])
		&& !std::memcmp(&romdata[0x104], &romdata[size + -2 * rombank_size() + 0x104], 0x134 - 0x104)) {
		unsigned char maybeMmm01 = romdata[size + -2 * rombank_size() + 0x147];
		if (maybeMmm01 >= 0x0B && maybeMmm01 <= 0x0D)
			return true;
		else if (romdata[0x147] == 0x01 && maybeMmm01 == 0x11) // hack to account for some bootleg mmm01 games
			return true;
	}

	return false;
}

bool presumedWisdomTree(unsigned char const romdata[], unsigned size) {
	if (!isHeaderChecksumOk(romdata) || (size / rombank_size()) != std::max(pow2ceil(size / rombank_size()), 2u))
		return false;

	if (romdata[0x147] == 0x00 && romdata[0x148] == 0x00 && (size / rombank_size()) > 2 && isHeaderChecksumOk(&romdata[2 * rombank_size()])
		&& !std::memcmp(&romdata[0x104], &romdata[2 * rombank_size() + 0x104], 0x134 - 0x104)) {
		static char const wisdomTreeStr20[11] { 0x57, 0x49, 0x53, 0x44, 0x4F, 0x4D, 0x20, 0x54, 0x52, 0x45, 0x45 };
		static char const wisdomTreeStr00[11] { 0x57, 0x49, 0x53, 0x44, 0x4F, 0x4D, 0x00, 0x54, 0x52, 0x45, 0x45 };
		for (unsigned i = 0; i < size; i++) {
			if (reinterpret_cast<char const *>(romdata)[i] == wisdomTreeStr20[0] && (size - i) >= sizeof wisdomTreeStr20) {
				if (!std::memcmp(&romdata[i], wisdomTreeStr20, sizeof wisdomTreeStr20)
					|| !std::memcmp(&romdata[i], wisdomTreeStr00, sizeof wisdomTreeStr00))
					return true;
			}
		}
	}

	return false;
}

bool hasBattery(unsigned char headerByte0x147) {
	switch (headerByte0x147) {
	case 0x03:
	case 0x06:
	case 0x09:
	case 0x0D:
	case 0x0F:
	case 0x10:
	case 0x13:
	case 0x1B:
	case 0x1E:
	case 0x20:
	case 0x22:
	case 0xFC:
	case 0xFD:
	case 0xFE:
	case 0xFF:
		return true;
	}

	return false;
}

bool hasRtc(unsigned headerByte0x147) {
	switch (headerByte0x147) {
	case 0x0F:
	case 0x10:
	case 0xFE:
		return true;
	}

	return false;
}

int asHex(char c) {
	return c >= 'A' ? c - 'A' + 0xA : c - '0';
}

}

Cartridge::Cartridge()
: mbc2_(false)
, huc1_(false)
, pocketCamera_(false)
, rtc_(time_)
, ir_()
, huc3_(time_, ir_)
{
	std::memset(romHeader, 0, sizeof romHeader);
}

void Cartridge::setStatePtrs(SaveState &state) {
	state.mem.vram.set(memptrs_.vramdata(), memptrs_.vramdataend() - memptrs_.vramdata());
	state.mem.sram.set(memptrs_.rambankdata(), memptrs_.rambankdataend() - memptrs_.rambankdata());
	state.mem.wram.set(memptrs_.wramdata(0), memptrs_.wramdataend() - memptrs_.wramdata(0));

	huc3_.setStatePtrs(state);
	camera_.setStatePtrs(state);
}

void Cartridge::saveState(SaveState &state, unsigned long const cc) {
	mbc_->saveState(state.mem);
	time_.saveState(state, cc);
	rtc_.saveState(state);
	ir_.saveState(state);
	huc3_.saveState(state);
	camera_.saveState(state);
}

void Cartridge::loadState(SaveState const &state) {
	bool const ds = (isCgb() && state.ppu.notCgbDmg) & state.mem.ioamhram.get()[0x14D] >> 7;
	camera_.loadState(state, ds);
	huc3_.loadState(state, ds);
	ir_.loadState(state, ds);
	rtc_.loadState(state);
	time_.loadState(state, ds);
	mbc_->loadState(state.mem);
}

std::string const Cartridge::saveBasePath() const {
	return saveDir_.empty()
	     ? defaultSaveBasePath_
	     : saveDir_ + stripDir(defaultSaveBasePath_);
}

void Cartridge::setSaveDir(std::string const &dir) {
	saveDir_ = dir;
	if (!saveDir_.empty() && saveDir_[saveDir_.length() - 1] != '/')
		saveDir_ += '/';
}

LoadRes Cartridge::loadROM(Array<unsigned char> &buffer,
                           bool const cgbMode,
                           std::string const &filepath)
{
	enum Cartridgetype { type_plain,
	                     type_mbc1,
	                     type_mbc2,
	                     type_mbc3,
	                     type_mbc5,
	                     type_m161,
	                     type_mmm01,
	                     type_huc1,
	                     type_huc3,
	                     type_pocketcamera,
	                     type_wisdomtree,
	                     type_sachen,
	                     type_tama5,
	                     type_pkjd,
	                     type_unl,
	                     type_mbc6,
	                     type_mbc7,
	                     type_vf001 };
	UnlMbc5::Kind unl = UnlMbc5::licheng;
	Cartridgetype type = type_plain;
	unsigned rambanks = 1;
	unsigned rombanks = 2;
	bool cgb = false;
	bool badMmm01 = false;

	{
		if (buffer.size() >= sizeof romHeader)
			std::memcpy(romHeader, buffer.get(), sizeof romHeader);
		else
			return LOADRES_IO_ERROR;

		if (romHeader[0x147] == 0x1B && romHeader[0x14A] == 0xE1)
			return LOADRES_UNSUPPORTED_MBC_EMS_MULTICART;
		else if (romHeader[0x147] == 0xC0 && romHeader[0x14A] == 0xD1)
			type = type_wisdomtree;
		else if (presumedWisdomTree(buffer.get(), buffer.size()))
			type = type_wisdomtree;
		else if (presumedVf001(buffer.get(), buffer.size()) >= 0) {
			type = type_vf001;
		} else if (presumedUnl(buffer.get(), buffer.size()) >= 0) {
			type = type_unl;
			unl = static_cast<UnlMbc5::Kind>(presumedUnl(buffer.get(), buffer.size()));
		} else if (presumedSachen(buffer.get(), buffer.size())) {
			type = type_sachen;
			// The header as the console reads it
			for (unsigned p = 0x100; p < sizeof romHeader; ++p)
				romHeader[p] = buffer.get()[Sachen::unscramble(p)];
			romHeader[0x147] = 0;
		}
		else if (presumedM161(buffer.get(), buffer.size())) {
			type = type_m161;
			romHeader[0x147] = 0; // hack to ensure no savedata (note: actual rom won't be touched)
			romHeader[0x14D] += 0x10; // also "fix" the header checksum (so PakInfo won't report it as bad)
		} else if (presumedMmm01(buffer.get(), buffer.size())) {
			type = type_mmm01;
			std::memcpy(romHeader, buffer.get() + buffer.size() + -2 * rombank_size(), sizeof romHeader);
		} else switch (romHeader[0x147]) {
		case 0x00: type = type_plain; break;
		case 0x01:
		case 0x02:
		case 0x03: type = type_mbc1; break;
		case 0x05:
		case 0x06: type = type_mbc2; break;
		case 0x08:
		case 0x09: type = type_plain; break;
		case 0x0B:
		case 0x0C:
		case 0x0D: type = type_mmm01; badMmm01 = true; break;
		case 0x0F:
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13: type = type_mbc3; break;
		case 0x19:
		case 0x1A:
		case 0x1B:
		case 0x1C:
		case 0x1D:
		case 0x1E: type = type_mbc5; break;
		case 0x20: type = type_mbc6; break;
		case 0x22: type = type_mbc7; break;
		case 0xBE: return LOADRES_UNSUPPORTED_MBC_BUNG_MULTICART;
		case 0xFC: type = type_pocketcamera; break;
		case 0xFD: type = type_tama5; break;
		case 0xFE: type = type_huc3; break;
		case 0xFF: type = type_huc1; break;
		// armGBA: prototypes and unlicensed games with nothing sensible here are guessed at below
		default:   type = type_plain; break;
		}

		/*switch (romHeader[0x0148]) {
		case 0x00: rombanks = 2; break;
		case 0x01: rombanks = 4; break;
		case 0x02: rombanks = 8; break;
		case 0x03: rombanks = 16; break;
		case 0x04: rombanks = 32; break;
		case 0x05: rombanks = 64; break;
		case 0x06: rombanks = 128; break;
		case 0x07: rombanks = 256; break;
		case 0x08: rombanks = 512; break;
		case 0x52: rombanks = 72; break;
		case 0x53: rombanks = 80; break;
		case 0x54: rombanks = 96; break;
		default: return -1;
		}*/

		rambanks = type == type_tama5 || type == type_mbc7 ? 1 : numRambanksFromH14x(romHeader[0x147], romHeader[0x149]);
		cgb = cgbMode;
	}

	rombanks = std::max(pow2ceil(buffer.size() / rombank_size()), 2u);

	// armGBA: a header that claims no mapper over more than 32 KiB, or HuC1 with a ROM size that is
	// none (unlicensed multicarts), is taken for the commonest mapper
	// armGBA: not the Doctor GB flash cartridge's own menu, whose hardware is its own: with no mapper
	// it finds no game on the card and says so, as on other emulators
	bool const doctorGb = !std::memcmp(romHeader + 0x134, "GB16M", 6);
	if ((type == type_plain && rombanks > 2 && romHeader[0x147] != 0x08 && romHeader[0x147] != 0x09 && !doctorGb)
			|| (type == type_huc1 && romHeader[0x148] > 0x08)) {
		type = rombanks > 2 ? type_mbc1 : type_plain;
	}
	// armGBA: the Telefang pirates sold as Pokemon Jade and Diamond: Telefang's title on an MBC3
	// header that gives a tenth of the ROM's size
	if (type == type_mbc3 && !std::memcmp(romHeader + 0x134, "TELEFANG", 8) && romHeader[0x148] < 5)
		type = type_pkjd;
	// armGBA: prototypes of Zelda DX were built for MBC5, as the game shipped, under a header that still
	// says MBC1: they write whole bank numbers past 31 to one register
	if (type == type_mbc1 && rombanks == 64 && (romHeader[0x143] & 0x80) && !std::memcmp(romHeader + 0x134, "ZELDA", 6))
		type = type_mbc5;
	// armGBA: MBC1 can't address more than 512 KiB of ROM along with more than 8 KiB of RAM; games that
	// claim both (the Space World 1997 Pokemon demos, pirate copies) are on something like MBC3
	// Likewise a pirate copy's 2 MiB under a header that still gives a small ROM's size
	if (type == type_mbc1 && rombanks > 32 && (rambanks > 1 || (romHeader[0x148] < 8 && (2u << romHeader[0x148]) < rombanks)))
		type = type_mbc3;

	if (badMmm01) {
		if (rombanks != (buffer.size() / rombank_size())) // probably just bad
			return LOADRES_BAD_FILE_OR_UNKNOWN_MBC;
		else {
			Array<unsigned char> const temp(rombank_size() * 2);
			std::memcpy(temp.get(), buffer.get(), temp.size());
			std::memmove(buffer.get(), buffer.get() + temp.size(), buffer.size() - temp.size());
			std::memcpy(buffer.get() + buffer.size() - temp.size(), temp.get(), temp.size());
		}
	}

	defaultSaveBasePath_.clear();
	ggUndoList_.clear();
	mbc_.reset();
	memptrs_.reset(type == type_sachen ? rombanks * 2 : type == type_vf001 ? rombanks + 2 : type == type_unl || type == type_mbc6 ? rombanks + 1 : rombanks, rambanks, cgb ? 8 : 2);
	time_.set(0);
	rtc_.set(false, 0);
	huc3_.set(false);
	camera_.set(0);
	mbc2_ = false;
	huc1_ = false;
	io_ = 0;
	romHook_ = 0;
	pocketCamera_ = false;

	std::memcpy(memptrs_.romdata(), buffer.get(), (buffer.size() / rombank_size() * rombank_size()));
	std::memset(memptrs_.romdata() + buffer.size() / rombank_size() * rombank_size(),
	            0xFF,
	            (rombanks - buffer.size() / rombank_size()) * rombank_size());
	enforce8bit(memptrs_.romdata(), rombanks * rombank_size());

	defaultSaveBasePath_ = stripExtension(filepath);

	sachen_ = type == type_sachen;
	if (type == type_sachen) {
		unsigned char *const rom = memptrs_.romdata();
		// The logo shown while the lock is closed: the first mapper's is a half page up
		unsigned const logo = presumedSachen(rom, rombanks * rombank_size()) == 1 ? 0x184 : 0x104;
		for (unsigned i = 0; i < sizeof sachenLogo_; ++i)
			sachenLogo_[i] = rom[Sachen::unscramble(logo + i)];
		std::memcpy(rom + rombanks * rombank_size(), rom, rombanks * rombank_size());
		for (unsigned bank = 0; bank < rombanks; ++bank) {
			for (unsigned p = 0x100; p < 0x200; ++p)
				rom[bank * rombank_size() + p] = rom[(rombanks + bank) * rombank_size() + Sachen::unscramble(p)];
		}
	}

	switch (type) {
	case type_plain: mbc_.reset(new Mbc0(memptrs_)); break;
	case type_mbc1:
		if (presumedMbc1m(memptrs_.romdata(), rombanks)) {
			mbc_.reset(new Mbc1Multi64(memptrs_));
		} else
			mbc_.reset(new Mbc1(memptrs_));

		break;
	case type_mbc2: mbc_.reset(new Mbc2(memptrs_)); mbc2_ = true; break;
	case type_mbc3:
		{
			bool mbc30 = rombanks > 0x80 || rambanks > 0x04;
			Rtc *rtc = hasRtc(romHeader[0x147]) ? &rtc_ : 0;
			if(mbc30)
				mbc_.reset(new Mbc30(memptrs_, rtc));
			else
				mbc_.reset(new Mbc3 (memptrs_, rtc));

			time_.set(rtc);
		}
		break;
	case type_mbc5: mbc_.reset(new Mbc5(memptrs_)); break;
	case type_m161: mbc_.reset(new M161(memptrs_)); break;
	case type_mmm01: mbc_.reset(new Mmm01(memptrs_)); break;
	case type_huc1: mbc_.reset(new HuC1(memptrs_, &ir_)); huc1_ = true; break;
	case type_huc3:
		huc3_.set(true);
		mbc_.reset(new HuC3(memptrs_, &huc3_));
		time_.set(&huc3_);
		break;
	case type_pocketcamera: mbc_.reset(new PocketCamera(memptrs_, &camera_)); pocketCamera_ = true; break;
	case type_wisdomtree: mbc_.reset(new WisdomTree(memptrs_)); break;
	case type_sachen: mbc_.reset(new Sachen(memptrs_)); break;
	case type_tama5: { Tama5 *const chip = new Tama5(memptrs_); io_ = chip; mbc_.reset(chip); } break;
	case type_vf001: { Vf001 *const chip = new Vf001(memptrs_, rombanks, presumedVf001(memptrs_.romdata(), rombanks * rombank_size()) == 1); romHook_ = chip; mbc_.reset(chip); } break;
	case type_mbc6: mbc_.reset(new Mbc6(memptrs_, rombanks)); break;
	case type_mbc7: { Mbc7 *const chip = new Mbc7(memptrs_); io_ = chip; mbc_.reset(chip); } break;
	case type_unl: mbc_.reset(new UnlMbc5(memptrs_, unl, rombanks)); break;
	case type_pkjd: { Pkjd *const chip = new Pkjd(memptrs_); io_ = chip; mbc_.reset(chip); } break;
	}

	return LOADRES_OK;
}

bool Cartridge::hasMbc3Rtc() const {
	return hasRtc(romHeader[0x147]) && !isHuC3();
}

unsigned Cartridge::getSavedataLength() {
	unsigned ret = 0;
	if (hasBattery(romHeader[0x147])) {
		ret = memptrs_.rambankdataend() - memptrs_.rambankdata();
	}
	if (hasRtc(romHeader[0x147])) {
		ret += 8 + (isHuC3() ? 0x104 : 14);
	}
	return ret;
}

void Cartridge::loadSavedata(unsigned long const cc) {
	std::string const &sbp = saveBasePath();

	if (hasBattery(romHeader[0x147])) {
		std::ifstream file((sbp + ".sav").c_str(), std::ios::binary | std::ios::in);

		if (file.is_open()) {
			file.read(reinterpret_cast<char*>(memptrs_.rambankdata()),
			          memptrs_.rambankdataend() - memptrs_.rambankdata());
			enforce8bit(memptrs_.rambankdata(), memptrs_.rambankdataend() - memptrs_.rambankdata());
		}
	}

	if (hasRtc(romHeader[0x147])) {
		std::ifstream file((sbp + ".rtc").c_str(), std::ios::binary | std::ios::in);
		if (file) {
			unsigned long long baseTime;
			baseTime = file.get() & 0xFF;
			baseTime = baseTime << 8 | (file.get() & 0xFF);
			baseTime = baseTime << 8 | (file.get() & 0xFF);
			baseTime = baseTime << 8 | (file.get() & 0xFF);
			baseTime = baseTime << 8 | (file.get() & 0xFF);

			if (!file.eof()) {
				baseTime = baseTime << 8 | (file.get() & 0xFF);
				baseTime = baseTime << 8 | (file.get() & 0xFF);
				baseTime = baseTime << 8 | (file.get() & 0xFF);
			} else
				baseTime >>= 8;

			unsigned long long const now = std::time(0);
			if (baseTime > now) // prevent malformed RTC files from giving negative times
				baseTime = now;

			if (isHuC3()) {
				unsigned char huc3Regs[0x100 + 4] { 0 };
				for (unsigned i = 0; i < 0x104; i++) {
					huc3Regs[i] = file.get() & 0xFF;
					if (file.eof()) {
						huc3Regs[i] = 0;
						break;
					}
				}
				huc3_.setHuC3Regs(huc3Regs);
			} else {
				unsigned long rtcRegs[11] { 0 };
				rtcRegs[rtc_dh] = file.get() & 0xC1;
				if (!file.eof()) {
					rtcRegs[rtc_dl] = file.get() & 0xFF;
					rtcRegs[rtc_h] = file.get() & 0x1F;
					rtcRegs[rtc_m] = file.get() & 0x3F;
					rtcRegs[rtc_s] = file.get() & 0x3F;
					rtcRegs[rtc_c] = file.get() & 0xFF;
					rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (file.get() & 0xFF);
					rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (file.get() & 0xFF);
					rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (file.get() & 0xFF);
					rtcRegs[rtc_dh + rtc_l] = file.get() & 0xC1;
					rtcRegs[rtc_dl + rtc_l] = file.get() & 0xFF;
					rtcRegs[rtc_h + rtc_l] = file.get() & 0x1F;
					rtcRegs[rtc_m + rtc_l] = file.get() & 0x3F;
					rtcRegs[rtc_s + rtc_l] = file.get() & 0x3F;
				} else
					rtcRegs[rtc_dh] = 0;

				rtc_.setRtcRegs(rtcRegs);
			}

			time_.setBaseTime(baseTime, cc);
		}
	}
}

void Cartridge::saveSavedata(unsigned long const cc) {
	std::string const &sbp = saveBasePath();

	if (hasBattery(romHeader[0x147])) {
		std::ofstream file((sbp + ".sav").c_str(), std::ios::binary | std::ios::out);
		file.write(reinterpret_cast<char const *>(memptrs_.rambankdata()),
		           memptrs_.rambankdataend() - memptrs_.rambankdata());
	}

	if (hasRtc(romHeader[0x147])) {
		std::ofstream file((sbp + ".rtc").c_str(), std::ios::binary | std::ios::out);
		unsigned long long baseTime = std::time(0);
		file.put(baseTime >> 56 & 0xFF);
		file.put(baseTime >> 48 & 0xFF);
		file.put(baseTime >> 40 & 0xFF);
		file.put(baseTime >> 32 & 0xFF);
		file.put(baseTime >> 24 & 0xFF);
		file.put(baseTime >> 16 & 0xFF);
		file.put(baseTime >>  8 & 0xFF);
		file.put(baseTime       & 0xFF);
		if (isHuC3()) {
			unsigned char huc3Regs[0x100 + 4];
			huc3_.getHuC3Regs(huc3Regs, cc);
			for (unsigned i = 0; i < 0x104; i++)
				file.put(huc3Regs[i] & 0xFF);
		} else {
			unsigned long rtcRegs[11];
			rtc_.getRtcRegs(rtcRegs, cc);
			file.put(rtcRegs[rtc_dh]         & 0xC1);
			file.put(rtcRegs[rtc_dl]         & 0xFF);
			file.put(rtcRegs[rtc_h]          & 0x1F);
			file.put(rtcRegs[rtc_m]          & 0x3F);
			file.put(rtcRegs[rtc_s]          & 0x3F);
			file.put(rtcRegs[rtc_c]    >> 24 & 0xFF);
			file.put(rtcRegs[rtc_c]    >> 16 & 0xFF);
			file.put(rtcRegs[rtc_c]    >>  8 & 0xFF);
			file.put(rtcRegs[rtc_c]          & 0xFF);
			file.put(rtcRegs[rtc_dh + rtc_l] & 0xC1);
			file.put(rtcRegs[rtc_dl + rtc_l] & 0xFF);
			file.put(rtcRegs[rtc_h + rtc_l]  & 0x1F);
			file.put(rtcRegs[rtc_m + rtc_l]  & 0x3F);
			file.put(rtcRegs[rtc_s + rtc_l]  & 0x3F);
		}
	}
}

void Cartridge::saveSavedata(char* dest, unsigned long const cc) {
	if (hasBattery(romHeader[0x147])) {
		int length = memptrs_.rambankdataend() - memptrs_.rambankdata();
		std::memcpy(dest, memptrs_.rambankdata(), length);
		dest += length;
	}

	if (hasRtc(romHeader[0x147])) {
		unsigned long long baseTime = std::time(0);
		*dest++ = baseTime >> 56 & 0xFF;
		*dest++ = baseTime >> 48 & 0xFF;
		*dest++ = baseTime >> 40 & 0xFF;
		*dest++ = baseTime >> 32 & 0xFF;
		*dest++ = baseTime >> 24 & 0xFF;
		*dest++ = baseTime >> 16 & 0xFF;
		*dest++ = baseTime >>  8 & 0xFF;
		*dest++ = baseTime       & 0xFF;
		if (isHuC3()) {
			unsigned char huc3Regs[0x100 + 4];
			huc3_.getHuC3Regs(huc3Regs, cc);
			std::memcpy(dest, huc3Regs, sizeof huc3Regs);
		} else {
			unsigned long rtcRegs[11];
			rtc_.getRtcRegs(rtcRegs, cc);
			*dest++ = rtcRegs[rtc_dh]         & 0xC1;
			*dest++ = rtcRegs[rtc_dl]         & 0xFF;
			*dest++ = rtcRegs[rtc_h]          & 0x1F;
			*dest++ = rtcRegs[rtc_m]          & 0x3F;
			*dest++ = rtcRegs[rtc_s]          & 0x3F;
			*dest++ = rtcRegs[rtc_c]    >> 24 & 0xFF;
			*dest++ = rtcRegs[rtc_c]    >> 16 & 0xFF;
			*dest++ = rtcRegs[rtc_c]    >>  8 & 0xFF;
			*dest++ = rtcRegs[rtc_c]          & 0xFF;
			*dest++ = rtcRegs[rtc_dh + rtc_l] & 0xC1;
			*dest++ = rtcRegs[rtc_dl + rtc_l] & 0xFF;
			*dest++ = rtcRegs[rtc_h + rtc_l]  & 0x1F;
			*dest++ = rtcRegs[rtc_m + rtc_l]  & 0x3F;
			*dest++ = rtcRegs[rtc_s + rtc_l]  & 0x3F;
		}
	}
}

void Cartridge::loadSavedata(char const *data, unsigned long const cc) {
	if (hasBattery(romHeader[0x147])) {
		int length = memptrs_.rambankdataend() - memptrs_.rambankdata();
		std::memcpy(memptrs_.rambankdata(), data, length);
		data += length;
		enforce8bit(memptrs_.rambankdata(), length);
	}

	if (hasRtc(romHeader[0x147])) {
		unsigned long long baseTime;
		baseTime = *data++ & 0xFF;
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);
		baseTime = baseTime << 8 | (*data++ & 0xFF);

		unsigned long long const now = std::time(0);
		if (baseTime > now) // prevent malformed save files from giving negative times
			baseTime = now;

		if (isHuC3()) {
			unsigned char huc3Regs[0x100 + 4];
			std::memcpy(huc3Regs, data, sizeof huc3Regs);
			huc3_.setHuC3Regs(huc3Regs);
		} else {
			unsigned long rtcRegs[11];
			rtcRegs[rtc_dh] = *data++ & 0xC1;
			rtcRegs[rtc_dl] = *data++ & 0xFF;
			rtcRegs[rtc_h] = *data++ & 0x1F;
			rtcRegs[rtc_m] = *data++ & 0x3F;
			rtcRegs[rtc_s] = *data++ & 0x3F;
			rtcRegs[rtc_c] = *data++ & 0xFF;
			rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (*data++ & 0xFF);
			rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (*data++ & 0xFF);
			rtcRegs[rtc_c] = rtcRegs[rtc_c] << 8 | (*data++ & 0xFF);
			rtcRegs[rtc_dh + rtc_l] = *data++ & 0xC1;
			rtcRegs[rtc_dl + rtc_l] = *data++ & 0xFF;
			rtcRegs[rtc_h + rtc_l] = *data++ & 0x1F;
			rtcRegs[rtc_m + rtc_l] = *data++ & 0x3F;
			rtcRegs[rtc_s + rtc_l] = *data++ & 0x3F;
			rtc_.setRtcRegs(rtcRegs);
		}

		time_.setBaseTime(baseTime, cc);
	}
}

bool Cartridge::getMemoryArea(int which, unsigned char **data, int *length) const {
	if (!data || !length)
		return false;

	switch (which) {
	case 0:
		*data = memptrs_.vramdata();
		*length = memptrs_.vramdataend() - memptrs_.vramdata();
		return true;
	case 1:
		*data = memptrs_.romdata();
		*length = memptrs_.romdataend() - memptrs_.romdata();
		return true;
	case 2:
		*data = memptrs_.wramdata(0);
		*length = memptrs_.wramdataend() - memptrs_.wramdata(0);
		return true;
	case 3:
		*data = memptrs_.rambankdata();
		*length = memptrs_.rambankdataend() - memptrs_.rambankdata();
		return true;
	default:
		return false;
	}
}

void Cartridge::applyGameGenie(std::string const &code) {
	if (6 < code.length()) {
		unsigned const val = (asHex(code[0]) << 4 | asHex(code[1])) & 0xFF;
		unsigned const addr = (    asHex(code[2])        <<  8
		                        |  asHex(code[4])        <<  4
		                        |  asHex(code[5])
		                        | (asHex(code[6]) ^ 0xF) << 12) & 0x7FFF;
		unsigned cmp = 0xFFFF;
		if (10 < code.length()) {
			cmp = (asHex(code[8]) << 4 | asHex(code[10])) ^ 0xFF;
			cmp = ((cmp >> 2 | cmp << 6) ^ 0x45) & 0xFF;
		}

		for (unsigned bank = 0; bank < rombanks(memptrs_); ++bank) {
			if (mbc_->isAddressWithinAreaRombankCanBeMappedTo(addr, bank)
					&& (cmp > 0xFF || memptrs_.romdata()[bank * rombank_size() + addr % rombank_size()] == cmp)) {
				ggUndoList_.push_back(AddrData(bank * rombank_size() + addr % rombank_size(),
				                      memptrs_.romdata()[bank * rombank_size() + addr % rombank_size()]));
				memptrs_.romdata()[bank * rombank_size() + addr % rombank_size()] = val;
			}
		}
	}
}

void Cartridge::setGameGenie(std::string const &codes) {
	if (loaded()) {
		for (std::vector<AddrData>::reverse_iterator it =
				ggUndoList_.rbegin(), end = ggUndoList_.rend(); it != end; ++it) {
			if (memptrs_.romdata() + it->addr < memptrs_.romdataend())
				memptrs_.romdata()[it->addr] = it->data;
		}

		ggUndoList_.clear();

		std::string code;
		for (std::size_t pos = 0; pos < codes.length(); pos += code.length() + 1) {
			code = codes.substr(pos, codes.find(';', pos) - pos);
			applyGameGenie(code);
		}
	}
}

PakInfo const Cartridge::pakInfo() const {
	if (loaded()) {
		unsigned crc = 0L;
		unsigned const rombs = rombanks(memptrs_);
		crc = crc32(crc, memptrs_.romdata(), rombs*0x4000ul);
		return PakInfo(presumedMbc1m(memptrs_.romdata(), rombs),
		               presumedM161(memptrs_.romdata(), rombs*0x4000ul),
		               presumedMmm01(memptrs_.romdata(), rombs*0x4000ul),
		               presumedWisdomTree(memptrs_.romdata(), rombs*0x4000ul),
		               rombs,
		               crc,
		               romHeader);
	}

	return PakInfo();
}

SYNCFUNC(Cartridge) {
	SSS(memptrs_);
	SSS(time_);
	SSS(rtc_);
	SSS(ir_);
	SSS(huc3_);
	SSS(camera_);
	TSS(mbc_);
}
