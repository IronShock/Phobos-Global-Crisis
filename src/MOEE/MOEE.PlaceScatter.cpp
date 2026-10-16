#include "MOEE.h"

#include <Phobos.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <TargetClass.h>
#include <AbstractClass.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

#include <Windows.h>

// Port of MO_ExperienceEnhancements/src/PlaceScatter.cpp.
//
// When placing a building from the sidebar, pushes your own ground units that
// sit inside the foundation out of the way.
//   Mode 0 = disabled, 1 = clear on click (default, hovering the ghost does not
//            touch units), 2 = also chase the hovering ghost automatically.
//
// Read-only queries + standard Mission::Move events on OutList.
namespace MOEE::PlaceScatter
{
	namespace
	{
		constexpr unsigned int ADDR_DISPLAY = 0x87F7E8u; // DisplayClass::Instance
		constexpr unsigned int ADDR_CURPLAYER = 0xA83D4Cu;
		constexpr unsigned int OFF_MAP_CELLS = 0x13Cu;
		constexpr unsigned int OFF_MAP_MAXC = 0x140u;

		// Placement ghost on DisplayClass.
		constexpr unsigned int PS_OFF_CENTER = 0x1174u;  // CellStruct X/Y
		constexpr unsigned int PS_OFF_TOPLEFT = 0x1178u; // CellStruct X/Y
		constexpr unsigned int PS_OFF_DATA = 0x117Cu;    // CellStruct* (null = not placing)

		// Object arrays (DynamicVectorClass: items @ +4, count @ +16).
		constexpr unsigned int ARR_UNIT = 0x8B4108u;
		constexpr unsigned int ARR_INF = 0xA83DE8u;
		constexpr unsigned int ARR_ITEMS = 4u;
		constexpr unsigned int ARR_COUNT = 16u;

		constexpr unsigned int OFF_HEALTH = 0x6Cu;
		constexpr unsigned int OFF_INLIMBO = 0x81u;
		constexpr unsigned int OFF_ISALIVE = 0x90u;
		constexpr unsigned int OFF_OWNER = 0x21Cu;
		constexpr unsigned int OFF_HOUSE_INDEX = 0x30u;

		// Per-cell object chain: ground layer @ +0xE4, bridge layer @ +0xE8,
		// next object @ +0x30.
		constexpr unsigned int OFF_CELL_FIRST = 0xE4u;
		constexpr unsigned int OFF_CELL_ALT = 0xE8u;
		constexpr unsigned int OFF_OBJ_NEXT = 0x30u;
		constexpr unsigned int OFF_CELL_LAND = 0xECu;
		constexpr unsigned int OFF_CELL_WALLOWN = 0x50u;
		constexpr unsigned int OFF_CELL_ALTFLAGS = 0x12Cu;

		constexpr int LAND_WATER = 2;
		constexpr int LAND_ROCK = 3;
		constexpr int ABS_TERRAIN = 36;

		constexpr unsigned int EXE_LO = 0x401000u;
		constexpr unsigned int EXE_HI = 0x8A0000u;
		constexpr int LEP_PER_CELL = 256;

		constexpr int DATA_CAP = 120;
		constexpr int CELL_CAP = 128;
		constexpr int QCAP = 64;
		constexpr int BFS_CAP = 384;
		constexpr int BFS_R = 10;
		constexpr int CACHE_FRESH = 5;
		constexpr int EV_GUARD = 64;

		// Screen eight directions (isometric): N, NE, E, SE, S, SW, W, NW.
		const int DDX[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
		const int DDY[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };
		const int ROT[8] = { 0, 1, 7, 2, 6, 3, 5, 4 };

		int CacheCells[CELL_CAP][2];
		int CacheN = 0;
		int CacheFrame = -100000;

		void* Queue[QCAP];
		int QueueHead = 0;
		int QueueTail = 0;

		int BfsX[BFS_CAP];
		int BfsY[BFS_CAP];

		unsigned int EvNext = 0;
		bool EvInit = false;

		int LastFrame = -1;
		int LastAuto = -100000;

		bool InExe(const void* p)
		{
			const auto v = static_cast<unsigned int>(reinterpret_cast<size_t>(p));
			return v >= EXE_LO && v < EXE_HI;
		}

		bool IsObject(void* p)
		{
			if (!p || !IsPlausiblePtr(p))
				return false;

			return InExe(*reinterpret_cast<void**>(p));
		}

		AbstractType WhatAmI(void* p)
		{
			if (!IsObject(p))
				return AbstractType::None;

			return reinterpret_cast<AbstractClass*>(p)->WhatAmI();
		}

		// ---- ghost footprint -------------------------------------------------

		int ReadGhost(int cells[][2])
		{
			const auto display = reinterpret_cast<unsigned char*>(ADDR_DISPLAY);
			const short cx = *reinterpret_cast<short*>(display + PS_OFF_CENTER);
			const short cy = *reinterpret_cast<short*>(display + PS_OFF_CENTER + 2);
			const short ox = *reinterpret_cast<short*>(display + PS_OFF_TOPLEFT);
			const short oy = *reinterpret_cast<short*>(display + PS_OFF_TOPLEFT + 2);
			void* data = *reinterpret_cast<void**>(display + PS_OFF_DATA);

			if (!data)
				return 0;

			if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512)
				return 0;

			if (ox < -8 || ox > 8 || oy < -8 || oy > 8)
				return 0;

			if (!IsPlausiblePtr(data))
				return 0;

			const int bx = static_cast<int>(cx) + static_cast<int>(ox);
			const int by = static_cast<int>(cy) + static_cast<int>(oy);
			int n = 0;

			for (int k = 0; k < DATA_CAP && n < CELL_CAP; ++k)
			{
				const short dx = *reinterpret_cast<short*>(static_cast<char*>(data) + k * 4u);
				const short dy = *reinterpret_cast<short*>(static_cast<char*>(data) + k * 4u + 2u);

				if (dx == static_cast<short>(0x7FFF) && dy == static_cast<short>(0x7FFF))
					break;

				if (dx < -10 || dx > 10 || dy < -10 || dy > 10)
					break;

				const int gx = bx + static_cast<int>(dx);
				const int gy = by + static_cast<int>(dy);

				if (gx < 0 || gy < 0 || gx >= 512 || gy >= 512)
					continue;

				cells[n][0] = gx;
				cells[n][1] = gy;
				++n;
			}

			return n;
		}

		bool InSet(int cells[][2], int n, int cx, int cy)
		{
			for (int i = 0; i < n; ++i)
			{
				if (cells[i][0] == cx && cells[i][1] == cy)
					return true;
			}

			return false;
		}

		void BBox(int cells[][2], int nCell, int* x0, int* y0, int* x1, int* y1)
		{
			*x0 = 512; *y0 = 512; *x1 = -1; *y1 = -1;

			for (int k = 0; k < nCell; ++k)
			{
				if (cells[k][0] < *x0) *x0 = cells[k][0];
				if (cells[k][1] < *y0) *y0 = cells[k][1];
				if (cells[k][0] > *x1) *x1 = cells[k][0];
				if (cells[k][1] > *y1) *y1 = cells[k][1];
			}
		}

		// ---- passability -----------------------------------------------------

		void* CellPtr(int cx, int cy)
		{
			if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512)
				return nullptr;

			const auto display = reinterpret_cast<unsigned char*>(ADDR_DISPLAY);
			auto cells = *reinterpret_cast<void***>(display + OFF_MAP_CELLS);
			const int maxc = *reinterpret_cast<int*>(display + OFF_MAP_MAXC);

			if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512)
				return nullptr;

			const int idx = (cy << 9) + cx;

			if (idx < 0 || idx >= maxc)
				return nullptr;

			return cells[idx];
		}

		bool CellBlocked(int cx, int cy)
		{
			void* cell = CellPtr(cx, cy);

			if (!cell || !IsPlausiblePtr(cell))
				return true;

			auto raw = static_cast<char*>(cell);
			const int land = *reinterpret_cast<int*>(raw + OFF_CELL_LAND);

			if (land == LAND_WATER || land == LAND_ROCK)
				return true;

			if (*reinterpret_cast<int*>(raw + OFF_CELL_WALLOWN) != -1)
				return true;

			if (*reinterpret_cast<unsigned int*>(raw + OFF_CELL_ALTFLAGS) & 0x2u)
				return true;

			for (int k = 0; k < 2; ++k)
			{
				void* o = (k == 0) ? *reinterpret_cast<void**>(raw + OFF_CELL_FIRST)
					: *reinterpret_cast<void**>(raw + OFF_CELL_ALT);

				for (int n = 0; o && n < 32; ++n)
				{
					if (!IsPlausiblePtr(o))
						break;

					const auto what = static_cast<int>(WhatAmI(o));

					if (what == static_cast<int>(AbstractType::Building) || what == ABS_TERRAIN)
						return true;

					o = *reinterpret_cast<void**>(static_cast<char*>(o) + OFF_OBJ_NEXT);
				}
			}

			return false;
		}

		bool Pass(int x, int y)
		{
			return CellPtr(x, y) != nullptr && !CellBlocked(x, y);
		}

		bool DiagFree(int ux, int uy, int qx, int qy)
		{
			return Pass(ux + qx, uy) && Pass(ux, uy + qy);
		}

		bool CandOk(int cells[][2], int nCell, int x, int y)
		{
			return !InSet(cells, nCell, x, y) && Pass(x, y);
		}

		// ---- move order ------------------------------------------------------

		bool FireMove(int houseIndex, void* unit, int dstX, int dstY)
		{
			CellStruct cell;
			cell.X = static_cast<short>(dstX);
			cell.Y = static_cast<short>(dstY);

			EventClass event(houseIndex,
				TargetClass(reinterpret_cast<AbstractClass*>(unit)),
				Mission::Move,
				TargetClass(),
				TargetClass(cell),
				TargetClass());

			return EventClass::OutList.Add(event);
		}

		int ScreenIdx(int qx, int qy)
		{
			if (qy < 0)
				return (qx < 0) ? 7 : ((qx > 0) ? 1 : 0);

			if (qy > 0)
				return (qx < 0) ? 5 : ((qx > 0) ? 3 : 4);

			return (qx < 0) ? 6 : 2;
		}

		int Bfs(int cells[][2], int nCell, int ux, int uy, int base, int* ox, int* oy)
		{
			int head = 0, tail = 1;
			int order[8];
			BfsX[0] = ux;
			BfsY[0] = uy;

			for (int i = 0; i < 8; ++i)
				order[i] = (base + ROT[i]) & 7;

			while (head < tail)
			{
				const int cx = BfsX[head];
				const int cy = BfsY[head];
				++head;

				if (!(cx == ux && cy == uy) && !InSet(cells, nCell, cx, cy))
				{
					*ox = cx;
					*oy = cy;
					return 1;
				}

				for (int d = 0; d < 8; ++d)
				{
					const int qx = DDX[order[d]];
					const int qy = DDY[order[d]];
					const int nx = cx + qx;
					const int ny = cy + qy;

					if (nx < 0 || ny < 0 || nx >= 512 || ny >= 512)
						continue;

					if (qx && qy && !DiagFree(cx, cy, qx, qy))
						continue;

					int ax = nx - ux; if (ax < 0) ax = -ax;
					int ay = ny - uy; if (ay < 0) ay = -ay;

					if ((ax > ay ? ax : ay) > BFS_R)
						continue;

					int i = 0;
					for (; i < tail; ++i)
					{
						if (BfsX[i] == nx && BfsY[i] == ny)
							break;
					}

					if (i < tail)
						continue;

					if (!Pass(nx, ny))
						continue;

					if (tail >= BFS_CAP)
						return 0;

					BfsX[tail] = nx;
					BfsY[tail] = ny;
					++tail;
				}
			}

			return 0;
		}

		// ---- queue -----------------------------------------------------------

		void Push(void* o)
		{
			if (QueueTail < QCAP)
				Queue[QueueTail++] = o;
		}

		void ScanArray(unsigned int arrBase, void* cur, int cells[][2], int nCell)
		{
			auto items = *reinterpret_cast<void***>(arrBase + ARR_ITEMS);
			const int count = *reinterpret_cast<int*>(arrBase + ARR_COUNT);

			if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 20000)
				return;

			for (int i = 0; i < count; ++i)
			{
				void* o = items[i];

				if (QueueTail >= QCAP || !IsObject(o))
					return;

				const auto what = WhatAmI(o);

				if (what != AbstractType::Unit && what != AbstractType::Infantry)
					continue;

				auto raw = static_cast<unsigned char*>(o);

				if (*reinterpret_cast<void**>(raw + OFF_OWNER) != cur)
					continue;

				if (!raw[OFF_ISALIVE] || raw[OFF_INLIMBO])
					continue;

				if (*reinterpret_cast<int*>(raw + OFF_HEALTH) <= 0)
					continue;

				CoordStruct coords;

				if (!reinterpret_cast<AbstractClass*>(o)->GetCoords(&coords))
					continue;

				const int cx = coords.X / LEP_PER_CELL;
				const int cy = coords.Y / LEP_PER_CELL;

				if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512)
					continue;

				if (!InSet(cells, nCell, cx, cy))
					continue;

				Push(o);
			}
		}

		// Order each queued unit out: BFS for the nearest cell outside the
		// footprint, push a bit further out, then issue Move. Stops if OutList
		// is full (the rest is retried on the next trigger).
		int Drain(void* cur, int houseIndex, int cells[][2], int nCell, int cx2, int cy2)
		{
			const int batch = Phobos::Config::MOEE_PlaceScatter_Batch;
			const int dist = Phobos::Config::MOEE_PlaceScatter_Dist;
			int sent = 0;

			while (QueueHead < QueueTail && sent < batch)
			{
				void* o = Queue[QueueHead++];

				if (!IsObject(o))
					continue;

				auto raw = static_cast<unsigned char*>(o);

				if (*reinterpret_cast<void**>(raw + OFF_OWNER) != cur)
					continue;

				CoordStruct coords;

				if (!reinterpret_cast<AbstractClass*>(o)->GetCoords(&coords))
					continue;

				const int ux = coords.X / LEP_PER_CELL;
				const int uy = coords.Y / LEP_PER_CELL;

				if (!InSet(cells, nCell, ux, uy))
					continue; // already outside

				int dx, dy;
				{
					const int ddx = (ux * 2 + 1) - cx2;
					const int ddy = (uy * 2 + 1) - cy2;
					const int sx = ddx - ddy;
					const int sy = ddx + ddy;
					dx = (sx > 0) ? 1 : ((sx < 0) ? -1 : 0);
					dy = (sy > 0) ? 1 : ((sy < 0) ? -1 : 0);
				}

				if (!dx && !dy)
					dx = 1;

				int ox = 0, oy = 0;

				if (!Bfs(cells, nCell, ux, uy, ScreenIdx(dx, dy), &ox, &oy))
					continue;

				const int ex = (ox > ux) ? 1 : ((ox < ux) ? -1 : 0);
				const int ey = (oy > uy) ? 1 : ((oy < uy) ? -1 : 0);

				for (int st = 1; st < dist; ++st)
				{
					const int nx = ox + ex;
					const int ny = oy + ey;

					if (!CandOk(cells, nCell, nx, ny))
						break;

					ox = nx;
					oy = ny;
				}

				if (!FireMove(houseIndex, o, ox, oy))
					return sent;

				++sent;
			}

			return sent;
		}

		int ClearFoot(void* cur, int houseIndex, int cells[][2], int nCell)
		{
			if (nCell <= 0)
				return 0;

			int x0, y0, x1, y1;
			BBox(cells, nCell, &x0, &y0, &x1, &y1);
			const int cx2 = x0 + x1 + 1;
			const int cy2 = y0 + y1 + 1;

			QueueHead = 0;
			QueueTail = 0;
			ScanArray(ARR_UNIT, cur, cells, nCell);
			ScanArray(ARR_INF, cur, cells, nCell);

			if (QueueTail <= 0)
				return 0;

			return Drain(cur, houseIndex, cells, nCell, cx2, cy2);
		}

		// Watch OutList for our own Place events; a placement clears the ghost
		// immediately, so the cached footprint is the only record of what was
		// under the building.
		void WatchPlace(void* cur, int houseIndex, int fr)
		{
			auto& outList = EventClass::OutList;
			const unsigned int next = static_cast<unsigned int>(outList.GetTail());

			if (!EvInit)
			{
				EvNext = next;
				EvInit = true;
				return;
			}

			const unsigned int adv = (next - EvNext) & 0x7Fu;

			if (!adv)
				return;

			if (adv > EV_GUARD) // wrapped more than half (load/menu): resync
			{
				EvNext = next;
				return;
			}

			const int nCell = (CacheN < CELL_CAP) ? CacheN : CELL_CAP;
			const int d = fr - CacheFrame;
			EventClass* array = outList.GetArray();
			unsigned int p = EvNext;

			for (int guard = 0; p != next && guard < EV_GUARD; ++guard)
			{
				EventClass& e = array[p];

				if (e.Type == EventType::Place && static_cast<int>(e.HouseIndex) == houseIndex)
				{
					int fire = 0;

					if (nCell > 0 && d >= 0 && d <= CACHE_FRESH)
					{
						const int ex = e.Place.Location.X;
						const int ey = e.Place.Location.Y;

						if (InSet(CacheCells, nCell, ex, ey))
						{
							fire = 1;
						}
						else
						{
							int x0, y0, x1, y1;
							BBox(CacheCells, nCell, &x0, &y0, &x1, &y1);

							if (ex >= x0 && ex <= x1 && ey >= y0 && ey <= y1)
								fire = 1;
						}
					}

					if (fire)
						ClearFoot(cur, houseIndex, CacheCells, nCell);
				}

				p = (p + 1u) & 0x7Fu;
			}

			EvNext = next;
		}
	}

	void Poll()
	{
		if (Phobos::Config::MOEE_PlaceScatter == 0)
			return;

		const int fr = Unsorted::CurrentFrame;

		if (fr == LastFrame)
			return; // host hook runs several times per frame

		LastFrame = fr;

		void* cur = *reinterpret_cast<void**>(ADDR_CURPLAYER);

		if (!cur || !IsPlausiblePtr(cur))
			return;

		const int houseIndex = *reinterpret_cast<int*>(static_cast<char*>(cur) + OFF_HOUSE_INDEX);

		int cells[CELL_CAP][2];
		const int nCell = ReadGhost(cells);

		if (nCell > 0)
		{
			CacheN = (nCell < CELL_CAP) ? nCell : CELL_CAP;

			for (int k = 0; k < CacheN; ++k)
			{
				CacheCells[k][0] = cells[k][0];
				CacheCells[k][1] = cells[k][1];
			}

			CacheFrame = fr;

			if (Phobos::Config::MOEE_PlaceScatter >= 2
				&& fr - LastAuto >= Phobos::Config::MOEE_PlaceScatter_Interval)
			{
				LastAuto = fr;
				ClearFoot(cur, houseIndex, cells, nCell);
			}
		}

		WatchPlace(cur, houseIndex, fr);
	}
}
