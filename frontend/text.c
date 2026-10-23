/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Text: Inter for the menus and Chakra Petch for titles (frontend/fonts, LICENSES/OFL-1.1.txt,
// built in by fonts/embed.cmake), rasterized by stb_truetype at the exact size asked for, so it is
// sharp at any window size. Each face and size gets an atlas of its glyphs the first time it's
// drawn; they are kept until the window's size or renderer changes.

#include "frontend.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include "fonts.inc"

enum {
	CACHED = 12,
	EXTRA = 12,
};

// Beyond ASCII: …  ‹ › • · ← ↑ → ↓ ▲ ▼ ✓.
static const int extraCodes[EXTRA] = { 0x2026, 0x2039, 0x203A, 0x2022, 0x00B7, 0x2190, 0x2191, 0x2192, 0x2193, 0x25B2, 0x25BC, 0x2713 };

struct Face {
	const unsigned char* data;
	stbtt_fontinfo info;
};

struct Atlas {
	enum FaceId face;
	int size;
	SDL_Texture* texture;
	stbtt_packedchar ascii[95];
	stbtt_packedchar extra[EXTRA];
	int ascent;
	unsigned used;
};

// The faces, and the atlases made, the least recently used made again first when all are taken.
static struct Face faces[FACES];
static struct Atlas atlases[CACHED];
static unsigned useCounter;

// The built-in fonts; false if one of them is damaged.
bool textInit(void) {
	static const unsigned char* const data[FACES] = { fontInterMedium, fontInterSemiBold, fontChakraPetchBold };
	for (int i = 0; i < FACES; ++i) {
		faces[i].data = data[i];
		if (!stbtt_InitFont(&faces[i].info, data[i], stbtt_GetFontOffsetForIndex(data[i], 0))) {
			return false;
		}
	}
	return true;
}

// Forgets the atlases, as their textures went with the renderer or the sizes changed.
void textReset(void) {
	for (int i = 0; i < CACHED; ++i) {
		if (atlases[i].texture) {
			SDL_DestroyTexture(atlases[i].texture);
		}
		memset(&atlases[i], 0, sizeof(atlases[i]));
	}
}

// The atlas for a face and size: printable ASCII and the extra glyphs packed into one texture, white
// with the glyphs' coverage as alpha, so it takes any color.
static struct Atlas* atlasFor(enum FaceId face, int size) {
	struct Atlas* oldest = &atlases[0];
	for (int i = 0; i < CACHED; ++i) {
		if (atlases[i].texture && atlases[i].face == face && atlases[i].size == size) {
			atlases[i].used = ++useCounter;
			return &atlases[i];
		}
		if (atlases[i].used < oldest->used) {
			oldest = &atlases[i];
		}
	}
	if (oldest->texture) {
		SDL_DestroyTexture(oldest->texture);
	}
	memset(oldest, 0, sizeof(*oldest));
	int const dimension = size <= 36 ? 512 : 1024;
	unsigned char* alpha = calloc(dimension, dimension);
	if (!alpha) {
		return NULL;
	}
	stbtt_pack_context pack = { 0 };
	stbtt_PackBegin(&pack, alpha, dimension, dimension, 0, 1, NULL);
	stbtt_pack_range ranges[2] = {
		{ (float) size, 32, NULL, 95, oldest->ascii, 0, 0 },
		{ (float) size, 0, (int*) extraCodes, EXTRA, oldest->extra, 0, 0 },
	};
	stbtt_PackFontRanges(&pack, faces[face].data, 0, ranges, 2);
	stbtt_PackEnd(&pack);
	uint32_t* pixels = malloc((size_t) dimension * dimension * 4);
	if (!pixels) {
		free(alpha);
		return NULL;
	}
	for (int i = 0; i < dimension * dimension; ++i) {
		pixels[i] = (uint32_t) alpha[i] << 24 | 0xFFFFFF;
	}
	oldest->texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dimension, dimension);
	SDL_UpdateTexture(oldest->texture, NULL, pixels, dimension * 4);
	SDL_SetTextureBlendMode(oldest->texture, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(oldest->texture, SDL_ScaleModeNearest);
	free(pixels);
	free(alpha);
	int ascent, descent, gap;
	stbtt_GetFontVMetrics(&faces[face].info, &ascent, &descent, &gap);
	oldest->ascent = (int) (ascent * stbtt_ScaleForPixelHeight(&faces[face].info, (float) size) + 0.5f);
	oldest->face = face;
	oldest->size = size;
	oldest->used = ++useCounter;
	return oldest;
}

// The next code point in UTF-8 text.
static int nextCode(const char** text) {
	const unsigned char* p = (const unsigned char*) *text;
	int code = *p++;
	int follow = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;
	code &= follow == 3 ? 0x07 : follow == 2 ? 0x0F : follow == 1 ? 0x1F : 0x7F;
	while (follow-- && (*p & 0xC0) == 0x80) {
		code = code << 6 | (*p++ & 0x3F);
	}
	*text = (const char*) p;
	return code;
}

// A code point's glyph; ? for one the atlas doesn't have.
static const stbtt_packedchar* glyphFor(const struct Atlas* atlas, int code) {
	if (code >= 32 && code < 127) {
		return &atlas->ascii[code - 32];
	}
	for (int i = 0; i < EXTRA; ++i) {
		if (extraCodes[i] == code) {
			return &atlas->extra[i];
		}
	}
	return &atlas->ascii['?' - 32];
}

// How wide text draws, in pixels.
int textWidth(enum FaceId face, int size, const char* text) {
	struct Atlas* atlas = atlasFor(face, size);
	if (!atlas) {
		return 0;
	}
	float width = 0;
	for (const char* p = text; *p;) {
		width += glyphFor(atlas, nextCode(&p))->xadvance;
	}
	return (int) (width + 0.5f);
}

// Draws text with its top at y; returns how wide it was.
int textDraw(enum FaceId face, int size, int x, int y, SDL_Color color, const char* text) {
	struct Atlas* atlas = atlasFor(face, size);
	if (!atlas) {
		return 0;
	}
	SDL_SetTextureColorMod(atlas->texture, color.r, color.g, color.b);
	SDL_SetTextureAlphaMod(atlas->texture, color.a);
	float pen = (float) x;
	for (const char* p = text; *p;) {
		const stbtt_packedchar* g = glyphFor(atlas, nextCode(&p));
		int const gw = g->x1 - g->x0, gh = g->y1 - g->y0;
		if (gw && gh) {
			SDL_Rect const from = { g->x0, g->y0, gw, gh };
			SDL_Rect const to = { (int) (pen + g->xoff + 0.5f), y + atlas->ascent + (int) (g->yoff + 0.5f), gw, gh };
			SDL_RenderCopy(app.renderer, atlas->texture, &from, &to);
		}
		pen += g->xadvance;
	}
	return (int) (pen + 0.5f) - x;
}

// Text cut to a width, with … where it was cut: from the end, or for a path from the start.
void textFit(enum FaceId face, int size, const char* text, int width, bool keepEnd, char* out, size_t outSize) {
	snprintf(out, outSize, "%s", text);
	if (textWidth(face, size, out) <= width) {
		return;
	}
	size_t const length = strlen(text);
	for (size_t cut = 1; cut < length; ++cut) {
		if (keepEnd) {
			snprintf(out, outSize, "\xE2\x80\xA6%s", text + cut);
		} else {
			snprintf(out, outSize, "%.*s\xE2\x80\xA6", (int) (length - cut), text);
		}
		if (textWidth(face, size, out) <= width) {
			return;
		}
	}
}
