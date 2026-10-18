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

#include "CameraController.h"

#include "graphics/Camera.h"
#include "graphics/Terrain.h"
#include "lib/external_libraries/libsdl.h"
#include "maths/MathUtil.h"
#include "maths/Matrix3D.h"
#include "maths/Quaternion.h"
#include "maths/Vector3D.h"
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/ConfigDB.h"
#include "ps/Game.h"
#include "ps/Globals.h"
#include "ps/Hotkey.h"
#include "ps/QuestVR.h"
#include "ps/VideoMode.h"
#include "ps/World.h"
#include "renderer/Renderer.h"
#include "renderer/SceneRenderer.h"
#include "renderer/WaterManager.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpPosition.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/system/Component.h"

#include <SDL_events.h>
#include <algorithm>
#include <numbers>
#include <functional>
#include <string>

// Maximum distance outside the edge of the map that the camera's
// focus point can be moved
static const float CAMERA_EDGE_MARGIN = 2.0f * TERRAIN_TILE_SIZE;

CCameraController::CCameraController(CCamera& camera)
	: ICameraController(camera),
	  m_ConstrainCamera(true),
	  m_FollowEntity(INVALID_ENTITY),
	  m_FollowFirstPerson(false),
	  m_BirdsEyeView(false),

	  // Dummy values (these will be filled in by the config file)
	  m_ViewScrollSpeed(0),
	  m_ViewScrollSpeedModifier(1),
	  m_ViewScrollMouseDetectDistance(3),
	  m_ViewRotateXSpeed(0),
	  m_ViewRotateXMin(0),
	  m_ViewRotateXMax(0),
	  m_ViewRotateXDefault(0),
	  m_ViewRotateYSpeed(0),
	  m_ViewRotateYSpeedWheel(0),
	  m_ViewRotateYDefault(0),
	  m_ViewRotateSpeedModifier(1),
	  m_ViewDragSpeed(0),
	  m_ViewDragInverted(false),
	  m_ViewZoomSpeed(0),
	  m_ViewZoomSpeedWheel(0),
	  m_ViewZoomMin(0),
	  m_ViewZoomMax(0),
	  m_ViewZoomDefault(0),
	  m_ViewZoomSpeedModifier(1),
	  m_ViewFOV(DEGTORAD(45.f)),
	  m_ViewNear(2.f),
	  m_ViewFar(4096.f),
	  m_HeightSmoothness(0.5f),
	  m_HeightMin(16.f),

	  m_PosX(0, 0, 0.01f),
	  m_PosY(0, 0, 0.01f),
	  m_PosZ(0, 0, 0.01f),
	  m_Zoom(0, 0, 0.1f),
	  m_RotateX(0, 0, 0.001f),
	  m_RotateY(0, 0, 0.001f)
{
	m_ViewDragInvertedConfigHook =
		std::make_unique<CConfigDBHook>(g_ConfigDB.RegisterHookAndCall("view.drag.inverted", [this]() {
			m_ViewDragInverted = g_ConfigDB.Get("view.drag.inverted", m_ViewDragInverted);
		}));
	m_ViewDragSpeedConfigHook =
		std::make_unique<CConfigDBHook>(g_ConfigDB.RegisterHookAndCall("view.drag.speed", [this]() {
			m_ViewDragSpeed = g_ConfigDB.Get("view.drag.speed", m_ViewDragSpeed);
		}));

	SViewPort vp;
	vp.m_X = 0;
	vp.m_Y = 0;
	vp.m_Width = g_VideoMode.GetWindowWidth();
	vp.m_Height = g_VideoMode.GetWindowHeight();
	m_Camera.SetViewPort(vp);

	SetCameraProjection();
	SetupCameraMatrixSmooth(&m_Camera.m_Orientation);
	m_Camera.UpdateFrustum();
}

CCameraController::~CCameraController() = default;

void CCameraController::LoadConfig()
{
	m_ViewScrollSpeed = g_ConfigDB.Get("view.scroll.speed", m_ViewScrollSpeed);
	m_ViewScrollSpeedModifier = g_ConfigDB.Get("view.scroll.speed.modifier", m_ViewScrollSpeedModifier);
	m_ViewScrollMouseDetectDistance = g_ConfigDB.Get("view.scroll.mouse.detectdistance",
		m_ViewScrollMouseDetectDistance);
	m_ViewRotateXSpeed = g_ConfigDB.Get("view.rotate.x.speed", m_ViewRotateXSpeed);
	m_ViewRotateXMin = g_ConfigDB.Get("view.rotate.x.min", m_ViewRotateXMin);
	m_ViewRotateXMax = g_ConfigDB.Get("view.rotate.x.max", m_ViewRotateXMax);
	m_ViewRotateXDefault = g_ConfigDB.Get("view.rotate.x.default", m_ViewRotateXDefault);
	m_ViewRotateYSpeed = g_ConfigDB.Get("view.rotate.y.speed", m_ViewRotateYSpeed);
	m_ViewRotateYSpeedWheel = g_ConfigDB.Get("view.rotate.y.speed.wheel", m_ViewRotateYSpeedWheel);
	m_ViewRotateYDefault = g_ConfigDB.Get("view.rotate.y.default", m_ViewRotateYDefault);
	m_ViewRotateSpeedModifier = g_ConfigDB.Get("view.rotate.speed.modifier", m_ViewRotateSpeedModifier);
	m_ViewDragSpeed = g_ConfigDB.Get("view.drag.speed", m_ViewDragSpeed);
	m_ViewDragInverted = g_ConfigDB.Get("view.drag.inverted", m_ViewDragInverted);
	m_ViewZoomSpeed = g_ConfigDB.Get("view.zoom.speed", m_ViewZoomSpeed);
	m_ViewZoomSpeedWheel = g_ConfigDB.Get("view.zoom.speed.wheel", m_ViewZoomSpeedWheel);
	m_ViewZoomMin = g_ConfigDB.Get("view.zoom.min", m_ViewZoomMin);
	m_ViewZoomMax = g_ConfigDB.Get("view.zoom.max", m_ViewZoomMax);
	m_ViewZoomDefault = g_ConfigDB.Get("view.zoom.default", m_ViewZoomDefault);
	m_ViewZoomSpeedModifier = g_ConfigDB.Get("view.zoom.speed.modifier", m_ViewZoomSpeedModifier);

	m_HeightSmoothness = g_ConfigDB.Get("view.height.smoothness", m_HeightSmoothness);
	m_HeightMin = g_ConfigDB.Get("view.height.min", m_HeightMin);

#define SETUP_SMOOTHNESS(CFG_PREFIX, SMOOTHED_VALUE) \
	SMOOTHED_VALUE.SetSmoothness( \
		g_ConfigDB.Get(CFG_PREFIX ".smoothness", SMOOTHED_VALUE.GetSmoothness()));

	SETUP_SMOOTHNESS("view.pos", m_PosX);
	SETUP_SMOOTHNESS("view.pos", m_PosY);
	SETUP_SMOOTHNESS("view.pos", m_PosZ);
	SETUP_SMOOTHNESS("view.zoom", m_Zoom);
	SETUP_SMOOTHNESS("view.rotate.x", m_RotateX);
	SETUP_SMOOTHNESS("view.rotate.y", m_RotateY);
#undef SETUP_SMOOTHNESS

	m_ViewNear = g_ConfigDB.Get("view.near", m_ViewNear);
	m_ViewFar = g_ConfigDB.Get("view.far", m_ViewFar);
	m_ViewFOV = g_ConfigDB.Get("view.fov", m_ViewFOV);

	// Convert to radians
	m_RotateX.SetValue(DEGTORAD(m_ViewRotateXDefault));
	m_RotateY.SetValue(DEGTORAD(m_ViewRotateYDefault));
	m_ViewFOV = DEGTORAD(m_ViewFOV);
}

void CCameraController::SetViewport(const SViewPort& vp)
{
	m_Camera.SetViewPort(vp);
	SetCameraProjection();
}

void CCameraController::Update(const float deltaRealTime)
{
	// Calculate mouse movement
	static int mouse_last_x = 0;
	static int mouse_last_y = 0;
	int mouse_dx = g_mouse_x - mouse_last_x;
	int mouse_dy = g_mouse_y - mouse_last_y;
	mouse_last_x = g_mouse_x;
	mouse_last_y = g_mouse_y;

	if (HotkeyIsPressed("camera.rotate.cw"))
		m_RotateY.AddSmoothly(m_ViewRotateYSpeed * deltaRealTime);
	if (HotkeyIsPressed("camera.rotate.ccw"))
		m_RotateY.AddSmoothly(-m_ViewRotateYSpeed * deltaRealTime);
	if (HotkeyIsPressed("camera.rotate.up") && !m_BirdsEyeView)
		m_RotateX.AddSmoothly(-m_ViewRotateXSpeed * deltaRealTime);
	if (HotkeyIsPressed("camera.rotate.down") && !m_BirdsEyeView)
		m_RotateX.AddSmoothly(m_ViewRotateXSpeed * deltaRealTime);

	float moveRightward = 0.f;
	float moveForward = 0.f;

	if (HotkeyIsPressed("camera.pan"))
	{
		if (m_ViewDragInverted) {
			moveRightward += m_ViewDragSpeed * -mouse_dx;
			moveForward += m_ViewDragSpeed * mouse_dy;
		}
		else {
			moveRightward += m_ViewDragSpeed * mouse_dx;
			moveForward += m_ViewDragSpeed * -mouse_dy;
		}
	}

	if (g_mouse_active && m_ViewScrollMouseDetectDistance > 0)
	{
		if (g_mouse_x >= g_VideoMode.GetWindowWidth() - m_ViewScrollMouseDetectDistance && g_mouse_x < g_VideoMode.GetWindowWidth())
			moveRightward += m_ViewScrollSpeed * deltaRealTime;
		else if (g_mouse_x < m_ViewScrollMouseDetectDistance && g_mouse_x >= 0)
			moveRightward -= m_ViewScrollSpeed * deltaRealTime;

		if (g_mouse_y >= g_VideoMode.GetWindowHeight() - m_ViewScrollMouseDetectDistance && g_mouse_y < g_VideoMode.GetWindowHeight())
			moveForward -= m_ViewScrollSpeed * deltaRealTime;
		else if (g_mouse_y < m_ViewScrollMouseDetectDistance && g_mouse_y >= 0)
			moveForward += m_ViewScrollSpeed * deltaRealTime;
	}

	if (HotkeyIsPressed("camera.right"))
		moveRightward += m_ViewScrollSpeed * deltaRealTime;
	if (HotkeyIsPressed("camera.left"))
		moveRightward -= m_ViewScrollSpeed * deltaRealTime;
	if (HotkeyIsPressed("camera.up"))
		moveForward += m_ViewScrollSpeed * deltaRealTime;
	if (HotkeyIsPressed("camera.down"))
		moveForward -= m_ViewScrollSpeed * deltaRealTime;

	{
		// Thumbsticks in Quest VR; panning speed follows the zoom so the
		// tabletop moves at a steady pace.
		const QuestVR::CameraInput vrInput{QuestVR::GetCameraInput()};
		const float distance{(m_Camera.GetOrientation().GetTranslation() - GetSmoothPivot(m_Camera)).Length()};
		const float zoomFactor{std::max(0.5f, distance / std::max(1.0f, m_ViewZoomDefault))};
		moveRightward += vrInput.right * m_ViewScrollSpeed * zoomFactor * deltaRealTime;
		moveForward += vrInput.forward * m_ViewScrollSpeed * zoomFactor * deltaRealTime;
		if (vrInput.rotate)
			m_RotateY.AddSmoothly(-vrInput.rotate * m_ViewRotateYSpeed * deltaRealTime);
		if (vrInput.zoom)
			m_Zoom.AddSmoothly(-vrInput.zoom * m_ViewZoomSpeed * deltaRealTime);
	}

	if (moveRightward || moveForward)
	{
		// Break out of following mode when the user starts scrolling
		m_FollowEntity = INVALID_ENTITY;

		float s = sin(m_RotateY.GetSmoothedValue());
		float c = cos(m_RotateY.GetSmoothedValue());
		m_PosX.AddSmoothly(c * moveRightward);
		m_PosZ.AddSmoothly(-s * moveRightward);
		m_PosX.AddSmoothly(s * moveForward);
		m_PosZ.AddSmoothly(c * moveForward);
	}

	if (m_FollowEntity)
	{
		CmpPtr<ICmpPosition> cmpPosition(*(g_Game->GetSimulation2()), m_FollowEntity);
		CmpPtr<ICmpRangeManager> cmpRangeManager(*(g_Game->GetSimulation2()), SYSTEM_ENTITY);
		if (cmpPosition && cmpPosition->IsInWorld() &&
			cmpRangeManager && cmpRangeManager->GetLosVisibility(m_FollowEntity, g_Game->GetViewedPlayerID()) == LosVisibility::VISIBLE)
		{
			// Get the most recent interpolated position
			float frameOffset = g_Game->GetSimulation2()->GetLastFrameOffset();
			CMatrix3D transform = cmpPosition->GetInterpolatedTransform(frameOffset);
			CVector3D pos = transform.GetTranslation();

			if (m_FollowFirstPerson)
			{
				float x, z, angle;
				cmpPosition->GetInterpolatedPosition2D(frameOffset, x, z, angle);
				float height = 4.f;
				m_Camera.m_Orientation.SetIdentity();
				m_Camera.m_Orientation.RotateX(std::numbers::pi_v<float> / 24.f);
				m_Camera.m_Orientation.RotateY(angle);
				m_Camera.m_Orientation.Translate(pos.X, pos.Y + height, pos.Z);

				m_Camera.UpdateFrustum();
				return;
			}
			else
			{
				// Move the camera to match the unit
				CCamera targetCam = m_Camera;
				SetupCameraMatrixSmoothRot(&targetCam.m_Orientation);

				CVector3D pivot = GetSmoothPivot(targetCam);
				CVector3D delta = pos - pivot;
				m_PosX.AddSmoothly(delta.X);
				m_PosY.AddSmoothly(delta.Y);
				m_PosZ.AddSmoothly(delta.Z);
			}
		}
		else
		{
			// The unit disappeared (died or garrisoned etc), so stop following it
			m_FollowEntity = INVALID_ENTITY;
		}
	}

	if (HotkeyIsPressed("camera.zoom.in"))
		m_Zoom.AddSmoothly(-m_ViewZoomSpeed * deltaRealTime);
	if (HotkeyIsPressed("camera.zoom.out"))
		m_Zoom.AddSmoothly(m_ViewZoomSpeed * deltaRealTime);

	if (m_ConstrainCamera)
		m_Zoom.ClampSmoothly(m_ViewZoomMin, m_ViewZoomMax);

	float zoomDelta = -m_Zoom.Update(deltaRealTime);
	if (zoomDelta)
	{
		CVector3D forwards = m_Camera.GetOrientation().GetIn();
		m_PosX.AddSmoothly(forwards.X * zoomDelta);
		m_PosY.AddSmoothly(forwards.Y * zoomDelta);
		m_PosZ.AddSmoothly(forwards.Z * zoomDelta);
	}

	if (m_ConstrainCamera)
	{
		if (m_BirdsEyeView)
			m_RotateX.ClampSmoothly(std::numbers::pi_v<float> / 2.f, std::numbers::pi_v<float> / 2.f);
		else
			m_RotateX.ClampSmoothly(DEGTORAD(m_ViewRotateXMin), DEGTORAD(m_ViewRotateXMax));
	}

	FocusHeight(true);

	// Ensure the ViewCamera focus is inside the map with the chosen margins
	// if not so - apply margins to the camera
	if (m_ConstrainCamera)
	{
		CCamera targetCam = m_Camera;
		SetupCameraMatrixSmoothRot(&targetCam.m_Orientation);

		const CTerrain& terrain = g_Game->GetWorld()->GetTerrain();

		CVector3D pivot = GetSmoothPivot(targetCam);
		CVector3D delta = targetCam.GetOrientation().GetTranslation() - pivot;

		CVector3D desiredPivot = pivot;

		CmpPtr<ICmpRangeManager> cmpRangeManager(*(g_Game->GetSimulation2()), SYSTEM_ENTITY);
		if (cmpRangeManager && cmpRangeManager->GetLosCircular())
		{
			// Clamp to a circular region around the center of the map
			const float r = terrain.GetMaxX() / 2.f;
			CVector3D center(r, desiredPivot.Y, r);
			float dist = (desiredPivot - center).Length();
			if (dist > r - CAMERA_EDGE_MARGIN)
				desiredPivot = center + (desiredPivot - center).Normalized() * (r - CAMERA_EDGE_MARGIN);
		}
		else
		{
			// Clamp to the square edges of the map
			desiredPivot.X = Clamp(desiredPivot.X, terrain.GetMinX() + CAMERA_EDGE_MARGIN,
				terrain.GetMaxX() - CAMERA_EDGE_MARGIN);
			desiredPivot.Z = Clamp(desiredPivot.Z, terrain.GetMinZ() + CAMERA_EDGE_MARGIN,
				terrain.GetMaxZ() - CAMERA_EDGE_MARGIN);
		}

		// Update the position so that pivot is within the margin
		m_PosX.SetValueSmoothly(desiredPivot.X + delta.X);
		m_PosZ.SetValueSmoothly(desiredPivot.Z + delta.Z);
	}

	m_PosX.Update(deltaRealTime);
	m_PosY.Update(deltaRealTime);
	m_PosZ.Update(deltaRealTime);

	// Handle rotation around the Y (vertical) axis
	{
		CCamera targetCam = m_Camera;
		SetupCameraMatrixSmooth(&targetCam.m_Orientation);

		float rotateYDelta = m_RotateY.Update(deltaRealTime);
		if (rotateYDelta)
		{
			// We've updated RotateY, and need to adjust Pos so that it's still
			// facing towards the original focus point (the terrain in the center
			// of the screen).

			CVector3D upwards(0.0f, 1.0f, 0.0f);

			CVector3D pivot = GetSmoothPivot(targetCam);
			CVector3D delta = targetCam.GetOrientation().GetTranslation() - pivot;

			CQuaternion q;
			q.FromAxisAngle(upwards, rotateYDelta);
			CVector3D d = q.Rotate(delta) - delta;

			m_PosX.Add(d.X);
			m_PosY.Add(d.Y);
			m_PosZ.Add(d.Z);
		}
	}

	// Handle rotation around the X (sideways, relative to camera) axis
	{
		CCamera targetCam = m_Camera;
		SetupCameraMatrixSmooth(&targetCam.m_Orientation);

		float rotateXDelta = m_RotateX.Update(deltaRealTime);
		if (rotateXDelta)
		{
			CVector3D rightwards = targetCam.GetOrientation().GetLeft() * -1.0f;

			CVector3D pivot = GetSmoothPivot(targetCam);
			CVector3D delta = targetCam.GetOrientation().GetTranslation() - pivot;

			CQuaternion q;
			q.FromAxisAngle(rightwards, rotateXDelta);
			CVector3D d = q.Rotate(delta) - delta;

			m_PosX.Add(d.X);
			m_PosY.Add(d.Y);
			m_PosZ.Add(d.Z);
		}
	}

	/* This is disabled since it doesn't seem necessary:

	// Ensure the camera's near point is never inside the terrain
	if (m_ConstrainCamera)
	{
	CMatrix3D target;
	target.SetIdentity();
	target.RotateX(m_RotateX.GetValue());
	target.RotateY(m_RotateY.GetValue());
	target.Translate(m_PosX.GetValue(), m_PosY.GetValue(), m_PosZ.GetValue());

	CVector3D nearPoint = target.GetTranslation() + target.GetIn() * defaultNear;
	float ground = g_Game->GetWorld()->GetTerrain()->GetExactGroundLevel(nearPoint.X, nearPoint.Z);
	float limit = ground + 16.f;
	if (nearPoint.Y < limit)
	m_PosY.AddSmoothly(limit - nearPoint.Y);
	}
	*/

	m_RotateY.Wrap(-std::numbers::pi_v<float>, std::numbers::pi_v<float>);

	// Update the camera matrix
	SetCameraProjection();
	SetupCameraMatrixSmooth(&m_Camera.m_Orientation);
	m_Camera.UpdateFrustum();
}

CVector3D CCameraController::GetSmoothPivot(CCamera& camera) const
{
	return camera.GetOrientation().GetTranslation() + camera.GetOrientation().GetIn() * m_Zoom.GetSmoothedValue();
}

CVector3D CCameraController::GetCameraPivot() const
{
	return GetSmoothPivot(m_Camera);
}

CVector3D CCameraController::GetCameraPosition() const
{
	return CVector3D(m_PosX.GetValue(), m_PosY.GetValue(), m_PosZ.GetValue());
}

CVector3D CCameraController::GetCameraRotation() const
{
	// The angle of rotation around the Z axis is not used.
	return CVector3D(m_RotateX.GetValue(), m_RotateY.GetValue(), 0.0f);
}

float CCameraController::GetCameraZoom() const
{
	return m_Zoom.GetValue();
}

void CCameraController::SetCamera(const CVector3D& pos, float rotX, float rotY, float zoom)
{
	m_PosX.SetValue(pos.X);
	m_PosY.SetValue(pos.Y);
	m_PosZ.SetValue(pos.Z);
	m_RotateX.SetValue(rotX);
	m_RotateY.SetValue(rotY);
	m_Zoom.SetValue(zoom);

	FocusHeight(false);

	SetupCameraMatrixNonSmooth(&m_Camera.m_Orientation);
	m_Camera.UpdateFrustum();

	// Break out of following mode so the camera really moves to the target
	m_FollowEntity = INVALID_ENTITY;

	// Leave bird's eye view
	m_BirdsEyeView = false;
}

void CCameraController::MoveCameraTarget(const CVector3D& target)
{
	// Maintain the same orientation and level of zoom, if we can
	// (do this by working out the point the camera is looking at, saving
	//  the difference between that position and the camera point, and restoring
	//  that difference to our new target)

	CCamera targetCam = m_Camera;
	SetupCameraMatrixNonSmooth(&targetCam.m_Orientation);

	CVector3D pivot = GetSmoothPivot(targetCam);
	CVector3D delta = target - pivot;

	m_PosX.SetValueSmoothly(delta.X + m_PosX.GetValue());
	m_PosZ.SetValueSmoothly(delta.Z + m_PosZ.GetValue());

	FocusHeight(false);

	// Break out of following mode so the camera really moves to the target
	m_FollowEntity = INVALID_ENTITY;
}

void CCameraController::ResetCameraTarget(const CVector3D& target)
{
	CMatrix3D orientation;
	orientation.SetIdentity();
	orientation.RotateX(DEGTORAD(m_ViewRotateXDefault));
	orientation.RotateY(DEGTORAD(m_ViewRotateYDefault));

	CVector3D delta = orientation.GetIn() * m_ViewZoomDefault;
	m_PosX.SetValue(target.X - delta.X);
	m_PosY.SetValue(target.Y - delta.Y);
	m_PosZ.SetValue(target.Z - delta.Z);
	m_RotateX.SetValue(DEGTORAD(m_ViewRotateXDefault));
	m_RotateY.SetValue(DEGTORAD(m_ViewRotateYDefault));
	m_Zoom.SetValue(m_ViewZoomDefault);

	FocusHeight(false);

	SetupCameraMatrixSmooth(&m_Camera.m_Orientation);
	m_Camera.UpdateFrustum();

	// Break out of following mode so the camera really moves to the target
	m_FollowEntity = INVALID_ENTITY;

	// Leave bird's eye view
	m_BirdsEyeView = false;
}

void CCameraController::FollowEntity(entity_id_t entity, bool firstPerson)
{
	m_FollowEntity = entity;
	m_FollowFirstPerson = firstPerson;
}

entity_id_t CCameraController::GetFollowedEntity()
{
	return m_FollowEntity;
}

void CCameraController::SetCameraProjection()
{
	m_Camera.SetPerspectiveProjection(m_ViewNear, m_ViewFar, m_ViewFOV);
}

void CCameraController::ResetCameraAngleZoom()
{
	CCamera targetCam = m_Camera;
	SetupCameraMatrixNonSmooth(&targetCam.m_Orientation);

	// Compute the zoom adjustment to get us back to the default
	CVector3D forwards = targetCam.GetOrientation().GetIn();

	CVector3D pivot = GetSmoothPivot(targetCam);
	CVector3D delta = pivot - targetCam.GetOrientation().GetTranslation();
	float dist = delta.Dot(forwards);
	m_Zoom.AddSmoothly(m_ViewZoomDefault - dist);

	// Reset orientations to default
	m_RotateX.SetValueSmoothly(DEGTORAD(m_ViewRotateXDefault));
	m_RotateY.SetValueSmoothly(DEGTORAD(m_ViewRotateYDefault));

	// Leave bird's eye view
	m_BirdsEyeView = false;
}

void CCameraController::ToggleBirdsEyeView()
{
	m_BirdsEyeView = !m_BirdsEyeView;
	float angle = m_BirdsEyeView ? std::numbers::pi_v<float> / 2.f : DEGTORAD(m_ViewRotateXDefault);
	m_RotateX.SetValueSmoothly(angle);
}

void CCameraController::SetupCameraMatrixSmooth(CMatrix3D* orientation)
{
	orientation->SetIdentity();
	orientation->RotateX(m_RotateX.GetSmoothedValue());
	orientation->RotateY(m_RotateY.GetSmoothedValue());
	orientation->Translate(m_PosX.GetSmoothedValue(), m_PosY.GetSmoothedValue(), m_PosZ.GetSmoothedValue());
}

void CCameraController::SetupCameraMatrixSmoothRot(CMatrix3D* orientation)
{
	orientation->SetIdentity();
	orientation->RotateX(m_RotateX.GetSmoothedValue());
	orientation->RotateY(m_RotateY.GetSmoothedValue());
	orientation->Translate(m_PosX.GetValue(), m_PosY.GetValue(), m_PosZ.GetValue());
}

void CCameraController::SetupCameraMatrixNonSmooth(CMatrix3D* orientation)
{
	orientation->SetIdentity();
	orientation->RotateX(m_RotateX.GetValue());
	orientation->RotateY(m_RotateY.GetValue());
	orientation->Translate(m_PosX.GetValue(), m_PosY.GetValue(), m_PosZ.GetValue());
}

void CCameraController::FocusHeight(bool smooth)
{
	/*
	The camera pivot height is moved towards ground level.
	To prevent excessive zoom when looking over a cliff,
	the target ground level is the maximum of the ground level at the camera's near and pivot points.
	The ground levels are filtered to achieve smooth camera movement.
	The filter radius is proportional to the zoom level.
	The camera height is clamped to prevent map penetration.
	*/

	if (!m_ConstrainCamera)
		return;

	CCamera targetCam = m_Camera;
	SetupCameraMatrixSmoothRot(&targetCam.m_Orientation);

	const CVector3D position = targetCam.GetOrientation().GetTranslation();
	const CVector3D forwards = targetCam.GetOrientation().GetIn();

	// horizontal view radius
	const float radius = sqrtf(forwards.X * forwards.X + forwards.Z * forwards.Z) * m_Zoom.GetSmoothedValue();
	const float near_radius = radius * m_HeightSmoothness;
	const float pivot_radius = radius * m_HeightSmoothness;

	const CVector3D nearPoint = position + forwards * m_ViewNear;
	const CVector3D pivotPoint = position + forwards * m_Zoom.GetSmoothedValue();

	const float ground = std::max(
		g_Game->GetWorld()->GetTerrain().GetExactGroundLevel(nearPoint.X, nearPoint.Z),
		g_Renderer.GetSceneRenderer().GetWaterManager().m_WaterHeight);

	// filter ground levels for smooth camera movement
	const float filtered_near_ground = g_Game->GetWorld()->GetTerrain().GetFilteredGroundLevel(nearPoint.X, nearPoint.Z, near_radius);
	const float filtered_pivot_ground = g_Game->GetWorld()->GetTerrain().GetFilteredGroundLevel(pivotPoint.X, pivotPoint.Z, pivot_radius);

	// filtered maximum visible ground level in view
	const float filtered_ground = std::max(
		std::max(filtered_near_ground, filtered_pivot_ground),
		g_Renderer.GetSceneRenderer().GetWaterManager().m_WaterHeight);

	// target camera height above pivot point
	const float pivot_height = -forwards.Y * (m_Zoom.GetSmoothedValue() - m_ViewNear);

	// minimum camera height above filtered ground level
	const float min_height = (m_HeightMin + ground - filtered_ground);

	const float target_height = std::max(pivot_height, min_height);
	const float height = (nearPoint.Y - filtered_ground);
	const float diff = target_height - height;

	if (fabsf(diff) < 0.0001f)
		return;

	if (smooth)
		m_PosY.AddSmoothly(diff);
	else
		m_PosY.Add(diff);
}

Input::Reaction CCameraController::HandleEvent(const SDL_Event& ev)
{
	switch (ev.type)
	{
	case SDL_HOTKEYPRESS:
	{
		std::string hotkey = static_cast<const char*>(ev.user.data1);
		if (hotkey == "camera.reset")
		{
			ResetCameraAngleZoom();
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.togglebirdseyeview")
		{
			ToggleBirdsEyeView();
			return Input::Reaction::HANDLED;
		}
		return Input::Reaction::PASS;
	}

	case SDL_HOTKEYDOWN:
	{
		std::string hotkey = static_cast<const char*>(ev.user.data1);

		// Mouse wheel must be treated using events instead of polling,
		// because SDL auto-generates a sequence of mousedown/mouseup events
		// and we never get to see the "down" state inside Update().
		if (hotkey == "camera.zoom.wheel.in")
		{
			m_Zoom.AddSmoothly(-m_ViewZoomSpeedWheel);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.zoom.wheel.out")
		{
			m_Zoom.AddSmoothly(m_ViewZoomSpeedWheel);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.rotate.wheel.cw")
		{
			m_RotateY.AddSmoothly(m_ViewRotateYSpeedWheel);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.rotate.wheel.ccw")
		{
			m_RotateY.AddSmoothly(-m_ViewRotateYSpeedWheel);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.scroll.speed.increase")
		{
			m_ViewScrollSpeed *= m_ViewScrollSpeedModifier;
			LOGMESSAGERENDER("Scroll speed increased to %.1f", m_ViewScrollSpeed);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.scroll.speed.decrease")
		{
			m_ViewScrollSpeed /= m_ViewScrollSpeedModifier;
			LOGMESSAGERENDER("Scroll speed decreased to %.1f", m_ViewScrollSpeed);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.rotate.speed.increase")
		{
			m_ViewRotateXSpeed *= m_ViewRotateSpeedModifier;
			m_ViewRotateYSpeed *= m_ViewRotateSpeedModifier;
			LOGMESSAGERENDER("Rotate speed increased to X=%.3f, Y=%.3f", m_ViewRotateXSpeed, m_ViewRotateYSpeed);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.rotate.speed.decrease")
		{
			m_ViewRotateXSpeed /= m_ViewRotateSpeedModifier;
			m_ViewRotateYSpeed /= m_ViewRotateSpeedModifier;
			LOGMESSAGERENDER("Rotate speed decreased to X=%.3f, Y=%.3f", m_ViewRotateXSpeed, m_ViewRotateYSpeed);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.zoom.speed.increase")
		{
			m_ViewZoomSpeed *= m_ViewZoomSpeedModifier;
			LOGMESSAGERENDER("Zoom speed increased to %.1f", m_ViewZoomSpeed);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "camera.zoom.speed.decrease")
		{
			m_ViewZoomSpeed /= m_ViewZoomSpeedModifier;
			LOGMESSAGERENDER("Zoom speed decreased to %.1f", m_ViewZoomSpeed);
			return Input::Reaction::HANDLED;
		}
		return Input::Reaction::PASS;
	}
	}

	return Input::Reaction::PASS;
}
