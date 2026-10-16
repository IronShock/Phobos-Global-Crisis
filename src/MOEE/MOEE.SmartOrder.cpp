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

// Port of MO_ExperienceEnhancements/src/SmartOrder.cpp (core).
//
// Bare Middle click: context-sensitive order.
//   - If a target object is under the cursor, ask the engine (per unit, via
//     ObjectClass::MouseOverObject) what the click would do and issue that
//     mission (Attack / Enter / Capture), falling back to Move.
//   - On empty ground, scatter the selection around the clicked cell (infantry
//     pack 3 per cell, everything else 1).
// All orders are standard MegaMission events on OutList (simulation-safe).
//
// Note: the upstream line-drag re-sampling (curved scatter along a drawn path)
// is not ported yet; a straight drag currently falls back to the point scatter
// at the release cell.
namespace MOEE::SmartOrder
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int UNIT_CAP = 1024;
		constexpr int SCATTER_R = 5;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		OU::PrevLine Lines[OU::PrevCap];
		int LineCount = 0;

		bool IsFoot(void* p)
		{
			const int what = OU::WhatAmI(p);
			return what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Infantry)
				|| what == static_cast<int>(AbstractType::Aircraft);
		}

		// The bare middle button yields to the other middle-click combos:
		// Ctrl -> FuzzyAttack, Shift -> SmartArea, Z -> PatrolCircle.
		bool ModifiersBlock()
		{
			return OU::KeyDown(VK_CONTROL) || OU::KeyDown(VK_SHIFT) || OU::KeyDown('Z')
				|| DeployPreview::ClaimsMiddle() != 0;
		}

		// Infantry pack 3 per cell, everything else 1.
		int CellCapacity(void* unit)
		{
			return OU::WhatAmI(unit) == static_cast<int>(AbstractType::Infantry) ? 3 : 1;
		}

		bool CellFree(int cx, int cy)
		{
			if (!OU::CellValid(cx, cy))
				return false;

			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);
			CellClass* pCell = MapClass::Instance.GetCellAt(cell);

			if (!pCell)
				return false;

			for (int layer = 0; layer < 2; ++layer)
			{
				ObjectClass* o = (layer == 0) ? pCell->FirstObject : pCell->AltObject;

				for (int steps = 0; o && steps < 64; ++steps)
				{
					if (!MOEE::IsPlausiblePtr(o))
						break;

					const auto what = o->WhatAmI();

					if (what == AbstractType::Building || what == AbstractType::Terrain)
						return false;

					o = o->NextObject;
				}
			}

			return true;
		}

		Mission ActionToMission(void* unit, void* target)
		{
			auto pObject = static_cast<ObjectClass*>(reinterpret_cast<AbstractClass*>(unit));

			// Ask the engine what this click means for this specific unit.
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
				return Mission::Move;
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
				target ? TargetClass(reinterpret_cast<AbstractClass*>(target)) : TargetClass(),
				TargetClass(cell),
				TargetClass());
			event.MegaMission.IsPlanningEvent = false;

			return EventClass::OutList.Add(event);
		}

		// Assign scatter destinations to the units, nearest cell first.
		// Fills outCx/outCy with the destination of each unit.
		void AssignScatter(void* units[], int nu, int cx, int cy, int* outCx, int* outCy)
		{
			int assigned = 0;

			// Ring scan: nearest cells first.
			for (int r = 0; r <= SCATTER_R && assigned < nu; ++r)
			{
				for (int dy = -r; dy <= r && assigned < nu; ++dy)
				{
					for (int dx = -r; dx <= r && assigned < nu; ++dx)
					{
						const int ax = dx > 0 ? dx : -dx;
						const int ay = dy > 0 ? dy : -dy;

						if ((ax > ay ? ax : ay) != r)
							continue;

						const int tx = cx + dx;
						const int ty = cy + dy;

						if (!CellFree(tx, ty))
							continue;

						int cap = 0;

						while (assigned < nu && cap < 3)
						{
							const int need = CellCapacity(units[assigned]);

							if (cap + need > 3 && cap > 0)
								break;

							outCx[assigned] = tx;
							outCy[assigned] = ty;
							cap += need;
							++assigned;
						}
					}
				}
			}

			// Anyone left (no free cell found) stays on the click cell.
			for (; assigned < nu; ++assigned)
			{
				outCx[assigned] = cx;
				outCy[assigned] = cy;
			}
		}

		void Press()
		{
			void* units[UNIT_CAP];
			int nu = 0;

			for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && IsFoot(o))
					units[nu++] = o;
			}

			if (nu <= 0)
				return;

			int cx = 0, cy = 0;
			ObjectClass* pTarget = nullptr;

			if (!OU::MouseCell(cx, cy, &pTarget))
				return;

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;
			int sent = 0;

			if (pTarget)
			{
				for (int i = 0; i < nu; ++i)
				{
					const Mission mission = ActionToMission(units[i], pTarget);

					if (!Fire(houseIndex, units[i], mission, pTarget, cx, cy))
						break;

					++sent;
				}
			}
			else
			{
				static int destX[UNIT_CAP];
				static int destY[UNIT_CAP];
				AssignScatter(units, nu, cx, cy, destX, destY);

				for (int i = 0; i < nu; ++i)
				{
					if (!Fire(houseIndex, units[i], Mission::Move, nullptr, destX[i], destY[i]))
						break;

					++sent;
				}
			}

			if (sent)
				Log("smartorder", "issued=%u target=%d", sent, pTarget ? 1 : 0);
		}
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_SmartOrder, Key);
			Log("smartorder", "%s", Key.disabled ? "OFF" : "ON");
		}

		if (Key.disabled)
			return;

		const bool down = OU::HotkeyDown(Key) && !ModifiersBlock();

		if (!down && PrevDown)
			Press();

		PrevDown = down;
	}

	void Draw()
	{
		LineCount = 0;

		if (Key.disabled || !OU::HotkeyDown(Key) || ModifiersBlock())
			return;

		void* units[UNIT_CAP];
		int nu = 0;

		for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
		{
			void* o = OU::SelectionAt(i);

			if (o && IsFoot(o))
				units[nu++] = o;
		}

		if (nu <= 0)
			return;

		int cx = 0, cy = 0;
		ObjectClass* pTarget = nullptr;

		if (!OU::MouseCell(cx, cy, &pTarget))
			return;

		if (pTarget)
		{
			for (int i = 0; i < nu && LineCount < OU::PrevCap; ++i)
				OU::PrevPushObjs(Lines, LineCount, units[i], pTarget);
		}
		else
		{
			static int destX[UNIT_CAP];
			static int destY[UNIT_CAP];
			AssignScatter(units, nu, cx, cy, destX, destY);

			for (int i = 0; i < nu && LineCount < OU::PrevCap; ++i)
				OU::PrevPushCell(Lines, LineCount, units[i], 0, destX[i], destY[i]);
		}

		if (LineCount > 0)
			OU::PrevDraw(Lines, LineCount, RGB(80, 255, 80));
	}
}
