#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <AbstractClass.h>
#include <ObjectClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/SmartArea.cpp.
//
// Shift+Middle: area-context order (the adaptive sibling of fuzzy attack).
// Scans the range box (nearest ring first) and, for every selected unit, picks
// the first target the engine says the unit can act on (attack / enter /
// capture / sabotage, via ObjectClass::MouseOverObject), respecting a per-target
// "SquadSize" quota. Units that find nothing simply wait - this module never
// issues a move order.
namespace MOEE::SmartArea
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int TARGET_CAP = 256;
		constexpr int UNIT_CAP = 1024;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		OU::PrevLine Lines[OU::PrevCap];
		int LineCount = 0;

		int Range()
		{
			return Phobos::Config::MOEE_SmartArea_Range;
		}

		int Squad()
		{
			return Phobos::Config::MOEE_SmartArea_SquadSize;
		}

		bool IsAttacker(void* p)
		{
			const int what = OU::WhatAmI(p);
			return what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Infantry)
				|| what == static_cast<int>(AbstractType::Aircraft);
		}

		bool IsTargetType(void* p)
		{
			const int what = OU::WhatAmI(p);
			return what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Infantry)
				|| what == static_cast<int>(AbstractType::Aircraft)
				|| what == static_cast<int>(AbstractType::Building);
		}

		// Ctrl is owned by FuzzyAttack, so SmartArea never claims while Ctrl is
		// held (also used by SmartOrder to yield the middle button).
		bool ComboHeld()
		{
			if (!Initialized)
				return false;

			if (Key.disabled)
				return false;

			if (OU::KeyDown(VK_CONTROL))
				return false;

			if ((Key.mods & 1u) && !OU::KeyDown(VK_CONTROL)) return false;
			if ((Key.mods & 2u) && !OU::KeyDown(VK_SHIFT)) return false;
			if ((Key.mods & 4u) && !OU::KeyDown(VK_MENU)) return false;

			for (int i = 0; i < Key.nExtra; ++i)
			{
				if (!OU::KeyDown(Key.extra[i]))
					return false;
			}

			return true;
		}

		int CollectTargets(int cx, int cy, void** targets)
		{
			const int lo = -(Range() / 2);
			const int hi = lo + Range() - 1;
			int n = 0;

			for (int r = 0; r <= Range(); ++r)
			{
				for (int dy = lo; dy <= hi; ++dy)
				{
					for (int dx = lo; dx <= hi; ++dx)
					{
						const int ax = dx > 0 ? dx : -dx;
						const int ay = dy > 0 ? dy : -dy;

						if ((ax > ay ? ax : ay) != r)
							continue;

						const int tx = cx + dx;
						const int ty = cy + dy;

						if (!OU::CellValid(tx, ty))
							continue;

						CellStruct cell;
						cell.X = static_cast<short>(tx);
						cell.Y = static_cast<short>(ty);
						CellClass* pCell = MapClass::Instance.GetCellAt(cell);

						if (!pCell)
							continue;

						for (int layer = 0; layer < 2; ++layer)
						{
							ObjectClass* o = (layer == 0) ? pCell->FirstObject : pCell->AltObject;

							for (int steps = 0; o && steps < 64; ++steps)
							{
								if (!MOEE::IsPlausiblePtr(o))
									break;

								if (IsTargetType(o) && n < TARGET_CAP)
									targets[n++] = o;

								o = o->NextObject;
							}
						}
					}
				}
			}

			return n;
		}

		// None = the unit cannot act on this target.
		Mission ActionToMission(void* unit, void* target)
		{
			auto pObject = static_cast<ObjectClass*>(reinterpret_cast<AbstractClass*>(unit));
			const Action action = pObject->MouseOverObject(
				static_cast<ObjectClass*>(target), false);

			switch (action)
			{
			case Action::Attack:
				return Mission::Attack;
			case Action::Enter:
				return Mission::Enter;
			case Action::Capture:
				return Mission::Capture;
			case Action::Sabotage:
				return Mission::Sabotage;
			default:
				return Mission::None;
			}
		}

		bool Fire(int houseIndex, void* unit, Mission mission, void* target, int cx, int cy)
		{
			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);

			EventClass event(houseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				mission,
				TargetClass(reinterpret_cast<AbstractClass*>(target)),
				TargetClass(cell),
				TargetClass());
			event.MegaMission.IsPlanningEvent = false;

			return EventClass::OutList.Add(event);
		}

		void Press()
		{
			void* units[UNIT_CAP];
			int nu = 0;

			for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && IsAttacker(o))
					units[nu++] = o;
			}

			if (nu <= 0)
				return;

			int cx = 0, cy = 0;

			if (!OU::MouseCell(cx, cy))
				return;

			void* targets[TARGET_CAP];
			const int nt = CollectTargets(cx, cy, targets);

			if (nt <= 0)
				return;

			int used[TARGET_CAP];

			for (int i = 0; i < nt; ++i)
				used[i] = 0;

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;
			int sent = 0;

			for (int u = 0; u < nu; ++u)
			{
				int pick = -1;

				// Prefer a target that still has quota.
				for (int t = 0; t < nt; ++t)
				{
					if (used[t] >= Squad())
						continue;

					if (ActionToMission(units[u], targets[t]) != Mission::None)
					{
						pick = t;
						break;
					}
				}

				// Otherwise take the first target the unit can still act on.
				if (pick < 0)
				{
					for (int t = 0; t < nt; ++t)
					{
						if (ActionToMission(units[u], targets[t]) != Mission::None)
						{
							pick = t;
							break;
						}
					}
				}

				if (pick < 0)
					continue; // nothing this unit can do: it waits

				const Mission mission = ActionToMission(units[u], targets[pick]);

				if (mission == Mission::None)
					continue;

				if (!Fire(houseIndex, units[u], mission, targets[pick], cx, cy))
					break;

				++used[pick];
				++sent;
			}

			if (sent)
				Log("smartarea", "issued=%u targets=%d units=%d", sent, nt, nu);
		}
	}

	// SmartOrder yields the middle button while this combo is held.
	int ClaimsMiddle()
	{
		if (!Initialized)
			return 0;

		if (Key.disabled)
			return 0;

		if (!OU::KeyDown(Key.vk))
			return 0;

		return ComboHeld() ? 1 : 0;
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_SmartArea, Key);
			Log("smartarea", "%s range=%d squad=%d",
				Key.disabled ? "OFF" : "ON", Range(), Squad());
		}

		if (Key.disabled)
			return;

		const bool down = OU::HotkeyDown(Key);

		if (!down && PrevDown && ComboHeld())
			Press();

		PrevDown = down;
	}

	void Draw()
	{
		LineCount = 0;

		if (Key.disabled || !OU::HotkeyDown(Key))
			return;

		int cx = 0, cy = 0;

		if (!OU::MouseCell(cx, cy))
			return;

		// Box outline (same cells as the scan).
		auto pSurface = DSurface::Temp;

		if (pSurface)
		{
			const int lo = -(Range() / 2);
			const int hi = lo + Range();
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
				const COLORREF box = RGB(255, 255, 255);
				pSurface->DrawLine(&p0, &p1, box);
				pSurface->DrawLine(&p1, &p2, box);
				pSurface->DrawLine(&p2, &p3, box);
				pSurface->DrawLine(&p3, &p0, box);
			}
		}
	}
}
