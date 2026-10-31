#include "TacticalMap.h"

#include <InputManagerClass.h>
#include <RadarClass.h>
#include <Surface.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

// GadgetClass::ExtractGadgetAt (0x4E15A0) is the non-virtual helper that
// GadgetClass::Input uses to find the widget under the mouse (the only caller in
// the executable is 0x4E16D4). Returning null while the tactical map is open
// blocks every HUD widget, including the subclasses that override the virtual
// Clicked, and no Action can be triggered.
//
// It is __thiscall with two stack arguments and returns the gadget in EAX
// (ret 8). The replaced 8 bytes are `sub esp,0x14` and `mov eax,[0x7f5be8]`;
// returning the thunk below skips both and leaves the stack untouched, which is
// fine because it returns immediately.
static void __declspec(naked) TacticalMap_GadgetBlockThunk()
{
	_asm { xor eax, eax }
	_asm { ret 8 }
}

DEFINE_HOOK(0x4E15A0, GadgetClass_ExtractGadgetAt_TacticalMap, 0x8)
{
	if (TacticalMap::Opened)
	{
		// The hit test runs every input frame for every gadget list, so it is also
		// the most reliable place to catch a fresh mouse press (see ProcessGadgetInput).
		GET_STACK(const int, x, 0x4);
		GET_STACK(const int, y, 0x8);

		const Point2D point { x, y };
		TacticalMap::ProcessGadgetInput(point);

		return reinterpret_cast<uintptr_t>(&TacticalMap_GadgetBlockThunk);
	}

	return 0;
}

// Diagnostic only: 0x653E66 is reached on the camera-jump branch right before the
// engine looks the target cell up. [esp+0x18] / [esp+0x1a] already hold the final,
// clamped cell, so logging it next to our own mapping validates the ruler maths.
DEFINE_HOOK(0x653E66, RadarClass_RTacticalClass_Action_LogFinalCell, 0x8)
{
	static int logged = 0;

	if (logged >= 30)
		return 0;

	++logged;

	const short cellX = R->Stack<short>(0x18);
	const short cellY = R->Stack<short>(0x1A);

	const RectangleStruct& rect = RadarClass::Instance.unknown_rect_149C;
	const RectangleStruct sidebar = DSurface::SidebarBounds;

	int mouseX = -1;
	int mouseY = -1;

	if (auto* const pInput = reinterpret_cast<int*>(InputManagerClass::Instance))
	{
		mouseX = pInput[0];
		mouseY = pInput[1];
	}

	int originY = sidebar.Y;

	if (DSurface::Sidebar)
		originY = sidebar.Y + sidebar.Height - DSurface::Sidebar->GetHeight();

	const int radarX = mouseX - sidebar.X - rect.X;
	const int radarY = mouseY - originY - rect.Y;

	Debug::Log("[Phobos::TacticalMap] engine cell: mouse={%d,%d} radarPx={%d,%d} cell={%d,%d}\n",
		mouseX, mouseY, radarX, radarY, cellX, cellY);

	TacticalMap::DebugComparePixel(radarX, radarY);

	return 0;
}
