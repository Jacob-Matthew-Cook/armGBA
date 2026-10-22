/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// armgba: the libretro core linked into an SDL2 program, with a window, sound, the keyboard, game
// controllers and the mouse. A game is named on the command line, dropped on the window or picked
// from the menu.

#include "frontend.h"
#include "second.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>

struct App app = { .format = RETRO_PIXEL_FORMAT_0RGB1555, .avEnable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO, .slot = 0 };

char configDirectory[PATH_SIZE];
char systemDirectory[PATH_SIZE];
char saveDirectory[PATH_SIZE];
char stateDirectory[PATH_SIZE];
char shotDirectory[PATH_SIZE];
char cheatDirectory[PATH_SIZE];

static SDL_AudioDeviceID audio;
static unsigned bytesPerFrame;
// A frame's sound, gathered as the core gives it, then sent on at the frame's end.
static int16_t frameSound[2 * 4096];
static size_t frameSoundCount;
static SDL_Texture* overlay;
static int overlayFilter = -1, overlayWidth, overlayHeight;
static uint16_t rumbleStrength[PORTS][2];
static bool coreFastForward;
// A save's memory as last written, to write it only when it changes.
static uint8_t* writtenSave[2];
static size_t writtenSize[2];

static void makeDirectories(const char* path) {
	char partial[PATH_SIZE];
	snprintf(partial, sizeof(partial), "%s", path);
	for (char* p = partial + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(partial, 0755);
			*p = '/';
		}
	}
	mkdir(partial, 0755);
}

static void dataDirectory(char* out, const char* xdg, const char* fallback, const char* leaf) {
	const char* base = getenv(xdg);
	if (base && *base) {
		snprintf(out, PATH_SIZE, "%s/armgba%s", base, leaf);
	} else {
		snprintf(out, PATH_SIZE, "%s/%s/armgba%s", getenv("HOME") ? getenv("HOME") : ".", fallback, leaf);
	}
	makeDirectories(out);
}

static void logMessage(enum retro_log_level level, const char* format, ...) {
	(void) level;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

// The core's calls back.

static bool setRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	if (port >= PORTS || !app.pads[port]) {
		return false;
	}
	rumbleStrength[port][effect == RETRO_RUMBLE_STRONG ? 0 : 1] = strength;
	SDL_GameControllerRumble(app.pads[port], rumbleStrength[port][0], rumbleStrength[port][1], 10000);
	return true;
}

static bool environment(unsigned command, void* data) {
	switch (command) {
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		if (data) {
			optionsDefine(data);
		}
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		optionsDisplay(data);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* variable = data;
		struct Option* option = optionFind(variable->key);
		variable->value = option ? option->value : NULL;
		return option != NULL;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLE: {
		const struct retro_variable* variable = data;
		if (!variable) {
			return true;
		}
		struct Option* option = optionFind(variable->key);
		if (option && variable->value) {
			optionSet(option, variable->value);
		}
		return option != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = optionsChanged;
		optionsChanged = false;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
		cheevosSetMemoryMaps(data);
		return true;
	case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
		return netPacketInterface(data);
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = systemDirectory;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = saveDirectory;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format format = *(const enum retro_pixel_format*) data;
		if (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888) {
			return false;
		}
		app.format = format;
		app.gameWidth = 0;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		app.geometry = *(const struct retro_game_geometry*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		osd("%s", ((const struct retro_message*) data)->msg);
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logMessage;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data) {
			*(int*) data = app.avEnable;
		}
		return true;
	case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: {
		const struct retro_fastforwarding_override* override = data;
		if (override) {
			coreFastForward = override->fastforward;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		((struct retro_rumble_interface*) data)->set_rumble_state = setRumble;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		if (data) {
			*(int*) data = netRollingBack() ? RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY : RETRO_SAVESTATE_CONTEXT_NORMAL;
		}
		return true;
	default:
		return false;
	}
}

static bool gameSmooth;

// SDL 2.0.14 can't change the scaling of a texture the renderer converts (RGB565 under OpenGL), so
// the game's texture is made again when it changes.
static void makeGameTexture(unsigned width, unsigned height) {
	if (app.game) {
		SDL_DestroyTexture(app.game);
	}
	gameSmooth = settings.filter == FILTER_SMOOTH;
	Uint32 const format = app.format == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, gameSmooth ? "linear" : "nearest");
	app.game = SDL_CreateTexture(app.renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	app.gameWidth = width;
	app.gameHeight = height;
}

static void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	// No picture: the last one stays. While a frame is run again, nothing is shown.
	if (!data || !(app.avEnable & RETRO_AV_ENABLE_VIDEO)) {
		return;
	}
	if (!app.game || (int) width != app.gameWidth || (int) height != app.gameHeight) {
		makeGameTexture(width, height);
		free(app.picture);
		app.picture = malloc(pitch * height);
	}
	SDL_UpdateTexture(app.game, NULL, data, (int) pitch);
	memcpy(app.picture, data, pitch * height);
	app.picturePitch = pitch;
	if (recording()) {
		recordVideo(data, width, height, pitch);
	}
}

static size_t audioBatch(const int16_t* data, size_t frames) {
	if (!(app.avEnable & RETRO_AV_ENABLE_AUDIO)) {
		return frames;
	}
	size_t const room = (sizeof(frameSound) / sizeof(*frameSound) - frameSoundCount) / 2;
	size_t const take = frames < room ? frames : room;
	memcpy(&frameSound[frameSoundCount], data, take * 4);
	frameSoundCount += take * 2;
	return frames;
}

static void audioSample(int16_t left, int16_t right) {
	int16_t frame[2] = { left, right };
	audioBatch(frame, 1);
}

static void inputPoll(void) {
}

// A controller binding's state.
static bool padHeld(SDL_GameController* pad, int binding) {
	if (!pad || binding == PAD_NONE) {
		return false;
	}
	if (binding == PAD_LEFT_TRIGGER || binding == PAD_RIGHT_TRIGGER) {
		SDL_GameControllerAxis axis = binding == PAD_LEFT_TRIGGER ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
		return SDL_GameControllerGetAxis(pad, axis) > 16384;
	}
	return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton) binding);
}

// Player port's buttons here, as libretro's bits: the keyboard plays player 1, and each controller
// its own player.
uint16_t localButtons(unsigned port) {
	if (port >= PORTS || app.menuOpen) {
		return 0;
	}
	const Uint8* keys = SDL_GetKeyboardState(NULL);
	uint16_t bits = 0;
	for (int id = 0; id < BUTTONS; ++id) {
		bool held = padHeld(app.pads[port], settings.pad[id]);
		held |= port == 0 && settings.keys[id] && keys[settings.keys[id]];
		bits |= held << id;
	}
	return bits;
}

static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port >= PORTS) {
		return 0;
	}
	if (device == RETRO_DEVICE_JOYPAD && id < BUTTONS) {
		uint16_t const bits = netRunning() ? netInput(port, id) : localButtons(port);
		return (bits >> id) & 1;
	}
	SDL_GameController* pad = app.pads[port];
	if (device == RETRO_DEVICE_ANALOG && pad && !app.menuOpen && !netRunning() && index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT &&
	    id <= RETRO_DEVICE_ID_ANALOG_Y) {
		SDL_GameControllerAxis axis = index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? (id ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX)
		                                                                      : (id ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX);
		int value = SDL_GameControllerGetAxis(pad, axis);
		return (int16_t) (value < -32767 ? -32767 : value);
	}
	return 0;
}

// Controllers take the players in the order they're plugged in.
void setPadForPort(void) {
	for (int port = 0; port < PORTS; ++port) {
		if (app.pads[port] && !SDL_GameControllerGetAttached(app.pads[port])) {
			SDL_GameControllerClose(app.pads[port]);
			app.pads[port] = NULL;
		}
	}
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (!SDL_IsGameController(i)) {
			continue;
		}
		SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
		bool open = false;
		for (int port = 0; port < PORTS; ++port) {
			open |= app.pads[port] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.pads[port])) == id;
		}
		for (int port = 0; port < PORTS && !open; ++port) {
			if (!app.pads[port]) {
				app.pads[port] = SDL_GameControllerOpen(i);
				open = true;
			}
		}
	}
}

// Saves, by the game's name: game.srm and game.rtc, as RetroArch names them.

static void savePath(char out[FILE_SIZE], const char* extension) {
	snprintf(out, FILE_SIZE, "%s/%s.%s", saveDirectory, app.gameName, extension);
}

static void readSave(unsigned id, int slot, const char* extension) {
	uint8_t* memory = retro_get_memory_data(id);
	size_t size = retro_get_memory_size(id);
	if (!memory || !size) {
		return;
	}
	char path[FILE_SIZE];
	savePath(path, extension);
	FILE* f = fopen(path, "rb");
	if (f) {
		fread(memory, 1, size, f);
		fclose(f);
	}
	free(writtenSave[slot]);
	writtenSave[slot] = malloc(size);
	memcpy(writtenSave[slot], memory, size);
	writtenSize[slot] = size;
}

static void writeSave(unsigned id, int slot, const char* extension) {
	uint8_t* memory = retro_get_memory_data(id);
	size_t size = retro_get_memory_size(id);
	if (!memory || !size || (writtenSave[slot] && writtenSize[slot] == size && !memcmp(writtenSave[slot], memory, size))) {
		return;
	}
	char path[FILE_SIZE];
	savePath(path, extension);
	FILE* f = fopen(path, "wb");
	if (!f) {
		osd("Can't write %s: %s", path, strerror(errno));
		return;
	}
	fwrite(memory, 1, size, f);
	fclose(f);
	free(writtenSave[slot]);
	writtenSave[slot] = malloc(size);
	memcpy(writtenSave[slot], memory, size);
	writtenSize[slot] = size;
}

void writeSaves(void) {
	if (app.loaded) {
		writeSave(RETRO_MEMORY_SAVE_RAM, 0, "srm");
		writeSave(RETRO_MEMORY_RTC, 1, "rtc");
	}
}

// Games.

void unloadGame(void) {
	if (!app.loaded) {
		return;
	}
	netStop();
	recordStop();
	if (settings.autoState && !cheevosHardcore()) {
		saveState(-1);
	}
	cheevosUnloadGame();
	writeSaves();
	retro_unload_game();
	retro_deinit();
	app.loaded = false;
	free((void*) app.gameData);
	app.gameData = NULL;
	if (app.game) {
		SDL_DestroyTexture(app.game);
		app.game = NULL;
	}
	app.gameWidth = 0;
	free(cheats);
	cheats = NULL;
	cheatCount = 0;
}

bool loadGame(const char* path) {
	unloadGame();
	// A game in an archive is named as RetroArch names it, archive#game, so the core sees its extension.
	char named[PATH_SIZE * 2];
	snprintf(named, sizeof(named), "%s", path);
	if (path_is_compressed_file(path)) {
		struct string_list* entries = file_archive_get_file_list(path, "gba|gb|gbc|bin|gba2|gb2");
		if (!entries || !entries->size) {
			string_list_free(entries);
			osd("No game in that archive");
			return false;
		}
		snprintf(named, sizeof(named), "%s#%s", path, entries->elems[0].data);
		string_list_free(entries);
	}
	size_t size;
	void* data = secondRead(named, &size);
	if (!data) {
		osd("Can't read %s", path);
		return false;
	}
	retro_set_environment(environment);
	retro_init();
	struct retro_game_info game = { named, data, size, NULL };
	if (!retro_load_game(&game)) {
		free(data);
		retro_deinit();
		osd("That game didn't load");
		return false;
	}
	app.gameData = data;
	app.gameSize = size;
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	app.geometry = av.geometry;
	snprintf(app.gamePath, sizeof(app.gamePath), "%s", path);
	const char* base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	snprintf(app.gameName, sizeof(app.gameName), "%s", base);
	char* dot = strrchr(app.gameName, '.');
	if (dot && dot != app.gameName) {
		*dot = '\0';
	}
	// The folder it came from is where the menu's game list opens next time.
	snprintf(settings.directory, sizeof(settings.directory), "%s", path);
	char* slash = strrchr(settings.directory, '/');
	if (slash) {
		*slash = '\0';
	}
	app.loaded = true;
	app.paused = false;
	readSave(RETRO_MEMORY_SAVE_RAM, 0, "srm");
	readSave(RETRO_MEMORY_RTC, 1, "rtc");
	cheatListLoad();
	cheatListApply();
	rewindReset();
	cheevosLoadGame();
	if (settings.autoState && !cheevosHardcore() && stateExists(-1)) {
		loadState(-1);
	}
	char title[NAME_SIZE + 16];
	snprintf(title, sizeof(title), "armGBA - %s", app.gameName);
	SDL_SetWindowTitle(app.window, title);
	return true;
}

// The picture.

// A pattern over the picture, a source pixel to each 3x3 of it: an LCD's dark gaps between its pixels,
// or a CRT's dark line under each row.
static void makeOverlay(void) {
	if (overlay && overlayFilter == settings.filter && overlayWidth == app.gameWidth && overlayHeight == app.gameHeight) {
		return;
	}
	if (overlay) {
		SDL_DestroyTexture(overlay);
		overlay = NULL;
	}
	overlayFilter = settings.filter;
	overlayWidth = app.gameWidth;
	overlayHeight = app.gameHeight;
	if ((settings.filter != FILTER_LCD && settings.filter != FILTER_SCANLINES) || !app.gameWidth) {
		return;
	}
	int const w = app.gameWidth * 3, h = app.gameHeight * 3;
	uint32_t* pixels = calloc((size_t) w * h, 4);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			bool dark = settings.filter == FILTER_LCD ? x % 3 == 2 || y % 3 == 2 : y % 3 == 2;
			pixels[y * w + x] = dark ? (settings.filter == FILTER_LCD ? 0x60000000u : 0x80000000u) : 0;
		}
	}
	overlay = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
	SDL_UpdateTexture(overlay, NULL, pixels, w * 4);
	SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(overlay, SDL_ScaleModeLinear);
	free(pixels);
}

static void present(void) {
	SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
	SDL_RenderClear(app.renderer);
	if (app.game && app.loaded && gameSmooth != (settings.filter == FILTER_SMOOTH)) {
		makeGameTexture(app.gameWidth, app.gameHeight);
		SDL_UpdateTexture(app.game, NULL, app.picture, (int) app.picturePitch);
	}
	if (app.game && app.loaded) {
		int width, height;
		SDL_GetRendererOutputSize(app.renderer, &width, &height);
		SDL_Rect to = { 0, 0, width, height };
		if (!settings.stretch) {
			float const aspect = app.geometry.aspect_ratio > 0 ? app.geometry.aspect_ratio : (float) app.gameWidth / app.gameHeight;
			to.h = (int) (width / aspect);
			if (to.h > height) {
				to.h = height;
				to.w = (int) (height * aspect);
			}
			if (settings.integerScale) {
				int const factor = SDL_max(1, SDL_min(to.w / app.gameWidth, to.h / app.gameHeight));
				to.w = app.gameWidth * factor;
				to.h = app.gameHeight * factor;
			}
		}
		to.x = (width - to.w) / 2;
		to.y = (height - to.h) / 2;
		SDL_RenderCopy(app.renderer, app.game, NULL, &to);
		makeOverlay();
		if (overlay) {
			SDL_RenderCopy(app.renderer, overlay, NULL, &to);
		}
	}
	uiDraw();
	SDL_RenderPresent(app.renderer);
}

void recreateRenderer(void) {
	if (app.game) {
		SDL_DestroyTexture(app.game);
		app.game = NULL;
	}
	if (overlay) {
		SDL_DestroyTexture(overlay);
		overlay = NULL;
	}
	if (app.renderer) {
		SDL_DestroyRenderer(app.renderer);
	}
	Uint32 const vsync = settings.vsync ? SDL_RENDERER_PRESENTVSYNC : 0;
	app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED | vsync);
	if (!app.renderer) {
		app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
	}
	// The next frame makes the game's texture again.
	app.gameWidth = 0;
	uiRecreateTextures();
}

void applyWindowSize(void) {
	SDL_SetWindowFullscreen(app.window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (!settings.fullscreen) {
		int const w = app.loaded && app.geometry.base_width ? (int) app.geometry.base_width : 240;
		int const h = app.loaded && app.geometry.base_height ? (int) app.geometry.base_height : 160;
		SDL_SetWindowSize(app.window, w * settings.scale, h * settings.scale);
	}
}

// Sound: the frame's samples, at the volume, sent on. With vsync, the frames follow the screen, so
// the sound is stretched or squeezed by up to half a percent to keep the queue where it should be.
static void sendSound(void) {
	if (!audio || !frameSoundCount) {
		frameSoundCount = 0;
		return;
	}
	if (recording()) {
		recordAudio(frameSound, frameSoundCount / 2);
	}
	size_t const frames = frameSoundCount / 2;
	unsigned const queued = SDL_GetQueuedAudioSize(audio);
	unsigned const target = bytesPerFrame * settings.latency;
	// Fast-forwarding, sound beyond a few frames ahead is dropped so it never lags behind.
	if (queued > target * 2) {
		frameSoundCount = 0;
		return;
	}
	double ratio = 1;
	if (settings.vsync) {
		ratio = 1 + 0.005 * ((double) target - queued) / target;
	}
	static int16_t out[2 * 8192];
	size_t const outFrames = SDL_min((size_t) lround(frames * ratio), sizeof(out) / sizeof(*out) / 2);
	int const volume = settings.volume * 256 / 100;
	for (size_t i = 0; i < outFrames; ++i) {
		double const at = (double) i * frames / outFrames;
		size_t const a = (size_t) at;
		size_t const b = a + 1 < frames ? a + 1 : a;
		double const t = at - a;
		for (int c = 0; c < 2; ++c) {
			double const sample = frameSound[a * 2 + c] * (1 - t) + frameSound[b * 2 + c] * t;
			out[i * 2 + c] = (int16_t) (sample * volume / 256);
		}
	}
	SDL_QueueAudio(audio, out, (Uint32) (outFrames * 4));
	frameSoundCount = 0;
}

// One frame of the game as it should run now: through netplay, rewinding, with run-ahead, or plainly.
void runFrames(void) {
	if (netRunning()) {
		netFrame();
	} else if (app.rewinding) {
		if (!rewindStep()) {
			app.rewinding = false;
		}
	} else if (settings.runahead && !cheevosHardcore()) {
		runAheadFrame();
	} else {
		retro_run();
	}
	if (!app.rewinding && !netRunning()) {
		rewindPush();
	}
	cheevosFrame();
	sendSound();
}

// Hotkeys and the menu.

static void doAction(enum Action action) {
	if (action == ACT_MENU) {
		if (app.menuOpen) {
			uiCloseMenu();
		} else {
			uiOpenMenu();
		}
		return;
	}
	if (action == ACT_FULLSCREEN) {
		settings.fullscreen = !settings.fullscreen;
		applyWindowSize();
		return;
	}
	if (!app.loaded) {
		return;
	}
	bool const hardcore = cheevosHardcore();
	switch (action) {
	case ACT_PAUSE:
		if (!netRunning()) {
			app.paused = !app.paused;
			osd(app.paused ? "Paused" : "Running");
		}
		break;
	case ACT_SAVE_STATE:
		if (hardcore || netRunning()) {
			osd(hardcore ? "Hardcore mode: no save states" : "Not in netplay");
		} else {
			osd(saveState(app.slot) ? "Saved state %d" : "State %d not saved", app.slot);
		}
		break;
	case ACT_LOAD_STATE:
		if (hardcore || netRunning()) {
			osd(hardcore ? "Hardcore mode: no save states" : "Not in netplay");
		} else {
			osd(loadState(app.slot) ? "Loaded state %d" : "No state %d", app.slot);
		}
		break;
	case ACT_SLOT_PREVIOUS:
	case ACT_SLOT_NEXT:
		app.slot = (app.slot + (action == ACT_SLOT_NEXT ? 1 : SLOTS - 1)) % SLOTS;
		osd("State slot %d%s", app.slot, stateExists(app.slot) ? "" : " (empty)");
		break;
	case ACT_SCREENSHOT:
		takeScreenshot();
		break;
	case ACT_RECORD:
		recordToggle();
		break;
	case ACT_RESET:
		if (!netRunning()) {
			retro_reset();
			cheevosReset();
			osd("Restarted");
		}
		break;
	default:
		break;
	}
}

static void handleEvent(const SDL_Event* event) {
	if (uiEvent(event)) {
		return;
	}
	switch (event->type) {
	case SDL_QUIT:
		app.running = false;
		break;
	case SDL_DROPFILE:
		loadGame(event->drop.file);
		SDL_free(event->drop.file);
		break;
	case SDL_CONTROLLERDEVICEADDED:
	case SDL_CONTROLLERDEVICEREMOVED:
		setPadForPort();
		break;
	case SDL_KEYDOWN:
		if (!event->key.repeat) {
			for (int a = 0; a < ACTIONS; ++a) {
				if (settings.hotkeys[a] && event->key.keysym.scancode == settings.hotkeys[a] && a != ACT_FAST_FORWARD && a != ACT_REWIND) {
					doAction((enum Action) a);
				}
			}
			if (event->key.keysym.sym == SDLK_RETURN && (event->key.keysym.mod & KMOD_ALT)) {
				doAction(ACT_FULLSCREEN);
			}
		}
		break;
	case SDL_CONTROLLERBUTTONDOWN:
		for (int a = 0; a < ACTIONS; ++a) {
			if (settings.padHotkeys[a] == event->cbutton.button && a != ACT_FAST_FORWARD && a != ACT_REWIND) {
				doAction((enum Action) a);
			}
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		if (event->button.button == SDL_BUTTON_RIGHT) {
			uiOpenMenu();
		}
		break;
	}
}

// The held hotkeys: fast forward and rewind.
static bool actionHeld(enum Action action) {
	if (app.menuOpen) {
		return false;
	}
	const Uint8* keys = SDL_GetKeyboardState(NULL);
	bool held = settings.hotkeys[action] && keys[settings.hotkeys[action]];
	for (int port = 0; port < PORTS; ++port) {
		held |= padHeld(app.pads[port], settings.padHotkeys[action]);
	}
	return held;
}

int main(int argc, char** argv) {
	const char* game = NULL;
	const char* connectTo = NULL;
	int hostMode = 0;
	settingsDefaults();
	dataDirectory(configDirectory, "XDG_CONFIG_HOME", ".config", "");
	dataDirectory(systemDirectory, "XDG_CONFIG_HOME", ".config", "/system");
	dataDirectory(saveDirectory, "XDG_DATA_HOME", ".local/share", "/saves");
	dataDirectory(stateDirectory, "XDG_DATA_HOME", ".local/share", "/states");
	dataDirectory(shotDirectory, "XDG_DATA_HOME", ".local/share", "/screenshots");
	dataDirectory(cheatDirectory, "XDG_DATA_HOME", ".local/share", "/cheats");
	settingsRead();
	netInit();
	optionsReadSaved();
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--fullscreen")) {
			settings.fullscreen = true;
		} else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
			int const scale = atoi(argv[++i]);
			settings.scale = SDL_max(1, SDL_min(8, scale));
		} else if (!strcmp(argv[i], "--host") || !strcmp(argv[i], "--host-together")) {
			hostMode = !strcmp(argv[i], "--host") ? 1 : 2;
		} else if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
			connectTo = argv[++i];
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			settings.netPort = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("usage: armgba [--fullscreen] [--scale N] [--host | --host-together | --connect ADDRESS] [--port N] [game]\n\n"
			       "Esc, the controller's Guide button or a right-click opens the menu, where everything else is.\n"
			       "Settings: ~/.config/armgba; saves, states, screenshots and cheats: ~/.local/share/armgba.\n");
			return 0;
		} else {
			game = argv[i];
		}
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "armgba: %s\n", SDL_GetError());
		return 1;
	}
	app.window = SDL_CreateWindow("armGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * settings.scale, 160 * settings.scale,
	                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!app.window) {
		fprintf(stderr, "armgba: %s\n", SDL_GetError());
		return 1;
	}
	recreateRenderer();
	if (!app.renderer || !uiInit()) {
		fprintf(stderr, "armgba: %s\n", SDL_GetError());
		return 1;
	}

	retro_set_environment(environment);
	retro_set_video_refresh(videoRefresh);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);

	SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 2, .samples = 512 }, have;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio) {
		SDL_PauseAudioDevice(audio, 0);
	} else {
		fprintf(stderr, "armgba: no sound: %s\n", SDL_GetError());
	}
	bytesPerFrame = (unsigned) (AUDIO_RATE / FRAME_RATE) * 4;

	cheevosInit();
	setPadForPort();
	if (game && loadGame(game)) {
		if (hostMode) {
			netHost(hostMode == 2);
		} else if (connectTo) {
			netJoin(connectTo);
		}
	} else {
		uiOpenMenu();
	}

	Uint64 const frequency = SDL_GetPerformanceFrequency();
	double const frameTicks = frequency / FRAME_RATE;
	Uint64 last = SDL_GetPerformanceCounter(), fpsSince = last;
	double owed = 0;
	unsigned frame = 0, fpsFrames = 0;
	app.running = true;
	while (app.running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			handleEvent(&event);
		}
		app.fastForward = actionHeld(ACT_FAST_FORWARD) || coreFastForward;
		app.rewinding = settings.rewind && !cheevosHardcore() && !netRunning() && actionHeld(ACT_REWIND);
		cheevosIdle();
		bool const idle = !app.loaded || ((app.paused || app.menuOpen) && !netRunning());
		if (idle) {
			present();
			if (!settings.vsync) {
				SDL_Delay(16);
			}
			last = SDL_GetPerformanceCounter();
			owed = 0;
			continue;
		}
		// How many frames are due: with vsync, as the time passes; else one, then the sound sets the pace.
		Uint64 const now = SDL_GetPerformanceCounter();
		int due = 1;
		if (settings.vsync) {
			owed += (double) (now - last) / frameTicks;
			due = (int) owed;
			owed -= due;
			due = SDL_min(due, 4);
		}
		last = now;
		if (app.fastForward && !netRunning()) {
			due = 0;
			Uint64 const until = now + frequency / 60;
			while (SDL_GetPerformanceCounter() < until && due < 12) {
				runFrames();
				++due;
			}
		} else {
			for (int i = 0; i < due; ++i) {
				runFrames();
			}
		}
		fpsFrames += due;
		if (now - fpsSince > frequency) {
			app.fps = fpsFrames * (double) frequency / (now - fpsSince);
			fpsFrames = 0;
			fpsSince = now;
		}
		present();
		if (++frame % 300 == 0) {
			writeSaves();
		}
		// Rewinding makes no sound, so then the clock sets the pace.
		if (!settings.vsync && !app.fastForward && audio && !app.rewinding) {
			while (SDL_GetQueuedAudioSize(audio) > bytesPerFrame * (unsigned) settings.latency) {
				SDL_Delay(1);
			}
		} else if (!settings.vsync && !app.fastForward) {
			Uint64 const until = now + (Uint64) frameTicks;
			while (SDL_GetPerformanceCounter() < until) {
				SDL_Delay(1);
			}
		}
	}
	unloadGame();
	settingsWrite();
	optionsWrite();
	if (audio) {
		SDL_CloseAudioDevice(audio);
	}
	SDL_Quit();
	return 0;
}
