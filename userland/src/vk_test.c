/*
 * vk_test: Metal over Vulkan, milestone 1 (TODO T38 (12)) -- does Mesa's Vulkan driver for the
 * Adreno (turnip) reach the GPU here through the kernel's driver (HKGPU) and the stand-in for
 * libdrm (tools/mesa/hackios-drm), and does the GPU do what it is told.
 *
 * There is no Vulkan loader on iOS: the driver is opened as the loader would, by its ICD entry
 * (vk_icdGetInstanceProcAddr). Then, one step a line: an instance, the physical device and what it
 * says of itself, a device and a queue, a 1 MB buffer in memory the processor sees, vkCmdFillBuffer
 * with one word and then another over part of it, each submitted with a fence and waited for, and
 * every word read back. The compositor and every application go on drawing with OpenGL ES; only
 * this process uses the driver.
 *
 * Milestone 2, after it: a 64 x 64 picture cleared to blue by a render pass and a red triangle
 * drawn in it (a graphics pipeline of two SPIR-V shaders, vk_test_shaders.h: turnip's compiler,
 * NIR to ir3, on the phone), copied into the buffer and looked at: the middle red, the corners
 * blue, and how many pixels are red (the triangle covers 512 of them), with a picture of it in
 * letters (one for 2 x 4 pixels: R red, B blue, ? anything else) since no file comes back from a run.
 *
 *   usage: vk_test [TU_DEBUG-OPTIONS] [DRIVER]
 *          OPTIONS such as sysmem,noubwc are added to TU_DEBUG (startup is always there): the same draw with
 *          turnip's ways of rendering taken away one by one, so that a fault says which part it is in
 *          (an rc job list has no environment of its own to set). DRIVER: /hackios/lib/libvulkan_freedreno.dylib.
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vk_test_shaders.h"

#define WORDS           (256 * 1024)            /* 1 MB */

/* the ICD's own entry (the loader's interface header has it; there is no loader here) */
typedef VkResult (VKAPI_PTR *PFN_vkNegotiateLoaderICDInterfaceVersion)(uint32_t *version);

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

#define INSTANCE_FN(instance, name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DEVICE_FN(device, name) PFN_##name name = (PFN_##name)gdpa(device, #name)

/* fill [first, first + count) words with `word` on the GPU, wait for it, and say how long it took */
static int
fill(VkDevice device, VkQueue queue, VkCommandBuffer commands, VkBuffer buffer, uint32_t first, uint32_t count,
    uint32_t word)
{
	DEVICE_FN(device, vkBeginCommandBuffer);
	DEVICE_FN(device, vkCmdFillBuffer);
	DEVICE_FN(device, vkEndCommandBuffer);
	DEVICE_FN(device, vkCreateFence);
	DEVICE_FN(device, vkQueueSubmit);
	DEVICE_FN(device, vkWaitForFences);
	DEVICE_FN(device, vkDestroyFence);
	DEVICE_FN(device, vkResetCommandBuffer);
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands };
	VkFence fence;
	VkResult result;
	uint64_t began, submitted, done;

	vkResetCommandBuffer(commands, 0);
	if ((result = vkBeginCommandBuffer(commands, &begin)) != VK_SUCCESS) {
		printf("vk_test: vkBeginCommandBuffer: %d\n", result);
		return 0;
	}
	vkCmdFillBuffer(commands, buffer, (VkDeviceSize)first * 4, (VkDeviceSize)count * 4, word);
	if ((result = vkEndCommandBuffer(commands)) != VK_SUCCESS || (result = vkCreateFence(device, &fence_info, NULL, &fence)) != VK_SUCCESS) {
		printf("vk_test: recording or the fence: %d\n", result);
		return 0;
	}
	began = now_us();
	result = vkQueueSubmit(queue, 1, &submit, fence);
	submitted = now_us();
	if (result != VK_SUCCESS) {
		printf("vk_test: vkQueueSubmit: %d\n", result);
		vkDestroyFence(device, fence, NULL);
		return 0;
	}
	result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
	done = now_us();
	printf("vk_test: fill of %u words with %#x: submitted in %llu us, the fence %s after %llu us more\n", count, word,
	    (unsigned long long)(submitted - began), result == VK_SUCCESS ? "signalled" : result == VK_TIMEOUT ? "TIMED OUT" : "FAILED",
	    (unsigned long long)(done - submitted));
	vkDestroyFence(device, fence, NULL);
	return result == VK_SUCCESS;
}

/* how many of [first, first + count) words are `word`, and the first that is not */
static int
check(const volatile uint32_t *words, uint32_t first, uint32_t count, uint32_t word)
{
	uint32_t right = 0, wrong_at = 0;

	for (uint32_t i = first; i < first + count; i++) {
		right += words[i] == word;
	}
	if (right == count) {
		printf("vk_test:   all %u words read back as %#x\n", count, word);
		return 1;
	}
	for (uint32_t i = first; i < first + count; i++) {
		if (words[i] != word) {
			wrong_at = i;
			break;
		}
	}
	printf("vk_test:   %u of %u words are %#x; the first that is not, word %u, is %#x\n", right, count, word, wrong_at,
	    words[wrong_at]);
	return 0;
}

#define SIDE    64

/* milestone 2: clear and a triangle, into a picture copied to `buffer` (mapped at `words`); 1 if all is as it should be */
static int
triangle(VkDevice device, VkQueue queue, VkCommandBuffer commands, uint32_t type, VkBuffer buffer,
    const volatile uint32_t *words)
{
	DEVICE_FN(device, vkCreateImage);
	DEVICE_FN(device, vkGetImageMemoryRequirements);
	DEVICE_FN(device, vkAllocateMemory);
	DEVICE_FN(device, vkBindImageMemory);
	DEVICE_FN(device, vkCreateImageView);
	DEVICE_FN(device, vkCreateRenderPass);
	DEVICE_FN(device, vkCreateFramebuffer);
	DEVICE_FN(device, vkCreateShaderModule);
	DEVICE_FN(device, vkCreatePipelineLayout);
	DEVICE_FN(device, vkCreateGraphicsPipelines);
	DEVICE_FN(device, vkResetCommandBuffer);
	DEVICE_FN(device, vkBeginCommandBuffer);
	DEVICE_FN(device, vkCmdBeginRenderPass);
	DEVICE_FN(device, vkCmdBindPipeline);
	DEVICE_FN(device, vkCmdDraw);
	DEVICE_FN(device, vkCmdEndRenderPass);
	DEVICE_FN(device, vkCmdCopyImageToBuffer);
	DEVICE_FN(device, vkCmdPipelineBarrier);
	DEVICE_FN(device, vkEndCommandBuffer);
	DEVICE_FN(device, vkCreateFence);
	DEVICE_FN(device, vkQueueSubmit);
	DEVICE_FN(device, vkWaitForFences);
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { SIDE, SIDE, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference colour_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
	                                 .pColorAttachments = &colour_ref };
	/* the render pass's writes before the copy that reads the picture */
	VkSubpassDependency after = {
		.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	VkRenderPassCreateInfo pass_info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
	                                     .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
	                                     .dependencyCount = 1, .pDependencies = &after };
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	VkShaderModule vertex, fragment;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkFence fence;
	VkMemoryRequirements needs;
	VkResult result;
	uint64_t began, made, done;
	uint32_t red = 0, blue = 0, other = 0, middle, corner;

	if ((result = vkCreateImage(device, &image_info, NULL, &image)) != VK_SUCCESS) {
		printf("vk_test: vkCreateImage: %d\n", result);
		return 0;
	}
	vkGetImageMemoryRequirements(device, image, &needs);
	{
		VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
		                                  .memoryTypeIndex = type };

		if ((needs.memoryTypeBits & (1u << type)) == 0 ||
		    (result = vkAllocateMemory(device, &allocate, NULL, &memory)) != VK_SUCCESS ||
		    (result = vkBindImageMemory(device, image, memory, 0)) != VK_SUCCESS) {
			printf("vk_test: the picture's memory (types %#x): %d\n", needs.memoryTypeBits, result);
			return 0;
		}
	}
	{
		VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		VkFramebufferCreateInfo framebuffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .attachmentCount = 1, .pAttachments = &view,
			.width = SIDE, .height = SIDE, .layers = 1,
		};

		if ((result = vkCreateImageView(device, &view_info, NULL, &view)) != VK_SUCCESS ||
		    (result = vkCreateRenderPass(device, &pass_info, NULL, &pass)) != VK_SUCCESS ||
		    (framebuffer_info.renderPass = pass, (result = vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer))) != VK_SUCCESS) {
			printf("vk_test: the view, render pass or framebuffer: %d\n", result);
			return 0;
		}
	}
	began = now_us();
	{
		VkShaderModuleCreateInfo vertex_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                         .codeSize = sizeof(vk_test_vertex), .pCode = vk_test_vertex };
		VkShaderModuleCreateInfo fragment_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                           .codeSize = sizeof(vk_test_fragment), .pCode = vk_test_fragment };
		VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" },
		};
		VkPipelineVertexInputStateCreateInfo input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		VkViewport viewport = { 0, 0, SIDE, SIDE, 0, 1 };
		VkRect2D scissor = { { 0, 0 }, { SIDE, SIDE } };
		VkPipelineViewportStateCreateInfo viewports = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		                                                .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1,
		                                                .pScissors = &scissor };
		VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		                                                  .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
		                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
		VkPipelineMultisampleStateCreateInfo samples = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		                                                 .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
		VkPipelineColorBlendAttachmentState blend_attachment = { .colorWriteMask = 0xf };
		VkPipelineColorBlendStateCreateInfo blend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		                                              .attachmentCount = 1, .pAttachments = &blend_attachment };
		VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
			.pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewports,
			.pRasterizationState = &raster, .pMultisampleState = &samples, .pColorBlendState = &blend,
			.renderPass = pass, .subpass = 0,
		};

		if ((result = vkCreateShaderModule(device, &vertex_info, NULL, &vertex)) != VK_SUCCESS ||
		    (result = vkCreateShaderModule(device, &fragment_info, NULL, &fragment)) != VK_SUCCESS ||
		    (result = vkCreatePipelineLayout(device, &layout_info, NULL, &layout)) != VK_SUCCESS) {
			printf("vk_test: the shader modules or pipeline layout: %d\n", result);
			return 0;
		}
		stages[0].module = vertex;
		stages[1].module = fragment;
		pipeline_info.layout = layout;
		if ((result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline)) != VK_SUCCESS) {
			printf("vk_test: vkCreateGraphicsPipelines: %d\n", result);
			return 0;
		}
	}
	made = now_us();
	printf("vk_test: the triangle's pipeline compiled (SPIR-V -> NIR -> ir3) in %llu us\n", (unsigned long long)(made - began));
	{
		VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		VkClearValue blue_clear = { .color = { .float32 = { 0, 0, 1, 1 } } };
		VkRenderPassBeginInfo pass_begin = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
		                                     .framebuffer = framebuffer, .renderArea = { { 0, 0 }, { SIDE, SIDE } },
		                                     .clearValueCount = 1, .pClearValues = &blue_clear };
		VkBufferImageCopy copy = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		                           .imageExtent = { SIDE, SIDE, 1 } };
		VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands };

		vkResetCommandBuffer(commands, 0);
		vkBeginCommandBuffer(commands, &begin);
		vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(commands, 3, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
		{
			/* the copy's writes made visible to this processor's reads after the fence */
			VkMemoryBarrier to_host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };

			vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
			    NULL, 0, NULL);
		}
		if ((result = vkEndCommandBuffer(commands)) != VK_SUCCESS ||
		    (result = vkCreateFence(device, &fence_info, NULL, &fence)) != VK_SUCCESS) {
			printf("vk_test: recording the triangle: %d\n", result);
			return 0;
		}
		began = now_us();
		result = vkQueueSubmit(queue, 1, &submit, fence);
		if (result == VK_SUCCESS) {
			result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
		}
		done = now_us();
		printf("vk_test: clear, triangle and copy: %s after %llu us\n", result == VK_SUCCESS ? "done" :
		    result == VK_TIMEOUT ? "TIMED OUT" : "FAILED", (unsigned long long)(done - began));
		if (result != VK_SUCCESS) {
			return 0;
		}
	}
	/* R8G8B8A8_UNORM, little-endian words: red is 0xff0000ff, blue 0xffff0000 */
	for (uint32_t i = 0; i < SIDE * SIDE; i++) {
		red += words[i] == 0xff0000ffu;
		blue += words[i] == 0xffff0000u;
		other += words[i] != 0xff0000ffu && words[i] != 0xffff0000u;
	}
	middle = words[(SIDE / 2) * SIDE + SIDE / 2];
	corner = words[1 * SIDE + 1];
	for (uint32_t y = 0; y < SIDE; y += 4) {
		char line[SIDE / 2 + 1];

		for (uint32_t x = 0; x < SIDE; x += 2) {
			uint32_t pixel = words[y * SIDE + x];

			line[x / 2] = pixel == 0xff0000ffu ? 'R' : pixel == 0xffff0000u ? 'B' : '?';
		}
		line[SIDE / 2] = 0;
		printf("vk_test:   |%s|\n", line);
	}
	printf("vk_test:   pixels: %u red, %u blue, %u other; the middle %#x, a corner %#x\n", red, blue, other, middle, corner);
	return middle == 0xff0000ffu && corner == 0xffff0000u && other == 0 && red > 448 && red < 576;
}

int
main(int argc, char **argv)
{
	const char *options = argc > 1 && strchr(argv[1], '/') == NULL ? argv[1] : NULL;
	const char *path = argc > 1 + (options != NULL) ? argv[1 + (options != NULL)] : "/hackios/lib/libvulkan_freedreno.dylib";
	char tu_debug[128];
	PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
	uint32_t icd_version = 5, count = 0, family = UINT32_MAX, type = UINT32_MAX;
	void *driver;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkResult result;
	int passed;

	setvbuf(stdout, NULL, _IONBF, 0);
	/* why a device is refused, if it is; and the ways of rendering asked to be left out */
	snprintf(tu_debug, sizeof(tu_debug), "startup%s%s", options != NULL ? "," : "", options != NULL ? options : "");
	setenv("TU_DEBUG", tu_debug, 1);
	printf("vk_test: TU_DEBUG=%s\n", tu_debug);
	if ((driver = dlopen(path, RTLD_NOW | RTLD_LOCAL)) == NULL) {
		printf("vk_test: the driver cannot be opened: %s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vk_icdGetInstanceProcAddr");
	negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)dlsym(driver, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (gipa == NULL) {
		printf("vk_test: the driver has no vk_icdGetInstanceProcAddr\n");
		return 1;
	}
	if (negotiate != NULL) {
		negotiate(&icd_version);
	}
	printf("vk_test: driver opened (ICD interface %u)\n", icd_version);

	{
		INSTANCE_FN(NULL, vkCreateInstance);
		VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_test",
		                          .apiVersion = VK_API_VERSION_1_1 };
		VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };

		if (vkCreateInstance == NULL || (result = vkCreateInstance(&info, NULL, &instance)) != VK_SUCCESS) {
			printf("vk_test: vkCreateInstance: %d\n", vkCreateInstance == NULL ? -1 : result);
			return 1;
		}
	}
	{
		INSTANCE_FN(instance, vkEnumeratePhysicalDevices);
		INSTANCE_FN(instance, vkGetPhysicalDeviceProperties);
		INSTANCE_FN(instance, vkGetPhysicalDeviceMemoryProperties);
		INSTANCE_FN(instance, vkGetPhysicalDeviceQueueFamilyProperties);
		VkPhysicalDeviceProperties properties;
		VkPhysicalDeviceMemoryProperties memory;
		VkQueueFamilyProperties families[8];
		uint32_t family_count = 8;

		result = vkEnumeratePhysicalDevices(instance, &count, NULL);
		printf("vk_test: %u physical device(s) (%d)\n", count, result);
		if (count == 0) {
			return 1;
		}
		count = 1;
		vkEnumeratePhysicalDevices(instance, &count, &physical);
		vkGetPhysicalDeviceProperties(physical, &properties);
		printf("vk_test: \"%s\", Vulkan %u.%u.%u, driver %#x, vendor %#x device %#x\n", properties.deviceName,
		    VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
		    VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion, properties.vendorID, properties.deviceID);
		vkGetPhysicalDeviceMemoryProperties(physical, &memory);
		for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
			VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;

			printf("vk_test:   memory type %u: heap %u, flags %#x\n", i, memory.memoryTypes[i].heapIndex, flags);
			if (type == UINT32_MAX && (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				type = i;
			}
		}
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
		for (uint32_t i = 0; i < family_count; i++) {
			printf("vk_test:   queue family %u: flags %#x, %u queue(s)\n", i, families[i].queueFlags, families[i].queueCount);
			if (family == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
				family = i;
			}
		}
		if (type == UINT32_MAX || family == UINT32_MAX) {
			printf("vk_test: no host-visible coherent memory or no graphics queue\n");
			return 1;
		}
	}
	{
		INSTANCE_FN(instance, vkCreateDevice);
		float priority = 1;
		VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		                                       .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
		VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		                            .pQueueCreateInfos = &queue_info };
		uint64_t began = now_us();

		if ((result = vkCreateDevice(physical, &info, NULL, &device)) != VK_SUCCESS) {
			printf("vk_test: vkCreateDevice: %d\n", result);
			return 1;
		}
		printf("vk_test: device made in %llu us\n", (unsigned long long)(now_us() - began));
		gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
	}
	{
		DEVICE_FN(device, vkGetDeviceQueue);
		DEVICE_FN(device, vkCreateBuffer);
		DEVICE_FN(device, vkGetBufferMemoryRequirements);
		DEVICE_FN(device, vkAllocateMemory);
		DEVICE_FN(device, vkBindBufferMemory);
		DEVICE_FN(device, vkMapMemory);
		DEVICE_FN(device, vkCreateCommandPool);
		DEVICE_FN(device, vkAllocateCommandBuffers);
		DEVICE_FN(device, vkDeviceWaitIdle);
		DEVICE_FN(device, vkDestroyDevice);
		VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WORDS * 4,
		                                   .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
		VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
		VkMemoryRequirements needs;
		VkBuffer buffer;
		VkDeviceMemory memory;
		VkCommandPool pool;
		VkCommandBuffer commands;
		volatile uint32_t *words;

		vkGetDeviceQueue(device, family, 0, &queue);
		if ((result = vkCreateBuffer(device, &buffer_info, NULL, &buffer)) != VK_SUCCESS) {
			printf("vk_test: vkCreateBuffer: %d\n", result);
			return 1;
		}
		vkGetBufferMemoryRequirements(device, buffer, &needs);
		{
			VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
			                                  .memoryTypeIndex = type };

			if ((result = vkAllocateMemory(device, &allocate, NULL, &memory)) != VK_SUCCESS ||
			    (result = vkBindBufferMemory(device, buffer, memory, 0)) != VK_SUCCESS ||
			    (result = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words)) != VK_SUCCESS) {
				printf("vk_test: the buffer's memory: %d\n", result);
				return 1;
			}
		}
		printf("vk_test: a %u-byte buffer, mapped at %p\n", WORDS * 4, (void *)words);
		memset((void *)words, 0, WORDS * 4);
		if ((result = vkCreateCommandPool(device, &pool_info, NULL, &pool)) != VK_SUCCESS) {
			printf("vk_test: vkCreateCommandPool: %d\n", result);
			return 1;
		}
		{
			VkCommandBufferAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			                                         .commandBufferCount = 1 };

			if ((result = vkAllocateCommandBuffers(device, &allocate, &commands)) != VK_SUCCESS) {
				printf("vk_test: vkAllocateCommandBuffers: %d\n", result);
				return 1;
			}
		}
		passed = fill(device, queue, commands, buffer, 0, WORDS, 0xdeadbeef) && check(words, 0, WORDS, 0xdeadbeef);
		/* the second over the middle half only: what is around it stays the first */
		passed = passed && fill(device, queue, commands, buffer, WORDS / 4, WORDS / 2, 0x12345678) &&
		    check(words, 0, WORDS / 4, 0xdeadbeef) && check(words, WORDS / 4, WORDS / 2, 0x12345678) &&
		    check(words, WORDS * 3 / 4, WORDS / 4, 0xdeadbeef);
		printf("vk_test: milestone 1 %s\n", passed ? "PASSED" : "FAILED");
		if (passed) {
			passed = triangle(device, queue, commands, type, buffer, words);
			printf("vk_test: milestone 2 %s\n", passed ? "PASSED" : "FAILED");
		}
		vkDeviceWaitIdle(device);
		vkDestroyDevice(device, NULL);
	}
	return passed ? 0 : 1;
}
