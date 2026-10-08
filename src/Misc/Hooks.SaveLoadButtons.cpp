#include <Phobos.h>

#include <GameOptionsClass.h>

#include <Ext/Rules/Body.h>
#include <Utilities/Macro.h>

// Removes the in-game ESC menu "Save Game" and "Load Game" buttons at the
// difficulties configured in [General] -> DisableSaveGame.Difficulty /
// DisableLoadGame.Difficulty. The buttons are hidden when the menu opens and
// their WM_COMMAND handlers are neutralized, so they cannot be triggered even
// via keyboard navigation.
//
// The in-game menu is a Win32 (modal) dialog whose proc is at 0x4F11B0. The
// dialog is created via DialogBoxIndirectParamA, so the HWND is only valid
// inside the proc (proc arg 1, held in ESI after the prologue). The dialog
// result (not the HWND) is what the dialog creator returns. The "Save Game"
// button has control ID 0x51F and opens LoadOptionsClass in Save mode;
// "Load Game" has control ID 0x51E and opens LoadOptionsClass::LoadDialog.

namespace SaveLoadButtonsTemp
{
	constexpr int SaveGameButton = 0x51F;
	constexpr int LoadGameButton = 0x51E;
}

// In-game menu dialog proc, right after the `call 0x622B50` at 0x4F11DC.
// ESI = real dialog HWND, EBX = uMsg. Replicate the patched
// `test eax,eax; jne 0x4F16F3` and hide the Save/Load buttons on
// WM_INITDIALOG (0x110), when the buttons exist as standard controls.
DEFINE_HOOK(0x4F11E1, InGameMenu_DialogProc_HideSaveLoadButtons, 0x8)
{
	GET(HWND, hDlg, ESI);
	GET(UINT, uMsg, EBX);

	// Replicate the patched `test eax,eax; jne 0x4F16F3` (0x622B50's return).
	if (R->EAX())
		return 0x4F16F3;

	if (uMsg == 0x110) // WM_INITDIALOG
	{
		auto const pRulesExt = RulesExt::Global();
		const bool saveDisabled = pRulesExt->IsSaveGameDisabled();
		const bool loadDisabled = pRulesExt->IsLoadGameDisabled();

		if (saveDisabled)
		{
			if (auto const hButton = GetDlgItem(hDlg, SaveLoadButtonsTemp::SaveGameButton))
				ShowWindow(hButton, SW_HIDE);
		}
		if (loadDisabled)
		{
			if (auto const hButton = GetDlgItem(hDlg, SaveLoadButtonsTemp::LoadGameButton))
				ShowWindow(hButton, SW_HIDE);
		}
	}

	return 0;
}

// In-game menu dialog proc WM_COMMAND dispatch - "Save Game" button (0x51F).
DEFINE_HOOK(0x4F1332, InGameMenu_DialogProc_DisableSaveGame, 0x6)
{
	GET(int, wCtrlID, EDI);

	if (wCtrlID == SaveLoadButtonsTemp::SaveGameButton)
	{
		if (RulesExt::Global()->IsSaveGameDisabled())
			return 0x4F16F1;
	}

	return 0;
}

// In-game menu dialog proc WM_COMMAND dispatch - "Load Game" button (0x51E).
DEFINE_HOOK(0x4F1348, InGameMenu_DialogProc_DisableLoadGame, 0x6)
{
	GET(int, wCtrlID, EDI);

	if (wCtrlID == SaveLoadButtonsTemp::LoadGameButton)
	{
		if (RulesExt::Global()->IsLoadGameDisabled())
			return 0x4F16F1;
	}

	return 0;
}
