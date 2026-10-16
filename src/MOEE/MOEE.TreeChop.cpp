#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>
#include <AbstractClass.h>
#include <FootClass.h>
#include <TechnoClass.h>
#include <TacticalClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>
#include <cstring>

// Port of MO_ExperienceEnhancements/src/TreeChop.cpp.
//
// Ctrl+Z+Middle: while held, a Range x Range box is drawn around the cursor;
// on release every tree inside is force-attacked and distributed round-robin
// across the selected ground units.
//
// Upstream located trees by scanning the Display cell chain and reverse
// engineering the type-ID offset. Here we simply iterate TerrainClass::Array and
// match the type ID prefix, which is the same criterion with far less guesswork.
// Preview lines use Phobos' own surface path instead of MO_EE's locked buffer.
namespace MOEE::TreeChop
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int TREE_CAP = 256;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		bool IsTree(const char* id)
		{
			return id && (std::strncmp(id, "TREE", 4) == 0 || std::strncmp(id, "TIBTRE", 6) == 0);
		}

		bool IsChopper(void* p)
		{
			const int what = OU::WhatAmI(p);
			return what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Infantry);
		}

		int Range()
		{
			return Phobos::Config::MOEE_TreeChop_Range;
		}

		// Force-attack a tree (MegaMission Attack, queued as a planning event).
		bool FireAttack(int houseIndex, void* unit, void* tree)
		{
			EventClass event(houseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				Mission::Attack,
				TargetClass(reinterpret_cast<AbstractClass*>(tree)),
				TargetClass(),
				TargetClass());
			event.MegaMission.IsPlanningEvent = true;

			return EventClass::OutList.Add(event);
		}

		void Press()
		{
			if (OU::SelectionCount() <= 0)
				return;

			int cx = 0, cy = 0;

			if (!OU::MouseCell(cx, cy))
				return;

			// Selected ground units.
			void* units[TREE_CAP];
			int nu = 0;

			for (int i = 0; i < OU::SelectionCount() && nu < TREE_CAP; ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && IsChopper(o))
					units[nu++] = o;
			}

			if (nu <= 0)
				return;

			// Trees inside the box.
			const int lo = -(Range() / 2);
			const int hi = lo + Range() - 1;

			void* trees[TREE_CAP];
			int nt = 0;

			auto& array = TerrainClass::Array;

			for (int i = 0; i < array.Count && nt < TREE_CAP; ++i)
			{
				TerrainClass* pTree = array.GetItem(i);

				if (!pTree || !MOEE::IsPlausiblePtr(pTree))
					continue;

				TerrainTypeClass* pType = pTree->Type;

				if (!pType || !IsTree(pType->ID))
					continue;

				CoordStruct coords;

				if (!OU::GetCoords(pTree, coords))
					continue;

				const int tx = coords.X / LEP_PER_CELL;
				const int ty = coords.Y / LEP_PER_CELL;

				if (tx < cx + lo || tx > cx + hi || ty < cy + lo || ty > cy + hi)
					continue;

				trees[nt++] = pTree;
			}

			if (nt <= 0)
				return;

			void* cur = *reinterpret_cast<void**>(0xA83D4Cu); // HouseClass::CurrentPlayer

			if (!cur || !MOEE::IsPlausiblePtr(cur))
				return;

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;
			int sent = 0;

			for (int i = 0; i < nt; ++i)
			{
				void* unit = units[i % nu];

				if (!FireAttack(houseIndex, unit, trees[i]))
					break;

				++sent;
			}

			if (sent)
				Log("treechop", "chopping trees=%d units=%d", sent, nu);
		}
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_TreeChop, Key);
			Log("treechop", "%s range=%d", Key.disabled ? "OFF" : "ON", Range());
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
		if (Key.disabled || !OU::HotkeyDown(Key))
			return;

		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		int cx = 0, cy = 0;

		if (!OU::MouseCell(cx, cy))
			return;

		const int lo = -(Range() / 2);
		const int hi = lo + Range();
		const int bx0 = cx + lo;
		const int by0 = cy + lo;
		const int bx1 = cx + hi;
		const int by1 = cy + hi;

		auto corner = [](int gx, int gy, Point2D& out) -> bool
		{
			CoordStruct c = { gx * LEP_PER_CELL, gy * LEP_PER_CELL, 0 };
			c.Z = MapClass::Instance.GetCellFloorHeight(c);
			auto [point, visible] = TacticalClass::Instance->CoordsToClient(c);
			out = point;
			return visible;
		};

		Point2D p0, p1, p2, p3;

		if (!corner(bx0, by0, p0) || !corner(bx1, by0, p1)
			|| !corner(bx1, by1, p2) || !corner(bx0, by1, p3))
			return;

		const COLORREF color = RGB(255, 255, 255);
		pSurface->DrawLine(&p0, &p1, color);
		pSurface->DrawLine(&p1, &p2, color);
		pSurface->DrawLine(&p2, &p3, color);
		pSurface->DrawLine(&p3, &p0, color);
	}
}
