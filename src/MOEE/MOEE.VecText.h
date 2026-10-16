#pragma once

// Port of MO_ExperienceEnhancements' GUI/VecText into Phobos (stage V1).
//
// Global vector-text takeover: the engine's bitmap glyph rasterisation is
// replaced with GDI-rendered TrueType glyphs while every metric / wrap /
// alignment path still goes through the engine.
//
// Safety design (differs from upstream):
//   * all hooks are Syringe-managed DEFINE_HOOK (no runtime VirtualAlloc
//     trampolines, which is what made the ChatEdit experiment unstable);
//   * fall-through is plain `return 0` (Syringe replays the original bytes);
//   * takeover sets the result in EAX and returns a naked `ret 0x10` stub
//     - GetTextDimension and BitFont::Blit are both thiscall + 4 stack args
//       with a `... ; ret 10h` epilogue (verified by disassembling gamemd.exe);
//   * any doubt falls back to the engine.
//
// Coexistence with Phobos' TextScale: both hook the same five addresses and
// chain (return 0 lets the other run). The chosen pixel size is multiplied by
// TextScale::GetScaleFactor() so high-resolution text scaling still applies.

namespace MOEE::VecText
{
	void EnsureInit();
}
