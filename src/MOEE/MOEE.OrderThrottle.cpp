#include "MOEE.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <Unsorted.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/OrderThrottle.cpp (single-player part).
//
// Vanilla EventClass::OutList holds only 128 events per frame; when one order
// per unit is issued (right-click move/attack, S stop, E enter, Z waypoints),
// everything past the 128th is silently dropped. In single player this module
// hooks the three "OutList full" branches and writes the event straight into
// DoList instead (capacity 16384), so nothing is dropped.
//
// Multiplayer is completely untouched: the same hooks reproduce the vanilla
// branch decision (OutList full -> fail, otherwise append), so behaviour is
// bit-identical to vanilla.
//
// Unlike upstream this needs no naked asm and no trampolines:
//   - the three sites are exact 10-byte `cmp [OutList.Count],0x80` instructions
//     with no relative jumps, so plain DEFINE_HOOKs can replace them,
//   - the direct write reuses YRpp's typed EventClass::DoList queue, whose Add()
//     stores the same timings as the engine (Imports::TimeGetTime == 0x7E1530).
namespace MOEE::OrderThrottle
{
	namespace
	{
		constexpr unsigned int ADDR_CURPLAYER = 0xA83D4Cu;
		constexpr unsigned int OFF_HOUSE_INDEX = 0x30u;

		// OutList / DoList branch targets.
		constexpr unsigned int MEGA_NORMAL = 0x646F01u;
		constexpr unsigned int MEGA_SUCC = 0x646F4Eu;
		constexpr unsigned int MEGA_FAIL = 0x646F58u;

		constexpr unsigned int STOP_NORMAL = 0x6FFE61u;
		constexpr unsigned int STOP_TAIL = 0x6FFEADu;

		constexpr unsigned int ENQ_NORMAL = 0x6521D5u;
		constexpr unsigned int ENQ_SUCC = 0x652225u;
		constexpr unsigned int ENQ_FAIL = 0x65222Au;
	}

	// Write an event straight into DoList (single player only), reproducing the
	// engine's own append bookkeeping: clear IsExecuted, stamp the local house and
	// set the execution frame to CurrentFrame + 1.
	bool SpillToDoList(const void* ev)
	{
		if (!ev || MOEE::IsMultiplayer())
			return false;

		EventClass event(*reinterpret_cast<const EventClass*>(ev));
		event.IsExecuted = false;

		if (void* cur = *reinterpret_cast<void**>(ADDR_CURPLAYER))
			event.HouseIndex = static_cast<char>(
				*reinterpret_cast<int*>(static_cast<char*>(cur) + OFF_HOUSE_INDEX));

		event.Frame = static_cast<unsigned int>(Unsorted::CurrentFrame) + 1u;

		return EventClass::DoList.Add(event);
	}
}

// MegaMission inlined append full branch (0x646EF5, 10 bytes). eax = event.
namespace { inline bool MOEE_ThrottleEnabled() { return Phobos::Config::MOEE_OrderThrottle != 0; } }

DEFINE_HOOK(0x646EF5, MOEE_OrderThrottle_MegaSpill, 0xA)
{
	GET(void*, pEvent, EAX);

	if (!MOEE_ThrottleEnabled() || MOEE::IsMultiplayer())
		return (EventClass::OutList.Count < EventClass::MAX_EVENTS)
			? 0x646F01u : 0x646F58u;

	return MOEE::OrderThrottle::SpillToDoList(pEvent) ? 0x646F4Eu : 0x646F58u;
}

// Slot 0x374 (S stop / E enter) inlined append full branch (0x6FFE55, 10 bytes).
// The event is a stack temporary at entry esp + 0x10.
DEFINE_HOOK(0x6FFE55, MOEE_OrderThrottle_StopSpill, 0xA)
{
	if (!MOEE_ThrottleEnabled() || MOEE::IsMultiplayer())
		return (EventClass::OutList.Count < EventClass::MAX_EVENTS)
			? 0x6FFE61u : 0x6FFEADu;

	const auto pEvent = reinterpret_cast<const void*>(
		static_cast<uintptr_t>(R->ESP()) + 0x10u);

	MOEE::OrderThrottle::SpillToDoList(pEvent);
	return 0x6FFEADu; // success and failure share this tail
}

// Canonical enqueue full branch (0x6521C9, 10 bytes). The event is a stack
// temporary at entry esp + 0x4 (Z waypoints and four other callers go here).
DEFINE_HOOK(0x6521C9, MOEE_OrderThrottle_EnqSpill, 0xA)
{
	if (!MOEE_ThrottleEnabled() || MOEE::IsMultiplayer())
		return (EventClass::OutList.Count < EventClass::MAX_EVENTS)
			? 0x6521D5u : 0x65222Au;

	const auto pEvent = reinterpret_cast<const void*>(
		static_cast<uintptr_t>(R->ESP()) + 0x4u);

	return MOEE::OrderThrottle::SpillToDoList(pEvent) ? 0x652225u : 0x65222Au;
}
