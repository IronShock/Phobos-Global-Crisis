#include "TextScale.h"

#include <Phobos.h>
#include <BitFont.h>
#include <Surface.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <MessageListClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
	// Real engine layout of BitFont's internal font data, verified against
	// gamemd.exe (BitFont::LoadInternalData @ 0x433990).
	struct InternalData
	{
		int FontWidth;               // +0x00
		int BytesPerRow;             // +0x04
		int Rows;                    // +0x08
		int LineHeight;              // +0x0C
		int GlyphCount;              // +0x10
		int GlyphStride;             // +0x14
		unsigned short* SymbolTable; // +0x18, 0x10000 entries: wchar -> glyph index + 1
		unsigned char* Bitmaps;      // +0x1C, GlyphCount * GlyphStride bytes
		int ValidCount;              // +0x20
	};

	static_assert(sizeof(InternalData) == 0x24, "InternalData layout mismatch");

	// Raw offsets inside the engine BitFont object. YRpp's BitFont layout omits
	// the vtable pointer, so these are addressed directly.
	constexpr int BF_InternalData = 0x04;
	constexpr int BF_GraphBuffer = 0x08;
	constexpr int BF_BytesPerRow = 0x18;
	constexpr int BF_LineHeight = 0x1C;

	constexpr int SYMBOL_COUNT = 0x10000;

	void* g_fontObject = nullptr;
	InternalData* g_originalData = nullptr;
	InternalData* g_scaledData = nullptr;
	void* g_originalGraph = nullptr;
	void* g_scaledGraph = nullptr;
	double g_builtFactor = 0.0;
	bool g_hasScaled = false;
	bool g_scaled = false;
	int g_exclusionDepth = 0;
	bool g_surfaceExcluded = false;

	// Original (unscaled) line pitch the engine passes to MessageListClass::Init.
	int g_messageListBaseHeight = 0;

	inline void*& RawPointer(void* object, int offset)
	{
		return *reinterpret_cast<void**>(reinterpret_cast<char*>(object) + offset);
	}

	inline InternalData* GetCurrentData(void* font)
	{
		return reinterpret_cast<InternalData*>(RawPointer(font, BF_InternalData));
	}

	inline bool GlyphBit(const unsigned char* bitmap, int byteStride, int x, int y)
	{
		return (bitmap[1 + y * byteStride + (x >> 3)] >> (7 - (x & 7))) & 1;
	}

	// Rebuilds the engine's scratch glyph buffer for the given data by reusing the
	// engine helper at 0x434700. Returns the freshly allocated buffer without
	// touching the font's previous state.
	void* BuildGraphBuffer(void* font, InternalData* data)
	{
		void* savedData = RawPointer(font, BF_InternalData);
		void* savedGraph = RawPointer(font, BF_GraphBuffer);

		RawPointer(font, BF_InternalData) = data;
		RawPointer(font, BF_GraphBuffer) = nullptr;

		reinterpret_cast<void(__thiscall*)(void*)>(0x434700)(font);

		void* built = RawPointer(font, BF_GraphBuffer);

		RawPointer(font, BF_InternalData) = savedData;
		RawPointer(font, BF_GraphBuffer) = savedGraph;

		return built;
	}

	void FreeScaledData()
	{
		if (g_scaledData)
		{
			std::free(g_scaledData->SymbolTable);
			std::free(g_scaledData->Bitmaps);
			std::free(g_scaledData);
		}

		std::free(g_scaledGraph);

		g_scaledData = nullptr;
		g_scaledGraph = nullptr;
		g_hasScaled = false;
		g_builtFactor = 0.0;
	}

	bool BuildScaled(void* font, InternalData* original, double scale)
	{
		if (!original || scale <= 1.0 || original->GlyphCount <= 0 || original->GlyphStride <= 0
			|| original->BytesPerRow <= 0 || original->Rows <= 0
			|| !original->SymbolTable || !original->Bitmaps)
		{
			return false;
		}

		const bool smooth = Phobos::Config::TextScale_Smooth;

		// Work out the largest result cell so all glyphs share one stride.
		int newMaxWidth = 0;

		for (int i = 0; i < original->GlyphCount; ++i)
		{
			const int width = original->Bitmaps[static_cast<size_t>(i) * original->GlyphStride];

			if (width > 0)
			{
				int advance = static_cast<int>(std::lround((width + 1) * scale)) - 1;

				if (advance > 255)
					advance = 255;

				if (advance > newMaxWidth)
					newMaxWidth = advance;
			}
		}

		const int newBytesPerRow = (newMaxWidth + 7) / 8;
		const int newRows = static_cast<int>(std::lround(original->Rows * scale));

		if (newBytesPerRow <= 0 || newRows <= 0)
			return false;

		const int newStride = 1 + newRows * newBytesPerRow;

		auto* symbolTable = static_cast<unsigned short*>(std::malloc(SYMBOL_COUNT * sizeof(unsigned short)));
		auto* bitmaps = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(original->GlyphCount) * newStride));

		if (!symbolTable || !bitmaps)
		{
			std::free(symbolTable);
			std::free(bitmaps);
			return false;
		}

		std::memcpy(symbolTable, original->SymbolTable, SYMBOL_COUNT * sizeof(unsigned short));
		std::memset(bitmaps, 0, static_cast<size_t>(original->GlyphCount) * newStride);

		for (int g = 0; g < original->GlyphCount; ++g)
		{
			const unsigned char* src = original->Bitmaps + static_cast<size_t>(g) * original->GlyphStride;
			unsigned char* dst = bitmaps + static_cast<size_t>(g) * newStride;

			const int oldWidth = src[0];

			if (oldWidth <= 0)
			{
				dst[0] = 0;
				continue;
			}

			int contentWidth = static_cast<int>(std::lround(oldWidth * scale));
			int advanceWidth = static_cast<int>(std::lround((oldWidth + 1) * scale)) - 1;

			if (contentWidth < 1)
				contentWidth = 1;

			if (advanceWidth > 255)
				advanceWidth = 255;

			if (contentWidth > advanceWidth)
				contentWidth = advanceWidth;

			dst[0] = static_cast<unsigned char>(advanceWidth);

			for (int y = 0; y < newRows; ++y)
			{
				unsigned char* dstRow = dst + 1 + y * newBytesPerRow;

				// Source footprint of this output row (in source pixels).
				const double fy0 = static_cast<double>(y) * original->Rows / newRows;
				const double fy1 = static_cast<double>(y + 1) * original->Rows / newRows;

				for (int x = 0; x < contentWidth; ++x)
				{
					const double fx0 = static_cast<double>(x) * oldWidth / contentWidth;
					const double fx1 = static_cast<double>(x + 1) * oldWidth / contentWidth;

					bool on;

					if (smooth)
					{
						// Area-coverage resampling: fraction of this output pixel's
						// source footprint covered by "on" source pixels.
						const int sxA = static_cast<int>(fx0);
						const int sxB = static_cast<int>(std::ceil(fx1)) - 1;
						const int syA = static_cast<int>(fy0);
						const int syB = static_cast<int>(std::ceil(fy1)) - 1;

						double covered = 0.0;

						for (int sy = syA; sy <= syB; ++sy)
						{
							if (sy < 0 || sy >= original->Rows)
								continue;

							const double oy = std::min(fy1, static_cast<double>(sy + 1)) - std::max(fy0, static_cast<double>(sy));

							if (oy <= 0.0)
								continue;

							for (int sx = sxA; sx <= sxB; ++sx)
							{
								if (sx < 0 || sx >= oldWidth)
									continue;

								if (!GlyphBit(src, original->BytesPerRow, sx, sy))
									continue;

								const double ox = std::min(fx1, static_cast<double>(sx + 1)) - std::max(fx0, static_cast<double>(sx));

								if (ox > 0.0)
									covered += ox * oy;
							}
						}

						const double area = (fx1 - fx0) * (fy1 - fy0);
						on = area > 0.0 && covered * 2.0 >= area;
					}
					else
					{
						const int sx = std::min(static_cast<int>(fx0), oldWidth - 1);
						const int sy = std::min(static_cast<int>(fy0), original->Rows - 1);
						on = GlyphBit(src, original->BytesPerRow, sx, sy);
					}

					if (on)
						dstRow[x >> 3] |= static_cast<unsigned char>(0x80 >> (x & 7));
				}
			}
		}

		auto* scaled = static_cast<InternalData*>(std::malloc(sizeof(InternalData)));

		if (!scaled)
		{
			std::free(symbolTable);
			std::free(bitmaps);
			return false;
		}

		scaled->FontWidth = static_cast<int>(std::lround(original->FontWidth * scale));
		scaled->BytesPerRow = newBytesPerRow;
		scaled->Rows = newRows;
		scaled->LineHeight = static_cast<int>(std::lround(original->LineHeight * scale));
		scaled->GlyphCount = original->GlyphCount;
		scaled->GlyphStride = newStride;
		scaled->SymbolTable = symbolTable;
		scaled->Bitmaps = bitmaps;
		scaled->ValidCount = original->ValidCount;

		FreeScaledData();

		g_scaledData = scaled;
		g_scaledGraph = BuildGraphBuffer(font, scaled);
		g_hasScaled = true;
		g_builtFactor = scale;

		Debug::Log("[Phobos] TextScale: rebuilt font at %.2fx [build-C] (cell %dx%d, stride %d, smooth=%d).\n",
			scale, newBytesPerRow * 8, newRows, newStride, static_cast<int>(smooth));

		return true;
	}

	void Apply(bool scaled)
	{
		if (!g_fontObject || !g_hasScaled || !g_originalData)
			return;

		InternalData* data = scaled ? g_scaledData : g_originalData;
		void* graph = scaled ? g_scaledGraph : g_originalGraph;

		RawPointer(g_fontObject, BF_InternalData) = data;
		RawPointer(g_fontObject, BF_GraphBuffer) = graph;
		*reinterpret_cast<int*>(reinterpret_cast<char*>(g_fontObject) + BF_BytesPerRow) = data->BytesPerRow;
		*reinterpret_cast<int*>(reinterpret_cast<char*>(g_fontObject) + BF_LineHeight) = data->LineHeight;

		g_scaled = scaled;
	}

	bool WantScaled()
	{
		if (Phobos::Config::TextScale <= 1.0 || g_exclusionDepth != 0)
			return false;

		if (!SessionClass::Instance.CurrentlyInGame)
			return false;

		if (Unsorted::WSDialogCount != 0 || Unsorted::SpecialDialog != 0)
			return false;

		if (!DSurface::Temp || DSurface::Temp == DSurface::Sidebar)
			return false;

		return true;
	}
}

bool TextScale::Enabled()
{
	return Phobos::Config::TextScale > 1.0;
}

double TextScale::GetScaleFactor()
{
	return Enabled() ? Phobos::Config::TextScale : 1.0;
}

void TextScale::Update()
{
	BitFont* font = BitFont::Instance;

	if (!font)
		return;

	void* fontObject = font;
	const double scale = Phobos::Config::TextScale;
	InternalData* current = GetCurrentData(fontObject);

	// A different font object (initial load or reload), or the engine replaced
	// the font data under us, means we have to recapture the original.
	if (fontObject != g_fontObject
		|| (current && current != g_scaledData && current != g_originalData))
	{
		FreeScaledData();

		g_fontObject = fontObject;
		g_scaled = false;
		g_originalData = current;
		g_originalGraph = RawPointer(fontObject, BF_GraphBuffer);

		if (Enabled() && g_originalData)
			BuildScaled(fontObject, g_originalData, scale);
	}
	else if (Enabled() && g_originalData && (!g_hasScaled || g_builtFactor != scale))
	{
		if (g_scaled)
			Apply(false);

		BuildScaled(fontObject, g_originalData, scale);
	}

	if (!g_hasScaled)
	{
		if (g_scaled)
			Apply(false);

		return;
	}

	const bool wantScaled = WantScaled();

	if (wantScaled != g_scaled)
		Apply(wantScaled);

	// Keep the vanilla message list's row pitch in sync with the font line height.
	if (g_messageListBaseHeight > 0)
		MessageListClass::Instance.Height = static_cast<int>(std::lround(g_messageListBaseHeight * scale));
}

void TextScale::BeginSurface(const Surface* pSurface)
{
	if (pSurface && pSurface == static_cast<const Surface*>(DSurface::Sidebar))
	{
		// Sidebar HUD text (numbers, labels) must keep its original size. Keep the
		// exclusion active for the whole call so measurement and drawing agree.
		if (!g_surfaceExcluded)
		{
			g_surfaceExcluded = true;
			PushExclusion();
		}
	}
	else
	{
		Update();
	}
}

void TextScale::EndSurface()
{
	if (g_surfaceExcluded)
	{
		g_surfaceExcluded = false;
		PopExclusion();
	}
}

void TextScale::PushExclusion()
{
	++g_exclusionDepth;
	Update();
}

void TextScale::PopExclusion()
{
	if (g_exclusionDepth > 0)
		--g_exclusionDepth;

	Update();
}

// Text entry points: refresh the active font variant before each measure/draw.

DEFINE_HOOK(0x433CF0, BitFont_GetTextDimension_TextScale, 0x6)
{
	TextScale::Update();
	return 0;
}

DEFINE_HOOK(0x434120, BitFont_Blit_TextScale, 0x6)
{
	TextScale::Update();
	return 0;
}

DEFINE_HOOK(0x434500, BitFont_DrawString_TextScale, 0x7)
{
	TextScale::Update();
	return 0;
}

DEFINE_HOOK(0x434B90, BitText_Print_TextScale, 0x5)
{
	TextScale::Update();
	return 0;
}

DEFINE_HOOK(0x434CD0, BitText_DrawText_TextScale, 0x5)
{
	TextScale::Update();
	return 0;
}

// Simple_Text_Print_Wide is the funnel every DSurface::DrawText goes through.
// The first stack argument is the target surface.
DEFINE_HOOK(0x4A5EB0, Simple_Text_Print_Wide_TextScale, 0x7)
{
	GET_STACK(const Surface*, pSurface, 0x4);
	TextScale::BeginSurface(pSurface);
	return 0;
}

// Covers the whole epilogue (`add esp,0x28; ret 0x1c` = 6 bytes) so the 5-byte
// injected jump cannot corrupt the following instruction.
DEFINE_HOOK(0x4A5FFF, Simple_Text_Print_Wide_End_TextScale, 0x6)
{
	TextScale::EndSurface();
	return 0;
}

DEFINE_HOOK(0x623880, DSurface_DrawBitFontStrings_TextScale, 0x5)
{
	TextScale::Update();
	return 0;
}

// The vanilla sidebar draws some text (e.g. super weapon readiness/charge) through
// a path that does not go through our font hooks, so it would otherwise inherit the
// last-applied (scaled)font. Force the original font for the whole sidebar draw.
// SidebarClass::Draw has a single entry and a single exit.
DEFINE_HOOK(0x6A6C30, SidebarClass_Draw_TextScale_Begin, 0x9)
{
	TextScale::PushExclusion();
	return 0;
}

// `mov dword ptr [0x887314], eax` right before the epilogue of SidebarClass::Draw.
DEFINE_HOOK(0x6A70C5, SidebarClass_Draw_TextScale_End, 0x5)
{
	TextScale::PopExclusion();
	return 0;
}

// MessageListClass::Init(int X, int Y, int MaxMsg, int MaxChars, int Height, ...)
// The vanilla message list stores its own row pitch (normally 14). Scale it to
// match the enlarged font so multi-line messages don't overlap.
DEFINE_HOOK(0x5D3A60, MessageListClass_Init_TextScale, 0x5)
{
	GET_STACK(int, baseHeight, 0x14);
	g_messageListBaseHeight = baseHeight;

	const double factor = TextScale::GetScaleFactor();

	if (factor != 1.0)
		R->Stack<int>(0x14, static_cast<int>(std::lround(baseHeight * factor)));

	return 0;
}
