#include <Helpers/Macro.h>
#include <AStarClass.h>
#include <CellClass.h>
#include <FootClass.h>
#include <Unsorted.h>
#include <unordered_map>

#include <Ext/Rules/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// Pathfinding failure throttle: a foot that keeps failing to reach its
// destination would otherwise re-run the full A* (the whole-map "A* without HS"
// search) every frame, which saturates the A* node buffer and floods the debug
// log. After 3 consecutive failures the unit enters a 90-frame cooldown during
// which FindPath returns "no path" without touching A* at all; once the cooldown
// expires retries are allowed again. Fixed constants (3 failures / 90 frames).
static std::unordered_map<FootClass*, std::pair<int, int>> s_pathFail;

// Straight-steer decision cache, keyed by the foot. Shared file scope so it can
// be dropped on scenario clear (otherwise keys from the previous scenario stay
// behind and can alias newly allocated feet).
struct StraightCacheEntry
{
	int Frame = -1000000;
	CellStruct CurCell { -1, -1 };
	CellStruct DestCell { -1, -1 };
	bool Result = false;
	CoordStruct Dest;
};

static std::unordered_map<FootClass*, StraightCacheEntry> s_straightCache;

void TechnoExt::ClearPathfindingCaches()
{
	s_pathFail.clear();
	s_straightCache.clear();
}

static bool PathFailCooldownActive(FootClass* pThis, int frame)
{
	auto& track = s_pathFail[pThis];

	if (frame - track.second >= 90)
		track.first = 0; // cooldown expired, allow retries

	return track.first >= 3 && frame - track.second < 90;
}

static void PathFailPrune(int frame)
{
	if (s_pathFail.size() <= 1024)
		return;

	for (auto it = s_pathFail.begin(); it != s_pathFail.end();)
	{
		if (frame - it->second.second > 300)
			it = s_pathFail.erase(it);
		else
			++it;
	}
}

// TA.VirtualUnit: virtual TA child units never pathfind. FootClass::FindPath
// (0x4D3920) is the entry used by all movement missions. Hook right after the
// stack probe / prologue where ECX still holds `this` and EBP = this is set up
// (0x4D3934, `test eax,eax; mov ebp,ecx; jne`), and for virtual units jump to
// the vanilla "no path" exit (0x4D3989) which clears PathDirections and returns.
// Also applies the pathfinding failure throttle here.
DEFINE_HOOK(0x4D3934, FootClass_FindPath_VirtualUnit, 0x6)
{
	enum { NoPathExit = 0x4D3989 };

	GET(FootClass* const, pThis, ECX);

	if (TechnoExt::IsVirtualUnit(pThis))
	{
		// Replicate the overwritten `mov ebp, ecx` so the no-path exit's
		// `mov [ebp+0x5E0], -1` writes to this, then set EAX = 0 (no path).
		R->EBP(reinterpret_cast<uintptr_t>(pThis));
		R->EAX(0);
		return NoPathExit;
	}

	// Skip the A* entirely while the unit is in its failure cooldown.
	PathFailPrune(Unsorted::CurrentFrame);

	if (PathFailCooldownActive(pThis, Unsorted::CurrentFrame))
	{
		R->EBP(reinterpret_cast<uintptr_t>(pThis));
		R->EAX(0);
		return NoPathExit;
	}

	return 0;
}

// FootClass::FindPath failure exit (0x4D3989, `mov [ebp+0x5E0], -1`, a full
// 10-byte instruction): count the failure and stamp the current frame so the
// entry throttle can decide when to back off. EBP = this (FootClass).
DEFINE_HOOK(0x4D3989, FootClass_FindPath_FailureTrack, 0xA)
{
	GET(FootClass* const, pThis, EBP);

	auto& track = s_pathFail[pThis];

	// While already in the cooldown, do not re-stamp the timestamp, otherwise
	// `frame - track.second < 90` never becomes false and the cooldown would
	// never expire, leaving the unit permanently unable to pathfind.
	if (track.first >= 3 && Unsorted::CurrentFrame - track.second < 90)
		return 0;

	track.first = track.first < 3 ? track.first + 1 : 3;
	track.second = Unsorted::CurrentFrame;

	return 0;
}

// [General]SmoothMove=yes (per-unit override: [<TechnoType>]SmoothMove=yes/no):
// after the A* finds a path, straighten it with Bresenham string-pulling so the
// unit moves as directly as possible toward its target instead of zig-zagging.
// Starting at each kept cell the farthest hop whose straight(-ish) Bresenham line
// is fully passable for the unit (checked via the unit's own Can_Enter_Cell,
// applying the game's diagonal corner rule) is taken, then the rewritten
// Directions are written back in place.
//
// Direction encoding (YR facing, clockwise from North): 0=N,1=NE,2=E,3=SE,4=S,
// 5=SW,6=W,7=NW. PathFinderData layout is matched by raw offset (private in YRpp).

static const int kSmoothDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int kSmoothDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

// PathFinderData layout (its members are private in YRpp), matched by raw offset.
struct SmPathData
{
	CellStruct StartCell;  // +0x00
	int TotalDistance;     // +0x04
	int PathLength;        // +0x08
	int* Directions;       // +0x0C
};

static int Abs(int v)
{
	return v < 0 ? -v : v;
}

static int DirFromDelta(int dx, int dy)
{
	if (dx == 0 && dy == 0)
		return -1;

	if (dx == 0)
		return dy > 0 ? 4 : 0; // S / N
	if (dy == 0)
		return dx > 0 ? 2 : 6; // E / W
	if (dx == dy)
		return dx > 0 ? 3 : 7; // SE / NW
	if (dx == -dy)
		return dx > 0 ? 1 : 5; // NE / SW

	return -1;
}

static int CellKey(int x, int y)
{
	return (y << 16) | (x & 0xFFFF);
}

static bool CellPassable(FootClass* pFoot, int cx, int cy, std::unordered_map<int, bool>& cache)
{
	const int key = CellKey(cx, cy);
	auto const it = cache.find(key);
	if (it != cache.end())
		return it->second;

	bool ok = false;
	if (pFoot && pFoot->Locomotor)
	{
		const CellStruct cell { static_cast<short>(cx), static_cast<short>(cy) };
		ok = pFoot->Locomotor->Can_Enter_Cell(cell) == Move::OK;
	}

	cache.emplace(key, ok);
	return ok;
}

// A diagonal step into (x+dx, y+dy) is legal if the diagonal cell is enterable
// and at least one of the two orthogonal neighbours is enterable (the game's
// corner rule, otherwise the unit would squeeze between two obstacles).
static bool DiagonalStepPassable(FootClass* pFoot, int x, int y, int dx, int dy, std::unordered_map<int, bool>& cache)
{
	if (!CellPassable(pFoot, x + dx, y + dy, cache))
		return false;

	return CellPassable(pFoot, x + dx, y, cache) || CellPassable(pFoot, x, y + dy, cache);
}

// True if the straight(-ish) line from (x0,y0) to (x1,y1) is fully passable.
// The destination cell itself is NOT validated - it is already on the A* path
// and may be the occupied arrival cell. On failure pBlocked (if non-null)
// receives the cell that rejected the line.
static bool BresenhamClear(FootClass* pFoot, int x0, int y0, int x1, int y1, std::unordered_map<int, bool>& cache, CellStruct* pBlocked = nullptr)
{
	const int dx = Abs(x1 - x0);
	const int sx = x0 < x1 ? 1 : -1;
	const int dy = -Abs(y1 - y0);
	const int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int x = x0, y = y0;

	for (;;)
	{
		if (x == x1 && y == y1)
			return true;

		const int e2 = 2 * err;
		int nx = x, ny = y;

		if (e2 >= dy)
		{
			err += dy;
			nx += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			ny += sy;
		}

		if (nx == x1 && ny == y1)
			return true; // reached the endpoint, which is not validated here

		if (nx != x && ny != y)
		{
			if (!DiagonalStepPassable(pFoot, x, y, nx - x, ny - y, cache))
			{
				if (pBlocked)
					*pBlocked = CellStruct { static_cast<short>(nx), static_cast<short>(ny) };

				return false;
			}
		}
		else if (!CellPassable(pFoot, nx, ny, cache))
		{
			if (pBlocked)
				*pBlocked = CellStruct { static_cast<short>(nx), static_cast<short>(ny) };

			return false;
		}

		x = nx;
		y = ny;
	}
}

// Writes the 8-directional steps from (x0,y0) to (x1,y1) into out (excluding the
// start cell). Returns the number of steps written (bounded by maxOut).
static int EmitBresenhamDirs(int x0, int y0, int x1, int y1, int* out, int maxOut)
{
	const int dx = Abs(x1 - x0);
	const int sx = x0 < x1 ? 1 : -1;
	const int dy = -Abs(y1 - y0);
	const int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int x = x0, y = y0;
	int n = 0;

	while (x != x1 || y != y1)
	{
		const int e2 = 2 * err;
		int nx = x, ny = y;

		if (e2 >= dy)
		{
			err += dy;
			nx += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			ny += sy;
		}

		const int dir = DirFromDelta(nx - x, ny - y);
		if (dir < 0 || n >= maxOut)
			break;

		out[n++] = dir;
		x = nx;
		y = ny;
	}

	return n;
}

// Hook right before the path directions are copied into PathDirections inside
// 0x4D3D90 (the movement pathfinding consumer), at 0x4D3E94. The instructions
// there are `lea esi,[esp+0x6C]; lea edi,[ebp+ebx*4+0x5E0]; rep movsd`, which
// copy ECX dwords from [esp+0x6C] (the real directions buffer the unit follows)
// into this->PathDirections[ebx] (+0x5E0). We rewrite the source buffer in
// place and set ECX to the smoothed length, so the game copies our line.
//
// At this point: ECX = copy count (min(PathLength, 24-ebx), always <= 24),
// EBP = this (FootClass), [esp+0x6C] = source directions,
// [esp+0x14] = PathFinderData* (StartCell at +0). Size 0xB (11) ends on an
// instruction boundary (lea esi = 4 bytes + lea edi = 7 bytes, before rep movsd).
DEFINE_HOOK(0x4D3E94, FootClass_FindPath_SmoothMove, 0xB)
{
	GET(FootClass* const, pFoot, EBP);
	GET(int, count, ECX);
	GET_STACK(PathFinderData*, pRawPath, 0x14);
	LEA_STACK(int*, pSrcDirs, 0x6C);

	if (!pFoot || !pFoot->Locomotor || !pSrcDirs || count <= 1 || count > 25)
		return 0;

	auto const pRules = RulesExt::Global();
	if (!pRules)
		return 0;

	const bool useSmoothMove = TechnoTypeExt::ExtMap.Find(pFoot->GetTechnoType())->SmoothMove.Get(pRules->SmoothMove);
	if (!useSmoothMove)
		return 0;

	const int startX = pRawPath ? static_cast<int>(reinterpret_cast<SmPathData*>(pRawPath)->StartCell.X) : 0;
	const int startY = pRawPath ? static_cast<int>(reinterpret_cast<SmPathData*>(pRawPath)->StartCell.Y) : 0;

	// Reconstruct the cell path from the source directions.
	int cellsX[26];
	int cellsY[26];
	int x = startX;
	int y = startY;
	cellsX[0] = x;
	cellsY[0] = y;

	for (int i = 0; i < count; i++)
	{
		const int d = pSrcDirs[i];
		if (d < 0 || d > 7)
			return 0;

		x += kSmoothDirX[d];
		y += kSmoothDirY[d];
		cellsX[i + 1] = x;
		cellsY[i + 1] = y;
	}

	// Passability cache for the whole pass; many cells are re-tested across hops.
	std::unordered_map<int, bool> passable;
	passable.reserve(count * 2 + 8);

	// Greedy string-pulling: from each kept cell take the farthest visible hop
	// and emit its Bresenham steps. Output can never exceed count (each hop is
	// the 8-distance-shortest line between its endpoints).
	int outDir[25];
	int outCount = 0;
	int i = 0;

	while (i < count && outCount < 25)
	{
		int bestJ = i + 1;

		for (int j = count; j > i + 1; j--)
		{
			if (BresenhamClear(pFoot, cellsX[i], cellsY[i], cellsX[j], cellsY[j], passable))
			{
				bestJ = j;
				break;
			}
		}

		const int n = EmitBresenhamDirs(cellsX[i], cellsY[i], cellsX[bestJ], cellsY[bestJ], outDir + outCount, 25 - outCount);
		if (n <= 0)
			return 0; // safety: never happens for a valid hop

		outCount += n;
		i = bestJ;
	}

	// Write the smoothed directions back into the copy source and adjust the count.
	// Note: write back even when outCount == count - the A* path can be a clumpy
	// staircase of the same length as the (straighter) Bresenham line. outCount
	// can never exceed count (each hop is the 8-distance-shortest line between
	// its endpoints), so <= is always safe.
	if (outCount > 0 && outCount <= count)
	{
		for (int k = 0; k < outCount; k++)
			pSrcDirs[k] = outDir[k];

		R->ECX(outCount);
	}

	return 0;
}

// ============================================================================
// B1: coordinate-level straight steering.
// When a foot has a destination and the straight line from its cell to the
// destination cell is fully passable, drive it straight at the destination
// coordinate instead of following the per-cell path (which would zig-zag).
// This covers both the vanilla DriveLocomotion and AdvancedDriveLocomotion,
// since it hooks the shared per-frame FootClass::Update.
// ============================================================================

// Returns the destination coordinate to steer straight at, or CoordStruct::Empty
// when the unit should keep following its path.
static CoordStruct GetStraightDestination(FootClass* pFoot, int currentFrame)
{
	if (!pFoot || !pFoot->Locomotor || !pFoot->Destination)
		return CoordStruct::Empty;

	// Gate on SmoothMove (global + per-type override).
	auto const pRules = RulesExt::Global();
	if (!pRules)
		return CoordStruct::Empty;

	if (!TechnoTypeExt::ExtMap.Find(pFoot->GetTechnoType())->SmoothMove.Get(pRules->SmoothMove))
		return CoordStruct::Empty;

	// Skip trains (they follow fixed tracks) and non-drive states.
	if (pFoot->GetTechnoType()->IsTrain)
		return CoordStruct::Empty;

	// Skip when disabled; Move_To would ignore these anyway, but skip early.
	if (pFoot->IsUnderEMP() || pFoot->IsParalyzed() || pFoot->IsBeingWarpedOut() || pFoot->IsWarpingIn())
		return CoordStruct::Empty;

	auto const destCoord = pFoot->Destination->GetDestination(pFoot);
	if (destCoord == CoordStruct::Empty)
		return CoordStruct::Empty;

	auto const curCell = pFoot->CurrentMapCoords;
	auto const destCell = CellClass::Coord2Cell(destCoord);

	if (destCell.X == curCell.X && destCell.Y == curCell.Y)
		return CoordStruct::Empty;

	// Cache the decision; recheck when either endpoint's cell changes or every 30 frames.
	auto& entry = s_straightCache[pFoot];

	if (currentFrame - entry.Frame < 30
		&& entry.CurCell.X == curCell.X && entry.CurCell.Y == curCell.Y
		&& entry.DestCell.X == destCell.X && entry.DestCell.Y == destCell.Y)
	{
		return entry.Result ? entry.Dest : CoordStruct::Empty;
	}

	std::unordered_map<int, bool> passable;
	entry.Result = BresenhamClear(pFoot, curCell.X, curCell.Y, destCell.X, destCell.Y, passable);
	entry.Dest = destCoord;
	entry.Frame = currentFrame;
	entry.CurCell = curCell;
	entry.DestCell = destCell;

	// Prune stale entries so dead units don't grow the cache forever.
	if (s_straightCache.size() > 2048)
	{
		for (auto it = s_straightCache.begin(); it != s_straightCache.end();)
		{
			if (currentFrame - it->second.Frame > 300)
				it = s_straightCache.erase(it);
			else
				++it;
		}
	}

	return entry.Result ? entry.Dest : CoordStruct::Empty;
}

// Per-frame FootClass::Update, right before the locomotion's per-frame call.
// ESI = this (FootClass). Covers vanilla and AdvancedDrive locomotion.
DEFINE_HOOK(0x4DA8B2, FootClass_Update_StraightSteer, 0x6)
{
	GET(FootClass* const, pThis, ESI);

	auto const dest = GetStraightDestination(pThis, Unsorted::CurrentFrame);

	if (dest != CoordStruct::Empty)
	{
		// No Move_To here: it resets the FacingComputer's turn-track state and
		// causes deviation. The FacingComputer entry hook (0x4B2630) writes the
		// target directly instead.
	}

	return 0;
}

// FacingComputer entry: aim the turn-track directly at the straight destination,
// bypassing Move_To (which resets the track state and causes long-range drift).
// ECX = locomotion, [ECX+0xC] = linked FootClass, [ECX+0x34..0x3C] = target coord.
DEFINE_HOOK(0x4B2630, DriveLocomotion_FacingComputer_StraightTarget, 0x7)
{
	GET(uintptr_t, locoReg, ECX);

	FootClass* const pFoot = *reinterpret_cast<FootClass**>(locoReg + 0xC);
	auto const dest = GetStraightDestination(pFoot, Unsorted::CurrentFrame);

	if (dest != CoordStruct::Empty)
		*reinterpret_cast<CoordStruct*>(locoReg + 0x34) = dest;

	return 0;
}
