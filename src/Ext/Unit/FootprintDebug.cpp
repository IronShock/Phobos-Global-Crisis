#include "FootprintDebug.h"

#include <CellClass.h>
#include <CCINIClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <UnitClass.h>
#include <Unsorted.h>

#include <Ext/Techno/Body.h>
#include <Ext/Unit/Footprint.h>

#include <Utilities/Debug.h>

#include <Helpers/Macro.h>

namespace
{
	constexpr int kLeptonsPerCell = 256;
	constexpr int kMapSide = 512;

	struct Burst
	{
		bool Enabled = true;
		bool Initialized = false;
	};

	Burst s_config;

	bool IsEnabled()
	{
		if (!s_config.Initialized)
		{
			s_config.Initialized = true;
			s_config.Enabled = CCINIClass::INI_RA2MD.ReadBool(
				"Phobos", "Debug.FootprintOverlay", true);

			Debug::Log("[Footprint] debug overlay %s\n", s_config.Enabled ? "ON" : "OFF");
		}

		return s_config.Enabled;
	}

	bool ProjectCorner(int gx, int gy, int z, Point2D& out)
	{
		CoordStruct world
		{
			gx * kLeptonsPerCell,
			gy * kLeptonsPerCell,
			z
		};

		auto [point, visible] = TacticalClass::Instance->CoordsToClient(world);
		out = point;
		return visible;
	}

	COLORREF GetColor(UnitClass* pUnit, const FootprintState* pState, bool own)
	{
		if (!own)
			return RGB(255, 80, 80);

		if (pState->Alt)
			return RGB(0, 255, 255);

		return RGB(0, 255, 0);
	}

	void DrawThickLine(Surface* pSurface, const Point2D& a, const Point2D& b, COLORREF color)
	{
		Point2D start = a;
		Point2D end = b;

		pSurface->DrawLine(&start, &end, color);

		// A second line one pixel down makes the outline readable at 800x600 and
		// at the larger zoom levels without needing a wide-line primitive.
		++start.Y;
		++end.Y;
		pSurface->DrawLine(&start, &end, color);
	}

	void DrawCell(Surface* pSurface, const CellClass* pCell, COLORREF color, bool thick)
	{
		const int x = pCell->MapCoords.X;
		const int y = pCell->MapCoords.Y;

		if (x < 0 || y < 0 || x >= kMapSide || y >= kMapSide)
			return;

		const int z = pCell->GetCoordsWithBridge().Z;

		Point2D c00, c10, c11, c01;

		if (!ProjectCorner(x, y, z, c00)
			|| !ProjectCorner(x + 1, y, z, c10)
			|| !ProjectCorner(x + 1, y + 1, z, c11)
			|| !ProjectCorner(x, y + 1, z, c01))
		{
			return;
		}

		if (thick)
		{
			DrawThickLine(pSurface, c00, c10, color);
			DrawThickLine(pSurface, c10, c11, color);
			DrawThickLine(pSurface, c11, c01, color);
			DrawThickLine(pSurface, c01, c00, color);
		}
		else
		{
			pSurface->DrawLine(&c00, &c10, color);
			pSurface->DrawLine(&c10, &c11, color);
			pSurface->DrawLine(&c11, &c01, color);
			pSurface->DrawLine(&c01, &c00, color);
		}
	}

	void DrawAnchor(Surface* pSurface, const FootprintState* pState)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(pState->Anchor);

		if (!pCell)
			return;

		const auto world = pCell->GetCoordsWithBridge();
		auto [center, visible] = TacticalClass::Instance->CoordsToClient(world);

		if (!visible)
			return;

		RectangleStruct rect
		{
			center.X - 2,
			center.Y - 2,
			5,
			5
		};

		pSurface->FillRect(&rect, RGB(255, 255, 255));
	}
}

void FootprintDebug::Draw()
{
	if (!Game::IsActive || !IsEnabled())
		return;

	auto const pSurface = DSurface::Temp;

	if (!pSurface || !TacticalClass::Instance)
		return;

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
			continue;

		if (!Footprint::IsMultiCell(pUnit))
			continue;

		const auto pState = Footprint::GetState(pUnit);

		if (!pState || !pState->Initialized || pState->Span.empty())
			continue;

		const bool own = !pUnit->Owner || pUnit->Owner == HouseClass::CurrentPlayer;
		const COLORREF color = GetColor(pUnit, pState, own);

		for (const auto& cell : pState->Span)
		{
			if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
				DrawCell(pSurface, pCell, color, true);
		}

		for (const auto& cell : pState->BarrierSpan)
		{
			if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
				DrawCell(pSurface, pCell, RGB(255, 140, 0), false);
		}

		for (const auto& cell : pState->ReservedSpan)
		{
			if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
				DrawCell(pSurface, pCell, RGB(255, 255, 0), false);
		}

		const int visualBucket = Footprint::BucketFromFacing(pUnit->PrimaryFacing.Current());

		if (visualBucket != pState->Bucket)
		{
			const auto visualSpan = Footprint::GetSpan(pUnit, pState->Anchor, visualBucket);

			for (const auto& cell : visualSpan)
			{
				if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
					DrawCell(pSurface, pCell, RGB(255, 0, 255), false);
			}
		}

		DrawAnchor(pSurface, pState);
	}
}
