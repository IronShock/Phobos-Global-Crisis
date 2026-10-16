#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <CellClass.h>
#include <MapClass.h>
#include <ObjectClass.h>
#include <FootClass.h>
#include <TechnoClass.h>
#include <AbstractClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <GeneralDefinitions.h>
#include <GeneralStructures.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/GridView.cpp (simplified).
//
// Toggles a ground grid over the tactical map. Each cell draws its north and
// west edges (so shared edges appear once); cells carrying a building or terrain
// are drawn red. Toggled with a hotkey.
//
// Simplification vs upstream: the visible window is a bounded square around the
// camera (no per-frame offscreen cache and no cliff-face back-face culling).
namespace MOEE::GridView
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;
		bool On = false;

		int Radius()
		{
			return Phobos::Config::MOEE_Grid_Radius;
		}

		bool CellBlocked(CellClass* pCell)
		{
			for (int layer = 0; layer < 2; ++layer)
			{
				ObjectClass* o = (layer == 0) ? pCell->FirstObject : pCell->AltObject;

				for (int steps = 0; o && steps < 32; ++steps)
				{
					if (!MOEE::IsPlausiblePtr(o))
						break;

					const auto what = o->WhatAmI();

					if (what == AbstractType::Building || what == AbstractType::Terrain)
						return true;

					o = o->NextObject;
				}
			}

			return false;
		}

		bool ProjectCorner(int gx, int gy, int z, Point2D& out)
		{
			CoordStruct c = { gx * LEP_PER_CELL, gy * LEP_PER_CELL, z };
			auto [point, visible] = TacticalClass::Instance->CoordsToClient(c);
			out = point;
			return visible;
		}
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_Grid, Key);
			Log("gridview", "%s radius=%d", Key.disabled ? "OFF" : "ON", Radius());
		}

		if (Key.disabled)
			return;

		const bool down = OU::HotkeyDown(Key);

		if (down && !PrevDown)
		{
			On = !On;
			Log("gridview", "grid %s", On ? "ON" : "OFF");
		}

		PrevDown = down;
	}

	void Draw()
	{
		if (!On)
			return;

		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		// Camera centre cell: viewport centre -> world -> cell.
		const int vw = *reinterpret_cast<int*>(0xB0CE30u); // ViewBounds.Width
		const int vh = *reinterpret_cast<int*>(0xB0CE34u); // ViewBounds.Height

		if (vw <= 16 || vh <= 16)
			return;

		const Point2D centrePoint = { vw / 2, vh / 2 };
		const CoordStruct centreWorld = TacticalClass::Instance->ClientToCoords(centrePoint);
		const int ccx = centreWorld.X / LEP_PER_CELL;
		const int ccy = centreWorld.Y / LEP_PER_CELL;
		const int r = Radius();

		const COLORREF normal = RGB(220, 220, 220);
		const COLORREF blocked = RGB(255, 60, 60);

		for (int y = ccy - r; y <= ccy + r; ++y)
		{
			for (int x = ccx - r; x <= ccx + r; ++x)
			{
				if (x < 0 || y < 0 || x >= 511 || y >= 511)
					continue;

				CellStruct cell;
				cell.X = static_cast<short>(x);
				cell.Y = static_cast<short>(y);
				CellClass* pCell = MapClass::Instance.GetCellAt(cell);

				if (!pCell)
					continue;

				const int z = MapClass::Instance.GetCellFloorHeight(
					CoordStruct { x * LEP_PER_CELL + LEP_PER_CELL / 2,
						y * LEP_PER_CELL + LEP_PER_CELL / 2, 0 });

				// Corners (x,y) .. (x+1,y+1).
				Point2D c00, c10, c01;

				if (!ProjectCorner(x, y, z, c00)
					|| !ProjectCorner(x + 1, y, z, c10)
					|| !ProjectCorner(x, y + 1, z, c01))
					continue;

				const COLORREF color = CellBlocked(pCell) ? blocked : normal;
				pSurface->DrawLine(&c00, &c10, color); // north edge
				pSurface->DrawLine(&c00, &c01, color); // west edge
			}
		}
	}
}
