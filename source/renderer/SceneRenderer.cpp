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

#include "SceneRenderer.h"

#include "graphics/Camera.h"
#include "graphics/Color.h"
#include "graphics/LOSTexture.h"
#include "graphics/LightEnv.h"
#include "graphics/Material.h"
#include "graphics/MaterialManager.h"
#include "graphics/MiniMapTexture.h"
#include "graphics/Model.h"
#include "graphics/ModelAbstract.h"
#include "graphics/ModelDef.h"
#include "graphics/ParticleManager.h"
#include "graphics/Patch.h"
#include "graphics/ShaderDefines.h"
#include "graphics/TerritoryTexture.h"
#include "lib/code_annotation.h"
#include "lib/config2.h"
#include "lib/debug.h"
#include "maths/Frustum.h"
#include "maths/MathUtil.h"
#include "maths/Matrix3D.h"
#include "maths/Plane.h"
#include "maths/Vector3D.h"
#include "maths/Vector4D.h"
#include "ps/CStrIntern.h"
#include "ps/CStrInternStatic.h"
#include "ps/Game.h"
#include "ps/Profile.h"
#include "ps/QuestVR.h"
#include "renderer/CPUSkinnedModelRenderer.h"
#include "renderer/DebugRenderer.h"
#include "renderer/GPUSkinnedModelRenderer.h"
#include "renderer/InstancingModelRenderer.h"
#include "renderer/ModelRenderer.h"
#include "renderer/OverlayRenderer.h"
#include "renderer/ParticleRenderer.h"
#include "renderer/RenderModifiers.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"
#include "renderer/ShadowMap.h"
#include "renderer/SilhouetteRenderer.h"
#include "renderer/SkyManager.h"
#include "renderer/TerrainOverlay.h"
#include "renderer/TerrainRenderer.h"
#include "renderer/WaterManager.h"
#include "renderer/backend/Backend.h"
#include "renderer/backend/IDevice.h"
#include "renderer/backend/IDeviceCommandContext.h"

#include <algorithm>
#include <cmath>
#include <optional>

struct SScreenRect
{
	int x1, y1, x2, y2;
};

/**
 * Struct CSceneRendererInternals: Truly hide data that is supposed to be hidden
 * in this structure so it won't even appear in header files.
 */
class CSceneRenderer::Internals
{
	NONCOPYABLE(Internals);
public:
	Internals(Renderer::Backend::IDevice* device)
		: waterManager(device), particleManager(*device), shadow(device)
	{
	}

	~Internals() = default;

	/// Water manager
	WaterManager waterManager;

	/// Sky manager
	SkyManager skyManager;

	/// Terrain renderer
	TerrainRenderer terrainRenderer;

	/// Overlay renderer
	OverlayRenderer overlayRenderer;

	/// Particle manager
	CParticleManager particleManager;

	/// Particle renderer
	ParticleRenderer particleRenderer;

	/// Material manager
	CMaterialManager materialManager;

	/// Shadow map
	ShadowMap shadow;

	SilhouetteRenderer silhouetteRenderer;

	// Various model renderers
	struct Models
	{
		// Submitted models are split on two axes:
		//  - Opaque vs Transparent - alpha-blended models are stored in a separate
		//    list so we can draw them above/below the alpha-blended water plane correctly
		//  - Skinned vs Unskinned - we don't need to duplicate mesh data per
		//    model instance (except for skinned models), so non-skinned models
		//    get different ModelVertexRenderers

		struct Submissions
		{
			std::vector<CModel*> submissions[CSceneRenderer::CULL_MAX];
		};

		// Unskinned submissions should be prepared and rendered with
		// VertexInstancingShader. Skinned - with Vertex*SkinningShader
		// depending on whether GPU skinning is enabled.
		Submissions OpaqueSkinned;
		Submissions OpaqueUnskinned;
		Submissions TransparentSkinned;
		Submissions TransparentUnskinned;

		std::vector<CModel*> uniqueSkinnedSubmissionsForUpdate;

		ModelRenderer modelRenderer;

		InstancingModelRenderer VertexInstancingShader;
		CPUSkinnedModelVertexRenderer VertexCPUSkinningShader;
		// We can't create GPU skinning renderer for renderer devices without
		// its support.
		std::optional<GPUSkinnedModelModelRenderer> VertexGPUSkinningShader;

		ShaderRenderModifier ModShader;

		bool GPUSkinningEnabled{false};
	} Model;

	CShaderDefines globalContext;

	/**
	 * Upload renderer data for all previously submitted models to backend.
	 *
	 * Must be called before any rendering calls and after all models
	 * for this frame have been prepared.
	 */
	void UploadModels(Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
	{
		PROFILE3("upload models");

		ModelVertexRenderer& modelVertexSkinningRenderer{
			Model.GPUSkinningEnabled
				? static_cast<ModelVertexRenderer&>(*Model.VertexGPUSkinningShader)
				: static_cast<ModelVertexRenderer&>(Model.VertexCPUSkinningShader)};
		modelVertexSkinningRenderer.UploadModelsData(deviceCommandContext, Model.uniqueSkinnedSubmissionsForUpdate);

		for (int cullGroup{0}; cullGroup < CSceneRenderer::CULL_MAX; ++cullGroup)
		{
			Model.VertexInstancingShader.UploadModelsData(deviceCommandContext, Model.OpaqueUnskinned.submissions[cullGroup]);
			Model.VertexInstancingShader.UploadModelsData(deviceCommandContext, Model.TransparentUnskinned.submissions[cullGroup]);
		}
	}

	/**
	 * PrepareModels: Calculate renderer data for all previously
	 * submitted models.
	 *
	 * Must be called before any rendering calls and after all models
	 * for this frame have been submitted.
	 */
	void PrepareModels(
		Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
	{
		PROFILE3("prepare models");

		ModelVertexRenderer& modelVertexSkinningRenderer{
			Model.GPUSkinningEnabled
				? static_cast<ModelVertexRenderer&>(*Model.VertexGPUSkinningShader)
				: static_cast<ModelVertexRenderer&>(Model.VertexCPUSkinningShader)};

		for (int cullGroup{0}; cullGroup < CSceneRenderer::CULL_MAX; ++cullGroup)
		{
			PrepareModels(modelVertexSkinningRenderer, Model.OpaqueSkinned.submissions[cullGroup]);
			PrepareModels(modelVertexSkinningRenderer, Model.TransparentSkinned.submissions[cullGroup]);
		}

		modelVertexSkinningRenderer.UpdateModelsData(deviceCommandContext, Model.uniqueSkinnedSubmissionsForUpdate);

		// See CPUSkinnedModelVertexRenderer::PrepareForRendering comment.
		if (!Model.GPUSkinningEnabled)
		{
			for (int cullGroup{0}; cullGroup < CSceneRenderer::CULL_MAX; ++cullGroup)
			{
				Model.VertexCPUSkinningShader.PrepareForRendering(Model.OpaqueSkinned.submissions[cullGroup]);
				Model.VertexCPUSkinningShader.PrepareForRendering(Model.TransparentSkinned.submissions[cullGroup]);
			}
		}

		// We don't need to update unskinned models.
	}

	void PrepareModels(
		ModelVertexRenderer& modelVertexRenderer, std::span<CModel*> submissions)
	{
		for (CModel* model : submissions)
		{
			model->ValidatePosition();

			CModelRData* rdata{static_cast<CModelRData*>(model->GetRenderData())};
			ENSURE(rdata->GetKey() == &modelVertexRenderer);

			if (rdata->m_UpdateFlags & RENDERDATA_UPDATE_VERTICES)
			{
				Model.uniqueSkinnedSubmissionsForUpdate.emplace_back(model);
				rdata->m_UpdateFlags = 0;
			}
		}
	}

	/**
	 * Submit: Submit a model for rendering this frame.
	 *
	 * preconditions : The model must not have been submitted to any
	 * ModelRenderer in this frame. Submit may only be called
	 * after EndFrame and before PrepareModels.
	 *
	 * @param model The model that will be added to the list of models
	 * submitted this frame.
	 */
	void Submit(const int cullGroup, ModelVertexRenderer& modelVertexRenderer, Models::Submissions& submissions, CModel* model)
	{
		CModelRData* rdata{static_cast<CModelRData*>(model->GetRenderData())};

		// Ensure model data is valid.
		// TODO: using a pointer as a key is unsafe.
		const void* key{&modelVertexRenderer};
		if (!rdata || rdata->GetKey() != key)
		{
			model->InvalidatePosition();
			rdata = modelVertexRenderer.CreateModelData(key, model);
			model->SetRenderData(rdata);
			model->SetDirty(~0u);
		}

		submissions.submissions[cullGroup].push_back(model);
	}

	/**
	 * Renders all non-alpha-blended models with the given context.
	 */
	void CallOpaqueModelRenderers(
		Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
		const CShaderDefines& context, int cullGroup, int flags, const ERenderMode renderMode)
	{
		ModelVertexRenderer& modelVertexSkinningRenderer{
			Model.GPUSkinningEnabled
				? static_cast<ModelVertexRenderer&>(*Model.VertexGPUSkinningShader)
				: static_cast<ModelVertexRenderer&>(Model.VertexCPUSkinningShader)};

		CShaderDefines contextSkinned = context;
		if (Model.GPUSkinningEnabled)
			contextSkinned.Add(str_USE_INSTANCING, str_1);
		Model.modelRenderer.Render(deviceCommandContext, modelVertexSkinningRenderer, Model.ModShader, contextSkinned, cullGroup, flags, renderMode, Model.OpaqueSkinned.submissions[cullGroup]);

		CShaderDefines contextUnskinned = context;
		contextUnskinned.Add(str_USE_INSTANCING, str_1);
		Model.modelRenderer.Render(deviceCommandContext, Model.VertexInstancingShader, Model.ModShader, contextUnskinned, cullGroup, flags, renderMode, Model.OpaqueUnskinned.submissions[cullGroup]);
	}

	/**
	 * Renders all alpha-blended models with the given context.
	 */
	void CallTransparentModelRenderers(
		Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
		const CShaderDefines& context, int cullGroup, int flags, const ERenderMode renderMode)
	{
		ModelVertexRenderer& modelVertexSkinningRenderer{
			Model.GPUSkinningEnabled
				? static_cast<ModelVertexRenderer&>(*Model.VertexGPUSkinningShader)
				: static_cast<ModelVertexRenderer&>(Model.VertexCPUSkinningShader)};

		CShaderDefines contextSkinned = context;
		if (Model.GPUSkinningEnabled)
			contextSkinned.Add(str_USE_INSTANCING, str_1);
		Model.modelRenderer.Render(deviceCommandContext, modelVertexSkinningRenderer, Model.ModShader, contextSkinned, cullGroup, flags, renderMode, Model.TransparentSkinned.submissions[cullGroup]);

		CShaderDefines contextUnskinned = context;
		contextUnskinned.Add(str_USE_INSTANCING, str_1);
		Model.modelRenderer.Render(deviceCommandContext, Model.VertexInstancingShader, Model.ModShader, contextUnskinned, cullGroup, flags, renderMode, Model.TransparentUnskinned.submissions[cullGroup]);
	}
};

CSceneRenderer::CSceneRenderer(Renderer::Backend::IDevice* device)
{
	m = std::make_unique<Internals>(device);

	m_TerrainRenderMode = SOLID;
	m_WaterRenderMode = SOLID;
	m_ModelRenderMode = SOLID;
	m_OverlayRenderMode = SOLID;

	m_DisplayTerrainPriorities = false;

	m_LightEnv = nullptr;

	m_CurrentScene = nullptr;
}

CSceneRenderer::~CSceneRenderer()
{
	// We no longer UnloadWaterTextures here -
	// that is the responsibility of the module that asked for
	// them to be loaded (i.e. CGameView).
	m.reset();
}

void CSceneRenderer::ReloadShaders([[maybe_unused]] Renderer::Backend::IDevice* device)
{
	m->globalContext = CShaderDefines();

	if (g_RenderingOptions.GetShadows())
	{
		m->globalContext.Add(str_USE_SHADOW, str_1);
		if (g_RenderingOptions.GetShadowPCF())
			m->globalContext.Add(str_USE_SHADOW_PCF, str_1);
		const int cascadeCount = m->shadow.GetCascadeCount();
		ENSURE(1 <= cascadeCount && cascadeCount <= 4);
		const CStrIntern cascadeCountStr[5] = {str_0, str_1, str_2, str_3, str_4};
		m->globalContext.Add(str_SHADOWS_CASCADE_COUNT, cascadeCountStr[cascadeCount]);
#if CONFIG2_GLES
		const bool useShadowSampler{device->GetBackend() != Renderer::Backend::Backend::GL};
#else
		const bool useShadowSampler{true};
#endif
		if (useShadowSampler)
			m->globalContext.Add(str_USE_SHADOW_SAMPLER, str_1);
	}

	m->globalContext.Add(str_RENDER_DEBUG_MODE,
		RenderDebugModeEnum::ToString(g_RenderingOptions.GetRenderDebugMode()));

	m->Model.GPUSkinningEnabled = g_RenderingOptions.GetGPUSkinning();
	if (m->Model.GPUSkinningEnabled)
	{
		if (!m->Model.VertexGPUSkinningShader.has_value())
			m->Model.VertexGPUSkinningShader.emplace();
	}
}

void CSceneRenderer::Initialize(const uint32_t width, const uint32_t height)
{
	// Let component renderers perform one-time initialization after graphics capabilities and
	// the shader path have been determined.
	m->waterManager.Initialize();
	m->terrainRenderer.Initialize();
	m->overlayRenderer.Initialize();

	CreateViewSizeDependentObjects(width, height);
}

void CSceneRenderer::CreateViewSizeDependentObjects(const uint32_t width, const uint32_t height)
{
	// need to recreate the shadow map object to resize the shadow texture
	m->shadow.CreateViewSizeDependentObjects(width, height);

	m->waterManager.CreateViewSizeDependentObjects(width, height);
}

void CSceneRenderer::DestroyViewSizeDependentObjects()
{
	m->waterManager.DestroyViewSizeDependentObjects();

	m->shadow.DestroyViewSizeDependentObjects();
}

void CSceneRenderer::BeginFrame()
{
	// choose model renderers for this frame
	m->Model.ModShader.SetShadowMap(&m->shadow);
	m->Model.ModShader.SetLightEnv(m_LightEnv);
}

void CSceneRenderer::SetSimulation(CSimulation2* simulation)
{
	// set current simulation context for terrain renderer
	m->terrainRenderer.SetSimulation(simulation);
}

void CSceneRenderer::RenderShadowMap(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
{
	PROFILE3_GPU(deviceCommandContext, "shadow map");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render shadow map");

	m->shadow.BeginRender(deviceCommandContext);

	const int cascadeCount = m->shadow.GetCascadeCount();
	ENSURE(0 <= cascadeCount && cascadeCount <= 4);
	for (int cascade = 0; cascade < cascadeCount; ++cascade)
	{
		m->shadow.PrepareCamera(deviceCommandContext, cascade);

		const int cullGroup = CULL_SHADOWS_CASCADE_0 + cascade;
		{
			PROFILE("render patches");
			m->terrainRenderer.RenderPatches(
				deviceCommandContext, cullGroup, {}, CColor(0.0f, 0.0f, 0.0f, 1.0f), false);
		}

		{
			PROFILE("render models");
			m->CallOpaqueModelRenderers(deviceCommandContext, {}, cullGroup, ModelFlag::CAST_SHADOWS, ERenderMode::SOLID);
		}

		{
			PROFILE("render transparent models");
			m->CallTransparentModelRenderers(deviceCommandContext, {}, cullGroup, ModelFlag::CAST_SHADOWS, ERenderMode::SOLID);
		}
	}

	m->shadow.EndRender(deviceCommandContext);
}

void CSceneRenderer::RenderPatches(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	const CShaderDefines& context, int cullGroup)
{
	PROFILE3("patches");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render patches");

	// Render all the patches, including blend pass.
	m->terrainRenderer.RenderTerrainShader(deviceCommandContext, context, cullGroup,
		g_RenderingOptions.GetShadows() ? &m->shadow : nullptr, m_TerrainRenderMode == WIREFRAME);

	if (m_TerrainRenderMode == EDGED_FACES)
	{
		// Edged faces: need to make a second pass over the data.

		// Render tiles edges.
		m->terrainRenderer.RenderPatches(
			deviceCommandContext, cullGroup, context, CColor(0.5f, 0.5f, 1.0f, 1.0f), true);

		// Render outline of each patch.
		m->terrainRenderer.RenderOutlines(deviceCommandContext, cullGroup);
	}
}

void CSceneRenderer::RenderModels(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	const CShaderDefines& context, int cullGroup)
{
	PROFILE3("models");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render models");

	int flags = 0;

	const ERenderMode modelRenderMode{
		m_ModelRenderMode == WIREFRAME ? WIREFRAME : SOLID};

	m->CallOpaqueModelRenderers(deviceCommandContext, context, cullGroup, flags, modelRenderMode);

	if (m_ModelRenderMode == EDGED_FACES)
		m->CallOpaqueModelRenderers(deviceCommandContext, {}, cullGroup, flags, EDGED_FACES);
}

void CSceneRenderer::RenderTransparentModels(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	const CShaderDefines& context, int cullGroup, ETransparentMode transparentMode)
{
	PROFILE3("transparent models");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render transparent models");

	int flags = 0;

	CShaderDefines contextOpaque = context;
	contextOpaque.Add(str_ALPHABLEND_PASS_OPAQUE, str_1);

	CShaderDefines contextBlend = context;
	contextBlend.Add(str_ALPHABLEND_PASS_BLEND, str_1);

	const ERenderMode modelRenderMode{
		m_ModelRenderMode == WIREFRAME ? WIREFRAME : SOLID};

	if (transparentMode == TRANSPARENT || transparentMode == TRANSPARENT_OPAQUE)
		m->CallTransparentModelRenderers(deviceCommandContext, contextOpaque, cullGroup, flags, modelRenderMode);

	if (transparentMode == TRANSPARENT || transparentMode == TRANSPARENT_BLEND)
		m->CallTransparentModelRenderers(deviceCommandContext, contextBlend, cullGroup, flags, modelRenderMode);

	if (m_ModelRenderMode == EDGED_FACES)
		m->CallTransparentModelRenderers(deviceCommandContext, {}, cullGroup, flags, EDGED_FACES);
}

// SetObliqueFrustumClipping: change the near plane to the given clip plane (in world space)
// Based on code from Game Programming Gems 5, from http://www.terathon.com/code/oblique.html
// - worldPlane is a clip plane in world space (worldPlane.Dot(v) >= 0 for any vector v passing the clipping test)
void CSceneRenderer::SetObliqueFrustumClipping(CCamera& camera, const CVector4D& worldPlane) const
{
	// First, we'll convert the given clip plane to camera space, then we'll
	// Get the view matrix and normal matrix (top 3x3 part of view matrix)
	CMatrix3D normalMatrix = camera.GetOrientation().GetTranspose();
	CVector4D camPlane = normalMatrix.Transform(worldPlane);

	CMatrix3D matrix = camera.GetProjection();

	// Calculate the clip-space corner point opposite the clipping plane
	// as (sgn(camPlane.x), sgn(camPlane.y), 1, 1) and
	// transform it into camera space by multiplying it
	// by the inverse of the projection matrix

	CVector4D q;
	q.X = (Sign(camPlane.X) - matrix[8] / matrix[11]) / matrix[0];
	q.Y = (Sign(camPlane.Y) - matrix[9] / matrix[11]) / matrix[5];
	q.Z = 1.0f / matrix[11];
	q.W = (1.0f - matrix[10] / matrix[11]) / matrix[14];

	// Calculate the scaled plane vector
	CVector4D c = camPlane * (2.0f * matrix[11] / camPlane.Dot(q));

	// Replace the third row of the projection matrix
	matrix[2] = c.X;
	matrix[6] = c.Y;
	matrix[10] = c.Z - matrix[11];
	matrix[14] = c.W;

	// Load it back into the camera
	camera.SetProjection(matrix);
}

void CSceneRenderer::ComputeReflectionCamera(CCamera& camera, const CBoundingBoxAligned& scissor) const
{
	WaterManager& wm = m->waterManager;

	CMatrix3D projection;
	if (m_ViewCamera.GetProjectionType() == CCamera::ProjectionType::PERSPECTIVE)
	{
		const float aspectRatio = 1.0f;
		// Expand fov slightly since ripples can reflect parts of the scene that
		// are slightly outside the normal camera view, and we want to avoid any
		// noticeable edge-filtering artifacts
		projection.SetPerspective(m_ViewCamera.GetFOV() * 1.05f, aspectRatio, m_ViewCamera.GetNearPlane(), m_ViewCamera.GetFarPlane());
	}
	else
		projection = m_ViewCamera.GetProjection();

	camera = m_ViewCamera;

	// Temporarily change the camera to one that is reflected.
	// Also, for texturing purposes, make it render to a view port the size of the
	// water texture, stretch the image according to our aspect ratio so it covers
	// the whole screen despite being rendered into a square, and cover slightly more
	// of the view so we can see wavy reflections of slightly off-screen objects.
	camera.m_Orientation.Scale(1, -1, 1);
	camera.m_Orientation.Translate(0, 2 * wm.m_WaterHeight, 0);
	camera.UpdateFrustum(scissor);
	// Clip slightly above the water to improve reflections of objects on the water
	// when the reflections are distorted.
	camera.ClipFrustum(CVector4D(0, 1, 0, -wm.m_WaterHeight + 2.0f));

	SViewPort vp;
	vp.m_Height = wm.m_RefTextureSize;
	vp.m_Width = wm.m_RefTextureSize;
	vp.m_X = 0;
	vp.m_Y = 0;
	camera.SetViewPort(vp);
	camera.SetProjection(projection);
	CMatrix3D scaleMat;
	scaleMat.SetScaling(g_Renderer.GetHeight() / static_cast<float>(std::max(1, g_Renderer.GetWidth())), 1.0f, 1.0f);
	camera.SetProjection(scaleMat * camera.GetProjection());

	CVector4D camPlane(0, 1, 0, -wm.m_WaterHeight + 0.5f);
	SetObliqueFrustumClipping(camera, camPlane);
}

void CSceneRenderer::ComputeRefractionCamera(CCamera& camera, const CBoundingBoxAligned& scissor) const
{
	WaterManager& wm = m->waterManager;

	CMatrix3D projection;
	if (m_ViewCamera.GetProjectionType() == CCamera::ProjectionType::PERSPECTIVE)
	{
		const float aspectRatio = 1.0f;
		// Expand fov slightly since ripples can reflect parts of the scene that
		// are slightly outside the normal camera view, and we want to avoid any
		// noticeable edge-filtering artifacts
		projection.SetPerspective(m_ViewCamera.GetFOV() * 1.05f, aspectRatio, m_ViewCamera.GetNearPlane(), m_ViewCamera.GetFarPlane());
	}
	else
		projection = m_ViewCamera.GetProjection();

	camera = m_ViewCamera;

	// Temporarily change the camera to make it render to a view port the size of the
	// water texture, stretch the image according to our aspect ratio so it covers
	// the whole screen despite being rendered into a square, and cover slightly more
	// of the view so we can see wavy refractions of slightly off-screen objects.
	camera.UpdateFrustum(scissor);
	camera.ClipFrustum(CVector4D(0, -1, 0, wm.m_WaterHeight + 0.5f));	// add some to avoid artifacts near steep shores.

	SViewPort vp;
	vp.m_Height = wm.m_RefTextureSize;
	vp.m_Width = wm.m_RefTextureSize;
	vp.m_X = 0;
	vp.m_Y = 0;
	camera.SetViewPort(vp);
	camera.SetProjection(projection);
	CMatrix3D scaleMat;
	scaleMat.SetScaling(g_Renderer.GetHeight() / static_cast<float>(std::max(1, g_Renderer.GetWidth())), 1.0f, 1.0f);
	camera.SetProjection(scaleMat * camera.GetProjection());
}

// RenderReflections: render the water reflections to the reflection texture
void CSceneRenderer::RenderReflections(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	const CShaderDefines& context, const CBoundingBoxAligned& scissor)
{
	PROFILE3_GPU(deviceCommandContext, "water reflections");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render water reflections");

	WaterManager& wm = m->waterManager;

	// Remember old camera
	CCamera normalCamera = m_ViewCamera;

	ComputeReflectionCamera(m_ViewCamera, scissor);
	const CBoundingBoxAligned reflectionScissor =
		m->terrainRenderer.ScissorWater(CULL_DEFAULT, m_ViewCamera);
	if (reflectionScissor.IsEmpty())
	{
		if (!wm.m_ReflectionFramebufferInitialized)
		{
			wm.m_ReflectionFramebufferInitialized = true;
			deviceCommandContext->BeginFramebufferPass(wm.m_ReflectionFramebuffer.get());
			deviceCommandContext->EndFramebufferPass();
		}
		m_ViewCamera = normalCamera;
		return;
	}

	// Save the model-view-projection matrix so the shaders can use it for projective texturing
	wm.m_ReflectionMatrix = m_ViewCamera.GetViewProjection();
	if (deviceCommandContext->GetDevice()->GetBackend() == Renderer::Backend::Backend::VULKAN)
	{
		CMatrix3D flip;
		flip.SetIdentity();
		flip._22 = -1.0f;
		wm.m_ReflectionMatrix = flip * wm.m_ReflectionMatrix;
	}

	float vpHeight = wm.m_RefTextureSize;
	float vpWidth = wm.m_RefTextureSize;

	SScreenRect screenScissor;
	screenScissor.x1 = static_cast<int>(floor((reflectionScissor[0].X * 0.5f + 0.5f) * vpWidth));
	screenScissor.y1 = static_cast<int>(floor((reflectionScissor[0].Y * 0.5f + 0.5f) * vpHeight));
	screenScissor.x2 = static_cast<int>(ceil((reflectionScissor[1].X * 0.5f + 0.5f) * vpWidth));
	screenScissor.y2 = static_cast<int>(ceil((reflectionScissor[1].Y * 0.5f + 0.5f) * vpHeight));

	deviceCommandContext->BeginFramebufferPass(wm.m_ReflectionFramebuffer.get());

	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = vpWidth;
	viewportRect.height = vpHeight;
	deviceCommandContext->SetViewports(1, &viewportRect);

	Renderer::Backend::IDeviceCommandContext::Rect scissorRect;
	scissorRect.x = screenScissor.x1;
	scissorRect.y = screenScissor.y1;
	scissorRect.width = screenScissor.x2 - screenScissor.x1;
	scissorRect.height = screenScissor.y2 - screenScissor.y1;
	deviceCommandContext->SetScissors(1, &scissorRect);

	// Render terrain and models
	RenderPatches(deviceCommandContext, context, CULL_REFLECTIONS);
	RenderModels(deviceCommandContext, context, CULL_REFLECTIONS);
	RenderTransparentModels(deviceCommandContext, context, CULL_REFLECTIONS, TRANSPARENT);

	// Particles are always oriented to face the camera in the vertex shader,
	// so they don't need the inverted cull face.
	if (g_RenderingOptions.GetParticles())
	{
		RenderParticles(deviceCommandContext, CULL_REFLECTIONS);
	}

	deviceCommandContext->SetScissors(0, nullptr);
	deviceCommandContext->EndFramebufferPass();

	wm.m_ReflectionFramebufferInitialized = true;

	// Reset old camera
	m_ViewCamera = normalCamera;
}

// RenderRefractions: render the water refractions to the refraction texture
void CSceneRenderer::RenderRefractions(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	const CShaderDefines& context, const CBoundingBoxAligned &scissor)
{
	PROFILE3_GPU(deviceCommandContext, "water refractions");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render water refractions");

	WaterManager& wm = m->waterManager;

	// Remember old camera
	CCamera normalCamera = m_ViewCamera;

	ComputeRefractionCamera(m_ViewCamera, scissor);
	const CBoundingBoxAligned refractionScissor =
		m->terrainRenderer.ScissorWater(CULL_DEFAULT, m_ViewCamera);
	if (refractionScissor.IsEmpty())
	{
		if (!wm.m_RefractionFramebufferInitialized)
		{
			wm.m_RefractionFramebufferInitialized = true;
			deviceCommandContext->BeginFramebufferPass(wm.m_ReflectionFramebuffer.get());
			deviceCommandContext->EndFramebufferPass();
		}
		m_ViewCamera = normalCamera;
		return;
	}

	CVector4D camPlane(0, -1, 0, wm.m_WaterHeight + 2.0f);
	SetObliqueFrustumClipping(m_ViewCamera, camPlane);

	// Save the model-view-projection matrix so the shaders can use it for projective texturing
	wm.m_RefractionMatrix = m_ViewCamera.GetViewProjection();
	wm.m_RefractionProjInvMatrix = m_ViewCamera.GetProjection().GetInverse();
	wm.m_RefractionViewInvMatrix = m_ViewCamera.GetOrientation();

	if (deviceCommandContext->GetDevice()->GetBackend() == Renderer::Backend::Backend::VULKAN)
	{
		CMatrix3D flip;
		flip.SetIdentity();
		flip._22 = -1.0f;
		wm.m_RefractionMatrix = flip * wm.m_RefractionMatrix;
		wm.m_RefractionProjInvMatrix = wm.m_RefractionProjInvMatrix * flip;
	}

	float vpHeight = wm.m_RefTextureSize;
	float vpWidth = wm.m_RefTextureSize;

	SScreenRect screenScissor;
	screenScissor.x1 = static_cast<int>(floor((refractionScissor[0].X * 0.5f + 0.5f) * vpWidth));
	screenScissor.y1 = static_cast<int>(floor((refractionScissor[0].Y * 0.5f + 0.5f) * vpHeight));
	screenScissor.x2 = static_cast<int>(ceil((refractionScissor[1].X * 0.5f + 0.5f) * vpWidth));
	screenScissor.y2 = static_cast<int>(ceil((refractionScissor[1].Y * 0.5f + 0.5f) * vpHeight));

	deviceCommandContext->BeginFramebufferPass(wm.m_RefractionFramebuffer.get());

	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = vpWidth;
	viewportRect.height = vpHeight;
	deviceCommandContext->SetViewports(1, &viewportRect);

	Renderer::Backend::IDeviceCommandContext::Rect scissorRect;
	scissorRect.x = screenScissor.x1;
	scissorRect.y = screenScissor.y1;
	scissorRect.width = screenScissor.x2 - screenScissor.x1;
	scissorRect.height = screenScissor.y2 - screenScissor.y1;
	deviceCommandContext->SetScissors(1, &scissorRect);

	// Render terrain and models
	RenderPatches(deviceCommandContext, context, CULL_REFRACTIONS);

	// Render debug-related terrain overlays to make it visible under water.
	ITerrainOverlay::RenderOverlaysBeforeWater(deviceCommandContext);

	RenderModels(deviceCommandContext, context, CULL_REFRACTIONS);
	RenderTransparentModels(deviceCommandContext, context, CULL_REFRACTIONS, TRANSPARENT_OPAQUE);

	deviceCommandContext->SetScissors(0, nullptr);
	deviceCommandContext->EndFramebufferPass();

	wm.m_RefractionFramebufferInitialized = true;

	// Reset old camera
	m_ViewCamera = normalCamera;
}

void CSceneRenderer::RenderSilhouettes(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
{
	PROFILE3("silhouettes");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render silhouettes");

	// Render silhouettes of units hidden behind terrain or occluders.
	// To avoid breaking the standard rendering of alpha-blended objects, this
	// has to be done in a separate pass.
	// First we render all occluders into depth, then render all units with
	// inverted depth test so any behind an occluder will get drawn in a constant
	// color.

	// TODO: do we need clear here?
	deviceCommandContext->ClearFramebuffer(false, true, true);

	// Render occluders:

	{
		PROFILE("render patches");
		m->terrainRenderer.RenderPatches(
			deviceCommandContext, CULL_SILHOUETTE_OCCLUDER, {}, CColor(0.0f, 0.0f, 0.0f, 1.0f), false);
	}

	{
		PROFILE("render model occluders");
		m->CallOpaqueModelRenderers(deviceCommandContext, {}, CULL_SILHOUETTE_OCCLUDER, 0, ERenderMode::SOLID);
	}

	{
		PROFILE("render transparent occluders");
		m->CallTransparentModelRenderers(deviceCommandContext, {}, CULL_SILHOUETTE_OCCLUDER, 0, ERenderMode::SOLID);
	}

	// Since we can't sort, we'll use the stencil buffer to ensure we only draw
	// a pixel once (using the color of whatever model happens to be drawn first).
	{
		PROFILE("render model casters");
		m->CallOpaqueModelRenderers(deviceCommandContext, {}, CULL_SILHOUETTE_CASTER, 0, ERenderMode::SOLID);
	}

	{
		PROFILE("render transparent casters");
		m->CallTransparentModelRenderers(deviceCommandContext, {}, CULL_SILHOUETTE_CASTER, 0, ERenderMode::SOLID);
	}
}

void CSceneRenderer::RenderParticles(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext,
	int cullGroup)
{
	PROFILE3("particles");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render particles");

	m->particleRenderer.RenderParticles(
		deviceCommandContext, cullGroup, m_ModelRenderMode == WIREFRAME);

	if (m_ModelRenderMode == EDGED_FACES)
	{
		m->particleRenderer.RenderParticles(
			deviceCommandContext, cullGroup, true);
		m->particleRenderer.RenderBounds(*deviceCommandContext, cullGroup);
	}
}

void CSceneRenderer::PrepareSubmissions(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
{
	PROFILE3_GPU(deviceCommandContext, "prepare submissions");
	GPU_SCOPED_LABEL(deviceCommandContext, "Prepare submissions");

	if (m->waterManager.m_RenderWater && m_WaterScissor.GetVolume() > 0 && m->waterManager.WillRenderFancyWater())
	{
		// Render the waves to the Fancy effects texture
		m->waterManager.RenderWaves(deviceCommandContext, m_CullCamera.GetFrustum());
	}

	m->skyManager.LoadAndUploadSkyTexturesIfNeeded(deviceCommandContext);

	GetScene().GetLOSTexture().InterpolateLOS(deviceCommandContext);
	GetScene().GetTerritoryTexture().UpdateIfNeeded(deviceCommandContext);
	GetScene().GetMiniMapTexture().Render(
		deviceCommandContext, GetScene().GetLOSTexture(), GetScene().GetTerritoryTexture());

	CShaderDefines context = m->globalContext;

	// Prepare model renderers
	m->PrepareModels(deviceCommandContext);

	m->terrainRenderer.PrepareForRendering();

	m->overlayRenderer.PrepareForRendering();

	m->particleRenderer.PrepareForRendering(context);

	m->UploadModels(deviceCommandContext);

	m->overlayRenderer.Upload(deviceCommandContext);

	m->particleRenderer.Upload(deviceCommandContext);

	if (g_RenderingOptions.GetShadows())
	{
		RenderShadowMap(deviceCommandContext);
	}

	if (m->waterManager.m_RenderWater)
	{
		if (m_WaterScissor.GetVolume() > 0 && m->waterManager.WillRenderFancyWater())
		{
			m->waterManager.UpdateQuality();

			PROFILE3_GPU(deviceCommandContext, "water scissor");
			if (g_RenderingOptions.GetWaterReflection())
				RenderReflections(deviceCommandContext, context, m_WaterScissor);

			if (g_RenderingOptions.GetWaterRefraction())
				RenderRefractions(deviceCommandContext, context, m_WaterScissor);

			if (g_RenderingOptions.GetWaterFancyEffects())
				m->terrainRenderer.RenderWaterFoamOccluders(deviceCommandContext, CULL_DEFAULT);
		}
	}
}

void CSceneRenderer::RenderSubmissions(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
{
	PROFILE3("render submissions");
	GPU_SCOPED_LABEL(deviceCommandContext, "Render submissions");

	ENSURE(m_CurrentScene);

	CShaderDefines context = m->globalContext;

	constexpr int cullGroup = CULL_DEFAULT;

	// In mixed reality the room is the sky.
	if (!QuestVR::IsMixedReality())
		m->skyManager.RenderSky(deviceCommandContext);

	// render submitted patches and models
	RenderPatches(deviceCommandContext, context, cullGroup);

	// render debug-related terrain overlays
	ITerrainOverlay::RenderOverlaysBeforeWater(deviceCommandContext);

	// render other debug-related overlays before water (so they can be seen when underwater)
	if (!g_RenderingOptions.GetCutsceneMode())
		m->overlayRenderer.RenderOverlaysBeforeWater(deviceCommandContext);

	RenderModels(deviceCommandContext, context, cullGroup);

	// render water
	if (m->waterManager.m_RenderWater && g_Game && m_WaterScissor.GetVolume() > 0)
	{
		if (m->waterManager.WillRenderFancyWater())
		{
			// Render transparent stuff, but only the solid parts that can occlude block water.
			RenderTransparentModels(deviceCommandContext, context, cullGroup, TRANSPARENT_OPAQUE);

			m->terrainRenderer.RenderWater(deviceCommandContext, context, cullGroup, &m->shadow);

			// Render transparent stuff again, but only the blended parts that overlap water.
			RenderTransparentModels(deviceCommandContext, context, cullGroup, TRANSPARENT_BLEND);
		}
		else
		{
			m->terrainRenderer.RenderWater(deviceCommandContext, context, cullGroup, &m->shadow);

			// Render transparent stuff, so it can overlap models/terrain.
			RenderTransparentModels(deviceCommandContext, context, cullGroup, TRANSPARENT);
		}
	}
	else
	{
		// render transparent stuff, so it can overlap models/terrain
		RenderTransparentModels(deviceCommandContext, context, cullGroup, TRANSPARENT);
	}

	// render debug-related terrain overlays
	ITerrainOverlay::RenderOverlaysAfterWater(deviceCommandContext, cullGroup);

	if (!g_RenderingOptions.GetCutsceneMode())
		// render some other overlays after water (so they can be displayed on top of water)
		m->overlayRenderer.RenderOverlaysAfterWater(deviceCommandContext);

	// particles are transparent so render after water
	if (g_RenderingOptions.GetParticles())
	{
		RenderParticles(deviceCommandContext, cullGroup);
	}

	// render debug lines
	if (g_RenderingOptions.GetDisplayFrustum())
		DisplayFrustum(*deviceCommandContext);

	if (g_RenderingOptions.GetDisplayShadowsFrustum())
		m->shadow.RenderDebugBounds(*deviceCommandContext);

	m->silhouetteRenderer.RenderDebugBounds(deviceCommandContext);
}

void CSceneRenderer::EndFrame()
{
	// empty lists
	m->terrainRenderer.EndFrame();
	m->overlayRenderer.EndFrame();
	m->particleRenderer.EndFrame();
	m->silhouetteRenderer.EndFrame();

	for (int cullGroup{0}; cullGroup < CSceneRenderer::CULL_MAX; ++cullGroup)
	{
		m->Model.OpaqueSkinned.submissions[cullGroup].clear();
		m->Model.TransparentSkinned.submissions[cullGroup].clear();
		m->Model.OpaqueUnskinned.submissions[cullGroup].clear();
		m->Model.TransparentUnskinned.submissions[cullGroup].clear();
	}
	m->Model.uniqueSkinnedSubmissionsForUpdate.clear();
}

void CSceneRenderer::DisplayFrustum(Renderer::Backend::IDeviceCommandContext& deviceCommandContext)
{
	g_Renderer.GetDebugRenderer().DrawCameraFrustum(deviceCommandContext, m_CullCamera, CColor(1.0f, 1.0f, 1.0f, 0.25f), 2);
	g_Renderer.GetDebugRenderer().DrawCameraFrustum(deviceCommandContext, m_CullCamera, CColor(1.0f, 1.0f, 1.0f, 1.0f), 2, true);
}

// Text overlay rendering
void CSceneRenderer::RenderTextOverlays(CCanvas2D& canvas)
{
	PROFILE3("text overlays");

	if (m_DisplayTerrainPriorities)
		m->terrainRenderer.RenderPriorities(canvas, CULL_DEFAULT);
}

// SetSceneCamera: setup projection and transform of camera and adjust viewport to current view
// The camera always represents the actual camera used to render a scene, not any virtual camera
// used for shadow rendering or reflections.
void CSceneRenderer::SetSceneCamera(const CCamera& viewCamera, const CCamera& cullCamera)
{
	m_ViewCamera = viewCamera;
	m_CullCamera = cullCamera;

	if (g_RenderingOptions.GetShadows())
		m->shadow.SetupFrame(m_CullCamera, m_LightEnv->GetSunDir());
}

void CSceneRenderer::Submit(CPatch* patch)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
	{
		m->shadow.AddShadowReceiverBound(patch->GetWorldBounds());
		m->silhouetteRenderer.AddOccluder(patch);
	}

	if (CULL_SHADOWS_CASCADE_0 <= m_CurrentCullGroup && m_CurrentCullGroup <= CULL_SHADOWS_CASCADE_3)
	{
		const int cascade = m_CurrentCullGroup - CULL_SHADOWS_CASCADE_0;
		m->shadow.AddShadowCasterBound(cascade, patch->GetWorldBounds());
	}

	m->terrainRenderer.Submit(m_CurrentCullGroup, patch);
}

void CSceneRenderer::Submit(SOverlayLine* overlay)
{
	// Overlays are only needed in the default cull group for now,
	// so just ignore submissions to any other group
	if (m_CurrentCullGroup == CULL_DEFAULT)
		m->overlayRenderer.Submit(overlay);
}

void CSceneRenderer::Submit(SOverlayTexturedLine* overlay)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
		m->overlayRenderer.Submit(overlay);
}

void CSceneRenderer::Submit(SOverlaySprite* overlay)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
		m->overlayRenderer.Submit(overlay);
}

void CSceneRenderer::Submit(SOverlayQuad* overlay)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
		m->overlayRenderer.Submit(overlay);
}

void CSceneRenderer::Submit(SOverlaySphere* overlay)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
		m->overlayRenderer.Submit(overlay);
}

void CSceneRenderer::Submit(CModelDecal* decal)
{
	// Decals can't cast shadows since they're flat on the terrain.
	// They can receive shadows, but the terrain under them will have
	// already been passed to AddShadowCasterBound, so don't bother
	// doing it again here.

	m->terrainRenderer.Submit(m_CurrentCullGroup, decal);
}

void CSceneRenderer::Submit(CParticleEmitter* emitter)
{
	m->particleRenderer.Submit(m_CurrentCullGroup, emitter);
}

void CSceneRenderer::SubmitNonRecursive(CModel* model)
{
	if (m_CurrentCullGroup == CULL_DEFAULT)
	{
		m->shadow.AddShadowReceiverBound(model->GetWorldBounds());

		if (model->GetFlags() & ModelFlag::SILHOUETTE_OCCLUDER)
			m->silhouetteRenderer.AddOccluder(model);
		if (model->GetFlags() & ModelFlag::SILHOUETTE_DISPLAY)
			m->silhouetteRenderer.AddCaster(model);
	}

	if (CULL_SHADOWS_CASCADE_0 <= m_CurrentCullGroup && m_CurrentCullGroup <= CULL_SHADOWS_CASCADE_3)
	{
		if (!(model->GetFlags() & ModelFlag::CAST_SHADOWS))
			return;

		const int cascade = m_CurrentCullGroup - CULL_SHADOWS_CASCADE_0;
		m->shadow.AddShadowCasterBound(cascade, model->GetWorldBounds());
	}

	const bool requiresSkinning{model->GetModelDef()->GetNumBones() != 0};
	ModelVertexRenderer& modelVertexSkinningRenderer{
		m->Model.GPUSkinningEnabled
			? static_cast<ModelVertexRenderer&>(*m->Model.VertexGPUSkinningShader)
			: static_cast<ModelVertexRenderer&>(m->Model.VertexCPUSkinningShader)};

	if (model->GetMaterial().UsesAlphaBlending())
	{
		if (requiresSkinning)
			m->Submit(m_CurrentCullGroup, modelVertexSkinningRenderer, m->Model.TransparentSkinned, model);
		else
			m->Submit(m_CurrentCullGroup, m->Model.VertexInstancingShader, m->Model.TransparentUnskinned, model);
	}
	else
	{
		if (requiresSkinning)
			m->Submit(m_CurrentCullGroup, modelVertexSkinningRenderer, m->Model.OpaqueSkinned, model);
		else
			m->Submit(m_CurrentCullGroup, m->Model.VertexInstancingShader, m->Model.OpaqueUnskinned, model);
	}
}

void CSceneRenderer::EnumerateSceneObjects(Scene& scene)
{
	m_CurrentScene = &scene;

	const CFrustum& frustum{m_CullCamera.GetFrustum()};

	m_CurrentCullGroup = CULL_DEFAULT;

	scene.EnumerateObjects(frustum, this);

	m->particleManager.RenderSubmit(*this, frustum);

	if (!g_RenderingOptions.GetCutsceneMode() && g_RenderingOptions.GetSilhouettes())
	{
		m->silhouetteRenderer.ComputeSubmissions(m_ViewCamera);

		m_CurrentCullGroup = CULL_DEFAULT;
		m->silhouetteRenderer.RenderSubmitOverlays(*this);

		m_CurrentCullGroup = CULL_SILHOUETTE_OCCLUDER;
		m->silhouetteRenderer.RenderSubmitOccluders(*this);

		m_CurrentCullGroup = CULL_SILHOUETTE_CASTER;
		m->silhouetteRenderer.RenderSubmitCasters(*this);
	}

	if (g_RenderingOptions.GetShadows())
	{
		for (int cascade = 0; cascade < m->shadow.GetCascadeCount(); ++cascade)
		{
			m_CurrentCullGroup = CULL_SHADOWS_CASCADE_0 + cascade;
			const CFrustum shadowFrustum = m->shadow.GetShadowCasterCullFrustum(cascade);
			scene.EnumerateObjects(shadowFrustum, this);
		}
	}

	if (m->waterManager.m_RenderWater)
	{
		m_WaterScissor = m->terrainRenderer.ScissorWater(CULL_DEFAULT, m_ViewCamera);

		if (m_WaterScissor.GetVolume() > 0 && m->waterManager.WillRenderFancyWater())
		{
			if (g_RenderingOptions.GetWaterReflection())
			{
				m_CurrentCullGroup = CULL_REFLECTIONS;

				CCamera reflectionCamera;
				ComputeReflectionCamera(reflectionCamera, m_WaterScissor);

				scene.EnumerateObjects(reflectionCamera.GetFrustum(), this);
			}

			if (g_RenderingOptions.GetWaterRefraction())
			{
				m_CurrentCullGroup = CULL_REFRACTIONS;

				CCamera refractionCamera;
				ComputeRefractionCamera(refractionCamera, m_WaterScissor);

				scene.EnumerateObjects(refractionCamera.GetFrustum(), this);
			}
		}
	}
	else
		m_WaterScissor = CBoundingBoxAligned{};

	m_CurrentCullGroup = -1;
}

void CSceneRenderer::RenderSceneOverlays(
	Renderer::Backend::IDeviceCommandContext* deviceCommandContext)
{
	if (!g_RenderingOptions.GetCutsceneMode())
	{
		if (g_RenderingOptions.GetSilhouettes())
			RenderSilhouettes(deviceCommandContext);

		m->silhouetteRenderer.RenderDebugOverlays(deviceCommandContext);

		// Render overlays that should appear on top of all other objects.
		m->overlayRenderer.RenderForegroundOverlays(deviceCommandContext, m_ViewCamera);
	}

	m_CurrentScene = nullptr;
}

Scene& CSceneRenderer::GetScene()
{
	ENSURE(m_CurrentScene);
	return *m_CurrentScene;
}

void CSceneRenderer::MakeShadersDirty()
{
	m->waterManager.m_NeedsReloading = true;
}

WaterManager& CSceneRenderer::GetWaterManager()
{
	return m->waterManager;
}

SkyManager& CSceneRenderer::GetSkyManager()
{
	return m->skyManager;
}

CParticleManager& CSceneRenderer::GetParticleManager()
{
	return m->particleManager;
}

TerrainRenderer& CSceneRenderer::GetTerrainRenderer()
{
	return m->terrainRenderer;
}

CMaterialManager& CSceneRenderer::GetMaterialManager()
{
	return m->materialManager;
}

ShadowMap& CSceneRenderer::GetShadowMap()
{
	return m->shadow;
}

void CSceneRenderer::ResetState()
{
	// Clear all emitters, that were created in previous games
	GetParticleManager().ClearUnattachedEmitters();
}
