/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "workers.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

enum {
	MAX_WORKERS = 4,
	// How long a waiting thread spins, then yields, before it sleeps: runs follow each other within
	// microseconds while consoles run, and a frame apart between frames.
	SPINS = 4000,
	YIELDS = 2000,
};

static struct Worker {
	pthread_t thread;
	int index;
	// The run it has finished last.
	atomic_uint done;
} workers[MAX_WORKERS];
static int started;
// The count the last workersStart was asked for.
static int asked;
static atomic_uint generation;
static void (*volatile currentJob)(int);
static volatile int currentCount;
static atomic_bool stopping;
static atomic_int sleeping;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

// Tells the CPU this thread is waiting, which spares the other thread on its core.
static void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ volatile("yield");
#endif
}

static void* workerMain(void* argument) {
	struct Worker* const worker = argument;
	unsigned seen = 0;
	for (;;) {
		unsigned now;
		int spins = 0;
		while ((now = atomic_load_explicit(&generation, memory_order_acquire)) == seen && !atomic_load(&stopping)) {
			if (++spins < SPINS) {
				relax();
				continue;
			}
			if (spins < SPINS + YIELDS) {
				sched_yield();
				continue;
			}
			pthread_mutex_lock(&mutex);
			atomic_fetch_add(&sleeping, 1);
			while (atomic_load(&generation) == seen && !atomic_load(&stopping)) {
				pthread_cond_wait(&wake, &mutex);
			}
			atomic_fetch_sub(&sleeping, 1);
			pthread_mutex_unlock(&mutex);
		}
		if (atomic_load(&stopping)) {
			return NULL;
		}
		seen = now;
		if (worker->index < currentCount) {
			currentJob(worker->index);
		}
		atomic_store_explicit(&worker->done, seen, memory_order_release);
	}
}

void workersStart(int count) {
	workersStop();
	long const cores = sysconf(_SC_NPROCESSORS_ONLN);
	int const wanted = count < MAX_WORKERS ? count : MAX_WORKERS;
	atomic_store(&stopping, false);
	asked = count;
	for (int i = 1; i < wanted && i < cores; ++i) {
		workers[i].index = i;
		atomic_store(&workers[i].done, atomic_load(&generation));
		if (pthread_create(&workers[i].thread, NULL, workerMain, &workers[i])) {
			break;
		}
		started = i;
	}
}

void workersStop(void) {
	asked = 0;
	if (!started) {
		return;
	}
	pthread_mutex_lock(&mutex);
	atomic_store(&stopping, true);
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&mutex);
	for (int i = 1; i <= started; ++i) {
		pthread_join(workers[i].thread, NULL);
	}
	started = 0;
}

int workersAsked(void) {
	return asked;
}

void workersRun(void (*job)(int index), int count) {
	currentJob = job;
	currentCount = count;
	unsigned const run = atomic_fetch_add_explicit(&generation, 1, memory_order_acq_rel) + 1;
	if (atomic_load(&sleeping)) {
		pthread_mutex_lock(&mutex);
		pthread_cond_broadcast(&wake);
		pthread_mutex_unlock(&mutex);
	}
	// Indices without a thread of their own run here.
	for (int i = 0; i < count; ++i) {
		if (i == 0 || i > started) {
			job(i);
		}
	}
	for (int i = 1; i <= started; ++i) {
		while (atomic_load_explicit(&workers[i].done, memory_order_acquire) != run) {
			relax();
		}
	}
}
