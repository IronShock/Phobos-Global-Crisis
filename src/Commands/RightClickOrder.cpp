#include "RightClickOrder.h"

#include <Phobos.h>

#include <DisplayClass.h>
#include <GeneralDefinitions.h>
#include <Surface.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <Windows.h>

namespace RightClickOrder
{
	// Defaults to allowing commands so that code paths which never pass through the
	// tactical message handler (e.g. radar clicks) keep working.
	bool IsRightButtonEvent = true;

	bool RightDragAnchorInit = false;
	bool RightDragScrolled = false;
}

// --- Tactical map mouse message handler ---------------------------------------
//
// The tactical mouse handler starts at 0x6930A0 (ScrollClass::MessageHandler),
// called from the window procedure (0x777640) with ecx = DisplayClass::Instance
// and a pointer to the window message id as its 2nd argument. It switches on
// that message id:
//   WM_LBUTTONDOWN (0x201) -> selection / command start
//   WM_LBUTTONUP   (0x202) -> order / selection finalize
//   WM_RBUTTONDOWN (0x204) -> cancel
//   WM_RBUTTONUP   (0x205) -> cancel
//
// 0x6930A0 is already hooked by ZoomManager's middle-click reset, so we hook the
// free 5-byte instruction right before the message switch instead (after the
// handler's prologue, esi = this and the message pointer is at [esp+0x2C]).
//
// When the option is enabled we rewrite right-button messages into left-button
// ones so the stock left-click order path runs for the right button, and record
// the true origin so the command can subsequently be suppressed for the left
// button. Building placement / superweapon targeting / repair / sell / power /
// planning modes are left untouched (stock left-confirm, right-cancel).
DEFINE_HOOK(0x6930EE, DisplayClass_TacticalMsg_RightClickRemap, 0x5)
{
	if (!Phobos::Config::RightClickCommands)
		return 0;

	GET(const DisplayClass*, pThis, ESI);
	UINT* const pMsg = R->Stack<UINT*>(0x2C);

	if (!pThis || !pMsg)
		return 0;

	// Clicks on the build sidebar keep their vanilla behaviour: the sidebar's own
	// handlers must receive the real right button, otherwise the remapped left
	// button would start construction on a right click. Skip the remap there.
	// The 4th argument points at the window message's lParam, where X is the low
	// 16 bits and Y the high 16 bits - not at a Point2D.
	if (const short* const pPos = R->Stack<short*>(0x34))
	{
		const int x = pPos[0];
		const int y = pPos[1];
		const auto& sidebar = DSurface::SidebarBounds;

		if (x >= sidebar.X && x < sidebar.X + sidebar.Width
			&& y >= sidebar.Y && y < sidebar.Y + sidebar.Height)
		{
			return 0;
		}
	}

	const bool specialMode =
		pThis->RepairMode || pThis->SellMode || pThis->PowerToggleMode ||
		pThis->PlanningMode || pThis->PlaceBeaconMode ||
		pThis->CurrentSWTypeIndex != -1 || pThis->CurrentBuilding != nullptr;

	if (specialMode)
		return 0;

	switch (*pMsg)
	{
	case WM_RBUTTONDOWN:
		RightClickOrder::RightDragAnchorInit = false;
		RightClickOrder::RightDragScrolled = false;
		*pMsg = WM_LBUTTONDOWN;
		RightClickOrder::IsRightButtonEvent = true;
		break;
	case WM_RBUTTONUP:
		// If the right button was used to scroll, do not turn the release into an
		// order (the units would move to wherever the camera ended up).
		if (RightClickOrder::RightDragScrolled)
		{
			RightClickOrder::RightDragScrolled = false;
			RightClickOrder::IsRightButtonEvent = true;
			break;
		}

		*pMsg = WM_LBUTTONUP;
		RightClickOrder::IsRightButtonEvent = true;
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		RightClickOrder::IsRightButtonEvent = false;
		break;
	default:
		break;
	}

	return 0;
}

// --- Right-button drag-scroll --------------------------------------------------
//
// The tactical mouse-move handler (0x692F30) picks a drag mode from the PHYSICAL
// button state (0x54F5C0), not the remapped message id: left button -> rubber-band
// selection (0x4AC380), right button -> 0x693440, which pans the view. 0x693440
// measures the pointer movement against [this+0x5550/0x5554] (the drag anchor) and
// only starts scrolling after the system drag threshold is exceeded.
//
// With the option on, the right button's own message branch is skipped, so the
// anchor was never recorded and stayed 0; that made the first move look like a huge
// drag and panned the view towards the bottom-right. We record the anchor from the
// first move frame, after which the stock threshold logic behaves correctly: a
// click (movement below the threshold) does not scroll, a real drag does.

DEFINE_HOOK(0x693440, ScrollClass_RightDrag_SetAnchor, 0x5)
{
	if (!Phobos::Config::RightClickCommands)
		return 0;

	GET(char*, pThis, ECX);
	Point2D* const pPoint = R->Stack<Point2D*>(0x4);

	if (!RightClickOrder::RightDragAnchorInit && pPoint)
	{
		*reinterpret_cast<int*>(pThis + 0x5550) = pPoint->X;
		*reinterpret_cast<int*>(pThis + 0x5554) = pPoint->Y;
		*(pThis + 0x5558) = 0;
		*(pThis + 0x554C) = 0;
		RightClickOrder::RightDragAnchorInit = true;
	}

	return 0;
}

// 0x6934B6 is where the game marks "right-drag in progress" after the threshold.
// Remember it so the release that ends a scroll-drag does not issue an order.
DEFINE_HOOK(0x6934B6, ScrollClass_RightDrag_MarkScrolled, 0x7)
{
	if (Phobos::Config::RightClickCommands)
		RightClickOrder::RightDragScrolled = true;

	return 0;
}

// --- Command dispatch ---------------------------------------------------------
//
// DisplayClass::ActiveClickWith(target, cell, action) applies a click action to
// every object in ObjectClass::CurrentObjects. When the option is enabled and the
// click originated from the physical left button, non-selection actions are
// skipped so the left button only selects. Selection actions are still allowed,
// and the right button (remapped above) issues the full set of commands.
static void __declspec(naked) ActiveClickWith_SkipThunk()
{
	_asm { xor eax, eax }
	_asm { ret 0xC }
}

DEFINE_HOOK(0x4AE750, DisplayClass_ActiveClickWith_LeftSelectOnly, 0x8)
{
	if (Phobos::Config::RightClickCommands && !RightClickOrder::IsRightButtonEvent)
	{
		// Consume the marker so it cannot leak into unrelated calls (radar
		// clicks, scripted actions, ...) that don't set it themselves.
		RightClickOrder::IsRightButtonEvent = true;

		const Action action = R->Stack<Action>(0xC);

		if (action != Action::Select && action != Action::ToggleSelect)
		{
			// Left-clicking empty ground clears the selection, matching the
			// select-only control scheme. A Move / NoMove / None action with no
			// target object is exactly that empty-ground click.
			if (!R->Stack<void*>(0x4)
				&& (action == Action::Move || action == Action::NoMove || action == Action::None))
			{
				// Vanilla deselect-all routine (iterates CurrentObjects, calling
				// ObjectClass::Deselect on each) used when a single unit is picked.
				reinterpret_cast<void(__cdecl*)()>(0x48DC90)();
			}

			if (Phobos::Config::RightClickCommands_Debug)
				Debug::Log("[RightClickOrder] Left-click action %d suppressed (select-only).\n", static_cast<int>(action));

			return reinterpret_cast<uintptr_t>(&ActiveClickWith_SkipThunk);
		}
	}

	RightClickOrder::IsRightButtonEvent = true;

	return 0;
}
