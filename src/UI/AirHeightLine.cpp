#include "AirHeightLine.h"

#include <Phobos.h>

#include <Drawing.h>
#include <FootClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>

#include <algorithm>
#include <cstdio>

namespace
{
	void DrawOne(TechnoClass* pThis)
	{
		auto const whatAmI = pThis->WhatAmI();

		if (whatAmI == AbstractType::Aircraft)
		{
			if (pThis->GetHeight() <= 0)
				return;
		}
		else if (whatAmI == AbstractType::Unit)
		{
			// The mod's helicopters are jumpjet vehicles, not AircraftClass.
			auto const pType = pThis->GetTechnoType();

			if (!pType || !pType->JumpJet || pThis->GetHeight() <= 0)
				return;
		}
		else
		{
			return;
		}

		auto const pTactical = TacticalClass::Instance;

		if (!pTactical)
			return;

		const Point2D start = pTactical->CoordsToClient(pThis->GetRenderCoords()).first;

		CoordStruct coords = pThis->GetCenterCoords();
		coords.Z = MapClass::Instance.GetCellFloorHeight(coords);

		auto const [end, visible] = pTactical->CoordsToClient(coords);

		if (!visible)
			return;

		auto const pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		const COLORREF color = Phobos::Config::AirHeightLine_Color;
		const int thickness = std::clamp(Phobos::Config::AirHeightLine_Thickness, 1, 4);

		for (int i = 0; i < thickness; ++i)
		{
			Point2D lineStart { start.X + i, start.Y };
			Point2D lineEnd { end.X + i, end.Y };

			if (Phobos::Config::AirHeightLine_Dashed)
			{
				pSurface->DrawDashed(&lineStart, &lineEnd, color, 0);
			}
			else if (Line_In_Bounds(&lineStart, &lineEnd, &DSurface::ViewBounds))
			{
				pSurface->DrawLine(&lineStart, &lineEnd, color);
			}
		}

		if (Phobos::Config::AirHeightLine_GroundMarker)
		{
			constexpr int size = 3;

			for (int i = 0; i < thickness; ++i)
			{
				Point2D left { end.X - size, end.Y + i };
				Point2D right { end.X + size, end.Y + i };
				Point2D top { end.X + i, end.Y - size };
				Point2D bottom { end.X + i, end.Y + size };

				pSurface->DrawLine(&left, &right, color);
				pSurface->DrawLine(&top, &bottom, color);
			}
		}

		if (Phobos::Config::AirHeightLine_ShowCoordinates)
		{
			CellStruct cell {};
			pTactical->CoordsToCell(&cell, &coords);

			wchar_t text[0x20] {};
			swprintf_s(text, L"%d,%d", cell.X, cell.Y);

			RectangleStruct bounds = pSurface->GetRect();
			Point2D textPos { end.X + 5, end.Y + 2 };

			pSurface->DrawText(text, &bounds, &textPos, color, 0,
				TextPrintType::Point6Grad | TextPrintType::NoShadow | TextPrintType::Background);
		}
	}
}

void AirHeightLine::DrawAll()
{
	if (!Phobos::Config::AirHeightLine_Enable)
		return;

	auto& array = TechnoClass::Array;

	for (int i = 0; i < array.Count; ++i)
	{
		auto const pThis = array.GetItem(i);

		if (pThis && pThis->IsSelected)
			DrawOne(pThis);
	}
}
