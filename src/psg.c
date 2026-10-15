/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// The four Game Boy sound channels in their GBA form, stepped the same way as mGBA's GB audio so
// the status bits games read back match.

enum {
	TIMING_FACTOR = 4,
	FRAME_CYCLES = 0x400000 >> 9,
};

static const int squareDuty[4][8] = {
	{ 0, 0, 0, 0, 0, 0, 0, 1 },
	{ 1, 0, 0, 0, 0, 0, 0, 1 },
	{ 1, 0, 0, 0, 0, 1, 1, 1 },
	{ 0, 1, 1, 1, 1, 1, 1, 0 },
};

// Steps from each position until squareDuty flips.
static const uint8_t dutySteps[4][8] = {
	{ 7, 6, 5, 4, 3, 2, 1, 1 },
	{ 1, 6, 5, 4, 3, 2, 1, 2 },
	{ 1, 4, 3, 2, 1, 4, 3, 2 },
	{ 1, 6, 5, 4, 3, 2, 1, 2 },
};

// SOUNDCNT_X's low byte: the master switch, and a bit for each channel that is playing.
static uint8_t* nr52(struct GBA* gba) {
	return (uint8_t*) &gba->io[IO(REG_SOUNDCNT_X)];
}

// Returns a square channel's output: its duty pattern's current step times its volume.
static void updateSquareSample(struct PsgSquare* ch) {
	ch->sample = squareDuty[ch->envelope.duty][ch->index] * ch->envelope.currentVolume;
}

// Steps a volume envelope once, up or down. At either end it stops; dead says which end.
static void updateEnvelope(struct PsgEnvelope* envelope) {
	if (envelope->direction) {
		++envelope->currentVolume;
	} else {
		--envelope->currentVolume;
	}
	if (envelope->currentVolume >= 15) {
		envelope->currentVolume = 15;
		envelope->dead = 1;
	} else if (envelope->currentVolume <= 0) {
		envelope->currentVolume = 0;
		envelope->dead = 2;
	} else {
		envelope->nextStep = envelope->stepTime;
	}
}

// Works out again, after its settings change, whether an envelope has nothing left to do.
static void updateEnvelopeDead(struct PsgEnvelope* envelope) {
	if (!envelope->stepTime) {
		envelope->dead = envelope->currentVolume ? 1 : 2;
	} else if (!envelope->direction && !envelope->currentVolume) {
		envelope->dead = 2;
	} else if (envelope->direction && envelope->currentVolume == 0xF) {
		envelope->dead = 1;
	} else if (envelope->dead) {
		envelope->nextStep = envelope->stepTime;
		envelope->dead = 0;
	}
}

// A trigger starts the envelope from its initial volume. Returns whether the channel's DAC is on: a
// channel with volume 0 going down makes no sound and stops playing.
static bool resetEnvelope(struct PsgEnvelope* envelope) {
	envelope->currentVolume = envelope->initialVolume;
	envelope->nextStep = envelope->stepTime;
	updateEnvelopeDead(envelope);
	return envelope->initialVolume || envelope->direction;
}

// The envelope register: step time, direction, initial volume.
static bool writeEnvelope(struct PsgEnvelope* envelope, uint8_t value) {
	envelope->stepTime = value & 7;
	envelope->direction = (value >> 3) & 1;
	envelope->initialVolume = value >> 4;
	if (!envelope->stepTime) {
		envelope->currentVolume &= 0xF;
	}
	updateEnvelopeDead(envelope);
	return envelope->initialVolume || envelope->direction;
}

// The duty and length register.
static void writeDuty(struct PsgEnvelope* envelope, uint8_t value) {
	envelope->length = value & 0x3F;
	envelope->duty = value >> 6;
}

// Channel 1's frequency sweep register. Turning a downward sweep upward after it has stepped
// switches the channel off, as on the hardware; returns whether the channel still plays.
static bool writeSweep(struct PsgSquare* ch, uint8_t value) {
	ch->sweep.shift = value & 7;
	bool oldDirection = ch->sweep.direction;
	ch->sweep.direction = (value >> 3) & 1;
	bool on = true;
	if (ch->sweep.occurred && oldDirection && !ch->sweep.direction) {
		on = false;
	}
	ch->sweep.occurred = false;
	ch->sweep.time = (value >> 4) & 7;
	if (!ch->sweep.time) {
		ch->sweep.time = 8;
	}
	return on;
}

// A trigger restarts the sweep's timer.
static void resetSweep(struct PsgSquare* ch) {
	ch->sweep.step = ch->sweep.time;
	ch->sweep.enable = (ch->sweep.step != 8) || ch->sweep.shift;
	ch->sweep.occurred = false;
}

// Steps the sweep once: the frequency moves by itself shifted right. Going past the top switches
// the channel off; returns whether it did.
static bool updateSweep(struct PsgSquare* ch, bool initial) {
	if (initial || ch->sweep.time != 8) {
		int frequency = ch->sweep.realFrequency;
		if (ch->sweep.direction) {
			frequency -= frequency >> ch->sweep.shift;
			if (!initial && frequency >= 0) {
				ch->control.frequency = frequency;
				ch->sweep.realFrequency = frequency;
			}
		} else {
			frequency += frequency >> ch->sweep.shift;
			if (frequency < 2048) {
				if (!initial && ch->sweep.shift) {
					ch->control.frequency = frequency;
					ch->sweep.realFrequency = frequency;
					if (!updateSweep(ch, true)) {
						return false;
					}
				}
			} else {
				return false;
			}
		}
		ch->sweep.occurred = true;
	}
	ch->sweep.step = ch->sweep.time;
	return true;
}

// The frame sequencer's tick, 512 times a second; lengths, the sweep and envelopes step from it.
static void frameEvent(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	// Called before a sample interval that ends now.
	int32_t now = timingCurrentTime(gba) - late;
	audioCatchUp(gba, now, false);
	timerCatchUp(gba, now, false);
	audioSample(gba, now);
	timingSchedule(gba, &gba->audio.psg.frameEvent, TIMING_FACTOR * FRAME_CYCLES - late);
	psgUpdateFrame(gba, now);
}

void psgInit(struct GBA* gba) {
	gba->audio.psg.frameEvent.callback = frameEvent;
	gba->audio.psg.frameEvent.context = NULL;
	gba->audio.psg.frameEvent.priority = 0x10;
}

// Resets as after power-on: channels silent, their envelopes finished.
void psgReset(struct GBA* gba) {
	struct Psg* psg = &gba->audio.psg;
	struct Event frameEvent = psg->frameEvent;
	memset(psg, 0, sizeof(*psg));
	psg->frameEvent = frameEvent;
	psg->ch1.sweep.time = 8;
	psg->ch1.envelope.dead = 2;
	psg->ch2.envelope.dead = 2;
	psg->ch4.envelope.dead = 2;
}

// Brings a square channel up to timestamp: its duty pattern moves on one step each period.
static void runSquare(struct PsgSquare* ch, int32_t timestamp) {
	int period = 4 * (2048 - ch->control.frequency) * TIMING_FACTOR;
	int32_t diff = timestamp - ch->lastUpdate;
	if (diff >= period) {
		diff /= period;
		ch->index = (ch->index + diff) & 7;
		ch->lastUpdate += diff * period;
		updateSquareSample(ch);
	}
}

// Returns when a square channel's sample next changes as psgRun steps it: where its duty pattern
// next flips, if it's heard. A stopped channel still steps once it's a long way behind.
static uint32_t squareChange(const struct PsgSquare* ch, bool playing) {
	if (!playing || ch->envelope.dead == 2) {
		return (uint32_t) ch->lastUpdate + 0x40000001;
	}
	uint32_t period = 4 * (2048 - ch->control.frequency) * TIMING_FACTOR;
	if (!ch->envelope.currentVolume && !ch->sample) {
		// Silent until something changes the envelope, which runs the channel first.
		return (uint32_t) ch->lastUpdate + 0x40000001;
	}
	if (ch->sample != squareDuty[ch->envelope.duty][ch->index] * ch->envelope.currentVolume) {
		// A write changed the duty or volume; the next step picks it up.
		return (uint32_t) ch->lastUpdate + period;
	}
	return (uint32_t) ch->lastUpdate + dutySteps[ch->envelope.duty][ch->index] * period;
}

struct NoiseSteps {
	uint8_t state;
	uint8_t outputs;
	uint8_t positive;
};

// The 7-bit noise LFSR's state after eight steps from each state, with the bits it put out (oldest
// lowest).
static const struct NoiseSteps* noise7Steps(void) {
	static struct NoiseSteps table[128];
	static bool ready;
	if (!ready) {
		for (int start = 0; start < 128; ++start) {
			unsigned lfsr = start;
			struct NoiseSteps* steps = &table[start];
			for (int i = 0; i < 8; ++i) {
				int lsb = (lfsr ^ (lfsr >> 1) ^ 1) & 1;
				lfsr >>= 1;
				if (lsb) {
					lfsr |= 0x40;
				} else {
					lfsr &= ~0x40u;
				}
				steps->outputs |= lsb << i;
				steps->positive += lsb;
			}
			steps->state = lfsr & 0x7F;
		}
		ready = true;
	}
	return table;
}

// Brings the channels named in channels (one bit each) up to timestamp, so their samples are what a
// mix at that time hears. Channels nothing can hear are left behind until they matter.
void psgRun(struct GBA* gba, int32_t timestamp, int channels) {
	struct Psg* psg = &gba->audio.psg;
	psg->cached = false;
	if (!psg->enable) {
		psg->nextChange = (uint32_t) timestamp + INT32_MAX;
		return;
	}
	if ((channels & 0x1) && ((psg->playingCh1 && psg->ch1.envelope.dead != 2) || timestamp - psg->ch1.lastUpdate > 0x40000000 || channels == 0x1)) {
		runSquare(&psg->ch1, timestamp);
	}
	if ((channels & 0x2) && ((psg->playingCh2 && psg->ch2.envelope.dead != 2) || timestamp - psg->ch2.lastUpdate > 0x40000000 || channels == 0x2)) {
		runSquare(&psg->ch2, timestamp);
	}
	if (psg->playingCh3 && (channels & 0x4)) {
		struct PsgWave* ch3 = &psg->ch3;
		int cycles = 2 * (2048 - ch3->rate) * TIMING_FACTOR;
		int32_t diff = timestamp - ch3->nextUpdate;
		if (diff >= 0) {
			diff = (diff / cycles) + 1;
			int volume;
			switch (ch3->volume) {
			case 0:
				volume = 4;
				break;
			case 1:
				volume = 0;
				break;
			case 2:
				volume = 1;
				break;
			default:
				volume = 2;
				break;
			}
			int start = 7;
			int end = 0;
			int mask = 0x1F;
			if (ch3->size) {
				mask = 0x3F;
			} else if (ch3->bank) {
				end = 4;
			} else {
				start = 3;
			}
			// Rotates the 4-bit samples through the active bank.
			for (int iter = 0; iter < (diff & mask); ++iter) {
				uint32_t bitsCarry = ch3->wavedata32[end] & 0x000000F0;
				uint32_t bits;
				for (int i = start; i >= end; --i) {
					bits = ch3->wavedata32[i] & 0x000000F0;
					ch3->wavedata32[i] = ((ch3->wavedata32[i] & 0x0F0F0F0F) << 4) | ((ch3->wavedata32[i] & 0xF0F0F000) >> 12);
					ch3->wavedata32[i] |= bitsCarry << 20;
					bitsCarry = bits;
				}
				ch3->sample = bitsCarry >> 4;
			}
			if (ch3->volume > 3) {
				ch3->sample += ch3->sample << 1;
			}
			ch3->sample >>= volume;
			ch3->nextUpdate += diff * cycles;
			ch3->readable = true;
		}
	}
	// Silent noise only steps when a write to it is about to change something, all at once. Until
	// then nothing can hear the LFSR, and a trigger restarts it from zero.
	struct PsgNoise* noise = &psg->ch4;
	bool silent = noise->envelope.dead == 2 && !noise->envelope.currentVolume && !noise->sample;
	if (psg->playingCh4 && (channels & 0x8) && (channels == 0x8 || !silent)) {
		static const uint16_t noiseMaskTable[0x40] = {
			0x3f, 0x3e, 0x3c, 0x3d, 0x39, 0x38, 0x3a, 0x3b, 0x33, 0x32, 0x30, 0x31, 0x35, 0x34, 0x36, 0x37,
			0x27, 0x26, 0x24, 0x25, 0x21, 0x20, 0x22, 0x23, 0x2b, 0x2a, 0x28, 0x29, 0x2d, 0x2c, 0x2e, 0x2f,
			0x0f, 0x0e, 0x0c, 0x0d, 0x09, 0x08, 0x0a, 0x0b, 0x03, 0x02, 0x00, 0x01, 0x05, 0x04, 0x06, 0x07,
			0x17, 0x16, 0x14, 0x15, 0x11, 0x10, 0x12, 0x13, 0x1b, 0x1a, 0x18, 0x19, 0x1d, 0x1c, 0x1e, 0x1f,
		};
		static const uint16_t noisePopulationTable[0x40] = {
			6, 5, 4, 5, 4, 3, 4, 5, 4, 3, 2, 3, 4, 3, 4, 5, 4, 3, 2, 3, 2, 1, 2, 3, 4, 3, 2, 3, 4, 3, 4, 5,
			4, 3, 2, 3, 2, 1, 2, 3, 2, 1, 0, 1, 2, 1, 2, 3, 4, 3, 2, 3, 2, 1, 2, 3, 4, 3, 2, 3, 4, 3, 4, 5,
		};
		struct PsgNoise* ch4 = &psg->ch4;
		int32_t cycles = ch4->ratio ? 2 * ch4->ratio : 1;
		cycles <<= ch4->frequency;
		cycles *= 8 * TIMING_FACTOR;
		int32_t diff = timestamp - ch4->lastEvent;
		if (diff >= cycles) {
			int32_t last = 0;
			int samples = 0;
			int positiveSamples = 0;
			int lsb = 0;
			int coeff;
			if (ch4->power) {
				coeff = 0x4040;
				// Eight steps at a time: the low 7 bits are a 7-bit LFSR, the rest the last
				// outputs.
				for (; last + cycles * 8 <= diff; last += cycles * 8) {
					const struct NoiseSteps* steps = &noise7Steps()[ch4->lfsr & 0x7F];
					ch4->lfsr = (steps->outputs << 7) | steps->state;
					samples += 8;
					positiveSamples += steps->positive;
					lsb = steps->outputs >> 7;
				}
			} else {
				int bits = 0;
				for (; last + cycles * 5 <= diff; last += cycles * 5) {
					bits = ch4->lfsr & 0x3F;
					ch4->lfsr >>= 5;
					ch4->lfsr |= 0x4000 * noiseMaskTable[bits] >> 4;
					ch4->lfsr &= 0x7FFF;
					samples += 5;
					positiveSamples += noisePopulationTable[bits];
				}
				lsb = noiseMaskTable[bits] & 1;
				coeff = 0x4000;
			}
			for (; last + cycles <= diff; last += cycles) {
				lsb = (ch4->lfsr ^ (ch4->lfsr >> 1) ^ 1) & 1;
				ch4->lfsr >>= 1;
				if (lsb) {
					ch4->lfsr |= coeff;
				} else {
					ch4->lfsr &= ~coeff;
				}
				++samples;
				positiveSamples += lsb;
			}
			ch4->sample = lsb * ch4->envelope.currentVolume;
			ch4->nSamples += samples;
			ch4->samples += positiveSamples * ch4->envelope.currentVolume;
			ch4->lastEvent += last;
		}
	}
	// Returns the first time a sample's run would change what psgSample gives.
	int32_t soonest = INT32_MAX;
	uint32_t when = squareChange(&psg->ch1, psg->playingCh1);
	if ((int32_t) (when - timestamp) < soonest) {
		soonest = when - timestamp;
	}
	when = squareChange(&psg->ch2, psg->playingCh2);
	if ((int32_t) (when - timestamp) < soonest) {
		soonest = when - timestamp;
	}
	if (psg->playingCh3 && (int32_t) ((uint32_t) psg->ch3.nextUpdate - timestamp) < soonest) {
		soonest = (uint32_t) psg->ch3.nextUpdate - timestamp;
	}
	silent = noise->envelope.dead == 2 && !noise->envelope.currentVolume && !noise->sample;
	if (psg->playingCh4 && !silent) {
		int32_t cycles = noise->ratio ? 2 * noise->ratio : 1;
		cycles <<= noise->frequency;
		cycles *= 8 * TIMING_FACTOR;
		when = (uint32_t) noise->lastEvent + cycles;
		if ((int32_t) (when - timestamp) < soonest) {
			soonest = when - timestamp;
		}
	}
	psg->nextChange = (uint32_t) timestamp + soonest;
}

// Runs one tick of the frame sequencer: lengths every other tick, the sweep every fourth, envelopes
// every eighth.
void psgUpdateFrame(struct GBA* gba, int32_t now) {
	struct Psg* psg = &gba->audio.psg;
	if (!psg->enable) {
		return;
	}
	if (psg->skipFrame) {
		psg->skipFrame = false;
		return;
	}
	psgRun(gba, now, 0x7);
	int frame = (psg->frame + 1) & 7;
	psg->frame = frame;

	switch (frame) {
	case 2:
	case 6:
		if (psg->ch1.sweep.enable) {
			--psg->ch1.sweep.step;
			if (psg->ch1.sweep.step == 0) {
				if (!updateSweep(&psg->ch1, false)) {
					psg->playingCh1 = false;
				}
				*nr52(gba) &= ~0x0001;
				*nr52(gba) |= psg->playingCh1;
			}
		}
		// Fall through.
	case 0:
	case 4:
		if (psg->ch1.control.length && psg->ch1.control.stop) {
			--psg->ch1.control.length;
			if (psg->ch1.control.length == 0) {
				psg->playingCh1 = false;
				*nr52(gba) &= ~0x0001;
			}
		}
		if (psg->ch2.control.length && psg->ch2.control.stop) {
			--psg->ch2.control.length;
			if (psg->ch2.control.length == 0) {
				psg->playingCh2 = false;
				*nr52(gba) &= ~0x0002;
			}
		}
		if (psg->ch3.length && psg->ch3.stop) {
			--psg->ch3.length;
			if (psg->ch3.length == 0) {
				psg->playingCh3 = false;
				*nr52(gba) &= ~0x0004;
			}
		}
		if (psg->ch4.length && psg->ch4.stop) {
			--psg->ch4.length;
			if (psg->ch4.length == 0) {
				psg->playingCh4 = false;
				*nr52(gba) &= ~0x0008;
			}
		}
		break;
	case 7:
		if (psg->playingCh1 && !psg->ch1.envelope.dead) {
			--psg->ch1.envelope.nextStep;
			if (psg->ch1.envelope.nextStep == 0) {
				updateEnvelope(&psg->ch1.envelope);
				updateSquareSample(&psg->ch1);
			}
		}
		if (psg->playingCh2 && !psg->ch2.envelope.dead) {
			--psg->ch2.envelope.nextStep;
			if (psg->ch2.envelope.nextStep == 0) {
				updateEnvelope(&psg->ch2.envelope);
				updateSquareSample(&psg->ch2);
			}
		}
		if (psg->playingCh4 && !psg->ch4.envelope.dead) {
			--psg->ch4.envelope.nextStep;
			if (psg->ch4.envelope.nextStep == 0) {
				int8_t sample = psg->ch4.sample;
				updateEnvelope(&psg->ch4.envelope);
				psg->ch4.sample = (sample > 0) * psg->ch4.envelope.currentVolume;
				if (psg->ch4.nSamples) {
					psg->ch4.samples -= sample;
					psg->ch4.samples += psg->ch4.sample;
				}
			}
		}
		break;
	}
}

// Mixes the four channels to left and right as the panning register says, then applies the master
// volume.
void psgSample(struct GBA* gba, int16_t* left, int16_t* right) {
	struct Psg* psg = &gba->audio.psg;
	int sampleLeft = 0;
	int sampleRight = 0;
	if (psg->ch1Left) {
		sampleLeft += psg->ch1.sample;
	}
	if (psg->ch1Right) {
		sampleRight += psg->ch1.sample;
	}
	if (psg->ch2Left) {
		sampleLeft += psg->ch2.sample;
	}
	if (psg->ch2Right) {
		sampleRight += psg->ch2.sample;
	}
	if (psg->ch3Left) {
		sampleLeft += psg->ch3.sample;
	}
	if (psg->ch3Right) {
		sampleRight += psg->ch3.sample;
	}
	sampleLeft <<= 3;
	sampleRight <<= 3;
	int16_t sample = psg->ch4.sample << 3;
	if (psg->ch4Left) {
		sampleLeft += sample;
	}
	if (psg->ch4Right) {
		sampleRight += sample;
	}
	*left = sampleLeft * (1 + psg->volumeLeft);
	*right = sampleRight * (1 + psg->volumeRight);
}

// A square channel's frequency high bits, length enable and trigger. The length counter's extra
// clock when enabled at certain frame steps is the hardware's behaviour.
static void writeControlSquare(struct GBA* gba, struct PsgSquare* ch, bool* playing, int channel, uint8_t value, bool sweep) {
	struct Psg* psg = &gba->audio.psg;
	ch->control.frequency &= 0xFF;
	ch->control.frequency |= (value & 7) << 8;
	bool wasStop = ch->control.stop;
	ch->control.stop = (value >> 6) & 1;
	if (!wasStop && ch->control.stop && ch->control.length && !(psg->frame & 1)) {
		--ch->control.length;
		if (ch->control.length == 0) {
			*playing = false;
		}
	}
	if (value & 0x80) {
		*playing = resetEnvelope(&ch->envelope);
		if (sweep) {
			ch->sweep.realFrequency = ch->control.frequency;
			resetSweep(ch);
			if (*playing && ch->sweep.shift) {
				*playing = updateSweep(ch, true);
			}
		}
		if (!ch->control.length) {
			ch->control.length = 64;
			if (ch->control.stop && !(psg->frame & 1)) {
				--ch->control.length;
			}
		}
		updateSquareSample(ch);
	}
	*nr52(gba) &= ~(1 << channel);
	*nr52(gba) |= *playing << channel;
}

// Handles register writes by Game Boy register number (10 = NR10, ...).
void psgWrite(struct GBA* gba, int nr, uint8_t value) {
	struct Psg* psg = &gba->audio.psg;
	int32_t now = timingCurrentTime(gba);
	audioCatchUp(gba, now, true);
	switch (nr) {
	// Channel 1: sweep, duty and length, envelope, frequency, and control.
	case 10:
		psgRun(gba, now, 0x1);
		if (!writeSweep(&psg->ch1, value)) {
			psg->playingCh1 = false;
			*nr52(gba) &= ~0x0001;
		}
		break;
	case 11:
		psgRun(gba, now, 0x1);
		writeDuty(&psg->ch1.envelope, value);
		psg->ch1.control.length = 64 - psg->ch1.envelope.length;
		break;
	case 12:
		psgRun(gba, now, 0x1);
		if (!writeEnvelope(&psg->ch1.envelope, value)) {
			psg->playingCh1 = false;
			*nr52(gba) &= ~0x0001;
		}
		break;
	case 13:
		psgRun(gba, now, 0x1);
		psg->ch1.control.frequency &= 0x700;
		psg->ch1.control.frequency |= value;
		break;
	case 14:
		psgRun(gba, now, 0x1);
		writeControlSquare(gba, &psg->ch1, &psg->playingCh1, 0, value, true);
		break;
	// Channel 2, a square wave with no sweep.
	case 21:
		psgRun(gba, now, 0x2);
		writeDuty(&psg->ch2.envelope, value);
		psg->ch2.control.length = 64 - psg->ch2.envelope.length;
		break;
	case 22:
		psgRun(gba, now, 0x2);
		if (!writeEnvelope(&psg->ch2.envelope, value)) {
			psg->playingCh2 = false;
			*nr52(gba) &= ~0x0002;
		}
		break;
	case 23:
		psgRun(gba, now, 0x2);
		psg->ch2.control.frequency &= 0x700;
		psg->ch2.control.frequency |= value;
		break;
	case 24:
		psgRun(gba, now, 0x2);
		writeControlSquare(gba, &psg->ch2, &psg->playingCh2, 1, value, false);
		break;
	// Channel 3 plays the wave RAM: on/off and bank, length, volume, frequency, control.
	case 30:
		psgRun(gba, now, 0x4);
		psg->ch3.enable = value >> 7;
		if (!psg->ch3.enable) {
			psg->playingCh3 = false;
			*nr52(gba) &= ~0x0004;
		}
		break;
	case 31:
		psgRun(gba, now, 0x4);
		psg->ch3.length = 256 - value;
		break;
	case 32: {
		psgRun(gba, now, 0x4);
		psg->ch3.volume = (value >> 5) & 3;
		psg->ch3.sample = psg->ch3.wavedata8[psg->ch3.window >> 1];
		if (!(psg->ch3.window & 1)) {
			psg->ch3.sample >>= 4;
		}
		psg->ch3.sample &= 0xF;
		static const int shifts[4] = { 4, 0, 1, 2 };
		psg->ch3.sample >>= shifts[psg->ch3.volume];
		break;
	}
	case 33:
		psgRun(gba, now, 0x4);
		psg->ch3.rate &= 0x700;
		psg->ch3.rate |= value;
		break;
	case 34: {
		psgRun(gba, now, 0x4);
		psg->ch3.rate &= 0xFF;
		psg->ch3.rate |= (value & 7) << 8;
		bool wasStop = psg->ch3.stop;
		psg->ch3.stop = (value >> 6) & 1;
		if (!wasStop && psg->ch3.stop && psg->ch3.length && !(psg->frame & 1)) {
			--psg->ch3.length;
			if (psg->ch3.length == 0) {
				psg->playingCh3 = false;
			}
		}
		if (value & 0x80) {
			psg->playingCh3 = psg->ch3.enable;
			if (!psg->ch3.length) {
				psg->ch3.length = 256;
				if (psg->ch3.stop && !(psg->frame & 1)) {
					--psg->ch3.length;
				}
			}
			psg->ch3.window = 0;
		}
		if (psg->playingCh3) {
			psg->ch3.readable = true;
			psg->ch3.nextUpdate = now + (6 + 2 * (2048 - psg->ch3.rate)) * TIMING_FACTOR;
		}
		*nr52(gba) &= ~0x0004;
		*nr52(gba) |= psg->playingCh3 << 2;
		break;
	}
	// Channel 4, noise: length, envelope, the shift register's clock and width, control.
	case 41:
		psgRun(gba, now, 0x8);
		writeDuty(&psg->ch4.envelope, value);
		psg->ch4.length = 64 - psg->ch4.envelope.length;
		break;
	case 42:
		psgRun(gba, now, 0x8);
		if (!writeEnvelope(&psg->ch4.envelope, value)) {
			psg->playingCh4 = false;
			*nr52(gba) &= ~0x0008;
		}
		break;
	case 43:
		psgRun(gba, now, 0x8);
		psg->ch4.ratio = value & 7;
		psg->ch4.frequency = value >> 4;
		psg->ch4.power = (value >> 3) & 1;
		break;
	case 44: {
		psgRun(gba, now, 0x8);
		bool wasStop = psg->ch4.stop;
		psg->ch4.stop = (value >> 6) & 1;
		if (!wasStop && psg->ch4.stop && psg->ch4.length && !(psg->frame & 1)) {
			--psg->ch4.length;
			if (psg->ch4.length == 0) {
				psg->playingCh4 = false;
			}
		}
		if (value & 0x80) {
			psg->playingCh4 = resetEnvelope(&psg->ch4.envelope);
			psg->ch4.lfsr = 0;
			if (!psg->ch4.length) {
				psg->ch4.length = 64;
				if (psg->ch4.stop && !(psg->frame & 1)) {
					--psg->ch4.length;
				}
			}
			if (psg->playingCh4) {
				psg->ch4.lastEvent = now;
			}
		}
		*nr52(gba) &= ~0x0008;
		*nr52(gba) |= psg->playingCh4 << 3;
		break;
	}
	// The master volume and each channel's left and right switches.
	case 50:
		psgRun(gba, now, 0xF);
		psg->volumeRight = value & 7;
		psg->volumeLeft = (value >> 4) & 7;
		break;
	case 51:
		psgRun(gba, now, 0xF);
		psg->ch1Right = value & 0x01;
		psg->ch2Right = value & 0x02;
		psg->ch3Right = value & 0x04;
		psg->ch4Right = value & 0x08;
		psg->ch1Left = value & 0x10;
		psg->ch2Left = value & 0x20;
		psg->ch3Left = value & 0x40;
		psg->ch4Left = value & 0x80;
		break;
	}
}

// The master switch. When off, every channel stops and the other sound registers are cleared.
void psgWriteNr52(struct GBA* gba, uint8_t value) {
	struct Psg* psg = &gba->audio.psg;
	audioCatchUp(gba, timingCurrentTime(gba), true);
	bool wasEnable = psg->enable;
	psg->enable = value >> 7;
	if (!psg->enable) {
		psg->playingCh1 = false;
		psg->playingCh2 = false;
		psg->playingCh3 = false;
		psg->playingCh4 = false;
		static const int cleared[] = { 10, 12, 13, 14, 22, 23, 24, 30, 32, 33, 34, 42, 43, 44, 50, 51, 11, 21, 31, 41 };
		for (size_t i = 0; i < sizeof(cleared) / sizeof(*cleared); ++i) {
			psgWrite(gba, cleared[i], 0);
		}
		*nr52(gba) &= ~0x000F;
	} else if (!wasEnable) {
		psg->skipFrame = false;
		psg->frame = 7;
	}
}
