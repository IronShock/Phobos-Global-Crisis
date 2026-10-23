#pragma once

#include <UnitClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <CellClass.h>
#include <Dir.h>

#include <array>
#include <vector>

// Multi-cell unit footprint.
//
// The unit still has exactly one real object. The footprint is a set of cell
// offsets around the unit's anchor cell, precomputed for each of the 8 facings.
// Occupancy is represented by CellClass::OccupationFlags (0x20) plus a global
// per-cell owner grid, so the engine's existing passability checks keep working.
enum class FootprintTurnMode : int
{
	Pivot = 0,
	Radius = 1
};

struct FootprintConfig
{
	bool Enabled = false;
	int Length = 1;       // cells along the facing direction
	int Width = 1;        // cells across the facing direction
	bool Rotates = true;  // false: footprint stays axis-aligned (e.g. 3x3)
	FootprintTurnMode TurnMode = FootprintTurnMode::Pivot;
	bool TurnAssist = true;
	bool Crush = true;
	int FireArc = 0;      // 0 = keep the vanilla firing arc

	// Offset of the rectangle centre from the anchor cell, in cells, along the
	// hull's local axes. Reserved for even-sized hulls (e.g. 7x2); 0 for now.
	int PivotForward = 0;
	int PivotLateral = 0;
};

struct FootprintGeometry
{
	bool Built = false;
	std::array<std::vector<CellStruct>, 8> Span;
	// [from * 8 + to], using the continuous sweep between the two facing angles.
	std::array<std::vector<CellStruct>, 64> SweepCW;
	std::array<std::vector<CellStruct>, 64> SweepCCW;
	std::array<CellStruct, 8> Min;
	std::array<CellStruct, 8> Max;

	void Build(const FootprintConfig& config);
};

struct FootprintState
{
	bool Initialized = false;
	bool Locked = false;
	int Bucket = 0;
	CellStruct Anchor { 0, 0 };
	bool Alt = false;
	std::vector<CellStruct> Span;
};

class Footprint
{
public:
	static bool IsMultiCell(TechnoClass* pThis);
	static bool IsMultiCell(TechnoTypeClass* pType);
	static const FootprintConfig& GetConfig(TechnoTypeClass* pType);
	static const FootprintGeometry& GetGeometry(TechnoTypeClass* pType);
	static FootprintState* GetState(TechnoClass* pThis);

	static int BucketFromFacing(const DirStruct& facing);
	static CellStruct AnchorFromCoords(const CoordStruct& coords);
	static std::vector<CellStruct> GetSpan(TechnoClass* pThis, const CellStruct& anchor, int bucket);
	static std::vector<CellStruct> GetSweep(TechnoClass* pThis, int from, int to, bool clockwise);
	static std::vector<CellStruct> GetSweepAt(TechnoClass* pThis, const CellStruct& anchor, int from, int to, bool clockwise);

	static UnitClass* OwnerAt(const CellStruct& cell, bool alt);
	static UnitClass* OwnerAt(const CellClass* pCell, bool alt);
	static UnitClass* FindOwnerByScan(const CellStruct& cell, bool alt);
	static void SetOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);
	static void ClearOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);

	static bool CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, CellStruct* pBlocked = nullptr);
	static bool CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, const CellStruct& anchor, CellStruct* pBlocked = nullptr);
	static bool Mark(UnitClass* pThis, const CoordStruct& coords);
	static bool Unmark(UnitClass* pThis);
	static bool CommitSpan(UnitClass* pThis, const std::vector<CellStruct>& span, int bucket, bool alt, const CellStruct& anchor);
	static void Release(UnitClass* pThis);
	static void ClearAll();
	static void FixupAfterLoad();
	static void EnsureLocomotor(UnitClass* pThis);

	static int GetYSort(TechnoClass* pThis);
	static bool IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt);
};
