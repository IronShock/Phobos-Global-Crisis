#pragma once

#include <BasicStructures.h>

#include <string>
#include <vector>

#include <windows.h>

class DSurface;

// Small GDI text rasteriser used by the tactical map annotations. Rendering
// through GDI is what makes CJK input possible: the game's bitmap font only has a
// few fixed sizes, while GDI can draw any size and any installed glyph.
struct GdiTextBitmap
{
	int Width = 0;
	int Height = 0;
	std::vector<DWORD> Pixels;   // 0xAARRGGBB, 0 = transparent
};

namespace TacticalMapGdi
{
	// Renders `text` at `pixelHeight` with a 1px drop shadow. Non-antialiased on
	// purpose: it keeps the key-colour transparency clean and the glyphs crisp.
	bool Render(const std::wstring& text, int pixelHeight, unsigned rgb, GdiTextBitmap& out);

	// Copies a rendered bitmap onto the surface, clipped to `clip`.
	void Blit(DSurface* pSurface, const GdiTextBitmap& bitmap, int x, int y, const RectangleStruct& clip);
}
