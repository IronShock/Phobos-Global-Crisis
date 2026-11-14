#pragma once

#include <GeneralDefinitions.h>

// Client-side selected-unit status panel.
//
// The panel is anchored to the bottom-left of the battlefield, directly above
// the vanilla advanced command bar. It owns no serialized state; every value is
// read from the current selection each frame and the only runtime state is the
// click-blocking gadget and the portrait cache (added in a later phase).
class UnitStatusPanel
{
public:
	static bool IsEnabled();

	// Panel rectangle in screen space. Filter buttons and the centre message
	// column use this to avoid overlapping the panel.
	static RectangleStruct GetPanelRect();

	// Called from SidebarClass::InitClear / InitIO together with the other
	// custom HUD gadgets.
	static void InitIO();
	static void InitClear();

	static void Draw();

	// True while the mouse is over the panel. Used by the click-routing hook so
	// clicking the panel never issues an order to the battlefield behind it.
	static bool IsHovering();
};
