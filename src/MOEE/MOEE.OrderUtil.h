#pragma once
#include <Windows.h>
#include <GeneralStructures.h>

class ObjectClass;

// Shared helpers for the ported order/QoL modules (MO_EE's OrderUtil).
//
// Pure read-only queries plus standard EventClass::OutList enqueue. Preview
// lines are drawn with Phobos' own surface path (TacticalClass::CoordsToClient
// + DSurface::Temp->DrawLine) instead of MO_EE's locked 16-bit surface.
namespace MOEE::OU
{
	// ---- object helpers ----
	bool IsObject(const void* p);                 // vtable inside the exe image
	int WhatAmI(const void* p);                   // AbstractType as int; -1 on failure
	bool GetCoords(void* p, CoordStruct& out);    // object centre (leptons)

	// ---- selection ----
	// Foreground object count in ObjectClass::CurrentObjects.
	int SelectionCount();
	ObjectClass* SelectionAt(int index);

	// ---- mouse / map ----
	// Converts the current cursor position to a map cell (zoom-aware via
	// DisplayClass::ProcessClickCoords). Returns false if off map.
	bool MouseCell(int& cx, int& cy, ObjectClass** target = nullptr, Point2D* screen = nullptr);
	bool CellValid(int cx, int cy);
	// DisplayClass::DecideAction for the given cell/target. Returns -1 if invalid.
	int DecideAction(int cx, int cy, void* target);

	// ---- preview lines ----
	struct PrevLine { int ulx, uly, ulz, tlx, tly, tlz; };
	constexpr int PrevCap = 128;

	bool PrevPush(PrevLine* buf, int& nio,
		int ulx, int uly, int ulz, int tlx, int tly, int tlz);
	bool PrevPushObjs(PrevLine* buf, int& nio, void* unit, void* target);
	// Start = unit centre, end = cell centre at ground height (or the unit's own
	// height when fly != 0).
	bool PrevPushCell(PrevLine* buf, int& nio, void* unit, int fly, int cx, int cy);
	void PrevDraw(const PrevLine* buf, int n, COLORREF color);
	// Redraw gate: refresh on cell/key change after minMs, or every refreshMs.
	bool PrevThrottle(unsigned& lastMs, int& lastCx, int& lastCy, int& lastKey,
		int cx, int cy, int key, unsigned minMs, unsigned refreshMs);

	// ---- hotkeys ----
	struct Hotkey
	{
		unsigned int vk = 0;
		unsigned int mods = 0;              // 1=Ctrl 2=Shift 4=Alt
		unsigned int extra[4] = { 0, 0, 0, 0 };
		int nExtra = 0;
		bool disabled = false;
	};

	// "Ctrl+Z+Middle" / "Middle" / "X2" / single letter / "None" / "0".
	bool ParseHotkey(const char* spec, Hotkey& out);
	bool KeyDown(unsigned int vk);
	bool HotkeyDown(const Hotkey& hk);
}
