// niagara.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <assert.h>
#include <stdio.h>
#include <vector>

#define VK_NO_PROTOTYPES
#include <volk/volk.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#define VK_CHECK(call)                 \
	do                                 \
	{                                  \
		VkResult result_ = call;       \
		assert(result_ == VK_SUCCESS); \
	} while (0)

std::vector<const char *> getRequiredExtensions()
{
	std::vector<const char *> extensions {};
	extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
	return extensions;
}

const std::vector<const char *> validationLayers = {
	"VK_LAYER_KHRONOS_validation"};


VkInstance createInstance()
{
	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	auto extensions = getRequiredExtensions();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
	createInfo.ppEnabledLayerNames = validationLayers.data();


	VkInstance instance;
	VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance));

	volkLoadInstance(instance);
	return instance;
}

VkPhysicalDevice pickPhysicalDevice(VkPhysicalDevice *physicalDevices, uint32_t physicalDeviceCount)
{
	for (uint32_t i = 0; i < physicalDeviceCount; ++i)
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physicalDevices[i], &props);

		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			printf("Picking discrete GPU: %s\n", props.deviceName);
			return physicalDevices[i];
		}
	}

	if (physicalDeviceCount > 0)
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physicalDevices[0], &props);

		printf("Picking fallback GPU: %s\n", props.deviceName);
		return physicalDevices[0];
	}

	printf("No physical devices available!");
	return 0;
}

VkDevice createDevice(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t *familyIndex)
{
	*familyIndex = 0; // SHORTCUT: this needs to be computed from queue properties // TODO: this produces a validation error

	float queuePriorities[] = {1.0f};

	VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queueInfo.queueFamilyIndex = *familyIndex;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = queuePriorities;

	const char *extensions[] =
		{
			VK_KHR_SWAPCHAIN_EXTENSION_NAME};

	VkDeviceCreateInfo createInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	createInfo.queueCreateInfoCount = 1;
	createInfo.pQueueCreateInfos = &queueInfo;

	createInfo.ppEnabledExtensionNames = extensions;
	createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);

	VkDevice device = 0;
	VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, 0, &device));

	return device;
}

VkSurfaceKHR createSurface(VkInstance instance, GLFWwindow *window)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
	VkWin32SurfaceCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
	createInfo.hinstance = GetModuleHandle(0);
	createInfo.hwnd = glfwGetWin32Window(window);

	VkSurfaceKHR surface = 0;
	VK_CHECK(vkCreateWin32SurfaceKHR(instance, &createInfo, 0, &surface));
	return surface;
#else
#error Unsupported platform
#endif
}

VkSwapchainKHR createSwapchain(VkDevice device, VkSurfaceKHR surface, uint32_t familyIndex, uint32_t width, uint32_t height)
{
	VkSwapchainCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	createInfo.surface = surface;
	createInfo.minImageCount = 2;
	createInfo.imageFormat = VK_FORMAT_R8G8B8A8_UNORM; // SHORTCUT: some devices only support BGRA
	createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	createInfo.imageExtent.width = width;
	createInfo.imageExtent.height = height;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	createInfo.queueFamilyIndexCount = 1;
	createInfo.pQueueFamilyIndices = &familyIndex;
	createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;

	VkSwapchainKHR swapchain = 0;
	VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, 0, &swapchain));

	return swapchain;
}

VkSemaphore createSemaphore(VkDevice device)
{
	VkSemaphoreCreateInfo createInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

	VkSemaphore semaphore = 0;
	VK_CHECK(vkCreateSemaphore(device, &createInfo, 0, &semaphore));

	return semaphore;
}

VkCommandPool createCommandPool(VkDevice device, uint32_t familyIndex)
{
	VkCommandPoolCreateInfo createInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	createInfo.queueFamilyIndex = familyIndex;

	VkCommandPool commandPool = 0;
	VK_CHECK(vkCreateCommandPool(device, &createInfo, 0, &commandPool));

	return commandPool;
}

int main()
{
	VK_CHECK(volkInitialize());

	int rc = glfwInit();
	assert(rc);

	VkInstance instance = createInstance();
	assert(instance);

	VkPhysicalDevice physicalDevices[16];
	uint32_t physicalDeviceCount = sizeof(physicalDevices) / sizeof(physicalDevices[0]);
	VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices));

	VkPhysicalDevice physicalDevice = pickPhysicalDevice(physicalDevices, physicalDeviceCount);
	assert(physicalDevice);

	uint32_t familyIndex = 0;
	VkDevice device = createDevice(instance, physicalDevice, &familyIndex);
	assert(device);

	GLFWwindow *window = glfwCreateWindow(1024, 768, "niagara", 0, 0);
	assert(window);

	VkSurfaceKHR surface = createSurface(instance, window);
	assert(surface);

	int windowWidth = 0, windowHeight = 0;
	glfwGetWindowSize(window, &windowWidth, &windowHeight);

	VkSwapchainKHR swapchain = createSwapchain(device, surface, familyIndex, windowWidth, windowHeight);
	assert(swapchain);

	VkSemaphore acquireSemaphore = createSemaphore(device);
	assert(acquireSemaphore);

	VkSemaphore releaseSemaphore = createSemaphore(device);
	assert(releaseSemaphore);

	VkQueue queue = 0;
	vkGetDeviceQueue(device, familyIndex, 0, &queue);

	VkImage swapchainImages[16]; // SHORTCUT: seriously?
	uint32_t swapchainImageCount = sizeof(swapchainImages) / sizeof(swapchainImages[0]);
	VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages));

	VkCommandPool commandPool = createCommandPool(device, familyIndex);
	assert(commandPool);

	VkCommandBufferAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocateInfo.commandPool = commandPool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = 0;
	VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer));

	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		uint32_t imageIndex = 0;
		VK_CHECK(vkAcquireNextImageKHR(device, swapchain, ~0ull, acquireSemaphore, VK_NULL_HANDLE, &imageIndex));

		VK_CHECK(vkResetCommandPool(device, commandPool, 0));

		VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

		VkClearColorValue color = {1, 0, 1, 1};

		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;

		vkCmdClearColorImage(commandBuffer, swapchainImages[imageIndex], VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);

		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		VkPipelineStageFlags submitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &acquireSemaphore;
		submitInfo.pWaitDstStageMask = &submitStageMask;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &releaseSemaphore;

		vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);

		VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &releaseSemaphore;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &swapchain;
		presentInfo.pImageIndices = &imageIndex;

		VK_CHECK(vkQueuePresentKHR(queue, &presentInfo));

		VK_CHECK(vkDeviceWaitIdle(device));
	}

	glfwDestroyWindow(window);

	vkDestroyInstance(instance, 0);
}