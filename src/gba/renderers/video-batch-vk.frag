/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* The batch renderer's pixel function for Vulkan. The build runs this through the C preprocessor,
 * puts "#version 450" in front and compiles it to SPIR-V. */

#define BATCH_SHARED(...) __VA_ARGS__
#define BATCH_FN
#define U(X) uint(X)
#define I(X) int(X)
#define iabs(X) abs(X)
#define FLAG_PRIORITY 0xC0000000u
#define FLAG_IS_BACKGROUND 0x08000000u
#define FLAG_UNWRITTEN 0xFC000000u
#define FLAG_REBLEND 0x04000000u
#define FLAG_TARGET_1 0x02000000u
#define FLAG_TARGET_2 0x01000000u
#define FLAG_OBJWIN 0x01000000u
#define FLAG_ORDER_MASK 0xF8000000u
#define BLEND_ALPHA 1
#define BLEND_BRIGHTEN 2
#define BLEND_DARKEN 3

/* Where each part of a frame sits in the frame buffer, in words; matches video-batch-vk.c */
#define LINE_WORDS 128
#define PALETTE_BASE 20480
#define SPRITE_BASE 61440

#define FETCH_LINE(Y, T) fetch4((Y) * LINE_WORDS + (T) * 4)
#define FETCH_SPRITE(R, T) fetch4(SPRITE_BASE + (R) * 1024 + (T) * 4)
#define FETCH_PAL(R, E) fetchPalette(R, E)
#define FETCH_VRAM16(H) fetchVram16(H)

layout(constant_id = 0) const bool FEATURE_OBJWIN = true;
layout(constant_id = 1) const bool FEATURE_MOSAIC = true;
layout(constant_id = 2) const bool FEATURE_AFFINE_SPRITES = true;
layout(constant_id = 3) const bool FEATURE_AFFINE_BG = true;
layout(constant_id = 4) const bool FEATURE_BRIGHTNESS = true;
layout(constant_id = 5) const bool FEATURE_BLEND = true;
layout(constant_id = 6) const bool FEATURE_WINDOWS = true;

layout(std430, set = 0, binding = 0) readonly buffer Frame {
	uint frameWords[];
};

layout(std430, set = 0, binding = 1) readonly buffer Vram {
	uint vramWords[];
};

/* The VRAM pool slot holding each 4 KiB page, two to a word */
layout(push_constant) uniform Segment {
	uint pages[12];
} segment;

layout(location = 0) out vec4 outColor;

uvec4 fetch4(int base) {
	return uvec4(frameWords[base], frameWords[base + 1], frameWords[base + 2], frameWords[base + 3]);
}

uint fetchPalette(int row, int entry) {
	return (frameWords[PALETTE_BASE + row * 256 + (entry >> 1)] >> ((entry & 1) * 16)) & 0xFFFFu;
}

uint fetchVram16(uint halfword) {
	uint index = min(halfword, 0xBFFFu);
	uint page = index >> 11;
	uint slot = (segment.pages[page >> 1] >> ((page & 1u) * 16u)) & 0xFFFFu;
	return (vramWords[slot * 1024u + ((index & 2047u) >> 1)] >> ((index & 1u) * 16u)) & 0xFFFFu;
}

#include "gba/renderers/video-batch-pixel.h"

void main() {
	ivec2 position = ivec2(gl_FragCoord.xy);
	uint color = batchPixel(position.x, position.y);
	uvec3 rgb = uvec3(color >> 11, (color >> 6) & 0x1Fu, color & 0x1Fu);
	outColor = vec4(vec3((rgb << 3) | (rgb >> 2)) / 255.0, 1.0);
}
