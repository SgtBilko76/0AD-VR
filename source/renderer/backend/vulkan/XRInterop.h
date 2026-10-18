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

#ifndef INCLUDED_RENDERER_BACKEND_VULKAN_XRINTEROP
#define INCLUDED_RENDERER_BACKEND_VULKAN_XRINTEROP

#include "graphics/Color.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Renderer::Backend { class IDevice; }
namespace Renderer::Backend { class ISwapChain; }

/**
 * Glue between the Vulkan backend and an OpenXR session (Meta Quest).
 * Vulkan handles are passed as opaque values so that callers don't need the
 * backend's Vulkan headers.
 */
namespace Renderer::Backend::Vulkan::XR
{

struct Requirements
{
	std::vector<std::string> instanceExtensions;
	std::vector<std::string> deviceExtensions;
	// Receives a VkInstance and returns the VkPhysicalDevice OpenXR requires.
	std::function<void*(void* instance)> choosePhysicalDevice;
};

/**
 * Must be called before the device is created. The device then enables the
 * extensions and uses the physical device OpenXR asks for.
 */
void SetRequirements(Requirements requirements);
const Requirements* GetRequirements();

struct Binding
{
	void* instance;
	void* physicalDevice;
	void* device;
	uint32_t queueFamilyIndex;
	uint32_t queueIndex;
};

bool GetBinding(IDevice* device, Binding& binding);

/**
 * Wraps OpenXR swapchain images (VkImage handles) as an engine swapchain.
 * acquireImage is called from AcquireNextBackbuffer and returns the index of
 * the image OpenXR handed out; Present doesn't release anything, the caller
 * releases the images after SubmitFrame.
 * viewFormat is the VkFormat used for the image views (the images must be
 * created mutable when it differs from the swapchain format).
 */
std::unique_ptr<ISwapChain> CreateSwapChain(
	IDevice* device, const char* name, const std::vector<uint64_t>& images,
	int32_t viewFormat, uint32_t width, uint32_t height,
	std::function<bool(uint32_t&)> acquireImage, const CColor& clearColor);

/**
 * Submits all recorded work to the queue. OpenXR swapchain images may be
 * released afterwards.
 */
void SubmitFrame(IDevice* device);

} // namespace Renderer::Backend::Vulkan::XR

#endif // INCLUDED_RENDERER_BACKEND_VULKAN_XRINTEROP
