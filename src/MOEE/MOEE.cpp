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

	static LONG CALLBACK MOEE_Veh(EXCEPTION_POINTERS* ep)
	{
		static int s_n = 0;

		if (ep && ep->ExceptionRecord && s_n < 8)
		{
			const DWORD code = ep->ExceptionRecord->ExceptionCode;

			if (code != 0x406D1388u && code != 0x80000003u && code != 0xE06D7363u)
			{
				const unsigned eip = ep->ContextRecord
					? static_cast<unsigned>(ep->ContextRecord->Eip) : 0u;
				s_n++;
				Log("fault", "code=0x%08X eip=0x%08X addr=0x%08X",
					static_cast<unsigned>(code), eip,
					static_cast<unsigned>(
						reinterpret_cast<size_t>(ep->ExceptionRecord->ExceptionAddress)));
			}
		}

		return EXCEPTION_CONTINUE_SEARCH;
	}

	void Poll()
	{
		// One-time diagnostics: log the Phobos image base and install a vectored
		// exception handler so a crash address can be mapped back to a function
		// via the build's Phobos.map (RVA = crash eip - base).
		static bool s_diag = false;

		if (!s_diag)
		{
			s_diag = true;
			AddVectoredExceptionHandler(1, MOEE_Veh);
			Log("diag", "phobos base=0x%08X",
				static_cast<unsigned>(reinterpret_cast<size_t>(Phobos::hInstance)));
		}

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
