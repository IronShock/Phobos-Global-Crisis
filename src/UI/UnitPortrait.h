#pragma once

#include <Windows.h>
#include <GeneralDefinitions.h>
#include <vector>

class TechnoClass;

struct PortraitImage
{
	int Width = 0;
	int Height = 0;
	std::vector<COLORREF> Pixels;
	// Opaque mask. A pixel can be opaque and still be pure black, so the
	// colour value alone cannot be used as the transparency key.
	std::vector<BYTE> Mask;
	// Per-pixel alpha for the optional drop shadow (0 = no shadow,
	// 255 = fully opaque). The shadow is drawn underneath the body.
	std::vector<BYTE> Shadow;

	// Bounds of the body and shadow inside the full square portrait canvas.
	// Cached here so multi-selection layout does not rescan every frame.
	RectangleStruct ContentBounds {};

	bool Empty() const
	{
		return Width <= 0 || Height <= 0 || Pixels.empty() || Mask.empty();
	}
};

// Renders and caches a grayscale model portrait for the selected Techno.
//
// SHP units are drawn with the engine's DSurface path, with the unit's own
// drawer palette and the south-east facing frame. Voxel units use the engine's
// rendered voxel cache. Pixels are kept in a plain vector because BSurface's
// GetPixel/SetPixel path is not reliable for programmatically built images.
class UnitPortrait
{
public:
	// Returns a cached portrait of exactly width x height, or nullptr.
	// The returned image's Mask marks which pixels are opaque; a zero colour
	// value is not a transparency marker.
	static const PortraitImage* Get(TechnoClass* pThis, int width, int height);

	// Returns only an already cached image; never triggers a SHP/voxel render.
	static const PortraitImage* TryGetCached(TechnoClass* pThis, int width, int height);

	// Drops all cached portraits (scenario load / resolution change).
	static void Clear();
};
