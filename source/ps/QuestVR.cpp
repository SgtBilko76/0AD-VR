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

#include "QuestVR.h"

#include "lib/sysdep/os.h"

#if !OS_ANDROID

namespace QuestVR
{
bool PreDeviceInit() { return false; }
bool PostDeviceInit(Renderer::Backend::IDevice*) { return false; }
void Shutdown() {}
bool IsActive() { return false; }
void GetPanelSize(int&, int&) {}
void GetEyeSize(int&, int&) {}
void GetEyeRenderSize(int&, int&) {}
void BeginFrame() {}
bool ShouldRender() { return false; }
bool IsMixedReality() { return false; }
bool GetEyeCamera(int, const CCamera&, CCamera&, CCamera&) { return false; }
Renderer::Backend::ISwapChain* GetEyeSwapChain(int) { return nullptr; }
Renderer::Backend::ISwapChain* GetPanelSwapChain() { return nullptr; }
void DrawLaser(Renderer::Backend::IDeviceCommandContext&, bool) {}
bool GetMenuEyeCamera(int, CCamera&) { return false; }
void DrawPanelOverlay(CCanvas2D&) {}
void EndFrame(Renderer::Backend::IDevice*, bool, bool, bool) {}
CameraInput GetCameraInput() { return {}; }
} // namespace QuestVR

#else // OS_ANDROID

#include "graphics/Camera.h"
#include "graphics/Canvas2D.h"
#include "graphics/GameView.h"
#include "graphics/HFTracer.h"
#include "graphics/Terrain.h"
#include "graphics/TextRenderer.h"
#include "graphics/TextureManager.h"
#include "gui/CGUI.h"
#include "gui/GUIManager.h"
#include "gui/ObjectTypes/CInput.h"
#include "lib/timer.h"
#include "maths/MathUtil.h"
#include "maths/Matrix3D.h"
#include "maths/Plane.h"
#include "maths/Rect.h"
#include "maths/Vector2D.h"
#include "maths/Vector3D.h"
#include "ps/CLogger.h"
#include "ps/ConfigDB.h"
#include "ps/Game.h"
#include "ps/Profile.h"
#include "ps/VideoMode.h"
#include "ps/World.h"
#include "renderer/DebugRenderer.h"
#include "renderer/Renderer.h"
#include "renderer/SceneRenderer.h"
#include "renderer/WaterManager.h"
#include "renderer/backend/ISwapChain.h"
#include "renderer/backend/vulkan/XRInterop.h"

#include <SDL_events.h>
#include <SDL_keycode.h>
#include <SDL_mouse.h>
#include <SDL_system.h>
#include <SDL_timer.h>
#include <algorithm>
#include <android/log.h>
#include <array>
#include <cmath>
#include <cstring>
#include <jni.h>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define VRLOG(...) __android_log_print(ANDROID_LOG_INFO, "QuestVR", __VA_ARGS__)

namespace QuestVR
{

namespace
{

// Layout of the virtual room, in metres relative to the recentred head pose
// (OpenXR: x right, y up, -z forward).
constexpr float TABLE_DOWN{0.45f};
constexpr float TABLE_FORWARD{0.65f};
constexpr float TABLE_HALF_SIZE{0.55f};
constexpr float PANEL_UP{0.08f};
constexpr float PANEL_FORWARD{1.6f};
constexpr float PANEL_WIDTH{1.6f};
// Portion of the game camera's half screen height that the table shows. The
// laser position is mapped through that camera, so the table must stay in the
// middle of the screen where the GUI doesn't cover the world.
constexpr float TABLE_SCREEN_FRACTION{0.45f};

constexpr float STICK_DEADZONE{0.15f};

struct Pose
{
	CVector3D position;
	// Orientation as unit quaternion (x, y, z, w), OpenXR conventions.
	float qx{0.0f}, qy{0.0f}, qz{0.0f}, qw{1.0f};

	CVector3D Rotate(const CVector3D& v) const
	{
		// v' = q * v * q^-1
		const CVector3D u{qx, qy, qz};
		const CVector3D t{u.Cross(v) * 2.0f};
		return v + t * qw + u.Cross(t);
	}
};

Pose ToPose(const XrPosef& pose)
{
	Pose result;
	result.position = CVector3D(pose.position.x, pose.position.y, pose.position.z);
	result.qx = pose.orientation.x;
	result.qy = pose.orientation.y;
	result.qz = pose.orientation.z;
	result.qw = pose.orientation.w;
	return result;
}

struct XRSwapChainData
{
	XrSwapchain handle{XR_NULL_HANDLE};
	int32_t width{0};
	int32_t height{0};
	std::unique_ptr<Renderer::Backend::ISwapChain> engineSwapChain;
	bool acquired{false};
};

enum Hand
{
	LEFT = 0,
	RIGHT = 1
};

struct State
{
	bool enabled{false};
	XrInstance instance{XR_NULL_HANDLE};
	XrSystemId systemId{XR_NULL_SYSTEM_ID};
	XrSession session{XR_NULL_HANDLE};
	XrSpace localSpace{XR_NULL_HANDLE};
	XrSpace viewSpace{XR_NULL_HANDLE};
	XrSessionState sessionState{XR_SESSION_STATE_UNKNOWN};
	bool sessionRunning{false};
	bool frameBegun{false};
	XrFrameState frameState{XR_TYPE_FRAME_STATE};

	std::array<XrViewConfigurationView, 2> viewConfigs{};
	std::array<XrView, 2> views{};
	bool viewsValid{false};

	std::array<XRSwapChainData, 2> eyeSwapChains;
	XRSwapChainData panelSwapChain;
	int panelWidth{1440};
	int panelHeight{900};
	int eyeWidth{0};
	int eyeHeight{0};
	// Dynamic resolution: the part of the eye image rendered this frame,
	// shrunk when frames are missed so the headset's rate holds.
	float renderScale{1.0f};
	int renderWidth{0};
	int renderHeight{0};
	double lastFrameStart{0.0};
	double averageInterval{0.0};
	double lastScaleChange{0.0};

	bool hasPassthrough{false};
	bool hasRefreshRate{false};
	bool hasPerformanceSettings{false};
	XrPassthroughFB passthrough{XR_NULL_HANDLE};
	XrPassthroughLayerFB passthroughLayer{XR_NULL_HANDLE};

	// Input.
	XrActionSet actionSet{XR_NULL_HANDLE};
	XrAction aimPoseAction{XR_NULL_HANDLE};
	XrAction triggerAction{XR_NULL_HANDLE};
	XrAction squeezeAction{XR_NULL_HANDLE};
	XrAction stickAction{XR_NULL_HANDLE};
	XrAction stickClickAction{XR_NULL_HANDLE};
	XrAction aAction{XR_NULL_HANDLE};
	XrAction bAction{XR_NULL_HANDLE};
	XrAction xAction{XR_NULL_HANDLE};
	XrAction yAction{XR_NULL_HANDLE};
	XrAction menuAction{XR_NULL_HANDLE};
	XrAction hapticAction{XR_NULL_HANDLE};
	std::array<XrPath, 2> handPaths{};
	std::array<XrSpace, 2> aimSpaces{};

	std::array<bool, 2> aimValid{};
	std::array<Pose, 2> aim{};
	std::array<float, 2> trigger{};
	std::array<float, 2> squeeze{};
	std::array<CVector2D, 2> stick{};
	std::array<bool, 2> stickClick{};
	bool a{false}, b{false}, x{false}, y{false}, menu{false};

	// Previous button states for edge detection.
	bool prevA{false}, prevB{false}, prevX{false}, prevY{false}, prevMenu{false};
	bool leftMouseDown{false};
	bool rightMouseDown{false};
	bool shiftDown{false};
	bool ctrlDown{false};
	double lastLeftClickTime{0.0};
	int lastLeftClickX{0}, lastLeftClickY{0};
	double recenterStart{-1.0};
	// Mixed reality: the battlefield stands in the real room (passthrough)
	// instead of black space. Toggled with a short left stick click.
	bool mixedReality{false};
	bool prevStickClickLeft{false};
	bool leftClickAlone{false};
	double leftClickStart{0.0};

	// Recentre anchor: yaw (radians) and position in LOCAL space.
	float anchorYaw{0.0f};
	CVector3D anchorPosition;

	// Laser.
	bool laserOnPanel{false};
	bool laserOnTable{false};
	int mouseX{0};
	int mouseY{0};
	bool mouseValid{false};

	CameraInput cameraInput;

	// Table mapping, refreshed every frame from the game camera.
	bool tableValid{false};
	CVector3D pivot;
	CVector3D gameRight;
	CVector3D gameForward;
	float tableScale{1.0f}; // metres per game unit
	float tableRange{1.0f}; // game units from the pivot to the table edge

	CVector3D laserHit;
	CVector3D laserEnd; // XR space
	// Left button pressed while pointing at the table: keep the pointer on
	// the table until release, so a selection box can be dragged.
	bool dragOnTable{false};
	bool dragStarted{false};
	CVector3D dragStart; // game space
} g_State;

const char* XrResultString(XrResult result)
{
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if (g_State.instance && xrResultToString(g_State.instance, result, buffer) == XR_SUCCESS)
		return buffer;
	snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(result));
	return buffer;
}

bool Check(XrResult result, const char* what)
{
	if (XR_SUCCEEDED(result))
		return true;
	VRLOG("%s failed: %s", what, XrResultString(result));
	LOGERROR("OpenXR: %s failed: %s", what, XrResultString(result));
	return false;
}

template<typename T>
void GetProc(const char* name, T& function)
{
	function = nullptr;
	xrGetInstanceProcAddr(g_State.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function));
}

std::vector<std::string> SplitExtensions(const std::string& list)
{
	std::vector<std::string> result;
	std::istringstream stream(list);
	std::string name;
	while (stream >> name)
		result.emplace_back(name);
	return result;
}

// The anchor frame: yaw rotation around +y and translation.
CVector3D AnchorRotate(const CVector3D& v)
{
	const float c{std::cos(g_State.anchorYaw)}, s{std::sin(g_State.anchorYaw)};
	return CVector3D(c * v.X + s * v.Z, v.Y, -s * v.X + c * v.Z);
}

CVector3D AnchorToLocal(const CVector3D& v)
{
	return g_State.anchorPosition + AnchorRotate(v);
}

XrPosef AnchorPose(const CVector3D& offset)
{
	XrPosef pose{};
	const CVector3D position{AnchorToLocal(offset)};
	pose.position = {position.X, position.Y, position.Z};
	pose.orientation = {0.0f, std::sin(g_State.anchorYaw * 0.5f), 0.0f, std::cos(g_State.anchorYaw * 0.5f)};
	return pose;
}

CVector3D TableCenter() { return AnchorToLocal(CVector3D(0.0f, -TABLE_DOWN, -TABLE_FORWARD)); }
CVector3D TableRight() { return AnchorRotate(CVector3D(1.0f, 0.0f, 0.0f)); }
CVector3D TableForward() { return AnchorRotate(CVector3D(0.0f, 0.0f, -1.0f)); }

float PanelHeight()
{
	return PANEL_WIDTH * g_State.panelHeight / g_State.panelWidth;
}

// Direction in XR space -> direction in game space.
CVector3D XRToGameDirection(const CVector3D& v)
{
	return g_State.gameRight * v.Dot(TableRight()) + CVector3D(0.0f, v.Y, 0.0f) +
		g_State.gameForward * v.Dot(TableForward());
}

CVector3D XRToGamePoint(const CVector3D& p)
{
	const CVector3D d{p - TableCenter()};
	return g_State.pivot + XRToGameDirection(d) * (1.0f / g_State.tableScale);
}

CVector3D GameToXRPoint(const CVector3D& p)
{
	const CVector3D d{p - g_State.pivot};
	return TableCenter() + (TableRight() * d.Dot(g_State.gameRight) +
		CVector3D(0.0f, d.Y, 0.0f) + TableForward() * d.Dot(g_State.gameForward)) * g_State.tableScale;
}

void UpdateTableMapping()
{
	g_State.tableValid = false;
	if (!g_Game || !g_Game->IsGameStarted() || !g_Game->GetView())
		return;
	CGameView& view{*g_Game->GetView()};
	const CCamera& camera{view.GetCamera()};
	const CMatrix3D& orientation{camera.GetOrientation()};
	const CVector3D in{orientation.GetIn()};
	const CVector3D up{orientation.GetUp()};
	const CVector3D right{-orientation.GetLeft()};

	CVector3D forward{in.X + up.X, 0.0f, in.Z + up.Z};
	if (forward.LengthSquared() < 1e-6f)
		return;
	forward.Normalize();
	CVector3D rightFlat{right.X, 0.0f, right.Z};
	if (rightFlat.LengthSquared() < 1e-6f)
		rightFlat = CVector3D(forward.Z, 0.0f, -forward.X);
	rightFlat.Normalize();

	const CVector3D pivot{view.GetCameraPivot()};
	const float distance{(orientation.GetTranslation() - pivot).Length()};
	const float fov{camera.GetFOV() > 0.0f ? camera.GetFOV() : DEGTORAD(45.0f)};
	const float range{std::max(8.0f, TABLE_SCREEN_FRACTION * distance * std::tan(fov * 0.5f))};

	g_State.pivot = pivot;
	g_State.gameRight = rightFlat;
	g_State.gameForward = forward;
	g_State.tableRange = range;
	g_State.tableScale = TABLE_HALF_SIZE / range;
	g_State.tableValid = true;
}

void PushMouseMotion(int x, int y)
{
	SDL_Event ev{};
	ev.type = SDL_MOUSEMOTION;
	ev.motion.windowID = SDL_GetWindowID(g_VideoMode.GetWindow());
	ev.motion.which = 0;
	ev.motion.state = (g_State.leftMouseDown ? SDL_BUTTON_LMASK : 0) |
		(g_State.rightMouseDown ? SDL_BUTTON_RMASK : 0);
	ev.motion.x = x;
	ev.motion.y = y;
	ev.motion.xrel = x - g_State.mouseX;
	ev.motion.yrel = y - g_State.mouseY;
	SDL_PushEvent(&ev);
}

void PushMouseButton(Uint8 button, bool down)
{
	SDL_Event ev{};
	ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	ev.button.windowID = SDL_GetWindowID(g_VideoMode.GetWindow());
	ev.button.which = 0;
	ev.button.button = button;
	ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.button.clicks = 1;
	ev.button.x = g_State.mouseX;
	ev.button.y = g_State.mouseY;
	if (button == SDL_BUTTON_LEFT && down)
	{
		const double now{timer_Time()};
		if (now - g_State.lastLeftClickTime < 0.4 &&
			std::abs(g_State.mouseX - g_State.lastLeftClickX) < 12 &&
			std::abs(g_State.mouseY - g_State.lastLeftClickY) < 12)
			ev.button.clicks = 2;
		g_State.lastLeftClickTime = now;
		g_State.lastLeftClickX = g_State.mouseX;
		g_State.lastLeftClickY = g_State.mouseY;
	}
	SDL_PushEvent(&ev);
}

Uint16 CurrentModifiers()
{
	return (g_State.shiftDown ? KMOD_LSHIFT : 0) | (g_State.ctrlDown ? KMOD_LCTRL : 0);
}

void PushKey(SDL_Keycode key, bool down)
{
	SDL_Event ev{};
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.windowID = SDL_GetWindowID(g_VideoMode.GetWindow());
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.sym = key;
	ev.key.keysym.scancode = SDL_GetScancodeFromKey(key);
	ev.key.keysym.mod = CurrentModifiers();
	SDL_PushEvent(&ev);
}

void TapKey(SDL_Keycode key)
{
	PushKey(key, true);
	PushKey(key, false);
}

void Haptic(int hand, float amplitude, float seconds)
{
	if (!g_State.hapticAction)
		return;
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
	vibration.amplitude = amplitude;
	vibration.duration = static_cast<XrDuration>(seconds * 1e9);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = g_State.hapticAction;
	info.subactionPath = g_State.handPaths[hand];
	xrApplyHapticFeedback(g_State.session, &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
}

float ApplyDeadzone(float value)
{
	if (std::abs(value) < STICK_DEADZONE)
		return 0.0f;
	return (value - std::copysign(STICK_DEADZONE, value)) / (1.0f - STICK_DEADZONE);
}

void Recenter()
{
	// Use the head pose: keep only its yaw so the table stays level.
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	if (!XR_SUCCEEDED(xrLocateSpace(g_State.viewSpace, g_State.localSpace,
		g_State.frameState.predictedDisplayTime, &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return;
	const Pose head{ToPose(location.pose)};
	const CVector3D forward{head.Rotate(CVector3D(0.0f, 0.0f, -1.0f))};
	g_State.anchorYaw = std::atan2(-forward.X, -forward.Z);
	g_State.anchorPosition = head.position;
	VRLOG("Recentred: yaw %.1f pos %.2f %.2f %.2f", RADTODEG(g_State.anchorYaw),
		head.position.X, head.position.Y, head.position.Z);
	Haptic(LEFT, 0.5f, 0.08f);
	Haptic(RIGHT, 0.5f, 0.08f);
}

// Virtual keyboard, drawn on the GUI panel whenever a text field has focus
// (the headset has no keyboard and native OpenXR apps get no system one).
struct VirtualKey
{
	std::wstring label;
	std::wstring shiftedLabel;
	SDL_Keycode key; // SDLK_UNKNOWN: types label
	float width;     // in key units
	CRect rect;      // panel pixels, from the last draw
};

struct KeyboardState
{
	bool visible{false};
	bool shift{false};
	IGUIObject* dismissedFor{nullptr};
	IGUIObject* field{nullptr};
	CRect area;
	std::vector<VirtualKey> keys;
	int pressedKey{-1};
	bool capturingClick{false};
} g_Keyboard;

constexpr SDL_Keycode KEY_SHIFT_TOGGLE{SDLK_LSHIFT};
constexpr SDL_Keycode KEY_CLOSE{SDLK_F15};

void BuildKeyboardLayout()
{
	if (!g_Keyboard.keys.empty())
		return;
	auto row = [](const wchar_t* lower, const wchar_t* upper)
	{
		for (size_t index = 0; lower[index]; ++index)
			g_Keyboard.keys.push_back({std::wstring(1, lower[index]), std::wstring(1, upper[index]),
				SDLK_UNKNOWN, 1.0f, CRect()});
	};
	row(L"1234567890", L"!\"#$%&/()=");
	g_Keyboard.keys.push_back({L"\u2190", L"\u2190", SDLK_BACKSPACE, 1.6f, CRect()});
	g_Keyboard.keys.push_back({L"", L"", SDLK_UNKNOWN, -1.0f, CRect()}); // row break
	row(L"qwertyuiop", L"QWERTYUIOP");
	row(L"-", L"_");
	g_Keyboard.keys.push_back({L"", L"", SDLK_UNKNOWN, -1.0f, CRect()});
	row(L"asdfghjkl", L"ASDFGHJKL");
	g_Keyboard.keys.push_back({L"Enter", L"Enter", SDLK_RETURN, 2.1f, CRect()});
	g_Keyboard.keys.push_back({L"", L"", SDLK_UNKNOWN, -1.0f, CRect()});
	g_Keyboard.keys.push_back({L"Shift", L"Shift", KEY_SHIFT_TOGGLE, 1.6f, CRect()});
	row(L"zxcvbnm,.", L"ZXCVBNM;:");
	g_Keyboard.keys.push_back({L"@", L"@", SDLK_UNKNOWN, 1.0f, CRect()});
	g_Keyboard.keys.push_back({L"", L"", SDLK_UNKNOWN, -1.0f, CRect()});
	g_Keyboard.keys.push_back({L"Hide", L"Hide", KEY_CLOSE, 1.6f, CRect()});
	g_Keyboard.keys.push_back({L"<", L"<", SDLK_LEFT, 1.0f, CRect()});
	g_Keyboard.keys.push_back({L"Space", L"Space", SDLK_SPACE, 5.6f, CRect()});
	g_Keyboard.keys.push_back({L">", L">", SDLK_RIGHT, 1.0f, CRect()});
	g_Keyboard.keys.push_back({L"'", L"?", SDLK_UNKNOWN, 1.0f, CRect()});
}

// The focused, editable text field of the top GUI page, if any.
IGUIObject* FocusedTextField()
{
	if (!g_GUI)
		return nullptr;
	std::shared_ptr<CGUI> gui{g_GUI->GetActiveGUI()};
	if (!gui)
		return nullptr;
	IGUIObject* object{gui->GetFocusedObject()};
	CInput* input{dynamic_cast<CInput*>(object)};
	if (!input || input->IsReadonly() || input->IsHiddenOrGhost())
		return nullptr;
	return object;
}

void UpdateKeyboardVisibility()
{
	IGUIObject* field{FocusedTextField()};
	if (field != g_Keyboard.dismissedFor)
		g_Keyboard.dismissedFor = nullptr;
	g_Keyboard.field = field;
	g_Keyboard.visible = field && field != g_Keyboard.dismissedFor;
	if (!g_Keyboard.visible)
		g_Keyboard.shift = false;
}

int KeyAt(int x, int y)
{
	if (!g_Keyboard.visible)
		return -1;
	const float scale{g_VideoMode.GetScale()};
	const CVector2D point(x / scale, y / scale);
	for (size_t index = 0; index < g_Keyboard.keys.size(); ++index)
		if (g_Keyboard.keys[index].width > 0.0f && g_Keyboard.keys[index].rect.PointInside(point))
			return static_cast<int>(index);
	return -1;
}

bool InsideKeyboard(int x, int y)
{
	const float scale{g_VideoMode.GetScale()};
	return g_Keyboard.visible && g_Keyboard.area.PointInside(CVector2D(x / scale, y / scale));
}

void PushText(const std::wstring& text)
{
	// SDL turns text events off while no native text input is active.
	SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	SDL_Event ev{};
	ev.type = SDL_TEXTINPUT;
	ev.text.windowID = SDL_GetWindowID(g_VideoMode.GetWindow());
	const std::string utf8{utf8_from_wstring(text)};
	strncpy(ev.text.text, utf8.c_str(), sizeof(ev.text.text) - 1);
	SDL_PushEvent(&ev);
}

void PressVirtualKey(int index)
{
	const VirtualKey& key{g_Keyboard.keys[index]};
	if (key.key == KEY_SHIFT_TOGGLE)
		g_Keyboard.shift = !g_Keyboard.shift;
	else if (key.key == KEY_CLOSE)
		g_Keyboard.dismissedFor = g_Keyboard.field;
	else if (key.key == SDLK_SPACE)
		PushText(L" ");
	else if (key.key != SDLK_UNKNOWN)
	{
		PushKey(key.key, true);
		PushKey(key.key, false);
	}
	else
	{
		PushText(g_Keyboard.shift ? key.shiftedLabel : key.label);
		g_Keyboard.shift = false;
	}
}

void DrawKeyboard(CCanvas2D& canvas)
{
	UpdateKeyboardVisibility();
	if (!g_Keyboard.visible)
		return;
	BuildKeyboardLayout();

	// Below the field if it's in the upper half of the panel, else above it.
	// GUI units (panel pixels / gui.scale), like the field's size.
	const float panelWidth{g_State.panelWidth / g_VideoMode.GetScale()};
	const float panelHeight{g_State.panelHeight / g_VideoMode.GetScale()};
	const float unit{std::min(64.0f, panelWidth / 13.5f)};
	const float gap{4.0f};
	const float width{unit * 12.1f};
	const float height{unit * 5.0f + gap * 6.0f};
	const CRect fieldRect{g_Keyboard.field->GetActualSize()};
	const float left{std::clamp(fieldRect.left + (fieldRect.GetWidth() - width) * 0.5f, 4.0f, panelWidth - width - 4.0f)};
	float top{fieldRect.bottom + 8.0f};
	if (top + height > panelHeight - 4.0f)
		top = std::max(4.0f, fieldRect.top - height - 8.0f);
	g_Keyboard.area = CRect(left, top, left + width, top + height);

	canvas.DrawRect(g_Keyboard.area, CColor(0.08f, 0.07f, 0.06f, 0.95f));

	float x{left + gap}, y{top + gap};
	for (VirtualKey& key : g_Keyboard.keys)
	{
		if (key.width < 0.0f)
		{
			x = left + gap;
			y += unit + gap;
			continue;
		}
		key.rect = CRect(x, y, x + unit * key.width - gap, y + unit);
		x += unit * key.width;
	}

	const int hovered{g_State.laserOnPanel ? KeyAt(g_State.mouseX, g_State.mouseY) : -1};
	CTextRenderer textRenderer;
	textRenderer.SetCurrentFont(CStrIntern("sans-bold-20"), CStrIntern());
	textRenderer.SetCurrentColor(CColor(1.0f, 0.95f, 0.8f, 1.0f));
	for (size_t index = 0; index < g_Keyboard.keys.size(); ++index)
	{
		const VirtualKey& key{g_Keyboard.keys[index]};
		if (key.width < 0.0f)
			continue;
		const bool active{key.key == KEY_SHIFT_TOGGLE && g_Keyboard.shift};
		const bool pressed{static_cast<int>(index) == g_Keyboard.pressedKey};
		CColor color{0.27f, 0.22f, 0.16f, 1.0f};
		if (static_cast<int>(index) == hovered)
			color = CColor(0.45f, 0.36f, 0.22f, 1.0f);
		if (pressed || active)
			color = CColor(0.7f, 0.15f, 0.1f, 1.0f);
		canvas.DrawRect(key.rect, color);
		const std::wstring& label{g_Keyboard.shift ? key.shiftedLabel : key.label};
		const float textX{key.rect.left + (label.size() > 1 ? 8.0f : key.rect.GetWidth() * 0.5f - 6.0f)};
		textRenderer.Put(textX, key.rect.top + unit * 0.65f, label.c_str());
	}
	canvas.DrawText(textRenderer);
}

// Intersects the right controller's laser with the panel and the table and
// moves the mouse accordingly.
void UpdateLaser()
{
	g_State.laserOnPanel = false;
	g_State.laserOnTable = false;
	if (!g_State.aimValid[RIGHT])
		return;

	const Pose& aim{g_State.aim[RIGHT]};
	const CVector3D origin{aim.position};
	const CVector3D direction{aim.Rotate(CVector3D(0.0f, 0.0f, -1.0f))};

	float bestDistance{std::numeric_limits<float>::max()};
	int targetX{0}, targetY{0};

	// Panel: a plane facing the anchor, PANEL_FORWARD in front of it.
	if (!g_State.dragOnTable)
	{
		const CVector3D center{AnchorToLocal(CVector3D(0.0f, PANEL_UP, -PANEL_FORWARD))};
		const CVector3D normal{AnchorRotate(CVector3D(0.0f, 0.0f, 1.0f))};
		const float denominator{direction.Dot(normal)};
		if (denominator < -1e-4f)
		{
			const float t{(center - origin).Dot(normal) / denominator};
			if (t > 0.0f)
			{
				const CVector3D hit{origin + direction * t - center};
				const float u{hit.Dot(AnchorRotate(CVector3D(1.0f, 0.0f, 0.0f)))};
				const float v{hit.Y};
				const float halfWidth{PANEL_WIDTH * 0.5f}, halfHeight{PanelHeight() * 0.5f};
				if (std::abs(u) <= halfWidth && std::abs(v) <= halfHeight)
				{
					bestDistance = t;
					g_State.laserEnd = origin + direction * t;
					targetX = static_cast<int>((u / PANEL_WIDTH + 0.5f) * g_State.panelWidth);
					targetY = static_cast<int>((0.5f - v / PanelHeight()) * g_State.panelHeight);
					g_State.laserOnPanel = true;
				}
			}
		}
	}

	// Table: cast the ray into the game world.
	if (g_State.tableValid && g_Game && g_Game->IsGameStarted() && g_Game->GetWorld())
	{
		const CVector3D gameOrigin{XRToGamePoint(origin)};
		CVector3D gameDirection{XRToGameDirection(direction)};
		gameDirection.Normalize();

		CTerrain& terrain{g_Game->GetWorld()->GetTerrain()};
		CHFTracer tracer(terrain);
		int tileX{0}, tileZ{0};
		CVector3D hit;
		bool found{tracer.RayIntersect(gameOrigin, gameDirection, tileX, tileZ, hit)};

		// Water surface, so ships and shores can be targeted.
		const float waterHeight{g_Renderer.GetSceneRenderer().GetWaterManager().m_WaterHeight};
		if (gameDirection.Y < -1e-4f)
		{
			const float t{(waterHeight - gameOrigin.Y) / gameDirection.Y};
			const CVector3D waterHit{gameOrigin + gameDirection * t};
			if (t > 0.0f && (!found || waterHit.Y > hit.Y))
			{
				hit = waterHit;
				found = true;
			}
		}

		if (!found && g_State.dragOnTable && gameDirection.Y < -1e-4f)
		{
			// Off the terrain while dragging: use the table plane.
			const float t{(g_State.pivot.Y - gameOrigin.Y) / gameDirection.Y};
			if (t > 0.0f)
			{
				hit = gameOrigin + gameDirection * t;
				found = true;
			}
		}

		if (found && g_State.dragOnTable)
		{
			// Keep the box inside the table.
			const float range{g_State.tableRange};
			const CVector3D offset{hit - g_State.pivot};
			const float a{std::clamp(offset.Dot(g_State.gameRight), -range, range)};
			const float b{std::clamp(offset.Dot(g_State.gameForward), -range, range)};
			hit = CVector3D(g_State.pivot.X, hit.Y, g_State.pivot.Z) + g_State.gameRight * a + g_State.gameForward * b;
		}

		if (found)
		{
			const CVector3D offset{hit - g_State.pivot};
			const float range{g_State.tableRange * 1.001f};
			if (std::abs(offset.Dot(g_State.gameRight)) <= range &&
				std::abs(offset.Dot(g_State.gameForward)) <= range)
			{
				const float distance{(GameToXRPoint(hit) - origin).Length()};
				if (distance < bestDistance)
				{
					const CVector2D screen{g_Game->GetView()->GetCamera().GetScreenCoordinates(hit)};
					targetX = static_cast<int>(screen.X);
					targetY = static_cast<int>(screen.Y);
					g_State.laserOnPanel = false;
					g_State.laserOnTable = true;
					g_State.laserHit = hit;
					g_State.laserEnd = GameToXRPoint(hit);
				}
			}
		}
	}

	if (!g_State.laserOnPanel && !g_State.laserOnTable)
		return;

	targetX = std::clamp(targetX, 0, g_State.panelWidth - 1);
	targetY = std::clamp(targetY, 0, g_State.panelHeight - 1);
	if (!g_State.mouseValid || targetX != g_State.mouseX || targetY != g_State.mouseY)
	{
		PushMouseMotion(targetX, targetY);
		g_State.mouseX = targetX;
		g_State.mouseY = targetY;
		g_State.mouseValid = true;
	}
}

bool GetBool(XrAction action, XrPath subaction = XR_NULL_PATH)
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	info.subactionPath = subaction;
	XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
	if (!XR_SUCCEEDED(xrGetActionStateBoolean(g_State.session, &info, &state)))
		return false;
	return state.isActive && state.currentState;
}

float GetFloat(XrAction action, XrPath subaction)
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	info.subactionPath = subaction;
	XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
	if (!XR_SUCCEEDED(xrGetActionStateFloat(g_State.session, &info, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

CVector2D GetVector(XrAction action, XrPath subaction)
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	info.subactionPath = subaction;
	XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
	if (!XR_SUCCEEDED(xrGetActionStateVector2f(g_State.session, &info, &state)) || !state.isActive)
		return {};
	return {state.currentState.x, state.currentState.y};
}

void UpdateInput()
{
	g_State.cameraInput = {};
	if (g_State.sessionState != XR_SESSION_STATE_FOCUSED)
		return;

	XrActiveActionSet activeSet{g_State.actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	if (!XR_SUCCEEDED(xrSyncActions(g_State.session, &syncInfo)))
		return;

	for (int hand : {LEFT, RIGHT})
	{
		const XrPath path{g_State.handPaths[hand]};
		g_State.trigger[hand] = GetFloat(g_State.triggerAction, path);
		g_State.squeeze[hand] = GetFloat(g_State.squeezeAction, path);
		g_State.stick[hand] = GetVector(g_State.stickAction, path);
		g_State.stickClick[hand] = GetBool(g_State.stickClickAction, path);

		XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
		g_State.aimValid[hand] =
			XR_SUCCEEDED(xrLocateSpace(g_State.aimSpaces[hand], g_State.localSpace,
				g_State.frameState.predictedDisplayTime, &location)) &&
			(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
			(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
		if (g_State.aimValid[hand])
			g_State.aim[hand] = ToPose(location.pose);
	}
	g_State.a = GetBool(g_State.aAction);
	g_State.b = GetBool(g_State.bAction);
	g_State.x = GetBool(g_State.xAction);
	g_State.y = GetBool(g_State.yAction);
	g_State.menu = GetBool(g_State.menuAction);

	// Recentre: hold both stick clicks for a second.
	if (g_State.stickClick[LEFT] && g_State.stickClick[RIGHT])
	{
		const double now{timer_Time()};
		if (g_State.recenterStart < 0.0)
			g_State.recenterStart = now;
		else if (now - g_State.recenterStart > 1.0)
		{
			Recenter();
			g_State.recenterStart = std::numeric_limits<double>::max();
		}
	}
	else
		g_State.recenterStart = -1.0;

	// Mixed reality toggle: a short left stick click on its own.
	if (g_State.stickClick[LEFT] && !g_State.prevStickClickLeft)
	{
		g_State.leftClickAlone = !g_State.stickClick[RIGHT];
		g_State.leftClickStart = timer_Time();
	}
	if (g_State.stickClick[RIGHT])
		g_State.leftClickAlone = false;
	if (!g_State.stickClick[LEFT] && g_State.prevStickClickLeft && g_State.leftClickAlone &&
		timer_Time() - g_State.leftClickStart < 0.6)
	{
		g_State.mixedReality = !g_State.mixedReality;
		g_ConfigDB.SetValueBool(CFG_USER, "quest.passthrough.ingame", g_State.mixedReality);
		g_ConfigDB.WriteValueToFile(CFG_USER, "quest.passthrough.ingame",
			g_State.mixedReality ? "true" : "false");
		Haptic(LEFT, 0.5f, 0.05f);
		VRLOG("Mixed reality %s", g_State.mixedReality ? "on" : "off");
	}
	g_State.prevStickClickLeft = g_State.stickClick[LEFT];

	UpdateLaser();

	// Modifiers first, so clicks in the same frame see them.
	const bool shift{g_State.trigger[LEFT] > (g_State.shiftDown ? 0.35f : 0.55f)};
	if (shift != g_State.shiftDown)
	{
		g_State.shiftDown = shift;
		PushKey(SDLK_LSHIFT, shift);
	}
	const bool ctrl{g_State.squeeze[LEFT] > (g_State.ctrlDown ? 0.35f : 0.55f)};
	if (ctrl != g_State.ctrlDown)
	{
		g_State.ctrlDown = ctrl;
		PushKey(SDLK_LCTRL, ctrl);
	}

	if (g_State.mouseValid)
	{
		const bool left{g_State.trigger[RIGHT] > (g_State.leftMouseDown ? 0.35f : 0.55f)};
		if (left && !g_State.leftMouseDown && g_State.laserOnPanel &&
			InsideKeyboard(g_State.mouseX, g_State.mouseY))
		{
			// Keyboard click: type, don't click the GUI underneath.
			g_State.leftMouseDown = true;
			g_Keyboard.capturingClick = true;
			g_Keyboard.pressedKey = KeyAt(g_State.mouseX, g_State.mouseY);
			if (g_Keyboard.pressedKey >= 0)
				PressVirtualKey(g_Keyboard.pressedKey);
			Haptic(RIGHT, 0.3f, 0.02f);
		}
		else if (!left && g_Keyboard.capturingClick)
		{
			g_State.leftMouseDown = false;
			g_Keyboard.capturingClick = false;
			g_Keyboard.pressedKey = -1;
		}
		else if (left != g_State.leftMouseDown && !g_Keyboard.capturingClick)
		{
			g_State.leftMouseDown = left;
			g_State.dragOnTable = left && g_State.laserOnTable;
			g_State.dragStarted = false;
			if (g_State.dragOnTable)
				g_State.dragStart = g_State.laserHit;
			PushMouseButton(SDL_BUTTON_LEFT, left);
			if (left)
				Haptic(RIGHT, 0.3f, 0.02f);
		}
		const bool right{g_State.squeeze[RIGHT] > (g_State.rightMouseDown ? 0.35f : 0.55f)};
		if (right != g_State.rightMouseDown)
		{
			g_State.rightMouseDown = right;
			PushMouseButton(SDL_BUTTON_RIGHT, right);
			if (right)
				Haptic(RIGHT, 0.3f, 0.02f);
		}
	}

	if (g_State.a && !g_State.prevA)
		TapKey(SDLK_F13); // hotkey.camera.follow in the Quest config
	if (g_State.b && !g_State.prevB)
		TapKey(SDLK_ESCAPE);
	if (g_State.x && !g_State.prevX)
		TapKey(SDLK_PERIOD); // hotkey.selection.idleworker
	if (g_State.y && !g_State.prevY)
		TapKey(SDLK_PAUSE);
	if (g_State.menu && !g_State.prevMenu)
		TapKey(SDLK_F10); // hotkey.session.gui.menu.toggle
	g_State.prevA = g_State.a;
	g_State.prevB = g_State.b;
	g_State.prevX = g_State.x;
	g_State.prevY = g_State.y;
	g_State.prevMenu = g_State.menu;

	// While both sticks are clicked for recentring, don't move the camera.
	if (g_State.recenterStart < 0.0)
	{
		g_State.cameraInput.right = ApplyDeadzone(g_State.stick[LEFT].X);
		g_State.cameraInput.forward = ApplyDeadzone(g_State.stick[LEFT].Y);
		g_State.cameraInput.rotate = ApplyDeadzone(g_State.stick[RIGHT].X);
		g_State.cameraInput.zoom = ApplyDeadzone(g_State.stick[RIGHT].Y);
	}
}

void ReleaseAllInput()
{
	if (g_State.leftMouseDown)
		PushMouseButton(SDL_BUTTON_LEFT, false);
	if (g_State.rightMouseDown)
		PushMouseButton(SDL_BUTTON_RIGHT, false);
	if (g_State.shiftDown)
		PushKey(SDLK_LSHIFT, false);
	if (g_State.ctrlDown)
		PushKey(SDLK_LCTRL, false);
	g_State.leftMouseDown = g_State.rightMouseDown = false;
	g_State.dragOnTable = false;
	g_State.shiftDown = g_State.ctrlDown = false;
	g_State.cameraInput = {};
}

bool CreateActions()
{
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(setInfo.actionSetName, "rts");
	strcpy(setInfo.localizedActionSetName, "RTS");
	if (!Check(xrCreateActionSet(g_State.instance, &setInfo, &g_State.actionSet), "xrCreateActionSet"))
		return false;

	xrStringToPath(g_State.instance, "/user/hand/left", &g_State.handPaths[LEFT]);
	xrStringToPath(g_State.instance, "/user/hand/right", &g_State.handPaths[RIGHT]);

	auto createAction = [](XrAction& action, const char* name, XrActionType type, bool perHand) -> bool
	{
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		strcpy(info.actionName, name);
		strcpy(info.localizedActionName, name);
		if (perHand)
		{
			info.countSubactionPaths = 2;
			info.subactionPaths = g_State.handPaths.data();
		}
		return Check(xrCreateAction(g_State.actionSet, &info, &action), name);
	};
	if (!createAction(g_State.aimPoseAction, "aim", XR_ACTION_TYPE_POSE_INPUT, true) ||
		!createAction(g_State.triggerAction, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
		!createAction(g_State.squeezeAction, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
		!createAction(g_State.stickAction, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) ||
		!createAction(g_State.stickClickAction, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
		!createAction(g_State.aAction, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
		!createAction(g_State.bAction, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
		!createAction(g_State.xAction, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
		!createAction(g_State.yAction, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
		!createAction(g_State.menuAction, "button_menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
		!createAction(g_State.hapticAction, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true))
		return false;

	std::vector<XrActionSuggestedBinding> bindings;
	auto bind = [&bindings](XrAction action, const char* path)
	{
		XrPath xrPath;
		xrStringToPath(g_State.instance, path, &xrPath);
		bindings.push_back({action, xrPath});
	};
	for (const char* hand : {"/user/hand/left", "/user/hand/right"})
	{
		const std::string prefix{hand};
		bind(g_State.aimPoseAction, (prefix + "/input/aim/pose").c_str());
		bind(g_State.triggerAction, (prefix + "/input/trigger/value").c_str());
		bind(g_State.squeezeAction, (prefix + "/input/squeeze/value").c_str());
		bind(g_State.stickAction, (prefix + "/input/thumbstick").c_str());
		bind(g_State.stickClickAction, (prefix + "/input/thumbstick/click").c_str());
		bind(g_State.hapticAction, (prefix + "/output/haptic").c_str());
	}
	bind(g_State.aAction, "/user/hand/right/input/a/click");
	bind(g_State.bAction, "/user/hand/right/input/b/click");
	bind(g_State.xAction, "/user/hand/left/input/x/click");
	bind(g_State.yAction, "/user/hand/left/input/y/click");
	bind(g_State.menuAction, "/user/hand/left/input/menu/click");

	XrPath profile;
	xrStringToPath(g_State.instance, "/interaction_profiles/oculus/touch_controller", &profile);
	XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile = profile;
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	suggested.suggestedBindings = bindings.data();
	if (!Check(xrSuggestInteractionProfileBindings(g_State.instance, &suggested), "xrSuggestInteractionProfileBindings"))
		return false;

	for (int hand : {LEFT, RIGHT})
	{
		XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.action = g_State.aimPoseAction;
		spaceInfo.subactionPath = g_State.handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;
		if (!Check(xrCreateActionSpace(g_State.session, &spaceInfo, &g_State.aimSpaces[hand]), "xrCreateActionSpace"))
			return false;
	}

	XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &g_State.actionSet;
	return Check(xrAttachSessionActionSets(g_State.session, &attachInfo), "xrAttachSessionActionSets");
}

bool CreateSwapChain(
	Renderer::Backend::IDevice* device, XRSwapChainData& data, const char* name,
	int64_t format, int width, int height, const CColor& clearColor)
{
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
		XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!Check(xrCreateSwapchain(g_State.session, &info, &data.handle), "xrCreateSwapchain"))
		return false;
	data.width = width;
	data.height = height;

	uint32_t count{0};
	xrEnumerateSwapchainImages(data.handle, 0, &count, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	if (!Check(xrEnumerateSwapchainImages(data.handle, count, &count,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages"))
		return false;
	std::vector<uint64_t> handles;
	for (const XrSwapchainImageVulkanKHR& image : images)
		handles.emplace_back(reinterpret_cast<uint64_t>(image.image));

	// The engine writes sRGB encoded values like on a normal monitor: render
	// through UNORM views into sRGB images so the compositor decodes them.
	const int32_t viewFormat{format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
		format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : static_cast<int32_t>(format)};

	XRSwapChainData* dataPtr{&data};
	data.engineSwapChain = Renderer::Backend::Vulkan::XR::CreateSwapChain(
		device, name, handles, viewFormat, width, height,
		[dataPtr](uint32_t& index) -> bool
		{
			XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (!Check(xrAcquireSwapchainImage(dataPtr->handle, &acquireInfo, &index), "xrAcquireSwapchainImage"))
				return false;
			XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			waitInfo.timeout = XR_INFINITE_DURATION;
			if (!Check(xrWaitSwapchainImage(dataPtr->handle, &waitInfo), "xrWaitSwapchainImage"))
			{
				XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(dataPtr->handle, &releaseInfo);
				return false;
			}
			dataPtr->acquired = true;
			return true;
		}, clearColor);
	return static_cast<bool>(data.engineSwapChain);
}

void HandleSessionStateChange(XrSessionState state)
{
	g_State.sessionState = state;
	VRLOG("Session state %d", static_cast<int>(state));
	switch (state)
	{
	case XR_SESSION_STATE_READY:
	{
		XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
		beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (Check(xrBeginSession(g_State.session, &beginInfo), "xrBeginSession"))
		{
			g_State.sessionRunning = true;
			if (g_State.hasRefreshRate)
			{
				PFN_xrRequestDisplayRefreshRateFB requestRefreshRate;
				GetProc("xrRequestDisplayRefreshRateFB", requestRefreshRate);
				PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates;
				GetProc("xrEnumerateDisplayRefreshRatesFB", enumerateRates);
				uint32_t rateCount{0};
				std::array<float, 16> rates{};
				if (enumerateRates &&
					XR_SUCCEEDED(enumerateRates(g_State.session, static_cast<uint32_t>(rates.size()), &rateCount, rates.data())))
				{
					std::string list;
					for (uint32_t index = 0; index < rateCount; ++index)
						list += std::to_string(static_cast<int>(rates[index])) + " ";
					VRLOG("Display refresh rates: %s", list.c_str());
				}
				if (requestRefreshRate)
					requestRefreshRate(g_State.session, g_ConfigDB.Get("quest.refreshrate", 72.0f));
			}
			if (g_State.hasPerformanceSettings)
			{
				PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel;
				GetProc("xrPerfSettingsSetPerformanceLevelEXT", setLevel);
				if (setLevel)
				{
					setLevel(g_State.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
					setLevel(g_State.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
				}
			}
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING:
		ReleaseAllInput();
		xrEndSession(g_State.session);
		g_State.sessionRunning = false;
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
	{
		g_State.sessionRunning = false;
		SDL_Event quit{};
		quit.type = SDL_QUIT;
		SDL_PushEvent(&quit);
		break;
	}
	case XR_SESSION_STATE_VISIBLE:
		ReleaseAllInput();
		break;
	default:
		break;
	}
}

void PollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(g_State.instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			HandleSessionStateChange(reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state);
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			HandleSessionStateChange(XR_SESSION_STATE_LOSS_PENDING);
			break;
		default:
			break;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

void StartPassthrough()
{
	g_State.mixedReality = g_ConfigDB.Get("quest.passthrough.ingame", false);
	if (!g_State.hasPassthrough || !g_ConfigDB.Get("quest.passthrough", true))
		return;
	PFN_xrCreatePassthroughFB createPassthrough;
	PFN_xrCreatePassthroughLayerFB createLayer;
	GetProc("xrCreatePassthroughFB", createPassthrough);
	GetProc("xrCreatePassthroughLayerFB", createLayer);
	if (!createPassthrough || !createLayer)
		return;
	XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
	info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	if (!Check(createPassthrough(g_State.session, &info, &g_State.passthrough), "xrCreatePassthroughFB"))
		return;
	XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
	layerInfo.passthrough = g_State.passthrough;
	layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	if (!Check(createLayer(g_State.session, &layerInfo, &g_State.passthroughLayer), "xrCreatePassthroughLayerFB"))
		g_State.passthroughLayer = XR_NULL_HANDLE;
}

} // anonymous namespace

bool PreDeviceInit()
{
	if (!g_ConfigDB.Get("quest.vr", true))
		return false;

	PFN_xrInitializeLoaderKHR initializeLoader{nullptr};
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
	JNIEnv* env{static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv())};
	JavaVM* vm{nullptr};
	env->GetJavaVM(&vm);
	jobject activity{env->NewGlobalRef(static_cast<jobject>(SDL_AndroidGetActivity()))};
	if (initializeLoader)
	{
		XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		loaderInfo.applicationVM = vm;
		loaderInfo.applicationContext = activity;
		if (!Check(initializeLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)),
			"xrInitializeLoaderKHR"))
			return false;
	}

	uint32_t extensionCount{0};
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
	auto hasExtension = [&extensions](const char* name)
	{
		return std::any_of(extensions.begin(), extensions.end(),
			[name](const XrExtensionProperties& p) { return strcmp(p.extensionName, name) == 0; });
	};
	if (!hasExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
	{
		LOGERROR("OpenXR runtime doesn't support Vulkan.");
		return false;
	}
	std::vector<const char*> enabled{
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
	g_State.hasPassthrough = hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	g_State.hasRefreshRate = hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	g_State.hasPerformanceSettings = hasExtension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (g_State.hasPassthrough)
		enabled.emplace_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	if (g_State.hasRefreshRate)
		enabled.emplace_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (g_State.hasPerformanceSettings)
		enabled.emplace_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

	XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = activity;
	XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
	createInfo.next = &androidInfo;
	strcpy(createInfo.applicationInfo.applicationName, "0 A.D. VR");
	strcpy(createInfo.applicationInfo.engineName, "Pyrogenesis");
	createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
	createInfo.enabledExtensionNames = enabled.data();
	if (!Check(xrCreateInstance(&createInfo, &g_State.instance), "xrCreateInstance"))
		return false;

	XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!Check(xrGetSystem(g_State.instance, &systemInfo, &g_State.systemId), "xrGetSystem"))
		return false;

	uint32_t viewCount{0};
	xrEnumerateViewConfigurationViews(g_State.instance, g_State.systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
	if (viewCount != 2)
		return false;
	for (XrViewConfigurationView& view : g_State.viewConfigs)
		view = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	xrEnumerateViewConfigurationViews(g_State.instance, g_State.systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, g_State.viewConfigs.data());
	const float eyeScale{std::clamp(g_ConfigDB.Get("quest.eyescale", 1.0f), 0.3f, 1.5f)};
	// The recommended size differs between runtime versions and modes (up
	// to ~2800 px wide); cap it so the doubled scene stays affordable.
	const float recommendedWidth{static_cast<float>(g_State.viewConfigs[0].recommendedImageRectWidth)};
	const float recommendedHeight{static_cast<float>(g_State.viewConfigs[0].recommendedImageRectHeight)};
	const float maxWidth{static_cast<float>(g_ConfigDB.Get("quest.eye.maxwidth", 2064))};
	float width{std::min(recommendedWidth * eyeScale, maxWidth)};
	// Fixed per-eye width (e.g. the Quest 3 panel's 2064), since the
	// recommended size varies between launches.
	const int fixedWidth{g_ConfigDB.Get("quest.eye.width", 0)};
	if (fixedWidth > 0)
		width = std::min(static_cast<float>(fixedWidth),
			static_cast<float>(g_State.viewConfigs[0].maxImageRectWidth));
	VRLOG("Recommended eye size %dx%d, max %dx%d", g_State.viewConfigs[0].recommendedImageRectWidth,
		g_State.viewConfigs[0].recommendedImageRectHeight, g_State.viewConfigs[0].maxImageRectWidth,
		g_State.viewConfigs[0].maxImageRectHeight);
	g_State.eyeWidth = static_cast<int>(width) & ~7;
	// The runtime sometimes mixes the recommended width and height of
	// different modes (e.g. 1680x2933); fall back to the Quest 3 panel's
	// aspect (2064x2208) when the ratio is implausible.
	float aspect{recommendedHeight / std::max(1.0f, recommendedWidth)};
	if (aspect < 0.9f || aspect > 1.2f)
		aspect = 2208.0f / 2064.0f;
	g_State.eyeHeight = static_cast<int>(width * aspect) & ~7;
	g_State.panelWidth = g_ConfigDB.Get("quest.panel.width", 1440);
	g_State.panelHeight = g_ConfigDB.Get("quest.panel.height", 900);

	PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions;
	PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions;
	PFN_xrGetVulkanGraphicsRequirementsKHR getRequirements;
	GetProc("xrGetVulkanInstanceExtensionsKHR", getInstanceExtensions);
	GetProc("xrGetVulkanDeviceExtensionsKHR", getDeviceExtensions);
	GetProc("xrGetVulkanGraphicsRequirementsKHR", getRequirements);
	if (!getInstanceExtensions || !getDeviceExtensions || !getRequirements)
		return false;

	XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	if (!Check(getRequirements(g_State.instance, g_State.systemId, &requirements), "xrGetVulkanGraphicsRequirementsKHR"))
		return false;

	auto readList = [](auto function) -> std::string
	{
		uint32_t size{0};
		function(g_State.instance, g_State.systemId, 0, &size, nullptr);
		std::string buffer(size, '\0');
		function(g_State.instance, g_State.systemId, size, &size, buffer.data());
		buffer.resize(strlen(buffer.c_str()));
		return buffer;
	};
	Renderer::Backend::Vulkan::XR::Requirements vulkanRequirements;
	vulkanRequirements.instanceExtensions = SplitExtensions(readList(getInstanceExtensions));
	vulkanRequirements.deviceExtensions = SplitExtensions(readList(getDeviceExtensions));
	vulkanRequirements.choosePhysicalDevice = [](void* vkInstance) -> void*
	{
		PFN_xrGetVulkanGraphicsDeviceKHR getDevice;
		GetProc("xrGetVulkanGraphicsDeviceKHR", getDevice);
		VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
		if (getDevice)
			Check(getDevice(g_State.instance, g_State.systemId,
				static_cast<VkInstance>(vkInstance), &physicalDevice), "xrGetVulkanGraphicsDeviceKHR");
		return physicalDevice;
	};
	Renderer::Backend::Vulkan::XR::SetRequirements(std::move(vulkanRequirements));

	g_State.enabled = true;
	VRLOG("OpenXR instance created; eye %dx%d, panel %dx%d",
		g_State.eyeWidth, g_State.eyeHeight, g_State.panelWidth, g_State.panelHeight);
	return true;
}

bool PostDeviceInit(Renderer::Backend::IDevice* device)
{
	if (!g_State.enabled)
		return false;

	Renderer::Backend::Vulkan::XR::Binding binding;
	if (!Renderer::Backend::Vulkan::XR::GetBinding(device, binding))
		return false;

	XrGraphicsBindingVulkanKHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	graphicsBinding.instance = static_cast<VkInstance>(binding.instance);
	graphicsBinding.physicalDevice = static_cast<VkPhysicalDevice>(binding.physicalDevice);
	graphicsBinding.device = static_cast<VkDevice>(binding.device);
	graphicsBinding.queueFamilyIndex = binding.queueFamilyIndex;
	graphicsBinding.queueIndex = binding.queueIndex;
	XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &graphicsBinding;
	sessionInfo.systemId = g_State.systemId;
	if (!Check(xrCreateSession(g_State.instance, &sessionInfo, &g_State.session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!Check(xrCreateReferenceSpace(g_State.session, &spaceInfo, &g_State.localSpace), "xrCreateReferenceSpace"))
		return false;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!Check(xrCreateReferenceSpace(g_State.session, &spaceInfo, &g_State.viewSpace), "xrCreateReferenceSpace"))
		return false;

	uint32_t formatCount{0};
	xrEnumerateSwapchainFormats(g_State.session, 0, &formatCount, nullptr);
	std::vector<int64_t> formats(formatCount);
	xrEnumerateSwapchainFormats(g_State.session, formatCount, &formatCount, formats.data());
	int64_t format{0};
	for (int64_t preferred : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM})
		if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
		{
			format = preferred;
			break;
		}
	if (!format)
	{
		LOGERROR("OpenXR: no supported swapchain format.");
		return false;
	}

	// Transparent, so menus can show just the laser over the panel and room.
	const CColor eyeClear{0.0f, 0.0f, 0.0f, 0.0f};
	for (int eye = 0; eye < 2; ++eye)
		if (!CreateSwapChain(device, g_State.eyeSwapChains[eye], eye ? "XR right eye" : "XR left eye",
			format, g_State.eyeWidth, g_State.eyeHeight, eyeClear))
			return false;
	if (!CreateSwapChain(device, g_State.panelSwapChain, "XR panel", format,
		g_State.panelWidth, g_State.panelHeight, CColor(0.0f, 0.0f, 0.0f, 0.0f)))
		return false;

	if (!CreateActions())
		return false;

	StartPassthrough();

	VRLOG("OpenXR session created");
	return true;
}

void Shutdown()
{
	if (g_State.session)
	{
		for (XRSwapChainData& data : g_State.eyeSwapChains)
			data.engineSwapChain.reset();
		g_State.panelSwapChain.engineSwapChain.reset();
		xrDestroySession(g_State.session);
		g_State.session = XR_NULL_HANDLE;
	}
	if (g_State.instance)
	{
		xrDestroyInstance(g_State.instance);
		g_State.instance = XR_NULL_HANDLE;
	}
	g_State.enabled = false;
}

bool IsActive()
{
	return g_State.session != XR_NULL_HANDLE;
}

void GetPanelSize(int& width, int& height)
{
	width = g_State.panelWidth;
	height = g_State.panelHeight;
}

void GetEyeSize(int& width, int& height)
{
	width = g_State.eyeWidth;
	height = g_State.eyeHeight;
}

void GetEyeRenderSize(int& width, int& height)
{
	width = g_State.renderWidth;
	height = g_State.renderHeight;
}

namespace
{
void UpdateRenderScale()
{
	const double now{timer_Time()};
	const double interval{now - g_State.lastFrameStart};
	g_State.lastFrameStart = now;
	const double period{g_State.frameState.predictedDisplayPeriod > 0 ?
		g_State.frameState.predictedDisplayPeriod * 1e-9 : 1.0 / 72.0};
	if (interval > 0.0 && interval < 0.5)
		g_State.averageInterval = g_State.averageInterval * 0.9 + interval * 0.1;

	const float minScale{std::clamp(g_ConfigDB.Get("quest.eye.minscale", 0.65f), 0.3f, 1.0f)};
	const bool dynamic{g_ConfigDB.Get("quest.eye.dynamic", true)};
	if (!dynamic)
		g_State.renderScale = 1.0f;
	else if (g_State.averageInterval > period * 1.06 && now - g_State.lastScaleChange > 0.3)
	{
		// Missing frames: drop resolution quickly.
		g_State.renderScale = std::max(minScale, g_State.renderScale - 0.05f);
		g_State.lastScaleChange = now;
	}
	else if (g_State.averageInterval < period * 1.01 && now - g_State.lastScaleChange > 2.0)
	{
		// Steady at full rate: try a little more.
		g_State.renderScale = std::min(1.0f, g_State.renderScale + 0.025f);
		g_State.lastScaleChange = now;
	}
	g_State.renderWidth = static_cast<int>(g_State.eyeWidth * g_State.renderScale) & ~7;
	g_State.renderHeight = static_cast<int>(g_State.eyeHeight * g_State.renderScale) & ~7;
	static double lastLog{0.0};
	if (now - lastLog > 5.0)
	{
		lastLog = now;
		VRLOG("Render scale %.3f (%dx%d), frame %.1f ms", g_State.renderScale, g_State.renderWidth,
			g_State.renderHeight, g_State.averageInterval * 1000.0);
	}
}
} // anonymous namespace

void BeginFrame()
{
	if (!IsActive())
		return;
	PROFILE3("XR begin frame");

	PollEvents();
	g_State.frameBegun = false;
	if (!g_State.sessionRunning)
	{
		SDL_Delay(10);
		return;
	}

	g_State.frameState = {XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
	if (!Check(xrWaitFrame(g_State.session, &waitInfo, &g_State.frameState), "xrWaitFrame"))
		return;
	XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
	if (!Check(xrBeginFrame(g_State.session, &beginInfo), "xrBeginFrame"))
		return;
	g_State.frameBegun = true;
	UpdateRenderScale();

	XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = g_State.frameState.predictedDisplayTime;
	locateInfo.space = g_State.localSpace;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	uint32_t viewCount{0};
	for (XrView& view : g_State.views)
		view = {XR_TYPE_VIEW};
	g_State.viewsValid =
		XR_SUCCEEDED(xrLocateViews(g_State.session, &locateInfo, &viewState, 2, &viewCount, g_State.views.data())) &&
		viewCount == 2 &&
		(viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
		(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

	UpdateTableMapping();
	UpdateInput();
}

bool IsMixedReality()
{
	return g_State.passthroughLayer && g_State.mixedReality;
}

bool ShouldRender()
{
	return g_State.frameBegun && g_State.frameState.shouldRender;
}

namespace
{
void BuildEyeCamera(int eye, CCamera& eyeCamera);
} // anonymous namespace

void UseMenuMapping();

bool GetMenuEyeCamera(int eye, CCamera& eyeCamera)
{
	if (!g_State.viewsValid || eye < 0 || eye > 1 || !g_State.aimValid[RIGHT])
		return false;
	UseMenuMapping();
	eyeCamera = CCamera();
	BuildEyeCamera(eye, eyeCamera);
	return true;
}

bool GetEyeCamera(int eye, const CCamera& gameCamera, CCamera& eyeCamera, CCamera& cullCamera)
{
	if (!g_State.viewsValid || !g_State.tableValid || eye < 0 || eye > 1)
		return false;

	eyeCamera = gameCamera;
	BuildEyeCamera(eye, eyeCamera);

	// Only draw what's on the table.
	cullCamera = eyeCamera;
	const float range{g_State.tableRange};
	const std::array<CVector3D, 4> normals{
		-g_State.gameRight, g_State.gameRight, -g_State.gameForward, g_State.gameForward};
	for (const CVector3D& normal : normals)
	{
		// Plane keeps points with normal . p + d >= 0, i.e. inside the square.
		CPlane plane;
		plane.m_Norm = normal;
		plane.m_Dist = range - normal.Dot(g_State.pivot);
		cullCamera.ClipFrustum(plane);
	}
	return true;
}

namespace
{
void BuildEyeCamera(int eye, CCamera& eyeCamera)
{

	const XrView& view{g_State.views[eye]};
	const Pose pose{ToPose(view.pose)};

	CMatrix3D orientation;
	const CVector3D right{XRToGameDirection(pose.Rotate(CVector3D(1.0f, 0.0f, 0.0f)))};
	const CVector3D up{XRToGameDirection(pose.Rotate(CVector3D(0.0f, 1.0f, 0.0f)))};
	const CVector3D forward{XRToGameDirection(pose.Rotate(CVector3D(0.0f, 0.0f, -1.0f)))};
	const CVector3D position{XRToGamePoint(pose.position)};
	orientation._11 = right.X; orientation._12 = up.X; orientation._13 = forward.X; orientation._14 = position.X;
	orientation._21 = right.Y; orientation._22 = up.Y; orientation._23 = forward.Y; orientation._24 = position.Y;
	orientation._31 = right.Z; orientation._32 = up.Z; orientation._33 = forward.Z; orientation._34 = position.Z;
	orientation._41 = 0.0f; orientation._42 = 0.0f; orientation._43 = 0.0f; orientation._44 = 1.0f;

	// Near/far in metres, converted to game units.
	const float nearPlane{0.05f / g_State.tableScale};
	const float farPlane{30.0f / g_State.tableScale};
	const float tanLeft{std::tan(view.fov.angleLeft)};
	const float tanRight{std::tan(view.fov.angleRight)};
	const float tanUp{std::tan(view.fov.angleUp)};
	const float tanDown{std::tan(view.fov.angleDown)};
	CMatrix3D projection;
	projection.SetZero();
	projection._11 = 2.0f / (tanRight - tanLeft);
	projection._13 = -(tanRight + tanLeft) / (tanRight - tanLeft);
	projection._22 = 2.0f / (tanUp - tanDown);
	projection._23 = -(tanUp + tanDown) / (tanUp - tanDown);
	projection._33 = -(farPlane + nearPlane) / (nearPlane - farPlane);
	projection._34 = 2.0f * farPlane * nearPlane / (nearPlane - farPlane);
	projection._43 = 1.0f;

	SViewPort viewport{0, 0, g_State.renderWidth, g_State.renderHeight};
	eyeCamera.SetViewPort(viewport);
	eyeCamera.SetProjection(projection);
	eyeCamera.SetNearFarPlanes(nearPlane, farPlane);
	eyeCamera.GetOrientation() = orientation;
	eyeCamera.UpdateFrustum();
}
} // anonymous namespace

void DrawLaser(Renderer::Backend::IDeviceCommandContext& deviceCommandContext, bool)
{
	if (!g_State.aimValid[RIGHT])
		return;
	const Pose& aim{g_State.aim[RIGHT]};
	const CVector3D direction{aim.Rotate(CVector3D(0.0f, 0.0f, -1.0f))};
	const CVector3D start{aim.position + direction * 0.02f};
	CVector3D end{g_State.laserEnd};
	if (!g_State.laserOnPanel && !g_State.laserOnTable)
		end = start + direction * 2.0f;

	const float unitsPerMetre{1.0f / g_State.tableScale};
	CDebugRenderer& debugRenderer{g_Renderer.GetDebugRenderer()};
	const CColor red{1.0f, 0.12f, 0.08f, 1.0f};
	// Not depth tested: buildings and trees must not hide the pointer.
	debugRenderer.DrawLine(deviceCommandContext, XRToGamePoint(start), XRToGamePoint(end),
		red, 0.0008f * unitsPerMetre, false);
	if (!g_State.laserOnTable)
		return;

	// Cursor on the battlefield: a flat ring with a centre dot, lying on the
	// ground and sized in table metres so it reads at any zoom.
	const CVector3D centre{g_State.laserHit + CVector3D(0.0f, 0.002f * unitsPerMetre, 0.0f)};
	const CColor ring{1.0f, 1.0f, 1.0f, 1.0f};
	std::vector<CVector3D> circle;
	constexpr int SEGMENTS{24};
	const float radius{0.012f * unitsPerMetre};
	for (int index = 0; index <= SEGMENTS; ++index)
	{
		const float angle{index * 2.0f * static_cast<float>(M_PI) / SEGMENTS};
		circle.emplace_back(centre + g_State.gameRight * (std::cos(angle) * radius) +
			g_State.gameForward * (std::sin(angle) * radius));
	}
	debugRenderer.DrawLine(deviceCommandContext, circle, CColor(0.0f, 0.0f, 0.0f, 0.6f), 0.0024f * unitsPerMetre, false);
	debugRenderer.DrawLine(deviceCommandContext, circle, ring, 0.0012f * unitsPerMetre, false);
	debugRenderer.DrawCircle(deviceCommandContext, centre, 0.002f * unitsPerMetre, red);

	// Selection box while dragging, aligned with the table like the game's
	// top-down screen rectangle.
	if (g_State.dragOnTable)
	{
		const CVector3D d{g_State.laserHit - g_State.dragStart};
		const CVector3D alongRight{g_State.gameRight * d.Dot(g_State.gameRight)};
		const CVector3D lift{0.0f, 0.003f * unitsPerMetre, 0.0f};
		const CVector3D p0{g_State.dragStart + lift};
		const std::vector<CVector3D> box{p0, p0 + alongRight, p0 + d, p0 + d - alongRight, p0};
		if (d.Length() > 0.004f * unitsPerMetre)
		{
			debugRenderer.DrawLine(deviceCommandContext, box, CColor(0.0f, 0.0f, 0.0f, 0.6f), 0.0024f * unitsPerMetre, false);
			debugRenderer.DrawLine(deviceCommandContext, box, CColor(0.4f, 1.0f, 0.4f, 1.0f), 0.0012f * unitsPerMetre, false);
		}
	}
}

void UseMenuMapping()
{
	// Outside a match, game space is the XR space itself (1 unit = 1 m).
	g_State.pivot = TableCenter();
	g_State.gameRight = TableRight();
	g_State.gameForward = TableForward();
	g_State.tableScale = 1.0f;
}

Renderer::Backend::ISwapChain* GetEyeSwapChain(int eye)
{
	return g_State.eyeSwapChains[eye].engineSwapChain.get();
}

Renderer::Backend::ISwapChain* GetPanelSwapChain()
{
	return g_State.panelSwapChain.engineSwapChain.get();
}

void DrawPanelOverlay(CCanvas2D& canvas)
{
	DrawKeyboard(canvas);
	if (!g_State.mouseValid || !g_State.laserOnPanel)
		return;
	// An arrow with a dark outline, tip at the laser point.
	const float scale{g_VideoMode.GetScale()};
	const float x{g_State.mouseX / scale}, y{g_State.mouseY / scale};
	const std::vector<CVector2D> arrow{
		{x, y}, {x, y + 22.0f}, {x + 6.0f, y + 16.0f}, {x + 15.0f, y + 15.0f}, {x, y}};
	canvas.DrawLine(arrow, 5.0f, CColor(0.0f, 0.0f, 0.0f, 0.8f));
	canvas.DrawLine(arrow, 2.0f, CColor(1.0f, 0.95f, 0.7f, 1.0f));
}

void EndFrame(Renderer::Backend::IDevice* device, bool eyesRendered, bool panelRendered, bool eyesOverlay)
{
	if (!g_State.frameBegun)
		return;
	g_State.frameBegun = false;
	PROFILE3("XR end frame");

	bool anyAcquired{false};
	for (XRSwapChainData& data : g_State.eyeSwapChains)
		anyAcquired |= data.acquired;
	anyAcquired |= g_State.panelSwapChain.acquired;
	if (anyAcquired)
		Renderer::Backend::Vulkan::XR::SubmitFrame(device);
	auto release = [](XRSwapChainData& data)
	{
		if (!data.acquired)
			return;
		XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		Check(xrReleaseSwapchainImage(data.handle, &releaseInfo), "xrReleaseSwapchainImage");
		data.acquired = false;
	};
	for (XRSwapChainData& data : g_State.eyeSwapChains)
		release(data);
	release(g_State.panelSwapChain);

	std::vector<XrCompositionLayerBaseHeader*> layers;

	XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
	const bool passthroughInGame{g_State.mixedReality};
	const bool gameEyes{eyesRendered && !eyesOverlay};
	if (g_State.passthroughLayer && (!gameEyes || passthroughInGame))
	{
		passthroughLayer.layerHandle = g_State.passthroughLayer;
		passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		passthroughLayer.space = XR_NULL_HANDLE;
		layers.emplace_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthroughLayer));
	}

	std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
	XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	if (eyesRendered && g_State.viewsValid)
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			XrCompositionLayerProjectionView& view{projectionViews[eye]};
			view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			view.pose = g_State.views[eye].pose;
			view.fov = g_State.views[eye].fov;
			view.subImage.swapchain = g_State.eyeSwapChains[eye].handle;
			view.subImage.imageRect.offset = {0, 0};
			view.subImage.imageRect.extent = {g_State.renderWidth, g_State.renderHeight};
			view.subImage.imageArrayIndex = 0;
		}
		projectionLayer.space = g_State.localSpace;
		projectionLayer.viewCount = 2;
		projectionLayer.views = projectionViews.data();
		if (eyesOverlay || (g_State.passthroughLayer && passthroughInGame))
			projectionLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
				XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		// In game the battlefield goes under the HUD panel; in menus the
		// laser goes on top of it.
		if (!eyesOverlay)
			layers.emplace_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projectionLayer));
	}

	XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	if (panelRendered)
	{
		// Menus are opaque; in game the HUD floats over the battlefield.
		if (gameEyes)
			quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
				XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		quadLayer.space = g_State.localSpace;
		quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quadLayer.subImage.swapchain = g_State.panelSwapChain.handle;
		quadLayer.subImage.imageRect.offset = {0, 0};
		quadLayer.subImage.imageRect.extent = {g_State.panelWidth, g_State.panelHeight};
		quadLayer.pose = AnchorPose(CVector3D(0.0f, PANEL_UP, -PANEL_FORWARD));
		quadLayer.size = {PANEL_WIDTH, PanelHeight()};
		layers.emplace_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quadLayer));
	}
	if (eyesRendered && eyesOverlay && g_State.viewsValid)
		layers.emplace_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projectionLayer));

	XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
	endInfo.displayTime = g_State.frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = g_State.frameState.shouldRender ? static_cast<uint32_t>(layers.size()) : 0;
	endInfo.layers = layers.data();
	Check(xrEndFrame(g_State.session, &endInfo), "xrEndFrame");
}

CameraInput GetCameraInput()
{
	return g_State.cameraInput;
}

} // namespace QuestVR

#endif // OS_ANDROID
