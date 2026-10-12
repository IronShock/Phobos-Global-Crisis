#include "Commands.h"

#include "ObjectInfo.h"
#include "NextIdleHarvester.h"
#include "DamageDisplay.h"
#include "FrameByFrame.h"
#include "FrameStep.h"
#include "ToggleDigitalDisplay.h"
#include "ToggleDesignatorRange.h"
#include "ReverseMove.h"
#include "AutoReverseToggle.h"
#include "TacticalGear.h"
#include "SaveVariablesToFile.h"
#include "ToggleSWSidebar.h"
#include "FireTacticalSW.h"
#include "ToggleMessageList.h"
#include "TacticalPause.h"
#include "ZoomCommands.h"
#include <New/Entity/FilterButtonClass.h>

#include <CCINIClass.h>

#include <Utilities/Macro.h>
#include <Ext/Sidebar/SWSidebar/SWSidebarClass.h>
#include <Misc/MessageColumn.h>
#include <Misc/ZoomManager.h>

DEFINE_HOOK(0x533066, CommandClassCallback_Register, 0x6)
{
	// Load it after Ares'

	if (ZoomManager::Enabled && ZoomManager::KeyEnabled)
	{
		MakeCommand<ZoomInCommandClass>();
		MakeCommand<ZoomOutCommandClass>();
		MakeCommand<ResetZoomCommandClass>();
	}

	MakeCommand<NextIdleHarvesterCommandClass>();
	MakeCommand<ToggleDigitalDisplayCommandClass>();
	MakeCommand<ToggleDesignatorRangeCommandClass>();
	MakeCommand<ReverseMoveCommandClass>();
	MakeCommand<AutoReverseToggleCommandClass>();
	if (Phobos::Config::TacticalGear)
	{
		MakeCommand<TacticalGearCommandClass<+1>>(); // Slower (downshift)
		MakeCommand<TacticalGearCommandClass<-1>>(); // Faster (upshift)
	}
	MakeCommand<ToggleMessageListCommandClass>();
	MakeCommand<ToggleSWSidebar>();

	if (Phobos::Config::TacticalPause)
		MakeCommand<TacticalPauseCommandClass>();

	// Target Filter hotkeys (Alt+Q/W/E/R/T/Y by default in keyboard.ini)
	MakeCommand<FilterHotkeyCommandClass<0>>();
	MakeCommand<FilterHotkeyCommandClass<1>>();
	MakeCommand<FilterHotkeyCommandClass<2>>();
	MakeCommand<FilterHotkeyCommandClass<3>>();
	MakeCommand<FilterHotkeyCommandClass<4>>();
	MakeCommand<FilterHotkeyCommandClass<5>>();

	if (Phobos::Config::SuperWeaponSidebarCommands)
	{
		SWSidebarClass::Commands[0] = MakeCommand<FireTacticalSWCommandClass<0>>();
		SWSidebarClass::Commands[1] = MakeCommand<FireTacticalSWCommandClass<1>>();
		SWSidebarClass::Commands[2] = MakeCommand<FireTacticalSWCommandClass<2>>();
		SWSidebarClass::Commands[3] = MakeCommand<FireTacticalSWCommandClass<3>>();
		SWSidebarClass::Commands[4] = MakeCommand<FireTacticalSWCommandClass<4>>();
		SWSidebarClass::Commands[5] = MakeCommand<FireTacticalSWCommandClass<5>>();
		SWSidebarClass::Commands[6] = MakeCommand<FireTacticalSWCommandClass<6>>();
		SWSidebarClass::Commands[7] = MakeCommand<FireTacticalSWCommandClass<7>>();
		SWSidebarClass::Commands[8] = MakeCommand<FireTacticalSWCommandClass<8>>();
		SWSidebarClass::Commands[9] = MakeCommand<FireTacticalSWCommandClass<9>>();
	}

	if (Phobos::Config::DevelopmentCommands)
	{
		MakeCommand<DamageDisplayCommandClass>();
		MakeCommand<SaveVariablesToFileCommandClass>();
		MakeCommand<ObjectInfoCommandClass>();
		MakeCommand<FrameByFrameCommandClass>();
		MakeCommand<FrameStepCommandClass<1>>(); // Single step in
		MakeCommand<FrameStepCommandClass<5>>(); // Speed 1
		MakeCommand<FrameStepCommandClass<10>>(); // Speed 2
		MakeCommand<FrameStepCommandClass<15>>(); // Speed 3
		MakeCommand<FrameStepCommandClass<30>>(); // Speed 4
		MakeCommand<FrameStepCommandClass<60>>(); // Speed 5
	}

	return 0;
}

static void MouseWheelDownCommand()
{
	if (MessageColumnClass::Instance.IsHovering())
		MessageColumnClass::Instance.ScrollDown();
}

static void MouseWheelUpCommand()
{
	if (MessageColumnClass::Instance.IsHovering())
		MessageColumnClass::Instance.ScrollUp();
}

DEFINE_HOOK(0x777998, Game_WndProc_ScrollMouseWheel, 0x6)
{
	GET(const WPARAM, WParam, ECX);

	if (ZoomManager::CanPlayerZoom() && ZoomManager::ScrollEnabled && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
	{
		if (WParam & 0x80000000u)
			ZoomManager::ZoomOut();
		else
			ZoomManager::ZoomIn();

		return 0;
	}

	if (WParam & 0x80000000u)
		MouseWheelDownCommand();
	else
		MouseWheelUpCommand();

	return 0;
}

static inline bool CheckSkipScrollSidebar()
{
	if (ZoomManager::CanPlayerZoom() && ZoomManager::ScrollEnabled && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
		return true;

	return MessageColumnClass::Instance.IsHovering();
}

DEFINE_HOOK(0x533F50, Game_ScrollSidebar_Skip, 0x5)
{
	enum { SkipScrollSidebar = 0x533FC3 };
	return CheckSkipScrollSidebar() ? SkipScrollSidebar : 0;
}
