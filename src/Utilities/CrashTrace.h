#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include <Fundamentals.h>
#include <Utilities/Debug.h>
#include <Utilities/PointerSanity.h>

// Small, allocation-free ring buffer that remembers the last few GetFireError
// decisions. A hard crash in this path (e.g. an indirect call into a corrupted
// locomotor vtable) leaves nothing useful in the normal log, so the vectored
// exception handler in MOEE.cpp dumps this buffer instead.
//
// The buffer is intentionally fixed-size and POD: it must stay safe to write
// while the heap may already be corrupt.
namespace CrashTrace
{
	enum Event : uint32_t
	{
		FireErrorEnter = 1,      // wrapper entered with a valid-looking context
		FireErrorBeforeEngine,   // about to call the class-specific GetFireError
		FireErrorAfterEngine,    // engine call returned, Phase = FireError value
		FireErrorRejected,       // guard refused to call the engine, Flags = reason
		FireErrorRejectedEngine, // engine call faulted but was caught by SEH
		LocomotorReplaced,       // ConvertToType replaced FootClass::Locomotor
		LocomotorSuspect,        // a locomotor failed the sanity check
		TypeConvertEnter,        // ConvertToType entered
		TypeConvertLeave,        // ConvertToType finished (Type/Phase = success)
	};

	constexpr uint32_t Capacity = 64;

	struct Entry
	{
		uint32_t Frame;
		uint32_t Event;
		uint32_t Phase;
		uint32_t This;
		uint32_t Target;
		uint32_t Type;
		uint32_t Locomotor;
		uint32_t LocomotorVTable;
		int32_t WeaponIndex;
		uint32_t Flags;
		char TypeId[24];
		char TargetId[24];
	};

	inline Entry Buffer[Capacity] {};
	inline uint32_t Cursor = 0;
	inline volatile LONG Dumped = 0;
	inline bool Enabled = true;

	inline const void* GetVTable(const void* pObject) noexcept
	{
		if (!PointerSanity::IsAligned(pObject))
			return nullptr;

		return reinterpret_cast<const void*>(*reinterpret_cast<const uintptr_t*>(pObject));
	}

	inline void Push(uint32_t event, const void* pThis, const void* pTarget,
		const void* pType, const char* pTypeId, const char* pTargetId,
		const void* pLocomotor, const void* pLocomotorVTable,
		int weaponIndex, uint32_t phase = 0, uint32_t flags = 0) noexcept
	{
		if (!Enabled)
			return;

		Entry& entry = Buffer[Cursor % Capacity];
		++Cursor;

		entry.Frame = static_cast<uint32_t>(Unsorted::CurrentFrame);
		entry.Event = event;
		entry.Phase = phase;
		entry.This = reinterpret_cast<uint32_t>(pThis);
		entry.Target = reinterpret_cast<uint32_t>(pTarget);
		entry.Type = reinterpret_cast<uint32_t>(pType);
		entry.Locomotor = reinterpret_cast<uint32_t>(pLocomotor);
		entry.LocomotorVTable = reinterpret_cast<uint32_t>(pLocomotorVTable);
		entry.WeaponIndex = weaponIndex;
		entry.Flags = flags;

		if (pTypeId)
			strncpy_s(entry.TypeId, pTypeId, _TRUNCATE);
		else
			entry.TypeId[0] = '\0';

		if (pTargetId)
			strncpy_s(entry.TargetId, pTargetId, _TRUNCATE);
		else
			entry.TargetId[0] = '\0';
	}

	inline void Dump() noexcept
	{
		if (InterlockedExchange(&Dumped, 1) != 0)
			return;

		const uint32_t total = Cursor;
		const uint32_t count = total < Capacity ? total : Capacity;

		Debug::Log("[CrashTrace] last %u of %u event(s):\n", count, total);

		for (uint32_t i = 0; i < count; ++i)
		{
			Entry const& e = Buffer[(total - count + i) % Capacity];

			Debug::Log("  f=%u ev=%u ph=%u this=%08X(%s) target=%08X(%s) type=%08X loco=%08X vt=%08X weapon=%d flags=%X\n",
				e.Frame, e.Event, e.Phase, e.This, e.TypeId[0] ? e.TypeId : "?",
				e.Target, e.TargetId[0] ? e.TargetId : "?",
				e.Type, e.Locomotor, e.LocomotorVTable, e.WeaponIndex, e.Flags);
		}
	}
}
