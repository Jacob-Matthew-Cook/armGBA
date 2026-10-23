/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's options: what they are, given to the frontend; their values, as the frontend has them;
// and which of them show, for the game loaded and the Link Mode chosen. libretro.c's readOptions
// takes their values in.

#include "core.h"
#include "gba.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct retro_core_options_v2* coreOptions;
// Which options were last shown, once set.
static bool optionsShown[32];
static bool optionsShownSet;

static void fillTurboPeriods(struct retro_core_option_v2_definition* definitions) {
	static char periods[TURBO_MOST - TURBO_FEWEST + 1][4];
	static char labels[TURBO_MOST - TURBO_FEWEST + 1][12];
	for (; definitions->key && strcmp(definitions->key, "armgba_turbo_period"); ++definitions) {
	}
	for (size_t i = 0; definitions->key && i < COUNT(periods); ++i) {
		snprintf(periods[i], sizeof(periods[i]), "%d", TURBO_FEWEST + (int) i);
		snprintf(labels[i], sizeof(labels[i]), "%d frames", TURBO_FEWEST + (int) i);
		definitions->values[i] = (struct retro_core_option_value) { periods[i], labels[i] };
	}
}

// Older frontends take each option as "Label; value|value", the default first, made here from the
// definitions.
static void setVariables(retro_environment_t callback, const struct retro_core_option_v2_definition* definitions) {
	static struct retro_variable variables[64];
	static char text[64][1024];
	int n = 0;
	for (; definitions->key && n < 63; ++definitions, ++n) {
		size_t at = snprintf(text[n], sizeof(text[n]), "%s; %s", definitions->desc, definitions->default_value);
		for (int i = 0; definitions->values[i].value && at < sizeof(text[n]); ++i) {
			if (strcmp(definitions->values[i].value, definitions->default_value)) {
				at += snprintf(text[n] + at, sizeof(text[n]) - at, "|%s", definitions->values[i].value);
			}
		}
		variables[n] = (struct retro_variable) { definitions->key, text[n] };
	}
	variables[n] = (struct retro_variable) { NULL, NULL };
	callback(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

// Keys and values match saved configurations; labels are what the menu shows. showOptions hides
// the options that do nothing for the game and Link Mode.
static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armgba_skip_bios", "Skip BIOS Intro", NULL, "Start games without the BIOS's intro: the Game Boy Advance logo, or armGBA's splash.", NULL, "system",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_bios", "BIOS", NULL,
	  "Nintendo's gba_bios.bin from the system folder, or armGBA's open-source BIOS built into the core. Nintendo's falls back to armGBA's when the file isn't there. Applies on Restart.",
	  NULL, "system",
	  { { "nintendo", "Nintendo's (System Folder)" }, { "open-source", "armGBA's (Built In)" }, { NULL, NULL } }, "nintendo" },
	{ "armgba_gb_model", "Game Boy Model", NULL,
	  "The console Game Boy and Game Boy Color games run on. On the Game Boy Advance, L or R stretches the picture. Applies on Restart.",
	  NULL, "system",
	  { { "Auto", NULL }, { "Game Boy", NULL }, { "Game Boy Pocket", "Game Boy Pocket / Light" }, { "Game Boy Color", NULL }, { "Game Boy Advance", NULL }, { NULL, NULL } }, "Auto" },
	{ "armgba_gb_boot_rom", "Game Boy Boot ROM", NULL,
	  "Start with the console's logo: Nintendo's boot ROM from the system folder (armGBA's when it isn't there), armGBA's open-source one built into the core, or none. Applies on Restart.",
	  NULL, "system",
	  { { "enabled", "Nintendo's (System Folder)" }, { "open-source", "armGBA's (Built In)" }, { "disabled", "Off" }, { NULL, NULL } }, "enabled" },
	{ "armgba_threaded_video", "Threaded Video", NULL,
	  "Draw the picture on a second CPU core, which helps fast-forward and run-ahead on slower devices. One Frame Late is faster still, but shows each frame a frame late.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { "one frame late", "One Frame Late" }, { NULL, NULL } }, "disabled" },
	{ "armgba_color_correction", "Color Correction", NULL, "Darker, softer colors, as the Game Boy Advance's screen showed them. Costs a little speed.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_color_correction", "Game Boy Color Correction", NULL,
	  "Colors as the Game Boy Color's screen showed them. Not used for games running on an original Game Boy.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_screen", "Game Boy Screen", NULL, "The colors original Game Boy games are shown in: plain grays, the first model's green, the Pocket's olive gray, or the Light's backlit teal.", NULL, "video",
	  { { "grayscale", "Grayscale" }, { "original", "Game Boy (Green)" }, { "pocket", "Game Boy Pocket (Olive)" }, { "light", "Game Boy Light (Teal)" }, { NULL, NULL } }, "grayscale" },
	{ "armgba_gb_contrast", "Game Boy Contrast", NULL,
	  "The contrast wheel of the first Game Boy, the Pocket and the Light: up darkens every shade, down lightens them. Pushing the right stick up or down turns it while playing.", NULL, "video",
	  { { "-5", NULL }, { "-4", NULL }, { "-3", NULL }, { "-2", NULL }, { "-1", NULL }, { "0", NULL },
	    { "1", "+1" }, { "2", "+2" }, { "3", "+3" }, { "4", "+4" }, { "5", "+5" }, { NULL, NULL } }, "0" },
	{ "armgba_gb_advance_border", "Game Boy on Advance Border", NULL,
	  "On the Game Boy Advance model: Cropped shows only the game's picture, Shown the black around it as that console's screen has.", NULL, "video",
	  { { "cropped", "Cropped" }, { "shown", "Shown" }, { NULL, NULL } }, "cropped" },
	{ "armgba_gb_advance_panel", "Game Boy on Advance Screen", NULL,
	  "For Game Boy games on the Advance model: show the Advance's screen or the Color's, with that screen's colors, LCD Grid and ghosting.",
	  NULL, "video", { { "advance", "Game Boy Advance" }, { "color", "Game Boy Color" }, { NULL, NULL } }, "advance" },
	{ "armgba_lcd_ghosting", "LCD Ghosting", NULL,
	  "The console's slow screen, which some games flicker sprites on to make them see-through: each change fades in over a few frames. Half fades twice as fast.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "half", "Half" }, { "enabled", "Full" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_interleave", "Interleaved Rows", NULL,
	  "The Game Boy Advance's screen drives alternate rows each frame, so a picture that alternates frames shows in stripes, as on the console. Needs LCD Ghosting.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_grid", "LCD Grid", NULL,
	  "Draws the gaps between the screen's pixels into a picture this many times larger. Pick the largest that fits, with integer scaling on. The armgba shaders do it on the GPU.",
	  NULL, "video_lcd",
	  { { "disabled", "Off" }, { "2", "2x" }, { "3", "3x" }, { "4", "4x" }, { "5", "5x" }, { "6", "6x" }, { "7", "7x" }, { "8", "8x" }, { NULL, NULL } },
	  "disabled" },
	{ "armgba_lcd_shadow", "Game Boy Pixel Shadow", NULL,
	  "With LCD Grid on, the first Game Boy's and the Pocket's pixels cast a soft shadow on the screen behind them: where it falls.",
	  NULL, "video_lcd",
	  { { "off", "Off" }, { "up", "Up" }, { "up right", "Up Right" }, { "right", "Right" }, { "down right", "Down Right" }, { "down", "Down" },
	    { "down left", "Down Left" }, { "left", "Left" }, { "up left", "Up Left" }, { NULL, NULL } },
	  "down right" },
	{ "armgba_lcd_threads", "CPU Effect Threads", NULL,
	  "How many CPU cores share LCD Ghosting's and LCD Grid's work each frame. Auto leaves one core for the rest, two with Threaded Video; 1 keeps it on the emulator's own.",
	  NULL, "video_lcd", { { "auto", "Auto" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { NULL, NULL } }, "auto" },
	{ "armgba_low_pass_filter", "Low-Pass Filter", NULL,
	  "Softens the sound's high notes, as the consoles' small speakers did. Higher is softer.", NULL, "audio",
	  { { "disabled", "Off" }, { "20", "20%" }, { "40", "40%" }, { "60", "60%" }, { "80", "80%" }, { NULL, NULL } }, "disabled" },
	{ "armgba_turbo_period", "Turbo Button Period", NULL,
	  "Frames per press while a turbo button is held: X and Y for A and B, and L3 and R3 for L and R on the Game Boy Advance.", NULL, "input",
	  { { NULL, NULL } }, "4" },
	{ "armgba_gb_tilt", "Tilt Sensor", NULL,
	  "What tilts the cartridge in motion sensor games, such as Kirby Tilt 'n' Tumble and WarioWare: Twisted!.",
	  NULL, "input",
	  { { "left stick", "Left Stick" }, { "right stick", "Right Stick" }, { "d-pad", "D-Pad" }, { NULL, NULL } }, "left stick" },
	{ "armgba_solar_sensor", "Solar Sensor", NULL,
	  "The sunlight Boktai's sensor sees. Pushing the right stick up or down changes it while playing.", NULL, "input",
	  { { "0", "0 (Dark)" }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", "10 (Full Sun)" }, { NULL, NULL } }, "5" },
	{ "armgba_restart_buttons", "Restart Buttons", NULL,
	  "Hold for a second to restart that player's console. On many handhelds Select + Start also quits RetroArch.",
	  NULL, "input",
	  { { "l r start", "L + R + Start" }, { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "select start a b", "Select + Start + A + B" }, { "off", "Off" }, { NULL, NULL } },
	  "l r start" },
	{ "armgba_link_mode", "Link Mode", NULL,
	  "Together: 2 to 4 consoles on each device, for one-device multiplayer or smooth netplay. Separate: each netplay device runs its own console. GameCube: link to Dolphin. Applies on Restart.",
	  NULL, "link",
	  { { "off", "Off" }, { "together", "Together" }, { "separate", "Separate" }, { "gamecube", "GameCube" }, { NULL, NULL } },
	  "off" },
	{ "armgba_link_players", "Players", NULL,
	  "How many consoles run. Four Game Boy Advances need a fast device. Applies on Restart.",
	  NULL, "link_players", { { "2", NULL }, { "3", NULL }, { "4", NULL }, { NULL, NULL } }, "2" },
	{ "armgba_player2_game", "Player 2's Game", NULL,
	  "The game on player 2's console, from the games beside player 1's. Players 3 and 4 play player 1's. No Cartridge is for games that send themselves over the cable. Applies on Restart.",
	  NULL, "link_players", { { "same", "Same Game" }, { NULL, NULL } }, "same" },
	{ "armgba_link_cartridge", "This Console's Cartridge", NULL,
	  "Removed: no cartridge, waiting for a game the other console sends over the cable, as single-pak games do. Applies on Restart.",
	  NULL, "link_connection", { { "inserted", "Inserted" }, { "removed", "Removed" }, { NULL, NULL } }, "inserted" },
	{ "armgba_link_port", "Link Port", NULL,
	  "What is plugged in: the Link Cable, or the Wireless Adapter that Pokemon FireRed, LeafGreen and Emerald use, smoothest over Wi-Fi. Applies on Restart.",
	  NULL, "link_connection", { { "cable", "Link Cable" }, { "wireless", "Wireless Adapter" }, { NULL, NULL } }, "cable" },
	{ "armgba_gb_link_port", "Game Boy Link Port", NULL,
	  "What joins the Game Boys: the Link Cable for two, or the Four Player Adapter for Wario Blast, F-1 Race and others, always used for 3 or 4. Applies on Restart.",
	  NULL, "link_connection", { { "cable", "Link Cable" }, { "adapter", "Four Player Adapter" }, { NULL, NULL } }, "cable" },
	{ "armgba_gb_link_timing", "Game Boy Link Timing", NULL,
	  "Standard: smooth on Wi-Fi, for games that link in turns, like trades and battles. Exact: in step like a real cable, for games that link all the time; slower on a long ping.",
	  NULL, "link_connection", { { "standard", "Standard" }, { "exact", "Exact" }, { NULL, NULL } }, "standard" },
	{ "armgba_gamecube_address", "GameCube Address", NULL,
	  "Where Dolphin runs, with a controller port set to GBA (TCP): found on this device or the local network, or this device only.",
	  NULL, "link_connection", { { "auto", "Find on the Network" }, { "127.0.0.1", "This Device" }, { NULL, NULL } }, "auto" },
	{ "armgba_link_screens", "Screens", NULL,
	  "How the screens are arranged. R2 changes it while playing.", NULL, "link_screens",
	  { { "side by side", "Side by Side" }, { "top and bottom", "Top and Bottom" }, { "player 1", "Player 1 Only" }, { "player 2", "Player 2 Only" }, { "player 3", "Player 3 Only" }, { "player 4", "Player 4 Only" }, { NULL, NULL } },
	  "side by side" },
	{ "armgba_link_l2", "L2 Button", NULL,
	  "What L2 does outside netplay: fast-forward while held, or go back to the previous screens. In netplay it always goes back.",
	  NULL, "link_screens", { { "fast forward", "Fast-Forward" }, { "previous screens", "Previous Screens" }, { NULL, NULL } }, "fast forward" },
	{ "armgba_switch_controller", "Switch Controller", NULL,
	  "Pressed on controller 1, moves it to the next console, the other controllers moving along with it: one controller can play every console. Stick clicks used here only switch. Not in netplay.",
	  NULL, "link_screens",
	  { { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "start l2 r2", "Start + L2 + R2" }, { "start l3", "Start + L3" }, { "start r3", "Start + R3" }, { "off", "Off" }, { NULL, NULL } },
	  "l r a b" },
	{ "armgba_back_up_save", "Back Up Save", NULL,
	  "Copies a player's save, as it is now, to a file beside it named for the time: Game.20261018-153012.srm. The game carries on.",
	  NULL, "saves", { { "off", "Off" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "off" },
	{ "armgba_load_save", "Load Save", NULL,
	  "Makes a backed-up save, or another player's current one, a player's save, and restarts their console. Load Into picks the player. The save it replaces is backed up first. Not during netplay.",
	  NULL, "saves", { { "off", "Off" }, { NULL, NULL } }, "off" },
	{ "armgba_load_into", "Load Into", NULL,
	  "The player Load Save gives a save to: the one it belongs to, or the player named here.",
	  NULL, "saves", { { "same", "Same Player" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "same" },
	{ "armgba_send_save", "Send Save", NULL,
	  "With Separate, while linked: sends the chosen save to the other player, who finds it under Received in Load Save. Linked devices also swap their current saves when they connect.",
	  NULL, "saves", { { "off", "Off" }, { "1 now", "Player 1: Now" }, { NULL, NULL } }, "off" },
	{ "armgba_new_save", "New Players' Saves", NULL,
	  "What a player on player 1's game starts with when they have no save: a new one, or a copy of player 1's. Kept in armGBA/Player 2 to 4. Applies on Restart.",
	  NULL, "saves_settings", { { "new", "Create New Save" }, { "copy", "Copy Player 1's Save" }, { NULL, NULL } }, "new" },
	{ "armgba_player1_saves", "Player 1's Save Folder", NULL,
	  "Player 1's save: in the save folder like other games', or in armGBA/Player 1 beside the other players' saves. When it changes, the newer is used. Applies when a game starts.",
	  NULL, "saves_settings", { { "retroarch", "Save Folder" }, { "armgba", "armGBA/Player 1 Folder" }, { NULL, NULL } }, "retroarch" },
	{ "armgba_keep_saves", "Backups Kept", NULL,
	  "How many backups each player keeps. When there are more, the oldest are deleted. A save the same as its newest backup isn't backed up again.",
	  NULL, "saves_settings", { { "5", "5" }, { "10", "10" }, { "20", "20" }, { "all", "All" }, { NULL, NULL } }, "10" },
	{ "armgba_list_saves", "Backups Listed", NULL,
	  "How many of each player's newest backups Load Save lists, up to 126 in all.",
	  NULL, "saves_settings", { { "10", "10" }, { "20", "20" }, { "40", "40" }, { "80", "80" }, { "126", "126" }, { NULL, NULL } }, "10" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category optionCategories[] = {
	{ "system", "System", "The console the game runs on and how it starts." },
	{ "video", "Video", "Colors, the picture and how it is drawn." },
	// Video's own group, and Link's and Saves': the standalone frontend shows them on their category's
	// page, under their names.
	{ "video_lcd", "Video: LCD Effects", "The screen's ghosting, its grid and the Game Boy's pixel shadow, drawn on the CPU." },
	{ "audio", "Audio", "How the sound is filtered." },
	{ "input", "Input", "Turbo, restart buttons and cartridge sensors." },
	{ "link", "Link", "Playing together: Link Mode, and the link's players, connection, screens and controls." },
	{ "link_players", "Link: Players", "How many consoles, and player 2's game." },
	{ "link_connection", "Link: Connection", "The link cable, the Wireless Adapter, the Four Player Adapter and the GameCube." },
	{ "link_screens", "Link: Screens and Controls", "How the consoles' screens are shown, and moving a controller between them." },
	{ "saves", "Saves", "Backing up, loading and sending saves." },
	{ "saves_settings", "Saves: Settings", "Where saves are kept, new players' saves, and how many backups." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 optionsV2 = { optionCategories, optionDefinitions };

const char* option(const char* key) {
	struct retro_variable variable = { key, NULL };
	return environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : NULL;
}

bool optionIs(const char* key, const char* wanted) {
	const char* const value = option(key);
	return value && !strcmp(value, wanted);
}


// The number for the option's value, or fallback if the value isn't one of the choices.
int optionChoice(const char* key, const struct Choice* choices, size_t count, int fallback) {
	const char* const value = option(key);
	for (size_t i = 0; value && i < count; ++i) {
		if (!strcmp(value, choices[i].value)) {
			return choices[i].number;
		}
	}
	return fallback;
}

bool togetherWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "together");
}

bool separateWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "separate");
}

bool gamecubeWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "gamecube");
}

int playersWanted(void) {
	const char* value = option("armgba_link_players");
	return clampInt(value ? atoi(value) : 2, 2, MAX_PLAYERS);
}

// Three or four Game Boys always use the Four Player Adapter.
bool adapterWanted(int count) {
	const char* value = option("armgba_gb_link_port");
	return count > 2 || (value && !strcmp(value, "adapter"));
}

// Hides the options that do nothing for the game and Link Mode. RetroArch calls it when an option
// changes, before the core has read it, so it reads the options itself; returns whether that changed
// which are shown.
bool showOptions(void) {
	bool const advance = gba && !gameBoy;
	int const devices = advance ? gba->gpio.devices : 0;
	bool const together = togetherWanted();
	bool const linked = together || separateWanted();
	const struct {
		const char* key;
		bool shown;
	} options[] = {
		{ "armgba_skip_bios", !gameBoy },
		{ "armgba_bios", !gameBoy },
		{ "armgba_threaded_video", !gameBoy },
		{ "armgba_lcd_shadow", lcdCastsShadow() && !optionIs("armgba_lcd_grid", "disabled") },
		{ "armgba_color_correction", !gameBoy },
		{ "armgba_solar_sensor", !gameBoy && (!advance || devices & HW_LIGHT_SENSOR) },
		{ "armgba_gb_tilt", (!gameBoy && !advance) || devices & (HW_TILT | HW_GYRO) || (gameBoy && tiltCartridge) },
		{ "armgba_link_port", !gameBoy && linked },
		{ "armgba_gb_model", !advance },
		{ "armgba_gb_boot_rom", !advance },
		{ "armgba_gb_color_correction", !advance },
		{ "armgba_gb_screen", !advance },
		{ "armgba_gb_contrast", lcdCastsShadow() },
		{ "armgba_gb_advance_border", !advance && (gbOnAdvance || optionIs("armgba_gb_model", "Game Boy Advance")) },
		{ "armgba_gb_advance_panel", !advance && (gbOnAdvance || optionIs("armgba_gb_model", "Game Boy Advance")) },
		{ "armgba_lcd_interleave", !optionIs("armgba_lcd_ghosting", "disabled") && (advance || (gbOnAdvance && !optionIs("armgba_gb_advance_panel", "color"))) },
		{ "armgba_lcd_threads", !optionIs("armgba_lcd_ghosting", "disabled") || !optionIs("armgba_lcd_grid", "disabled") },
		{ "armgba_gb_link_port", !advance && together },
		{ "armgba_gb_link_timing", !advance && separateWanted() },
		{ "armgba_link_players", together },
		{ "armgba_link_screens", together },
		{ "armgba_link_l2", together },
		{ "armgba_switch_controller", together },
		{ "armgba_player2_game", together },
		{ "armgba_new_save", together },
		{ "armgba_load_into", together },
		{ "armgba_send_save", separateWanted() },
		{ "armgba_link_cartridge", advance && (separateWanted() || gamecubeWanted()) },
		{ "armgba_gamecube_address", advance && gamecubeWanted() },
	};
	enum { OPTIONS = COUNT(options) };
	_Static_assert(OPTIONS <= COUNT(optionsShown), "optionsShown has room for every option shown and hidden here");
	bool changed = false;
	for (int i = 0; i < OPTIONS; ++i) {
		if (!optionsShownSet || optionsShown[i] != options[i].shown) {
			optionsShown[i] = options[i].shown;
			struct retro_core_option_display display = { options[i].key, options[i].shown };
			environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
			changed = true;
		}
	}
	optionsShownSet = true;
	return changed;
}

// Sets the options again after a list of choices changed. That shows every option, so they are
// hidden again.
void setOptions(void) {
	environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, coreOptions);
	optionsShownSet = false;
	showOptions();
}

// Gives the frontend the options: as definitions it can show and hide, returning true, or to an older
// frontend as plain variables.
bool optionsGive(retro_environment_t callback) {
	fillTurboPeriods(optionDefinitions);
	unsigned version = 0;
	if (callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &optionsV2);
		coreOptions = &optionsV2;
		struct retro_core_options_update_display_callback display = { showOptions };
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &display);
		return true;
	}
	setVariables(callback, optionDefinitions);
	return false;
}

struct retro_core_option_v2_definition* optionDefinition(const char* key) {
	for (struct retro_core_option_v2_definition* definition = optionDefinitions; definition->key; ++definition) {
		if (!strcmp(definition->key, key)) {
			return definition;
		}
	}
	return NULL;
}
