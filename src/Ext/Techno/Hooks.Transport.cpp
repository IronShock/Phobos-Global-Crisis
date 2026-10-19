#include "Body.h"
#include "TransportEntry.h"

#include <Ext/Scenario/Body.h>
#include <Utilities/AresHelper.h>
#include <Utilities/AresFunctions.h>
#include <Utilities/Debug.h>

#include <CCINIClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <UnitTypeClass.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

DEFINE_HOOK_AGAIN(0x6FA33C, TechnoClass_ThreatEvals_OpenToppedOwner, 0x6) // TechnoClass::AI
DEFINE_HOOK_AGAIN(0x6F89F4, TechnoClass_ThreatEvals_OpenToppedOwner, 0x6) // TechnoClass::EvaluateCell
DEFINE_HOOK_AGAIN(0x6F7EC2, TechnoClass_ThreatEvals_OpenToppedOwner, 0x6) // TechnoClass::EvaluateObject
DEFINE_HOOK(0x6F8FD7, TechnoClass_ThreatEvals_OpenToppedOwner, 0x5)       // TechnoClass::Greatest_Threat
{
	enum { SkipCheckOne = 0x6F8FDC, SkipCheckTwo = 0x6F7EDA, SkipCheckThree = 0x6F8A0F, SkipCheckFour = 0x6FA37A };

	TechnoClass* pThis = nullptr;
	auto returnAddress = SkipCheckOne;

	switch (R->Origin())
	{
	case 0x6F8FD7:
		pThis = R->ESI<TechnoClass*>();
		break;
	case 0x6F7EC2:
		pThis = R->EDI<TechnoClass*>();
		returnAddress = SkipCheckTwo;
		break;
	case 0x6F89F4:
		pThis = R->ESI<TechnoClass*>();
		returnAddress = SkipCheckThree;
	case 0x6FA33C:
		pThis = R->ESI<TechnoClass*>();
		returnAddress = SkipCheckFour;
	default:
		return 0;
	}

	if (auto const pTransport = pThis->Transporter)
	{
		if (TechnoExt::ExtMap.Find(pTransport)->TypeExtData->Passengers_SyncOwner)
			return returnAddress;
	}

	return 0;
}

DEFINE_HOOK(0x701881, TechnoClass_ChangeHouse_Passenger_SyncOwner, 0x5)
{
	GET(TechnoClass*, pThis, ESI);

	if (auto pPassenger = pThis->Passengers.GetFirstPassenger())
	{
		if (TechnoExt::ExtMap.Find(pThis)->TypeExtData->Passengers_SyncOwner)
		{
			const auto pOwner = pThis->Owner;

			do
			{
				pPassenger->SetOwningHouse(pOwner, false);
				pPassenger = abstract_cast<FootClass*>(pPassenger->NextObject);
			}
			while (pPassenger);
		}
	}

	return 0;
}

// Temporary diagnostics: identify exactly who adds/removes cargo passengers.
static inline TechnoClass* TransportFromPassengers(PassengersClass* pThis)
{
	return reinterpret_cast<TechnoClass*>(reinterpret_cast<char*>(pThis) - 0x114);
}

DEFINE_HOOK(0x4734B0, PassengersClass_RemovePassenger_Debug, 0x5)
{
	GET(PassengersClass*, pThis, ECX);
	auto const pTransport = TransportFromPassengers(pThis);
	auto const pPassenger = R->Stack<FootClass*>(0x4);

	Debug::Log("[cargo] frame=%d transport=%p op=remove passenger=%p before=%d first=%p caller=%p\n",
		Unsorted::CurrentFrame, static_cast<void*>(pTransport), static_cast<void*>(pPassenger),
		pThis->NumPassengers, static_cast<void*>(pThis->FirstPassenger),
		reinterpret_cast<void*>(R->Stack<DWORD>(0x0)));

	return 0;
}

DEFINE_HOOK(0x473430, PassengersClass_RemoveFirstPassenger_Debug, 0x5)
{
	GET(PassengersClass*, pThis, ECX);
	auto const pTransport = TransportFromPassengers(pThis);

	Debug::Log("[cargo] frame=%d transport=%p op=remove_first passenger=%p before=%d caller=%p\n",
		Unsorted::CurrentFrame, static_cast<void*>(pTransport),
		static_cast<void*>(pThis->FirstPassenger), pThis->NumPassengers,
		reinterpret_cast<void*>(R->Stack<DWORD>(0x0)));

	return 0;
}

DEFINE_HOOK(0x4DE630, FootClass_EnterAsPassenger_Debug, 0x6)
{
	GET(FootClass*, pThis, ECX);
	auto const pPassenger = R->Stack<FootClass*>(0x4);

	Debug::Log("[cargo] frame=%d transport=%s(%p) op=enter_as_passenger passenger=%p before=%d caller=%p\n",
		Unsorted::CurrentFrame, pThis->GetTechnoType()->ID, static_cast<void*>(pThis),
		static_cast<void*>(pPassenger), pThis->Passengers.NumPassengers,
		reinterpret_cast<void*>(R->Stack<DWORD>(0x0)));

	return 0;
}

DEFINE_HOOK(0x71067B, TechnoClass_EnterTransport, 0x7)
{
	enum { SkipGameCode = 0x7106D5 };

	GET(TechnoClass*, pThis, ESI);
	GET(FootClass*, pPassenger, EDI);

	if (auto const pTransport = abstract_cast<UnitClass*>(pThis))
	{
		// Shared gate: already inside, wrong side, not at the vehicle yet,
		// full, or still inside the per-transport entry interval.
		if (!PassengerEnterDirections::TryBeginEnter(pTransport, pPassenger))
			return SkipGameCode;
	}

	if (pPassenger)
	{
		auto const pType = pPassenger->GetTechnoType();
		auto const pExt = TechnoExt::ExtMap.Find(pPassenger);
		auto const whatAmI = pPassenger->WhatAmI();
		auto const pTransTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;

		if (pTransTypeExt->Passengers_SyncOwner && pTransTypeExt->Passengers_SyncOwner_RevertOnExit)
			pExt->OriginalPassengerOwner = pPassenger->Owner;

		if (whatAmI != AbstractType::Aircraft && whatAmI != AbstractType::Building
			&& pType->Ammo > 0 && pExt->TypeExtData->ReloadInTransport)
		{
			ScenarioExt::Global()->TransportReloaders.push_back(pExt);
		}
	}

	return 0;
}

DEFINE_HOOK(0x4DE722, FootClass_LeaveTransport, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(FootClass*, pPassenger, EAX);

	if (pPassenger)
	{
		auto const pType = pPassenger->GetTechnoType();
		auto const pExt = TechnoExt::ExtMap.Find(pPassenger);
		auto const whatAmI = pPassenger->WhatAmI();
		auto const pTransTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;

		// Remove from transport reloader list before switching house
		if (whatAmI != AbstractType::Aircraft && whatAmI != AbstractType::Building
			&& pType->Ammo > 0 && pExt->TypeExtData->ReloadInTransport)
		{
			auto& vec = ScenarioExt::Global()->TransportReloaders;
			vec.erase(std::remove(vec.begin(), vec.end(), pExt), vec.end());
		}

		if (pTransTypeExt->Passengers_SyncOwner
			&& pTransTypeExt->Passengers_SyncOwner_RevertOnExit
			&& pExt->OriginalPassengerOwner)
		{
			pPassenger->SetOwningHouse(pExt->OriginalPassengerOwner, false);
		}
	}

	return 0;
}

// Has to be done here, before Ares survivor hook to take effect.
DEFINE_HOOK(0x737F80, UnitClass_ReceiveDamage_Cargo_SyncOwner, 0x6)
{
	GET(UnitClass*, pThis, ESI);

	if (auto pPassenger = pThis->Passengers.GetFirstPassenger())
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->Type);

		if (pTypeExt->Passengers_SyncOwner && pTypeExt->Passengers_SyncOwner_RevertOnExit)
		{
			do
			{
				auto const pExt = TechnoExt::ExtMap.Find(pPassenger);

				if (pExt->OriginalPassengerOwner)
					pPassenger->SetOwningHouse(pExt->OriginalPassengerOwner, false);

				pPassenger = abstract_cast<FootClass*>(pPassenger->NextObject);
			}
			while (pPassenger);
		}
	}

	return 0;
}

DEFINE_HOOK(0x51DF82, InfantryClass_FireAt_ReloadInTransport, 0x6)
{
	GET(InfantryClass* const, pThis, ESI);

	if (pThis->Transporter)
	{
		auto const pType = pThis->Type;
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

		if (pTypeExt->ReloadInTransport && pType->Ammo > 0 && pThis->Ammo < pType->Ammo)
			pThis->StartReloading();
	}

	return 0;
}

// Customizable OpenTopped Properties
// Author: Otamaa
DEFINE_HOOK(0x6F72D2, TechnoClass_IsCloseEnoughToTarget_OpenTopped_RangeBonus, 0xC)
{
	GET(TechnoClass* const, pThis, ESI);

	if (auto const pTransport = pThis->Transporter)
	{
		auto const pExt = TechnoExt::ExtMap.Find(pTransport)->TypeExtData;
		R->EAX(pExt->OpenTopped_RangeBonus.Get(RulesClass::Instance->OpenToppedRangeBonus));
		return 0x6F72DE;
	}

	return 0;
}

DEFINE_HOOK(0x71A82C, TemporalClass_AI_Opentopped_WarpDistance, 0xC)
{
	GET(TemporalClass* const, pThis, ESI);

	if (auto const pTransport = pThis->Owner->Transporter)
	{
		auto const pExt = TechnoExt::ExtMap.Find(pTransport)->TypeExtData;
		R->EDX(pExt->OpenTopped_WarpDistance.Get(RulesClass::Instance->OpenToppedWarpDistance));
		return 0x71A838;
	}

	return 0;
}

DEFINE_HOOK(0x710552, TechnoClass_SetOpenTransportCargoTarget_ShareTarget, 0x6)
{
	enum { ReturnFromFunction = 0x71057F };

	GET(TechnoClass* const, pThis, ECX);
	GET_STACK(AbstractClass* const, pTarget, STACK_OFFSET(0x8, 0x4));

	if (pTarget)
	{
		auto const pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;

		if (!pTypeExt->OpenTopped_ShareTransportTarget)
			return ReturnFromFunction;
	}

	return 0;
}

#pragma region NoQueueUpToEnterAndUnload

// Use a square range because it doesn't seem necessary to calculate the circular range
static inline bool IsCloseEnoughToEnter(UnitClass* pTransport, FootClass* pPassenger)
{
	return (std::abs(pPassenger->Location.X - pTransport->Location.X) < 384
		&& std::abs(pPassenger->Location.Y - pTransport->Location.Y) < 384
		&& std::abs(pPassenger->Location.Z - pTransport->Location.Z) < Unsorted::CellHeight);
}

// Fast entry must not limbo a passenger that is still approaching from a distance.
// This is the "visually at the vehicle" check: roughly the vehicle's own cell edge.
constexpr int PassengerAtVehicleDistance = 96; // leptons
constexpr int PassengerDirectionEntryDistance = 320; // leptons, one cell + tolerance
constexpr double PassengerDirectionCheckDistance = 384.0; // leptons
constexpr double PassengerLineHalfWidth = 64.0; // leptons
constexpr double PassengerMaxLineAngle = 30.0; // degrees

static inline bool IsAtVehicle(UnitClass* pTransport, FootClass* pPassenger)
{
	return (std::abs(pPassenger->Location.X - pTransport->Location.X) <= PassengerAtVehicleDistance
		&& std::abs(pPassenger->Location.Y - pTransport->Location.Y) <= PassengerAtVehicleDistance
		&& std::abs(pPassenger->Location.Z - pTransport->Location.Z) < Unsorted::CellHeight);
}

// Rewrite from 0x73758A, replace send RadioCommand::QueryCanEnter
static inline bool CanEnterNow(UnitClass* pTransport, FootClass* pPassenger, bool ignoreQueue = false)
{
	if (!pTransport->Owner->IsAlliedWith(pPassenger) || pTransport->IsBeingWarpedOut())
		return false;

	// Added to prevent unexpected enter action
	if (pTransport->OnBridge || pPassenger->Deactivated || pPassenger->IsUnderEMP())
		return false;

	if (pPassenger->IsMindControlled() || pPassenger->ParasiteEatingMe)
		return false;

	const auto pManager = pPassenger->CaptureManager;

	if (pManager && pManager->IsControllingSomething())
		return false;

	const auto pTransportType = pTransport->Type;

	// Added to fit with AmphibiousEnter
	if (pTransport->GetCell()->LandType == LandType::Water && !TechnoTypeExt::ExtMap.Find(pTransportType)->AmphibiousEnter.Get(RulesExt::Global()->AmphibiousEnter))
		return false;

	const bool bySize = TechnoTypeExt::ExtMap.Find(pTransportType)->Passengers_BySize;
	const int passengerSize = static_cast<int>(pPassenger->GetTechnoType()->Size);

	if (passengerSize > static_cast<int>(pTransportType->SizeLimit))
		return false;

	const int maxSize = pTransportType->Passengers;
	const int predictSize = bySize ? (pTransport->Passengers.GetTotalSize() + passengerSize) : (pTransport->Passengers.NumPassengers + 1);
	const auto pLink = abstract_cast<FootClass*>(pTransport->GetNthLink());
	const bool needCalculate = !ignoreQueue && pLink && pLink != pPassenger && pLink->Destination == pTransport;

	// When the most important passenger is close, need to prevent overlap
	if (needCalculate)
	{
		if (IsCloseEnoughToEnter(pTransport, pLink))
			return (predictSize <= (maxSize - (bySize ? static_cast<int>(pLink->GetTechnoType()->Size) : 1)));

		if (predictSize > (maxSize - (bySize ? static_cast<int>(pLink->GetTechnoType()->Size) : 1)))
		{
			pLink->QueueMission(Mission::None, false);
			pLink->SetDestination(nullptr, true);
			pLink->SendCommand(RadioCommand::NotifyUnlink, pTransport);
		}
	}

	return predictSize <= maxSize;
}

// Rewrite from 0x51A21B/0x73A6D1
static inline void DoEnterNow(UnitClass* pTransport, FootClass* pPassenger)
{
	if (!pTransport || !pPassenger)
		return;

	// Never add a passenger that is already in the cargo list. Transporter /
	// InLimbo are not reliable for engine-added passengers.
	if (PassengerEnterDirections::IsPassengerInside(pTransport, pPassenger))
		return;

	// Hard capacity guard: never append past the transport's passenger limit.
	if (pTransport->Type->Passengers > 0
		&& pTransport->Passengers.NumPassengers >= pTransport->Type->Passengers)
	{
		Debug::Log("[Passengers.EnterDirections][add] transport=%s blocked=full passengers=%d/%d\n",
			pTransport->Type->ID, pTransport->Passengers.NumPassengers, pTransport->Type->Passengers);
		return;
	}

	const int passengersBefore = pTransport->Passengers.NumPassengers;

	// Hard gate: every direct AddPassenger path must pass the direction check.
	if (!PassengerEnterDirections::IsEntryAllowedByDirection(pTransport, pPassenger, "DoEnterNow"))
		return;

	// Vanilla only for infantry, but why
	if (const auto pTag = pTransport->AttachedTag)
		pTag->RaiseEvent(TriggerEvent::EnteredBy, pPassenger, CellStruct::Empty);

	// Vanilla did not handle SpawnManager and SlaveManager, so I don't care about these here either
	pPassenger->SetArchiveTarget(nullptr);
	pPassenger->MissionAccumulateTime = 0;
	pPassenger->GattlingValue = 0;
	pPassenger->CurrentGattlingStage = 0;

	pPassenger->Limbo(); // Don't swap order casually
	pPassenger->OnBridge = false; // Don't swap order casually, important
	pPassenger->NextObject = nullptr; // Don't swap order casually, very important

	pPassenger->SetDestination(nullptr, true); // Added, to prevent passengers from return to board position when survive
	pPassenger->QueueUpToEnter = nullptr; // Added, to prevent passengers from wanting to get on after getting off
	pPassenger->FrozenStill = true; // Added, to prevent the vehicles from stacking together when unloading
	pPassenger->SetSpeedPercentage(0.0); // Added, to stop the passengers and let OpenTopped work normally

	const auto pPassengerType = pPassenger->GetTechnoType();

	// Reinstalling Locomotor can avoid various issues such as teleportation, ignoring commands, and automatic return
	while (LocomotionClass::End_Piggyback(pPassenger->Locomotor));

	if (const auto pNewLoco = LocomotionClass::CreateInstance(pPassengerType->Locomotor))
	{
		pPassenger->Locomotor = std::move(pNewLoco);
		pPassenger->Locomotor->Link_To_Object(pPassenger);
	}

	// Use the same attachment mode as the engine's normal entry path. The
	// default chain mode would attach the passenger's NextObject list too.
	TechnoAttachmentTemp::SetSingleObjectAttach(true);
	pTransport->AddPassenger(pPassenger); // Don't swap order casually, very very important
	TechnoAttachmentTemp::SetSingleObjectAttach(false);
	pPassenger->Transporter = pTransport;

	Debug::Log("[Passengers.EnterDirections][add] transport=%s(%p) frame=%d before=%d after=%d expected=%d next=%p\n",
		pTransport->Type->ID, static_cast<void*>(pTransport), Unsorted::CurrentFrame,
		passengersBefore, pTransport->Passengers.NumPassengers, passengersBefore + 1,
		static_cast<void*>(pPassenger->NextObject));

	if (pTransport->Type->OpenTopped)
		pTransport->EnteredOpenTopped(pPassenger);

	if (pPassengerType->OpenTopped)
		pPassenger->SetTargetForPassengers(nullptr);

	pPassenger->Undiscover();
}

#pragma region PassengerEnterDirections

// Directions are relative to the transport's current body facing:
// 0 front, 1 frontright, 2 right, 3 backright, 4 back, 5 backleft, 6 left, 7 frontleft.
namespace PassengerEnterDirections
{
	constexpr int Front = 1 << 0;
	constexpr int FrontRight = 1 << 1;
	constexpr int Right = 1 << 2;
	constexpr int BackRight = 1 << 3;
	constexpr int Back = 1 << 4;
	constexpr int BackLeft = 1 << 5;
	constexpr int Left = 1 << 6;
	constexpr int FrontLeft = 1 << 7;
	constexpr int All = Front | FrontRight | Right | BackRight | Back | BackLeft | Left | FrontLeft;
	constexpr int None = 0;

	// Transient parallel-entry targets. Not serialized; used to keep every
	// passenger independent after the engine queue link is cleared.
	std::unordered_map<FootClass*, UnitClass*> s_parallelEntryTargets;

	// Transient pacing state. These maps are not serialized.
	std::unordered_map<UnitClass*, int> s_lastEnterFrame;
	std::unordered_map<UnitClass*, int> s_lastUnloadFrame;



	constexpr int DefaultEnterStaggerFrames = 4;
	constexpr int DefaultUnloadStaggerFrames = 2;

	RulesClass* s_lastRules = nullptr;
	std::unordered_map<UnitTypeClass*, int> s_directionCache;
	std::unordered_map<UnitTypeClass*, int> s_enterStaggerCache;
	std::unordered_map<UnitTypeClass*, int> s_unloadStaggerCache;

	void EnsureRulesCache()
	{
		if (s_lastRules == RulesClass::Instance)
			return;

		s_parallelEntryTargets.clear();
		s_lastEnterFrame.clear();
		s_lastUnloadFrame.clear();
		s_directionCache.clear();
		s_enterStaggerCache.clear();
		s_unloadStaggerCache.clear();
		s_lastRules = RulesClass::Instance;
	}

	bool IsPassengerInside(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		for (auto pCurrent = pTransport->Passengers.GetFirstPassenger();
			pCurrent;
			pCurrent = abstract_cast<FootClass*>(pCurrent->NextObject))
		{
			if (pCurrent == pPassenger)
				return true;
		}

		return false;
	}

	int Parse(const char* text)
	{
		if (!text || !*text)
			return All;

		int flags = 0;
		bool sawDirection = false;

		for (auto&& part : std::string_view { text } | std::views::split(','))
		{
			std::string_view token { part.begin(), part.end() };
			const auto first = token.find_first_not_of(" \t\r");
			const auto last = token.find_last_not_of(" \t\r");

			if (first == std::string_view::npos)
				continue;

			token = token.substr(first, last - first + 1);
			const std::string name { token };

			if (_stricmp(name.c_str(), "all") == 0)
				return All;

			if (_stricmp(name.c_str(), "none") == 0)
				return None;

			static const std::pair<const char*, int> Names[] =
			{
				{ "front", Front },
				{ "frontright", FrontRight },
				{ "right", Right },
				{ "backright", BackRight },
				{ "back", Back },
				{ "backleft", BackLeft },
				{ "left", Left },
				{ "frontleft", FrontLeft },
			};

			bool matched = false;

			for (auto const& [pName, bit] : Names)
			{
				if (_stricmp(name.c_str(), pName) == 0)
				{
					flags |= bit;
					sawDirection = true;
					matched = true;
					break;
				}
			}

			if (!matched)
				Debug::Log("[Passengers.EnterDirections] Unknown direction '%s'.\n", name.c_str());
		}

		return sawDirection ? flags : All;
	}

	int Get(UnitTypeClass* pType)
	{
		if (!pType || !CCINIClass::INI_Rules)
			return All;

		EnsureRulesCache();

		if (auto const it = s_directionCache.find(pType); it != s_directionCache.end())
			return it->second;

		char buffer[0x80] {};
		const int read = CCINIClass::INI_Rules->ReadString(
			pType->ID, "Passengers.EnterDirections", "", buffer, sizeof(buffer));

		const int flags = read > 0 ? Parse(buffer) : All;

		Debug::Log("[Passengers.EnterDirections] %s = '%s' (0x%02X)\n",
			pType->ID, read > 0 ? buffer : "absent", flags);

		s_directionCache.emplace(pType, flags);
		return flags;
	}

	int ReadStaggerFrames(UnitTypeClass* pType, const char* key, int fallback,
		std::unordered_map<UnitTypeClass*, int>& cache)
	{
		if (!pType || !CCINIClass::INI_Rules)
			return fallback;

		EnsureRulesCache();

		if (auto const it = cache.find(pType); it != cache.end())
			return it->second;

		const int value = std::max(0, CCINIClass::INI_Rules->ReadInteger(pType->ID, key, fallback));
		cache.emplace(pType, value);
		return value;
	}

	int GetEnterStaggerFrames(UnitTypeClass* pType)
	{
		return ReadStaggerFrames(pType, "Passengers.EnterStaggerFrames",
			DefaultEnterStaggerFrames, s_enterStaggerCache);
	}

	int GetUnloadStaggerFrames(UnitTypeClass* pType)
	{
		return ReadStaggerFrames(pType, "Passengers.UnloadStaggerFrames",
			DefaultUnloadStaggerFrames, s_unloadStaggerCache);
	}

	// Returns 0..7 relative to the transport's current body facing, or -1 when
	// the passenger is exactly on the transport's centre.
	int GetRelativeDirection(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return -1;

		auto const pTransportCell = pTransport->GetCell();

		if (!pTransportCell)
			return -1;

		const CoordStruct transportCoords = pTransport->GetCoords();
		const CoordStruct passengerCoords = pPassenger->GetCoords();

		// On the transport centre: every neighbour is equally close, so
		// picking the nearest cell would return an arbitrary side. Treat a
		// tiny deadzone around the centre as "at vehicle" and accept it.
		constexpr int CentreDeadzone = 16; // leptons
		if (std::abs(passengerCoords.X - transportCoords.X) <= CentreDeadzone
			&& std::abs(passengerCoords.Y - transportCoords.Y) <= CentreDeadzone)
		{
			return -1;
		}

		int bestWorld = -1;
		long long bestDistance = std::numeric_limits<long long>::max();

		// Use the engine's own neighbour cells. This keeps the direction mapping
		// consistent with FacingType / AdjacentCell instead of assuming a
		// particular world-to-screen or isometric angle.
		for (int dir = 0; dir < 8; ++dir)
		{
			auto const pCell = pTransportCell->GetNeighbourCell(static_cast<FacingType>(dir));

			if (!pCell)
				continue;

			const CoordStruct cellCoords = pCell->GetCoords();
			const long long dx = static_cast<long long>(passengerCoords.X) - cellCoords.X;
			const long long dy = static_cast<long long>(passengerCoords.Y) - cellCoords.Y;
			const long long distance = dx * dx + dy * dy;

			if (distance < bestDistance)
			{
				bestDistance = distance;
				bestWorld = dir;
			}
		}

		if (bestWorld < 0)
			return -1;

		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
		return (bestWorld - facing + 8) % 8;
	}

	struct LineCheckResult
	{
		bool Pass = false;
		int Relative = -1;
		double Dot = 0.0;
		double Cross = 0.0;
		double Along = 0.0;
		double Perp = 0.0;
		double Angle = 0.0;
	};

	LineCheckResult CheckLine(UnitClass* pTransport, FootClass* pPassenger, int flags)
	{
		LineCheckResult result {};

		if (!pTransport || !pPassenger || flags == None)
			return result;

		const CoordStruct transportCoords = pTransport->GetCoords();
		const CoordStruct passengerCoords = pPassenger->GetCoords();
		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
		const double vx = static_cast<double>(passengerCoords.X - transportCoords.X);
		const double vy = static_cast<double>(passengerCoords.Y - transportCoords.Y);

		double bestPerp = std::numeric_limits<double>::max();

		for (int relative = 0; relative < 8; ++relative)
		{
			if (!(flags & (1 << relative)))
				continue;

			const int world = (facing + relative) % 8;
			const CellStruct& dir = Unsorted::AdjacentCell[world];
			const double dirLength = std::sqrt(static_cast<double>(dir.X * dir.X + dir.Y * dir.Y));

			if (dirLength <= 0.0)
				continue;

			const double dot = vx * dir.X + vy * dir.Y;
			const double cross = vx * dir.Y - vy * dir.X;
			const double along = dot / dirLength;
			const double perp = std::abs(cross) / dirLength;
			const double angle = std::atan2(std::abs(cross), dot) * (180.0 / 3.14159265358979323846);

			if (result.Relative < 0 || perp < bestPerp)
			{
				result.Relative = relative;
				result.Dot = dot;
				result.Cross = cross;
				result.Along = along;
				result.Perp = perp;
				result.Angle = angle;
				bestPerp = perp;
			}

			// Direction gate: in front of the allowed side, within 64 leptons of
			// the centre line and 30 degrees. The 96-lepton "at vehicle" check is
			// applied separately; this gate only decides whether the side is right.
			if (along > 0.0
				&& along <= PassengerDirectionCheckDistance
				&& perp <= PassengerLineHalfWidth
				&& angle <= PassengerMaxLineAngle)
			{
				result.Pass = true;
				result.Relative = relative;
				result.Dot = dot;
				result.Cross = cross;
				result.Along = along;
				result.Perp = perp;
				result.Angle = angle;
				break;
			}
		}

		return result;
	}

	std::unordered_map<FootClass*, int> s_lastGateLogFrame;

	void LogGate(UnitClass* pTransport, FootClass* pPassenger, int flags, const char* gate,
		bool allowed, const char* reason, const LineCheckResult& line)
	{
		if (!pTransport || !pPassenger)
			return;

		const int frame = Unsorted::CurrentFrame;
		auto const it = s_lastGateLogFrame.find(pPassenger);

		if (it != s_lastGateLogFrame.end() && frame - it->second < 30)
			return;

		s_lastGateLogFrame[pPassenger] = frame;

		auto const pTransportCell = pTransport->GetCell();
		auto const pPassengerCell = pPassenger->GetCell();
		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
		const int relative = GetRelativeDirection(pTransport, pPassenger);
		const CoordStruct transportCoords = pTransport->GetCoords();
		const CoordStruct passengerCoords = pPassenger->GetCoords();

		Debug::Log("[Passengers.EnterDirections][gate] gate=%s transport=%s(%p) flags=0x%02X facing=%d relative=%d "
			"pcell=(%d,%d) last=(%d,%d) tcell=(%d,%d) pcoords=(%d,%d,%d) tcoords=(%d,%d,%d) "
			"dot=%.1f cross=%.1f perp=%.1f angle=%.1f along=%.1f line=%d mission=%d dest=%p queue=%p link=%p passengers=%d/%d parallel=%p result=%s reason=%s\n",
			gate, pTransport->Type->ID, static_cast<void*>(pTransport), flags, facing, relative,
			pPassengerCell ? pPassengerCell->MapCoords.X : -1,
			pPassengerCell ? pPassengerCell->MapCoords.Y : -1,
			pPassenger->LastMapCoords.X, pPassenger->LastMapCoords.Y,
			pTransportCell ? pTransportCell->MapCoords.X : -1,
			pTransportCell ? pTransportCell->MapCoords.Y : -1,
			passengerCoords.X, passengerCoords.Y, passengerCoords.Z,
			transportCoords.X, transportCoords.Y, transportCoords.Z,
			line.Dot, line.Cross, line.Perp, line.Angle, line.Along, line.Pass ? 1 : 0,
			static_cast<int>(pPassenger->CurrentMission),
			static_cast<void*>(pPassenger->Destination),
			static_cast<void*>(pPassenger->QueueUpToEnter),
			static_cast<void*>(pTransport->GetNthLink()),
			pTransport->Passengers.NumPassengers, pTransport->Type->Passengers,
			static_cast<void*>(s_parallelEntryTargets.contains(pPassenger)
				? s_parallelEntryTargets[pPassenger] : nullptr),
			allowed ? "allow" : "block", reason);
	}

	CellClass* FindAllowedCell(UnitClass* pTransport, FootClass* pPassenger, int flags)
	{
		if (!pTransport || !pPassenger || flags == None)
			return nullptr;

		auto const pTransportCell = pTransport->GetCell();

		if (!pTransportCell)
			return nullptr;

		auto const pPassengerCell = pPassenger->GetCell();
		const CellStruct passengerCoords = pPassengerCell ? pPassengerCell->MapCoords : pTransportCell->MapCoords;
		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());

		CellClass* pBest = nullptr;
		int bestScore = std::numeric_limits<int>::max();

		for (int relative = 0; relative < 8; ++relative)
		{
			if (!(flags & (1 << relative)))
				continue;

			const int world = (facing + relative) % 8;
			auto const pCell = pTransportCell->GetNeighbourCell(static_cast<FacingType>(world));

			if (!pCell)
				continue;

			// Buildings block the cell; infantry/vehicles may share or be pushed.
			if (pCell->FirstObject && pCell->FirstObject != pPassenger
				&& pCell->FirstObject != pTransport
				&& pCell->FirstObject->WhatAmI() == AbstractType::Building)
			{
				continue;
			}

			const int score = std::abs(pCell->MapCoords.X - passengerCoords.X)
				+ std::abs(pCell->MapCoords.Y - passengerCoords.Y);

			if (score < bestScore)
			{
				bestScore = score;
				pBest = pCell;
			}
		}

		return pBest;
	}

	bool IsEntryAllowedByDirection(UnitClass* pTransport, FootClass* pPassenger, const char* gate)
	{
		if (!pTransport || !pPassenger)
			return true;

		// Only infantry are restricted; vehicle passengers keep vanilla behaviour.
		if (pPassenger->WhatAmI() != AbstractType::Infantry)
			return true;

		const int flags = Get(pTransport->Type);

		if (flags == All)
			return true;

		if (flags == None)
		{
			LogGate(pTransport, pPassenger, flags, gate, false, "none", {});
			return false;
		}

		// A passenger standing on the transport's cell is already at the
		// vehicle. Its sub-cell offset can resolve to any side, so do not
		// reject it as a side approach.
		auto const pPassengerCell = pPassenger->GetCell();
		auto const pTransportCell = pTransport->GetCell();

		if (pPassengerCell && pTransportCell && pPassengerCell == pTransportCell)
		{
			LogGate(pTransport, pPassenger, flags, gate, true, "same-cell", {});
			return true;
		}

		const int relative = GetRelativeDirection(pTransport, pPassenger);

		if (relative < 0)
		{
			LogGate(pTransport, pPassenger, flags, gate, true, "centre", {});
			return true;
		}

		const CoordStruct transportCoords = pTransport->GetCoords();
		const CoordStruct passengerCoords = pPassenger->GetCoords();
		const int dx = std::abs(passengerCoords.X - transportCoords.X);
		const int dy = std::abs(passengerCoords.Y - transportCoords.Y);
		const bool directionAllowed = (flags & (1 << relative)) != 0;
		const bool closeEnough = dx <= PassengerDirectionEntryDistance
			&& dy <= PassengerDirectionEntryDistance;
		const bool allowed = directionAllowed && closeEnough;

		// Diagnostic only: the old line values remain visible in the log.
		LineCheckResult line = CheckLine(pTransport, pPassenger, flags);

		LogGate(pTransport, pPassenger, flags, gate, allowed,
			allowed ? "sector" : (directionAllowed ? "distance" : "direction"), line);

		return allowed;
	}

	bool TryBeginEnter(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		// The engine consumes this gate for every entry path. Never allow a
		// passenger that is already in cargo to be attached again.
		if (IsPassengerInside(pTransport, pPassenger))
			return false;

		// Direction restrictions and pacing apply to infantry only.
		if (pPassenger->WhatAmI() != AbstractType::Infantry)
			return true;

		const int flags = Get(pTransport->Type);

		if (flags == All)
			return true;

		// The engine/per-cell paths call this only when the passenger is
		// actually entering. Do not add a second distance or CanEnterNow
		// check here: rejecting the engine's own validated entry leaves the
		// passenger standing next to the transport forever.
		if (!IsEntryAllowedByDirection(pTransport, pPassenger, "TryBeginEnter"))
			return false;

		if (pTransport->Type->Passengers > 0
			&& pTransport->Passengers.NumPassengers >= pTransport->Type->Passengers)
			return false;

		const int staggerFrames = GetEnterStaggerFrames(pTransport->Type);

		if (staggerFrames > 0)
		{
			const int frame = Unsorted::CurrentFrame;
			auto const it = s_lastEnterFrame.find(pTransport);

			if (it != s_lastEnterFrame.end() && frame - it->second < staggerFrames)
				return false;

			s_lastEnterFrame[pTransport] = frame;

			Debug::Log("[Passengers.EnterDirections][stagger-enter] transport=%s(%p) frame=%d interval=%d passenger=%p\n",
				pTransport->Type->ID, static_cast<void*>(pTransport), frame, staggerFrames,
				static_cast<void*>(pPassenger));
		}

		return true;
	}

	bool TryBeginUnload(UnitClass* pTransport)
	{
		if (!pTransport)
			return false;

		// No pacing for unrestricted transports or transports that explicitly
		// use the old instant-unload feature.
		if (Get(pTransport->Type) == All)
			return true;

		const int staggerFrames = GetUnloadStaggerFrames(pTransport->Type);

		if (staggerFrames <= 0)
			return true;

		const int frame = Unsorted::CurrentFrame;
		auto const it = s_lastUnloadFrame.find(pTransport);

		if (it != s_lastUnloadFrame.end() && frame - it->second < staggerFrames)
			return false;

		s_lastUnloadFrame[pTransport] = frame;

		Debug::Log("[Passengers.EnterDirections][stagger-unload] transport=%s(%p) frame=%d interval=%d\n",
			pTransport->Type->ID, static_cast<void*>(pTransport), frame, staggerFrames);

		return true;
	}

	void FinishUnload(UnitClass* pTransport)
	{
		if (pTransport)
			s_lastUnloadFrame.erase(pTransport);
	}

	void EnsureMovingTo(FootClass* pPassenger, AbstractClass* pDestination)
	{
		if (!pPassenger || !pDestination)
			return;

		if (pPassenger->Destination != pDestination)
			pPassenger->SetDestination(pDestination, true);

		const Mission mission = pPassenger->CurrentMission;

		// Even when the destination is already correct, a queued passenger may
		// have a non-moving mission. Kick it so everyone moves independently.
		if (mission != Mission::Enter && mission != Mission::Move)
		{
			pPassenger->QueueMission(Mission::Move, false);
			pPassenger->NextMission();
		}
	}

	bool IsOwnEntryDestination(UnitClass* pTransport, FootClass* pPassenger, CellClass* pAllowedCell)
	{
		AbstractClass* const pDestination = pPassenger->Destination;

		if (!pDestination)
			return true;

		if (pDestination == pTransport || pDestination == pAllowedCell)
			return true;

		// The engine may use a cell as the enter destination. Only the
		// transport cell or the selected allowed cell belong to this order.
		if (auto const pCell = abstract_cast<CellClass*>(pDestination))
		{
			return pCell == pTransport->GetCell()
				|| pCell == pAllowedCell;
		}

		// A different object/cell is a new player order; do not override it.
		return false;
	}

	void HandleEntry(UnitClass* pTransport, FootClass* pPassenger, int flags)
	{
		if (flags == None)
			return;

		// Already inside the cargo list: never run the entry logic again.
		if (IsPassengerInside(pTransport, pPassenger))
			return;

		// Respect a new Move/Stop/Guard order: only an Enter mission or a
		// queued passenger may keep boarding.
		if (pPassenger->CurrentMission != Mission::Enter
			&& pPassenger->QueueUpToEnter != pTransport)
			return;

		auto const pTransportCell = pTransport->GetCell();
		auto const pPassengerCell = pPassenger->GetCell();

		if (!pTransportCell || !pPassengerCell)
			return;

		auto const pAllowedCell = FindAllowedCell(pTransport, pPassenger, flags);

		// A new Move/Stop order replaces the boarding destination. Do not
		// drag the passenger back to the transport in that case.
		if (!IsOwnEntryDestination(pTransport, pPassenger, pAllowedCell))
		{
			s_parallelEntryTargets.erase(pPassenger);
			return;
		}

		// No usable allowed cell: at least walk to the transport instead of
		// freezing in place. The entry gate remains active.
		if (!pAllowedCell)
		{
			EnsureMovingTo(pPassenger, pTransport);
			return;
		}

		const bool canEnter = CanEnterNow(pTransport, pPassenger, true);

		if (Unsorted::CurrentFrame % 30 == 0)
		{
			const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
			const int relative = GetRelativeDirection(pTransport, pPassenger);

			Debug::Log("[Passengers.EnterDirections] %s flags=0x%02X facing=%d relative=%d pcell=(%d,%d) tcell=(%d,%d) allowed=(%d,%d) atVehicle=%d canEnter=%d current=%d/%d\n",
				pTransport->Type->ID, flags, facing, relative,
				pPassengerCell->MapCoords.X, pPassengerCell->MapCoords.Y,
				pTransportCell->MapCoords.X, pTransportCell->MapCoords.Y,
				pAllowedCell->MapCoords.X, pAllowedCell->MapCoords.Y,
				IsAtVehicle(pTransport, pPassenger) ? 1 : 0,
				canEnter ? 1 : 0,
				pTransport->Passengers.NumPassengers, pTransport->Type->Passengers);
		}

		// Direction-restricted transports never add cargo directly. The
		// engine owns Limbo / AddPassenger / the cargo chain; TryBeginEnter
		// only gates the engine's own entry paths.
		if (pPassengerCell == pAllowedCell)
			EnsureMovingTo(pPassenger, pTransport);
		else
			EnsureMovingTo(pPassenger, pAllowedCell);
	}
}

#pragma endregion

// Update after unit location update
DEFINE_HOOK(0x4DA8A0, FootClass_Update_AfterLocomotorProcess, 0x6)
{
	GET(FootClass* const, pThis, ESI);

	// Update laser trails after locomotor process, to ensure that the updated position is not the previous frame's position
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	pExt->UpdateLaserTrails();

	for (auto const& attachment : pExt->ChildAttachments)
		attachment->AI();

	// Passengers already inside (or limboed) must never run the entry logic
	// again: doing so would limbo/re-add them and corrupt the cargo list.
	if (pThis->Transporter)
	{
		PassengerEnterDirections::s_parallelEntryTargets.erase(pThis);
		return 0;
	}

	// A new Move/Stop/Guard order must be able to cancel the boarding attempt.
	const bool entering = pThis->CurrentMission == Mission::Enter
		|| pThis->QueueUpToEnter != nullptr;

	if (!entering)
	{
		PassengerEnterDirections::s_parallelEntryTargets.erase(pThis);
		return 0;
	}

	// The core part of the fast enter action. Fall back to the radio link when
	// QueueUpToEnter is not set, so every linked passenger still walks in.
	TechnoClass* pEntryTarget = pThis->CurrentMission == Mission::Enter
		? pThis->GetNthLink() : pThis->QueueUpToEnter;

	if (!pEntryTarget)
		pEntryTarget = pThis->GetNthLink();

	if (!pEntryTarget)
		pEntryTarget = abstract_cast<TechnoClass*>(pThis->Target);

	if (!pEntryTarget)
	{
		if (auto const it = PassengerEnterDirections::s_parallelEntryTargets.find(pThis);
			it != PassengerEnterDirections::s_parallelEntryTargets.end())
		{
			pEntryTarget = it->second;
		}
	}

	if (const auto pDest = abstract_cast<UnitClass*>(pEntryTarget))
	{
		// Cargo membership is the only reliable "already inside" check.
		if (PassengerEnterDirections::IsPassengerInside(pDest, pThis))
		{
			PassengerEnterDirections::s_parallelEntryTargets.erase(pThis);
			return 0;
		}

		const auto pType = pDest->Type;

		const int enterDirections = PassengerEnterDirections::Get(pType);

		if (enterDirections != PassengerEnterDirections::All
			&& pThis->WhatAmI() == AbstractType::Infantry)
		{
			PassengerEnterDirections::HandleEntry(pDest, pThis, enterDirections);

			if (pThis->Transporter)
				PassengerEnterDirections::s_parallelEntryTargets.erase(pThis);

			return 0;
		}

		if (pType->Passengers > 0 && TechnoTypeExt::ExtMap.Find(pType)->NoQueueUpToEnter.Get(RulesExt::Global()->NoQueueUpToEnter))
		{
			const auto absType = pThis->WhatAmI();

			if (absType == AbstractType::Infantry || absType == AbstractType::Unit)
			{
				if (IsAtVehicle(pDest, pThis) && CanEnterNow(pDest, pThis, true))
				{
					DoEnterNow(pDest, pThis);

					if (pThis->Transporter)
						PassengerEnterDirections::s_parallelEntryTargets.erase(pThis);
				}
				else if (!pDest->OnBridge && pThis->Destination != pDest)
				{
					// Every passenger walks to the transport independently. They
					// enter as soon as they reach the vehicle, so multiple
					// passengers board in parallel instead of waiting in a queue.
					PassengerEnterDirections::EnsureMovingTo(pThis, pDest);
				}
			}
		}
	}

	return 0;
}

// Rewrite from 0x4835D5/0x74004B, replace check pThis->GetCell()->LandType != LandType::Water
static inline bool CanUnloadNow(UnitClass* pTransport, FootClass* pPassenger)
{
	if (TechnoTypeExt::ExtMap.Find(pTransport->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload))
		return GroundType::Array[static_cast<int>(pTransport->GetCell()->LandType)].Cost[static_cast<int>(pPassenger->GetTechnoType()->SpeedType)] != 0.0;

	return pTransport->GetCell()->LandType != LandType::Water;
}

static inline bool ShouldFastUnload(UnitTypeClass* pType)
{
	return TechnoTypeExt::ExtMap.Find(pType)->NoQueueUpToUnload.Get(RulesExt::Global()->NoQueueUpToUnload)
		|| PassengerEnterDirections::Get(pType) != PassengerEnterDirections::All;
}

// The unload loop computes the world direction in ECX and then indexes
// Unsorted::AdjacentCell with it. Rewriting ECX is safe here (6-byte
// instruction boundary), unlike a 2-byte hook that would corrupt the loop.
DEFINE_HOOK(0x73D934, UnitClass_Mission_Unload_DirectionGate, 0x7)
{
	GET(UnitClass*, pThis, ESI);
	GET(int, direction, ECX);

	const int flags = PassengerEnterDirections::Get(pThis->Type);

	if (flags == PassengerEnterDirections::All)
		return 0;

	const int facing = static_cast<int>(pThis->PrimaryFacing.Current().GetFacing<8>());
	const int relative = (direction - facing + 8) % 8;

	if (relative >= 0 && (flags & (1 << relative)) != 0)
		return 0;

	int allowedDirection = -1;

	for (int offset = 0; offset < 8; ++offset)
	{
		const int candidate = (facing + offset) % 8;

		if (flags & (1 << offset))
		{
			allowedDirection = candidate;
			break;
		}
	}

	if (allowedDirection >= 0)
		R->ECX(allowedDirection);

	Debug::Log("[Passengers.EnterDirections][unload] transport=%s dir=%d facing=%d relative=%d result=%s\n",
		pThis->Type->ID, direction, facing, relative,
		allowedDirection >= 0 ? "redirect" : "no_allowed_direction");

	return 0;
}

namespace TransportUnloadTemp
{
	bool ShouldPlaySound = false;
}

// Interrupted due to insufficient location or other reasons
DEFINE_HOOK(0x73DC9C, UnitClass_Mission_Unload_NoQueueUpToUnloadBreak, 0xA)
{
	enum { SkipGameCode = 0x73E289 };

	GET(UnitClass* const, pThis, ESI);
	GET(FootClass* const, pPassenger, EDI);

	// Restore vanilla function
	pPassenger->Undiscover();

	// Clean up the unload space
	const bool alt = pThis->OnBridge;
	const auto pCell = pThis->GetCell();
	const auto coord = pCell->GetCoords();

	for (int i = 0; i < 8; ++i)
	{
		const auto pAdjCell = pCell->GetNeighbourCell(static_cast<FacingType>(i));
		const auto pTechno = pAdjCell->FindTechnoNearestTo(Point2D::Empty, alt, pThis);

		if (pTechno && pTechno->Owner->IsAlliedWith(pThis))
			pAdjCell->ScatterContent(coord, true, true, alt);
	}

	// Play the sound when interrupted
	if (TransportUnloadTemp::ShouldPlaySound) // Only when NoQueueUpToUnload enabled
	{
		TransportUnloadTemp::ShouldPlaySound = false;
		const auto pType = pThis->Type;

		if (ShouldFastUnload(pType))
			VoxClass::PlayAtPos(pType->LeaveTransportSound, &pThis->Location);
	}

	return SkipGameCode;
}

// Within a single frame, cycle to get off the car
DEFINE_HOOK(0x73DC1E, UnitClass_Mission_Unload_NoQueueUpToUnloadLoop, 0xA)
{
	enum { UnloadLoop = 0x73D8CB, UnloadReturn = 0x73E289, NoUnloadReturn = 0x73D8AA };

	GET(UnitClass* const, pThis, ESI);

	const auto pType = pThis->Type;
	const auto pPassenger = pThis->Passengers.GetFirstPassenger();

	if (ShouldFastUnload(pType))
	{
		if (!pPassenger || pThis->Passengers.NumPassengers <= pThis->NonPassengerCount)
		{
			PassengerEnterDirections::FinishUnload(pThis);

			// If unloading is required within one frame, the sound will only be played when the last passenger leaves
			VoxClass::PlayAtPos(pType->LeaveTransportSound, &pThis->Location);
			TransportUnloadTemp::ShouldPlaySound = false;
			return UnloadReturn;
		}
		else if (!CanUnloadNow(pThis, pPassenger))
		{
			VoxClass::PlayAtPos(pType->LeaveTransportSound, &pThis->Location);
			TransportUnloadTemp::ShouldPlaySound = false;
			pThis->MissionStatus = 0; // Retry
			return NoUnloadReturn;
		}

		// Restricted transports release one passenger per configured interval.
		if (!PassengerEnterDirections::TryBeginUnload(pThis))
		{
			pThis->MissionStatus = 0; // Retry next frame
			return NoUnloadReturn;
		}

		TransportUnloadTemp::ShouldPlaySound = true;
		R->EBX(0); // Reset
		return UnloadLoop;
	}

	// PlayAtPos has already handled the situation where Sound is less than 0 internally, so unnecessary checks will be skipped
	VoxClass::PlayAtPos(pType->LeaveTransportSound, &pThis->Location);

	if (!pPassenger || CanUnloadNow(pThis, pPassenger))
		return UnloadReturn;

	pThis->MissionStatus = 0; // Retry
	return NoUnloadReturn;
}

#pragma endregion

#pragma region TransportFix

DEFINE_HOOK(0x51D45B, InfantryClass_Scatter_NoProcess, 0x6)
{
	enum { SkipGameCode = 0x51D47B };

	REF_STACK(const int, addr, STACK_OFFSET(0x50, 0));
	// Skip process in InfantryClass::UpdatePosition which can create invisible barrier
	return (addr == 0x51A4B5) ? SkipGameCode : 0;
}

DEFINE_HOOK(0x4D92BF, FootClass_Mission_Enter_CheckLink, 0x5)
{
	enum { NextAction = 0x4D92ED, NotifyUnlink = 0x4D92CE, DoNothing = 0x4D946C };

	GET(UnitClass* const, pThis, ESI);
	GET(const RadioCommand, answer, EAX);
	// Restore vanilla check
	if (pThis->IsTether || answer == RadioCommand::AnswerPositive)
		return NextAction;
	// The link should not be disconnected while the transporter is in motion (passengers waiting to enter),
	// as this will result in the first passenger not getting on board
	return answer == RadioCommand::RequestLoading ? DoNothing : NotifyUnlink;
}

DEFINE_HOOK(0x73769E, UnitClass_ReceiveCommand_NoEnterOnBridge, 0x6)
{
	enum { NoEnter = 0x73780F };

	GET(UnitClass* const, pThis, ESI);
	GET(TechnoClass* const, pCall, EDI);
	// If both the transport vehicle and passengers are on the bridge, they should not board
	return pThis->OnBridge && pCall->OnBridge ? NoEnter : 0;
}

DEFINE_HOOK(0x70D842, FootClass_UpdateEnter_NoMoveToBridge, 0x5)
{
	enum { NoMove = 0x70D84F };

	GET(FootClass*, pThis, ESI);
	GET(TechnoClass*, pEnter, EDI);

	if (pThis->WhatAmI() == AbstractType::Infantry)
	{
		if (auto const pTransport = abstract_cast<UnitClass*>(pEnter))
		{
			const int flags = PassengerEnterDirections::Get(pTransport->Type);

			if (flags != PassengerEnterDirections::All)
			{
				// Already touching the transport: let vanilla/per-cell entry run.
				if (pThis->GetCell() == pTransport->GetCell())
					return 0;

				if (flags != PassengerEnterDirections::None)
				{
					// Keep the target independently of the engine queue.
					PassengerEnterDirections::s_parallelEntryTargets[pThis] = pTransport;
					pThis->QueueUpToEnter = nullptr;
					pThis->SetArchiveTarget(nullptr);

					if (auto const pCell = PassengerEnterDirections::FindAllowedCell(pTransport, pThis, flags))
					{
						AbstractClass* pDestination = pThis->GetCell() == pCell
							? static_cast<AbstractClass*>(pTransport) : pCell;
						PassengerEnterDirections::EnsureMovingTo(pThis, pDestination);
					}
				}

				// Never let the vanilla path send the passenger to the transport cell
				// from a disallowed side.
				return NoMove;
			}
		}
	}

	// If the transport vehicle is on the bridge, passengers should wait in place for the transport vehicle to arrive
	return pEnter->OnBridge && (pEnter->WhatAmI() == AbstractType::Unit && static_cast<UnitClass*>(pEnter)->Type->Passengers > 0) ? NoMove : 0;
}

DEFINE_HOOK(0x70D910, FootClass_QueueEnter_NoMoveToBridge, 0x5)
{
	enum { NoMove = 0x70D977 };

	GET(FootClass*, pThis, ESI);
	GET(TechnoClass*, pEnter, EAX);

	if (pThis->WhatAmI() == AbstractType::Infantry)
	{
		if (auto const pTransport = abstract_cast<UnitClass*>(pEnter))
		{
			const int flags = PassengerEnterDirections::Get(pTransport->Type);

			if (flags != PassengerEnterDirections::All)
			{
				// Already touching the transport: let vanilla/per-cell entry run.
				if (pThis->GetCell() == pTransport->GetCell())
					return 0;

				if (flags != PassengerEnterDirections::None)
				{
					// Keep the target independently of the engine queue.
					PassengerEnterDirections::s_parallelEntryTargets[pThis] = pTransport;
					pThis->QueueUpToEnter = nullptr;
					pThis->SetArchiveTarget(nullptr);

					if (auto const pCell = PassengerEnterDirections::FindAllowedCell(pTransport, pThis, flags))
					{
						AbstractClass* pDestination = pThis->GetCell() == pCell
							? static_cast<AbstractClass*>(pTransport) : pCell;
						PassengerEnterDirections::EnsureMovingTo(pThis, pDestination);
					}
				}

				return NoMove;
			}
		}
	}

	// If the transport vehicle is on the bridge, passengers should wait in place for the transport vehicle to arrive
	return pEnter->OnBridge && (pEnter->WhatAmI() == AbstractType::Unit && static_cast<UnitClass*>(pEnter)->Type->Passengers > 0) ? NoMove : 0;
}

DEFINE_HOOK(0x7196BB, TeleportLocomotionClass_Process_MarkDown, 0xA)
{
	enum { SkipGameCode = 0x7196C5 };

	GET(FootClass*, pLinkedTo, ECX);
	// When Teleport units board transport vehicles on the bridge, the lack of this repair can lead to numerous problems
	// An impassable invisible barrier will be generated on the bridge (the object linked list of the cell will leave it)
	// And the transport vehicle will board on the vehicle itself (BFRT Passenger:..., BFRT)
	// If any infantry attempts to pass through this position on the bridge later, it will cause the game to freeze
	auto shouldMarkDown = [pLinkedTo]()
	{
		if (pLinkedTo->GetCurrentMission() != Mission::Enter)
			return true;

		const auto pEnter = pLinkedTo->GetNthLink();

		return (!pEnter || pEnter->GetTechnoType()->Passengers <= 0);
	};

	if (shouldMarkDown())
		pLinkedTo->Mark(MarkType::Down);

	return SkipGameCode;
}

#pragma endregion

#pragma region AmphibiousEnterAndUnload

// Related fix
DEFINE_HOOK(0x4B08EF, DriveLocomotionClass_Process_CheckUnload, 0x5)
{
	enum { SkipGameCode = 0x4B078C, ContinueProcess = 0x4B0903 };

	GET(ILocomotion* const, iloco, ESI);

	__assume(iloco != nullptr);

	const auto pFoot = static_cast<LocomotionClass*>(iloco)->LinkedTo;

	if (pFoot->GetCurrentMission() != Mission::Unload)
		return ContinueProcess;

	return (pFoot->GetTechnoType()->Passengers > 0 && pFoot->Passengers.GetFirstPassenger()) ? ContinueProcess : SkipGameCode;
}

DEFINE_HOOK(0x69FFB6, ShipLocomotionClass_Process_CheckUnload, 0x5)
{
	enum { SkipGameCode = 0x69FE39, ContinueProcess = 0x69FFCA };

	GET(ILocomotion* const, iloco, ESI);

	__assume(iloco != nullptr);

	const auto pFoot = static_cast<LocomotionClass*>(iloco)->LinkedTo;

	if (pFoot->GetCurrentMission() != Mission::Unload)
		return ContinueProcess;

	return (pFoot->GetTechnoType()->Passengers > 0 && pFoot->Passengers.GetFirstPassenger()) ? ContinueProcess : SkipGameCode;
}

// Rewrite from 0x718505
DEFINE_HOOK_AGAIN(0x7190B0, TeleportLocomotionClass_MovingTo_ReplaceMovementZone, 0x6)
DEFINE_HOOK(0x718F1E, TeleportLocomotionClass_MovingTo_ReplaceMovementZone, 0x6)
{
	GET(TechnoTypeClass* const, pType, EAX);

	auto movementZone = pType->MovementZone;

	if (movementZone == MovementZone::Fly || movementZone == MovementZone::Destroyer)
		movementZone = MovementZone::Normal;
	else if (movementZone == MovementZone::AmphibiousDestroyer)
		movementZone = MovementZone::Amphibious;

	R->EBP(movementZone);
	return R->Origin() + 0x6;
}

// Enter building
DEFINE_JUMP(LJMP, 0x43C38D, 0x43C3FF); // Skip amphibious and naval check if no Ares

// Enter unit
DEFINE_HOOK(0x73796B, UnitClass_ReceiveCommand_AmphibiousEnter, 0x7)
{
	enum { ContinueCheck = 0x737990, MoveToPassenger = 0x737974 };

	GET(UnitClass* const, pThis, ESI);

	if (pThis->OnBridge)
		return MoveToPassenger;

	if (TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousEnter.Get(RulesExt::Global()->AmphibiousEnter))
		return ContinueCheck;

	GET(CellClass* const, pCell, EBP);

	return (pCell->LandType != LandType::Water) ? ContinueCheck : MoveToPassenger;
}

// Unit unload

DEFINE_HOOK(0x7400B5, UnitClass_MouseOverObject_AmphibiousUnload, 0x7)
{
	enum { ContinueCheck = 0x7400C6, CannotUnload = 0x7400BE };

	GET(CellClass* const, pCell, EBX);

	if (pCell->LandType == LandType::Water) // I don't know why WW made a reverse judgment here? Because of the coast/beach?
		return ContinueCheck;

	GET(UnitClass* const, pThis, ESI);

	return TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload) ? ContinueCheck : CannotUnload;
}

DEFINE_HOOK(0x70107A, TechnoClass_CanDeploySlashUnload_AmphibiousUnload, 0x7)
{
	enum { ContinueCheck = 0x701087, CannotUnload = 0x700DCE };

	GET(CellClass* const, pCell, EBP);

	if (pCell->LandType != LandType::Water)
		return ContinueCheck;

	GET(UnitClass* const, pThis, ESI);

	return TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload) ? ContinueCheck : CannotUnload;
}

DEFINE_HOOK(0x73D769, UnitClass_Mission_Unload_AmphibiousUnload, 0x7)
{
	enum { MoveToLand = 0x73D772, UnloadCheck = 0x73D7E4 };

	GET(UnitClass* const, pThis, ESI);

	const auto pPassenger = pThis->Passengers.GetFirstPassenger();

	return (!pPassenger || CanUnloadNow(pThis, pPassenger)) ? UnloadCheck : MoveToLand;
}

DEFINE_HOOK(0x73D7AB, UnitClass_Mission_Unload_FindUnloadPosition, 0x5)
{
	GET(UnitClass* const, pThis, ESI);

	if (TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload))
	{
		if (const auto pPassenger = pThis->Passengers.GetFirstPassenger())
		{
			REF_STACK(SpeedType, speedType, STACK_OFFSET(0xBC, -0xB4));
			REF_STACK(MovementZone, movementZone, STACK_OFFSET(0xBC, -0xAC));

			const auto pType = pPassenger->GetTechnoType();
			speedType = pType->SpeedType; // Replace hard code SpeedType::Wheel
			movementZone = pType->MovementZone; // Replace hard code MovementZone::Normal
		}
	}

	return 0;
}

DEFINE_HOOK(0x73D7B7, UnitClass_Mission_Unload_CheckInvalidCell, 0x6)
{
	enum { CannotUnload = 0x73D87F };

	GET(const CellStruct, cell, EAX);

	return cell != CellStruct::Empty ? 0 : CannotUnload;
}

DEFINE_HOOK(0x740C9C, UnitClass_GetUnloadDirection_CheckUnloadPosition, 0x7)
{
	GET(UnitClass* const, pThis, EDI);

	if (TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload))
	{
		if (const auto pPassenger = pThis->Passengers.GetFirstPassenger())
		{
			GET(const int, speedType, EDX);
			R->EDX(speedType + static_cast<int>(pPassenger->GetTechnoType()->SpeedType)); // Replace hard code SpeedType::Foot
		}
	}

	return 0;
}

DEFINE_HOOK(0x73DAD8, UnitClass_Mission_Unload_PassengerLeavePosition, 0x5)
{
	GET(UnitClass* const, pThis, ESI);

	if (TechnoTypeExt::ExtMap.Find(pThis->Type)->AmphibiousUnload.Get(RulesExt::Global()->AmphibiousUnload))
	{
		GET(FootClass* const, pPassenger, EDI);
		REF_STACK(MovementZone, movementZone, STACK_OFFSET(0xBC, -0xAC));
		movementZone = pPassenger->GetTechnoType()->MovementZone; // Replace hard code MovementZone::Normal
	}

	return 0;
}

#pragma endregion
