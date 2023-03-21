#include "common.h"

#include "device.h"
#include "resources.h"
#include "shaders.h"
#include "swapchain.h"

#include <stdio.h>

#include <vector>
#include <algorithm>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <objparser.h>
#include <meshoptimizer.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_transform.hpp>

bool meshShadingEnabled = true;

VkSemaphore createSemaphore(VkDevice device)
{
	VkSemaphoreCreateInfo createInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

	VkSemaphore semaphore = 0;
	VK_CHECK(vkCreateSemaphore(device, &createInfo, 0, &semaphore));

	return semaphore;
}

VkCommandPool createCommandPool(VkDevice device, uint32_t familyIndex)
{
	VkCommandPoolCreateInfo createInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	createInfo.queueFamilyIndex = familyIndex;

	VkCommandPool commandPool = 0;
	VK_CHECK(vkCreateCommandPool(device, &createInfo, 0, &commandPool));

	return commandPool;
}

VkRenderPass createRenderPass(VkDevice device, VkFormat colorFormat, VkFormat depthFormat)
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = colorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[1].format = depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorAttachment = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthAttachment = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachment;
	subpass.pDepthStencilAttachment = &depthAttachment;

	VkRenderPassCreateInfo createInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	createInfo.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
	createInfo.pAttachments = attachments;
	createInfo.subpassCount = 1;
	createInfo.pSubpasses = &subpass;

	VkRenderPass renderPass = 0;
	VK_CHECK(vkCreateRenderPass(device, &createInfo, 0, &renderPass));

	return renderPass;
}

VkFramebuffer createFramebuffer(VkDevice device, VkRenderPass renderPass, VkImageView colorView, VkImageView depthView, uint32_t width, uint32_t height)
{
	VkImageView attachments[] = { colorView, depthView };

	VkFramebufferCreateInfo createInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	createInfo.renderPass = renderPass;
	createInfo.attachmentCount = ARRAYSIZE(attachments);
	createInfo.pAttachments = attachments;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.layers = 1;

	VkFramebuffer framebuffer = 0;
	VK_CHECK(vkCreateFramebuffer(device, &createInfo, 0, &framebuffer));

	return framebuffer;
}

VkQueryPool createQueryPool(VkDevice device, uint32_t queryCount)
{
	VkQueryPoolCreateInfo createInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	createInfo.queryCount = queryCount;

	VkQueryPool queryPool = 0;
	VK_CHECK(vkCreateQueryPool(device, &createInfo, 0, &queryPool));

	return queryPool;
}

struct alignas(16) Meshlet
{
	glm::vec3 center;
	float radius;
	int8_t cone_axis[3];
	int8_t cone_cutoff;

	uint32_t dataOffset; // dataOffset..dataOffset+vertexCount-1 stores vertex indices, we store indices packed in 4b units after that
	uint8_t vertexCount;
	uint8_t triangleCount;
};

struct alignas(16) Globals
{
	glm::mat4 projection;
};

struct alignas(16) MeshDraw
{
	glm::vec3 position;
	float scale;
	glm::quat orientation;

	union
	{
		uint32_t commandData[7];

		struct
		{
			VkDrawIndexedIndirectCommand commandIndirect; // 5 uint32_t
			VkDrawMeshTasksIndirectCommandNV commandIndirectMS; // 2 uint32_t
		};
	};
};

struct Vertex
{
	float vx, vy, vz;
	uint8_t nx, ny, nz, nw;
	uint16_t tu, tv;
};

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<Meshlet> meshlets;
	std::vector<uint32_t> meshletdata;
};

bool loadMesh(Mesh& result, const char* path)
{
	ObjFile file;
	if (!objParseFile(file, path))
		return false;

	size_t index_count = file.f_size / 3;

	std::vector<Vertex> vertices(index_count);

	for (size_t i = 0; i < index_count; ++i)
	{
		Vertex& v = vertices[i];

		int vi = file.f[i * 3 + 0];
		int vti = file.f[i * 3 + 1];
		int vni = file.f[i * 3 + 2];

		float nx = vni < 0 ? 0.f : file.vn[vni * 3 + 0];
		float ny = vni < 0 ? 0.f : file.vn[vni * 3 + 1];
		float nz = vni < 0 ? 1.f : file.vn[vni * 3 + 2];

		v.vx = file.v[vi * 3 + 0];
		v.vy = file.v[vi * 3 + 1];
		v.vz = file.v[vi * 3 + 2];
		v.nx = uint8_t(nx * 127.f + 127.f); // TODO: fix rounding
		v.ny = uint8_t(ny * 127.f + 127.f); // TODO: fix rounding
		v.nz = uint8_t(nz * 127.f + 127.f); // TODO: fix rounding
		v.tu = meshopt_quantizeHalf(vti < 0 ? 0.f : file.vt[vti * 3 + 0]);
		v.tv = meshopt_quantizeHalf(vti < 0 ? 0.f : file.vt[vti * 3 + 1]);
	}

	std::vector<uint32_t> remap(index_count);
	size_t vertex_count = meshopt_generateVertexRemap(remap.data(), 0, index_count, vertices.data(), index_count, sizeof(Vertex));

	result.vertices.resize(vertex_count);
	result.indices.resize(index_count);

	meshopt_remapVertexBuffer(result.vertices.data(), vertices.data(), index_count, sizeof(Vertex), remap.data());
	meshopt_remapIndexBuffer(result.indices.data(), 0, index_count, remap.data());

	meshopt_optimizeVertexCache(result.indices.data(), result.indices.data(), index_count, vertex_count);
	meshopt_optimizeVertexFetch(result.vertices.data(), result.indices.data(), index_count, result.vertices.data(), vertex_count, sizeof(Vertex));

	return true;
}

void buildMeshlets(Mesh& mesh)
{
	const size_t max_vertices = 64;
	const size_t max_triangles = 124;

	std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(mesh.indices.size(), max_vertices, max_triangles));
	meshlets.resize(meshopt_buildMeshlets(meshlets.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.size(), max_vertices, max_triangles));

	// TODO: we don't *really* need this but this makes sure we can assume that we need all 32 meshlets in task shader
	while (meshlets.size() % 32)
		meshlets.push_back(meshopt_Meshlet());

	for (auto& meshlet : meshlets)
	{
		size_t dataOffset = mesh.meshletdata.size();

		for (unsigned int i = 0; i < meshlet.vertex_count; ++i)
			mesh.meshletdata.push_back(meshlet.vertices[i]);

		const unsigned int* indexGroups = reinterpret_cast<const unsigned int*>(meshlet.indices);
		unsigned int indexGroupCount = (meshlet.triangle_count * 3 + 3) / 4;

		for (unsigned int i = 0; i < indexGroupCount; ++i)
			mesh.meshletdata.push_back(indexGroups[i]);

		meshopt_Bounds bounds = meshopt_computeMeshletBounds(&meshlet, &mesh.vertices[0].vx, mesh.vertices.size(), sizeof(Vertex));

		Meshlet m = {};
		m.dataOffset = uint32_t(dataOffset);
		m.triangleCount = meshlet.triangle_count;
		m.vertexCount = meshlet.vertex_count;

		m.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
		m.radius = bounds.radius;
		m.cone_axis[0] = bounds.cone_axis_s8[0];
		m.cone_axis[1] = bounds.cone_axis_s8[1];
		m.cone_axis[2] = bounds.cone_axis_s8[2];
		m.cone_cutoff = bounds.cone_cutoff_s8;

		mesh.meshlets.push_back(m);
	}
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if (action == GLFW_PRESS)
	{
		if (key == GLFW_KEY_R)
		{
			meshShadingEnabled = !meshShadingEnabled;
		}
	}
}

glm::mat4 perspectiveProjection(float fovY, float aspectWbyH, float zNear)
{
	float f = 1.0f / tanf(fovY / 2.0f);
	return glm::mat4(
		f / aspectWbyH, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, zNear, 0.0f);
}

int main(int argc, const char** argv)
{
	if (argc < 2)
	{
		printf("Usage: %s [mesh]\n", argv[0]);
		return 1;
	}

	int rc = glfwInit();
	assert(rc);

	VK_CHECK(volkInitialize());

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	VkInstance instance = createInstance();
	assert(instance);

	volkLoadInstance(instance);

#ifdef _DEBUG
	VkDebugReportCallbackEXT debugCallback = registerDebugCallback(instance);
#endif

	VkPhysicalDevice physicalDevices[16];
	uint32_t physicalDeviceCount = sizeof(physicalDevices) / sizeof(physicalDevices[0]);
	VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices));

	VkPhysicalDevice physicalDevice = pickPhysicalDevice(physicalDevices, physicalDeviceCount);
	assert(physicalDevice);

	uint32_t extensionCount = 0;
	VK_CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice, 0, &extensionCount, 0));

	std::vector<VkExtensionProperties> extensions(extensionCount);
	VK_CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice, 0, &extensionCount, extensions.data()));

	bool meshShadingSupported = false;
	for (auto& ext : extensions)
		if (strcmp(ext.extensionName, "VK_NV_mesh_shader") == 0)
		{
			meshShadingSupported = true;
			break;
		}

	meshShadingEnabled = meshShadingSupported;

	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties(physicalDevice, &props);
	assert(props.limits.timestampComputeAndGraphics);

	uint32_t familyIndex = getGraphicsFamilyIndex(physicalDevice);
	assert(familyIndex != VK_QUEUE_FAMILY_IGNORED);

	VkDevice device = createDevice(instance, physicalDevice, familyIndex, meshShadingSupported);
	assert(device);

	GLFWwindow* window = glfwCreateWindow(1024, 768, "niagara", 0, 0);
	assert(window);

	glfwSetKeyCallback(window, keyCallback);

	VkSurfaceKHR surface = createSurface(instance, window);
	assert(surface);

	VkBool32 presentSupported = 0;
	VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, familyIndex, surface, &presentSupported));
	assert(presentSupported);

	VkFormat swapchainFormat = getSwapchainFormat(physicalDevice, surface);
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

	VkSemaphore acquireSemaphore = createSemaphore(device);
	assert(acquireSemaphore);

	VkSemaphore releaseSemaphore = createSemaphore(device);
	assert(releaseSemaphore);

	VkQueue queue = 0;
	vkGetDeviceQueue(device, familyIndex, 0, &queue);

	VkRenderPass renderPass = createRenderPass(device, swapchainFormat, depthFormat);
	assert(renderPass);

	bool rcs = false;

	Shader meshVS = {};
	rcs = loadShader(meshVS, device, "shaders/mesh.vert.spv");
	assert(rcs);

	Shader meshFS = {};
	rcs = loadShader(meshFS, device, "shaders/mesh.frag.spv");
	assert(rcs);

	Shader meshletMS = {};
	Shader meshletTS = {};
	if (meshShadingSupported)
	{
		rcs = loadShader(meshletMS, device, "shaders/meshlet.mesh.spv");
		assert(rcs);

		rcs = loadShader(meshletTS, device, "shaders/meshlet.task.spv");
		assert(rcs);
	}

	// TODO: this is critical for performance!
	VkPipelineCache pipelineCache = 0;

	Program meshProgram = createProgram(device, VK_PIPELINE_BIND_POINT_GRAPHICS, { &meshVS, &meshFS }, sizeof(Globals));

	Program meshProgramMS = {};
	if (meshShadingSupported)
		meshProgramMS = createProgram(device, VK_PIPELINE_BIND_POINT_GRAPHICS, { &meshletTS, &meshletMS, &meshFS }, sizeof(Globals));

	VkPipeline meshPipeline = createGraphicsPipeline(device, pipelineCache, renderPass, { &meshVS, &meshFS }, meshProgram.layout);
	assert(meshPipeline);

	VkPipeline meshPipelineMS = 0;
	if (meshShadingSupported)
	{
		meshPipelineMS = createGraphicsPipeline(device, pipelineCache, renderPass, { &meshletTS, &meshletMS, &meshFS }, meshProgramMS.layout);
		assert(meshPipelineMS);
	}

	Swapchain swapchain;
	createSwapchain(swapchain, physicalDevice, device, surface, familyIndex, swapchainFormat, renderPass);

	VkQueryPool queryPool = createQueryPool(device, 128);
	assert(queryPool);

	VkCommandPool commandPool = createCommandPool(device, familyIndex);
	assert(commandPool);

	VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocateInfo.commandPool = commandPool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = 0;
	VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer));

	VkPhysicalDeviceMemoryProperties memoryProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

	Mesh mesh;
	bool rcm = loadMesh(mesh, argv[1]);
	assert(rcm);

	if (meshShadingSupported)
	{
		buildMeshlets(mesh);
	}

	Buffer scratch = {};
	createBuffer(scratch, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	Buffer vb = {};
	createBuffer(vb, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	Buffer ib = {};
	createBuffer(ib, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	Buffer mb = {};
	Buffer mdb = {};
	if (meshShadingSupported)
	{
		createBuffer(mb, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		createBuffer(mdb, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	}

	uploadBuffer(device, commandPool, commandBuffer, queue, vb, scratch, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
	uploadBuffer(device, commandPool, commandBuffer, queue, ib, scratch, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));

	if (meshShadingSupported)
	{
		uploadBuffer(device, commandPool, commandBuffer, queue, mb, scratch, mesh.meshlets.data(), mesh.meshlets.size() * sizeof(Meshlet));
		uploadBuffer(device, commandPool, commandBuffer, queue, mdb, scratch, mesh.meshletdata.data(), mesh.meshletdata.size() * sizeof(uint32_t));
	}

	uint32_t drawCount = 3000;
	std::vector<MeshDraw> draws(drawCount);

	srand(42);

	for (uint32_t i = 0; i < drawCount; ++i)
	{
		draws[i].position[0] = (float(rand()) / RAND_MAX) * 40 - 20;
		draws[i].position[1] = (float(rand()) / RAND_MAX) * 40 - 20;
		draws[i].position[2] = (float(rand()) / RAND_MAX) * 40 - 20;
		draws[i].scale = (float(rand()) / RAND_MAX) + 1;

		glm::vec3 axis((float(rand()) / RAND_MAX) * 2 - 1, (float(rand()) / RAND_MAX) * 2 - 1, (float(rand()) / RAND_MAX) * 2 - 1);
		float angle = glm::radians((float(rand()) / RAND_MAX) * 90.f);

		draws[i].orientation = glm::rotate(glm::quat(1, 0, 0, 0), angle, axis);

		memset(draws[i].commandData, 0, sizeof(draws[i].commandData));
		draws[i].commandIndirect.indexCount = uint32_t(mesh.indices.size());
		draws[i].commandIndirect.instanceCount = 1;
		draws[i].commandIndirectMS.taskCount = uint32_t(mesh.meshlets.size() / 32);
	}

	Buffer db = {};
	createBuffer(db, device, memoryProperties, 128 * 1024 * 1024, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	uploadBuffer(device, commandPool, commandBuffer, queue, db, scratch, draws.data(), draws.size() * sizeof(MeshDraw));

	Image colorTarget = {};
	Image depthTarget = {};
	VkFramebuffer targetFB = 0;

	double frameCpuAvg = 0;
	double frameGpuAvg = 0;

	while (!glfwWindowShouldClose(window))
	{
		double frameCpuBegin = glfwGetTime() * 1000;

		glfwPollEvents();

		if (resizeSwapchainIfNecessary(swapchain, physicalDevice, device, surface, familyIndex, swapchainFormat, renderPass) || !targetFB)
		{
			if (colorTarget.image)
				destroyImage(colorTarget, device);
			if (depthTarget.image)
				destroyImage(depthTarget, device);
			if (targetFB)
				vkDestroyFramebuffer(device, targetFB, 0);

			createImage(colorTarget, device, memoryProperties, swapchain.width, swapchain.height, swapchainFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			createImage(depthTarget, device, memoryProperties, swapchain.width, swapchain.height, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
			targetFB = createFramebuffer(device, renderPass, colorTarget.imageView, depthTarget.imageView, swapchain.width, swapchain.height);
		}

		uint32_t imageIndex = 0;
		VK_CHECK(vkAcquireNextImageKHR(device, swapchain.swapchain, ~0ull, acquireSemaphore, VK_NULL_HANDLE, &imageIndex));

		VK_CHECK(vkResetCommandPool(device, commandPool, 0));

		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

		vkCmdResetQueryPool(commandBuffer, queryPool, 0, 128);
		vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 0);

		VkImageMemoryBarrier renderBeginBarriers[] =
		{
			imageBarrier(colorTarget.image, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			imageBarrier(depthTarget.image, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT),
		};

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, 0, 0, 0, ARRAYSIZE(renderBeginBarriers), renderBeginBarriers);

		VkClearValue clearValues[2] = {};
		clearValues[0].color = { 48.f / 255.f, 10.f / 255.f, 36.f / 255.f, 1 };
		clearValues[1].depthStencil = { 0.f, 0 };

		VkRenderPassBeginInfo passBeginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		passBeginInfo.renderPass = renderPass;
		passBeginInfo.framebuffer = targetFB;
		passBeginInfo.renderArea.extent.width = swapchain.width;
		passBeginInfo.renderArea.extent.height = swapchain.height;
		passBeginInfo.clearValueCount = ARRAYSIZE(clearValues);
		passBeginInfo.pClearValues = clearValues;

		vkCmdBeginRenderPass(commandBuffer, &passBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = { 0, float(swapchain.height), float(swapchain.width), -float(swapchain.height), 0, 1 };
		VkRect2D scissor = { {0, 0}, {uint32_t(swapchain.width), uint32_t(swapchain.height)} };

		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		Globals globals = {};
		globals.projection = perspectiveProjection(glm::radians(70.f), float(swapchain.width) / float(swapchain.height), 0.01f);

		if (meshShadingSupported && meshShadingEnabled)
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipelineMS);

			DescriptorInfo descriptors[] = { db.buffer, mb.buffer, mdb.buffer, vb.buffer };
			vkCmdPushDescriptorSetWithTemplateKHR(commandBuffer, meshProgramMS.updateTemplate, meshProgramMS.layout, 0, descriptors);

			vkCmdPushConstants(commandBuffer, meshProgramMS.layout, meshProgramMS.pushConstantStages, 0, sizeof(globals), &globals);
			vkCmdDrawMeshTasksIndirectNV(commandBuffer, db.buffer, offsetof(MeshDraw, commandIndirectMS), uint32_t(draws.size()), sizeof(MeshDraw));
		}
		else
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);

			DescriptorInfo descriptors[] = { db.buffer, vb.buffer };
			vkCmdPushDescriptorSetWithTemplateKHR(commandBuffer, meshProgram.updateTemplate, meshProgram.layout, 0, descriptors);

			vkCmdBindIndexBuffer(commandBuffer, ib.buffer, 0, VK_INDEX_TYPE_UINT32);

			vkCmdPushConstants(commandBuffer, meshProgram.layout, meshProgram.pushConstantStages, 0, sizeof(globals), &globals);
			vkCmdDrawIndexedIndirect(commandBuffer, db.buffer, offsetof(MeshDraw, commandIndirect), uint32_t(draws.size()), sizeof(MeshDraw));
		}

		vkCmdEndRenderPass(commandBuffer);

		VkImageMemoryBarrier copyBarriers[] =
		{
			imageBarrier(colorTarget.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
			imageBarrier(swapchain.images[imageIndex], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
		};

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, 0, 0, 0, ARRAYSIZE(copyBarriers), copyBarriers);

		VkImageCopy copyRegion = {};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.extent = { swapchain.width, swapchain.height, 1 };

		vkCmdCopyImage(commandBuffer, colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain.images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		VkImageMemoryBarrier presentBarrier = imageBarrier(swapchain.images[imageIndex], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, 0, 0, 0, 1, &presentBarrier);

		vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);

		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		VkPipelineStageFlags submitStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;

		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &acquireSemaphore;
		submitInfo.pWaitDstStageMask = &submitStageMask;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &releaseSemaphore;

		VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &releaseSemaphore;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &swapchain.swapchain;
		presentInfo.pImageIndices = &imageIndex;

		VK_CHECK(vkQueuePresentKHR(queue, &presentInfo));

		VK_CHECK(vkDeviceWaitIdle(device));

		uint64_t queryResults[2];
		VK_CHECK(vkGetQueryPoolResults(device, queryPool, 0, ARRAYSIZE(queryResults), sizeof(queryResults), queryResults, sizeof(queryResults[0]), VK_QUERY_RESULT_64_BIT));

		double frameGpuBegin = double(queryResults[0]) * props.limits.timestampPeriod * 1e-6;
		double frameGpuEnd = double(queryResults[1]) * props.limits.timestampPeriod * 1e-6;

		double frameCpuEnd = glfwGetTime() * 1000;

		frameCpuAvg = frameCpuAvg * 0.95 + (frameCpuEnd - frameCpuBegin) * 0.05;
		frameGpuAvg = frameGpuAvg * 0.95 + (frameGpuEnd - frameGpuBegin) * 0.05;

		double trianglesPerSec = double(drawCount) * double(mesh.indices.size() / 3) / double(frameGpuAvg * 1e-3);
		double kittensPerSec = double(drawCount) / double(frameGpuAvg * 1e-3);

		char title[256];
		sprintf(title, "cpu: %.2f ms; gpu: %.2f ms; triangles %d; meshlets %d; mesh shading %s; %.1fB tri/sec, %.1fM kittens/sec", frameCpuAvg, frameGpuAvg, int(mesh.indices.size() / 3), int(mesh.meshlets.size()), meshShadingSupported && meshShadingEnabled ? "ON" : "OFF", trianglesPerSec * 1e-9, kittensPerSec * 1e-6);
		glfwSetWindowTitle(window, title);
	}

	VK_CHECK(vkDeviceWaitIdle(device));

	if (colorTarget.image)
		destroyImage(colorTarget, device);
	if (depthTarget.image)
		destroyImage(depthTarget, device);
	if (targetFB)
		vkDestroyFramebuffer(device, targetFB, 0);

	destroyBuffer(db, device);

	if (meshShadingSupported)
	{
		destroyBuffer(mb, device);
		destroyBuffer(mdb, device);
	}

	destroyBuffer(ib, device);
	destroyBuffer(vb, device);

	destroyBuffer(scratch, device);

	vkDestroyCommandPool(device, commandPool, 0);

	vkDestroyQueryPool(device, queryPool, 0);

	destroySwapchain(device, swapchain);

	vkDestroyPipeline(device, meshPipeline, 0);
	destroyProgram(device, meshProgram);

	if (meshShadingSupported)
	{
		vkDestroyPipeline(device, meshPipelineMS, 0);
		destroyProgram(device, meshProgramMS);
	}

	vkDestroyShaderModule(device, meshVS.module, 0);
	vkDestroyShaderModule(device, meshFS.module, 0);

	if (meshShadingSupported)
	{
		vkDestroyShaderModule(device, meshletTS.module, 0);
		vkDestroyShaderModule(device, meshletMS.module, 0);
	}

	vkDestroyRenderPass(device, renderPass, 0);

	vkDestroySemaphore(device, releaseSemaphore, 0);
	vkDestroySemaphore(device, acquireSemaphore, 0);

	vkDestroySurfaceKHR(instance, surface, 0);

	glfwDestroyWindow(window);

	vkDestroyDevice(device, 0);

#ifdef _DEBUG
	vkDestroyDebugReportCallbackEXT(instance, debugCallback, 0);
#endif

	vkDestroyInstance(instance, 0);
}
