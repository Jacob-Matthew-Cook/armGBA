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
// A on the right, B below, X above, Y on the left.

#include "frontend.h"

#include <math.h>
#include <string.h>

#include "text.h"

static SDL_Joystick* joystick;
static bool held[SDL_CONTROLLER_BUTTON_MAX];

enum { FINGERS = 10 };
static struct {
	SDL_FingerID id;
	float x, y;
	bool down;
} fingers[FINGERS];

enum Shape { ROUND, PILL, CROSS };

// The controls, by the shorter side s of the window: where, how big and what they press.
struct Control {
	enum Shape shape;
	int button;
	const char* label;
	float x, y, w, h;
};

enum { MENU = -1, CONTROLS = 10 };

static int layout(struct Control out[CONTROLS], int width, int height) {
	float const s = (float) SDL_min(width, height);
	float const pad = 0.20f * s, low = height - 0.30f * s, face = 0.105f * s, round = 0.12f * s;
	float const pillW = 0.26f * s, pillH = 0.09f * s, smallW = 0.15f * s, smallH = 0.06f * s;
	struct Control const c[CONTROLS] = {
		{ CROSS, SDL_CONTROLLER_BUTTON_DPAD_UP, NULL, pad, low, 0.30f * s, 0.30f * s },
		{ ROUND, SDL_CONTROLLER_BUTTON_B, "A", width - pad + face, low, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_A, "B", width - pad, low + face, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_Y, "X", width - pad, low - face, round, round },
		{ ROUND, SDL_CONTROLLER_BUTTON_X, "Y", width - pad - face, low, round, round },
		{ PILL, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L", 0.04f * s + pillW / 2, 0.06f * s + pillH / 2, pillW, pillH },
		{ PILL, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R", width - 0.04f * s - pillW / 2, 0.06f * s + pillH / 2, pillW, pillH },
		{ PILL, SDL_CONTROLLER_BUTTON_BACK, "Select", width / 2.0f - 0.10f * s, height - 0.05f * s, smallW, smallH },
		{ PILL, SDL_CONTROLLER_BUTTON_START, "Start", width / 2.0f + 0.10f * s, height - 0.05f * s, smallW, smallH },
		{ ROUND, MENU, NULL, width / 2.0f, 0.045f * s, 0.06f * s, 0.06f * s },
	};
	memcpy(out, c, sizeof(c));
	return CONTROLS;
}

// What a finger at x, y presses: the D-pad's directions by angle, diagonals between, outside a small
// middle; a round button within a little more than its size, two between two; a pill a little past it.
static void pressedAt(const struct Control* c, float x, float y, bool out[SDL_CONTROLLER_BUTTON_MAX]) {
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
		if (fabsf(dx) < c->w * 0.65f && fabsf(dy) < c->h * 0.9f) {
			out[c->button] = true;
		}
		break;
	}
}

static void release(void) {
	memset(fingers, 0, sizeof(fingers));
	for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
		if (held[b] && joystick) {
			SDL_JoystickSetVirtualButton(joystick, b, SDL_RELEASED);
		}
		held[b] = false;
	}
}

// Attached while wanted and no other controller is connected; detached, the controllers' ports are
// filled again from the first.
void touchUpdate(void) {
	bool wanted = false;
#ifdef __ANDROID__
	wanted = settings.touchControls;
	for (int i = 0; i < SDL_NumJoysticks() && wanted; ++i) {
		wanted = !SDL_IsGameController(i) || SDL_JoystickIsVirtual(i);
	}
#endif
	if (wanted && !joystick) {
		SDL_VirtualJoystickDesc desc;
		SDL_zero(desc);
		desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
		desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
		// Its buttons in SDL's order, through the D-pad, so each is the controller's of the same number.
		desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
		desc.button_mask = (1u << desc.nbuttons) - 1;
		desc.name = "On-Screen Controls";
		int const index = SDL_JoystickAttachVirtualEx(&desc);
		joystick = index >= 0 ? SDL_JoystickOpen(index) : NULL;
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

static bool shown(void) {
	return joystick && app.loaded && !app.menuOpen;
}

// A touch: the buttons held are remade from every finger down; the menu button opens the menu.
bool touchEvent(const SDL_Event* event) {
	if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERMOTION && event->type != SDL_FINGERUP) {
		return false;
	}
	if (!shown()) {
		release();
		return false;
	}
	int width, height;
	SDL_GetRendererOutputSize(app.renderer, &width, &height);
	float const x = event->tfinger.x * width, y = event->tfinger.y * height;
	struct Control controls[CONTROLS];
	layout(controls, width, height);
	if (event->type == SDL_FINGERDOWN) {
		struct Control const* const menu = &controls[CONTROLS - 1];
		if ((x - menu->x) * (x - menu->x) + (y - menu->y) * (y - menu->y) < menu->w * menu->w * 1.5f) {
			release();
			uiOpenMenu();
			return true;
		}
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
		fingers[slot].id = event->tfinger.fingerId;
		fingers[slot].x = x;
		fingers[slot].y = y;
		fingers[slot].down = event->type != SDL_FINGERUP;
	}
	bool now[SDL_CONTROLLER_BUTTON_MAX] = { false };
	for (int i = 0; i < FINGERS; ++i) {
		for (int c = 0; c < CONTROLS && fingers[i].down; ++c) {
			pressedAt(&controls[c], fingers[i].x, fingers[i].y, now);
		}
	}
	for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
		if (now[b] != held[b]) {
			held[b] = now[b];
			SDL_JoystickSetVirtualButton(joystick, b, now[b] ? SDL_PRESSED : SDL_RELEASED);
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
void touchDraw(void) {
	if (!shown()) {
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
		} else if (c->button == MENU) {
			// Three lines.
			drawShape(c->x, c->y, c->w, c->h, up, edge, e);
			for (int k = -1; k <= 1; ++k) {
				fillRectF(c->x - c->w * 0.24f, c->y + k * c->h * 0.18f - c->h * 0.04f, c->w * 0.48f, c->h * 0.08f, text);
			}
		} else {
			drawShape(c->x, c->y, c->w, c->h, held[c->button] ? down : up, edge, e);
			label(c->label, c->x, c->y, (int) (c->h * (c->shape == ROUND ? 0.5f : 0.55f)), text);
		}
	}
}
