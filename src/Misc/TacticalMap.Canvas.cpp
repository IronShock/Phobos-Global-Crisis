#include "TacticalMap.Canvas.h"

#include <Surface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
	constexpr int MaskSize = TacticalMapCanvas::Size;
	constexpr int BlendAlpha = 235;

	std::vector<DWORD> Pixels;
	int DirtyMinX = 1;
	int DirtyMinY = 1;
	int DirtyMaxX = 0;
	int DirtyMaxY = 0;

	int LastX = 0;
	int LastY = 0;
	int Radius = 3;
	unsigned Rgb = 0xFFFFFF;
	bool Erasing = false;
	bool Active = false;
	std::vector<std::pair<int, DWORD>> StrokeEdits;

	inline DWORD* At(int x, int y)
	{
		return &Pixels[static_cast<size_t>(y) * MaskSize + x];
	}

	void MarkDirty(int minX, int minY, int maxX, int maxY)
	{
		if (DirtyMaxX < DirtyMinX)
		{
			DirtyMinX = minX;
			DirtyMinY = minY;
			DirtyMaxX = maxX;
			DirtyMaxY = maxY;
			return;
		}

		DirtyMinX = std::min(DirtyMinX, minX);
		DirtyMinY = std::min(DirtyMinY, minY);
		DirtyMaxX = std::max(DirtyMaxX, maxX);
		DirtyMaxY = std::max(DirtyMaxY, maxY);
	}


	void RecalculateDirty()
	{
		DirtyMinX = 1;
		DirtyMinY = 1;
		DirtyMaxX = 0;
		DirtyMaxY = 0;

		if (Pixels.empty())
			return;

		for (int y = 0; y < MaskSize; ++y)
		{
			const DWORD* const row = Pixels.data() + static_cast<size_t>(y) * MaskSize;

			for (int x = 0; x < MaskSize; ++x)
			{
				if (row[x])
					MarkDirty(x, y, x, y);
			}
		}
	}

	void Stamp(int x, int y)
	{
		if (Pixels.empty())
			return;

		const int r2 = Radius * Radius;

		int minX = x - Radius;
		int maxX = x + Radius;
		int minY = y - Radius;
		int maxY = y + Radius;

		minX = std::max(minX, 0);
		minY = std::max(minY, 0);
		maxX = std::min(maxX, MaskSize - 1);
		maxY = std::min(maxY, MaskSize - 1);

		if (minX > maxX || minY > maxY)
			return;

		const DWORD value = Erasing ? 0u : (0xFF000000u | (Rgb & 0x00FFFFFFu));

		for (int py = minY; py <= maxY; ++py)
		{
			const int dy = py - y;

			for (int px = minX; px <= maxX; ++px)
			{
				const int dx = px - x;

				if (dx * dx + dy * dy > r2)
					continue;

				DWORD& target = *At(px, py);

				if (target == value)
					continue;

				StrokeEdits.emplace_back(py * MaskSize + px, target);
				target = value;
			}
		}

		MarkDirty(minX, minY, maxX, maxY);
	}

	void StampLine(int x0, int y0, int x1, int y1)
	{
		const int dx = x1 - x0;
		const int dy = y1 - y0;
		const int steps = std::max(std::abs(dx), std::abs(dy));

		if (steps <= 0)
		{
			Stamp(x0, y0);
			return;
		}

		for (int i = 0; i <= steps; ++i)
			Stamp(x0 + dx * i / steps, y0 + dy * i / steps);
	}

	inline int ClampToByte(int v)
	{
		return v < 0 ? 0 : (v > 255 ? 255 : v);
	}
}

void TacticalMapCanvas::Reset()
{
	Pixels.clear();
	Pixels.shrink_to_fit();
	DirtyMinX = 1;
	DirtyMinY = 1;
	DirtyMaxX = 0;
	DirtyMaxY = 0;
	Active = false;
	StrokeEdits.clear();
}

bool TacticalMapCanvas::HasContent()
{
	return DirtyMaxX >= DirtyMinX;
}

void TacticalMapCanvas::BeginStroke(int x, int y, int radius, unsigned rgb, bool erase)
{
	if (Pixels.empty())
		Pixels.assign(static_cast<size_t>(MaskSize) * MaskSize, 0u);

	Radius = std::clamp(radius, 1, 64);
	Rgb = rgb & 0x00FFFFFFu;
	Erasing = erase;
	Active = true;
	StrokeEdits.clear();
	LastX = std::clamp(x, 0, MaskSize - 1);
	LastY = std::clamp(y, 0, MaskSize - 1);

	Stamp(LastX, LastY);
}

void TacticalMapCanvas::ContinueStroke(int x, int y)
{
	if (!Active || Pixels.empty())
		return;

	const int nx = std::clamp(x, 0, MaskSize - 1);
	const int ny = std::clamp(y, 0, MaskSize - 1);

	if (nx == LastX && ny == LastY)
		return;

	StampLine(LastX, LastY, nx, ny);
	LastX = nx;
	LastY = ny;
}

void TacticalMapCanvas::EndStroke()
{
	Active = false;
}

bool TacticalMapCanvas::TakeStrokeEdits(std::vector<std::pair<int, DWORD>>& out)
{
	out = std::move(StrokeEdits);
	StrokeEdits.clear();
	return !out.empty();
}

void TacticalMapCanvas::ApplyEdits(const std::vector<std::pair<int, DWORD>>& edits)
{
	if (Pixels.empty())
		return;

	for (auto it = edits.rbegin(); it != edits.rend(); ++it)
	{
		if (it->first >= 0 && it->first < static_cast<int>(Pixels.size()))
			Pixels[static_cast<size_t>(it->first)] = it->second;
	}

	RecalculateDirty();
}

void TacticalMapCanvas::Save(std::vector<DWORD>& out)
{
	out = Pixels;
}

void TacticalMapCanvas::Restore(const std::vector<DWORD>& in)
{
	Pixels = in;

	if (Pixels.size() != static_cast<size_t>(MaskSize) * MaskSize)
	{
		Pixels.clear();
		DirtyMinX = 1;
		DirtyMinY = 1;
		DirtyMaxX = 0;
		DirtyMaxY = 0;
		return;
	}

	RecalculateDirty();
}


void TacticalMapCanvas::Render(DSurface* pSurface, const RectangleStruct& dst)
{
	if (!pSurface || Pixels.empty() || DirtyMaxX < DirtyMinX || dst.Width <= 0 || dst.Height <= 0)
		return;

	const int bpp = pSurface->GetBytesPerPixel();

	if (bpp != 2 && bpp != 4)
		return;

	// Only touch the destination area covered by the painted bounding box.
	int x0 = dst.X + static_cast<int>(static_cast<long long>(DirtyMinX) * dst.Width / MaskSize);
	int y0 = dst.Y + static_cast<int>(static_cast<long long>(DirtyMinY) * dst.Height / MaskSize);
	int x1 = dst.X + static_cast<int>(static_cast<long long>(DirtyMaxX + 1) * dst.Width / MaskSize) + 1;
	int y1 = dst.Y + static_cast<int>(static_cast<long long>(DirtyMaxY + 1) * dst.Height / MaskSize) + 1;

	const RectangleStruct surfaceRect = pSurface->GetRect();

	x0 = std::max(x0, dst.X);
	y0 = std::max(y0, dst.Y);
	x1 = std::min(x1, dst.X + dst.Width);
	y1 = std::min(y1, dst.Y + dst.Height);
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, surfaceRect.Width);
	y1 = std::min(y1, surfaceRect.Height);

	if (x0 >= x1 || y0 >= y1)
		return;

	BYTE* const base = static_cast<BYTE*>(pSurface->Lock(0, 0));

	if (!base)
		return;

	const int pitch = pSurface->GetPitch();
	const int width = x1 - x0;

	thread_local std::vector<int> uMap;
	uMap.resize(width);

	for (int i = 0; i < width; ++i)
		uMap[i] = (x0 + i - dst.X) * MaskSize / dst.Width;

	for (int y = y0; y < y1; ++y)
	{
		const int v = (y - dst.Y) * MaskSize / dst.Height;
		const DWORD* const maskRow = Pixels.data() + static_cast<size_t>(v) * MaskSize;
		BYTE* const row = base + static_cast<size_t>(y) * pitch;

		if (bpp == 2)
		{
			WORD* const pixels = reinterpret_cast<WORD*>(row);

			for (int i = 0; i < width; ++i)
			{
				const DWORD c = maskRow[uMap[i]];

				if (!c)
					continue;

				const int sr = static_cast<int>((c >> 16) & 0xFF);
				const int sg = static_cast<int>((c >> 8) & 0xFF);
				const int sb = static_cast<int>(c & 0xFF);

				WORD& d = pixels[x0 + i];
				const int dr = ((d >> 11) & 0x1F) * 255 / 31;
				const int dg = ((d >> 5) & 0x3F) * 255 / 63;
				const int db = (d & 0x1F) * 255 / 31;

				const int r = ClampToByte((sr * BlendAlpha + dr * (255 - BlendAlpha)) / 255);
				const int g = ClampToByte((sg * BlendAlpha + dg * (255 - BlendAlpha)) / 255);
				const int b = ClampToByte((sb * BlendAlpha + db * (255 - BlendAlpha)) / 255);

				d = static_cast<WORD>(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
			}
		}
		else
		{
			DWORD* const pixels = reinterpret_cast<DWORD*>(row);

			for (int i = 0; i < width; ++i)
			{
				const DWORD c = maskRow[uMap[i]];

				if (!c)
					continue;

				const int sr = static_cast<int>((c >> 16) & 0xFF);
				const int sg = static_cast<int>((c >> 8) & 0xFF);
				const int sb = static_cast<int>(c & 0xFF);

				DWORD& d = pixels[x0 + i];
				const int db = static_cast<int>(d & 0xFF);
				const int dg = static_cast<int>((d >> 8) & 0xFF);
				const int dr = static_cast<int>((d >> 16) & 0xFF);

				const int r = ClampToByte((sr * BlendAlpha + dr * (255 - BlendAlpha)) / 255);
				const int g = ClampToByte((sg * BlendAlpha + dg * (255 - BlendAlpha)) / 255);
				const int b = ClampToByte((sb * BlendAlpha + db * (255 - BlendAlpha)) / 255);

				d = static_cast<DWORD>((d & 0xFF000000u) | (r << 16) | (g << 8) | b);
			}
		}
	}

	pSurface->Unlock();
}
