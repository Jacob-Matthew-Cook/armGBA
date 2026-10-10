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
#define GL_RGBA16UI 0x8D76
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
	X(void, Finish, (void)) \
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
	TEX_MAX
};

// Each draw fills its own set of textures, so none is written while the GPU may still read it
#define BATCH_GL_RING 4

struct GBAVideoBatchGL {
	BATCH_GL_FUNCTIONS(BATCH_GL_DECLARE)
	uintptr_t (*getFramebuffer)(void);
	unsigned textures[BATCH_GL_RING][TEX_MAX];
	uint32_t vramStale[BATCH_GL_RING];
	int slot;
	unsigned programs[BATCH_FEATURE_ALL + 1];
	unsigned vao;
};

static const char* const _vertexShader =
	"#version 300 es\n"
	"void main() {\n"
	"	vec2 corner = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	gl_Position = vec4(corner, 0.0, 1.0);\n"
	"}\n";

// The batch renderer's pixel function, shared with the C reference
static const char* const _fragmentPrelude =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"precision highp usampler2D;\n"
	"uniform usampler2D vram;\n"
	"uniform usampler2D lines;\n"
	"uniform usampler2D palettes;\n"
	"uniform usampler2D sprites;\n"
	"out vec4 outColor;\n"
	"#define BATCH_FN\n"
	"#define U(X) uint(X)\n"
	"#define I(X) int(X)\n"
	"#define iabs(X) abs(X)\n"
	"#define FLAG_PRIORITY 0xC0000000u\n"
	"#define FLAG_IS_BACKGROUND 0x08000000u\n"
	"#define FLAG_UNWRITTEN 0xFC000000u\n"
	"#define FLAG_REBLEND 0x04000000u\n"
	"#define FLAG_TARGET_1 0x02000000u\n"
	"#define FLAG_TARGET_2 0x01000000u\n"
	"#define FLAG_OBJWIN 0x01000000u\n"
	"#define FLAG_ORDER_MASK 0xF8000000u\n"
	"#define BLEND_ALPHA 1\n"
	"#define BLEND_BRIGHTEN 2\n"
	"#define BLEND_DARKEN 3\n"
	"#define FETCH_LINE(Y, T) texelFetch(lines, ivec2(T, Y), 0)\n"
	"#define FETCH_SPRITE(R, T) texelFetch(sprites, ivec2(T, R), 0)\n"
	"#define FETCH_PAL(R, E) texelFetch(palettes, ivec2(E, R), 0).r\n"
	"#define FETCH_VRAM16(H) texelFetch(vram, ivec2(int(min(H, 0xBFFFu) & 2047u), int(min(H, 0xBFFFu) >> 11)), 0).r\n";

#define BATCH_SHARED(...) #__VA_ARGS__
static const char* const _fragmentShared =
#include "gba/renderers/video-batch-pixel.h"
;
#undef BATCH_SHARED

static const char* const _fragmentMain =
	"\nvoid main() {\n"
	"	ivec2 position = ivec2(gl_FragCoord.xy);\n"
	"	uint color = batchPixel(position.x, 159 - position.y);\n"
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

static void _texture(struct GBAVideoBatchGL* gl, int slot, int unit, int internalFormat, int width, int height, unsigned format, unsigned type) {
	gl->ActiveTexture(GL_TEXTURE0 + unit);
	gl->BindTexture(GL_TEXTURE_2D, gl->textures[slot][unit]);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl->TexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, NULL);
}

// The shader for one set of features, compiled the first time a frame needs it
static unsigned _program(struct GBAVideoBatchGL* gl, unsigned features) {
	if (gl->programs[features]) {
		return gl->programs[features];
	}
	static const char* const names[] = { "OBJWIN", "MOSAIC", "AFFINE_SPRITES", "AFFINE_BG", "BRIGHTNESS", "BLEND", "WINDOWS" };
	char defines[512] = "";
	size_t used = 0;
	unsigned i;
	for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		used += snprintf(&defines[used], sizeof(defines) - used, "#define FEATURE_%s %s\n", names[i], features & (1 << i) ? "true" : "false");
	}
	size_t length = strlen(_fragmentPrelude) + used + strlen(_fragmentShared) + strlen(_fragmentMain) + 1;
	char* source = malloc(length);
	snprintf(source, length, "%s%s%s%s", _fragmentPrelude, defines, _fragmentShared, _fragmentMain);
	unsigned vertex = _compile(gl, GL_VERTEX_SHADER, _vertexShader);
	unsigned fragment = _compile(gl, GL_FRAGMENT_SHADER, source);
	free(source);
	if (!vertex || !fragment) {
		return 0;
	}
	unsigned program = gl->CreateProgram();
	gl->AttachShader(program, vertex);
	gl->AttachShader(program, fragment);
	gl->LinkProgram(program);
	gl->DeleteShader(vertex);
	gl->DeleteShader(fragment);
	int ok = 0;
	gl->GetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];
		gl->GetProgramInfoLog(program, sizeof(log), NULL, log);
		mLOG(GBA_VIDEO, ERROR, "Batch renderer program: %s", log);
		gl->DeleteProgram(program);
		return 0;
	}
	gl->UseProgram(program);
	static const char* const samplers[TEX_MAX] = { "vram", "lines", "palettes", "sprites" };
	for (i = 0; i < TEX_MAX; ++i) {
		gl->Uniform1i(gl->GetUniformLocation(program, samplers[i]), i);
	}
	gl->programs[features] = program;
	return program;
}

bool GBAVideoBatchRendererInitGL(struct GBAVideoBatchRenderer* batch, void* (*getProc)(const char*), uintptr_t (*getFramebuffer)(void)) {
	struct GBAVideoBatchGL* gl = calloc(1, sizeof(*gl));
	BATCH_GL_FUNCTIONS(BATCH_GL_LOAD)
	gl->getFramebuffer = getFramebuffer;
	// The shader that does everything, and the one most frames need
	if (!_program(gl, BATCH_FEATURE_ALL) || !_program(gl, 0)) {
		free(gl);
		return false;
	}

	gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	int slot;
	for (slot = 0; slot < BATCH_GL_RING; ++slot) {
		gl->GenTextures(TEX_MAX, gl->textures[slot]);
		_texture(gl, slot, TEX_VRAM, GL_R16UI, 2048, BATCH_VRAM_PAGES, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
		_texture(gl, slot, TEX_LINES, GL_RGBA32UI, BATCH_LINE_WORDS / 4, GBA_VIDEO_VERTICAL_PIXELS, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
		_texture(gl, slot, TEX_PALETTES, GL_R16UI, 512, GBA_VIDEO_VERTICAL_PIXELS, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
		_texture(gl, slot, TEX_SPRITES, GL_RGBA32UI, 256, GBA_VIDEO_VERTICAL_PIXELS, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
		gl->vramStale[slot] = (1U << BATCH_VRAM_PAGES) - 1;
	}
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
	int slot;
	for (slot = 0; slot < BATCH_GL_RING; ++slot) {
		gl->DeleteTextures(TEX_MAX, gl->textures[slot]);
	}
	gl->DeleteVertexArrays(1, &gl->vao);
	unsigned i;
	for (i = 0; i <= BATCH_FEATURE_ALL; ++i) {
		if (gl->programs[i]) {
			gl->DeleteProgram(gl->programs[i]);
		}
	}
	free(gl);
	batch->gl = NULL;
}

static void _unpack(struct GBAVideoBatchGL* gl, int unit) {
	gl->ActiveTexture(GL_TEXTURE0 + unit);
	gl->BindTexture(GL_TEXTURE_2D, gl->textures[gl->slot][unit]);
}

// The batch renderer's copy of VRAM moved on; every texture set catches up before it next draws
void GBAVideoBatchGLUploadVRAM(struct GBAVideoBatchRenderer* batch, uint32_t pages) {
	struct GBAVideoBatchGL* gl = batch->gl;
	int slot;
	for (slot = 0; slot < BATCH_GL_RING; ++slot) {
		gl->vramStale[slot] |= pages;
	}
}

#include <time.h>
static double _glNow(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}
static double _glTime[6];
static unsigned _glDraws;

void GBAVideoBatchGLDraw(struct GBAVideoBatchRenderer* batch, int startY, int endY) {
	struct GBAVideoBatchGL* gl = batch->gl;
	static int timed = -1;
	if (timed < 0) {
		timed = getenv("ARMGBA_BATCH_GLTIME") != NULL;
	}
	double t0 = timed ? _glNow() : 0;
	gl->slot = (gl->slot + 1) % BATCH_GL_RING;
	gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 2);
	gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	uint32_t pages = gl->vramStale[gl->slot];
	gl->vramStale[gl->slot] = 0;
	if (pages) {
		_unpack(gl, TEX_VRAM);
	}
	while (pages) {
		unsigned first = __builtin_ctz(pages);
		unsigned count = __builtin_ctz(~(pages >> first));
		pages &= ~(((1U << count) - 1) << first);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, first, 2048, count, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &batch->vram[first << 11]);
	}
	double t1 = timed ? _glNow() : 0;

	// Only as much of each line as this batch uses: one window segment, and the longest sprite list
	unsigned features = 0;
	int width = 7;
	int palettes[2] = { 0xFF, 0 };
	int sprites[2] = { 0xFF, -1 };
	int y;
	for (y = startY; y < endY; ++y) {
		const uint32_t* line = batch->lines[y];
		features |= batch->features[y];
		int palette = (line[2] >> 16) & 0xFF;
		if (palette < palettes[0]) {
			palettes[0] = palette;
		}
		if (palette > palettes[1]) {
			palettes[1] = palette;
		}
		int count = line[3] & 0xFF;
		if (count) {
			int row = line[2] >> 24;
			if (row < sprites[0]) {
				sprites[0] = row;
			}
			if (row > sprites[1]) {
				sprites[1] = row;
			}
			if (width < 7 + (count + 7) / 8) {
				width = 7 + (count + 7) / 8;
			}
		}
	}
	if (features & BATCH_FEATURE_WINDOWS) {
		width = 31;
	}
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl->PixelStorei(GL_UNPACK_ROW_LENGTH, BATCH_LINE_WORDS / 4);
	_unpack(gl, TEX_LINES);
	gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, startY, width, endY - startY, GL_RGBA_INTEGER, GL_UNSIGNED_INT, batch->lines[startY]);
	gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	double t2 = timed ? _glNow() : 0;
	if (palettes[0] <= palettes[1]) {
		_unpack(gl, TEX_PALETTES);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, palettes[0], 512, palettes[1] - palettes[0] + 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, batch->palettes[palettes[0]]);
	}
	if (sprites[0] <= sprites[1]) {
		_unpack(gl, TEX_SPRITES);
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, sprites[0], 256, sprites[1] - sprites[0] + 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, batch->sprites[sprites[0]]);
	}

	double t3 = timed ? _glNow() : 0;
	int i;
	for (i = 0; i < TEX_MAX; ++i) {
		gl->ActiveTexture(GL_TEXTURE0 + i);
		gl->BindTexture(GL_TEXTURE_2D, gl->textures[gl->slot][i]);
		gl->BindSampler(i, 0);
	}
	unsigned program = _program(gl, features);
	if (!program) {
		program = gl->programs[BATCH_FEATURE_ALL];
	}
	gl->BindFramebuffer(GL_FRAMEBUFFER, gl->getFramebuffer());
	gl->UseProgram(program);
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
	static int repeat = -1;
	if (repeat < 0) {
		repeat = getenv("ARMGBA_BATCH_REPEAT") ? atoi(getenv("ARMGBA_BATCH_REPEAT")) : 1;
	}
	double t4 = timed ? _glNow() : 0;
	for (i = 0; i < repeat; ++i) {
		gl->DrawArrays(GL_TRIANGLES, 0, 3);
	}
	gl->Disable(GL_SCISSOR_TEST);
	if (timed) {
		double t5 = _glNow();
		_glTime[0] += t1 - t0;
		_glTime[1] += t2 - t1;
		_glTime[2] += t3 - t2;
		_glTime[3] += t4 - t3;
		_glTime[4] += t5 - t4;
		if (++_glDraws == 2000) {
			fprintf(stderr, "per draw: vram %.1f us, lines %.1f us, palettes+sprites %.1f us, state %.1f us, draw %.1f us\n", _glTime[0] * 500, _glTime[1] * 500, _glTime[2] * 500, _glTime[3] * 500, _glTime[4] * 500);
			memset(_glTime, 0, sizeof(_glTime));
			_glDraws = 0;
		}
	}
}

void GBAVideoBatchGLFinish(struct GBAVideoBatchRenderer* batch) {
	batch->gl->Finish();
}

void GBAVideoBatchGLStartFrame(struct GBAVideoBatchRenderer* batch) {
	UNUSED(batch);
}
