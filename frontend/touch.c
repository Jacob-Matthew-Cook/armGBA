/*
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// On-Screen Controls (Android): a controller of their own, one of SDL's virtual joysticks, played by
// touch and drawn over the game, there while no other controller is connected; the controller's
// buttons, hotkeys and menus work with it as with any. Placed and named as on a Nintendo controller:
// A on the right, B below, X above, Y on the left, L2 above L and R2 above R, the D-pad and the
// buttons a little below half-way, where thumbs rest, the sticks below them and further in, and L3
// and R3 in the lower corners. A tap at the top middle shows the menu button on any touch screen,
// with the controls or without them.

#include "frontend.h"

#include <math.h>
#include <string.h>

#include "text.h"

static SDL_Joystick* joystick;
// The buttons, then L2 and R2, which are the triggers' axes.
enum { L2 = SDL_CONTROLLER_BUTTON_MAX, R2, HELD };
static bool held[HELD];

enum { FINGERS = 10 };
// A finger that comes down on a stick moves it until it lifts, wherever it goes.
static struct {
	SDL_FingerID id;
	float x, y;
	bool down;
	int stick;
} fingers[FINGERS];
// Where each stick is pushed, -1 to 1 each way.
static float sticks[2][2];

enum Shape { ROUND, PILL, CROSS, STICK };

// The controls, by the shorter side s of the window: where, how big and what they press.
struct Control {
	enum Shape shape;
	int button;
	const char* label;
	float x, y, w, h;
};

enum { MENU = -1, CONTROLS = 16 };
// The menu button slides down from the top after a tap near it, and back up a while later.
enum { MENU_SHOWN_MS = 3000, MENU_SLIDE_MS = 220 };
static Uint32 menuShownUntil;

static int layout(struct Control out[CONTROLS], int width, int height) {
	float const s = (float) SDL_min(width, height);
	float const pad = 0.20f * s, low = height - 0.45f * s, face = 0.105f * s, round = 0.12f * s;
	float const pillW = 0.26f * s, pillH = 0.08f * s, smallW = 0.15f * s, smallH = 0.06f * s, stickX = 0.34f * s, stickY = height - 0.17f * s, stick = 0.24f * s;
	float const top = 0.03f * s + pillH / 2, under = top + 0.10f * s, click = 0.085f * s;
	struct Control const c[CONTROLS] = {
		{ CROSS, SDL_CONTROLLER_BUTTON_DPAD_UP, NULL, pad, low, 0.30f * s, 0.30f * s },
		{ ROUND, SDL_CONTROLLER_BUTTON_B, "A", width - pad + face, low, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_A, "B", width - pad, low + face, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_Y, "X", width - pad, low - face, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_X, "Y", width - pad - face, low, round, round },
		{ PILL, L2, "L2", 0.04f * s + pillW / 2, top, pillW, pillH },
		{ PILL, R2, "R2", width - 0.04f * s - pillW / 2, top, pillW, pillH },
		{ PILL, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L", 0.04f * s + pillW / 2, under, pillW, pillH },
		{ PILL, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R", width - 0.04f * s - pillW / 2, under, pillW, pillH },
		{ ROUND, SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3", 0.065f * s, height - 0.065f * s, click, click },
		{ ROUND, SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3", width - 0.065f * s, height - 0.065f * s, click, click },
		{ PILL, SDL_CONTROLLER_BUTTON_BACK, "Select", width / 2.0f - 0.10f * s, height - 0.05f * s, smallW, smallH },
		{ PILL, SDL_CONTROLLER_BUTTON_START, "Start", width / 2.0f + 0.10f * s, height - 0.05f * s, smallW, smallH },
		{ STICK, 0, NULL, stickX, stickY, stick, stick },
		{ STICK, 1, NULL, width - stickX, stickY, stick, stick },
		{ ROUND, MENU, NULL, width / 2.0f, 0.07f * s, 0.06f * s, 0.06f * s },
	};
	memcpy(out, c, sizeof(c));
	return CONTROLS;
}

// What a finger at x, y presses: the D-pad's directions by angle, diagonals between, outside a small
// middle; a round button within a little more than its size, two between two; a pill a little past it.
static void pressedAt(const struct Control* c, float x, float y, bool out[HELD]) {
	float const dx = x - c->x, dy = y - c->y;
	switch (c->shape) {
	case CROSS: {
		float const distance = sqrtf(dx * dx + dy * dy);
		if (distance < c->w * 0.12f || distance > c->w * 0.85f) {
			return;
		}
		float const a = atan2f(dy, dx), eighth = (float) M_PI / 8;
		out[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] |= fabsf(a) < 3 * eighth;
		out[SDL_CONTROLLER_BUTTON_DPAD_LEFT] |= fabsf(a) > 5 * eighth;
		out[SDL_CONTROLLER_BUTTON_DPAD_DOWN] |= a > eighth && a < 7 * eighth;
		out[SDL_CONTROLLER_BUTTON_DPAD_UP] |= a < -eighth && a > -7 * eighth;
		break;
	}
	case ROUND:
		if (c->button != MENU && dx * dx + dy * dy < c->w * c->w * 0.85f) {
			out[c->button] = true;
		}
		break;
	case PILL:
		if (fabsf(dx) < c->w * 0.65f && fabsf(dy) < c->h * 0.62f) {
			out[c->button] = true;
		}
		break;
	case STICK:
		break;
	}
}

static void moveStick(int which, float x, float y) {
	sticks[which][0] = x;
	sticks[which][1] = y;
	if (joystick) {
		SDL_JoystickSetVirtualAxis(joystick, 2 * which, (Sint16) (x * 32767));
		SDL_JoystickSetVirtualAxis(joystick, 2 * which + 1, (Sint16) (y * 32767));
	}
}

// A button, or L2 or R2 as a trigger all the way in or out.
static void press(int b, bool down) {
	held[b] = down;
	if (b < SDL_CONTROLLER_BUTTON_MAX) {
		SDL_JoystickSetVirtualButton(joystick, b, down ? SDL_PRESSED : SDL_RELEASED);
	} else {
		SDL_JoystickSetVirtualAxis(joystick, b == L2 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
		                           down ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
	}
}

static void release(void) {
	memset(fingers, 0, sizeof(fingers));
	if (joystick) {
		moveStick(0, 0, 0);
		moveStick(1, 0, 0);
		for (int b = 0; b < HELD; ++b) {
			press(b, false);
		}
	}
}

// Attached while wanted and no other controller is connected; detached, the controllers' ports are
// filled again from the first.
static bool padsConnected;

bool touchOnly(void) {
#ifdef __ANDROID__
	return !padsConnected;
#else
	return false;
#endif
}

void touchUpdate(void) {
	padsConnected = false;
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		padsConnected |= SDL_IsGameController(i) && !SDL_JoystickIsVirtual(i);
	}
	bool wanted = false;
#ifdef __ANDROID__
	wanted = settings.touchControls && !padsConnected;
#endif
	if (wanted && !joystick) {
		SDL_VirtualJoystickDesc desc;
		SDL_zero(desc);
		desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
		desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
		// Its buttons in SDL's order, through the D-pad, so each is the controller's of the same number.
		desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
		desc.button_mask = (1u << desc.nbuttons) - 1;
		desc.naxes = SDL_CONTROLLER_AXIS_MAX;
		desc.axis_mask = (1u << desc.naxes) - 1;
		desc.name = "On-Screen Controls";
		int const index = SDL_JoystickAttachVirtualEx(&desc);
		joystick = index >= 0 ? SDL_JoystickOpen(index) : NULL;
		release();
	} else if (!wanted && joystick) {
		release();
		SDL_JoystickID const id = SDL_JoystickInstanceID(joystick);
		SDL_JoystickClose(joystick);
		joystick = NULL;
		for (int i = 0; i < SDL_NumJoysticks(); ++i) {
			if (SDL_JoystickGetDeviceInstanceID(i) == id) {
				SDL_JoystickDetachVirtual(i);
				break;
			}
		}
		for (int port = 0; port < PORTS; ++port) {
			if (app.pads[port]) {
				SDL_GameControllerClose(app.pads[port]);
				app.pads[port] = NULL;
			}
		}
	}
}

// Over a game, outside the menus; the menu button is there with or without the rest.
static bool overGame(void) {
	return app.loaded && !app.menuOpen;
}

// A touch: the buttons held are remade from every finger down; the menu button opens the menu.
bool touchEvent(const SDL_Event* event) {
	if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERMOTION && event->type != SDL_FINGERUP) {
		return false;
	}
	if (!overGame()) {
		release();
		return false;
	}
	int width, height;
	SDL_GetRendererOutputSize(app.renderer, &width, &height);
	float const x = event->tfinger.x * width, y = event->tfinger.y * height;
	struct Control controls[CONTROLS];
	layout(controls, width, height);
	// The top middle, between the shoulder buttons: a tap shows the menu button, and another while
	// it shows opens the menu.
	float const s = (float) SDL_min(width, height);
	if (event->type == SDL_FINGERDOWN && y < 0.16f * s && x > 0.34f * s && x < width - 0.34f * s) {
		if (SDL_TICKS_PASSED(SDL_GetTicks(), menuShownUntil)) {
			menuShownUntil = SDL_GetTicks() + MENU_SHOWN_MS;
		} else {
			menuShownUntil = 0;
			release();
			uiOpenMenu();
		}
		return true;
	}
	if (!joystick) {
		return false;
	}
	int slot = -1;
	for (int i = 0; i < FINGERS; ++i) {
		if (fingers[i].down && fingers[i].id == event->tfinger.fingerId) {
			slot = i;
		}
	}
	for (int i = 0; i < FINGERS && slot < 0 && event->type == SDL_FINGERDOWN; ++i) {
		slot = fingers[i].down ? -1 : i;
	}
	if (slot >= 0) {
		if (event->type == SDL_FINGERDOWN) {
			fingers[slot].stick = -1;
			for (int c = 0; c < CONTROLS; ++c) {
				float const dx = x - controls[c].x, dy = y - controls[c].y;
				if (controls[c].shape == STICK && dx * dx + dy * dy < controls[c].w * controls[c].w * 0.36f) {
					fingers[slot].stick = controls[c].button;
				}
			}
		}
		fingers[slot].id = event->tfinger.fingerId;
		fingers[slot].x = x;
		fingers[slot].y = y;
		fingers[slot].down = event->type != SDL_FINGERUP;
		// Full over most of the way to the base's edge, and round.
		int const which = fingers[slot].stick;
		if (which >= 0) {
			struct Control const* const c = &controls[CONTROLS - 3 + which];
			float dx = 0, dy = 0;
			if (fingers[slot].down) {
				dx = (x - c->x) / (c->w * 0.4f);
				dy = (y - c->y) / (c->w * 0.4f);
				float const length = sqrtf(dx * dx + dy * dy);
				if (length > 1) {
					dx /= length;
					dy /= length;
				}
			}
			moveStick(which, dx, dy);
		}
	}
	bool now[HELD] = { false };
	for (int i = 0; i < FINGERS; ++i) {
		for (int c = 0; c < CONTROLS && fingers[i].down && fingers[i].stick < 0; ++c) {
			pressedAt(&controls[c], fingers[i].x, fingers[i].y, now);
		}
	}
	for (int b = 0; b < HELD; ++b) {
		if (now[b] != held[b]) {
			press(b, now[b]);
		}
	}
	return true;
}

// A convex shape's outline, a circle or a pill (a rectangle with round ends), steps points round.
enum { STEPS = 40 };

static void outline(float x, float y, float w, float h, SDL_FPoint out[STEPS]) {
	float const r = h / 2, straight = (w - h) / 2;
	for (int i = 0; i < STEPS; ++i) {
		float const a = 2 * (float) M_PI * (i + 0.5f) / STEPS;
		out[i] = (SDL_FPoint) { x + (cosf(a) >= 0 ? straight : -straight) + r * cosf(a), y + r * sinf(a) };
	}
}

// Filled in fill, with an edge of edge's color e wide inside it.
static void drawShape(float x, float y, float w, float h, SDL_Color fill, SDL_Color edge, float e) {
	SDL_FPoint outer[STEPS], inner[STEPS];
	outline(x, y, w, h, outer);
	outline(x, y, w - 2 * e, h - 2 * e, inner);
	SDL_Vertex v[3 * STEPS + 1];
	int index[9 * STEPS], n = 0;
	v[0] = (SDL_Vertex) { { x, y }, fill, { 0, 0 } };
	for (int i = 0; i < STEPS; ++i) {
		v[1 + i] = (SDL_Vertex) { inner[i], fill, { 0, 0 } };
		v[1 + STEPS + i] = (SDL_Vertex) { inner[i], edge, { 0, 0 } };
		v[1 + 2 * STEPS + i] = (SDL_Vertex) { outer[i], edge, { 0, 0 } };
	}
	for (int i = 0; i < STEPS; ++i) {
		int const j = (i + 1) % STEPS;
		int const q[9] = { 0, 1 + i, 1 + j, 1 + STEPS + i, 1 + 2 * STEPS + i, 1 + STEPS + j, 1 + STEPS + j, 1 + 2 * STEPS + i, 1 + 2 * STEPS + j };
		for (int k = 0; k < 9; ++k) {
			index[n++] = q[k];
		}
	}
	SDL_RenderGeometry(app.renderer, NULL, v, 3 * STEPS + 1, index, n);
}

static void fillRectF(float x, float y, float w, float h, SDL_Color color) {
	SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
	SDL_RenderFillRectF(app.renderer, &(SDL_FRect) { x, y, w, h });
}

static void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color color) {
	SDL_Vertex const v[3] = { { { x0, y0 }, color, { 0, 0 } }, { { x1, y1 }, color, { 0, 0 } }, { { x2, y2 }, color, { 0, 0 } } };
	SDL_RenderGeometry(app.renderer, NULL, v, 3, NULL, 0);
}

static void label(const char* text, float x, float y, int size, SDL_Color color) {
	textDraw(FACE_STRONG, size, (int) (x - textWidth(FACE_STRONG, size, text) / 2.0f), (int) (y - size * 0.45f), color, text);
}

// Dark and see-through with a light edge, to show on light and dark games alike; light when pressed.
void touchHideMenuButton(void) {
	menuShownUntil = 0;
}

void touchDraw(void) {
	if (!overGame()) {
		return;
	}
	int width, height;
	SDL_GetRendererOutputSize(app.renderer, &width, &height);
	struct Control controls[CONTROLS];
	layout(controls, width, height);
	SDL_Color const up = { 16, 20, 28, 110 }, down = { 255, 255, 255, 120 }, edge = { 255, 255, 255, 120 }, text = { 255, 255, 255, 210 };
	float const e = SDL_max(1.5f, SDL_min(width, height) * 0.004f);
	SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < CONTROLS; ++i) {
		struct Control const* c = &controls[i];
		if (!joystick && c->button != MENU) {
			continue;
		}
		if (c->shape == CROSS) {
			// The middle square and an arm each way with an arrow on it, then the edge round it all.
			float const arm = c->w / 3, half = arm / 2;
			static const int ways[4][3] = { { SDL_CONTROLLER_BUTTON_DPAD_UP, 0, -1 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1, 0 },
				                            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, 1 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, -1, 0 } };
			fillRectF(c->x - half, c->y - half, arm, arm, up);
			for (int k = 0; k < 4; ++k) {
				int const wx = ways[k][1], wy = ways[k][2];
				float const ax = c->x + wx * arm, ay = c->y + wy * arm;
				fillRectF(ax - half, ay - half, arm, arm, held[ways[k][0]] ? down : up);
				float const tip = arm * 0.3f, base = arm * 0.05f, side = arm * 0.22f;
				fillTriangle(ax + wx * tip, ay + wy * tip, ax - wx * base - wy * side, ay - wy * base + wx * side, ax - wx * base + wy * side,
				             ay - wy * base - wx * side, text);
				// The arm's end and sides; the sides of the arms across reach in to close the corners.
				if (wx) {
					fillRectF(wx > 0 ? ax + half - e : ax - half, ay - half, e, arm, edge);
					float const from = wx > 0 ? ax - half - e : ax - half + e;
					fillRectF(from, ay - half, arm, e, edge);
					fillRectF(from, ay + half - e, arm, e, edge);
				} else {
					fillRectF(ax - half, wy > 0 ? ay + half - e : ay - half, arm, e, edge);
					float const from = wy > 0 ? ay - half : ay - half + e;
					fillRectF(ax - half, from, e, arm - e, edge);
					fillRectF(ax + half - e, from, e, arm - e, edge);
				}
			}
		} else if (c->shape == STICK) {
			// The base, and the knob where it's pushed; fainter than the buttons, as they cover the game.
			SDL_Color const faint = { up.r, up.g, up.b, 60 }, faintEdge = { edge.r, edge.g, edge.b, 85 };
			bool const moved = sticks[c->button][0] || sticks[c->button][1];
			drawShape(c->x, c->y, c->w, c->h, faint, faintEdge, e);
			float const knob = c->w * 0.46f, reach = c->w * 0.4f - knob / 2 + c->w * 0.1f;
			drawShape(c->x + sticks[c->button][0] * reach, c->y + sticks[c->button][1] * reach, knob, knob, moved ? down : faint, faintEdge, e);
		} else if (c->button == MENU) {
			// Three lines, while shown: sliding in from above the screen, easing as it arrives, and out
			// again the same way.
			Uint32 const now = SDL_GetTicks();
			if (SDL_TICKS_PASSED(now, menuShownUntil)) {
				continue;
			}
			float const in = (float) (now - (menuShownUntil - MENU_SHOWN_MS)) / MENU_SLIDE_MS, out = (float) (menuShownUntil - now) / MENU_SLIDE_MS;
			float const t = 1 - SDL_min(1.0f, SDL_min(in, out)), eased = t * t * t;
			float const size = c->w * 1.4f, y = c->y - eased * (c->y + size);
			drawShape(c->x, y, size, size, up, edge, e);
			for (int k = -1; k <= 1; ++k) {
				fillRectF(c->x - c->w * 0.34f, y + k * c->h * 0.25f - c->h * 0.055f, c->w * 0.68f, c->h * 0.11f, text);
			}
		} else {
			drawShape(c->x, c->y, c->w, c->h, held[c->button] ? down : up, edge, e);
			label(c->label, c->x, c->y, (int) (c->h * (c->shape == ROUND && c->label[1] ? 0.4f : c->shape == ROUND ? 0.5f : 0.55f)), text);
		}
	}
}
