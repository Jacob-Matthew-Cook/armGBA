/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Sharp text at any size (text.c).

#pragma once

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

enum FaceId {
	FACE_BODY,
	FACE_STRONG,
	FACE_TITLE,
	FACES,
};

bool textInit(void);
void textReset(void);
int textWidth(enum FaceId face, int size, const char* text);
// Draws text whose line box starts at y; returns its width.
int textDraw(enum FaceId face, int size, int x, int y, SDL_Color color, const char* text);
void textFit(enum FaceId face, int size, const char* text, int width, bool keepEnd, char* out, size_t outSize);
