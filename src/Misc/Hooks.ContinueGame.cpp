#include <Phobos.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>

#include <ScenarioClass.h>
#include <SessionClass.h>

#include <GameStrings.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <RulesClass.h>
#include <StringTable.h>
#include <Unsorted.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

// Dedicated per-mission continue saves used by the CnCNet client's
// "Continue Game" button on the campaign selection screen. A save named
// "ContinueGame_<map>.cgs" is written when the player leaves a running
// campaign mission, and it is deleted when the mission is won. The non-SAV
// extension keeps these files out of the engine's regular in-game "Load Game"
// list (which only shows *.sav files), while the client's "Continue Game"
// button knows to look for them by the "ContinueGame_" name prefix.
//
// Saving must happen in a safe engine context: the in-game menu Quit hook
// performs the save synchronously while the game is still frozen behind the
// modal ESC menu, and the exit-event hook is a fallback for quit paths that
// bypass the vanilla menu dialog. The game-exit hook is a fallback for exits
// that never pass through the main loop again (e.g. closing the window
// mid-mission).

namespace ContinueGameTemp
{
	constexpr const char* Prefix = "ContinueGame_";
	constexpr const char* LogFile = "debug/continuegame.log";

	// Set by the in-game menu Quit hook, consumed by the main loop hook.
	bool PendingContinueSave = false;

	// Becomes true once a continue save was written in this game session so the
	// shutdown hook does not re-enter the save machinery a second time.
	bool SavedContinueGame = false;

	// Crash-safe log: written directly to disk and flushed after every line so
	// entries survive a hard crash while the regular (buffered) debug.log may
	// lose them. Also echoed to the regular game log for convenience.
	void Log(const char* pFormat, ...)
	{
		char buffer[0x400];
		va_list args;
		va_start(args, pFormat);
		vsprintf_s(buffer, pFormat, args);
		va_end(args);

		Debug::Log("%s", buffer);

		FILE* pFile = fopen(LogFile, "a");
		if (!pFile)
			pFile = fopen("continuegame.log", "a");
		if (!pFile)
			return;

		fprintf(pFile, "%s\n", buffer);
		fflush(pFile);
		fclose(pFile);
	}

	// Current campaign scenario file name without extension, e.g. "TUTO4" for
	// "TUTO4.MAP". Empty if no scenario is loaded.
	std::string GetScenarioBaseName()
	{
		if (!ScenarioClass::Instance)
			return {};

		std::filesystem::path path(ScenarioClass::Instance->FileName);
		return path.stem().string();
	}

	// True while a campaign mission is actually running (as opposed to being in
	// the main menu after quitting a mission). Used by the game-exit hook so it
	// only writes a continue save when there is a real mission state to capture.
	bool IsCampaignMissionActive()
	{
		return SessionClass::IsCampaign()
			&& SessionClass::Instance.CurrentlyInGame
			&& ScenarioClass::Instance
			&& ScenarioClass::Instance->FileName[0] != '\0';
	}

	// Shows a message on screen. A forced redraw is only done outside of special
	// dialogs, mirroring the pattern used by the regular (passive) save so we do
	// not render underneath a modal dialog such as the in-game ESC menu.
	void PrintMessage(const wchar_t* pMessage)
	{
		MessageListClass::Instance.PrintMessage(
			pMessage,
			RulesClass::Instance->MessageDelay,
			HouseClass::CurrentPlayer->ColorSchemeIndex,
			/* bSilent: */ true
		);

		if (Game::SpecialDialog == 0)
		{
			MapClass::Instance.MarkNeedsRedraw(2);
			MapClass::Instance.Render();
		}
	}
}

void Phobos::SaveContinueGame()
{
	ContinueGameTemp::PendingContinueSave = false;

	if (ContinueGameTemp::SavedContinueGame)
	{
		ContinueGameTemp::Log("SaveContinueGame: skipped (already saved this session)");
		return;
	}

	if (!Phobos::Config::ContinueGameOnExit || !SessionClass::IsCampaign())
	{
		ContinueGameTemp::Log("SaveContinueGame: skipped (ContinueGameOnExit=%d GameMode=%d)",
			Phobos::Config::ContinueGameOnExit, static_cast<int>(SessionClass::Instance.GameMode));
		return;
	}

	std::string baseName = ContinueGameTemp::GetScenarioBaseName();
	if (baseName.empty())
	{
		ContinueGameTemp::Log("SaveContinueGame: skipped (empty scenario base name)");
		return;
	}

	char fileName[0x80];
	_snprintf_s(fileName, sizeof(fileName), "%s%s.cgs", ContinueGameTemp::Prefix, baseName.c_str());

	std::wstring description = ScenarioClass::Instance->UINameLoaded;
	description += L" - Continue";

	ContinueGameTemp::PrintMessage(StringTable::LoadString(GameStrings::TXT_SAVING_GAME));

	// The engine serializes the whole ScenarioClass struct into the save,
	// including the pause counter (@0x62C), IsGamePaused (@0x630) and
	// UserInputLocked (@0x35A2). If any of these is non-zero at save time, the
	// loaded (continue) game would start paused or with user input locked. Zero
	// them for the save and restore the runtime state afterwards.
	auto const scenario = ScenarioClass::Instance;
	const DWORD savedPauseCounter = scenario->unknown_62C;
	const bool savedIsGamePaused = scenario->IsGamePaused;
	const bool savedUserInputLocked = scenario->UserInputLocked;
	scenario->unknown_62C = 0;
	scenario->IsGamePaused = false;
	scenario->UserInputLocked = false;
	Unsorted::UserInputLocked = false;

	ContinueGameTemp::Log("SaveContinueGame: writing %s (base=%s)", fileName, baseName.c_str());
	const bool bSaved = ScenarioClass::SaveGame(fileName, description.c_str());

	scenario->unknown_62C = savedPauseCounter;
	scenario->IsGamePaused = savedIsGamePaused;
	scenario->UserInputLocked = savedUserInputLocked;

	ContinueGameTemp::Log("SaveContinueGame: SaveGame returned %d", bSaved);

	if (bSaved)
		ContinueGameTemp::SavedContinueGame = true;
}

void Phobos::DeleteContinueGame()
{
	if (!SessionClass::IsCampaign())
		return;

	std::string baseName = ContinueGameTemp::GetScenarioBaseName();
	if (baseName.empty())
		return;

	char fileName[0x80];
	_snprintf_s(fileName, sizeof(fileName), "%s%s.cgs", ContinueGameTemp::Prefix, baseName.c_str());

	ContinueGameTemp::Log("DeleteContinueGame: removing %s", fileName);
	std::error_code ec;
	std::filesystem::path savePath = std::filesystem::path("Saved Games") / fileName;
	std::filesystem::remove(savePath, ec);
	ContinueGameTemp::Log("DeleteContinueGame: result ec=%d", ec.value());
}

// The in-game ESC menu "Quit" button (control ID 0x520) inside the dialog proc
// at 0x4F11B0. This is only reached for a genuine quit click while a mission is
// paused behind the menu. The save is performed here, synchronously, while the
// game is still frozen behind the modal dialog.
DEFINE_HOOK(0x4F148F, InGameMenu_Quit_SaveContinueGame, 0x6)
{
	// This hook sits on top of `mov edi, dword ptr [0x7E1498]`, which the
	// vanilla code right after this hook relies on via `call edi`. Restore it so
	// the dialog's quit handler completes normally.
	R->EDI(*reinterpret_cast<DWORD*>(0x7E1498));

	ContinueGameTemp::Log("hook InGameMenu_Quit: entered (GameMode=%d)", static_cast<int>(SessionClass::Instance.GameMode));

	if (SessionClass::IsCampaign())
	{
		// The game is still frozen behind the modal ESC menu here, so the
		// continue save can be performed synchronously while nothing advances.
		// The double-save guard inside SaveContinueGame prevents any fallback
		// hook from writing the save a second time.
		Phobos::SaveContinueGame();
		ContinueGameTemp::Log("hook InGameMenu_Quit: continue save performed synchronously");
	}

	return 0;
}

// The vanilla game's "Processing EXIT event" path in the main event handler.
// This runs for every quit (the debug log logs it on each exit) and lives in
// the same main-loop region as the reliable scenario-start autosave hook, so it
// does not depend on the in-game menu dialog being the vanilla one. The mission
// state is still fully intact at this point (teardown only happens after the
// main loop finishes), so the continue save is written directly here.
// Victory/defeat use their own flows and set ScenarioClass::EndOfGame, which is
// excluded to honor the "no save on win/lose" requirement.
DEFINE_HOOK(0x4C7903, ExitEvent_Process_SaveContinueGame, 0x7)
{
	ContinueGameTemp::Log("hook ExitEvent_Process: entered (GameMode=%d EndOfGame=%d)",
		static_cast<int>(SessionClass::Instance.GameMode),
		ScenarioClass::Instance ? ScenarioClass::Instance->EndOfGame : -1);

	if (SessionClass::IsCampaign()
		&& ScenarioClass::Instance
		&& !ScenarioClass::Instance->EndOfGame)
	{
		Phobos::SaveContinueGame();
	}

	return 0;
}

// The game's exit/shutdown function. Fallback for exits that never get another
// main-loop frame (e.g. Alt+F4 / window close mid-mission). Guarded so it only
// saves a real campaign mission state and never re-saves after the main loop
// already produced the continue save.
DEFINE_HOOK(0x6BEC60, Game_Exit_SaveContinueGame, 0x5)
{
	ContinueGameTemp::Log("hook Game_Exit: entered (Saved=%d Active=%d GameMode=%d)",
		ContinueGameTemp::SavedContinueGame, ContinueGameTemp::IsCampaignMissionActive(),
		static_cast<int>(SessionClass::Instance.GameMode));

	if (!ContinueGameTemp::SavedContinueGame && ContinueGameTemp::IsCampaignMissionActive())
		Phobos::SaveContinueGame();

	return 0;
}

// Victory transition (DoWin). A completed mission no longer needs its continue
// save, so delete it to keep the client's "Continue Game" button consistent.
DEFINE_HOOK(0x685D80, DoWin_DeleteContinueGame, 0x6)
{
	if (SessionClass::IsCampaign())
		Phobos::DeleteContinueGame();

	return 0;
}
