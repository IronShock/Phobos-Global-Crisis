#pragma once

struct RectangleStruct;
class DSurface;
class RadarClass;

// Custom minimap markers configured per TechnoType with RadarSignal=yes.
//
// The marker position is derived from the object's own coordinates through the
// engine's radar projection (RadarClass::GetCrdOnRadar). The engine's radar
// tracking table is deliberately not used: it stores one entry per footprint
// pixel, and adjacent buildings can share/reassign pixels, which makes a
// per-object centroid jump around between rebuilds.
//
// RadarSignal.Image is used on both the minimap (native size) and the
// full-screen tactical map (scaled up to the same proportion).
namespace RadarSignal
{
	// Draws the minimap markers onto pSurface, clamped to radarRect (the radar
	// image rectangle on that surface) so markers at the edge stay fully visible.
	void DrawMarkers(DSurface* pSurface, const RectangleStruct& radarRect);

	// Sidebar surface, called right after the radar image has been blitted and
	// before the engine draws the camera view box. The tactical map enlarges this
	// same sidebar rectangle, so the markers scale with it automatically.
	void DrawToSidebar(RadarClass* pRadar);
}
