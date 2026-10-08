#pragma once
#include <AircraftClass.h>

#include <vector>

// TODO: Implement proper extended AircraftClass.

class HouseClass;
class AbstractClass;
class AircraftClass;
class PhobosStreamReader;
class PhobosStreamWriter;

// Shared temporary state used to pass the SpyPlane superweapon's SpawnPoints
// waypoint cells from HouseClass::SendSpyPlanes to the aircraft placement hook.
// One cell is randomly picked for each aircraft at placement time.
namespace SpyPlaneSpawnPointTemp
{
	inline std::vector<CellStruct> SpawnPoints;

	// Indices of SpawnPoints already handed out to aircraft of the current
	// SpyPlane summon, so a new plane never reuses a waypoint while unused
	// distinct ones remain.
	inline std::vector<int> UsedSpawnPointIndices;

	// A SpyPlane superweapon summon that has been delayed by SpawnPoints.Delay.
	// The original HouseClass::SendSpyPlanes call is skipped and re-invoked
	// with these captured parameters once the expiry frame is reached.
	struct DeferredSendSpyPlanes
	{
		int ExpireFrame;
		HouseClass* pHouse;
		int AircraftTypeIdx;
		int Count;
		int Mission;
		AbstractClass* pTarget;
		AbstractClass* pDestination;
	};

	inline std::vector<DeferredSendSpyPlanes> Pending;
	inline bool InDeferredCall = false;
}

// Shared temporary state for the ParaDrop SpawnPoints feature. Ares creates
// para-drop planes through the exe AircraftClass ctor (never reaching the
// vanilla HouseClass::SendParadropPlanes funnel), so the ctor hook records the
// planes here and a per-frame hook relocates them to their SpawnPoints waypoint
// cell shortly after Ares has placed them.
namespace ParaDropSpawnPointTemp
{
	// A para-drop aircraft that should enter from a SpawnPoints waypoint cell
	// instead of the map edge.
	struct PendingAircraftReloc
	{
		AircraftClass* Aircraft;
		CellStruct Cell;
		int WaitFrames = 0;
		int ExpireFrame = 0;      // >0: defer the relocation until this frame (SpawnPoints.Delay)
	};

	inline std::vector<PendingAircraftReloc> PendingReloc;
}

class AircraftExt
{
public:
	static void FireWeapon(AircraftClass* pThis, AbstractClass* pTarget);
	static bool PlaceReinforcementAircraft(AircraftClass* pThis, CellStruct edgeCell);
	static DirType GetLandingDir(AircraftClass* pThis, BuildingClass* pDock = nullptr);

	static void Clear();
	static void PointerGotInvalid(void* ptr, bool removed);
	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);
};
