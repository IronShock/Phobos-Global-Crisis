#pragma once

#include <UnitClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <CellClass.h>
#include <Dir.h>

class AircraftClass;
class BuildingClass;
class PhobosStreamReader;
class PhobosStreamWriter;

#include <array>
#include <cstdint>
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

enum class CarrierRunwayPhase : unsigned char
{
	Idle = 0,
	WaitingForRunway,
	TaxiToA,
	AlignAtA,
	TakeoffRoll,
	LandingApproach,
	LandingRoll,
	TaxiToSlot,
	TaxiToStaging,
	TakeoffDeparture
};

// Fully manual landing controller phases. The engine's Move/Enter/Return
// missions are not used at any point; these phases are driven entirely by
// SetLocation/facing/speed from Footprint.cpp.
enum class CarrierLandingPhase : unsigned char
{
	None = 0,
	ReturnToApproach,
	OrbitWait,
	Lineup,
	FinalApproach,
	Touchdown,
	LandingRoll,
	TaxiToSlot,
	GoAround
};

inline constexpr unsigned int CarrierRunwaySaveMagic = 0x52574E31; // 'RWN1'
// Version 2 stores independent per-runway state arrays. Version 1 is still
// readable and is migrated to runway 0 on load.
inline constexpr unsigned int CarrierRunwaySaveVersion = 2;

// Appended after the runway block. Kept separate so saves written before the
// manual landing controller (runway version <= 2) remain readable.
inline constexpr unsigned int CarrierLandingSaveMagic = 0x4C4E4431; // 'LND1'
inline constexpr unsigned int CarrierLandingSaveVersion = 1;

// A landing aircraft that has already stopped at Z and is taxiing back to its
// slot. It no longer occupies the runway, so the next landing may start while
// this aircraft is still moving.
struct CarrierRunwayTaxiTask
{
	AircraftClass* Aircraft = nullptr;
	int Slot = -1;
	CoordStruct Target {};
	std::vector<CoordStruct> Path;
	size_t PathIndex = 0;
	int TurnTimer = 0;
	double Speed = 0.0;
	bool TrailEnabled = false;

	template <typename T>
	void Serialize(T& Stm)
	{
		Stm
			.Process(this->Aircraft)
			.Process(this->Slot)
			.Process(this->Target)
			.Process(this->Path)
			.Process(this->PathIndex)
			.Process(this->TurnTimer)
			.Process(this->Speed)
			.Process(this->TrailEnabled);
	}

	template <typename T>
	bool Load(T& Stm, bool RegisterForChange)
	{
		(void)RegisterForChange;
		this->Serialize(Stm);
		return true;
	}

	template <typename T>
	bool Save(T& Stm) const
	{
		const_cast<CarrierRunwayTaxiTask*>(this)->Serialize(Stm);
		return true;
	}
};

// An aircraft that has left Z and is still flying the straight departure
// climb. It no longer occupies the runway, so the next aircraft may start.
struct CarrierRunwayDeparture
{
	AircraftClass* Aircraft = nullptr;
	CoordStruct Start {};
	CoordStruct End {};
	double Speed = 0.0;
	double MaxSpeed = 0.0;
	double Acceleration = 0.0;
	bool TrailEnabled = true;
	CellStruct CommandDestination { -1, -1 };
	TechnoClass* CommandTarget = nullptr;
	int CommandMission = 0;

	template <typename T>
	void Serialize(T& Stm)
	{
		Stm
			.Process(this->Aircraft)
			.Process(this->Start)
			.Process(this->End)
			.Process(this->Speed)
			.Process(this->MaxSpeed)
			.Process(this->Acceleration)
			.Process(this->TrailEnabled)
			.Process(this->CommandDestination)
			.Process(this->CommandTarget)
			.Process(this->CommandMission);
	}

	template <typename T>
	bool Load(T& Stm, bool RegisterForChange)
	{
		(void)RegisterForChange;
		this->Serialize(Stm);
		return true;
	}

	template <typename T>
	bool Save(T& Stm) const
	{
		const_cast<CarrierRunwayDeparture*>(this)->Serialize(Stm);
		return true;
	}
};

struct CarrierRunwayState
{
	AircraftClass* Occupant = nullptr;
	AircraftClass* TakeoffRequest = nullptr;
	int OccupantSlot = -1;
	int TakeoffRequestSlot = -1;
	// The active aircraft's original command, kept outside the engine mission
	// state while the runway controller owns it.
	CellStruct CommandDestination { -1, -1 };
	TechnoClass* CommandTarget = nullptr;
	int CommandMission = 0;
	// The waiting aircraft's command, copied before its parked state is pinned.
	CellStruct PendingDestination { -1, -1 };
	TechnoClass* PendingTarget = nullptr;
	int PendingMission = 0;
	CarrierRunwayPhase Phase = CarrierRunwayPhase::Idle;
	int Timer = 0;
	double Speed = 0.0;
	double MaxSpeed = 0.0;
	double Acceleration = 0.0;
	CoordStruct StartCoords {};
	CoordStruct EndCoords {};
	CoordStruct LastCoords {};
	std::vector<CoordStruct> Path;
	size_t PathIndex = 0;
	bool TrailEnabled = false;
	std::vector<CarrierRunwayTaxiTask> TaxiTasks;
	std::vector<CarrierRunwayDeparture> Departures;
	// Which configured runway point this state is using. Kept inside the
	// state so a queued/active aircraft never drifts to another runway when
	// the carrier turns.
	int RunwayIndex = 0;

	template <typename T>
	void SerializeBase(T& Stm)
	{
		Stm
			.Process(this->Occupant)
			.Process(this->TakeoffRequest)
			.Process(this->OccupantSlot)
			.Process(this->TakeoffRequestSlot)
			.Process(this->CommandDestination)
			.Process(this->CommandTarget)
			.Process(this->CommandMission)
			.Process(this->PendingDestination)
			.Process(this->PendingTarget)
			.Process(this->PendingMission)
			.Process(this->Phase)
			.Process(this->Timer)
			.Process(this->Speed)
			.Process(this->MaxSpeed)
			.Process(this->Acceleration)
			.Process(this->StartCoords)
			.Process(this->EndCoords)
			.Process(this->LastCoords)
			.Process(this->Path)
			.Process(this->PathIndex)
			.Process(this->TrailEnabled)
			.Process(this->TaxiTasks);
	}

	// Legacy serialization: optional departure tail, no runway index.
	template <typename T>
	void Serialize(T& Stm)
	{
		this->SerializeBase(Stm);

		if constexpr (requires { Stm.Remaining(); })
		{
			if (Stm.Remaining() >= 2 * sizeof(size_t))
				Stm.Process(this->Departures);
		}
		else
		{
			Stm.Process(this->Departures);
		}
	}

	// Current serialization used by the version-2 runway arrays.
	template <typename T>
	void SerializeFull(T& Stm)
	{
		this->SerializeBase(Stm);
		Stm.Process(this->RunwayIndex)
			.Process(this->Departures);
	}

	template <typename T>
	bool Load(T& Stm, bool RegisterForChange)
	{
		(void)RegisterForChange;
		this->SerializeFull(Stm);
		return true;
	}

	template <typename T>
	bool Save(T& Stm) const
	{
		const_cast<CarrierRunwayState*>(this)->SerializeFull(Stm);
		return true;
	}
};

// Per-payload manual landing progress. One entry per CarrierPayloads slot so
// an aircraft keeps its approach point, runway reservation and taxi path even
// while another payload is landing on a different runway.
struct CarrierLandingProgress
{
	CarrierLandingPhase Phase = CarrierLandingPhase::None;
	int RunwayIndex = -1;
	bool ExplicitRunway = false;
	// True while this aircraft has reserved the runway for its approach. The
	// runway's Occupant is only set once the aircraft actually enters state 7.
	bool Reserved = false;

	CoordStruct ApproachPoint {};
	CoordStruct LineupPoint {};
	CoordStruct TouchdownPoint {};
	CoordStruct RollEndPoint {};
	int CruiseZ = 0;
	unsigned short RunwayFacingRaw = 0;

	int PhaseTimer = 0;
	int FailedAttempts = 0;
	int StuckTimer = 0;
	CoordStruct LastPos {};
	int LastPosTimer = 0;

	double Speed = 0.0;
	std::vector<CoordStruct> TaxiPath;
	size_t TaxiPathIndex = 0;
	double TaxiSpeed = 0.0;
	int TaxiTurnTimer = 0;

	template <typename T>
	void Serialize(T& Stm)
	{
		Stm
			.Process(this->Phase)
			.Process(this->RunwayIndex)
			.Process(this->ExplicitRunway)
			.Process(this->Reserved)
			.Process(this->ApproachPoint)
			.Process(this->LineupPoint)
			.Process(this->TouchdownPoint)
			.Process(this->RollEndPoint)
			.Process(this->CruiseZ)
			.Process(this->RunwayFacingRaw)
			.Process(this->PhaseTimer)
			.Process(this->FailedAttempts)
			.Process(this->StuckTimer)
			.Process(this->LastPos)
			.Process(this->LastPosTimer)
			.Process(this->Speed)
			.Process(this->TaxiPath)
			.Process(this->TaxiPathIndex)
			.Process(this->TaxiSpeed)
			.Process(this->TaxiTurnTimer);
	}

	template <typename T>
	bool Load(T& Stm, bool RegisterForChange)
	{
		(void)RegisterForChange;
		this->Serialize(Stm);
		return true;
	}

	template <typename T>
	bool Save(T& Stm) const
	{
		const_cast<CarrierLandingProgress*>(this)->Serialize(Stm);
		return true;
	}
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
	bool Alt = false;	std::vector<CellStruct> Span;
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
	std::vector<CarrierLandingProgress> CarrierPayloadLanding;
	std::vector<CoordStruct> CarrierPayloadLastCoords;
	std::vector<int> CarrierPayloadReloadTimer;
	// Destination the engine restored on the frame the aircraft landed; it must
	// not be mistaken for a new player order.
	std::vector<CellStruct> CarrierPayloadIgnoredDest;
	// One-shot launch request captured from AircraftClass::AssignDestination.
	std::vector<unsigned char> CarrierPayloadLaunchRequest;
	std::vector<int> CarrierPayloadLandingTimer;
	// Cell the player actually clicked for a parked payload's move order.
	std::vector<CellStruct> CarrierPayloadRequestedDest;
	// Destination cell currently assigned by the return autopilot. Used to
	// distinguish our own native Move order from a player order.
	std::vector<CellStruct> CarrierPayloadReturnCell;
	CoordStruct CarrierLastCoords {};
	int CarrierLastBucket = -1;
	// Scratch states used by the legacy single-runway code paths while a
	// per-runway state is loaded/updated. The authoritative state lives in the
	// arrays below; this keeps the state machine functions unchanged while
	// still allowing independent runways to operate concurrently.
	CarrierRunwayState Runway;
	CarrierRunwayState LandingRunway;
	std::vector<CarrierRunwayState> TakeoffRunways;
	std::vector<CarrierRunwayState> LandingRunways;

	// Aim reconciliation: the visual hull rotated past a bucket boundary while
	// the unit was stationary. The swept area is reserved until the rotation
	// reaches ReconcileTarget.
	bool ReconcileActive = false;
	int ReconcileTarget = -1;
	bool ReconcileClockwise = true;

	// Persisted through TechnoExt::ExtData. Transient flags above (Locked,
	// Translating, Reconcile*) are deliberately not saved; the load path resets
	// them and rebuilds the owner grid from Initialized/Span.
	//
	// CarrierPayloadRequestedDest / CarrierPayloadReturnCell were added after
	// the original save layout. Current saves write them, old saves do not.
	// The reader accepts both by checking the remaining block size before the
	// stable CarrierLastCoords/Bucket tail. New saves append the runway block
	// after that tail, with its own magic/version.
	template <typename T>
	void Serialize(T& Stm)
	{
		Stm
			.Process(this->Initialized)
			.Process(this->Bucket)
			.Process(this->Anchor)
			.Process(this->Alt)
			.Process(this->Span)
			.Process(this->BarrierSpan)
			.Process(this->ReservedSpan)
			.Process(this->CarrierPayloadsInitialized)
			.Process(this->CarrierDockBuilding)
			.Process(this->CarrierDockBuildings)
			.Process(this->CarrierPayloads)
			.Process(this->CarrierPayloadDocked)
			.Process(this->CarrierPayloadLastCoords)
			.Process(this->CarrierPayloadReloadTimer)
			.Process(this->CarrierPayloadIgnoredDest)
			.Process(this->CarrierPayloadLaunchRequest)
			.Process(this->CarrierPayloadLandingTimer);

		constexpr size_t tailSize = sizeof(CoordStruct) + sizeof(int);
		constexpr size_t transientFieldsMinSize = 2 * (2 * sizeof(size_t));

		if constexpr (requires { Stm.Remaining(); })
		{
			if (Stm.Remaining() >= tailSize + transientFieldsMinSize)
			{
				Stm.Process(this->CarrierPayloadRequestedDest)
					.Process(this->CarrierPayloadReturnCell);
			}

			if (Stm.Remaining() >= tailSize)
			{
				Stm.Process(this->CarrierLastCoords)
					.Process(this->CarrierLastBucket);
			}
			else if (Stm.Remaining() >= sizeof(CoordStruct))
			{
				Stm.Process(this->CarrierLastCoords);
			}

			// The runway block is appended after the stable footprint tail.
			// Old saves and the short-lived pre-runway saves end here.
			if (Stm.Remaining() >= sizeof(std::uint32_t) * 2)
			{
				std::uint32_t magic = 0;
				std::uint32_t version = 0;
				Stm.Process(magic).Process(version);

				if (magic == CarrierRunwaySaveMagic)
				{
					if (version >= 2)
					{
						Stm.Process(this->TakeoffRunways)
							.Process(this->LandingRunways);
					}
					else if (version >= 1)
					{
						// Version 1 used one shared runway and an older
						// CarrierRunwayState layout. Its byte layout is not
						// forward-compatible with the per-runway fields added
						// since then, so do not misinterpret it. Consume the
						// rest of this object's save block so the container's
						// end-of-block check still succeeds, then start with
						// clean runway arrays. Payload states are still
						// restored; any aircraft that was mid-runway is safely
						// parked on the next update.
						this->TakeoffRunways.clear();
						this->LandingRunways.clear();

						unsigned char discard[128];

						while (Stm.Remaining() > 0)
						{
							const size_t count = std::min(Stm.Remaining(), sizeof(discard));

							if (!Stm.Read(discard, count))
								break;
						}
					}
				}
			}

			// Manual landing progress is appended after the runway block so
			// pre-controller saves end cleanly here.
			if (Stm.Remaining() >= sizeof(std::uint32_t) * 2)
			{
				std::uint32_t landingMagic = 0;
				std::uint32_t landingVersion = 0;
				Stm.Process(landingMagic).Process(landingVersion);

				if (landingMagic == CarrierLandingSaveMagic && landingVersion >= 1)
					Stm.Process(this->CarrierPayloadLanding);
			}
		}
		else
		{
			// Keep the two transient carrier fields in the stream so old,
			// pre-runway and new saves all use the same detectable layout.
			Stm.Process(this->CarrierPayloadRequestedDest)
				.Process(this->CarrierPayloadReturnCell)
				.Process(this->CarrierLastCoords)
				.Process(this->CarrierLastBucket);

			std::uint32_t magic = CarrierRunwaySaveMagic;
			std::uint32_t version = CarrierRunwaySaveVersion;
			Stm.Process(magic).Process(version);
			Stm.Process(this->TakeoffRunways)
				.Process(this->LandingRunways);

			std::uint32_t landingMagic = CarrierLandingSaveMagic;
			std::uint32_t landingVersion = CarrierLandingSaveVersion;
			Stm.Process(landingMagic).Process(landingVersion);
			Stm.Process(this->CarrierPayloadLanding);
		}
	}
};

class Footprint
{
public:
	// Master switch. False when MultiCell.Disabled is set, or when
	// MultiCell.DisabledInMultiplayer (default) is set and the current session
	// is networked. Every entry point must consult this so a multiplayer match
	// never runs the custom footprint code.
	static bool MultiCellAllowed();
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
	// Per-frame deck update for one payload aircraft: parked/rearm/launch and
	// the custom return-to-deck landing that replaces native airport docking.
	static void UpdateCarrierPayload(AircraftClass* pAircraft);
	// Early-AI lock for parked payloads. Runs before the engine mission AI so a
	// stale Destination/Mission cannot start a spontaneous takeoff.
	static void LockParkedCarrierPayload(TechnoClass* pPayload);
	// Called from the AssignDestination hook: the player pointed a parked
	// payload somewhere, so launch it even if the engine rejects the mission.
	static void NotifyDestinationAssigned(TechnoClass* pThis, AbstractClass* pDest);
	// Diagnostic command logger for carrier payloads.
	static void LogPayloadCommand(const char* tag, TechnoClass* pThis, AbstractClass* pTarget);
	// While an aircraft is in the runway takeoff sequence, remember incoming
	// move/attack orders so they can be executed after departure completes.
	static void RememberRunwayCommandDestination(TechnoClass* pThis, AbstractClass* pDest);
	static void RememberRunwayCommandTarget(TechnoClass* pThis, AbstractClass* pTarget);
	// Records the real clicked cell for the currently selected parked payloads.
	static void CaptureClickDestination(const CellStruct& cell);
	static void CaptureAttackClick(TechnoClass* pTarget);
	// Deferred landing/takeoff entry points. Hooks and click handling must only
	// queue these; the actual aircraft state change happens once per frame in
	// ProcessCarrierIntents before any object AI.
	static void QueueCarrierLandingIntent(AircraftClass* pAircraft, UnitClass* pCarrier,
		int runwayIndex, bool explicitRunway, bool autoReturn);
	static void QueueCarrierTakeoffIntent(AircraftClass* pAircraft, UnitClass* pCarrier);
	static void ProcessCarrierIntents();
	// Intercept a click on the carrier itself as a return request before the
	// vanilla Aircraft Enter/return mission can recurse without a native dock.
	static bool CaptureCarrierReturnClick(TechnoClass* pTarget);
	// Convert an engine Enter/Return mission on an airborne payload into the
	// custom runway return state before native dock/return logic can recurse.
	static bool RequestCarrierLandingFromMission(TechnoClass* pPayload);
	// True when an engine mission must not reach MissionClass for a carrier
	// payload: the native no-dock Enter/Return path, or any order while the
	// runway controller owns the aircraft. The QueueMission hook converts
	// these to Mission::Sleep at the engine's real entry point.
	static bool ShouldSuppressCarrierMission(TechnoClass* pPayload, Mission mission);
	// Deletes payload aircraft whose carrier died. Runs from the global logic
	// tick before any object AI, never from an aircraft's own AI.
	static void ProcessPendingKills();
	static UnitClass* GetCarrierPayloadOwner(TechnoClass* pPayload);
	static bool IsCarrierPayloadParked(TechnoClass* pPayload);
	// True for every payload state whose movement is owned by the deck
	// controller (parked, returning, landing, takeoff/runway). State 0
	// (normal airborne flight) is the only one the engine may drive.
	static bool IsCarrierPayloadDeckControlled(TechnoClass* pPayload);
	// state 3/4/5: final approach or vertical descent. The engine aircraft
	// update is skipped so the autopilot fully owns movement.
	static bool IsCarrierPayloadLanding(TechnoClass* pPayload);
	// state 2 or 7: fully manual return/landing sequence. The engine aircraft
	// update must be skipped for these states even though state 2 is not
	// "on the runway" and does not block player orders.
	static bool IsCarrierPayloadLandingSequence(TechnoClass* pPayload);
	// state 6/7/8: runway takeoff or the final landing runway phase. The engine
	// aircraft update is skipped and every engine command is blocked.
	static bool IsCarrierPayloadRunwayControlled(TechnoClass* pPayload);
	// Takeoff (6/8) and manual landing (7) states. State 2 is the native
	// return flight and must be allowed to change Move/Guard missions.
	static bool IsCarrierPayloadRunwayCommandLocked(TechnoClass* pPayload);
	// True while the runway controller wants the aircraft's trailer enabled
	// even though it is not using the engine's airborne update.
	static bool IsCarrierPayloadRunwayTrailEnabled(TechnoClass* pPayload);
	// World Z of the payload's current deck slot, or -1 when the object is not
	// a carrier payload. Used by the SHP aircraft shadow so a landing aircraft
	// casts its shadow on the deck instead of the water below the ship.
	static int GetCarrierPayloadDeckZ(TechnoClass* pPayload);
	// First-frame carrier sinking setup: central explosion, deck casualties,
	// and the delay before the hull starts descending.
	static void BeginCarrierSinkDestruction(UnitClass* pThis);
	static int GetCarrierSinkStartFrame(UnitClass* pThis);
	// Returns this frame's Z descent for a sinking carrier. normalStep is the
	// vanilla per-frame step; the result averages normalStep / 20 per frame.
	static int AdvanceCarrierSink(UnitClass* pThis, int normalStep);
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
	// Called when a unit is removed/destroyed. Releases the deck of a carrier:
	// untracks payloads and destroys the hidden dock proxies.
	static void OnUnitDestroyed(UnitClass* pThis);
	// Called from TechnoExt::InvalidatePointer; drops cached payload links that
	// point at an object the engine is about to delete.
	static void OnPointerInvalidated(void* ptr);

	static int GetYSort(TechnoClass* pThis);
	static bool IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt);
	static bool HasObstacleNear(TechnoClass* pThis, int radius);
	// Writes a full deck/footprint snapshot of every live carrier to debug.log.
	// Bound to the "Dump Carrier State" debug command.
	static void DumpCarrierState();
};
