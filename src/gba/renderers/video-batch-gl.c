/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/renderers/video-batch.h>

#include <mgba/core/log.h>

// Only the OpenGL ES 3.0 calls used here, loaded from the frontend
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_R8UI 0x8232
#define GL_R16UI 0x8234
#define GL_RGBA32UI 0x8D70
#define GL_RED_INTEGER 0x8D94
#define GL_RGBA_INTEGER 0x8D99
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_SCISSOR_TEST 0x0C11
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_TRIANGLES 0x0004
#define GL_TRUE 1

#define BATCH_GL_FUNCTIONS(X) \
	X(void, ActiveTexture, (unsigned)) \
	X(void, AttachShader, (unsigned, unsigned)) \
	X(void, BindBuffer, (unsigned, unsigned)) \
	X(void, BindFramebuffer, (unsigned, unsigned)) \
	X(void, BindSampler, (unsigned, unsigned)) \
	X(void, BindTexture, (unsigned, unsigned)) \
	X(void, BindVertexArray, (unsigned)) \
	X(void, ColorMask, (unsigned char, unsigned char, unsigned char, unsigned char)) \
	X(void, CompileShader, (unsigned)) \
	X(unsigned, CreateProgram, (void)) \
	X(unsigned, CreateShader, (unsigned)) \
	X(void, DeleteProgram, (unsigned)) \
	X(void, DeleteShader, (unsigned)) \
	X(void, DeleteTextures, (int, const unsigned*)) \
	X(void, DeleteVertexArrays, (int, const unsigned*)) \
	X(void, Disable, (unsigned)) \
	X(void, DrawArrays, (unsigned, int, int)) \
	X(void, Enable, (unsigned)) \
	X(void, GenTextures, (int, unsigned*)) \
	X(void, GenVertexArrays, (int, unsigned*)) \
	X(void, GetProgramInfoLog, (unsigned, int, int*, char*)) \
	X(void, GetProgramiv, (unsigned, unsigned, int*)) \
	X(void, GetShaderInfoLog, (unsigned, int, int*, char*)) \
	X(void, GetShaderiv, (unsigned, unsigned, int*)) \
	X(int, GetUniformLocation, (unsigned, const char*)) \
	X(void, LinkProgram, (unsigned)) \
	X(void, PixelStorei, (unsigned, int)) \
	X(void, Scissor, (int, int, int, int)) \
	X(void, ShaderSource, (unsigned, int, const char* const*, const int*)) \
	X(void, TexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void*)) \
	X(void, TexParameteri, (unsigned, unsigned, int)) \
	X(void, TexSubImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void*)) \
	X(void, Uniform1i, (int, int)) \
	X(void, UseProgram, (unsigned)) \
	X(void, Viewport, (int, int, int, int))

#define BATCH_GL_DECLARE(RET, NAME, ARGS) RET (*NAME) ARGS;
#define BATCH_GL_LOAD(RET, NAME, ARGS) \
	gl->NAME = (RET (*) ARGS) getProc("gl" #NAME); \
	if (!gl->NAME) { \
		mLOG(GBA_VIDEO, ERROR, "Batch renderer: missing gl" #NAME); \
		free(gl); \
		return false; \
	}

enum {
	TEX_VRAM = 0,
	TEX_LINES,
	TEX_PALETTES,
	TEX_SPRITES,
	TEX_LISTS,
	TEX_MAX
};

struct GBAVideoBatchGL {
	BATCH_GL_FUNCTIONS(BATCH_GL_DECLARE)
	uintptr_t (*getFramebuffer)(void);
	unsigned textures[TEX_MAX];
	unsigned program;
	unsigned vao;
	int nPalettes;
	int nSprites;
};

static const char* const _vertexShader =
	"#version 300 es\n"
	"void main() {\n"
	"	vec2 corner = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	gl_Position = vec4(corner, 0.0, 1.0);\n"
	"}\n";

// The batch renderer's _pixel, line for line
static const char* const _fragmentShader =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"precision highp usampler2D;\n"
	"uniform usampler2D vram;\n"
	"uniform usampler2D lines;\n"
	"uniform usampler2D palettes;\n"
	"uniform usampler2D sprites;\n"
	"uniform usampler2D lists;\n"
	"out vec4 outColor;\n"

	"const uint FLAG_PRIORITY = 0xC0000000u;\n"
	"const uint FLAG_IS_BACKGROUND = 0x08000000u;\n"
	"const uint FLAG_UNWRITTEN = 0xFC000000u;\n"
	"const uint FLAG_REBLEND = 0x04000000u;\n"
	"const uint FLAG_TARGET_1 = 0x02000000u;\n"
	"const uint FLAG_TARGET_2 = 0x01000000u;\n"
	"const uint FLAG_OBJWIN = 0x01000000u;\n"
	"const uint FLAG_ORDER_MASK = 0xF8000000u;\n"
	"const int BLEND_ALPHA = 1;\n"
	"const int BLEND_BRIGHTEN = 2;\n"
	"const int BLEND_DARKEN = 3;\n"
	"const ivec2 SIZES[16] = ivec2[16](ivec2(8, 8), ivec2(16, 16), ivec2(32, 32), ivec2(64, 64), ivec2(16, 8), ivec2(32, 8), ivec2(32, 16), ivec2(64, 32),\n"
	"	ivec2(8, 16), ivec2(8, 32), ivec2(16, 32), ivec2(32, 64), ivec2(0, 0), ivec2(0, 0), ivec2(0, 0), ivec2(0, 0));\n"

	"int cdiv(int a, int b) {\n"
	"	int q = abs(a) / abs(b);\n"
	"	return (a < 0) != (b < 0) ? -q : q;\n"
	"}\n"
	"int cmod(int a, int b) {\n"
	"	return a - cdiv(a, b) * b;\n"
	"}\n"
	"uint vram16(uint address) {\n"
	"	uint index = min(address >> 1, 0xBFFFu);\n"
	"	return texelFetch(vram, ivec2(int(index & 2047u), int(index >> 11)), 0).r;\n"
	"}\n"
	"uint vram8(uint address) {\n"
	"	return (vram16(address) >> ((address & 1u) << 3)) & 0xFFu;\n"
	"}\n"
	"uint color555(uint value) {\n"
	"	return ((value & 0x1Fu) << 11) | ((value & 0x3E0u) << 1) | ((value & 0x7C00u) >> 10);\n"
	"}\n"
	"uint brighten(uint color, uint y) {\n"
	"	uint c = 0u;\n"
	"	uint a = color & 0x1Fu;\n"
	"	c |= (a + ((0x1Fu - a) * y) / 16u) & 0x1Fu;\n"
	"	a = color & 0x7C0u;\n"
	"	c |= (a + ((0x7C0u - a) * y) / 16u) & 0x7C0u;\n"
	"	a = color & 0xF800u;\n"
	"	c |= (a + ((0xF800u - a) * y) / 16u) & 0xF800u;\n"
	"	return c;\n"
	"}\n"
	"uint darken(uint color, uint y) {\n"
	"	uint c = 0u;\n"
	"	uint a = color & 0x1Fu;\n"
	"	c |= (a - (a * y) / 16u) & 0x1Fu;\n"
	"	a = color & 0x7C0u;\n"
	"	c |= (a - (a * y) / 16u) & 0x7C0u;\n"
	"	a = color & 0xF800u;\n"
	"	c |= (a - (a * y) / 16u) & 0xF800u;\n"
	"	return c;\n"
	"}\n"
	"uint variant(uint color, int effect, uint bldy) {\n"
	"	if (effect == BLEND_BRIGHTEN) {\n"
	"		return brighten(color, bldy);\n"
	"	}\n"
	"	if (effect == BLEND_DARKEN) {\n"
	"		return darken(color, bldy);\n"
	"	}\n"
	"	return color;\n"
	"}\n"
	"uint mix5(uint weightA, uint colorA, uint weightB, uint colorB) {\n"
	"	uint a = (colorA & 0xF81Fu) | ((colorA & 0x7C0u) << 16);\n"
	"	uint b = (colorB & 0xF81Fu) | ((colorB & 0x7C0u) << 16);\n"
	"	uint c = (a * weightA + b * weightB) / 16u;\n"
	"	if ((c & 0x08000000u) != 0u) {\n"
	"		c = (c & ~0x0FC00000u) | 0x07C00000u;\n"
	"	}\n"
	"	if ((c & 0x0020u) != 0u) {\n"
	"		c = (c & ~0x003Fu) | 0x001Fu;\n"
	"	}\n"
	"	if ((c & 0x10000u) != 0u) {\n"
	"		c = (c & ~0x1F800u) | 0xF800u;\n"
	"	}\n"
	"	return (c & 0xF81Fu) | ((c >> 16) & 0x07C0u);\n"
	"}\n"
	"uint blendObjwin(uint blda, uint bldb, uint color, uint current) {\n"
	"	if (color >= current) {\n"
	"		if ((current & FLAG_TARGET_1) != 0u && (color & FLAG_TARGET_2) != 0u) {\n"
	"			return mix5(blda, current, bldb, color);\n"
	"		}\n"
	"		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);\n"
	"	}\n"
	"	return (color & ~FLAG_TARGET_2) | (current & FLAG_OBJWIN);\n"
	"}\n"
	"uint blendNoObjwin(uint blda, uint bldb, uint color, uint current) {\n"
	"	if (color >= current) {\n"
	"		if ((current & FLAG_TARGET_1) != 0u && (color & FLAG_TARGET_2) != 0u) {\n"
	"			return mix5(blda, current, bldb, color);\n"
	"		}\n"
	"		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);\n"
	"	}\n"
	"	return color & ~FLAG_TARGET_2;\n"
	"}\n"
	"uint noBlendObjwin(uint color, uint current) {\n"
	"	if (color < current) {\n"
	"		return color | (current & FLAG_OBJWIN);\n"
	"	}\n"
	"	return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);\n"
	"}\n"
	"uint noBlendNoObjwin(uint color, uint current) {\n"
	"	if (color >= current) {\n"
	"		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);\n"
	"	}\n"
	"	return color;\n"
	"}\n"
	"bool bit(uint value, int b) {\n"
	"	return ((value >> b) & 1u) != 0u;\n"
	"}\n"

	"uint pixel(int x, int y) {\n"
	"	uvec4 l0 = texelFetch(lines, ivec2(0, y), 0);\n"
	"	uvec4 l1 = texelFetch(lines, ivec2(1, y), 0);\n"
	"	uvec4 l2 = texelFetch(lines, ivec2(2, y), 0);\n"
	"	uint head = l0.x;\n"
	"	if (bit(head, 23)) {\n"
	"		return 0xFFDFu;\n"
	"	}\n"
	"	uint dispcnt = head & 0xFFFFu;\n"
	"	int mode = int(dispcnt & 7u);\n"
	"	int effect = int((head >> 16) & 3u);\n"
	"	uint blda = l0.y & 0x1Fu;\n"
	"	uint bldb = (l0.y >> 5) & 0x1Fu;\n"
	"	uint bldy = (l0.y >> 10) & 0x1Fu;\n"
	"	uint mosaic = l0.y >> 16;\n"
	"	int paletteRow = int(l0.w & 0xFFFFu);\n"
	"	bool target1Obj = bit(head, 19);\n"
	"	bool brightness = effect == BLEND_BRIGHTEN || effect == BLEND_DARKEN;\n"

	"	int nWindows = int(l0.z & 0xFFu);\n"
	"	uint objwin = (l0.z >> 8) & 0xFFu;\n"
	"	int objwinPriority = int(l0.z << 8) >> 24;\n"
	"	uint segments[5] = uint[5](l1.x, l1.y, l1.z, l1.w, l2.x);\n"
	"	int start = 0;\n"
	"	uint segment = segments[0];\n"
	"	for (int w = 0; w < nWindows - 1 && x >= int(segment & 0xFFu); ++w) {\n"
	"		start = int(segment & 0xFFu);\n"
	"		segment = segments[w + 1];\n"
	"	}\n"
	"	int end = int(segment & 0xFFu);\n"
	"	uint control = (segment >> 8) & 0xFFu;\n"
	"	int controlPriority = int(segment << 8) >> 24;\n"
	"	bool objwinEnable = bit(dispcnt, 15);\n"
	"	bool winBlend = bit(control, 5);\n"
	"	bool objwinBlend = bit(objwin, 5);\n"

	"	uvec4 bgs[8];\n"
	"	for (int i = 0; i < 8; ++i) {\n"
	"		bgs[i] = texelFetch(lines, ivec2(3 + i, y), 0);\n"
	"	}\n"
	"	bool target2 = bit(head, 22);\n"
	"	bool anyTarget1 = false;\n"
	"	for (int i = 0; i < 4; ++i) {\n"
	"		uint bg = bgs[i * 2].x;\n"
	"		if (bit(bg, 17) && bit(bg, 1)) {\n"
	"			target2 = true;\n"
	"		}\n"
	"		if (bit(bg, 16)) {\n"
	"			anyTarget1 = true;\n"
	"		}\n"
	"	}\n"

	"	uint backdrop = color555(texelFetch(palettes, ivec2(0, paletteRow), 0).r);\n"
	"	if (bit(head, 20) && brightness && winBlend) {\n"
	"		backdrop = variant(backdrop, effect, bldy);\n"
	"	}\n"
	"	uint row = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND | backdrop;\n"
	"	uint spriteLayer = FLAG_UNWRITTEN;\n"

	"	if (bit(dispcnt, 12) && (bit(control, 4) || objwinEnable)) {\n"
	"		int spriteRow = int(l0.w >> 16);\n"
	"		int mosaicV = int((mosaic >> 12) & 0xFu) + 1;\n"
	"		int mosaicY = y - y % mosaicV;\n"
	"		int n = int(head >> 24);\n"
	"		bool mapping1D = bit(dispcnt, 6);\n"
	"		for (int i = 0; i < n; ++i) {\n"
	"			int index = int(texelFetch(lists, ivec2(i, y), 0).r);\n"
	"			uvec4 s0 = texelFetch(sprites, ivec2(index * 2, spriteRow), 0);\n"
	"			uvec4 s1 = texelFetch(sprites, ivec2(index * 2 + 1, spriteRow), 0);\n"
	"			uint a = s0.x & 0xFFFFu;\n"
	"			uint b = s0.x >> 16;\n"
	"			uint c = s0.y;\n"
	"			int spriteY = int(s0.z << 16) >> 16;\n"
	"			int spriteEndY = int(s0.z) >> 16;\n"
	"			int localY = y;\n"
	"			bool spriteMosaic = bit(a, 12);\n"
	"			if (spriteMosaic && mosaicV > 1) {\n"
	"				localY = mosaicY;\n"
	"				if (localY < spriteY && spriteY < 160) {\n"
	"					localY = spriteY;\n"
	"				}\n"
	"				if (localY >= (spriteEndY & 0xFF)) {\n"
	"					localY = spriteEndY - 1;\n"
	"				}\n"
	"			}\n"
	"			ivec2 size = SIZES[int((a >> 14) * 4u + (b >> 14))];\n"
	"			int width = size.x;\n"
	"			int height = size.y;\n"
	"			int objMode = int((a >> 10) & 3u);\n"
	"			bool is256 = bit(a, 13);\n"
	"			uint flags = ((c >> 10) & 3u) << 30;\n"
	"			if ((winBlend && target1Obj && effect == BLEND_ALPHA) || objMode == 1) {\n"
	"				flags |= FLAG_TARGET_1;\n"
	"			}\n"
	"			if (objMode == 2) {\n"
	"				flags |= FLAG_OBJWIN;\n"
	"			}\n"
	"			if ((flags & FLAG_OBJWIN) != 0u && controlPriority < objwinPriority) {\n"
	"				continue;\n"
	"			}\n"
	"			int spriteX = int((b & 0x1FFu) << 23) >> 23;\n"
	"			uint align = is256 && !mapping1D ? 1u : 0u;\n"
	"			uint tile = c & 0x3FFu;\n"
	"			uint charBase = (tile & ~align) * 0x20u;\n"
	"			uint maskLo = mapping1D ? 0x7FFEu : 0x3FEu;\n"
	"			uint maskHi = mapping1D ? 0u : charBase & 0x7C00u;\n"
	"			if (mode >= 3 && tile < 512u) {\n"
	"				continue;\n"
	"			}\n"
	"			bool objwinSlowPath = objwinEnable && objwinBlend != winBlend;\n"
	"			bool spriteVariant = target1Obj && winBlend && brightness;\n"
	"			if (objMode == 1 || (target1Obj && effect == BLEND_ALPHA) || objwinSlowPath) {\n"
	"				if (target2) {\n"
	"					flags |= FLAG_REBLEND;\n"
	"					spriteVariant = false;\n"
	"				} else {\n"
	"					flags &= ~FLAG_TARGET_1;\n"
	"				}\n"
	"			}\n"
	"			bool objwinVariant = spriteVariant && objwinBlend;\n"
	"			uint paletteBase = 0x100u;\n"
	"			if (!is256) {\n"
	"				paletteBase += (c >> 12) << 4;\n"
	"			}\n"
	"			int inY = localY - int(a & 0xFFu);\n"
	"			int stride = mapping1D ? (width >> (is256 ? 0 : 1)) : 0x80;\n"
	"			int localX;\n"
	"			int tileY;\n"
	"			if (bit(a, 8)) {\n"
	"				int doubleSize = int((a >> 9) & 1u);\n"
	"				int totalWidth = width << doubleSize;\n"
	"				int totalHeight = height << doubleSize;\n"
	"				int pa = int(s0.w << 16) >> 16;\n"
	"				int pb = int(s0.w) >> 16;\n"
	"				int pc = int(s1.x << 16) >> 16;\n"
	"				int pd = int(s1.x) >> 16;\n"
	"				if (inY < 0) {\n"
	"					inY += 256;\n"
	"				}\n"
	"				int outX = spriteX >= start ? spriteX : start;\n"
	"				int condition = spriteX + totalWidth;\n"
	"				int inX = outX - spriteX;\n"
	"				if (end < condition) {\n"
	"					condition = end;\n"
	"				}\n"
	"				int mosaicH = 1;\n"
	"				if (spriteMosaic) {\n"
	"					mosaicH = int((mosaic >> 8) & 0xFu) + 1;\n"
	"					if (condition != end && cmod(condition, mosaicH) != 0) {\n"
	"						condition += mosaicH - cmod(condition, mosaicH);\n"
	"					}\n"
	"				}\n"
	"				int xAccum = pa * (inX - 1 - (totalWidth >> 1)) + pb * (inY - (totalHeight >> 1)) + (width << 7);\n"
	"				int yAccum = pc * (inX - 1 - (totalWidth >> 1)) + pd * (inY - (totalHeight >> 1)) + (height << 7);\n"
	"				if (pa != 0) {\n"
	"					int skip = 0;\n"
	"					if ((xAccum >> 8) < 0) {\n"
	"						skip = cdiv(-xAccum - 1, pa);\n"
	"					} else if ((xAccum >> 8) >= width) {\n"
	"						skip = cdiv((width << 8) - xAccum, pa);\n"
	"					}\n"
	"					xAccum += pa * skip;\n"
	"					yAccum += pc * skip;\n"
	"					outX += skip;\n"
	"				}\n"
	"				if (pc != 0) {\n"
	"					int skip = 0;\n"
	"					if ((yAccum >> 8) < 0) {\n"
	"						skip = cdiv(-yAccum - 1, pc);\n"
	"					} else if ((yAccum >> 8) >= height) {\n"
	"						skip = cdiv((height << 8) - yAccum, pc);\n"
	"					}\n"
	"					xAccum += pa * skip;\n"
	"					yAccum += pc * skip;\n"
	"					outX += skip;\n"
	"				}\n"
	"				if (outX < start || outX >= condition || x < outX || x >= condition) {\n"
	"					continue;\n"
	"				}\n"
	"				uint widthMask = ~uint(width - 1);\n"
	"				uint heightMask = ~uint(height - 1);\n"
	"				int step;\n"
	"				if ((flags & FLAG_OBJWIN) != 0u || mosaicH <= 1) {\n"
	"					if ((uint((xAccum + pa) >> 8) & widthMask) != 0u || (uint((yAccum + pc) >> 8) & heightMask) != 0u) {\n"
	"						continue;\n"
	"					}\n"
	"					step = x - outX + 1;\n"
	"				} else {\n"
	"					int sampleX = x - x % mosaicH;\n"
	"					step = sampleX >= outX ? sampleX - outX + 1 : 0;\n"
	"				}\n"
	"				localX = (xAccum + pa * step) >> 8;\n"
	"				tileY = (yAccum + pc * step) >> 8;\n"
	"				if ((uint(localX) & widthMask) != 0u || (uint(tileY) & heightMask) != 0u) {\n"
	"					continue;\n"
	"				}\n"
	"			} else {\n"
	"				int outX = spriteX >= start ? spriteX : start;\n"
	"				int condition = spriteX + width;\n"
	"				int mosaicH = 1;\n"
	"				if (spriteMosaic) {\n"
	"					mosaicH = int((mosaic >> 8) & 0xFu) + 1;\n"
	"					if (cmod(condition, mosaicH) != 0) {\n"
	"						condition += mosaicH - cmod(condition, mosaicH);\n"
	"					}\n"
	"				}\n"
	"				if (int(a & 0xFFu) + height - 256 >= 0) {\n"
	"					inY += 256;\n"
	"				}\n"
	"				if (bit(b, 13)) {\n"
	"					inY = height - inY - 1;\n"
	"				}\n"
	"				if (end < condition) {\n"
	"					condition = end;\n"
	"				}\n"
	"				if (x < outX || x >= condition) {\n"
	"					continue;\n"
	"				}\n"
	"				int inX = x - spriteX;\n"
	"				int xOffset = 1;\n"
	"				if (bit(b, 12)) {\n"
	"					inX = width - inX - 1;\n"
	"					xOffset = -1;\n"
	"				}\n"
	"				localX = inX;\n"
	"				if ((flags & FLAG_OBJWIN) == 0u && mosaicH > 1) {\n"
	"					localX = clamp(inX - xOffset * (x % mosaicH), 0, width - 1);\n"
	"				}\n"
	"				tileY = inY;\n"
	"			}\n"

	"			uint tileData;\n"
	"			if (!is256) {\n"
	"				uint xBase = uint((localX & ~7) * 4 + ((localX >> 1) & 2));\n"
	"				uint yBase = uint((tileY & ~7) * stride + (tileY & 7) * 4) + maskHi;\n"
	"				tileData = vram16(0x10000u + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFEu));\n"
	"				tileData = (tileData >> ((localX & 3) << 2)) & 0xFu;\n"
	"			} else {\n"
	"				uint xBase = uint((localX & ~7) * 8 + (localX & 6));\n"
	"				uint yBase = uint((tileY & ~7) * stride + (tileY & 7) * 8) + maskHi;\n"
	"				tileData = vram16(0x10000u + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFEu));\n"
	"				tileData = (tileData >> ((localX & 1) << 3)) & 0xFFu;\n"
	"			}\n"

	"			uint current = spriteLayer;\n"
	"			uint reordered = (current & ~(FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1)) | (flags & (FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1));\n"
	"			if ((flags & FLAG_OBJWIN) != 0u) {\n"
	"				if (tileData != 0u) {\n"
	"					row |= FLAG_OBJWIN;\n"
	"				} else if (current != FLAG_UNWRITTEN && (current & FLAG_ORDER_MASK) > flags) {\n"
	"					spriteLayer = reordered;\n"
	"				}\n"
	"			} else if ((current & FLAG_ORDER_MASK) > flags) {\n"
	"				if (tileData != 0u) {\n"
	"					uint color = color555(texelFetch(palettes, ivec2(int(paletteBase + tileData), paletteRow), 0).r);\n"
	"					if ((objwinSlowPath && (row & FLAG_OBJWIN) != 0u) ? objwinVariant : spriteVariant) {\n"
	"						color = variant(color, effect, bldy);\n"
	"					}\n"
	"					spriteLayer = color | flags;\n"
	"				} else if (current != FLAG_UNWRITTEN) {\n"
	"					spriteLayer = reordered;\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"	}\n"

	"	for (uint priority = 0u; priority < 4u; ++priority) {\n"
	"		uint sprite = spriteLayer & ~FLAG_OBJWIN;\n"
	"		if ((sprite & FLAG_UNWRITTEN) != FLAG_UNWRITTEN && (sprite >> 30) == priority) {\n"
	"			if (bit(head, 21)) {\n"
	"				sprite |= FLAG_TARGET_2;\n"
	"			}\n"
	"			if (objwinEnable) {\n"
	"				bool objwinDisable = !bit(objwin, 4);\n"
	"				bool objwinOnly = !objwinDisable && !bit(control, 4);\n"
	"				if (objwinDisable && !bit(control, 4)) {\n"
	"				} else if (objwinDisable ? (row & FLAG_OBJWIN) == 0u : (!objwinOnly || (row & FLAG_OBJWIN) != 0u)) {\n"
	"					row = blendObjwin(blda, bldb, sprite, row);\n"
	"				}\n"
	"			} else if (bit(control, 4)) {\n"
	"				row = blendNoObjwin(blda, bldb, sprite, row);\n"
	"			}\n"
	"		}\n"

	"		for (int index = 0; index < 4; ++index) {\n"
	"			uvec4 bgA = bgs[index * 2];\n"
	"			uvec4 bgB = bgs[index * 2 + 1];\n"
	"			uint bgControl = bgA.x;\n"
	"			if (!bit(bgControl, 0) || ((bgControl >> 2) & 3u) != priority) {\n"
	"				continue;\n"
	"			}\n"
	"			bool windowed = bit(control, index);\n"
	"			bool objwinBg = bit(objwin, index);\n"
	"			if (!windowed && !(objwinEnable && objwinBg)) {\n"
	"				continue;\n"
	"			}\n"
	"			int bgMode;\n"
	"			if (index < 2) {\n"
	"				if (mode >= 2) {\n"
	"					continue;\n"
	"				}\n"
	"				bgMode = 0;\n"
	"			} else if (index == 2) {\n"
	"				if (mode > 5) {\n"
	"					continue;\n"
	"				}\n"
	"				bgMode = mode == 1 ? 2 : mode;\n"
	"			} else {\n"
	"				if (mode != 0 && mode != 2) {\n"
	"					continue;\n"
	"				}\n"
	"				bgMode = mode;\n"
	"			}\n"

	"			bool target1 = bit(bgControl, 16);\n"
	"			uint flags = (priority << 30) | (uint(index) << 28) | FLAG_IS_BACKGROUND;\n"
	"			if (bit(bgControl, 17)) {\n"
	"				flags |= FLAG_TARGET_2;\n"
	"			}\n"
	"			uint objwinFlags = flags;\n"
	"			if (effect == BLEND_ALPHA) {\n"
	"				if (blda == 0x10u && bldb == 0u) {\n"
	"					flags &= ~FLAG_TARGET_2;\n"
	"					objwinFlags &= ~FLAG_TARGET_2;\n"
	"				} else if (target1) {\n"
	"					if (winBlend) {\n"
	"						flags |= FLAG_TARGET_1;\n"
	"					}\n"
	"					if (objwinBlend) {\n"
	"						objwinFlags |= FLAG_TARGET_1;\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"			bool bgVariant = target1 && winBlend && brightness;\n"
	"			bool objwinVariant = objwinEnable && target1 && objwinBlend && brightness;\n"
	"			bool objwinForceEnable = objwinBg && windowed;\n"
	"			bool objwinOnly = !objwinBg;\n"
	"			bool bgMosaic = bit(bgControl, 14);\n"
	"			int mosaicV = int((mosaic >> 4) & 0xFu) + 1;\n"
	"			int mosaicH = int(mosaic & 0xFu) + 1;\n"

	"			if (bgMode >= 2) {\n"
	"				int sx = int(bgA.z);\n"
	"				int sy = int(bgA.w);\n"
	"				int dx = int(bgB.x << 16) >> 16;\n"
	"				int dy = int(bgB.x) >> 16;\n"
	"				int sampleX = x;\n"
	"				if (bgMosaic) {\n"
	"					sx -= (y % mosaicV) * (int(bgB.y << 16) >> 16);\n"
	"					sy -= (y % mosaicV) * (int(bgB.y) >> 16);\n"
	"					sampleX = x - x % mosaicH;\n"
	"				}\n"
	"				int localX = sx + sampleX * dx;\n"
	"				int localY = sy + sampleX * dy;\n"
	"				if (bgMode == 3 || bgMode == 5) {\n"
	"					int width = bgMode == 3 ? 240 : 160;\n"
	"					int height = bgMode == 3 ? 160 : 128;\n"
	"					if (localX < 0 || localY < 0 || (localX >> 8) >= width || (localY >> 8) >= height) {\n"
	"						continue;\n"
	"					}\n"
	"					uint color;\n"
	"					if (bgMode == 3) {\n"
	"						color = color555(vram16(uint(((localX >> 8) + (localY >> 8) * 240) << 1)));\n"
	"					} else {\n"
	"						uint offset = bit(dispcnt, 4) ? 0xA000u : 0u;\n"
	"						color = color555(vram16(offset + uint((localX >> 8) * 2 + (localY >> 8) * 320)));\n"
	"					}\n"
	"					if (!objwinEnable || ((row & FLAG_OBJWIN) == 0u) != objwinOnly) {\n"
	"						uint mergedFlags = (row & FLAG_OBJWIN) != 0u ? objwinFlags : flags;\n"
	"						if (bgVariant) {\n"
	"							color = variant(color, effect, bldy);\n"
	"						}\n"
	"						row = blendObjwin(blda, bldb, color | mergedFlags, row);\n"
	"					}\n"
	"					continue;\n"
	"				}\n"
	"				if (bgMode == 4) {\n"
	"					if (localX < 0 || localY < 0 || (localX >> 8) >= 240 || (localY >> 8) >= 160) {\n"
	"						continue;\n"
	"					}\n"
	"					uint offset = bit(dispcnt, 4) ? 0xA000u : 0u;\n"
	"					uint entry = vram8(offset + uint((localX >> 8) + (localY >> 8) * 240));\n"
	"					if (entry == 0u || (row & 0xFE000000u) == 0u) {\n"
	"						continue;\n"
	"					}\n"
	"					uint color = color555(texelFetch(palettes, ivec2(int(entry), paletteRow), 0).r);\n"
	"					if (!objwinEnable) {\n"
	"						if (bgVariant) {\n"
	"							color = variant(color, effect, bldy);\n"
	"						}\n"
	"						row = blendNoObjwin(blda, bldb, color | flags, row);\n"
	"					} else if (objwinForceEnable || ((row & FLAG_OBJWIN) == 0u) == objwinOnly) {\n"
	"						if ((row & FLAG_OBJWIN) != 0u ? objwinVariant : bgVariant) {\n"
	"							color = variant(color, effect, bldy);\n"
	"						}\n"
	"						row = blendObjwin(blda, bldb, color | ((row & FLAG_OBJWIN) != 0u ? objwinFlags : flags), row);\n"
	"					}\n"
	"					continue;\n"
	"				}\n"
	"			}\n"

	"			uint pixelData;\n"
	"			uint paletteData = 0u;\n"
	"			if (bgMode == 0) {\n"
	"				int sampleX = x;\n"
	"				int inY = y;\n"
	"				if (bgMosaic) {\n"
	"					if ((mosaic & 0xFu) != 0u) {\n"
	"						sampleX = x - x % mosaicH;\n"
	"					}\n"
	"					inY -= inY % mosaicV;\n"
	"				}\n"
	"				int inX = (sampleX + int(bgA.y & 0xFFFFu)) & 0x1FF;\n"
	"				inY += int(bgA.y >> 16);\n"
	"				uint size = (bgControl >> 11) & 3u;\n"
	"				uint yBase = uint(inY & 0xF8);\n"
	"				if (size == 2u) {\n"
	"					yBase += uint(inY & 0x100);\n"
	"				} else if (size == 3u) {\n"
	"					yBase += uint(inY & 0x100) << 1;\n"
	"				}\n"
	"				yBase = ((((bgControl >> 6) & 0x1Fu) << 11) >> 1) + (yBase << 2);\n"
	"				uint xBase = uint(inX & 0xF8);\n"
	"				if ((size & 1u) != 0u) {\n"
	"					xBase += uint(inX & 0x100) << 5;\n"
	"				}\n"
	"				uint mapData = vram16((yBase + (xBase >> 3)) << 1);\n"
	"				int localY = inY & 7;\n"
	"				if (bit(mapData, 11)) {\n"
	"					localY = 7 - localY;\n"
	"				}\n"
	"				int column = inX & 7;\n"
	"				if (bit(mapData, 10)) {\n"
	"					column = 7 - column;\n"
	"				}\n"
	"				uint charBase = ((bgControl >> 4) & 3u) << 14;\n"
	"				if (!bit(bgControl, 13)) {\n"
	"					charBase += ((mapData & 0x3FFu) << 5) + uint(localY << 2);\n"
	"					if (charBase >= 0x10000u) {\n"
	"						continue;\n"
	"					}\n"
	"					pixelData = vram8(charBase + uint(column >> 1));\n"
	"					pixelData = (column & 1) != 0 ? pixelData >> 4 : pixelData & 0xFu;\n"
	"					paletteData = (mapData >> 12) << 4;\n"
	"				} else {\n"
	"					charBase += ((mapData & 0x3FFu) << 6) + uint(localY << 3);\n"
	"					if (charBase >= 0x10000u) {\n"
	"						continue;\n"
	"					}\n"
	"					pixelData = vram8(charBase + uint(column));\n"
	"				}\n"
	"			} else {\n"
	"				int sx = int(bgA.z);\n"
	"				int sy = int(bgA.w);\n"
	"				int dx = int(bgB.x << 16) >> 16;\n"
	"				int dy = int(bgB.x) >> 16;\n"
	"				int sampleX = x;\n"
	"				if (bgMosaic) {\n"
	"					sx -= (y % mosaicV) * (int(bgB.y << 16) >> 16);\n"
	"					sy -= (y % mosaicV) * (int(bgB.y) >> 16);\n"
	"					sampleX = x - x % mosaicH;\n"
	"				}\n"
	"				int localX = sx + sampleX * dx;\n"
	"				int localY = sy + sampleX * dy;\n"
	"				int size = int((bgControl >> 11) & 3u);\n"
	"				int sizeAdjusted = 0x8000 << size;\n"
	"				if (bit(bgControl, 15)) {\n"
	"					localX &= sizeAdjusted - 1;\n"
	"					localY &= sizeAdjusted - 1;\n"
	"				} else if (((localX | localY) & ~(sizeAdjusted - 1)) != 0) {\n"
	"					continue;\n"
	"				}\n"
	"				uint screenBase = ((bgControl >> 6) & 0x1Fu) << 11;\n"
	"				uint charBase = ((bgControl >> 4) & 3u) << 14;\n"
	"				uint mapData = vram8(screenBase + uint(localX >> 11) + (uint((localY >> 7) & 0x7F0) << size));\n"
	"				pixelData = vram8(charBase + (mapData << 6) + uint((localY & 0x700) >> 5) + uint((localX & 0x700) >> 8));\n"
	"			}\n"

	"			if (pixelData == 0u || (row & 0xFE000000u) == 0u) {\n"
	"				continue;\n"
	"			}\n"
	"			uint color = color555(texelFetch(palettes, ivec2(int(paletteData | pixelData), paletteRow), 0).r);\n"
	"			bool reblend = (row & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND;\n"
	"			if (!objwinEnable) {\n"
	"				if (bgVariant && !reblend) {\n"
	"					color = variant(color, effect, bldy);\n"
	"				}\n"
	"				if ((flags & FLAG_TARGET_2) != 0u) {\n"
	"					row = blendNoObjwin(blda, bldb, color | flags, row);\n"
	"				} else {\n"
	"					row = noBlendNoObjwin(color | flags, row);\n"
	"				}\n"
	"			} else if (objwinForceEnable || ((row & FLAG_OBJWIN) == 0u) == objwinOnly) {\n"
	"				uint mergedFlags = flags;\n"
	"				if ((row & FLAG_OBJWIN) != 0u) {\n"
	"					mergedFlags = objwinFlags;\n"
	"					if (objwinVariant) {\n"
	"						color = variant(color, effect, bldy);\n"
	"					}\n"
	"				} else if (bgVariant && !reblend) {\n"
	"					color = variant(color, effect, bldy);\n"
	"				}\n"
	"				if ((flags & FLAG_TARGET_2) != 0u) {\n"
	"					row = blendObjwin(blda, bldb, color | mergedFlags, row);\n"
	"				} else {\n"
	"					row = noBlendObjwin(color | mergedFlags, row);\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"	}\n"

	"	if ((bit(head, 18) || anyTarget1) && bit(head, 22)) {\n"
	"		if ((row & FLAG_TARGET_1) != 0u) {\n"
	"			uint color = color555(texelFetch(palettes, ivec2(0, paletteRow), 0).r);\n"
	"			if (bit(head, 20) && brightness && winBlend) {\n"
	"				color = variant(color, effect, bldy);\n"
	"			}\n"
	"			row = mix5(bldb, color, blda, row);\n"
	"		}\n"
	"	}\n"
	"	if (bit(head, 18) && brightness) {\n"
	"		uint mask = FLAG_REBLEND | FLAG_IS_BACKGROUND;\n"
	"		uint match = FLAG_REBLEND;\n"
	"		bool apply = true;\n"
	"		if (objwinEnable && objwinBlend != winBlend) {\n"
	"			mask |= FLAG_OBJWIN;\n"
	"			if (objwinBlend) {\n"
	"				match |= FLAG_OBJWIN;\n"
	"			}\n"
	"		} else if (!winBlend) {\n"
	"			apply = false;\n"
	"		}\n"
	"		if (apply && (row & mask) == match) {\n"
	"			row = variant(row, effect, bldy);\n"
	"		}\n"
	"	}\n"
	"	return row & 0xFFFFu;\n"
	"}\n"

	"void main() {\n"
	"	ivec2 position = ivec2(gl_FragCoord.xy);\n"
	"	uint color = pixel(position.x, 159 - position.y);\n"
	"	uvec3 rgb = uvec3(color >> 11, (color >> 6) & 0x1Fu, color & 0x1Fu);\n"
	"	outColor = vec4(vec3((rgb << 3) | (rgb >> 2)) / 255.0, 1.0);\n"
	"}\n";

static unsigned _compile(struct GBAVideoBatchGL* gl, unsigned type, const char* source) {
	unsigned shader = gl->CreateShader(type);
	gl->ShaderSource(shader, 1, &source, NULL);
	gl->CompileShader(shader);
	int ok = 0;
	gl->GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		gl->GetShaderInfoLog(shader, sizeof(log), NULL, log);
		mLOG(GBA_VIDEO, ERROR, "Batch renderer shader: %s", log);
		gl->DeleteShader(shader);
		return 0;
	}
	return shader;
}

static void _texture(struct GBAVideoBatchGL* gl, int unit, int internalFormat, int width, int height, unsigned format, unsigned type) {
	gl->ActiveTexture(GL_TEXTURE0 + unit);
	gl->BindTexture(GL_TEXTURE_2D, gl->textures[unit]);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl->TexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, NULL);
}

bool GBAVideoBatchRendererInitGL(struct GBAVideoBatchRenderer* batch, void* (*getProc)(const char*), uintptr_t (*getFramebuffer)(void)) {
	struct GBAVideoBatchGL* gl = calloc(1, sizeof(*gl));
	BATCH_GL_FUNCTIONS(BATCH_GL_LOAD)
	gl->getFramebuffer = getFramebuffer;

	unsigned vertex = _compile(gl, GL_VERTEX_SHADER, _vertexShader);
	unsigned fragment = _compile(gl, GL_FRAGMENT_SHADER, _fragmentShader);
	if (!vertex || !fragment) {
		free(gl);
		return false;
	}
	gl->program = gl->CreateProgram();
	gl->AttachShader(gl->program, vertex);
	gl->AttachShader(gl->program, fragment);
	gl->LinkProgram(gl->program);
	gl->DeleteShader(vertex);
	gl->DeleteShader(fragment);
	int ok = 0;
	gl->GetProgramiv(gl->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];
		gl->GetProgramInfoLog(gl->program, sizeof(log), NULL, log);
		mLOG(GBA_VIDEO, ERROR, "Batch renderer program: %s", log);
		gl->DeleteProgram(gl->program);
		free(gl);
		return false;
	}
	gl->UseProgram(gl->program);
	static const char* const names[TEX_MAX] = { "vram", "lines", "palettes", "sprites", "lists" };
	int i;
	for (i = 0; i < TEX_MAX; ++i) {
		gl->Uniform1i(gl->GetUniformLocation(gl->program, names[i]), i);
	}

	gl->GenTextures(TEX_MAX, gl->textures);
	gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	_texture(gl, TEX_VRAM, GL_R16UI, 2048, BATCH_VRAM_PAGES, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
	_texture(gl, TEX_LINES, GL_RGBA32UI, BATCH_LINE_WORDS / 4, GBA_VIDEO_VERTICAL_PIXELS, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
	_texture(gl, TEX_PALETTES, GL_R16UI, 512, GBA_VIDEO_VERTICAL_PIXELS, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
	_texture(gl, TEX_SPRITES, GL_RGBA32UI, 256, GBA_VIDEO_VERTICAL_PIXELS, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
	_texture(gl, TEX_LISTS, GL_R8UI, 128, GBA_VIDEO_VERTICAL_PIXELS, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	gl->GenVertexArrays(1, &gl->vao);

	batch->gl = gl;
	batch->vramDirty = (1U << BATCH_VRAM_PAGES) - 1;
	return true;
}

void GBAVideoBatchRendererDeinitGL(struct GBAVideoBatchRenderer* batch) {
	struct GBAVideoBatchGL* gl = batch->gl;
	if (!gl) {
		return;
	}
	gl->DeleteTextures(TEX_MAX, gl->textures);
	gl->DeleteVertexArrays(1, &gl->vao);
	gl->DeleteProgram(gl->program);
	free(gl);
	batch->gl = NULL;
}

static void _unpack(struct GBAVideoBatchGL* gl, int unit) {
	gl->ActiveTexture(GL_TEXTURE0 + unit);
	gl->BindTexture(GL_TEXTURE_2D, gl->textures[unit]);
}

void GBAVideoBatchGLUploadVRAM(struct GBAVideoBatchRenderer* batch, uint32_t pages) {
	struct GBAVideoBatchGL* gl = batch->gl;
	gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 2);
	gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	_unpack(gl, TEX_VRAM);
	while (pages) {
		unsigned first = __builtin_ctz(pages);
		unsigned count = __builtin_ctz(~(pages >> first));
		pages &= ~(((1U << count) - 1) << first);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, first, 2048, count, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &batch->d.vram[first << 11]);
	}
}

void GBAVideoBatchGLDraw(struct GBAVideoBatchRenderer* batch, int startY, int endY) {
	struct GBAVideoBatchGL* gl = batch->gl;
	gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	_unpack(gl, TEX_LINES);
	gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, startY, BATCH_LINE_WORDS / 4, endY - startY, GL_RGBA_INTEGER, GL_UNSIGNED_INT, batch->lines[startY]);
	_unpack(gl, TEX_LISTS);
	gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, startY, 128, endY - startY, GL_RED_INTEGER, GL_UNSIGNED_BYTE, batch->spriteLists[startY]);
	if (batch->nPalettes > gl->nPalettes) {
		_unpack(gl, TEX_PALETTES);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, gl->nPalettes, 512, batch->nPalettes - gl->nPalettes, GL_RED_INTEGER, GL_UNSIGNED_SHORT, batch->palettes[gl->nPalettes]);
		gl->nPalettes = batch->nPalettes;
	}
	if (batch->nSprites > gl->nSprites) {
		_unpack(gl, TEX_SPRITES);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, gl->nSprites, 256, batch->nSprites - gl->nSprites, GL_RGBA_INTEGER, GL_UNSIGNED_INT, batch->sprites[gl->nSprites]);
		gl->nSprites = batch->nSprites;
	}

	int i;
	for (i = 0; i < TEX_MAX; ++i) {
		gl->ActiveTexture(GL_TEXTURE0 + i);
		gl->BindTexture(GL_TEXTURE_2D, gl->textures[i]);
		gl->BindSampler(i, 0);
	}
	gl->BindFramebuffer(GL_FRAMEBUFFER, gl->getFramebuffer());
	gl->UseProgram(gl->program);
	gl->BindVertexArray(gl->vao);
	gl->Disable(GL_BLEND);
	gl->Disable(GL_DEPTH_TEST);
	gl->Disable(GL_STENCIL_TEST);
	gl->Disable(GL_CULL_FACE);
	gl->Disable(GL_DITHER);
	gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl->Viewport(0, 0, GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
	gl->Enable(GL_SCISSOR_TEST);
	gl->Scissor(0, GBA_VIDEO_VERTICAL_PIXELS - endY, GBA_VIDEO_HORIZONTAL_PIXELS, endY - startY);
	gl->DrawArrays(GL_TRIANGLES, 0, 3);
	gl->Disable(GL_SCISSOR_TEST);
}

void GBAVideoBatchGLStartFrame(struct GBAVideoBatchRenderer* batch) {
	struct GBAVideoBatchGL* gl = batch->gl;
	gl->nPalettes = 0;
	gl->nSprites = 0;
}
