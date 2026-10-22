#version 450

// Copyright (c) 2026 Magnominus
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// armGBA's LCD Grid for RetroArch, as the standalone frontend draws it: the gaps between the
// console screen's pixels, and the shadow the first Game Boy's and the Pocket's pixels cast on the
// reflector behind them. A gap is centered on the edge between two pixels; an output pixel takes the
// gap's color by how much of it the gap covers, across (between columns) and down (between rows). The
// first Game Boy's and the Pocket's show the screen's color while off there, the Color's and
// Advance's black. The shadow darkens the screen 1.4 dots from each pixel the chosen way, softly, by
// up to Shadow at a pixel as dark as the screen goes. The presets beside this hold each screen's
// values, measured from photos of the consoles (see libretro/lcd.h).

layout(push_constant) uniform Push {
	vec4 SourceSize;
	vec4 OutputSize;
	float GAP_SHARE;
	float GAP_ACROSS;
	float GAP_DOWN;
	float GAP_RED;
	float GAP_GREEN;
	float GAP_BLUE;
	float SHADOW;
	float SHADOW_WAY;
} params;

#pragma parameter GAP_SHARE "Gap Width (Share of a Pixel)" 0.15 0.0 0.5 0.01
#pragma parameter GAP_ACROSS "Gap Strength Between Columns" 0.75 0.0 1.0 0.05
#pragma parameter GAP_DOWN "Gap Strength Between Rows" 0.75 0.0 1.0 0.05
#pragma parameter GAP_RED "Gap Color Red" 0.0 0.0 255.0 1.0
#pragma parameter GAP_GREEN "Gap Color Green" 0.0 0.0 255.0 1.0
#pragma parameter GAP_BLUE "Gap Color Blue" 0.0 0.0 255.0 1.0
#pragma parameter SHADOW "Pixel Shadow" 0.0 0.0 0.5 0.01
#pragma parameter SHADOW_WAY "Pixel Shadow Way (Up, then Clockwise)" 4.0 1.0 8.0 1.0

layout(std140, set = 0, binding = 0) uniform UBO {
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

// How much of an output pixel, size wide in dots, centered at p, the gap at the nearest edge covers.
float gapCoverage(float p, float size) {
	float edge = abs(p - floor(p + 0.5));
	float halfGap = params.GAP_SHARE * 0.5;
	return clamp((min(edge + size * 0.5, halfGap) - max(edge - size * 0.5, -halfGap)) / size, 0.0, 1.0);
}

float level(vec3 c) {
	return dot(c, vec3(2.0, 5.0, 1.0));
}

void main() {
	vec2 at = vTexCoord * params.SourceSize.xy;
	vec2 size = params.SourceSize.xy * params.OutputSize.zw;
	vec3 color = texture(Source, (floor(at) + 0.5) * params.SourceSize.zw).rgb;
	vec3 gap = vec3(params.GAP_RED, params.GAP_GREEN, params.GAP_BLUE) / 255.0;
	float across = gapCoverage(at.x, size.x) * params.GAP_ACROSS;
	float down = gapCoverage(at.y, size.y) * params.GAP_DOWN;
	color = mix(color, gap, across + down - across * down);
	if (params.SHADOW > 0.0) {
		float way = (params.SHADOW_WAY - 1.0) * 0.785398;
		vec2 toward = vec2(sin(way), -cos(way));
		toward /= max(abs(toward.x), abs(toward.y));
		toward *= abs(toward.x) > 0.5 && abs(toward.y) > 0.5 ? 1.0 : 1.4;
		vec3 caster = texture(Source, (at - toward) * params.SourceSize.zw).rgb;
		float start = level(gap) * 0.9375;
		float dark = clamp((start - level(caster)) / max(start, 0.001), 0.0, 1.0);
		color *= 1.0 - params.SHADOW * dark;
	}
	FragColor = vec4(color, 1.0);
}
