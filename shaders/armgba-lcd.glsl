// armGBA's LCD Grid for RetroArch's gl driver; armgba-lcd.slang says what it draws.

#pragma parameter GAP_SHARE "Gap Width (Share of a Pixel)" 0.15 0.0 0.5 0.01
#pragma parameter GAP_ACROSS "Gap Strength Between Columns" 0.75 0.0 1.0 0.05
#pragma parameter GAP_DOWN "Gap Strength Between Rows" 0.75 0.0 1.0 0.05
#pragma parameter GAP_RED "Gap Color Red" 0.0 0.0 255.0 1.0
#pragma parameter GAP_GREEN "Gap Color Green" 0.0 0.0 255.0 1.0
#pragma parameter GAP_BLUE "Gap Color Blue" 0.0 0.0 255.0 1.0
#pragma parameter SHADOW "Pixel Shadow" 0.0 0.0 0.5 0.01
#pragma parameter SHADOW_WAY "Pixel Shadow Way (Up, then Clockwise)" 4.0 1.0 8.0 1.0

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 TEX0;

uniform mat4 MVPMatrix;

void main() {
	gl_Position = MVPMatrix * VertexCoord;
	TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float GAP_SHARE;
uniform COMPAT_PRECISION float GAP_ACROSS;
uniform COMPAT_PRECISION float GAP_DOWN;
uniform COMPAT_PRECISION float GAP_RED;
uniform COMPAT_PRECISION float GAP_GREEN;
uniform COMPAT_PRECISION float GAP_BLUE;
uniform COMPAT_PRECISION float SHADOW;
uniform COMPAT_PRECISION float SHADOW_WAY;
#else
#define GAP_SHARE 0.15
#define GAP_ACROSS 0.75
#define GAP_DOWN 0.75
#define GAP_RED 0.0
#define GAP_GREEN 0.0
#define GAP_BLUE 0.0
#define SHADOW 0.0
#define SHADOW_WAY 4.0
#endif

// How much of an output pixel, size wide in dots, centered at p, the gap at the nearest edge covers.
float gapCoverage(float p, float size) {
	float edge = abs(p - floor(p + 0.5));
	float halfGap = GAP_SHARE * 0.5;
	return clamp((min(edge + size * 0.5, halfGap) - max(edge - size * 0.5, -halfGap)) / size, 0.0, 1.0);
}

float level(vec3 c) {
	return dot(c, vec3(2.0, 5.0, 1.0));
}

void main() {
	vec2 at = TEX0.xy * TextureSize;
	vec2 size = InputSize / OutputSize;
	vec3 color = COMPAT_TEXTURE(Texture, (floor(at) + 0.5) / TextureSize).rgb;
	vec3 gap = vec3(GAP_RED, GAP_GREEN, GAP_BLUE) / 255.0;
	float across = gapCoverage(at.x, size.x) * GAP_ACROSS;
	float down = gapCoverage(at.y, size.y) * GAP_DOWN;
	color = mix(color, gap, across + down - across * down);
	if (SHADOW > 0.0) {
		float way = (SHADOW_WAY - 1.0) * 0.785398;
		vec2 toward = vec2(sin(way), -cos(way));
		toward /= max(abs(toward.x), abs(toward.y));
		toward *= abs(toward.x) > 0.5 && abs(toward.y) > 0.5 ? 1.0 : 1.4;
		vec3 caster = COMPAT_TEXTURE(Texture, (at - toward) / TextureSize).rgb;
		float start = level(gap) * 0.9375;
		float dark = clamp((start - level(caster)) / max(start, 0.001), 0.0, 1.0);
		color *= 1.0 - SHADOW * dark;
	}
	FragColor = vec4(color, 1.0);
}
#endif
