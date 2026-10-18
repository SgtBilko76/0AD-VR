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

#ifndef INCLUDED_RENDERER
#define INCLUDED_RENDERER

#include "ps/Singleton.h"
#include "renderer/RenderingOptions.h"

#include <cstring>
#include <memory>
#include <span>

class CDebugRenderer;
class CFontManager;
class CGameView;
class CPostprocManager;
class CSceneRenderer;
class CShaderManager;
class CTextureManager;
class CTimeManager;
class CVertexBufferManager;
namespace PS::Memory { class LinearAllocator; }
namespace Renderer::Backend { class IDevice; }
namespace Renderer::Backend { class IDeviceCommandContext; }
namespace Renderer::Backend { class ISwapChain; }
namespace Renderer::Backend { class IVertexInputLayout; }
namespace Renderer::Backend { struct SVertexAttributeFormat; }

/**
 * Higher level interface on top of the whole frame rendering. It does know
 * what should be rendered and via which renderer but shouldn't know how to
 * render a particular area, like UI or scene.
 */
class CRenderer : public Singleton<CRenderer>
{
public:
	// stats class - per frame counts of number of draw calls, poly counts etc
	struct Stats
	{
		// set all stats to zero
		void Reset() { memset(this, 0, sizeof(*this)); }
		// number of draw calls per frame - total DrawElements + Begin/End immediate mode loops
		size_t m_DrawCalls;
		// number of terrain triangles drawn
		size_t m_TerrainTris;
		// number of water triangles drawn
		size_t m_WaterTris;
		// number of (non-transparent) model triangles drawn
		size_t m_ModelTris;
		// number of overlay triangles drawn
		size_t m_OverlayTris;
		// number of splat passes for alphamapping
		size_t m_BlendSplats;
		// number of particles
		size_t m_Particles;
	};

	enum class ScreenShotType
	{
		NONE,
		DEFAULT,
		BIG
	};

public:
	CRenderer(Renderer::Backend::IDevice* device);
	~CRenderer();

	// open up the renderer: performs any necessary initialisation
	bool Open(const uint32_t width, const uint32_t height);

	void CreateViewSizeDependentObjects(const uint32_t width, const uint32_t height);
	void DestroyViewSizeDependentObjects();

	// return view width
	int GetWidth() const { return m_Width; }
	// return view height
	int GetHeight() const { return m_Height; }

	void RenderFrame(bool needsPresent);

	// signal frame start
	void BeginFrame();
	// signal frame end
	void EndFrame();

	// trigger a reload of shaders (when parameters they depend on have changed)
	void MakeShadersDirty();

	// return stats accumulated for current frame
	Stats& GetStats() { return m_Stats; }

	CTextureManager& GetTextureManager();

	CVertexBufferManager& GetVertexBufferManager();

	CShaderManager& GetShaderManager();

	CFontManager& GetFontManager();

	CTimeManager& GetTimeManager();

	CPostprocManager& GetPostprocManager();

	CSceneRenderer& GetSceneRenderer();

	CDebugRenderer& GetDebugRenderer();

	/**
	 * Performs a complete frame without presenting to force loading all needed
	 * resources. It's used for the first frame on a game start.
	 * TODO: It might be better to preload resources without a complete frame
	 * rendering.
	 */
	void PreloadResourcesBeforeNextFrame();

	/**
	 * Makes a screenshot on the next RenderFrame according of the given
	 * screenshot type.
	 */
	void MakeScreenShotOnNextFrame(ScreenShotType screenShotType);

	Renderer::Backend::IDeviceCommandContext* GetDeviceCommandContext();

	/**
	 * Returns a cached vertex input layout. The renderer owns the layout to be
	 * able to share it between different clients. As backend should have
	 * as few different layouts as possible.
	 * The function isn't cheap so it should be called as rarely as possible.
	 * TODO: we need to make VertexArray less error prone by passing layout.
	 */
	Renderer::Backend::IVertexInputLayout* GetVertexInputLayout(
		const std::span<const Renderer::Backend::SVertexAttributeFormat> attributes);

	/**
	 * Currently using the linear allocated is allowed in small scopes to avoid
	 * high memory overhead. To validate that use PS::Memory::ScopedLinearAllocator.
	 */
	PS::Memory::LinearAllocator& GetLinearAllocator();

protected:
	friend class CDecalRData;
	friend class CPatchRData;
	friend class CPUSkinnedModelVertexRenderer;
	friend class CRenderingOptions;
	friend class GPUSkinnedModelModelRenderer;
	friend class InstancingModelRenderer;

	bool ShouldRender() const;

	void RenderFrameVR();

	void RenderFrameImpl(
		Renderer::Backend::ISwapChain& swapChain,
		const bool renderGUI, const bool renderLogger);
	void RenderScreenShot(const bool needsPresent);
	void RenderBigScreenShot(const bool needsPresent);

	void RenderGameAndGUI(
		Renderer::Backend::ISwapChain& swapChain, CGameView& gameView,
		const bool renderGUI, const bool renderLogger);
	void RenderGameWithPostProcessingAndGUI(
		Renderer::Backend::ISwapChain& swapChain, CGameView& gameView,
		CPostprocManager& postprocManager,
		const bool renderGUI, const bool renderLogger);
	void RenderGUIOnly(
		Renderer::Backend::ISwapChain& swapChain,
		const bool renderGUI, const bool renderLogger);

	void RenderGameOverlays(CGameView& gameView);
	void Render2D(const bool renderGUI, const bool renderLogger);

	void ReloadShaders();

	// Private data that is not needed by inline functions.
	class Internals;
	std::unique_ptr<Internals> m;
	// view width
	int m_Width = 0;
	// view height
	int m_Height = 0;

	// per-frame renderer stats
	Stats m_Stats;

	bool m_ShouldPreloadResourcesBeforeNextFrame = false;
};

#define g_Renderer CRenderer::GetSingleton()

#endif // INCLUDED_RENDERER
