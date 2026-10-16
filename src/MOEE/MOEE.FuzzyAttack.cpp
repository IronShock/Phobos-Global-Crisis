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

// Port of MO_ExperienceEnhancements/src/FuzzyAttack.cpp.
//
// Ctrl+Middle: fuzzy force-attack for hard-to-click targets. While held, a
// range box and red target lines are drawn; on release the units inside the
// selection attack the enemies found in the box (nearest ring first), grouped
// "SquadSize to one".
//
// Upstream walked raw cell chains and self-calibrated the Owner offset; here we
// use YRpp (CellClass::FirstObject/AltObject + ObjectClass::NextObject +
// TechnoClass::Owner) and skip allies. Preview lines use Phobos' surface path.
namespace MOEE::FuzzyAttack
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int ENEMY_CAP = 256;
		constexpr int UNIT_CAP = 512;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		// Preview state (filled while the key is held).
		OU::PrevLine Lines[OU::PrevCap];
		int LineCount = 0;

		int Range()
		{
			return Phobos::Config::MOEE_FuzzyAttack_Range;
		}

		int Squad()
		{
			return Phobos::Config::MOEE_FuzzyAttack_SquadSize;
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

			if (what != static_cast<int>(AbstractType::Unit)
				&& what != static_cast<int>(AbstractType::Infantry)
				&& what != static_cast<int>(AbstractType::Aircraft)
				&& what != static_cast<int>(AbstractType::Building))
				return false;

			auto pTechno = static_cast<TechnoClass*>(reinterpret_cast<AbstractClass*>(p));
			HouseClass* pOwner = pTechno->Owner;

			// Enemies only (allies/own are never targeted).
			return pOwner && !pOwner->IsAlliedWith(HouseClass::CurrentPlayer);
		}

		// Collect enemies inside the Range box, nearest ring first.
		int FindEnemies(int cx, int cy, void** enemies, int* cellsOut)
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

								if (IsTargetType(o) && n < ENEMY_CAP)
								{
									bool dup = false;

									for (int k = 0; k < n; ++k)
									{
										if (enemies[k] == o) { dup = true; break; }
									}

									if (!dup)
									{
										enemies[n] = o;
										cellsOut[n] = (ty << 9) + tx;
										++n;
									}
								}

								o = o->NextObject;
							}
						}
					}
				}
			}

			return n;
		}

		// Split the units over the first nUsed enemies (balanced, difference <= 1).
		void AssignGroups(int nu, int ne, int* countPerEnemy, int& nUsed)
		{
			if (nu <= Squad())
			{
				nUsed = 1;
				countPerEnemy[0] = nu;
				return;
			}

			nUsed = nu / Squad();

			if (nUsed < 1) nUsed = 1;
			if (nUsed > ne) nUsed = ne;

			const int base = nu / nUsed;
			int extra = nu % nUsed;

			for (int i = 0; i < nUsed; ++i)
			{
				countPerEnemy[i] = base + (extra > 0 ? 1 : 0);

				if (extra > 0)
					--extra;
			}
		}

		bool FireAttack(int houseIndex, void* unit, void* target)
		{
			EventClass event(houseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				Mission::Attack,
				TargetClass(reinterpret_cast<AbstractClass*>(target)),
				TargetClass(),
				TargetClass());
			event.MegaMission.IsPlanningEvent = false;

			return EventClass::OutList.Add(event);
		}

		// Shared by Press() and Draw(): resolve the selection and the enemies.
		bool Resolve(void* units[UNIT_CAP], int& nu, void* enemies[ENEMY_CAP],
			int enemiesCell[ENEMY_CAP], int& ne, int& cx, int& cy)
		{
			nu = 0;

			for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && IsAttacker(o))
					units[nu++] = o;
			}

			if (nu <= 0)
				return false;

			if (!OU::MouseCell(cx, cy))
				return false;

			ne = FindEnemies(cx, cy, enemies, enemiesCell);
			return ne > 0;
		}

		void Press()
		{
			void* units[UNIT_CAP];
			void* enemies[ENEMY_CAP];
			int enemyCell[ENEMY_CAP];
			int nu = 0, ne = 0, cx = 0, cy = 0;

			if (!Resolve(units, nu, enemies, enemyCell, ne, cx, cy))
				return;

			int countPerEnemy[ENEMY_CAP];
			int nUsed = 1;
			AssignGroups(nu, ne, countPerEnemy, nUsed);

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;
			int unitIndex = 0;

			for (int e = 0; e < nUsed; ++e)
			{
				for (int c = 0; c < countPerEnemy[e] && unitIndex < nu; ++c, ++unitIndex)
				{
					if (!FireAttack(houseIndex, units[unitIndex], enemies[e]))
					{
						Log("fuzzyattack", "units=%d enemies=%d groups=%d", nu, ne, nUsed);
						return;
					}
				}
			}

			Log("fuzzyattack", "units=%d enemies=%d groups=%d", nu, ne, nUsed);
		}
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_FuzzyAttack, Key);
			Log("fuzzyattack", "%s range=%d squad=%d",
				Key.disabled ? "OFF" : "ON", Range(), Squad());
		}

		if (Key.disabled)
			return;

		const bool down = OU::HotkeyDown(Key);

		if (!down && PrevDown)
			Press();

		PrevDown = down;
	}

	void Draw()
	{
		LineCount = 0;

		if (Key.disabled || !OU::HotkeyDown(Key))
			return;

		void* units[UNIT_CAP];
		void* enemies[ENEMY_CAP];
		int enemyCell[ENEMY_CAP];
		int nu = 0, ne = 0, cx = 0, cy = 0;

		if (!Resolve(units, nu, enemies, enemyCell, ne, cx, cy))
			return;

		int countPerEnemy[ENEMY_CAP];
		int nUsed = 1;
		AssignGroups(nu, ne, countPerEnemy, nUsed);

		int unitIndex = 0;

		for (int e = 0; e < nUsed; ++e)
		{
			for (int c = 0; c < countPerEnemy[e] && unitIndex < nu; ++c, ++unitIndex)
				OU::PrevPushObjs(Lines, LineCount, units[unitIndex], enemies[e]);
		}

		// Range box (same cells the scan uses).
		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

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

		if (LineCount > 0)
			OU::PrevDraw(Lines, LineCount, RGB(255, 60, 60));
	}
}
