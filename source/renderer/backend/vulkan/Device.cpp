/* Copyright (C) 2026 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "Device.h"

#include "lib/alignment.h"
#include "lib/build_version.h"
#include "lib/debug.h"
#include "lib/sysdep/os.h"
#include "ps/CLogger.h"
#include "ps/ConfigDB.h"
#include "ps/Profile.h"
#include "ps/containers/StaticVector.h"
#include "renderer/backend/Format.h"
#include "renderer/backend/vulkan/Buffer.h"
#include "renderer/backend/vulkan/DescriptorManager.h"
#include "renderer/backend/vulkan/DeviceCommandContext.h"
#include "renderer/backend/vulkan/DeviceSelection.h"
#include "renderer/backend/vulkan/Framebuffer.h"
#include "renderer/backend/vulkan/Mapping.h"
#include "renderer/backend/vulkan/PipelineState.h"
#include "renderer/backend/vulkan/RenderPassManager.h"
#include "renderer/backend/vulkan/RingCommandContext.h"
#include "renderer/backend/vulkan/SamplerManager.h"
#include "renderer/backend/vulkan/ShaderProgram.h"
#include "renderer/backend/vulkan/SubmitScheduler.h"
#include "renderer/backend/vulkan/SwapChain.h"
#include "renderer/backend/vulkan/Texture.h"
#include "renderer/backend/vulkan/Utilities.h"
#include "renderer/backend/vulkan/XRInterop.h"
#include "scriptinterface/Object.h"
#include "scriptinterface/Request.h"

#include <SDL_version.h>
#include <SDL_video.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <js/RootingAPI.h>
#include <js/Value.h>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <vk_platform.h>

// According to https://wiki.libsdl.org/SDL_Vulkan_LoadLibrary the following
// functionality is supported since SDL 2.0.6.
#if SDL_VERSION_ATLEAST(2, 0, 6)
#include <SDL_vulkan.h>
#endif

namespace Renderer
{

namespace Backend
{

namespace Vulkan
{

namespace
{

constexpr size_t QUERY_POOL_SIZE{NUMBER_OF_FRAMES_IN_FLIGHT * 1024};

std::vector<const char*> GetRequiredSDLExtensions(SDL_Window* window)
{
	if (!window)
		return {};

	const size_t MAX_EXTENSION_COUNT = 16;
	unsigned int SDLExtensionCount = MAX_EXTENSION_COUNT;
	const char* SDLExtensions[MAX_EXTENSION_COUNT];
	ENSURE(SDL_Vulkan_GetInstanceExtensions(window, &SDLExtensionCount, SDLExtensions));
	std::vector<const char*> requiredExtensions;
	requiredExtensions.reserve(SDLExtensionCount);
	std::copy_n(SDLExtensions, SDLExtensionCount, std::back_inserter(requiredExtensions));
	return requiredExtensions;
}

std::vector<std::string> GetAvailableValidationLayers()
{
	uint32_t layerCount = 0;
	ENSURE_VK_SUCCESS(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));

	std::vector<VkLayerProperties> availableLayers(layerCount);
	ENSURE_VK_SUCCESS(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()));

	for (const VkLayerProperties& layer : availableLayers)
	{
		LOGMESSAGE("Vulkan validation layer: '%s' (%s) v%u.%u.%u.%u",
			layer.layerName, layer.description,
			VK_API_VERSION_VARIANT(layer.specVersion),
			VK_API_VERSION_MAJOR(layer.specVersion),
			VK_API_VERSION_MINOR(layer.specVersion),
			VK_API_VERSION_PATCH(layer.specVersion));
	}

	std::vector<std::string> availableValidationLayers;
	availableValidationLayers.reserve(layerCount);
	for (const VkLayerProperties& layer : availableLayers)
		availableValidationLayers.emplace_back(layer.layerName);
	return availableValidationLayers;
}

std::vector<std::string> GetAvailableInstanceExtensions(const char* layerName = nullptr)
{
	uint32_t extensionCount = 0;
	ENSURE_VK_SUCCESS(vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, nullptr));
	std::vector<VkExtensionProperties> extensions(extensionCount);
	ENSURE_VK_SUCCESS(vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, extensions.data()));

	std::vector<std::string> availableExtensions;
	for (const VkExtensionProperties& extension : extensions)
		availableExtensions.emplace_back(extension.extensionName);
	return availableExtensions;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
	void* /*userData*/)
{
	if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) || (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT))
		LOGMESSAGE("Vulkan: %s", callbackData->pMessage);
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		struct HideRule
		{
			VkDebugUtilsMessageTypeFlagsEXT flags;
			std::string_view pattern;
			bool skip;
		};
		constexpr HideRule hideRules[] =
		{
			// Not consumed shader output is a known problem which produces too
			// many warning.
			{VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, "OutputNotConsumed", false},
			// TODO: check vkGetImageMemoryRequirements2 for prefersDedicatedAllocation.
			{VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, "vkBindMemory-small-dedicated-allocation", false},
			{VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, "vkAllocateMemory-small-allocation", false},
			// We have some unnecessary clears which were needed for GL.
			// Ignore message for now, because they're spawned each frame.
			{VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, "ClearCmdBeforeDraw", true},
			{VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, "vkCmdClearAttachments-clear-after-load", true},
			// TODO: investigate probably false-positive report.
			{VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, "vkCmdBeginRenderPass-StoreOpDontCareThenLoadOpLoad", true},
		};
		const auto it = std::find_if(std::begin(hideRules), std::end(hideRules),
			[messageType, message = std::string_view{callbackData->pMessage}](const HideRule& hideRule) -> bool
			{
				return (hideRule.flags & messageType) && message.find(hideRule.pattern) != std::string_view::npos;
			});
		if (it == std::end(hideRules))
			LOGWARNING("Vulkan: %s", callbackData->pMessage);
		else if (!it->skip)
			LOGMESSAGE("Vulkan: %s", callbackData->pMessage);
	}
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		LOGERROR("Vulkan: %s", callbackData->pMessage);

	return VK_FALSE;
}

// A workaround function to meet calling conventions of Vulkan, SDL and GLAD.
GLADapiproc GetInstanceProcAddr(VkInstance instance, const char* name)
{
#if SDL_VERSION_ATLEAST(2, 0, 6)
	PFN_vkGetInstanceProcAddr function = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	PFN_vkVoidFunction result = function(instance, name);
	// Global commands (like vkEnumerateInstanceExtensionProperties) must be
	// queried with a null instance; strict loaders (e.g. Android) return null
	// otherwise.
	if (!result && instance != VK_NULL_HANDLE)
		result = function(VK_NULL_HANDLE, name);
	return reinterpret_cast<GLADapiproc>(result);
#else
	return nullptr;
#endif
}

} // anonymous namespace

// static
std::unique_ptr<CDevice> CDevice::Create(SDL_Window* window)
{
	if (!window)
	{
		LOGERROR("Can't create Vulkan device without window.");
		return nullptr;
	}

	GLADuserptrloadfunc gladLoadFunction = reinterpret_cast<GLADuserptrloadfunc>(GetInstanceProcAddr);

	std::unique_ptr<CDevice> device(new CDevice());
	device->m_Window = window;

#ifdef NDEBUG
	bool enableDebugMessages{g_ConfigDB.Get("renderer.backend.debugmessages", false)};
	bool enableDebugLabels{g_ConfigDB.Get("renderer.backend.debuglabels", false)};
	bool enableDebugScopedLabels{g_ConfigDB.Get("renderer.backend.debugscopedlabels", false)};
#else
	bool enableDebugMessages = true;
	bool enableDebugLabels = true;
	bool enableDebugScopedLabels = true;
#endif

	int gladVulkanVersion = gladLoadVulkanUserPtr(nullptr, gladLoadFunction, nullptr);
	if (!gladVulkanVersion)
	{
		LOGERROR("GLAD unable to load vulkan.");
		return nullptr;
	}

	VkApplicationInfo applicationInfo{};
	applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "0 A.D.";
	applicationInfo.applicationVersion = VK_MAKE_VERSION(PS_VERSION_MAJOR, PS_VERSION_MINOR, PS_VERSION_PATCH);
	applicationInfo.pEngineName = "Pyrogenesis";
	applicationInfo.engineVersion = applicationInfo.applicationVersion;
	applicationInfo.apiVersion = VK_API_VERSION_1_1;

	std::vector<const char*> requiredInstanceExtensions = GetRequiredSDLExtensions(window);
	const XR::Requirements* xrRequirements{XR::GetRequirements()};
	if (xrRequirements)
	{
		for (const std::string& extension : xrRequirements->instanceExtensions)
			if (std::find_if(requiredInstanceExtensions.begin(), requiredInstanceExtensions.end(),
				[&extension](const char* name) { return extension == name; }) == requiredInstanceExtensions.end())
				requiredInstanceExtensions.emplace_back(extension.c_str());
	}

	device->m_ValidationLayers = GetAvailableValidationLayers();
	auto hasValidationLayer = [&layers = device->m_ValidationLayers](const char* name) -> bool
	{
		return std::find(layers.begin(), layers.end(), name) != layers.end();
	};
	device->m_InstanceExtensions = GetAvailableInstanceExtensions();
	auto hasInstanceExtension = [&extensions = device->m_InstanceExtensions](const char* name) -> bool
	{
		return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
	};

#ifdef NDEBUG
	bool enableDebugContext{g_ConfigDB.Get("renderer.backend.debugcontext", false)};
#else
	bool enableDebugContext = true;
#endif

	if (!hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		enableDebugMessages = enableDebugLabels = enableDebugScopedLabels = false;
	const bool enableDebugLayers = enableDebugContext || enableDebugMessages || enableDebugLabels || enableDebugScopedLabels;
	if (enableDebugLayers)
		requiredInstanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	std::vector<const char*> requestedValidationLayers;
	const bool enableValidationFeatures = enableDebugMessages && hasValidationLayer("VK_LAYER_KHRONOS_validation");
	if (enableValidationFeatures)
		requestedValidationLayers.emplace_back("VK_LAYER_KHRONOS_validation");

	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/master/docs/synchronization_usage.md
	VkValidationFeatureEnableEXT validationFeatureEnables[] =
	{
		VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
	};
	VkValidationFeaturesEXT	validationFeatures{};
	validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validationFeatures.enabledValidationFeatureCount = std::size(validationFeatureEnables);
	validationFeatures.pEnabledValidationFeatures = validationFeatureEnables;

	VkInstanceCreateInfo instanceCreateInfo{};
	instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceCreateInfo.pApplicationInfo = &applicationInfo;
	instanceCreateInfo.enabledExtensionCount = requiredInstanceExtensions.size();
	instanceCreateInfo.ppEnabledExtensionNames = requiredInstanceExtensions.data();
	if (requestedValidationLayers.empty())
	{
		instanceCreateInfo.enabledLayerCount = 0;
		instanceCreateInfo.ppEnabledLayerNames = nullptr;
	}
	else
	{
		instanceCreateInfo.enabledLayerCount = requestedValidationLayers.size();
		instanceCreateInfo.ppEnabledLayerNames = requestedValidationLayers.data();
	}

	// Enabling validation features might significantly reduce performance,
	// even more than the standard validation layer.
	if (enableValidationFeatures && enableDebugContext)
	{
		instanceCreateInfo.pNext = &validationFeatures;
	}

	const VkResult createInstanceResult = vkCreateInstance(&instanceCreateInfo, nullptr, &device->m_Instance);
	if (createInstanceResult != VK_SUCCESS)
	{
		if (createInstanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
			LOGERROR("Can't create Vulkan instance: incompatible driver.");
		else if (createInstanceResult == VK_ERROR_EXTENSION_NOT_PRESENT)
			LOGERROR("Can't create Vulkan instance: extension not present.");
		else if (createInstanceResult == VK_ERROR_LAYER_NOT_PRESENT)
			LOGERROR("Can't create Vulkan instance: layer not present.");
		else
			LOGERROR("Unknown error during Vulkan instance creation: %d (%s)",
				static_cast<int>(createInstanceResult), Utilities::GetVkResultName(createInstanceResult));
		return nullptr;
	}

	gladVulkanVersion = gladLoadVulkanUserPtr(nullptr, gladLoadFunction, device->m_Instance);
	if (!gladVulkanVersion)
	{
		LOGERROR("GLAD unable to re-load vulkan after its instance creation.");
		return nullptr;
	}

	if (GLAD_VK_EXT_debug_utils && enableDebugMessages)
	{
		VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
		debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debugCreateInfo.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debugCreateInfo.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debugCreateInfo.pfnUserCallback = DebugCallback;
		debugCreateInfo.pUserData = nullptr;

		ENSURE_VK_SUCCESS(vkCreateDebugUtilsMessengerEXT(
			device->m_Instance, &debugCreateInfo, nullptr, &device->m_DebugMessenger));
	}

	if (window)
		ENSURE(SDL_Vulkan_CreateSurface(window, device->m_Instance, &device->m_Surface));

	const std::vector<const char*> requiredDeviceExtensions =
	{
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};
	std::vector<SAvailablePhysicalDevice> availablePhyscialDevices =
		GetAvailablePhysicalDevices(device->m_Instance, device->m_Surface, requiredDeviceExtensions);
	for (const SAvailablePhysicalDevice& device : availablePhyscialDevices)
	{
		LOGMESSAGE("Vulkan available device: '%s' Type: %u Supported: %c",
			device.properties.deviceName, static_cast<uint32_t>(device.properties.deviceType),
			IsPhysicalDeviceUnsupported(device) ? 'N' : 'Y');
		LOGMESSAGE("  ID: %u VendorID: %u API Version: %u Driver Version: %u",
			device.properties.deviceID, device.properties.vendorID,
			device.properties.apiVersion, device.properties.driverVersion);
		LOGMESSAGE("  hasRequiredExtensions: %c hasOutputToSurfaceSupport: %c",
			device.hasRequiredExtensions ? 'Y' : 'N', device.hasOutputToSurfaceSupport ? 'Y' : 'N');
		LOGMESSAGE("  graphicsQueueFamilyIndex: %u presentQueueFamilyIndex: %u families: %zu",
			device.graphicsQueueFamilyIndex, device.presentQueueFamilyIndex,
			device.queueFamilies.size());
		LOGMESSAGE("  maxBoundDescriptorSets: %u", device.properties.limits.maxBoundDescriptorSets);
		for (const VkSurfaceFormatKHR& surfaceFormat : device.surfaceFormats)
		{
			LOGMESSAGE("  Surface format: %u colorSpace: %u Supported: %c",
				static_cast<uint32_t>(surfaceFormat.format),
				static_cast<uint32_t>(surfaceFormat.colorSpace),
				IsSurfaceFormatSupported(surfaceFormat) ? 'Y' : 'N');
		}
		for (uint32_t memoryTypeIndex = 0; memoryTypeIndex < device.memoryProperties.memoryTypeCount; ++memoryTypeIndex)
		{
			const VkMemoryType& type = device.memoryProperties.memoryTypes[memoryTypeIndex];
			LOGMESSAGE("  Memory Type Index: %u Flags: %u Heap Index: %u",
				memoryTypeIndex, static_cast<uint32_t>(type.propertyFlags), type.heapIndex);
		}
		for (uint32_t memoryHeapIndex = 0; memoryHeapIndex < device.memoryProperties.memoryHeapCount; ++memoryHeapIndex)
		{
			const VkMemoryHeap& heap = device.memoryProperties.memoryHeaps[memoryHeapIndex];
			LOGMESSAGE("  Memory Heap Index: %u Size: %zu Flags: %u",
				memoryHeapIndex, static_cast<size_t>(heap.size / 1024), static_cast<uint32_t>(heap.flags));
		}
	}
	device->m_AvailablePhysicalDevices = availablePhyscialDevices;
	// We need to remove unsupported devices first.
	availablePhyscialDevices.erase(
		std::remove_if(
			availablePhyscialDevices.begin(), availablePhyscialDevices.end(),
			IsPhysicalDeviceUnsupported),
		availablePhyscialDevices.end());
	if (availablePhyscialDevices.empty())
	{
		LOGERROR("Vulkan can not find any supported and suitable device.");
		return nullptr;
	}

	const int deviceIndexOverride{g_ConfigDB.Get("renderer.backend.vulkan.deviceindexoverride", -1)};
	auto chosenDeviceIt = device->m_AvailablePhysicalDevices.end();
	if (deviceIndexOverride >= 0)
	{
		chosenDeviceIt = std::find_if(
			device->m_AvailablePhysicalDevices.begin(), device->m_AvailablePhysicalDevices.end(),
			[deviceIndexOverride](const SAvailablePhysicalDevice& availableDevice)
			{
				return availableDevice.index == static_cast<uint32_t>(deviceIndexOverride);
			});
		if (chosenDeviceIt == device->m_AvailablePhysicalDevices.end())
			LOGWARNING("Device with override index %d not found.", deviceIndexOverride);
	}
	if (chosenDeviceIt == device->m_AvailablePhysicalDevices.end() && xrRequirements &&
		xrRequirements->choosePhysicalDevice)
	{
		// OpenXR requires rendering on the device the headset runtime uses.
		const VkPhysicalDevice xrPhysicalDevice{reinterpret_cast<VkPhysicalDevice>(
			xrRequirements->choosePhysicalDevice(reinterpret_cast<void*>(device->m_Instance)))};
		chosenDeviceIt = std::find_if(
			device->m_AvailablePhysicalDevices.begin(), device->m_AvailablePhysicalDevices.end(),
			[xrPhysicalDevice](const SAvailablePhysicalDevice& availableDevice)
			{
				return availableDevice.device == xrPhysicalDevice;
			});
		if (chosenDeviceIt == device->m_AvailablePhysicalDevices.end())
			LOGWARNING("Vulkan device required by OpenXR not found.");
	}
	if (chosenDeviceIt == device->m_AvailablePhysicalDevices.end())
	{
		const bool chooseBestDevice{g_ConfigDB.Get("renderer.backend.vulkan.choosebestdevice", false)};
		if (chooseBestDevice)
		{
			// We need to choose the best available device fits our needs.
			chosenDeviceIt = min_element(
				availablePhyscialDevices.begin(), availablePhyscialDevices.end(),
				ComparePhysicalDevices);
		}
		else
		{
			// By default we assume that the Vulkan driver provides a decent default.
			chosenDeviceIt = availablePhyscialDevices.begin();
		}
	}
	device->m_ChoosenDevice = *chosenDeviceIt;
	const SAvailablePhysicalDevice& choosenDevice = device->m_ChoosenDevice;
	device->m_AvailablePhysicalDevices.erase(std::remove_if(
		device->m_AvailablePhysicalDevices.begin(), device->m_AvailablePhysicalDevices.end(),
		[physicalDevice = choosenDevice.device](const SAvailablePhysicalDevice& device)
		{
			return physicalDevice == device.device;
		}), device->m_AvailablePhysicalDevices.end());

	gladVulkanVersion = gladLoadVulkanUserPtr(choosenDevice.device, gladLoadFunction, device->m_Instance);
	if (!gladVulkanVersion)
	{
		LOGERROR("GLAD unable to re-load vulkan after choosing its physical device.");
		return nullptr;
	}

#if !OS_MACOSX
	auto hasDeviceExtension = [&extensions = choosenDevice.extensions](const char* name) -> bool
	{
		return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
	};
	const bool hasDescriptorIndexing = hasDeviceExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
#else
	// Metal on macOS doesn't support combined samplers natively. Currently
	// they break compiling SPIR-V shaders with descriptor indexing into MTL
	// shaders when using MoltenVK.
	const bool hasDescriptorIndexing = false;
#endif
	const bool hasNeededDescriptorIndexingFeatures =
		hasDescriptorIndexing &&
		choosenDevice.descriptorIndexingProperties.maxUpdateAfterBindDescriptorsInAllPools >= 65536 &&
		choosenDevice.descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing &&
		choosenDevice.descriptorIndexingFeatures.runtimeDescriptorArray &&
		choosenDevice.descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount &&
		choosenDevice.descriptorIndexingFeatures.descriptorBindingPartiallyBound &&
		choosenDevice.descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending &&
		choosenDevice.descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind;

	std::vector<const char*> deviceExtensions = requiredDeviceExtensions;
	if (hasDescriptorIndexing)
		deviceExtensions.emplace_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
	if (xrRequirements)
	{
		for (const std::string& extension : xrRequirements->deviceExtensions)
			if (std::find_if(deviceExtensions.begin(), deviceExtensions.end(),
				[&extension](const char* name) { return extension == name; }) == deviceExtensions.end())
				deviceExtensions.emplace_back(extension.c_str());
	}

	device->m_GraphicsQueueFamilyIndex = choosenDevice.graphicsQueueFamilyIndex;
	const std::array<size_t, 1> queueFamilyIndices{{
		choosenDevice.graphicsQueueFamilyIndex
	}};

	PS::StaticVector<VkDeviceQueueCreateInfo, 1> queueCreateInfos;
	const float queuePriority = 1.0f;
	std::transform(queueFamilyIndices.begin(), queueFamilyIndices.end(),
		std::back_inserter(queueCreateInfos),
		[&queuePriority](const size_t queueFamilyIndex)
		{
			VkDeviceQueueCreateInfo queueCreateInfo{};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
			return queueCreateInfo;
		});

	// https://github.com/KhronosGroup/Vulkan-Guide/blob/master/chapters/enabling_features.adoc
	VkPhysicalDeviceFeatures deviceFeatures{};
	VkPhysicalDeviceFeatures2 deviceFeatures2{};
	VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexingFeatures{};

	deviceFeatures.textureCompressionBC = choosenDevice.features.textureCompressionBC;
	deviceFeatures.samplerAnisotropy = choosenDevice.features.samplerAnisotropy;
	deviceFeatures.fillModeNonSolid = choosenDevice.features.fillModeNonSolid;

	descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
	descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing =
		choosenDevice.descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing;
	descriptorIndexingFeatures.runtimeDescriptorArray =
		choosenDevice.descriptorIndexingFeatures.runtimeDescriptorArray;
	descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount =
		choosenDevice.descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount;
	descriptorIndexingFeatures.descriptorBindingPartiallyBound =
		choosenDevice.descriptorIndexingFeatures.descriptorBindingPartiallyBound;
	descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending =
		choosenDevice.descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending;
	descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind =
		choosenDevice.descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind;

	deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	deviceFeatures2.features = deviceFeatures;
	if (hasNeededDescriptorIndexingFeatures)
		deviceFeatures2.pNext = &descriptorIndexingFeatures;

	VkDeviceCreateInfo deviceCreateInfo{};
	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.enabledExtensionCount = deviceExtensions.size();
	deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();
	deviceCreateInfo.pEnabledFeatures = nullptr;
	deviceCreateInfo.pNext = &deviceFeatures2;
	deviceCreateInfo.enabledLayerCount = 0;
	deviceCreateInfo.ppEnabledLayerNames = nullptr;

	const VkResult createDeviceResult = vkCreateDevice(
		choosenDevice.device, &deviceCreateInfo, nullptr, &device->m_Device);
	if (createDeviceResult != VK_SUCCESS)
	{
		if (createDeviceResult == VK_ERROR_FEATURE_NOT_PRESENT)
			LOGERROR("Can't create Vulkan device: feature not present.");
		else if (createDeviceResult == VK_ERROR_EXTENSION_NOT_PRESENT)
			LOGERROR("Can't create Vulkan device: extension not present.");
		else
			LOGERROR("Unknown error during Vulkan device creation: %d (%s)",
				static_cast<int>(createDeviceResult), Utilities::GetVkResultName(createDeviceResult));
		return nullptr;
	}

	VmaVulkanFunctions vulkanFunctions{};
	vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	vulkanFunctions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
	vulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
	vulkanFunctions.vkAllocateMemory = vkAllocateMemory;
	vulkanFunctions.vkFreeMemory = vkFreeMemory;
	vulkanFunctions.vkMapMemory = vkMapMemory;
	vulkanFunctions.vkUnmapMemory = vkUnmapMemory;
	vulkanFunctions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
	vulkanFunctions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
	vulkanFunctions.vkBindBufferMemory = vkBindBufferMemory;
	vulkanFunctions.vkBindImageMemory = vkBindImageMemory;
	vulkanFunctions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
	vulkanFunctions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
	vulkanFunctions.vkCreateBuffer = vkCreateBuffer;
	vulkanFunctions.vkDestroyBuffer = vkDestroyBuffer;
	vulkanFunctions.vkCreateImage = vkCreateImage;
	vulkanFunctions.vkDestroyImage = vkDestroyImage;
	vulkanFunctions.vkCmdCopyBuffer = vkCmdCopyBuffer;
	// Functions promoted to Vulkan 1.1.
	vulkanFunctions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
	vulkanFunctions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
	vulkanFunctions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
	vulkanFunctions.vkBindImageMemory2KHR = vkBindImageMemory2;
	vulkanFunctions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;

	VmaAllocatorCreateInfo allocatorCreateInfo{};
	allocatorCreateInfo.instance = device->m_Instance;
	allocatorCreateInfo.physicalDevice = choosenDevice.device;
	allocatorCreateInfo.device = device->m_Device;
	allocatorCreateInfo.vulkanApiVersion = applicationInfo.apiVersion;
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
	const VkResult createVMAAllocatorResult =
		vmaCreateAllocator(&allocatorCreateInfo, &device->m_VMAAllocator);
	if (createVMAAllocatorResult != VK_SUCCESS)
	{
		LOGERROR("Failed to create VMA allocator: %d (%s)",
			static_cast<int>(createVMAAllocatorResult), Utilities::GetVkResultName(createVMAAllocatorResult));
		return nullptr;
	}

	// We need to use VK_SHARING_MODE_CONCURRENT if we have graphics and present
	// in different queues.
	vkGetDeviceQueue(device->m_Device, choosenDevice.graphicsQueueFamilyIndex,
		0, &device->m_GraphicsQueue);
	ENSURE(device->m_GraphicsQueue != VK_NULL_HANDLE);

	Capabilities& capabilities = device->m_Capabilities;

	capabilities.debugLabels = enableDebugLabels;
	capabilities.debugScopedLabels = enableDebugScopedLabels;
	capabilities.S3TC = choosenDevice.features.textureCompressionBC;
	capabilities.computeShaders = true;
	capabilities.storage = choosenDevice.properties.limits.maxStorageBufferRange >= GiB;
	capabilities.instancing = true;
	capabilities.maxSampleCount = 1;
	const VkSampleCountFlags sampleCountFlags =
		choosenDevice.properties.limits.framebufferColorSampleCounts
		& choosenDevice.properties.limits.framebufferDepthSampleCounts
		& choosenDevice.properties.limits.framebufferStencilSampleCounts;
	const std::array<VkSampleCountFlagBits, 5> allowedSampleCountBits =
	{
		VK_SAMPLE_COUNT_1_BIT,
		VK_SAMPLE_COUNT_2_BIT,
		VK_SAMPLE_COUNT_4_BIT,
		VK_SAMPLE_COUNT_8_BIT,
		VK_SAMPLE_COUNT_16_BIT,
	};
	for (size_t index = 0; index < allowedSampleCountBits.size(); ++index)
		if (sampleCountFlags & allowedSampleCountBits[index])
			device->m_Capabilities.maxSampleCount = 1u << index;
	capabilities.multisampling = device->m_Capabilities.maxSampleCount > 1;
	capabilities.anisotropicFiltering = choosenDevice.features.samplerAnisotropy;
	capabilities.maxAnisotropy = choosenDevice.properties.limits.maxSamplerAnisotropy;
	capabilities.maxTextureSize =
		choosenDevice.properties.limits.maxImageDimension2D;
	capabilities.timestamps =
		choosenDevice.properties.limits.timestampComputeAndGraphics
		&& choosenDevice.properties.limits.timestampPeriod > 0;
	if (capabilities.timestamps)
	{
		capabilities.timestampMultiplier =
			device->m_ChoosenDevice.properties.limits.timestampPeriod / 1e9;
	}

	device->m_RenderPassManager =
		std::make_unique<CRenderPassManager>(device.get());
	device->m_SamplerManager = std::make_unique<CSamplerManager>(device.get());

	device->m_SubmitScheduler = CSubmitScheduler::Create(
		device.get(), device->m_GraphicsQueue);
	if (!device->m_SubmitScheduler)
		return nullptr;

	const bool useDescriptorIndexing{hasNeededDescriptorIndexingFeatures &&
		!g_ConfigDB.Get("renderer.backend.vulkan.disabledescriptorindexing", false)};
	device->m_DescriptorManager =
		std::make_unique<CDescriptorManager>(device.get(), useDescriptorIndexing);

	device->m_Name = choosenDevice.properties.deviceName;
	device->m_Version =
		std::to_string(VK_API_VERSION_VARIANT(choosenDevice.properties.apiVersion)) +
		"." + std::to_string(VK_API_VERSION_MAJOR(choosenDevice.properties.apiVersion)) +
		"." + std::to_string(VK_API_VERSION_MINOR(choosenDevice.properties.apiVersion)) +
		"." + std::to_string(VK_API_VERSION_PATCH(choosenDevice.properties.apiVersion));

	device->m_DriverInformation = std::to_string(choosenDevice.properties.driverVersion);

	// Refs:
	// * https://www.khronos.org/registry/vulkan/specs/1.3-extensions/man/html/VkPhysicalDeviceProperties.html
	// * https://pcisig.com/membership/member-companies
	device->m_VendorID = std::to_string(choosenDevice.properties.vendorID);

	device->m_Extensions = choosenDevice.extensions;

	if (device->m_Capabilities.timestamps)
	{
		VkQueryPoolCreateInfo queryPoolInfo{};
		queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryPoolInfo.queryCount = QUERY_POOL_SIZE;

		ENSURE_VK_SUCCESS(vkCreateQueryPool(
			device->GetVkDevice(), &queryPoolInfo, nullptr, &device->m_QueryPool));

		device->m_Queries.resize(QUERY_POOL_SIZE);
	}

	return device;
}

CDevice::CDevice() = default;

CDevice::~CDevice()
{
	// The order of destroying does matter to avoid use-after-free and validation
	// layers complaints.

	m_SubmitScheduler.reset();

	if (m_QueryPool)
		vkDestroyQueryPool(GetVkDevice(), m_QueryPool, nullptr);

	ProcessDeviceObjectToDestroyQueue(true);

	m_RenderPassManager.reset();
	m_SamplerManager.reset();
	m_DescriptorManager.reset();

	ProcessObjectToDestroyQueue(true);

	if (m_VMAAllocator != VK_NULL_HANDLE)
		vmaDestroyAllocator(m_VMAAllocator);

	if (m_Device != VK_NULL_HANDLE)
		vkDestroyDevice(m_Device, nullptr);

	if (m_Surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);

	if (GLAD_VK_EXT_debug_utils && m_DebugMessenger)
		vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);

	if (m_Instance != VK_NULL_HANDLE)
		vkDestroyInstance(m_Instance, nullptr);
}

void CDevice::Report(const Script::Request& rq, JS::HandleValue settings)
{
	Script::SetProperty(rq, settings, "name", "vulkan");

	Script::SetProperty(rq, settings, "extensions", m_Extensions);

	JS::RootedValue device(rq.cx);
	Script::CreateObject(rq, &device);
	ReportAvailablePhysicalDevice(m_ChoosenDevice, rq, device);
	Script::SetProperty(rq, settings, "choosen_device", device);

	JS::RootedValueVector availableDevices{rq.cx};
	if (!availableDevices.reserve(m_AvailablePhysicalDevices.size()))
		throw std::runtime_error{"Reserve failed"};
	for (const SAvailablePhysicalDevice& d : m_AvailablePhysicalDevices)
	{
		JS::RootedValue device(rq.cx);
		Script::CreateObject(rq, &device);
		ReportAvailablePhysicalDevice(d, rq, device);
		if (!availableDevices.append(device))
			throw std::runtime_error{"Append failed"};
	}
	Script::SetProperty(rq, settings, "available_devices",
		JS::RootedValue{rq.cx, JS::ObjectValue(*JS::NewArrayObject(rq.cx, availableDevices))});

	Script::SetProperty(rq, settings, "instance_extensions", m_InstanceExtensions);
	Script::SetProperty(rq, settings, "validation_layers", m_ValidationLayers);
}

std::unique_ptr<ISwapChain> CDevice::CreateSwapChain(
	const char* name, SDL_Window* window,
	int surfaceDrawableWidth, int surfaceDrawableHeight,
	const bool vsync, std::unique_ptr<ISwapChain> oldSwapChain)
{
	ENSURE(window == m_Window);
	ENSURE(m_Window);
	if (window)
		SDL_Vulkan_GetDrawableSize(window, &surfaceDrawableWidth, &surfaceDrawableHeight);
	return CSwapChain::Create(
		this, m_SubmitScheduler.get(),
		m_GraphicsQueueFamilyIndex, m_GraphicsQueue,
		name, m_Surface,
		surfaceDrawableWidth, surfaceDrawableHeight,
		vsync, std::move(oldSwapChain));
}

void CDevice::WaitUntilIdle()
{
	vkDeviceWaitIdle(m_Device);

	// Since we know there is no GPU work in progress we can free resources
	// queued for deletion.
	ProcessDeviceObjectToDestroyQueue(true);
	ProcessObjectToDestroyQueue(true);
}

std::unique_ptr<IGraphicsPipelineState> CDevice::CreateGraphicsPipelineState(
	const SGraphicsPipelineStateDesc& pipelineStateDesc)
{
	return CGraphicsPipelineState::Create(this, pipelineStateDesc);
}

std::unique_ptr<IComputePipelineState> CDevice::CreateComputePipelineState(
	const SComputePipelineStateDesc& pipelineStateDesc)
{
	return CComputePipelineState::Create(this, pipelineStateDesc);
}

std::unique_ptr<IVertexInputLayout> CDevice::CreateVertexInputLayout(
	const std::span<const SVertexAttributeFormat> attributes)
{
	return std::make_unique<CVertexInputLayout>(this, attributes);
}

std::unique_ptr<ITexture> CDevice::CreateTexture(
	const char* name, const ITexture::Type type, const uint32_t usage,
	const Format format, const uint32_t width, const uint32_t height,
	const Sampler::Desc& defaultSamplerDesc, const uint32_t MIPLevelCount, const uint32_t sampleCount)
{
	return CTexture::Create(
		this, name, type, usage, format, width, height,
		defaultSamplerDesc, MIPLevelCount, sampleCount);
}

std::unique_ptr<ITexture> CDevice::CreateTexture2D(
	const char* name, const uint32_t usage,
	const Format format, const uint32_t width, const uint32_t height,
	const Sampler::Desc& defaultSamplerDesc, const uint32_t MIPLevelCount, const uint32_t sampleCount)
{
	return CreateTexture(
		name, ITexture::Type::TEXTURE_2D, usage, format,
		width, height, defaultSamplerDesc, MIPLevelCount, sampleCount);
}

std::unique_ptr<IFramebuffer> CDevice::CreateFramebuffer(
	const char* name, SColorAttachment* colorAttachment,
	SDepthStencilAttachment* depthStencilAttachment)
{
	return CFramebuffer::Create(
		this, name, colorAttachment, depthStencilAttachment);
}

std::unique_ptr<IBuffer> CDevice::CreateBuffer(
	const char* name, const IBuffer::Type type, const uint32_t size, const uint32_t usage)
{
	return CreateCBuffer(name, type, size, usage);
}

std::unique_ptr<CBuffer> CDevice::CreateCBuffer(
	const char* name, const IBuffer::Type type, const uint32_t size, const uint32_t usage)
{
	return CBuffer::Create(this, name, type, size, usage);
}

std::unique_ptr<IShaderProgram> CDevice::CreateShaderProgram(
	const CStr& name, const CShaderDefines& defines)
{
	return CShaderProgram::Create(this, name, defines);
}

std::unique_ptr<IDeviceCommandContext> CDevice::CreateCommandContext()
{
	return CDeviceCommandContext::Create(this);
}

void CDevice::OnPresent()
{
	ProcessObjectToDestroyQueue();
	ProcessDeviceObjectToDestroyQueue();

	++m_FrameID;
}

void CDevice::OnSwapChainRecreation()
{
	// We're sure that a client must call WaitUntilIdle to be able to
	// recreate a swapchain.
	ProcessObjectToDestroyQueue(true);
	ProcessDeviceObjectToDestroyQueue(true);
}

bool CDevice::IsTextureFormatSupported(const Format format) const
{
	switch (format)
	{
	case Format::UNDEFINED:
		return false;

	case Format::R8G8B8_UNORM:
		return false;

	case Format::BC1_RGB_UNORM:
	case Format::BC1_RGB_SRGB:
	case Format::BC1_RGBA_UNORM:
	case Format::BC1_RGBA_SRGB:
	case Format::BC2_UNORM:
	case Format::BC2_SRGB:
	case Format::BC3_UNORM:
	case Format::BC3_SRGB:
		if (m_Capabilities.S3TC)
			return true;
		else
			break;

	default:
		break;
	}

	VkFormatProperties formatProperties{};
	vkGetPhysicalDeviceFormatProperties(
		m_ChoosenDevice.device, Mapping::FromFormat(format), &formatProperties);
	return formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}

bool CDevice::IsFramebufferFormatSupported(const Format format) const
{
	VkFormatProperties formatProperties{};
	vkGetPhysicalDeviceFormatProperties(
		m_ChoosenDevice.device, Mapping::FromFormat(format), &formatProperties);
	if (IsDepthFormat(format))
		return formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	return formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}

Format CDevice::GetPreferredDepthStencilFormat(
	const uint32_t usage, const bool depth, const bool stencil) const
{
	ENSURE(depth || stencil);
	Format format = Format::UNDEFINED;
	if (stencil)
	{
		// https://github.com/KhronosGroup/Vulkan-Guide/blob/main/chapters/depth.adoc#depth-formats
		// At least one of VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT
		// must also be supported.
		if (IsFormatSupportedForUsage(Format::D24_UNORM_S8_UINT, usage))
			format = Format::D24_UNORM_S8_UINT;
		else
			format = Format::D32_SFLOAT_S8_UINT;
	}
	else
	{
		std::array<Format, 3> formatRequestOrder;
		// TODO: add most known vendors to enum.
		// https://developer.nvidia.com/blog/vulkan-dos-donts/
		if (m_ChoosenDevice.properties.vendorID == 0x10DE)
			formatRequestOrder = {Format::D24_UNORM, Format::D32_SFLOAT, Format::D16_UNORM};
		else
			formatRequestOrder = {Format::D32_SFLOAT, Format::D24_UNORM, Format::D16_UNORM};
		for (const Format formatRequest : formatRequestOrder)
			if (IsFormatSupportedForUsage(formatRequest, usage))
			{
				format = formatRequest;
				break;
			}
	}
	return format;
}

uint32_t CDevice::AllocateQuery()
{
	ENSURE(m_Capabilities.timestamps);
	auto it = std::find_if(
		m_Queries.begin(), m_Queries.end(), [](const CDevice::Query& query)
		{
			return !query.occupied;
		});
	ENSURE(it != m_Queries.end());
	it->occupied = true;
	return std::distance(m_Queries.begin(), it);
}

void CDevice::FreeQuery(const uint32_t handle)
{
	ENSURE(handle < m_Queries.size());
	ENSURE(m_Queries[handle].occupied);
	m_Queries[handle].occupied = false;
}

bool CDevice::IsQueryResultAvailable(const uint32_t handle) const
{
	ENSURE(handle < m_Queries.size());
	const Query& query{m_Queries[handle]};
	ENSURE(query.occupied);
	ENSURE(query.submitted);
	if (query.lastUsageFrameID + NUMBER_OF_FRAMES_IN_FLIGHT > m_FrameID)
		return false;
	uint64_t data{};
	return vkGetQueryPoolResults(
		GetVkDevice(), m_QueryPool, handle, 1, sizeof(data), &data, 0, VK_QUERY_RESULT_64_BIT) != VK_NOT_READY;
}

uint64_t CDevice::GetQueryResult(const uint32_t handle)
{
	ENSURE(handle < m_Queries.size());
	Query& query{m_Queries[handle]};
	ENSURE(query.occupied);
	ENSURE(query.submitted);
	uint64_t data{};
	ENSURE_VK_SUCCESS(vkGetQueryPoolResults(
		GetVkDevice(), m_QueryPool, handle, 1, sizeof(data), &data, 0, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	query.submitted = false;
	return data;
}

void CDevice::CollectStatistics(StatisticsVector& statistics) const
{
	VmaBudget heapBudgets[VK_MAX_MEMORY_HEAPS];
	vmaGetHeapBudgets(m_VMAAllocator, heapBudgets);

	VmaStatistics totalStatistics{};
	for (uint32_t index{0}; index < m_ChoosenDevice.memoryProperties.memoryHeapCount; ++index)
	{
		totalStatistics.blockCount += heapBudgets[index].statistics.blockCount;
		totalStatistics.allocationCount += heapBudgets[index].statistics.allocationCount;
		totalStatistics.blockBytes += heapBudgets[index].statistics.blockBytes;
		totalStatistics.allocationBytes += heapBudgets[index].statistics.allocationBytes;
	}

	statistics.emplace_back("VMA total blockCount", "", totalStatistics.blockCount);
	statistics.emplace_back("VMA total allocationCount", "", totalStatistics.allocationCount);
	statistics.emplace_back("VMA total blockBytes", "MiB", static_cast<uint32_t>(totalStatistics.blockBytes / MiB));
	statistics.emplace_back("VMA total allocationBytes", "MiB", static_cast<uint32_t>(totalStatistics.allocationBytes / MiB));

	m_DescriptorManager->CollectStatistics(statistics);
	m_SamplerManager->CollectStatistics(statistics);
	m_RenderPassManager->CollectStatistics(statistics);
}

bool CDevice::IsFormatSupportedForUsage(const Format format, const uint32_t usage) const
{
	VkFormatProperties formatProperties{};
	vkGetPhysicalDeviceFormatProperties(
		m_ChoosenDevice.device, Mapping::FromFormat(format), &formatProperties);
	VkFormatFeatureFlags expectedFeatures = 0;
	if (usage & ITexture::Usage::COLOR_ATTACHMENT)
		expectedFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if (usage & ITexture::Usage::DEPTH_STENCIL_ATTACHMENT)
		expectedFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (usage & ITexture::Usage::SAMPLED)
		expectedFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if (usage & ITexture::Usage::TRANSFER_SRC)
		expectedFeatures |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if (usage & ITexture::Usage::TRANSFER_DST)
		expectedFeatures |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	return (formatProperties.optimalTilingFeatures & expectedFeatures) == expectedFeatures;
}

void CDevice::InsertTimestampQuery(VkCommandBuffer commandBuffer, const uint32_t handle, const bool isScopeBegin)
{
	ENSURE(handle < m_Queries.size());
	vkCmdResetQueryPool(
		commandBuffer, m_QueryPool, handle, 1);
	vkCmdWriteTimestamp(
		commandBuffer, isScopeBegin ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool, handle);
	m_Queries[handle].lastUsageFrameID = m_FrameID;
	m_Queries[handle].submitted = true;
}

void CDevice::ScheduleObjectToDestroy(
	VkObjectType type, const uint64_t handle, const VmaAllocation allocation)
{
	m_ObjectToDestroyQueue.push({m_FrameID, type, handle, allocation});
}

void CDevice::ScheduleTextureToDestroy(const DeviceObjectUID uid)
{
	m_TextureToDestroyQueue.push({m_FrameID, uid});
}

void CDevice::ScheduleBufferToDestroy(const DeviceObjectUID uid)
{
	m_BufferToDestroyQueue.push({m_FrameID, uid});
}

void CDevice::SetObjectName(VkObjectType type, const uint64_t handle, const char* name)
{
	if (!m_Capabilities.debugLabels)
		return;
	VkDebugUtilsObjectNameInfoEXT nameInfo{};
	nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	nameInfo.objectType = type;
	nameInfo.objectHandle = handle;
	nameInfo.pObjectName = name;
	vkSetDebugUtilsObjectNameEXT(m_Device, &nameInfo);
}

std::unique_ptr<CRingCommandContext> CDevice::CreateRingCommandContext(const size_t size)
{
	return CRingCommandContext::Create(
		this, size, m_GraphicsQueueFamilyIndex, *m_SubmitScheduler);
}

void CDevice::ProcessObjectToDestroyQueue(const bool ignoreFrameID)
{
	while (!m_ObjectToDestroyQueue.empty() &&
		(ignoreFrameID || m_ObjectToDestroyQueue.front().frameID + NUMBER_OF_FRAMES_IN_FLIGHT < m_FrameID))
	{
		ObjectToDestroy& object = m_ObjectToDestroyQueue.front();
#if VK_USE_64_BIT_PTR_DEFINES
		void* handle = reinterpret_cast<void*>(object.handle);
#else
		const uint64_t handle = object.handle;
#endif
		switch (object.type)
		{
		case VK_OBJECT_TYPE_IMAGE:
			vmaDestroyImage(GetVMAAllocator(), static_cast<VkImage>(handle), object.allocation);
			break;
		case VK_OBJECT_TYPE_BUFFER:
			vmaDestroyBuffer(GetVMAAllocator(), static_cast<VkBuffer>(handle), object.allocation);
			break;
		case VK_OBJECT_TYPE_IMAGE_VIEW:
			vkDestroyImageView(m_Device, static_cast<VkImageView>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_BUFFER_VIEW:
			vkDestroyBufferView(m_Device, static_cast<VkBufferView>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_FRAMEBUFFER:
			vkDestroyFramebuffer(m_Device, static_cast<VkFramebuffer>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_RENDER_PASS:
			vkDestroyRenderPass(m_Device, static_cast<VkRenderPass>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_SAMPLER:
			vkDestroySampler(m_Device, static_cast<VkSampler>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_SHADER_MODULE:
			vkDestroyShaderModule(m_Device, static_cast<VkShaderModule>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
			vkDestroyPipelineLayout(m_Device, static_cast<VkPipelineLayout>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_PIPELINE:
			vkDestroyPipeline(m_Device, static_cast<VkPipeline>(handle), nullptr);
			break;
		case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
			vkDestroyDescriptorPool(m_Device, static_cast<VkDescriptorPool>(handle), nullptr);
			break;
		default:
			debug_warn("Unsupported object to destroy type.");
		}
		m_ObjectToDestroyQueue.pop();
	}
}

void CDevice::ProcessDeviceObjectToDestroyQueue(const bool ignoreFrameID)
{
	while (!m_TextureToDestroyQueue.empty() &&
		(ignoreFrameID || m_TextureToDestroyQueue.front().first + NUMBER_OF_FRAMES_IN_FLIGHT < m_FrameID))
	{
		GetDescriptorManager().OnTextureDestroy(m_TextureToDestroyQueue.front().second);
		m_TextureToDestroyQueue.pop();
	}

	while (!m_BufferToDestroyQueue.empty() &&
		(ignoreFrameID || m_BufferToDestroyQueue.front().first + NUMBER_OF_FRAMES_IN_FLIGHT < m_FrameID))
	{
		GetDescriptorManager().OnBufferDestroy(m_BufferToDestroyQueue.front().second);
		m_BufferToDestroyQueue.pop();
	}
}

DeviceObjectUID CDevice::GenerateNextDeviceObjectUID()
{
	ENSURE(m_LastAvailableUID < std::numeric_limits<DeviceObjectUID>::max());
	return m_LastAvailableUID++;
}

std::unique_ptr<IDevice> CreateDevice(SDL_Window* window)
{
	return Vulkan::CDevice::Create(window);
}

} // namespace Vulkan

} // namespace Backend

} // namespace Renderer
