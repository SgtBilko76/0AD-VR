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

#ifndef INCLUDED_RENDERER_BACKEND_VULKAN_DEVICE
#define INCLUDED_RENDERER_BACKEND_VULKAN_DEVICE

#include "ps/CStr.h"
#include "renderer/backend/Backend.h"
#include "renderer/backend/IBuffer.h"
#include "renderer/backend/IDevice.h"
#include "renderer/backend/ITexture.h"
#include "renderer/backend/vulkan/DeviceForward.h"
#include "renderer/backend/vulkan/DeviceObjectUID.h"
#include "renderer/backend/vulkan/DeviceSelection.h"
#include "renderer/backend/vulkan/VMA.h"

#include <cstddef>
#include <cstdint>
#include <glad/vulkan.h>
#include <js/TypeDecls.h>
#include <limits>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Renderer::Backend { enum class Format; }
namespace Renderer::Backend::Vulkan { class CBuffer; }
namespace Renderer::Backend::Vulkan { class CDescriptorManager; }
namespace Renderer::Backend::Vulkan { class CRenderPassManager; }
namespace Renderer::Backend::Vulkan { class CRingCommandContext; }
namespace Renderer::Backend::Vulkan { class CSamplerManager; }
namespace Renderer::Backend::Vulkan { class CSubmitScheduler; }
namespace Renderer::Backend::Vulkan { class CSwapChain; }
namespace Renderer::Backend::Vulkan { class CTexture; }

typedef struct SDL_Window SDL_Window;

namespace Renderer
{

namespace Backend
{

namespace Vulkan
{

static constexpr size_t NUMBER_OF_FRAMES_IN_FLIGHT = 3;

class CDevice final : public IDevice
{
public:
	/**
	 * Creates the Vulkan device.
	 */
	static std::unique_ptr<CDevice> Create(SDL_Window* window);

	~CDevice() override;

	Backend GetBackend() const override { return Backend::VULKAN; }

	const std::string& GetName() const override { return m_Name; }
	const std::string& GetVersion() const override { return m_Version; }
	const std::string& GetDriverInformation() const override { return m_DriverInformation; }
	const std::vector<std::string>& GetExtensions() const override { return m_Extensions; }

	void Report(const Script::Request& rq, JS::HandleValue settings) override;

	std::unique_ptr<IDeviceCommandContext> CreateCommandContext() override;

	std::unique_ptr<ISwapChain> CreateSwapChain(
		const char* name, SDL_Window* window,
		int surfaceDrawableWidth, int surfaceDrawableHeight,
		const bool vsync, std::unique_ptr<ISwapChain> oldSwapChain) override;

	void WaitUntilIdle() override;

	std::unique_ptr<IGraphicsPipelineState> CreateGraphicsPipelineState(
		const SGraphicsPipelineStateDesc& pipelineStateDesc) override;

	std::unique_ptr<IComputePipelineState> CreateComputePipelineState(
		const SComputePipelineStateDesc& pipelineStateDesc) override;

	std::unique_ptr<IVertexInputLayout> CreateVertexInputLayout(
		const std::span<const SVertexAttributeFormat> attributes) override;

	std::unique_ptr<ITexture> CreateTexture(
		const char* name, const ITexture::Type type, const uint32_t usage,
		const Format format, const uint32_t width, const uint32_t height,
		const Sampler::Desc& defaultSamplerDesc, const uint32_t MIPLevelCount, const uint32_t sampleCount) override;

	std::unique_ptr<ITexture> CreateTexture2D(
		const char* name, const uint32_t usage,
		const Format format, const uint32_t width, const uint32_t height,
		const Sampler::Desc& defaultSamplerDesc, const uint32_t MIPLevelCount = 1, const uint32_t sampleCount = 1) override;

	std::unique_ptr<IFramebuffer> CreateFramebuffer(
		const char* name, SColorAttachment* colorAttachment,
		SDepthStencilAttachment* depthStencilAttachment) override;

	std::unique_ptr<IBuffer> CreateBuffer(
		const char* name, const IBuffer::Type type, const uint32_t size, const uint32_t usage) override;

	std::unique_ptr<CBuffer> CreateCBuffer(
		const char* name, const IBuffer::Type type, const uint32_t size, const uint32_t usage);

	std::unique_ptr<IShaderProgram> CreateShaderProgram(
		const CStr& name, const CShaderDefines& defines) override;

	bool IsTextureFormatSupported(const Format format) const override;

	bool IsFramebufferFormatSupported(const Format format) const override;

	Format GetPreferredDepthStencilFormat(
		const uint32_t usage, const bool depth, const bool stencil) const override;

	bool IsQueryResultAvailable(const uint32_t handle) const override;

	uint32_t AllocateQuery() override;

	void FreeQuery(const uint32_t handle) override;

	uint64_t GetQueryResult(const uint32_t handle) override;

	const Capabilities& GetCapabilities() const override { return m_Capabilities; }

	void CollectStatistics(StatisticsVector& statistics) const override;

	VkDevice GetVkDevice() const { return m_Device; }

	VmaAllocator GetVMAAllocator() { return m_VMAAllocator; }

	void InsertTimestampQuery(
		VkCommandBuffer commandBuffer, const uint32_t handle, const bool isScopeBegin);

	void ScheduleObjectToDestroy(
		VkObjectType type, const void* handle, const VmaAllocation allocation)
	{
		ScheduleObjectToDestroy(type, reinterpret_cast<uint64_t>(handle), allocation);
	}

	void ScheduleObjectToDestroy(
		VkObjectType type, const uint64_t handle, const VmaAllocation allocation);

	void ScheduleTextureToDestroy(const DeviceObjectUID uid);

	void ScheduleBufferToDestroy(const DeviceObjectUID uid);

	void OnPresent();

	void OnSwapChainRecreation();

	void SetObjectName(VkObjectType type, const void* handle, const char* name)
	{
		SetObjectName(type, reinterpret_cast<uint64_t>(handle), name);
	}

	void SetObjectName(VkObjectType type, const uint64_t handle, const char* name);

	std::unique_ptr<CRingCommandContext> CreateRingCommandContext(const size_t size);

	const SAvailablePhysicalDevice& GetChoosenPhysicalDevice() const { return m_ChoosenDevice; }

	CRenderPassManager& GetRenderPassManager() { return *m_RenderPassManager; }

	CSamplerManager& GetSamplerManager() { return *m_SamplerManager; }

	CDescriptorManager& GetDescriptorManager() { return *m_DescriptorManager; }

	DeviceObjectUID GenerateNextDeviceObjectUID();

	uint32_t GetFrameID() const { return m_FrameID; }

	// Accessors needed to bind an OpenXR session to this device.
	VkInstance GetVkInstance() const { return m_Instance; }
	VkPhysicalDevice GetVkPhysicalDevice() const { return m_ChoosenDevice.device; }
	uint32_t GetGraphicsQueueFamilyIndex() const { return m_GraphicsQueueFamilyIndex; }
	VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
	CSubmitScheduler& GetSubmitScheduler() { return *m_SubmitScheduler; }

private:
	CDevice();

	void ProcessObjectToDestroyQueue(const bool ignoreFrameID = false);
	void ProcessDeviceObjectToDestroyQueue(const bool ignoreFrameID = false);

	bool IsFormatSupportedForUsage(const Format format, const uint32_t usage) const;

	std::string m_Name;
	std::string m_Version;
	std::string m_VendorID;
	std::string m_DriverInformation;
	std::vector<std::string> m_Extensions;
	std::vector<std::string> m_InstanceExtensions;
	std::vector<std::string> m_ValidationLayers;

	SAvailablePhysicalDevice m_ChoosenDevice{};
	std::vector<SAvailablePhysicalDevice> m_AvailablePhysicalDevices;

	Capabilities m_Capabilities{};

	VkInstance m_Instance = VK_NULL_HANDLE;

	VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

	SDL_Window* m_Window = nullptr;
	VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	VmaAllocator m_VMAAllocator = VK_NULL_HANDLE;
	VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
	uint32_t m_GraphicsQueueFamilyIndex = std::numeric_limits<uint32_t>::max();

	VkQueryPool m_QueryPool{VK_NULL_HANDLE};
	struct Query
	{
		uint32_t lastUsageFrameID{};
		bool occupied{};
		bool submitted{};
	};
	std::vector<Query> m_Queries;

	uint32_t m_FrameID = 0;

	struct ObjectToDestroy
	{
		uint32_t frameID;
		VkObjectType type;
		uint64_t handle;
		VmaAllocation allocation;
	};
	std::queue<ObjectToDestroy> m_ObjectToDestroyQueue;
	std::queue<std::pair<uint32_t, DeviceObjectUID>> m_TextureToDestroyQueue;
	std::queue<std::pair<uint32_t, DeviceObjectUID>> m_BufferToDestroyQueue;

	std::unique_ptr<CRenderPassManager> m_RenderPassManager;
	std::unique_ptr<CSamplerManager> m_SamplerManager;
	std::unique_ptr<CDescriptorManager> m_DescriptorManager;
	std::unique_ptr<CSubmitScheduler> m_SubmitScheduler;

	DeviceObjectUID m_LastAvailableUID{1};
};

} // namespace Vulkan

} // namespace Backend

} // namespace Renderer

#endif // INCLUDED_RENDERER_BACKEND_VULKAN_DEVICE
