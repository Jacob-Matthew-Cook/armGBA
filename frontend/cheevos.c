/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// RetroAchievements, through their rcheevos library (libretro/deps/rcheevos). It talks to
// retroachievements.org with the system's libcurl, loaded when first needed so armgba runs without
// it, on a thread of its own; the answers are handed to rcheevos on the main thread. Only the login
// token is kept, never the password.

#include "frontend.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_libretro.h"

// The few parts of libcurl's interface used here, as its curl.h numbers them.
enum {
	CURLOPT_WRITEDATA = 10001,
	CURLOPT_URL = 10002,
	CURLOPT_POSTFIELDS = 10015,
	CURLOPT_USERAGENT = 10018,
	CURLOPT_HTTPHEADER = 10023,
	CURLOPT_WRITEFUNCTION = 20011,
	CURLOPT_TIMEOUT = 13,
	CURLOPT_FOLLOWLOCATION = 52,
	CURLOPT_NOSIGNAL = 99,
	CURLINFO_RESPONSE_CODE = 0x200002,
	CURL_GLOBAL_DEFAULT = 3,
};

// libcurl's functions, found in the library once it's loaded.
static struct {
	void* library;
	int (*globalInit)(long flags);
	void* (*init)(void);
	int (*setopt)(void* curl, int option, ...);
	int (*perform)(void* curl);
	int (*getinfo)(void* curl, int info, ...);
	void (*cleanup)(void* curl);
	void* (*slistAppend)(void* list, const char* text);
	void (*slistFree)(void* list);
} curl;

// A request to the server and, once the worker has fetched it, its answer. Jobs wait on one list for
// the worker and on another for the main thread.
struct Job {
	char* url;
	char* post;
	char* contentType;
	rc_client_server_callback_t callback;
	void* callbackData;
	char* body;
	size_t length;
	int status;
	struct Job* next;
};

static rc_client_t* client;
static rc_libretro_memory_regions_t regions;
static struct retro_memory_map maps;
static struct retro_memory_descriptor* descriptors;
static char userAgent[128];
static char status[160];

static SDL_Thread* worker;
static SDL_mutex* lock;
static SDL_cond* wake;
static struct Job* waiting;
static struct Job* done;

// HTTP.

// Loads libcurl, under the names distributions give it, the first time it's needed; false if it isn't
// installed or lacks a function used here.
static bool curlLoad(void) {
	static bool tried;
	if (curl.library || tried) {
		return curl.library != NULL;
	}
	tried = true;
	static const char* const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
	for (size_t i = 0; i < sizeof(names) / sizeof(*names) && !curl.library; ++i) {
		curl.library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!curl.library) {
		return false;
	}
	curl.globalInit = (int (*)(long)) dlsym(curl.library, "curl_global_init");
	curl.init = (void* (*)(void)) dlsym(curl.library, "curl_easy_init");
	curl.setopt = (int (*)(void*, int, ...)) dlsym(curl.library, "curl_easy_setopt");
	curl.perform = (int (*)(void*)) dlsym(curl.library, "curl_easy_perform");
	curl.getinfo = (int (*)(void*, int, ...)) dlsym(curl.library, "curl_easy_getinfo");
	curl.cleanup = (void (*)(void*)) dlsym(curl.library, "curl_easy_cleanup");
	curl.slistAppend = (void* (*)(void*, const char*)) dlsym(curl.library, "curl_slist_append");
	curl.slistFree = (void (*)(void*)) dlsym(curl.library, "curl_slist_free_all");
	if (!curl.globalInit || !curl.init || !curl.setopt || !curl.perform || !curl.getinfo || !curl.cleanup || !curl.slistAppend || !curl.slistFree) {
		dlclose(curl.library);
		curl.library = NULL;
		return false;
	}
	curl.globalInit(CURL_GLOBAL_DEFAULT);
	return true;
}

// The answer as it arrives, kept with a 0 after it; returning less than given stops the transfer.
static size_t received(char* data, size_t size, size_t count, void* context) {
	struct Job* job = context;
	char* grown = realloc(job->body, job->length + size * count + 1);
	if (!grown) {
		return 0;
	}
	job->body = grown;
	memcpy(job->body + job->length, data, size * count);
	job->length += size * count;
	job->body[job->length] = '\0';
	return size * count;
}

// Fetches a job's URL, posting its data if it has any, within 30 seconds. The status is the HTTP
// code, or one that has rcheevos try again.
static void fetch(struct Job* job) {
	void* handle = curl.init();
	if (!handle) {
		job->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		return;
	}
	void* headers = NULL;
	curl.setopt(handle, CURLOPT_URL, job->url);
	curl.setopt(handle, CURLOPT_USERAGENT, userAgent);
	curl.setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl.setopt(handle, CURLOPT_NOSIGNAL, 1L);
	curl.setopt(handle, CURLOPT_TIMEOUT, 30L);
	curl.setopt(handle, CURLOPT_WRITEFUNCTION, received);
	curl.setopt(handle, CURLOPT_WRITEDATA, job);
	if (job->post) {
		char header[160];
		snprintf(header, sizeof(header), "Content-Type: %s", job->contentType ? job->contentType : "application/x-www-form-urlencoded");
		headers = curl.slistAppend(NULL, header);
		curl.setopt(handle, CURLOPT_HTTPHEADER, headers);
		curl.setopt(handle, CURLOPT_POSTFIELDS, job->post);
	}
	long code = 0;
	if (curl.perform(handle) == 0 && curl.getinfo(handle, CURLINFO_RESPONSE_CODE, &code) == 0) {
		job->status = (int) code;
	} else {
		job->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
	}
	if (headers) {
		curl.slistFree(headers);
	}
	curl.cleanup(handle);
}

// The worker thread: fetches the waiting jobs in order and hands each on to the main thread.
static int work(void* unused) {
	(void) unused;
	for (;;) {
		SDL_LockMutex(lock);
		while (!waiting) {
			SDL_CondWait(wake, lock);
		}
		struct Job* job = waiting;
		waiting = job->next;
		SDL_UnlockMutex(lock);
		fetch(job);
		SDL_LockMutex(lock);
		struct Job** end = &done;
		while (*end) {
			end = &(*end)->next;
		}
		job->next = NULL;
		*end = job;
		SDL_UnlockMutex(lock);
	}
	return 0;
}

static char* copy(const char* text) {
	return text ? strdup(text) : NULL;
}

// rcheevos's requests: queued for the worker, which starts with the first. Without libcurl the job
// fails at once, its answer handed back on the main thread all the same.
static void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callbackData, rc_client_t* from) {
	(void) from;
	struct Job* job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->url = copy(request->url);
	job->post = copy(request->post_data);
	job->contentType = copy(request->content_type);
	job->callback = callback;
	job->callbackData = callbackData;
	if (!curlLoad()) {
		job->status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
		snprintf(status, sizeof(status), "Needs libcurl, which isn't installed");
		SDL_LockMutex(lock);
		job->next = done;
		done = job;
		SDL_UnlockMutex(lock);
		return;
	}
	if (!worker) {
		worker = SDL_CreateThread(work, "achievements", NULL);
	}
	SDL_LockMutex(lock);
	struct Job** end = &waiting;
	while (*end) {
		end = &(*end)->next;
	}
	*end = job;
	SDL_CondSignal(wake);
	SDL_UnlockMutex(lock);
}

// The answers, on the main thread.
static void deliver(void) {
	SDL_LockMutex(lock);
	struct Job* list = done;
	done = NULL;
	SDL_UnlockMutex(lock);
	while (list) {
		struct Job* job = list;
		list = job->next;
		rc_api_server_response_t const response = { job->body ? job->body : "", job->length, job->status };
		job->callback(&response, job->callbackData);
		free(job->url);
		free(job->post);
		free(job->contentType);
		free(job->body);
		free(job);
	}
}

// The game's memory.

// The core's memory regions, for rcheevos's own map of a console's memory where the core gave none.
static void coreMemory(uint32_t id, rc_libretro_core_memory_info_t* info) {
	info->data = retro_get_memory_data(id);
	info->size = retro_get_memory_size(id);
}

// rcheevos reads the game's memory by the console's addresses.
static uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t size, rc_client_t* from) {
	(void) from;
	return rc_libretro_memory_read(&regions, address, buffer, size);
}

// The core's memory map, copied as it may not outlive the call.
void cheevosSetMemoryMaps(const struct retro_memory_map* map) {
	free(descriptors);
	descriptors = NULL;
	maps.descriptors = NULL;
	maps.num_descriptors = 0;
	if (!map || !map->num_descriptors) {
		return;
	}
	descriptors = malloc(map->num_descriptors * sizeof(*descriptors));
	if (descriptors) {
		memcpy(descriptors, map->descriptors, map->num_descriptors * sizeof(*descriptors));
		maps.descriptors = descriptors;
		maps.num_descriptors = map->num_descriptors;
	}
}

// Events.

// What rcheevos reports, as messages. A reset in hardcore mode restarts the game.
static void event(const rc_client_event_t* e, rc_client_t* from) {
	(void) from;
	switch (e->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		osd("Achievement: %s (%u points)", e->achievement->title, e->achievement->points);
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		osd("Every achievement in this game earned");
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		osd("Leaderboard: %s", e->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		osd("Leaderboard failed: %s", e->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		osd("Leaderboard: %s, %s", e->leaderboard->title, e->leaderboard->tracker_value ? e->leaderboard->tracker_value : "sent");
		break;
	case RC_CLIENT_EVENT_RESET:
		retro_reset();
		osd("Hardcore mode: the game started again");
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		osd("RetroAchievements: %s", e->server_error->error_message);
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		osd("RetroAchievements: can't reach the server; unlocks will be sent later");
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		osd("RetroAchievements: unlocks sent");
		break;
	default:
		break;
	}
}

// The session.

// After logging in: the token is kept for the next start, or forgotten if the server refused it. A
// game already loaded then has its achievements loaded.
static void loggedIn(int result, const char* error, rc_client_t* from, void* userdata) {
	(void) userdata;
	if (result != RC_OK) {
		snprintf(status, sizeof(status), "Couldn't log in: %s", error ? error : rc_error_str(result));
		osd("%s", status);
		if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) {
			settings.cheevosToken[0] = '\0';
			settingsWrite();
		}
		return;
	}
	const rc_client_user_t* user = rc_client_get_user_info(from);
	snprintf(settings.cheevosUser, sizeof(settings.cheevosUser), "%s", user->username);
	snprintf(settings.cheevosToken, sizeof(settings.cheevosToken), "%s", user->token);
	settingsWrite();
	snprintf(status, sizeof(status), "Logged in as %s", user->display_name);
	osd("RetroAchievements: logged in as %s", user->display_name);
	if (app.loaded && !rc_client_is_game_loaded(from) && rc_client_get_load_game_state(from) == RC_CLIENT_LOAD_GAME_STATE_NONE) {
		cheevosLoadGame();
	}
}

// After the game's achievements load: how many are earned.
static void gameLoaded(int result, const char* error, rc_client_t* from, void* userdata) {
	(void) userdata;
	if (result == RC_NO_GAME_LOADED) {
		snprintf(status, sizeof(status), "This game has no achievements");
		return;
	}
	if (result != RC_OK) {
		snprintf(status, sizeof(status), "Couldn't load achievements: %s", error ? error : rc_error_str(result));
		osd("%s", status);
		return;
	}
	status[0] = '\0';
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(from, &summary);
	osd("Achievements: %u of %u earned%s", summary.num_unlocked_achievements, summary.num_core_achievements,
	    rc_client_get_hardcore_enabled(from) ? ", hardcore" : "");
}

// Starts rcheevos, logging in again with the token kept, if there is one.
void cheevosInit(void) {
	lock = SDL_CreateMutex();
	wake = SDL_CreateCond();
	client = rc_client_create(readMemory, serverCall);
	if (!client) {
		return;
	}
	char clause[64] = "";
	rc_client_get_user_agent_clause(client, clause, sizeof(clause));
	struct retro_system_info info;
	retro_get_system_info(&info);
	snprintf(userAgent, sizeof(userAgent), "armGBA/%s %s", info.library_version, clause);
	rc_client_set_event_handler(client, event);
	rc_client_set_hardcore_enabled(client, settings.hardcore);
	if (settings.cheevosUser[0] && settings.cheevosToken[0]) {
		snprintf(status, sizeof(status), "Logging in...");
		rc_client_begin_login_with_token(client, settings.cheevosUser, settings.cheevosToken, loggedIn, NULL);
	}
}

void cheevosLogin(const char* user, const char* password) {
	if (!client) {
		return;
	}
	snprintf(settings.cheevosUser, sizeof(settings.cheevosUser), "%s", user);
	snprintf(status, sizeof(status), "Logging in...");
	rc_client_begin_login_with_password(client, user, password, loggedIn, NULL);
}

// Forgets the token and the game's achievements.
void cheevosLogout(void) {
	if (client) {
		rc_client_unload_game(client);
		rc_client_logout(client);
	}
	settings.cheevosToken[0] = '\0';
	settingsWrite();
	status[0] = '\0';
}

// The console RetroAchievements files the game under, from its header.
static uint32_t consoleOf(const uint8_t* data, size_t size) {
	if (size >= 0xC0 && data[0xB2] == 0x96) {
		return RC_CONSOLE_GAMEBOY_ADVANCE;
	}
	if (size >= 0x150 && data[0x104] == 0xCE && data[0x105] == 0xED) {
		return data[0x143] & 0x80 ? RC_CONSOLE_GAMEBOY_COLOR : RC_CONSOLE_GAMEBOY;
	}
	return RC_CONSOLE_UNKNOWN;
}

// The loaded game's achievements, found by the hash of its file, when logged in.
void cheevosLoadGame(void) {
	if (!client || (!rc_client_get_user_info(client) && !settings.cheevosToken[0])) {
		return;
	}
	uint32_t const console = consoleOf(app.gameData, app.gameSize);
	char hash[33];
	if (console == RC_CONSOLE_UNKNOWN || !rc_hash_generate_from_buffer(hash, console, app.gameData, app.gameSize)) {
		return;
	}
	rc_libretro_memory_destroy(&regions);
	rc_libretro_memory_init(&regions, maps.num_descriptors ? &maps : NULL, coreMemory, console);
	rc_client_set_hardcore_enabled(client, settings.hardcore);
	snprintf(status, sizeof(status), "Loading achievements...");
	rc_client_begin_load_game(client, hash, gameLoaded, NULL);
}

void cheevosUnloadGame(void) {
	if (client) {
		rc_client_unload_game(client);
	}
	rc_libretro_memory_destroy(&regions);
	cheevosSetMemoryMaps(NULL);
	if (client && rc_client_get_user_info(client)) {
		snprintf(status, sizeof(status), "Logged in as %s", rc_client_get_user_info(client)->display_name);
	}
}

void cheevosReset(void) {
	if (client) {
		rc_client_reset(client);
	}
}

void cheevosSetHardcore(bool on) {
	if (client) {
		rc_client_set_hardcore_enabled(client, on);
	}
}

// Each frame the game runs: the answers come in, and the achievements are checked.
void cheevosFrame(void) {
	if (client) {
		deliver();
		rc_client_do_frame(client);
	}
}

// Each pass of the main loop, the game running or not: the answers come in, and the session is kept up.
void cheevosIdle(void) {
	if (client) {
		deliver();
		rc_client_idle(client);
	}
}

// Hardcore mode is only in force while a game with achievements is loaded.
bool cheevosHardcore(void) {
	return client && rc_client_get_hardcore_enabled(client) && rc_client_is_game_loaded(client);
}

// The game's achievements, one line each: those earned ticked, with any progress shown.
int cheevosList(char lines[][160], int most) {
	if (!client || !rc_client_is_game_loaded(client)) {
		return 0;
	}
	rc_client_achievement_list_t* list =
	    rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	int count = 0;
	for (uint32_t b = 0; list && b < list->num_buckets; ++b) {
		for (uint32_t i = 0; i < list->buckets[b].num_achievements && count < most; ++i) {
			const rc_client_achievement_t* a = list->buckets[b].achievements[i];
			snprintf(lines[count++], 160, "%s%s: %s%s%s", a->unlocked ? "\xE2\x9C\x93 " : "", a->title, a->description,
			         a->measured_progress[0] ? " \xC2\xB7 " : "", a->measured_progress);
		}
	}
	rc_client_destroy_achievement_list(list);
	return count;
}

const char* cheevosStatus(void) {
	if (client && rc_client_is_game_loaded(client) && !status[0]) {
		static char earned[96];
		rc_client_user_game_summary_t summary;
		rc_client_get_user_game_summary(client, &summary);
		snprintf(earned, sizeof(earned), "%u of %u earned, %u of %u points%s", summary.num_unlocked_achievements, summary.num_core_achievements,
		         summary.points_unlocked, summary.points_core, rc_client_get_hardcore_enabled(client) ? ", hardcore" : "");
		return earned;
	}
	if (status[0]) {
		return status;
	}
	return client ? "Not logged in" : "Not available";
}
