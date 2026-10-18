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

#include "XRInterop.h"

#include "lib/debug.h"
#include "ps/Profile.h"
#include "renderer/backend/ISwapChain.h"
#include "renderer/backend/ITexture.h"
#include "renderer/backend/Sampler.h"
#include "renderer/backend/vulkan/Device.h"
#include "renderer/backend/vulkan/Framebuffer.h"
#include "renderer/backend/vulkan/RingCommandContext.h"
#include "renderer/backend/vulkan/SubmitScheduler.h"
#include "renderer/backend/vulkan/Texture.h"
#include "renderer/backend/vulkan/Utilities.h"

#include <cstdio>
#include <limits>
#include <optional>
#include <unordered_map>

namespace Renderer::Backend::Vulkan
{

/**
 * An engine swapchain over images owned by an OpenXR swapchain.
 */
class CXRSwapChain final : public ISwapChain
{
public:
	CXRSwapChain(
		CDevice* device, const char* name, const std::vector<uint64_t>& images,
		const VkFormat viewFormat, const uint32_t width, const uint32_t height,
		std::function<bool(uint32_t&)> acquireImage, const CColor& clearColor)
		: m_Device{device}, m_AcquireImage{std::move(acquireImage)}, m_ClearColor{clearColor}
	{
		char nameBuffer[96];
		for (size_t index = 0; index < images.size(); ++index)
		{
			const VkImage image{reinterpret_cast<VkImage>(images[index])};
			m_Images.emplace_back(image);
			snprintf(nameBuffer, std::size(nameBuffer), "%s #%zu", name, index);
			m_Textures.emplace_back(CTexture::WrapBackbufferImage(
				device, nameBuffer, image, viewFormat,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				width, height));
		}
		snprintf(nameBuffer, std::size(nameBuffer), "%s depth", name);
		m_DepthTexture = CTexture::Create(
			device, nameBuffer, ITexture::Type::TEXTURE_2D,
			ITexture::Usage::DEPTH_STENCIL_ATTACHMENT,
			device->GetPreferredDepthStencilFormat(
				ITexture::Usage::DEPTH_STENCIL_ATTACHMENT, true, true),
			width, height, Sampler::MakeDefaultSampler(
				Sampler::Filter::NEAREST, Sampler::AddressMode::CLAMP_TO_EDGE),
			1, 1);
		m_Backbuffers.resize(images.size());
		m_CommandContext = CRingCommandContext::Create(
			device, NUMBER_OF_FRAMES_IN_FLIGHT, device->GetGraphicsQueueFamilyIndex(),
			device->GetSubmitScheduler());
		m_IsValid = m_DepthTexture && m_CommandContext;
	}

	~CXRSwapChain() override
	{
		m_Backbuffers.clear();
		m_Textures.clear();
		m_DepthTexture.reset();
	}

	IDevice* GetDevice() override { return m_Device; }

	bool IsValid() const override { return m_IsValid; }

	bool AcquireNextBackbuffer() override
	{
		ENSURE(m_CurrentImageIndex == std::numeric_limits<uint32_t>::max());
		uint32_t index{0};
		if (!m_AcquireImage(index) || index >= m_Images.size())
			return false;
		m_CurrentImageIndex = index;

		// We always clear or overwrite the whole image, so its previous
		// content doesn't matter. OpenXR expects COLOR_ATTACHMENT_OPTIMAL on
		// release, which is also what our render passes leave.
		const VkCommandBuffer commandBuffer{m_CommandContext->GetCommandBuffer()};
		Utilities::SubmitImageMemoryBarrier(
			commandBuffer, m_Images[index], 0, 0,
			0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		if (!m_DepthTexture->IsInitialized())
		{
			Utilities::SubmitImageMemoryBarrier(
				commandBuffer, m_DepthTexture->GetImage(), 0, 0,
				0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
			m_DepthTexture->SetInitialized();
		}
		m_CommandContext->Flush();
		return true;
	}

	IFramebuffer* GetCurrentBackbuffer(
		const AttachmentLoadOp colorAttachmentLoadOp,
		const AttachmentStoreOp colorAttachmentStoreOp,
		const AttachmentLoadOp depthStencilAttachmentLoadOp,
		const AttachmentStoreOp depthStencilAttachmentStoreOp) override
	{
		ENSURE(m_CurrentImageIndex != std::numeric_limits<uint32_t>::max());
		auto& framebuffers{m_Backbuffers[m_CurrentImageIndex]};
		const uint32_t key{
			static_cast<uint32_t>(colorAttachmentLoadOp) |
			static_cast<uint32_t>(colorAttachmentStoreOp) << 8 |
			static_cast<uint32_t>(depthStencilAttachmentLoadOp) << 16 |
			static_cast<uint32_t>(depthStencilAttachmentStoreOp) << 24};
		auto it = framebuffers.find(key);
		if (it == framebuffers.end())
		{
			SColorAttachment colorAttachment{};
			colorAttachment.texture = m_Textures[m_CurrentImageIndex].get();
			colorAttachment.loadOp = colorAttachmentLoadOp;
			colorAttachment.storeOp = colorAttachmentStoreOp;
			colorAttachment.clearColor = m_ClearColor;

			SDepthStencilAttachment depthStencilAttachment{};
			depthStencilAttachment.texture = m_DepthTexture.get();
			depthStencilAttachment.loadOp = depthStencilAttachmentLoadOp;
			depthStencilAttachment.storeOp = depthStencilAttachmentStoreOp;

			it = framebuffers.emplace(key, CFramebuffer::Create(
				m_Device, "XR backbuffer", &colorAttachment, &depthStencilAttachment)).first;
		}
		return it->second.get();
	}

	void Present() override
	{
		// The image is released by the owner after SubmitFrame.
		m_CurrentImageIndex = std::numeric_limits<uint32_t>::max();
	}

private:
	CDevice* m_Device{nullptr};
	std::function<bool(uint32_t&)> m_AcquireImage;
	CColor m_ClearColor;
	bool m_IsValid{false};
	uint32_t m_CurrentImageIndex{std::numeric_limits<uint32_t>::max()};
	std::vector<VkImage> m_Images;
	std::vector<std::unique_ptr<CTexture>> m_Textures;
	std::unique_ptr<CTexture> m_DepthTexture;
	std::vector<std::unordered_map<uint32_t, std::unique_ptr<CFramebuffer>>> m_Backbuffers;
	std::unique_ptr<CRingCommandContext> m_CommandContext;
};

namespace XR
{

namespace
{
std::optional<Requirements> g_Requirements;
} // anonymous namespace

void SetRequirements(Requirements requirements)
{
	g_Requirements = std::move(requirements);
}

const Requirements* GetRequirements()
{
	return g_Requirements ? &*g_Requirements : nullptr;
}

bool GetBinding(IDevice* device, Binding& binding)
{
	if (!device || device->GetBackend() != Backend::VULKAN)
		return false;
	CDevice* vulkanDevice{static_cast<CDevice*>(device)};
	binding.instance = reinterpret_cast<void*>(vulkanDevice->GetVkInstance());
	binding.physicalDevice = reinterpret_cast<void*>(vulkanDevice->GetVkPhysicalDevice());
	binding.device = reinterpret_cast<void*>(vulkanDevice->GetVkDevice());
	binding.queueFamilyIndex = vulkanDevice->GetGraphicsQueueFamilyIndex();
	binding.queueIndex = 0;
	return true;
}

std::unique_ptr<ISwapChain> CreateSwapChain(
	IDevice* device, const char* name, const std::vector<uint64_t>& images,
	int32_t viewFormat, uint32_t width, uint32_t height,
	std::function<bool(uint32_t&)> acquireImage, const CColor& clearColor)
{
	std::unique_ptr<CXRSwapChain> swapChain{std::make_unique<CXRSwapChain>(
		static_cast<CDevice*>(device), name, images, static_cast<VkFormat>(viewFormat),
		width, height, std::move(acquireImage), clearColor)};
	if (!swapChain->IsValid())
		return nullptr;
	return swapChain;
}

void SubmitFrame(IDevice* device)
{
	PROFILE3("XR submit");
	CDevice* vulkanDevice{static_cast<CDevice*>(device)};
	vulkanDevice->GetSubmitScheduler().Flush();
	vulkanDevice->OnPresent();
}

} // namespace XR

} // namespace Renderer::Backend::Vulkan
