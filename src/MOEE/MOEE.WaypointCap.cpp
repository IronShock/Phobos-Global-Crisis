#include "MOEE.h"

#include <Phobos.h>
#include <Unsorted.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/WaypointCap.cpp.
//
// Two vanilla rejections limit waypoint orders:
//   - 0x6384DC `cmp eax,0x10 / jne`: the 17th unit on the same route is refused.
//   - 0x6391A7 `cmp ebp,0x80`     : planning rejects when branch+new count > 128.
// Single player only; multiplayer always behaves like vanilla (16 / 128).
//
// Unlike upstream this needs no naked asm:
//   - the branch site is a DEFINE_HOOK that reproduces the branch,
//   - the "will be full" prompt is a DEFINE_HOOK that reproduces the compare,
//   - the planning check is an in-place 32-bit immediate patch of
//     `cmp ebp,imm32` (same length, no trampoline). Poll() keeps it in sync with
//     the live single/multiplayer state.
//
// Note: values above 128 additionally rely on the order dispatch not overflowing
// the engine OutList (128); that is handled by the OrderThrottle module.
namespace MOEE::WaypointCap
{
	namespace
	{
		constexpr unsigned int ADDR_WP_CHECK = 0x006384DCu;
		constexpr unsigned int ADDR_WP_NORMAL = 0x00638477u; // jne target: build the branch
		constexpr unsigned int ADDR_WP_REFUSE = 0x006384E1u; // equal branch: play sound, refuse

		constexpr unsigned int ADDR_WP_WARN = 0x006383B4u;
		constexpr unsigned int ADDR_WP_WARN_GO = 0x006383BAu;   // je target: set UI "at cap" state
		constexpr unsigned int ADDR_WP_WARN_NO = 0x006383C4u;   // jne target: continue

		constexpr unsigned int ADDR_WP_ORDER_IMM = 0x006391A8u; // imm32 of `cmp ebp,0x80`
		constexpr unsigned int ADDR_WP_ORDER_CMP = 0x006391A7u; // `81 FD` opcode

		constexpr unsigned int WP_VANILLA = 16u;
		constexpr unsigned int WP_INF = 0x7FFFFFFFu;
		constexpr unsigned int WP_MAXCFG = 4096u;

		unsigned int BranchCap = WP_VANILLA; // same-route cap in effect (per frame)
		unsigned int WarnAt = WP_VANILLA - 1;
		unsigned int OrderCap = 0x80u;       // planning cap in effect (per frame)
		bool Patched = false;
		bool MpLogged = false;

		unsigned int Cfg()
		{
			int cfg = Phobos::Config::MOEE_WaypointCap;

			if (cfg < 0)
				cfg = 0;

			if (static_cast<unsigned int>(cfg) > WP_MAXCFG)
				cfg = static_cast<int>(WP_MAXCFG);

			return static_cast<unsigned int>(cfg);
		}

		// Write the planning cap in place (same-length imm32 patch).
		bool PatchOrderCap(unsigned int value)
		{
			if (!Patched)
			{
				auto op = reinterpret_cast<unsigned char*>(ADDR_WP_ORDER_CMP);

				if (op[0] != 0x81 || op[1] != 0xFD)
				{
					Log("waypointcap", "order site mismatch - OFF");
					Patched = true; // do not retry
					return false;
				}

				Patched = true;
			}

			auto imm = reinterpret_cast<unsigned int*>(ADDR_WP_ORDER_IMM);

			if (*imm == value)
				return true;

			DWORD old = 0;

			if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old))
				return false;

			*imm = value;
			VirtualProtect(imm, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), imm, 4);
			return true;
		}
	}

	// Refuse logging (kept from upstream: kind 0 = branch wall, 1 = planning wall).
	void LogRefuse(int kind, unsigned int count, unsigned int cap)
	{
		Log("waypointcap", "refuse kind=%d cnt=%u cap=%u", kind, count, cap);
	}

	void Poll()
	{
		const bool mp = IsMultiplayer();

		if (mp)
		{
			BranchCap = WP_VANILLA;
			WarnAt = WP_VANILLA - 1;
			OrderCap = 0x80u;

			if (!MpLogged)
			{
				MpLogged = true;
				Log("waypointcap", "MP bypass (vanilla 16/128)");
			}
		}
		else
		{
			MpLogged = false;
			const unsigned int cfg = Cfg();
			BranchCap = (cfg > 0) ? cfg : WP_INF;
			WarnAt = (cfg > 0) ? (cfg - 1) : WP_INF;
			OrderCap = (cfg > 0) ? cfg : WP_INF;
		}

		if (Unsorted::CurrentFrame < 60)
			return;

		PatchOrderCap(OrderCap);
	}
}

// 0x6384DC: `cmp eax,0x10 / jne` (5 bytes). eax = current same-route count.
// Reproduce the branch decision with the effective cap.
DEFINE_HOOK(0x6384DC, MOEE_WaypointCap_BranchLimit, 0x5)
{
	GET(unsigned int, count, EAX);

	if (count < MOEE::WaypointCap::BranchCap)
		return 0x638477u;

	MOEE::WaypointCap::LogRefuse(0, count, MOEE::WaypointCap::BranchCap);
	return 0x6384E1u;
}

// 0x6383B4: `cmp [ebp+0x14],0xF / jne` (6 bytes). ebp = planning token.
// Reproduce the compare against the effective "will be full" position.
DEFINE_HOOK(0x6383B4, MOEE_WaypointCap_WarnLimit, 0x6)
{
	const auto token = reinterpret_cast<unsigned int*>(
		static_cast<uintptr_t>(R->EBP()));
	const unsigned int value = token[0x14 / 4];

	if (value == MOEE::WaypointCap::WarnAt)
		return 0x6383BAu;

	return 0x6383C4u;
}
