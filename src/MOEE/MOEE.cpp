#include "MOEE.h"

#include <Phobos.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <cstdarg>
#include <cstdio>

namespace MOEE
{
	bool IsPlausiblePtr(const void* p)
	{
		// Same heuristic as MO_EE's IsPlausiblePtr: heap/exe range, 4-byte aligned.
		const auto u = static_cast<unsigned>(reinterpret_cast<size_t>(p));
		return u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0;
	}

	bool IsMultiplayer()
	{
		return SessionClass::IsMultiplayer(); // GameMode LAN/Internet
	}

	void Log(const char* pTag, const char* pFormat, ...)
	{
		char buffer[512];
		int offset = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
			"[MOEE:%s] ", pTag ? pTag : "?");

		if (offset < 0)
			offset = 0;

		va_list args;
		va_start(args, pFormat);
		_vsnprintf_s(buffer + offset, sizeof(buffer) - static_cast<size_t>(offset),
			_TRUNCATE, pFormat, args);
		va_end(args);

		Debug::Log("%s\n", buffer);
	}

	void Poll()
	{
		MegaListCap::Poll();
		Observer::Poll();
		WaypointCap::Poll();
		AttackMove::Poll();
		RepairAll::Poll();
		PlaceScatter::Poll();
		TreeChop::Poll();
		FuzzyAttack::Poll();
		DeployPreview::Poll();
		SmartOrder::Poll();
		PatrolCircle::Poll();
		SmartArea::Poll();
		GridView::Poll();
	}

	void Draw()
	{
		// Preview overlays from the smart-order modules are drawn here.
		TreeChop::Draw();
		FuzzyAttack::Draw();
		SmartOrder::Draw();
		PatrolCircle::Draw();
		SmartArea::Draw();
		DeployPreview::Draw();
		WeaponRange::Draw();
		GridView::Draw();
	}
}
