#include "Phobos.h"

#include <Drawing.h>
#include <HouseClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <GameOptionsClass.h>

#include <Utilities/Debug.h>
#include <Utilities/Patch.h>
#include <Utilities/Macro.h>
#include "Utilities/AresHelper.h"
#include "Utilities/GeneralUtils.h"
#include "Utilities/Parser.h"
#include <Ext/TechnoType/Body.h>
#include <Ext/Rules/Body.h>

#include <Windows.h>
#include <RadBeam.h>
#include <New/Entity/FilterButtonClass.h>
#include <New/Entity/AttachmentClass.h>
#include <FPSCounter.h>
#include <GameStrings.h>

#ifndef IS_RELEASE_VER
bool HideWarning = false;
#endif

HANDLE Phobos::hInstance = 0;

char Phobos::readBuffer[Phobos::readLength];
wchar_t Phobos::wideBuffer[Phobos::readLength];

const char* Phobos::AppIconPath = nullptr;

bool Phobos::DisplayDamageNumbers = false;
bool Phobos::IsLoadingSaveGame = false;

bool Phobos::Optimizations::Applied = false;
bool Phobos::Optimizations::DisableRadDamageOnBuildings = true;
bool Phobos::Optimizations::DisableSyncLogging = false;

#ifdef STR_GIT_COMMIT
const wchar_t* Phobos::VersionDescription = L"Phobos nightly build (" STR_GIT_COMMIT L" @ " STR_GIT_BRANCH L"). DO NOT SHIP IN MODS!";
#elif !defined(IS_RELEASE_VER)
const wchar_t* Phobos::VersionDescription = L"Phobos development build #" _STR(BUILD_NUMBER) L". Please test the build before shipping.";
#else
//const wchar_t* Phobos::VersionDescription = L"Phobos release build v" FILE_VERSION_STR L".";
#endif


void Phobos::CmdLineParse(char** ppArgs, int nNumArgs)
{
	bool foundInheritance = false;
	bool foundInclude = false;
	bool dontSetExceptionHandler =
#ifdef DEBUG
		false;
#else
		false;
#endif // DEBUG
	Parser<bool> boolParser { };

	// > 1 because the exe path itself counts as an argument, too!
	for (int i = 1; i < nNumArgs; i++)
	{
		const char* pArg = ppArgs[i];
		std::string arg = pArg;

		if (_stricmp(pArg, "-Icon") == 0)
		{
			Phobos::AppIconPath = ppArgs[++i];
		}
#ifndef IS_RELEASE_VER
		if (_stricmp(pArg, "-b=" _STR(BUILD_NUMBER)) == 0)
		{
			HideWarning = true;
		}
#endif
		if (_stricmp(pArg, "-Inheritance") == 0)
		{
			foundInheritance = true;
		}
		if (_stricmp(pArg, "-Include") == 0)
		{
			foundInclude = true;
		}
		if (arg.starts_with("-ExceptionHandler="))
		{
			auto delimIndex = arg.find("=");
			auto value = arg.substr(delimIndex + 1, arg.size() - delimIndex - 1);

			bool v = dontSetExceptionHandler;
			if (boolParser.TryParse(value.c_str(), &v))
				dontSetExceptionHandler = !v;
		}
	}

	if (foundInclude)
	{
		Patch::Apply_RAW(0x474200, // Apply CCINIClass_ReadCCFile1_DisableAres
			{ 0x8B, 0xF1, 0x8D, 0x54, 0x24, 0x0C }
		);

		Patch::Apply_RAW(0x474314, // Apply CCINIClass_ReadCCFile2_DisableAres
			{ 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00 }
		);
	}
	else
	{
		Patch::Apply_RAW(0x474230, // Revert CCINIClass_Load_Inheritance
			{ 0x8B, 0xE8, 0x88, 0x5E, 0x40 }
		);
	}

	if (foundInheritance)
	{
		Patch::Apply_RAW(0x528A10, // Apply INIClass_GetString_DisableAres
			{ 0x83, 0xEC, 0x0C, 0x33, 0xC0 }
		);

		Patch::Apply_RAW(0x526CC0, // Apply INIClass_GetKeyName_DisableAres
			{ 0x8B, 0x54, 0x24, 0x04, 0x83, 0xEC, 0x0C }
		);
	}
	else
	{
		Patch::Apply_RAW(0x528BAC, // Revert INIClass_GetString_Inheritance_NoEntry
			{ 0x8B, 0x7C, 0x24, 0x2C, 0x33, 0xC0, 0x8B, 0x4C, 0x24, 0x28 }
		);
	}

	Game::DontSetExceptionHandler = dontSetExceptionHandler;

	Debug::Log("Initialized version: " PRODUCT_VERSION "\n");
	Debug::Log("ExceptionHandler is %s\n", dontSetExceptionHandler ? "not present" : "present");
}

void Phobos::ExeRun()
{
	Patch::ApplyStatic();

#ifdef DEBUG

	if (Phobos::DetachFromDebugger())
	{
		MessageBoxW(NULL,
		L"You can now attach a debugger.\n\n"

		L"Press OK to continue YR execution.",
		L"Debugger Notice", MB_OK);
	}
	else
	{
		MessageBoxW(NULL,
		L"You can now attach a debugger.\n\n"

		L"To attach a debugger find the YR process in Process Hacker "
		L"/ Visual Studio processes window and detach debuggers from it, "
		L"then you can attach your own debugger. After this you should "
		L"terminate Syringe.exe because it won't automatically exit when YR is closed.\n\n"

		L"Press OK to continue YR execution.",
		L"Debugger Notice", MB_OK);
	}

	if (!Console::Create())
	{
		MessageBoxW(NULL,
		L"Failed to allocate the debug console!",
		L"Debug Console Notice", MB_OK);
	}

#endif
}

void Phobos::ExeTerminate()
{
	Console::Release();
}

// =============================
// hooks

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID v)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		Phobos::hInstance = hInstance;
	}
	return true;
}

DEFINE_HOOK(0x7CD810, ExeRun, 0x9)
{
	Phobos::ExeRun();
	AresHelper::Init();

	return 0;
}

// Avoid confusing the profiler unless really necessary
#ifdef DEBUG
DEFINE_NAKED_HOOK(0x7CD8EA, _ExeTerminate)
{
	// Call WinMain
	SET_REG32(EAX, 0x6BB9A0);
	CALL(EAX);
	PUSH_REG(EAX);

	__asm {call Phobos::ExeTerminate};

	// Jump back
	POP_REG(EAX);
	SET_REG32(EBX, 0x7CD8EF);
	__asm {jmp ebx};
}
#endif
DEFINE_HOOK(0x52F639, _YR_CmdLineParse, 0x5)
{
	GET(char**, ppArgs, ESI);
	GET(int, nNumArgs, EDI);

	Phobos::CmdLineParse(ppArgs, nNumArgs);
	Debug::LogDeferredFinalize();
	return 0;
}

DEFINE_HOOK(0x67E44D, LoadGame_SetFlag, 0x5)
{
	Phobos::IsLoadingSaveGame = true;
	return 0;
}

DEFINE_HOOK(0x67E68A, LoadGame_UnsetFlag, 0x5)
{
	Phobos::IsLoadingSaveGame = false;
	Phobos::ApplyOptimizations();

	// Restore AttachmentClass::Data pointers after all loading is complete.
	// At this point, TechnoTypeExt::ExtData (including AttachmentData vectors)
	// has been loaded and Swizzle fixup has processed all pointer remappings.
	AttachmentClass::FixupDataPointers();

	// Restore ParentAttachment pointers on child units and verify their
	// locomotors. AttachmentClass* cannot be swizzled (not a game object),
	// so the child's ParentAttachment is stale after load. Without this,
	// AttachmentLocomotionClass::GetAttachment() returns null, causing
	// Draw_Matrix to fall back to the default and the child renders invisible.
	AttachmentClass::FixupParentAttachments();

	// Re-resolve the AttachmentDataEntry::Type / TechnoType indices from the
	// names stored in the save. TechnoTypeClass::Array ordering is not stable
	// across save/load (large rules + INI inheritance can reorder it), so raw
	// indices saved to the stream would resolve to the wrong types. This must
	// run now, after all TechnoTypeClass data has finished loading, but before
	// CreatePendingChildren() creates any children.
	TechnoTypeExt::ResolveAttachmentDataIndices();

	// Create children for attachment slots that were created during save
	// loading for technos that are not present in the save - their OnCreated()
	// was deferred via bIsPendingChildCreation. This MUST run here (after all
	// techno objects have been deserialized) and NOT in LoadGame_Phobos
	// (0x67E826), because at 0x67E826 AttachmentClass::Array is still empty.
	// The Enumerable arrays (AttachmentTypeClass::Array) have been rebuilt by
	// the type registry load at 0x67E826, so GetType()/GetChildType() resolve
	// correctly here.
	AttachmentClass::CreatePendingChildren();

	return 0;
}

DEFINE_HOOK(0x683E7F, ScenarioClass_Start_Optimizations, 0x7)
{
	Debug::Log("[Phobos] MissionResult: scenario started\n");

	Phobos::ApplyOptimizations();
	RulesExt::ApplyRemoveShroudGlobally();

	return 0;
}

// ============================================================================
// FPS overlay & Game Timer commands - self-contained in Phobos.cpp
// ============================================================================

static bool s_FPSEnabled = false;       // toggled by "New FPS Counter" hotkey
static bool s_GameTimerEnabled = true;   // toggled by "Toggle Game Timer" hotkey

// Target frame rate for each GameOptionsClass::GameSpeed value (0 = fastest .. 6 = slowest).
// Index 0 is displayed as "MAX" rather than a numeric FPS.
static constexpr int GameSpeedFPS[7] = { 60, 60, 30, 20, 15, 12, 10 };

// ----------------------------------------------------------------------------
// FPS toggle command - appears in Options -> Keyboard -> Interface
// ----------------------------------------------------------------------------
class ToggleFPSCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override
	{
		return "New FPS Counter";
	}

	virtual const wchar_t* GetUIName() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_FPS_COUNTER", L"New FPS Counter");
	}

	virtual const wchar_t* GetUICategory() const override
	{
		return StringTable::LoadString(GameStrings::TXT_INTERFACE);
	}

	virtual const wchar_t* GetUIDescription() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_FPS_COUNTER_DESC",
			L"Toggle FPS display (shows current and average frame rate).");
	}

	virtual void Execute(WWKey eInput) const override
	{
		s_FPSEnabled = !s_FPSEnabled;
	}
};

// ----------------------------------------------------------------------------
// Game Timer toggle command - appears in Options -> Keyboard -> Interface
// ----------------------------------------------------------------------------
class ToggleGameTimerCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override
	{
		return "Toggle Game Timer";
	}

	virtual const wchar_t* GetUIName() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_GAME_TIMER", L"Toggle Game Timer");
	}

	virtual const wchar_t* GetUICategory() const override
	{
		return StringTable::LoadString(GameStrings::TXT_INTERFACE);
	}

	virtual const wchar_t* GetUIDescription() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_GAME_TIMER_DESC",
			L"Toggle operation time elapsed display.");
	}

	virtual void Execute(WWKey eInput) const override
	{
		s_GameTimerEnabled = !s_GameTimerEnabled;
	}
};

// ----------------------------------------------------------------------------
// Cinema Mode toggle command - appears in Options -> Keyboard -> Interface
// Hides all HUD panels (sidebar, radar, message list, overlays) for a
// cinematic view. The mouse cursor and unit indicators are kept visible.
// ----------------------------------------------------------------------------
class ToggleCinemaModeCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override
	{
		return "Cinema Mode";
	}

	virtual const wchar_t* GetUIName() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_CINEMA_MODE", L"Cinema Mode");
	}

	virtual const wchar_t* GetUICategory() const override
	{
		return StringTable::LoadString(GameStrings::TXT_INTERFACE);
	}

	virtual const wchar_t* GetUIDescription() const override
	{
		return GeneralUtils::LoadStringUnlessMissing("TXT_CINEMA_MODE_DESC",
			L"Hide all UI panels for a cinematic view.");
	}

	virtual void Execute(WWKey eInput) const override
	{
		Phobos::UI::CinemaMode = !Phobos::UI::CinemaMode;
	}
};

// ----------------------------------------------------------------------------
// Register both commands. Syringe chains multiple hooks at the same address,
// so this runs alongside CommandClassCallback_Register in Commands.cpp.
// ----------------------------------------------------------------------------
DEFINE_HOOK(0x533066, CommandClassCallback_RegisterFPSCommand, 0x6)
{
	CommandClass::Array.AddItem(GameCreate<ToggleFPSCommandClass>());
	CommandClass::Array.AddItem(GameCreate<ToggleGameTimerCommandClass>());
	CommandClass::Array.AddItem(GameCreate<ToggleCinemaModeCommandClass>());
	return 0;
}

// ----------------------------------------------------------------------------
// Cinema Mode: skip MessageListClass::Draw (0x5D49A0 -> 0x5D4A9E ret) to hide
// the top-left battle message list.
// ----------------------------------------------------------------------------
DEFINE_HOOK(0x5D49A0, MessageListClass_Draw_CinemaMode, 0x6)
{
	return Phobos::UI::CinemaMode ? 0x5D4A9E : 0;
}

// ----------------------------------------------------------------------------
// Cinema Mode: skip TacticalClass::DrawTimer (0x6D4B50 -> 0x6D4DAC ret 0x10)
// to hide all superweapon countdown timers. Covers both native ShowTimers
// loops and the Charge.FullTimer extra timers (all call DrawTimer).
// Chains with the existing PrintTimerOnTactical_Start hook at the same address.
// ----------------------------------------------------------------------------
DEFINE_HOOK(0x6D4B50, TacticalClass_DrawTimer_CinemaMode, 0x6)
{
	return Phobos::UI::CinemaMode ? 0x6D4DAC : 0;
}

// ----------------------------------------------------------------------------
// Main drawing hook - timer (top-right) + filter buttons + FPS (bottom-left)
// ----------------------------------------------------------------------------
DEFINE_HOOK(0x4F4583, GScreenClass_DrawText, 0x6)
{
	if (Phobos::UI::CinemaMode)
		return 0;

	const int marginX = Phobos::Config::MessageDisplayInCenter ? 28 : 10;
	int coordY = 0;

#ifndef IS_RELEASE_VER
#ifndef STR_GIT_COMMIT
	if (!HideWarning)
#endif // !STR_GIT_COMMIT
	{
		auto wanted = Drawing::GetTextDimensions(Phobos::VersionDescription, { 0, 0 }, 0, 2, 0);

		RectangleStruct rect = {
			DSurface::Composite->GetWidth() - wanted.Width - marginX,
			0,
			wanted.Width + 10,
			wanted.Height + 10
		};

		Point2D location { rect.X + 5, 5 };

		DSurface::Composite->FillRect(&rect, COLOR_BLACK);
		DSurface::Composite->DrawText(Phobos::VersionDescription, &location, COLOR_RED);

	// add margin for next text
		coordY = rect.Height;
	}
#endif // !IS_RELEASE_VER

	// === Operation Time Elapsed (real-world time) ===
	// Controlled by "Toggle Game Timer" hotkey instead of Phobos::Config::ShowGameTime.
	if (s_GameTimerEnabled && !HouseClass::CurrentPlayer->IsObserver())
	{
		wchar_t buffer[0x20] {};

		const auto& timer = ScenarioClass::Instance->ElapsedTimer;
		int realTime = timer.TimeLeft;

		if (timer.StartTime != -1)
			realTime += SystemTimer::GetTime() - timer.StartTime;

		realTime /= 60;  // convert from 60Hz ticks to seconds
		const int hours = realTime / 3600;
		const int minutes = (realTime / 60) % 60;
		const int seconds = realTime % 60;
		const auto text = GeneralUtils::LoadStringUnlessMissing("TXT_GAMETIME", L"Operation Time Elapsed:");

		if (hours > 0)
		{
			swprintf(buffer, std::size(buffer), L"%ls %d:%02d:%02d", text, hours, minutes, seconds);
		}
		else
		{
			swprintf(buffer, std::size(buffer), L"%ls %02d:%02d", text, minutes, seconds);
		}

		auto wantedB = Drawing::GetTextDimensions(buffer, { 0, 0 }, 0, 2, 0);

		RectangleStruct rectB = {
			DSurface::Composite->GetWidth() - wantedB.Width - marginX,
			coordY,
			wantedB.Width + 10,
			wantedB.Height + 10
		};

		Point2D locationB { rectB.X + 5, rectB.Y + 5 };
		ColorStruct color { 0x0, 0x0, 0x0 };
		DSurface::Composite->FillRectTrans(&rectB, &color, Phobos::Config::ShowGameTime_BoardOpacity);
		DSurface::Composite->DrawText(buffer, &locationB, COLOR_WHITE);

		// Stack the game speed display right below the timer.
		coordY = rectB.Y + rectB.Height;
	}

	// === Current Game Speed (top-right, only when TacticalGear is enabled) ===
	if (Phobos::Config::TacticalGear)
	{
		const auto text = GeneralUtils::LoadStringUnlessMissing("TXT_GAME_SPEED", L"Current Game Speed:");

		int speed = GameOptionsClass::Instance.GameSpeed;
		if (speed < 0) speed = 0;
		if (speed > 6) speed = 6;

		wchar_t speedBuffer[0x40] {};
		const int gear = 6 - speed;
		if (speed == 0)
			swprintf(speedBuffer, std::size(speedBuffer), L"%ls %d (MAX)", text, gear);
		else
			swprintf(speedBuffer, std::size(speedBuffer), L"%ls %d (%d FPS)", text, gear, GameSpeedFPS[speed]);

		auto wanted = Drawing::GetTextDimensions(speedBuffer, { 0, 0 }, 0, 2, 0);

		RectangleStruct rect = {
			DSurface::Composite->GetWidth() - wanted.Width - marginX,
			coordY,
			wanted.Width + 10,
			wanted.Height + 10
		};

		Point2D location { rect.X + 5, rect.Y + 5 };
		ColorStruct color { 0x0, 0x0, 0x0 };
		DSurface::Composite->FillRectTrans(&rect, &color, Phobos::Config::ShowGameTime_BoardOpacity);
		DSurface::Composite->DrawText(speedBuffer, &location, COLOR_WHITE);
	}

	// === Filter button background + FPS overlay (bottom-left) ===
	// Draw the semi-transparent background box behind filter buttons.
	FilterButtonClass::DrawBackground();

	// Draw FPS counter above the background box when enabled.
	// Left-aligned: current FPS on the left, average FPS ("Avg.") on the right.
	if (s_FPSEnabled)
	{
		wchar_t fpsBuffer[0x40] {};
		const unsigned int currentFPS = FPSCounter::CurrentFrameRate;
		const double averageFPS = FPSCounter::GetAverageFrameRate();
		swprintf(fpsBuffer, std::size(fpsBuffer), L"FPS: %d   Avg: %.1f",
			static_cast<int>(currentFPS), averageFPS);

		const auto pSurface = DSurface::Composite;
		auto bounds = pSurface->GetRect();
		auto textSize = Drawing::GetTextDimensions(fpsBuffer, { 0, 0 }, 0, 2, 0);

		// Geometry matches FilterButtonClass::InitButtons / DrawBackground
		constexpr int startX = 5;
		constexpr int padding = 4;
		const int bgX = startX - padding;
		const int bgY = pSurface->GetHeight() - 65 - padding;

		// Left-aligned position above background box
		const int fpsX = bgX + 3;  // 3px left padding
		const int fpsY = bgY - textSize.Height - 2;

		// Semi-transparent background for FPS text
		ColorStruct fpsBlack { 0x0, 0x0, 0x0 };
		RectangleStruct fpsRect = {
			fpsX - 3,
			fpsY - 1,
			textSize.Width + 6,
			textSize.Height + 2
		};
		pSurface->FillRectTrans(&fpsRect, &fpsBlack, 40);

		// Draw FPS text left-aligned (no Center flag)
		Point2D fpsPos { fpsX, fpsY };
		const COLORREF textColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 255));
		constexpr TextPrintType printType =
			TextPrintType::FullShadow | TextPrintType::Point8 |
			TextPrintType::Background;
		pSurface->DrawTextA(fpsBuffer, &bounds, &fpsPos, textColor, 0, printType);
	}

	return 0;
}

// Mainly used to disable hooks for optimization.
// Called after loading saved game and at end of scenario start after all INI data etc has been initialized.
// Only executed once per game session.
void Phobos::ApplyOptimizations()
{
	if (Phobos::Optimizations::Applied)
		return;

	// Disable BuildingClass_AI_Radiation
	if (Phobos::Optimizations::DisableRadDamageOnBuildings)
		Patch::Apply_RAW(0x43FB23, { 0x53, 0x55, 0x56, 0x8B, 0xF1 });

	if (!SessionClass::IsMultiplayer())
	{
		// Disable Random2Class_Random_SyncLog
		Patch::Apply_RAW(0x65C7D0, { 0xC3, 0x90, 0x90, 0x90, 0x90 });

		// Disable Random2Class_RandomRanged_SyncLog
		Patch::Apply_RAW(0x65C88A, { 0xC2, 0x08, 0x00, 0x90, 0x90 });

		// Disable FacingClass_Set_SyncLog
		Patch::Apply_RAW(0x4C9300, { 0x83, 0xEC, 0x10, 0x53, 0x56 });

		// Disable InfantryClass_AssignTarget_SyncLog
		Patch::Apply_RAW(0x51B1F0, { 0x53, 0x56, 0x8B, 0xF1, 0x57 });

		// Disable BuildingClass_AssignTarget_SyncLog
		Patch::Apply_RAW(0x443B90, { 0x56, 0x8B, 0xF1, 0x57, 0x83, 0xBE, 0xAC, 0x0, 0x0, 0x0, 0x13 });

		// Disable TechnoClass_AssignTarget_SyncLog
		Patch::Apply_RAW(0x6FCDB0, { 0x83, 0xEC, 0x0C, 0x53, 0x56 });

		// Disable AircraftClass_AssignDestination_SyncLog
		Patch::Apply_RAW(0x41AA80, { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 });

		// Disable BuildingClass_AssignDestination_SyncLog
		Patch::Apply_RAW(0x455D50, { 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xAC, 0x0, 0x0, 0x0, 0x13 });

		// Disable InfantryClass_AssignDestination_SyncLog
		Patch::Apply_RAW(0x51AA40, { 0x83, 0xEC, 0x2C, 0x53, 0x55 });

		// Disable UnitClass_AssignDestination_SyncLog
		Patch::Apply_RAW(0x741970, { 0x81, 0xEC, 0x80, 0x0, 0x0, 0x00 });

		// Disable AircraftClass_OverrideMission_SyncLog
		Patch::Apply_RAW(0x41BB30, { 0x8B, 0x81, 0xAC, 0x0, 0x0, 0x0 });

		// Disable FootClass_OverrideMission_SyncLog
		Patch::Apply_RAW(0x4D8F40, { 0x8B, 0x54, 0x24, 0x4, 0x56 });

		// Disable TechnoClass_OverrideMission_SyncLog
		Patch::Apply_RAW(0x7013A0, { 0x8B, 0x54, 0x24, 0x4, 0x56 });

		Phobos::Optimizations::DisableSyncLogging = true;
	}

	Phobos::Optimizations::Applied = true;
}
