/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_GBA_H
#define ARMGBA_GBA_H

#include "render.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The machine: memory, CPU, bus, timers, DMA, video, audio and cartridge state, and the functions
// the parts use to call each other.

enum {
	SIZE_BIOS = 0x4000,
	SIZE_EWRAM = 0x40000,
	SIZE_IWRAM = 0x8000,
	SIZE_IO = 0x400,
	SIZE_PALETTE = 0x400,
	SIZE_VRAM = 0x18000,
	SIZE_OAM = 0x400,
	SIZE_ROM = 0x2000000,
	SIZE_SRAM = 0x8000,
	SIZE_SRAM512 = 0x10000,
	SIZE_FLASH512 = 0x10000,
	SIZE_FLASH1M = 0x20000,
	SIZE_EEPROM = 0x2000,
	SIZE_EEPROM512 = 0x200,
};

enum {
	REGION_BIOS = 0x0,
	REGION_EWRAM = 0x2,
	REGION_IWRAM = 0x3,
	REGION_IO = 0x4,
	REGION_PALETTE = 0x5,
	REGION_VRAM = 0x6,
	REGION_OAM = 0x7,
	REGION_ROM0 = 0x8,
	REGION_ROM2_EX = 0xD,
	REGION_SRAM = 0xE,
	REGION_SRAM_MIRROR = 0xF,
};

enum {
	MODE_USER = 0x10,
	MODE_FIQ = 0x11,
	MODE_IRQ = 0x12,
	MODE_SUPERVISOR = 0x13,
	MODE_ABORT = 0x17,
	MODE_UNDEFINED = 0x1B,
	MODE_SYSTEM = 0x1F,
};

enum {
	PSR_N = 1u << 31,
	PSR_Z = 1u << 30,
	PSR_C = 1u << 29,
	PSR_V = 1u << 28,
	PSR_I = 1u << 7,
	PSR_F = 1u << 6,
	PSR_T = 1u << 5,
};

enum {
	IRQ_VBLANK,
	IRQ_HBLANK,
	IRQ_VCOUNTER,
	IRQ_TIMER0,
	IRQ_TIMER1,
	IRQ_TIMER2,
	IRQ_TIMER3,
	IRQ_SIO,
	IRQ_DMA0,
	IRQ_DMA1,
	IRQ_DMA2,
	IRQ_DMA3,
	IRQ_KEYPAD,
	IRQ_GAMEPAK,
};

// I/O register offsets
enum {
	REG_DISPCNT = 0x000,
	REG_STEREOCNT = 0x002,
	REG_DISPSTAT = 0x004,
	REG_VCOUNT = 0x006,
	REG_BG0CNT = 0x008,
	REG_BG1CNT = 0x00A,
	REG_BG2CNT = 0x00C,
	REG_BG3CNT = 0x00E,
	REG_SOUND1CNT_LO = 0x060,
	REG_SOUND1CNT_HI = 0x062,
	REG_SOUND1CNT_X = 0x064,
	REG_SOUND2CNT_LO = 0x068,
	REG_SOUND2CNT_HI = 0x06C,
	REG_SOUND3CNT_LO = 0x070,
	REG_SOUND3CNT_HI = 0x072,
	REG_SOUND3CNT_X = 0x074,
	REG_SOUND4CNT_LO = 0x078,
	REG_SOUND4CNT_HI = 0x07C,
	REG_SOUNDCNT_LO = 0x080,
	REG_SOUNDCNT_HI = 0x082,
	REG_SOUNDCNT_X = 0x084,
	REG_SOUNDBIAS = 0x088,
	REG_WAVE_RAM0_LO = 0x090,
	REG_WAVE_RAM3_HI = 0x09E,
	REG_FIFO_A_LO = 0x0A0,
	REG_FIFO_A_HI = 0x0A2,
	REG_FIFO_B_LO = 0x0A4,
	REG_FIFO_B_HI = 0x0A6,
	REG_DMA0SAD_LO = 0x0B0,
	REG_DMA0CNT_LO = 0x0B8,
	REG_DMA0CNT_HI = 0x0BA,
	REG_DMA3CNT_HI = 0x0DE,
	REG_TM0CNT_LO = 0x100,
	REG_TM0CNT_HI = 0x102,
	REG_TM3CNT_HI = 0x10E,
	REG_SIODATA32_LO = 0x120,
	REG_SIOMULTI0 = 0x120,
	REG_SIOMULTI1 = 0x122,
	REG_SIOMULTI2 = 0x124,
	REG_SIOMULTI3 = 0x126,
	REG_SIOCNT = 0x128,
	REG_SIOMLT_SEND = 0x12A,
	REG_KEYINPUT = 0x130,
	REG_KEYCNT = 0x132,
	REG_RCNT = 0x134,
	REG_JOYCNT = 0x140,
	REG_JOY_RECV_LO = 0x150,
	REG_JOY_RECV_HI = 0x152,
	REG_JOY_TRANS_LO = 0x154,
	REG_JOY_TRANS_HI = 0x156,
	REG_JOYSTAT = 0x158,
	REG_IE = 0x200,
	REG_IF = 0x202,
	REG_WAITCNT = 0x204,
	REG_IME = 0x208,
	REG_MAX = 0x20A,
	REG_POSTFLG = 0x300,
	REG_EXWAITCNT_LO = 0x800,
	REG_EXWAITCNT_HI = 0x802,
	// EXWAITCNT sits outside the I/O block; mGBA keeps it in two slots past the end.
	REG_INTERNAL_EXWAITCNT_LO = 0x400,
	REG_INTERNAL_EXWAITCNT_HI = 0x402,
	REG_INTERNAL_MAX = 0x404,
};

#define IO(reg) ((reg) >> 1)

// The register file follows mGBA's conventions so states compare directly: between instructions,
// r[15] holds the next instruction's address plus one instruction width.
struct CPU {
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t spsr;
	int32_t cycles;
	int32_t nextEvent;
	uint32_t bank[6][7];
	uint32_t bankSpsr[6];
	uint32_t prefetch[2];
	uint32_t mode;
	int32_t halted;
	// The carry out of the last ARM data-processing shifter, in bit 8 (the other bits are whatever
	// was convenient to store). mGBA's MULS and MLAS set C from it.
	uint32_t shifterCarry;
	// The I flag as the IRQ check sees it: its value when the previous instruction started, or as a
	// pipeline refill left it.
	bool irqMasked;
	bool unused;
	// Whether the next code fetch is sequential to the last one.
	bool fetchSeq;
	// Whether the cartridge prefetcher is running. It sits right after fetchSeq at an even offset,
	// so compiled code can set both with one store.
	bool prefetching;
};

struct Event;
struct GBA;
typedef void (*EventCallback)(struct GBA*, void* context, uint32_t late);

// A host pointer in the machine's state takes eight bytes whatever the CPU, so a state is laid out
// the same in 32-bit and 64-bit builds and loads in either.
#define HOST_POINTER(declaration, name) union { declaration; uint64_t name##Slot; }

struct Event {
	HOST_POINTER(EventCallback callback, callback);
	HOST_POINTER(void* context, context);
	uint32_t when;
	unsigned priority;
	// When set, the run loop returns at the instruction boundary after this event.
	bool endsRun;
	HOST_POINTER(struct Event* next, next);
};

struct Timing {
	HOST_POINTER(struct Event* root, root);
	HOST_POINTER(struct Event* reroot, reroot);
	uint32_t masterCycles;
	uint64_t globalCycles;
};

struct Timer {
	uint16_t reload;
	bool countUp;
	bool doIrq;
	bool enable;
	int prescaleBits;
	// While running, the timer counts on the clock from `counter`, starting at time `started`. A
	// cascaded or stopped timer just holds `counter`.
	bool running;
	uint32_t counter;
	uint32_t started;
	// Overflows that only feed the sound FIFOs are batched and run when something needs them; the
	// event is for the overflow after them.
	int quiet;
	// Writes reach the timer one cycle later.
	uint16_t pendingReload;
	uint16_t pendingControl;
	struct Event event;
	struct Event reloadWrite;
	struct Event controlWrite;
};

struct Dma {
	uint16_t reg;
	uint32_t source;
	uint32_t dest;
	// Where the transfer has got to
	uint32_t nextSource;
	uint32_t nextDest;
	int32_t nextCount;
	// The last value the channel read; a source below EWRAM reads this value again.
	uint32_t latch;
	// Whether this is a sound FIFO's channel, which moves four words to a fixed address.
	bool fifo;
	// A request starts the channel two cycles later.
	struct Event start;
};

// A change the interrupt controller makes a few cycles after its cause.
struct IrqChange {
	struct Event event;
	bool value;
	bool busy;
};

// Writes to IE, IF and IME, and requests from the hardware, reach the registers one cycle later.
// HALT sees IE & IF change one cycle after that, and the CPU's IRQ line follows two cycles after
// that.
struct Irq {
	uint16_t ie;
	uint16_t iflags;
	bool ime;
	bool available;
	bool line;
	// The IRQ line as the controller last drove it, before the CPU sees it.
	bool lineDriven;
	struct IrqChange apply[4];
	struct IrqChange availableChange[4];
	struct IrqChange lineChange[4];
	// H-blank IRQ requests since quietFrom that aren't in IF yet, because IE leaves them out (see
	// gbaIrqSync).
	bool quietPending;
	uint32_t quietFrom;
	// The time until which IF in io stays valid without calling gbaIrqSync.
	uint32_t ifStable;
};

struct Video {
	int vcount;
	uint32_t frameCounter;
	// When the current line started, as far as videoSync has caught up.
	uint32_t lineStart;
	// The time until which VCOUNT and DISPSTAT in io stay valid without calling videoSync.
	uint32_t stableUntil;
	// Whether the current line has been drawn, and the H-blank of the next line to draw; from then
	// on, a store to video memory waits for it.
	bool drawn;
	uint32_t drawDue;
	// The next H-blank or line start that does something, and its line.
	struct Event event;
	int eventLine;
	// The event that sets the VCOUNT match flag one cycle into the line.
	struct Event matchEvent;
	// V-blank, H-blank and VCOUNT match IRQ requests, one cycle after their flags.
	struct Event irqEvent[3];
	// The event that latches DISPCNT 40 cycles into the line.
	struct Event latchEvent;
	// The display capture DMA's request, 3 cycles into the line.
	struct Event captureEvent;
};

struct PsgEnvelope {
	int length;
	int duty;
	int stepTime;
	int initialVolume;
	int currentVolume;
	bool direction;
	int dead;
	int nextStep;
};

struct PsgSquare {
	struct {
		int shift;
		int time;
		int step;
		bool direction;
		bool enable;
		bool occurred;
		int realFrequency;
	} sweep;
	struct PsgEnvelope envelope;
	struct {
		int frequency;
		int length;
		bool stop;
	} control;
	int8_t index;
	int32_t lastUpdate;
	int8_t sample;
};

struct PsgWave {
	bool size;
	bool bank;
	bool enable;
	unsigned length;
	int volume;
	int rate;
	bool stop;
	int window;
	bool readable;
	union {
		uint32_t wavedata32[8];
		uint8_t wavedata8[32];
	};
	int32_t nextUpdate;
	int8_t sample;
};

struct PsgNoise {
	struct PsgEnvelope envelope;
	int ratio;
	int frequency;
	bool power;
	bool stop;
	int length;
	uint32_t lfsr;
	int nSamples;
	int samples;
	int32_t lastEvent;
	int8_t sample;
};

struct Psg {
	struct PsgSquare ch1;
	struct PsgSquare ch2;
	struct PsgWave ch3;
	struct PsgNoise ch4;
	int8_t volumeRight;
	int8_t volumeLeft;
	bool ch1Right, ch2Right, ch3Right, ch4Right;
	bool ch1Left, ch2Left, ch3Left, ch4Left;
	bool playingCh1, playingCh2, playingCh3, playingCh4;
	int frame;
	bool skipFrame;
	bool enable;
	int32_t lastSample;
	int sampleIndex;
	struct Event frameEvent;
	// psgSample's output as of the last sample, valid until a channel next steps.
	bool cached;
	int16_t cachedLeft;
	int16_t cachedRight;
	int32_t nextChange;
};

enum {
	AUDIO_FIFO_SIZE = 8,
	AUDIO_MAX_SAMPLES = 16,
};

struct Fifo {
	uint32_t fifo[AUDIO_FIFO_SIZE];
	int fifoWrite;
	int fifoRead;
	uint32_t internalSample;
	int internalRemaining;
	int dmaSource;
	int8_t samples[AUDIO_MAX_SAMPLES];
};

struct StereoSample {
	int16_t left;
	int16_t right;
};

struct Audio {
	struct Psg psg;
	struct Fifo chA;
	struct Fifo chB;
	int volume;
	bool volumeChA;
	bool volumeChB;
	bool chARight, chALeft, chATimer;
	bool chBRight, chBLeft, chBTimer;
	bool enable;
	uint16_t soundbias;
	int32_t sampleInterval;
	int32_t lastSample;
	int sampleIndex;
	struct StereoSample currentSamples[AUDIO_MAX_SAMPLES];
	// When the sample interval being filled ends. Intervals are finished only when something needs
	// them.
	int32_t nextSample;
	// Output for the frontend.
	struct StereoSample out[4096];
	uint32_t outCount;
};

enum SavedataType {
	SAVEDATA_AUTODETECT = -1,
	SAVEDATA_FORCE_NONE = 0,
	SAVEDATA_SRAM = 1,
	SAVEDATA_FLASH512 = 2,
	SAVEDATA_FLASH1M = 3,
	SAVEDATA_EEPROM = 4,
	SAVEDATA_EEPROM512 = 5,
	SAVEDATA_SRAM512 = 6,
};

struct Savedata {
	enum SavedataType type;
	HOST_POINTER(uint8_t* data, data);
	int command;
	int flashState;
	HOST_POINTER(uint8_t* currentBank, currentBank);
	uint32_t readBitsRemaining;
	uint32_t readAddress;
	uint32_t writeAddress;
	// The Flash sector an erase last worked on; it reads busy while the dust event is scheduled.
	int settling;
	struct Event dust;
	bool dirty;
	// Whether a frontend keeps its save file in storage. mGBA's libretro core keeps one as large as
	// storage, so detecting the save type keeps what is already there.
	bool backed;
	// Save memory; the frontend loads and stores it here.
	uint8_t storage[SIZE_FLASH1M];
};

struct Gpio {
	int devices;
	uint8_t readWrite;
	uint8_t writeLatch;
	uint8_t pinState;
	uint8_t direction;
	struct {
		int bytesRemaining;
		int bitsRead;
		uint32_t bits;
		bool commandActive;
		uint32_t command;
		bool sckEdge;
		bool sioOutput;
		uint8_t control;
		uint8_t time[7];
		int64_t lastLatch;
		int64_t offset;
	} rtc;
	// Returns seconds since the epoch; the system clock when not set.
	HOST_POINTER(int64_t (*clock)(void* context), clock);
	HOST_POINTER(void* clockContext, clockContext);
	// The frontend's sensor controls, set before each frame in mGBA's units: the turn for the gyro,
	// the tilt on each axis for the tilt sensor, and the light sensor's reading (lower is
	// brighter).
	int32_t gyroZ;
	int32_t tiltInputX;
	int32_t tiltInputY;
	uint8_t luminance;
	// Sensor state: the gyro's reading being shifted out, the light sensor's counter against its
	// reading, the tilt sensor's last reading and handshake step, and whether the rumble motor is
	// on.
	uint16_t gyroSample;
	bool gyroEdge;
	uint8_t lightCounter;
	uint8_t lightSample;
	bool lightEdge;
	uint16_t tiltX;
	uint16_t tiltY;
	uint8_t tiltState;
	bool rumble;
};

// A link cable to other consoles, run by the frontend. Without one, nothing is attached.
struct SioLink {
	void* context;
	// Consoles on the cable, this one included, and this one's place: 0 is the parent.
	int players;
	int id;
	// Whether every other console on the cable is in multiplayer mode, which with this one's own
	// makes the SD line high. The frontend keeps it, and calls sioLinkChanged when it changes.
	bool ready;
	// A transfer this console clocks: begin sends this console's data as the transfer starts, with
	// the cycles until it ends; finish waits for the others' and fills `in` (one value in normal mode,
	// three in multiplayer). Finish can instead return false, having stopped the console
	// (returnRequested and timingInterrupt), and is called again on the same cycle when the console
	// runs on.
	void (*begin)(void* context, int mode, uint32_t out, int32_t cycles);
	bool (*finish)(void* context, uint32_t in[3]);
	// Called when the game writes the data it puts on the cable; may be NULL.
	void (*wrote)(void* context);
};

// The air between Wireless Adapters: the frontend carries the messages one adapter sends to the
// others, to one console or to all (to = -1), and hands them in with rfuReceive. In-core, a message
// arrives as it is sent and the adapters add the radio's delays; over a network the network's time
// stands in for them. The frontend keeps it; a state doesn't hold it.
struct RfuAir {
	// This console's number on the air.
	int self;
	bool network;
	void (*send)(const struct RfuAir* air, int to, const uint8_t* message, size_t size);
};

enum {
	RFU_SLOTS = 4,
	// The most a host sends at once, and a client.
	RFU_HOST_BYTES = 90,
	RFU_CLIENT_BYTES = 16,
	// The most consoles an adapter hears.
	RFU_ROOMS = 8,
	// The most packets an adapter holds from one sender over a network.
	RFU_QUEUE = 4,
};

// A packet over the air, and when it gets there by the receiving console's clock.
struct RfuPacket {
	uint8_t length;
	int32_t arrival;
	uint8_t data[RFU_HOST_BYTES];
};

// The packets heard from one sender and not yet read, oldest first (rfu.c hear()).
struct RfuQueue {
	uint8_t count;
	struct RfuPacket packets[RFU_QUEUE];
};

// The Game Boy Advance Wireless Adapter on the link port (rfu.c): the serial side the game talks to,
// and the radio side, a host with up to four clients.
struct Rfu {
	bool attached;
	// The serial side: what it is doing, and the word it puts on the wire at the next transfer.
	uint8_t wire;
	uint32_t next;
	uint32_t out;
	// The "NINTENDO" exchange that finds the adapter.
	uint8_t idCount;
	uint16_t idSend;
	uint16_t idRecv;
	uint16_t idLast;
	// The command under way: its data, then its reply.
	uint8_t command;
	uint8_t length;
	uint8_t count;
	uint32_t words[32];
	// What the adapter sends as clock master, ending a wait.
	uint32_t push[3];
	uint8_t pushLength;
	uint8_t pushCount;
	bool pushing;
	// The radio side.
	uint8_t radio;
	bool open;
	uint16_t id;
	uint32_t broadcast[6];
	uint32_t setup;
	uint32_t seed;
	struct {
		bool active;
		uint16_t id;
		uint8_t console;
		struct RfuQueue rx;
	} clients[RFU_SLOTS];
	// A client's host and slot, and a connection under way.
	uint8_t hostConsole;
	uint16_t hostId;
	uint8_t number;
	bool answered;
	int32_t connected;
	// A client's packets from its host; a client's packet waiting for the host's next one, or the
	// host's last packet.
	struct RfuQueue rx;
	struct RfuPacket tx;
	// The rooms heard: each host's console, whether it is open now, and when it was last heard.
	struct {
		bool heard;
		bool live;
		uint8_t console;
		uint16_t id;
		uint8_t slot;
		int32_t at;
		uint32_t broadcast[6];
	} rooms[RFU_ROOMS];
	// A wait: when it times out, when the host's exchange is done, and slots lost meanwhile.
	bool timed;
	int32_t deadline;
	bool exchange;
	int32_t exchanged;
	uint8_t lost;
	uint8_t lostReason;
	// When it last asked the rooms around to make themselves heard.
	int32_t asked;
	struct Event event;
};

struct Sio {
	uint16_t siocnt;
	uint16_t rcnt;
	int mode;
	struct Event completeEvent;
	HOST_POINTER(const struct SioLink* link, link);
	struct Rfu rfu;
	HOST_POINTER(const struct RfuAir* air, air);
};

// The cartridge prefetcher. While the cartridge bus is free, it reads ahead of the CPU's code
// fetches, one read every `duty` cycles, into a buffer of 8 halfwords (4 words in ARM state). At
// time t it is min(t - base, full) cycles ahead of the fetch at `head`, so a fetch there costs
// max(1, base + duty - t) and moves base on by duty.
struct Prefetcher {
	int width;
	int32_t duty;
	// The furthest ahead it can get: the buffer full, or the last read once it is turned off.
	int32_t full;
	uint32_t head;
	uint32_t base;
};

struct Jit;

// Padding that puts struct GBA's memories on a page boundary; the assertion after the struct says
// when to change it. The struct is laid out the same on every CPU, so one value serves all.
enum { PAD_TO_PAGE = 3695 };

// Hardware some cartridges have beyond a ROM and a save chip (cart.c).
enum {
	MATRIX_PAGES = 16,
};

struct Cart {
	// The whole image of a cartridge with the Matrix mapper, which is larger than the address
	// space.
	HOST_POINTER(uint8_t* file, file);
	uint64_t fileSize;
	bool matrix;
	uint32_t matrixCmd;
	uint32_t matrixPaddr;
	uint32_t matrixVaddr;
	uint32_t matrixSize;
	// Where in the image each page of the window comes from.
	uint32_t matrixMap[MATRIX_PAGES];
	// Vast Fame's protection: 0 for none, or which of its three scramblings. The modes are -1 until
	// set.
	int vfame;
	int vfameSramMode;
	int vfameRomMode;
	uint8_t vfameSequence[5];
	bool vfameChanging;
	// AGBPrint, a development cartridge's debug output: its lock, and its request, bank, get and
	// put registers.
	uint16_t printProtect;
	uint16_t printCtx[4];
	// A multi-game cartridge: its bank, offset and size registers, and which part of the image
	// shows now.
	bool multi;
	bool multiLocked;
	uint8_t multiBank;
	uint8_t multiOffset;
	uint8_t multiSize;
	uint32_t multiAt;
	uint32_t multiLength;
};

struct GBA {
	struct CPU cpu;

	// Read by generated code.
	int32_t activeRegion;
	// The region whose wait states instruction fetches use. mGBA leaves them unchanged on a jump to
	// an address it can't fetch from, so code there runs at the previous region's speed.
	int32_t fetchRegion;
	uint32_t lastPrefetchedPc;
	bool prefetchEnabled;
	uint32_t romSize;
	uint32_t romMask;
	HOST_POINTER(uint8_t* rom, rom);
	// A multiboot image loaded instead of a cartridge; reset copies it into EWRAM.
	HOST_POINTER(uint8_t* multiboot, multiboot);
	uint32_t multibootSize;
	uint8_t waitN16[256];
	uint8_t waitS16[256];
	uint8_t waitN32[256];
	uint8_t waitS32[256];
	uint32_t biosPrefetch;
	uint8_t writePcCost16[256];
	uint8_t writePcCost32[256];

	// Recompiler bookkeeping, read and written by generated code.
	uint32_t exitPc;
	uint32_t exitOps[2];
	uint32_t exitReason;
	uint32_t exitFlags;
	HOST_POINTER(void* exitLink, exitLink);
	HOST_POINTER(void* siteReturn, siteReturn);
	bool siteValid;
	uint32_t biosPrefetchNext;
	HOST_POINTER(void* lookup, lookup);
	HOST_POINTER(void* hostStack, hostStack);
	bool jitRunning;
	// Operands compiled code passes to a helper.
	uint32_t jitScratch[4];
	// Set when a careful copy should run the events inside an instruction once the instruction is
	// done.
	bool jitDeferred;
	// Set when compiled code has to leave at its next instruction boundary.
	bool jitLeave;
	// The prefetch buffer's base in compiled code, counted like r14d relative to the next event.
	int32_t jitBase;
	// A waiting loop's last time round: the block key its way back left, when it started (like
	// r14d), eventRuns, nextEvent, the prefetch buffer's base minus r14d, and video.stableUntil.
	uint32_t idleMark;
	int32_t idleCycles;
	uint32_t idleRuns;
	int32_t idleNext;
	int32_t idleLead;
	uint32_t idleStable;
	// Whether stopping the prefetch buffer costs a cycle, indexed by [32-bit][duty cycle][how far
	// ahead it is].
	uint8_t jitStopPenalty[2][32][128];
	// Counts processEvents calls, so code called from compiled code can tell that events ran.
	uint32_t eventRuns;
	// eventRuns when the run loop last examined an idle loop's head. The head's compiled code
	// leaves for the run loop to look again once events have run since.
	uint32_t idleHeadRuns;
	// stepTraceOpen's log of instructions and their start times.
	HOST_POINTER(void* traceFile, traceFile);
	uint32_t traceFromPc;
	int32_t traceFromTime;
	int64_t traceLeft;
	bool tracing;
	// Set while the exact CPU goes round a loop to see whether it is idle; its reads and writes go
	// to jit.c.
	bool idleProbing;

	struct Timing timing;
	HOST_POINTER(struct Jit* jit, jit);
	// The renderer's second thread, if it has one.
	HOST_POINTER(struct RenderThread* renderThread, renderThread);
	struct Irq irq;
	struct Timer timers[4];
	struct Dma dma[4];
	// Channels started and waiting for the bus, one bit each; they run before the CPU's next
	// access.
	uint8_t dmaRunnable;
	int activeDma;
	// Set when a channel starts while another runs: the running one stops after its current unit to
	// let it in.
	bool dmaReenter;
	// 1 | channel << 1 while a channel is transferring.
	int performingDma;
	// The last value any channel moved, which reads of nothing return during and just after DMA.
	uint32_t dmaLatch;
	bool lastAccessDma;
	// The CPU's internal cycles while DMA runs are free, up to how long the DMA took.
	int32_t dmaIdleCredit;
	// The cartridge's address counter: sequential accesses read on from it, whatever the address.
	uint32_t romLatch;
	// Where the GBA expects the next sequential cartridge access, from the last address it asked
	// for.
	uint32_t romNext;
	struct Prefetcher prefetcher;
	// Turning the prefetcher off makes the next code fetch from the cartridge nonsequential.
	bool prefetchDisabled;
	bool earlyExit;
	// Set by a frontend's event callback to get control back once events are processed.
	bool returnRequested;
	bool biosLoaded;
	struct Gpio gpio;
	struct Video video;
	struct Audio audio;
	struct Savedata savedata;
	struct Sio sio;
	uint16_t keysActive;
	uint16_t keysLast;

	uint16_t io[REG_INTERNAL_MAX / 2];
	// Set when compiled code came from palette, VRAM or OAM since the last flush, so stores there
	// go to the JIT.
	bool videoCode;
	// Padding so the memories compiled code reaches into start a multiple of 4096 bytes in: on
	// AArch64 that takes one add instruction, any other offset two.
	uint8_t toPage[PAD_TO_PAGE];
	uint8_t iwramCode[SIZE_IWRAM / 4];
	uint8_t ewramCode[SIZE_EWRAM / 4];
	uint8_t bios[SIZE_BIOS];
	uint8_t iwram[SIZE_IWRAM];
	uint8_t vram[SIZE_VRAM];
	uint8_t ewram[SIZE_EWRAM];
	uint8_t palette[SIZE_PALETTE];
	uint8_t oam[SIZE_OAM];
	// Mode 2 with both of its backgrounds on: the display has VRAM to itself while it draws a line.
	bool vramStall;
	struct Cart cart;
	// Last, so that growing it doesn't move the memories.
	struct Renderer renderer;
};

_Static_assert(offsetof(struct GBA, iwramCode) % 4096 == 0, "change PAD_TO_PAGE so that iwramCode starts on a page");
// States are the struct as it lies in memory; this size is the same in every build.
_Static_assert(sizeof(struct GBA) == 788504, "struct GBA changed: check it is laid out the same on 32-bit and 64-bit CPUs (tests/state-layout.sh), then update this size and STATE_MAGIC");

#define CPU_FREQUENCY 0x1000000

// arm.c
void cpuReset(struct GBA*);
void cpuSetPrivilegeMode(struct GBA*, unsigned mode);
void cpuWriteCpsr(struct GBA*, uint32_t cpsr);
int32_t cpuWritePc(struct GBA*);
bool cpuModeHasSpsr(unsigned mode);
bool cpuMultiplyCarryFull(uint32_t multiplier);
bool cpuMultiplyCarryLow(uint32_t multiplicand, uint32_t multiplier, uint32_t accum);
bool cpuMultiplyCarryHigh(uint32_t multiplicand, uint32_t multiplier, uint32_t accumHi, bool sign);
void jitHelperMsr(struct GBA*, uint32_t value, uint32_t packed);

// gba.c
struct GBA* gbaCreate(void);
void gbaDestroy(struct GBA*);
bool gbaLoadBios(struct GBA*, const void* data, size_t size);
bool gbaLoadRom(struct GBA*, const void* data, size_t size);
void gbaReset(struct GBA*, bool skipBios);
void gbaRunPart(struct GBA*);
void gbaSetKeys(struct GBA*, uint16_t keys);
void gbaRaiseIrq(struct GBA*, int irq, uint32_t late);
bool gbaQuietHblank(struct GBA*);
void gbaIrqSync(struct GBA*);
void gbaIrqQuietChanged(struct GBA*, bool wasQuiet);
void gbaWriteIe(struct GBA*, uint16_t value);
void gbaWriteIf(struct GBA*, uint16_t value);
void gbaWriteIme(struct GBA*, uint16_t value);
void gbaTestKeypadIrq(struct GBA*, uint32_t late);
void gbaHalt(struct GBA*);
void gbaStop(struct GBA*);
void gbaInterrupt(struct GBA*);
void gbaFrameStarted(struct GBA*, uint32_t late);
void gbaFrameEnded(struct GBA*);

// state.c: save states, for the same game in the same build, taken between frames.
size_t gbaStateSize(void);
void gbaSaveState(struct GBA*, void* state);
bool gbaLoadState(struct GBA*, const void* state, size_t size);

// memory.c
const uint8_t* codePointer(struct GBA*, uint32_t address);
uint32_t codeFetch32(struct GBA*, uint32_t address);
uint16_t codeFetch16(struct GBA*, uint32_t address);
void memoryInit(struct GBA*);
void memoryReset(struct GBA*);
void memorySetActiveRegion(struct GBA*, uint32_t address);
bool memoryCanFetch(struct GBA*, uint32_t address);
void memoryAdjustWaitstates(struct GBA*, uint16_t parameters);
void memoryAdjustEwramWaitstates(struct GBA*, uint16_t parameters);
uint32_t memoryLoad32(struct GBA*, uint32_t address, int* cycles);
uint32_t memoryLoad16(struct GBA*, uint32_t address, int* cycles);
uint32_t memoryLoad8(struct GBA*, uint32_t address, int* cycles);
void memoryStore32(struct GBA*, uint32_t address, uint32_t value, int* cycles);
void memoryStore16(struct GBA*, uint32_t address, uint16_t value, int* cycles);
// Moves count DMA units between plain regions without leaving them; returns the last latch.
int memoryDmaCopy(struct GBA*, struct Dma*, int count, bool wide, int32_t sourceStep, int32_t destStep);
void memoryStore8(struct GBA*, uint32_t address, uint8_t value, int* cycles);
uint32_t memoryLoadMultiple(struct GBA*, uint32_t address, int mask, int direction, int* cycles);
uint32_t memoryStoreMultiple(struct GBA*, uint32_t address, int mask, int direction, int* cycles);
int32_t memoryStall(struct GBA*, int32_t wait);
uint32_t memoryLoadBad(struct GBA*);
uint32_t memoryView32(struct GBA*, uint32_t address);
uint16_t memoryView16(struct GBA*, uint32_t address);

// timing.c
void timingClear(struct GBA*);
void timingSchedule(struct GBA*, struct Event*, int32_t when);
void timingScheduleAbsolute(struct GBA*, struct Event*, int32_t when);
void timingDeschedule(struct GBA*, struct Event*);
bool timingIsScheduled(struct GBA*, struct Event*);
int32_t timingTick(struct GBA*, int32_t cycles);
int32_t timingNextEvent(struct GBA*);
static inline int32_t timingCurrentTime(struct GBA* gba) {
	return gba->timing.masterCycles + gba->cpu.cycles;
}
int32_t timingUntil(struct GBA*, struct Event*);
void timingInterrupt(struct GBA*);
void timingResetNextEvent(struct GBA*);
// Whether the events due by time can run late, at the next instruction boundary: none of them
// starts DMA or ends a run.
bool timingDeferrable(struct GBA*, int32_t time);
void processEvents(struct GBA*);

// step.c: the exact CPU, used for the instructions around events.
void stepReset(struct GBA*);
void stepInstruction(struct GBA*);
// After a PC write, refills the prefetch from the new PC.
void stepReload(struct GBA*);
// Logs each instruction the exact CPU runs, and each compiled block entered, with its start time.
// Starts at TRACE_FROM=<hex pc> or TRACE_TIME=<cycle>, for TRACE_COUNT=<n> lines.
void stepTraceOpen(struct GBA*, const char* path);
void stepTrace(struct GBA*);
// Runs a decoded instruction whose fetch is done and whose condition has passed.
struct Inst;
void stepExecute(struct GBA*, const struct Inst*);

// io.c
void ioInit(struct GBA*);
uint16_t ioRead(struct GBA*, uint32_t address);
void ioWrite(struct GBA*, uint32_t address, uint16_t value);
void ioWrite8(struct GBA*, uint32_t address, uint8_t value);
void ioWrite32(struct GBA*, uint32_t address, uint32_t value);

// video.c
void videoInit(struct GBA*);
void videoReset(struct GBA*);
void videoStartAt(struct GBA*, int vcount, int32_t untilHblank);
// Brings the line, VCOUNT and DISPSTAT up to date.
void videoSync(struct GBA*);
int videoLine(struct GBA*);
void videoHblankDmaArmed(struct GBA*);
uint32_t videoNextHblank(struct GBA*, uint32_t from);
uint16_t videoWriteDispstat(struct GBA*, uint16_t value);
void videoDispcntWritten(struct GBA*);
int32_t videoVramStall(struct GBA*, int32_t wait);
bool videoEventIsHblank(struct GBA*);
void videoEventRestore(struct GBA*, bool hblank);
// Whether a video event can run at the end of the instruction it falls in, because it starts no
// DMA.
bool videoDeferrable(struct GBA*, const struct Event*);
void videoCaptureArmed(struct GBA*);

// timer.c
void timerInit(struct GBA*);
uint16_t timerReadCounter(struct GBA*, int timer);
void timerWriteReload(struct GBA*, int timer, uint16_t reload);
void timerWriteControl(struct GBA*, int timer, uint16_t control);
// Runs the quiet overflows due by time, including at it.
void timerCatchUp(struct GBA*, uint32_t time, bool inclusive);
// After a change to what overflows do, runs the quiet overflows due by now and turns the rest back
// into events.
void timerReplan(struct GBA*, uint32_t time);
// Whether a timer event can run at the end of the instruction it falls in, because it starts no
// DMA.
bool timerDeferrable(struct GBA*, const struct Event*);

// dma.c
void dmaInit(struct GBA*);
void dmaReset(struct GBA*);
uint32_t dmaWriteSource(struct GBA*, int dma, uint32_t address);
uint32_t dmaWriteDest(struct GBA*, int dma, uint32_t address);
uint16_t dmaWriteControl(struct GBA*, int dma, uint16_t control);
void dmaRequestHblank(struct GBA*, uint32_t late);
void dmaRequestVblank(struct GBA*, uint32_t late);
void dmaRequestDisplayStart(struct GBA*, uint32_t late);
void dmaRequestFifo(struct GBA*, int dma, uint32_t late);
int32_t dmaRun(struct GBA*);
// Whether a FIFO's request starts the channel.
bool dmaFifoArmed(struct GBA*, int dma);
bool dmaStartEvent(const struct Event*);
// Whether a request with this timing would start a channel.
bool dmaTimingArmed(struct GBA*, int timing);

// bus.c: timed accesses, each moving the clock on by its own cycles.
int32_t busCycles(struct GBA*, uint32_t address, int size, bool seq);
void busTick(struct GBA*, int32_t cycles);
uint32_t busRead(struct GBA*, uint32_t address, int size, bool seq);
void busWrite(struct GBA*, uint32_t address, uint32_t value, int size, bool seq);
uint32_t busFetch(struct GBA*, uint32_t address, bool thumb);
bool busRefillCartridge(struct GBA*, uint32_t pc, bool thumb);
void busIdle(struct GBA*);
uint32_t busDmaRead(struct GBA*, uint32_t address, int size, bool seq);
void busDmaWrite(struct GBA*, uint32_t address, uint32_t value, int size, bool seq);

// audio.c
void audioInit(struct GBA*);
void audioReset(struct GBA*);
void audioSample(struct GBA*, int32_t timestamp);
// Handles an overflow of the timer a FIFO plays at, at time.
void audioSampleFifo(struct GBA*, int fifo, uint32_t time);
// Finishes the sample intervals that end before time, or at it too.
void audioCatchUp(struct GBA*, int32_t time, bool inclusive);
void audioSync(struct GBA*);
void audioWriteSoundcntHi(struct GBA*, uint16_t value);
void audioWriteSoundcntX(struct GBA*, uint16_t value);
void audioWriteSoundbias(struct GBA*, uint16_t value);
void audioWriteWaveRam(struct GBA*, int address, uint32_t value);
uint32_t audioReadWaveRam(struct GBA*, int address);
uint32_t audioWriteFifo(struct GBA*, uint32_t address, uint32_t value);

// psg.c
void psgInit(struct GBA*);
void psgReset(struct GBA*);
void psgRun(struct GBA*, int32_t timestamp, int channels);
void psgUpdateFrame(struct GBA*, int32_t now);
void psgSample(struct GBA*, int16_t* left, int16_t* right);
void psgWrite(struct GBA*, int nr, uint8_t value);
void psgWriteNr52(struct GBA*, uint8_t value);

// savedata.c
void savedataInit(struct GBA*);
void savedataReset(struct GBA*);
void savedataForceType(struct GBA*, enum SavedataType);
void savedataInitSram(struct GBA*);
void savedataInitFlash(struct GBA*);
void savedataInitEeprom(struct GBA*);
uint8_t savedataReadFlash(struct GBA*, uint16_t address);
void savedataWriteFlash(struct GBA*, uint16_t address, uint8_t value);
uint16_t savedataReadEeprom(struct GBA*);
void savedataWriteEeprom(struct GBA*, uint16_t value, uint32_t writeSize);
size_t savedataSize(struct GBA*);

// overrides.c
bool cartLoad(struct GBA*, const uint8_t* data, size_t size);
void cartFree(struct GBA*);
void cartReset(struct GBA*);
void cartRestore(struct GBA*);
void cartMatrixWrite(struct GBA*, uint32_t reg, uint32_t value);
void cartMatrixWrite16(struct GBA*, uint32_t reg, uint16_t value);
uint32_t cartVfameRead(uint32_t address, int size);
bool cartPrintRead(struct GBA*, uint32_t address, uint16_t* value);
bool cartPrintWrite(struct GBA*, uint32_t address, uint16_t value);
const uint8_t* cartPrintCode(struct GBA*, uint32_t address);
void cartVfameSramWrite(struct GBA*, uint32_t address, uint8_t value);
bool cartMultiWrite(struct GBA*, uint32_t address, uint8_t value);
int overridesApply(struct GBA*);
bool overridesLogoValid(struct GBA*);

// gpio.c
enum {
	HW_NONE = 0,
	HW_RTC = 1,
	HW_RUMBLE = 2,
	HW_LIGHT_SENSOR = 4,
	HW_GYRO = 8,
	HW_TILT = 16,
	HW_GB_PLAYER = 32,
	HW_GB_PLAYER_DETECTION = 64,
	HW_EREADER = 128,
	HW_NO_OVERRIDE = 0x8000,
};
void gpioSetDevices(struct GBA*, int devices);
void gpioReset(struct GBA*);
void gpioWrite(struct GBA*, uint32_t address, uint16_t value);
// The tilt sensor, mapped into the save memory's addresses; offset is the address's low 24 bits.
uint8_t gpioTiltRead(struct GBA*, uint32_t offset);
void gpioTiltWrite(struct GBA*, uint32_t offset, uint8_t value);

// sio.c
void sioInit(struct GBA*);
void sioReset(struct GBA*);
void sioWrite(struct GBA*, uint32_t address, uint16_t value);
uint16_t sioRead(struct GBA*, uint32_t address);
void sioAttach(struct GBA*, const struct SioLink*);
void sioLinkChanged(struct GBA*);
bool sioMultiplayer(const struct GBA*);
// For linked consoles run together exactly: cycles until the transfer under way ends (-1 for none),
// and the fewest cycles a transfer started now would take at the port's settings (INT32_MAX while it
// isn't set up for a link).
int32_t sioTransferLeft(struct GBA*);
int32_t sioShortestTransfer(const struct GBA*);
uint32_t sioOutgoing(struct GBA*, int* mode, bool* ready);
void sioReceive(struct GBA*, const uint32_t values[4]);

// rfu.c: the Wireless Adapter. sio.c hands it each transfer the game clocks, and tells it of writes
// to SIOCNT and RCNT.
void rfuInit(struct GBA*);
void rfuAttach(struct GBA*, bool attached, const struct RfuAir*);
void rfuReset(struct GBA*);
uint32_t rfuTransfer(struct GBA*, uint32_t out);
uint16_t rfuSiocnt(struct GBA*, uint16_t before, uint16_t after);
void rfuRcnt(struct GBA*, uint16_t before, uint16_t after);
void rfuSiocntWritten(struct GBA*);
// A message from another console's adapter.
void rfuReceive(struct GBA*, int from, const uint8_t* message, size_t size);
// Another console left the air, or all did (-1).
void rfuPeerGone(struct GBA*, int console);

#endif
