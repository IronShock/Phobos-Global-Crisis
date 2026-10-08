#include "ReverseMove.h"

#include <Utilities/GeneralUtils.h>
#include <Utilities/Macro.h>
#include <CommandClass.h>
#include <Unsorted.h>

const char* ReverseMoveCommandClass::GetName() const
{
	return "ReverseMove";
}

const wchar_t* ReverseMoveCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_REVERSE_MOVE", L"Reverse Move");
}

const wchar_t* ReverseMoveCommandClass::GetUICategory() const
{
	return CATEGORY_CONTROL;
}

const wchar_t* ReverseMoveCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_REVERSE_MOVE_DESC", L"Hold this key while commanding units to move in reverse.");
}

void ReverseMoveCommandClass::Execute(WWKey eInput) const
{
	// Discover the bound virtual key code by looking up this command
	// in the Hotkeys table. The lower byte of the ID is the VK code.
	for (auto const& node : CommandClass::Hotkeys)
	{
		if (node.Data == this)
		{
			BoundVK = node.ID & 0xFF;
			break;
		}
	}

	IsHeld = true;
}

void ReverseMoveCommandClass::UpdateHeldState()
{
	if (BoundVK == 0)
		return;

	// Query the actual physical key state from Windows.
	// The high bit (0x8000) indicates the key is currently down.
	SHORT state = GetAsyncKeyState(BoundVK);
	IsHeld = (state & 0x8000) != 0;

	if (IsHeld)
		LastHeldFrame = Unsorted::CurrentFrame;
}

bool ReverseMoveCommandClass::IsEffectivelyHeld(int graceFrames)
{
	return IsHeld
		|| (LastHeldFrame >= 0 && Unsorted::CurrentFrame - LastHeldFrame <= graceFrames);
}

// Poll the physical key state once per game frame so IsHeld / LastHeldFrame
// stay accurate even when no reverse-capable unit is currently moving (which
// is where UpdateHeldState used to be called from). Runs every logic frame.
DEFINE_HOOK(0x55B4E1, ReverseMove_PerFrame, 0x5)
{
	ReverseMoveCommandClass::UpdateHeldState();
	return 0;
}
