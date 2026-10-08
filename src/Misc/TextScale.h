#pragma once

class Surface;

// Global in-game bitmap-font scaling.
//
// YR renders all text with a single bitmap font whose glyphs are fixed-size 1bpp
// masks. To enlarge text we rebuild the engine's font data (`InternalData`) at a
// whole-number scale using exact pixel replication, which keeps every glyph crisp
// while every engine layout / measurement path produces correctly spaced text.
//
// The scaled copy is swapped in per text call based on the current game context:
//   - only while actually in a game (shell menus keep the original size)
//   - not while drawing to the sidebar surface (HUD text keeps its size)
//   - not while an in-game dialog is open
//
// Configured through `RA2MD.INI [Phobos] TextScale` (1.0 / 2.0 / 3.0).
class TextScale
{
public:
	// Recompute which font variant should currently be used and apply it.
	static void Update();

	// Current scale factor (1.0 when the feature is disabled). Used by UI parts
	// that keep their own row/line pitch so it can be scaled to match the font.
	static double GetScaleFactor();

	// Begin/end a top-level string draw; the target surface decides whether the
	// whole call (measurement + drawing) is excluded from scaling.
	static void BeginSurface(const Surface* pSurface);
	static void EndSurface();

	// Temporarily force the original (unscaled) font. Used for specific UI
	// elements that must not scale (e.g. digital display numbers).
	static void PushExclusion();
	static void PopExclusion();

	class ScopedExclusion
	{
	public:
		ScopedExclusion() { TextScale::PushExclusion(); }
		~ScopedExclusion() { TextScale::PopExclusion(); }
	};

private:
	static bool Enabled();
};
