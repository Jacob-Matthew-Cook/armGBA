/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core. Game Boy Advance games run on armGBA; Game Boy and Game Boy Color games run on
// the Gambatte core in gambatte/ when it is built in (ARMGBA_GB).

#include "libretro.h"

#include "cheats.h"
#include "gba.h"
#ifdef ARMGBA_GB
#include "gb.h"
#include "link.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>

enum {
	AUDIO_RATE = 32768,
};

#define FRAME_RATE (16777216.0 / 280896.0)

static retro_environment_t environment;
static retro_video_refresh_t videoRefresh;
static retro_audio_sample_batch_t audioBatch;
static retro_input_poll_t inputPoll;
static retro_input_state_t inputState;
static retro_log_printf_t logPrintf;

static struct GBA* gba;
static bool skipBios;
// What moves a cartridge's tilt sensor or gyro: 0 is the left stick, 1 the right stick, 2 the
// D-pad.
static int tiltControl;
// Turbo timing: each press-and-release cycle lasts turboPeriod frames, and the button is held for
// the first turboPulse of them.
static unsigned turboPeriod = 4;
static unsigned turboPulse = 2;
// Sunlight level for Boktai's solar sensor, 0 to 10.
static int solarLevel = 5;
// The frontend's rumble callback, if it has one, and whether the motor is currently on.
static retro_set_rumble_state_t setRumble;
static bool rumbling;
// Threaded video: 0 off, 1 on, 2 on with each frame shown one frame late so drawing can overlap the
// next frame.
static int threadedVideo;
// Color correction for the Game Boy Advance's screen: a lookup table from each RGB565 color
// (ignoring green's low bit) to its corrected color.
static bool colorCorrection;
static uint16_t* correctedColors;
static uint16_t correctedFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
#ifdef ARMGBA_GB
static bool gameBoy;
static enum LibretroGbModel gbModel;
static bool gbColorCorrection;
// On the Game Boy Advance model, a Game Boy game is shown as that console shows it: centered on its
// screen, or stretched across it. When cropped, the black border is left out.
static bool gbOnAdvance;
static bool gbStretched;
static bool gbCropped;
static uint32_t gbAdvanceFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
// Both consoles of a Game Boy link can run here: player 1's on the first controller, player 2's on
// the second, the cable between them. A second console started on a copy of the first one's game
// gets a copy of its save before the first frame.
static bool gbPair;
static bool gbPairCopySave;
// Where the two screens go: side by side, one above the other, or one console's alone.
enum { SCREENS_SIDE, SCREENS_STACKED, SCREENS_FIRST, SCREENS_SECOND, SCREENS_COUNT };
static int screens;
static int screensOption = -1;
static uint32_t pairFrame[LIBRETRO_GB_WIDTH * 2 * LIBRETRO_GB_HEIGHT];
static int16_t pairSamples[2 * 2048];
#endif

// Cheat codes from the frontend, by index, and the Game Boy Advance operations they decode to. All
// codes are decoded again, in order, whenever one changes, because a master code changes how the
// codes after it decode.
static char** cheatCodes;
static size_t cheatCodeCount;
static struct CheatSet cheats;
#ifdef ARMGBA_GB
// Whether the memory map given to the frontend is a Game Boy Color's, which has more work RAM.
static bool gbMapColor;

// Two Game Boy games loaded together, one per console, each with its own save and clock files.
enum {
	SUBSYSTEM_LINK = 1,
	MEMORY_SECOND_SAVE = 0x100,
	MEMORY_SECOND_RTC = 0x101,
};
#endif

// Writes a line to the frontend's log, or to stderr if it has none.
static void logMessage(enum retro_log_level level, const char* message) {
	if (logPrintf) {
		logPrintf(level, "%s\n", message);
	} else {
		fprintf(stderr, "%s\n", message);
	}
}

// Shows a message on screen for a few seconds and logs it.
static void showMessage(const char* message) {
	struct retro_message shown = { message, 180 };
	environment(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
	logMessage(RETRO_LOG_ERROR, message);
}

// Tells the frontend when the rumble motor turns on or off.
static void updateRumble(bool on) {
	if (on != rumbling && setRumble) {
		setRumble(0, RETRO_RUMBLE_STRONG, on ? 0xFFFF : 0);
		setRumble(0, RETRO_RUMBLE_WEAK, on ? 0xFFFF : 0);
	}
	rumbling = on;
}

// Stores the frontend's callbacks and declares this core's options.
void retro_set_environment(retro_environment_t callback) {
	environment = callback;
#ifdef ARMGBA_GB
	static const struct retro_subsystem_memory_info firstMemory[] = { { "srm", RETRO_MEMORY_SAVE_RAM }, { "rtc", RETRO_MEMORY_RTC } };
	static const struct retro_subsystem_memory_info secondMemory[] = { { "srm", MEMORY_SECOND_SAVE }, { "rtc", MEMORY_SECOND_RTC } };
	static const struct retro_subsystem_rom_info roms[] = {
		{ "Player 1's Game", "gb|gbc", false, false, true, firstMemory, 2 },
		{ "Player 2's Game", "gb|gbc", false, false, true, secondMemory, 2 },
	};
	static const struct retro_subsystem_info subsystems[] = {
		{ "Two Game Boys (Link Cable)", "gblink", roms, 2, SUBSYSTEM_LINK },
		{ NULL, NULL, NULL, 0, 0 },
	};
	callback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*) subsystems);
#endif
	struct retro_log_callback logging;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
		logPrintf = logging.log;
	}
	// Option keys and values match saved configurations; the labels are what the menu shows. Each
	// game sees only its own console's options, listed in order of use: turbo first, then starting
	// and running, colors, and controls.
	static struct retro_core_option_v2_definition definitions[] = {
		{ "armgba_turbo_period", "Turbo Button Period", NULL,
		  "How many frames apart X and Y press A and B while held, and L3 and R3 press L and R in Game Boy Advance games, as in gambatte's and gpSP's cores.", NULL, NULL,
		  {
		    { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", NULL }, { "11", NULL }, { "12", NULL }, { "13", NULL }, { "14", NULL }, { "15", NULL },
		    { "16", NULL }, { "17", NULL }, { "18", NULL }, { "19", NULL }, { "20", NULL }, { "21", NULL }, { "22", NULL }, { "23", NULL }, { "24", NULL }, { "25", NULL }, { "26", NULL }, { "27", NULL },
		    { "28", NULL }, { "29", NULL }, { "30", NULL }, { "31", NULL }, { "32", NULL }, { "33", NULL }, { "34", NULL }, { "35", NULL }, { "36", NULL }, { "37", NULL }, { "38", NULL }, { "39", NULL },
		    { "40", NULL }, { "41", NULL }, { "42", NULL }, { "43", NULL }, { "44", NULL }, { "45", NULL }, { "46", NULL }, { "47", NULL }, { "48", NULL }, { "49", NULL }, { "50", NULL }, { "51", NULL },
		    { "52", NULL }, { "53", NULL }, { "54", NULL }, { "55", NULL }, { "56", NULL }, { "57", NULL }, { "58", NULL }, { "59", NULL }, { "60", NULL }, { "61", NULL }, { "62", NULL }, { "63", NULL },
		    { "64", NULL }, { "65", NULL }, { "66", NULL }, { "67", NULL }, { "68", NULL }, { "69", NULL }, { "70", NULL }, { "71", NULL }, { "72", NULL }, { "73", NULL }, { "74", NULL }, { "75", NULL },
		    { "76", NULL }, { "77", NULL }, { "78", NULL }, { "79", NULL }, { "80", NULL }, { "81", NULL }, { "82", NULL }, { "83", NULL }, { "84", NULL }, { "85", NULL }, { "86", NULL }, { "87", NULL },
		    { "88", NULL }, { "89", NULL }, { "90", NULL }, { "91", NULL }, { "92", NULL }, { "93", NULL }, { "94", NULL }, { "95", NULL }, { "96", NULL }, { "97", NULL }, { "98", NULL }, { "99", NULL },
		    { "100", NULL }, { "101", NULL }, { "102", NULL }, { "103", NULL }, { "104", NULL }, { "105", NULL }, { "106", NULL }, { "107", NULL }, { "108", NULL }, { "109", NULL }, { "110", NULL }, { "111", NULL },
		    { "112", NULL }, { "113", NULL }, { "114", NULL }, { "115", NULL }, { "116", NULL }, { "117", NULL }, { "118", NULL }, { "119", NULL }, { "120", NULL }, { NULL, NULL }
		  },
		  "4" },
		{ "armgba_skip_bios", "Skip BIOS Intro", NULL, "Start games without the Game Boy Advance logo.", NULL, NULL,
		  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
		{ "armgba_threaded_video", "Threaded Video", NULL,
		  "Draw the picture on a second CPU core. Helps fast-forward and runahead on slower devices, and uses a little more power. One Frame Late is faster still, and shows each frame one frame late.", NULL, NULL,
		  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { "one frame late", "One Frame Late" }, { NULL, NULL } }, "disabled" },
#ifdef ARMGBA_GB
		{ "armgba_gb_model", "Game Boy Model", NULL,
		  "The console Game Boy and Game Boy Color games run on. On the Game Boy Advance, L or R stretches the picture across its screen. Applies on restart.",
		  NULL, NULL,
		  { { "Auto", NULL }, { "Game Boy", NULL }, { "Game Boy Pocket", "Game Boy Pocket / Light" }, { "Game Boy Color", NULL }, { "Game Boy Advance", NULL }, { NULL, NULL } }, "Auto" },
		{ "armgba_gb_boot_rom", "Boot ROM", NULL,
		  "Start through the console's logo: On uses Nintendo's boot ROM when the system directory has it, Open-Source SameBoy's free one built into the core. Applies on restart.",
		  NULL, NULL,
		  { { "enabled", "On" }, { "disabled", "Off" }, { "open-source", "Open-Source" }, { NULL, NULL } }, "enabled" },
#endif
		{ "armgba_color_correction", "Color Correction", NULL, "Darker, softer colors, as the Game Boy Advance's screen showed them. Costs a little speed.", NULL, NULL,
		  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
#ifdef ARMGBA_GB
		{ "armgba_gb_color_correction", "Color Correction", NULL,
		  "Colors as the Game Boy Color's screen showed them. Not used for games running on an original Game Boy.", NULL, NULL,
		  { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } }, "disabled" },
		{ "armgba_gb_screen", "Game Boy Screen", NULL, "The colors original Game Boy games are shown in: plain grays, the first model's green, the Pocket's olive gray, or the Light's backlit teal.", NULL, NULL,
		  { { "grayscale", "Grayscale" }, { "original", "Game Boy (Green)" }, { "pocket", "Game Boy Pocket (Olive)" }, { "light", "Game Boy Light (Teal)" }, { NULL, NULL } }, "grayscale" },
		{ "armgba_gb_advance_border", "Game Boy Advance Border", NULL,
		  "On the Game Boy Advance model: Cropped shows only the game's picture, Shown the black around it as that console's screen has.", NULL, NULL,
		  { { "cropped", "Cropped" }, { "shown", "Shown" }, { NULL, NULL } }, "cropped" },
		{ "armgba_link_cable", "Link Cable", NULL,
		  "How Game Boy games link in netplay. Own Console: each player plays their own game and save, and the cable crosses the network, which stalls on a slow one. Both Consoles: every device runs both players' Game Boys, smooth with RetroArch's rollback, from the host's games and saves; it also plays two-player on one device, player 2 on the second controller. Applies when a game starts.",
		  NULL, NULL,
		  { { "own", "Own Console" }, { "both", "Both Consoles" }, { NULL, NULL } }, "own" },
		{ "armgba_link_screens", "Both Consoles Screens", NULL,
		  "With Both Consoles: where the two screens go. L2 and R2 change it while playing.", NULL, NULL,
		  { { "side by side", "Side by Side" }, { "top and bottom", "Top and Bottom" }, { "player 1", "Player 1 Only" }, { "player 2", "Player 2 Only" }, { NULL, NULL } },
		  "side by side" },
		{ "armgba_gb_screen_off", "Screen Off Behavior", NULL,
		  "What shows while a game has its screen off, as between menus: what the console showed (white on the Game Boy Color and Advance), or the game's background color, which spares OLED screens a bright flash.", NULL, NULL,
		  { { "hardware", "As Hardware" }, { "background", "Background Color" }, { NULL, NULL } }, "hardware" },
#endif
		{ "armgba_gb_tilt", "Tilt Sensor", NULL,
		  "What tilts or turns the cartridge in games with a motion sensor: Kirby Tilt 'n' Tumble, Yoshi Topsy-Turvy, WarioWare: Twisted!, where turning is left and right.",
		  NULL, NULL,
		  { { "left stick", "Left Stick" }, { "right stick", "Right Stick" }, { "d-pad", "D-Pad" }, { NULL, NULL } }, "left stick" },
		{ "armgba_solar_sensor", "Solar Sensor", NULL,
		  "The sunlight Boktai's sensor sees, from none to full sun. The right stick, pushed up or down, changes it while playing.", NULL, NULL,
		  { { "0", "0 (Dark)" }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", "10 (Full Sun)" }, { NULL, NULL } }, "5" },
		{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
	};
	static struct retro_core_options_v2 options = { NULL, definitions };
	unsigned version = 0;
	if (callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
		return;
	}
	static const struct retro_variable variables[] = {
		{ "armgba_turbo_period", "Turbo Button Period; 4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60|61|62|63|64|65|66|67|68|69|70|71|72|73|74|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99|100|101|102|103|104|105|106|107|108|109|110|111|112|113|114|115|116|117|118|119|120" },
		{ "armgba_skip_bios", "Skip BIOS Intro; disabled|enabled" },
		{ "armgba_threaded_video", "Threaded Video; disabled|enabled|one frame late" },
#ifdef ARMGBA_GB
		{ "armgba_gb_model", "Game Boy Model; Auto|Game Boy|Game Boy Pocket|Game Boy Color|Game Boy Advance" },
		{ "armgba_gb_boot_rom", "Boot ROM; enabled|disabled|open-source" },
#endif
		{ "armgba_color_correction", "Color Correction; disabled|enabled" },
#ifdef ARMGBA_GB
		{ "armgba_gb_color_correction", "Color Correction; disabled|enabled" },
		{ "armgba_gb_screen", "Game Boy Screen; grayscale|original|pocket|light" },
		{ "armgba_gb_advance_border", "Game Boy Advance Border; cropped|shown" },
		{ "armgba_link_cable", "Link Cable; own|both" },
		{ "armgba_link_screens", "Both Consoles Screens; side by side|top and bottom|player 1|player 2" },
		{ "armgba_gb_screen_off", "Screen Off Behavior; hardware|background" },
#endif
		{ "armgba_gb_tilt", "Tilt Sensor; left stick|right stick|d-pad" },
		{ "armgba_solar_sensor", "Solar Sensor; 5|6|7|8|9|10|0|1|2|3|4" },
		{ NULL, NULL },
	};
	callback(RETRO_ENVIRONMENT_SET_VARIABLES, (void*) variables);
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
	videoRefresh = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
	audioBatch = callback;
}

void retro_set_input_poll(retro_input_poll_t callback) {
	inputPoll = callback;
}

void retro_set_input_state(retro_input_state_t callback) {
	inputState = callback;
}

// The libretro API version this core implements.
unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

// Reports the core's name, version and the file types it loads.
void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armGBA";
	info->library_version = "0.1";
#ifdef ARMGBA_GB
	info->valid_extensions = "gba|bin|gb|gbc";
#else
	info->valid_extensions = "gba|bin";
#endif
	info->need_fullpath = false;
	info->block_extract = false;
}

// The picture size: 240x160, or 160x144 for a Game Boy game, or the two screens of both consoles. On the Game Boy Advance model, a Game
// Boy game is 240x160 with the border shown, or 160x144 cropped (240x144 when stretched).
static void geometry(struct retro_game_geometry* geometry) {
	geometry->base_width = SCREEN_WIDTH;
	geometry->base_height = SCREEN_HEIGHT;
	geometry->max_width = SCREEN_WIDTH;
	geometry->max_height = SCREEN_HEIGHT;
	geometry->aspect_ratio = 3.0f / 2.0f;
#ifdef ARMGBA_GB
	if (gameBoy && !gbOnAdvance) {
		geometry->base_width = LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = 10.0f / 9.0f;
	}
	if (gameBoy && gbOnAdvance && gbCropped) {
		geometry->base_width = gbStretched ? SCREEN_WIDTH : LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = gbStretched ? 5.0f / 3.0f : 10.0f / 9.0f;
	}
	if (gameBoy && gbPair) {
		geometry->max_width = LIBRETRO_GB_WIDTH * 2;
		geometry->max_height = LIBRETRO_GB_HEIGHT * 2;
		geometry->base_width = screens == SCREENS_SIDE ? LIBRETRO_GB_WIDTH * 2 : LIBRETRO_GB_WIDTH;
		geometry->base_height = screens == SCREENS_STACKED ? LIBRETRO_GB_HEIGHT * 2 : LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = (float) geometry->base_width / geometry->base_height;
	}
#endif
}

#ifdef ARMGBA_GB
// Tells the frontend the picture size has changed.
static void announceGeometry(void) {
	struct retro_game_geometry changed;
	geometry(&changed);
	environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &changed);
}
#endif

// Reports the picture size and timing for the loaded game, at the consoles' own frame rates.
void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
#ifdef ARMGBA_GB
	gbOnAdvance = gameBoy && !gbPair && libretroGbOnAdvance();
#endif
	geometry(&info->geometry);
	// A Game Boy frame lasts as long as a Game Boy Advance frame.
	info->timing.fps = FRAME_RATE;
	info->timing.sample_rate = AUDIO_RATE;
}

// Nothing needs setting up before a game is loaded.
void retro_init(void) {
	cheatsInit(&cheats);
	linkSetEnvironment(environment);
}

// Frees what the core allocated once the frontend is done with it.
void retro_deinit(void) {
	free(correctedColors);
	correctedColors = NULL;
	colorCorrection = false;
}

// The core always uses one controller.
void retro_set_controller_port_device(unsigned port, unsigned device) {
}

// Builds the color correction table from Pokefan531's measurements of the screen: its primaries
// mixed in linear light, at a gamma of 2.2.
static bool buildCorrectedColors(void) {
	if (correctedColors) {
		return true;
	}
	correctedColors = malloc(0x8000 * sizeof(*correctedColors));
	if (!correctedColors) {
		return false;
	}
	static const float mix[3][3] = {
		{ 0.80f, 0.275f, -0.075f },
		{ 0.135f, 0.64f, 0.225f },
		{ 0.195f, 0.155f, 0.65f },
	};
	float linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = powf(i / 31.0f, 2.2f);
	}
	for (int i = 0; i < 0x8000; ++i) {
		float in[3] = { linear[i >> 10], linear[(i >> 5) & 0x1F], linear[i & 0x1F] };
		int out[3];
		for (int c = 0; c < 3; ++c) {
			float value = mix[c][0] * in[0] + mix[c][1] * in[1] + mix[c][2] * in[2];
			value = value < 0 ? 0 : value > 1 ? 1 : value;
			out[c] = (int) (powf(value, 1 / 2.2f) * (c == 1 ? 63 : 31) + 0.5f);
		}
		correctedColors[i] = out[0] << 11 | out[1] << 5 | out[2];
	}
	return true;
}

// Returns an option's current value, or NULL if the frontend has none.
static const char* option(const char* key) {
	struct retro_variable variable = { key, NULL };
	return environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : NULL;
}

static void showButtons(bool forGameBoy);

// Whether the loaded game is a Game Boy one.
static bool gameBoyLoaded(void) {
#ifdef ARMGBA_GB
	return gameBoy;
#else
	return false;
#endif
}

// Reads every option and applies the ones that can change while a game runs.
static void readOptions(void) {
	const char* value = option("armgba_skip_bios");
	skipBios = value && !strcmp(value, "enabled");
	value = option("armgba_threaded_video");
	int threaded = !value ? 0 : !strcmp(value, "enabled") ? 1 : !strcmp(value, "one frame late") ? 2 : 0;
	if (gba && threaded != threadedVideo) {
		renderThreadStop(gba);
		if (threaded) {
			renderThreadStart(gba, threaded == 2);
		}
	}
	threadedVideo = threaded;
	value = option("armgba_color_correction");
	colorCorrection = value && !strcmp(value, "enabled") && buildCorrectedColors();
#ifdef ARMGBA_GB
	value = option("armgba_gb_model");
	gbModel = !value                               ? LIBRETRO_GB_AUTO
	        : !strcmp(value, "Game Boy")            ? LIBRETRO_GB_DMG
	        : !strcmp(value, "Game Boy Pocket")     ? LIBRETRO_GB_MGB
	        : !strcmp(value, "Game Boy Color")      ? LIBRETRO_GB_CGB
	        : !strcmp(value, "Game Boy Advance")    ? LIBRETRO_GB_AGB
	                                                : LIBRETRO_GB_AUTO;
	value = option("armgba_gb_boot_rom");
	libretroGbSetBootRom(value && !strcmp(value, "disabled")      ? LIBRETRO_GB_BOOT_OFF
	                     : value && !strcmp(value, "open-source") ? LIBRETRO_GB_BOOT_SAMEBOY
	                                                              : LIBRETRO_GB_BOOT_NINTENDO);
	value = option("armgba_gb_screen");
	int const shades = !value ? 0 : !strcmp(value, "pocket") ? 1 : !strcmp(value, "original") ? 2 : !strcmp(value, "light") ? 3 : 0;
	value = option("armgba_gb_color_correction");
	gbColorCorrection = value && !strcmp(value, "enabled");
	value = option("armgba_gb_screen_off");
	bool const offBackground = value && !strcmp(value, "background");
	for (int i = gbPair; i >= 0; --i) {
		libretroGbSelect(i);
		libretroGbSetScreen(shades);
		if (gameBoy) {
			libretroGbSetColorCorrection(gbColorCorrection);
		}
		libretroGbSetScreenOffBackground(offBackground);
	}
	value = option("armgba_link_screens");
	int const chosenScreens = !value ? SCREENS_SIDE : !strcmp(value, "top and bottom") ? SCREENS_STACKED : !strcmp(value, "player 1") ? SCREENS_FIRST
	                        : !strcmp(value, "player 2") ? SCREENS_SECOND : SCREENS_SIDE;
	// L2 and R2 change the screens too; the option wins only when it changes.
	if (chosenScreens != screensOption) {
		screensOption = screens = chosenScreens;
		if (gameBoy && gbPair) {
			announceGeometry();
		}
	}
	value = option("armgba_gb_advance_border");
	bool const cropped = !value || strcmp(value, "shown");
	if (cropped != gbCropped) {
		gbCropped = cropped;
		if (gameBoy && gbOnAdvance) {
			announceGeometry();
		}
	}
#endif
	value = option("armgba_turbo_period");
	turboPeriod = value ? atoi(value) : 4;
	turboPeriod = turboPeriod < 4 ? 4 : turboPeriod > 120 ? 120 : turboPeriod;
	turboPulse = turboPeriod / 2 < 2 ? 2 : turboPeriod / 2 > 15 ? 15 : turboPeriod / 2;
	value = option("armgba_gb_tilt");
	int const tilt = !value ? 0 : !strcmp(value, "right stick") ? 1 : !strcmp(value, "d-pad") ? 2 : 0;
	if (tilt != tiltControl) {
		tiltControl = tilt;
		if (gba || gameBoyLoaded()) {
			showButtons(gameBoyLoaded());
		}
	}
	value = option("armgba_solar_sensor");
	if (value) {
		solarLevel = atoi(value);
	}
}

// Loads gba_bios.bin from the system directory.
static bool loadBios(void) {
	const char* directory = NULL;
	if (!environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) || !directory) {
		showMessage("armGBA: no system directory for gba_bios.bin");
		return false;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/gba_bios.bin", directory);
	FILE* f = fopen(path, "rb");
	uint8_t bios[SIZE_BIOS];
	bool read = f && fread(bios, 1, sizeof(bios), f) == sizeof(bios);
	if (f) {
		fclose(f);
	}
	if (!read) {
		showMessage("armGBA needs gba_bios.bin in the system directory");
		return false;
	}
	return gbaLoadBios(gba, bios, sizeof(bios));
}

// Tells the frontend what each button does for the loaded console, including the stick, D-pad or
// click that moves a cartridge's tilt sensor, wherever the Tilt Sensor option puts it.
static void showButtons(bool forGameBoy) {
	static struct retro_input_descriptor shown[40];
	size_t n = 0;
#define BUTTON(device, index, id, name) shown[n++] = (struct retro_input_descriptor) { port, device, index, id, name }
	unsigned port = 0;
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B");
#ifdef ARMGBA_GB
	if (gbPair) {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Previous Screens");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Next Screens");
	} else
#endif
	{
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Fast Forward");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Fast Forward");
	}
	if (forGameBoy) {
		// A Game Boy has no L or R; on the Game Boy Advance model they stretch the picture. With
		// the tilt sensor on the D-pad, L is the flick.
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, tiltControl == 2 ? "Flick (Motion Sensor)" : "Stretch (Game Boy Advance Model)");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Stretch (Game Boy Advance Model)");
	} else {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Turbo L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Turbo R");
	}
	if (tiltControl < 2) {
		unsigned const stick = tiltControl ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X, "Tilt / Turn (Motion Sensor)");
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y,
		       !forGameBoy && tiltControl == 1 ? "Tilt Up / Down, Sunlight" : "Tilt Up / Down (Motion Sensor)");
		if (forGameBoy) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, tiltControl ? RETRO_DEVICE_ID_JOYPAD_R3 : RETRO_DEVICE_ID_JOYPAD_L3, "Flick (Motion Sensor)");
		}
	}
	if (!forGameBoy && tiltControl != 1) {
		BUTTON(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Sunlight (Solar Sensor)");
	}
#ifdef ARMGBA_GB
	// Player 2's console, on the second controller.
	if (gbPair) {
		port = 1;
		static const unsigned ids[12] = {
			RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_RIGHT,
			RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
			RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
		};
		for (int i = 0; i < 12; ++i) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, ids[i], shown[i].description);
		}
	}
#endif
#undef BUTTON
	shown[n] = (struct retro_input_descriptor) { 0, 0, 0, 0, NULL };
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, shown);
}

// Shows only the options of the loaded game's console, and names its buttons.
static void showOptions(bool forGameBoy) {
	static const char* const advance[] = { "armgba_skip_bios", "armgba_threaded_video", "armgba_color_correction", "armgba_solar_sensor" };
	static const char* const gameBoyKeys[] = { "armgba_gb_model", "armgba_gb_boot_rom", "armgba_gb_color_correction", "armgba_gb_screen", "armgba_gb_advance_border", "armgba_gb_screen_off",
		                                       "armgba_link_cable", "armgba_link_screens" };
	for (unsigned i = 0; i < sizeof(advance) / sizeof(advance[0]); ++i) {
		struct retro_core_option_display display = { advance[i], !forGameBoy };
		environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	for (unsigned i = 0; i < sizeof(gameBoyKeys) / sizeof(gameBoyKeys[0]); ++i) {
		struct retro_core_option_display display = { gameBoyKeys[i], forGameBoy };
		environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	showButtons(forGameBoy);
}

#ifdef ARMGBA_GB
// Describes memory for the frontend's achievements and cheat search, the same way mGBA's and
// gambatte-libretro's cores do.
static void announceMemory(void) {
	struct retro_memory_descriptor descriptors[11];
	size_t count = 0;
#ifdef ARMGBA_GB
	if (gameBoy) {
		count = libretroGbMemoryMap(descriptors);
		gbMapColor = libretroGbIsColor();
	}
#endif
	if (!count && gba) {
		size_t romSize = gba->romSize + (gba->romSize & 1);
		const struct retro_memory_descriptor advance[] = {
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->iwram, 0, 0x03000000, 0xFF000000, 0, SIZE_IWRAM, NULL },
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->ewram, 0, 0x02000000, 0xFF000000, 0, SIZE_EWRAM, NULL },
			{ 0, gba->savedata.storage, 0, 0x0E000000, 0, 0, retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x08000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0A000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0C000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->bios, 0, 0x00000000, 0, 0, SIZE_BIOS, NULL },
			{ 0, gba->vram, 0, 0x06000000, 0xFF000000, 0, SIZE_VRAM, NULL },
			{ 0, gba->palette, 0, 0x05000000, 0xFF000000, 0, SIZE_PALETTE, NULL },
			{ 0, gba->oam, 0, 0x07000000, 0xFF000000, 0, SIZE_OAM, NULL },
			{ 0, gba->io, 0, 0x04000000, 0, 0, SIZE_IO, NULL },
		};
		count = sizeof(advance) / sizeof(advance[0]);
		memcpy(descriptors, advance, sizeof(advance));
	}
	struct retro_memory_map map = { descriptors, count };
	environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool yes = true;
	environment(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
}

#ifdef ARMGBA_GB
// A restart or a loaded state can switch the machine to or from a Game Boy Color, which changes the
// memory map.
static void checkGameBoyMemory(void) {
	if (gbMapColor != libretroGbIsColor()) {
		announceMemory();
	}
}
#endif

// Game Boy and Game Boy Color games are told apart from Game Boy Advance games by file extension.
static bool isGameBoy(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

// Starts a Game Boy or Game Boy Color game on the Gambatte side. With a second game, or with the
// Link Cable option on Both Consoles, both consoles of a link run here, and the network cable is
// left out so RetroArch's own netplay runs them.
static bool loadGameBoy(const struct retro_game_info* game, const struct retro_game_info* second) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no XRGB8888");
		return false;
	}
	const char* directory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
	const char* value = option("armgba_link_cable");
	gbPair = second || (value && !strcmp(value, "both"));
	gbPairCopySave = gbPair && !second;
	libretroGbSetPair(gbPair);
	readOptions();
	char error[256];
	for (int i = gbPair; i >= 0; --i) {
		const struct retro_game_info* const loaded = i && second ? second : game;
		libretroGbSelect(i);
		if (!libretroGbLoad(loaded->data, loaded->size, directory, gbModel, gbColorCorrection, AUDIO_RATE, error, sizeof(error))) {
			showMessage(error);
			libretroGbUnload();
			libretroGbSelect(0);
			libretroGbUnload();
			gbPair = false;
			libretroGbSetPair(false);
			return false;
		}
	}
	gameBoy = true;
	gbStretched = false;
	if (gbPair) {
		environment(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, NULL);
	}
	linkSetGameBoy(!gbPair);
	showOptions(true);
	announceMemory();
	return true;
}
#endif

// Starts a game on the side it belongs to.
bool retro_load_game(const struct retro_game_info* game) {
	if (!game || !game->data) {
		return false;
	}
#ifdef ARMGBA_GB
	gameBoy = false;
	if (isGameBoy(game->path)) {
		return loadGameBoy(game, NULL);
	}
#endif
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no RGB565");
		return false;
	}
	gba = gbaCreate();
	if (!gba) {
		return false;
	}
	// RetroArch loads the save file into save memory after this, and writes it back from there.
	gba->savedata.backed = true;
	gba->renderer.rgb565 = true;
	if (!loadBios() || !gbaLoadRom(gba, game->data, game->size)) {
		gbaDestroy(gba);
		gba = NULL;
		return false;
	}
	showOptions(false);
	threadedVideo = 0;
	readOptions();
	gbaReset(gba, skipBios);
	linkSetGba(gba);
	announceMemory();
	struct retro_rumble_interface rumble = { NULL };
	setRumble = gba->gpio.devices & HW_RUMBLE && environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) ? rumble.set_rumble_state : NULL;
	rumbling = false;
	return true;
}

// Two Game Boy games, one for each console of a link.
bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
#ifdef ARMGBA_GB
	if (type == SUBSYSTEM_LINK && count == 2 && info[0].data && info[1].data && isGameBoy(info[0].path) && isGameBoy(info[1].path)) {
		gameBoy = false;
		return loadGameBoy(&info[0], &info[1]);
	}
#endif
	return false;
}

// Frees whichever machine was running, and its cheat codes.
void retro_unload_game(void) {
	updateRumble(false);
	cheatsClear(&cheats, NULL);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
#ifdef ARMGBA_GB
	if (gameBoy) {
		for (int i = gbPair; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbUnload();
		}
		gameBoy = false;
		gbPair = false;
		libretroGbSetPair(false);
	}
#endif
	linkSetGba(NULL);
	if (gba) {
		gbaDestroy(gba);
		gba = NULL;
	}
}

// The frontend's Restart.
void retro_reset(void) {
	// Restart can come straight after an option change, before a frame has read it, so read the
	// options first.
	readOptions();
#ifdef ARMGBA_GB
	if (gameBoy) {
		for (int i = gbPair; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbReset(gbModel);
		}
		libretroGbSetPair(gbPair);
		checkGameBoyMemory();
		return;
	}
#endif
	gbaReset(gba, skipBios);
}

// Reads the motion sensor control: a stick, or the D-pad pushed all the way, giving -0x7FFF to
// 0x7FFF on each axis. Also reports whether the stick is clicked, or L is held when using the
// D-pad.
static void readMotion(uint16_t keys, int* x, int* y, bool* click) {
	if (tiltControl == 2) {
		*x = keys & 0x10 ? 0x7FFF : keys & 0x20 ? -0x7FFF : 0;
		*y = keys & 0x80 ? 0x7FFF : keys & 0x40 ? -0x7FFF : 0;
		*click = keys & 0x200;
	} else {
		unsigned stick = tiltControl ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		*x = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X);
		*y = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y);
		*click = inputState(0, RETRO_DEVICE_JOYPAD, 0, tiltControl ? RETRO_DEVICE_ID_JOYPAD_R3 : RETRO_DEVICE_ID_JOYPAD_L3);
	}
}

#ifdef ARMGBA_GB
// Feeds the tilt sensor of a Game Boy cartridge that has one (Kirby Tilt 'n' Tumble). Clicking the
// stick flicks the console upwards, which is how Kirby jumps.
static void readTilt(uint16_t keys) {
	static bool held;
	int x;
	int y;
	bool click;
	readMotion(keys, &x, &y, &click);
	if (click && !held) {
		libretroGbFlick();
	}
	held = click;
	libretroGbSetTilt(x, y);
}
#endif

// Feeds a Game Boy Advance cartridge's sensors before each frame. The tilt sensor and the gyro
// (whose turn is the control's left and right) are scaled as mGBA's SDL frontend scales a stick.
// The light sensor uses mGBA's levels, and pushing the right stick up or down changes the level by
// one step.
static void readSensors(uint16_t keys) {
	int devices = gba->gpio.devices;
	if (devices & (HW_TILT | HW_GYRO)) {
		int x;
		int y;
		bool click;
		readMotion(keys, &x, &y, &click);
		gba->gpio.tiltInputX = x * 0x3800;
		gba->gpio.tiltInputY = y * 0x3800;
		gba->gpio.gyroZ = x * 11000;
	}
	if (devices & HW_LIGHT_SENSOR) {
		static const int levels[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };
		static bool held;
		int const push = inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		bool brighter = push < -0x4000;
		bool darker = push > 0x4000;
		if ((brighter || darker) && !held) {
			solarLevel += brighter ? (solarLevel < 10) : -(solarLevel > 0);
			char message[32];
			snprintf(message, sizeof(message), "Sunlight %d of 10", solarLevel);
			struct retro_message shown = { message, 120 };
			environment(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
		}
		held = brighter || darker;
		gba->gpio.luminance = 0xFF - (0x16 + (solarLevel ? levels[solarLevel - 1] : 0));
	}
}

// Returns whether a held turbo button presses its button this frame: pressed for the first frames
// of every turbo period, as in gambatte's core.
static bool turbo(unsigned port, unsigned button, unsigned* counter) {
	if (!inputState(port, RETRO_DEVICE_JOYPAD, 0, button)) {
		*counter = 0;
		return false;
	}
	bool const down = *counter < turboPulse;
	*counter = *counter + 1 >= turboPeriod ? 0 : *counter + 1;
	return down;
}

// Turbo L and R: L3 and R3 held press a Game Boy Advance game's L and R.
static uint16_t readTurboShoulders(void) {
	static unsigned turboL, turboR;
	return (turbo(0, RETRO_DEVICE_ID_JOYPAD_L3, &turboL) ? 0x200 : 0) | (turbo(0, RETRO_DEVICE_ID_JOYPAD_R3, &turboR) ? 0x100 : 0);
}

// Returns the buttons held on a controller, one bit each in the order of the GBA's KEYINPUT
// register.
static uint16_t readKeys(unsigned port) {
	static const unsigned buttons[10] = {
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_SELECT,
		RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_RIGHT,
		RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_UP,
		RETRO_DEVICE_ID_JOYPAD_DOWN,
		RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_L,
	};
	uint16_t keys = 0;
	for (int i = 0; i < 10; ++i) {
		if (inputState(port, RETRO_DEVICE_JOYPAD, 0, buttons[i])) {
			keys |= 1 << i;
		}
	}
	// Turbo A and B: X and Y held press A and B.
	static unsigned turboA[2], turboB[2];
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_X, &turboA[port]) ? 0x1 : 0;
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_Y, &turboB[port]) ? 0x2 : 0;
	// As in mGBA's core by default, pressing opposite directions together presses neither.
	if ((keys & 0x30) == 0x30) {
		keys &= ~0x30;
	}
	if ((keys & 0xC0) == 0xC0) {
		keys &= ~0xC0;
	}
	return keys;
}

#ifdef ARMGBA_GB
// On the Game Boy Advance model, L or R switches between the game's own size and stretched across
// the screen, like that console's shoulder buttons. With the tilt sensor on the D-pad, only R does,
// since L is the flick.
static void readStretch(uint16_t keys) {
	static bool held;
	bool now = keys & (tiltControl == 2 ? 0x100 : 0x300);
	if (now && !held) {
		gbStretched = !gbStretched;
		if (gbCropped) {
			announceGeometry();
		}
	}
	held = now;
}

// Returns the picture as a Game Boy Advance shows it, on black: centered on its 240x160 screen, or
// stretched to 240x144 by doubling every other column.
static const uint32_t* advanceFrame(const uint32_t* picture) {
	uint32_t* row = gbAdvanceFrame + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH;
	int const border = (SCREEN_WIDTH - LIBRETRO_GB_WIDTH) / 2;
	for (int y = 0; y < LIBRETRO_GB_HEIGHT; ++y, row += SCREEN_WIDTH, picture += LIBRETRO_GB_WIDTH) {
		if (gbStretched) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				row[x] = picture[x * 2 / 3];
			}
		} else {
			memset(row, 0, border * sizeof(*row));
			memcpy(row + border, picture, LIBRETRO_GB_WIDTH * sizeof(*row));
			memset(row + border + LIBRETRO_GB_WIDTH, 0, border * sizeof(*row));
		}
	}
	return gbAdvanceFrame;
}
#endif

// Holding L2 or R2 fast-forwards at the frontend's fast-forward speed (its Maximum Run Speed), as
// in gpSP's core.
static void readFastForward(void) {
	static bool held;
	// Not in netplay: the other players' consoles would wait for this one's. With both consoles
	// here, L2 and R2 change the screens instead.
	bool now = !linkSession() && !gbPair && (inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) || inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2));
	if (now == held) {
		return;
	}
	held = now;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = now, .notification = true, .inhibit_toggle = now };
	environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

#ifdef ARMGBA_GB
// With both consoles here, R2 on either controller moves to the next way of showing the screens and
// L2 to the one before.
static void readScreensKeys(void) {
	static bool held[2][2];
	static const char* const names[SCREENS_COUNT] = { "Side by Side", "Top and Bottom", "Player 1 Only", "Player 2 Only" };
	int step = 0;
	for (unsigned port = 0; port < 2; ++port) {
		for (int k = 0; k < 2; ++k) {
			bool const now = inputState(port, RETRO_DEVICE_JOYPAD, 0, k ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_L2);
			if (now && !held[port][k]) {
				step += k ? 1 : -1;
			}
			held[port][k] = now;
		}
	}
	if (!step) {
		return;
	}
	screens = ((screens + step) % SCREENS_COUNT + SCREENS_COUNT) % SCREENS_COUNT;
	announceGeometry();
	struct retro_message shown = { names[screens], 120 };
	environment(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
}

// Runs both consoles for a frame, and shows their screens and plays their sound as chosen: both
// mixed when both screens show, otherwise the shown console's.
static void runPair(void) {
	uint16_t const keys[2] = { readKeys(0), readKeys(1) };
	readTilt(keys[0]);
	readScreensKeys();
	if (gbPairCopySave) {
		gbPairCopySave = false;
		size_t size;
		size_t secondSize;
		libretroGbSelect(1);
		void* const second = libretroGbSaveRam(&secondSize);
		libretroGbSelect(0);
		const void* const first = libretroGbSaveRam(&size);
		if (first && second && size == secondSize) {
			memcpy(second, first, size);
		}
	}
	const uint32_t* pictures[2];
	const int16_t* samples[2];
	size_t frames[2];
	libretroGbRunPair(keys, pictures, samples, frames);
	int const width = LIBRETRO_GB_WIDTH;
	int const height = LIBRETRO_GB_HEIGHT;
	if (screens == SCREENS_FIRST || screens == SCREENS_SECOND) {
		int const shown = screens == SCREENS_SECOND;
		videoRefresh(pictures[shown], width, height, width * sizeof(uint32_t));
		audioBatch(samples[shown], frames[shown]);
		return;
	}
	if (screens == SCREENS_SIDE) {
		for (int y = 0; y < height; ++y) {
			memcpy(pairFrame + y * width * 2, pictures[0] + y * width, width * sizeof(uint32_t));
			memcpy(pairFrame + y * width * 2 + width, pictures[1] + y * width, width * sizeof(uint32_t));
		}
		videoRefresh(pairFrame, width * 2, height, width * 2 * sizeof(uint32_t));
	} else {
		memcpy(pairFrame, pictures[0], width * height * sizeof(uint32_t));
		memcpy(pairFrame + width * height, pictures[1], width * height * sizeof(uint32_t));
		videoRefresh(pairFrame, width, height * 2, width * sizeof(uint32_t));
	}
	size_t count = frames[0] < frames[1] ? frames[0] : frames[1];
	count = count < sizeof(pairSamples) / sizeof(pairSamples[0]) / 2 ? count : sizeof(pairSamples) / sizeof(pairSamples[0]) / 2;
	for (size_t i = 0; i < count * 2; ++i) {
		pairSamples[i] = (samples[0][i] + samples[1][i]) / 2;
	}
	audioBatch(pairSamples, count);
}
#endif

// Runs one frame of the game and hands its picture and sound to the frontend.
void retro_run(void) {
	bool updated = false;
	if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readOptions();
	}
	inputPoll();
	readFastForward();
#ifdef ARMGBA_GB
	if (gbPair) {
		runPair();
		return;
	}
	if (gameBoy) {
		const int16_t* samples;
		size_t frames;
		uint16_t keys = readKeys(0);
		readTilt(keys);
		const uint32_t* picture = libretroGbRunFrame(keys, &samples, &frames);
		// A restart or a loaded state can change the model.
		if (gbOnAdvance != libretroGbOnAdvance()) {
			gbOnAdvance = !gbOnAdvance;
			announceGeometry();
		}
		if (gbOnAdvance) {
			readStretch(keys);
		}
		if (gbOnAdvance && !gbCropped) {
			videoRefresh(advanceFrame(picture), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));
		} else if (gbOnAdvance && gbStretched) {
			// The stretched picture, without the black above and below it.
			videoRefresh(advanceFrame(picture) + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH, SCREEN_WIDTH, LIBRETRO_GB_HEIGHT,
			             SCREEN_WIDTH * sizeof(uint32_t));
		} else {
			videoRefresh(picture, LIBRETRO_GB_WIDTH, LIBRETRO_GB_HEIGHT, LIBRETRO_GB_WIDTH * sizeof(uint32_t));
		}
		audioBatch(samples, frames);
		return;
	}
#endif
	uint16_t keys = readKeys(0) | readTurboShoulders();
	gbaSetKeys(gba, keys);
	readSensors(keys);
	linkGbaFrame(gba);
	gbaRunFrame(gba);
	updateRumble(gba->gpio.rumble);
	if (cheats.count || cheats.patchCount) {
		cheatsApply(&cheats, gba);
	}
	if (colorCorrection) {
		const uint16_t* frame = (const uint16_t*) gba->renderer.frame;
		for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
			correctedFrame[i] = correctedColors[(frame[i] & 0x1F) | ((frame[i] >> 1) & 0x7FE0)];
		}
		videoRefresh(correctedFrame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	} else {
		videoRefresh(gba->renderer.frame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	}
	audioBatch((const int16_t*) gba->audio.out, gba->audio.outCount);
	gba->audio.outCount = 0;
}

// Save states: the size, then saving and loading. A Game Boy Advance state is the machine's whole
// state; a Game Boy state is Gambatte's.
size_t retro_serialize_size(void) {
#ifdef ARMGBA_GB
	if (gameBoy) {
		return libretroGbStateSize();
	}
#endif
	return gbaStateSize();
}

bool retro_serialize(void* data, size_t size) {
#ifdef ARMGBA_GB
	if (gameBoy) {
		return libretroGbSaveState(data, size);
	}
#endif
	if (!gba || size < gbaStateSize()) {
		return false;
	}
	gbaSaveState(gba, data);
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
#ifdef ARMGBA_GB
	if (gameBoy) {
		bool loaded = libretroGbLoadState(data, size);
		checkGameBoyMemory();
		return loaded;
	}
#endif
	return gba && gbaLoadState(gba, data, size);
}

// Decodes every code again, in order.
static void applyCheatCodes(void) {
#ifdef ARMGBA_GB
	if (gameBoy) {
		if (!libretroGbSetCheats((const char* const*) cheatCodes, cheatCodeCount)) {
			logMessage(RETRO_LOG_WARN, "armGBA: a cheat code is neither Game Genie nor GameShark");
		}
		return;
	}
#endif
	cheatsClear(&cheats, gba);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		if (cheatCodes[i] && !cheatsAdd(&cheats, cheatCodes[i])) {
			char message[160];
			snprintf(message, sizeof(message), "armGBA: cheat code %.100s isn't one armGBA reads", cheatCodes[i]);
			logMessage(RETRO_LOG_WARN, message);
		}
	}
}

// Cheat codes: GameShark, Action Replay and CodeBreaker for the Game Boy Advance; Game Genie and
// GameShark for the Game Boy.
void retro_cheat_reset(void) {
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
	applyCheatCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	if (index >= cheatCodeCount) {
		char** grown = realloc(cheatCodes, (index + 1) * sizeof(*cheatCodes));
		if (!grown) {
			return;
		}
		memset(&grown[cheatCodeCount], 0, (index + 1 - cheatCodeCount) * sizeof(*grown));
		cheatCodes = grown;
		cheatCodeCount = index + 1;
	}
	free(cheatCodes[index]);
	cheatCodes[index] = enabled && code ? strdup(code) : NULL;
	applyCheatCodes();
}

// NTSC timing, in libretro's terms.
unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

// Memory the frontend may read or keep: the cartridge's save memory (loaded and saved as the save
// file), a Game Boy cartridge's clock (as the .rtc file), and work RAM for achievements and cheats.
void* retro_get_memory_data(unsigned id) {
#ifdef ARMGBA_GB
	if (gbPair && (id == MEMORY_SECOND_SAVE || id == MEMORY_SECOND_RTC)) {
		size_t size;
		libretroGbSelect(1);
		void* const data = id == MEMORY_SECOND_SAVE ? libretroGbSaveRam(&size) : libretroGbRtc(&size);
		libretroGbSelect(0);
		return data;
	}
	if (gameBoy) {
		size_t size;
		return id == RETRO_MEMORY_SAVE_RAM ? libretroGbSaveRam(&size) : id == RETRO_MEMORY_SYSTEM_RAM ? libretroGbSystemRam(&size)
		     : id == RETRO_MEMORY_RTC ? libretroGbRtc(&size) : NULL;
	}
#endif
	if (!gba) {
		return NULL;
	}
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return gba->savedata.storage;
	case RETRO_MEMORY_SYSTEM_RAM:
		return gba->ewram;
	default:
		return NULL;
	}
}

// Until the game shows its save type, save memory is as large as the largest save type, as in
// mGBA's core, so a save file from either core loads.
size_t retro_get_memory_size(unsigned id) {
#ifdef ARMGBA_GB
	if (gbPair && (id == MEMORY_SECOND_SAVE || id == MEMORY_SECOND_RTC)) {
		size_t size;
		libretroGbSelect(1);
		id == MEMORY_SECOND_SAVE ? libretroGbSaveRam(&size) : libretroGbRtc(&size);
		libretroGbSelect(0);
		return size;
	}
	if (gameBoy) {
		size_t size = 0;
		if (id == RETRO_MEMORY_SAVE_RAM) {
			libretroGbSaveRam(&size);
		} else if (id == RETRO_MEMORY_SYSTEM_RAM) {
			libretroGbSystemRam(&size);
		} else if (id == RETRO_MEMORY_RTC) {
			libretroGbRtc(&size);
		}
		return size;
	}
#endif
	if (!gba) {
		return 0;
	}
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return gba->savedata.type == SAVEDATA_AUTODETECT ? SIZE_FLASH1M : savedataSize(gba);
	case RETRO_MEMORY_SYSTEM_RAM:
		return SIZE_EWRAM;
	default:
		return 0;
	}
}
