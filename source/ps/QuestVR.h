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

#ifndef INCLUDED_QUESTVR
#define INCLUDED_QUESTVR

class CCamera;
class CCanvas2D;
namespace Renderer::Backend { class IDevice; }
namespace Renderer::Backend { class IDeviceCommandContext; }
namespace Renderer::Backend { class ISwapChain; }

/**
 * Standalone VR mode for Meta Quest (OpenXR + Vulkan).
 *
 * The battlefield is drawn in stereo as a tabletop model in front of the
 * player; the GUI is drawn on a floating panel. The right controller is a
 * laser pointer for both: pointing at the table is translated into the
 * mouse position the game's own (top-down) camera would see, so selection
 * and orders go through the normal mouse code paths.
 *
 * On other platforms everything is a no-op and IsActive() is false.
 */
namespace QuestVR
{

/**
 * Creates the OpenXR instance and tells the Vulkan backend what it needs.
 * Must be called before the Vulkan device is created.
 */
bool PreDeviceInit();

/**
 * Creates the OpenXR session and swapchains for the created device.
 */
bool PostDeviceInit(Renderer::Backend::IDevice* device);

void Shutdown();

/**
 * True once the XR session exists: the engine then renders through
 * RenderFrame VR paths and never touches the window swapchain.
 */
bool IsActive();

/**
 * Size of the GUI panel; used as the window size.
 */
void GetPanelSize(int& width, int& height);

/**
 * Size of an eye image; used for the renderer's view size dependent objects.
 */
void GetEyeSize(int& width, int& height);

/**
 * Size of the top-left part of the eye images rendered this frame (dynamic
 * resolution).
 */
void GetEyeRenderSize(int& width, int& height);

/**
 * Waits for the next XR frame and translates controller input into SDL
 * events. Call once per frame before events are dispatched.
 */
void BeginFrame();

/**
 * Whether the current XR frame should be rendered.
 */
bool ShouldRender();

/**
 * Mixed reality: the real room shows around the battlefield.
 */
bool IsMixedReality();

/**
 * Builds the camera for the given eye from the game's view camera.
 * Returns false if eye poses aren't available.
 */
bool GetEyeCamera(int eye, const CCamera& gameCamera, CCamera& eyeCamera, CCamera& cullCamera);

/**
 * Draws the laser beam (and its target marker on the table) into the
 * current eye pass, using the scene renderer's view camera.
 */
void DrawLaser(Renderer::Backend::IDeviceCommandContext& deviceCommandContext, bool depthTest);

/**
 * Eye camera outside a match, where only the laser is drawn.
 */
bool GetMenuEyeCamera(int eye, CCamera& eyeCamera);

Renderer::Backend::ISwapChain* GetEyeSwapChain(int eye);
Renderer::Backend::ISwapChain* GetPanelSwapChain();

/**
 * Draws the laser cursor on the GUI panel.
 */
void DrawPanelOverlay(CCanvas2D& canvas);

/**
 * Submits rendered images and ends the XR frame. eyesOverlay: the eye images
 * only hold the laser and go on top of the panel.
 */
void EndFrame(Renderer::Backend::IDevice* device, bool eyesRendered, bool panelRendered, bool eyesOverlay);

/**
 * Analog camera input from the thumbsticks, in -1..1.
 */
struct CameraInput
{
	float right{0.0f};
	float forward{0.0f};
	float rotate{0.0f};
	float zoom{0.0f};
};
CameraInput GetCameraInput();

} // namespace QuestVR

#endif // INCLUDED_QUESTVR
