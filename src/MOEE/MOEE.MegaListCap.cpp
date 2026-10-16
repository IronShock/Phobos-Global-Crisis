#include "MOEE.h"

#include <Phobos.h>
#include <Unsorted.h>

#include <Windows.h>

#include <iterator>

// Port of MO_ExperienceEnhancements/src/MegaListCap.cpp.
//
// EventClass::MegaMissionList 256 -> 4096.
// The executor (0x64C739) moves at most 256 events per frame from DoList into
// MegaMissionList and drops the rest by setting IsExecuted in the DoList slot.
// MegaMissionList = QueueClass<Event,256> @ 0xA83ED0:
//   Count +0, Head +4, Tail +8, Array[256] @ +12 (111 bytes each),
//   Timings[256] @ 0xA8ADDC.
// There is no room to grow those arrays in place, so Array and Timings are
// moved to a heap buffer with 4096 slots while the queue header stays put.
// This rewrites 38 same-length imm32/disp32 sites: init base+count, enqueue
// cap, enqueue array/timing bases, tail wrap mask, dequeue array base, head
// advance mask, head/index wrap masks (10 sites) and 20 scattered dequeue
// reads. No trampolines are used.
//
// Multiplayer-safe: in MP at most OutList (128) events are pending per frame,
// far below 256, and the new/old base+mask mapping is identical for those
// indices. Direct DoList writes only ever happen in single player.
namespace MOEE::MegaListCap
{
	namespace
	{
		constexpr unsigned int ML_N = 0x1000u;      // 4096 slots (mask 0xFFF)
		constexpr unsigned int ML_MASK = 0xFFFu;
		constexpr unsigned int EVT_SIZE = 111u;

		unsigned char* Buffer = nullptr;            // Array[4096]
		unsigned char* Timings = nullptr;           // Timings[4096]
		bool Initialized = false;
		bool Done = false;
		bool MpLogged = false;

		// Patch one site: verify the expected shape, then write a same-length
		// immediate. Returns true when the site already holds the target value or
		// was written; false when the byte shape does not match.
		bool Patch(unsigned int addr, const unsigned char* want, int len,
			int immOffset, unsigned int newValue)
		{
			if (!addr)
				return false;

			if (Unsorted::CurrentFrame < 60) // wait for the exe to unpack
				return false;

			auto site = reinterpret_cast<unsigned char*>(addr);

			for (int i = 0; i < len; ++i)
			{
				if (want[i] == 0xAA) // wildcard
					continue;

				if (site[i] != want[i])
					return false;
			}

			if (*reinterpret_cast<unsigned int*>(site + immOffset) == newValue)
				return true;

			DWORD old = 0;

			if (!VirtualProtect(site, static_cast<SIZE_T>(len),
				PAGE_EXECUTE_READWRITE, &old))
				return false;

			*reinterpret_cast<unsigned int*>(site + immOffset) = newValue;
			VirtualProtect(site, static_cast<SIZE_T>(len), old, &old);
			FlushInstructionCache(GetCurrentProcess(), site,
				static_cast<SIZE_T>(len));
			return true;
		}

		// Redirect one of the 20 scattered dequeue reads: `[idx*111 + oldBase+off]`.
		// Verifies the 2-byte addressing form then rewrites disp32 to the new base.
		bool Rebase(unsigned int dispAddr, unsigned int off)
		{
			if (!dispAddr || off >= EVT_SIZE)
				return false;

			if (Unsorted::CurrentFrame < 60)
				return false;

			auto d = reinterpret_cast<unsigned char*>(dispAddr);
			const unsigned int oldImm = 0xA83EDCu + off;
			const unsigned int newImm =
				static_cast<unsigned int>(reinterpret_cast<size_t>(Buffer)) + off;

			if (*reinterpret_cast<unsigned int*>(d) != oldImm
				&& *reinterpret_cast<unsigned int*>(d) != newImm)
				return false;

			if (*reinterpret_cast<unsigned int*>(d) == newImm)
				return true;

			if (d[-1] != 0x40)
				return false;

			const unsigned char mrm = d[-2];

			if (mrm != 0x9C && mrm != 0x94 && mrm != 0x84 && mrm != 0x8C)
				return false;

			DWORD old = 0;

			if (!VirtualProtect(d, 4, PAGE_EXECUTE_READWRITE, &old))
				return false;

			*reinterpret_cast<unsigned int*>(d) = newImm;
			VirtualProtect(d, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), d, 4);
			return true;
		}

		// Patch shapes (verified against the exe; 0xAA is a wildcard).
		const unsigned char InitBase[5] = { 0xB8, 0xAA, 0xAA, 0xAA, 0xAA };
		const unsigned char InitCount[5] = { 0xB9, 0x00, 0x01, 0x00, 0x00 };
		const unsigned char EnqCap[10] =
			{ 0x81, 0x3D, 0xD0, 0x3E, 0xA8, 0x00, 0x00, 0x01, 0x00, 0x00 };
		const unsigned char EnqArray[7] =
			{ 0x8D, 0xBC, 0x40, 0xAA, 0xAA, 0xAA, 0xAA };
		const unsigned char EnqTiming[7] =
			{ 0x89, 0x04, 0x8D, 0xAA, 0xAA, 0xAA, 0xAA };
		const unsigned char TailMask[6] =
			{ 0x81, 0xE2, 0xFF, 0x00, 0x00, 0x00 };
		const unsigned char HeadAdvanceMask[6] =
			{ 0x81, 0xE7, 0xFF, 0x00, 0x00, 0x00 };
		const unsigned char DeqArray[7] =
			{ 0x8D, 0x8C, 0x40, 0xAA, 0xAA, 0xAA, 0xAA };
		const unsigned char HeadMask[5] = { 0x25, 0xFF, 0x00, 0x00, 0x00 };

		const unsigned int HeadSites[] =
		{
			0x0064CB14u, 0x0064CB65u, 0x0064CD4Cu, 0x0064CD73u, 0x0064CDB8u,
			0x0064CE60u, 0x0064D6E9u, 0x0064D722u, 0x0064D81Fu, 0x0064D858u
		};

		const unsigned int RebaseAddr[] =
		{
			0x0064CD63u, 0x0064CE6Eu, 0x0064D707u, 0x0064D83Du,
			0x0064CD5Au, 0x0064D6F7u, 0x0064D82Du,
			0x0064CB22u, 0x0064CB76u,
			0x0064CB3Au, 0x0064CB7Du,
			0x0064CB2Du,
			0x0064CB58u, 0x0064CB84u, 0x0064CD81u, 0x0064CDE5u,
			0x0064D737u, 0x0064D86Du,
			0x0064CB49u, 0x0064CDCDu
		};

		const unsigned char RebaseOff[] =
		{
			7, 7, 7, 7,
			11, 11, 11,
			12, 12,
			14, 14,
			18,
			19, 19, 19, 19, 19, 19,
			23, 23
		};
	}

	void Poll()
	{
		if (Done)
			return;

		// Single-player only: install no patches at all in multiplayer (the
		// queue never exceeds 256 there, see the file header).
		if (MOEE::IsMultiplayer())
		{
			if (!MpLogged)
			{
				MpLogged = true;
				Log("megalist", "MP bypass (SP only)");
			}

			return;
		}

		if (!Initialized)
		{
			Initialized = true;
			Buffer = static_cast<unsigned char*>(VirtualAlloc(nullptr,
				ML_N * EVT_SIZE + ML_N * 4u, MEM_COMMIT | MEM_RESERVE,
				PAGE_READWRITE));

			if (!Buffer)
			{
				Log("megalist", "VirtualAlloc fail - OFF");
				Done = true;
				return;
			}

			Timings = Buffer + ML_N * EVT_SIZE;
		}

		if (Unsorted::CurrentFrame < 60)
			return;

		bool ok = true;

		if (!Patch(0x004E7F00u, InitBase, 5, 1,
			static_cast<unsigned int>(reinterpret_cast<size_t>(Buffer))))
			ok = false;

		if (!Patch(0x004E7F05u, InitCount, 5, 1, ML_N))
			ok = false;

		if (!Patch(0x0064C739u, EnqCap, 10, 6, ML_N))
			ok = false;

		if (!Patch(0x0064C774u, EnqArray, 7, 3,
			static_cast<unsigned int>(reinterpret_cast<size_t>(Buffer))))
			ok = false;

		if (!Patch(0x0064C78Cu, EnqTiming, 7, 3,
			static_cast<unsigned int>(reinterpret_cast<size_t>(Timings))))
			ok = false;

		if (!Patch(0x0064C79Fu, TailMask, 6, 2, ML_MASK))
			ok = false;

		if (!Patch(0x0064CC21u, HeadAdvanceMask, 6, 2, ML_MASK))
			ok = false;

		if (!Patch(0x0064CC03u, DeqArray, 7, 3,
			static_cast<unsigned int>(reinterpret_cast<size_t>(Buffer))))
			ok = false;

		for (const unsigned int site : HeadSites)
		{
			if (!Patch(site, HeadMask, 5, 1, ML_MASK))
				ok = false;
		}

		for (size_t i = 0; i < std::size(RebaseAddr); ++i)
		{
			if (!Rebase(RebaseAddr[i], RebaseOff[i]))
				ok = false;
		}

		if (ok && !Done)
		{
			Done = true;
			Log("megalist", "cap 256->4096 ON (38 sites)");
		}
	}
}
