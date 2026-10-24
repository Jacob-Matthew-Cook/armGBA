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
// Color's and Advance's gaps are black. The Game Boy screens' colors are each pixel seen with the
// gaps around it, so Keep the Screen's Brightness sets each pixel, and beside the darkest ones the
// gaps, until each square averages its color, in linear light (libretro/lcd.c's libretroLcdKeep has
// the gaps' colors); on the Color and the Advance the gaps lighten beside their brightest colors. The shadow
// darkens the screen 1.4 dots from each pixel the chosen way, softly, by up to Shadow at a pixel as
// dark as the screen goes, counting from the unlit screen's color. The presets beside this hold each
// screen's values, measured from photos of the consoles (see libretro/lcd.h).

layout(push_constant) uniform Push {
	vec4 SourceSize;
	vec4 OutputSize;
	float GAP_SHARE;
	float GAP_ACROSS;
	float GAP_DOWN;
	float GAP_RED;
	float GAP_GREEN;
	float GAP_BLUE;
	float KEEP_BRIGHTNESS;
	float UNLIT_RED;
	float UNLIT_GREEN;
	float UNLIT_BLUE;
	float SHADOW;
	float SHADOW_WAY;
} params;

#pragma parameter GAP_SHARE "Gap Width (Share of a Pixel)" 0.15 0.0 0.5 0.01
#pragma parameter GAP_ACROSS "Gap Strength Between Columns" 0.75 0.0 1.0 0.05
#pragma parameter GAP_DOWN "Gap Strength Between Rows" 0.75 0.0 1.0 0.05
#pragma parameter GAP_RED "Gap Color Red" 0.0 0.0 255.0 1.0
#pragma parameter GAP_GREEN "Gap Color Green" 0.0 0.0 255.0 1.0
#pragma parameter GAP_BLUE "Gap Color Blue" 0.0 0.0 255.0 1.0
#pragma parameter KEEP_BRIGHTNESS "Keep the Screen's Brightness" 0.0 0.0 1.0 1.0
#pragma parameter UNLIT_RED "Unlit Screen Red" 0.0 0.0 255.0 1.0
#pragma parameter UNLIT_GREEN "Unlit Screen Green" 0.0 0.0 255.0 1.0
#pragma parameter UNLIT_BLUE "Unlit Screen Blue" 0.0 0.0 255.0 1.0
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

// The share of a tent two output pixels wide, centered on 0, that lies below u output pixels.
float tentBelow(float u) {
	u = clamp(u, -1.0, 1.0);
	return u < 0.0 ? (u + 1.0) * (u + 1.0) * 0.5 : 1.0 - (1.0 - u) * (1.0 - u) * 0.5;
}

// How much of the gaps at the edges either side of p an output pixel, size wide in dots, centered at
// p, sees through a tent two output pixels wide: each gap spreads over the same two pixels wherever it
// lands, so lines under a pixel wide stay even instead of some looking darker.
float gapCoverage(float p, float size) {
	float halfGap = params.GAP_SHARE * 0.5;
	float below = floor(p) - p;
	float above = below + 1.0;
	return min(tentBelow((below + halfGap) / size) - tentBelow((below - halfGap) / size) +
	           tentBelow((above + halfGap) / size) - tentBelow((above - halfGap) / size), 1.0);
}

// How strongly the grid shows, a dot being pitch output pixels wide, as lcd.h's libretroLcdFade: a
// tenth of the photos' darkness for each, all of it from 10 on. A gap under a pixel wide lands
// unevenly; fainter, it reads as the screen's texture rather than as lines.
float gridFade(float pitch) {
	return clamp(pitch * 0.1, 0.0, 1.0);
}

// sRGB to linear light, and back.
vec3 toLinear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 fromLinear(vec3 l) {
	return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));
}

float level(vec3 c) {
	return dot(c, vec3(2.0, 5.0, 1.0));
}

void main() {
	vec2 at = vTexCoord * params.SourceSize.xy;
	vec2 size = params.SourceSize.xy * params.OutputSize.zw;
	vec3 color = texture(Source, (floor(at) + 0.5) * params.SourceSize.zw).rgb;
	vec3 gap = vec3(params.GAP_RED, params.GAP_GREEN, params.GAP_BLUE) / 255.0;
	float fade = gridFade(1.0 / max(size.x, size.y));
	float across = gapCoverage(at.x, size.x) * params.GAP_ACROSS * fade;
	float down = gapCoverage(at.y, size.y) * params.GAP_DOWN * fade;
	float covered = across + down - across * down;
	vec3 unlit = vec3(params.UNLIT_RED, params.UNLIT_GREEN, params.UNLIT_BLUE) / 255.0;
	if (params.KEEP_BRIGHTNESS > 0.5) {
		// The pixel set, in linear light, so its square with the gaps around it averages the color it
		// had: the Game Boy screens' colors are measured that way. Beside a pixel too dark for the gap,
		// the gaps darken; on a black matrix, beside one too bright, they lighten.
		float acrossAll = params.GAP_SHARE * params.GAP_ACROSS * fade, downAll = params.GAP_SHARE * params.GAP_DOWN * fade;
		float shown = max(acrossAll + downAll - acrossAll * downAll, 0.0001);
		vec3 want = toLinear(color);
		vec3 lit = toLinear(gap);
		vec3 inside = (want - shown * lit) / (1.0 - shown);
		lit = mix(lit, want / shown, step(inside, vec3(0.0)));
		if (level(unlit) == 0.0) {
			lit = mix(lit, (want - (1.0 - shown)) / shown, step(vec3(1.0), inside));
		}
		color = fromLinear(mix(clamp(inside, 0.0, 1.0), clamp(lit, 0.0, 1.0), covered));
	} else {
		color = mix(color, gap, covered);
	}
	if (params.SHADOW > 0.0) {
		float way = (params.SHADOW_WAY - 1.0) * 0.785398;
		vec2 toward = vec2(sin(way), -cos(way));
		toward /= max(abs(toward.x), abs(toward.y));
		toward *= abs(toward.x) > 0.5 && abs(toward.y) > 0.5 ? 1.0 : 1.4;
		vec3 caster = texture(Source, (at - toward) * params.SourceSize.zw).rgb;
		float start = level(unlit) * 0.9375;
		float dark = clamp((start - level(caster)) / max(start, 0.001), 0.0, 1.0);
		color *= 1.0 - params.SHADOW * dark;
	}
	FragColor = vec4(color, 1.0);
}
