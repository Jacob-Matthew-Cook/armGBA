/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// armgba: armGBA's own frontend. The libretro core is linked in, and SDL2 gives it a window, sound,
// the keyboard and game controllers. A game is named on the command line or dropped on the window.
// The core's options are kept in ~/.config/armgba/armgba.cfg, written with every choice listed; BIOS
// and boot ROM files go in ~/.config/armgba/system, saves and save states in ~/.local/share/armgba.

#include "libretro.h"
#include "second.h"

#include <SDL.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>

enum {
	PATH_SIZE = 4096,
	NAME_SIZE = 256,
	// A directory, a file name and its extension.
	FILE_SIZE = PATH_SIZE + NAME_SIZE + 16,
	PORTS = 4,
	OPTIONS_MAX = 160,
	// Sound queued ahead of what's playing, in frames: the frame pace follows the sound card.
	AHEAD_FRAMES = 3,
	// Saves are written when they change, checked this often (frames).
	SAVE_CHECK_FRAMES = 300,
};

struct Option {
	char* key;
	char* value;
	char* defaultValue;
	char* description;
	char* choices;
};

static struct Option options[OPTIONS_MAX];
static int optionCount;
static bool optionsChanged;
// Values read from the configuration file before the core names its options.
static char* savedKeys[OPTIONS_MAX];
static char* savedValues[OPTIONS_MAX];
static int savedCount;

static char configPath[FILE_SIZE];
static char systemDirectory[PATH_SIZE];
static char saveDirectory[PATH_SIZE];
static char stateDirectory[PATH_SIZE];

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static int textureWidth, textureHeight;
static enum retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_game_geometry geometry;
static SDL_AudioDeviceID audio;
static unsigned bytesPerFrame;
static SDL_GameController* pads[PORTS];
static uint16_t rumbleStrength[PORTS][2];

static bool gameLoaded;
static char gameName[NAME_SIZE];
static bool paused;
static bool fastForwardHeld;
static bool coreFastForward;
static char message[512];
static Uint32 messageUntil;

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

static char* copyString(const char* s) {
	return s ? strdup(s) : NULL;
}

static void logMessage(enum retro_log_level level, const char* format, ...) {
	(void) level;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void updateTitle(void) {
	char title[NAME_SIZE + 600];
	if (message[0] && SDL_GetTicks() < messageUntil) {
		snprintf(title, sizeof(title), "armGBA - %s", message);
	} else if (gameLoaded) {
		snprintf(title, sizeof(title), "armGBA - %s%s", gameName, paused ? " (paused)" : "");
	} else {
		snprintf(title, sizeof(title), "armGBA - drop a game on this window");
	}
	SDL_SetWindowTitle(window, title);
}

static void showMessage(const char* text, unsigned frames) {
	fprintf(stderr, "armgba: %s\n", text);
	snprintf(message, sizeof(message), "%s", text);
	messageUntil = SDL_GetTicks() + frames * 1000 / 60;
	updateTitle();
}

// Options.

static struct Option* findOption(const char* key) {
	for (int i = 0; i < optionCount; ++i) {
		if (!strcmp(options[i].key, key)) {
			return &options[i];
		}
	}
	return NULL;
}

static void setOption(struct Option* option, const char* value) {
	if (strcmp(option->value, value)) {
		free(option->value);
		option->value = strdup(value);
		optionsChanged = true;
	}
}

static void readOptionDefinitions(const struct retro_core_options_v2* definitions) {
	for (const struct retro_core_option_v2_definition* d = definitions->definitions; d->key && optionCount < OPTIONS_MAX; ++d) {
		if (findOption(d->key)) {
			continue;
		}
		struct Option* option = &options[optionCount++];
		option->key = strdup(d->key);
		option->defaultValue = copyString(d->default_value ? d->default_value : d->values[0].value);
		option->description = copyString(d->desc);
		char choices[1024] = "";
		for (const struct retro_core_option_value* v = d->values; v->value; ++v) {
			size_t length = strlen(choices);
			snprintf(choices + length, sizeof(choices) - length, "%s\"%s\"", length ? ", " : "", v->value);
		}
		option->choices = strdup(choices);
		option->value = copyString(option->defaultValue);
		for (int i = 0; i < savedCount; ++i) {
			if (!strcmp(savedKeys[i], option->key)) {
				free(option->value);
				option->value = strdup(savedValues[i]);
			}
		}
	}
}

// Lines of key = "value", as RetroArch writes them; # starts a comment.
static void readConfiguration(void) {
	FILE* f = fopen(configPath, "r");
	if (!f) {
		return;
	}
	char line[2048];
	while (fgets(line, sizeof(line), f) && savedCount < OPTIONS_MAX) {
		char* key = line + strspn(line, " \t");
		if (*key == '#' || !strchr(key, '=')) {
			continue;
		}
		char* equals = strchr(key, '=');
		char* end = equals;
		while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
			--end;
		}
		*end = '\0';
		char* value = equals + 1 + strspn(equals + 1, " \t\"");
		value[strcspn(value, "\"\r\n")] = '\0';
		savedKeys[savedCount] = strdup(key);
		savedValues[savedCount] = strdup(value);
		++savedCount;
	}
	fclose(f);
}

static void writeConfiguration(void) {
	if (!optionCount) {
		return;
	}
	FILE* f = fopen(configPath, "w");
	if (!f) {
		return;
	}
	fprintf(f, "# armGBA's options: each line's choices are listed above it. Read when armgba starts.\n\n");
	for (int i = 0; i < optionCount; ++i) {
		fprintf(f, "# %s: %s (default \"%s\")\n%s = \"%s\"\n\n", options[i].description ? options[i].description : options[i].key,
		        options[i].choices, options[i].defaultValue, options[i].key, options[i].value);
	}
	fclose(f);
}

// The core's calls back.

static bool setRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	if (port >= PORTS || !pads[port]) {
		return false;
	}
	rumbleStrength[port][effect == RETRO_RUMBLE_STRONG ? 0 : 1] = strength;
	SDL_GameControllerRumble(pads[port], rumbleStrength[port][0], rumbleStrength[port][1], 10000);
	return true;
}

static bool environment(unsigned command, void* data) {
	switch (command) {
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		if (data) {
			readOptionDefinitions(data);
		}
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* variable = data;
		struct Option* option = findOption(variable->key);
		variable->value = option ? option->value : NULL;
		return option != NULL;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLE: {
		const struct retro_variable* variable = data;
		if (!variable) {
			return true;
		}
		struct Option* option = findOption(variable->key);
		if (option && variable->value) {
			setOption(option, variable->value);
		}
		return option != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = optionsChanged;
		optionsChanged = false;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;
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
		pixelFormat = format;
		textureWidth = 0;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		geometry = *(const struct retro_game_geometry*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message* m = data;
		showMessage(m->msg, m->frames);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logMessage;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data) {
			*(int*) data = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
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
			*(int*) data = RETRO_SAVESTATE_CONTEXT_NORMAL;
		}
		return true;
	default:
		return false;
	}
}

static void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	// No picture: the last one stays.
	if (!data) {
		return;
	}
	if (!texture || (int) width != textureWidth || (int) height != textureHeight) {
		if (texture) {
			SDL_DestroyTexture(texture);
		}
		Uint32 format = pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;
		texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
		textureWidth = width;
		textureHeight = height;
	}
	SDL_UpdateTexture(texture, NULL, data, (int) pitch);
}

static size_t audioBatch(const int16_t* data, size_t frames) {
	// Fast-forwarding, sound beyond a few frames ahead is dropped so it never lags behind.
	if (audio && SDL_GetQueuedAudioSize(audio) < bytesPerFrame * AHEAD_FRAMES * 2) {
		SDL_QueueAudio(audio, data, (Uint32) (frames * 4));
	}
	return frames;
}

static void audioSample(int16_t left, int16_t right) {
	int16_t frame[2] = { left, right };
	audioBatch(frame, 1);
}

static void inputPoll(void) {
}

// Keyboard, controller 1: mGBA's keys. The turbo buttons are C and V, L2 to R3 are 1 to 4.
static const SDL_Scancode keys[16] = {
	[RETRO_DEVICE_ID_JOYPAD_B] = SDL_SCANCODE_Z,
	[RETRO_DEVICE_ID_JOYPAD_Y] = SDL_SCANCODE_C,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = SDL_SCANCODE_BACKSPACE,
	[RETRO_DEVICE_ID_JOYPAD_START] = SDL_SCANCODE_RETURN,
	[RETRO_DEVICE_ID_JOYPAD_UP] = SDL_SCANCODE_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN] = SDL_SCANCODE_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = SDL_SCANCODE_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT] = SDL_SCANCODE_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A] = SDL_SCANCODE_X,
	[RETRO_DEVICE_ID_JOYPAD_X] = SDL_SCANCODE_V,
	[RETRO_DEVICE_ID_JOYPAD_L] = SDL_SCANCODE_A,
	[RETRO_DEVICE_ID_JOYPAD_R] = SDL_SCANCODE_S,
	[RETRO_DEVICE_ID_JOYPAD_L2] = SDL_SCANCODE_1,
	[RETRO_DEVICE_ID_JOYPAD_R2] = SDL_SCANCODE_2,
	[RETRO_DEVICE_ID_JOYPAD_L3] = SDL_SCANCODE_3,
	[RETRO_DEVICE_ID_JOYPAD_R3] = SDL_SCANCODE_4,
};

// Controllers by position: libretro's A is the right face button and B the bottom one, as on a
// Game Boy, where SDL names them by an Xbox pad's labels.
static const int buttons[16] = {
	[RETRO_DEVICE_ID_JOYPAD_B] = SDL_CONTROLLER_BUTTON_A,
	[RETRO_DEVICE_ID_JOYPAD_Y] = SDL_CONTROLLER_BUTTON_X,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = SDL_CONTROLLER_BUTTON_BACK,
	[RETRO_DEVICE_ID_JOYPAD_START] = SDL_CONTROLLER_BUTTON_START,
	[RETRO_DEVICE_ID_JOYPAD_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A] = SDL_CONTROLLER_BUTTON_B,
	[RETRO_DEVICE_ID_JOYPAD_X] = SDL_CONTROLLER_BUTTON_Y,
	[RETRO_DEVICE_ID_JOYPAD_L] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
	[RETRO_DEVICE_ID_JOYPAD_R] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	[RETRO_DEVICE_ID_JOYPAD_L2] = -1,
	[RETRO_DEVICE_ID_JOYPAD_R2] = -2,
	[RETRO_DEVICE_ID_JOYPAD_L3] = SDL_CONTROLLER_BUTTON_LEFTSTICK,
	[RETRO_DEVICE_ID_JOYPAD_R3] = SDL_CONTROLLER_BUTTON_RIGHTSTICK,
};

static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port >= PORTS) {
		return 0;
	}
	SDL_GameController* pad = pads[port];
	if (device == RETRO_DEVICE_JOYPAD && id < 16) {
		if (port == 0 && SDL_GetKeyboardState(NULL)[keys[id]]) {
			return 1;
		}
		if (!pad) {
			return 0;
		}
		if (buttons[id] < 0) {
			// The triggers, pressed past half way.
			SDL_GameControllerAxis axis = buttons[id] == -1 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
			return SDL_GameControllerGetAxis(pad, axis) > 16384;
		}
		return SDL_GameControllerGetButton(pad, buttons[id]);
	}
	if (device == RETRO_DEVICE_ANALOG && pad && index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y) {
		SDL_GameControllerAxis axis = index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? (id ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX)
		                                                                      : (id ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX);
		int value = SDL_GameControllerGetAxis(pad, axis);
		return (int16_t) (value < -32767 ? -32767 : value);
	}
	return 0;
}

// Controllers take the ports in the order they're plugged in.
static void openPads(void) {
	for (int port = 0; port < PORTS; ++port) {
		if (pads[port] && !SDL_GameControllerGetAttached(pads[port])) {
			SDL_GameControllerClose(pads[port]);
			pads[port] = NULL;
		}
	}
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (!SDL_IsGameController(i)) {
			continue;
		}
		SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
		bool open = false;
		for (int port = 0; port < PORTS; ++port) {
			open |= pads[port] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[port])) == id;
		}
		for (int port = 0; port < PORTS && !open; ++port) {
			if (!pads[port]) {
				pads[port] = SDL_GameControllerOpen(i);
				open = true;
			}
		}
	}
}

// Saves, by the game's name: game.srm and game.rtc, as RetroArch names them.

static void savePath(char out[FILE_SIZE], const char* directory, const char* extension) {
	snprintf(out, FILE_SIZE, "%s/%s.%s", directory, gameName, extension);
}

static void readSave(unsigned id, int slot, const char* extension) {
	uint8_t* memory = retro_get_memory_data(id);
	size_t size = retro_get_memory_size(id);
	if (!memory || !size) {
		return;
	}
	char path[FILE_SIZE];
	savePath(path, saveDirectory, extension);
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
	savePath(path, saveDirectory, extension);
	FILE* f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "armgba: can't write %s: %s\n", path, strerror(errno));
		return;
	}
	fwrite(memory, 1, size, f);
	fclose(f);
	free(writtenSave[slot]);
	writtenSave[slot] = malloc(size);
	memcpy(writtenSave[slot], memory, size);
	writtenSize[slot] = size;
}

static void writeSaves(void) {
	writeSave(RETRO_MEMORY_SAVE_RAM, 0, "srm");
	writeSave(RETRO_MEMORY_RTC, 1, "rtc");
}

static void saveState(void) {
	size_t size = retro_serialize_size();
	void* data = size ? malloc(size) : NULL;
	char path[FILE_SIZE];
	savePath(path, stateDirectory, "state");
	FILE* f = data && retro_serialize(data, size) ? fopen(path, "wb") : NULL;
	if (f) {
		fwrite(data, 1, size, f);
		fclose(f);
		showMessage("State saved", 120);
	} else {
		showMessage("State not saved", 120);
	}
	free(data);
}

static void loadState(void) {
	char path[FILE_SIZE];
	savePath(path, stateDirectory, "state");
	size_t size;
	void* data = secondRead(path, &size);
	showMessage(data && retro_unserialize(data, size) ? "State loaded" : "No state to load", 120);
	free(data);
}

// Games.

static void unloadGame(void) {
	if (!gameLoaded) {
		return;
	}
	writeSaves();
	retro_unload_game();
	retro_deinit();
	gameLoaded = false;
	if (texture) {
		SDL_DestroyTexture(texture);
		texture = NULL;
	}
}

static bool loadGame(const char* path) {
	unloadGame();
	// A game in an archive is named as RetroArch names it, archive#game, so the core sees its extension.
	char named[PATH_SIZE * 2];
	snprintf(named, sizeof(named), "%s", path);
	if (path_is_compressed_file(path)) {
		struct string_list* entries = file_archive_get_file_list(path, "gba|gb|gbc|bin");
		if (!entries || !entries->size) {
			string_list_free(entries);
			showMessage("No game in the archive", 240);
			return false;
		}
		snprintf(named, sizeof(named), "%s#%s", path, entries->elems[0].data);
		string_list_free(entries);
	}
	size_t size;
	void* data = secondRead(named, &size);
	if (!data) {
		showMessage("Can't read the game", 240);
		return false;
	}
	retro_set_environment(environment);
	retro_init();
	struct retro_game_info game = { named, data, size, NULL };
	if (!retro_load_game(&game)) {
		free(data);
		retro_deinit();
		showMessage("The game didn't load", 240);
		return false;
	}
	free(data);
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	geometry = av.geometry;
	const char* base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	snprintf(gameName, sizeof(gameName), "%s", base);
	char* dot = strrchr(gameName, '.');
	if (dot && dot != gameName) {
		*dot = '\0';
	}
	gameLoaded = true;
	paused = false;
	readSave(RETRO_MEMORY_SAVE_RAM, 0, "srm");
	readSave(RETRO_MEMORY_RTC, 1, "rtc");
	updateTitle();
	return true;
}

// The picture in the window, as large as fits with the core's aspect ratio.
static void present(void) {
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (texture && gameLoaded) {
		int width, height;
		SDL_GetRendererOutputSize(renderer, &width, &height);
		float aspect = geometry.aspect_ratio > 0 ? geometry.aspect_ratio : (float) textureWidth / textureHeight;
		SDL_Rect to = { 0, 0, width, (int) (width / aspect) };
		if (to.h > height) {
			to.h = height;
			to.w = (int) (height * aspect);
		}
		to.x = (width - to.w) / 2;
		to.y = (height - to.h) / 2;
		SDL_RenderCopy(renderer, texture, NULL, &to);
	}
	SDL_RenderPresent(renderer);
}

static const char help[] =
	"Game: arrow keys, X = A, Z = B, A = L, S = R, Enter = Start, Backspace = Select, "
	"C / V = turbo B / A, 1-4 = L2, R2, L3, R3. Controllers work too, one per player.\n\n"
	"Tab (hold): fast forward   P: pause   F2: restart   F5: save state   F7: load state   "
	"F11 or Alt+Enter: full screen   Esc: quit\n\n"
	"Options: ~/.config/armgba/armgba.cfg (read at start). BIOS and boot ROM files: ~/.config/armgba/system.";

static void dataDirectory(char* out, const char* xdg, const char* fallback, const char* leaf) {
	const char* base = getenv(xdg);
	if (base && *base) {
		snprintf(out, PATH_SIZE, "%s/armgba%s", base, leaf);
	} else {
		snprintf(out, PATH_SIZE, "%s/%s/armgba%s", getenv("HOME") ? getenv("HOME") : ".", fallback, leaf);
	}
	makeDirectories(out);
}

int main(int argc, char** argv) {
	const char* game = NULL;
	bool fullscreen = false;
	int scale = 3;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--fullscreen")) {
			fullscreen = true;
		} else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
			scale = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("usage: armgba [--fullscreen] [--scale N] [game]\n\n%s\n", help);
			return 0;
		} else {
			game = argv[i];
		}
	}
	scale = scale < 1 ? 1 : scale > 10 ? 10 : scale;

	char configDirectory[PATH_SIZE];
	dataDirectory(configDirectory, "XDG_CONFIG_HOME", ".config", "");
	dataDirectory(systemDirectory, "XDG_CONFIG_HOME", ".config", "/system");
	dataDirectory(saveDirectory, "XDG_DATA_HOME", ".local/share", "/saves");
	dataDirectory(stateDirectory, "XDG_DATA_HOME", ".local/share", "/states");
	snprintf(configPath, sizeof(configPath), "%s/armgba.cfg", configDirectory);
	readConfiguration();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "armgba: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	window = SDL_CreateWindow("armGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * scale, 160 * scale,
	                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
	if (!renderer) {
		renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
	}
	if (!renderer) {
		fprintf(stderr, "armgba: %s\n", SDL_GetError());
		return 1;
	}

	retro_set_environment(environment);
	retro_set_video_refresh(videoRefresh);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);

	// Sound at the core's rate; the frames are paced by how much of it is waiting to play.
	SDL_AudioSpec want = { .freq = 32768, .format = AUDIO_S16SYS, .channels = 2, .samples = 512 }, have;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio) {
		SDL_PauseAudioDevice(audio, 0);
	} else {
		fprintf(stderr, "armgba: no sound: %s\n", SDL_GetError());
	}
	double const frameRate = 16777216.0 / 280896.0;
	bytesPerFrame = (unsigned) (32768 / frameRate) * 4;

	openPads();
	if (game) {
		loadGame(game);
	}
	updateTitle();

	Uint64 const ticksPerFrame = (Uint64) (SDL_GetPerformanceFrequency() / frameRate);
	Uint64 next = SDL_GetPerformanceCounter();
	unsigned frame = 0;
	bool running = true;
	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT:
				running = false;
				break;
			case SDL_DROPFILE:
				loadGame(event.drop.file);
				SDL_free(event.drop.file);
				break;
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
				openPads();
				break;
			case SDL_KEYDOWN: {
				SDL_Keycode key = event.key.keysym.sym;
				bool const alt = event.key.keysym.mod & KMOD_ALT;
				if (key == SDLK_ESCAPE) {
					running = false;
				} else if (key == SDLK_F11 || (key == SDLK_RETURN && alt)) {
					fullscreen = !fullscreen;
					SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
				} else if (key == SDLK_F1) {
					SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "armGBA", help, window);
				} else if (gameLoaded && !event.key.repeat) {
					if (key == SDLK_p) {
						paused = !paused;
						updateTitle();
					} else if (key == SDLK_F2) {
						retro_reset();
					} else if (key == SDLK_F5) {
						saveState();
					} else if (key == SDLK_F7) {
						loadState();
					}
				}
				if (key == SDLK_TAB) {
					fastForwardHeld = true;
				}
				break;
			}
			case SDL_KEYUP:
				if (event.key.keysym.sym == SDLK_TAB) {
					fastForwardHeld = false;
				}
				break;
			}
		}
		if (message[0] && SDL_GetTicks() >= messageUntil) {
			message[0] = '\0';
			updateTitle();
		}
		if (!gameLoaded || paused) {
			present();
			SDL_Delay(16);
			next = SDL_GetPerformanceCounter();
			continue;
		}
		retro_run();
		present();
		if (++frame % SAVE_CHECK_FRAMES == 0) {
			writeSaves();
		}
		if (fastForwardHeld || coreFastForward) {
			next = SDL_GetPerformanceCounter();
			continue;
		}
		if (audio) {
			while (SDL_GetQueuedAudioSize(audio) > bytesPerFrame * AHEAD_FRAMES) {
				SDL_Delay(1);
			}
		} else {
			next += ticksPerFrame;
			Uint64 now = SDL_GetPerformanceCounter();
			if (next > now) {
				SDL_Delay((Uint32) ((next - now) * 1000 / SDL_GetPerformanceFrequency()));
			} else {
				next = now;
			}
		}
	}
	unloadGame();
	writeConfiguration();
	if (audio) {
		SDL_CloseAudioDevice(audio);
	}
	SDL_Quit();
	return 0;
}
