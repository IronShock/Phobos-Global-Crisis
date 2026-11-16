#include "Footprint.h"

#include <AircraftClass.h>
#include <FlyLocomotionClass.h>
#include <BuildingClass.h>
#include <InfantryClass.h>
#include <MapClass.h>
#include <OverlayTypeClass.h>
#include <TacticalClass.h>
#include <TerrainClass.h>
#include <Unsorted.h>
#include <WWMouseClass.h>

#include <Ext/BuildingType/Body.h>
#include <Ext/Anim/Body.h>
#include <Ext/Cell/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Locomotion/FootprintLocomotionClass.h>

#include <SpawnManagerClass.h>
#include <Utilities/Constructs.h>

#include <Phobos.h>
#include <ScenarioClass.h>
#include <SessionClass.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

void TurnAircraftToward(AircraftClass* pAircraft, const DirStruct& desired, int turnStep);

namespace
{
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kDegToRad = kPi / 180.0;
	constexpr int kMapSide = 512;
	constexpr int kMaxCells = MapClass::MaxCells;

	std::vector<UnitClass*> s_ownerNormal;
	std::vector<UnitClass*> s_ownerAlt;
	std::vector<UnitClass*> s_barrierNormal;
	std::vector<UnitClass*> s_barrierAlt;
	bool s_gridReady = false;

	std::unordered_map<TechnoTypeClass*, FootprintGeometry> s_geometries;
	std::unordered_map<TechnoClass*, UnitClass*> s_carrierPayloads;
	// Use a set so BeginCarrierSinkDestruction and OnUnitDestroyed cannot queue
	// the same aircraft twice and hand a stale pointer to a second kill.
	std::unordered_set<AircraftClass*> s_pendingKills;

	// Carrier return autopilot: detect when the carrier has actually stopped.
	// The fly locomotor keeps reporting movement while the ship turns or
	// decelerates, so a short debounce is used before allowing a landing.
	std::unordered_map<UnitClass*, CoordStruct> s_carrierMoveRef;
	std::unordered_map<UnitClass*, int> s_carrierLastMoveFrame;
	std::unordered_map<UnitClass*, unsigned> s_carrierLastFacing;

	// After the player cancels a return, do not immediately force the aircraft
	// home again just because it is still out of ammo.
	std::unordered_map<TechnoClass*, int> s_returnSuppressUntil;

	// Captured at the moment the return starts: the aircraft's actual current
	// flight speed, used by the short manual final phase.
	std::unordered_map<AircraftClass*, double> s_carrierReturnSpeed;

	// Native-return stuck detection: if Mission Move makes no progress, hand
	// over to the manual final phase instead of freezing.
	std::unordered_map<AircraftClass*, double> s_returnProgressRef;
	std::unordered_map<AircraftClass*, int> s_returnStuckChecks;

	// First frame the carrier entered the engine sinking state. Used to delay
	// the hull descent while the destruction explosion and deck casualties
	// play out.
	std::unordered_map<UnitClass*, int> s_carrierSinkStartFrame;

	// Fractional descent accumulator for the 20x slower carrier sink. Kept
	// here (instead of a function-local static in the hook) so loading or
	// starting a scenario can clear stale per-object entries.
	std::unordered_map<UnitClass*, int> s_carrierSinkAccum;
	std::unordered_set<UnitClass*> s_runwayLoggedCarriers;
	// Landing-runway choice is made once per approach. Recomputing it every
	// frame made the approach point jump between nearby landing runways.
	std::unordered_map<AircraftClass*, int> s_landingRunwayIndex;
	// A player clicked a specific landing runway. If it is occupied, the
	// aircraft must wait instead of silently diverting to another runway.
	std::unordered_set<AircraftClass*> s_landingRunwayExplicit;

	// Landing progress watchdog. A state-7 aircraft that neither moves nor
	// turns for 180 frames is considered stuck and goes around.
	struct LandingWatchState
	{
		CoordStruct Location {};
		unsigned short Facing = 0;
		int StuckFrames = 0;
	};

	std::unordered_map<AircraftClass*, LandingWatchState> s_landingWatch;

	// Same idea for the post-landing taxi to the parking slot. This is a
	// final fallback only; normal per-waypoint turning still drives the task.
	struct TaxiWatchState
	{
		CoordStruct Location {};
		unsigned short Facing = 0;
		int StuckFrames = 0;
	};

	std::unordered_map<AircraftClass*, TaxiWatchState> s_taxiWatch;

	// Set by the Enter/Return mission hook, consumed on the next normal
	// payload update. Never mutate the carrier/payload state from inside the
	// engine mission callback; that caused stack-overflow recursion.
	std::unordered_set<AircraftClass*> s_pendingCarrierLanding;

	// Deferred landing/takeoff intents. Hooks and click handling never mutate
	// an aircraft directly; they only append here. ProcessCarrierIntents runs
	// from LogicClass_Update_BeforeAll, once per frame, before any object AI.
	enum class CarrierLandingIntentKind : unsigned char
	{
		ManualCarrierClick,
		ManualRunwayClick,
		AutoOutOfAmmo
	};

	struct CarrierLandingIntent
	{
		AircraftClass* Aircraft = nullptr;
		UnitClass* Carrier = nullptr;
		int RunwayIndex = -1;
		bool ExplicitRunway = false;
		CarrierLandingIntentKind Kind = CarrierLandingIntentKind::ManualCarrierClick;
		int Frame = 0;
	};

	std::vector<CarrierLandingIntent> s_carrierLandingIntents;
	std::unordered_set<AircraftClass*> s_carrierLandingIntentAircraft;

	struct CarrierTakeoffIntent
	{
		AircraftClass* Aircraft = nullptr;
		UnitClass* Carrier = nullptr;
		int Frame = 0;
	};

	std::vector<CarrierTakeoffIntent> s_carrierTakeoffIntents;
	std::unordered_set<AircraftClass*> s_carrierTakeoffIntentAircraft;

	// A click while the aircraft is already returning/landing (state 2-5) is
	// a player cancel + move order. It cannot go through the engine's mission
	// path because an airport-bound destination is converted to Enter, which
	// the carrier gate suppresses.
	struct CarrierCancelIntent
	{
		AircraftClass* Aircraft = nullptr;
		CellStruct Destination { -1, -1 };
	};

	std::vector<CarrierCancelIntent> s_carrierCancelIntents;

	// Takeoff command restoration is deferred to the next global tick so the
	// aircraft is not handed back to the engine while its own Update is still
	// on the stack.
	struct CarrierTakeoffHandoff
	{
		AircraftClass* Aircraft = nullptr;
		UnitClass* Carrier = nullptr;
		CellStruct CommandDestination { -1, -1 };
		TechnoClass* CommandTarget = nullptr;
		int CommandMission = 0;
	};

	std::vector<CarrierTakeoffHandoff> s_carrierTakeoffHandoffs;

	// Recursion guards. These are diagnostics as much as safety: a reentrant
	// footprint Mark or payload update used to surface as C00000FD.
	thread_local int s_carrierPayloadUpdateDepth = 0;
	thread_local int s_footprintMarkDepth = 0;

	struct CarrierPayloadUpdateGuard
	{
		CarrierPayloadUpdateGuard() { ++s_carrierPayloadUpdateDepth; }
		~CarrierPayloadUpdateGuard() { --s_carrierPayloadUpdateDepth; }
	};

	struct FootprintMarkGuard
	{
		FootprintMarkGuard() { ++s_footprintMarkDepth; }
		~FootprintMarkGuard() { --s_footprintMarkDepth; }
	};

	const char* LandingPhaseName(CarrierLandingPhase phase)
	{
		switch (phase)
		{
		case CarrierLandingPhase::None: return "None";
		case CarrierLandingPhase::ReturnToApproach: return "ReturnToApproach";
		case CarrierLandingPhase::OrbitWait: return "OrbitWait";
		case CarrierLandingPhase::Lineup: return "Lineup";
		case CarrierLandingPhase::FinalApproach: return "FinalApproach";
		case CarrierLandingPhase::Touchdown: return "Touchdown";
		case CarrierLandingPhase::LandingRoll: return "LandingRoll";
		case CarrierLandingPhase::TaxiToSlot: return "TaxiToSlot";
		case CarrierLandingPhase::GoAround: return "GoAround";
		default: return "Unknown";
		}
	}

	const char* PayloadStateName(unsigned char state)
	{
		switch (state)
		{
		case 0: return "Airborne";
		case 1: return "Parked";
		case 2: return "ReturnLanding";
		case 3: return "LegacyFinal3";
		case 4: return "LegacyDescent4";
		case 5: return "LegacyManual5";
		case 6: return "RunwayTakeoff";
		case 7: return "RunwayLanding7";
		case 8: return "TakeoffDeparture";
		default: return "Unknown";
		}
	}

	void LogLandingProgress(const char* tag, AircraftClass* pAircraft,
		FootprintState* pState, size_t index)
	{
		if (!pAircraft || !pState || index >= pState->CarrierPayloadDocked.size()
			|| index >= pState->CarrierPayloadLanding.size())
		{
			Debug::CarrierLog("[DiagLanding] %s invalid aircraft/state/index=%u\n",
				tag ? tag : "?", static_cast<unsigned>(index));
			return;
		}

		auto const& progress = pState->CarrierPayloadLanding[index];
		auto const& dest = pAircraft->Destination;

		Debug::CarrierLog("[DiagLanding] %s f=%d slot=%u state=%u(%s) phase=%u(%s) mission=%d selected=%d inAir=%d ammo=%d\n",
			tag ? tag : "?", Unsorted::CurrentFrame, static_cast<unsigned>(index),
			static_cast<unsigned>(pState->CarrierPayloadDocked[index]),
			PayloadStateName(pState->CarrierPayloadDocked[index]),
			static_cast<unsigned>(progress.Phase), LandingPhaseName(progress.Phase),
			static_cast<int>(pAircraft->GetCurrentMission()), pAircraft->IsSelected ? 1 : 0,
			pAircraft->InAir ? 1 : 0, pAircraft->Ammo);

		Debug::CarrierLog("[DiagLanding]   loc=(%d,%d,%d) facing=%u dest=%p target=%p dock=%p speed=%.2f\n",
			pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
			static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
			static_cast<void*>(dest), static_cast<void*>(pAircraft->Target),
			static_cast<void*>(pAircraft->DockNowHeadingTo), progress.Speed);

		Debug::CarrierLog("[DiagLanding]   runway=%d explicit=%d reserved=%d cruiseZ=%d runwayFacing=%u A=(%d,%d,%d) Z=(%d,%d,%d)\n",
			progress.RunwayIndex, progress.ExplicitRunway ? 1 : 0,
			progress.Reserved ? 1 : 0, progress.CruiseZ,
			static_cast<unsigned>(progress.RunwayFacingRaw),
			progress.TouchdownPoint.X, progress.TouchdownPoint.Y, progress.TouchdownPoint.Z,
			progress.RollEndPoint.X, progress.RollEndPoint.Y, progress.RollEndPoint.Z);

		Debug::CarrierLog("[DiagLanding]   approach=(%d,%d,%d) lineup=(%d,%d,%d) timer=%d stuck=%d failed=%d lastPos=(%d,%d,%d) lastTimer=%d\n",
			progress.ApproachPoint.X, progress.ApproachPoint.Y, progress.ApproachPoint.Z,
			progress.LineupPoint.X, progress.LineupPoint.Y, progress.LineupPoint.Z,
			progress.PhaseTimer, progress.StuckTimer, progress.FailedAttempts,
			progress.LastPos.X, progress.LastPos.Y, progress.LastPos.Z, progress.LastPosTimer);

		Debug::CarrierLog("[DiagLanding]   taxiPath=%u/%u speed=%.2f turn=%d\n",
			static_cast<unsigned>(progress.TaxiPathIndex),
			static_cast<unsigned>(progress.TaxiPath.size()),
			progress.TaxiSpeed, progress.TaxiTurnTimer);
	}

	void LogCarrierRunways(const char* tag, FootprintState* pState)
	{
		if (!pState)
			return;

		Debug::CarrierLog("[DiagRunways] %s f=%d landingRunways=%u payloads=%u\n",
			tag ? tag : "?", Unsorted::CurrentFrame,
			static_cast<unsigned>(pState->LandingRunways.size()),
			static_cast<unsigned>(pState->CarrierPayloads.size()));

		for (size_t i = 0; i < pState->LandingRunways.size(); ++i)
		{
			auto const& runway = pState->LandingRunways[i];
			Debug::CarrierLog("[DiagRunways]   rw%u occupant=%p slot=%d phase=%d timer=%d speed=%.1f index=%d\n",
				static_cast<unsigned>(i), static_cast<void*>(runway.Occupant),
				runway.OccupantSlot, static_cast<int>(runway.Phase),
				runway.Timer, runway.Speed, runway.RunwayIndex);
		}

		for (size_t i = 0; i < pState->CarrierPayloadLanding.size(); ++i)
		{
			auto const& progress = pState->CarrierPayloadLanding[i];
			Debug::CarrierLog("[DiagRunways]   slot%u phase=%s runway=%d reserved=%d explicit=%d\n",
				static_cast<unsigned>(i), LandingPhaseName(progress.Phase),
				progress.RunwayIndex, progress.Reserved ? 1 : 0,
				progress.ExplicitRunway ? 1 : 0);
		}
	}

	// Fallback deck coordinates if this mod's art file fails to provide the
	// DockingOffset tags before the carrier is initialized.
	constexpr CoordStruct kDefaultCarrierDockOffsets[8] =
	{
		{ 640, 768, 0 },
		{ 256, 768, 0 },
		{ -2176, 768, 0 },
		{ -2560, 768, 0 },
		{ -3328, 640, 0 },
		{ -3328, -640, 0 },
		{ -2944, -768, 0 },
		{ -2560, -896, 0 }
	};

	CoordStruct GetCarrierDockOffset(TechnoTypeExt::ExtData* pTypeExt, size_t index)
	{
		if (pTypeExt && index < pTypeExt->AircraftCarrier_DockingOffsets.size())
		{
			const auto& offset = pTypeExt->AircraftCarrier_DockingOffsets[index];

			if (offset.X != 0 || offset.Y != 0 || offset.Z != 0)
				return offset;
		}

		return index < std::size(kDefaultCarrierDockOffsets)
			? kDefaultCarrierDockOffsets[index]
			: CoordStruct::Empty;
	}

	CoordStruct GetCarrierDockSlotCoords(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt, size_t index)
	{
		auto offset = GetCarrierDockOffset(pTypeExt, index);
		offset.Z += Unsorted::CellHeight;

		// Tuned deck offsets: authored directly relative to the hull centre.
		// Do not change this transform; the DockingOffset values were adjusted
		// against it.
		auto mtx = Matrix3D::GetIdentity();
		mtx.RotateZ(static_cast<float>(pCarrier->PrimaryFacing.Current().GetRadian<32>()));
		mtx.Translate(static_cast<float>(offset.X), static_cast<float>(offset.Y), static_cast<float>(offset.Z));

		const auto result = mtx.GetTranslation();
		return pCarrier->GetRenderCoords()
			+ CoordStruct { static_cast<int>(result.X), static_cast<int>(result.Y), static_cast<int>(result.Z) };
	}

	// Deck parking direction, absolute world facing. Slots without an
	// AircraftDockingDirN value use 0 (north); they must not follow the ship.
	DirStruct GetCarrierSlotFacing(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt, size_t index)
	{
		(void)pCarrier;

		const int dockDir = pTypeExt && index < pTypeExt->AircraftCarrier_DockingDirs.size()
			? pTypeExt->AircraftCarrier_DockingDirs[index]
			: 0;

		return DirStruct(static_cast<DirType>(dockDir & 0xFF));
	}

	double GetRunwayTaxiSpeed(TechnoTypeExt::ExtData* pTypeExt)
	{
		const double configured = pTypeExt
			? pTypeExt->AircraftCarrier_RunwayTaxiSpeed.Get()
			: 6.0;

		// Taxi speed follows the same rough scale as the game's ground speed
		// tags instead of treating 6 as six full cells every second. Default
		// 6 => about 15 lepton/frame, a slow deck taxi rather than a sprint.
		return std::clamp(configured * 2.5, 2.0, 64.0);
	}

	int GetRunwayTakeoffDelay(TechnoTypeExt::ExtData* pTypeExt)
	{
		return std::max(0, pTypeExt
			? pTypeExt->AircraftCarrier_RunwayTakeoffDelay.Get()
			: 45);
	}

	int GetRunwayApproachDistance(TechnoTypeExt::ExtData* pTypeExt)
	{
		return std::max(0, pTypeExt
			? pTypeExt->AircraftCarrier_RunwayApproachDistance.Get()
			: 4096);
	}

	int GetRunwayTurnRate(TechnoTypeExt::ExtData* pTypeExt)
	{
		return std::clamp(pTypeExt
			? pTypeExt->AircraftCarrier_RunwayTurnRate.Get()
			: 512, 32, 4096);
	}

	double GetTakeoffAcceleration(TechnoTypeExt::ExtData* pTypeExt)
	{
		const double cellsPerSecond2 = pTypeExt
			? pTypeExt->AircraftCarrier_RunwayTakeoffAcceleration.Get()
			: 2.0;

		// cells/s^2 -> lepton/frame^2 at 15 logic frames and 256 leptons/cell.
		return std::max(0.0, cellsPerSecond2 * 256.0 / (15.0 * 15.0));
	}

	int GetTakeoffInitialSpeed(TechnoTypeExt::ExtData* pTypeExt)
	{
		return std::clamp(pTypeExt
			? pTypeExt->AircraftCarrier_RunwayTakeoffInitialSpeed.Get()
			: 0, 0, 100);
	}

	double GetLandingDeceleration(TechnoTypeExt::ExtData* pTypeExt)
	{
		const double cellsPerSecond2 = pTypeExt
			? pTypeExt->AircraftCarrier_RunwayLandingDeceleration.Get()
			: 2.0;

		// cells/s^2 -> lepton/frame^2, same time base as takeoff acceleration.
		return std::max(0.0, cellsPerSecond2 * 256.0 / (15.0 * 15.0));
	}

	int GetLandingMaxOverrun(TechnoTypeExt::ExtData* pTypeExt)
	{
		return std::clamp(pTypeExt
			? pTypeExt->AircraftCarrier_RunwayLandingMaxOverrun.Get()
			: 512, 0, 4096);
	}

	double GetAircraftFlightSpeed(AircraftClass* pAircraft)
	{
		if (!pAircraft)
			return 8.0;

		// GetDefaultSpeed/GetCurrentSpeed are the engine's real lepton-per-
		// frame speeds (the same values movement code compares against). Do
		// not use Type.Speed directly here: Type.Speed is an abstract tag and
		// converting it as cells/second made the runway roll several times
		// faster than the aircraft's normal flight.
		double speed = std::max(
			static_cast<double>(pAircraft->GetDefaultSpeed()),
			static_cast<double>(pAircraft->GetCurrentSpeed()));

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
			speed = std::max(speed, std::max(pFly->CurrentSpeed, pFly->TargetSpeed));

		if (speed < 1.0 && pAircraft->Type)
			speed = static_cast<double>(pAircraft->Type->Speed);

		return std::clamp(speed, 4.0, 64.0);
	}

	bool HasRunwayMode(TechnoTypeExt::ExtData* pTypeExt, bool landing);

	bool HasRunwayMode(TechnoTypeExt::ExtData* pTypeExt)
	{
		return HasRunwayMode(pTypeExt, false) || HasRunwayMode(pTypeExt, true);
	}

	bool HasRunwayMode(TechnoTypeExt::ExtData* pTypeExt, bool landing)
	{
		if (!pTypeExt
			|| pTypeExt->AircraftCarrier_TakeOffMode != AircraftCarrierTakeOffMode::Runway)
		{
			return false;
		}

		return landing
			? !pTypeExt->AircraftCarrier_LandingRunwayA.empty()
				&& !pTypeExt->AircraftCarrier_LandingRunwayZ.empty()
			: !pTypeExt->AircraftCarrier_TakeoffRunwayA.empty()
				&& !pTypeExt->AircraftCarrier_TakeoffRunwayZ.empty();
	}

	int GetCarrierRunwayCount(TechnoTypeExt::ExtData* pTypeExt, bool landing)
	{
		if (!pTypeExt)
			return 0;

		return landing
			? std::min(pTypeExt->AircraftCarrier_LandingRunwayA.size(),
				pTypeExt->AircraftCarrier_LandingRunwayZ.size())
			: std::min(pTypeExt->AircraftCarrier_TakeoffRunwayA.size(),
				pTypeExt->AircraftCarrier_TakeoffRunwayZ.size());
	}

	bool GetCarrierRunwayPoints(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt,
		bool landing, int runwayIndex, CoordStruct& start, CoordStruct& end)
	{
		if (!pCarrier || !HasRunwayMode(pTypeExt, landing))
			return false;

		const int count = static_cast<int>(GetCarrierRunwayCount(pTypeExt, landing));

		if (count <= 0)
			return false;

		const int index = std::clamp(runwayIndex, 0, count - 1);
		const auto& a = landing
			? pTypeExt->AircraftCarrier_LandingRunwayA[index]
			: pTypeExt->AircraftCarrier_TakeoffRunwayA[index];
		const auto& z = landing
			? pTypeExt->AircraftCarrier_LandingRunwayZ[index]
			: pTypeExt->AircraftCarrier_TakeoffRunwayZ[index];

		auto const toWorld = [pCarrier](const CoordStruct& offset)
		{
			CoordStruct local = offset;
			local.Z += Unsorted::CellHeight;

			auto mtx = Matrix3D::GetIdentity();
			mtx.RotateZ(static_cast<float>(pCarrier->PrimaryFacing.Current().GetRadian<32>()));
			mtx.Translate(static_cast<float>(local.X), static_cast<float>(local.Y), static_cast<float>(local.Z));

			const auto result = mtx.GetTranslation();
			return pCarrier->GetRenderCoords()
				+ CoordStruct { static_cast<int>(result.X), static_cast<int>(result.Y), static_cast<int>(result.Z) };
		};

		start = toWorld(a);
		end = toWorld(z);
		return start != end && start.Z == end.Z;
	}

	double HorizontalDistance(const CoordStruct& a, const CoordStruct& b);

	int ChooseNearestRunway(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt,
		bool landing, const CoordStruct& from)
	{
		const int count = static_cast<int>(GetCarrierRunwayCount(pTypeExt, landing));

		if (!pCarrier || count <= 0)
			return 0;

		int best = 0;
		double bestDistance = std::numeric_limits<double>::max();

		for (int i = 0; i < count; ++i)
		{
			CoordStruct a {};
			CoordStruct z {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, landing, i, a, z))
				continue;

			const double distance = HorizontalDistance(from, a);

			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}

		return best;
	}

	void EnsureRunwayStates(FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt)
	{
		if (!pState || !pTypeExt)
			return;

		const int takeoffCount = GetCarrierRunwayCount(pTypeExt, false);
		const int landingCount = GetCarrierRunwayCount(pTypeExt, true);

		if (static_cast<int>(pState->TakeoffRunways.size()) != takeoffCount)
			pState->TakeoffRunways.resize(takeoffCount);

		if (static_cast<int>(pState->LandingRunways.size()) != landingCount)
			pState->LandingRunways.resize(landingCount);

		if (pState->CarrierPayloadLanding.size() < pState->CarrierPayloads.size())
			pState->CarrierPayloadLanding.resize(pState->CarrierPayloads.size());

		// A save made mid-landing may restore state 7 before the runway's
		// Occupant pointer is rebuilt. Re-register it from the per-payload
		// progress so FindLandingRunwayIndex and the lock hooks keep working.
		const size_t landingCountToRestore = std::min(
			pState->CarrierPayloads.size(), pState->CarrierPayloadLanding.size());

		for (size_t i = 0; i < landingCountToRestore; ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft || i >= pState->CarrierPayloadDocked.size()
				|| pState->CarrierPayloadDocked[i] != 7)
			{
				continue;
			}

			auto const& progress = pState->CarrierPayloadLanding[i];

			if (progress.RunwayIndex >= 0
				&& progress.RunwayIndex < static_cast<int>(pState->LandingRunways.size()))
			{
				auto& runway = pState->LandingRunways[progress.RunwayIndex];

				if (!runway.Occupant || runway.Occupant == pAircraft)
				{
					runway.Occupant = pAircraft;
					runway.OccupantSlot = static_cast<int>(i);
					runway.RunwayIndex = progress.RunwayIndex;
				}
			}
		}

		for (auto& runway : pState->TakeoffRunways)
		{
			if (runway.RunwayIndex < 0 || runway.RunwayIndex >= takeoffCount)
				runway.RunwayIndex = 0;
		}

		for (auto& runway : pState->LandingRunways)
		{
			if (runway.RunwayIndex < 0 || runway.RunwayIndex >= landingCount)
				runway.RunwayIndex = 0;
		}
	}

	int FindTakeoffRunwayIndex(FootprintState* pState, AircraftClass* pAircraft)
	{
		if (!pState || !pAircraft)
			return -1;

		for (size_t i = 0; i < pState->TakeoffRunways.size(); ++i)
		{
			auto const& runway = pState->TakeoffRunways[i];

			if (runway.Occupant == pAircraft || runway.TakeoffRequest == pAircraft)
				return static_cast<int>(i);

			for (auto const& task : runway.Departures)
			{
				if (task.Aircraft == pAircraft)
					return static_cast<int>(i);
			}
		}

		return -1;
	}

	int ChooseTakeoffRunwayForRequest(UnitClass* pCarrier, AircraftClass* pAircraft,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt)
			return -1;

		EnsureRunwayStates(pState, pTypeExt);
		const int count = static_cast<int>(pState->TakeoffRunways.size());
		int bestFree = -1;
		int bestQueued = -1;
		double bestFreeDistance = std::numeric_limits<double>::max();
		double bestQueuedDistance = std::numeric_limits<double>::max();

		for (int i = 0; i < count; ++i)
		{
			auto const& runway = pState->TakeoffRunways[i];
			CoordStruct a {};
			CoordStruct z {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, false, i, a, z))
				continue;

			const double distance = HorizontalDistance(pAircraft->Location, a);

			if (!runway.Occupant && !runway.TakeoffRequest)
			{
				if (distance < bestFreeDistance)
				{
					bestFreeDistance = distance;
					bestFree = i;
				}
			}
			else if (!runway.TakeoffRequest)
			{
				if (distance < bestQueuedDistance)
				{
					bestQueuedDistance = distance;
					bestQueued = i;
				}
			}
		}

		return bestFree >= 0 ? bestFree : bestQueued;
	}

	int ChooseLandingRunwayForReturn(UnitClass* pCarrier, AircraftClass* pAircraft,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, int requested)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt)
			return -1;

		EnsureRunwayStates(pState, pTypeExt);
		const int count = static_cast<int>(pState->LandingRunways.size());

		if (requested >= 0 && requested < count)
		{
			auto const& runway = pState->LandingRunways[requested];

			if ((!runway.Occupant || runway.Occupant == pAircraft)
				&& (!runway.TakeoffRequest || runway.TakeoffRequest == pAircraft))
			{
				return requested;
			}
		}

		int best = -1;
		double bestDistance = std::numeric_limits<double>::max();

		for (int i = 0; i < count; ++i)
		{
			auto const& runway = pState->LandingRunways[i];

			if ((runway.Occupant && runway.Occupant != pAircraft)
				|| (runway.TakeoffRequest && runway.TakeoffRequest != pAircraft))
			{
				continue;
			}

			CoordStruct a {};
			CoordStruct z {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, true, i, a, z))
				continue;

			const double distance = HorizontalDistance(pAircraft->Location, a);

			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}

		return best;
	}

	struct RunwayStateGuard
	{
		CarrierRunwayState* storage;
		CarrierRunwayState* scratch;

		RunwayStateGuard(CarrierRunwayState& storage, CarrierRunwayState& scratch)
			: storage(&storage), scratch(&scratch)
		{
			*this->scratch = *this->storage;
		}

		~RunwayStateGuard()
		{
			*this->storage = *this->scratch;
		}
	};

	DirStruct GetRunwayDirection(const CoordStruct& start, const CoordStruct& end)
	{
		return DirStruct(std::atan2(
			static_cast<double>(start.Y - end.Y),
			static_cast<double>(end.X - start.X)));
	}

	double HorizontalDistance(const CoordStruct& a, const CoordStruct& b)
	{
		const double dx = static_cast<double>(a.X - b.X);
		const double dy = static_cast<double>(a.Y - b.Y);
		return std::sqrt(dx * dx + dy * dy);
	}

	CoordStruct GetRunwayApproachStart(const CoordStruct& start, const CoordStruct& end, int distance)
	{
		const double length = HorizontalDistance(start, end);

		if (length < 1.0 || distance <= 0)
			return start;

		const double dx = static_cast<double>(end.X - start.X) / length;
		const double dy = static_cast<double>(end.Y - start.Y) / length;

		return CoordStruct
		{
			start.X - static_cast<int>(std::lround(dx * distance)),
			start.Y - static_cast<int>(std::lround(dy * distance)),
			start.Z
		};
	}

	void UpdateCarrierMoveState(UnitClass* pCarrier)
	{
		if (!pCarrier)
			return;

		auto const it = s_carrierLastMoveFrame.find(pCarrier);

		if (it == s_carrierLastMoveFrame.end())
		{
			s_carrierMoveRef[pCarrier] = pCarrier->Location;
			s_carrierLastFacing[pCarrier] = pCarrier->PrimaryFacing.Current().Raw;
			s_carrierLastMoveFrame[pCarrier] = Unsorted::CurrentFrame;
			return;
		}

		const auto& ref = s_carrierMoveRef[pCarrier];

		// Ignore sub-cell position jitter; a real move is several leptons.
		if (std::abs(ref.X - pCarrier->Location.X) > 8
			|| std::abs(ref.Y - pCarrier->Location.Y) > 8
			|| std::abs(ref.Z - pCarrier->Location.Z) > 8)
		{
			s_carrierMoveRef[pCarrier] = pCarrier->Location;
			it->second = Unsorted::CurrentFrame;
		}

		const unsigned facing = pCarrier->PrimaryFacing.Current().Raw;
		auto& lastFacing = s_carrierLastFacing[pCarrier];

		// Ignore tiny facing jitter from the hull controller. Only a real
		// rotation (about 3 degrees) counts as movement.
		const int rawDiff = std::abs(static_cast<int>(facing) - static_cast<int>(lastFacing));
		const int wrappedDiff = std::min(rawDiff, 0x10000 - rawDiff);

		if (wrappedDiff > 512)
		{
			lastFacing = facing;
			it->second = Unsorted::CurrentFrame;
		}
	}

	bool IsCarrierStopped(UnitClass* pCarrier)
	{
		if (!pCarrier)
			return false;

		auto const it = s_carrierLastMoveFrame.find(pCarrier);

		if (it == s_carrierLastMoveFrame.end())
			return false;

		// Position AND facing must be unchanged for 15 frames. This also
		// catches a ship that is only rotating in place, which moves the deck
		// slots even when the hull position does not change.
		return Unsorted::CurrentFrame - it->second > 15;
	}

	CarrierLandingProgress& EnsureLandingProgress(FootprintState* pState, size_t index)
	{
		if (pState->CarrierPayloadLanding.size() <= index)
			pState->CarrierPayloadLanding.resize(index + 1);

		return pState->CarrierPayloadLanding[index];
	}

	bool IsLandingRunwayReserved(FootprintState* pState, int runwayIndex, AircraftClass* pSelf)
	{
		if (!pState || runwayIndex < 0
			|| runwayIndex >= static_cast<int>(pState->LandingRunways.size()))
		{
			return true;
		}

		auto const& runway = pState->LandingRunways[runwayIndex];

		if (runway.Occupant && runway.Occupant != pSelf)
			return true;

		const size_t count = std::min(pState->CarrierPayloads.size(),
			pState->CarrierPayloadLanding.size());

		bool selfExplicitWait = false;
		bool selfActiveReserved = false;

		for (size_t i = 0; i < count; ++i)
		{
			if (pState->CarrierPayloads[i] == pSelf)
			{
				auto const& selfProgress = pState->CarrierPayloadLanding[i];
				selfExplicitWait = selfProgress.ExplicitRunway
					&& selfProgress.RunwayIndex == runwayIndex
					&& selfProgress.Phase == CarrierLandingPhase::OrbitWait;

				// An aircraft that already holds the runway and has left the
				// holding pattern must not be blocked by another aircraft that
				// is merely waiting. Without this, an explicit waiter kept
				// knocking the active explicit aircraft back into OrbitWait and
				// both looped forever.
				selfActiveReserved = selfProgress.Reserved
					&& selfProgress.RunwayIndex == runwayIndex
					&& selfProgress.Phase != CarrierLandingPhase::None
					&& selfProgress.Phase != CarrierLandingPhase::GoAround
					&& selfProgress.Phase != CarrierLandingPhase::OrbitWait;
				break;
			}
		}

		for (size_t i = 0; i < count; ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft || pAircraft == pSelf)
				continue;

			auto const& progress = pState->CarrierPayloadLanding[i];

			if (progress.RunwayIndex != runwayIndex)
				continue;

			// OrbitWait is a waiting state, not an active reservation. Two
			// aircraft that both fell back to OrbitWait used to keep seeing
			// each other's stale Reserved flag and neither could ever land.
			if (progress.Reserved
				&& progress.Phase != CarrierLandingPhase::None
				&& progress.Phase != CarrierLandingPhase::GoAround
				&& progress.Phase != CarrierLandingPhase::OrbitWait)
			{
				return true;
			}

			// The other aircraft is only waiting. If we already own the runway
			// and have left OrbitWait, it must not block us.
			if (selfActiveReserved)
				continue;

			// Stable tie-breaker for two aircraft that are both still before
			// the runway (OrbitWait or ReturnToApproach) with no reservation
			// yet. The lower pointer always gets the runway first; this also
			// covers the automatic (non-explicit) return path, which has no
			// click order to break the tie.
			if ((progress.Phase == CarrierLandingPhase::OrbitWait
					|| progress.Phase == CarrierLandingPhase::ReturnToApproach)
				&& pAircraft < pSelf)
			{
				return true;
			}

			// An explicit click on an occupied runway blocks automatic
			// selection of that runway without creating a second reservation.
			// The explicit aircraft itself ignores this entry and waits for the
			// current occupant to leave.
			if (progress.ExplicitRunway
				&& progress.Phase == CarrierLandingPhase::OrbitWait)
			{
				// Block automatic returns unconditionally. Two explicit
				// requests for the same runway are resolved by a stable
				// pointer-order tie-breaker so they cannot deadlock.
				if (!selfExplicitWait || pAircraft < pSelf)
					return true;
			}
		}

		return false;
	}

	int ChooseFreeLandingRunway(UnitClass* pCarrier, FootprintState* pState,
		TechnoTypeExt::ExtData* pTypeExt, AircraftClass* pSelf,
		const CoordStruct& from, int requested, bool explicitRequest)
	{
		EnsureRunwayStates(pState, pTypeExt);
		const int count = static_cast<int>(pState->LandingRunways.size());

		if (count <= 0)
			return -1;

		if (requested >= 0 && requested < count)
		{
			if (!IsLandingRunwayReserved(pState, requested, pSelf) || explicitRequest)
				return requested;
		}

		int best = -1;
		double bestDistance = std::numeric_limits<double>::max();

		for (int i = 0; i < count; ++i)
		{
			if (i == requested || IsLandingRunwayReserved(pState, i, pSelf))
				continue;

			CoordStruct a {};
			CoordStruct z {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, true, i, a, z))
				continue;

			const double distance = HorizontalDistance(from, a);

			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}

		return best;
	}

	void ClearCarrierLandingProgress(FootprintState* pState, AircraftClass* pAircraft,
		size_t index, bool releaseRunway)
	{
		if (!pState)
			return;

		if (index < pState->CarrierPayloadLanding.size())
		{
			auto& progress = pState->CarrierPayloadLanding[index];

			if (releaseRunway && progress.Reserved
				&& progress.RunwayIndex >= 0
				&& progress.RunwayIndex < static_cast<int>(pState->LandingRunways.size()))
			{
				auto& runway = pState->LandingRunways[progress.RunwayIndex];

				if (runway.Occupant == pAircraft)
				{
					runway.Occupant = nullptr;
					runway.OccupantSlot = -1;
					runway.Phase = CarrierRunwayPhase::Idle;
					runway.Timer = 0;
					runway.Speed = 0.0;
					runway.TrailEnabled = false;
					runway.Path.clear();
					runway.PathIndex = 0;
				}
			}

			progress = CarrierLandingProgress {};
		}

		if (releaseRunway)
		{
			for (auto& runway : pState->LandingRunways)
			{
				if (runway.Occupant == pAircraft)
				{
					runway.Occupant = nullptr;
					runway.OccupantSlot = -1;
					runway.Phase = CarrierRunwayPhase::Idle;
					runway.TrailEnabled = false;
				}
			}
		}
	}

	void BeginCarrierLandingControl(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt,
		int runwayIndex, bool explicitRunway)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt)
			return;

		EnsureRunwayStates(pState, pTypeExt);
		ClearCarrierLandingProgress(pState, pAircraft, index, true);

		auto& progress = EnsureLandingProgress(pState, index);
		const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		progress.CruiseZ = MapClass::Instance.GetCellFloorHeight(pAircraft->Location) + flightLevel;
		progress.ExplicitRunway = explicitRunway;
		progress.LastPos = pAircraft->Location;

		const int count = static_cast<int>(pState->LandingRunways.size());
		int selected = -1;

		if (runwayIndex >= 0 && runwayIndex < count)
		{
			// An explicit runway is reserved even while occupied; the aircraft
			// orbits until that exact runway becomes free.
			selected = runwayIndex;
		}
		else
		{
			selected = ChooseFreeLandingRunway(pCarrier, pState, pTypeExt, pAircraft,
				pAircraft->Location, -1, false);
		}

		if (selected >= 0 && selected < count)
		{
			progress.RunwayIndex = selected;
			progress.ExplicitRunway = explicitRunway;

			// Only one aircraft may reserve a runway. An explicit request for
			// an occupied/reserved runway waits in OrbitWait without creating
			// a competing reservation; IsLandingRunwayReserved still keeps
			// automatic returns away from it.
			const bool runwayAvailable = !IsLandingRunwayReserved(pState, selected, pAircraft);
			progress.Reserved = runwayAvailable;
			progress.Phase = runwayAvailable
				? CarrierLandingPhase::ReturnToApproach
				: CarrierLandingPhase::OrbitWait;

			CoordStruct start {};
			CoordStruct end {};

			if (GetCarrierRunwayPoints(pCarrier, pTypeExt, true, selected, start, end))
			{
				progress.TouchdownPoint = start;
				progress.RollEndPoint = end;

				// The landing approach is on the extended runway centreline
				// before A, moving in the same A -> Z direction as takeoff.
				// Only the vertical profile is reversed: descend to A instead
				// of climbing away from Z.
				progress.ApproachPoint = GetRunwayApproachStart(start, end,
					GetRunwayApproachDistance(pTypeExt));
				progress.LineupPoint = progress.ApproachPoint;
				progress.RunwayFacingRaw = GetRunwayDirection(start, end).Raw;
			}
			else
			{
				progress.Phase = CarrierLandingPhase::OrbitWait;
				progress.RunwayIndex = -1;
				progress.Reserved = false;
			}
		}
		else
		{
			progress.RunwayIndex = -1;
			progress.Reserved = false;
			progress.Phase = CarrierLandingPhase::OrbitWait;
		}

		progress.PhaseTimer = 0;
		progress.FailedAttempts = 0;
		progress.StuckTimer = 0;
		progress.LastPos = pAircraft->Location;
		progress.LastPosTimer = 0;
		progress.Speed = 0.0;
		progress.TaxiPath.clear();
		progress.TaxiPathIndex = 0;
		progress.TaxiSpeed = 0.0;
		progress.TaxiTurnTimer = 0;

		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = true;
		pAircraft->SetSpeedPercentage(0.0);
		pAircraft->QueueMission(Mission::Sleep, true);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		pState->CarrierPayloadDocked[index] = 2;
		s_landingRunwayIndex[pAircraft] = progress.RunwayIndex;
		if (explicitRunway)
			s_landingRunwayExplicit.insert(pAircraft);
		else
			s_landingRunwayExplicit.erase(pAircraft);

		Debug::CarrierLog("[AircraftCarrier] %s landing control begins slot=%u runway=%d explicit=%d phase=%d carrierStopped=%d\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
			progress.RunwayIndex, explicitRunway ? 1 : 0,
			static_cast<int>(progress.Phase), IsCarrierStopped(pCarrier) ? 1 : 0);

		LogLandingProgress("BeginLanding", pAircraft, pState, index);
		LogCarrierRunways("BeginLanding", pState);
	}

	void CancelCarrierLanding(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, unsigned char& state, const char* reason,
		bool preservePlayerOrder = false)
	{
		if (!pAircraft || !pState)
			return;

		LogLandingProgress("CancelBefore", pAircraft, pState, index);
		ClearCarrierLandingProgress(pState, pAircraft, index, true);

		state = 0;
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = true;
		pAircraft->SetSpeedPercentage(1.0);

		if (!preservePlayerOrder)
		{
			pAircraft->Destination = nullptr;
			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);
			pAircraft->QueueMission(Mission::Sleep, true);
		}

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
		{
			pFly->IsLanding = false;
			pFly->IsTakingOff = false;
			pFly->IsElevating = false;
			pFly->FlightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		}

		s_landingRunwayIndex.erase(pAircraft);
		s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
		s_landingRunwayExplicit.erase(pAircraft);
		s_returnSuppressUntil[pAircraft] = Unsorted::CurrentFrame + 300;

		if (pCarrier)
			UpdateCarrierMoveState(pCarrier);

		Debug::CarrierLog("[AircraftCarrier] %s landing cancelled (%s) slot=%u\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", reason ? reason : "unknown",
			static_cast<unsigned>(index));
	}

	int GetCarrierTurnStep(AircraftClass* pAircraft)
	{
		if (!pAircraft)
			return 512;

		int step = static_cast<int>(pAircraft->PrimaryFacing.ROT.Raw);

		// Some types expose ROT in type units instead of the already scaled
		// facing rate. Fall back to a sane fixed-wing turn if the value would
		// make the aircraft effectively unable to turn.
		if (step < 64)
			step = 512;

		return std::clamp(step, 64, 4096);
	}

	// Ground taxi movement. This deliberately does not use the aircraft's
	// flight ROT or facing to decide where the aircraft moves: at low taxi
	// speed the flight turning radius is much larger than the deck and would
	// make the aircraft circle around the runway point. Vehicles follow the
	// path; the aircraft does the same here.
	void MoveCarrierTaxiStep(AircraftClass* pAircraft, CoordStruct& current,
		const CoordStruct& target, double speed)
	{
		if (!pAircraft)
			return;

		const double dx = static_cast<double>(target.X - current.X);
		const double dy = static_cast<double>(target.Y - current.Y);
		const double distance = std::sqrt(dx * dx + dy * dy);

		if (distance < 0.5)
			return;

		const double step = std::min(std::max(1.0, speed), distance);
		const auto facing = DirStruct(std::atan2(
			static_cast<double>(current.Y - target.Y),
			static_cast<double>(target.X - current.X)));

		current.X += static_cast<int>(std::lround(dx / distance * step));
		current.Y += static_cast<int>(std::lround(dy / distance * step));
		current.Z = target.Z;

		TurnAircraftToward(pAircraft, facing, GetCarrierTurnStep(pAircraft));
		pAircraft->SetLocation(current);
		pAircraft->InAir = false;
		pAircraft->DockNowHeadingTo = nullptr;

		// Bypass AssignDestination: the runway lock would reject our own clear.
		pAircraft->Destination = nullptr;

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();
	}

	// Move one frame along the current facing, turning toward the target at
	// the aircraft's own ROT. speed is in leptons per frame.
	void MoveCarrierAutopilot(AircraftClass* pAircraft, CoordStruct& current,
		const CoordStruct& target, double speed, int turnStep, bool forceDirect)
	{
		if (!pAircraft)
			return;

		const double mdx = static_cast<double>(target.X - current.X);
		const double mdy = static_cast<double>(target.Y - current.Y);
		const double moveDistance = std::sqrt(mdx * mdx + mdy * mdy);

		if (moveDistance < 0.5)
			return;

		const auto desired = DirStruct(std::atan2(
			static_cast<double>(current.Y - target.Y),
			static_cast<double>(target.X - current.X)));

		unsigned short raw = pAircraft->PrimaryFacing.Current().Raw;

		if (forceDirect || moveDistance <= speed)
		{
			raw = desired.Raw;
		}
		else
		{
			const int diff = static_cast<short>(desired.Raw - raw);
			raw = static_cast<unsigned short>(raw + std::clamp(diff, -turnStep, turnStep));
		}

		pAircraft->PrimaryFacing.SetCurrent(DirStruct(raw));
		pAircraft->PrimaryFacing.SetDesired(DirStruct(raw));

		const double rad = static_cast<double>(raw) * (kPi * 2.0 / 65536.0);
		const double step = std::min(speed, moveDistance);

		current.X += static_cast<int>(std::lround(std::sin(rad) * step));
		current.Y += static_cast<int>(std::lround(-std::cos(rad) * step));
	}

	// Direct steering for the manual landing controller. Unlike
	// MoveCarrierAutopilot (which moves along the current facing and can spiral
	// around a distant waypoint at cruise speed), this always moves along the
	// vector to the target while the visual facing turns toward it.
	void MoveCarrierDirect(AircraftClass* pAircraft, CoordStruct& current,
		const CoordStruct& target, double speed, int turnStep)
	{
		if (!pAircraft)
			return;

		const double dx = static_cast<double>(target.X - current.X);
		const double dy = static_cast<double>(target.Y - current.Y);
		const double distance = std::sqrt(dx * dx + dy * dy);

		if (distance < 0.5)
			return;

		const auto desired = DirStruct(std::atan2(
			static_cast<double>(current.Y - target.Y),
			static_cast<double>(target.X - current.X)));

		TurnAircraftToward(pAircraft, desired, turnStep);

		const double step = std::min(std::max(1.0, speed), distance);
		current.X += static_cast<int>(std::lround(dx / distance * step));
		current.Y += static_cast<int>(std::lround(dy / distance * step));
	}

	void UpdateCarrierFlightLevel(AircraftClass* pAircraft, const CoordStruct& location)
	{
		if (!pAircraft)
			return;

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
		{
			pFly->IsLanding = false;
			pFly->IsTakingOff = false;
			pFly->IsElevating = false;
			// On the deck the aircraft's altitude relative to the deck is 0.
			// Setting FlightLevel to the deck world Z here left the SHP body
			// and shadow one cell above the ship.
			pFly->FlightLevel = pAircraft->InAir
				? location.Z - MapClass::Instance.GetCellFloorHeight(location)
				: 0;
		}
	}

	DirStruct FacingFromDelta(int dx, int dy)
	{
		static const int forwardX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const int forwardY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

		const double length = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);

		if (length < 0.001)
			return DirStruct(0);

		int best = 0;
		double bestDot = -1.0e9;

		for (int bucket = 0; bucket < 8; ++bucket)
		{
			const double dot = (dx * forwardX[bucket] + dy * forwardY[bucket]) / length;

			if (dot > bestDot)
			{
				bestDot = dot;
				best = bucket;
			}
		}

		return DirStruct(best * 0x2000);
	}

	// A payload/proxy pointer restored from a save or retained during scenario
	// teardown can outlive its object. Never dereference it before checking
	// that the live container still owns it.
	bool IsLiveAircraft(AircraftClass* pAircraft)
	{
		if (!pAircraft || !AircraftClass::Array.IsInitialized)
			return false;

		return AircraftClass::Array.FindItemIndex(pAircraft) != -1;
	}

	bool IsLiveBuilding(BuildingClass* pBuilding)
	{
		if (!pBuilding || !BuildingClass::Array.IsInitialized)
			return false;

		return BuildingClass::Array.FindItemIndex(pBuilding) != -1;
	}

	std::vector<CoordStruct> BuildRunwayTaxiPath(UnitClass* pCarrier, FootprintState* pState,
		AircraftClass* pSelf, const CoordStruct& target)
	{
		if (!pCarrier || !pState || !pSelf)
			return {};

		const auto span = Footprint::GetSpan(pCarrier, pState->Anchor, pState->Bucket);

		if (span.empty())
			return {};

		using CellKey = std::pair<int, int>;

		std::set<CellKey> walkable;
		std::set<CellKey> blocked;

		for (auto const& cell : span)
			walkable.emplace(cell.X, cell.Y);

		auto const startCell = CellClass::Coord2Cell(pSelf->Location);
		auto const targetCell = CellClass::Coord2Cell(target);
		const CellKey start { startCell.X, startCell.Y };
		const CellKey goal { targetCell.X, targetCell.Y };

		walkable.insert(start);
		walkable.insert(goal);

		// A landing aircraft can stop beyond the hull span (the runway line
		// extends past the bow). Add the line between its current position and
		// the parking slot so it can taxi back onto the deck instead of
		// waiting forever for a path that only covers the hull.
		{
			const auto selfCoords = pSelf->Location;
			const double length = HorizontalDistance(selfCoords, target);
			const int steps = std::max(1, static_cast<int>(length / 128.0));

			for (int i = 0; i <= steps; ++i)
			{
				const double t = static_cast<double>(i) / static_cast<double>(steps);
				const auto point = CoordStruct
				{
					static_cast<int>(std::lround(selfCoords.X + (target.X - selfCoords.X) * t)),
					static_cast<int>(std::lround(selfCoords.Y + (target.Y - selfCoords.Y) * t)),
					target.Z
				};
				const auto cell = CellClass::Coord2Cell(point);
				walkable.emplace(cell.X, cell.Y);
			}
		}

		const size_t count = std::min(pState->CarrierPayloads.size(),
			pState->CarrierPayloadDocked.size());

		for (size_t i = 0; i < count; ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft || pAircraft == pSelf || !IsLiveAircraft(pAircraft))
				continue;

			const unsigned char state = pState->CarrierPayloadDocked[i];

			// Only aircraft actually on the deck block a taxi path. Airborne
			// payloads and returning aircraft are above the hull.
			if (state != 1 && state != 6 && state != 7)
				continue;

			const auto cell = CellClass::Coord2Cell(pAircraft->Location);
			blocked.emplace(cell.X, cell.Y);
		}

		// The path target may be the destination slot of a landing aircraft;
		// never mark the start/goal as blocked.
		blocked.erase(start);
		blocked.erase(goal);

		if (start == goal)
			return { target };

		auto const heuristic = [goal](const CellKey& cell)
		{
			const double dx = static_cast<double>(cell.first - goal.first);
			const double dy = static_cast<double>(cell.second - goal.second);
			return std::sqrt(dx * dx + dy * dy);
		};

		std::map<CellKey, double> gScore;
		std::map<CellKey, CellKey> cameFrom;
		std::set<CellKey> closed;
		using OpenNode = std::pair<double, CellKey>;
		std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<OpenNode>> open;

		gScore[start] = 0.0;
		open.emplace(heuristic(start), start);

		static const CellKey directions[8] =
		{
			{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
			{ 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
		};

		bool found = false;
		int iterations = 0;

		while (!open.empty() && iterations++ < 4096)
		{
			const auto [f, current] = open.top();
			(void)f;
			open.pop();

			if (current == goal)
			{
				found = true;
				break;
			}

			if (!closed.insert(current).second)
				continue;

			for (auto const& dir : directions)
			{
				const CellKey next { current.first + dir.first, current.second + dir.second };

				if (!walkable.contains(next) || (blocked.contains(next) && next != goal))
					continue;

				// Do not cut diagonally through a blocked or off-deck corner.
				if (dir.first != 0 && dir.second != 0)
				{
					const CellKey sideA { current.first + dir.first, current.second };
					const CellKey sideB { current.first, current.second + dir.second };

					if (!walkable.contains(sideA) || !walkable.contains(sideB)
						|| blocked.contains(sideA) || blocked.contains(sideB))
					{
						continue;
					}
				}

				const double cost = (dir.first != 0 && dir.second != 0) ? 1.41421356237 : 1.0;
				const double tentative = gScore[current] + cost;
				auto const it = gScore.find(next);

				if (it == gScore.end() || tentative < it->second)
				{
					gScore[next] = tentative;
					cameFrom[next] = current;
					open.emplace(tentative + heuristic(next), next);
				}
			}
		}

		if (!found)
			return {};

		std::vector<CellKey> cells;
		CellKey current = goal;

		while (current != start)
		{
			cells.push_back(current);
			auto const it = cameFrom.find(current);

			if (it == cameFrom.end())
				return {};

			current = it->second;
		}

		std::reverse(cells.begin(), cells.end());

		std::vector<CoordStruct> result;
		result.reserve(cells.size() + 1);

		for (auto const& cell : cells)
			result.push_back(CellClass::Cell2Coord(CellStruct { static_cast<short>(cell.first), static_cast<short>(cell.second) }, target.Z));

		result.push_back(target);
		return result;
	}

	// Legacy / pre-rewrite carrier builds used one real 1x1 BuildingClass per
	// deck slot as a native airport proxy. They are no longer needed; release
	// any that a loaded save restored so they cannot keep blocking cells.
	void ReleaseDockProxies(FootprintState* pState)
	{
		if (!pState)
			return;

		for (auto const pDock : pState->CarrierDockBuildings)
		{
			// A restored old save can contain a pointer that the engine has
			// already removed; dereferencing it here corrupts the heap during
			// scenario teardown.
			if (!IsLiveBuilding(pDock))
				continue;

			if (!pDock->InLimbo)
				pDock->Limbo();

			BuildingClass::Array.Remove(pDock);
			pDock->UnInit();
		}

		pState->CarrierDockBuildings.clear();
		pState->CarrierDockBuilding = nullptr;
	}

	void EnsureGrid()
	{
		if (s_gridReady)
			return;

		s_ownerNormal.assign(kMaxCells, nullptr);
		s_ownerAlt.assign(kMaxCells, nullptr);
		s_barrierNormal.assign(kMaxCells, nullptr);
		s_barrierAlt.assign(kMaxCells, nullptr);
		s_gridReady = true;
	}

	bool IsValidCell(const CellStruct& cell)
	{
		return cell.X >= 0 && cell.Y >= 0 && cell.X < kMapSide && cell.Y < kMapSide;
	}

	// A payload pointer restored from a save or retained during scenario
	// teardown can outlive its aircraft. Never dereference it before checking
	// that the live container still owns it.
	bool HasForeignOwner(UnitClass* pThis, const std::vector<CellStruct>& cells, bool alt)
	{
		if (!pThis)
			return true;

		for (const auto& cell : cells)
		{
			if (auto const pOwner = Footprint::OwnerAt(cell, alt); pOwner && pOwner != pThis)
				return true;

			if (auto const pBarrier = Footprint::BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
				return true;
		}

		return false;
	}

	int CellIndex(const CellStruct& cell)
	{
		if (!IsValidCell(cell))
			return -1;

		const int idx = MapClass::GetCellIndex(cell);
		return (idx >= 0 && idx < kMaxCells) ? idx : -1;
	}

	std::vector<CellStruct> MakeSpanAtAngle(const FootprintConfig& config, double angleDeg, double margin)
	{
		std::vector<CellStruct> result;

		const double angle = angleDeg * kDegToRad;
		const double forwardX = std::sin(angle);
		const double forwardY = -std::cos(angle);
		const double lateralX = std::cos(angle);
		const double lateralY = std::sin(angle);

		// MakeDiscreteSpan puts an even-sized hull at offsets -L/2+1 .. L/2,
		// so its geometric centre is +0.5 forward, not 0. Sampling around 0
		// added an extra cell at one end and biased the sweep. The diagonal
		// length margin used by the old 5x1 prototype must not be applied to a
		// 36x9 hull: it expands the 45-degree endpoints by several cells.
		const double centreForward = (config.Length % 2 == 0) ? 0.5 : 0.0;
		const double centreLateral = (config.Width % 2 == 0) ? 0.5 : 0.0;
		const double halfLength = config.Length * 0.5;
		const double halfWidth = config.Width * 0.5;
		const double radius = std::sqrt(halfLength * halfLength + halfWidth * halfWidth) + margin + 1.0;
		const int range = static_cast<int>(std::ceil(radius));

		for (int dy = -range; dy <= range; ++dy)
		{
			for (int dx = -range; dx <= range; ++dx)
			{
				const double forward = dx * forwardX + dy * forwardY
					- config.PivotForward - centreForward;
				const double lateral = dx * lateralX + dy * lateralY
					- config.PivotLateral - centreLateral;

				if (std::abs(forward) <= halfLength + margin + 1.0e-9
					&& std::abs(lateral) <= halfWidth + margin + 1.0e-9)
				{
					result.push_back(CellStruct { static_cast<short>(dx), static_cast<short>(dy) });
				}
			}
		}

		return result;
	}

	std::vector<CellStruct> MakeDiscreteSpan(const FootprintConfig& config, int bucket)
	{
		static const int forwardX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const int forwardY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
		static const int lateralX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static const int lateralY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

		const int b = bucket & 7;
		const int fMin = (config.Length % 2 == 1) ? -(config.Length / 2) : -(config.Length / 2) + 1;
		const int fMax = config.Length / 2;
		const int lMin = (config.Width % 2 == 1) ? -(config.Width / 2) : -(config.Width / 2) + 1;
		const int lMax = config.Width / 2;

		std::vector<CellStruct> result;
		result.reserve(static_cast<size_t>(config.Length * config.Width));

		for (int f = fMin; f <= fMax; ++f)
		{
			for (int l = lMin; l <= lMax; ++l)
			{
				result.push_back(CellStruct
				{
					static_cast<short>(f * forwardX[b] + l * lateralX[b]),
					static_cast<short>(f * forwardY[b] + l * lateralY[b])
				});
			}
		}

		return result;
	}

	void LogBlockedOccupy(UnitClass* pUnit, CellClass* pCell, const CellStruct& cell,
		const char* reason, int level, bool bridge, bool alt)
	{
		static int lastFrame = -100;
		static int count = 0;

		if (Unsorted::CurrentFrame != lastFrame)
		{
			lastFrame = Unsorted::CurrentFrame;
			count = 0;
		}

		if (count++ >= 8)
			return;

		const auto pType = pUnit ? pUnit->GetTechnoType() : nullptr;
		const auto pUnitContent = pCell ? pCell->GetUnit(alt) : nullptr;
		const auto pBuilding = pCell ? pCell->GetBuilding() : nullptr;
		const auto pTerrain = pCell ? pCell->GetTerrain(false) : nullptr;

		Debug::CarrierLog("[FootprintBlock] %s cell=(%d,%d) reason=%s"
			" type(SpeedType=%d MoveZone=%d) anchorLevel=%d/%d"
			" cell(level=%d bridge=%d land=%d pass=%d flags=%08X/%08X)"
			" unit=%s building=%s terrain=%s\n",
			pType ? pType->ID : "?",
			cell.X, cell.Y, reason,
			pType ? static_cast<int>(pType->SpeedType) : -1,
			pType ? static_cast<int>(pType->MovementZone) : -1,
			level, bridge ? 1 : 0,
			pCell ? pCell->GetLevel() : -1,
			pCell && pCell->ContainsBridge() ? 1 : 0,
			pCell ? static_cast<int>(pCell->LandType) : -1,
			pCell ? static_cast<int>(pCell->Passability) : -1,
			pCell ? pCell->OccupationFlags : 0,
			pCell ? pCell->AltOccupationFlags : 0,
			pUnitContent ? pUnitContent->GetTechnoType()->ID : "-",
			pBuilding ? pBuilding->Type->ID : "-",
			pTerrain ? pTerrain->Type->ID : "-");
	}

	std::vector<CellStruct> MakeSweepArc(const FootprintConfig& config, int from, int to, bool clockwise)
	{
		if (!config.Rotates || from == to)
			return MakeDiscreteSpan(config, from);

		std::set<std::pair<int, int>> cells;
		const int target = to & 7;
		const int step = clockwise ? 1 : -1;
		int bucket = from & 7;

		for (;;)
		{
			for (const auto& cell : MakeDiscreteSpan(config, bucket))
				cells.emplace(cell.X, cell.Y);

			if (bucket == target)
				break;

			bucket = (bucket + step + 8) & 7;
		}

		if (cells.empty())
			return {};

		// Return the actual swept hull cells, not the enclosing rectangle.
		// The locomotor reserves this result one 45-degree step at a time.
		std::vector<CellStruct> result;
		result.reserve(cells.size());

		for (const auto& cell : cells)
		{
			result.push_back(CellStruct
			{
				static_cast<short>(cell.first),
				static_cast<short>(cell.second)
			});
		}

		return result;
	}

	std::vector<CellStruct> MakeTurnStepSweep(const FootprintConfig& config,
		const CellStruct& anchor, int from, bool clockwise)
	{
		std::set<std::pair<int, int>> cells;
		const int step = clockwise ? 1 : -1;
		const int startBucket = from & 7;
		const int nextBucket = (startBucket + step + 8) & 7;
		const double startAngle = startBucket * 45.0;
		const double endAngle = startAngle + step * 45.0;

		// Sample the continuous 45-degree arc. Sampling only the two discrete
		// endpoint spans misses the end-cap cells swept while the hull rotates
		// between them, which is exactly the hollow area visible in issue1.
		constexpr int kSamples = 16;

		for (int i = 0; i <= kSamples; ++i)
		{
			const double t = static_cast<double>(i) / static_cast<double>(kSamples);
			const double angle = startAngle + (endAngle - startAngle) * t;

			for (const auto& cell : MakeSpanAtAngle(config, angle, 0.0))
				cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);
		}

		// Keep the exact discrete footprint at both ends. The point-sampled arc
		// can under-fill a cell whose square is only clipped at the endpoint.
		for (const auto& cell : MakeDiscreteSpan(config, startBucket))
			cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);

		for (const auto& cell : MakeDiscreteSpan(config, nextBucket))
			cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);

		std::vector<CellStruct> result;
		result.reserve(cells.size());

		for (const auto& [x, y] : cells)
			result.push_back(CellStruct { static_cast<short>(x), static_cast<short>(y) });

		return result;
	}

	void FillSingleCellGaps(std::set<std::pair<int, int>>& cells)
	{
		if (cells.empty())
			return;

		int minX = cells.begin()->first;
		int maxX = minX;
		int minY = cells.begin()->second;
		int maxY = minY;

		for (const auto& [x, y] : cells)
		{
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}

		bool changed = true;

		while (changed)
		{
			changed = false;

			for (int y = minY; y <= maxY; ++y)
			{
				for (int x = minX; x <= maxX; ++x)
				{
					const std::pair<int, int> cell { x, y };

					if (cells.find(cell) != cells.end())
						continue;

					const bool horizontalGap = cells.find({ x - 1, y }) != cells.end()
						&& cells.find({ x + 1, y }) != cells.end();
					const bool verticalGap = cells.find({ x, y - 1 }) != cells.end()
						&& cells.find({ x, y + 1 }) != cells.end();

					if (horizontalGap || verticalGap)
					{
						cells.insert(cell);
						changed = true;
					}
				}
			}
		}
	}

	void FillEnclosedHoles(std::set<std::pair<int, int>>& cells)
	{
		if (cells.empty())
			return;

		int minX = cells.begin()->first;
		int maxX = minX;
		int minY = cells.begin()->second;
		int maxY = minY;

		for (const auto& [x, y] : cells)
		{
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}

		const int x0 = minX - 1;
		const int y0 = minY - 1;
		const int x1 = maxX + 1;
		const int y1 = maxY + 1;
		std::set<std::pair<int, int>> outside;
		std::queue<std::pair<int, int>> queue;

		auto pushOutside = [&](int x, int y)
		{
			if (x < x0 || y < y0 || x > x1 || y > y1)
				return;

			const std::pair<int, int> cell { x, y };

			if (cells.find(cell) != cells.end() || outside.find(cell) != outside.end())
				return;

			outside.insert(cell);
			queue.push(cell);
		};

		for (int x = x0; x <= x1; ++x)
		{
			pushOutside(x, y0);
			pushOutside(x, y1);
		}

		for (int y = y0; y <= y1; ++y)
		{
			pushOutside(x0, y);
			pushOutside(x1, y);
		}

		while (!queue.empty())
		{
			const auto [x, y] = queue.front();
			queue.pop();

			pushOutside(x - 1, y);
			pushOutside(x + 1, y);
			pushOutside(x, y - 1);
			pushOutside(x, y + 1);
		}

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const std::pair<int, int> cell { x, y };

				if (cells.find(cell) == cells.end() && outside.find(cell) == outside.end())
					cells.insert(cell);
			}
		}
	}

	std::vector<CellStruct> MakeStepReservation(
		const std::vector<CellStruct>& currentHull,
		const std::vector<CellStruct>& currentBarriers,
		const std::vector<CellStruct>& sweep)
	{
		std::set<std::pair<int, int>> current;
		std::set<std::pair<int, int>> occupied;

		for (const auto& cell : currentHull)
		{
			current.emplace(cell.X, cell.Y);
			occupied.emplace(cell.X, cell.Y);
		}

		for (const auto& cell : currentBarriers)
		{
			current.emplace(cell.X, cell.Y);
			occupied.emplace(cell.X, cell.Y);
		}

		for (const auto& cell : sweep)
			occupied.emplace(cell.X, cell.Y);

		// Close single-cell sampling jumps first (the two missing cells at the
		// 45-degree tips), then close any larger enclosed holes. A hollow ring
		// of swept cells would otherwise draw as an empty white box and let
		// normal units sit inside the reservation.
		FillSingleCellGaps(occupied);
		FillEnclosedHoles(occupied);

		std::vector<CellStruct> result;
		result.reserve(occupied.size());

		for (const auto& [x, y] : occupied)
		{
			if (current.find({ x, y }) == current.end())
				result.push_back(CellStruct { static_cast<short>(x), static_cast<short>(y) });
		}

		return result;
	}
}

void FootprintGeometry::Build(const FootprintConfig& config)
{
	if (Built)
		return;

	const auto primary = MakeDiscreteSpan(config, 0);

	for (int bucket = 0; bucket < 8; ++bucket)
	{
		Span[bucket] = config.Rotates
			? MakeDiscreteSpan(config, bucket)
			: primary;

		CellStruct min { 0, 0 };
		CellStruct max { 0, 0 };

		for (const auto& cell : Span[bucket])
		{
			min.X = std::min(min.X, cell.X);
			min.Y = std::min(min.Y, cell.Y);
			max.X = std::max(max.X, cell.X);
			max.Y = std::max(max.Y, cell.Y);
		}

		Min[bucket] = min;
		Max[bucket] = max;
	}

	for (int from = 0; from < 8; ++from)
	{
		for (int to = 0; to < 8; ++to)
		{
			const int index = from * 8 + to;

			if (!config.Rotates || from == to)
			{
				SweepCW[index] = Span[from];
				SweepCCW[index] = Span[from];
				continue;
			}

			SweepCW[index] = MakeSweepArc(config, from, to, true);
			SweepCCW[index] = MakeSweepArc(config, from, to, false);
		}
	}

	Built = true;
}

bool Footprint::MultiCellAllowed()
{
	if (Phobos::Config::MultiCellDisabled)
		return false;

	return !(Phobos::Config::MultiCellDisabledInMultiplayer
		&& SessionClass::IsMultiplayer());
}

bool Footprint::IsMultiCell(TechnoTypeClass* pType)
{
	if (!pType || !MultiCellAllowed())
		return false;

	const auto pExt = TechnoTypeExt::ExtMap.TryFind(pType);

	if (!pExt || !pExt->MultiCell.Enabled)
		return false;

	return pExt->MultiCell.Length > 1 || pExt->MultiCell.Width > 1;
}

bool Footprint::IsMultiCell(TechnoClass* pThis)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Unit)
		return false;

	return IsMultiCell(pThis->GetTechnoType());
}

const FootprintConfig& Footprint::GetConfig(TechnoTypeClass* pType)
{
	return TechnoTypeExt::ExtMap.Find(pType)->MultiCell;
}

const FootprintGeometry& Footprint::GetGeometry(TechnoTypeClass* pType)
{
	auto it = s_geometries.find(pType);

	if (it == s_geometries.end())
	{
		FootprintGeometry geometry;
		geometry.Build(GetConfig(pType));
		it = s_geometries.emplace(pType, std::move(geometry)).first;
	}

	return it->second;
}

FootprintState* Footprint::GetState(TechnoClass* pThis)
{
	if (!pThis)
		return nullptr;

	const auto pExt = TechnoExt::ExtMap.TryFind(pThis);
	return pExt ? &pExt->Footprint : nullptr;
}

int Footprint::BucketFromFacing(const DirStruct& facing)
{
	return static_cast<int>(facing.GetValue<3>()) & 7;
}

CellStruct Footprint::AnchorFromCoords(const CoordStruct& coords)
{
	return CellStruct
	{
		static_cast<short>(coords.X / Unsorted::LeptonsPerCell),
		static_cast<short>(coords.Y / Unsorted::LeptonsPerCell)
	};
}

std::vector<CellStruct> Footprint::GetSpan(TechnoClass* pThis, const CellStruct& anchor, int bucket)
{
	std::vector<CellStruct> result;

	if (!pThis)
		return result;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);

	if (!config.Enabled)
	{
		result.push_back(anchor);
		return result;
	}

	const auto& geometry = GetGeometry(pType);
	const int index = config.Rotates ? (bucket & 7) : 0;

	for (const auto& offset : geometry.Span[index])
		result.push_back(anchor + offset);

	return result;
}

std::vector<CellStruct> Footprint::GetSweep(TechnoClass* pThis, int from, int to, bool clockwise)
{
	if (!pThis)
		return {};

	return GetSweepAt(pThis, pThis->GetMapCoords(), from, to, clockwise);
}

std::vector<CellStruct> Footprint::GetSweepAt(TechnoClass* pThis, const CellStruct& anchor, int from, int to, bool clockwise)
{
	std::vector<CellStruct> result;

	if (!pThis)
		return result;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);

	if (!config.Enabled)
		return result;

	if (!config.Rotates || from == to)
		return GetSpan(pThis, anchor, from);

	const auto& geometry = GetGeometry(pType);
	const auto& offsets = clockwise
		? geometry.SweepCW[(from & 7) * 8 + (to & 7)]
		: geometry.SweepCCW[(from & 7) * 8 + (to & 7)];

	for (const auto& offset : offsets)
		result.push_back(anchor + offset);

	return result;
}

bool Footprint::ReserveTurnStep(UnitClass* pThis, const CellStruct& anchor,
	int from, int to, bool clockwise, bool alt)
{
	if (!pThis)
		return false;

	const auto pState = GetState(pThis);

	if (!pState)
		return false;

	const int start = from & 7;

	if ((to & 7) == start)
		return true;

	const int step = clockwise ? 1 : -1;
	const int next = (start + step + 8) & 7;
	const auto& config = GetConfig(pThis->GetTechnoType());

	// Reserve the actual hull area swept while rotating from the current
	// 45-degree bucket to the next one. This includes temporary end-cap cells
	// that are not part of either endpoint footprint and must stay occupied
	// until the turn passes them. It is not an axis-aligned envelope, and the
	// next bucket's barrier cells are committed separately when the turn ends.
	const auto sweep = MakeTurnStepSweep(config, anchor, start, clockwise);
	const auto reservation = MakeStepReservation(
		pState->Span, pState->BarrierSpan, sweep);

	if (reservation.empty() || !CanOccupy(pThis, reservation, alt, anchor))
		return false;

	ReleaseReservation(pThis);

	if (!ReserveSweep(pThis, reservation, alt))
		return false;

	Debug::CarrierLog("[FootprintReserve] %s cell=(%d,%d) %d -> %d (%s) next=%d reserved=%d\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, start, to & 7,
		clockwise ? "cw" : "ccw", next, static_cast<int>(pState->ReservedSpan.size()));

	return true;
}

UnitClass* Footprint::OwnerAt(const CellStruct& cell, bool alt)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return nullptr;

	return alt ? s_ownerAlt[index] : s_ownerNormal[index];
}

UnitClass* Footprint::OwnerAt(const CellClass* pCell, bool alt)
{
	return pCell ? OwnerAt(pCell->MapCoords, alt) : nullptr;
}

UnitClass* Footprint::FindOwnerByScan(const CellStruct& cell, bool alt)
{
	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
			continue;

		const auto pState = GetState(pUnit);

		if (!pState || !pState->Initialized || pState->Alt != alt)
			continue;

		if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
		{
			SetOwner(cell, alt, pUnit);
			return pUnit;
		}
	}

	return nullptr;
}

void Footprint::SetOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	(alt ? s_ownerAlt : s_ownerNormal)[index] = pOwner;
}

void Footprint::ClearOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	auto& owner = (alt ? s_ownerAlt : s_ownerNormal)[index];

	if (owner == pOwner)
		owner = nullptr;
}

UnitClass* Footprint::BarrierOwnerAt(const CellStruct& cell, bool alt)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return nullptr;

	return alt ? s_barrierAlt[index] : s_barrierNormal[index];
}

UnitClass* Footprint::BarrierOwnerAt(const CellClass* pCell, bool alt)
{
	return pCell ? BarrierOwnerAt(pCell->MapCoords, alt) : nullptr;
}

void Footprint::SetBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	(alt ? s_barrierAlt : s_barrierNormal)[index] = pOwner;
}

void Footprint::ClearBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	auto& owner = (alt ? s_barrierAlt : s_barrierNormal)[index];

	if (owner == pOwner)
		owner = nullptr;
}

std::vector<CellStruct> Footprint::ComputeBarriers(const std::vector<CellStruct>& span)
{
	std::set<std::pair<int, int>> hull;
	std::set<std::pair<int, int>> barriers;

	for (const auto& cell : span)
		hull.emplace(cell.X, cell.Y);

	for (const auto& cell : span)
	{
		for (int dx : { -1, 1 })
		{
			for (int dy : { -1, 1 })
			{
				if (hull.find({ cell.X + dx, cell.Y + dy }) == hull.end())
					continue;

				const std::pair<int, int> sideA { cell.X + dx, cell.Y };
				const std::pair<int, int> sideB { cell.X, cell.Y + dy };

				if (hull.find(sideA) == hull.end())
					barriers.insert(sideA);

				if (hull.find(sideB) == hull.end())
					barriers.insert(sideB);
			}
		}
	}

	std::vector<CellStruct> result;
	result.reserve(barriers.size());

	for (const auto& cell : barriers)
	{
		result.push_back(CellStruct
		{
			static_cast<short>(cell.first),
			static_cast<short>(cell.second)
		});
	}

	return result;
}

bool Footprint::IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt)
{
	if (!pThis || !pCell)
		return false;

	if (OwnerAt(pCell, alt) == pThis)
		return true;

	if (BarrierOwnerAt(pCell, alt) == pThis)
		return true;

	return false;
}

bool Footprint::IsCarrierHullCell(TechnoClass* pThis, const CellStruct& cell)
{
	if (!pThis)
		return false;

	if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
	{
		if (IsOwnCell(pThis, pCell, false) || IsOwnCell(pThis, pCell, true))
			return true;
	}

	const auto pState = GetState(pThis);

	if (!pState || !pState->Initialized)
		return false;

	if (pState->Anchor == cell)
		return true;

	for (auto const& offset : pState->Span)
	{
		if (offset == cell)
			return true;

		const CellStruct absolute
		{
			static_cast<short>(pState->Anchor.X + offset.X),
			static_cast<short>(pState->Anchor.Y + offset.Y)
		};

		if (absolute == cell)
			return true;
	}

	return false;
}

bool Footprint::HasObstacleNear(TechnoClass* pThis, int radius)
{
	if (!pThis || radius <= 0)
		return false;

	const auto center = pThis->GetMapCoords();

	for (int dy = -radius; dy <= radius; ++dy)
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			const CellStruct cell
			{
				static_cast<short>(center.X + dx),
				static_cast<short>(center.Y + dy)
			};

			if (const auto pOwner = OwnerAt(cell, false); pOwner && pOwner != pThis)
				return true;

			if (const auto pBarrier = BarrierOwnerAt(cell, false); pBarrier && pBarrier != pThis)
				return true;

			if (const auto pOwner = OwnerAt(cell, true); pOwner && pOwner != pThis)
				return true;

			if (const auto pBarrier = BarrierOwnerAt(cell, true); pBarrier && pBarrier != pThis)
				return true;
		}
	}

	return false;
}

bool Footprint::CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, CellStruct* pBlocked)
{
	return pThis
		? CanOccupy(pThis, span, alt, pThis->GetMapCoords(), pBlocked)
		: false;
}

bool Footprint::CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, const CellStruct& anchor, CellStruct* pBlocked)
{
	if (!pThis)
		return false;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);
	const auto pAnchorCell = MapClass::Instance.TryGetCellAt(anchor);

	if (!pAnchorCell)
		return false;

	const int level = pAnchorCell->GetLevel();
	const bool bridge = pAnchorCell->ContainsBridge();
	const int maxClimb = std::max(0, config.MaxClimb);

	// [CDEST] relies on inherited MovementZone; if a hull uses Float but inherited
	// Normal/None, treat it as Water instead of rejecting every water cell.
	auto movementZone = pType->MovementZone;

	if (pType->SpeedType == SpeedType::Float
		&& (movementZone == MovementZone::None || movementZone == MovementZone::Normal))
	{
		movementZone = MovementZone::Water;
	}

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
		{
			LogBlockedOccupy(pThis, nullptr, cell, "outside-map", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		const auto pBarrierOwner = BarrierOwnerAt(cell, alt);

		if (pBarrierOwner && pBarrierOwner != pThis)
		{
			LogBlockedOccupy(pThis, pCell, cell, "barrier", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->ContainsBridge() != bridge)
		{
			LogBlockedOccupy(pThis, pCell, cell, "level-bridge", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		const int cellLevel = pCell->GetLevel();

		if (std::abs(cellLevel - level) > maxClimb)
		{
			LogBlockedOccupy(pThis, pCell, cell, "slope", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		const auto pCellBuilding = pCell->GetBuilding();
		const auto pState = GetState(pThis);

		// The carrier's hidden AIRCARRDOCK proxies sit on its deck. They must
		// not block the carrier that owns them, including the occupation flags
		// checked by IsClearToMove(), otherwise the ship can never move.
		const bool ownDock = pState && pCellBuilding
			&& (pCellBuilding == pState->CarrierDockBuilding
				|| std::find(pState->CarrierDockBuildings.begin(),
					pState->CarrierDockBuildings.end(), pCellBuilding) != pState->CarrierDockBuildings.end());

		// Ignore infantry and vehicles here; vehicles are checked below so that
		// the unit's own footprint cells stay passable, and infantry is handled
		// by the crush setting.
		if (!ownDock
			&& !pCell->IsClearToMove(pType->SpeedType, true, true, -1, movementZone, cellLevel, bridge))
		{
			LogBlockedOccupy(pThis, pCell, cell, "clear-to-move", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCellBuilding && !ownDock)
		{
			LogBlockedOccupy(pThis, pCell, cell, "building", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (const auto pTerrain = pCell->GetTerrain(false))
		{
			if (!TerrainTypeExt::ExtMap.Find(pTerrain->Type)->IsPassable)
			{
				LogBlockedOccupy(pThis, pCell, cell, "terrain", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}

		if (const auto pUnit = pCell->GetUnit(alt))
		{
			if (pUnit != pThis)
			{
				LogBlockedOccupy(pThis, pCell, cell, "unit", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}
		else if ((alt ? pCell->AltOccupationFlags : pCell->OccupationFlags) & 0x20)
		{
			// A vehicle bit without a matching object is either a stale
			// footprint, a barrier cell, or a vanilla unit whose cell lookup is
			// unavailable. Own barriers are passable for the hull itself.
			if (!pBarrierOwner || pBarrierOwner != pThis)
			{
				LogBlockedOccupy(pThis, pCell, cell, "stale-flag", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}

		if (!config.Crush && pCell->GetInfantry(false))
		{
			LogBlockedOccupy(pThis, pCell, cell, "infantry", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->OverlayTypeIndex >= 0)
		{
			const auto pOverlay = OverlayTypeClass::Array.Items[pCell->OverlayTypeIndex];

			if (pOverlay && pOverlay->Wall)
			{
				LogBlockedOccupy(pThis, pCell, cell, "wall", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}
	}

	return true;
}

bool Footprint::IsClimbAllowed(UnitClass* pThis, const CellStruct& from, const CellStruct& to)
{
	if (!pThis)
		return false;

	const auto pFromCell = MapClass::Instance.TryGetCellAt(from);
	const auto pToCell = MapClass::Instance.TryGetCellAt(to);

	if (!pFromCell || !pToCell || pFromCell->ContainsBridge() != pToCell->ContainsBridge())
		return false;

	const auto& config = GetConfig(pThis->GetTechnoType());
	return std::abs(pToCell->GetLevel() - pFromCell->GetLevel()) <= std::max(0, config.MaxClimb);
}

void Footprint::EnsureAircraftCarrierSpawns(UnitClass* pThis)
{
	if (!pThis || !pThis->Owner || !pThis->IsAlive || pThis->InLimbo || pThis->IsSinking)
		return;

	const auto pType = pThis->GetTechnoType();
	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

	if (!pTypeExt || !TechnoTypeExt::IsAircraftCarrier(pType))
		return;

	const auto pState = GetState(pThis);

	if (!pState)
		return;

	if (HasRunwayMode(pTypeExt) && s_runwayLoggedCarriers.insert(pThis).second)
	{
		CoordStruct start {};
		CoordStruct end {};

		const int logRunway = ChooseNearestRunway(pThis, pTypeExt, false, pThis->Location);

		if (GetCarrierRunwayPoints(pThis, pTypeExt, false, logRunway, start, end))
		{
			Debug::CarrierLog("[AircraftCarrier] runway mode A=(%d,%d,%d) Z=(%d,%d,%d)\n",
				start.X, start.Y, start.Z, end.X, end.Y, end.Z);
		}
	}

	// Old saves / pre-placed units may still carry a SpawnManager created by
	// the earlier carrier implementation. It creates a second set of J35s
	// behind the normal payload objects, so remove it completely.
	if (pThis->SpawnManager)
	{
		pThis->SpawnManager->KillNodes();
		GameDelete(pThis->SpawnManager);
		pThis->SpawnManager = nullptr;
		pState->CarrierPayloadsInitialized = false;
		pState->CarrierPayloads.clear();
		pState->CarrierPayloadDocked.clear();
		pState->CarrierPayloadLastCoords.clear();
		pState->CarrierPayloadReloadTimer.clear();
		pState->CarrierPayloadReturnCell.clear();

		Debug::CarrierLog("[AircraftCarrier] removed legacy SpawnManager for %s\n", pType->ID);
	}

	// The deck uses no native airport proxies. Saves made with the old proxy
	// build may still restore them; release them so they cannot block cells.
	if (pState->CarrierDockBuilding || !pState->CarrierDockBuildings.empty())
	{
		ReleaseDockProxies(pState);
		Debug::CarrierLog("[AircraftCarrier] released legacy dock proxies for %s\n", pType->ID);
	}

	if (pState->CarrierPayloadsInitialized)
	{
		// The payload lookup is only a cache; rebuild it after a load.
		for (auto const pAircraft : pState->CarrierPayloads)
		{
			if (pAircraft)
				s_carrierPayloads[pAircraft] = pThis;
		}

		const size_t count = pState->CarrierPayloads.size();

		// Keep the parallel arrays in step with saves that predate a field.
		if (pState->CarrierPayloadDocked.size() < count)
			pState->CarrierPayloadDocked.resize(count, 1);

		if (pState->CarrierPayloadLastCoords.size() < count)
			pState->CarrierPayloadLastCoords.resize(count, CoordStruct::Empty);

		if (pState->CarrierPayloadReloadTimer.size() < count)
			pState->CarrierPayloadReloadTimer.resize(count, 0);

		if (pState->CarrierPayloadIgnoredDest.size() < count)
			pState->CarrierPayloadIgnoredDest.resize(count, CellStruct { -1, -1 });

		if (pState->CarrierPayloadLaunchRequest.size() < count)
			pState->CarrierPayloadLaunchRequest.resize(count, 0);

		if (pState->CarrierPayloadLandingTimer.size() < count)
			pState->CarrierPayloadLandingTimer.resize(count, 0);

		if (pState->CarrierPayloadLanding.size() < count)
			pState->CarrierPayloadLanding.resize(count);

		if (pState->CarrierPayloadRequestedDest.size() < count)
			pState->CarrierPayloadRequestedDest.resize(count, CellStruct { -1, -1 });

		if (pState->CarrierPayloadReturnCell.size() < count)
			pState->CarrierPayloadReturnCell.resize(count, CellStruct { -1, -1 });

		// ReturnCell is transient and no longer written to saves. When a
		// returning aircraft is restored, rebuild it from the engine Move
		// destination so state 2 does not mistake its own return order for a
		// new player command and cancel the approach.
		for (size_t i = 0; i < count; ++i)
		{
			const unsigned char state = pState->CarrierPayloadDocked[i];

			if (state < 2 || state > 5
				|| pState->CarrierPayloadReturnCell[i].X >= 0)
			{
				continue;
			}

			if (auto const pAircraft = pState->CarrierPayloads[i];
				pAircraft && pAircraft->Destination)
			{
				pState->CarrierPayloadReturnCell[i] =
					CellClass::Coord2Cell(pAircraft->Destination->GetCoords());
			}
		}

		const bool moved = pState->CarrierLastCoords.X != pThis->Location.X
			|| pState->CarrierLastCoords.Y != pThis->Location.Y
			|| pState->CarrierLastCoords.Z != pThis->Location.Z
			|| pState->CarrierLastBucket != pState->Bucket;

		for (size_t i = 0; i < count; ++i)
		{
			const auto pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			// Only state 1 is parked. State 2 is returning and must keep flying:
			// treating any non-zero value as parked teleported it to the deck.
			if (pState->CarrierPayloadDocked[i] != 1)
				continue;

			const auto expected = GetCarrierDockSlotCoords(pThis, pTypeExt, i);

			// The facing controller keeps rotating parked aircraft back to its
			// own desired heading, so the tuned slot facing must be written
			// every frame, not only when the position drifts.
			const DirStruct facing = GetCarrierSlotFacing(pThis, pTypeExt, i);
			pAircraft->PrimaryFacing.SetDesired(facing);
			pAircraft->InAir = false;

			const bool positionUnchanged = !moved
				&& pAircraft->Location.X == expected.X
				&& pAircraft->Location.Y == expected.Y
				&& pAircraft->Location.Z == expected.Z;

			if (!positionUnchanged)
				pAircraft->SetLocation(expected);

			pState->CarrierPayloadLastCoords[i] = expected;
		}

		pState->CarrierLastCoords = pThis->Location;
		pState->CarrierLastBucket = pState->Bucket;
		return;
	}

	const int docks = std::max(0, pTypeExt->AircraftCarrier_NumberOfDocks.Get());

	if (!pTypeExt->AircraftCarrier_Payloads.empty() && docks > 0)
	{
		std::vector<AircraftTypeClass*> slotTypes;
		slotTypes.reserve(docks);

		for (size_t i = 0; i < pTypeExt->AircraftCarrier_Payloads.size() && static_cast<int>(slotTypes.size()) < docks; ++i)
		{
			const auto pPayload = pTypeExt->AircraftCarrier_Payloads[i];

			if (!pPayload)
				continue;

			int count = 1;

			if (i < pTypeExt->AircraftCarrier_PayloadsNum.size())
				count = pTypeExt->AircraftCarrier_PayloadsNum[i];
			else if (pTypeExt->AircraftCarrier_PayloadsNum.size() == 1)
				count = pTypeExt->AircraftCarrier_PayloadsNum[0];

			count = std::clamp(count, 0, docks - static_cast<int>(slotTypes.size()));

			for (int j = 0; j < count; ++j)
				slotTypes.push_back(pPayload);
		}

		for (size_t i = 0; i < slotTypes.size(); ++i)
		{
			auto const pAircraft = static_cast<AircraftClass*>(slotTypes[i]->CreateObject(pThis->Owner));

			if (!pAircraft)
				continue;

			const auto coords = GetCarrierDockSlotCoords(pThis, pTypeExt, i);

			pAircraft->Spawned = false;
			pAircraft->SpawnOwner = nullptr;
			pAircraft->SetLocation(coords);

			if (!pAircraft->Unlimbo(coords, pThis->PrimaryFacing.Current().GetDir()))
			{
				Debug::Log("[AircraftCarrier] failed to Unlimbo payload %s for %s\n",
					slotTypes[i]->ID, pType->ID);
				continue;
			}

			// Unlimbo snaps Z back to the ground. Restore the deck altitude
			// immediately so the aircraft does not pop up one frame later.
			pAircraft->SetLocation(coords);

			// Unlimbo does not reliably preserve the aircraft's parked facing.
			// Set both current and desired, otherwise the fly facing controller
			// rotates the parked aircraft back to its default direction.
			const DirStruct facing = GetCarrierSlotFacing(pThis, pTypeExt, i);
			pAircraft->PrimaryFacing.SetCurrent(facing);
			pAircraft->PrimaryFacing.SetDesired(facing);

			// Parked payloads are real units, but they must not be treated as
			// airborne targets while they sit on the deck.
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->SetDestination(nullptr, true);
			pAircraft->InAir = false;
			pAircraft->QueueMission(Mission::Sleep, true);

			pState->CarrierPayloads.push_back(pAircraft);
			pState->CarrierPayloadDocked.push_back(1);
			pState->CarrierPayloadLastCoords.push_back(pAircraft->Location);
			pState->CarrierPayloadReloadTimer.push_back(-60);
			pState->CarrierPayloadLaunchRequest.push_back(0);
			pState->CarrierPayloadLandingTimer.push_back(0);
			pState->CarrierPayloadLanding.push_back(CarrierLandingProgress {});
			pState->CarrierPayloadRequestedDest.push_back(CellStruct { -1, -1 });
			pState->CarrierPayloadReturnCell.push_back(CellStruct { -1, -1 });
			s_carrierPayloads[pAircraft] = pThis;
		}
	}

	for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
	{
		auto const pAircraft = pState->CarrierPayloads[i];

		if (!pAircraft)
			continue;

		const int ruleDir = i < pTypeExt->AircraftCarrier_DockingDirs.size()
			? pTypeExt->AircraftCarrier_DockingDirs[i]
			: 0;
		const auto expected = GetCarrierDockSlotCoords(pThis, pTypeExt, i);
		const auto facing = GetCarrierSlotFacing(pThis, pTypeExt, i);

		Debug::CarrierLog("[AircraftCarrier] deck %s slot=%d ruleDir=%d facing=%u carrierFacing=%u expected=(%d,%d,%d) actual=(%d,%d,%d)\n",
			pType->ID, static_cast<int>(i), ruleDir,
			static_cast<unsigned>(facing.Raw),
			static_cast<unsigned>(pThis->PrimaryFacing.Current().Raw),
			expected.X, expected.Y, expected.Z,
			pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
	}

	pState->CarrierPayloadsInitialized = true;
	pState->CarrierLastCoords = pThis->Location;
	pState->CarrierLastBucket = pState->Bucket;

	Debug::CarrierLog("[AircraftCarrier] initialized %d payload aircraft for %s\n",
		static_cast<int>(pState->CarrierPayloads.size()), pType->ID);
}

UnitClass* Footprint::GetCarrierPayloadOwner(TechnoClass* pPayload)
{
	auto const it = s_carrierPayloads.find(pPayload);
	return it == s_carrierPayloads.end() ? nullptr : it->second;
}

bool Footprint::IsCarrierPayloadLanding(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];
	// state 2 (native return flight) must keep the vanilla aircraft update.
	// Only the manual final alignment / vertical descent owns the aircraft.
	return state == 3 || state == 4 || state == 5;
}

bool Footprint::IsCarrierPayloadRunwayControlled(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];

	// State 2 (manual return/orbit) is deliberately not "runway controlled":
	// it must still receive player orders so they can cancel the landing.
	return state == 6 || state == 7 || state == 8;
}

bool Footprint::IsCarrierPayloadLandingSequence(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];
	return state == 2 || state == 7;
}

bool Footprint::IsCarrierPayloadRunwayCommandLocked(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];
	return state == 6 || state == 7 || state == 8;
}

bool Footprint::IsCarrierPayloadRunwayTrailEnabled(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];

	// Takeoff-roll only. Alignment, landing and departure must not enable
	// the aircraft's trail; this is also what gates the J35 LaserTrail.
	if (state != 6)
		return false;

	for (auto const& runway : pState->TakeoffRunways)
	{
		if (runway.Occupant == pAircraft)
			return runway.TrailEnabled;
	}

	return false;
}

int Footprint::GetCarrierPayloadDeckZ(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return -1;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return -1;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return -1;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return -1;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

	if (!pTypeExt)
		return -1;

	return GetCarrierDockSlotCoords(pCarrier, pTypeExt, index).Z;
}

bool Footprint::IsCarrierPayloadParked(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	return index < pState->CarrierPayloadDocked.size()
		&& pState->CarrierPayloadDocked[index] == 1;
}

bool Footprint::IsCarrierPayloadDeckControlled(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char state = pState->CarrierPayloadDocked[index];

	// State 0 is normal airborne flight and must keep the engine's AI.
	return state >= 1 && state <= 8;
}

void Footprint::NotifyDestinationAssigned(TechnoClass* pThis, AbstractClass* pDest)
{
	if (!pThis || !pDest || pThis->WhatAmI() != AbstractType::Aircraft)
		return;

	const auto pCarrier = GetCarrierPayloadOwner(pThis);

	if (!pCarrier || !pCarrier->IsAlive)
		return;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto pAircraft = static_cast<AircraftClass*>(pThis);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size() || pState->CarrierPayloadDocked[index] != 1)
		return;

	// Do NOT turn this into a launch request. AssignDestination is also used
	// by the engine for selection/return/landing bookkeeping; doing so made a
	// parked aircraft take off just because it was selected. Launch requests
	// are captured only from real player clicks (CaptureClickDestination /
	// CaptureAttackClick).
	const auto destCoords = pDest->GetCoords();

	Debug::CarrierLog("[CarrierCmd] AssignDest slot=%d selected=%d state=%d mission=%d destCell=(%d,%d,%d) plane=(%d,%d,%d)\n",
		static_cast<int>(index), pAircraft->IsSelected ? 1 : 0,
		static_cast<int>(pState->CarrierPayloadDocked[index]),
		static_cast<int>(pAircraft->GetCurrentMission()),
		destCoords.X, destCoords.Y, destCoords.Z,
		pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
}

void Footprint::LogPayloadCommand(const char* tag, TechnoClass* pThis, AbstractClass* pTarget)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Aircraft)
		return;

	// A runaway order loop can call this thousands of times in one frame.
	// Log at most once per aircraft per frame; the reentrancy breaker below
	// reports the loop itself.
	static TechnoClass* s_lastLoggedPlane = nullptr;
	static int s_lastLoggedFrame = -1;

	if (pThis == s_lastLoggedPlane && s_lastLoggedFrame == Unsorted::CurrentFrame)
		return;

	s_lastLoggedPlane = pThis;
	s_lastLoggedFrame = Unsorted::CurrentFrame;

	const auto pCarrier = GetCarrierPayloadOwner(pThis);

	if (!pCarrier || !pCarrier->IsAlive)
		return;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto pAircraft = static_cast<AircraftClass*>(pThis);
	const auto it = std::find(pState->CarrierPayloads.begin(),
		pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(
		std::distance(pState->CarrierPayloads.begin(), it));

	const unsigned char state = index < pState->CarrierPayloadDocked.size()
		? pState->CarrierPayloadDocked[index] : 255;
	const auto coords = pTarget ? pTarget->GetCoords() : CoordStruct::Empty;
	const CarrierLandingPhase phase = index < pState->CarrierPayloadLanding.size()
		? pState->CarrierPayloadLanding[index].Phase : CarrierLandingPhase::None;

	Debug::CarrierLog("[DiagCmd] %s f=%d plane=%p state=%u(%s) phase=%s mission=%d sel=%d target=%p coords=(%d,%d,%d) loc=(%d,%d,%d) inAir=%d dest=%p\n",
		tag ? tag : "?", Unsorted::CurrentFrame, static_cast<void*>(pAircraft),
		static_cast<unsigned>(state), PayloadStateName(state),
		LandingPhaseName(phase), static_cast<int>(pAircraft->GetCurrentMission()),
		pAircraft->IsSelected ? 1 : 0, static_cast<void*>(pTarget),
		coords.X, coords.Y, coords.Z,
		pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
		pAircraft->InAir ? 1 : 0, static_cast<void*>(pAircraft->Destination));
}

void Footprint::RememberRunwayCommandDestination(TechnoClass* pThis, AbstractClass* pDest)
{
	if (!pThis || !pDest || pThis->WhatAmI() != AbstractType::Aircraft)
		return;

	const auto pCarrier = GetCarrierPayloadOwner(pThis);

	if (!pCarrier || !pCarrier->IsAlive)
		return;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto pAircraft = static_cast<AircraftClass*>(pThis);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return;

	const unsigned char state = pState->CarrierPayloadDocked[index];

	if (state != 6 && state != 8)
		return;

	const int runwayIndex = FindTakeoffRunwayIndex(pState, pAircraft);

	if (runwayIndex < 0)
		return;

	auto& runway = pState->TakeoffRunways[runwayIndex];
	const CellStruct destCell = CellClass::Coord2Cell(pDest->GetCoords());

	if (state == 8)
	{
		for (auto& task : runway.Departures)
		{
			if (task.Aircraft == pAircraft)
			{
				task.CommandDestination = destCell;
				task.CommandTarget = nullptr;
				task.CommandMission = static_cast<int>(Mission::Move);
				return;
			}
		}

		return;
	}

	if (runway.TakeoffRequest == pAircraft)
	{
		runway.PendingDestination = destCell;
		runway.PendingTarget = nullptr;
		runway.PendingMission = static_cast<int>(Mission::Move);
	}
	else if (runway.Occupant == pAircraft)
	{
		runway.CommandDestination = destCell;
		runway.CommandTarget = nullptr;
		runway.CommandMission = static_cast<int>(Mission::Move);
	}
}

void Footprint::RememberRunwayCommandTarget(TechnoClass* pThis, AbstractClass* pTarget)
{
	if (!pThis || !pTarget || pThis->WhatAmI() != AbstractType::Aircraft)
		return;

	const auto pTechno = abstract_cast<TechnoClass*>(pTarget);

	if (!pTechno)
		return;

	const auto pCarrier = GetCarrierPayloadOwner(pThis);

	if (!pCarrier || !pCarrier->IsAlive)
		return;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto pAircraft = static_cast<AircraftClass*>(pThis);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return;

	const unsigned char state = pState->CarrierPayloadDocked[index];

	if (state != 6 && state != 8)
		return;

	const int runwayIndex = FindTakeoffRunwayIndex(pState, pAircraft);

	if (runwayIndex < 0)
		return;

	auto& runway = pState->TakeoffRunways[runwayIndex];

	if (state == 8)
	{
		for (auto& task : runway.Departures)
		{
			if (task.Aircraft == pAircraft)
			{
				task.CommandDestination = CellStruct { -1, -1 };
				task.CommandTarget = pTechno;
				task.CommandMission = static_cast<int>(Mission::Attack);
				return;
			}
		}

		return;
	}

	if (runway.TakeoffRequest == pAircraft)
	{
		runway.PendingDestination = CellStruct { -1, -1 };
		runway.PendingTarget = pTechno;
		runway.PendingMission = static_cast<int>(Mission::Attack);
	}
	else if (runway.Occupant == pAircraft)
	{
		runway.CommandDestination = CellStruct { -1, -1 };
		runway.CommandTarget = pTechno;
		runway.CommandMission = static_cast<int>(Mission::Attack);
	}
}

void Footprint::ProcessPendingKills()
{
	if (s_pendingKills.empty())
		return;

	auto pending = std::move(s_pendingKills);
	s_pendingKills.clear();

	for (auto const pAircraft : pending)
	{
		// The pointer may have been invalidated between queueing and this
		// global tick. Validate membership before touching any field.
		if (IsLiveAircraft(pAircraft))
			TechnoExt::Kill(pAircraft, nullptr, pAircraft->Owner);
	}
}

	void RequestRunwayTakeoff(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state);

	int FindLandingRunwayAtCell(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt,
		const CellStruct& cell)
	{
		const int count = static_cast<int>(GetCarrierRunwayCount(pTypeExt, true));

		for (int i = 0; i < count; ++i)
		{
			CoordStruct a {};
			CoordStruct z {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, true, i, a, z))
				continue;

			const auto aCell = CellClass::Coord2Cell(a);
			const auto zCell = CellClass::Coord2Cell(z);
			const int da = std::abs(aCell.X - cell.X) + std::abs(aCell.Y - cell.Y);
			const int dz = std::abs(zCell.X - cell.X) + std::abs(zCell.Y - cell.Y);

			if (da <= 2 || dz <= 2)
				return i;

			// Clicking anywhere on the runway line also selects that runway.
			const double vx = static_cast<double>(zCell.X - aCell.X);
			const double vy = static_cast<double>(zCell.Y - aCell.Y);
			const double wx = static_cast<double>(cell.X - aCell.X);
			const double wy = static_cast<double>(cell.Y - aCell.Y);
			const double lengthSq = vx * vx + vy * vy;

			if (lengthSq > 0.001)
			{
				const double t = std::clamp((wx * vx + wy * vy) / lengthSq, 0.0, 1.0);
				const double px = aCell.X + vx * t;
				const double py = aCell.Y + vy * t;
				const double dx = cell.X - px;
				const double dy = cell.Y - py;

				if (std::sqrt(dx * dx + dy * dy) <= 1.5)
					return i;
			}
		}

		return -1;
	}

	void RequestRunwayLanding(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, int runwayIndex)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt)
			return;

		if (index >= pState->CarrierPayloadDocked.size())
			return;

		// Entry point used only by ProcessCarrierIntents (before object AI).
		// It must not be called from click/target/mission callbacks.
		BeginCarrierLandingControl(pCarrier, pAircraft, index, pState, pTypeExt,
			runwayIndex, runwayIndex >= 0);
	}

static CellStruct s_lastMouseCell { -1, -1 };

void Footprint::SetLastMouseCell(const CellStruct& cell)
{
	s_lastMouseCell = cell;
}

CellStruct Footprint::GetLastMouseCell()
{
	return s_lastMouseCell;
}

void Footprint::CaptureClickDestination(const CellStruct& cell)
{
	if (cell.X < 0 || cell.Y < 0
		|| cell.X >= MapClass::MaxCells || cell.Y >= MapClass::MaxCells)
	{
		return;
	}

	Debug::CarrierLog("[DiagClick] CaptureClickDestination f=%d cell=(%d,%d) selected=%d\n",
		Unsorted::CurrentFrame, cell.X, cell.Y, ObjectClass::CurrentObjects.Count);

	for (int i = 0; i < ObjectClass::CurrentObjects.Count; ++i)
	{
		auto const pTechno = abstract_cast<TechnoClass*>(ObjectClass::CurrentObjects.GetItem(i));

		if (!pTechno)
			continue;

		const auto pCarrier = GetCarrierPayloadOwner(pTechno);

		if (!pCarrier || !pCarrier->IsAlive)
			continue;

		if (IsCarrierPayloadRunwayControlled(pTechno))
			continue;

		const auto pState = GetState(pCarrier);

		if (!pState)
			continue;

		const auto pAircraft = static_cast<AircraftClass*>(pTechno);
		const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

		if (it == pState->CarrierPayloads.end())
			continue;

		const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

		if (index >= pState->CarrierPayloadDocked.size())
			continue;

		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());
		const unsigned char currentState = pState->CarrierPayloadDocked[index];

		Debug::CarrierLog("[DiagClick]   candidate plane=%p state=%u(%s) selected=%d carrier=%p runwayMode=%d\n",
			static_cast<void*>(pAircraft), static_cast<unsigned>(currentState),
			PayloadStateName(currentState), pAircraft->IsSelected ? 1 : 0,
			static_cast<void*>(pCarrier), HasRunwayMode(pTypeExt, true) ? 1 : 0);

		// A selected airborne payload clicking its carrier or a configured
		// landing runway requests a manual landing, even with full ammo.
		if (currentState == 0 && pAircraft->IsSelected && HasRunwayMode(pTypeExt, true))
		{
			const int specificRunway = FindLandingRunwayAtCell(pCarrier, pTypeExt, cell);
			const bool ownCell = IsCarrierHullCell(pCarrier, cell);

			Debug::CarrierLog("[DiagClick]   landing-click ownCell=%d specificRunway=%d cell=(%d,%d)\n",
				ownCell ? 1 : 0, specificRunway, cell.X, cell.Y);

			if (ownCell || specificRunway >= 0)
			{
				Footprint::QueueCarrierLandingIntent(pAircraft, pCarrier,
					specificRunway, specificRunway >= 0, false);
				continue;
			}
		}

		// A selected payload that is already returning/landing must still
		// accept a real move click. Defer the cancel+move to the global tick;
		// the engine's Enter conversion is suppressed by the carrier gate.
		if (currentState >= 2 && currentState <= 5 && pAircraft->IsSelected)
		{
			const bool ownCell = IsCarrierHullCell(pCarrier, cell);

			if (!ownCell)
			{
				s_carrierCancelIntents.push_back({ pAircraft, cell });
				Debug::CarrierLog("[DiagClick]   cancel landing intent plane=%p cell=(%d,%d)\n",
					static_cast<void*>(pAircraft), cell.X, cell.Y);
				continue;
			}
		}

		if (currentState != 1)
			continue;

		// Clicks on the carrier's own cells are return/selection, not a move
		// order. Recording them as a launch request made the aircraft take off
		// again right after landing.
		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (IsOwnCell(pCarrier, pCell, pState->Alt))
				continue;
		}

		if (pState->CarrierPayloadLaunchRequest.size() > index)
			pState->CarrierPayloadLaunchRequest[index] = 1;

		if (pState->CarrierPayloadRequestedDest.size() > index)
			pState->CarrierPayloadRequestedDest[index] = cell;

		if (HasRunwayMode(pTypeExt, false))
			Footprint::QueueCarrierTakeoffIntent(pAircraft, pCarrier);
	}
}

void Footprint::CaptureAttackClick(TechnoClass* pTarget)
{
	for (int i = 0; i < ObjectClass::CurrentObjects.Count; ++i)
	{
		auto const pTechno = abstract_cast<TechnoClass*>(ObjectClass::CurrentObjects.GetItem(i));

		if (!pTechno)
			continue;

		const auto pCarrier = GetCarrierPayloadOwner(pTechno);

		if (!pCarrier || !pCarrier->IsAlive)
			continue;

		if (IsCarrierPayloadRunwayControlled(pTechno))
			continue;

		// Clicking the carrier itself is selection / return, never an attack.
		if (pTarget == pCarrier)
			continue;

		// Clicking a friendly unit/building is selection, not an attack order.
		if (!pTarget->Owner || pTarget->Owner == pTechno->Owner
			|| pTarget->Owner->IsAlliedWith(pTechno->Owner))
		{
			continue;
		}

		const auto pState = GetState(pCarrier);

		if (!pState)
			continue;

		const auto pAircraft = static_cast<AircraftClass*>(pTechno);
		const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

		if (it == pState->CarrierPayloads.end())
			continue;

		const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

		if (index < pState->CarrierPayloadDocked.size() && pState->CarrierPayloadDocked[index] == 1
			&& pState->CarrierPayloadLaunchRequest.size() > index)
		{
			pState->CarrierPayloadLaunchRequest[index] = 1;
			pAircraft->SetTarget(pTarget);
			pAircraft->SetArchiveTarget(pTarget);

			if (auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());
				HasRunwayMode(pTypeExt, false))
			{
				Footprint::QueueCarrierTakeoffIntent(pAircraft, pCarrier);
			}
		}
	}
}

bool Footprint::RequestCarrierLandingFromMission(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char payloadState = pState->CarrierPayloadDocked[index];

	// Already returning/landing: swallow Enter/Return so the native no-dock
	// path cannot recurse. State 6/7/8 is handled by the command lock.
	if (payloadState == 2 || payloadState == 3 || payloadState == 4 || payloadState == 5)
		return true;

	if (payloadState != 0)
		return false;

	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

	if (!pTypeExt || !HasRunwayMode(pTypeExt, true))
		return false;

	// Do not mutate carrier/payload state from inside the engine mission
	// callback. Record a deferred intent; ProcessCarrierIntents runs before
	// any object AI on the next frame.
	Footprint::QueueCarrierLandingIntent(pAircraft, pCarrier, -1, false, false);
	return true;
}

bool Footprint::ShouldSuppressCarrierMission(TechnoClass* pPayload, Mission mission)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return false;

	const unsigned char payloadState = pState->CarrierPayloadDocked[index];

	// The runway controller owns every order while the aircraft is taxiing,
	// rolling, departing or on the landing runway. Let only Sleep through.
	if (payloadState == 6 || payloadState == 7 || payloadState == 8)
		return mission != Mission::Sleep;

	// A parked payload is owned by the deck pin. The engine's idle mission AI
	// otherwise re-queues Guard/Area_Guard every tick and re-assigns the
	// aircraft's own slot cell as a destination, which makes the whole deck
	// drift and rotate. Move/Attack/Stop stay allowed so player/AI launch
	// orders can still be detected by the state-1 handler.
	if (payloadState == 1)
	{
		switch (mission)
		{
		case Mission::Enter:
		case Mission::Return:
		case Mission::Guard:
		case Mission::Area_Guard:
		case Mission::Hunt:
		case Mission::Ambush:
		case Mission::Sticky:
			return true;
		default:
			return false;
		}
	}

	// Enter/Return is the vanilla "go home" mission. A carrier has no native
	// dock, so this path retries/re-enters forever. For an already returning
	// payload it must never reach MissionClass. For an airborne payload it is
	// converted into our deferred landing intent.
	if (mission == Mission::Enter || mission == Mission::Return)
	{
		if (payloadState != 0)
			return true;

		return RequestCarrierLandingFromMission(pPayload);
	}

	return false;
}

bool Footprint::CaptureCarrierReturnClick(TechnoClass* pTarget)
{
	if (!pTarget)
		return false;

	bool handled = false;

	for (int i = 0; i < ObjectClass::CurrentObjects.Count; ++i)
	{
		auto const pTechno = abstract_cast<TechnoClass*>(ObjectClass::CurrentObjects.GetItem(i));

		if (!pTechno || pTechno->WhatAmI() != AbstractType::Aircraft)
			continue;

		const auto pCarrier = GetCarrierPayloadOwner(pTechno);

		if (!pCarrier || pCarrier != pTarget || !pCarrier->IsAlive)
			continue;

		const auto pState = GetState(pCarrier);

		if (!pState)
			continue;

		const auto pAircraft = static_cast<AircraftClass*>(pTechno);
		const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

		if (it == pState->CarrierPayloads.end())
			continue;

		const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

		if (index >= pState->CarrierPayloadDocked.size()
			|| pState->CarrierPayloadDocked[index] != 0)
		{
			continue;
		}

		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

		if (!pTypeExt || !HasRunwayMode(pTypeExt, true))
			continue;

		// Resolve the actual clicked cell so clicking a runway line selects
		// that exact runway instead of silently falling back to automatic
		// selection.
		int specificRunway = -1;

		// The click hook cached the cell that DisplayClass resolved this frame.
		const CellStruct cell = Footprint::GetLastMouseCell();

		if (cell.X >= 0 && cell.Y >= 0)
			specificRunway = FindLandingRunwayAtCell(pCarrier, pTypeExt, cell);

		// Defer until ProcessCarrierIntents. Do not mutate the payload from
		// inside the click/target-resolution path.
		Footprint::QueueCarrierLandingIntent(pAircraft, pCarrier,
			specificRunway, specificRunway >= 0, false);
		handled = true;
	}

	return handled;
}

void Footprint::QueueCarrierLandingIntent(AircraftClass* pAircraft, UnitClass* pCarrier,
	int runwayIndex, bool explicitRunway, bool autoReturn)
{
	if (!pAircraft || !pCarrier || !pAircraft->IsAlive || !pCarrier->IsAlive)
		return;

	if (!s_carrierLandingIntentAircraft.insert(pAircraft).second)
	{
		// A pending intent may already exist (carrier click + clicked runway
		// cell). Explicit runway information always wins.
		if (explicitRunway)
		{
			for (auto& pending : s_carrierLandingIntents)
			{
				if (pending.Aircraft == pAircraft)
				{
					pending.RunwayIndex = runwayIndex;
					pending.ExplicitRunway = true;
					pending.Kind = CarrierLandingIntentKind::ManualRunwayClick;
					break;
				}
			}
		}

		return;
	}

	CarrierLandingIntent intent;
	intent.Aircraft = pAircraft;
	intent.Carrier = pCarrier;
	intent.RunwayIndex = runwayIndex;
	intent.ExplicitRunway = explicitRunway;
	intent.Kind = autoReturn
		? CarrierLandingIntentKind::AutoOutOfAmmo
		: (explicitRunway
			? CarrierLandingIntentKind::ManualRunwayClick
			: CarrierLandingIntentKind::ManualCarrierClick);
	intent.Frame = Unsorted::CurrentFrame;
	s_carrierLandingIntents.push_back(intent);

	Debug::CarrierLog("[CarrierIntent] queue landing %s carrier=%s runway=%d explicit=%d auto=%d\n",
		pAircraft->Type ? pAircraft->Type->ID : "?",
		pCarrier->Type ? pCarrier->Type->ID : "?",
		runwayIndex, explicitRunway ? 1 : 0, autoReturn ? 1 : 0);
}

void Footprint::QueueCarrierTakeoffIntent(AircraftClass* pAircraft, UnitClass* pCarrier)
{
	if (!pAircraft || !pCarrier || !pAircraft->IsAlive || !pCarrier->IsAlive)
		return;

	if (!s_carrierTakeoffIntentAircraft.insert(pAircraft).second)
		return;

	CarrierTakeoffIntent intent;
	intent.Aircraft = pAircraft;
	intent.Carrier = pCarrier;
	intent.Frame = Unsorted::CurrentFrame;
	s_carrierTakeoffIntents.push_back(intent);

	Debug::CarrierLog("[CarrierIntent] queue takeoff %s carrier=%s\n",
		pAircraft->Type ? pAircraft->Type->ID : "?",
		pCarrier->Type ? pCarrier->Type->ID : "?");
}

void Footprint::ProcessCarrierIntents()
{
	if (!s_carrierLandingIntents.empty() || !s_carrierTakeoffIntents.empty()
		|| !s_carrierTakeoffHandoffs.empty())
	{
		Debug::CarrierLog("[DiagIntents] f=%d landingIntents=%u takeoffIntents=%u handoffs=%u\n",
			Unsorted::CurrentFrame,
			static_cast<unsigned>(s_carrierLandingIntents.size()),
			static_cast<unsigned>(s_carrierTakeoffIntents.size()),
			static_cast<unsigned>(s_carrierTakeoffHandoffs.size()));
	}

	// A real player click on a returning/landing payload cancels the landing
	// and applies the clicked destination. This cannot go through the engine
	// because an airport-bound destination becomes Mission::Enter, which the
	// carrier gate intentionally suppresses.
	if (!s_carrierCancelIntents.empty())
	{
		auto intents = std::move(s_carrierCancelIntents);
		s_carrierCancelIntents.clear();

		for (auto const& intent : intents)
		{
			auto const pAircraft = intent.Aircraft;

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			auto const pCarrier = GetCarrierPayloadOwner(pAircraft);

			if (!pCarrier || !pCarrier->IsAlive)
				continue;

			auto const pState = GetState(pCarrier);

			if (!pState)
				continue;

			auto const it = std::find(pState->CarrierPayloads.begin(),
				pState->CarrierPayloads.end(), pAircraft);

			if (it == pState->CarrierPayloads.end())
				continue;

			const size_t index = static_cast<size_t>(
				std::distance(pState->CarrierPayloads.begin(), it));

			if (index >= pState->CarrierPayloadDocked.size())
				continue;

			const unsigned char payloadState = pState->CarrierPayloadDocked[index];

			if (payloadState < 2 || payloadState > 5)
				continue;

			CancelCarrierLanding(pCarrier, pAircraft, index, pState,
				pState->CarrierPayloadDocked[index], "player click", true);

			if (auto const pCell = MapClass::Instance.TryGetCellAt(intent.Destination))
			{
				pAircraft->SetDestination(pCell, true);
				pAircraft->QueueMission(Mission::Move, true);
			}

			Debug::CarrierLog("[AircraftCarrier] %s landing cancelled by click, move to (%d,%d)\n",
				pAircraft->Type ? pAircraft->Type->ID : "?",
				intent.Destination.X, intent.Destination.Y);
		}
	}

	// Restore commands that were captured when a takeoff departure completed.
	// This runs before any object AI, never from inside an aircraft Update.
	if (!s_carrierTakeoffHandoffs.empty())
	{
		auto handoffs = std::move(s_carrierTakeoffHandoffs);
		s_carrierTakeoffHandoffs.clear();

		for (auto const& handoff : handoffs)
		{
			auto const pAircraft = handoff.Aircraft;
			auto const pCarrier = handoff.Carrier;

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			if (!pCarrier || !pCarrier->IsAlive)
				continue;

			auto const pState = GetState(pCarrier);

			if (!pState)
				continue;

			auto const it = std::find(pState->CarrierPayloads.begin(),
				pState->CarrierPayloads.end(), pAircraft);

			if (it == pState->CarrierPayloads.end())
				continue;

			const size_t index = static_cast<size_t>(
				std::distance(pState->CarrierPayloads.begin(), it));

			if (index >= pState->CarrierPayloadDocked.size()
				|| pState->CarrierPayloadDocked[index] != 0)
			{
				continue;
			}

			const auto commandMission = static_cast<Mission>(handoff.CommandMission);
			const bool hasDestination = handoff.CommandDestination.X >= 0;
			const bool attackCommand = handoff.CommandTarget
				&& (commandMission == Mission::Attack || commandMission == Mission::AttackMove);

			if (handoff.CommandTarget)
				pAircraft->SetTarget(handoff.CommandTarget);

			if (hasDestination)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(handoff.CommandDestination))
					pAircraft->SetDestination(pCell, true);
			}

			if (handoff.CommandTarget && hasDestination && commandMission == Mission::AttackMove)
				pAircraft->QueueMission(Mission::AttackMove, true);
			else if (attackCommand || (handoff.CommandTarget && !hasDestination))
				pAircraft->QueueMission(Mission::Attack, true);
			else if (hasDestination)
				pAircraft->QueueMission(
					commandMission == Mission::QMove ? Mission::QMove : Mission::Move, true);
			else
				pAircraft->QueueMission(Mission::Guard, false);

			Debug::CarrierLog("[AircraftCarrier] %s deferred takeoff handoff slot=%u completed\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));
		}
	}

	// Landing intents.
	if (!s_carrierLandingIntents.empty())
	{
		auto intents = std::move(s_carrierLandingIntents);
		s_carrierLandingIntents.clear();

		for (auto const& intent : intents)
		{
			auto const pAircraft = intent.Aircraft;
			auto const pCarrier = intent.Carrier;

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			if (!pCarrier || !pCarrier->IsAlive)
				continue;

			auto const pState = GetState(pCarrier);

			if (!pState)
				continue;

			auto const it = std::find(pState->CarrierPayloads.begin(),
				pState->CarrierPayloads.end(), pAircraft);

			if (it == pState->CarrierPayloads.end())
				continue;

			const size_t index = static_cast<size_t>(
				std::distance(pState->CarrierPayloads.begin(), it));

			if (index >= pState->CarrierPayloadDocked.size()
				|| pState->CarrierPayloadDocked[index] != 0)
			{
				continue;
			}

			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

			if (!pTypeExt)
				continue;

			// No landing runway configured: fall back to the legacy manual
			// slot descent (state 3/4/5). This is still controller-driven and
			// does not use Mission::Enter/Return.
			if (!HasRunwayMode(pTypeExt, true))
			{
				pState->CarrierPayloadDocked[index] = 3;
				pAircraft->Destination = nullptr;
				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->DockNowHeadingTo = nullptr;
				pAircraft->InAir = true;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = 0;

				Debug::CarrierLog("[AircraftCarrier] %s no landing runway, legacy slot descent slot=%u\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));
				continue;
			}

			Debug::CarrierLog("[DiagIntent] landing plane=%p carrier=%p kind=%d runway=%d explicit=%d state=%u selected=%d\n",
				static_cast<void*>(pAircraft), static_cast<void*>(pCarrier),
				static_cast<int>(intent.Kind), intent.RunwayIndex,
				intent.ExplicitRunway ? 1 : 0,
				static_cast<unsigned>(pState->CarrierPayloadDocked[index]),
				pAircraft->IsSelected ? 1 : 0);

			UpdateCarrierMoveState(pCarrier);

			if (!IsCarrierStopped(pCarrier))
			{
				Debug::CarrierLog("[AircraftCarrier] %s landing request rejected: carrier moving (runway=%d)\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", intent.RunwayIndex);

				if (intent.Kind == CarrierLandingIntentKind::AutoOutOfAmmo)
					s_returnSuppressUntil[pAircraft] = Unsorted::CurrentFrame + 300;

				continue;
			}

			int runway = intent.RunwayIndex;
			bool explicitRunway = intent.ExplicitRunway;

			if (explicitRunway
				&& (runway < 0 || runway >= static_cast<int>(pState->LandingRunways.size())))
			{
				explicitRunway = false;
				runway = -1;
			}

			if (!explicitRunway)
			{
				runway = ChooseFreeLandingRunway(pCarrier, pState, pTypeExt,
					pAircraft, pAircraft->Location, runway, false);
			}

			RequestRunwayLanding(pCarrier, pAircraft, index, pState, pTypeExt, runway);

			LogLandingProgress("AfterLandingIntent", pAircraft, pState, index);
			LogCarrierRunways("AfterLandingIntent", pState);
		}

		s_carrierLandingIntentAircraft.clear();
	}

	// Takeoff intents.
	if (!s_carrierTakeoffIntents.empty())
	{
		auto intents = std::move(s_carrierTakeoffIntents);
		s_carrierTakeoffIntents.clear();

		for (auto const& intent : intents)
		{
			auto const pAircraft = intent.Aircraft;
			auto const pCarrier = intent.Carrier;

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			if (!pCarrier || !pCarrier->IsAlive)
				continue;

			auto const pState = GetState(pCarrier);

			if (!pState)
				continue;

			auto const it = std::find(pState->CarrierPayloads.begin(),
				pState->CarrierPayloads.end(), pAircraft);

			if (it == pState->CarrierPayloads.end())
				continue;

			const size_t index = static_cast<size_t>(
				std::distance(pState->CarrierPayloads.begin(), it));

			if (index >= pState->CarrierPayloadDocked.size()
				|| pState->CarrierPayloadDocked[index] != 1)
			{
				continue;
			}

			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

			if (!pTypeExt || !HasRunwayMode(pTypeExt, false))
				continue;

			UpdateCarrierMoveState(pCarrier);
			RequestRunwayTakeoff(pCarrier, pAircraft, index, pState, pTypeExt,
				pState->CarrierPayloadDocked[index]);
		}

		s_carrierTakeoffIntentAircraft.clear();
	}

	// Periodically release runway reservations held by payloads that were
	// destroyed mid-landing. This runs during normal gameplay (not during
	// pointer invalidation), where UnitClass::Array is stable.
	if (Unsorted::CurrentFrame % 60 == 0)
	{
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
				continue;

			if (!TechnoTypeExt::IsAircraftCarrier(pUnit->GetTechnoType()))
				continue;

			auto const pState = GetState(pUnit);

			if (!pState || pState->CarrierPayloadLanding.empty())
				continue;

			const size_t count = std::min(pState->CarrierPayloads.size(),
				std::min(pState->CarrierPayloadLanding.size(),
					pState->CarrierPayloadDocked.size()));

			for (size_t i = 0; i < count; ++i)
			{
				auto const pAircraft = pState->CarrierPayloads[i];

				if (pAircraft
					&& !IsLiveAircraft(pAircraft)
					&& pState->CarrierPayloadDocked[i] != 1)
				{
					ClearCarrierLandingProgress(pState, pAircraft, i, true);
				}
			}
		}
	}
}

// Clear both the current and the queued mission. Writing CurrentMission alone
// leaves QueuedMission = Enter/Move/Area_Guard, and the engine starts it again
// on the next AI tick (this is what kept re-issuing destinations every frame).
static void ForceCarrierSleep(AircraftClass* pAircraft)
{
	if (!pAircraft)
		return;

	pAircraft->CurrentMission = Mission::Sleep;
	pAircraft->QueuedMission = Mission::Sleep;
	pAircraft->MissionStatus = 0;
}

// A parked or deck-bound payload must not keep a fly-locomotor takeoff or
// landing sub-state alive. If it does, the locomotor applies a small vertical
// offset every tick and the deck pin snaps it back: the aircraft visibly
// convulses. Clear the state together with the mission.
static void ResetCarrierFlightState(AircraftClass* pAircraft)
{
	if (!pAircraft)
		return;

	if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
	{
		pFly->IsLanding = false;
		pFly->IsTakingOff = false;
		pFly->IsElevating = false;
		pFly->FlightLevel = 0;
	}
}

void Footprint::LockParkedCarrierPayload(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return;

	const unsigned char runwayState = pState->CarrierPayloadDocked[index];

	if (runwayState == 2 || runwayState == 7)
		LogLandingProgress("LockBefore", pAircraft, pState, index);

	if (runwayState == 2)
	{
		// State 2 is the manual return/orbit. Pin it before the engine's
		// movement update, but let a real player order survive so
		// UpdateCarrierPayload can cancel the landing.
		const auto mission = pAircraft->GetCurrentMission();
		// Guard is also set by the engine's idle/auto-acquire code; treating it
		// as a player order cancelled the landing immediately after touchdown.
		const bool playerOrder = pAircraft->IsSelected
			&& (mission == Mission::Move || mission == Mission::QMove
				|| mission == Mission::Attack || mission == Mission::AttackMove
				|| mission == Mission::Stop)
			&& (mission == Mission::Stop
				|| pAircraft->Destination || pAircraft->Target);

		Debug::CarrierLog("[DiagLock] state2 slot=%u playerOrder=%d mission=%d sel=%d dest=%p target=%p inAir=%d\n",
			static_cast<unsigned>(index), playerOrder ? 1 : 0, static_cast<int>(mission),
			pAircraft->IsSelected ? 1 : 0, static_cast<void*>(pAircraft->Destination),
			static_cast<void*>(pAircraft->Target), pAircraft->InAir ? 1 : 0);

		if (!playerOrder)
		{
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->Destination = nullptr;
			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);
			// Keep the fly locomotor from moving this aircraft before the
			// manual controller runs. The controller sets InAir back to true
			// after it applies this frame's movement.
			pAircraft->InAir = false;
			pAircraft->SetSpeedPercentage(0.0);
			pAircraft->PrimaryFacing.SetDesired(pAircraft->PrimaryFacing.Current());
			pAircraft->Target = nullptr;
			pAircraft->ArchiveTarget = nullptr;
			ForceCarrierSleep(pAircraft);

			if (auto const pLoco = pAircraft->Locomotor)
				pLoco->Stop_Moving();

			LogLandingProgress("LockPinned", pAircraft, pState, index);
		}

		return;
	}

	if (runwayState == 6 || runwayState == 7 || runwayState == 8)
	{
		// The runway controller owns this aircraft. Erase every engine order
		// before mission AI runs, otherwise a mid-taxi player click makes the
		// aircraft fight the controller with its own turn/move logic.
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);

		// Runway rollout/taxi must stay on the deck. In particular, state 6
		// must not be allowed to lift vertically when a player/engine order
		// arrives during taxi or alignment.
		if (runwayState == 6)
		{
			pAircraft->InAir = false;
			// The engine altitude field is also fed to the SHP draw/shadow.
			// A stale flight altitude here is why the parked aircraft visibly
			// floats above the deck with the shadow left on the deck.
			pAircraft->SetHeight(0);
		}

		if (runwayState == 7)
		{
			// The manual landing controller owns altitude. Keep the fly
			// locomotor from moving the aircraft during the engine's update;
			// UpdateCarrierLandingRunway sets InAir back to true while it is
			// still descending and false after touchdown.
			pAircraft->InAir = false;
			LogLandingProgress("LockRunway7", pAircraft, pState, index);
		}

		// Landing and departure are fully manual. Keep the engine speed in
		// sync with the manual departure speed so handing control back does
		// not snap the model from zero to full speed.
		double speedPercentage = 0.0;

		if (runwayState == 8)
		{
			for (auto const& task : pState->Runway.Departures)
			{
				if (task.Aircraft == pAircraft)
				{
					const double maxSpeed = task.MaxSpeed > 0.0
						? task.MaxSpeed
						: GetAircraftFlightSpeed(pAircraft);
					speedPercentage = std::clamp(task.Speed / std::max(1.0, maxSpeed), 0.0, 1.0);
					break;
				}
			}
		}

		pAircraft->SetSpeedPercentage(speedPercentage);

		// Freeze engine facing; the state machine rotates manually.
		pAircraft->PrimaryFacing.SetDesired(pAircraft->PrimaryFacing.Current());

		// QueueMission alone is not enough: the engine re-queues Move/Enter
		// on some aircraft every frame. Pin both current and queued mission
		// for the duration of the runway-controlled state; otherwise the
		// queued order starts again on the next tick.
		pAircraft->Target = nullptr;
		pAircraft->ArchiveTarget = nullptr;
		ForceCarrierSleep(pAircraft);
		ResetCarrierFlightState(pAircraft);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		return;
	}

	if (runwayState != 1)
		return;

	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());
	const auto mission = pAircraft->GetCurrentMission();
	const bool aiOwner = pCarrier->Owner && !pCarrier->Owner->IsControlledByHuman();

	// AI carriers have no click hook. Capture a real AI Move/Attack order here,
	// before the parked pin below erases it, and feed it into the runway queue.
	if (aiOwner && HasRunwayMode(pTypeExt, false)
		&& !(index < pState->CarrierPayloadReloadTimer.size()
			&& pState->CarrierPayloadReloadTimer[index] < 0))
	{
		const bool moveOrder = (mission == Mission::Move || mission == Mission::QMove)
			&& pAircraft->Destination
			&& pAircraft->Destination->GetCoords().DistanceFrom(pCarrier->GetCoords()) > 512.0;
		const bool attackOrder = (mission == Mission::Attack || mission == Mission::AttackMove)
			&& pAircraft->Target;

		if (moveOrder || attackOrder)
		{
			RequestRunwayTakeoff(pCarrier, pAircraft, index, pState, pTypeExt,
				pState->CarrierPayloadDocked[index]);

			if (pState->CarrierPayloadDocked[index] == 6)
				return;
		}
	}

	// Runs before the engine's mission AI. A stale engine destination or a
	// leftover Enter/Move mission would otherwise start a takeoff before the
	// normal parked-state handler gets a chance to run.
	pAircraft->DockNowHeadingTo = nullptr;
	pAircraft->Destination = nullptr;
	pAircraft->SetTarget(nullptr);
	pAircraft->SetArchiveTarget(nullptr);
	pAircraft->InAir = false;
	pAircraft->SetHeight(0);

	if (pAircraft->GetCurrentMission() != Mission::Sleep
		|| pAircraft->QueuedMission != Mission::Sleep)
	{
		ForceCarrierSleep(pAircraft);
	}

	ResetCarrierFlightState(pAircraft);

	if (pAircraft->Locomotor)
		pAircraft->Locomotor->Stop_Moving();
}

	void TurnAircraftToward(AircraftClass* pAircraft, const DirStruct& desired, int turnStep)
	{
		if (!pAircraft)
			return;

		unsigned short raw = pAircraft->PrimaryFacing.Current().Raw;
		const int diff = static_cast<short>(desired.Raw - raw);
		raw = static_cast<unsigned short>(raw + std::clamp(diff, -turnStep, turnStep));
		pAircraft->PrimaryFacing.SetCurrent(DirStruct(raw));
		pAircraft->PrimaryFacing.SetDesired(DirStruct(raw));
	}

	void SpawnCarrierRunwayTrail(AircraftClass* pAircraft, bool force = false)
	{
		if (!pAircraft || !pAircraft->Type || !pAircraft->Type->Trailer)
			return;

		// The engine trailer hook lives inside AircraftClass::Update, which is
		// skipped while the runway controller owns the aircraft. Emit the same
		// trailer anim manually at the engine's approximate interval.
		if (!force && Unsorted::CurrentFrame % 6 != 0)
			return;

		auto const pAnim = GameCreate<AnimClass>(
			pAircraft->Type->Trailer, pAircraft->GetCoords(), 1, 1);

		if (!pAnim)
			return;

		auto const pAnimExt = AnimExt::ExtMap.Find(pAnim);
		AnimExt::SetAnimOwnerHouseKind(pAnim, pAircraft->Owner, nullptr, false, true);
		pAnimExt->SetInvoker(pAircraft);
		pAnimExt->IsTechnoTrailerAnim = true;
	}

	void ParkCarrierPayloadAtSlot(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state,
		const char* reason)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt)
			return;

		LogLandingProgress("ParkBefore", pAircraft, pState, index);
		const auto slot = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);

		pAircraft->SetLocation(slot);
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->Destination = nullptr;
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = false;
		pAircraft->SetHeight(0);
		pAircraft->SetSpeedPercentage(0.0);
		ForceCarrierSleep(pAircraft);
		ResetCarrierFlightState(pAircraft);

		const DirStruct slotFacing = GetCarrierSlotFacing(pCarrier, pTypeExt, index);
		pAircraft->PrimaryFacing.SetCurrent(slotFacing);
		pAircraft->PrimaryFacing.SetDesired(slotFacing);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (index < pState->CarrierPayloadLaunchRequest.size())
			pState->CarrierPayloadLaunchRequest[index] = 0;

		if (index < pState->CarrierPayloadRequestedDest.size())
			pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

		if (index < pState->CarrierPayloadReturnCell.size())
			pState->CarrierPayloadReturnCell[index] = CellStruct { -1, -1 };

		if (index < pState->CarrierPayloadReloadTimer.size())
			pState->CarrierPayloadReloadTimer[index] = 0;

		ClearCarrierLandingProgress(pState, pAircraft, index, true);

		state = 1;
		s_landingRunwayIndex.erase(pAircraft);
		s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
		s_landingRunwayExplicit.erase(pAircraft);

		Debug::CarrierLog("[AircraftCarrier] %s parked in slot %u (%s)\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
			reason ? reason : "runway");
	}

	void CancelRunwayTakeoff(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state,
		const char* reason)
	{
		auto& runway = pState->Runway;

		if (runway.TakeoffRequest == pAircraft)
		{
			runway.TakeoffRequest = nullptr;
			runway.TakeoffRequestSlot = -1;
		}

		if (runway.Occupant == pAircraft)
		{
			runway.Occupant = nullptr;
			runway.OccupantSlot = -1;
			runway.CommandDestination = CellStruct { -1, -1 };
			runway.CommandTarget = nullptr;
			runway.CommandMission = 0;
			runway.Phase = CarrierRunwayPhase::Idle;
			runway.Timer = 0;
			runway.Speed = 0.0;
			runway.Path.clear();
			runway.PathIndex = 0;
			runway.TrailEnabled = false;
		}

		ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state, reason);
	}

	void CompleteRunwayTakeoff(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, unsigned char& state)
	{
		auto& runway = pState->Runway;
		// Clear the controlled flag before assigning the restored command, so
		// the AssignDestination guard does not reject our own SetDestination.
		state = 0;
		s_landingRunwayIndex.erase(pAircraft);
		s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
		s_landingRunwayExplicit.erase(pAircraft);
		pAircraft->SetSpeedPercentage(1.0);

		// Hand the restored command over on the next global tick. Doing it here
		// would call SetDestination/QueueMission while the aircraft's own
		// Update is still on the stack.
		CarrierTakeoffHandoff handoff;
		handoff.Aircraft = pAircraft;
		handoff.Carrier = pCarrier;
		handoff.CommandDestination = runway.CommandDestination;
		handoff.CommandTarget = runway.CommandTarget;
		handoff.CommandMission = runway.CommandMission;
		s_carrierTakeoffHandoffs.push_back(handoff);

		runway.CommandDestination = CellStruct { -1, -1 };
		runway.CommandTarget = nullptr;
		runway.CommandMission = 0;
		runway.Occupant = nullptr;
		runway.OccupantSlot = -1;
		runway.Phase = CarrierRunwayPhase::Idle;
		runway.Timer = 0;
		runway.Speed = 0.0;
		runway.Path.clear();
		runway.PathIndex = 0;
		runway.TrailEnabled = false;

		if (index < pState->CarrierPayloadLaunchRequest.size())
			pState->CarrierPayloadLaunchRequest[index] = 0;

		if (index < pState->CarrierPayloadRequestedDest.size())
			pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

		Debug::CarrierLog("[AircraftCarrier] %s runway takeoff complete slot %u\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));
	}

	void StartRunwayTakeoffActive(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		auto& runway = pState->Runway;
		const int runwayIndex = runway.RunwayIndex;

		if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, false, runwayIndex,
			runway.StartCoords, runway.EndCoords))
		{
			CancelRunwayTakeoff(pCarrier, pAircraft, index, pState, pTypeExt, state,
				"runway offsets invalid");
			return;
		}

		runway.Occupant = pAircraft;
		runway.OccupantSlot = static_cast<int>(index);
		runway.Timer = 0;
		// Capture the real flight speed before the engine is frozen.
		runway.MaxSpeed = GetAircraftFlightSpeed(pAircraft);
		runway.Acceleration = GetTakeoffAcceleration(pTypeExt);
		runway.Speed = runway.MaxSpeed
			* static_cast<double>(GetTakeoffInitialSpeed(pTypeExt)) / 100.0;
		runway.PathIndex = 0;
		runway.TrailEnabled = false;
		state = 6;

		// The engine may already have lifted the aircraft (a Move order was
		// processed before this deferred takeoff intent). Snap it back to its
		// deck slot before the taxi path starts; otherwise the controller
		// turns in place at altitude and never reaches the runway.
		const auto slotCoords = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
		pAircraft->SetLocation(slotCoords);
		pAircraft->InAir = false;
		pAircraft->SetHeight(0);

		ResetCarrierFlightState(pAircraft);

		// Keep the command captured from the parked order (or queued pending
		// command). Further commands issued during taxi/roll/departure update
		// the same memory and are executed after departure completes.

		// Direct taxi to A. The old staging point required matching A's Y
		// coordinate and caused an unnecessary turn/stop phase.
		runway.Phase = CarrierRunwayPhase::TaxiToA;
		runway.LastCoords = runway.StartCoords;
		runway.Path = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, runway.StartCoords);

		runway.Timer = runway.Path.empty() ? 0 : 1;

		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = false;
		pAircraft->Destination = nullptr;
		pAircraft->Target = nullptr;
		pAircraft->ArchiveTarget = nullptr;
		ForceCarrierSleep(pAircraft);
		pAircraft->SetSpeedPercentage(0.0);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (index < pState->CarrierPayloadLaunchRequest.size())
			pState->CarrierPayloadLaunchRequest[index] = 0;

		if (index < pState->CarrierPayloadRequestedDest.size())
			pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

		Debug::CarrierLog("[AircraftCarrier] %s runway takeoff starts slot %u runway=%d path=%u\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
			runwayIndex, static_cast<unsigned>(runway.Path.size()));
	}

	void RequestRunwayTakeoff(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		if (!pCarrier || !pAircraft || !pState || !pTypeExt || !HasRunwayMode(pTypeExt, false))
			return;

		EnsureRunwayStates(pState, pTypeExt);
		const int selected = ChooseTakeoffRunwayForRequest(pCarrier, pAircraft, pState, pTypeExt);

		if (selected < 0 || selected >= static_cast<int>(pState->TakeoffRunways.size()))
			return;

		RunwayStateGuard guard(pState->TakeoffRunways[selected], pState->Runway);
		auto& runway = pState->Runway;
		runway.RunwayIndex = selected;

		if (runway.Occupant == pAircraft || runway.TakeoffRequest == pAircraft)
			return;

		CellStruct commandDest { -1, -1 };

		if (pAircraft->Destination)
			commandDest = CellClass::Coord2Cell(pAircraft->Destination->GetCoords());

		auto commandTarget = abstract_cast<TechnoClass*>(pAircraft->Target);
		int commandMission = static_cast<int>(pAircraft->GetCurrentMission());

		if (index < pState->CarrierPayloadRequestedDest.size()
			&& pState->CarrierPayloadRequestedDest[index].X >= 0)
		{
			commandDest = pState->CarrierPayloadRequestedDest[index];
			commandMission = static_cast<int>(Mission::Move);
		}

		if (commandTarget)
		{
			const auto missionNow = static_cast<Mission>(commandMission);

			if (missionNow != Mission::Attack && missionNow != Mission::AttackMove)
				commandMission = static_cast<int>(Mission::Attack);
		}
		else if (commandDest.X >= 0)
		{
			const auto missionNow = static_cast<Mission>(commandMission);

			if (missionNow != Mission::Move && missionNow != Mission::QMove
				&& missionNow != Mission::AttackMove)
			{
				commandMission = static_cast<int>(Mission::Move);
			}
		}

		if (runway.Occupant || runway.TakeoffRequest)
		{
			// Strict single runway: remember only the first waiting request.
			// Any later click must not leave a latent launch request behind.
			if (!runway.TakeoffRequest)
			{
				runway.TakeoffRequest = pAircraft;
				runway.TakeoffRequestSlot = static_cast<int>(index);
				runway.PendingDestination = commandDest;
				runway.PendingTarget = commandTarget;
				runway.PendingMission = commandMission;
				state = 6;
				pAircraft->DockNowHeadingTo = nullptr;
				pAircraft->InAir = false;
				pAircraft->Destination = nullptr;
				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->QueueMission(Mission::Sleep, true);

				if (auto const pLoco = pAircraft->Locomotor)
					pLoco->Stop_Moving();

				if (index < pState->CarrierPayloadRequestedDest.size())
					pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

				Debug::CarrierLog("[AircraftCarrier] %s waiting for runway\n",
					pAircraft->Type ? pAircraft->Type->ID : "?");
			}
			else
			{
				if (index < pState->CarrierPayloadLaunchRequest.size())
					pState->CarrierPayloadLaunchRequest[index] = 0;

				if (index < pState->CarrierPayloadRequestedDest.size())
					pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };
			}

			return;
		}

		if (!IsCarrierStopped(pCarrier))
		{
			runway.TakeoffRequest = pAircraft;
			runway.TakeoffRequestSlot = static_cast<int>(index);
			runway.PendingDestination = commandDest;
			runway.PendingTarget = commandTarget;
			runway.PendingMission = commandMission;
			state = 6;
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->InAir = false;
			pAircraft->Destination = nullptr;
			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);
			pAircraft->QueueMission(Mission::Sleep, true);

			if (auto const pLoco = pAircraft->Locomotor)
				pLoco->Stop_Moving();

			if (index < pState->CarrierPayloadRequestedDest.size())
				pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

			Debug::CarrierLog("[AircraftCarrier] %s waiting for carrier stop\n",
				pAircraft->Type ? pAircraft->Type->ID : "?");
			return;
		}

		runway.CommandDestination = commandDest;
		runway.CommandTarget = commandTarget;
		runway.CommandMission = commandMission;
		StartRunwayTakeoffActive(pCarrier, pAircraft, index, pState, pTypeExt, state);
	}

	CarrierRunwayTaxiTask* FindRunwayTaxiTask(CarrierRunwayState& runway, AircraftClass* pAircraft)
	{
		for (auto& task : runway.TaxiTasks)
		{
			if (task.Aircraft == pAircraft)
				return &task;
		}

		return nullptr;
	}

	void BeginRunwayLandingTaxi(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		auto& runway = pState->LandingRunway;
		const auto slot = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);

		CarrierRunwayTaxiTask task;
		task.Aircraft = pAircraft;
		task.Slot = static_cast<int>(index);
		task.Target = slot;
		task.Path = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, slot);
		task.PathIndex = 0;
		task.TurnTimer = task.Path.empty() ? 0 : 1;
		task.Speed = GetRunwayTaxiSpeed(pTypeExt);
		task.TrailEnabled = false;
		runway.TaxiTasks.push_back(std::move(task));

		pAircraft->InAir = false;
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->SetSpeedPercentage(0.0);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		state = 7;

		runway.Occupant = nullptr;
		runway.OccupantSlot = -1;
		runway.Phase = CarrierRunwayPhase::Idle;
		runway.Timer = 0;
		runway.Speed = 0.0;
		runway.TrailEnabled = false;
		runway.Path.clear();
		runway.PathIndex = 0;

		Debug::CarrierLog("[AircraftCarrier] %s runway landing complete, taxiing to slot %u path=%u\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
			static_cast<unsigned>(runway.TaxiTasks.back().Path.size()));
	}

	void UpdateCarrierRunwayTaxiTask(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state,
		CarrierRunwayTaxiTask& task)
	{
		if (!IsCarrierStopped(pCarrier))
		{
			// The carrier moved after touchdown; discard the stale world-space
			// taxi path and rebuild it once the ship is stationary again.
			task.Path.clear();
			return;
		}

		if (pAircraft->GetCurrentMission() != Mission::Sleep)
			pAircraft->QueueMission(Mission::Sleep, true);

		{
			auto& watch = s_taxiWatch[pAircraft];

			if (watch.Location == pAircraft->Location
				&& watch.Facing == pAircraft->PrimaryFacing.Current().Raw)
			{
				++watch.StuckFrames;
			}
			else
			{
				watch.Location = pAircraft->Location;
				watch.Facing = pAircraft->PrimaryFacing.Current().Raw;
				watch.StuckFrames = 0;
			}

			if (watch.StuckFrames >= 180)
			{
				Debug::CarrierLog("[CarrierTaxi] %s stuck for 180 frames; snapping to slot %u\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));
				pAircraft->SetLocation(task.Target);
				ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state,
					"taxi stuck fallback");

				auto& tasks = pState->LandingRunway.TaxiTasks;
				tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
					[pAircraft](const CarrierRunwayTaxiTask& item)
					{ return item.Aircraft == pAircraft; }),
					tasks.end());

				return;
			}
		}

		if (Unsorted::CurrentFrame % 15 == 0)
		{
			Debug::CarrierLog("[CarrierTaxi] %s slot=%u path=%u/%u loc=(%d,%d,%d) target=(%d,%d,%d) inAir=%d speed=%.2f turn=%d\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
				static_cast<unsigned>(task.PathIndex), static_cast<unsigned>(task.Path.size()),
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				task.Target.X, task.Target.Y, task.Target.Z,
				pAircraft->InAir ? 1 : 0, task.Speed, task.TurnTimer);
		}

		if (task.Path.empty())
		{
			task.Path = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, task.Target);
			task.PathIndex = 0;
			task.TurnTimer = 1;

			if (task.Path.empty())
			{
				if (Unsorted::CurrentFrame % 60 == 0)
				{
					Debug::CarrierLog("[AircraftCarrier] %s taxi path blocked, waiting\n",
						pAircraft->Type ? pAircraft->Type->ID : "?");
				}

				return;
			}
		}

		if (task.PathIndex >= task.Path.size())
		{
			ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state, "landing taxi done");

			auto& tasks = pState->LandingRunway.TaxiTasks;
			tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
				[pAircraft](const CarrierRunwayTaxiTask& item) { return item.Aircraft == pAircraft; }),
				tasks.end());
			return;
		}

		auto current = pAircraft->Location;
		auto const target = task.Path[task.PathIndex];
		const double distance = HorizontalDistance(current, target);
		const double speed = std::max(1.0, task.Speed);

		if (task.TurnTimer > 0)
		{
			const auto desired = GetRunwayDirection(current, target);
			TurnAircraftToward(pAircraft, desired, GetRunwayTurnRate(pTypeExt));
			pAircraft->SetLocation(current);
			pAircraft->InAir = false;

			const int diff = std::abs(static_cast<short>(
				desired.Raw - pAircraft->PrimaryFacing.Current().Raw));

			if (diff <= 256)
			{
				pAircraft->PrimaryFacing.SetCurrent(desired);
				pAircraft->PrimaryFacing.SetDesired(desired);
				task.TurnTimer = 0;
			}

			return;
		}

		if (distance <= std::max(1.0, speed))
		{
			pAircraft->SetLocation(target);
			++task.PathIndex;

			if (task.PathIndex < task.Path.size())
				task.TurnTimer = 1;

			return;
		}

		MoveCarrierTaxiStep(pAircraft, current, target, speed);
	}

	void UpdateCarrierRunwayPayload(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state,
		CarrierRunwayState& runway)
	{
		const auto taxiSpeed = GetRunwayTaxiSpeed(pTypeExt);

		if (!HasRunwayMode(pTypeExt))
		{
			ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state,
				"runway disabled");
			return;
		}

		if (runway.Occupant == pAircraft && runway.TrailEnabled)
			SpawnCarrierRunwayTrail(pAircraft);

		// A takeoff request may wait in its slot while another aircraft uses
		// the runway. It remains state 6 so the normal parked handler cannot
		// erase its command.
		if (runway.TakeoffRequest == pAircraft)
		{
			const auto slot = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
			pAircraft->SetLocation(slot);
			pAircraft->InAir = false;
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->PrimaryFacing.SetDesired(GetCarrierSlotFacing(pCarrier, pTypeExt, index));

			if (pAircraft->GetCurrentMission() != Mission::Sleep)
				pAircraft->QueueMission(Mission::Sleep, true);

			if (auto const pLoco = pAircraft->Locomotor)
				pLoco->Stop_Moving();

			if (!runway.Occupant && IsCarrierStopped(pCarrier))
			{
				runway.TakeoffRequest = nullptr;
				runway.TakeoffRequestSlot = -1;
				runway.CommandDestination = runway.PendingDestination;
				runway.CommandTarget = runway.PendingTarget;
				runway.CommandMission = runway.PendingMission;
				runway.PendingDestination = CellStruct { -1, -1 };
				runway.PendingTarget = nullptr;
				runway.PendingMission = 0;
				StartRunwayTakeoffActive(pCarrier, pAircraft, index, pState, pTypeExt, state);
			}

			return;
		}

		if (state == 6 && runway.Occupant == pAircraft)
		{
			if (pAircraft->GetCurrentMission() != Mission::Sleep)
				pAircraft->QueueMission(Mission::Sleep, true);

			if (Unsorted::CurrentFrame % 15 == 0)
			{
				Debug::CarrierLog("[RunwayTakeoff] %s phase=%d path=%u/%u loc=(%d,%d,%d) start=(%d,%d,%d) end=(%d,%d,%d) last=(%d,%d,%d) speed=%.2f timer=%d dest=%p mission=%d target=%p reload=%d\n",
					pAircraft->Type ? pAircraft->Type->ID : "?",
					static_cast<int>(runway.Phase),
					static_cast<unsigned>(runway.PathIndex),
					static_cast<unsigned>(runway.Path.size()),
					pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
					runway.StartCoords.X, runway.StartCoords.Y, runway.StartCoords.Z,
					runway.EndCoords.X, runway.EndCoords.Y, runway.EndCoords.Z,
					runway.LastCoords.X, runway.LastCoords.Y, runway.LastCoords.Z,
					runway.Speed, runway.Timer, static_cast<void*>(pAircraft->Destination),
					static_cast<int>(pAircraft->GetCurrentMission()),
					static_cast<void*>(pAircraft->Target),
					(index < pState->CarrierPayloadReloadTimer.size()
						? pState->CarrierPayloadReloadTimer[index] : 0));
			}

			// The carrier must not move once the aircraft is on the runway.
			// Takeoff roll may finish using the cached line if the ship moves
			// after the roll has started.
			if (!IsCarrierStopped(pCarrier)
				&& runway.Phase != CarrierRunwayPhase::TakeoffRoll
				&& runway.Phase != CarrierRunwayPhase::TakeoffDeparture)
			{
				CancelRunwayTakeoff(pCarrier, pAircraft, index, pState, pTypeExt, state,
					"carrier moved before takeoff");
				return;
			}

			switch (runway.Phase)
			{
			case CarrierRunwayPhase::TaxiToStaging:
			case CarrierRunwayPhase::TaxiToA:
			{
				const bool staging = runway.Phase == CarrierRunwayPhase::TaxiToStaging;
				const auto pathTarget = staging ? runway.LastCoords : runway.StartCoords;

				if (runway.Path.empty())
				{
					runway.Path = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, pathTarget);
					runway.PathIndex = 0;

					if (runway.Path.empty())
					{
						if (Unsorted::CurrentFrame % 60 == 0)
						{
							Debug::CarrierLog("[AircraftCarrier] %s takeoff taxi path blocked, waiting\n",
								pAircraft->Type ? pAircraft->Type->ID : "?");
						}

						return;
					}
				}

				if (runway.PathIndex >= runway.Path.size())
				{
					pAircraft->SetLocation(pathTarget);

					if (staging)
					{
						runway.Phase = CarrierRunwayPhase::TaxiToA;
						runway.LastCoords = runway.StartCoords;
						runway.Path = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, runway.StartCoords);
						runway.PathIndex = 0;
						runway.Timer = 1;

						Debug::CarrierLog("[AircraftCarrier] %s aligned with runway Y, heading to A\n",
							pAircraft->Type ? pAircraft->Type->ID : "?");
						return;
					}

					runway.Phase = CarrierRunwayPhase::AlignAtA;
					runway.Timer = 0;
					runway.TrailEnabled = false;

					Debug::CarrierLog("[AircraftCarrier] %s reached runway A, aligning\n",
						pAircraft->Type ? pAircraft->Type->ID : "?");
					return;
				}

				auto current = pAircraft->Location;
				auto const target = runway.Path[runway.PathIndex];
				const double distance = HorizontalDistance(current, target);

				// Vehicle-like turn at every waypoint: stop, rotate in place,
				// then start the next segment. Keep the aircraft on the deck
				// during the turn; if the engine lifted it before the deferred
				// takeoff started, the old code turned forever at altitude.
				if (runway.Timer > 0)
				{
					current.Z = target.Z;
					const auto desired = GetRunwayDirection(current, target);
					TurnAircraftToward(pAircraft, desired, GetRunwayTurnRate(pTypeExt));
					pAircraft->SetLocation(current);
					pAircraft->InAir = false;

					const int diff = std::abs(static_cast<short>(
						desired.Raw - pAircraft->PrimaryFacing.Current().Raw));
					++runway.Timer;

					// The engine's facing can be restored every tick by its
					// own locomotor. Break the deadlock after a short grace
					// period and snap to the path direction.
					if (diff <= 256 || runway.Timer > 180)
					{
						pAircraft->PrimaryFacing.SetCurrent(desired);
						pAircraft->PrimaryFacing.SetDesired(desired);
						runway.Timer = 0;
					}

					return;
				}

				if (Unsorted::CurrentFrame % 30 == 0)
				{
					Debug::CarrierLog("[AircraftCarrier] %s taxi f=%d idx=%u/%u loc=(%d,%d,%d) target=(%d,%d,%d) dist=%.1f speed=%.1f\n",
						pAircraft->Type ? pAircraft->Type->ID : "?", Unsorted::CurrentFrame,
						static_cast<unsigned>(runway.PathIndex), static_cast<unsigned>(runway.Path.size()),
						current.X, current.Y, current.Z, target.X, target.Y, target.Z,
						distance, taxiSpeed);
				}

				if (distance <= std::max(1.0, taxiSpeed))
				{
					pAircraft->SetLocation(target);
					++runway.PathIndex;

					if (runway.PathIndex < runway.Path.size())
						runway.Timer = 1;

					return;
				}

				MoveCarrierTaxiStep(pAircraft, current, target, taxiSpeed);
				return;
			}

			case CarrierRunwayPhase::AlignAtA:
			{
				// Snap to the live runway point. The carrier is required to be
				// stopped, but recomputing here removes any one- or two-frame
				// facing/transform drift accumulated during taxi and also
				// makes the final position independent of the cached path end.
				CoordStruct liveStart {};
				CoordStruct liveEnd {};

				if (GetCarrierRunwayPoints(pCarrier, pTypeExt, false, runway.RunwayIndex,
					liveStart, liveEnd)
					&& (liveStart != runway.StartCoords || liveEnd != runway.EndCoords))
				{
					runway.StartCoords = liveStart;
					runway.EndCoords = liveEnd;
				}

				pAircraft->SetLocation(runway.StartCoords);
				pAircraft->InAir = false;

				if (auto const pLoco = pAircraft->Locomotor)
					pLoco->Stop_Moving();

				const auto desired = GetRunwayDirection(runway.StartCoords, runway.EndCoords);
				const int rawDiff = std::abs(static_cast<short>(
					desired.Raw - pAircraft->PrimaryFacing.Current().Raw));

				if (rawDiff <= 512)
				{
					pAircraft->PrimaryFacing.SetCurrent(desired);
					pAircraft->PrimaryFacing.SetDesired(desired);
					runway.TrailEnabled = false;
					++runway.Timer;

					if (runway.Timer >= GetRunwayTakeoffDelay(pTypeExt))
					{
						runway.Phase = CarrierRunwayPhase::TakeoffRoll;
						runway.Timer = 0;

						if (runway.MaxSpeed <= 0.0)
						{
							runway.MaxSpeed = GetAircraftFlightSpeed(pAircraft);
							runway.Speed = runway.MaxSpeed;
						}

						// First roll frame must emit the trailer immediately;
						// the normal top-of-update emitter runs before this
						// phase transition.
						runway.TrailEnabled = true;
						SpawnCarrierRunwayTrail(pAircraft, true);

						Debug::CarrierLog("[AircraftCarrier] %s takeoff roll begins slot %u speed=%.1f trailer=%p\n",
							pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
							runway.Speed,
							pAircraft->Type ? static_cast<void*>(pAircraft->Type->Trailer) : nullptr);
					}
				}
				else
				{
					TurnAircraftToward(pAircraft, desired, GetRunwayTurnRate(pTypeExt));
					runway.TrailEnabled = false;
				}

				return;
			}

			case CarrierRunwayPhase::TakeoffRoll:
			{
				auto current = pAircraft->Location;
				const auto end = runway.EndCoords;
				const double remaining = HorizontalDistance(current, end);

				if (runway.MaxSpeed > 0.0)
				{
					runway.Speed = std::min(runway.MaxSpeed,
						std::max(0.0, runway.Speed) + runway.Acceleration);
				}

				const double speed = std::max(1.0, runway.Speed);
				const double step = std::min(speed, remaining);
				const auto direction = GetRunwayDirection(runway.StartCoords, end);

				pAircraft->PrimaryFacing.SetCurrent(direction);
				pAircraft->PrimaryFacing.SetDesired(direction);

				if (remaining > 0.5)
				{
					const double dx = static_cast<double>(end.X - runway.StartCoords.X);
					const double dy = static_cast<double>(end.Y - runway.StartCoords.Y);
					const double length = std::sqrt(dx * dx + dy * dy);

					if (length > 0.001)
					{
						current.X += static_cast<int>(std::lround((dx / length) * step));
						current.Y += static_cast<int>(std::lround((dy / length) * step));
					}

					current.Z = runway.StartCoords.Z;
					pAircraft->SetLocation(current);
				}

				runway.TrailEnabled = true;

				if (remaining <= 0.5 || step >= remaining)
				{
					pAircraft->SetLocation(end);
					pAircraft->InAir = true;
					pAircraft->DockNowHeadingTo = nullptr;
					pAircraft->Destination = nullptr;
					pAircraft->Target = nullptr;
					pAircraft->ArchiveTarget = nullptr;
					ForceCarrierSleep(pAircraft);
					pAircraft->SetSpeedPercentage(0.0);

					if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
					{
						pFly->IsLanding = false;
						pFly->IsTakingOff = false;
						pFly->IsElevating = false;
					}

					CarrierRunwayDeparture departure;
					departure.Aircraft = pAircraft;
					departure.Start = runway.StartCoords;
					departure.End = end;
					departure.Speed = std::max(0.0, runway.Speed);
					departure.MaxSpeed = runway.MaxSpeed > 0.0
						? runway.MaxSpeed
						: GetAircraftFlightSpeed(pAircraft);
					departure.Acceleration = runway.Acceleration;
					// Trail is takeoff-roll only; departure climb is clean.
					departure.TrailEnabled = false;
					departure.CommandDestination = runway.CommandDestination;
					departure.CommandTarget = runway.CommandTarget;
					departure.CommandMission = runway.CommandMission;
					runway.Departures.push_back(std::move(departure));

					// Z point is now free for the next aircraft. The departing
					// aircraft remains runway-controlled in state 8 until the
					// straight manual climb reaches flight level, preserving the
					// original smooth pull-up instead of handing over to the
					// normal aircraft autopilot at deck altitude.
					runway.Occupant = nullptr;
					runway.OccupantSlot = -1;
					runway.Phase = CarrierRunwayPhase::Idle;
					runway.Timer = 0;
					runway.Speed = 0.0;
					runway.MaxSpeed = 0.0;
					runway.Acceleration = 0.0;
					runway.Path.clear();
					runway.PathIndex = 0;
					runway.TrailEnabled = false;
					runway.CommandDestination = CellStruct { -1, -1 };
					runway.CommandTarget = nullptr;
					runway.CommandMission = 0;
					state = 8;

					Debug::CarrierLog("[AircraftCarrier] %s runway released at Z, smooth departure climb continues slot %u\n",
						pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));
				}

				return;
			}

			case CarrierRunwayPhase::TakeoffDeparture:
			{
				auto current = pAircraft->Location;
				const auto start = runway.StartCoords;
				const auto end = runway.EndCoords;
				const double dx = static_cast<double>(end.X - start.X);
				const double dy = static_cast<double>(end.Y - start.Y);
				const double length = std::sqrt(dx * dx + dy * dy);
				const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
				const int targetZ = MapClass::Instance.GetCellFloorHeight(end) + flightLevel;
				const int climbStep = std::clamp(
					pAircraft->Type ? pAircraft->Type->Speed / 2 : 6, 3, 12);
				const double speed = std::max(1.0, runway.Speed);
				const auto direction = GetRunwayDirection(start, end);

				pAircraft->PrimaryFacing.SetCurrent(direction);
				pAircraft->PrimaryFacing.SetDesired(direction);

				if (length > 0.001)
				{
					current.X += static_cast<int>(std::lround(dx / length * speed));
					current.Y += static_cast<int>(std::lround(dy / length * speed));
				}

				if (current.Z < targetZ)
					current.Z = std::min(targetZ, current.Z + climbStep);
				else
					current.Z = targetZ;

				pAircraft->SetLocation(current);
				pAircraft->InAir = true;
				pAircraft->DockNowHeadingTo = nullptr;

				if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
				{
					pFly->IsLanding = false;
					pFly->IsTakingOff = false;
					pFly->IsElevating = false;
					pFly->FlightLevel = current.Z - MapClass::Instance.GetCellFloorHeight(current);
				}

				runway.TrailEnabled = true;

				if (current.Z >= targetZ)
					CompleteRunwayTakeoff(pCarrier, pAircraft, index, pState, state);

				return;
			}

			default:
				runway.Phase = CarrierRunwayPhase::TaxiToA;
				return;
			}
		}

		if (state == 6)
		{
			// The occupant/request pointer was invalidated by a load or a
			// destroyed aircraft. Do not leave this payload frozen.
			ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state,
				"runway takeoff state missing");
			return;
		}

		if (state == 7)
		{
			if (runway.Occupant == pAircraft)
			{
				if (pAircraft->GetCurrentMission() != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				pAircraft->Destination = nullptr;
				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->SetSpeedPercentage(0.0);

				if (auto const pLoco = pAircraft->Locomotor)
					pLoco->Stop_Moving();

				// A restored or malformed runway state must never send the
				// aircraft toward an empty (0,0) endpoint forever.
				if (runway.StartCoords == runway.EndCoords
					|| HorizontalDistance(runway.StartCoords, runway.EndCoords) < 16.0)
				{
					runway.Occupant = nullptr;
					runway.OccupantSlot = -1;
					runway.Phase = CarrierRunwayPhase::Idle;
					runway.TrailEnabled = false;
					state = 2;
					pAircraft->InAir = true;
					return;
				}

				{
					auto const loc = pAircraft->Location;
					const unsigned short facing = pAircraft->PrimaryFacing.Current().Raw;
					auto& watch = s_landingWatch[pAircraft];

					if (watch.Location == loc && watch.Facing == facing)
						++watch.StuckFrames;
					else
					{
						watch.Location = loc;
						watch.Facing = facing;
						watch.StuckFrames = 0;
					}

					if (Unsorted::CurrentFrame % 15 == 0)
					{
						const auto direction = GetRunwayDirection(loc, runway.StartCoords);
						const int facingDiff = std::abs(static_cast<short>(
							direction.Raw - pAircraft->PrimaryFacing.Current().Raw));
						const double remaining = HorizontalDistance(loc, runway.StartCoords);

						Debug::CarrierLog("[RunwayLanding] %s phase=%d loc=(%d,%d,%d) start=(%d,%d,%d) end=(%d,%d,%d) last=(%d,%d,%d) speed=%.2f rem=%.1f facing=%u diff=%d stuck=%d dest=%p\n",
							pAircraft->Type ? pAircraft->Type->ID : "?",
							static_cast<int>(runway.Phase),
							loc.X, loc.Y, loc.Z,
							runway.StartCoords.X, runway.StartCoords.Y, runway.StartCoords.Z,
							runway.EndCoords.X, runway.EndCoords.Y, runway.EndCoords.Z,
							runway.LastCoords.X, runway.LastCoords.Y, runway.LastCoords.Z,
							runway.Speed, remaining, static_cast<unsigned>(facing), facingDiff,
							watch.StuckFrames, static_cast<void*>(pAircraft->Destination));
					}

					if ((runway.Phase == CarrierRunwayPhase::LandingApproach
							|| runway.Phase == CarrierRunwayPhase::LandingRoll)
						&& watch.StuckFrames >= 180)
					{
						runway.Occupant = nullptr;
						runway.OccupantSlot = -1;
						runway.Phase = CarrierRunwayPhase::Idle;
						runway.Timer = 0;
						runway.Speed = 0.0;
						runway.TrailEnabled = false;
						state = 2;
						pAircraft->InAir = true;
						pAircraft->Destination = nullptr;
						pAircraft->QueueMission(Mission::Sleep, true);
						s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
						Debug::CarrierLog("[AircraftCarrier] %s landing stuck for 180 frames; going around\n",
							pAircraft->Type ? pAircraft->Type->ID : "?");
						return;
					}
				}

				if (runway.Phase == CarrierRunwayPhase::LandingApproach)
				{
					if (!IsCarrierStopped(pCarrier))
					{
						runway.Occupant = nullptr;
						runway.OccupantSlot = -1;
						runway.Phase = CarrierRunwayPhase::Idle;
						runway.TrailEnabled = false;
						state = 2;
						pAircraft->InAir = true;
						Debug::CarrierLog("[AircraftCarrier] %s runway approach aborted, carrier moved\n",
							pAircraft->Type ? pAircraft->Type->ID : "?");
						return;
					}

					auto current = pAircraft->Location;
					const auto start = runway.StartCoords;
					const auto end = runway.EndCoords;
					const double startDistance = std::max(1.0, HorizontalDistance(runway.LastCoords, start));
					const double remaining = HorizontalDistance(current, start);
					const double speed = std::max(1.0,
						runway.Speed > 0.0
							? runway.Speed
							: GetAircraftFlightSpeed(pAircraft));
					const auto direction = GetRunwayDirection(current, start);
					const int facingDiff = std::abs(static_cast<short>(
						direction.Raw - pAircraft->PrimaryFacing.Current().Raw));

					// Turn toward A before starting the descent. While turning,
					// hold position and altitude; do not snap facing or slide
					// sideways toward A.
					if (facingDiff > 512)
					{
						TurnAircraftToward(pAircraft, direction, GetRunwayTurnRate(pTypeExt));
						// Hold the lineup point exactly; do not let residual
						// FootClass movement make the aircraft slide while it
						// rotates toward A.
						pAircraft->SetLocation(runway.LastCoords);
						pAircraft->InAir = true;
						runway.TrailEnabled = true;
						return;
					}

					if (remaining <= std::max(16.0, speed) || current.Z <= start.Z + 8)
					{
						current = start;
						pAircraft->SetLocation(current);
						pAircraft->InAir = false;
						pAircraft->DockNowHeadingTo = nullptr;
						runway.Phase = CarrierRunwayPhase::LandingRoll;
						runway.Timer = 0;
						runway.Speed = GetAircraftFlightSpeed(pAircraft);
						runway.TrailEnabled = true;
						return;
					}

					// Aligned with A: keep the heading locked while descending
					// along the runway line. Re-setting both current and desired
					// every frame prevents the engine facing controller from
					// flipping the aircraft back across the alignment threshold.
					pAircraft->PrimaryFacing.SetCurrent(direction);
					pAircraft->PrimaryFacing.SetDesired(direction);

					// Move as a stable vector instead of independently rounding
					// X and Y. The old per-axis rounding could produce a zero
					// step while the aircraft was otherwise aligned.
					MoveCarrierAutopilot(pAircraft, current, start, speed,
						GetRunwayTurnRate(pTypeExt), true);
					const double ratio = std::clamp(remaining / startDistance, 0.0, 1.0);
					current.Z = start.Z + static_cast<int>(std::lround(
						(static_cast<double>(runway.LastCoords.Z) - start.Z) * ratio));
					pAircraft->SetLocation(current);
					pAircraft->InAir = true;
					runway.TrailEnabled = true;
					return;
				}

				if (runway.Phase == CarrierRunwayPhase::LandingRoll)
				{
					auto current = pAircraft->Location;
					const auto start = runway.StartCoords;
					const auto end = runway.EndCoords;
					const double length = std::max(1.0, HorizontalDistance(start, end));
					const double dirX = (end.X - start.X) / length;
					const double dirY = (end.Y - start.Y) / length;
					double along = (current.X - start.X) * dirX
						+ (current.Y - start.Y) * dirY;

					if (along < 0.0)
					{
						along = 0.0;
						current = start;
						pAircraft->SetLocation(current);
					}

					const double maxOverrun = static_cast<double>(GetLandingMaxOverrun(pTypeExt));
					const double stopAlong = length + maxOverrun;
					const auto direction = GetRunwayDirection(start, end);
					double speed = runway.Speed > 0.0
						? runway.Speed
						: GetAircraftFlightSpeed(pAircraft);

					// True constant deceleration instead of a percentage curve
					// that forces the aircraft to reach Z exactly.
					speed = std::max(0.0, speed - GetLandingDeceleration(pTypeExt));
					runway.Speed = speed;

					pAircraft->PrimaryFacing.SetCurrent(direction);
					pAircraft->PrimaryFacing.SetDesired(direction);

					if (speed <= 0.75 || along >= stopAlong)
					{
						if (along > stopAlong)
						{
							current.X = start.X + static_cast<int>(std::lround(dirX * stopAlong));
							current.Y = start.Y + static_cast<int>(std::lround(dirY * stopAlong));
							current.Z = start.Z;
							pAircraft->SetLocation(current);
						}

						runway.TrailEnabled = false;
						pAircraft->InAir = false;
						BeginRunwayLandingTaxi(pCarrier, pAircraft, index, pState, pTypeExt, state);
						return;
					}

					const double remainingToBoundary = stopAlong - along;
					const double step = std::min(speed, remainingToBoundary);

					current.X += static_cast<int>(std::lround(dirX * step));
					current.Y += static_cast<int>(std::lround(dirY * step));
					current.Z = start.Z;
					pAircraft->SetLocation(current);
					pAircraft->InAir = false;
					runway.TrailEnabled = true;

					// Reaching the configured maximum overrun is a hard stop,
					// even if the aircraft still has residual speed.
					if (remainingToBoundary <= step + 0.01)
					{
						runway.Speed = 0.0;
						runway.TrailEnabled = false;
						BeginRunwayLandingTaxi(pCarrier, pAircraft, index, pState, pTypeExt, state);
					}

					return;
				}

				// An active occupant should only be in approach/roll. If a save
				// restored an intermediate phase, normalize safely.
				BeginRunwayLandingTaxi(pCarrier, pAircraft, index, pState, pTypeExt, state);
				return;
			}

			if (auto const pTask = FindRunwayTaxiTask(runway, pAircraft))
			{
				UpdateCarrierRunwayTaxiTask(pCarrier, pAircraft, index, pState, pTypeExt, state, *pTask);
				return;
			}

			// No active slot and no taxi task: restore the aircraft to its
			// parking slot instead of leaving it frozen on the deck.
			ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState, pTypeExt, state,
				"runway state missing");
		}
	}

	void UpdateRunwayDeparture(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, unsigned char& state)
	{
		auto& runway = pState->Runway;
		auto const it = std::find_if(runway.Departures.begin(), runway.Departures.end(),
			[pAircraft](const CarrierRunwayDeparture& task) { return task.Aircraft == pAircraft; });

		if (it == runway.Departures.end())
		{
			// Missing task: return to a safe normal aircraft state.
			state = 0;
			pAircraft->InAir = true;
			pAircraft->SetSpeedPercentage(1.0);
			pAircraft->QueueMission(Mission::Guard, false);
			return;
		}

		auto& task = *it;
		if (task.TrailEnabled)
			SpawnCarrierRunwayTrail(pAircraft);

		auto current = pAircraft->Location;
		const auto start = task.Start;
		const auto end = task.End;
		const double dx = static_cast<double>(end.X - start.X);
		const double dy = static_cast<double>(end.Y - start.Y);
		const double length = std::sqrt(dx * dx + dy * dy);
		const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		const int targetZ = MapClass::Instance.GetCellFloorHeight(end) + flightLevel;
		const int climbStep = std::clamp(
			pAircraft->Type ? pAircraft->Type->Speed / 2 : 6, 3, 12);

		if (task.MaxSpeed > 0.0)
		{
			task.Speed = std::min(task.MaxSpeed,
				std::max(0.0, task.Speed) + task.Acceleration);
		}

		const double speed = std::max(1.0, task.Speed);
		const auto direction = GetRunwayDirection(start, end);

		pAircraft->PrimaryFacing.SetCurrent(direction);
		pAircraft->PrimaryFacing.SetDesired(direction);

		if (length > 0.001)
		{
			current.X += static_cast<int>(std::lround(dx / length * speed));
			current.Y += static_cast<int>(std::lround(dy / length * speed));
		}

		if (current.Z < targetZ)
			current.Z = std::min(targetZ, current.Z + climbStep);
		else
			current.Z = targetZ;

		pAircraft->SetLocation(current);
		pAircraft->InAir = true;
		pAircraft->DockNowHeadingTo = nullptr;

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
		{
			pFly->IsLanding = false;
			pFly->IsTakingOff = false;
			pFly->IsElevating = false;
			pFly->FlightLevel = current.Z - MapClass::Instance.GetCellFloorHeight(current);
		}

		if (current.Z < targetZ)
			return;

		// Departure climbed to flight level: hand the aircraft back to normal
		// flight with the latest remembered move/attack command.
		state = 0;
		const double maxSpeed = task.MaxSpeed > 0.0
			? task.MaxSpeed
			: GetAircraftFlightSpeed(pAircraft);
		pAircraft->SetSpeedPercentage(std::clamp(task.Speed / std::max(1.0, maxSpeed), 0.0, 1.0));
		pAircraft->PrimaryFacing.SetDesired(pAircraft->PrimaryFacing.Current());

		CarrierTakeoffHandoff handoff;
		handoff.Aircraft = pAircraft;
		handoff.Carrier = pCarrier;
		handoff.CommandDestination = task.CommandDestination;
		handoff.CommandTarget = task.CommandTarget;
		handoff.CommandMission = task.CommandMission;
		s_carrierTakeoffHandoffs.push_back(handoff);

		Debug::CarrierLog("[AircraftCarrier] %s runway departure complete slot %u (handoff queued)\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index));

		runway.Departures.erase(it);
	}

	void UpdateCarrierRunwayReturn(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		EnsureRunwayStates(pState, pTypeExt);
		s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
		CoordStruct start {};
		CoordStruct end {};
		auto landingIt = s_landingRunwayIndex.find(pAircraft);
		const int requested = landingIt != s_landingRunwayIndex.end() ? landingIt->second : -1;
		bool explicitRequest = s_landingRunwayExplicit.contains(pAircraft);

		if (explicitRequest
			&& (requested < 0 || requested >= static_cast<int>(pState->LandingRunways.size())))
		{
			explicitRequest = false;
			s_landingRunwayExplicit.erase(pAircraft);
		}

		int selected = -1;

		if (explicitRequest)
		{
			if (requested >= 0
				&& requested < static_cast<int>(pState->LandingRunways.size()))
			{
				auto& runway = pState->LandingRunways[requested];

				if (!runway.TakeoffRequest || runway.TakeoffRequest == pAircraft)
					runway.TakeoffRequest = pAircraft;

				if ((!runway.Occupant || runway.Occupant == pAircraft)
					&& runway.TakeoffRequest == pAircraft)
				{
					selected = requested;
				}
			}
		}
		else
		{
			selected = ChooseLandingRunwayForReturn(pCarrier, pAircraft, pState, pTypeExt, requested);
		}

		if (selected < 0 && !explicitRequest)
		{
			// Nothing is free. Keep the nearest configured runway only for
			// geometry while the aircraft orbits; do not reserve it.
			selected = requested >= 0
				? requested
				: ChooseNearestRunway(pCarrier, pTypeExt, true, pAircraft->Location);
			selected = -1 - selected; // encode "geometry only" as negative
		}

		const bool hasRunway = selected >= 0;
		const int geometryIndex = hasRunway
			? selected
			: (-selected - 1);

		if (hasRunway)
			s_landingRunwayIndex[pAircraft] = selected;
		else if (Unsorted::CurrentFrame % 60 == 0)
		{
			Debug::CarrierLog("[AircraftCarrier] %s waiting for landing runway (explicit=%d req=%d count=%u)\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", explicitRequest ? 1 : 0,
				requested, static_cast<unsigned>(pState->LandingRunways.size()));
		}

		CarrierRunwayState dummy;
		CarrierRunwayState& storage = hasRunway
			? pState->LandingRunways[selected]
			: dummy;
		RunwayStateGuard guard(storage, pState->LandingRunway);
		auto& runway = pState->LandingRunway;
		runway.RunwayIndex = geometryIndex;

		if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, true, geometryIndex, start, end))
		{
			if (runway.TakeoffRequest == pAircraft)
				runway.TakeoffRequest = nullptr;

			s_landingRunwayIndex.erase(pAircraft);
		s_landingWatch.erase(pAircraft);
		s_taxiWatch.erase(pAircraft);
			s_landingRunwayExplicit.erase(pAircraft);
			state = 0;
			return;
		}

		const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		const int cruiseZ = MapClass::Instance.GetCellFloorHeight(start) + flightLevel;
		const bool carrierStopped = IsCarrierStopped(pCarrier);
		const bool runwayFree = hasRunway
			&& (!runway.Occupant || runway.Occupant == pAircraft);

		if (!carrierStopped || !runwayFree)
		{
			const auto center = pCarrier->GetCoords();
			const int odx = center.X - pAircraft->Location.X;
			const int ody = center.Y - pAircraft->Location.Y;
			const double centerDistance = std::sqrt(
				static_cast<double>(odx) * odx + static_cast<double>(ody) * ody);

			constexpr double kOrbitEnterRadius = 4096.0;
			constexpr double kOrbitRadius = 2048.0;
			constexpr int verticalStep = 8;

			if (centerDistance <= kOrbitEnterRadius)
			{
				pAircraft->Destination = nullptr;

				if (pAircraft->Locomotor)
					pAircraft->Locomotor->Stop_Moving();

				if (pAircraft->GetCurrentMission() != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				auto current = pAircraft->Location;
				const double speed = static_cast<double>(pAircraft->Type ? pAircraft->Type->Speed : 8);
				const double phase = static_cast<double>(index) * (kPi * 2.0 / 8.0);
				const double rawAngle = centerDistance > 64.0
					? std::atan2(static_cast<double>(current.Y - center.Y),
						static_cast<double>(current.X - center.X))
					: 0.0;
				const double orbitSpeed = std::max(speed, 32.0);
				const double step = std::clamp(orbitSpeed / kOrbitRadius, 0.008, 0.03);
				const double angle = rawAngle + phase + step;

				CoordStruct target
				{
					center.X + static_cast<int>(std::lround(std::cos(angle) * kOrbitRadius)),
					center.Y + static_cast<int>(std::lround(std::sin(angle) * kOrbitRadius)),
					cruiseZ
				};

				MoveCarrierAutopilot(pAircraft, current, target, orbitSpeed,
					GetCarrierTurnStep(pAircraft), false);

				if (current.Z > cruiseZ)
					current.Z = std::max(cruiseZ, current.Z - verticalStep);
				else if (current.Z < cruiseZ)
					current.Z = std::min(cruiseZ, current.Z + verticalStep);

				if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
					pFly->FlightLevel = current.Z - MapClass::Instance.GetCellFloorHeight(current);

				pAircraft->SetLocation(current);
				pAircraft->InAir = true;
				return;
			}

			// Still far away and the runway is busy / the carrier is moving:
			// fly to a standoff point around the carrier, but never start the
			// runway from here.
			const auto carrierCell = CellClass::Coord2Cell(center);
			const auto aircraftCell = CellClass::Coord2Cell(pAircraft->Location);
			double dx = static_cast<double>(aircraftCell.X - carrierCell.X);
			double dy = static_cast<double>(aircraftCell.Y - carrierCell.Y);
			double length = std::sqrt(dx * dx + dy * dy);

			if (length < 0.1)
			{
				dx = 1.0;
				dy = 0.0;
				length = 1.0;
			}

			const CellStruct holdCell
			{
				static_cast<short>(carrierCell.X + std::lround(dx / length * 16.0)),
				static_cast<short>(carrierCell.Y + std::lround(dy / length * 16.0))
			};

			if (auto const pCell = MapClass::Instance.TryGetCellAt(holdCell))
			{
				if (pAircraft->Destination != pCell || pAircraft->GetCurrentMission() != Mission::Move)
				{
					pAircraft->Destination = pCell;
					pAircraft->QueueMission(Mission::Move, true);
				}
			}

			pAircraft->InAir = true;
			return;
		}

		const auto approach = GetRunwayApproachStart(start, end,
			GetRunwayApproachDistance(pTypeExt));
		const auto approachCell = CellClass::Coord2Cell(approach);
		const double distanceToApproach = HorizontalDistance(pAircraft->Location, approach);
		const bool closeEnough = distanceToApproach <= 768.0;

		if (!closeEnough)
		{
			runway.StartCoords = start;
			runway.EndCoords = end;
			runway.Speed = GetAircraftFlightSpeed(pAircraft);
			runway.TrailEnabled = true;

			if (auto const pCell = MapClass::Instance.TryGetCellAt(approachCell))
			{
				if (pAircraft->Destination != pCell || pAircraft->GetCurrentMission() != Mission::Move)
				{
					pAircraft->Destination = pCell;
					pAircraft->QueueMission(Mission::Move, true);
				}
			}

			pAircraft->InAir = true;

			if (index < pState->CarrierPayloadReturnCell.size())
				pState->CarrierPayloadReturnCell[index] = approachCell;

			return;
		}

		runway.Occupant = pAircraft;
		runway.OccupantSlot = static_cast<int>(index);

		if (runway.TakeoffRequest == pAircraft)
			runway.TakeoffRequest = nullptr;

		runway.CommandDestination = CellStruct { -1, -1 };
		runway.CommandTarget = nullptr;
		runway.CommandMission = 0;
		runway.Phase = CarrierRunwayPhase::LandingApproach;
		runway.Timer = 0;
		runway.StartCoords = start;
		runway.EndCoords = end;
		runway.LastCoords = pAircraft->Location;
		runway.Speed = GetAircraftFlightSpeed(pAircraft);
		runway.Path.clear();
		runway.PathIndex = 0;
		runway.TrailEnabled = true;
		state = 7;

		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = true;

		if (pAircraft->Locomotor)
			pAircraft->Locomotor->Stop_Moving();

		Debug::CarrierLog("[AircraftCarrier] %s runway approach begins slot %u loc=(%d,%d,%d)\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
			pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
	}

	// Advances the phase watchdog. Uses a position window instead of exact
	// per-frame equality: a controller that oscillates by one lepton must still
	// be detected as stuck.
	bool UpdateLandingWatchdog(CarrierLandingProgress& progress,
		const CoordStruct& location, int stuckLimit)
	{
		if (HorizontalDistance(progress.LastPos, location) > 16.0
			|| std::abs(progress.LastPos.Z - location.Z) > 16)
		{
			progress.LastPos = location;
			progress.StuckTimer = 0;
		}
		else
		{
			++progress.StuckTimer;
		}

		++progress.PhaseTimer;

		return progress.StuckTimer >= stuckLimit || progress.PhaseTimer >= 1800;
	}

	void StartLandingGoAround(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, unsigned char& state, const char* reason)
	{
		if (!pAircraft || !pState || index >= pState->CarrierPayloadLanding.size())
			return;

		LogLandingProgress("GoAroundBefore", pAircraft, pState, index);
		auto& progress = pState->CarrierPayloadLanding[index];

		if (progress.RunwayIndex >= 0
			&& progress.RunwayIndex < static_cast<int>(pState->LandingRunways.size()))
		{
			auto& runway = pState->LandingRunways[progress.RunwayIndex];

			if (runway.Occupant == pAircraft)
			{
				runway.Occupant = nullptr;
				runway.OccupantSlot = -1;
				runway.Phase = CarrierRunwayPhase::Idle;
				runway.Timer = 0;
				runway.Speed = 0.0;
				runway.TrailEnabled = false;
				runway.Path.clear();
				runway.PathIndex = 0;
			}
		}

		progress.Phase = CarrierLandingPhase::GoAround;
		progress.PhaseTimer = 0;
		progress.StuckTimer = 0;
		progress.LastPos = pAircraft->Location;
		progress.TaxiPath.clear();
		progress.TaxiPathIndex = 0;
		progress.TaxiTurnTimer = 0;
		state = 2;

		Debug::CarrierLog("[AircraftCarrier] %s landing go-around (%s) slot=%u failures=%d\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", reason ? reason : "unknown",
			static_cast<unsigned>(index), progress.FailedAttempts);
	}

	void UpdateCarrierLandingRunway(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state);

	void UpdateCarrierLandingReturn(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		auto& progress = EnsureLandingProgress(pState, index);

		const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		const int climbStep = std::clamp(
			pAircraft->Type ? pAircraft->Type->Speed / 2 : 6, 3, 12);
		const double cruiseSpeed = GetAircraftFlightSpeed(pAircraft);
		const int turnStep = GetCarrierTurnStep(pAircraft);

		if (Unsorted::CurrentFrame % 5 == 0)
			LogLandingProgress("ReturnUpdate", pAircraft, pState, index);

		if (Unsorted::CurrentFrame % 15 == 0)
		{
			const double toApproach = HorizontalDistance(pAircraft->Location, progress.ApproachPoint);
			const double toTouchdown = HorizontalDistance(pAircraft->Location, progress.TouchdownPoint);
			const bool carrierStopped = IsCarrierStopped(pCarrier);
			const bool reservedByOther = progress.RunwayIndex >= 0
				&& IsLandingRunwayReserved(pState, progress.RunwayIndex, pAircraft);

			Debug::CarrierLog("[CarrierReturn] %s slot=%u phase=%d runway=%d reserved=%d reservedByOther=%d carrierStopped=%d loc=(%d,%d,%d) cruiseZ=%d toApproach=%.0f toA=%.0f timer=%d failed=%d\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
				static_cast<int>(progress.Phase), progress.RunwayIndex,
				progress.Reserved ? 1 : 0, reservedByOther ? 1 : 0, carrierStopped ? 1 : 0,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				progress.CruiseZ, toApproach, toTouchdown,
				progress.PhaseTimer, progress.FailedAttempts);
		}

		// The engine must never own movement in this state.
		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = true;
		pAircraft->SetSpeedPercentage(0.0);

		if (pAircraft->GetCurrentMission() != Mission::Sleep)
			pAircraft->QueueMission(Mission::Sleep, true);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (!IsCarrierStopped(pCarrier))
		{
			CancelCarrierLanding(pCarrier, pAircraft, index, pState, state, "carrier moved");
			return;
		}

		// Refresh all runway geometry from the current ship transform.
		if (progress.RunwayIndex >= 0)
		{
			CoordStruct start {};
			CoordStruct end {};

			if (!GetCarrierRunwayPoints(pCarrier, pTypeExt, true, progress.RunwayIndex, start, end))
			{
				ClearCarrierLandingProgress(pState, pAircraft, index, true);
				progress = CarrierLandingProgress {};
				progress.Phase = CarrierLandingPhase::OrbitWait;
			}
			else
			{
				progress.TouchdownPoint = start;
				progress.RollEndPoint = end;

				progress.ApproachPoint = GetRunwayApproachStart(start, end,
					GetRunwayApproachDistance(pTypeExt));
				progress.LineupPoint = progress.ApproachPoint;
				progress.RunwayFacingRaw = GetRunwayDirection(start, end).Raw;
				progress.CruiseZ = MapClass::Instance.GetCellFloorHeight(start) + flightLevel;
			}
		}

		if (progress.RunwayIndex < 0 && progress.Phase != CarrierLandingPhase::OrbitWait)
			progress.Phase = CarrierLandingPhase::OrbitWait;

		if (progress.Phase == CarrierLandingPhase::None)
			progress.Phase = CarrierLandingPhase::OrbitWait;

		if (progress.RunwayIndex >= 0
			&& progress.Phase != CarrierLandingPhase::OrbitWait
			&& progress.Phase != CarrierLandingPhase::GoAround)
		{
			progress.Reserved = true;
		}

		if (progress.Phase == CarrierLandingPhase::OrbitWait)
		{
			const auto center = pCarrier->GetCoords();
			auto current = pAircraft->Location;
			const double dx = static_cast<double>(current.X - center.X);
			const double dy = static_cast<double>(current.Y - center.Y);
			const double distance = std::sqrt(dx * dx + dy * dy);
			const double angle = distance > 64.0
				? std::atan2(dy, dx) + 0.025
				: 0.0;

			CoordStruct target
			{
				center.X + static_cast<int>(std::lround(std::cos(angle) * 2048.0)),
				center.Y + static_cast<int>(std::lround(std::sin(angle) * 2048.0)),
				progress.CruiseZ > 0 ? progress.CruiseZ : current.Z
			};

			MoveCarrierDirect(pAircraft, current, target, cruiseSpeed, turnStep);

			if (current.Z < target.Z)
				current.Z = std::min(target.Z, current.Z + climbStep);
			else if (current.Z > target.Z)
				current.Z = std::max(target.Z, current.Z - climbStep);

			pAircraft->SetLocation(current);
			pAircraft->InAir = true;
			UpdateCarrierFlightLevel(pAircraft, current);

			// Retry runway acquisition periodically. An occupied explicit
			// runway keeps its reservation, so this only succeeds when it is
			// actually free.
			if (Unsorted::CurrentFrame % 60 == 0)
			{
				int runway = progress.RunwayIndex;

				if (runway >= 0
					&& !IsLandingRunwayReserved(pState, runway, pAircraft))
				{
					progress.Reserved = true;
					progress.Phase = CarrierLandingPhase::ReturnToApproach;
					progress.PhaseTimer = 0;
					progress.StuckTimer = 0;
					progress.LastPos = current;
				}
				else if (progress.RunwayIndex < 0)
				{
					runway = ChooseFreeLandingRunway(pCarrier, pState, pTypeExt,
						pAircraft, current, -1, false);

					if (runway >= 0)
					{
						progress.RunwayIndex = runway;
						progress.Reserved = true;
						progress.Phase = CarrierLandingPhase::ReturnToApproach;
						progress.PhaseTimer = 0;
						progress.StuckTimer = 0;
						progress.LastPos = current;
					}
				}
			}

			return;
		}

		if (progress.Phase == CarrierLandingPhase::GoAround)
		{
			auto current = pAircraft->Location;
			// Climb away from A back toward the approach point (reverse of the
			// landing direction), not across the deck.
			const auto dir = GetRunwayDirection(progress.TouchdownPoint, progress.ApproachPoint);
			const double rad = static_cast<double>(dir.Raw) * (kPi * 2.0 / 65536.0);

			current.X += static_cast<int>(std::lround(std::sin(rad) * cruiseSpeed));
			current.Y -= static_cast<int>(std::lround(std::cos(rad) * cruiseSpeed));

			if (current.Z < progress.CruiseZ)
				current.Z = std::min(progress.CruiseZ, current.Z + climbStep);
			else
				current.Z = progress.CruiseZ;

			pAircraft->PrimaryFacing.SetCurrent(dir);
			pAircraft->PrimaryFacing.SetDesired(dir);
			pAircraft->SetLocation(current);
			pAircraft->InAir = true;
			UpdateCarrierFlightLevel(pAircraft, current);

			if (current.Z < progress.CruiseZ)
				return;

			++progress.FailedAttempts;
			const int failedAttempts = progress.FailedAttempts;
			const int cruiseZ = progress.CruiseZ;
			const int runwayIndex = progress.RunwayIndex;
			progress.PhaseTimer = 0;
			progress.StuckTimer = 0;
			progress.LastPos = current;

			if (failedAttempts < 3 && runwayIndex >= 0)
			{
				progress.Phase = CarrierLandingPhase::ReturnToApproach;
				Debug::CarrierLog("[AircraftCarrier] %s re-approach attempt %d/3 runway=%d\n",
					pAircraft->Type ? pAircraft->Type->ID : "?",
					failedAttempts, runwayIndex);
			}
			else
			{
				ClearCarrierLandingProgress(pState, pAircraft, index, true);
				auto& reset = EnsureLandingProgress(pState, index);
				reset.Phase = CarrierLandingPhase::OrbitWait;
				reset.CruiseZ = cruiseZ;
				reset.FailedAttempts = failedAttempts;
				reset.LastPos = current;

				Debug::CarrierLog("[AircraftCarrier] %s orbital hold after %d failed approaches\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", reset.FailedAttempts);
			}

			return;
		}

		if (progress.Phase == CarrierLandingPhase::ReturnToApproach)
		{
			if (progress.RunwayIndex >= 0
				&& IsLandingRunwayReserved(pState, progress.RunwayIndex, pAircraft))
			{
				progress.Phase = CarrierLandingPhase::OrbitWait;
				progress.Reserved = false;
				progress.PhaseTimer = 0;
				progress.StuckTimer = 0;
				return;
			}

			auto current = pAircraft->Location;

			if (current.Z < progress.CruiseZ)
				current.Z = std::min(progress.CruiseZ, current.Z + climbStep);
			else if (current.Z > progress.CruiseZ)
				current.Z = std::max(progress.CruiseZ, current.Z - climbStep);

			const double distance = HorizontalDistance(current, progress.ApproachPoint);

			if (distance <= 2048.0
				&& progress.RunwayIndex >= 0
				&& !IsLandingRunwayReserved(pState, progress.RunwayIndex, pAircraft))
			{
				auto& runway = pState->LandingRunways[progress.RunwayIndex];
				runway.Occupant = pAircraft;
				runway.OccupantSlot = static_cast<int>(index);
				runway.RunwayIndex = progress.RunwayIndex;
				runway.StartCoords = progress.TouchdownPoint;
				runway.EndCoords = progress.RollEndPoint;
				runway.Phase = CarrierRunwayPhase::LandingApproach;
				runway.Timer = 0;
				runway.Speed = 0.0;
				runway.Path.clear();
				runway.PathIndex = 0;
				runway.TrailEnabled = false;

				// Do not snap the facing here. Line up with the landing
				// runway direction (A -> Z) first; the descent starts only
				// when the aircraft is actually aligned.
				progress.Phase = CarrierLandingPhase::Lineup;
				progress.PhaseTimer = 0;
				progress.StuckTimer = 0;
				progress.LastPos = current;
				progress.Speed = std::max(8.0, std::min(cruiseSpeed, 32.0));

				pAircraft->SetLocation(current);
				UpdateCarrierFlightLevel(pAircraft, current);
				Debug::CarrierLog("[AircraftCarrier] %s lineup at approach slot=%u runway=%d facing=%u target=%u\n",
					pAircraft->Type ? pAircraft->Type->ID : "?",
					static_cast<unsigned>(index), progress.RunwayIndex,
					static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
					static_cast<unsigned>(progress.RunwayFacingRaw));
				return;
			}

			MoveCarrierDirect(pAircraft, current, progress.ApproachPoint,
				cruiseSpeed, turnStep);
			pAircraft->SetLocation(current);
			pAircraft->InAir = true;
			UpdateCarrierFlightLevel(pAircraft, current);

			if (UpdateLandingWatchdog(progress, current, 240))
				StartLandingGoAround(pCarrier, pAircraft, index, pState, state,
					"return approach stuck");

			return;
		}

		if (progress.Phase == CarrierLandingPhase::Lineup)
		{
			auto current = pAircraft->Location;

			if (current.Z < progress.CruiseZ)
				current.Z = std::min(progress.CruiseZ, current.Z + climbStep);
			else if (current.Z > progress.CruiseZ)
				current.Z = std::max(progress.CruiseZ, current.Z - climbStep);

			const auto desired = DirStruct(progress.RunwayFacingRaw);
			const int diff = std::abs(static_cast<short>(
				desired.Raw - pAircraft->PrimaryFacing.Current().Raw));

			if (diff > 512)
			{
				// Turn toward the runway heading at the approach point. This
				// replaces the old one-frame SetCurrent snap; the higher rate
				// keeps the lineup from looking like a long stationary spin.
				TurnAircraftToward(pAircraft, desired, std::max(turnStep, 4096));

				// Keep closing on the approach point while turning instead of
				// hovering in place and rotating.
				const double toApproach = HorizontalDistance(current, progress.ApproachPoint);

				if (toApproach > 16.0)
				{
					const double dx = static_cast<double>(progress.ApproachPoint.X - current.X);
					const double dy = static_cast<double>(progress.ApproachPoint.Y - current.Y);
					const double step = std::min(std::max(8.0, cruiseSpeed * 0.6), toApproach);
					current.X += static_cast<int>(std::lround(dx / toApproach * step));
					current.Y += static_cast<int>(std::lround(dy / toApproach * step));
				}

				pAircraft->SetLocation(current);
				pAircraft->InAir = true;
				UpdateCarrierFlightLevel(pAircraft, current);

				if (UpdateLandingWatchdog(progress, current, 240))
				{
					StartLandingGoAround(pCarrier, pAircraft, index, pState, state,
						"lineup stuck");
				}

				return;
			}

			pAircraft->PrimaryFacing.SetCurrent(desired);
			pAircraft->PrimaryFacing.SetDesired(desired);
			progress.Phase = CarrierLandingPhase::FinalApproach;
			progress.PhaseTimer = 0;
			progress.StuckTimer = 0;
			progress.LastPos = current;
			progress.Speed = std::max(8.0, std::min(cruiseSpeed, 32.0));

			Debug::CarrierLog("[AircraftCarrier] %s landing descent begins slot=%u runway=%d facing=%u\n",
				pAircraft->Type ? pAircraft->Type->ID : "?",
				static_cast<unsigned>(index), progress.RunwayIndex,
				static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw));

			return;
		}

		// The descent/roll/taxi phases stay in state 2 as well. Keeping the
		// whole landing in one state avoids the engine's runway-controlled
		// hooks (and the selection/occupancy side effects they had at the
		// state 2 -> 7 transition).
		if (progress.Phase == CarrierLandingPhase::Lineup
			|| progress.Phase == CarrierLandingPhase::FinalApproach
			|| progress.Phase == CarrierLandingPhase::Touchdown
			|| progress.Phase == CarrierLandingPhase::LandingRoll
			|| progress.Phase == CarrierLandingPhase::TaxiToSlot)
		{
			UpdateCarrierLandingRunway(pCarrier, pAircraft, index, pState, pTypeExt, state);
		}
	}

	void UpdateCarrierLandingRunway(UnitClass* pCarrier, AircraftClass* pAircraft, size_t index,
		FootprintState* pState, TechnoTypeExt::ExtData* pTypeExt, unsigned char& state)
	{
		auto& progress = EnsureLandingProgress(pState, index);

		if (Unsorted::CurrentFrame % 5 == 0)
			LogLandingProgress("RunwayUpdate", pAircraft, pState, index);

		if (progress.RunwayIndex < 0
			|| progress.RunwayIndex >= static_cast<int>(pState->LandingRunways.size()))
		{
			StartLandingGoAround(pCarrier, pAircraft, index, pState, state,
				"runway state missing");
			return;
		}

		auto& runway = pState->LandingRunways[progress.RunwayIndex];

		if (Unsorted::CurrentFrame % 15 == 0)
		{
			const double toA = HorizontalDistance(pAircraft->Location, progress.TouchdownPoint);
			const double toZ = HorizontalDistance(pAircraft->Location, progress.RollEndPoint);
			const bool carrierStopped = IsCarrierStopped(pCarrier);

			Debug::CarrierLog("[CarrierLanding] %s slot=%u phase=%u(%s) runway=%d rwOccupant=%p rwSlot=%d rwPhase=%d loc=(%d,%d,%d) A=(%d,%d,%d) Z=(%d,%d,%d) toA=%.0f toZ=%.0f Z=%d cruiseZ=%d speed=%.1f carrierStopped=%d timer=%d stuck=%d taxi=%u/%u\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<unsigned>(index),
				static_cast<unsigned>(progress.Phase), LandingPhaseName(progress.Phase),
				progress.RunwayIndex, static_cast<void*>(runway.Occupant),
				runway.OccupantSlot, static_cast<int>(runway.Phase),
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				progress.TouchdownPoint.X, progress.TouchdownPoint.Y, progress.TouchdownPoint.Z,
				progress.RollEndPoint.X, progress.RollEndPoint.Y, progress.RollEndPoint.Z,
				toA, toZ, pAircraft->Location.Z, progress.CruiseZ, progress.Speed,
				carrierStopped ? 1 : 0, progress.PhaseTimer, progress.StuckTimer,
				static_cast<unsigned>(progress.TaxiPathIndex),
				static_cast<unsigned>(progress.TaxiPath.size()));
		}

		if (runway.Occupant != pAircraft)
		{
			Debug::CarrierLog("[DiagLanding] runway occupant mismatch slot=%u rw=%d old=%p new=%p\n",
				static_cast<unsigned>(index), progress.RunwayIndex,
				static_cast<void*>(runway.Occupant), static_cast<void*>(pAircraft));
			runway.Occupant = pAircraft;
			runway.OccupantSlot = static_cast<int>(index);
			runway.RunwayIndex = progress.RunwayIndex;
		}

		const double cruiseSpeed = GetAircraftFlightSpeed(pAircraft);
		const int turnStep = GetCarrierTurnStep(pAircraft);

		pAircraft->Destination = nullptr;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->SetSpeedPercentage(0.0);

		if (pAircraft->GetCurrentMission() != Mission::Sleep)
			pAircraft->QueueMission(Mission::Sleep, true);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (!IsCarrierStopped(pCarrier))
		{
			CancelCarrierLanding(pCarrier, pAircraft, index, pState, state, "carrier moved");
			return;
		}

		switch (progress.Phase)
		{
		case CarrierLandingPhase::Lineup:
		{
			// Legacy save phase. The new controller goes straight from the
			// return flight to the descent; no separate lineup is needed.
			progress.Phase = CarrierLandingPhase::FinalApproach;
			progress.PhaseTimer = 0;
			progress.StuckTimer = 0;
			progress.LastPos = pAircraft->Location;
			progress.Speed = std::max(8.0, std::min(cruiseSpeed, 32.0));
			return;
		}

		case CarrierLandingPhase::FinalApproach:
		{
			// Reverse of takeoff departure: fly from beyond Z back toward A
			// while the altitude interpolates from cruise level down to the
			// deck. It touches down exactly at A.
			auto current = pAircraft->Location;
			const auto start = progress.ApproachPoint;
			const auto end = progress.TouchdownPoint;
			const double total = std::max(1.0, HorizontalDistance(start, end));
			const double speed = std::max(1.0,
				progress.Speed > 0.0 ? progress.Speed : cruiseSpeed);

			MoveCarrierDirect(pAircraft, current, end, speed, turnStep);

			const double remainingAfter = HorizontalDistance(current, end);
			const double ratio = std::clamp(remainingAfter / total, 0.0, 1.0);
			const int cruiseZ = progress.CruiseZ;
			current.Z = end.Z + static_cast<int>(std::lround(
				static_cast<double>(cruiseZ - end.Z) * ratio));

			const auto runwayDir = DirStruct(progress.RunwayFacingRaw);
			TurnAircraftToward(pAircraft, runwayDir, turnStep);
			const int alignDiff = std::abs(static_cast<short>(
				runwayDir.Raw - pAircraft->PrimaryFacing.Current().Raw));

			pAircraft->SetLocation(current);
			pAircraft->InAir = true;
			UpdateCarrierFlightLevel(pAircraft, current);

			if (alignDiff <= 512
				&& (remainingAfter <= std::max(16.0, speed) || current.Z <= end.Z + 8))
			{
				pAircraft->SetLocation(end);
				pAircraft->InAir = false;
				UpdateCarrierFlightLevel(pAircraft, end);
				progress.Phase = CarrierLandingPhase::Touchdown;
				progress.PhaseTimer = 0;
				progress.StuckTimer = 0;
				progress.LastPos = end;
				return;
			}

			if (UpdateLandingWatchdog(progress, current, 360))
				StartLandingGoAround(pCarrier, pAircraft, index, pState, state,
					"final approach stuck");

			return;
		}

		case CarrierLandingPhase::Touchdown:
		{
			pAircraft->SetLocation(progress.TouchdownPoint);
			pAircraft->InAir = false;
			pAircraft->SetHeight(0);
			UpdateCarrierFlightLevel(pAircraft, progress.TouchdownPoint);
			progress.Phase = CarrierLandingPhase::LandingRoll;
			progress.PhaseTimer = 0;
			progress.StuckTimer = 0;
			progress.LastPos = progress.TouchdownPoint;
			// Start the roll at the approach speed; do not accelerate at
			// touchdown. The roll then applies negative acceleration.
			if (progress.Speed <= 0.0)
				progress.Speed = GetAircraftFlightSpeed(pAircraft);

			runway.Phase = CarrierRunwayPhase::LandingRoll;
			runway.Timer = 0;
			runway.Speed = progress.Speed;
			runway.StartCoords = progress.TouchdownPoint;
			runway.EndCoords = progress.RollEndPoint;
			return;
		}

		case CarrierLandingPhase::LandingRoll:
		{
			auto current = pAircraft->Location;
			const auto start = progress.TouchdownPoint;
			const auto end = progress.RollEndPoint;
			const double length = std::max(1.0, HorizontalDistance(start, end));
			const double dirX = static_cast<double>(end.X - start.X) / length;
			const double dirY = static_cast<double>(end.Y - start.Y) / length;
			double along = (current.X - start.X) * dirX
				+ (current.Y - start.Y) * dirY;

			if (along < 0.0)
			{
				along = 0.0;
				current = start;
				pAircraft->SetLocation(current);
				UpdateCarrierFlightLevel(pAircraft, current);
			}

			const double maxOverrun = static_cast<double>(GetLandingMaxOverrun(pTypeExt));
			const double stopAlong = length + maxOverrun;
			const auto direction = GetRunwayDirection(start, end);

			double speed = progress.Speed > 0.0
				? progress.Speed
				: GetAircraftFlightSpeed(pAircraft);
			speed = std::max(0.0, speed - GetLandingDeceleration(pTypeExt));
			progress.Speed = speed;

			pAircraft->PrimaryFacing.SetCurrent(direction);
			pAircraft->PrimaryFacing.SetDesired(direction);
			pAircraft->InAir = false;
			runway.TrailEnabled = true;

			if (speed <= 0.75 || along >= stopAlong)
			{
				runway.TrailEnabled = false;
				progress.Phase = CarrierLandingPhase::TaxiToSlot;
				progress.PhaseTimer = 0;
				progress.StuckTimer = 0;
				progress.LastPos = current;
				progress.TaxiPath.clear();
				progress.TaxiPathIndex = 0;
				progress.TaxiTurnTimer = 0;
				progress.TaxiSpeed = GetRunwayTaxiSpeed(pTypeExt);
				return;
			}

			const double remaining = stopAlong - along;
			const double step = std::min(speed, remaining);
			current.X += static_cast<int>(std::lround(dirX * step));
			current.Y += static_cast<int>(std::lround(dirY * step));
			current.Z = start.Z;
			pAircraft->SetLocation(current);
			UpdateCarrierFlightLevel(pAircraft, current);

			if (UpdateLandingWatchdog(progress, current, 180))
			{
				progress.Phase = CarrierLandingPhase::TaxiToSlot;
				progress.PhaseTimer = 0;
				progress.StuckTimer = 0;
				progress.LastPos = current;
				progress.TaxiPath.clear();
				progress.TaxiPathIndex = 0;
				progress.TaxiTurnTimer = 0;
				progress.TaxiSpeed = GetRunwayTaxiSpeed(pTypeExt);
			}

			return;
		}

		case CarrierLandingPhase::TaxiToSlot:
		{
			const auto slot = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
			auto current = pAircraft->Location;

			if (progress.TaxiPath.empty())
			{
				progress.TaxiPath = BuildRunwayTaxiPath(pCarrier, pState, pAircraft, slot);
				progress.TaxiPathIndex = 0;
				// Move continuously. The old stop-at-every-waypoint turn made
				// the landing taxi feel much slower than the takeoff taxi.
				progress.TaxiTurnTimer = 0;
				progress.TaxiSpeed = std::min(64.0,
					GetRunwayTaxiSpeed(pTypeExt) * 1.5);

				if (progress.TaxiPath.empty())
				{
					// Wait for the deck to clear. After a long timeout fall
					// back to the parking slot so the aircraft cannot remain
					// frozen on the runway forever.
					if (UpdateLandingWatchdog(progress, current, 300))
					{
						pAircraft->SetLocation(slot);
						ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState,
							pTypeExt, state, "landing taxi timeout");
					}

					pAircraft->InAir = false;
					return;
				}
			}

			if (progress.TaxiPathIndex >= progress.TaxiPath.size())
			{
				ParkCarrierPayloadAtSlot(pCarrier, pAircraft, index, pState,
					pTypeExt, state, "landing taxi done");
				return;
			}

			const auto target = progress.TaxiPath[progress.TaxiPathIndex];
			const double distance = HorizontalDistance(current, target);
			const double speed = std::max(1.0, progress.TaxiSpeed);

			if (progress.TaxiTurnTimer > 0)
			{
				const auto desired = GetRunwayDirection(current, target);
				TurnAircraftToward(pAircraft, desired, GetRunwayTurnRate(pTypeExt));
				pAircraft->SetLocation(current);
				pAircraft->InAir = false;
				UpdateCarrierFlightLevel(pAircraft, current);

				const int diff = std::abs(static_cast<short>(
					desired.Raw - pAircraft->PrimaryFacing.Current().Raw));

				if (diff <= 256)
				{
					pAircraft->PrimaryFacing.SetCurrent(desired);
					pAircraft->PrimaryFacing.SetDesired(desired);
					progress.TaxiTurnTimer = 0;
				}

				if (UpdateLandingWatchdog(progress, current, 300))
				{
					progress.TaxiPath.clear();
					progress.TaxiPathIndex = 0;
					progress.TaxiTurnTimer = 0;
				}

				return;
			}

			if (distance <= std::max(1.0, speed))
			{
				pAircraft->SetLocation(target);
				UpdateCarrierFlightLevel(pAircraft, target);
				++progress.TaxiPathIndex;
				progress.StuckTimer = 0;
				progress.LastPos = target;
				return;
			}

			MoveCarrierTaxiStep(pAircraft, current, target, speed);
			UpdateCarrierFlightLevel(pAircraft, current);

			if (UpdateLandingWatchdog(progress, current, 300))
			{
				progress.TaxiPath.clear();
				progress.TaxiPathIndex = 0;
				progress.TaxiTurnTimer = 0;
			}

			return;
		}

		case CarrierLandingPhase::GoAround:
			state = 2;
			return;

		default:
			progress.Phase = CarrierLandingPhase::Lineup;
			progress.PhaseTimer = 0;
			progress.StuckTimer = 0;
			return;
		}
	}

void Footprint::UpdateCarrierPayload(AircraftClass* pAircraft)
{
	if (!pAircraft || !pAircraft->IsAlive)
		return;

	if (s_carrierPayloadUpdateDepth > 0)
	{
		Debug::CarrierLog("[CarrierGuard] reentrant UpdateCarrierPayload for %p\n",
			static_cast<void*>(pAircraft));
		return;
	}

	CarrierPayloadUpdateGuard updateGuard;

	const auto pCarrier = GetCarrierPayloadOwner(pAircraft);

	if (!pCarrier || !pCarrier->IsAlive)
	{
		s_carrierPayloads.erase(pAircraft);
		return;
	}

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	UpdateCarrierMoveState(pCarrier);

	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return;

	if (pState->CarrierPayloadReloadTimer.size() < pState->CarrierPayloads.size())
		pState->CarrierPayloadReloadTimer.resize(pState->CarrierPayloads.size(), 0);

	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

	if (!pTypeExt)
		return;

	const auto mission = pAircraft->GetCurrentMission();

	EnsureRunwayStates(pState, pTypeExt);

	if (index == 0 || (pState->CarrierPayloadDocked[index] != 1
		&& Unsorted::CurrentFrame % 15 == 0))
	{
		const auto destCoords = pAircraft->Destination
			? pAircraft->Destination->GetCoords()
			: CoordStruct::Empty;

		Debug::CarrierLog("[CarrierFrame] slot=%d f=%d state=%d mission=%d target=%p dest=%p destCell=(%d,%d,%d)"
			" inAirField=%d loc=(%d,%d,%d) facing=%u sel=%d req=%d ammo=%d hasAmmo=%d\n",
			static_cast<int>(index), Unsorted::CurrentFrame,
			static_cast<int>(pState->CarrierPayloadDocked[index]),
			static_cast<int>(mission), static_cast<void*>(pAircraft->Target),
			static_cast<void*>(pAircraft->Destination),
			destCoords.X, destCoords.Y, destCoords.Z,
			pAircraft->InAir ? 1 : 0,
			pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
			static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
			pAircraft->IsSelected ? 1 : 0,
			(index < pState->CarrierPayloadLaunchRequest.size()
				? pState->CarrierPayloadLaunchRequest[index] : 0),
			pAircraft->Ammo, TechnoExt::HasAnyWeaponAmmo(pAircraft) ? 1 : 0);
	}

	// Deck state: 0 = airborne, 1 = parked, 2 = returning, 6/7 = runway
	// takeoff/landing. This is deliberately independent of the engine's mission
	// state machine: AirportBound aircraft re-queue Enter every time they want
	// to go home, and there is no native dock to satisfy it any more.
	unsigned char& state = pState->CarrierPayloadDocked[index];

	if (state == 2 || state == 7 || Unsorted::CurrentFrame % 30 == 0)
		LogLandingProgress("UpdateCarrierPayload", pAircraft, pState, index);

	EnsureRunwayStates(pState, pTypeExt);

	if (state == 6)
	{
		const int runwayIndex = FindTakeoffRunwayIndex(pState, pAircraft);

		if (runwayIndex >= 0)
		{
			RunwayStateGuard guard(pState->TakeoffRunways[runwayIndex], pState->Runway);
			UpdateCarrierRunwayPayload(pCarrier, pAircraft, index, pState, pTypeExt, state,
				pState->Runway);
		}
		else
		{
			pState->Runway = CarrierRunwayState {};
			UpdateCarrierRunwayPayload(pCarrier, pAircraft, index, pState, pTypeExt, state,
				pState->Runway);
		}

		return;
	}

	if (state == 7)
	{
		UpdateCarrierLandingRunway(pCarrier, pAircraft, index, pState, pTypeExt, state);
		return;
	}

	if (state == 8)
	{
		const int runwayIndex = FindTakeoffRunwayIndex(pState, pAircraft);

		if (runwayIndex >= 0)
		{
			RunwayStateGuard guard(pState->TakeoffRunways[runwayIndex], pState->Runway);
			UpdateRunwayDeparture(pCarrier, pAircraft, index, pState, state);
		}
		else
		{
			pState->Runway = CarrierRunwayState {};
			UpdateRunwayDeparture(pCarrier, pAircraft, index, pState, state);
		}

		return;
	}

	// Shared touchdown cleanup for the return autopilot. Keeps the
	// parked-state bookkeeping in one place.
	auto const finishLanding = [&](const CoordStruct& slot)
	{
		if (index < pState->CarrierPayloadIgnoredDest.size())
		{
			pState->CarrierPayloadIgnoredDest[index] = pAircraft->Destination
				? CellClass::Coord2Cell(pAircraft->Destination->GetCoords())
				: CellStruct { -1, -1 };
		}

		pAircraft->SetLocation(slot);
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->SetDestination(nullptr, true);
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->InAir = false;
		pAircraft->QueueMission(Mission::Sleep, true);
		pAircraft->PrimaryFacing.SetDesired(GetCarrierSlotFacing(pCarrier, pTypeExt, index));

		if (auto const pLoco = pAircraft->Locomotor)
		{
			if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pLoco))
			{
				pFly->IsLanding = false;
				pFly->FlightLevel = 0;
			}
		}

		state = 1;
		s_carrierReturnSpeed.erase(pAircraft);
		s_returnProgressRef.erase(pAircraft);
		s_returnStuckChecks.erase(pAircraft);

		// A request captured while the aircraft was still flying must not
		// launch it again on the first parked frame.
		if (index < pState->CarrierPayloadLaunchRequest.size())
			pState->CarrierPayloadLaunchRequest[index] = 0;

		if (index < pState->CarrierPayloadRequestedDest.size())
			pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };

		if (index < pState->CarrierPayloadReloadTimer.size())
			pState->CarrierPayloadReloadTimer[index] = 0;

		Debug::CarrierLog("[AircraftCarrier] %s landed in slot %d\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index));
	};

	// Only an explicit Move to somewhere away from the ship, or an Attack
	// mission, counts as a player order. The engine assigns short Move orders
	// to the deck, and auto-acquire sets Area_Guard targets; neither may launch
	// or cancel a return.
	const auto hasPlayerOrder = [&]() -> bool
	{
		// Scripted/AI orders and the engine's own restored destinations are not
		// player commands. The player must have the aircraft selected.
		if (!pAircraft->IsSelected)
			return false;

		if (mission == Mission::Attack || mission == Mission::AttackMove)
			return true;

		// Only an explicit player Stop cancels a manual return. Guard is set
		// automatically by the engine when the aircraft reaches the deck and
		// must not abort the last moments of the landing.
		if (mission == Mission::Stop)
			return true;

		// The engine converts any destination assigned to an airport-bound
		// aircraft into Mission::Enter, which the carrier gate suppresses. A
		// selected payload with a destination is therefore a real player move
		// order unless the destination is the carrier itself, one of its
		// landing runway cells, the standoff return cell, or the approach /
		// touchdown geometry. Treating the carrier click as a move order was
		// repeatedly cancelling the final approach at A.
		if (pAircraft->Destination)
		{
			const auto destCoords = pAircraft->Destination->GetCoords();
			const auto destCell = CellClass::Coord2Cell(destCoords);
			const auto returnCell = index < pState->CarrierPayloadReturnCell.size()
				? pState->CarrierPayloadReturnCell[index]
				: CellStruct { -1, -1 };

			if (returnCell.X >= 0 && destCell == returnCell)
				return false;

			if (auto const pCell = MapClass::Instance.TryGetCellAt(destCell))
			{
				if (IsOwnCell(pCarrier, pCell, pState->Alt)
					|| IsOwnCell(pCarrier, pCell, !pState->Alt))
				{
					return false;
				}
			}

			if (pTypeExt && FindLandingRunwayAtCell(pCarrier, pTypeExt, destCell) >= 0)
				return false;

			if (index < pState->CarrierPayloadLanding.size())
			{
				auto const& progress = pState->CarrierPayloadLanding[index];

				if (HorizontalDistance(destCoords, progress.ApproachPoint) < 512.0
					|| HorizontalDistance(destCoords, progress.TouchdownPoint) < 512.0
					|| HorizontalDistance(destCoords, progress.RollEndPoint) < 512.0)
				{
					return false;
				}
			}

			// The auto-return standoff cell is a few cells behind the ship and
			// can be farther than the stored CarrierPayloadReturnCell. A target
			// this close to the carrier is a return/dock destination, not a
			// player move that should cancel the landing.
			if (HorizontalDistance(destCoords, pCarrier->GetCoords()) <= 4096.0)
				return false;

			return true;
		}

		return false;
	};

	if (state == 1)
	{
		// Never release a parked payload just because the engine nudged it.
		// The pin below brings it back to the slot; releasing it here was one
		// of the causes of "parked aircraft takes off by itself".

		// Unlimbo and the scenario/AI set a Move destination on freshly spawned
		// aircraft. Ignore every order for a short grace period, otherwise all
		// payloads launch themselves on the frame they are created.
		if (index < pState->CarrierPayloadReloadTimer.size()
			&& pState->CarrierPayloadReloadTimer[index] < 0)
		{
			++pState->CarrierPayloadReloadTimer[index];
			pAircraft->DockNowHeadingTo = nullptr;

			if (pAircraft->Destination)
				pAircraft->SetDestination(nullptr, true);

			if (auto const pLoco = pAircraft->Locomotor)
				pLoco->Stop_Moving();

			pAircraft->InAir = false;
			pAircraft->QueueMission(Mission::Sleep, true);
			return;
		}

		bool newPlayerDestination = false;

		if (pAircraft->IsSelected && pAircraft->Destination)
		{
			const auto destCoords = pAircraft->Destination->GetCoords();
			const auto destCell = CellClass::Coord2Cell(destCoords);
			const auto ignored = index < pState->CarrierPayloadIgnoredDest.size()
				? pState->CarrierPayloadIgnoredDest[index]
				: CellStruct { -1, -1 };

			newPlayerDestination = destCell != ignored
				&& destCoords.DistanceFrom(pCarrier->GetCoords()) > 512.0;
		}

		const bool launchRequested = index < pState->CarrierPayloadLaunchRequest.size()
			&& pState->CarrierPayloadLaunchRequest[index] != 0;

		const bool aiMoveOrder = (mission == Mission::Move || mission == Mission::QMove)
			&& pAircraft->Destination
			&& pAircraft->Destination->GetCoords().DistanceFrom(pCarrier->GetCoords()) > 512.0;
		const bool aiAttackOrder = (mission == Mission::Attack || mission == Mission::AttackMove)
			&& pAircraft->Target;
		const bool aiRunwayOrder = HasRunwayMode(pTypeExt, false)
			&& pCarrier->Owner && !pCarrier->Owner->IsControlledByHuman()
			&& (aiMoveOrder || aiAttackOrder);

		// A request left over from the landing phase or from an engine
		// destination must not launch an unselected parked aircraft.
		if (launchRequested && !pAircraft->IsSelected
			&& index < pState->CarrierPayloadLaunchRequest.size())
		{
			pState->CarrierPayloadLaunchRequest[index] = 0;
		}

		// Only a click-generated request on a selected aircraft launches, or
		// an AI carrier's real Move/Attack order. Engine auto-acquire and the
		// leftover Move must never take a parked aircraft off the deck.
		const bool ordered = (launchRequested && pAircraft->IsSelected) || aiRunwayOrder;

		// Enter is the engine's "go home" request, not a player order. Replace
		// it (start_mission=true) so Mission_Enter stops running; there is no
		// order to lose here.
		if (mission == Mission::Enter && !ordered)
		{
			pAircraft->DockNowHeadingTo = nullptr;

			if (pAircraft->Destination)
				pAircraft->SetDestination(nullptr, true);

			ForceCarrierSleep(pAircraft);
		}

		// An empty aircraft cannot attack. Ignore a stale Attack order while it
		// is still rearming; a real move order still launches it.
		const bool canLaunch = launchRequested
			|| aiRunwayOrder
			|| TechnoExt::HasAnyWeaponAmmo(pAircraft);

		if (HasRunwayMode(pTypeExt, false) && ordered && canLaunch)
		{
			RequestRunwayTakeoff(pCarrier, pAircraft, index, pState, pTypeExt, state);
			return;
		}

		const bool carrierReady = ordered && canLaunch && IsCarrierStopped(pCarrier);

		if (ordered && canLaunch && !carrierReady)
		{
			Debug::CarrierLog("[AircraftCarrier] %s launch blocked: carrier is moving\n",
				pAircraft->Type ? pAircraft->Type->ID : "?");
		}

		if (carrierReady)
		{
			state = 0;

			if (index < pState->CarrierPayloadLaunchRequest.size())
				pState->CarrierPayloadLaunchRequest[index] = 0;

			// Start exactly at the slot. A large artificial lift made the
			// aircraft appear to jump sideways when the engine's first Move
			// step landed; let the fly locomotor do the climb.
			CoordStruct lift = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
			lift.Z += 64;
			pAircraft->SetLocation(lift);

			if (auto const pLoco = pAircraft->Locomotor)
				pLoco->Stop_Moving();

			// Launch must actually leave the ground state.
			pAircraft->InAir = true;

			// Override the engine's clamped destination with the cell the player
			// really clicked.
			if (index < pState->CarrierPayloadRequestedDest.size()
				&& pState->CarrierPayloadRequestedDest[index].X >= 0)
			{
				const auto pRequested = MapClass::Instance.TryGetCellAt(
					pState->CarrierPayloadRequestedDest[index]);

				if (pRequested)
				{
					pAircraft->SetDestination(pRequested, true);
					pAircraft->QueueMission(Mission::Move, true);
				}

				pState->CarrierPayloadRequestedDest[index] = CellStruct { -1, -1 };
			}

			pState->CarrierPayloadReloadTimer[index] = 0;

			const auto destCoords = pAircraft->Destination
				? pAircraft->Destination->GetCoords()
				: CoordStruct::Empty;

			Debug::CarrierLog("[AircraftCarrier] %s launched from slot %d (mission=%d target=%p dest=%p destCell=(%d,%d,%d) self=(%d,%d,%d))\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index),
				static_cast<int>(mission), static_cast<void*>(pAircraft->Target),
				static_cast<void*>(pAircraft->Destination),
				destCoords.X, destCoords.Y, destCoords.Z,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
			return;
		}

		// No order: kill the engine's own Move/Enter so it cannot drag the
		// aircraft off the deck between frames.
		if (pAircraft->Destination)
			pAircraft->SetDestination(nullptr, true);

		if (auto const pLoco = pAircraft->Locomotor)
			pLoco->Stop_Moving();

		if (mission != Mission::Sleep)
			pAircraft->QueueMission(Mission::Sleep, true);

		// Rearm on deck (legacy Ammo field). Per-weapon pools reload through
		// TechnoExt::IsAtAirfield().
		const int maxAmmo = pAircraft->Type ? pAircraft->Type->Ammo : 0;

		if (maxAmmo > 0 && pAircraft->Ammo < maxAmmo)
		{
			auto& timer = pState->CarrierPayloadReloadTimer[index];

			if (++timer >= 30)
			{
				timer = 0;
				++pAircraft->Ammo;
			}
		}
		else
		{
			pState->CarrierPayloadReloadTimer[index] = 0;
		}

		// Keep the parked aircraft pinned to the moving deck.
		const auto expected = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);

		const int dx = std::abs(pAircraft->Location.X - expected.X);
		const int dy = std::abs(pAircraft->Location.Y - expected.Y);
		const int dz = std::abs(pAircraft->Location.Z - expected.Z);

		if (dx > 2 || dy > 2 || dz > 2)
			pAircraft->SetLocation(expected);

		pAircraft->InAir = false;

		// Snap once inside tolerance. Continuously requesting a desired facing
		// while the engine/transform jitters by a few raw units makes parked
		// aircraft rotate back and forth forever.
		const DirStruct facing = GetCarrierSlotFacing(pCarrier, pTypeExt, index);
		const int facingDiff = std::abs(static_cast<short>(
			facing.Raw - pAircraft->PrimaryFacing.Current().Raw));

		if (facingDiff > 256)
		{
			pAircraft->PrimaryFacing.SetDesired(facing);
		}
		else
		{
			pAircraft->PrimaryFacing.SetCurrent(facing);
			pAircraft->PrimaryFacing.SetDesired(facing);
		}

		return;
	}

	// Fully manual return/approach. A real player order cancels the landing
	// and returns control immediately; the engine never drives movement here.
	if (state == 2)
	{
		if (hasPlayerOrder())
		{
			CancelCarrierLanding(pCarrier, pAircraft, index, pState, state,
				"player order during return", true);
			return;
		}

		UpdateCarrierLandingReturn(pCarrier, pAircraft, index, pState, pTypeExt, state);
		return;
	}

	// Returning aircraft can be recalled by the player with a new order.
	if ((state >= 2 && state <= 5) && hasPlayerOrder())
	{
		state = 0;
		pAircraft->InAir = true;
		pAircraft->DockNowHeadingTo = nullptr;

		// Do not immediately force an out-of-ammo aircraft back home again.
		s_returnSuppressUntil[pAircraft] = Unsorted::CurrentFrame + 300;
		s_carrierReturnSpeed.erase(pAircraft);
		s_returnProgressRef.erase(pAircraft);
		s_returnStuckChecks.erase(pAircraft);

		Debug::CarrierLog("[AircraftCarrier] %s return cancelled, new order mission=%d\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(mission));
		return;
	}

	// Carrier return autopilot.
	// state 2 = return flight to a hold/approach point near the ship.
	// state 3 = final alignment directly above the parking slot.
	// state 4 = vertical descent onto the deck.
	//
	// The carrier must be stopped before states 3/4 are allowed. State 2 uses
	// the native Move mission so turning and speed stay vanilla. States 3/4
	// take over manually for the short final alignment and vertical descent.
	if (state == 2 || state == 3 || state == 4)
	{
		const auto slot = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
		const bool carrierStopped = IsCarrierStopped(pCarrier);
		const int flightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
		const int cruiseZ = MapClass::Instance.GetCellFloorHeight(slot) + flightLevel;
		// Speed-tag driven, scaled to a visible flight speed. A raw Speed=19
		// would otherwise crawl at roughly one cell per second.
		const int verticalStep = std::clamp(
			pAircraft->Type ? pAircraft->Type->Speed / 2 : 6, 3, 12);

		// Only the horizontal alignment radius is widened (10x). Height and
		// touchdown tolerances keep their original values.
		constexpr double kFinalAlignRadius = 128.0;
		constexpr int kFinalAlignHeight = 256;
		constexpr int kTouchdownHeight = 8;

		int timer = 0;

		if (index < pState->CarrierPayloadLandingTimer.size())
			timer = pState->CarrierPayloadLandingTimer[index];

		// ---- state 2: native return flight to a hold cell ----------------
		if (state == 2)
		{
			// The engine owns movement, turning and speed here. Do not call
			// SetLocation / Stop_Moving / change locomotor speeds.
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->InAir = true;
			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);

			const int sdx = slot.X - pAircraft->Location.X;
			const int sdy = slot.Y - pAircraft->Location.Y;
			const double horizontal = std::sqrt(
				static_cast<double>(sdx) * sdx + static_cast<double>(sdy) * sdy);

			if (carrierStopped && horizontal <= 1536.0)
			{
				state = 3;

				// Hand over to the native final approach. Clear the old Move
				// destination once; do not repeat this every frame, otherwise a
				// player order can be erased.
				if (pAircraft->Destination)
					pAircraft->SetDestination(nullptr, true);

				if (mission != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				if (index < pState->CarrierPayloadReturnCell.size())
					pState->CarrierPayloadReturnCell[index] = CellStruct { -1, -1 };

				if (index < pState->CarrierPayloadLastCoords.size())
					pState->CarrierPayloadLastCoords[index] = slot;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = 0;

				s_returnProgressRef.erase(pAircraft);
				s_returnStuckChecks.erase(pAircraft);

				Debug::CarrierLog("[AircraftCarrier] %s begin final approach slot %d h=%.0f loc=(%d,%d,%d)\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index),
					horizontal,
					pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);

				return;
			}

			// While the carrier is moving, hold in a real orbit around it instead
			// of flying to a fixed point and hovering. Once it stops, the normal
			// approach/landing code below takes over.
			if (!carrierStopped)
			{
				const auto center = pCarrier->GetCoords();
				const int odx = center.X - pAircraft->Location.X;
				const int ody = center.Y - pAircraft->Location.Y;
				const double centerDistance = std::sqrt(
					static_cast<double>(odx) * odx + static_cast<double>(ody) * ody);

				constexpr double kOrbitEnterRadius = 4096.0;
				constexpr double kOrbitRadius = 2048.0;

				if (centerDistance <= kOrbitEnterRadius)
				{
					if (pAircraft->Destination)
						pAircraft->SetDestination(nullptr, true);

					if (pAircraft->Locomotor)
						pAircraft->Locomotor->Stop_Moving();

					if (mission != Mission::Sleep)
						pAircraft->QueueMission(Mission::Sleep, true);

					auto current = pAircraft->Location;
					auto const speedIt = s_carrierReturnSpeed.find(pAircraft);
					const double speed = speedIt != s_carrierReturnSpeed.end()
						? speedIt->second
						: static_cast<double>(pAircraft->Type ? pAircraft->Type->Speed : 8);

					// Phase-shift each slot so the eight aircraft spread around the
					// circle instead of stacking on one point.
					const double phase = static_cast<double>(index)
						* (kPi * 2.0 / 8.0);
					const double rawAngle = centerDistance > 64.0
						? std::atan2(static_cast<double>(current.Y - center.Y),
							static_cast<double>(current.X - center.X))
						: 0.0;
					const double orbitSpeed = std::max(speed, 32.0);
					const double step = std::clamp(orbitSpeed / kOrbitRadius, 0.008, 0.03);
					const double angle = rawAngle + phase + step;

					CoordStruct target
					{
						center.X + static_cast<int>(std::lround(std::cos(angle) * kOrbitRadius)),
						center.Y + static_cast<int>(std::lround(std::sin(angle) * kOrbitRadius)),
						cruiseZ
					};

					MoveCarrierAutopilot(pAircraft, current, target, orbitSpeed,
						GetCarrierTurnStep(pAircraft), false);

					if (current.Z > cruiseZ)
						current.Z = std::max(cruiseZ, current.Z - verticalStep);
					else if (current.Z < cruiseZ)
						current.Z = std::min(cruiseZ, current.Z + verticalStep);

					if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
						pFly->FlightLevel = current.Z
							- MapClass::Instance.GetCellFloorHeight(current);

					pAircraft->SetLocation(current);

					if (index < pState->CarrierPayloadLandingTimer.size())
						pState->CarrierPayloadLandingTimer[index] = 0;

					if (Unsorted::CurrentFrame % 30 == 0)
					{
						Debug::CarrierLog("[AircraftCarrier] %s holding orbit center=(%d,%d) loc=(%d,%d,%d) r=%.0f\n",
							pAircraft->Type ? pAircraft->Type->ID : "?",
							center.X, center.Y,
							current.X, current.Y, current.Z, centerDistance);
					}

					return;
				}
			}

			// Standoff cell: 6 cells behind while the carrier moves, 3 behind
			// once it stops. Try shorter offsets if the cell is invalid/own hull.
			const auto slotCell = CellClass::Coord2Cell(slot);
			const int face = static_cast<int>(pCarrier->PrimaryFacing.Current().GetFacing<8>());
			const auto back = Unsorted::AdjacentCoord[(face + 4) & 7];
			const int desiredCells = carrierStopped ? 3 : 6;

			CellStruct targetCell { -1, -1 };

			for (int cells = desiredCells; cells >= 2; --cells)
			{
				const int offX = (back.X * cells) / Unsorted::LeptonsPerCell;
				const int offY = (back.Y * cells) / Unsorted::LeptonsPerCell;

				const CellStruct candidate
				{
					static_cast<short>(slotCell.X + offX),
					static_cast<short>(slotCell.Y + offY)
				};

				if (auto const pCell = MapClass::Instance.TryGetCellAt(candidate))
				{
					if (!IsOwnCell(pCarrier, pCell, pState->Alt))
					{
						targetCell = candidate;
						break;
					}
				}
			}

			if (targetCell.X < 0)
				targetCell = slotCell;

			if (index < pState->CarrierPayloadReturnCell.size())
				pState->CarrierPayloadReturnCell[index] = targetCell;

			auto const pTargetCell = MapClass::Instance.TryGetCellAt(targetCell);

			if (pTargetCell
				&& (pAircraft->Destination != pTargetCell || mission != Mission::Move))
			{
				pAircraft->SetDestination(pTargetCell, true);

				if (mission != Mission::Move)
					pAircraft->QueueMission(Mission::Move, true);

				Debug::CarrierLog("[AircraftCarrier] %s return move cell=(%d,%d) stopped=%d h=%.0f loc=(%d,%d,%d)\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", targetCell.X, targetCell.Y,
					carrierStopped ? 1 : 0, horizontal,
					pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
			}

			// Native move made no progress while the carrier is stopped: take
			// over manually instead of waiting forever.
			if (carrierStopped && Unsorted::CurrentFrame % 60 == 0)
			{
				auto const progressIt = s_returnProgressRef.find(pAircraft);

				if (progressIt == s_returnProgressRef.end())
				{
					s_returnProgressRef[pAircraft] = horizontal;
					s_returnStuckChecks[pAircraft] = 0;
				}
				else
				{
					// Progress means the distance to the pad is actually getting
					// smaller. An orbit moves a lot but never closes the distance,
					// so tracking movement alone would miss it.
					const double gained = progressIt->second - horizontal;

					if (gained < 64.0)
					{
						if (++s_returnStuckChecks[pAircraft] >= 3)
						{
							state = 3;

							if (pAircraft->Destination)
								pAircraft->SetDestination(nullptr, true);

							if (pAircraft->Locomotor)
								pAircraft->Locomotor->Stop_Moving();

							if (mission != Mission::Sleep)
								pAircraft->QueueMission(Mission::Sleep, true);

							if (index < pState->CarrierPayloadLandingTimer.size())
								pState->CarrierPayloadLandingTimer[index] = 0;

							s_returnProgressRef.erase(pAircraft);
							s_returnStuckChecks.erase(pAircraft);

							Debug::CarrierLog("[AircraftCarrier] %s native move stuck, switching to manual final\n",
								pAircraft->Type ? pAircraft->Type->ID : "?");

							return;
						}
					}
					else
					{
						s_returnStuckChecks[pAircraft] = 0;
						progressIt->second = horizontal;
					}
				}
			}

			return;
		}

		// ---- states 3/4/5: final approach and vertical descent -------------
		// Once the final approach starts, a transient carrierStopped flicker
		// must not send the aircraft back to the circling state. Only a real
		// slot displacement (>512) aborts; small hull jitter is followed.
		if (index < pState->CarrierPayloadLastCoords.size()
			&& pState->CarrierPayloadLastCoords[index].DistanceFrom(slot) > 128.0)
		{
			state = 2;
			timer = 0;

			if (index < pState->CarrierPayloadLandingTimer.size())
				pState->CarrierPayloadLandingTimer[index] = 0;

			if (index < pState->CarrierPayloadReturnCell.size())
				pState->CarrierPayloadReturnCell[index] = CellStruct { -1, -1 };

			s_returnProgressRef.erase(pAircraft);
			s_returnStuckChecks.erase(pAircraft);

			Debug::CarrierLog("[AircraftCarrier] %s carrier moved, aborting final approach\n",
				pAircraft->Type ? pAircraft->Type->ID : "?");

			return;
		}

		// ---- state 3: native Move_To final approach ------------------------
		if (state == 3)
		{
			const CoordStruct target { slot.X, slot.Y, cruiseZ };

			if (timer == 0)
			{
				// Entering the final approach. Clear the old Move order once.
				if (pAircraft->Destination)
					pAircraft->SetDestination(nullptr, true);

				if (pAircraft->Locomotor)
					pAircraft->Locomotor->Stop_Moving();

				if (mission != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->DockNowHeadingTo = nullptr;
				pAircraft->InAir = true;

				if (index < pState->CarrierPayloadLastCoords.size())
					pState->CarrierPayloadLastCoords[index] = slot;

				if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
				{
					pFly->IsLanding = false;
					pFly->IsTakingOff = false;
					pFly->IsElevating = false;
					pFly->FlightLevel = pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;

					if (pFly->CurrentSpeed < 1.0)
						pFly->CurrentSpeed = 1.0;

					if (pFly->TargetSpeed < 1.0)
						pFly->TargetSpeed = std::max(1.0,
							static_cast<double>(pAircraft->Type ? pAircraft->Type->Speed : 8));
				}

				if (pAircraft->Locomotor)
				{
					pAircraft->Locomotor->Move_To(target);

					if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
					{
						pFly->IsTakingOff = false;
						pFly->IsLanding = false;
						pFly->FlightLevel =
							pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
					}
				}

				const int entryDx = slot.X - pAircraft->Location.X;
				const int entryDy = slot.Y - pAircraft->Location.Y;
				s_returnProgressRef[pAircraft] = std::sqrt(
					static_cast<double>(entryDx) * entryDx
					+ static_cast<double>(entryDy) * entryDy);
				s_returnStuckChecks[pAircraft] = 0;

				timer = 1;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = timer;
			}

			auto const pLoco = pAircraft->Locomotor;
			const int dxNow = slot.X - pAircraft->Location.X;
			const int dyNow = slot.Y - pAircraft->Location.Y;
			const double horizontalNow = std::sqrt(
				static_cast<double>(dxNow) * dxNow + static_cast<double>(dyNow) * dyNow);

			if (pLoco && !pLoco->Is_Moving() && horizontalNow > kFinalAlignRadius)
			{
				pLoco->Move_To(target);

				if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pLoco))
				{
					pFly->IsTakingOff = false;
					pFly->IsLanding = false;
					pFly->FlightLevel =
						pAircraft->Type ? pAircraft->Type->GetFlightLevel() : 0;
				}
			}

			// If Move_To cannot close the distance (full-speed turn radius
			// larger than the target), switch to a one-way manual direct final.
			if (Unsorted::CurrentFrame % 60 == 0)
			{
				auto const progressIt = s_returnProgressRef.find(pAircraft);

				if (progressIt != s_returnProgressRef.end())
				{
					const double gained = progressIt->second - horizontalNow;

					if (gained < 64.0)
					{
						if (++s_returnStuckChecks[pAircraft] >= 2)
						{
							state = 5;
							timer = 0;

							if (pAircraft->Destination)
								pAircraft->SetDestination(nullptr, true);

							if (pLoco)
								pLoco->Stop_Moving();

							if (mission != Mission::Sleep)
								pAircraft->QueueMission(Mission::Sleep, true);

							if (index < pState->CarrierPayloadLandingTimer.size())
								pState->CarrierPayloadLandingTimer[index] = 0;

							s_returnProgressRef.erase(pAircraft);
							s_returnStuckChecks.erase(pAircraft);

							Debug::CarrierLog("[AircraftCarrier] %s Move_To not converging, manual direct final\n",
								pAircraft->Type ? pAircraft->Type->ID : "?");

							return;
						}
					}
					else
					{
						s_returnStuckChecks[pAircraft] = 0;
						progressIt->second = horizontalNow;
					}
				}
			}

			const int fdx = slot.X - pAircraft->Location.X;
			const int fdy = slot.Y - pAircraft->Location.Y;
			const double finalHorizontal = std::sqrt(
				static_cast<double>(fdx) * fdx + static_cast<double>(fdy) * fdy);
			const int finalHeight = std::abs(pAircraft->Location.Z - cruiseZ);

			if (finalHorizontal <= kFinalAlignRadius && finalHeight <= kFinalAlignHeight)
			{
				if (pAircraft->Destination)
					pAircraft->SetDestination(nullptr, true);

				if (pLoco)
					pLoco->Stop_Moving();

				if (mission != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				state = 4;
				timer = 0;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = 0;

				s_returnProgressRef.erase(pAircraft);
				s_returnStuckChecks.erase(pAircraft);

				Debug::CarrierLog("[AircraftCarrier] %s above slot %d, begin vertical descent z=%d deck=%d\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index),
					pAircraft->Location.Z, slot.Z);

				return;
			}

			++timer;

			if (index < pState->CarrierPayloadLandingTimer.size())
				pState->CarrierPayloadLandingTimer[index] = timer;

			if (Unsorted::CurrentFrame % 30 == 0)
			{
				Debug::CarrierLog("[AircraftCarrier] final state=3 h=%.0f dz=%d loc=(%d,%d,%d) slot=(%d,%d,%d) moving=%d\n",
					finalHorizontal, pAircraft->Location.Z - cruiseZ,
					pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
					slot.X, slot.Y, slot.Z,
					(pLoco && pLoco->Is_Moving()) ? 1 : 0);
			}

			return;
		}

		// ---- state 5: manual direct final (never circles) ------------------
		if (state == 5)
		{
			if (timer == 0)
			{
				if (pAircraft->Destination)
					pAircraft->SetDestination(nullptr, true);

				if (pAircraft->Locomotor)
					pAircraft->Locomotor->Stop_Moving();

				if (mission != Mission::Sleep)
					pAircraft->QueueMission(Mission::Sleep, true);

				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->DockNowHeadingTo = nullptr;
				pAircraft->InAir = true;

				timer = 1;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = timer;
			}

			auto current = pAircraft->Location;
			auto const speedIt = s_carrierReturnSpeed.find(pAircraft);
			const double speed = speedIt != s_carrierReturnSpeed.end()
				? speedIt->second
				: static_cast<double>(pAircraft->Type ? pAircraft->Type->Speed : 8);

			MoveCarrierAutopilot(pAircraft, current, slot, speed,
				GetCarrierTurnStep(pAircraft), true);

			if (current.Z > cruiseZ)
				current.Z = std::max(cruiseZ, current.Z - verticalStep);
			else if (current.Z < cruiseZ)
				current.Z = std::min(cruiseZ, current.Z + verticalStep);

			if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
				pFly->FlightLevel = current.Z
					- MapClass::Instance.GetCellFloorHeight(current);

			pAircraft->PrimaryFacing.SetDesired(GetCarrierSlotFacing(pCarrier, pTypeExt, index));
			pAircraft->SetLocation(current);

			const int fdx = slot.X - current.X;
			const int fdy = slot.Y - current.Y;
			const double finalHorizontal = std::sqrt(
				static_cast<double>(fdx) * fdx + static_cast<double>(fdy) * fdy);
			const int finalHeight = std::abs(current.Z - cruiseZ);

			if (finalHorizontal <= kFinalAlignRadius && finalHeight <= kFinalAlignHeight)
			{
				state = 4;
				timer = 0;

				if (index < pState->CarrierPayloadLandingTimer.size())
					pState->CarrierPayloadLandingTimer[index] = 0;

				Debug::CarrierLog("[AircraftCarrier] %s above slot %d, begin vertical descent z=%d deck=%d\n",
					pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index),
					current.Z, slot.Z);

				return;
			}

			++timer;

			if (index < pState->CarrierPayloadLandingTimer.size())
				pState->CarrierPayloadLandingTimer[index] = timer;

			return;
		}

		// ---- state 4: hold XY, descend vertically --------------------------
		if (timer == 0)
		{
			if (pAircraft->Destination)
				pAircraft->SetDestination(nullptr, true);

			if (pAircraft->Locomotor)
				pAircraft->Locomotor->Stop_Moving();

			if (mission != Mission::Sleep)
				pAircraft->QueueMission(Mission::Sleep, true);

			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->InAir = true;

			timer = 1;

			if (index < pState->CarrierPayloadLandingTimer.size())
				pState->CarrierPayloadLandingTimer[index] = timer;
		}

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
		{
			pFly->IsLanding = false;
			pFly->IsTakingOff = false;
			pFly->IsElevating = false;
		}

		auto current = pAircraft->Location;
		current.X = slot.X;
		current.Y = slot.Y;

		if (current.Z > slot.Z)
			current.Z = std::max(slot.Z, current.Z - verticalStep);
		else if (current.Z < slot.Z)
			current.Z = std::min(slot.Z, current.Z + verticalStep);

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
			pFly->FlightLevel = current.Z
				- MapClass::Instance.GetCellFloorHeight(current);

		pAircraft->PrimaryFacing.SetDesired(GetCarrierSlotFacing(pCarrier, pTypeExt, index));
		pAircraft->SetLocation(current);

		++timer;

		if (index < pState->CarrierPayloadLandingTimer.size())
			pState->CarrierPayloadLandingTimer[index] = timer;

		if (std::abs(current.Z - slot.Z) <= kTouchdownHeight || timer > 1800)
		{
			finishLanding(slot);
			return;
		}

		return;

	}
	if (state == 0)
	{
		const bool outOfAmmo = !TechnoExt::HasAnyWeaponAmmo(pAircraft);

		// The Stop hotkey puts a selected aircraft into Guard. A scripted or
		// AI Guard must not send the aircraft home.
		const bool stopped = mission == Mission::Guard && pAircraft->IsSelected;
		const bool manualReturn = mission == Mission::Enter
			|| mission == Mission::Return
			|| mission == Mission::Stop
			|| stopped;

		auto const suppressIt = s_returnSuppressUntil.find(pAircraft);
		const bool suppressed = suppressIt != s_returnSuppressUntil.end()
			&& Unsorted::CurrentFrame < suppressIt->second;

		// Manual Enter/Stop always wins. Auto-return after ammo depletion waits
		// for the cancel cooldown so a player order is not immediately
		// overwritten.
		// A player-issued Move/Attack that is still running outranks the
		// automatic out-of-ammo return.
		const bool playerOrderActive = pAircraft->IsSelected
			&& (mission == Mission::Move || mission == Mission::QMove
				|| mission == Mission::Attack || mission == Mission::AttackMove)
			&& pAircraft->Destination != nullptr;

		const bool wantsReturn = manualReturn
			|| (outOfAmmo && !suppressed && !playerOrderActive);

		if (!wantsReturn)
			return;

		// Never change the aircraft state from inside its own Update. Queue a
		// deferred intent; ProcessCarrierIntents handles it before the next
		// object AI tick.
		if (index < pState->CarrierPayloadReturnCell.size())
			pState->CarrierPayloadReturnCell[index] = CellStruct { -1, -1 };

		Footprint::QueueCarrierLandingIntent(pAircraft, pCarrier, -1, false,
			!manualReturn);

		// Suppress the engine's Enter/Return handling for this frame. The
		// intent owns the transition on the next global tick.
		pAircraft->DockNowHeadingTo = nullptr;
		pAircraft->SetDestination(nullptr, true);
		pAircraft->SetTarget(nullptr);
		pAircraft->SetArchiveTarget(nullptr);
		pAircraft->InAir = true;
		pAircraft->QueueMission(Mission::Sleep, true);

		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
		{
			pFly->IsLanding = false;
			pFly->IsTakingOff = false;
			pFly->IsElevating = false;
		}

		if (index < pState->CarrierPayloadLandingTimer.size())
			pState->CarrierPayloadLandingTimer[index] = 0;

		Debug::CarrierLog("[AircraftCarrier] %s return intent queued (mission=%d ammo=%d hasAmmo=%d auto=%d) loc=(%d,%d,%d)\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(mission),
			pAircraft->Ammo, outOfAmmo ? 0 : 1, manualReturn ? 0 : 1,
			pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);
	}
}

bool Footprint::Mark(UnitClass* pThis, const CoordStruct& coords)
{
	if (!pThis || !IsMultiCell(pThis))
		return false;

	if (s_footprintMarkDepth > 0)
	{
		Debug::CarrierLog("[FootprintGuard] reentrant Mark for %p\n",
			static_cast<void*>(pThis));
		return true;
	}

	FootprintMarkGuard markGuard;

	const auto pState = GetState(pThis);

	if (!pState)
		return false;

	// Pre-placed map units may never enter the normal Unlimbo/Process path.
	// Mark() is guaranteed for every footprint unit, so initialize the
	// SpawnManager and its payload nodes here as well.
	EnsureAircraftCarrierSpawns(pThis);

	// While the turn controller owns the hull, or the locomotor is between
	// cell centres, the engine's mid-rotation/mid-move Mark calls must not
	// retarget the footprint to the cell currently under the model.
	if (pState->Locked || pState->Translating)
		return true;

	const bool wasInitialized = pState->Initialized;
	const auto anchor = AnchorFromCoords(coords);
	int bucket = wasInitialized
		? pState->Bucket
		: BucketFromFacing(pThis->PrimaryFacing.Current());
	auto span = GetSpan(pThis, anchor, bucket);
	const auto pAnchorCell = MapClass::Instance.TryGetCellAt(anchor);

	if (!pAnchorCell)
		return false;

	const int height = MapClass::Instance.GetCellFloorHeight(coords) + CellClass::BridgeHeight;
	const bool alt = (coords.Z >= height && pAnchorCell->ContainsBridge());

	// A map-placed 1x1 ship may not have room for a 3-cell hull in its spawn
	// orientation. Try every facing before giving up; this keeps campaign
	// placements usable without editing every map.
	if (!wasInitialized)
	{
		CellStruct blocked { 0, 0 };

		if (!CanOccupy(pThis, span, alt, anchor, &blocked))
		{
			bool found = false;

			for (int candidate = 0; candidate < 8; ++candidate)
			{
				if (candidate == bucket)
					continue;

				auto candidateSpan = GetSpan(pThis, anchor, candidate);

				if (CanOccupy(pThis, candidateSpan, alt, anchor, &blocked))
				{
					bucket = candidate;
					span = std::move(candidateSpan);
					found = true;
					break;
				}
			}

			if (found)
			{
				pThis->PrimaryFacing.SetCurrent(DirStruct(bucket * 0x2000));

				Debug::CarrierLog("[Footprint] spawn reorient %s anchor=(%d,%d) bucket=%d\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
			}
			else
			{
				Debug::Log("[Footprint] spawn invalid %s anchor=(%d,%d): no facing fits, blocked=(%d,%d)\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, blocked.X, blocked.Y);
			}
		}
	}

	const auto barriers = ComputeBarriers(span);

	if (pState->Initialized
		&& pState->Anchor == anchor
		&& pState->Bucket == bucket
		&& pState->Alt == alt
		&& pState->Span == span
		&& pState->BarrierSpan == barriers)
	{
		return true;
	}

	// Check the complete new hull and barrier set before clearing the old
	// footprint. Two hulls moving side by side must never overwrite each
	// other's cells halfway through a Mark.
	if (HasForeignOwner(pThis, span, alt) || HasForeignOwner(pThis, barriers, alt))
	{
		Debug::CarrierLog("[Footprint] mark blocked %s anchor=(%d,%d) bucket=%d by another hull/barrier\n",
			pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
		return false;
	}

	if (wasInitialized)
		Unmark(pThis);

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetOwner(cell, alt, pThis);
	}

	for (const auto& cell : barriers)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		// Never overwrite another hull or another unit's barrier.
		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
			continue;

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
	}

	pState->Initialized = true;
	pState->Anchor = anchor;
	pState->Bucket = bucket;
	pState->Alt = alt;
	pState->Span = span;
	pState->BarrierSpan = barriers;

	Debug::CarrierLog("[Footprint] span %s anchor=(%d,%d) bucket=%d cells=%d barriers=%d alt=%d%s\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
		static_cast<int>(span.size()), static_cast<int>(barriers.size()), alt ? 1 : 0,
		wasInitialized ? "" : " (initial)");

	return true;
}

bool Footprint::Unmark(UnitClass* pThis, const CoordStruct* pCoords)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState || !pState->Initialized)
		return false;

	// A locked/translating hull is mid-turn or between cell centres; the
	// controller commits the final footprint when it reaches the cell centre.
	if (pState->Locked || pState->Translating)
		return true;

	if (!pState->ReservedSpan.empty())
		ReleaseReservation(pThis);

	// The engine routinely unmarks and re-marks a stationary unit every frame.
	// Clearing the footprint here would make the hull and its barriers flicker,
	// so other units would plan through a momentarily free cell and then stall.
	// Only a real relocation (different anchor) or Limbo/Release force-clears.
	if (pCoords && pThis->IsAlive && !pThis->InLimbo)
	{
		const auto coordsAnchor = AnchorFromCoords(*pCoords);

		if (pState->Anchor == coordsAnchor)
			return true;
	}

	auto span = pState->Span;

	if (span.empty())
		span = GetSpan(pThis, pState->Anchor, pState->Bucket);

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (OwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearOwner(cell, false, pThis);
		}

		if (OwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearOwner(cell, true, pThis);
		}

		if (const auto pCellExt = CellExt::ExtMap.TryFind(pCell))
		{
			if (pCellExt->IncomingUnit == pThis)
				pCellExt->IncomingUnit = nullptr;

			if (pCellExt->IncomingUnitAlt == pThis)
				pCellExt->IncomingUnitAlt = nullptr;
		}
	}

	for (const auto& cell : pState->BarrierSpan)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (BarrierOwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, false, pThis);
		}

		if (BarrierOwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, true, pThis);
		}
	}

	pState->Initialized = false;
	pState->Translating = false;
	pState->Span.clear();
	pState->BarrierSpan.clear();
	pState->ReconcileActive = false;
	pState->ReconcileTarget = -1;

	return true;
}

bool Footprint::CommitSpan(UnitClass* pThis, const std::vector<CellStruct>& span, int bucket, bool alt, const CellStruct& anchor)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState)
		return false;

	const auto& oldSpan = pState->Span;
	const auto& oldBarriers = pState->BarrierSpan;
	const auto newBarriers = ComputeBarriers(span);

	// Validate the whole target footprint first. This prevents two units from
	// interleaving their clears/sets and stealing each other's ownership.
	if (HasForeignOwner(pThis, span, alt) || HasForeignOwner(pThis, newBarriers, alt))
	{
		Debug::CarrierLog("[Footprint] commit blocked %s anchor=(%d,%d) bucket=%d by another hull/barrier\n",
			pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
		return false;
	}

	for (const auto& cell : oldSpan)
	{
		if (std::find(span.begin(), span.end(), cell) != span.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (OwnerAt(cell, false) == pThis)
			{
				pCell->OccupationFlags &= ~0x20;
				ClearOwner(cell, false, pThis);
			}

			if (OwnerAt(cell, true) == pThis)
			{
				pCell->AltOccupationFlags &= ~0x20;
				ClearOwner(cell, true, pThis);
			}
		}
	}

	for (const auto& cell : oldBarriers)
	{
		if (std::find(newBarriers.begin(), newBarriers.end(), cell) != newBarriers.end())
			continue;

		// A cell that is part of the active turn reservation must stay blocked
		// until the reservation is released, even if it is no longer a gap
		// barrier of the current hull bucket.
		if (std::find(pState->ReservedSpan.begin(), pState->ReservedSpan.end(), cell) != pState->ReservedSpan.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (BarrierOwnerAt(cell, false) == pThis)
			{
				pCell->OccupationFlags &= ~0x20;
				ClearBarrierOwner(cell, false, pThis);
			}

			if (BarrierOwnerAt(cell, true) == pThis)
			{
				pCell->AltOccupationFlags &= ~0x20;
				ClearBarrierOwner(cell, true, pThis);
			}
		}
	}

	for (const auto& cell : span)
	{
		if (std::find(oldSpan.begin(), oldSpan.end(), cell) != oldSpan.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (alt)
				pCell->AltOccupationFlags |= 0x20;
			else
				pCell->OccupationFlags |= 0x20;

			SetOwner(cell, alt, pThis);
		}
	}

	for (const auto& cell : newBarriers)
	{
		if (std::find(oldBarriers.begin(), oldBarriers.end(), cell) != oldBarriers.end())
			continue;

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
			continue;

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
	}

	pState->Initialized = true;
	pState->Bucket = bucket;
	pState->Anchor = anchor;
	pState->Alt = alt;
	pState->Span = span;
	pState->BarrierSpan = newBarriers;

	return true;
}

bool Footprint::ReserveSweep(UnitClass* pThis, const std::vector<CellStruct>& sweep, bool alt)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState)
		return false;

	std::vector<CellStruct> added;
	added.reserve(sweep.size());

	auto rollback = [&]()
	{
		for (const auto& cell : added)
		{
			const auto pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!pCell)
				continue;

			if (BarrierOwnerAt(cell, alt) == pThis)
			{
				if (alt)
					pCell->AltOccupationFlags &= ~0x20;
				else
					pCell->OccupationFlags &= ~0x20;

				ClearBarrierOwner(cell, alt, pThis);
			}
		}

		pState->ReservedSpan.erase(
			std::remove_if(pState->ReservedSpan.begin(), pState->ReservedSpan.end(),
				[&added](const CellStruct& cell)
				{
					return std::find(added.begin(), added.end(), cell) != added.end();
				}),
			pState->ReservedSpan.end());
	};

	for (const auto& cell : sweep)
	{
		if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
			continue;

		if (std::find(pState->BarrierSpan.begin(), pState->BarrierSpan.end(), cell) != pState->BarrierSpan.end())
			continue;

		if (std::find(pState->ReservedSpan.begin(), pState->ReservedSpan.end(), cell) != pState->ReservedSpan.end())
			continue;

		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
		{
			rollback();
			return false;
		}

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
		{
			rollback();
			return false;
		}

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
		{
			rollback();
			return false;
		}

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
		pState->ReservedSpan.push_back(cell);
		added.push_back(cell);
	}

	return true;
}

void Footprint::ReleaseReservation(UnitClass* pThis)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState || pState->ReservedSpan.empty())
		return;

	for (const auto& cell : pState->ReservedSpan)
	{
		const bool inHull = std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end();
		const bool inBarrier = std::find(pState->BarrierSpan.begin(), pState->BarrierSpan.end(), cell) != pState->BarrierSpan.end();

		if (inHull || inBarrier)
		{
			// The hull keeps its own 0x20; a reservation marker on a hull cell is
			// redundant and should not linger as a barrier owner.
			if (inHull && !inBarrier)
				ClearBarrierOwner(cell, pState->Alt, pThis);

			continue;
		}

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (BarrierOwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, false, pThis);
		}

		if (BarrierOwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, true, pThis);
		}
	}

	pState->ReservedSpan.clear();
}

void Footprint::Release(UnitClass* pThis)
{
	if (auto const pState = GetState(pThis))
		pState->Locked = false;

	ReleaseReservation(pThis);
	Unmark(pThis);
}

void Footprint::ClearAll()
{
	// These caches are keyed by live object pointers. A load inside the same
	// process or a scenario restart must not leave entries from the previous
	// game behind; a reused address would otherwise point at unrelated state.
	s_pendingKills.clear();
	s_carrierMoveRef.clear();
	s_carrierLastMoveFrame.clear();
	s_carrierLastFacing.clear();
	s_returnSuppressUntil.clear();
	s_carrierReturnSpeed.clear();
	s_returnProgressRef.clear();
	s_returnStuckChecks.clear();
	s_carrierSinkStartFrame.clear();
	s_carrierSinkAccum.clear();
	s_runwayLoggedCarriers.clear();
	s_landingRunwayIndex.clear();
	s_landingWatch.clear();
	s_taxiWatch.clear();
	s_pendingCarrierLanding.clear();
	s_landingRunwayExplicit.clear();
	s_carrierLandingIntents.clear();
	s_carrierLandingIntentAircraft.clear();
	s_carrierTakeoffIntents.clear();
	s_carrierTakeoffIntentAircraft.clear();
	s_carrierTakeoffHandoffs.clear();

	EnsureGrid();
	std::fill(s_ownerNormal.begin(), s_ownerNormal.end(), nullptr);
	std::fill(s_ownerAlt.begin(), s_ownerAlt.end(), nullptr);
	std::fill(s_barrierNormal.begin(), s_barrierNormal.end(), nullptr);
	std::fill(s_barrierAlt.begin(), s_barrierAlt.end(), nullptr);

	// Scenario start can run after map units have already been marked. Clearing
	// only the owner grid would leave their 0x20 flags orphaned and make the
	// hull reject its own bow/stern cells. Rebuild from the live units instead.
	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !IsMultiCell(pUnit))
			continue;

		const auto pState = GetState(pUnit);

		if (pState && pState->Initialized && !pState->Span.empty())
		{
			pState->Locked = false;
			pState->Translating = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			if (pState->BarrierSpan.empty())
				pState->BarrierSpan = ComputeBarriers(pState->Span);

			for (const auto& cell : pState->Span)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetOwner(cell, pState->Alt, pUnit);
			}

			for (const auto& cell : pState->BarrierSpan)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (const auto pOwner = OwnerAt(cell, pState->Alt); pOwner && pOwner != pUnit)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetBarrierOwner(cell, pState->Alt, pUnit);
			}

			for (const auto& cell : pState->ReservedSpan)
			{
				if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
					continue;

				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (const auto pOwner = OwnerAt(cell, pState->Alt); pOwner && pOwner != pUnit)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetBarrierOwner(cell, pState->Alt, pUnit);
			}
		}
		else
		{
			Mark(pUnit, pUnit->Location);
		}
	}

	// The payload lookup is a cache, not authoritative state: rebuild it from
	// the live carrier states so it can never keep pointers from a previous
	// scenario or a stale save.
	s_carrierPayloads.clear();

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !IsMultiCell(pUnit))
			continue;

		if (auto const pState = GetState(pUnit); pState && pState->CarrierPayloadsInitialized)
		{
			for (auto const pAircraft : pState->CarrierPayloads)
			{
				if (pAircraft)
					s_carrierPayloads[pAircraft] = pUnit;
			}
		}
	}
}

void Footprint::FixupAfterLoad()
{
	ClearAll();

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !IsMultiCell(pUnit))
			continue;

		EnsureLocomotor(pUnit);

		// Turn reservations are transient. A save made mid-turn restores the
		// cells it blocked, so release them explicitly before rebuilding the
		// hull; otherwise they would stay occupied until the unit moves again.
		if (auto const pState = GetState(pUnit))
		{
			pState->Locked = false;
			pState->Translating = false;
			ReleaseReservation(pUnit);
		}

		Mark(pUnit, pUnit->Location);
	}
}

void Footprint::EnsureLocomotor(UnitClass* pThis)
{
	if (!pThis || !IsMultiCell(pThis))
		return;

	if (locomotion_cast<FootprintLocomotionClass*>(pThis->Locomotor))
		return;

	auto pNewLoco = LocomotionClass::CreateInstance(__uuidof(FootprintLocomotionClass));

	if (!pNewLoco)
	{
		Debug::Log("[Footprint] failed to create locomotor for %s\n", pThis->GetTechnoType()->ID);
		return;
	}

	pNewLoco->Link_To_Object(pThis);
	pThis->Locomotor = pNewLoco;

	Debug::CarrierLog("[Footprint] replaced locomotor for %s\n", pThis->GetTechnoType()->ID);
}

int Footprint::GetYSort(TechnoClass* pThis)
{
	int base = pThis->ObjectClass::GetYSort();

	if (!IsMultiCell(pThis))
		return base;

	const auto pState = GetState(pThis);

	if (!pState || !pState->Initialized || pState->Span.empty())
		return base;

	short maxY = pState->Anchor.Y;

	for (const auto& offset : pState->Span)
		maxY = std::max<short>(maxY, offset.Y);

	return base + (maxY - pState->Anchor.Y) * Unsorted::LeptonsPerCell;
}

void Footprint::ReconcileFacing(UnitClass* pThis)
{
	if (!pThis || !IsMultiCell(pThis))
		return;

	const auto pState = GetState(pThis);

	if (!pState || !pState->Initialized)
		return;

	// The locomotor owns the hull while it is moving or executing its own turn.
	if (pThis->Locomotor && pThis->Locomotor->Is_Moving_Now())
		return;

	if (pState->Locked && !pState->ReconcileActive)
		return;

	const int currentBucket = BucketFromFacing(pThis->PrimaryFacing.Current());

	if (pState->ReconcileActive)
	{
		if (currentBucket != pState->Bucket)
		{
			const auto currentHull = GetSpan(pThis, pState->Anchor, currentBucket);
			CommitSpan(pThis, currentHull, currentBucket, pState->Alt, pState->Anchor);

			if (currentBucket != pState->ReconcileTarget)
			{
				if (!ReserveTurnStep(pThis, pState->Anchor, currentBucket,
					pState->ReconcileTarget, pState->ReconcileClockwise, pState->Alt))
				{
					ReleaseReservation(pThis);
					pState->Locked = false;
					pState->ReconcileActive = false;
					pState->ReconcileTarget = -1;
					pThis->PrimaryFacing.SetCurrent(DirStruct(currentBucket * 0x2000));

					Debug::CarrierLog("[FootprintAim] %s blocked at bucket %d, aborted at (%d,%d)\n",
						pThis->GetTechnoType()->ID, currentBucket,
						pState->Anchor.X, pState->Anchor.Y);
					return;
				}
			}
		}

		if (currentBucket == pState->ReconcileTarget)
		{
			const auto targetSpan = GetSpan(pThis, pState->Anchor, pState->ReconcileTarget);
			CommitSpan(pThis, targetSpan, pState->ReconcileTarget, pState->Alt, pState->Anchor);
			ReleaseReservation(pThis);
			pState->Locked = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			Debug::CarrierLog("[FootprintAim] %s finished bucket %d at (%d,%d)\n",
				pThis->GetTechnoType()->ID, currentBucket,
				pState->Anchor.X, pState->Anchor.Y);
		}
		else if (!pThis->PrimaryFacing.IsRotating())
		{
			// The desired direction changed (or the rotation was interrupted)
			// and the old target will never be reached. Commit whatever bucket
			// the hull actually reached and release the sweep.
			const auto currentSpan = GetSpan(pThis, pState->Anchor, currentBucket);
			CommitSpan(pThis, currentSpan, currentBucket, pState->Alt, pState->Anchor);
			ReleaseReservation(pThis);
			pState->Locked = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			Debug::CarrierLog("[FootprintAim] %s interrupted, committed bucket %d at (%d,%d)\n",
				pThis->GetTechnoType()->ID, currentBucket,
				pState->Anchor.X, pState->Anchor.Y);
		}

		return;
	}

	if (currentBucket == pState->Bucket)
		return;

	const int from = pState->Bucket;
	const int desiredBucket = BucketFromFacing(pThis->PrimaryFacing.Desired());
	int target = desiredBucket != from ? desiredBucket : currentBucket;

	if (target == from)
		return;

	const int delta = (target - from + 8) & 7;
	const bool tryClockwise = delta <= 4;
	const bool tryCounterClockwise = delta >= 4;

	auto tryDirection = [&](bool clockwise) -> bool
	{
		if (!ReserveTurnStep(pThis, pState->Anchor, from, target,
			clockwise, pState->Alt))
		{
			return false;
		}

		pState->Locked = true;
		pState->ReconcileActive = true;
		pState->ReconcileTarget = target;
		pState->ReconcileClockwise = clockwise;

		Debug::CarrierLog("[FootprintAim] %s bucket %d -> %d (%s) at (%d,%d), reserved=%d\n",
			pThis->GetTechnoType()->ID, from, target,
			clockwise ? "cw" : "ccw",
			pState->Anchor.X, pState->Anchor.Y,
			static_cast<int>(pState->ReservedSpan.size()));

		return true;
	};

	if (tryClockwise && tryDirection(true))
		return;

	if (tryCounterClockwise && tryDirection(false))
		return;

	// The hull cannot sweep to the aim direction. Keep the visual hull aligned
	// with the collision footprint instead of showing a turn that never happens.
	pThis->PrimaryFacing.SetCurrent(DirStruct(from * 0x2000));

	static int lastLogFrame = -1000;

	if (Unsorted::CurrentFrame - lastLogFrame >= 30)
	{
		lastLogFrame = Unsorted::CurrentFrame;

		Debug::CarrierLog("[FootprintAim] %s blocked %d -> %d at (%d,%d), snapped back\n",
			pThis->GetTechnoType()->ID, from, target,
			pState->Anchor.X, pState->Anchor.Y);
	}
}

void Footprint::DumpCarrierState()
{
	Debug::Log("[CarrierDump] begin: units=%d payloadMap=%d\n",
		static_cast<int>(UnitClass::Array.Count),
		static_cast<int>(s_carrierPayloads.size()));

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || !IsMultiCell(pUnit))
			continue;

		const auto pType = pUnit->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
		const auto pState = GetState(pUnit);

		if (!pState)
			continue;

		Debug::Log("[CarrierDump] unit=%s ptr=%p loc=(%d,%d,%d) cell=(%d,%d)"
			" init=%d locked=%d translating=%d bucket=%d alt=%d"
			" span=%d barrier=%d reserved=%d reconcile=%d/%d\n",
			pType->ID, static_cast<void*>(pUnit),
			pUnit->Location.X, pUnit->Location.Y, pUnit->Location.Z,
			pUnit->GetMapCoords().X, pUnit->GetMapCoords().Y,
			pState->Initialized ? 1 : 0, pState->Locked ? 1 : 0,
			pState->Translating ? 1 : 0, pState->Bucket, pState->Alt ? 1 : 0,
			static_cast<int>(pState->Span.size()),
			static_cast<int>(pState->BarrierSpan.size()),
			static_cast<int>(pState->ReservedSpan.size()),
			pState->ReconcileActive ? 1 : 0, pState->ReconcileTarget);

		if (!TechnoTypeExt::IsAircraftCarrier(pType) || !pTypeExt)
			continue;

		for (size_t i = 0; i < pState->CarrierDockBuildings.size(); ++i)
		{
			auto const pDock = pState->CarrierDockBuildings[i];

			if (!pDock)
			{
				Debug::Log("[CarrierDump]   slot %d dock=<null>\n", static_cast<int>(i));
				continue;
			}

			const auto dockCoords = pDock->GetDockCoords(nullptr);
			Debug::Log("[CarrierDump]   slot %d dock=%p type=%s alive=%d limbo=%d loc=(%d,%d,%d)"
				" linkCapacity=%d dockCoords=(%d,%d,%d)\n",
				static_cast<int>(i), static_cast<void*>(pDock),
				pDock->Type ? pDock->Type->ID : "?",
				pDock->IsAlive ? 1 : 0, pDock->InLimbo ? 1 : 0,
				pDock->Location.X, pDock->Location.Y, pDock->Location.Z,
				static_cast<int>(pDock->RadioLinks.Capacity),
				dockCoords.X, dockCoords.Y, dockCoords.Z);
		}

		for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft)
			{
				Debug::Log("[CarrierDump]   payload %d = <null>\n", static_cast<int>(i));
				continue;
			}

			const auto expected = GetCarrierDockSlotCoords(pUnit, pTypeExt, i);
			Debug::Log("[CarrierDump]   payload %d ptr=%p type=%s alive=%d limbo=%d mission=%d"
				" inAir=%d ammo=%d health=%d loc=(%d,%d,%d) expected=(%d,%d,%d)"
				" facing=%u expectedFacing=%u pushed=%d parked=%d\n",
				static_cast<int>(i), static_cast<void*>(pAircraft),
				pAircraft->Type ? pAircraft->Type->ID : "?",
				pAircraft->IsAlive ? 1 : 0, pAircraft->InLimbo ? 1 : 0,
				static_cast<int>(pAircraft->GetCurrentMission()),
				pAircraft->IsInAir() ? 1 : 0, pAircraft->Ammo,
				pAircraft->Health,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				expected.X, expected.Y, expected.Z,
				static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
				static_cast<unsigned>(GetCarrierSlotFacing(pUnit, pTypeExt, i).Raw),
				(pAircraft->Destination ? 1 : 0),
				IsCarrierPayloadParked(pAircraft) ? 1 : 0);
		}
	}

	Debug::Log("[CarrierDump] end\n");
}

void Footprint::OnPointerInvalidated(void* ptr)
{
	if (!ptr)
		return;

	s_returnSuppressUntil.erase(static_cast<TechnoClass*>(ptr));
	s_carrierReturnSpeed.erase(static_cast<AircraftClass*>(ptr));
	s_returnProgressRef.erase(static_cast<AircraftClass*>(ptr));
	s_returnStuckChecks.erase(static_cast<AircraftClass*>(ptr));
	s_pendingKills.erase(static_cast<AircraftClass*>(ptr));
	s_landingRunwayIndex.erase(static_cast<AircraftClass*>(ptr));
	s_landingWatch.erase(static_cast<AircraftClass*>(ptr));
	s_taxiWatch.erase(static_cast<AircraftClass*>(ptr));
	s_pendingCarrierLanding.erase(static_cast<AircraftClass*>(ptr));
	s_landingRunwayExplicit.erase(static_cast<AircraftClass*>(ptr));
	s_carrierLandingIntentAircraft.erase(static_cast<AircraftClass*>(ptr));
	s_carrierTakeoffIntentAircraft.erase(static_cast<AircraftClass*>(ptr));

	s_carrierLandingIntents.erase(std::remove_if(s_carrierLandingIntents.begin(),
		s_carrierLandingIntents.end(),
		[ptr](const CarrierLandingIntent& intent)
		{
			return intent.Aircraft == ptr || intent.Carrier == ptr;
		}), s_carrierLandingIntents.end());

	s_carrierTakeoffIntents.erase(std::remove_if(s_carrierTakeoffIntents.begin(),
		s_carrierTakeoffIntents.end(),
		[ptr](const CarrierTakeoffIntent& intent)
		{
			return intent.Aircraft == ptr || intent.Carrier == ptr;
		}), s_carrierTakeoffIntents.end());

	s_carrierTakeoffHandoffs.erase(std::remove_if(s_carrierTakeoffHandoffs.begin(),
		s_carrierTakeoffHandoffs.end(),
		[ptr](const CarrierTakeoffHandoff& handoff)
		{
			return handoff.Aircraft == ptr || handoff.Carrier == ptr;
		}), s_carrierTakeoffHandoffs.end());

	s_carrierMoveRef.erase(static_cast<UnitClass*>(ptr));
	s_carrierLastMoveFrame.erase(static_cast<UnitClass*>(ptr));
	s_carrierLastFacing.erase(static_cast<UnitClass*>(ptr));
	s_carrierSinkStartFrame.erase(static_cast<UnitClass*>(ptr));
	s_carrierSinkAccum.erase(static_cast<UnitClass*>(ptr));
	s_runwayLoggedCarriers.erase(static_cast<UnitClass*>(ptr));

	for (auto it = s_carrierPayloads.begin(); it != s_carrierPayloads.end(); )
	{
		if (it->first == ptr || it->second == ptr)
			it = s_carrierPayloads.erase(it);
		else
			++it;
	}

}

void Footprint::BeginCarrierSinkDestruction(UnitClass* pThis)
{
	if (!pThis || s_carrierSinkStartFrame.find(pThis) != s_carrierSinkStartFrame.end())
		return;

	const auto pType = pThis->GetTechnoType();

	if (!TechnoTypeExt::IsAircraftCarrier(pType))
		return;

	s_carrierSinkStartFrame[pThis] = Unsorted::CurrentFrame;

	// One central explosion, not a per-cell building-style blast. The last
	// non-null Explosion entry is the big finish anim in this mod's list.
	if (pType)
	{
		AnimTypeClass* pBig = nullptr;

		for (int i = 0; i < pType->Explosion.Count; ++i)
		{
			if (pType->Explosion[i])
				pBig = pType->Explosion[i];
		}

		if (pBig)
		{
			auto center = pThis->GetCoords();
			center.Z = pThis->GetRenderCoords().Z + 128;
			AnimExt::CreateRandomAnim({ pBig }, center, pThis, pThis->Owner);
		}
	}

	if (auto const pState = GetState(pThis))
	{
		// Secondary blasts: sample ~20% of the hull cells, one anim per cell,
		// with a hard cap. This is much lighter than the old per-cell list and
		// makes the explosion visible across a hull this large.
		if (pType)
		{
			std::vector<AnimTypeClass*> anims;

			for (int i = 0; i < pType->Explosion.Count; ++i)
			{
				if (pType->Explosion[i])
					anims.push_back(pType->Explosion[i]);
			}

			if (!anims.empty())
			{
				auto cells = pState->Span;

				if (cells.empty())
					cells = GetSpan(pThis, pState->Anchor, pState->Bucket);

				constexpr int kBlastCellChance = 20;
				constexpr int kMaxBlastAnims = 64;
				int spawned = 0;

				for (auto const& cell : cells)
				{
					if (spawned >= kMaxBlastAnims)
						break;

					if (ScenarioClass::Instance->Random.RandomRanged(0, 99) >= kBlastCellChance)
						continue;

					auto const pCell = MapClass::Instance.TryGetCellAt(cell);

					if (!pCell)
						continue;

					auto coords = pCell->GetCoordsWithBridge();
					coords.Z = pThis->GetRenderCoords().Z + 96;

					AnimExt::CreateRandomAnim(anims, coords, pThis, pThis->Owner);
					++spawned;
				}

				Debug::Log("[AircraftCarrier] sinking blast cells=%d spawned=%d\n",
					static_cast<int>(cells.size()), spawned);
			}
		}

		for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft)
				continue;

			s_carrierPayloads.erase(pAircraft);

			const unsigned char payloadState = i < pState->CarrierPayloadDocked.size()
				? pState->CarrierPayloadDocked[i]
				: 0;

			if (payloadState == 1 && IsLiveAircraft(pAircraft))
			{
				// Parked deck aircraft die with the explosion. Only queue the
				// kill here; the global logic tick performs it outside every
				// object's AI and anim container edit.
				s_pendingKills.insert(pAircraft);
			}
			else if (IsLiveAircraft(pAircraft) && !pAircraft->InLimbo)
			{
				// Airborne payloads simply lose their home and continue flying.
				pAircraft->InAir = true;
				pAircraft->SetDestination(nullptr, true);
				pAircraft->DockNowHeadingTo = nullptr;
				pAircraft->SetTarget(nullptr);
				pAircraft->SetArchiveTarget(nullptr);
				pAircraft->QueueMission(Mission::Guard, false);
			}
		}

		pState->CarrierPayloads.clear();
		pState->CarrierPayloadDocked.clear();
		pState->CarrierPayloadLastCoords.clear();
		pState->CarrierPayloadReloadTimer.clear();
		pState->CarrierPayloadReturnCell.clear();
		pState->CarrierPayloadsInitialized = false;
	}

	Debug::Log("[AircraftCarrier] sinking destruction started for %s\n",
		pType ? pType->ID : "?");
}

int Footprint::GetCarrierSinkStartFrame(UnitClass* pThis)
{
	if (!pThis)
		return -1;

	auto const it = s_carrierSinkStartFrame.find(pThis);

	return it == s_carrierSinkStartFrame.end() ? -1 : it->second;
}

int Footprint::AdvanceCarrierSink(UnitClass* pThis, int normalStep)
{
	constexpr int kDivisor = 20;

	if (!pThis || normalStep <= 0)
		return 0;

	int& accum = s_carrierSinkAccum[pThis];
	accum += normalStep;
	const int step = accum / kDivisor;
	accum %= kDivisor;
	return step;
}

void Footprint::OnUnitDestroyed(UnitClass* pThis)
{
	if (!pThis)
		return;

	s_carrierMoveRef.erase(pThis);
	s_carrierLastMoveFrame.erase(pThis);
	s_carrierLastFacing.erase(pThis);

	auto const pState = GetState(pThis);

	if (!pState)
		return;

	// Parked aircraft are part of the deck: they die with the ship. Airborne
	// payloads simply lose their home and continue as normal aircraft.
	for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
	{
		auto const pAircraft = pState->CarrierPayloads[i];

		if (!pAircraft)
			continue;

		s_carrierPayloads.erase(pAircraft);

		const unsigned char state = i < pState->CarrierPayloadDocked.size()
			? pState->CarrierPayloadDocked[i]
			: 0;

		// Request destruction of everything still parked on the deck. The
		// actual kill runs from the next global logic tick, outside every
		// object's AI; doing it inside this callback destroys anims re-entrantly
		// and crashed inside Container<AnimTypeExt>::Remove.
		if (state == 1 && IsLiveAircraft(pAircraft))
		{
			s_pendingKills.insert(pAircraft);
		}

		// Airborne payloads lose their home and continue as ordinary aircraft.
		if (state != 1 && IsLiveAircraft(pAircraft) && !pAircraft->InLimbo)
		{
			pAircraft->InAir = true;
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->SetDestination(nullptr, true);
			pAircraft->SetTarget(nullptr);
			pAircraft->SetArchiveTarget(nullptr);
			pAircraft->QueueMission(Mission::Guard, false);
		}
	}

	pState->CarrierPayloads.clear();
	pState->CarrierPayloadDocked.clear();
	pState->CarrierPayloadLastCoords.clear();
	pState->CarrierPayloadReloadTimer.clear();
	pState->CarrierPayloadReturnCell.clear();
	pState->CarrierPayloadsInitialized = false;
	pState->Runway = CarrierRunwayState {};
	pState->LandingRunway = CarrierRunwayState {};
	pState->TakeoffRunways.clear();
	pState->LandingRunways.clear();
	s_runwayLoggedCarriers.erase(pThis);
	s_carrierSinkStartFrame.erase(pThis);
	s_carrierSinkAccum.erase(pThis);

	// Legacy / pre-rewrite dock proxies cannot outlive the carrier: they are
	// immune to damage, so nothing else would ever remove them from the map.
	ReleaseDockProxies(pState);

	Debug::Log("[AircraftCarrier] released deck of destroyed %s\n",
		pThis->GetTechnoType() ? pThis->GetTechnoType()->ID : "?");
}
