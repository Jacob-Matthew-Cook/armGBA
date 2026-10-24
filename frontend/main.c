/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// armgba: the libretro core linked into an SDL2 program, with a window, sound, the keyboard, game
// controllers and the mouse. A game is named on the command line, dropped on the window or picked
// from the menu.

#include "frontend.h"
#include "lcd.h"
#include "games.h"
#include "patch.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>

struct App app = { .format = RETRO_PIXEL_FORMAT_0RGB1555, .avEnable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO, .slot = 0 };

char configDirectory[PATH_SIZE];
char systemDirectory[PATH_SIZE];
char ownSystemDirectory[PATH_SIZE];
char saveDirectory[PATH_SIZE];
char ownSaveDirectory[PATH_SIZE];
char stateDirectory[PATH_SIZE];
char ownStateDirectory[PATH_SIZE];
char shotDirectory[PATH_SIZE];
char ownShotDirectory[PATH_SIZE];
char cheatDirectory[PATH_SIZE];

static SDL_AudioDeviceID audio;
static unsigned bytesPerFrame;
// A frame's sound, gathered as the core gives it, then sent on at the frame's end.
static int16_t frameSound[2 * 4096];
static size_t frameSoundCount;
static SDL_Texture* overlay;
static int overlayWidth, overlayHeight, overlayShownWidth, overlayShownHeight;
static uint32_t overlayGap = 1;
static SDL_Texture* shadow;
static bool shadowStale;
static uint16_t rumbleStrength[PORTS][2];
static bool coreFastForward;
// A save's memory as last written, to write it only when it changes.
static uint8_t* writtenSave[2];
static size_t writtenSize[2];

// path and each folder above it, as mkdir -p makes them.
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

// armgba's folder under the XDG base directory xdg names, or under fallback in the home folder.
static void dataDirectory(char* out, const char* xdg, const char* fallback, const char* leaf) {
	const char* base = getenv(xdg);
	if (base && *base) {
		snprintf(out, PATH_SIZE, "%s/armgba%s", base, leaf);
	} else {
		snprintf(out, PATH_SIZE, "%s/%s/armgba%s", getenv("HOME") ? getenv("HOME") : ".", fallback, leaf);
	}
	makeDirectories(out);
}

// What RetroArch's retroarch.cfg says of its folders, when there is one: its BIOS and screenshot
// folders (empty if they aren't there), and where it keeps saves.
static struct {
	bool found;
	char system[PATH_SIZE];
	char shots[PATH_SIZE];
	// Saves: beside the game, or in saves, then in a folder named for the game's folder and one named
	// for the core when it sorts them.
	bool savesBesideGame;
	char saves[PATH_SIZE];
	bool sortByContent, sortByCore;
} retroarch;

// A folder retroarch.cfg names, as RetroArch reads it: "~" its home, ":" the folder retroarch.cfg is
// in. False for "default" or nothing, which RetroArch takes as its own choice.
static bool retroarchPath(char out[PATH_SIZE], const char* value, const char* configFolder) {
	const char* const home = getenv("HOME") ? getenv("HOME") : ".";
	if (!value[0] || !strcmp(value, "default")) {
		return false;
	}
	if (value[0] == '~') {
		snprintf(out, PATH_SIZE, "%s%s", home, value + 1);
	} else if (value[0] == ':') {
		snprintf(out, PATH_SIZE, "%s%s", configFolder, value + 1);
	} else {
		snprintf(out, PATH_SIZE, "%s", value);
	}
	return true;
}

static bool isFolder(const char* path) {
	struct stat st;
	return path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Reads the first retroarch.cfg found: XDG_CONFIG_HOME's, ~/.config's, then the Flatpak's.
static void findRetroarch(void) {
	const char* const home = getenv("HOME") ? getenv("HOME") : ".";
	const char* const xdg = getenv("XDG_CONFIG_HOME");
	char folders[3][PATH_SIZE];
	snprintf(folders[0], PATH_SIZE, "%s/retroarch", xdg && *xdg ? xdg : "");
	snprintf(folders[1], PATH_SIZE, "%s/.config/retroarch", home);
	snprintf(folders[2], PATH_SIZE, "%s/.var/app/org.libretro.RetroArch/config/retroarch", home);
	for (int i = xdg && *xdg ? 0 : 1; i < 3 && !retroarch.found; ++i) {
		char path[PATH_SIZE + 16];
		snprintf(path, sizeof(path), "%s/retroarch.cfg", folders[i]);
		FILE* f = fopen(path, "r");
		if (!f) {
			continue;
		}
		retroarch.found = true;
		snprintf(retroarch.system, PATH_SIZE, "%s/system", folders[i]);
		retroarch.savesBesideGame = true;
		char line[PATH_SIZE + 64];
		while (fgets(line, sizeof(line), f)) {
			char key[64];
			const char* const open = strchr(line, '"');
			const char* const close = open ? strchr(open + 1, '"') : NULL;
			if (!close || sscanf(line, " %63[a-z_]", key) != 1) {
				continue;
			}
			char value[PATH_SIZE];
			snprintf(value, sizeof(value), "%.*s", (int) (close - open - 1), open + 1);
			bool const yes = !strcmp(value, "true");
			if (!strcmp(key, "system_directory")) {
				retroarchPath(retroarch.system, value, folders[i]);
			} else if (!strcmp(key, "screenshot_directory")) {
				retroarchPath(retroarch.shots, value, folders[i]);
			} else if (!strcmp(key, "savefile_directory")) {
				retroarch.savesBesideGame = !retroarchPath(retroarch.saves, value, folders[i]);
			} else if (!strcmp(key, "savefiles_in_content_dir")) {
				retroarch.savesBesideGame |= yes;
			} else if (!strcmp(key, "sort_savefiles_by_content_enable")) {
				retroarch.sortByContent = yes;
			} else if (!strcmp(key, "sort_savefiles_enable")) {
				retroarch.sortByCore = yes;
			}
		}
		fclose(f);
		// savefiles_in_content_dir may come before or after savefile_directory.
		if (retroarch.savesBesideGame) {
			retroarch.saves[0] = '\0';
		}
	}
	if (!isFolder(retroarch.system)) {
		retroarch.system[0] = '\0';
	}
	if (!isFolder(retroarch.shots)) {
		retroarch.shots[0] = '\0';
	}
}

// RetroArch's folder which ("bios", "saves" or "screenshots") for the menu, or NULL if it has none;
// its saves may be beside each game.
const char* retroarchFolder(const char* which) {
	if (!strcmp(which, "saves")) {
		return !retroarch.found ? NULL : retroarch.savesBesideGame ? "beside each game" : retroarch.saves;
	}
	const char* const folder = !strcmp(which, "bios") ? retroarch.system : retroarch.shots;
	return folder[0] ? folder : NULL;
}

// Whether folder holds a BIOS or boot ROM the core reads.
static bool holdsBios(const char* folder) {
	static const char* const names[] = { "gba_bios.bin", "gb_bios.bin", "dmg_boot.bin", "mgb_boot.bin", "gbc_bios.bin",
		                                 "cgb_boot.bin", "cgb_agb_boot.bin", "cgb_agb0_boot.bin", "agb_boot.bin" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		char path[PATH_SIZE + 32];
		snprintf(path, sizeof(path), "%s/%s", folder, names[i]);
		if (access(path, R_OK) == 0) {
			return true;
		}
	}
	return false;
}

// One of the Folders: armGBA's own (setting "" or "armgba"), RetroArch's ("retroarch", when it has
// one), or the folder typed in; made if it's written to and isn't there.
static void chooseFolder(char out[PATH_SIZE], const char* setting, const char* own, const char* retroarchOwn, bool written) {
	bool const theirs = !strcmp(setting, "retroarch") && retroarchOwn[0];
	snprintf(out, PATH_SIZE, "%s", theirs ? retroarchOwn : !setting[0] || !strcmp(setting, "armgba") || !strcmp(setting, "retroarch") ? own : setting);
	if (written) {
		makeDirectories(out);
	}
}

// The BIOS, state and screenshot folders, as the Folders say. Auto BIOS is armGBA's when it holds a
// BIOS or boot ROM, RetroArch's otherwise. The core reads the BIOS folder as each game loads.
void chooseFolders(void) {
	const char* bios = settings.biosFolder;
	if (!bios[0]) {
		bios = !retroarch.system[0] || holdsBios(ownSystemDirectory) ? "armgba" : "retroarch";
	}
	chooseFolder(systemDirectory, bios, ownSystemDirectory, retroarch.system, false);
	chooseFolder(stateDirectory, settings.stateFolder, ownStateDirectory, "", true);
	chooseFolder(shotDirectory, settings.shotFolder, ownShotDirectory, retroarch.shots, true);
}

// The save folder for the game at path, as it loads, so a change can't put the running game's save
// over another: RetroArch's as RetroArch picks it, beside the game or in its saves folder, sorted as
// it sorts them. Auto is armGBA's when it has this game's save, RetroArch's otherwise.
static void chooseSaveFolder(const char* path) {
	char gameFolder[PATH_SIZE], name[NAME_SIZE];
	snprintf(gameFolder, sizeof(gameFolder), "%s", path);
	char* const slash = strrchr(gameFolder, '/');
	snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
	char* const dot = strrchr(name, '.');
	if (dot && dot != name) {
		*dot = '\0';
	}
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(gameFolder, sizeof(gameFolder), ".");
	}
	char theirs[PATH_SIZE] = "";
	if (retroarch.found) {
		snprintf(theirs, PATH_SIZE, "%s", retroarch.savesBesideGame ? gameFolder : retroarch.saves);
		if (!retroarch.savesBesideGame && retroarch.sortByContent) {
			const char* const folderName = strrchr(gameFolder, '/') ? strrchr(gameFolder, '/') + 1 : gameFolder;
			size_t const length = strlen(theirs);
			snprintf(theirs + length, PATH_SIZE - length, "/%s", folderName);
		}
		if (!retroarch.savesBesideGame && retroarch.sortByCore) {
			size_t const length = strlen(theirs);
			snprintf(theirs + length, PATH_SIZE - length, "/armGBA");
		}
	}
	const char* chosen = settings.saveFolder;
	if (!chosen[0]) {
		char own[FILE_SIZE];
		snprintf(own, sizeof(own), "%s/%s.srm", ownSaveDirectory, name);
		chosen = !theirs[0] || access(own, R_OK) == 0 ? "armgba" : "retroarch";
	}
	chooseFolder(saveDirectory, chosen, ownSaveDirectory, theirs, true);
}

// The core's log, to stderr.
static void logMessage(enum retro_log_level level, const char* format, ...) {
	(void) level;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

// The core's calls back.

// The core sets each motor's strength; SDL takes both at once, kept on until changed.
static bool setRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	if (port >= PORTS || !app.pads[port]) {
		return false;
	}
	rumbleStrength[port][effect == RETRO_RUMBLE_STRONG ? 0 : 1] = strength;
	SDL_GameControllerRumble(app.pads[port], rumbleStrength[port][0], rumbleStrength[port][1], 10000);
	return true;
}

// The core's way to show and hide its options again for their values.
static retro_core_options_update_display_callback_t updateShown;

// Has the core show and hide its options again after one changes.
void refreshShownOptions(void) {
	if (updateShown && app.loaded) {
		updateShown();
	}
}

// What the core asks of the frontend: its options, folders, pixel format, logging, rumble, the link
// cable's netpacket interface, and whether this frame is shown and heard. The rest it does without.
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
		// With LCD Rendering on the GPU, the core's grid is off whatever size it's set to.
		if (option && settings.lcdOnGpu && !strcmp(variable->key, "armgba_lcd_grid")) {
			variable->value = "disabled";
		}
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
	// Whether an option changed since the core last asked.
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = optionsChanged;
		optionsChanged = false;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
		updateShown = data ? ((const struct retro_core_options_update_display_callback*) data)->callback : NULL;
		return true;
	// What this frontend has no use for, accepted.
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
	// RGB565 for the Advance, XRGB8888 for the Game Boy; the texture is made again for it.
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
	// Whether this frame is shown and heard: not while run-ahead or netplay runs frames ahead or again.
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
	// Netplay's states need only load back into this same core.
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		if (data) {
			*(int*) data = netRollingBack() ? RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY : RETRO_SAVESTATE_CONTEXT_NORMAL;
		}
		return true;
	default:
		return false;
	}
}

// Whether the game's texture was made for Smooth.
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

// The core's picture: to the game's texture, and kept for screenshots and the pixel shadow.
static void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	// No picture: the last one stays. While a frame is run again, nothing is shown.
	if (!data || !(app.avEnable & RETRO_AV_ENABLE_VIDEO)) {
		return;
	}
	if (!app.game || (int) width != app.gameWidth || (int) height != app.gameHeight || pitch != app.picturePitch) {
		makeGameTexture(width, height);
		free(app.picture);
		app.picture = malloc(pitch * height);
		if (!app.picture) {
			app.picturePitch = 0;
			return;
		}
	}
	SDL_UpdateTexture(app.game, NULL, data, (int) pitch);
	memcpy(app.picture, data, pitch * height);
	app.picturePitch = pitch;
	shadowStale = true;
	if (recording()) {
		recordVideo(data, width, height, pitch);
	}
}

// The core's sound, gathered for the frame; sendSound sends it on.
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

// A controller's Hotkey: the one chosen, or on Auto its Home button, or Select without one.
static int hotkeyOf(SDL_GameController* pad) {
	if (settings.padHotkey != PAD_AUTO) {
		return settings.padHotkey;
	}
	return pad && SDL_GameControllerHasButton(pad, SDL_CONTROLLER_BUTTON_GUIDE) ? SDL_CONTROLLER_BUTTON_GUIDE : SDL_CONTROLLER_BUTTON_BACK;
}

// The controller playing a port, by its joystick's id; -1 for none.
static int portOf(SDL_JoystickID which) {
	for (int port = 0; port < PORTS; ++port) {
		if (app.pads[port] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.pads[port])) == which) {
			return port;
		}
	}
	return -1;
}

// A hotkey's buttons on each controller, held back from the game after the hotkey until they're let go.
static uint32_t heldBack[PORTS];

bool padHotkeyPressed(SDL_JoystickID which, int binding, int action) {
	int const port = portOf(which);
	if (port < 0 || binding == PAD_NONE || binding != settings.padHotkeys[action]) {
		return false;
	}
	int const hotkey = hotkeyOf(app.pads[port]);
	if (hotkey == binding || !padHeld(app.pads[port], hotkey)) {
		return false;
	}
	heldBack[port] |= padBit(hotkey) | padBit(binding);
	return true;
}

// Player port's buttons here, as libretro's bits: the keyboard plays player 1, and each controller
// its own player.
uint16_t localButtons(unsigned port) {
	if (port >= PORTS || app.menuOpen) {
		return 0;
	}
	for (int b = 0; b < 32; ++b) {
		if (heldBack[port] >> b & 1 && !padHeld(app.pads[port], b == 30 ? PAD_LEFT_TRIGGER : b == 31 ? PAD_RIGHT_TRIGGER : b)) {
			heldBack[port] &= ~(1u << b);
		}
	}
	const Uint8* keys = SDL_GetKeyboardState(NULL);
	uint16_t bits = 0;
	for (int id = 0; id < BUTTONS; ++id) {
		bool held = !(padBit(settings.pad[id]) & heldBack[port]) && padHeld(app.pads[port], settings.pad[id]);
		held |= port == 0 && settings.keys[id] && keys[settings.keys[id]];
		bits |= held << id;
	}
	return bits;
}

// The buttons the core reads: netplay's for each player when connected; the sticks only when not.
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
				if (app.pads[port]) {
					osd("Controller %d: %s", port + 1, SDL_GameControllerName(app.pads[port]));
				}
			}
		}
	}
}

// Saves, by the game's name: game.srm and game.rtc, as RetroArch names them.

static void savePath(char out[FILE_SIZE], const char* extension) {
	snprintf(out, FILE_SIZE, "%s/%s.%s", saveDirectory, app.gameName, extension);
}

// The save's memory as it is now, as written.
static void rememberSave(int slot, const uint8_t* memory, size_t size) {
	free(writtenSave[slot]);
	writtenSave[slot] = malloc(size);
	writtenSize[slot] = writtenSave[slot] ? size : 0;
	if (writtenSave[slot]) {
		memcpy(writtenSave[slot], memory, size);
	}
}

// The game's save memory from its file, when there is one.
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
	rememberSave(slot, memory, size);
}

// The game's save memory to its file, when it has changed since last read or written.
static void writeSave(unsigned id, int slot, const char* extension) {
	uint8_t* memory = retro_get_memory_data(id);
	size_t size = retro_get_memory_size(id);
	if (!memory || !size || (writtenSave[slot] && writtenSize[slot] == size && !memcmp(writtenSave[slot], memory, size))) {
		return;
	}
	char path[FILE_SIZE];
	savePath(path, extension);
	if (writeWholeFile(path, memory, size)) {
		rememberSave(slot, memory, size);
	}
}

// The cartridge's save and its clock's.
void writeSaves(void) {
	if (app.loaded) {
		writeSave(RETRO_MEMORY_SAVE_RAM, 0, "srm");
		writeSave(RETRO_MEMORY_RTC, 1, "rtc");
	}
}

// Games.

// Closes the game: netplay and recording stop, and the save, and the state when Save and Load State
// on Exit and Start is on, are written.
void unloadGame(void) {
	if (!app.loaded) {
		return;
	}
	netStop();
	recordStop();
	app.fastForwardOn = false;
	if (settings.autoState && !cheevosHardcore()) {
		saveState(-1);
	}
	cheevosUnloadGame();
	writeSaves();
	undoForget();
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

// Loads a game, or the first game in an archive, with its save, cheats and achievements, and its
// state when Auto Save State is on.
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
	void* data = gameRead(named, &size);
	if (!data) {
		osd("Can't read %s", path);
		return false;
	}
	// A patch beside it applies, as in RetroArch.
	char patchName[NAME_SIZE];
	int const patches = patchGame(path, &data, &size, patchName, sizeof(patchName));
	chooseSaveFolder(path);
	retro_set_environment(environment);
	retro_init();
	struct retro_game_info game = { named, data, size, NULL };
	if (!retro_load_game(&game)) {
		free(data);
		retro_deinit();
		osd("That game didn't load");
		return false;
	}
	// The game's file is kept for its achievements' hash and netplay's CRC.
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
		undoForget();
	}
	if (patches < 0) {
		osd("%s doesn't fit this game, which runs unpatched", patchName);
	} else if (patches) {
		osd(patches == 1 ? "Patched" : "Patched with %d patches", patches);
	}
	recentAdd(path);
	char title[NAME_SIZE + 16];
	snprintf(title, sizeof(title), "armGBA - %s", app.gameName);
	SDL_SetWindowTitle(app.window, title);
	return true;
}

// The picture.

// The core's LCD grid: how many times larger it makes the picture, 1 when it's off or LCD Rendering
// is on the GPU.
static int coreGridScale(void) {
	if (settings.lcdOnGpu) {
		return 1;
	}
	struct Option* const o = optionFind("armgba_lcd_grid");
	int const scale = o ? atoi(o->value) : 0;
	return scale > 1 ? scale : 1;
}

// The GPU LCD grid: the gaps between the console screen's pixels, drawn at the size the picture is
// shown, each spread over the two output pixels nearest it (lcd.h's libretroLcdGapSpread) and faded
// at small sizes (libretroLcdFade). The first Game Boy's and the Pocket's show the screen's color
// while off there, the Color's and Advance's a black matrix; lcd.h has each screen's values. It's
// blended as in linear light: over black, color c becomes c * (1 - a)^(1 / 2.2), which an ordinary
// blend gives with alpha 1 - (1 - a)^(1 / 2.2); the Game Boy's gaps, near its pixels' colors, take
// the same.
static void makeOverlay(SDL_Rect to) {
	uint32_t gap = 0;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	bool const grid = settings.lcdOnGpu && settings.gpuGrid;
	if (overlay && grid && overlayWidth == app.gameWidth && overlayHeight == app.gameHeight && overlayShownWidth == to.w &&
	    overlayShownHeight == to.h && overlayGap == gap) {
		return;
	}
	if (overlay) {
		SDL_DestroyTexture(overlay);
		overlay = NULL;
	}
	overlayWidth = app.gameWidth;
	overlayHeight = app.gameHeight;
	overlayShownWidth = to.w;
	overlayShownHeight = to.h;
	overlayGap = gap;
	if (!grid || !app.gameWidth || to.w <= 0 || to.h <= 0) {
		return;
	}
	int const w = to.w, h = to.h;
	uint32_t* pixels = calloc((size_t) w * h, 4);
	int* across = calloc((size_t) w, sizeof(int));
	if (!pixels || !across) {
		free(pixels);
		free(across);
		return;
	}
	uint8_t linear[256];
	for (int a = 0; a < 256; ++a) {
		linear[a] = (uint8_t) lroundf((1 - powf(1 - a / 255.0f, 1 / 2.2f)) * 255);
	}
	const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
	// Each column's and row's share of gap as an alpha, 0 to 255; a pixel takes either's.
	float const pitchX = (float) w / app.gameWidth, pitchY = (float) h / app.gameHeight;
	float const fade = libretroLcdFade(fminf(pitchX, pitchY));
	for (int x = 0; x < w; ++x) {
		across[x] = (int) lroundf(libretroLcdGapSpread(x, pitchX, look->share * pitchX) * look->across * fade * 255);
	}
	for (int y = 0; y < h; ++y) {
		int const down = (int) lroundf(libretroLcdGapSpread(y, pitchY, look->share * pitchY) * look->down * fade * 255);
		uint32_t* const row = pixels + (size_t) y * w;
		for (int x = 0; x < w; ++x) {
			row[x] = (uint32_t) linear[across[x] + down - (across[x] * down + 127) / 255] << 24 | (gap & 0xFFFFFF);
		}
	}
	overlay = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
	SDL_UpdateTexture(overlay, NULL, pixels, w * 4);
	SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(overlay, SDL_ScaleModeNearest);
	free(pixels);
	free(across);
}

// The shadow the first Game Boy's and the Pocket's pixels cast on the reflector behind them, the way
// the setting points (lcd.h has how far and how dark): a texture a dot to each of the picture's
// pixels, remade when the picture changes, drawn scaled smoothly over the picture.
static void drawShadow(SDL_Rect to, uint32_t lit) {
	if (!shadow || shadowStale) {
		int w = 0, h = 0;
		if (shadow) {
			SDL_QueryTexture(shadow, NULL, NULL, &w, &h);
		}
		if (!shadow || w != app.gameWidth || h != app.gameHeight) {
			if (shadow) {
				SDL_DestroyTexture(shadow);
			}
			shadow = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, app.gameWidth, app.gameHeight);
			if (!shadow) {
				return;
			}
			SDL_SetTextureBlendMode(shadow, SDL_BLENDMODE_BLEND);
			SDL_SetTextureScaleMode(shadow, SDL_ScaleModeLinear);
		}
		void* pixels;
		int pitch;
		if (SDL_LockTexture(shadow, NULL, &pixels, &pitch) != 0) {
			return;
		}
		// A pixel's darkness, from a sixteenth below the unlit screen, as an alpha: a fifth (51) at black.
		// Game Boy pictures, the only ones with a shadow, are XRGB8888.
		int const litLevel = 2 * (lit >> 16 & 0xFF) + 5 * (lit >> 8 & 0xFF) + (lit & 0xFF), start = litLevel - litLevel / 16;
		int const scale = start ? (LIBRETRO_LCD_SHADOW << 16) / start : 0;
		for (int y = 0; y < app.gameHeight; ++y) {
			const uint32_t* const row = (const uint32_t*) (app.picture + y * app.picturePitch);
			uint32_t* const out = (uint32_t*) ((uint8_t*) pixels + y * pitch);
			for (int x = 0; x < app.gameWidth; ++x) {
				uint32_t const c = row[x];
				int const level = 2 * (int) (c >> 16 & 0xFF) + 5 * (int) (c >> 8 & 0xFF) + (int) (c & 0xFF);
				int const dark = level < start ? (start - level) * scale >> 16 : 0;
				out[x] = (uint32_t) dark << 24;
			}
		}
		SDL_UnlockTexture(shadow);
		shadowStale = false;
	}
	// Up, then on clockwise.
	int const dx = libretroLcdWays[settings.shadow - 1][0], dy = libretroLcdWays[settings.shadow - 1][1];
	float const reach = libretroLcdReach(settings.shadow - 1);
	SDL_Rect const cast = { to.x + (int) lroundf(dx * reach * to.w / app.gameWidth), to.y + (int) lroundf(dy * reach * to.h / app.gameHeight),
		                    to.w, to.h };
	SDL_RenderSetClipRect(app.renderer, &to);
	SDL_RenderCopy(app.renderer, shadow, NULL, &cast);
	SDL_RenderSetClipRect(app.renderer, NULL);
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
		// The console's shape, as large as fits; in whole multiples with Integer Scaling.
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
		makeOverlay(to);
		if (overlay) {
			SDL_RenderCopy(app.renderer, overlay, NULL, &to);
		}
		uint32_t lit = 0;
		enum LibretroLcd const lcd = libretroLcd(&lit);
		if (overlay && settings.shadow && app.picture && app.format == RETRO_PIXEL_FORMAT_XRGB8888 &&
		    (lcd == LIBRETRO_LCD_DMG || lcd == LIBRETRO_LCD_POCKET)) {
			drawShadow(to, lit);
		}
	}
	uiDraw();
	SDL_RenderPresent(app.renderer);
}

// Makes the renderer again, as VSync changes; a software one if the GPU's can't be made.
void recreateRenderer(void) {
	if (app.game) {
		SDL_DestroyTexture(app.game);
		app.game = NULL;
	}
	if (overlay) {
		SDL_DestroyTexture(overlay);
		overlay = NULL;
	}
	if (shadow) {
		SDL_DestroyTexture(shadow);
		shadow = NULL;
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

// Full screen, or a window the console's screen times the window size.
void applyWindowSize(void) {
	SDL_SetWindowFullscreen(app.window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (!settings.fullscreen) {
		// The console's screen times the window size, whatever the core's LCD Grid made the picture.
		int const grid = coreGridScale();
		int const w = app.loaded && app.geometry.base_width ? (int) app.geometry.base_width / grid : 240;
		int const h = app.loaded && app.geometry.base_height ? (int) app.geometry.base_height / grid : 160;
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

// Whether the action being done came from a controller's hotkey, for what its messages name.
static bool actionFromPad;

void requestQuit(bool controller) {
	static Uint32 asked;
	Uint32 const now = SDL_GetTicks();
	if (asked && now - asked < 3000) {
		app.running = false;
		return;
	}
	asked = now;
	if (controller) {
		osd("Press Hotkey + %s again to quit", padName(settings.padHotkeys[ACT_QUIT]));
	} else {
		osd("Press %s again to quit", scancodeName(settings.hotkeys[ACT_QUIT]));
	}
}

// A hotkey pressed. Menu and Full Screen work without a game; Pause and Restart not during netplay.
static void doAction(enum Action action) {
	if (action == ACT_MENU) {
		if (app.menuOpen) {
			uiCloseMenu();
		} else {
			uiOpenMenu();
		}
		return;
	}
	if (action == ACT_QUIT) {
		requestQuit(actionFromPad);
		return;
	}
	if (action == ACT_FPS) {
		settings.showFps = !settings.showFps;
		osd("Show Frame Rate: %s", settings.showFps ? "On" : "Off");
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
	switch (action) {
	case ACT_PAUSE:
		if (!netRunning()) {
			app.paused = !app.paused;
			osd(app.paused ? "Paused" : "Running");
		}
		break;
	case ACT_SAVE_STATE:
	case ACT_LOAD_STATE:
		stateCommand(action == ACT_LOAD_STATE);
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

// A controller button or trigger pressed: the hotkey it makes with the Hotkey held. Fast-Forward
// turns on and off, as on ROCKNIX; Rewind works while held.
static void padPressed(SDL_JoystickID which, int binding) {
	for (int a = 0; a < ACTIONS; ++a) {
		if (a == ACT_REWIND || !padHotkeyPressed(which, binding, a)) {
			continue;
		}
		if (a == ACT_FAST_FORWARD) {
			app.fastForwardOn = !app.fastForwardOn && app.loaded;
			osd("Fast-Forward %s", app.fastForwardOn ? "On" : "Off");
		} else {
			actionFromPad = true;
			doAction((enum Action) a);
			actionFromPad = false;
		}
	}
}

// An event the menus didn't take: quitting, a file dropped on the window to load, controllers
// plugged in or out, hotkeys, and a right-click for the menu.
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
		padPressed(event->cbutton.which, event->cbutton.button);
		break;
	case SDL_CONTROLLERAXISMOTION:
		if (event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
			int const port = portOf(event->caxis.which);
			int const side = event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
			static bool down[PORTS][2];
			bool const now = event->caxis.value > 16384;
			if (port >= 0 && now != down[port][side]) {
				down[port][side] = now;
				if (now) {
					padPressed(event->caxis.which, side ? PAD_RIGHT_TRIGGER : PAD_LEFT_TRIGGER);
				}
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
	// A controller's Fast-Forward turns on and off instead.
	for (int port = 0; port < PORTS && action != ACT_FAST_FORWARD; ++port) {
		SDL_GameController* const pad = app.pads[port];
		int const hotkey = hotkeyOf(pad), second = settings.padHotkeys[action];
		if (second != PAD_NONE && second != hotkey && padHeld(pad, hotkey) && padHeld(pad, second)) {
			heldBack[port] |= padBit(hotkey) | padBit(second);
			held = true;
		}
	}
	return held;
}

// Reads the settings and arguments, opens the window, sound and controllers, then runs the game until
// Quit, writing the saves every five seconds.
int main(int argc, char** argv) {
	const char* game = NULL;
	const char* connectTo = NULL;
	bool listControllers = false;
	int hostMode = 0;
	// The settings, then the arguments over them.
	settingsDefaults();
	dataDirectory(configDirectory, "XDG_CONFIG_HOME", ".config", "");
	dataDirectory(ownSystemDirectory, "XDG_CONFIG_HOME", ".config", "/system");
	findRetroarch();
	dataDirectory(ownSaveDirectory, "XDG_DATA_HOME", ".local/share", "/saves");
	dataDirectory(ownStateDirectory, "XDG_DATA_HOME", ".local/share", "/states");
	dataDirectory(ownShotDirectory, "XDG_DATA_HOME", ".local/share", "/screenshots");
	snprintf(saveDirectory, PATH_SIZE, "%s", ownSaveDirectory);
	dataDirectory(cheatDirectory, "XDG_DATA_HOME", ".local/share", "/cheats");
	settingsRead();
	chooseFolders();
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
		} else if (!strcmp(argv[i], "--controllers")) {
			listControllers = true;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("usage: armgba [--fullscreen] [--scale N] [--host | --host-together | --connect ADDRESS] [--port N] [--controllers] [game]\n\n"
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
	if (listControllers) {
		// Each joystick SDL sees: its GUID, whether a mapping makes it a controller, its name.
		for (int i = 0; i < SDL_NumJoysticks(); ++i) {
			char guid[33];
			SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
			printf("%s %s %s\n", guid, SDL_IsGameController(i) ? "mapped  " : "unmapped", SDL_JoystickNameForIndex(i));
		}
		return 0;
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
	// The core's calls back.
	retro_set_video_refresh(videoRefresh);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);

	// Stereo at the core's rate. Without a sound device the game runs silent.
	SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 2, .samples = 512 }, have;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio) {
		SDL_PauseAudioDevice(audio, 0);
	} else {
		fprintf(stderr, "armgba: no sound: %s\n", SDL_GetError());
	}
	bytesPerFrame = (unsigned) (AUDIO_RATE / FRAME_RATE) * 4;

	// A game given loads, and hosts or joins if asked; without one, the menu opens.
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

	// The main loop: events, then the frames due, then the picture.
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
		app.fastForward = actionHeld(ACT_FAST_FORWARD) || app.fastForwardOn || coreFastForward;
		app.rewinding = settings.rewind && !cheevosHardcore() && !netRunning() && actionHeld(ACT_REWIND);
		cheevosIdle();
		// Paused, in the menus or without a game: only the picture. Netplay runs on behind the menus.
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
		// Fast forward: as many frames as a 60th of a second takes, up to Fast-Forward Speed's, or 12.
		if (app.fastForward && !netRunning()) {
			due = 0;
			Uint64 const until = now + frequency / 60;
			int const most = settings.fastForward ? settings.fastForward : 12;
			while (SDL_GetPerformanceCounter() < until && due < most) {
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
	// Quitting: the game's save, the settings and the options are written.
	unloadGame();
	settingsWrite();
	optionsWrite();
	if (audio) {
		SDL_CloseAudioDevice(audio);
	}
	SDL_Quit();
	return 0;
}
