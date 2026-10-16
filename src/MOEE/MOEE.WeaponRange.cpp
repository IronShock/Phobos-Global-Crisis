#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <ObjectClass.h>
#include <AbstractClass.h>
#include <WeaponTypeClass.h>
#include <TacticalClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <GeneralDefinitions.h>
#include <GeneralStructures.h>
#include <Utilities/Macro.h>

#include <Windows.h>
#include <cmath>

// Port of MO_ExperienceEnhancements/src/WeaponRange.cpp (core).
//
// Ctrl + a single selected unit: draw its current weapon's range as an ellipse
// on the tactical map (blue = maximum, red = minimum when non-zero).
//
// Upstream additionally drew AA / deployed-weapon / cloak-detection / spy
// identification ranges; those are not ported yet. Drawing goes through Phobos'
// surface path instead of MO_EE's locked buffer.
namespace MOEE::WeaponRange
{
	namespace
	{
		constexpr int SEGMENTS = 48;

		void DrawRing(const CoordStruct& centre, int radius)
		{
			if (radius <= 0)
				return;

			auto pSurface = DSurface::Temp;

			if (!pSurface)
				return;

			Point2D points[SEGMENTS];
			bool visible[SEGMENTS];

			for (int i = 0; i < SEGMENTS; ++i)
			{
				const double angle = (2.0 * 3.14159265358979 * i) / SEGMENTS;
				CoordStruct world = {
					centre.X + static_cast<int>(std::cos(angle) * radius),
					centre.Y + static_cast<int>(std::sin(angle) * radius),
					centre.Z
				};

				auto [point, vis] = TacticalClass::Instance->CoordsToClient(world);
				points[i] = point;
				visible[i] = vis;
			}

			const COLORREF color = RGB(90, 160, 255);

			for (int i = 0; i < SEGMENTS; ++i)
			{
				const int j = (i + 1) % SEGMENTS;

				if (visible[i] || visible[j])
					pSurface->DrawLine(&points[i], &points[j], color);
			}
		}
	}

	void Draw()
	{
		if (!Phobos::Config::MOEE_WeaponRange)
			return;

		if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000))
			return;

		if (OU::SelectionCount() != 1)
			return;

		void* unit = OU::SelectionAt(0);

		if (!unit)
			return;

		const int what = OU::WhatAmI(unit);

		if (what != static_cast<int>(AbstractType::Unit)
			&& what != static_cast<int>(AbstractType::Infantry)
			&& what != static_cast<int>(AbstractType::Aircraft))
			return;

		auto pTechno = static_cast<TechnoClass*>(reinterpret_cast<AbstractClass*>(unit));
		auto pWeapon = pTechno->GetWeapon(pTechno->CurrentWeaponNumber);

		if (!pWeapon || !pWeapon->WeaponType)
			return;

		CoordStruct centre;

		if (!OU::GetCoords(unit, centre))
			return;

		const int maxRange = pWeapon->WeaponType->Range;
		const int minRange = pWeapon->WeaponType->MinimumRange;

		DrawRing(centre, maxRange);
		DrawRing(centre, minRange);
	}
}
