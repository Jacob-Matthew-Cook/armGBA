/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// Direct sound FIFOs and the mixer. Timing follows mGBA, since FIFO refills are DMA transfers that
// take CPU time.

enum {
	SAMPLE_INTERVAL = CPU_FREQUENCY / 0x4000,
	MASTER_VOLUME = 0x100,
};

// SOUNDBIAS: the level the output swings around, and the PWM output's bit depth and rate.
static int biasLevel(uint16_t soundbias) {
	return soundbias & 0x3FE;
}

static int resolution(uint16_t soundbias) {
	return (soundbias >> 14) & 3;
}

void audioInit(struct GBA* gba) {
	psgInit(gba);
}

// Resets the sound: off, FIFOs empty, the default bias, and the frame sequencer started.
void audioReset(struct GBA* gba) {
	struct Audio* audio = &gba->audio;
	psgReset(gba);
	timingDeschedule(gba, &audio->psg.frameEvent);
	timingSchedule(gba, &audio->psg.frameEvent, 0);
	audio->nextSample = timingCurrentTime(gba);
	memset(&audio->chA, 0, sizeof(audio->chA));
	memset(&audio->chB, 0, sizeof(audio->chB));
	audio->chA.dmaSource = 1;
	audio->chB.dmaSource = 2;
	audio->soundbias = 0x200;
	audio->volume = 0;
	audio->volumeChA = false;
	audio->volumeChB = false;
	audio->lastSample = 0;
	audio->sampleIndex = 0;
	audio->chARight = false;
	audio->chALeft = false;
	audio->chATimer = false;
	audio->chBRight = false;
	audio->chBLeft = false;
	audio->chBTimer = false;
	audio->enable = false;
	audio->sampleInterval = CPU_FREQUENCY / 0x8000;
	audio->outCount = 0;
}

// The FIFO channels' volume, panning, timer and reset bits, and the tone channels' share of the
// mix.
void audioWriteSoundcntHi(struct GBA* gba, uint16_t value) {
	struct Audio* audio = &gba->audio;
	audioSync(gba);
	timerReplan(gba, timingCurrentTime(gba));
	audio->volume = value & 3;
	audio->volumeChA = (value >> 2) & 1;
	audio->volumeChB = (value >> 3) & 1;
	audio->chARight = (value >> 8) & 1;
	audio->chALeft = (value >> 9) & 1;
	audio->chATimer = (value >> 10) & 1;
	audio->chBRight = (value >> 12) & 1;
	audio->chBLeft = (value >> 13) & 1;
	audio->chBTimer = (value >> 14) & 1;
	if (value & 0x0800) {
		audio->chA.fifoWrite = 0;
		audio->chA.fifoRead = 0;
	}
	if (value & 0x8000) {
		audio->chB.fifoWrite = 0;
		audio->chB.fifoRead = 0;
	}
}

// The master switch.
void audioWriteSoundcntX(struct GBA* gba, uint16_t value) {
	struct Audio* audio = &gba->audio;
	audioSync(gba);
	timerReplan(gba, timingCurrentTime(gba));
	audio->enable = (value >> 7) & 1;
	psgWriteNr52(gba, value);
	if (!audio->enable) {
		for (int i = REG_SOUND1CNT_LO; i < REG_SOUNDCNT_HI; i += 2) {
			gba->io[IO(i)] = 0;
		}
		audio->psg.ch3.size = 0;
		audio->psg.ch3.bank = 0;
		audio->psg.ch3.volume = 0;
		audio->volume = 0;
		audio->volumeChA = 0;
		audio->volumeChB = 0;
		gba->io[IO(REG_SOUNDCNT_HI)] &= 0xFF00;
	}
}

// Sets the bias, which also sets the rate samples are made at.
void audioWriteSoundbias(struct GBA* gba, uint16_t value) {
	struct Audio* audio = &gba->audio;
	int32_t timestamp = timingCurrentTime(gba);
	audioSync(gba);
	audio->soundbias = value;
	int32_t oldSampleInterval = audio->sampleInterval;
	audio->sampleInterval = 0x200 >> resolution(value);
	if (oldSampleInterval != audio->sampleInterval) {
		timestamp -= audio->lastSample;
		audio->sampleIndex = timestamp >> (9 - resolution(value));
		if (audio->sampleIndex >= AUDIO_MAX_SAMPLES) {
			audio->sampleIndex = 0;
		}
	}
}

// Channel 3's wave RAM is two banks: a game reads and writes the one that isn't playing.
static int waveBank(struct GBA* gba) {
	// With the hardware off, bank 0 counts as selected, so access goes to bank 1.
	return gba->audio.enable ? !gba->audio.psg.ch3.bank : 1;
}

// Channel 3 is brought up to date first, since what it plays is about to change.
void audioWriteWaveRam(struct GBA* gba, int address, uint32_t value) {
	audioSync(gba);
	int bank = waveBank(gba);
	psgRun(gba, timingCurrentTime(gba), 0x4);
	gba->audio.psg.ch3.wavedata32[address | (bank * 4)] = value;
}

uint32_t audioReadWaveRam(struct GBA* gba, int address) {
	audioSync(gba);
	int bank = waveBank(gba);
	psgRun(gba, timingCurrentTime(gba), 0x4);
	return gba->audio.psg.ch3.wavedata32[address | (bank * 4)];
}

// Writes four bytes into a FIFO, which is a ring of words. The timers' quiet overflows were planned
// around its old fill level, so they are planned again.
uint32_t audioWriteFifo(struct GBA* gba, uint32_t address, uint32_t value) {
	struct Fifo* channel;
	switch (address) {
	case REG_FIFO_A_LO:
		channel = &gba->audio.chA;
		break;
	case REG_FIFO_B_LO:
		channel = &gba->audio.chB;
		break;
	default:
		return value;
	}
	timerReplan(gba, timingCurrentTime(gba));
	channel->fifo[channel->fifoWrite] = value;
	++channel->fifoWrite;
	if (channel->fifoWrite == AUDIO_FIFO_SIZE) {
		channel->fifoWrite = 0;
	}
	return channel->fifo[channel->fifoWrite];
}

// A timer overflow clocks a FIFO: with room for four words, it asks its DMA channel for more, and
// its next byte becomes the sample. The byte holds from this overflow to the end of the output
// sample under way, which the loop over samples[] fills in.
void audioSampleFifo(struct GBA* gba, int fifoId, uint32_t time) {
	struct Audio* audio = &gba->audio;
	audioCatchUp(gba, time, true);
	struct Fifo* channel = fifoId == 0 ? &audio->chA : &audio->chB;
	int fifoSize;
	if (channel->fifoWrite >= channel->fifoRead) {
		fifoSize = channel->fifoWrite - channel->fifoRead;
	} else {
		fifoSize = AUDIO_FIFO_SIZE - channel->fifoRead + channel->fifoWrite;
	}
	if (AUDIO_FIFO_SIZE - fifoSize > 4 && channel->dmaSource > 0) {
		dmaRequestFifo(gba, channel->dmaSource, timingCurrentTime(gba) - time);
	}
	if (!channel->internalRemaining && fifoSize) {
		channel->internalSample = channel->fifo[channel->fifoRead];
		channel->internalRemaining = 4;
		++channel->fifoRead;
		if (channel->fifoRead == AUDIO_FIFO_SIZE) {
			channel->fifoRead = 0;
		}
	}
	int res = resolution(audio->soundbias);
	int32_t until = audio->nextSample - time - 1;
	int bits = 2 << res;
	until += 1 << (9 - res);
	until >>= 9 - res;
	if (bits < until) {
		until = bits;
	}
	for (int i = bits - until; i < bits; ++i) {
		channel->samples[i] = channel->internalSample;
	}
	if (channel->internalRemaining) {
		channel->internalSample >>= 8;
		--channel->internalRemaining;
	}
}

// Returns the mixed sample with the bias added, clipped to the 10-bit output's range.
static inline int16_t applyBias(int bias, int sample) {
	sample += bias;
	if (sample >= 0x400) {
		sample = 0x3FF;
	} else if (sample < 0) {
		sample = 0;
	}
	return ((sample - bias) * MASTER_VOLUME * 3) >> 4;
}

// Makes one output sample: the tone channels and the two FIFOs mixed, biased, and written to the
// buffer the frontend takes each frame. The settings hold for the whole call, so they are read
// once; otherwise the tone channels' catching up, which is a call, would make them be read again
// for every sample.
void audioSample(struct GBA* gba, int32_t timestamp) {
	struct Audio* audio = &gba->audio;
	timestamp -= audio->lastSample;
	timestamp -= audio->sampleIndex * audio->sampleInterval;
	int maxSample = 2 << resolution(audio->soundbias);
	struct Psg* psg = &audio->psg;
	int psgShift = 4 - audio->volume;
	int bias = biasLevel(audio->soundbias);
	int32_t interval = audio->sampleInterval;
	int32_t lastSample = audio->lastSample;
	int shiftA = !audio->volumeChA;
	int shiftB = !audio->volumeChB;
	bool leftA = audio->chALeft;
	bool rightA = audio->chARight;
	bool leftB = audio->chBLeft;
	bool rightB = audio->chBRight;
	const int8_t* samplesA = audio->chA.samples;
	const int8_t* samplesB = audio->chB.samples;
	struct StereoSample* out = audio->currentSamples;
	int sample;
	for (sample = audio->sampleIndex; timestamp >= interval && sample < maxSample; ++sample, timestamp -= interval) {
		int32_t when = sample * interval + lastSample;
		if (!psg->cached || (int32_t) ((uint32_t) when - psg->nextChange) >= 0) {
			psgRun(gba, when, 0xF);
			psgSample(gba, &psg->cachedLeft, &psg->cachedRight);
			psg->cached = true;
		}
		int16_t sampleLeft = psg->cachedLeft >> psgShift;
		int16_t sampleRight = psg->cachedRight >> psgShift;
		int a = (samplesA[sample] << 2) >> shiftA;
		int b = (samplesB[sample] << 2) >> shiftB;
		if (leftA) {
			sampleLeft += a;
		}
		if (rightA) {
			sampleRight += a;
		}
		if (leftB) {
			sampleLeft += b;
		}
		if (rightB) {
			sampleRight += b;
		}
		out[sample].left = applyBias(bias, sampleLeft);
		out[sample].right = applyBias(bias, sampleRight);
	}
	audio->sampleIndex = sample;
	if (sample == maxSample) {
		audio->lastSample += SAMPLE_INTERVAL;
		audio->sampleIndex = 0;
	}
}

// The end of a sample interval.
static void finishInterval(struct GBA* gba, int32_t time) {
	struct Audio* audio = &gba->audio;
	audioSample(gba, time);
	int samples = 2 << resolution(audio->soundbias);
	memset(audio->chA.samples, audio->chA.samples[samples - 1], sizeof(audio->chA.samples));
	memset(audio->chB.samples, audio->chB.samples[samples - 1], sizeof(audio->chB.samples));
	// The frontend gets 32768 Hz at any resolution: two samples per interval, each the mean of its
	// half.
	int half = samples / 2;
	for (int h = 0; h < 2 && audio->outCount < sizeof(audio->out) / sizeof(*audio->out); ++h) {
		int left = 0;
		int right = 0;
		for (int i = h * half; i < (h + 1) * half; ++i) {
			left += audio->currentSamples[i].left;
			right += audio->currentSamples[i].right;
		}
		audio->out[audio->outCount++] = (struct StereoSample) { left / half, right / half };
	}
}

// The CPU can't see the mixer, so intervals are finished in batches rather than as events.
void audioCatchUp(struct GBA* gba, int32_t time, bool inclusive) {
	struct Audio* audio = &gba->audio;
	while ((int32_t) (audio->nextSample - time) < (inclusive ? 1 : 0)) {
		// The FIFOs' samples from before the interval ends go in it.
		timerCatchUp(gba, audio->nextSample, false);
		finishInterval(gba, audio->nextSample);
		audio->nextSample += SAMPLE_INTERVAL;
	}
}

// Called before a change to what the mixer reads: the sound up to now goes out as it was, with the
// FIFOs' samples due by now in it.
void audioSync(struct GBA* gba) {
	int32_t now = timingCurrentTime(gba);
	audioCatchUp(gba, now, true);
	timerCatchUp(gba, now, true);
	audioSample(gba, now);
	// What comes next changes it.
	gba->audio.psg.cached = false;
}
