#pragma once

#include <Windows.h>
#include <cstdint>

// Cheap structural validation for engine objects before we hand them back to
// the game's virtual functions.
//
// Rationale: the engine keeps raw AbstractClass/TechnoClass pointers in a lot
// of places (target scans, team scripts, locomotor COM interfaces). If one of
// those is stale or its memory got reused, the next virtual call reads a bogus
// vtable slot and jumps to whatever value is there (e.g. EIP=0x4E0). These
// checks verify that a pointer still looks like a live game object (aligned,
// with a vtable whose methods live in a loaded image) without touching the
// object's own memory layout, so they are safe to run on a suspect pointer.
namespace PointerSanity
{
	struct ModuleRange
	{
		uintptr_t Begin;
		uintptr_t End;
	};

	inline uintptr_t ModuleSize(HMODULE hModule) noexcept
	{
		if (!hModule)
			return 0;

		auto const pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);

		if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;

		auto const pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const BYTE*>(hModule) + pDos->e_lfanew);

		if (pNt->Signature != IMAGE_NT_SIGNATURE)
			return 0;

		return pNt->OptionalHeader.SizeOfImage;
	}

	inline ModuleRange MakeModuleRange(HMODULE hModule) noexcept
	{
		auto const base = reinterpret_cast<uintptr_t>(hModule);
		return ModuleRange { base, base + ModuleSize(hModule) };
	}

	// The modules that host the vtables of engine objects in this build. Any
	// other image falls back to the (slower) VirtualQuery path below.
	inline const ModuleRange* KnownRanges(size_t& count) noexcept
	{
		static ModuleRange ranges[] =
		{
			MakeModuleRange(GetModuleHandleW(nullptr)),
			MakeModuleRange(GetModuleHandleW(L"Phobos.dll")),
			MakeModuleRange(GetModuleHandleW(L"Ares.dll")),
		};

		count = sizeof(ranges) / sizeof(ranges[0]);
		return ranges;
	}

	inline bool IsKnownModuleRange(const void* p, size_t size) noexcept
	{
		if (!p)
			return false;

		auto const value = reinterpret_cast<uintptr_t>(p);

		if (value < 0x10000u)
			return false;

		size_t count = 0;
		auto const ranges = KnownRanges(count);

		for (size_t i = 0; i < count; ++i)
		{
			if (ranges[i].Begin && value >= ranges[i].Begin && value + size <= ranges[i].End)
				return true;
		}

		return false;
	}

	// True when p is inside a loaded PE image. This accepts custom classes
	// contributed by other injected DLLs (their vtables are image data), while
	// rejecting heap pointers - which is exactly the corruption we want to catch.
	inline bool IsImageRange(const void* p, size_t size) noexcept
	{
		if (IsKnownModuleRange(p, size))
			return true;

		MEMORY_BASIC_INFORMATION mbi {};

		if (!VirtualQuery(p, &mbi, sizeof(mbi)))
			return false;

		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE)
			return false;

		auto const begin = reinterpret_cast<uintptr_t>(p);
		auto const regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

		return begin + size <= regionEnd;
	}

	inline bool IsImagePtr(const void* p) noexcept
	{
		return IsImageRange(p, sizeof(void*));
	}

	inline bool IsAligned(const void* p, size_t alignment = 4) noexcept
	{
		return p && (reinterpret_cast<uintptr_t>(p) & (alignment - 1)) == 0;
	}

	// AbstractClass/TechnoClass/TechnoTypeClass all keep their vtable at
	// offset 0 and WhatAmI at vtable+0x2C. A live object therefore has a
	// readable vtable in an image and an image-backed WhatAmI implementation.
	inline bool IsSaneObject(const void* p) noexcept
	{
		if (!IsAligned(p))
			return false;

		auto const vtable = *reinterpret_cast<const uintptr_t*>(p);

		if (!IsImageRange(reinterpret_cast<const void*>(vtable), 0x30))
			return false;

		auto const whatAmI = *reinterpret_cast<const uintptr_t*>(vtable + 0x2C);

		return IsImagePtr(reinterpret_cast<const void*>(whatAmI));
	}

	// ILocomotion is a COM interface stored in FootClass::Locomotor. Its vtable
	// necessarily contains the Is_Moving_Now (0x80) and Can_Fire (0x8C) slots
	// that TechnoClass::GetFireError calls, so a valid locomotor must expose
	// image-backed implementations there.
	inline bool IsSaneLocomotor(const void* pLocomotor) noexcept
	{
		if (!IsAligned(pLocomotor))
			return false;

		auto const vtable = *reinterpret_cast<const uintptr_t*>(pLocomotor);

		if (!IsImageRange(reinterpret_cast<const void*>(vtable), 0x90))
			return false;

		auto const slots = reinterpret_cast<const uintptr_t*>(vtable);

		return IsImagePtr(reinterpret_cast<const void*>(slots[0x80 / 4]))
			&& IsImagePtr(reinterpret_cast<const void*>(slots[0x8C / 4]));
	}
}
