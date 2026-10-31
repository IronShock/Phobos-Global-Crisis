#include "TacticalMap.GdiText.h"

#include <BasicStructures.h>
#include <Surface.h>

#include <algorithm>
#include <cstring>

namespace
{
	// Key colour used for transparency. Deliberately obscure so that no palette
	// colour (including pink/magenta) can collide with it.
	constexpr DWORD KeyColor = 0x00010203;

	inline int To565(DWORD c)
	{
		const int r = static_cast<int>((c >> 16) & 0xFF);
		const int g = static_cast<int>((c >> 8) & 0xFF);
		const int b = static_cast<int>(c & 0xFF);

		return ((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255);
	}
}

bool TacticalMapGdi::Render(const std::wstring& text, int pixelHeight, unsigned rgb, GdiTextBitmap& out)
{
	out.Pixels.clear();
	out.Width = 0;
	out.Height = 0;

	if (text.empty() || pixelHeight < 6)
		return false;

	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	ReleaseDC(nullptr, screen);

	if (!dc)
		return false;

	HFONT font = CreateFontW(
		-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

	if (!font)
	{
		DeleteDC(dc);
		return false;
	}

	HGDIOBJ oldFont = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);

	SIZE size {};
	GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);

	if (size.cx <= 0 || size.cy <= 0)
	{
		SelectObject(dc, oldFont);
		DeleteObject(font);
		DeleteDC(dc);
		return false;
	}

	const int width = size.cx + 3;
	const int height = size.cy + 3;

	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height; // top-down
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

	if (!bitmap || !bits)
	{
		SelectObject(dc, oldFont);
		DeleteObject(font);
		DeleteDC(dc);
		return false;
	}

	HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

	DWORD* pixels = static_cast<DWORD*>(bits);
	std::fill(pixels, pixels + static_cast<size_t>(width) * height, KeyColor);

	// Drop shadow first, then the body.
	SetTextColor(dc, RGB(0, 0, 0));
	TextOutW(dc, 2, 2, text.c_str(), static_cast<int>(text.size()));

	SetTextColor(dc, RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
	TextOutW(dc, 0, 0, text.c_str(), static_cast<int>(text.size()));

	out.Width = width;
	out.Height = height;
	out.Pixels.resize(static_cast<size_t>(width) * height);

	for (size_t i = 0; i < out.Pixels.size(); ++i)
	{
		const DWORD c = pixels[i];
		out.Pixels[i] = ((c & 0x00FFFFFF) == KeyColor) ? 0u : (0xFF000000u | (c & 0x00FFFFFFu));
	}

	SelectObject(dc, oldBitmap);
	DeleteObject(bitmap);
	SelectObject(dc, oldFont);
	DeleteObject(font);
	DeleteDC(dc);

	return true;
}

void TacticalMapGdi::Blit(DSurface* pSurface, const GdiTextBitmap& bitmap, int x, int y, const RectangleStruct& clip)
{
	if (!pSurface || bitmap.Pixels.empty())
		return;

	const int bpp = pSurface->GetBytesPerPixel();

	if (bpp != 2 && bpp != 4)
		return;

	const RectangleStruct surfaceRect = pSurface->GetRect();

	const int x0 = std::max({ x, clip.X, 0 });
	const int y0 = std::max({ y, clip.Y, 0 });
	const int x1 = std::min({ x + bitmap.Width, clip.X + clip.Width, surfaceRect.Width });
	const int y1 = std::min({ y + bitmap.Height, clip.Y + clip.Height, surfaceRect.Height });

	if (x0 >= x1 || y0 >= y1)
		return;

	BYTE* const base = static_cast<BYTE*>(pSurface->Lock(0, 0));

	if (!base)
		return;

	const int pitch = pSurface->GetPitch();

	for (int py = y0; py < y1; ++py)
	{
		const DWORD* const src = bitmap.Pixels.data() + static_cast<size_t>(py - y) * bitmap.Width;
		BYTE* const row = base + static_cast<size_t>(py) * pitch;

		if (bpp == 2)
		{
			WORD* const dst = reinterpret_cast<WORD*>(row);

			for (int px = x0; px < x1; ++px)
			{
				const DWORD c = src[px - x];

				if (!c)
					continue;

				dst[px] = static_cast<WORD>(To565(c));
			}
		}
		else
		{
			DWORD* const dst = reinterpret_cast<DWORD*>(row);

			for (int px = x0; px < x1; ++px)
			{
				const DWORD c = src[px - x];

				if (!c)
					continue;

				dst[px] = (dst[px] & 0xFF000000u) | (c & 0x00FFFFFFu);
			}
		}
	}

	pSurface->Unlock();
}
