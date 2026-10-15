/* Copyright (c) 2013-2021 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>
#include <time.h>

// Cartridge GPIO pins at 0x080000C4 and the devices behind them, as mGBA has them: the real-time
// clock, Boktai's light sensor, WarioWare: Twisted!'s gyro and the rumble motor. The pin registers read
// back through the ROM, so their state is written into the ROM buffer. The tilt sensor of Yoshi
// Topsy-Turvy and Korokoro Puzzle sits in the save memory's addresses instead.

enum {
	GPIO_REG_DATA = 0xC4,
	GPIO_REG_DIRECTION = 0xC6,
	GPIO_REG_CONTROL = 0xC8,
};

enum {
	RTC_RESET = 0,
	RTC_DATETIME = 2,
	RTC_FORCE_IRQ = 3,
	RTC_CONTROL = 4,
	RTC_TIME = 6,
};

static const int RTC_BYTES[8] = { 0, 0, 7, 0, 1, 0, 3, 0 };

// A command byte's register number
static int rtcCommand(uint32_t command) {
	return (command >> 4) & 7;
}

// The port's registers are readable in the ROM's address space when a game switches that on
static void storePin(struct GBA* gba, uint32_t offset, uint16_t value) {
	if (gba->rom) {
		memcpy(&gba->rom[offset], &value, 2);
	}
}

// The pins as they read: the ones the game drives, and the device's on the others
static void outputPins(struct GBA* gba, unsigned pins) {
	struct Gpio* hw = &gba->gpio;
	hw->pinState &= hw->direction;
	hw->pinState |= pins & ~hw->direction & 0xF;
	if (hw->readWrite) {
		storePin(gba, GPIO_REG_DATA, hw->pinState);
	}
}

// The clock counts in binary-coded decimal
static unsigned bcd(unsigned value) {
	int counter = value % 10;
	value /= 10;
	counter += (value % 10) << 4;
	return counter;
}

// Latches the host's time into the clock's registers, less the offset a game has set
static void rtcUpdateClock(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	time_t t = hw->clock ? (time_t) hw->clock(hw->clockContext) : time(NULL);
	hw->rtc.lastLatch = t;
	t -= hw->rtc.offset;
	struct tm date;
	localtime_r(&t, &date);
	hw->rtc.time[0] = bcd(date.tm_year - 100);
	hw->rtc.time[1] = bcd(date.tm_mon + 1);
	hw->rtc.time[2] = bcd(date.tm_mday);
	hw->rtc.time[3] = bcd(date.tm_wday);
	hw->rtc.time[4] = bcd((hw->rtc.control & 0x40) ? date.tm_hour : date.tm_hour % 12);
	hw->rtc.time[5] = bcd(date.tm_min);
	hw->rtc.time[6] = bcd(date.tm_sec);
}

// The first byte after chip select: which register, and whether it is read or written
static void rtcBeginCommand(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	uint32_t command = hw->rtc.bits;
	if ((command & 0xF) == 0x06) {
		hw->rtc.command = command;
		hw->rtc.bytesRemaining = RTC_BYTES[rtcCommand(command)];
		hw->rtc.commandActive = true;
		switch (rtcCommand(command)) {
		case RTC_RESET:
			hw->rtc.control = 0;
			break;
		case RTC_DATETIME:
		case RTC_TIME:
			rtcUpdateClock(gba);
			break;
		}
	}
	hw->rtc.bits = 0;
	hw->rtc.bitsRead = 0;
}

// A byte of a write has arrived
static void rtcProcessByte(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	if (rtcCommand(hw->rtc.command) == RTC_CONTROL) {
		hw->rtc.control = hw->rtc.bits;
	}
	hw->rtc.bits = 0;
	hw->rtc.bitsRead = 0;
	--hw->rtc.bytesRemaining;
	if (hw->rtc.bytesRemaining <= 0) {
		hw->rtc.bytesRemaining = RTC_BYTES[rtcCommand(hw->rtc.command)];
	}
}

// The bit the clock puts on its data pin during a read
static unsigned rtcOutput(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	uint8_t outputByte = 0xFF;
	switch (rtcCommand(hw->rtc.command)) {
	case RTC_CONTROL:
		outputByte = hw->rtc.control;
		break;
	case RTC_DATETIME:
	case RTC_TIME:
		outputByte = hw->rtc.time[7 - hw->rtc.bytesRemaining];
		break;
	}
	return (outputByte >> hw->rtc.bitsRead) & 1;
}

// Pins: 0 SCK, 1 SIO, 2 CS
static void rtcReadPins(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	outputPins(gba, hw->pinState & 2);
	if (!(hw->pinState & 4)) {
		hw->rtc.bitsRead = 0;
		hw->rtc.bytesRemaining = 0;
		hw->rtc.commandActive = false;
		hw->rtc.command = 0;
		hw->rtc.sckEdge = true;
		hw->rtc.sioOutput = true;
		outputPins(gba, 2);
		return;
	}
	if (!hw->rtc.commandActive) {
		outputPins(gba, 2);
		if (!(hw->pinState & 1)) {
			hw->rtc.bits &= ~(1u << hw->rtc.bitsRead);
			hw->rtc.bits |= ((hw->pinState & 2) >> 1) << hw->rtc.bitsRead;
		}
		if (!hw->rtc.sckEdge && (hw->pinState & 1)) {
			++hw->rtc.bitsRead;
			if (hw->rtc.bitsRead == 8) {
				rtcBeginCommand(gba);
			}
		}
	} else if (!(hw->rtc.command & 0x80)) {
		outputPins(gba, 2);
		if (!(hw->pinState & 1)) {
			hw->rtc.bits &= ~(1u << hw->rtc.bitsRead);
			hw->rtc.bits |= ((hw->pinState & 2) >> 1) << hw->rtc.bitsRead;
		}
		if (!hw->rtc.sckEdge && (hw->pinState & 1)) {
			if (((hw->rtc.bits >> hw->rtc.bitsRead) & 1) ^ ((hw->pinState & 2) >> 1)) {
				hw->rtc.bits &= ~(1u << hw->rtc.bitsRead);
			}
			++hw->rtc.bitsRead;
			if (hw->rtc.bitsRead == 8) {
				rtcProcessByte(gba);
			}
		}
	} else {
		if (hw->rtc.sckEdge && !(hw->pinState & 1)) {
			hw->rtc.sioOutput = rtcOutput(gba);
			++hw->rtc.bitsRead;
			if (hw->rtc.bitsRead == 8) {
				--hw->rtc.bytesRemaining;
				if (hw->rtc.bytesRemaining <= 0) {
					hw->rtc.bytesRemaining = RTC_BYTES[rtcCommand(hw->rtc.command)];
				}
				hw->rtc.bitsRead = 0;
			}
		}
		outputPins(gba, hw->rtc.sioOutput << 1);
	}
	hw->rtc.sckEdge = !!(hw->pinState & 1);
}

// The gyro's reading, 16 bits shifted out on pin 2: a rising pin 0 samples it, each falling pin 1 moves
// it a bit
static void gyroReadPins(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	bool output = hw->gyroEdge && !(hw->pinState & 2);
	if (hw->pinState & 1) {
		// About 12 bits around 0x700, a bit cropped so that it can't go negative
		hw->gyroSample = (hw->gyroZ >> 21) + 0x700;
		output = true;
	}
	if (output) {
		unsigned bit = hw->gyroSample >> 15;
		hw->gyroSample <<= 1;
		outputPins(gba, bit << 2);
	}
	hw->gyroEdge = !!(hw->pinState & 2);
}

// Pin 3 runs the motor
static void rumbleReadPins(struct GBA* gba) {
	gba->gpio.rumble = !!(gba->gpio.pinState & 8);
}

// Boktai's sensor: pin 1 resets a counter that pin 0 clocks, and pin 3 reads high once the count
// reaches the light's reading, so brighter light answers sooner. Pin 2 selects another chip.
static void lightReadPins(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	if (hw->pinState & 4) {
		return;
	}
	if (hw->pinState & 2) {
		hw->lightCounter = 0;
		hw->lightEdge = true;
		hw->lightSample = hw->luminance;
	}
	if ((hw->pinState & 1) && hw->lightEdge) {
		++hw->lightCounter;
	}
	hw->lightEdge = !(hw->pinState & 1);

	bool sendBit = hw->lightCounter >= hw->lightSample;
	outputPins(gba, (sendBit << 3) | (hw->pinState & 0x7));
}

// Called after the game changes the pins: the devices attached look at them
static void readPins(struct GBA* gba) {
	int devices = gba->gpio.devices;
	if (devices & HW_RTC) {
		rtcReadPins(gba);
	}
	if (devices & HW_GYRO) {
		gyroReadPins(gba);
	}
	if (devices & HW_RUMBLE) {
		rumbleReadPins(gba);
	}
	if (devices & HW_LIGHT_SENSOR) {
		lightReadPins(gba);
	}
}

// The tilt sensor reads when the game writes 0x55 to 0x8000 and then 0xAA to 0x8100; its two
// 12-bit axes, about 0x3A0 when level, read from 0x8200 to 0x8500
void gpioTiltWrite(struct GBA* gba, uint32_t offset, uint8_t value) {
	struct Gpio* hw = &gba->gpio;
	switch (offset) {
	case 0x8000:
		if (value == 0x55) {
			hw->tiltState = 1;
		}
		break;
	case 0x8100:
		if (value == 0xAA && hw->tiltState == 1) {
			hw->tiltState = 0;
			hw->tiltX = 0x3A0 - (hw->tiltInputX >> 22);
			hw->tiltY = 0x3A0 - (hw->tiltInputY >> 22);
		}
		break;
	}
}

uint8_t gpioTiltRead(struct GBA* gba, uint32_t offset) {
	struct Gpio* hw = &gba->gpio;
	switch (offset) {
	case 0x8200:
		return hw->tiltX & 0xFF;
	case 0x8300:
		return ((hw->tiltX >> 8) & 0xF) | 0x80;
	case 0x8400:
		return hw->tiltY & 0xFF;
	case 0x8500:
		return (hw->tiltY >> 8) & 0xF;
	}
	return 0xFF;
}

// Which devices the cartridge has, from the override list
void gpioSetDevices(struct GBA* gba, int devices) {
	struct Gpio* hw = &gba->gpio;
	hw->devices = devices;
	hw->readWrite = 0;
	hw->writeLatch = 0;
	hw->pinState = 0;
	hw->direction = 0;
	if (devices & HW_RTC) {
		memset(&hw->rtc, 0, sizeof(hw->rtc));
		hw->rtc.sckEdge = true;
		hw->rtc.sioOutput = true;
		hw->rtc.control = 0x40;
	}
	hw->gyroSample = 0;
	hw->gyroEdge = false;
	hw->lightCounter = 0;
	hw->lightEdge = false;
	hw->lightSample = 0xFF;
	hw->tiltX = 0xFFF;
	hw->tiltY = 0xFFF;
	hw->tiltState = 0;
	hw->rumble = false;
}

// All pins inputs, the clock idle
void gpioReset(struct GBA* gba) {
	struct Gpio* hw = &gba->gpio;
	hw->readWrite = 0;
	hw->writeLatch = 0;
	hw->pinState = 0;
	hw->direction = 0;
}

// Pokémon ROM hacks get the VBA-compatible pin behavior, as in mGBA
void gpioWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	struct Gpio* hw = &gba->gpio;
	bool vbaCompat = hw->devices & 0x10000;
	switch (address) {
	case GPIO_REG_DATA:
		hw->writeLatch = value & 0xF;
		if (!vbaCompat) {
			hw->pinState &= ~hw->direction;
			hw->pinState |= hw->writeLatch & hw->direction;
		} else {
			hw->pinState = hw->writeLatch;
		}
		readPins(gba);
		break;
	case GPIO_REG_DIRECTION:
		hw->direction = value & 0xF;
		if (!vbaCompat) {
			hw->pinState &= ~hw->direction;
			hw->pinState |= hw->writeLatch & hw->direction;
			readPins(gba);
		}
		break;
	case GPIO_REG_CONTROL:
		hw->readWrite = value & 0x1;
		break;
	}
	if (hw->readWrite) {
		storePin(gba, GPIO_REG_DATA, hw->pinState);
		storePin(gba, GPIO_REG_DIRECTION, hw->direction);
		storePin(gba, GPIO_REG_CONTROL, hw->readWrite);
	} else {
		storePin(gba, GPIO_REG_DATA, 0);
		storePin(gba, GPIO_REG_DIRECTION, 0);
		storePin(gba, GPIO_REG_CONTROL, 0);
	}
}
