#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <AbstractClass.h>
#include <ObjectClass.h>
#include <TechnoClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <BuildingTypeClass.h>
#include <FootClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>
#include <cstring>

// Port of MO_ExperienceEnhancements/src/DeployPreview.cpp (subset).
//
// Middle-held preview of a base-deployable unit's footprint, plus auto-deploy:
// on release the unit is ordered to the target cell; once it arrives it is
// deployed (EventType::Deploy, byte-identical to pressing D).
//
// Simplification vs upstream: one tracked unit at a time and no along-line
// distribution of several MCVs (the drag still picks the release cell).
namespace MOEE::DeployPreview
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int TRACK_MAX = 8;

		struct Track { void* unit; int cx; int cy; int deadline; };

		Track Tracks[TRACK_MAX];
		bool Active = false;
		bool HaveStart = false;
		int StartX = 0, StartY = 0;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		bool Enabled()
		{
			return Phobos::Config::MOEE_DeployPreview;
		}

		// The deployable unit type of a selected unit (null if none).
		BuildingTypeClass* DeployTargetOf(void* unit)
		{
			const int what = OU::WhatAmI(unit);

			if (what != static_cast<int>(AbstractType::Unit)
				&& what != static_cast<int>(AbstractType::Building))
				return nullptr;

			auto pTechno = static_cast<TechnoClass*>(reinterpret_cast<AbstractClass*>(unit));
			TechnoTypeClass* pType = pTechno->GetTechnoType();

			if (!pType)
				return nullptr;

			return pType->DeploysInto;
		}

		// First selected deployable unit (drives the preview).
		void* FirstDeployable()
		{
			for (int i = 0; i < OU::SelectionCount(); ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && DeployTargetOf(o))
					return o;
			}

			return nullptr;
		}

		bool SendMove(int houseIndex, void* unit, int cx, int cy)
		{
			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);

			EventClass event(houseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				Mission::Move,
				TargetClass(),
				TargetClass(cell),
				TargetClass());

			return EventClass::OutList.Add(event);
		}

		// Deploy event (target constructor, same as upstream's 0x4C65E0 call).
		bool SendDeploy(int houseIndex, void* unit)
		{
			TargetClass whom(reinterpret_cast<AbstractClass*>(unit));

			alignas(EventClass) unsigned char buffer[sizeof(EventClass)];
			std::memset(buffer, 0, sizeof(buffer));
			using Fn_TargetEventCtor = void* (__thiscall*)(void*, int, int, int, unsigned int);
			reinterpret_cast<Fn_TargetEventCtor>(0x4C65E0u)(buffer, houseIndex,
				static_cast<int>(EventType::Deploy), whom.m_ID,
				static_cast<unsigned int>(whom.m_RTTI));

			return EventClass::OutList.Add(*reinterpret_cast<EventClass*>(buffer));
		}

		void AddTrack(void* unit, int cx, int cy)
		{
			for (auto& t : Tracks)
			{
				if (!t.unit)
				{
					t.unit = unit;
					t.cx = cx;
					t.cy = cy;
					t.deadline = Unsorted::CurrentFrame + Phobos::Config::MOEE_DeployPreview_Timeout;
					return;
				}
			}
		}

		void Press(int ex, int ey)
		{
			void* unit = FirstDeployable();

			if (!unit)
				return;

			const int cx = ex;
			const int cy = ey;

			if (!OU::CellValid(cx, cy))
				return;

			if (!HouseClass::CurrentPlayer)
				return;

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;

			if (!SendMove(houseIndex, unit, cx, cy))
				return;

			if (Phobos::Config::MOEE_DeployPreview_AutoDeploy)
				AddTrack(unit, cx, cy);

			Log("deploypreview", "move+track unit=%p cell=%d,%d", unit, cx, cy);
		}
	}

	int ClaimsMiddle()
	{
		if (!Initialized || !Enabled() || Key.disabled)
			return 0;

		if (!OU::KeyDown(Key.vk))
			return 0;

		// Only claims the bare middle button, and only with a deployable selected.
		return FirstDeployable() != nullptr;
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_DeployPreview_Key, Key);
			Log("deploypreview", "%s auto=%d", Key.disabled ? "OFF" : "ON",
				Phobos::Config::MOEE_DeployPreview_AutoDeploy ? 1 : 0);
		}

		const int fr = Unsorted::CurrentFrame;

		// Arrival tracking / auto deploy.
		for (auto& t : Tracks)
		{
			if (!t.unit)
				continue;

			const int what = OU::WhatAmI(t.unit);
			CoordStruct coords;

			const bool alive = (what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Building))
				&& OU::GetCoords(t.unit, coords);

			if (!alive || fr > t.deadline)
			{
				t.unit = nullptr;
				continue;
			}

			const int ux = coords.X / LEP_PER_CELL;
			const int uy = coords.Y / LEP_PER_CELL;

			if (ux == t.cx && uy == t.cy)
			{
				if (HouseClass::CurrentPlayer)
					SendDeploy(HouseClass::CurrentPlayer->ArrayIndex, t.unit);

				t.unit = nullptr;
			}
		}

		if (Key.disabled || !Enabled())
			return;

		const bool down = OU::HotkeyDown(Key);

		if (down && !PrevDown)
		{
			int sx = 0, sy = 0;

			if (OU::MouseCell(sx, sy))
			{
				StartX = sx;
				StartY = sy;
				HaveStart = true;
			}
		}

		if (!down && PrevDown && HaveStart && ClaimsMiddle())
		{
			int ex = 0, ey = 0;

			if (OU::MouseCell(ex, ey))
				Press(ex, ey);
		}

		if (!down)
			HaveStart = false;

		PrevDown = down;
	}

	void Draw()
	{
		if (Key.disabled || !Enabled() || !OU::HotkeyDown(Key))
			return;

		void* unit = FirstDeployable();

		if (!unit)
			return;

		BuildingTypeClass* pInto = DeployTargetOf(unit);

		if (!pInto)
			return;

		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		int cx = 0, cy = 0;

		if (!OU::MouseCell(cx, cy))
			return;

		const int fw = pInto->GetFoundationWidth();
		const int fh = pInto->GetFoundationHeight(false);

		if (fw <= 0 || fh <= 0)
			return;

		const int lo = -(fw / 2);
		const int hi = lo + fw;

		auto corner = [](int gx, int gy, Point2D& out) -> bool
		{
			CoordStruct c = { gx * LEP_PER_CELL, gy * LEP_PER_CELL, 0 };
			c.Z = MapClass::Instance.GetCellFloorHeight(c);
			auto [point, visible] = TacticalClass::Instance->CoordsToClient(c);
			out = point;
			return visible;
		};

		Point2D p0, p1, p2, p3;

		if (corner(cx + lo, cy + lo, p0) && corner(cx + hi, cy + lo, p1)
			&& corner(cx + hi, cy + hi, p2) && corner(cx + lo, cy + hi, p3))
		{
			const COLORREF color = RGB(255, 220, 0);
			pSurface->DrawLine(&p0, &p1, color);
			pSurface->DrawLine(&p1, &p2, color);
			pSurface->DrawLine(&p2, &p3, color);
			pSurface->DrawLine(&p3, &p0, color);
		}
	}
}
