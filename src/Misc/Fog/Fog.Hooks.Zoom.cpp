#include "Fog.h"

// TacticalClass::RegisterCellAsVisible culls cells against DSurface::ViewBounds,
// which is the window viewport (A). While the zoom manager renders a larger
// offscreen viewport (R, TacticalClass::view_bound), any cell outside A is
// dropped from the visible-cell list, so its fog/shroud update never reaches the
// zoom-out surface. Swap the real viewport in for the duration of the call.
namespace
{
	RectangleStruct SavedViewBounds;
}

DEFINE_HOOK(0x6DA7D0, Fog_RegisterCellAsVisible_ZoomViewport_Enter, 0x5)
{
	SavedViewBounds = DSurface::ViewBounds;

	if (TacticalClass::Instance)
	{
		auto const& view = *reinterpret_cast<RectangleStruct*>(0xB0CE28u);

		if (view.Width >= 16 && view.Height >= 16)
			DSurface::ViewBounds = view;
	}

	return 0; // replay `mov eax, [0x87F7F4]`
}

DEFINE_HOOK(0x6DA96F, Fog_RegisterCellAsVisible_ZoomViewport_Exit, 0x6)
{
	DSurface::ViewBounds = SavedViewBounds;
	return 0; // replay `pop edi; pop esi; pop ebx; add esp, 0xC`
}
