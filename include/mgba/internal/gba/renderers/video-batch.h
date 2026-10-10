/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef VIDEO_BATCH_H
#define VIDEO_BATCH_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/renderers/video-software.h>

// Scanlines are recorded as they come and drawn together, one pixel at a time, when VRAM changes or the frame ends
#define BATCH_LINE_WORDS 128
#define BATCH_SPRITE_WORDS 8
#define BATCH_VRAM_PAGES (GBA_SIZE_VRAM >> 12)
#define BATCH_MAGIC 0x68637462
// Past this much VRAM rewriting during one frame, the rest of the frame is drawn on the CPU
#define BATCH_MAX_SPLITS 16
#define BATCH_MAX_SPLIT_PAGES 128

// What a scanline needs beyond plain tiles and sprites; the GPU draws with a shader that leaves out the rest
enum {
	BATCH_FEATURE_OBJWIN = 1,
	BATCH_FEATURE_MOSAIC = 2,
	BATCH_FEATURE_AFFINE_SPRITES = 4,
	BATCH_FEATURE_AFFINE_BG = 8,
	BATCH_FEATURE_BRIGHTNESS = 16,
	BATCH_FEATURE_BLEND = 32,
	BATCH_FEATURE_WINDOWS = 64,
	BATCH_FEATURE_ALL = 127
};

// The feature sets games turned out to need, compiled when the core loads instead of mid-game
extern const uint8_t GBAVideoBatchCommonFeatures[];
extern const size_t GBAVideoBatchCommonFeaturesSize;

struct GBAVideoBatchGL;
struct GBAVideoBatchVK;

struct GBAVideoBatchSprite {
	uint16_t entry;
	int16_t cycles;
	int16_t index;
	uint8_t features;
};

struct GBAVideoBatchRenderer {
	struct GBAVideoRenderer d;

	// Draws with Vulkan or OpenGL ES 3 when set, on the CPU otherwise
	struct GBAVideoBatchGL* gl;
	struct GBAVideoBatchVK* vk;

	// Keeps the windows, affine steps, layer enables and sprite list; draws nothing
	struct GBAVideoSoftwareRenderer sw;

	mColor* outputBuffer;
	int outputBufferStride;

	uint32_t lines[GBA_VIDEO_VERTICAL_PIXELS][BATCH_LINE_WORDS];
	uint8_t features[GBA_VIDEO_VERTICAL_PIXELS];
	uint64_t spriteCoverage[GBA_VIDEO_VERTICAL_PIXELS][2];
	struct GBAVideoBatchSprite spriteInfo[128];
	uint32_t programKey[2];
	uint32_t programs[MAX_WINDOW][5];
	int nPrograms;
	uint16_t palettes[GBA_VIDEO_VERTICAL_PIXELS][512];
	uint32_t sprites[GBA_VIDEO_VERTICAL_PIXELS][128][BATCH_SPRITE_WORDS];
	uint16_t vram[GBA_SIZE_VRAM / 2];

	int nPalettes;
	int nSprites;
	bool paletteDirty;
	bool oamDirty;
	uint32_t vramDirty;
	int drawnY;
	int nextY;
	int splits;
	int splitPages;
	int cpuFrom;

	// Optional: a full software renderer fed the same writes, compared every frame
	struct GBAVideoSoftwareRenderer* check;
	mColor* checkBuffer;
	FILE* checkLog;
	unsigned frame;
};

void GBAVideoBatchRendererCreate(struct GBAVideoBatchRenderer* renderer);
void GBAVideoBatchRendererReloadVRAM(struct GBAVideoBatchRenderer* renderer);

bool GBAVideoBatchRendererInitGL(struct GBAVideoBatchRenderer* renderer, void* (*getProc)(const char*), uintptr_t (*getFramebuffer)(void));
void GBAVideoBatchRendererDeinitGL(struct GBAVideoBatchRenderer* renderer);
void GBAVideoBatchGLUploadVRAM(struct GBAVideoBatchRenderer* renderer, uint32_t pages);
void GBAVideoBatchGLDraw(struct GBAVideoBatchRenderer* renderer, int startY, int endY);
void GBAVideoBatchGLStartFrame(struct GBAVideoBatchRenderer* renderer);

#ifdef BUILD_BATCH_VULKAN
#include <vulkan/vulkan.h>

// What the frontend lends: its device, and a way to show an image drawn by a command buffer it submits
struct GBAVideoBatchVulkanHost {
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	PFN_vkGetDeviceProcAddr getDeviceProcAddr;
	uint32_t queueFamily;
	void* context;
	uint32_t (*syncIndex)(void* context);
	uint32_t (*syncIndexMask)(void* context);
	void (*present)(void* context, VkImageView view, const VkImageViewCreateInfo* viewInfo, VkCommandBuffer commands);
};

bool GBAVideoBatchRendererInitVulkan(struct GBAVideoBatchRenderer* renderer, const struct GBAVideoBatchVulkanHost* host);
void GBAVideoBatchRendererDeinitVulkan(struct GBAVideoBatchRenderer* renderer);
void GBAVideoBatchVKUploadVRAM(struct GBAVideoBatchRenderer* renderer, uint32_t pages);
void GBAVideoBatchVKSegment(struct GBAVideoBatchRenderer* renderer, int startY, int endY);
void GBAVideoBatchVKFinishFrame(struct GBAVideoBatchRenderer* renderer);
#endif

CXX_GUARD_END

#endif
