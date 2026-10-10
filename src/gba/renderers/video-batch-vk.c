/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/renderers/video-batch.h>

#ifdef BUILD_BATCH_VULKAN
#include <mgba/core/log.h>
#include <mgba-util/threading.h>

#include <time.h>
#ifdef __linux__
#include <unistd.h>
#endif

#include "video-batch-copy.h"
#include "video-batch-frag.h"
#include "video-batch-vert.h"

// The frame buffer, in words: lines, then palette rows two entries to a word, then sprite tables, then lines drawn on the CPU;
// matches video-batch-vk.frag and the copy shader the build writes
#define FRAME_PALETTES 20480
#define FRAME_SPRITES 61440
#define FRAME_CPU 225280
#define FRAME_WORDS (FRAME_CPU + GBA_VIDEO_VERTICAL_PIXELS * GBA_VIDEO_HORIZONTAL_PIXELS / 2)

// VRAM lives in 4 KiB pages copied into slots; a frame's segments each see the slots that held VRAM when they were recorded
#define VRAM_SLOTS 1024
#define MAX_FRAMES 4

#define BATCH_VK_FUNCTIONS(X) \
	X(AllocateCommandBuffers) \
	X(AllocateDescriptorSets) \
	X(AllocateMemory) \
	X(BeginCommandBuffer) \
	X(BindBufferMemory) \
	X(BindImageMemory) \
	X(CmdBeginRenderPass) \
	X(CmdBindDescriptorSets) \
	X(CmdBindPipeline) \
	X(CmdDraw) \
	X(CmdEndRenderPass) \
	X(CmdPushConstants) \
	X(CmdSetScissor) \
	X(CmdSetViewport) \
	X(CreateBuffer) \
	X(CreateCommandPool) \
	X(CreateDescriptorPool) \
	X(CreateDescriptorSetLayout) \
	X(CreateFramebuffer) \
	X(CreateGraphicsPipelines) \
	X(CreateImage) \
	X(CreateImageView) \
	X(CreatePipelineLayout) \
	X(CreateRenderPass) \
	X(CreateShaderModule) \
	X(DestroyBuffer) \
	X(DestroyCommandPool) \
	X(DestroyDescriptorPool) \
	X(DestroyDescriptorSetLayout) \
	X(DestroyFramebuffer) \
	X(DestroyImage) \
	X(DestroyImageView) \
	X(DestroyPipeline) \
	X(DestroyPipelineLayout) \
	X(DestroyRenderPass) \
	X(DestroyShaderModule) \
	X(DeviceWaitIdle) \
	X(EndCommandBuffer) \
	X(FreeMemory) \
	X(GetBufferMemoryRequirements) \
	X(GetImageMemoryRequirements) \
	X(MapMemory) \
	X(ResetCommandBuffer) \
	X(UpdateDescriptorSets)

#define BATCH_VK_DECLARE(NAME) PFN_vk ## NAME NAME;
#define BATCH_VK_LOAD(NAME) \
	vk->NAME = (PFN_vk ## NAME) host->getDeviceProcAddr(host->device, "vk" #NAME); \
	if (!vk->NAME) { \
		mLOG(GBA_VIDEO, ERROR, "Batch renderer: missing vk" #NAME); \
		free(vk); \
		return false; \
	}

struct GBAVideoBatchSegment {
	int startY;
	int endY;
	unsigned features;
	uint32_t pages[12];
};

struct GBAVideoBatchVKFrame {
	VkImage image;
	VkDeviceMemory imageMemory;
	VkImageView view;
	VkImageViewCreateInfo viewInfo;
	VkFramebuffer framebuffer;
	VkBuffer data;
	VkDeviceMemory dataMemory;
	uint32_t* words;
	VkDescriptorSet set;
	VkCommandBuffer commands;
};

struct GBAVideoBatchVK {
	BATCH_VK_FUNCTIONS(BATCH_VK_DECLARE)
	struct GBAVideoBatchVulkanHost host;
	VkPhysicalDeviceMemoryProperties memory;
	VkRenderPass renderPass;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout layout;
	VkDescriptorPool descriptors;
	VkCommandPool commandPool;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkShaderModule copyFragment;
	VkPipeline pipelines[BATCH_FEATURE_ALL + 1];
	VkPipeline copy;
#ifndef DISABLE_THREADING
	Thread compiler;
	bool compiling;
	bool stopCompiling;
#endif
	struct GBAVideoBatchVKFrame frames[MAX_FRAMES];
	int nFrames;
	VkBuffer vram;
	VkDeviceMemory vramMemory;
	uint32_t* vramWords;
	uint16_t pageSlot[BATCH_VRAM_PAGES];
	uint64_t slotFrame[VRAM_SLOTS];
	unsigned nextSlot;
	uint64_t frame;
	struct GBAVideoBatchSegment segments[GBA_VIDEO_VERTICAL_PIXELS];
	int nSegments;
};

static bool _memoryType(struct GBAVideoBatchVK* vk, uint32_t types, VkMemoryPropertyFlags flags, uint32_t* type) {
	uint32_t i;
	for (i = 0; i < vk->memory.memoryTypeCount; ++i) {
		if ((types & (1U << i)) && (vk->memory.memoryTypes[i].propertyFlags & flags) == flags) {
			*type = i;
			return true;
		}
	}
	return false;
}

static bool _buffer(struct GBAVideoBatchVK* vk, VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
	VkDevice device = vk->host.device;
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->CreateBuffer(device, &info, NULL, buffer) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements requirements;
	vk->GetBufferMemoryRequirements(device, *buffer, &requirements);
	VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	if (!_memoryType(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &allocate.memoryTypeIndex) ||
	    vk->AllocateMemory(device, &allocate, NULL, memory) != VK_SUCCESS) {
		return false;
	}
	return vk->BindBufferMemory(device, *buffer, *memory, 0) == VK_SUCCESS &&
	       vk->MapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
}

static bool _image(struct GBAVideoBatchVK* vk, struct GBAVideoBatchVKFrame* frame) {
	VkDevice device = vk->host.device;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vk->CreateImage(device, &info, NULL, &frame->image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements requirements;
	vk->GetImageMemoryRequirements(device, frame->image, &requirements);
	VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	if (!_memoryType(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocate.memoryTypeIndex) &&
	    !_memoryType(vk, requirements.memoryTypeBits, 0, &allocate.memoryTypeIndex)) {
		return false;
	}
	if (vk->AllocateMemory(device, &allocate, NULL, &frame->imageMemory) != VK_SUCCESS ||
	    vk->BindImageMemory(device, frame->image, frame->imageMemory, 0) != VK_SUCCESS) {
		return false;
	}
	frame->viewInfo = (VkImageViewCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = frame->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A },
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (vk->CreateImageView(device, &frame->viewInfo, NULL, &frame->view) != VK_SUCCESS) {
		return false;
	}
	VkFramebufferCreateInfo framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = vk->renderPass,
		.attachmentCount = 1,
		.pAttachments = &frame->view,
		.width = GBA_VIDEO_HORIZONTAL_PIXELS,
		.height = GBA_VIDEO_VERTICAL_PIXELS,
		.layers = 1,
	};
	return vk->CreateFramebuffer(device, &framebuffer, NULL, &frame->framebuffer) == VK_SUCCESS;
}

static bool _setup(struct GBAVideoBatchVK* vk) {
	VkDevice device = vk->host.device;
	VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	VkSubpassDependency dependencies[2] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		},
	};
	VkRenderPassCreateInfo renderPass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies,
	};
	if (vk->CreateRenderPass(device, &renderPass, NULL, &vk->renderPass) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo setLayout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings,
	};
	if (vk->CreateDescriptorSetLayout(device, &setLayout, NULL, &vk->setLayout) != VK_SUCCESS) {
		return false;
	}
	VkPushConstantRange pages = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(((struct GBAVideoBatchSegment*) 0)->pages) };
	VkPipelineLayoutCreateInfo layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &vk->setLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pages,
	};
	if (vk->CreatePipelineLayout(device, &layout, NULL, &vk->layout) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES };
	VkDescriptorPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = MAX_FRAMES,
		.poolSizeCount = 1,
		.pPoolSizes = &poolSize,
	};
	if (vk->CreateDescriptorPool(device, &pool, NULL, &vk->descriptors) != VK_SUCCESS) {
		return false;
	}
	VkCommandPoolCreateInfo commandPool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk->host.queueFamily,
	};
	if (vk->CreateCommandPool(device, &commandPool, NULL, &vk->commandPool) != VK_SUCCESS) {
		return false;
	}
	VkShaderModuleCreateInfo vertex = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(_batchVertexSpirv),
		.pCode = _batchVertexSpirv,
	};
	VkShaderModuleCreateInfo fragment = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(_batchFragmentSpirv),
		.pCode = _batchFragmentSpirv,
	};
	VkShaderModuleCreateInfo copy = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(_batchCopySpirv),
		.pCode = _batchCopySpirv,
	};
	if (vk->CreateShaderModule(device, &vertex, NULL, &vk->vertex) != VK_SUCCESS ||
	    vk->CreateShaderModule(device, &fragment, NULL, &vk->fragment) != VK_SUCCESS ||
	    vk->CreateShaderModule(device, &copy, NULL, &vk->copyFragment) != VK_SUCCESS) {
		return false;
	}

	if (!_buffer(vk, VRAM_SLOTS * 4096, &vk->vram, &vk->vramMemory, (void**) &vk->vramWords)) {
		return false;
	}
	int i;
	for (i = 0; i < vk->nFrames; ++i) {
		struct GBAVideoBatchVKFrame* frame = &vk->frames[i];
		if (!_image(vk, frame) || !_buffer(vk, FRAME_WORDS * 4, &frame->data, &frame->dataMemory, (void**) &frame->words)) {
			return false;
		}
		VkDescriptorSetAllocateInfo set = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk->descriptors,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk->setLayout,
		};
		if (vk->AllocateDescriptorSets(device, &set, &frame->set) != VK_SUCCESS) {
			return false;
		}
		VkDescriptorBufferInfo buffers[2] = {
			{ frame->data, 0, VK_WHOLE_SIZE },
			{ vk->vram, 0, VK_WHOLE_SIZE },
		};
		VkWriteDescriptorSet writes[2] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame->set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[0] },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame->set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[1] },
		};
		vk->UpdateDescriptorSets(device, 2, writes, 0, NULL);
		VkCommandBufferAllocateInfo commands = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk->commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		if (vk->AllocateCommandBuffers(device, &commands, &frame->commands) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

static VkPipeline _createPipeline(struct GBAVideoBatchVK* vk, VkShaderModule fragment, unsigned features);

static void _compile(struct GBAVideoBatchVK* vk, unsigned features) {
	if (__atomic_load_n(&vk->pipelines[features], __ATOMIC_ACQUIRE)) {
		return;
	}
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	VkPipeline pipeline = _createPipeline(vk, vk->fragment, features);
	clock_gettime(CLOCK_MONOTONIC, &end);
	if (getenv("ARMGBA_BATCH_TIME")) {
		fprintf(stderr, "pipeline %02X: %.1f ms\n", features, (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) * 1e-6);
	}
	__atomic_store_n(&vk->pipelines[features], pipeline, __ATOMIC_RELEASE);
}

// The pipeline for one set of features, or the one that does everything until that is built
static VkPipeline _pipeline(struct GBAVideoBatchVK* vk, unsigned features) {
	VkPipeline pipeline = __atomic_load_n(&vk->pipelines[features], __ATOMIC_ACQUIRE);
	return pipeline ? pipeline : vk->pipelines[BATCH_FEATURE_ALL];
}

// Builds every pipeline while the game runs: the one that does everything, the ones games use most, then the rest
static void _compileAll(struct GBAVideoBatchVK* vk) {
	_compile(vk, BATCH_FEATURE_ALL);
	size_t i;
	for (i = 0; i < GBAVideoBatchCommonFeaturesSize; ++i) {
#ifndef DISABLE_THREADING
		if (__atomic_load_n(&vk->stopCompiling, __ATOMIC_ACQUIRE)) {
			return;
		}
#endif
		_compile(vk, GBAVideoBatchCommonFeatures[i]);
	}
	unsigned features;
	for (features = 0; features < BATCH_FEATURE_ALL; ++features) {
#ifndef DISABLE_THREADING
		if (__atomic_load_n(&vk->stopCompiling, __ATOMIC_ACQUIRE)) {
			return;
		}
#endif
		_compile(vk, features);
	}
}

#ifndef DISABLE_THREADING
static THREAD_ENTRY _compilerThread(void* context) {
	ThreadSetName("Batch shaders");
#ifdef __linux__
	// Below the emulator's own threads; the result is only a hint
	int ignored = nice(10);
	UNUSED(ignored);
#endif
	_compileAll(context);
	THREAD_EXIT(0);
}
#endif

static VkPipeline _createPipeline(struct GBAVideoBatchVK* vk, VkShaderModule fragment, unsigned features) {
	VkBool32 values[7];
	VkSpecializationMapEntry entries[7];
	unsigned i;
	for (i = 0; i < 7; ++i) {
		values[i] = !!(features & (1 << i));
		entries[i] = (VkSpecializationMapEntry) { i, i * sizeof(VkBool32), sizeof(VkBool32) };
	}
	VkSpecializationInfo specialization = { 7, entries, sizeof(values), values };
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vk->vertex, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main", .pSpecializationInfo = &specialization },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState attachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
	};
	VkDynamicState dynamic[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic,
	};
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamicState,
		.layout = vk->layout,
		.renderPass = vk->renderPass,
	};
	VkPipeline pipeline;
	if (vk->CreateGraphicsPipelines(vk->host.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) {
		mLOG(GBA_VIDEO, ERROR, "Batch renderer: pipeline %02X failed", features);
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

bool GBAVideoBatchRendererInitVulkan(struct GBAVideoBatchRenderer* batch, const struct GBAVideoBatchVulkanHost* host) {
	struct GBAVideoBatchVK* vk = calloc(1, sizeof(*vk));
	BATCH_VK_FUNCTIONS(BATCH_VK_LOAD)
	vk->host = *host;
	PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties) host->getInstanceProcAddr(host->instance, "vkGetPhysicalDeviceMemoryProperties");
	memoryProperties(host->gpu, &vk->memory);
	uint32_t mask = host->syncIndexMask(host->context);
	vk->nFrames = 0;
	while (vk->nFrames < MAX_FRAMES && (mask >> vk->nFrames)) {
		++vk->nFrames;
	}
	if (!vk->nFrames) {
		vk->nFrames = 1;
	}
	batch->vk = vk;
	if (_setup(vk)) {
		vk->copy = _createPipeline(vk, vk->copyFragment, 0);
	}
	if (!vk->copy) {
		mLOG(GBA_VIDEO, ERROR, "Batch renderer: Vulkan setup failed");
		GBAVideoBatchRendererDeinitVulkan(batch);
		return false;
	}
	int i;
	for (i = 0; i < BATCH_VRAM_PAGES; ++i) {
		vk->pageSlot[i] = i;
	}
#ifndef DISABLE_THREADING
	vk->compiling = !ThreadCreate(&vk->compiler, _compilerThread, vk);
	if (!vk->compiling)
#endif
	{
		_compileAll(vk);
	}
	vk->nextSlot = BATCH_VRAM_PAGES;
	// Frame numbers start past the frames in flight, so no slot looks recently used
	vk->frame = MAX_FRAMES + 2;
	batch->vramDirty = (1U << BATCH_VRAM_PAGES) - 1;
	GBAVideoBatchRendererRestartFrame(batch);
	return true;
}

void GBAVideoBatchRendererDeinitVulkan(struct GBAVideoBatchRenderer* batch) {
	struct GBAVideoBatchVK* vk = batch->vk;
	if (!vk) {
		return;
	}
	VkDevice device = vk->host.device;
#ifndef DISABLE_THREADING
	if (vk->compiling) {
		__atomic_store_n(&vk->stopCompiling, true, __ATOMIC_RELEASE);
		ThreadJoin(&vk->compiler);
	}
#endif
	vk->DeviceWaitIdle(device);
	int i;
	for (i = 0; i <= BATCH_FEATURE_ALL; ++i) {
		if (vk->pipelines[i]) {
			vk->DestroyPipeline(device, vk->pipelines[i], NULL);
		}
	}
	if (vk->copy) {
		vk->DestroyPipeline(device, vk->copy, NULL);
	}
	if (vk->copyFragment) {
		vk->DestroyShaderModule(device, vk->copyFragment, NULL);
	}
	for (i = 0; i < MAX_FRAMES; ++i) {
		struct GBAVideoBatchVKFrame* frame = &vk->frames[i];
		if (frame->framebuffer) {
			vk->DestroyFramebuffer(device, frame->framebuffer, NULL);
		}
		if (frame->view) {
			vk->DestroyImageView(device, frame->view, NULL);
		}
		if (frame->image) {
			vk->DestroyImage(device, frame->image, NULL);
		}
		if (frame->imageMemory) {
			vk->FreeMemory(device, frame->imageMemory, NULL);
		}
		if (frame->data) {
			vk->DestroyBuffer(device, frame->data, NULL);
		}
		if (frame->dataMemory) {
			vk->FreeMemory(device, frame->dataMemory, NULL);
		}
	}
	if (vk->vram) {
		vk->DestroyBuffer(device, vk->vram, NULL);
	}
	if (vk->vramMemory) {
		vk->FreeMemory(device, vk->vramMemory, NULL);
	}
	if (vk->vertex) {
		vk->DestroyShaderModule(device, vk->vertex, NULL);
	}
	if (vk->fragment) {
		vk->DestroyShaderModule(device, vk->fragment, NULL);
	}
	if (vk->commandPool) {
		vk->DestroyCommandPool(device, vk->commandPool, NULL);
	}
	if (vk->descriptors) {
		vk->DestroyDescriptorPool(device, vk->descriptors, NULL);
	}
	if (vk->layout) {
		vk->DestroyPipelineLayout(device, vk->layout, NULL);
	}
	if (vk->setLayout) {
		vk->DestroyDescriptorSetLayout(device, vk->setLayout, NULL);
	}
	if (vk->renderPass) {
		vk->DestroyRenderPass(device, vk->renderPass, NULL);
	}
	free(vk);
	batch->vk = NULL;
}

// A slot the GPU is done with: not mapped now, and last read by a frame whose resources came back
static unsigned _freeSlot(struct GBAVideoBatchVK* vk) {
	uint32_t mapped[VRAM_SLOTS / 32] = { 0 };
	int i;
	for (i = 0; i < BATCH_VRAM_PAGES; ++i) {
		mapped[vk->pageSlot[i] >> 5] |= 1U << (vk->pageSlot[i] & 31);
	}
	unsigned tries;
	for (tries = 0; tries < VRAM_SLOTS; ++tries) {
		unsigned slot = vk->nextSlot;
		vk->nextSlot = (vk->nextSlot + 1) % VRAM_SLOTS;
		if (mapped[slot >> 5] & (1U << (slot & 31))) {
			continue;
		}
		if (vk->slotFrame[slot] + vk->nFrames < vk->frame) {
			return slot;
		}
	}
	// Every slot may still be read: wait for the GPU, after which only the current frame's are busy
	if (getenv("ARMGBA_BATCH_TIME")) {
		fprintf(stderr, "VRAM pool full at frame %llu\n", (unsigned long long) vk->frame);
	}
	vk->DeviceWaitIdle(vk->host.device);
	for (tries = 0; tries < VRAM_SLOTS; ++tries) {
		unsigned slot = vk->nextSlot;
		vk->nextSlot = (vk->nextSlot + 1) % VRAM_SLOTS;
		if (!(mapped[slot >> 5] & (1U << (slot & 31))) && vk->slotFrame[slot] < vk->frame) {
			return slot;
		}
	}
	return vk->pageSlot[0];
}

bool GBAVideoBatchVKReady(struct GBAVideoBatchRenderer* batch) {
	return __atomic_load_n(&batch->vk->pipelines[BATCH_FEATURE_ALL], __ATOMIC_ACQUIRE);
}

void GBAVideoBatchVKUploadVRAM(struct GBAVideoBatchRenderer* batch, uint32_t pages) {
	struct GBAVideoBatchVK* vk = batch->vk;
	while (pages) {
		unsigned page = __builtin_ctz(pages);
		pages &= pages - 1;
		unsigned slot = _freeSlot(vk);
		memcpy(&vk->vramWords[slot * 1024], &batch->d.vram[page << 11], 4096);
		vk->pageSlot[page] = slot;
		vk->slotFrame[slot] = vk->frame;
	}
}

void GBAVideoBatchVKSegment(struct GBAVideoBatchRenderer* batch, int startY, int endY) {
	struct GBAVideoBatchVK* vk = batch->vk;
	struct GBAVideoBatchSegment* segment = &vk->segments[vk->nSegments];
	++vk->nSegments;
	segment->startY = startY;
	segment->endY = endY;
	segment->features = 0;
	int y;
	for (y = startY; y < endY; ++y) {
		segment->features |= batch->features[y];
	}
	int i;
	for (i = 0; i < BATCH_VRAM_PAGES; i += 2) {
		segment->pages[i >> 1] = vk->pageSlot[i] | ((uint32_t) vk->pageSlot[i + 1] << 16);
		vk->slotFrame[vk->pageSlot[i]] = vk->frame;
		vk->slotFrame[vk->pageSlot[i + 1]] = vk->frame;
	}
}

// Everything the frame's segments read goes to the frame buffer, then one command buffer draws them all
void GBAVideoBatchVKFinishFrame(struct GBAVideoBatchRenderer* batch) {
	struct GBAVideoBatchVK* vk = batch->vk;
	int cpuFrom = batch->cpuFrom >= 0 ? batch->cpuFrom : batch->nextY;
	if (!vk->nSegments && cpuFrom >= batch->nextY) {
		return;
	}
	uint32_t index = vk->host.syncIndex(vk->host.context) % vk->nFrames;
	struct GBAVideoBatchVKFrame* frame = &vk->frames[index];
	int y;
	for (y = 0; y < batch->nextY; ++y) {
		const uint32_t* line = batch->lines[y];
		int width = 7;
		int sprites = line[3] & 0xFF;
		if (sprites) {
			width += (sprites + 7) / 8;
		}
		if (batch->features[y] & BATCH_FEATURE_WINDOWS) {
			width = 31;
		}
		memcpy(&frame->words[y * BATCH_LINE_WORDS], line, width * 16);
	}
	for (y = cpuFrom; y < batch->nextY; ++y) {
		memcpy(&frame->words[FRAME_CPU + y * GBA_VIDEO_HORIZONTAL_PIXELS / 2], &batch->outputBuffer[batch->outputBufferStride * y], GBA_VIDEO_HORIZONTAL_PIXELS * 2);
	}
	memcpy(&frame->words[FRAME_PALETTES], batch->palettes, batch->nPalettes * sizeof(batch->palettes[0]));
	memcpy(&frame->words[FRAME_SPRITES], batch->sprites, batch->nSprites * sizeof(batch->sprites[0]));

	VkCommandBuffer commands = frame->commands;
	vk->ResetCommandBuffer(commands, 0);
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->BeginCommandBuffer(commands, &begin);
	VkRenderPassBeginInfo renderPass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = vk->renderPass,
		.framebuffer = frame->framebuffer,
		.renderArea = { { 0, 0 }, { GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS } },
	};
	vk->CmdBeginRenderPass(commands, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport viewport = { 0, 0, GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, 0, 1 };
	vk->CmdSetViewport(commands, 0, 1, &viewport);
	vk->CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->layout, 0, 1, &frame->set, 0, NULL);
	VkPipeline bound = VK_NULL_HANDLE;
	int i;
	for (i = 0; i < vk->nSegments; ++i) {
		const struct GBAVideoBatchSegment* segment = &vk->segments[i];
		VkPipeline pipeline = _pipeline(vk, segment->features);
		if (pipeline != bound) {
			vk->CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound = pipeline;
		}
		vk->CmdPushConstants(commands, vk->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(segment->pages), segment->pages);
		VkRect2D scissor = { { 0, segment->startY }, { GBA_VIDEO_HORIZONTAL_PIXELS, segment->endY - segment->startY } };
		vk->CmdSetScissor(commands, 0, 1, &scissor);
		vk->CmdDraw(commands, 3, 1, 0, 0);
	}
	if (cpuFrom < batch->nextY) {
		vk->CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->copy);
		VkRect2D scissor = { { 0, cpuFrom }, { GBA_VIDEO_HORIZONTAL_PIXELS, batch->nextY - cpuFrom } };
		vk->CmdSetScissor(commands, 0, 1, &scissor);
		vk->CmdDraw(commands, 3, 1, 0, 0);
	}
	vk->CmdEndRenderPass(commands);
	vk->EndCommandBuffer(commands);

	vk->host.present(vk->host.context, frame->view, &frame->viewInfo, commands);
	vk->nSegments = 0;
	++vk->frame;
}
#endif
