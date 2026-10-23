#pragma once

#include <BasicStructures.h>

#include <utility>
#include <vector>

class DSurface;

// Freehand annotation layer for the tactical map.
//
// The bitmap is kept at a fixed resolution in map-normalised space, so strokes
// stay glued to the same spot on the map when the overlay is closed/reopened or
// the resolution changes. Erasing clears pixels, which is why a raster mask is
// used instead of vector polylines.
class TacticalMapCanvas
{
public:
	static constexpr int Size = 1024;

	static void Reset();
	static bool HasContent();

	// Mask-space coordinates, radius in mask pixels, colour as 0xRRGGBB.
	static void BeginStroke(int x, int y, int radius, unsigned rgb, bool erase);
	static void ContinueStroke(int x, int y);
	static void EndStroke();

	// Undo support: each stroke records the pixels it overwrote.
	static bool TakeStrokeEdits(std::vector<std::pair<int, DWORD>>& out);
	static void ApplyEdits(const std::vector<std::pair<int, DWORD>>& edits);
	static void Save(std::vector<DWORD>& out);
	static void Restore(const std::vector<DWORD>& in);

	// Composites the painted pixels onto `dst`. `dst` is the on-screen rectangle of
	// the minimap image; the mask covers it one-to-one.
	static void Render(DSurface* pSurface, const RectangleStruct& dst);
};
