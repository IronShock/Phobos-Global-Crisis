#include "MOEE.h"

#include <Phobos.h>
#include <GeneralStructures.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements' Clamp_Center takeover.
//
// At high resolutions the tactical viewport can be larger than the map. Vanilla
// Clamp_Center then clamps to an inverted range (min > max), so the camera
// toggles between the two bounds every frame - the "map jitters left and right"
// bug. This replaces the whole function: when an axis is degenerate the camera
// is pinned to the map centre instead.
namespace MOEE::MapCenter
{
	void ClampCenter(Point2D* c);
}

namespace MOEE::MapCenter
{
	void ClampCenter(Point2D* c)
	{
		if (!c || !IsPlausiblePtr(c))
			return;

		// DisplayClass map pixel rect (fields verified by upstream).
		const int bx = *reinterpret_cast<int*>(0x87F8DCu);
		const int gx = *reinterpret_cast<int*>(0x87F8E4u);
		const int gy = *reinterpret_cast<int*>(0x87F8E8u);
		const int cw = *reinterpret_cast<int*>(0x87F8ECu);
		const int ch = *reinterpret_cast<int*>(0x87F8F0u);

		if (cw <= 0 || ch <= 0 || cw > 8192 || ch > 8192)
			return;

		if (gx < -4096 || gx > 4096 || gy < -4096 || gy > 4096)
			return;

		const int ml = 30 * (2 * gx - bx);
		const int mt = 15 * (bx + 2 * gy - 5);
		const int mw = 60 * cw;
		const int mh = 30 * ch + 135;

		const int vw = *reinterpret_cast<int*>(0xB0CE30u); // ViewBounds.Width
		const int vh = *reinterpret_cast<int*>(0xB0CE34u); // ViewBounds.Height

		if (vw <= 16 || vh <= 16)
			return;

		const bool fix = Phobos::Config::MOEE_MapCenter;

		// Degenerate axis = map smaller than the viewport.
		const bool degX = fix && (mw < vw);
		const bool degY = fix && (mh < vh);

		const int midX = ml + mw / 2;
		const int midY = mt + mh / 2;
		const int mnX = vw / 2 + ml;
		const int mxX = mnX + mw - vw;
		const int mnY = vh / 2 + mt;
		const int mxY = mnY + mh - vh;

		if (degY)
			c->Y = midY;
		else if (c->Y < mnY)
			c->Y = mnY;
		else if (c->Y > mxY)
			c->Y = mxY;

		if (degX)
			c->X = midX;
		else if (c->X < mnX)
			c->X = mnX;
		else if (c->X > mxX)
			c->X = mxX;
	}
}

// Stub matching the engine's `void Clamp_Center(Point2D*)` (one stack arg, ecx =
// this, `ret 4`). MSVC only allows __thiscall on member functions, so we use a
// naked shim instead.
namespace
{
	void __cdecl ClampCenterC(Point2D* c)
	{
		MOEE::MapCenter::ClampCenter(c);
	}

	__declspec(naked) void ClampCenterStub()
	{
		__asm
		{
			mov eax, [esp + 4]     // Point2D* c
			push eax
			call ClampCenterC
			add esp, 4
			ret 4
		}
	}
}

DEFINE_FUNCTION_JUMP(LJMP, 0x6D8640, ClampCenterStub)
