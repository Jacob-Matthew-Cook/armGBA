/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The renderer on a second thread. The emulation thread keeps the renderer's registers as before
// but draws nothing: it logs each call to the renderer, and with each line, the VRAM pages, OAM and
// palette changed since the last one. The worker makes the same calls in the same order on a copy
// of the renderer and of that memory, so it draws what one thread would have. The frame is waited
// for where it ends; or, when started late, the frame before it is handed over there instead, so
// drawing a frame overlaps the next one's emulation, at the cost of a frame's delay.

#include "gba.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	LOG_SIZE = 8 << 20,
	// The most one entry takes: a line with all of VRAM, OAM and the palette.
	LOG_MOST = 16 + SIZE_VRAM + SIZE_OAM + SIZE_PALETTE,
	PAGE_SIZE = 0x800,
	PAGES = SIZE_VRAM / PAGE_SIZE,
};

enum {
	LOG_REGISTER,
	// The rest of the log up to its end is unused, since entries don't wrap around.
	LOG_SKIP,
	LOG_LINE,
	LOG_START_LINE,
	LOG_LATCH,
	LOG_FINISH,
};

struct RenderThread {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	pthread_cond_t idle;
	// The copy the worker draws from; only its renderer, VRAM, OAM and palette are used.
	struct GBA* copy;
	// A ring: positions only grow, and an entry is at its position modulo LOG_SIZE.
	uint8_t* log;
	// How much is logged, and how much of that the worker has done.
	atomic_ullong written;
	atomic_ullong done;
	atomic_bool asleep;
	atomic_bool awaited;
	bool quit;
	// When late: the last frame the worker finished, and where in the log the last frame logged
	// ends.
	bool late;
	uint64_t finishLogged;
	uint16_t finished[SCREEN_HEIGHT][SCREEN_WIDTH];
};

// Waking a sleeping core takes longer than most waits here, so a thread first spins for a while on
// what it waits for: the worker for something logged, the other thread for the worker to have done
// up to target. It spins until then or until a millisecond has passed, and returns whether the wait
// ended.
static bool spin(struct RenderThread* t, bool worker, uint64_t target) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; worker ? atomic_load(&t->done) == atomic_load(&t->written) : atomic_load(&t->done) < target; ++i) {
		if (!(i & 0xFF)) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec > 1000000) {
				return false;
			}
		}
#if defined(__x86_64__)
		__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
		__asm__ volatile("yield");
#endif
	}
	return true;
}

// Makes a logged call on the copy; returns the entry's kind.
static uint32_t play(struct GBA* copy, const uint8_t* entry, uint32_t* size) {
	uint32_t head[4];
	memcpy(head, entry, sizeof(head));
	*size = 16;
	switch (head[0]) {
	case LOG_REGISTER:
		copy->renderer.copyCycle = head[3];
		renderWriteRegister(copy, head[1], head[2]);
		break;
	case LOG_START_LINE:
		renderStartLine(copy, head[1]);
		break;
	case LOG_LATCH:
		renderLatchDispcnt(copy);
		break;
	case LOG_FINISH:
		renderFinishFrame(copy);
		break;
	case LOG_SKIP:
		*size = head[1];
		break;
	case LOG_LINE: {
		const uint8_t* data = entry + 16;
		uint64_t pages = head[2] | (uint64_t) head[3] << 32;
		uint32_t changed = head[1] >> 8;
		if (changed & RENDER_OAM) {
			memcpy(copy->oam, data, SIZE_OAM);
			data += SIZE_OAM;
		}
		if (changed & RENDER_PALETTE) {
			memcpy(copy->palette, data, SIZE_PALETTE);
			data += SIZE_PALETTE;
		}
		for (int page = 0; page < PAGES; ++page) {
			if (pages & (1ull << page)) {
				memcpy(&copy->vram[page * PAGE_SIZE], data, PAGE_SIZE);
				copy->renderer.vramChanged[page] = 1;
				data += PAGE_SIZE;
			}
		}
		copy->renderer.memoryChanged |= changed;
		renderScanline(copy, head[1] & 0xFF);
		*size = (data - entry + 3) & ~3u;
		break;
	}
	}
	return head[0];
}

// The worker: takes entries off the log as they are written and makes the renderer calls they
// describe on its own copy. With nothing to do, it sleeps until woken.
static void* work(void* context) {
	struct RenderThread* t = context;
	for (;;) {
		uint64_t done = atomic_load(&t->done);
		if (done == atomic_load(&t->written)) {
			if (atomic_load(&t->awaited)) {
				pthread_mutex_lock(&t->mutex);
				pthread_cond_signal(&t->idle);
				pthread_mutex_unlock(&t->mutex);
			}
			if (!spin(t, true, 0)) {
				pthread_mutex_lock(&t->mutex);
				atomic_store(&t->asleep, true);
				while (done == atomic_load(&t->written) && !t->quit) {
					pthread_cond_wait(&t->wake, &t->mutex);
				}
				atomic_store(&t->asleep, false);
				bool quit = t->quit;
				pthread_mutex_unlock(&t->mutex);
				if (quit) {
					return NULL;
				}
			}
			continue;
		}
		uint32_t size;
		bool finish = play(t->copy, &t->log[done & (LOG_SIZE - 1)], &size) == LOG_FINISH;
		if (finish && t->late) {
			memcpy(t->finished, t->copy->renderer.frame, sizeof(t->finished));
		}
		atomic_store(&t->done, done + size);
		if (finish && atomic_load(&t->awaited)) {
			pthread_mutex_lock(&t->mutex);
			pthread_cond_signal(&t->idle);
			pthread_mutex_unlock(&t->mutex);
		}
	}
}

// Waits until the worker has done the log up to target: the end of a frame, or everything logged.
static void await(struct RenderThread* t, uint64_t target) {
	if (spin(t, false, target)) {
		return;
	}
	pthread_mutex_lock(&t->mutex);
	atomic_store(&t->awaited, true);
	while (atomic_load(&t->done) < target) {
		pthread_cond_wait(&t->idle, &t->mutex);
	}
	atomic_store(&t->awaited, false);
	pthread_mutex_unlock(&t->mutex);
}

// Waits until the worker has done everything written so far.
static void drain(struct RenderThread* t) {
	await(t, atomic_load(&t->written));
}

// Makes an entry of size bytes visible to the worker, and wakes it if it sleeps.
static void publish(struct RenderThread* t, uint32_t size) {
	atomic_store(&t->written, atomic_load(&t->written) + size);
	if (atomic_load(&t->asleep)) {
		pthread_mutex_lock(&t->mutex);
		pthread_cond_signal(&t->wake);
		pthread_mutex_unlock(&t->mutex);
	}
}

// Finds room for the biggest entry, in one piece and clear of what the worker has yet to do.
static uint8_t* reserve(struct RenderThread* t) {
	uint64_t written = atomic_load(&t->written);
	uint32_t at = written & (LOG_SIZE - 1);
	if (at + LOG_MOST > LOG_SIZE) {
		uint32_t head[4] = { LOG_SKIP, LOG_SIZE - at, 0, 0 };
		memcpy(&t->log[at], head, sizeof(head));
		publish(t, LOG_SIZE - at);
		written += LOG_SIZE - at;
		at = 0;
	}
	if (written + LOG_MOST - atomic_load(&t->done) > LOG_SIZE) {
		drain(t);
	}
	return &t->log[at];
}

// Logs an entry that is a call with up to three arguments.
static void entry(struct GBA* gba, uint32_t kind, uint32_t a, uint32_t b, uint32_t c) {
	struct RenderThread* t = gba->renderThread;
	uint32_t head[4] = { kind, a, b, c };
	memcpy(reserve(t), head, sizeof(head));
	publish(t, sizeof(head));
}

// The calls the emulation thread logs instead of making: a register write, a line start, a latch, a
// line to draw, and the end of a frame.
void renderThreadRegister(struct GBA* gba, uint32_t address, uint16_t value) {
	entry(gba, LOG_REGISTER, address, value, timingCurrentTime(gba) - gba->video.lineStart);
}

void renderThreadStartLine(struct GBA* gba, int y) {
	entry(gba, LOG_START_LINE, y, 0, 0);
}

void renderThreadLatch(struct GBA* gba) {
	entry(gba, LOG_LATCH, 0, 0, 0);
}

void renderThreadScanline(struct GBA* gba, int y) {
	struct RenderThread* t = gba->renderThread;
	struct Renderer* renderer = &gba->renderer;
	uint8_t* start = reserve(t);
	uint8_t* data = start + 16;
	uint32_t changed = renderer->memoryChanged;
	renderer->memoryChanged = 0;
	if (changed & RENDER_OAM) {
		memcpy(data, gba->oam, SIZE_OAM);
		data += SIZE_OAM;
	}
	if (changed & RENDER_PALETTE) {
		memcpy(data, gba->palette, SIZE_PALETTE);
		data += SIZE_PALETTE;
	}
	uint64_t pages = 0;
	for (int page = 0; page < PAGES; ++page) {
		if (renderer->vramChanged[page]) {
			renderer->vramChanged[page] = 0;
			pages |= 1ull << page;
			memcpy(data, &gba->vram[page * PAGE_SIZE], PAGE_SIZE);
			data += PAGE_SIZE;
		}
	}
	uint32_t head[4] = { LOG_LINE, y | changed << 8, (uint32_t) pages, (uint32_t) (pages >> 32) };
	memcpy(start, head, sizeof(head));
	// Entries start on word boundaries.
	publish(t, (data - start + 3) & ~3u);
}

void renderThreadFinishFrame(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (!t->late) {
		entry(gba, LOG_FINISH, 0, 0, 0);
		drain(t);
		memcpy(gba->renderer.frame, t->copy->renderer.frame, sizeof(gba->renderer.frame));
		return;
	}
	// Takes the frame before this one, before the worker can finish another over it.
	if (t->finishLogged) {
		await(t, t->finishLogged);
		memcpy(gba->renderer.frame, t->finished, sizeof(gba->renderer.frame));
	}
	entry(gba, LOG_FINISH, 0, 0, 0);
	t->finishLogged = atomic_load(&t->written);
}

// The copy takes on the renderer and its memory as they are, with nothing logged outstanding.
void renderThreadClone(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	struct GBA* copy = t->copy;
	drain(t);
	memcpy(&copy->renderer, &gba->renderer, sizeof(copy->renderer));
	memcpy(copy->vram, gba->vram, sizeof(copy->vram));
	memcpy(copy->oam, gba->oam, sizeof(copy->oam));
	memcpy(copy->palette, gba->palette, sizeof(copy->palette));
	copy->renderer.copy = true;
	// When late, the frame handed over next is the one this renderer last finished.
	memcpy(t->finished, gba->renderer.frame, sizeof(t->finished));
}

// The worker's renderer, for the frontend to take the frame from.
const struct Renderer* renderThreadRenderer(struct GBA* gba) {
	drain(gba->renderThread);
	return &gba->renderThread->copy->renderer;
}

// Starts the worker with a copy of the renderer and of video memory as they are now. With late,
// each frame is handed over one frame after it was made, so the emulation thread never waits.
bool renderThreadStart(struct GBA* gba, bool late) {
	if (gba->renderThread) {
		return true;
	}
	struct RenderThread* t = calloc(1, sizeof(*t));
	if (t) {
		t->copy = calloc(1, sizeof(*t->copy));
		t->log = malloc(LOG_SIZE);
	}
	if (!t || !t->copy || !t->log) {
		if (t) {
			free(t->copy);
			free(t->log);
		}
		free(t);
		return false;
	}
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->wake, NULL);
	pthread_cond_init(&t->idle, NULL);
	t->late = late;
	gba->renderThread = t;
	renderThreadClone(gba);
	if (pthread_create(&t->thread, NULL, work, t)) {
		gba->renderThread = NULL;
		free(t->copy);
		free(t->log);
		free(t);
		return false;
	}
	return true;
}

// Lets the worker finish what is logged, stops it, and gives drawing back to the emulation thread.
void renderThreadStop(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (!t) {
		return;
	}
	drain(t);
	pthread_mutex_lock(&t->mutex);
	t->quit = true;
	pthread_cond_signal(&t->wake);
	pthread_mutex_unlock(&t->mutex);
	pthread_join(t->thread, NULL);
	// The copy has drawn lines this renderer hasn't, so this renderer takes over the copy's state to
	// carry on alone, keeping its own record of changes not yet logged.
	struct Renderer* renderer = &gba->renderer;
	uint8_t memoryChanged = renderer->memoryChanged;
	uint8_t vramChanged[PAGES];
	memcpy(vramChanged, renderer->vramChanged, sizeof(vramChanged));
	memcpy(renderer, &t->copy->renderer, sizeof(*renderer));
	renderer->memoryChanged = memoryChanged;
	memcpy(renderer->vramChanged, vramChanged, sizeof(vramChanged));
	renderer->copy = false;
	gba->renderThread = NULL;
	free(t->copy);
	free(t->log);
	free(t);
}
