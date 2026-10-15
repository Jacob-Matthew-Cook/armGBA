/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define _GNU_SOURCE
// Times one emulator on a game with scripted input.
// usage: bench armgba|mgba|ref bios.bin rom.gba frames [skip-frames]   (ref: the exact CPU alone)

#include "game.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>

// A sampling profiler: records where the program counter is every 100 microseconds.
enum {
	MAX_SAMPLES = 1 << 20,
};
static uintptr_t samples[MAX_SAMPLES];
static volatile size_t sampleCount;

static void onSample(int sig, siginfo_t* info, void* context) {
	ucontext_t* uc = context;
	if (sampleCount < MAX_SAMPLES) {
#if defined(__x86_64__)
		samples[sampleCount++] = uc->uc_mcontext.gregs[REG_RIP];
#else
		samples[sampleCount++] = uc->uc_mcontext.pc;
#endif
	}
}

static void startProfiling(void) {
	struct sigaction sa = { 0 };
	sa.sa_sigaction = onSample;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval timer = { { 0, 100 }, { 0, 100 } };
	setitimer(ITIMER_PROF, &timer, NULL);
}

static void stopProfiling(void) {
	struct itimerval timer = { 0 };
	setitimer(ITIMER_PROF, &timer, NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s.maps", getenv("PROFILE"));
	FILE* maps = fopen("/proc/self/maps", "r");
	FILE* copy = fopen(path, "w");
	char line[512];
	while (fgets(line, sizeof(line), maps)) {
		fputs(line, copy);
	}
	fclose(maps);
	fclose(copy);
	FILE* out = fopen(getenv("PROFILE"), "w");
	for (size_t i = 0; i < sampleCount; ++i) {
		fprintf(out, "%lx\n", (unsigned long) samples[i]);
	}
	fclose(out);
}

static uint16_t keysFor(int frame) {
	uint32_t h = (frame / 6 + 7) * 0x9E3779B1u;
	h ^= h >> 15;
	h *= 0x85EBCA77u;
	h ^= h >> 13;
	uint16_t keys = 0;
	if ((h & 0xF) < 3) {
		keys |= 1 << 0;
	}
	if (((h >> 4) & 0x3F) == 0) {
		keys |= 1 << 3;
	}
	if (((h >> 10) & 0x7) == 0) {
		keys |= 1 << 1;
	}
	static const uint16_t dirs[8] = { 0, 0x10, 0x20, 0x40, 0x80, 0x50, 0xA0, 0 };
	keys |= dirs[(h >> 13) & 7];
	return keys;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s armgba|mgba|ref bios.bin rom.gba frames [skip-frames]\n", argv[0]);
		return 1;
	}
	const struct GameEmu* emu = !strcmp(argv[1], "mgba") ? &mgbaGame : !strcmp(argv[1], "ref") ? &refGame : &armgbaGame;
	FILE* f = fopen(argv[3], "rb");
	if (!f) {
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* rom = malloc(size);
	if (fread(rom, 1, size, f) != size) {
		return 1;
	}
	fclose(f);
	int frames = atoi(argv[4]);
	int skip = argc > 5 ? atoi(argv[5]) : 0;
	if (!emu->load(argv[2], rom, size, true)) {
		return 1;
	}
	for (int i = 0; i < skip; ++i) {
		emu->setKeys(keysFor(i));
		emu->runFrame();
	}
	struct timespec t0, t1;
	if (getenv("PROFILE")) {
		startProfiling();
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = skip; i < skip + frames; ++i) {
		emu->setKeys(keysFor(i));
		emu->runFrame();
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (getenv("PROFILE")) {
		stopProfiling();
	}
	if (getenv("BENCH_HASH")) {
		// Hashes the state at the end, to tell apart runs that went different ways.
		static struct Snapshot s;
		emu->snapshot(&s);
		uint32_t h = 2166136261u;
		const uint8_t* bytes = (const uint8_t*) &s;
		for (size_t i = 0; i < sizeof(s); ++i) {
			h = (h ^ bytes[i]) * 16777619u;
		}
		printf("state hash %08X pc %08X audio %08X\n", h, s.r[15], emu->audioHash ? emu->audioHash() : 0);
		if (getenv("BENCH_IWRAM")) {
			FILE* f = fopen(getenv("BENCH_IWRAM"), "wb");
			fwrite(s.iwram, 1, sizeof(s.iwram), f);
			fclose(f);
		}
	}
	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("%s: %d frames in %.2fs = %.0f fps\n", emu->name, frames, seconds, frames / seconds);
	return 0;
}
