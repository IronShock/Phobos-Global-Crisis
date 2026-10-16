#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <AbstractClass.h>
#include <FootClass.h>
#include <TechnoClass.h>
#include <ObjectClass.h>
#include <DisplayClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TacticalClass.h>
#include <WWMouseClass.h>
#include <Surface.h>
#include <Drawing.h>

#include <cctype>
#include <cstring>
#include <utility>

namespace MOEE::OU
{
	namespace
	{
		constexpr unsigned int EXE_LO = 0x401000u;
		constexpr unsigned int EXE_HI = 0x8A0000u;
		constexpr int LEP_PER_CELL = 256;
		constexpr int MAP_CELLS = 512;

		bool InExe(const void* p)
		{
			const auto v = static_cast<unsigned int>(reinterpret_cast<size_t>(p));
			return v >= EXE_LO && v < EXE_HI;
		}

		unsigned int MapKeyToken(const char* token)
		{
			if (!token || !*token)
				return 0;

			const size_t n = std::strlen(token);

			if (n == 1)
			{
				const unsigned char c = static_cast<unsigned char>(token[0]);

				if (c >= 'a' && c <= 'z') return static_cast<unsigned int>(c - 'a' + 'A');
				if (c >= 'A' && c <= 'Z') return static_cast<unsigned int>(c);
				if (c >= '0' && c <= '9') return static_cast<unsigned int>(c);
				return 0;
			}

			if ((token[0] == 'F' || token[0] == 'f') && n >= 2)
			{
				int num = 0;
				bool ok = true;

				for (size_t i = 1; i < n; ++i)
				{
					if (token[i] < '0' || token[i] > '9') { ok = false; break; }
					num = num * 10 + (token[i] - '0');
				}

				if (ok && num >= 1 && num <= 24)
					return static_cast<unsigned int>(VK_F1 + num - 1);
			}

			static const struct { const char* name; unsigned int vk; } named[] =
			{
				{ "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
				{ "Return", VK_RETURN }, { "Backspace", VK_BACK }, { "Back", VK_BACK },
				{ "Insert", VK_INSERT }, { "Ins", VK_INSERT }, { "Delete", VK_DELETE },
				{ "Del", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END },
				{ "PageUp", VK_PRIOR }, { "PgUp", VK_PRIOR }, { "PageDown", VK_NEXT },
				{ "PgDn", VK_NEXT }, { "Up", VK_UP }, { "Down", VK_DOWN },
				{ "Left", VK_LEFT }, { "Right", VK_RIGHT }, { "Pause", VK_PAUSE },
				{ "Num0", VK_NUMPAD0 }, { "Num1", VK_NUMPAD1 }, { "Num2", VK_NUMPAD2 },
				{ "Num3", VK_NUMPAD3 }, { "Num4", VK_NUMPAD4 }, { "Num5", VK_NUMPAD5 },
				{ "Num6", VK_NUMPAD6 }, { "Num7", VK_NUMPAD7 }, { "Num8", VK_NUMPAD8 },
				{ "Num9", VK_NUMPAD9 },
				{ "Middle", VK_MBUTTON }, { "MMB", VK_MBUTTON }, { "MButton", VK_MBUTTON },
				{ "LMB", VK_LBUTTON }, { "RMB", VK_RBUTTON },
				{ "X1", VK_XBUTTON1 }, { "X2", VK_XBUTTON2 },
			};

			for (const auto& e : named)
			{
				if (_stricmp(token, e.name) == 0)
					return e.vk;
			}

			return 0;
		}
	}

	bool IsObject(const void* p)
	{
		if (!p || !IsPlausiblePtr(p))
			return false;

		return InExe(*reinterpret_cast<void* const*>(p));
	}

	int WhatAmI(const void* p)
	{
		if (!IsObject(p))
			return -1;

		return static_cast<int>(
			reinterpret_cast<const AbstractClass*>(p)->WhatAmI());
	}

	bool GetCoords(void* p, CoordStruct& out)
	{
		if (!IsObject(p))
			return false;

		CoordStruct cs = { 0, 0, 0 };

		if (!reinterpret_cast<AbstractClass*>(p)->GetCoords(&cs))
			return false;

		if (cs.X < 0 || cs.Y < 0
			|| cs.X >= MAP_CELLS * LEP_PER_CELL || cs.Y >= MAP_CELLS * LEP_PER_CELL)
			return false;

		if (cs.Z < -8192 || cs.Z > 8192)
			return false;

		out = cs;
		return true;
	}

	int SelectionCount()
	{
		return ObjectClass::CurrentObjects.Count;
	}

	ObjectClass* SelectionAt(int index)
	{
		auto& objects = ObjectClass::CurrentObjects;

		if (index < 0 || index >= objects.Count)
			return nullptr;

		return objects.GetItem(index);
	}

	bool CellValid(int cx, int cy)
	{
		if (cx < 0 || cy < 0 || cx >= MAP_CELLS || cy >= MAP_CELLS)
			return false;

		CellStruct cell;
		cell.X = static_cast<short>(cx);
		cell.Y = static_cast<short>(cy);
		return MapClass::Instance.GetCellAt(cell) != nullptr;
	}

	bool MouseCell(int& cx, int& cy, ObjectClass** target, Point2D* screen)
	{
		if (target)
			*target = nullptr;

		if (!WWMouseClass::Instance)
			return false;

		Point2D point;
		WWMouseClass::Instance->GetCoords(&point);

		CellStruct cell = { 0, 0 };
		CoordStruct coord = { 0, 0, 0 };
		ObjectClass* hit = nullptr;
		BYTE a5 = 0, a6 = 0;

		// Reads the physical screen point from the display.
		if (!DisplayClass::Instance.ProcessClickCoords(&point, &cell, &coord, &hit, &a5, &a6))
			return false;

		if (!CellValid(cell.X, cell.Y))
			return false;

		cx = cell.X;
		cy = cell.Y;

		if (screen)
			*screen = point;

		if (target && hit && IsObject(hit))
			*target = hit;

		return true;
	}

	int DecideAction(int cx, int cy, void* target)
	{
		if (!target || !IsObject(target))
			return -1;

		if (cx < 0 || cy < 0 || cx >= MAP_CELLS || cy >= MAP_CELLS)
			return -1;

		CellStruct cell;
		cell.X = static_cast<short>(cx);
		cell.Y = static_cast<short>(cy);

		using Fn_DecideAction = int(__thiscall*)(void*, void*, void*, int);
		const int result = reinterpret_cast<Fn_DecideAction>(0x692610u)(
			&DisplayClass::Instance, &cell, target, 1);

		if (result < 0 || result > 80)
			return -1;

		return result;
	}

	bool PrevPush(PrevLine* buf, int& nio,
		int ulx, int uly, int ulz, int tlx, int tly, int tlz)
	{
		if (!buf || nio < 0 || nio >= PrevCap)
			return false;

		buf[nio++] = { ulx, uly, ulz, tlx, tly, tlz };
		return true;
	}

	bool PrevPushObjs(PrevLine* buf, int& nio, void* unit, void* target)
	{
		CoordStruct a, b;

		if (!GetCoords(unit, a) || !GetCoords(target, b))
			return false;

		return PrevPush(buf, nio, a.X, a.Y, a.Z, b.X, b.Y, b.Z);
	}

	bool PrevPushCell(PrevLine* buf, int& nio, void* unit, int fly, int cx, int cy)
	{
		CoordStruct a;

		if (!GetCoords(unit, a))
			return false;

		int gz = 0;

		if (fly)
		{
			gz = a.Z;
		}
		else
		{
			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);
			CellClass* pCell = MapClass::Instance.GetCellAt(cell);

			if (!pCell)
				return false;

			const Point2D zero = { 0, 0 };
			using Fn_FloorHeight = int(__thiscall*)(void*, const Point2D*);
			gz = reinterpret_cast<Fn_FloorHeight>(0x47B3A0u)(pCell, &zero);
		}

		return PrevPush(buf, nio, a.X, a.Y, a.Z,
			cx * LEP_PER_CELL + LEP_PER_CELL / 2,
			cy * LEP_PER_CELL + LEP_PER_CELL / 2, gz);
	}

	void PrevDraw(const PrevLine* buf, int n, COLORREF color)
	{
		if (!buf || n <= 0)
			return;

		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		if (n > PrevCap)
			n = PrevCap;

		for (int k = 0; k < n; ++k)
		{
			CoordStruct a = { buf[k].ulx, buf[k].uly, buf[k].ulz };
			CoordStruct b = { buf[k].tlx, buf[k].tly, buf[k].tlz };

			auto [pa, va] = TacticalClass::Instance->CoordsToClient(a);
			auto [pb, vb] = TacticalClass::Instance->CoordsToClient(b);
			pSurface->DrawLine(&pa, &pb, color);
		}
	}

	bool PrevThrottle(unsigned& lastMs, int& lastCx, int& lastCy, int& lastKey,
		int cx, int cy, int key, unsigned minMs, unsigned refreshMs)
	{
		const unsigned now = GetTickCount();

		if (now - lastMs < minMs)
			return false;

		if (cx == lastCx && cy == lastCy && key == lastKey && now - lastMs < refreshMs)
			return false;

		lastMs = now;
		lastCx = cx;
		lastCy = cy;
		lastKey = key;
		return true;
	}

	bool ParseHotkey(const char* spec, Hotkey& out)
	{
		out = Hotkey();

		if (!spec || !*spec)
			return false;

		if (_stricmp(spec, "None") == 0 || _stricmp(spec, "Off") == 0
			|| _stricmp(spec, "Disabled") == 0 || _stricmp(spec, "0") == 0)
		{
			out.disabled = true;
			return true;
		}

		char buffer[128];
		strncpy_s(buffer, spec, _TRUNCATE);

		unsigned int lastKey = 0;
		bool haveKey = false;
		unsigned int keys[8] = { 0 };
		int nKeys = 0;

		char* context = nullptr;

		for (char* token = strtok_s(buffer, "+ \t", &context); token;
			token = strtok_s(nullptr, "+ \t", &context))
		{
			if (_stricmp(token, "Ctrl") == 0 || _stricmp(token, "Control") == 0)
				out.mods |= 1u;
			else if (_stricmp(token, "Shift") == 0)
				out.mods |= 2u;
			else if (_stricmp(token, "Alt") == 0)
				out.mods |= 4u;
			else
			{
				const unsigned int vk = MapKeyToken(token);

				if (vk && nKeys < 8)
				{
					keys[nKeys++] = vk;
					lastKey = vk;
					haveKey = true;
				}
			}
		}

		if (!haveKey)
		{
			out.disabled = true;
			return true;
		}

		out.vk = lastKey;

		// Leading keys must be held too (last one is the primary key).
		for (int i = 0; i + 1 < nKeys && out.nExtra < 4; ++i)
			out.extra[out.nExtra++] = keys[i];

		return true;
	}

	bool KeyDown(unsigned int vk)
	{
		return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
	}

	bool HotkeyDown(const Hotkey& hk)
	{
		if (hk.disabled || !hk.vk)
			return false;

		if ((hk.mods & 1u) && !KeyDown(VK_CONTROL)) return false;
		if ((hk.mods & 2u) && !KeyDown(VK_SHIFT)) return false;
		if ((hk.mods & 4u) && !KeyDown(VK_MENU)) return false;

		for (int i = 0; i < hk.nExtra; ++i)
		{
			if (!KeyDown(hk.extra[i]))
				return false;
		}

		return KeyDown(hk.vk);
	}
}
