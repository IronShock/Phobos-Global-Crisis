#include "MOEE.h"
#include "MOEE.OrderUtil.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <AbstractClass.h>
#include <ObjectClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/PatrolCircle.cpp (simplified).
//
// Z+Middle: place a patrol ring for the whole selection.
//   - Tap (or a short drag): a Size x Size square ring centred on the release
//     cell. Longer drags expand the start..end bounding box by Size/2 into a
//     rectangular ring.
//   - Every waypoint is issued to every unit as a planning MegaMission(Move,
//     Destination = Follow = cell). Once all of them are queued, each unit
//     connects its first node (EventType::PlanConnect) and the plan is committed
//     (EventType::PlanCommit).
//
// Simplifications vs upstream (which gates on live PlanningToken state across
// frames and re-samples curved loops): here the three phases are separated by a
// fixed short wait, and a dragged line expands as an axis-aligned rectangle
// instead of a direction-aware one. The event sequence itself is the same.
namespace MOEE::PatrolCircle
{
	namespace
	{
		constexpr int LEP_PER_CELL = 256;
		constexpr int UNIT_CAP = 1024;
		constexpr int WP_CAP = 16;
		constexpr int PEND_CAP = 4096;
		constexpr int DRAIN_PER_FRAME = 100;

		enum class Phase { Idle, DrainMega, WaitMega, DrainConnect, Commit, WaitCommit };

		struct Pend { void* unit; short x; short y; bool connect; };

		OU::Hotkey Key;
		bool Initialized = false;
		bool PrevDown = false;
		bool HaveStart = false;
		int StartX = 0, StartY = 0;

		static Pend Pending[PEND_CAP];
		int PendCount = 0;
		int PendHead = 0;
		Phase Current = Phase::Idle;
		int WaitStart = 0;
		int HouseIndex = 0;

		int CornerX[WP_CAP];
		int CornerY[WP_CAP];
		int CornerCount = 0;

		OU::PrevLine Lines[OU::PrevCap];
		int LineCount = 0;

		int Size()
		{
			return Phobos::Config::MOEE_PatrolCircle_Size;
		}

		bool IsFoot(void* p)
		{
			const int what = OU::WhatAmI(p);
			return what == static_cast<int>(AbstractType::Unit)
				|| what == static_cast<int>(AbstractType::Infantry)
				|| what == static_cast<int>(AbstractType::Aircraft);
		}

		void BuildRing(int sx, int sy, int ex, int ey)
		{
			const int half = Size() / 2;
			CornerCount = 0;

			const int dx = ex - sx;
			const int dy = ey - sy;
			const int cheb = (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy)
				? (dx > 0 ? dx : -dx) : (dy > 0 ? dy : -dy);

			if (cheb < 2)
			{
				// Tap: Size x Size square centred on the release cell.
				const int lo = -(Size() / 2);
				const int hi = lo + Size() - 1;

				CornerX[0] = ex + lo; CornerY[0] = ey + lo;
				CornerX[1] = ex + hi; CornerY[1] = ey + lo;
				CornerX[2] = ex + hi; CornerY[2] = ey + hi;
				CornerX[3] = ex + lo; CornerY[3] = ey + hi;
			}
			else
			{
				const int x0 = (sx < ex ? sx : ex) - half;
				const int x1 = (sx > ex ? sx : ex) + half;
				const int y0 = (sy < ey ? sy : ey) - half;
				const int y1 = (sy > ey ? sy : ey) + half;

				CornerX[0] = x0; CornerY[0] = y0;
				CornerX[1] = x1; CornerY[1] = y0;
				CornerX[2] = x1; CornerY[2] = y1;
				CornerX[3] = x0; CornerY[3] = y1;
			}

			for (int i = 0; i < 4; ++i)
			{
				if (OU::CellValid(CornerX[i], CornerY[i]))
					CornerCount = i + 1;
			}

			if (CornerCount != 4)
				CornerCount = 0;
		}

		bool FireMega(void* unit, int cx, int cy)
		{
			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);

			EventClass event(HouseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				Mission::Move,
				TargetClass(),
				TargetClass(cell),
				TargetClass(cell)); // Follow = Destination (required for the loop)
			event.MegaMission.IsPlanningEvent = true;

			return EventClass::OutList.Add(event);
		}

		bool FireConnect(void* unit, int cx, int cy)
		{
			CellStruct cell;
			cell.X = static_cast<short>(cx);
			cell.Y = static_cast<short>(cy);

			EventClass event(HouseIndex, EventType::PlanConnect,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				TargetClass(cell));

			return EventClass::OutList.Add(event);
		}

		void FireCommit()
		{
			EventClass event(HouseIndex, EventType::PlanCommit);
			EventClass::OutList.Add(event);
		}

		void Press(int ex, int ey)
		{
			void* units[UNIT_CAP];
			int nu = 0;

			for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
			{
				void* o = OU::SelectionAt(i);

				if (o && IsFoot(o))
					units[nu++] = o;
			}

			if (nu <= 0)
				return;

			BuildRing(HaveStart ? StartX : ex, HaveStart ? StartY : ey, ex, ey);

			if (CornerCount <= 0)
				return;

			if (!HouseClass::CurrentPlayer)
				return;

			HouseIndex = HouseClass::CurrentPlayer->ArrayIndex;

			// Phase 1: every waypoint to every unit.
			PendCount = 0;
			PendHead = 0;

			for (int k = 0; k < CornerCount && PendCount < PEND_CAP; ++k)
			{
				for (int u = 0; u < nu && PendCount < PEND_CAP; ++u)
					Pending[PendCount++] = { units[u], static_cast<short>(CornerX[k]),
						static_cast<short>(CornerY[k]), false };
			}

			Current = Phase::DrainMega;
		}

		// Returns true once the mega pass is fully drained.
		bool DrainMega()
		{
			int sent = 0;

			while (PendHead < PendCount && sent < DRAIN_PER_FRAME)
			{
				const Pend& p = Pending[PendHead];

				if (!FireMega(p.unit, p.x, p.y))
					return false;

				++PendHead;
				++sent;
			}

			if (PendHead >= PendCount)
			{
				PendHead = PendCount = 0;
				return true;
			}

			return false;
		}

		bool DrainConnect()
		{
			int sent = 0;

			while (PendHead < PendCount && sent < DRAIN_PER_FRAME)
			{
				const Pend& p = Pending[PendHead];

				if (!FireConnect(p.unit, p.x, p.y))
					return false;

				++PendHead;
				++sent;
			}

			if (PendHead >= PendCount)
			{
				PendHead = PendCount = 0;
				return true;
			}

			return false;
		}

		void BuildConnectPass(void* const units[], int nu)
		{
			PendCount = 0;
			PendHead = 0;

			for (int u = 0; u < nu && PendCount < PEND_CAP; ++u)
				Pending[PendCount++] = { units[u], static_cast<short>(CornerX[0]),
					static_cast<short>(CornerY[0]), true };
		}
	}

	void Poll()
	{
		if (!Initialized)
		{
			Initialized = true;
			OU::ParseHotkey(Phobos::Config::MOEE_PatrolCircle, Key);
			Log("pcircle", "%s size=%d", Key.disabled ? "OFF" : "ON", Size());
		}

		if (Key.disabled)
			return;

		const bool down = OU::HotkeyDown(Key);
		const int fr = Unsorted::CurrentFrame;

		if (down && !PrevDown)
		{
			int sx = 0, sy = 0;

			if (OU::MouseCell(sx, sy))
			{
				StartX = sx;
				StartY = sy;
				HaveStart = true;
			}
		}

		if (!down && PrevDown)
		{
			int ex = 0, ey = 0;

			if (OU::MouseCell(ex, ey))
				Press(ex, ey);

			HaveStart = false;
		}

		PrevDown = down;

		// Phase machine (Poll runs once per logic frame).
		switch (Current)
		{
		case Phase::DrainMega:
			if (DrainMega())
			{
				Current = Phase::WaitMega;
				WaitStart = fr;
			}
			break;

		case Phase::WaitMega:
			if (fr - WaitStart >= 6)
			{
				// Re-resolve the selection for the connect pass.
				void* units[UNIT_CAP];
				int nu = 0;

				for (int i = 0; i < OU::SelectionCount() && nu < UNIT_CAP; ++i)
				{
					void* o = OU::SelectionAt(i);

					if (o && IsFoot(o))
						units[nu++] = o;
				}

				if (nu <= 0)
				{
					Current = Phase::Idle;
					break;
				}

				BuildConnectPass(units, nu);
				Current = Phase::DrainConnect;
			}
			break;

		case Phase::DrainConnect:
			if (DrainConnect())
			{
				Current = Phase::Commit;
				WaitStart = fr;
			}
			break;

		case Phase::Commit:
			if (Phobos::Config::MOEE_PatrolCircle_Commit)
				FireCommit();

			Log("pcircle", "ring committed (corners=%d)", CornerCount);
			Current = Phase::WaitCommit;
			WaitStart = fr;
			break;

		case Phase::WaitCommit:
			if (fr - WaitStart >= 2)
				Current = Phase::Idle;
			break;

		default:
			break;
		}
	}

	void Draw()
	{
		LineCount = 0;

		if (Key.disabled || !OU::HotkeyDown(Key))
			return;

		auto pSurface = DSurface::Temp;

		if (!pSurface)
			return;

		int cx = 0, cy = 0;

		if (!OU::MouseCell(cx, cy))
			return;

		const int sx = HaveStart ? StartX : cx;
		const int sy = HaveStart ? StartY : cy;

		BuildRing(sx, sy, cx, cy);

		if (CornerCount < 4)
			return;

		auto corner = [](int gx, int gy, Point2D& out) -> bool
		{
			CoordStruct c = { gx * LEP_PER_CELL + LEP_PER_CELL / 2,
				gy * LEP_PER_CELL + LEP_PER_CELL / 2, 0 };
			c.Z = MapClass::Instance.GetCellFloorHeight(c);
			auto [point, visible] = TacticalClass::Instance->CoordsToClient(c);
			out = point;
			return visible;
		};

		Point2D p[4];
		const COLORREF color = RGB(255, 255, 255);

		for (int i = 0; i < 4; ++i)
		{
			int j = (i + 1) & 3;

			if (corner(CornerX[i], CornerY[i], p[i])
				&& corner(CornerX[j], CornerY[j], p[j]))
				pSurface->DrawLine(&p[i], &p[j], color);
		}
	}
}
