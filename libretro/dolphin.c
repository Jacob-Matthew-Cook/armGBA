/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Dolphin's side is its GBA (TCP) device, which mGBA's dolphin.c (Jeffrey Pfau, MPL) also talks to. */
#include "dolphin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/gba.h"

enum {
	DATA_PORT = 54970,
	CLOCK_PORT = 49420,
	// How often the port is looked at while the game runs: a byte's time at 115200 baud.
	GRAIN = CPU_FREQUENCY / 115200 * 8,
	// How long the console waits for Dolphin's next command once it has caught up with it. With none
	// in that time the GameCube game isn't talking to it, and it runs on by itself until one comes.
	WAIT_MS = 100,
	// A search: how long connections may take, and the wait before the next one, in milliseconds.
	SEARCH_MS = 1000,
	RETRY_MS = 2000,
	MAX_TRIES = 256,
};

static struct {
	char host[256];
	bool wanted;
	int data;
	int clock;
	// A search under way: connections to each address tried, and when it started (or, while waiting
	// for the next, when that starts).
	int tries[MAX_TRIES];
	struct in_addr addresses[MAX_TRIES];
	int count;
	int64_t when;
	int clockTry;
	struct in_addr found;
	// Serving: cycles the console may still run before Dolphin's next command, whether that command's
	// time has come, and whether Dolphin has gone quiet.
	struct Event event;
	int32_t slice;
	bool clocked;
	bool idle;
} dol = { .data = -1, .clock = -1, .clockTry = -1 };

static void closeSocket(int* fd) {
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void stopSearch(void) {
	for (int i = 0; i < dol.count; ++i) {
		closeSocket(&dol.tries[i]);
	}
	dol.count = 0;
	closeSocket(&dol.clockTry);
}

static int64_t now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Disconnects, and looks again after a while.
static void unplug(void) {
	stopSearch();
	closeSocket(&dol.data);
	closeSocket(&dol.clock);
	dol.when = now() + RETRY_MS;
}

// Starts connecting to a port without waiting. Returns the socket, or -1.
static int startConnect(struct in_addr address, int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = address };
	if (connect(fd, (struct sockaddr*) &to, sizeof(to)) < 0 && errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	return fd;
}

// 1 once a connection is made, 0 while it's still being made, -1 if it failed.
static int connected(int fd) {
	struct pollfd p = { fd, POLLOUT, 0 };
	if (poll(&p, 1, 0) <= 0) {
		return 0;
	}
	int error = 0;
	socklen_t size = sizeof(error);
	return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0 ? 1 : -1;
}

static void tryAddress(struct in_addr address) {
	if (dol.count < MAX_TRIES) {
		dol.addresses[dol.count] = address;
		dol.tries[dol.count] = startConnect(address, DATA_PORT);
		++dol.count;
	}
}

// Tries the chosen address, or for "auto" this device and every other on its local networks (the
// /24 around each address at most).
static void startSearch(void) {
	if (strcmp(dol.host, "auto")) {
		struct in_addr address;
		if (inet_pton(AF_INET, dol.host, &address) == 1) {
			tryAddress(address);
		} else {
			struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
			struct addrinfo* list = NULL;
			if (getaddrinfo(dol.host, NULL, &hints, &list) == 0 && list) {
				tryAddress(((struct sockaddr_in*) list->ai_addr)->sin_addr);
			}
			freeaddrinfo(list);
		}
		return;
	}
	tryAddress((struct in_addr) { htonl(INADDR_LOOPBACK) });
	struct ifaddrs* list = NULL;
	if (getifaddrs(&list) < 0) {
		return;
	}
	for (struct ifaddrs* i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) {
			continue;
		}
		uint32_t const self = ntohl(((struct sockaddr_in*) i->ifa_addr)->sin_addr.s_addr);
		uint32_t mask = ntohl(((struct sockaddr_in*) i->ifa_netmask)->sin_addr.s_addr);
		if ((self >> 24) == 127) {
			continue;
		}
		mask |= 0xFFFFFF00;
		for (uint32_t host = 1; host < (~mask & 0xFFFFFFFF); ++host) {
			uint32_t const address = (self & mask) | host;
			if (address != self) {
				tryAddress((struct in_addr) { htonl(address) });
			}
		}
	}
	freeifaddrs(list);
}

// Moves the search on, without waiting.
static void search(void) {
	if (now() < dol.when) {
		return;
	}
	if (dol.data < 0 && !dol.count) {
		startSearch();
		dol.when = now();
	}
	if (dol.data < 0) {
		// A failed connection is closed at once: its error, once read, is gone.
		for (int i = 0; i < dol.count && dol.data < 0; ++i) {
			int const state = dol.tries[i] >= 0 ? connected(dol.tries[i]) : 0;
			if (state < 0) {
				closeSocket(&dol.tries[i]);
			} else if (state > 0) {
				dol.data = dol.tries[i];
				dol.found = dol.addresses[i];
				dol.tries[i] = -1;
			}
		}
		if (dol.data >= 0) {
			stopSearch();
			dol.clockTry = startConnect(dol.found, CLOCK_PORT);
		}
	}
	if (dol.data >= 0 && dol.clockTry >= 0) {
		int const state = connected(dol.clockTry);
		if (state > 0) {
			dol.clock = dol.clockTry;
			dol.clockTry = -1;
			int on = 1;
			setsockopt(dol.data, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
			dol.slice = 0;
			dol.clocked = false;
			dol.idle = true;
			return;
		}
		if (state < 0) {
			closeSocket(&dol.clockTry);
			closeSocket(&dol.data);
		}
	}
	if (now() - dol.when > SEARCH_MS && dol.clock < 0) {
		unplug();
	}
}

// Whether something has come within ms milliseconds.
static void waitFor(int fd, int ms) {
	struct pollfd p = { fd, POLLIN, 0 };
	poll(&p, 1, ms);
}

// Up to n bytes that have come, left to be taken; -1 once Dolphin has gone.
static int peek(int fd, uint8_t* bytes, int n) {
	ssize_t const got = recv(fd, bytes, n, MSG_PEEK | MSG_DONTWAIT);
	if (got == 0) {
		return -1;
	}
	if (got < 0) {
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
	}
	return (int) got;
}

// Answers Dolphin's command once all of it has come: 1 if it did, 0 if it hasn't come, -1 if Dolphin
// has gone. Out of JOY bus mode the console doesn't answer, and Dolphin sees no GBA there.
static int serve(struct GBA* gba) {
	// The command, then room for the longest answer.
	uint8_t bytes[6];
	int const got = peek(dol.data, bytes, 5);
	if (got <= 0) {
		return got;
	}
	// A write brings its four bytes; the others are a byte.
	int const length = bytes[0] == 0x15 ? 5 : 1;
	if (got < length) {
		return 0;
	}
	recv(dol.data, bytes, length, MSG_DONTWAIT);
	int const answer = sioJoyCommand(gba, bytes[0], bytes + 1);
	if (answer && send(dol.data, bytes + 1, answer, MSG_NOSIGNAL) != answer) {
		return -1;
	}
	return 1;
}

// Before each command Dolphin sends the time since the last, in this console's cycles. The console
// answers as it comes and runs on that long, then waits for the next.
static void eventRun(struct GBA* gba, void* context, uint32_t late) {
	(void) context;
	dol.slice -= (int32_t) late;
	if (!dol.clocked) {
		if (dol.slice < 0 && !dol.idle) {
			waitFor(dol.clock, WAIT_MS);
		}
		uint8_t bytes[4];
		int const got = peek(dol.clock, bytes, sizeof(bytes));
		if (got < 0) {
			unplug();
			return;
		}
		if (got == 4) {
			recv(dol.clock, bytes, sizeof(bytes), MSG_DONTWAIT);
			int32_t const time = (int32_t) ((uint32_t) bytes[0] << 24 | bytes[1] << 16 | bytes[2] << 8 | bytes[3]);
			dol.slice = dol.idle ? time : dol.slice + time;
			dol.clocked = true;
			dol.idle = false;
		} else if (dol.slice < 0) {
			dol.idle = true;
		}
	}
	if (dol.clocked) {
		int served = serve(gba);
		if (!served) {
			// The command follows its time at once.
			waitFor(dol.data, WAIT_MS);
			served = serve(gba);
		}
		if (served < 0) {
			unplug();
			return;
		}
		dol.clocked = !served;
	}
	dol.slice -= GRAIN;
	timingSchedule(gba, &dol.event, GRAIN);
}

void dolphinSetHost(const char* host) {
	bool const wanted = host && *host;
	if (wanted == dol.wanted && (!wanted || !strcmp(host, dol.host))) {
		return;
	}
	unplug();
	dol.when = 0;
	dol.wanted = wanted;
	snprintf(dol.host, sizeof(dol.host), "%s", wanted ? host : "");
	dol.event.callback = eventRun;
	dol.event.priority = 0x80;
}

void dolphinGbaFrame(struct GBA* gba) {
	if (dol.clock < 0) {
		timingDeschedule(gba, &dol.event);
		if (dol.wanted) {
			search();
		}
	}
	// Once connected it carries on from frame to frame; a state loaded doesn't hold it.
	if (dol.clock >= 0 && !timingIsScheduled(gba, &dol.event)) {
		timingSchedule(gba, &dol.event, 0);
	}
}

bool dolphinConnected(void) {
	return dol.clock >= 0;
}

const char* dolphinAddress(void) {
	return inet_ntoa(dol.found);
}
