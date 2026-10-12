#pragma once

// Mainstream-RTS control scheme implementation ("RightClickCommands").
//
// Vanilla Yuri's Revenge issues orders with the LEFT mouse button
// (DisplayClass::ActiveClickWith applies the click action to every selected
// object) and uses the RIGHT button to cancel. This feature makes the left
// button select only and moves order issuing to the right button, matching
// mainstream RTS games (StarCraft / Warcraft / C&C 3 / Generals).
//
// See RightClickOrder.cpp for the hooks. The switch is read from the [Phobos]
// section of RA2MD.ini (RightClickCommands=yes/no) and is written there by the
// client options panel.
namespace RightClickOrder
{
	// True when the mouse event currently being processed originated from the
	// physical RIGHT button. Set by the tactical message handler hook.
	extern bool IsRightButtonEvent;

	// Right-button drag-scroll bookkeeping. The first move frame (re)records the
	// drag anchor so the vanilla drag threshold works; RightDragScrolled tells the
	// release that a scroll-drag happened and must not issue an order.
	extern bool RightDragAnchorInit;
	extern bool RightDragScrolled;
}
