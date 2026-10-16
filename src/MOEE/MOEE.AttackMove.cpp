#include "MOEE.h"

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
		struct Hotkey
		{
			unsigned int vk = VK_XBUTTON2; // X2
			unsigned int mods = 0;
			bool disabled = false;
		};

		Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;

		bool ModsDown(unsigned int mods)
		{
			if ((mods & 1u) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
			if ((mods & 2u) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
			if ((mods & 4u) && !(GetAsyncKeyState(VK_MENU) & 0x8000)) return false;
			return true;
		}

		bool KeyDown(unsigned int vk)
		{
			return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
		}

		bool ParseKey(const char* token, unsigned int& vkOut)
		{
			if (!token || !*token)
				return false;

			if (_stricmp(token, "X1") == 0) { vkOut = VK_XBUTTON1; return true; }
			if (_stricmp(token, "X2") == 0) { vkOut = VK_XBUTTON2; return true; }
			if (_stricmp(token, "Middle") == 0 || _stricmp(token, "MButton") == 0 || _stricmp(token, "MMB") == 0)
			{
				vkOut = VK_MBUTTON;
				return true;
			}
			if (_stricmp(token, "LMB") == 0) { vkOut = VK_LBUTTON; return true; }
			if (_stricmp(token, "RMB") == 0) { vkOut = VK_RBUTTON; return true; }

			if (strlen(token) == 1)
			{
				const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(token[0])));

				if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
				{
					vkOut = static_cast<unsigned int>(c);
					return true;
				}
			}

			if ((token[0] == 'F' || token[0] == 'f') && strlen(token) <= 3)
			{
				const int n = atoi(token + 1);

				if (n >= 1 && n <= 24)
				{
					vkOut = VK_F1 + n - 1;
					return true;
				}
			}

			return false;
		}

		// "Ctrl+Z+Middle" / "X2" / single letter / "0" (disabled) / "None".
		void Parse(const char* spec)
		{
			Key = Hotkey();

			if (!spec || !*spec)
				return;

			if (_stricmp(spec, "0") == 0 || _stricmp(spec, "None") == 0
				|| _stricmp(spec, "Off") == 0 || _stricmp(spec, "Disabled") == 0)
			{
				Key.disabled = true;
				return;
			}

			char buffer[128];
			strncpy_s(buffer, spec, _TRUNCATE);

			char* context = nullptr;
			char* token = strtok_s(buffer, "+ ", &context);
			unsigned int lastVk = 0;
			bool haveKey = false;

			while (token)
			{
				if (_stricmp(token, "Ctrl") == 0 || _stricmp(token, "Control") == 0)
					Key.mods |= 1u;
				else if (_stricmp(token, "Shift") == 0)
					Key.mods |= 2u;
				else if (_stricmp(token, "Alt") == 0)
					Key.mods |= 4u;
				else if (_stricmp(token, "Win") == 0)
				{ /* not supported, ignored */ }
				else
				{
					unsigned int vk = 0;

					if (ParseKey(token, vk))
					{
						lastVk = vk;
						haveKey = true;
					}
				}

				token = strtok_s(nullptr, "+ ", &context);
			}

			if (!haveKey)
				Key.disabled = true;
			else
				Key.vk = lastVk;
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

		void EnsureInit()
		{
			if (Initialized)
				return;

			Initialized = true;
			Parse(Phobos::Config::MOEE_AttackMove);
			Log("attackmove", "%s (spec=\"%s\")", Key.disabled ? "OFF" : "ON", Phobos::Config::MOEE_AttackMove);
		}
	}

	void Poll()
	{
		EnsureInit();

		if (Key.disabled)
			return;

		const bool down = KeyDown(Key.vk) && ModsDown(Key.mods);

		if (down && !PrevDown)
			Press();

		PrevDown = down;
	}
}
