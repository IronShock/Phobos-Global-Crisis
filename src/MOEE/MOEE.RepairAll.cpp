#include "MOEE.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <AbstractClass.h>
#include <Unsorted.h>
#include <WWMouseClass.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>
#include <cstring>

// Port of MO_ExperienceEnhancements/src/RepairAll.cpp.
//
// Vanilla requires entering the repair cursor and clicking every damaged
// building. This module offers Ctrl+repair / double-click repair / fully
// automatic repair, using the existing wrench trigger points (no new keys).
//
// Behaviour matches upstream, but the implementation is simpler: the scan and
// the enqueue happen in the same frame, so upstream's pointer queue and
// Fetch_ID liveness checks are unnecessary - no pointer can be freed between
// scanning and enqueueing.
//
// Multiplayer-safe: the only simulation change is a standard EventType::Repair
// event on OutList, byte-identical to clicking the wrench by hand (the engine
// still charges money and applies repairs itself); everything else is
// read-only.
namespace MOEE::RepairAll
{
	namespace
	{
		// Read-only engine offsets (verified against this exe build).
		constexpr unsigned int ADDR_BLD_ARR = 0xA8EB40u; // BuildingClass::Array
		constexpr unsigned int ADDR_CURPLAYER = 0xA83D4Cu;
		constexpr unsigned int ARR_ITEMS = 4u;   // DynamicVectorClass items
		constexpr unsigned int ARR_COUNT = 16u;  // DynamicVectorClass count

		constexpr unsigned int OFF_HEALTH = 0x6Cu;
		constexpr unsigned int OFF_INLIMBO = 0x81u;
		constexpr unsigned int OFF_ISALIVE = 0x90u;
		constexpr unsigned int OFF_OWNER = 0x21Cu;
		constexpr unsigned int OFF_MISSION = 0xACu;
		constexpr unsigned int OFF_HOUSE_IDX = 0x30u;
		constexpr unsigned int OFF_BLD_TYPE = 0x520u;
		constexpr unsigned int OFF_BLD_REPAIRING = 0x6E8u;
		constexpr unsigned int OFF_TYPE_STRENGTH = 0xA0u;

		constexpr unsigned int VT_CANREPAIR = 0x94u; // wrench eligibility, result in AL only
		constexpr int MISSION_CONSTRUCTION = 0x12;
		constexpr int MISSION_SELLING = 0x13;

		constexpr unsigned int EXE_LO = 0x401000u;
		constexpr unsigned int EXE_HI = 0x8A0000u;
		constexpr int DBL_SLOP = 12;

		bool Pending = false;
		bool KeyFresh = false;
		int LastFrame = -1;
		int LastScan = -100000;

		int DblFrame = -1;
		int DblX = 0;
		int DblY = 0;

		int RepairVK = 0x4B; // default 'K'
		bool VKLoaded = false;

		bool InExe(const void* p)
		{
			const auto v = static_cast<unsigned int>(reinterpret_cast<size_t>(p));
			return v >= EXE_LO && v < EXE_HI;
		}

		bool IsObject(void* p)
		{
			if (!p || !IsPlausiblePtr(p))
				return false;

			return InExe(*reinterpret_cast<void**>(p));
		}

		void LoadVK()
		{
			if (VKLoaded)
				return;

			VKLoaded = true;
			// Relative path resolves against the game directory (the process CWD).
			const int vk = GetPrivateProfileIntA("Hotkey", "ToggleRepair", 0x4B, "KeyboardMD.ini");

			if (vk > 0 && vk < 256)
				RepairVK = vk;
		}

		// Ask the engine whether the wrench is usable (vtable +0x94). The return
		// value is only guaranteed in AL, hence the unsigned char signature.
		bool CanRepair(void* b)
		{
			if (!IsObject(b))
				return false;

			auto vt = *reinterpret_cast<void***>(b);
			auto fn = reinterpret_cast<unsigned char(__thiscall*)(void*)>(vt[VT_CANREPAIR / 4]);

			if (!InExe(reinterpret_cast<void*>(fn)))
				return false;

			return fn(b) != 0;
		}

		bool MissionBusy(void* b)
		{
			const int mission = *reinterpret_cast<int*>(static_cast<char*>(b) + OFF_MISSION);
			return mission == MISSION_CONSTRUCTION || mission == MISSION_SELLING;
		}

		bool CtrlDown()
		{
			return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		}

		bool RepairKeyDown()
		{
			return (GetAsyncKeyState(RepairVK) & 0x8000) != 0;
		}

		// Double-press detection. The icon path also compares the cursor
		// position; the hotkey path relies on the key having been released
		// between presses (excludes auto-repeat).
		bool DblHit(bool checkPos)
		{
			int mx = 0, my = 0;

			if (checkPos && WWMouseClass::Instance)
			{
				Point2D pt;
				WWMouseClass::Instance->GetCoords(&pt);
				mx = pt.X;
				my = pt.Y;
			}

			const int fr = Unsorted::CurrentFrame;

			if (DblFrame >= 0)
			{
				int dx = mx - DblX;
				int dy = my - DblY;
				const int d = fr - DblFrame;

				if (dx < 0) dx = -dx;
				if (dy < 0) dy = -dy;

				if (d >= 1 && d <= Phobos::Config::MOEE_RepairAll_DblFrames
					&& (!checkPos || (dx <= DBL_SLOP && dy <= DBL_SLOP)))
				{
					DblFrame = -1;
					return true;
				}
			}

			DblFrame = fr;
			DblX = mx;
			DblY = my;
			return false;
		}

		void Scan()
		{
			auto items = *reinterpret_cast<void***>(ADDR_BLD_ARR + ARR_ITEMS);
			const int count = *reinterpret_cast<int*>(ADDR_BLD_ARR + ARR_COUNT);

			if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096)
				return;

			void* cur = *reinterpret_cast<void**>(ADDR_CURPLAYER);

			if (!cur || !IsPlausiblePtr(cur))
				return;

			const int houseIndex = *reinterpret_cast<int*>(static_cast<char*>(cur) + OFF_HOUSE_IDX);
			const int batch = Phobos::Config::MOEE_RepairAll_Batch;
			int sent = 0;

			for (int i = 0; i < count && sent < batch; ++i)
			{
				void* b = items[i];

				if (!IsObject(b))
					continue;

				auto raw = static_cast<unsigned char*>(b);

				if (!raw[OFF_ISALIVE] || raw[OFF_INLIMBO] || raw[OFF_BLD_REPAIRING])
					continue;

				if (*reinterpret_cast<void**>(raw + OFF_OWNER) != cur)
					continue;

				const int hp = *reinterpret_cast<int*>(raw + OFF_HEALTH);

				if (hp <= 0)
					continue;

				if (reinterpret_cast<AbstractClass*>(b)->WhatAmI() != AbstractType::Building)
					continue;

				if (!CanRepair(b) || MissionBusy(b))
					continue;

				void* type = *reinterpret_cast<void**>(raw + OFF_BLD_TYPE);

				if (!IsObject(type))
					continue;

				if (*reinterpret_cast<int*>(static_cast<char*>(type) + OFF_TYPE_STRENGTH) <= hp)
					continue; // not damaged

				TargetClass whom(reinterpret_cast<AbstractClass*>(b));

				// The YRpp (int,int) / (int,const int&) Target constructors are
				// ambiguous for a plain int argument, so build the event through
				// the engine's own constructor (0x4C65E0), as upstream does.
				alignas(EventClass) unsigned char eventBuffer[sizeof(EventClass)];
				std::memset(eventBuffer, 0, sizeof(eventBuffer));
				using Fn_TargetEventCtor = void* (__thiscall*)(void*, int, int, int, unsigned int);
				reinterpret_cast<Fn_TargetEventCtor>(0x4C65E0u)(eventBuffer, houseIndex,
					static_cast<int>(EventType::Repair), whom.m_ID,
					static_cast<unsigned int>(whom.m_RTTI));

				if (!EventClass::OutList.Add(*reinterpret_cast<EventClass*>(eventBuffer)))
					break; // OutList full; the next trigger re-scans

				++sent;
			}

			if (sent)
				Log("repairall", "queued=%d", sent);
		}
	}

	// Called from the two trigger hooks.
	bool TriggerHook(bool fromButton)
	{
		if (Phobos::Config::MOEE_RepairAll == 0)
			return false;

		bool dbl = false;

		if (Phobos::Config::MOEE_RepairAll_DoubleClick)
		{
			if (fromButton)
				dbl = DblHit(true);
			else if (KeyFresh)
			{
				KeyFresh = false;
				dbl = DblHit(false);
			}
		}

		if (CtrlDown() || dbl)
		{
			Pending = true;
			return true;
		}

		return false;
	}

	void Poll()
	{
		if (Phobos::Config::MOEE_RepairAll == 0)
			return;

		LoadVK();

		const int fr = Unsorted::CurrentFrame;

		if (fr == LastFrame)
			return; // the host hook runs many times per frame

		LastFrame = fr;

		if (!RepairKeyDown())
			KeyFresh = true;

		if (Pending)
		{
			Pending = false;
			Scan();
			LastScan = fr;
		}
		else if (Phobos::Config::MOEE_RepairAll >= 2
			&& (LastScan < 0 || fr - LastScan >= Phobos::Config::MOEE_RepairAll_Interval))
		{
			LastScan = fr;
			Scan();
		}
	}
}

// ToggleRepairModeCommandClass::Execute (7 bytes: `push -1; mov ecx,0x87F7E8`).
DEFINE_HOOK(0x536FA0, MOEE_RepairAll_Hotkey, 0x7)
{
	if (MOEE::RepairAll::TriggerHook(false))
		return 0x536FACu; // ret 4

	return 0;
}

// Sidebar repair-button branch (6 bytes: `mov ecx,[0x8871E0]`).
DEFINE_HOOK(0x6A78D9, MOEE_RepairAll_Button, 0x6)
{
	if (MOEE::RepairAll::TriggerHook(true))
		return 0x6A78FFu;

	return 0;
}
