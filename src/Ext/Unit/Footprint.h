#pragma once

#include <UnitClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <CellClass.h>
#include <Dir.h>

class AircraftClass;
class BuildingClass;

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
	// Damage each occupied hull cell independently instead of collapsing the
	// owner into one hit per damage pass.
	bool PerCellDamage = false;
	// Once any actual hull cell is hit, force every actual hull cell through
	// independent damage calculation, even outside the warhead's CellSpread.
	bool HitAllCells = false;
	// Maximum height-level difference allowed inside the footprint and between
	// consecutive movement cells. 0 keeps the old strict flat-footprint rule.
	int MaxClimb = 1;

	// Offset of the rectangle centre from the anchor cell, in cells, along the
	// hull's local axes. Useful for even-sized hulls or art whose origin is not
	// the geometric centre.
	double PivotForward = 0.0;
	double PivotLateral = 0.0;
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
	// True while the locomotor is interpolating between two cell centres.
	// Engine Mark/Unmark calls at intermediate positions must not retarget the
	// footprint to the cell currently under the model.
	bool Translating = false;
	int Bucket = 0;
	CellStruct Anchor { 0, 0 };
	bool Alt = false;
	std::vector<CellStruct> Span;
	std::vector<CellStruct> BarrierSpan;
	// Transient no-pass cells for the next 45-degree turn stage: the actual
	// hull area swept while rotating to the next bucket, excluding cells the
	// current hull/barrier already covers. They block other units but are never
	// selectable/damageable.
	std::vector<CellStruct> ReservedSpan;

	// Mobile carrier payload slots. These are ordinary AircraftClass objects,
	// not SpawnManager nodes; they stay selectable and are positioned manually
	// at the carrier's DockingOffset points.
	bool CarrierPayloadsInitialized = false;
	BuildingClass* CarrierDockBuilding = nullptr;
	// One single-dock proxy per parking slot. Native GetDockCoords for a
	// NumberOfDocks==1 building returns the building position directly and
	// does not depend on an aircraft's radio-link index.
	std::vector<BuildingClass*> CarrierDockBuildings;
	std::vector<AircraftClass*> CarrierPayloads;
	std::vector<unsigned char> CarrierPayloadDocked;
	std::vector<CoordStruct> CarrierPayloadLastCoords;
	CoordStruct CarrierLastCoords {};
	int CarrierLastBucket = -1;

	// Aim reconciliation: the visual hull rotated past a bucket boundary while
	// the unit was stationary. The swept area is reserved until the rotation
	// reaches ReconcileTarget.
	bool ReconcileActive = false;
	int ReconcileTarget = -1;
	bool ReconcileClockwise = true;
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
	static UnitClass* BarrierOwnerAt(const CellStruct& cell, bool alt);
	static UnitClass* BarrierOwnerAt(const CellClass* pCell, bool alt);
	static void SetBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);
	static void ClearBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);
	static std::vector<CellStruct> ComputeBarriers(const std::vector<CellStruct>& span);
	static void SetOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);
	static void ClearOwner(const CellStruct& cell, bool alt, UnitClass* pOwner);

	static bool ReserveTurnStep(UnitClass* pThis, const CellStruct& anchor, int from, int to, bool clockwise, bool alt);
	static bool CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, CellStruct* pBlocked = nullptr);
	static bool CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, const CellStruct& anchor, CellStruct* pBlocked = nullptr);
	static bool IsClimbAllowed(UnitClass* pThis, const CellStruct& from, const CellStruct& to);
	static void EnsureAircraftCarrierSpawns(UnitClass* pThis);
	static UnitClass* GetCarrierPayloadOwner(TechnoClass* pPayload);
	static bool IsCarrierPayloadParked(TechnoClass* pPayload);
	static bool Mark(UnitClass* pThis, const CoordStruct& coords);
	static bool Unmark(UnitClass* pThis, const CoordStruct* pCoords = nullptr);
	static bool CommitSpan(UnitClass* pThis, const std::vector<CellStruct>& span, int bucket, bool alt, const CellStruct& anchor);
	static bool ReserveSweep(UnitClass* pThis, const std::vector<CellStruct>& sweep, bool alt);
	static void ReleaseReservation(UnitClass* pThis);
	static void Release(UnitClass* pThis);
	static void ClearAll();
	static void FixupAfterLoad();
	static void EnsureLocomotor(UnitClass* pThis);
	static void ReconcileFacing(UnitClass* pThis);

	static int GetYSort(TechnoClass* pThis);
	static bool IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt);
	static bool HasObstacleNear(TechnoClass* pThis, int radius);
};
