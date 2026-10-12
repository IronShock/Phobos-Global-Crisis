#include "Body.h"
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <ThemeClass.h>

#include <Ext/Scenario/Body.h>

#include <windows.h>
#include <fstream>

#include <Utilities/Debug.h>

// Writes the outcome of the current single-player mission to Client\MissionResult.ini
// (resolved from the game's executable directory) so the CnCNet client can award
// victory badges and unlock achievements after the game exits. On a victory the set
// local and global variables (from Phobos' redirected variable storage) are appended
// so the client can resolve NeedUnlock achievements.
static void WriteMissionResult(bool won)
{
	char exePath[MAX_PATH];
	const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);

	std::string path(exePath, length);
	const size_t slash = path.find_last_of("\\/");
	if (slash != std::string::npos)
		path = path.substr(0, slash + 1);
	path += "Client\\MissionResult.ini";

	std::ofstream file(path, std::ios::trunc);
	if (file.is_open())
	{
		file << "[Result]\n";
		file << "Won=" << (won ? "yes" : "no") << "\n";

		if (won)
		{
			if (const auto pExt = ScenarioExt::Global())
			{
				bool wroteHeader = false;
				for (int side = 0; side < 2; ++side)
				{
					for (auto const& [index, variable] : pExt->Variables[side])
					{
						Debug::Log("[Phobos] MissionResult: %s variable %d = %s (%d)\n",
							side == 0 ? "local" : "global", index, variable.Name, variable.Value);

						if (variable.Value != 0 && variable.Name[0] != '\0')
						{
							if (!wroteHeader)
							{
								file << "\n[Globals]\n";
								wroteHeader = true;
							}
							file << variable.Name << "=1\n";
						}
					}
				}
			}
		}
	}
}

// HouseClass::Win - the current player's house won the mission.
// Hooked after the prologue so that `this` (already saved in EBX) is not clobbered
// by the hook body's function calls.
DEFINE_HOOK(0x4FC9E6, HouseClass_Win_MissionResult, 0x8)
{
	GET(HouseClass* const, pHouse, EBX);

	if (pHouse && pHouse->IsHumanPlayer)
	{
		WriteMissionResult(true);
		Debug::Log("[Phobos] MissionResult: won\n");
	}

	return 0;
}

// HouseClass::Lose - the current player's house lost the mission.
// Hooked after the prologue so that `this` (already saved in EBP) is not clobbered
// by the hook body's function calls.
DEFINE_HOOK(0x4FCBD6, HouseClass_Lose_MissionResult, 0x8)
{
	GET(HouseClass* const, pHouse, EBP);

	if (pHouse && pHouse->IsHumanPlayer)
	{
		WriteMissionResult(false);
		Debug::Log("[Phobos] MissionResult: lost\n");
	}

	return 0;
}

// Ingame music switch when defeated
DEFINE_HOOK_AGAIN(0x4FCB7D, HouseClass_WinLoseTheme, 0x5)  // HouseClass::Flag_To_Win
DEFINE_HOOK(0x4FCD66, HouseClass_WinLoseTheme, 0x5)        // HouseClass::Flag_To_Lose
{
	const auto pThis = HouseClass::CurrentPlayer;

	// This fires after all victory triggers have executed (the win music stage), so
	// re-write the result to capture the final local/global variable state.
	if (pThis && pThis->IsWinner)
	{
		WriteMissionResult(true);
		Debug::Log("[Phobos] MissionResult: won (final)\n");
	}

	const auto pSide = SideClass::Array.GetItemOrDefault(pThis->SideIndex);
	const auto pSideExt = SideExt::ExtMap.TryFind(pSide);

	if (pSideExt)
	{
		const int themeIndex = (pThis->IsWinner) ? pSideExt->IngameScore_WinTheme : pSideExt->IngameScore_LoseTheme;
		if (themeIndex >= 0)
			ThemeClass::Instance.Play(themeIndex);
	}

	return 0;
}
