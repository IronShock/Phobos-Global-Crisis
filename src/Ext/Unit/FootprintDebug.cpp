#include "FootprintDebug.h"

#include <set>
#include <utility>

#include <CellClass.h>
#include <CCINIClass.h>
#include <Drawing.h>
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
				"Phobos", "Debug.FootprintOverlay", false);

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

	void DrawCellSetOutline(Surface* pSurface, const std::vector<CellStruct>& cells,
		COLORREF color, bool thick);

	void DrawStagedOutline(Surface* pSurface, const FootprintState* pState, COLORREF color)
	{
		if (!pSurface || !pState)
			return;

		std::vector<CellStruct> staged;
		staged.reserve(pState->Span.size()
			+ pState->BarrierSpan.size()
			+ pState->ReservedSpan.size());

		auto addCells = [&staged](const std::vector<CellStruct>& cells)
		{
			staged.insert(staged.end(), cells.begin(), cells.end());
		};

		addCells(pState->Span);
		addCells(pState->BarrierSpan);
		addCells(pState->ReservedSpan);

		DrawCellSetOutline(pSurface, staged, color, true);
	}

	void DrawCellSetOutline(Surface* pSurface, const std::vector<CellStruct>& cells,
		COLORREF color, bool thick)
	{
		if (!pSurface || cells.empty())
			return;

		std::set<std::pair<int, int>> set;

		for (const auto& cell : cells)
			set.emplace(cell.X, cell.Y);

		auto hasCell = [&set](int x, int y)
		{
			return set.find({ x, y }) != set.end();
		};

		auto drawLine = [&](const Point2D& a, const Point2D& b)
		{
			if (thick)
			{
				DrawThickLine(pSurface, a, b, color);
			}
			else
			{
				Point2D start = a;
				Point2D end = b;
				pSurface->DrawLine(&start, &end, color);
			}
		};

		for (const auto& [x, y] : set)
		{
			if (x < 0 || y < 0 || x >= kMapSide || y >= kMapSide)
				continue;

			const auto pCell = MapClass::Instance.TryGetCellAt(
				CellStruct { static_cast<short>(x), static_cast<short>(y) });

			if (!pCell)
				continue;

			const int z = pCell->GetCoordsWithBridge().Z;
			Point2D c00, c10, c11, c01;

			if (!ProjectCorner(x, y, z, c00)
				|| !ProjectCorner(x + 1, y, z, c10)
				|| !ProjectCorner(x + 1, y + 1, z, c11)
				|| !ProjectCorner(x, y + 1, z, c01))
			{
				continue;
			}

			if (!hasCell(x, y - 1))
				drawLine(c00, c10);

			if (!hasCell(x + 1, y))
				drawLine(c10, c11);

			if (!hasCell(x, y + 1))
				drawLine(c11, c01);

			if (!hasCell(x - 1, y))
				drawLine(c01, c00);
		}
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
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->IsSelected)
			continue;

		if (!Footprint::IsMultiCell(pUnit))
			continue;

		const auto pState = Footprint::GetState(pUnit);

		if (!pState || !pState->Initialized)
			continue;

		// Surface::DrawLine takes a game RGB565 colour, not a Windows RGB
		// macro. Convert explicitly so yellow stays yellow and barrier blue
		// stays blue instead of both rendering as white.
		const COLORREF hullColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 0));
		const COLORREF barrierColor = Drawing::RGB_To_Int(ColorStruct(0, 174, 239));
		const COLORREF reserveColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 255));

		// Hull and barrier are solid occupied sets; only their outer boundary
		// should be visible. Drawing every cell produces a dense interior grid.
		DrawCellSetOutline(pSurface, pState->BarrierSpan, barrierColor, false);
		DrawCellSetOutline(pSurface, pState->Span, hullColor, true);

		// White outlines the whole occupied stage as one shape. Outlining only
		// ReservedSpan produced separate hollow boxes around individual cells.
		// Hull, barriers and reserved cells are all actually occupied, so the
		// union has no empty interior to outline.
		if (!pState->ReservedSpan.empty())
			DrawStagedOutline(pSurface, pState, reserveColor);
	}
}
