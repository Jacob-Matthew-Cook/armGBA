/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// One pixel of the batch renderer, as C for the CPU and as GLSL ES 3.00 for the GPU.
// The includer defines BATCH_SHARED, BATCH_FN, the FEATURE_ switches, the types uint and uvec4, U(), I(), iabs(), the FLAG_ and BLEND_ constants and the FETCH_ reads.
// Inside: no preprocessor lines, no arrays, only .x .y .z .w on vectors.
BATCH_SHARED(

BATCH_FN int cdiv(int a, int b) {
	int q = iabs(a) / iabs(b);
	return (a < 0) != (b < 0) ? -q : q;
}

BATCH_FN int cmod(int a, int b) {
	return a - cdiv(a, b) * b;
}

BATCH_FN bool bit(uint value, int b) {
	return ((value >> U(b)) & 1u) != 0u;
}

BATCH_FN uint vram16(uint address) {
	return FETCH_VRAM16(address >> 1);
}

BATCH_FN uint vram8(uint address) {
	return (vram16(address) >> ((address & 1u) << 3)) & 0xFFu;
}

BATCH_FN uint color555(uint value) {
	return ((value & 0x1Fu) << 11) | ((value & 0x3E0u) << 1) | ((value & 0x7C00u) >> 10);
}

BATCH_FN uint brighten(uint color, uint y) {
	uint c = 0u;
	uint a = color & 0x1Fu;
	c |= (a + ((0x1Fu - a) * y) / 16u) & 0x1Fu;
	a = color & 0x7C0u;
	c |= (a + ((0x7C0u - a) * y) / 16u) & 0x7C0u;
	a = color & 0xF800u;
	c |= (a + ((0xF800u - a) * y) / 16u) & 0xF800u;
	return c;
}

BATCH_FN uint darken(uint color, uint y) {
	uint c = 0u;
	uint a = color & 0x1Fu;
	c |= (a - (a * y) / 16u) & 0x1Fu;
	a = color & 0x7C0u;
	c |= (a - (a * y) / 16u) & 0x7C0u;
	a = color & 0xF800u;
	c |= (a - (a * y) / 16u) & 0xF800u;
	return c;
}

BATCH_FN uint variant(uint color, int effect, uint bldy) {
	if (effect == BLEND_BRIGHTEN) {
		return brighten(color, bldy);
	}
	if (effect == BLEND_DARKEN) {
		return darken(color, bldy);
	}
	return color;
}

BATCH_FN uint mix5(uint weightA, uint colorA, uint weightB, uint colorB) {
	uint a = (colorA & 0xF81Fu) | ((colorA & 0x7C0u) << 16);
	uint b = (colorB & 0xF81Fu) | ((colorB & 0x7C0u) << 16);
	uint c = (a * weightA + b * weightB) / 16u;
	if ((c & 0x08000000u) != 0u) {
		c = (c & ~0x0FC00000u) | 0x07C00000u;
	}
	if ((c & 0x0020u) != 0u) {
		c = (c & ~0x003Fu) | 0x001Fu;
	}
	if ((c & 0x10000u) != 0u) {
		c = (c & ~0x1F800u) | 0xF800u;
	}
	return (c & 0xF81Fu) | ((c >> 16) & 0x07C0u);
}

BATCH_FN uint blendObjwin(uint blda, uint bldb, uint color, uint current) {
	if (color >= current) {
		if ((current & FLAG_TARGET_1) != 0u && (color & FLAG_TARGET_2) != 0u) {
			return mix5(blda, current, bldb, color);
		}
		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return (color & ~FLAG_TARGET_2) | (current & FLAG_OBJWIN);
}

BATCH_FN uint blendNoObjwin(uint blda, uint bldb, uint color, uint current) {
	if (color >= current) {
		if ((current & FLAG_TARGET_1) != 0u && (color & FLAG_TARGET_2) != 0u) {
			return mix5(blda, current, bldb, color);
		}
		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return color & ~FLAG_TARGET_2;
}

BATCH_FN uint noBlendObjwin(uint color, uint current) {
	if (color < current) {
		return color | (current & FLAG_OBJWIN);
	}
	return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);
}

BATCH_FN uint noBlendNoObjwin(uint color, uint current) {
	if (color >= current) {
		return current & (0x00FFFFFFu | FLAG_REBLEND | FLAG_OBJWIN);
	}
	return color;
}

BATCH_FN uint compositeSprite(uint sprite, uint row, uint control, uint objwin, bool objwinEnable, uint blda, uint bldb) {
	if (objwinEnable) {
		bool objwinDisable = !bit(objwin, 4);
		bool objwinOnly = !objwinDisable && !bit(control, 4);
		if (objwinDisable && !bit(control, 4)) {
			return row;
		}
		if (objwinDisable ? (row & FLAG_OBJWIN) == 0u : (!objwinOnly || (row & FLAG_OBJWIN) != 0u)) {
			return blendObjwin(blda, bldb, sprite, row);
		}
		return row;
	}
	if (bit(control, 4)) {
		return blendNoObjwin(blda, bldb, sprite, row);
	}
	return row;
}

// The sprite layer the software renderer would build at this pixel, in OAM order; the object window lands on row
BATCH_FN uint spritePixel(int x, int y, int start, int end, uint control, int controlPriority, int objwinPriority, uint head, uint misc, uint mosaic, uint objwin, bool target2, int spriteRow, int n, uint row) {
	uint spriteLayer = FLAG_UNWRITTEN;
	uint dispcnt = head & 0xFFFFu;
	int mode = I(dispcnt & 7u);
	int effect = I((head >> 16) & 3u);
	bool brightness = FEATURE_BRIGHTNESS && effect >= BLEND_BRIGHTEN;
	uint bldy = (misc >> 10) & 0x1Fu;
	int paletteRow = I((misc >> 16) & 0xFFu);
	bool target1Obj = bit(head, 19);
	bool objwinEnable = FEATURE_OBJWIN && bit(dispcnt, 15);
	bool winBlend = bit(control, 5);
	bool objwinBlend = bit(objwin, 5);
	bool mapping1D = bit(dispcnt, 6);
	int mosaicV = I((mosaic >> 12) & 0xFu) + 1;
	int mosaicY = y - y % mosaicV;
	uint column = 0x100u << U(x >> 5);
	int i;
	for (i = 0; i < n; ++i) {
		uvec4 list = FETCH_LIST(y, i >> 2);
		uint entry = (i & 3) == 0 ? list.x : (i & 3) == 1 ? list.y : (i & 3) == 2 ? list.z : list.w;
		if ((entry & column) == 0u) {
			continue;
		}
		uvec4 s0 = FETCH_SPRITE(spriteRow, I(entry & 0xFFu) * 2);
		uvec4 s1 = FETCH_SPRITE(spriteRow, I(entry & 0xFFu) * 2 + 1);
		uint a = s0.x & 0xFFFFu;
		uint b = s0.x >> 16;
		uint c = s0.y;
		int spriteY = I(s0.z << 16) >> 16;
		int spriteEndY = I(s0.z) >> 16;
		int localY = y;
		bool spriteMosaic = FEATURE_MOSAIC && bit(a, 12);
		if (spriteMosaic && mosaicV > 1) {
			localY = mosaicY;
			if (localY < spriteY && spriteY < 160) {
				localY = spriteY;
			}
			if (localY >= (spriteEndY & 0xFF)) {
				localY = spriteEndY - 1;
			}
		}
		int width = I(s1.y & 0xFFu);
		int height = I((s1.y >> 8) & 0xFFu);
		int objMode = I((a >> 10) & 3u);
		bool is256 = bit(a, 13);
		uint flags = ((c >> 10) & 3u) << 30;
		if ((winBlend && target1Obj && effect == BLEND_ALPHA) || objMode == 1) {
			flags |= FLAG_TARGET_1;
		}
		if (objMode == 2) {
			flags |= FLAG_OBJWIN;
			if (controlPriority < objwinPriority) {
				continue;
			}
		}
		int spriteX = I((b & 0x1FFu) << 23) >> 23;
		uint align = is256 && !mapping1D ? 1u : 0u;
		uint tile = c & 0x3FFu;
		uint charBase = (tile & ~align) * 0x20u;
		uint maskLo = mapping1D ? 0x7FFEu : 0x3FEu;
		uint maskHi = mapping1D ? 0u : charBase & 0x7C00u;
		if (mode >= 3 && tile < 512u) {
			continue;
		}
		bool objwinSlowPath = objwinEnable && objwinBlend != winBlend;
		bool spriteVariant = target1Obj && winBlend && brightness;
		if (objMode == 1 || (target1Obj && effect == BLEND_ALPHA) || objwinSlowPath) {
			if (target2) {
				flags |= FLAG_REBLEND;
				spriteVariant = false;
			} else {
				flags &= ~FLAG_TARGET_1;
			}
		}
		bool objwinVariant = spriteVariant && objwinBlend;
		uint paletteBase = 0x100u;
		if (!is256) {
			paletteBase += (c >> 12) << 4;
		}
		int inY = localY - I(a & 0xFFu);
		int stride = mapping1D ? (width >> (is256 ? 0 : 1)) : 0x80;
		int localX;
		int tileY;
		if (FEATURE_AFFINE_SPRITES && bit(a, 8)) {
			int doubleSize = I((a >> 9) & 1u);
			int totalWidth = width << doubleSize;
			int totalHeight = height << doubleSize;
			int pa = I(s0.w << 16) >> 16;
			int pb = I(s0.w) >> 16;
			int pc = I(s1.x << 16) >> 16;
			int pd = I(s1.x) >> 16;
			if (inY < 0) {
				inY += 256;
			}
			int outX = spriteX >= start ? spriteX : start;
			int condition = spriteX + totalWidth;
			int inX = outX - spriteX;
			if (end < condition) {
				condition = end;
			}
			int mosaicH = 1;
			if (spriteMosaic) {
				mosaicH = I((mosaic >> 8) & 0xFu) + 1;
				if (condition != end && cmod(condition, mosaicH) != 0) {
					condition += mosaicH - cmod(condition, mosaicH);
				}
			}
			int xAccum = pa * (inX - 1 - (totalWidth >> 1)) + pb * (inY - (totalHeight >> 1)) + (width << 7);
			int yAccum = pc * (inX - 1 - (totalWidth >> 1)) + pd * (inY - (totalHeight >> 1)) + (height << 7);
			if (pa != 0) {
				int skip = 0;
				if ((xAccum >> 8) < 0) {
					skip = cdiv(-xAccum - 1, pa);
				} else if ((xAccum >> 8) >= width) {
					skip = cdiv((width << 8) - xAccum, pa);
				}
				xAccum += pa * skip;
				yAccum += pc * skip;
				outX += skip;
			}
			if (pc != 0) {
				int skip = 0;
				if ((yAccum >> 8) < 0) {
					skip = cdiv(-yAccum - 1, pc);
				} else if ((yAccum >> 8) >= height) {
					skip = cdiv((height << 8) - yAccum, pc);
				}
				xAccum += pa * skip;
				yAccum += pc * skip;
				outX += skip;
			}
			if (outX < start || outX >= condition || x < outX || x >= condition) {
				continue;
			}
			uint widthMask = ~U(width - 1);
			uint heightMask = ~U(height - 1);
			int steps;
			if ((flags & FLAG_OBJWIN) != 0u || mosaicH <= 1) {
				// Drawing stops at the first pixel outside the sprite
				if ((U((xAccum + pa) >> 8) & widthMask) != 0u || (U((yAccum + pc) >> 8) & heightMask) != 0u) {
					continue;
				}
				steps = x - outX + 1;
			} else {
				int sampleX = x - x % mosaicH;
				steps = sampleX >= outX ? sampleX - outX + 1 : 0;
			}
			localX = (xAccum + pa * steps) >> 8;
			tileY = (yAccum + pc * steps) >> 8;
			if ((U(localX) & widthMask) != 0u || (U(tileY) & heightMask) != 0u) {
				continue;
			}
		} else {
			int outX = spriteX >= start ? spriteX : start;
			int condition = spriteX + width;
			int mosaicH = 1;
			if (spriteMosaic) {
				mosaicH = I((mosaic >> 8) & 0xFu) + 1;
				if (cmod(condition, mosaicH) != 0) {
					condition += mosaicH - cmod(condition, mosaicH);
				}
			}
			if (I(a & 0xFFu) + height - 256 >= 0) {
				inY += 256;
			}
			if (bit(b, 13)) {
				inY = height - inY - 1;
			}
			if (end < condition) {
				condition = end;
			}
			if (x < outX || x >= condition) {
				continue;
			}
			int inX = x - spriteX;
			int xOffset = 1;
			if (bit(b, 12)) {
				inX = width - inX - 1;
				xOffset = -1;
			}
			localX = inX;
			if ((flags & FLAG_OBJWIN) == 0u && mosaicH > 1) {
				localX = inX - xOffset * (x % mosaicH);
				if (localX < 0) {
					localX = 0;
				} else if (localX > width - 1) {
					localX = width - 1;
				}
			}
			tileY = inY;
		}

		uint tileData;
		if (!is256) {
			uint xBase = U((localX & ~7) * 4 + ((localX >> 1) & 2));
			uint yBase = U((tileY & ~7) * stride + (tileY & 7) * 4) + maskHi;
			tileData = vram16(0x10000u + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFEu));
			tileData = (tileData >> U((localX & 3) << 2)) & 0xFu;
		} else {
			uint xBase = U((localX & ~7) * 8 + (localX & 6));
			uint yBase = U((tileY & ~7) * stride + (tileY & 7) * 8) + maskHi;
			tileData = vram16(0x10000u + ((yBase + ((xBase + charBase) & maskLo)) & 0x7FFEu));
			tileData = (tileData >> U((localX & 1) << 3)) & 0xFFu;
		}

		uint current = spriteLayer;
		uint reordered = (current & ~(FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1)) | (flags & (FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1));
		if ((flags & FLAG_OBJWIN) != 0u) {
			if (tileData != 0u) {
				row |= FLAG_OBJWIN;
			} else if (current != FLAG_UNWRITTEN && (current & FLAG_ORDER_MASK) > flags) {
				spriteLayer = reordered;
			}
		} else if ((current & FLAG_ORDER_MASK) > flags) {
			if (tileData != 0u) {
				uint color = color555(FETCH_PAL(paletteRow, I(paletteBase + tileData)));
				if ((objwinSlowPath && (row & FLAG_OBJWIN) != 0u) ? objwinVariant : spriteVariant) {
					color = variant(color, effect, bldy);
				}
				spriteLayer = color | flags;
			} else if (current != FLAG_UNWRITTEN) {
				spriteLayer = reordered;
			}
		}
	}
	// The object window bit rides along below the order bits, which the caller splits off
	return (spriteLayer & ~FLAG_OBJWIN) | (row & FLAG_OBJWIN);
}

// One background layer composited onto row, as the software renderer draws it
BATCH_FN uint backgroundPixel(int x, uint slot, uvec4 p, uint head, uint misc, int paletteRow, uint row) {
	uint dispcnt = head & 0xFFFFu;
	int effect = I((head >> 16) & 3u);
	uint blda = misc & 0x1Fu;
	uint bldb = (misc >> 5) & 0x1Fu;
	uint bldy = (misc >> 10) & 0x1Fu;
	bool objwinEnable = FEATURE_OBJWIN && bit(dispcnt, 15);
	int index = I(slot & 3u);
	uint priority = (slot >> 2) & 3u;
	int bgMode = I((slot >> 4) & 7u);
	uint base = (priority << 30) | (U(index) << 28) | FLAG_IS_BACKGROUND;
	uint flags = base | (bit(slot, 7) ? FLAG_TARGET_1 : 0u) | (bit(slot, 8) ? FLAG_TARGET_2 : 0u);
	uint objwinFlags = base | (bit(slot, 9) ? FLAG_TARGET_1 : 0u) | (bit(slot, 10) ? FLAG_TARGET_2 : 0u);
	bool bgVariant = FEATURE_BRIGHTNESS && bit(slot, 11);
	bool objwinVariant = FEATURE_BRIGHTNESS && bit(slot, 12);
	bool objwinForceEnable = bit(slot, 13);
	bool objwinOnly = bit(slot, 14);
	uint pixelData;
	uint paletteData = 0u;

	if (!FEATURE_AFFINE_BG || bgMode == 0) {
		int inX = (x + I(p.y & 0x1FFu)) & 0x1FF;
		if (FEATURE_MOSAIC) {
			int mosaicH = I((p.y >> 12) & 0xFu) + 1;
			inX = (x - x % mosaicH + I(p.y & 0x1FFu)) & 0x1FF;
		}
		uint xBase = U(inX & 0xF8);
		if (bit(p.y, 9)) {
			xBase += U(inX & 0x100) << 5;
		}
		uint mapData = FETCH_VRAM16(p.x + (xBase >> 3));
		int localY = I((p.y >> 16) & 7u);
		if (bit(mapData, 11)) {
			localY = 7 - localY;
		}
		int column = inX & 7;
		if (bit(mapData, 10)) {
			column = 7 - column;
		}
		if (!bit(p.y, 10)) {
			uint charBase = p.z + ((mapData & 0x3FFu) << 5) + U(localY << 2);
			if (charBase >= 0x10000u) {
				return row;
			}
			pixelData = vram8(charBase + U(column >> 1));
			pixelData = (column & 1) != 0 ? pixelData >> 4 : pixelData & 0xFu;
			paletteData = (mapData >> 12) << 4;
		} else {
			uint charBase = p.z + ((mapData & 0x3FFu) << 6) + U(localY << 3);
			if (charBase >= 0x10000u) {
				return row;
			}
			pixelData = vram8(charBase + U(column));
		}
	} else {
		int sampleX = x;
		if (FEATURE_MOSAIC) {
			int mosaicH = I((p.w >> 12) & 0xFu) + 1;
			sampleX = x - x % mosaicH;
		}
		int localX = I(p.x) + sampleX * (I(p.z << 16) >> 16);
		int localY = I(p.y) + sampleX * (I(p.z) >> 16);
		if (bgMode == 2) {
			int size = I((p.w >> 7) & 3u);
			int sizeAdjusted = 0x8000 << size;
			if (bit(p.w, 9)) {
				localX &= sizeAdjusted - 1;
				localY &= sizeAdjusted - 1;
			} else if (((localX | localY) & ~(sizeAdjusted - 1)) != 0) {
				return row;
			}
			uint mapData = vram8(((p.w & 0x1Fu) << 11) + U(localX >> 11) + (U((localY >> 7) & 0x7F0) << U(size)));
			pixelData = vram8((((p.w >> 5) & 3u) << 14) + (mapData << 6) + U((localY & 0x700) >> 5) + U((localX & 0x700) >> 8));
		} else {
			int width = bgMode == 5 ? 160 : 240;
			int height = bgMode == 5 ? 128 : 160;
			if (localX < 0 || localY < 0 || (localX >> 8) >= width || (localY >> 8) >= height) {
				return row;
			}
			uint offset = bit(p.w, 16) ? 0xA000u : 0u;
			if (bgMode == 4) {
				pixelData = vram8(offset + U((localX >> 8) + (localY >> 8) * 240));
				if (pixelData == 0u || (row & 0xFE000000u) == 0u) {
					return row;
				}
				uint color = color555(FETCH_PAL(paletteRow, I(pixelData)));
				if (!objwinEnable) {
					if (bgVariant) {
						color = variant(color, effect, bldy);
					}
					return blendNoObjwin(blda, bldb, color | flags, row);
				}
				if (objwinForceEnable || ((row & FLAG_OBJWIN) == 0u) == objwinOnly) {
					if ((row & FLAG_OBJWIN) != 0u ? objwinVariant : bgVariant) {
						color = variant(color, effect, bldy);
					}
					return blendObjwin(blda, bldb, color | ((row & FLAG_OBJWIN) != 0u ? objwinFlags : flags), row);
				}
				return row;
			}
			// Direct color: no transparency, always composited
			uint color;
			if (bgMode == 3) {
				color = color555(vram16(U(((localX >> 8) + (localY >> 8) * 240) << 1)));
			} else {
				color = color555(vram16(offset + U((localX >> 8) * 2 + (localY >> 8) * 320)));
			}
			if (!objwinEnable || ((row & FLAG_OBJWIN) == 0u) != objwinOnly) {
				uint mergedFlags = (row & FLAG_OBJWIN) != 0u ? objwinFlags : flags;
				if (bgVariant) {
					color = variant(color, effect, bldy);
				}
				return blendObjwin(blda, bldb, color | mergedFlags, row);
			}
			return row;
		}
	}

	if (pixelData == 0u || (row & 0xFE000000u) == 0u) {
		return row;
	}
	uint color = color555(FETCH_PAL(paletteRow, I(paletteData | pixelData)));
	bool reblend = (row & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND;
	if (!objwinEnable) {
		if (bgVariant && !reblend) {
			color = variant(color, effect, bldy);
		}
		if ((flags & FLAG_TARGET_2) != 0u) {
			return blendNoObjwin(blda, bldb, color | flags, row);
		}
		return noBlendNoObjwin(color | flags, row);
	}
	if (objwinForceEnable || ((row & FLAG_OBJWIN) == 0u) == objwinOnly) {
		uint mergedFlags = flags;
		if ((row & FLAG_OBJWIN) != 0u) {
			mergedFlags = objwinFlags;
			if (objwinVariant) {
				color = variant(color, effect, bldy);
			}
		} else if (bgVariant && !reblend) {
			color = variant(color, effect, bldy);
		}
		if ((flags & FLAG_TARGET_2) != 0u) {
			return blendObjwin(blda, bldb, color | mergedFlags, row);
		}
		return noBlendObjwin(color | mergedFlags, row);
	}
	return row;
}

BATCH_FN uint batchPixel(int x, int y) {
	uvec4 header = FETCH_LINE(y, 0);
	uint head = header.y;
	if (bit(head, 23)) {
		return 0xFFDFu;
	}
	uint dispcnt = head & 0xFFFFu;
	int effect = I((head >> 16) & 3u);
	bool brightness = FEATURE_BRIGHTNESS && effect >= BLEND_BRIGHTEN;
	uint misc = header.z;
	uint blda = misc & 0x1Fu;
	uint bldb = (misc >> 5) & 0x1Fu;
	uint bldy = (misc >> 10) & 0x1Fu;
	int paletteRow = I((misc >> 16) & 0xFFu);
	uint mosaic = (header.w >> 8) & 0xFFFFu;
	uint objwin = header.w >> 24;
	bool objwinEnable = FEATURE_OBJWIN && bit(dispcnt, 15);
	bool objwinBlend = bit(objwin, 5);

	int segment = 0;
	int start = 0;
	int end = 240;
	if (FEATURE_WINDOWS) {
		uint ends = header.x;
		segment = (x >= I(ends & 0xFFu) ? 1 : 0) + (x >= I((ends >> 8) & 0xFFu) ? 1 : 0) + (x >= I((ends >> 16) & 0xFFu) ? 1 : 0) + (x >= I(ends >> 24) ? 1 : 0);
		start = segment == 0 ? 0 : I((ends >> U(8 * segment - 8)) & 0xFFu);
		end = segment == 4 ? 255 : I((ends >> U(8 * segment)) & 0xFFu);
		if (end == 255) {
			end = 240;
		}
	}
	uvec4 program = FETCH_LINE(y, 1 + 2 * segment);
	uvec4 programMore = FETCH_LINE(y, 2 + 2 * segment);
	uint control = program.x & 0xFFu;
	bool winBlend = bit(control, 5);

	uint backdrop = color555(FETCH_PAL(paletteRow, 0));
	uint row = backdrop;
	if (bit(head, 20) && brightness && winBlend) {
		row = variant(row, effect, bldy);
	}
	row |= FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND;

	uint sprite = FLAG_UNWRITTEN;
	int n = I(header.w & 0xFFu);
	if (n > 0 && (bit(control, 4) || objwinEnable)) {
		sprite = spritePixel(x, y, start, end, control, I(program.x << 16) >> 24, I(program.x << 8) >> 24, head, misc, mosaic, objwin, bit(head, 24), I(misc >> 24), n, row);
		row |= sprite & FLAG_OBJWIN;
		sprite &= ~FLAG_OBJWIN;
	}
	bool spritePending = (sprite & FLAG_UNWRITTEN) != FLAG_UNWRITTEN;
	uint spritePriority = sprite >> 30;
	if (bit(head, 21)) {
		sprite |= FLAG_TARGET_2;
	}

	int slots = I(program.x >> 24);
	int k;
	for (k = 0; k < slots; ++k) {
		uint slot = k == 0 ? program.y : k == 1 ? program.z : k == 2 ? program.w : programMore.x;
		if (spritePending && ((slot >> 2) & 3u) >= spritePriority) {
			row = compositeSprite(sprite, row, control, objwin, objwinEnable, blda, bldb);
			spritePending = false;
		}
		// Done once two layers met, or once one did where nothing blends
		if ((row & 0xFA000000u) == 0u || (!FEATURE_BLEND && (row & FLAG_UNWRITTEN) != FLAG_UNWRITTEN)) {
			break;
		}
		row = backgroundPixel(x, slot, FETCH_LINE(y, 11 + I(slot & 3u)), head, misc, paletteRow, row);
	}
	if (spritePending && (FEATURE_BLEND || (row & FLAG_UNWRITTEN) == FLAG_UNWRITTEN)) {
		row = compositeSprite(sprite, row, control, objwin, objwinEnable, blda, bldb);
	}

	if (FEATURE_BLEND && (bit(head, 18) || bit(head, 25)) && bit(head, 22)) {
		if ((row & FLAG_TARGET_1) != 0u) {
			uint color = backdrop;
			if (bit(head, 20) && brightness && winBlend) {
				color = variant(color, effect, bldy);
			}
			row = mix5(bldb, color, blda, row);
		}
	}
	if (FEATURE_BLEND && bit(head, 18) && brightness) {
		uint mask = FLAG_REBLEND | FLAG_IS_BACKGROUND;
		uint match = FLAG_REBLEND;
		bool apply = true;
		if (objwinEnable && objwinBlend != winBlend) {
			mask |= FLAG_OBJWIN;
			if (objwinBlend) {
				match |= FLAG_OBJWIN;
			}
		} else if (!winBlend) {
			apply = false;
		}
		if (apply && (row & mask) == match) {
			row = variant(row, effect, bldy);
		}
	}
	return row & 0xFFFFu;
}

)
