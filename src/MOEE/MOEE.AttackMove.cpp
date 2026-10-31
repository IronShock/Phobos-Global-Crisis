#include "MOEE.h"
#include "MOEE.Commands.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <ObjectClass.h>
#include <DisplayClass.h>
#include <WWMouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <GeneralDefinitions.h>

#include <Windows.h>
#include <cctype>
#include <cstring>

// Port of MO_ExperienceEnhancements/src/AttackMove.cpp.
//
// Vanilla Ctrl+Shift+LMB issues an attack-move (Action AttackMoveNav/Tar) which
// drives Mission::AttackMove as a FootClass overlay. This module lets any hotkey
// issue the same order as a standard MegaMission event (Mission=29), so it goes
// through the normal, desync-safe OutList path.
//
// Upstream did raw OutList/DoList memory surgery and a custom frame throttle;
// here the order is a plain `EventClass::OutList.Add()` (like Phobos' own
// formation command). Holding Z queues a planning waypoint, matching vanilla.
namespace MOEE::AttackMove
{
	namespace
	{
		bool PrevDown = false;

		// Fixed mouse button: X2 (side button). The keyboard part is optional and
		// configurable in KeyboardMD.ini -> [Hotkey] (MOEE Attack Move).
		bool ComboDown()
		{
			const auto part = MOEE::GetKeyPart(CommandNames::AttackMove);
			return MOEE::KeyPartHeldExact(part) && OU::KeyDown(VK_XBUTTON2);
		}

		void Press()
		{
			auto& objects = ObjectClass::CurrentObjects;

			if (objects.Count <= 0 || !WWMouseClass::Instance)
				return;

			Point2D point;
			WWMouseClass::Instance->GetCoords(&point);

			CellStruct cell;
			CoordStruct coord;
			ObjectClass* pTarget = nullptr;
			BYTE a5 = 0, a6 = 0;

			// Reads the current hover cell/target.
			if (!DisplayClass::Instance.ProcessClickCoords(&point, &cell, &coord, &pTarget, &a5, &a6))
				return;

			if (!MapClass::Instance.GetCellAt(cell))
				return;

			if (pTarget)
			{
				const auto what = pTarget->WhatAmI();

				if (what != AbstractType::Unit && what != AbstractType::Infantry
					&& what != AbstractType::Aircraft && what != AbstractType::Building)
					pTarget = nullptr;
			}

			const int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;
			const bool planning = (GetAsyncKeyState('Z') & 0x8000) != 0;
			unsigned int sent = 0;

			for (int i = 0; i < objects.Count; ++i)
			{
				auto pObject = objects.GetItem(i);

				if (!pObject)
					continue;

				const auto what = pObject->WhatAmI();

				if (what != AbstractType::Unit && what != AbstractType::Infantry
					&& what != AbstractType::Aircraft)
					continue;

				TargetClass src(static_cast<AbstractClass*>(pObject));
				TargetClass target(pTarget ? static_cast<AbstractClass*>(pTarget) : nullptr);
				TargetClass dest(cell);

				EventClass event(houseIndex, src, Mission::AttackMove, target, dest, TargetClass());
				event.MegaMission.IsPlanningEvent = planning;

				if (!EventClass::OutList.Add(event))
					break; // OutList full: drop the rest, same as one vanilla batch

				++sent;
			}

			if (sent)
				Log("attackmove", "issued=%u target=%d plan=%d", sent, pTarget ? 1 : 0, planning ? 1 : 0);
		}

	}

	void Poll()
	{
		const bool down = ComboDown();

		if (down && !PrevDown)
			Press();

		PrevDown = down;
	}
}
