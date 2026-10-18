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

#include "Renderer.h"

#include "graphics/Camera.h"
#include "graphics/Canvas2D.h"
#include "graphics/CinemaManager.h"
#include "graphics/Color.h"
#include "graphics/FontManager.h"
#include "graphics/GameView.h"
#include "graphics/ModelDef.h"
#include "graphics/ShaderManager.h"
#include "graphics/TerrainTextureManager.h"
#include "graphics/TextureManager.h"
#include "gui/GUIManager.h"
#include "i18n/L10n.h"
#include "lib/alignment.h"
#include "lib/allocators/shared_ptr.h"
#include "lib/code_annotation.h"
#include "lib/debug.h"
#include "lib/file/vfs/vfs.h"
#include "lib/file/vfs/vfs_path.h"
#include "lib/file/vfs/vfs_util.h"
#include "lib/hash.h"
#include "lib/os_path.h"
#include "lib/secure_crt.h"
#include "lib/status.h"
#include "lib/tex/tex.h"
#include "maths/Matrix3D.h"
#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/ConfigDB.h"
#include "ps/Filesystem.h"
#include "ps/Game.h"
#include "ps/GameSetup/Config.h"
#include "ps/Globals.h"
#include "ps/memory/LinearAllocator.h"
#include "ps/Hotkey.h"
#include "ps/Profile.h"
#include "ps/ProfileViewer.h"
#include "ps/QuestVR.h"
#include "ps/Profiler2.h"
#include "ps/Util.h"
#include "ps/VideoMode.h"
#include "ps/World.h"
#include "renderer/DebugRenderer.h"
#include "renderer/PostprocManager.h"
#include "renderer/RenderingOptions.h"
#include "renderer/SceneRenderer.h"
#include "renderer/TimeManager.h"
#include "renderer/VertexBufferManager.h"
#include "renderer/backend/Backend.h"
#include "renderer/backend/IDevice.h"
#include "renderer/backend/IDeviceCommandContext.h"
#include "renderer/backend/IFramebuffer.h"
#include "renderer/backend/IShaderProgram.h"
#include "renderer/backend/ISwapChain.h"
#include "tools/atlas/GameInterface/GameLoop.h"
#include "tools/atlas/GameInterface/View.h"

#include <SDL_events.h>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

size_t g_NextScreenShotNumber = 0;

///////////////////////////////////////////////////////////////////////////////////
// CRendererStatsTable - Profile display of rendering stats

/**
 * Class CRendererStatsTable: Implementation of AbstractProfileTable to
 * display the renderer stats in-game.
 *
 * Accesses CRenderer::m_Stats by keeping the reference passed to the
 * constructor.
 */
class CRendererStatsTable : public AbstractProfileTable
{
	NONCOPYABLE(CRendererStatsTable);
public:
	CRendererStatsTable(const CRenderer::Stats& st, const PS::Memory::LinearAllocator& linearAllocator);

	// Implementation of AbstractProfileTable interface
	CStr GetName() override;
	CStr GetTitle() override;
	size_t GetNumberRows() override;
	const std::vector<ProfileColumn>& GetColumns() override;
	CStr GetCellText(size_t row, size_t col) override;
	AbstractProfileTable* GetChild(size_t row) override;

private:
	/// Reference to the renderer singleton's stats
	const CRenderer::Stats& Stats;
	const PS::Memory::LinearAllocator& m_LinearAllocator;

	/// Column descriptions
	std::vector<ProfileColumn> columnDescriptions;

	enum
	{
		Row_DrawCalls = 0,
		Row_TerrainTris,
		Row_WaterTris,
		Row_ModelTris,
		Row_OverlayTris,
		Row_BlendSplats,
		Row_Particles,
		Row_VBReserved,
		Row_VBAllocated,
		Row_TextureMemory,
		Row_ShadersLoaded,
		Row_LinearAllocator,

		// Must be last to count number of rows
		NumberRows
	};
};

// Construction
CRendererStatsTable::CRendererStatsTable(const CRenderer::Stats& st, const PS::Memory::LinearAllocator& linearAllocator)
	: Stats(st), m_LinearAllocator(linearAllocator)
{
	columnDescriptions.push_back(ProfileColumn("Name", 230));
	columnDescriptions.push_back(ProfileColumn("Value", 100));
}

// Implementation of AbstractProfileTable interface
CStr CRendererStatsTable::GetName()
{
	return "renderer";
}

CStr CRendererStatsTable::GetTitle()
{
	return "Renderer statistics";
}

size_t CRendererStatsTable::GetNumberRows()
{
	return NumberRows;
}

const std::vector<ProfileColumn>& CRendererStatsTable::GetColumns()
{
	return columnDescriptions;
}

CStr CRendererStatsTable::GetCellText(size_t row, size_t col)
{
	char buf[256];

	switch(row)
	{
	case Row_DrawCalls:
		if (col == 0)
			return "# draw calls";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_DrawCalls);
		return buf;

	case Row_TerrainTris:
		if (col == 0)
			return "# terrain tris";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_TerrainTris);
		return buf;

	case Row_WaterTris:
		if (col == 0)
			return "# water tris";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_WaterTris);
		return buf;

	case Row_ModelTris:
		if (col == 0)
			return "# model tris";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_ModelTris);
		return buf;

	case Row_OverlayTris:
		if (col == 0)
			return "# overlay tris";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_OverlayTris);
		return buf;

	case Row_BlendSplats:
		if (col == 0)
			return "# blend splats";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_BlendSplats);
		return buf;

	case Row_Particles:
		if (col == 0)
			return "# particles";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)Stats.m_Particles);
		return buf;

	case Row_VBReserved:
		if (col == 0)
			return "VB reserved";
		sprintf_s(buf, sizeof(buf), "%lu kB", static_cast<unsigned long>(g_Renderer.GetVertexBufferManager().GetBytesReserved() / 1024));
		return buf;

	case Row_VBAllocated:
		if (col == 0)
			return "VB allocated";
		sprintf_s(buf, sizeof(buf), "%lu kB", static_cast<unsigned long>(g_Renderer.GetVertexBufferManager().GetBytesAllocated() / 1024));
		return buf;

	case Row_TextureMemory:
		if (col == 0)
			return "textures uploaded";
		sprintf_s(buf, sizeof(buf), "%lu kB", (unsigned long)g_Renderer.GetTextureManager().GetBytesUploaded() / 1024);
		return buf;

	case Row_ShadersLoaded:
		if (col == 0)
			return "shader effects loaded";
		sprintf_s(buf, sizeof(buf), "%lu", (unsigned long)g_Renderer.GetShaderManager().GetNumEffectsLoaded());
		return buf;

	case Row_LinearAllocator:
		if (col == 0)
			return "linear allocator";
		sprintf_s(buf, sizeof(buf), "%lu", static_cast<unsigned long>(m_LinearAllocator.GetCapacity()));
		return buf;

	default:
		return "???";
	}
}

AbstractProfileTable* CRendererStatsTable::GetChild(size_t /*row*/)
{
	return 0;
}

class CRendererBackendStatsTable : public AbstractProfileTable
{
public:
	CRendererBackendStatsTable(const Renderer::Backend::IDevice& device)
		: m_Device(device)
	{
		m_ColumnDescriptions.push_back(ProfileColumn("Name", 230));
		m_ColumnDescriptions.push_back(ProfileColumn("Value", 100));
	}

	CStr GetName() override { return "renderer.backend"; }
	CStr GetTitle() override { return "Renderer backend statistics"; }
	size_t GetNumberRows() override { UpdateIfNeeded(); return m_Statistics.size(); }
	const std::vector<ProfileColumn>& GetColumns() override { return m_ColumnDescriptions; }
	AbstractProfileTable* GetChild(size_t) override { return nullptr; }

	CStr GetCellText(size_t row, size_t col) override
	{
		UpdateIfNeeded();
		if (row >= m_Statistics.size() || col > 2)
			return "";
		if (col == 0)
			return CStr(m_Statistics[row].name);
		return std::visit(
			[]<typename T>(const T& value) -> std::string
			{
				return std::to_string(value);
			},
			m_Statistics[row].value) + " " + CStr{m_Statistics[row].unit};
	}

	void MakeOutdated() { m_Outdated = true; }

private:
	void UpdateIfNeeded()
	{
		if (!m_Outdated)
			return;
		m_Outdated = false;
		m_Statistics.clear();
		m_Device.CollectStatistics(m_Statistics);
	}

	const Renderer::Backend::IDevice& m_Device;

	std::vector<ProfileColumn> m_ColumnDescriptions;
	Renderer::Backend::IDevice::StatisticsVector m_Statistics;

	// We need to recalculate statistics only when a new frame happened.
	bool m_Outdated{true};
};

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////////
// CRenderer implementation

/**
 * Struct CRendererInternals: Truly hide data that is supposed to be hidden
 * in this structure so it won't even appear in header files.
 */
class CRenderer::Internals
{
	NONCOPYABLE(Internals);
public:
	Renderer::Backend::IDevice* device;

	std::unique_ptr<Renderer::Backend::IDeviceCommandContext> deviceCommandContext;

	/// true if CRenderer::Open has been called
	bool IsOpen;

	/// true if shaders need to be reloaded
	bool ShadersDirty;

	/// Table to display renderer stats in-game via profile system
	CRendererStatsTable profileTable;
	CRendererBackendStatsTable backendProfileTable;

	/// Shader manager
	CShaderManager shaderManager;

	/// Texture manager
	CTextureManager textureManager;

	CVertexBufferManager vertexBufferManager;

	/// Time manager
	CTimeManager timeManager;

	/// Postprocessing effect manager
	CPostprocManager postprocManager;

	CSceneRenderer sceneRenderer;

	CDebugRenderer debugRenderer;

	CFontManager fontManager;

	// During rendering we need to collect and sort many objects. To reduce
	// the allocation cost and increase cache locality we use the
	// LinearAllocator.
	// If we need to have more than 16MiB of continious memory then we're doing
	// a lot of unnecessary work.
	PS::Memory::LinearAllocator linearAllocator{1 * MiB, 16 * MiB};

	struct VertexAttributesHash
	{
		size_t operator()(const std::vector<Renderer::Backend::SVertexAttributeFormat>& attributes) const;
	};

	std::unordered_map<
		std::vector<Renderer::Backend::SVertexAttributeFormat>,
		std::unique_ptr<Renderer::Backend::IVertexInputLayout>, VertexAttributesHash> vertexInputLayouts;

	ScreenShotType m_ScreenShotType{ScreenShotType::NONE};

	struct InputHandler
	{
		ScreenShotType& screenshotType;

		Input::Reaction operator()(const SDL_Event& ev)
		{
			if (ev.type != SDL_HOTKEYPRESS)
				return Input::Reaction::PASS;

			std::string_view hotkey{static_cast<const char*>(ev.user.data1)};
			if (hotkey == "screenshot")
			{
				screenshotType = ScreenShotType::DEFAULT;
				return Input::Reaction::HANDLED;
			}
			if (hotkey == "bigscreenshot")
			{
				screenshotType = ScreenShotType::BIG;
				return Input::Reaction::HANDLED;
			}
			return Input::Reaction::PASS;
		}
	};

	Input::Handler<InputHandler> m_InputHandler{g_VideoMode.m_InputManager, Input::Slot::SCREENSHOT,
		{m_ScreenShotType}};

	Internals(Renderer::Backend::IDevice* device) :
		device(device),
		deviceCommandContext(device->CreateCommandContext()),
		IsOpen(false), ShadersDirty(true), profileTable(g_Renderer.m_Stats, linearAllocator),
		backendProfileTable(*device),
		shaderManager(device), textureManager(g_VFS, false, device), vertexBufferManager(device),
		postprocManager(device), sceneRenderer(device), fontManager(device)
	{
	}
};

size_t CRenderer::Internals::VertexAttributesHash::operator()(
	const std::vector<Renderer::Backend::SVertexAttributeFormat>& attributes) const
{
	size_t seed = 0;
	hash_combine(seed, attributes.size());
	for (const Renderer::Backend::SVertexAttributeFormat& attribute : attributes)
	{
		hash_combine(seed, attribute.stream);
		hash_combine(seed, attribute.format);
		hash_combine(seed, attribute.offset);
		hash_combine(seed, attribute.stride);
		hash_combine(seed, attribute.rate);
		hash_combine(seed, attribute.bindingSlot);
	}
	return seed;
}

CRenderer::CRenderer(Renderer::Backend::IDevice* device)
{
	PROFILE2("InitRenderer");

	m = std::make_unique<Internals>(device);

	g_ProfileViewer.AddRootTable(&m->profileTable);
	g_ProfileViewer.AddRootTable(&m->backendProfileTable);

	m_Width = 0;
	m_Height = 0;

	m_Stats.Reset();

	// Create terrain related stuff.
	new CTerrainTextureManager(device);

	if (QuestVR::IsActive())
	{
		int eyeWidth{0}, eyeHeight{0};
		QuestVR::GetEyeSize(eyeWidth, eyeHeight);
		Open(eyeWidth, eyeHeight);
	}
	else
		Open(g_VideoMode.GetWindowWidth(), g_VideoMode.GetWindowHeight());

	// Setup lighting environment. Since the Renderer accesses the
	// lighting environment through a pointer, this has to be done before
	// the first Frame.
	GetSceneRenderer().SetLightEnv(&g_LightEnv);

	ModelDefActivateFastImpl();
	ColorActivateFastImpl();
}

CRenderer::~CRenderer()
{
	PROFILE2("~CRenderer");
	delete &g_TexMan;

	// We no longer UnloadWaterTextures here -
	// that is the responsibility of the module that asked for
	// them to be loaded (i.e. CGameView).
	m.reset();
}

void CRenderer::ReloadShaders()
{
	ENSURE(m->IsOpen);

	m->sceneRenderer.ReloadShaders(m->device);
	m->ShadersDirty = false;
}

bool CRenderer::Open(const uint32_t width, const uint32_t height)
{
	m->IsOpen = true;

	// Dimensions
	m_Width = width;
	m_Height = height;

	m->debugRenderer.Initialize();

	if (m->postprocManager.IsEnabled())
		m->postprocManager.Initialize(width, height);

	m->sceneRenderer.Initialize(width, height);

	return true;
}

void CRenderer::CreateViewSizeDependentObjects(const uint32_t width, const uint32_t height)
{
	m_Width = width;
	m_Height = height;

	m->postprocManager.CreateViewSizeDependentObjects(width, height);

	m->sceneRenderer.CreateViewSizeDependentObjects(width, height);
}

void CRenderer::DestroyViewSizeDependentObjects()
{
	m->sceneRenderer.DestroyViewSizeDependentObjects();
	m->postprocManager.DestroyViewSizeDependentObjects();
}

bool CRenderer::ShouldRender() const
{
	const bool shouldRenderWhenOutOfFocus{
		g_ConfigDB.Get("renderer.renderwhenoutoffocus", true)};
	return !g_app_minimized && (g_app_has_focus || shouldRenderWhenOutOfFocus);
}

void CRenderer::RenderFrame(const bool needsPresent)
{
	// Do not render if not focused while in fullscreen or minimised,
	// as that triggers a difficult-to-reproduce crash on some graphic cards.
	if (QuestVR::IsActive())
	{
		RenderFrameVR();
		return;
	}

	if (!ShouldRender())
		return;

	if (m->m_ScreenShotType == ScreenShotType::BIG)
	{
		RenderBigScreenShot(needsPresent);
	}
	else if (m->m_ScreenShotType == ScreenShotType::DEFAULT)
	{
		RenderScreenShot(needsPresent);
	}
	else
	{
		Renderer::Backend::ISwapChain* swapChain{g_VideoMode.GetOrCreateSwapChain()};
		if (!swapChain || !swapChain->IsValid())
			return;

		// In case of no acquired backbuffer we have nothing render to.
		if (needsPresent && !swapChain->AcquireNextBackbuffer())
			return;

		if (m_ShouldPreloadResourcesBeforeNextFrame)
		{
			m_ShouldPreloadResourcesBeforeNextFrame = false;
			// We don't need to render logger for the preload.
			RenderFrameImpl(*swapChain, true, false);
		}

		RenderFrameImpl(*swapChain, true, true);

		m->deviceCommandContext->Flush();
		if (needsPresent)
			swapChain->Present();
	}
}

void CRenderer::RenderFrameVR()
{
	if (!QuestVR::ShouldRender())
	{
		QuestVR::EndFrame(m->device, false, false, false);
		return;
	}

	PROFILE3("render VR");
	m->m_ScreenShotType = ScreenShotType::NONE;

	g_Profiler2.RecordGPUFrameStart(m->deviceCommandContext.get());

	g_TexMan.UploadResourcesIfNeeded(m->deviceCommandContext.get());
	m->textureManager.MakeUploadProgress(m->deviceCommandContext.get());

	if (g_Game)
		m->sceneRenderer.SetSimulation(g_Game->GetSimulation2());

	// The battlefield as a stereo tabletop.
	bool eyesRendered{false};
	if (g_Game && g_Game->IsGameStarted())
	{
		CGameView& gameView{*g_Game->GetView()};
		g_Game->CachePlayerColors();
		CCamera eyeCamera, cullCamera;
		if (QuestVR::GetEyeCamera(0, gameView.GetCamera(), eyeCamera, cullCamera))
		{
			eyesRendered = true;
			for (int eye = 0; eye < 2; ++eye)
			{
				if (eye > 0)
					QuestVR::GetEyeCamera(eye, gameView.GetCamera(), eyeCamera, cullCamera);
				Renderer::Backend::ISwapChain* swapChain{QuestVR::GetEyeSwapChain(eye)};
				if (!swapChain || !swapChain->AcquireNextBackbuffer())
				{
					eyesRendered = false;
					break;
				}

				m->sceneRenderer.SetSceneCamera(eyeCamera, cullCamera);
				BeginFrame();
				gameView.EnumerateSceneObjects();
				m->sceneRenderer.PrepareSubmissions(m->deviceCommandContext.get());

				Renderer::Backend::IFramebuffer* framebuffer{swapChain->GetCurrentBackbuffer(
					Renderer::Backend::AttachmentLoadOp::CLEAR,
					Renderer::Backend::AttachmentStoreOp::STORE,
					Renderer::Backend::AttachmentLoadOp::CLEAR,
					Renderer::Backend::AttachmentStoreOp::DONT_CARE)};
				Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
				// Top-left part of the image (the backend's y is bottom-up).
				QuestVR::GetEyeRenderSize(viewportRect.width, viewportRect.height);
				viewportRect.y = framebuffer->GetHeight() - viewportRect.height;
				m->deviceCommandContext->BeginFramebufferPass(framebuffer);
				m->deviceCommandContext->SetViewports(1, &viewportRect);
				m->sceneRenderer.RenderSubmissions(m->deviceCommandContext.get());
				RenderGameOverlays(gameView);
				QuestVR::DrawLaser(*m->deviceCommandContext, true);
				m->deviceCommandContext->EndFramebufferPass();
				swapChain->Present();

				EndFrame();
			}
		}
	}

	// Menus: eye images hold only the laser pointer.
	bool eyesOverlay{false};
	if (!eyesRendered && !(g_Game && g_Game->IsGameStarted()))
	{
		CCamera eyeCamera;
		for (int eye = 0; eye < 2; ++eye)
		{
			if (!QuestVR::GetMenuEyeCamera(eye, eyeCamera))
				break;
			Renderer::Backend::ISwapChain* swapChain{QuestVR::GetEyeSwapChain(eye)};
			if (!swapChain || !swapChain->AcquireNextBackbuffer())
				break;
			m->sceneRenderer.SetSceneCamera(eyeCamera, eyeCamera);
			Renderer::Backend::IFramebuffer* framebuffer{swapChain->GetCurrentBackbuffer(
				Renderer::Backend::AttachmentLoadOp::CLEAR,
				Renderer::Backend::AttachmentStoreOp::STORE,
				Renderer::Backend::AttachmentLoadOp::CLEAR,
				Renderer::Backend::AttachmentStoreOp::DONT_CARE)};
			Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
			QuestVR::GetEyeRenderSize(viewportRect.width, viewportRect.height);
			viewportRect.y = framebuffer->GetHeight() - viewportRect.height;
			m->deviceCommandContext->BeginFramebufferPass(framebuffer);
			m->deviceCommandContext->SetViewports(1, &viewportRect);
			QuestVR::DrawLaser(*m->deviceCommandContext, false);
			m->deviceCommandContext->EndFramebufferPass();
			swapChain->Present();
			eyesRendered = eyesOverlay = eye == 1;
		}
	}

	// GUI on the floating panel.
	bool panelRendered{false};
	Renderer::Backend::ISwapChain* panel{QuestVR::GetPanelSwapChain()};
	if (panel && panel->AcquireNextBackbuffer())
	{
		BeginFrame();
		Renderer::Backend::IFramebuffer* framebuffer{panel->GetCurrentBackbuffer(
			Renderer::Backend::AttachmentLoadOp::CLEAR,
			Renderer::Backend::AttachmentStoreOp::STORE,
			Renderer::Backend::AttachmentLoadOp::CLEAR,
			Renderer::Backend::AttachmentStoreOp::DONT_CARE)};
		Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
		viewportRect.width = framebuffer->GetWidth();
		viewportRect.height = framebuffer->GetHeight();
		m->deviceCommandContext->BeginFramebufferPass(framebuffer);
		m->deviceCommandContext->SetViewports(1, &viewportRect);
		Render2D(true, true);
		{
			CCanvas2D canvas(g_VideoMode.GetWindowWidth(), g_VideoMode.GetWindowHeight(),
				g_VideoMode.GetScale(), m->deviceCommandContext.get());
			QuestVR::DrawPanelOverlay(canvas);
		}
		m->deviceCommandContext->EndFramebufferPass();
		panel->Present();
		EndFrame();
		panelRendered = true;
	}

	g_Profiler2.RecordGPUFrameEnd(m->deviceCommandContext.get());

	m->deviceCommandContext->Flush();
	QuestVR::EndFrame(m->device, eyesRendered, panelRendered, eyesOverlay);

	m->linearAllocator.Release();
}

void CRenderer::RenderFrameImpl(
	Renderer::Backend::ISwapChain& swapChain,
	const bool renderGUI, const bool renderLogger)
{
	PROFILE3("render");

	g_Profiler2.RecordGPUFrameStart(m->deviceCommandContext.get());

	g_TexMan.UploadResourcesIfNeeded(m->deviceCommandContext.get());

	m->textureManager.MakeUploadProgress(m->deviceCommandContext.get());

	// prepare before starting the renderer frame
	if (g_Game && g_Game->IsGameStarted())
	{
		const CGameView& gameView{*g_Game->GetView()};
		m->sceneRenderer.SetSceneCamera(gameView.GetCamera(), gameView.GetCullCamera());

		g_Game->CachePlayerColors();
	}

	if (g_Game)
		m->sceneRenderer.SetSimulation(g_Game->GetSimulation2());

	// start new frame
	BeginFrame();

	if (g_Game && g_Game->IsGameStarted())
	{
		CGameView& gameView{*g_Game->GetView()};
		gameView.EnumerateSceneObjects();

		m->sceneRenderer.PrepareSubmissions(m->deviceCommandContext.get());

		CPostprocManager& postprocManager{GetPostprocManager()};
		if (postprocManager.IsEnabled())
		{
			postprocManager.Initialize(m_Width, m_Height);
			RenderGameWithPostProcessingAndGUI(
				swapChain, gameView, postprocManager, renderGUI, renderLogger);
		}
		else
		{
			RenderGameAndGUI(swapChain, gameView, renderGUI, renderLogger);
		}
	}
	else
	{
		RenderGUIOnly(swapChain, renderGUI, renderLogger);
	}

	EndFrame();

	const Stats& stats = GetStats();
	PROFILE2_ATTR("draw calls: %zu", stats.m_DrawCalls);
	PROFILE2_ATTR("terrain tris: %zu", stats.m_TerrainTris);
	PROFILE2_ATTR("water tris: %zu", stats.m_WaterTris);
	PROFILE2_ATTR("model tris: %zu", stats.m_ModelTris);
	PROFILE2_ATTR("overlay tris: %zu", stats.m_OverlayTris);
	PROFILE2_ATTR("blend splats: %zu", stats.m_BlendSplats);
	PROFILE2_ATTR("particles: %zu", stats.m_Particles);

	g_Profiler2.RecordGPUFrameEnd(m->deviceCommandContext.get());

	m->linearAllocator.Release();
}

void CRenderer::RenderGameAndGUI(
	Renderer::Backend::ISwapChain& swapChain, CGameView& gameView,
	const bool renderGUI, const bool renderLogger)
{
	// We don't need to clear the color attachment of the framebuffer as the sky
	// is going to be rendered anyway.
	Renderer::Backend::IFramebuffer* framebuffer{
		swapChain.GetCurrentBackbuffer(
			Renderer::Backend::AttachmentLoadOp::DONT_CARE,
			Renderer::Backend::AttachmentStoreOp::STORE,
			Renderer::Backend::AttachmentLoadOp::CLEAR,
			Renderer::Backend::AttachmentStoreOp::DONT_CARE)};

	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = m_Width;
	viewportRect.height = m_Height;

	m->deviceCommandContext->BeginFramebufferPass(framebuffer);
	m->deviceCommandContext->SetViewports(1, &viewportRect);

	m->sceneRenderer.RenderSubmissions(m->deviceCommandContext.get());
	RenderGameOverlays(gameView);
	Render2D(renderGUI, renderLogger);

	m->deviceCommandContext->EndFramebufferPass();
}

void CRenderer::RenderGameWithPostProcessingAndGUI(
	Renderer::Backend::ISwapChain& swapChain, CGameView& gameView,
	CPostprocManager& postprocManager,
	const bool renderGUI, const bool renderLogger)
{
	ENSURE(postprocManager.IsEnabled());
	// We have to update the post process manager with real near/far planes
	// that we use for the scene rendering.
	postprocManager.SetDepthBufferClipPlanes(
		m->sceneRenderer.GetViewCamera().GetNearPlane(),
		m->sceneRenderer.GetViewCamera().GetFarPlane()
	);

	// In the first pass we render the game scene to a post processing texture
	// instead of a backbuffer to be able to apply effects to it.
	Renderer::Backend::IFramebuffer* framebuffer{postprocManager.PrepareAndGetOutputFramebuffer()};
	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = framebuffer->GetWidth();
	viewportRect.height = framebuffer->GetHeight();

	m->deviceCommandContext->BeginFramebufferPass(framebuffer);
	m->deviceCommandContext->SetViewports(1, &viewportRect);

	m->sceneRenderer.RenderSubmissions(m->deviceCommandContext.get());

	m->deviceCommandContext->EndFramebufferPass();

	if (postprocManager.IsMultisampleEnabled())
		postprocManager.ResolveMultisampleFramebuffer(m->deviceCommandContext.get());

	// In the second pass we apply our post processing. There might multiple
	// ping pong passes to apply different effects.
	postprocManager.ApplyPostproc(m->deviceCommandContext.get());

	// During the final pass we blit our post processing result to the
	// backbuffer and then draw all 2D over it.
	Renderer::Backend::IFramebuffer* backbuffer{
		swapChain.GetCurrentBackbuffer(
			Renderer::Backend::AttachmentLoadOp::LOAD,
			Renderer::Backend::AttachmentStoreOp::STORE,
			Renderer::Backend::AttachmentLoadOp::LOAD,
			Renderer::Backend::AttachmentStoreOp::DONT_CARE)};
	postprocManager.BlitOutputFramebuffer(
		m->deviceCommandContext.get(), backbuffer);

	m->deviceCommandContext->BeginFramebufferPass(backbuffer);

	viewportRect.width = m_Width;
	viewportRect.height = m_Height;
	m->deviceCommandContext->SetViewports(1, &viewportRect);

	RenderGameOverlays(gameView);
	Render2D(renderGUI, renderLogger);

	m->deviceCommandContext->EndFramebufferPass();
}

void CRenderer::RenderGUIOnly(
	Renderer::Backend::ISwapChain& swapChain,
	const bool renderGUI, const bool renderLogger)
{
	// We have a fullscreen background in our UI so we don't need
	// to clear the color attachment.
	// We don't need a depth test to render so we don't care about the
	// depth-stencil attachment content.
	// In case of Atlas we don't have g_Game, so we still need to clear depth.
	const Renderer::Backend::AttachmentLoadOp depthStencilLoadOp =
		g_AtlasGameLoop && g_AtlasGameLoop->view
			? Renderer::Backend::AttachmentLoadOp::CLEAR
			: Renderer::Backend::AttachmentLoadOp::DONT_CARE;
	Renderer::Backend::IFramebuffer* backbuffer =
		swapChain.GetCurrentBackbuffer(
			Renderer::Backend::AttachmentLoadOp::DONT_CARE,
			Renderer::Backend::AttachmentStoreOp::STORE,
			depthStencilLoadOp,
			Renderer::Backend::AttachmentStoreOp::DONT_CARE);
	m->deviceCommandContext->BeginFramebufferPass(backbuffer);

	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = m_Width;
	viewportRect.height = m_Height;
	m->deviceCommandContext->SetViewports(1, &viewportRect);

	Render2D(renderGUI, renderLogger);

	m->deviceCommandContext->EndFramebufferPass();
}

void CRenderer::RenderGameOverlays(CGameView& gameView)
{
	m->sceneRenderer.RenderSceneOverlays(m->deviceCommandContext.get());

	gameView.GetCinema()->Render(*m->deviceCommandContext);

	// If we're in Atlas game view, render special tools
	if (g_AtlasGameLoop && g_AtlasGameLoop->view)
	{
		g_AtlasGameLoop->view->DrawCinemaPathTool(*m->deviceCommandContext);
	}
}

void CRenderer::Render2D(const bool renderGUI, const bool renderLogger)
{
	CCanvas2D canvas(g_VideoMode.GetWindowWidth(), g_VideoMode.GetWindowHeight(), g_VideoMode.GetScale(), m->deviceCommandContext.get());

	m->sceneRenderer.RenderTextOverlays(canvas);

	if (renderGUI)
	{
		GPU_SCOPED_LABEL(m->deviceCommandContext.get(), "Render GUI");
		// All GUI elements are drawn in Z order to render semi-transparent
		// objects correctly.
		g_GUI->Draw(canvas);
	}

	// If we're in Atlas game view, render special overlays (e.g. editor bandbox).
	if (g_AtlasGameLoop && g_AtlasGameLoop->view)
	{
		g_AtlasGameLoop->view->DrawOverlays(canvas);
	}

	{
		GPU_SCOPED_LABEL(m->deviceCommandContext.get(), "Render console");
		g_Console->Render(canvas);
	}

	if (renderLogger)
	{
		GPU_SCOPED_LABEL(m->deviceCommandContext.get(), "Render logger");
		g_Logger->Render(canvas);
	}

	{
		GPU_SCOPED_LABEL(m->deviceCommandContext.get(), "Render profiler");
		// Profile information
		g_ProfileViewer.RenderProfile(canvas);
	}

	GetFontManager().UploadAtlasTexturesToGPU(m->deviceCommandContext.get());
}

void CRenderer::RenderScreenShot(const bool needsPresent)
{
	m->m_ScreenShotType = ScreenShotType::NONE;

	// get next available numbered filename
	// note: %04d -> always 4 digits, so sorting by filename works correctly.
	const VfsPath filenameFormat(L"screenshots/screenshot%04d.png");
	VfsPath filename;
	vfs::NextNumberedFilename(g_VFS, filenameFormat, g_NextScreenShotNumber, filename);

	const size_t width = static_cast<size_t>(g_VideoMode.GetWindowWidth()), height = static_cast<size_t>(g_VideoMode.GetWindowHeight());
	const size_t bpp = 24;

	const size_t img_size = width * height * bpp / 8;
	const size_t hdr_size = tex_hdr_size(filename);
	std::shared_ptr<std::uint8_t> buf;
	AllocateAligned(buf, hdr_size + img_size, maxSectorSize);
	void* img = buf.get() + hdr_size;
	Tex t;
	if (t.wrap(width, height, bpp, TEX_BOTTOM_UP, buf, hdr_size) < 0)
		return;

	Renderer::Backend::ISwapChain* swapChain{g_VideoMode.GetOrCreateSwapChain()};
	if (!swapChain || !swapChain->IsValid())
		return;

	if (needsPresent && !swapChain->AcquireNextBackbuffer())
		return;

	// Hide log messages and re-render
	RenderFrameImpl(*swapChain, false, false);

	m->deviceCommandContext->ReadbackFramebufferSync(*swapChain, 0, 0, width, height, img);
	m->deviceCommandContext->Flush();
	if (needsPresent)
		swapChain->Present();

	if (tex_write(&t, filename) == INFO::OK)
	{
		OsPath realPath;
		g_VFS->GetRealPath(filename, realPath);

		LOGMESSAGERENDER("Screenshot written to '%s'", realPath.string8());

		debug_printf(
			CStr(g_L10n.Translate("Screenshot written to '%s'") + "\n").c_str(),
			realPath.string8().c_str());
	}
	else
		LOGERROR("Error writing screenshot to '%s'", filename.string8());
}

void CRenderer::RenderBigScreenShot(const bool needsPresent)
{
	m->m_ScreenShotType = ScreenShotType::NONE;

	// If the game hasn't started yet then use WriteScreenshot to generate the image.
	if (!g_Game)
		return RenderScreenShot(needsPresent);

	const int tiles{g_ConfigDB.Get("screenshot.tiles", 4)};
	const int tileWidth{g_ConfigDB.Get("screenshot.tilewidth", 256)};
	const int tileHeight{g_ConfigDB.Get("screenshot.tileheight", 256)};
	if (tiles <= 0 || tileWidth <= 0 || tileHeight <= 0 || tileWidth * tiles % 4 != 0 || tileHeight * tiles % 4 != 0)
	{
		LOGWARNING("Invalid big screenshot size: tiles=%d tileWidth=%d tileHeight=%d", tiles, tileWidth, tileHeight);
		return;
	}

	if (g_VideoMode.GetWindowWidth() < tileWidth && g_VideoMode.GetWindowHeight() < tileHeight)
	{
		LOGWARNING(
			"The window size is too small for a big screenshot, increase the"
			" window size %dx%d or decrease the tile size %dx%d",
			g_VideoMode.GetWindowWidth(), g_VideoMode.GetWindowHeight(), tileWidth, tileHeight);
		return;
	}

	// get next available numbered filename
	// note: %04d -> always 4 digits, so sorting by filename works correctly.
	const VfsPath filenameFormat(L"screenshots/screenshot%04d.bmp");
	VfsPath filename;
	vfs::NextNumberedFilename(g_VFS, filenameFormat, g_NextScreenShotNumber, filename);

	const int imageWidth = tileWidth * tiles, imageHeight = tileHeight * tiles;
	const int bpp = 24;

	const size_t imageSize = imageWidth * imageHeight * bpp / 8;
	const size_t tileSize = tileWidth * tileHeight * bpp / 8;
	const size_t headerSize = tex_hdr_size(filename);
	void* tileData = malloc(tileSize);
	if (!tileData)
	{
		WARN_IF_ERR(ERR::NO_MEM);
		return;
	}
	std::shared_ptr<std::uint8_t> imageBuffer;
	AllocateAligned(imageBuffer, headerSize + imageSize, maxSectorSize);

	Tex t;
	void* img = imageBuffer.get() + headerSize;
	if (t.wrap(imageWidth, imageHeight, bpp, TEX_BOTTOM_UP, imageBuffer, headerSize) < 0)
	{
		free(tileData);
		return;
	}

	const CCamera oldCamera{g_Game->GetView()->GetCamera()};

	// Resize various things so that the sizes and aspect ratios are correct
	{
		DestroyViewSizeDependentObjects();
		m->device->WaitUntilIdle();
		CreateViewSizeDependentObjects(tileWidth, tileHeight);

		SViewPort vp = { 0, 0, tileWidth, tileHeight };
		g_Game->GetView()->SetViewport(vp);
	}

	// Render each tile
	CMatrix3D projection;
	projection.SetIdentity();
	const float aspectRatio = 1.0f * tileWidth / tileHeight;
	for (int tileY = 0; tileY < tiles; ++tileY)
	{
		for (int tileX = 0; tileX < tiles; ++tileX)
		{
			// Adjust the camera to render the appropriate region
			if (oldCamera.GetProjectionType() == CCamera::ProjectionType::PERSPECTIVE)
			{
				projection.SetPerspectiveTile(
					oldCamera.GetFOV(), aspectRatio, oldCamera.GetNearPlane(), oldCamera.GetFarPlane(),
					tiles, tileX, tileY);
			}
			CCamera camera{g_Game->GetView()->GetCamera()};
			camera.SetProjection(projection);
			g_Game->GetView()->SetCamera(camera);

			Renderer::Backend::ISwapChain* swapChain{g_VideoMode.GetOrCreateSwapChain()};
			if (!swapChain || !swapChain->IsValid())
				continue;

			if (!needsPresent || swapChain->AcquireNextBackbuffer())
			{
				RenderFrameImpl(*swapChain, false, false);

				m->deviceCommandContext->ReadbackFramebufferSync(*swapChain, 0, 0, tileWidth, tileHeight, tileData);
				m->deviceCommandContext->Flush();

				if (needsPresent)
					swapChain->Present();
			}

			// Copy the tile pixels into the main image
			for (int y = 0; y < tileHeight; ++y)
			{
				void* dest = static_cast<char*>(img) + ((tileY * tileHeight + y) * imageWidth + (tileX * tileWidth)) * bpp / 8;
				void* src = static_cast<char*>(tileData) + y * tileWidth * bpp / 8;
				memcpy(dest, src, tileWidth * bpp / 8);
			}
		}
	}

	// Restore the viewport settings
	{
		DestroyViewSizeDependentObjects();
		m->device->WaitUntilIdle();

		SViewPort vp = { 0, 0, g_VideoMode.GetWindowWidth(), g_VideoMode.GetWindowHeight() };
		CreateViewSizeDependentObjects(vp.m_Width, vp.m_Height);

		g_Game->GetView()->SetViewport(vp);
		g_Game->GetView()->SetCamera(oldCamera);
	}

	if (tex_write(&t, filename) == INFO::OK)
	{
		OsPath realPath;
		g_VFS->GetRealPath(filename, realPath);

		LOGMESSAGERENDER("Screenshot written to '%s'", realPath.string8());

		debug_printf(
			CStr(g_L10n.Translate("Screenshot written to '%s'") + "\n").c_str(),
			realPath.string8().c_str());
	}
	else
		LOGERROR("Error writing screenshot to '%s'", filename.string8());

	free(tileData);
}

void CRenderer::BeginFrame()
{
	PROFILE("begin frame");

	// Zero out all the per-frame stats.
	m_Stats.Reset();

	if (m->ShadersDirty)
		ReloadShaders();

	m->sceneRenderer.BeginFrame();
}

void CRenderer::EndFrame()
{
	PROFILE3("end frame");

	m->sceneRenderer.EndFrame();

	m->linearAllocator.Release();

	m->backendProfileTable.MakeOutdated();
}

void CRenderer::MakeShadersDirty()
{
	m->ShadersDirty = true;
	m->sceneRenderer.MakeShadersDirty();
}

CTextureManager& CRenderer::GetTextureManager()
{
	return m->textureManager;
}

CVertexBufferManager& CRenderer::GetVertexBufferManager()
{
	return m->vertexBufferManager;
}

CShaderManager& CRenderer::GetShaderManager()
{
	return m->shaderManager;
}

CTimeManager& CRenderer::GetTimeManager()
{
	return m->timeManager;
}

CPostprocManager& CRenderer::GetPostprocManager()
{
	return m->postprocManager;
}

CSceneRenderer& CRenderer::GetSceneRenderer()
{
	return m->sceneRenderer;
}

CDebugRenderer& CRenderer::GetDebugRenderer()
{
	return m->debugRenderer;
}

CFontManager& CRenderer::GetFontManager()
{
	return m->fontManager;
}

void CRenderer::PreloadResourcesBeforeNextFrame()
{
	m_ShouldPreloadResourcesBeforeNextFrame = true;
}

void CRenderer::MakeScreenShotOnNextFrame(ScreenShotType screenShotType)
{
	m->m_ScreenShotType = screenShotType;
}

Renderer::Backend::IDeviceCommandContext* CRenderer::GetDeviceCommandContext()
{
	return m->deviceCommandContext.get();
}

Renderer::Backend::IVertexInputLayout* CRenderer::GetVertexInputLayout(
	const std::span<const Renderer::Backend::SVertexAttributeFormat> attributes)
{
	const auto [it, inserted] = m->vertexInputLayouts.emplace(
		std::vector<Renderer::Backend::SVertexAttributeFormat>{attributes.begin(), attributes.end()}, nullptr);
	if (inserted)
		it->second = m->device->CreateVertexInputLayout(attributes);
	return it->second.get();
}

PS::Memory::LinearAllocator& CRenderer::GetLinearAllocator()
{
	return m->linearAllocator;
}
