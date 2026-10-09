/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/thread-proxy.h>

#include <mgba/core/tile-cache.h>
#include <mgba/internal/gba/gba.h>

#ifndef DISABLE_THREADING

static void mVideoThreadProxyInit(struct mVideoLogger* logger);
static void mVideoThreadProxyReset(struct mVideoLogger* logger);
static void mVideoThreadProxyDeinit(struct mVideoLogger* logger);

static THREAD_ENTRY _proxyThread(void* renderer);

static bool _writeData(struct mVideoLogger* logger, const void* data, size_t length);
static bool _readData(struct mVideoLogger* logger, void* data, size_t length, bool block);
static void _postEvent(struct mVideoLogger* logger, enum mVideoLoggerEvent);

static void _lock(struct mVideoLogger* logger);
static void _unlock(struct mVideoLogger* logger);
static void _wait(struct mVideoLogger* logger);
static void _wake(struct mVideoLogger* logger, int y);

// Each thread polls this many times before sleeping on the other. Sleeping between scanlines makes
// both cores look idle, and an ondemand governor then lowers their clock
#define PROXY_SPIN 100000

void mVideoThreadProxyCreate(struct mVideoThreadProxy* renderer) {
	mVideoLoggerRendererCreate(&renderer->d, false);
	renderer->d.block = true;

	renderer->d.init = mVideoThreadProxyInit;
	renderer->d.reset = mVideoThreadProxyReset;
	renderer->d.deinit = mVideoThreadProxyDeinit;
	renderer->d.lock = _lock;
	renderer->d.unlock = _unlock;
	renderer->d.wait = _wait;
	renderer->d.wake = _wake;

	renderer->d.writeData = _writeData;
	renderer->d.readData = _readData;
	renderer->d.postEvent = _postEvent;
}

void mVideoThreadProxyInit(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	ConditionInit(&proxyRenderer->fromThreadCond);
	ConditionInit(&proxyRenderer->toThreadCond);
	MutexInit(&proxyRenderer->mutex);
	RingFIFOInit(&proxyRenderer->dirtyQueue, 0x40000);

	proxyRenderer->threadState = PROXY_THREAD_IDLE;
	ThreadCreate(&proxyRenderer->thread, _proxyThread, proxyRenderer);
}

void mVideoThreadProxyReset(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	MutexLock(&proxyRenderer->mutex);
	while (proxyRenderer->threadState == PROXY_THREAD_BUSY) {
		ConditionWake(&proxyRenderer->toThreadCond);
		ConditionWait(&proxyRenderer->fromThreadCond, &proxyRenderer->mutex);
	}
	RingFIFOClear(&proxyRenderer->dirtyQueue);
	MutexUnlock(&proxyRenderer->mutex);
}

void mVideoThreadProxyDeinit(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	bool waiting = false;
	MutexLock(&proxyRenderer->mutex);
	while (proxyRenderer->threadState == PROXY_THREAD_BUSY) {
		ConditionWake(&proxyRenderer->toThreadCond);
		ConditionWait(&proxyRenderer->fromThreadCond, &proxyRenderer->mutex);
	}
	if (proxyRenderer->threadState == PROXY_THREAD_IDLE) {
		proxyRenderer->threadState = PROXY_THREAD_STOPPED;
		ConditionWake(&proxyRenderer->toThreadCond);
		waiting = true;
	}
	MutexUnlock(&proxyRenderer->mutex);
	if (waiting) {
		ThreadJoin(&proxyRenderer->thread);
	}
	RingFIFODeinit(&proxyRenderer->dirtyQueue);
	ConditionDeinit(&proxyRenderer->fromThreadCond);
	ConditionDeinit(&proxyRenderer->toThreadCond);
	MutexDeinit(&proxyRenderer->mutex);
}

void _proxyThreadRecover(struct mVideoThreadProxy* proxyRenderer) {
	MutexLock(&proxyRenderer->mutex);
	if (proxyRenderer->threadState != PROXY_THREAD_STOPPED) {
		MutexUnlock(&proxyRenderer->mutex);
		return;
	}
	RingFIFOClear(&proxyRenderer->dirtyQueue);
	MutexUnlock(&proxyRenderer->mutex);
	ThreadJoin(&proxyRenderer->thread);
	proxyRenderer->threadState = PROXY_THREAD_IDLE;
	ThreadCreate(&proxyRenderer->thread, _proxyThread, proxyRenderer);
}

// Both sides retry under the lock before sleeping: the other thread only signals after its own
// work, and polls without the lock
static bool _writeData(struct mVideoLogger* logger, const void* data, size_t length) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	if (RingFIFOWrite(&proxyRenderer->dirtyQueue, data, length)) {
		return true;
	}
	mLOG(GBA_VIDEO, DEBUG, "Can't write %"PRIz"u bytes. Proxy thread asleep?", length);
	MutexLock(&proxyRenderer->mutex);
	while (!RingFIFOWrite(&proxyRenderer->dirtyQueue, data, length)) {
		if (proxyRenderer->threadState == PROXY_THREAD_STOPPED) {
			mLOG(GBA_VIDEO, ERROR, "Proxy thread stopped prematurely!");
			MutexUnlock(&proxyRenderer->mutex);
			return false;
		}
		ConditionWake(&proxyRenderer->toThreadCond);
		ConditionWait(&proxyRenderer->fromThreadCond, &proxyRenderer->mutex);
	}
	MutexUnlock(&proxyRenderer->mutex);
	return true;
}

static bool _readData(struct mVideoLogger* logger, void* data, size_t length, bool block) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	int spin = block ? PROXY_SPIN : 0;
	do {
		if (RingFIFORead(&proxyRenderer->dirtyQueue, data, length)) {
			return true;
		}
	} while (spin--);
	if (!block) {
		return false;
	}
	mLOG(GBA_VIDEO, DEBUG, "Can't read %"PRIz"u bytes. CPU thread asleep?", length);
	MutexLock(&proxyRenderer->mutex);
	while (!RingFIFORead(&proxyRenderer->dirtyQueue, data, length)) {
		ConditionWake(&proxyRenderer->fromThreadCond);
		ConditionWait(&proxyRenderer->toThreadCond, &proxyRenderer->mutex);
	}
	MutexUnlock(&proxyRenderer->mutex);
	return true;
}

static void _postEvent(struct mVideoLogger* logger, enum mVideoLoggerEvent event) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	MutexLock(&proxyRenderer->mutex);
	proxyRenderer->event = event;
	ConditionWake(&proxyRenderer->toThreadCond);
	MutexUnlock(&proxyRenderer->mutex);
	int spin;
	for (spin = 0; spin < PROXY_SPIN && event; ++spin) {
		ATOMIC_LOAD(event, proxyRenderer->event);
	}
	MutexLock(&proxyRenderer->mutex);
	while (proxyRenderer->event) {
		ConditionWake(&proxyRenderer->toThreadCond);
		ConditionWait(&proxyRenderer->fromThreadCond, &proxyRenderer->mutex);
	}
	MutexUnlock(&proxyRenderer->mutex);
}

static void _lock(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	MutexLock(&proxyRenderer->mutex);
}

static void _wait(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	if (proxyRenderer->threadState == PROXY_THREAD_STOPPED) {
		mLOG(GBA_VIDEO, ERROR, "Proxy thread stopped prematurely!");
		_proxyThreadRecover(proxyRenderer);
		return;
	}
	MutexLock(&proxyRenderer->mutex);
	ConditionWake(&proxyRenderer->toThreadCond);
	MutexUnlock(&proxyRenderer->mutex);
	int spin;
	for (spin = 0; spin < PROXY_SPIN && RingFIFOSize(&proxyRenderer->dirtyQueue); ++spin);
	MutexLock(&proxyRenderer->mutex);
	while (RingFIFOSize(&proxyRenderer->dirtyQueue)) {
		ConditionWake(&proxyRenderer->toThreadCond);
		ConditionWait(&proxyRenderer->fromThreadCond, &proxyRenderer->mutex);
	}
	MutexUnlock(&proxyRenderer->mutex);
}

static void _unlock(struct mVideoLogger* logger) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	MutexUnlock(&proxyRenderer->mutex);
}

static void _wake(struct mVideoLogger* logger, int y) {
	struct mVideoThreadProxy* proxyRenderer = (struct mVideoThreadProxy*) logger;
	if ((y & 15) == 15) {
		MutexLock(&proxyRenderer->mutex);
		ConditionWake(&proxyRenderer->toThreadCond);
		MutexUnlock(&proxyRenderer->mutex);
	}
}

static THREAD_ENTRY _proxyThread(void* logger) {
	struct mVideoThreadProxy* proxyRenderer = logger;
	ThreadSetName("Proxy Rendering");

	MutexLock(&proxyRenderer->mutex);
	ConditionWake(&proxyRenderer->fromThreadCond);
	while (proxyRenderer->threadState != PROXY_THREAD_STOPPED) {
		// Data written while the last batch ran was announced while nobody waited
		if (!proxyRenderer->event && !RingFIFOSize(&proxyRenderer->dirtyQueue)) {
			MutexUnlock(&proxyRenderer->mutex);
			int spin;
			for (spin = 0; spin < PROXY_SPIN; ++spin) {
				enum mVideoLoggerEvent event;
				enum mVideoThreadProxyState state;
				ATOMIC_LOAD(event, proxyRenderer->event);
				ATOMIC_LOAD(state, proxyRenderer->threadState);
				if (event || state == PROXY_THREAD_STOPPED || RingFIFOSize(&proxyRenderer->dirtyQueue)) {
					break;
				}
			}
			MutexLock(&proxyRenderer->mutex);
			if (proxyRenderer->threadState != PROXY_THREAD_STOPPED && !proxyRenderer->event && !RingFIFOSize(&proxyRenderer->dirtyQueue)) {
				ConditionWait(&proxyRenderer->toThreadCond, &proxyRenderer->mutex);
			}
		}
		if (proxyRenderer->threadState == PROXY_THREAD_STOPPED) {
			break;
		}
		proxyRenderer->threadState = PROXY_THREAD_BUSY;
		if (proxyRenderer->event) {
			proxyRenderer->d.handleEvent(&proxyRenderer->d, proxyRenderer->event);
			proxyRenderer->event = 0;
		} else {
			MutexUnlock(&proxyRenderer->mutex);
			if (!mVideoLoggerRendererRun(&proxyRenderer->d, false)) {
				// FIFO was corrupted
				proxyRenderer->threadState = PROXY_THREAD_STOPPED;
				mLOG(GBA_VIDEO, ERROR, "Proxy thread queue got corrupted!");
			}
			MutexLock(&proxyRenderer->mutex);
		}
		ConditionWake(&proxyRenderer->fromThreadCond);
		if (proxyRenderer->threadState != PROXY_THREAD_STOPPED) {
			proxyRenderer->threadState = PROXY_THREAD_IDLE;
		}
	}
	MutexUnlock(&proxyRenderer->mutex);
	THREAD_EXIT(0);
}

#endif
