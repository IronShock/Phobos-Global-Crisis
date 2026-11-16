#include "MOEE.h"
#include "MOEE.Commands.h"

#include <Phobos.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Ext/Unit/FootprintDebug.h>
#include <Utilities/Debug.h>
#include <Utilities/CrashTrace.h>

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

		if (ep && ep->ExceptionRecord)
		{
			const DWORD code = ep->ExceptionRecord->ExceptionCode;

			if (code != 0x406D1388u && code != 0x80000003u && code != 0xE06D7363u)
			{
				// Dump once, before the (possibly corrupt) game state is torn down.
				// This preserves the last GetFireError context even when the crash
				// itself happens deep inside a virtual call.
				CrashTrace::Dump();

				static int s_stackN = 0;

				const unsigned eip = ep->ContextRecord
					? static_cast<unsigned>(ep->ContextRecord->Eip) : 0u;
				const unsigned esp = ep->ContextRecord
					? static_cast<unsigned>(ep->ContextRecord->Esp) : 0u;
				const unsigned ebp = ep->ContextRecord
					? static_cast<unsigned>(ep->ContextRecord->Ebp) : 0u;

				if (s_n < 8)
				{
					s_n++;
					Log("fault", "code=0x%08X eip=0x%08X addr=0x%08X esp=0x%08X ebp=0x%08X",
						static_cast<unsigned>(code), eip,
						static_cast<unsigned>(
							reinterpret_cast<size_t>(ep->ExceptionRecord->ExceptionAddress)),
						esp, ebp);
				}

				if (s_stackN < 4)
				{
					s_stackN++;

					// CaptureStackBackTrace may itself be unable to walk a
					// completely exhausted stack, so also dump the raw EBP
					// chain. The chain is what identifies the recursive
					// function when the fault is 0xC00000FD.
					typedef USHORT(WINAPI* FnCaptureStack)(ULONG, ULONG, PVOID*, PULONG);
					static FnCaptureStack pCapture = reinterpret_cast<FnCaptureStack>(
						GetProcAddress(GetModuleHandleA("kernel32.dll"),
							"CaptureStackBackTrace"));

					if (pCapture)
					{
						void* frames[24] = {};
						const USHORT count = pCapture(0, 24, frames, nullptr);
						Log("stack", "capture frames=%u esp=0x%08X ebp=0x%08X",
							static_cast<unsigned>(count), esp, ebp);

						for (USHORT i = 0; i < count; ++i)
							Log("stack", "#%02u %p", static_cast<unsigned>(i), frames[i]);
					}

					DWORD frame = ebp;

					for (int i = 0; i < 40 && frame; ++i)
					{
						if (IsBadReadPtr(reinterpret_cast<void*>(frame), 8))
							break;

						const DWORD ret = *reinterpret_cast<DWORD*>(frame + 4);
						const DWORD next = *reinterpret_cast<DWORD*>(frame);
						Log("stack", "ebp#%02d ret=%p next=%p", i,
							reinterpret_cast<void*>(ret),
							reinterpret_cast<void*>(next));

						if (next <= frame)
							break;

						frame = next;
					}
				}
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
			Log("diag", "GetFireError guard=%d trace=%d",
				Phobos::Config::GetFireErrorGuard ? 1 : 0,
				Phobos::Config::GetFireErrorTrace ? 1 : 0);

			// Keyboard-side bindings, read once the engine has loaded KeyboardMD.ini.
			static const char* const keyNames[] =
			{
				CommandNames::AttackMove, CommandNames::TreeChop, CommandNames::FuzzyAttack,
				CommandNames::SmartOrder, CommandNames::PatrolCircle, CommandNames::SmartArea,
				CommandNames::DeployPreview, CommandNames::WeaponRange, CommandNames::RepairAll,
			};

			for (const char* name : keyNames)
			{
				const auto part = GetKeyPart(name);
				Log("keys", "%s mods=%u vk=0x%02X", name, part.mods, part.vk);
			}
		}

		if (Disabled())
			return;

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
	}

	void Draw()
	{
		if (Disabled())
			return;

		// Preview overlays from the smart-order modules are drawn here.
		TreeChop::Draw();
		FuzzyAttack::Draw();
		SmartOrder::Draw();
		PatrolCircle::Draw();
		SmartArea::Draw();
		DeployPreview::Draw();
		WeaponRange::Draw();
		FootprintDebug::Draw();
	}
}
