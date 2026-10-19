#include "Body.h"
#include "TransportEntry.h"

#include <Ext/Scenario/Body.h>
#include <Utilities/AresHelper.h>
#include <Utilities/AresFunctions.h>
#include <Utilities/Debug.h>

#include <CCINIClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <UnitTypeClass.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

// Shared pre-entry setup used by both the engine's EnterTransport path and
// the manual fast-entry path. Has to run before the passenger is attached.
static inline void PreparePassengerEntry(TechnoClass* pTransport, FootClass* pPassenger)
{
	if (!pTransport || !pPassenger)
		return;

	auto const pType = pPassenger->GetTechnoType();
	auto const pExt = TechnoExt::ExtMap.Find(pPassenger);
	auto const whatAmI = pPassenger->WhatAmI();
	auto const pTransTypeExt = TechnoExt::ExtMap.Find(pTransport)->TypeExtData;

	if (pTransTypeExt->Passengers_SyncOwner && pTransTypeExt->Passengers_SyncOwner_RevertOnExit)
		pExt->OriginalPassengerOwner = pPassenger->Owner;

	if (whatAmI != AbstractType::Aircraft && whatAmI != AbstractType::Building
		&& pType->Ammo > 0 && pExt->TypeExtData->ReloadInTransport)
	{
		ScenarioExt::Global()->TransportReloaders.push_back(pExt);
	}
}

DEFINE_HOOK(0x71067B, TechnoClass_EnterTransport, 0x7)
{
	enum { SkipGameCode = 0x7106D5 };

	GET(TechnoClass*, pThis, ESI);
	GET(FootClass*, pPassenger, EDI);

	if (pPassenger)
	{
		if (auto const pTransport = abstract_cast<UnitClass*>(pThis))
		{
			// Never let the engine add a passenger that is already attached.
			// This is the general duplicate guard; it also covers the manual
			// fast-entry path racing with the engine path in the same frame.
			// It does not block normal scenario/savegame cargo restore: a
			// passenger that is not yet in the list is allowed through.
			if (PassengerDirections::IsPassengerInside(pTransport, pPassenger))
			{
				if (Phobos::Config::DebugGCDiagnostics)
				{
					Debug::Log("[Passengers.EnterDirections][cargo] duplicate add blocked transport=%s(%u) passenger=%p id=%u transporter=%p passengers=%d frame=%d\n",
						pTransport->Type->ID, pTransport->UniqueID, static_cast<void*>(pPassenger),
						pPassenger->UniqueID, static_cast<void*>(pPassenger->Transporter),
						pTransport->Passengers.NumPassengers, Unsorted::CurrentFrame);
				}

				return SkipGameCode;
			}

			if (!PassengerDirections::IsEntryApproachAllowed(pTransport, pPassenger))
			{
				if (Phobos::Config::DebugGCDiagnostics)
				{
					Debug::Log("[Passengers.EnterDirections][direction] engine add blocked transport=%s(%u) passenger=%p cell=(%d,%d) last=(%d,%d) frame=%d\n",
						pTransport->Type->ID, pTransport->UniqueID, static_cast<void*>(pPassenger),
						pPassenger->GetCell() ? pPassenger->GetCell()->MapCoords.X : -1,
						pPassenger->GetCell() ? pPassenger->GetCell()->MapCoords.Y : -1,
						pPassenger->LastMapCoords.X, pPassenger->LastMapCoords.Y,
						Unsorted::CurrentFrame);
				}

				return SkipGameCode;
			}
		}
	}

	PreparePassengerEntry(pThis, pPassenger);

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

// Fast entry must not limbo a passenger that is still approaching from a
// distance. Restricted transports use a cell based approach instead.
constexpr int PassengerAtVehicleDistance = 96; // leptons

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
static inline bool DoEnterNow(UnitClass* pTransport, FootClass* pPassenger)
{
	if (!pTransport || !pPassenger)
		return false;

	if (pPassenger->Transporter)
		return false;

	// Never add a passenger that is already in the cargo list.
	if (PassengerDirections::IsPassengerInside(pTransport, pPassenger))
		return false;

	const auto pTransportTypeExt = TechnoTypeExt::ExtMap.Find(pTransport->Type);
	const bool bySize = pTransportTypeExt->Passengers_BySize;
	const int maxSize = pTransport->Type->Passengers;
	const int passengerSize = static_cast<int>(pPassenger->GetTechnoType()->Size);
	const int predictedSize = bySize
		? (pTransport->Passengers.GetTotalSize() + passengerSize)
		: (pTransport->Passengers.NumPassengers + 1);

	// Hard capacity guard: never append past the transport's passenger limit.
	if (maxSize > 0 && predictedSize > maxSize)
		return false;

	// Stateless direction guard. The approach controller already selected an
	// allowed cell; this protects direct callers as well.
	if (!PassengerDirections::IsEntryAllowedByDirection(pTransport, pPassenger))
		return false;

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

	// Keep Passengers.SyncOwner / ReloadInTransport working on the manual
	// fast-entry path too.
	PreparePassengerEntry(pTransport, pPassenger);

	pTransport->AddPassenger(pPassenger); // Don't swap order casually, very very important
	pPassenger->Transporter = pTransport;

	if (pTransport->Type->OpenTopped)
		pTransport->EnteredOpenTopped(pPassenger);

	if (pPassengerType->OpenTopped)
		pPassenger->SetTargetForPassengers(nullptr);

	pPassenger->Undiscover();
	return true;
}

#pragma region PassengerDirections

namespace PassengerDirections
{
		constexpr int MaxDirections = 8;

		struct EntryState
		{
			DWORD TransportID = 0;
			CellStruct ApproachCell = CellStruct::Empty;
			CellStruct LastPassengerCell = CellStruct::Empty;
			CellStruct PreviousCell = CellStruct::Empty;
			CellStruct TransportCell = CellStruct::Empty;
			int Relative = -1;
			int Facing = -1;
			int LastProgressFrame = 0;
			int LastMoveFrame = 0;
			bool VanillaEntering = false;
		};

		// Transient state only. Not serialized; cleared on scenario load.
		// Keyed by UniqueID so a recycled object pointer cannot inherit stale
		// boarding state.
		std::unordered_map<DWORD, EntryState> s_entryStates;
		// transport UniqueID -> reserved staging cell key -> passenger UniqueID.
		// A fan of staging cells is needed so a whole group can walk at once.
		std::unordered_map<DWORD, std::unordered_map<int, DWORD>> s_reservations;
		std::unordered_map<DWORD, int> s_lastEnterFrame;
		std::unordered_map<DWORD, int> s_lastUnloadFrame;
		// transport UniqueID -> currently admitted passenger UniqueID (0 = none)
		std::unordered_map<DWORD, DWORD> s_activeEntrant;
		std::unordered_map<DWORD, TechnoClass*> s_liveTechnos;
		int s_liveFrame = -1;
		ScenarioClass* s_lastScenario = nullptr;

		inline int CellKey(const CellStruct& cell)
		{
			return static_cast<int>(
				(static_cast<unsigned int>(static_cast<unsigned short>(cell.X)) << 16)
				| static_cast<unsigned int>(static_cast<unsigned short>(cell.Y)));
		}

		inline bool LogEnabled()
		{
			return Phobos::Config::DebugGCDiagnostics;
		}

		inline int GetStagger(UnitTypeClass* pType, bool unload)
		{
			if (!pType)
				return 0;

			const auto pExt = TechnoTypeExt::ExtMap.Find(pType);
			return unload
				? static_cast<int>(pExt->Passengers_UnloadStaggerFrames)
				: static_cast<int>(pExt->Passengers_EnterStaggerFrames);
		}

		void ReleaseReservation(DWORD transportID, DWORD passengerID)
		{
			if (!transportID || !passengerID)
				return;

			auto const it = s_reservations.find(transportID);

			if (it == s_reservations.end())
				return;

			auto& cells = it->second;

			for (auto cell = cells.begin(); cell != cells.end();)
			{
				if (cell->second == passengerID)
					cell = cells.erase(cell);
				else
					++cell;
			}
		}

		void ClearPassengerState(FootClass* pPassenger)
		{
			if (!pPassenger)
				return;

			auto const it = s_entryStates.find(pPassenger->UniqueID);

			if (it == s_entryStates.end())
				return;

			ReleaseReservation(it->second.TransportID, pPassenger->UniqueID);

			if (auto const active = s_activeEntrant.find(it->second.TransportID);
				active != s_activeEntrant.end() && active->second == pPassenger->UniqueID)
			{
				s_activeEntrant.erase(active);
			}

			s_entryStates.erase(it);
		}

		void CancelPassenger(FootClass* pPassenger, const char* reason)
		{
			if (!pPassenger)
				return;

			ClearPassengerState(pPassenger);

			pPassenger->QueueUpToEnter = nullptr;
			pPassenger->SetArchiveTarget(nullptr);
			pPassenger->SetDestination(nullptr, true);
			pPassenger->QueueMission(Mission::None, false);
			pPassenger->NextMission();

			if (Phobos::Config::DebugGCDiagnostics)
			{
				Debug::Log("[Passengers.EnterDirections] cancel passenger=%p id=%u reason=%s frame=%d\n",
					static_cast<void*>(pPassenger), pPassenger->UniqueID, reason,
					Unsorted::CurrentFrame);
			}
		}

		void PruneStates()
		{
			auto const& live = s_liveTechnos;

			for (auto it = s_entryStates.begin(); it != s_entryStates.end();)
			{
				if (!live.contains(it->first) || !live.contains(it->second.TransportID))
				{
					ReleaseReservation(it->second.TransportID, it->first);
					it = s_entryStates.erase(it);
				}
				else
				{
					++it;
				}
			}

			for (auto it = s_reservations.begin(); it != s_reservations.end();)
			{
				if (!live.contains(it->first))
					it = s_reservations.erase(it);
				else
					++it;
			}

			for (auto it = s_lastEnterFrame.begin(); it != s_lastEnterFrame.end();)
			{
				if (!live.contains(it->first))
					it = s_lastEnterFrame.erase(it);
				else
					++it;
			}

			for (auto it = s_lastUnloadFrame.begin(); it != s_lastUnloadFrame.end();)
			{
				if (!live.contains(it->first))
					it = s_lastUnloadFrame.erase(it);
				else
					++it;
			}

			for (auto it = s_activeEntrant.begin(); it != s_activeEntrant.end();)
			{
				if (!live.contains(it->first) || !live.contains(it->second))
					it = s_activeEntrant.erase(it);
				else
					++it;
			}
		}

		int RelativeFromCell(UnitClass* pTransport, CellClass* pCell, int facingOverride = -1)
		{
			if (!pTransport || !pCell)
				return -1;

			auto const pTransportCell = pTransport->GetCell();

			if (!pTransportCell)
				return -1;

			const int dx = pCell->MapCoords.X - pTransportCell->MapCoords.X;
			const int dy = pCell->MapCoords.Y - pTransportCell->MapCoords.Y;

			for (int dir = 0; dir < MaxDirections; ++dir)
			{
				if (Unsorted::AdjacentCell[dir].X != dx || Unsorted::AdjacentCell[dir].Y != dy)
					continue;

				const int facing = facingOverride >= 0
					? facingOverride
					: static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
				return (dir - facing + MaxDirections) % MaxDirections;
			}

			return -1;
		}

		bool IsValidApproachCell(UnitClass* pTransport, FootClass* pPassenger, CellClass* pCell)
		{
			if (!pTransport || !pPassenger || !pCell)
				return false;

			auto const pTransportCell = pTransport->GetCell();

			if (!pTransportCell)
				return false;

			// Same bridge layer. Ground height differences (ramps, slopes) are
			// handled by the locomotor and must not block boarding.
			if (pCell->ContainsBridge() != pTransportCell->ContainsBridge())
				return false;

			if (pTransportCell->ContainsBridge() && pCell->GetLevel() != pTransportCell->GetLevel())
				return false;

			const auto speedType = pPassenger->GetTechnoType()->SpeedType;

			if (GroundType::Array[static_cast<int>(pCell->LandType)].Cost[static_cast<int>(speedType)] <= 0.0)
				return false;

			if (auto const pContent = pCell->GetContent())
			{
				if (pContent != pTransport && pContent != pPassenger)
				{
					const auto absType = pContent->WhatAmI();

					// Buildings, vehicles and landed aircraft block the cell;
					// infantry may share.
					if (absType == AbstractType::Building
						|| absType == AbstractType::Unit
						|| absType == AbstractType::Aircraft)
						return false;
				}
			}

			return true;
		}

		bool IsValidUnloadCell(UnitClass* pTransport, FootClass* pPassenger, CellClass* pCell)
		{
			if (!pTransport || !pPassenger || !pCell)
				return false;

			auto const pTransportCell = pTransport->GetCell();

			if (!pTransportCell)
				return false;

			if (pCell->ContainsBridge() != pTransportCell->ContainsBridge())
				return false;

			if (pTransportCell->ContainsBridge() && pCell->GetLevel() != pTransportCell->GetLevel())
				return false;

			const auto speedType = pPassenger->GetTechnoType()->SpeedType;

			if (GroundType::Array[static_cast<int>(pCell->LandType)].Cost[static_cast<int>(speedType)] <= 0.0)
				return false;

			if (auto const pContent = pCell->GetContent())
			{
				if (pContent != pTransport && pContent != pPassenger
					&& pContent->WhatAmI() == AbstractType::Building)
					return false;
			}

			return true;
		}

		void GetBehindVector(UnitClass* pTransport, int flags, int facing, int& sumX, int& sumY)
		{
			sumX = 0;
			sumY = 0;

			if (!pTransport)
				return;

			if (facing < 0)
				facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());

			for (int relative = 0; relative < MaxDirections; ++relative)
			{
				if (!(flags & (1 << relative)))
					continue;

				auto const& dir = Unsorted::AdjacentCell[(facing + relative) % MaxDirections];
				sumX += dir.X;
				sumY += dir.Y;
			}
		}

		bool IsBehind(UnitClass* pTransport, const CellStruct& cell, int flags, int facing)
		{
			auto const pTransportCell = pTransport ? pTransport->GetCell() : nullptr;

			if (!pTransportCell)
				return false;

			int sumX = 0;
			int sumY = 0;
			GetBehindVector(pTransport, flags, facing, sumX, sumY);

			if (sumX == 0 && sumY == 0)
				return true;

			const int dx = cell.X - pTransportCell->MapCoords.X;
			const int dy = cell.Y - pTransportCell->MapCoords.Y;
			return dx * sumX + dy * sumY > 0;
		}

		bool IsFrontCell(UnitClass* pTransport, CellClass* pCell, int flags, int facing = -1)
		{
			if (!pTransport || !pCell)
				return false;

			const int relative = RelativeFromCell(pTransport, pCell, facing);
			return relative >= 0 && (flags & (1 << relative)) != 0;
		}

		// Finds a staging cell for the passenger. frontOnly restricts the
		// search to the adjacent allowed entry cells; otherwise a fan of cells
		// behind the allowed side is used so a whole group can walk at once.
		CellClass* FindStagingCell(UnitClass* pTransport, FootClass* pPassenger, int flags, bool frontOnly, int facing)
		{
			auto const pTransportCell = pTransport ? pTransport->GetCell() : nullptr;

			if (!pTransportCell)
				return nullptr;

			auto const pPassengerCell = pPassenger ? pPassenger->GetCell() : nullptr;
			int sumX = 0;
			int sumY = 0;
			GetBehindVector(pTransport, flags, facing, sumX, sumY);

			auto& cells = s_reservations[pTransport->UniqueID];

			CellClass* pBest = nullptr;
			int bestScore = std::numeric_limits<int>::max();

			constexpr int Radius = 4;

			for (int dy = -Radius; dy <= Radius; ++dy)
			{
				for (int dx = -Radius; dx <= Radius; ++dx)
				{
					if (dx == 0 && dy == 0)
						continue;

					CellStruct coords {};
					coords.X = static_cast<short>(pTransportCell->MapCoords.X + dx);
					coords.Y = static_cast<short>(pTransportCell->MapCoords.Y + dy);

					auto const pCell = MapClass::Instance.TryGetCellAt(coords);

					if (!pCell)
						continue;

					if (frontOnly)
					{
						if (!IsFrontCell(pTransport, pCell, flags, facing))
							continue;
					}
					else if ((sumX != 0 || sumY != 0) && dx * sumX + dy * sumY <= 0)
					{
						// Only cells behind the allowed side are staging cells.
						continue;
					}

					if (!IsValidApproachCell(pTransport, pPassenger, pCell))
						continue;

					const int key = CellKey(pCell->MapCoords);
					auto const reserved = cells.find(key);

					if (reserved != cells.end() && reserved->second != pPassenger->UniqueID)
						continue;

					const int distanceToTransport = std::abs(dx) + std::abs(dy);
					const int distanceToPassenger = pPassengerCell
						? std::abs(coords.X - pPassengerCell->MapCoords.X)
							+ std::abs(coords.Y - pPassengerCell->MapCoords.Y)
						: 0;
					const int score = distanceToTransport * 16 + distanceToPassenger;

					if (score < bestScore)
					{
						bestScore = score;
						pBest = pCell;
					}
				}
			}

			return pBest;
		}

		void AssignApproachCell(UnitClass* pTransport, FootClass* pPassenger, CellClass* pCell,
			int relative, int facing, EntryState& state)
		{
			if (!pTransport || !pPassenger || !pCell)
				return;

			ReleaseReservation(pTransport->UniqueID, pPassenger->UniqueID);

			auto& cells = s_reservations[pTransport->UniqueID];
			const int key = CellKey(pCell->MapCoords);

			if (auto const it = cells.find(key); it == cells.end() || it->second == pPassenger->UniqueID)
				cells[key] = pPassenger->UniqueID;

			state.TransportID = pTransport->UniqueID;
			state.TransportCell = pTransport->GetCell()->MapCoords;
			state.ApproachCell = pCell->MapCoords;
			state.Relative = relative;
			state.Facing = facing;
			state.LastMoveFrame = 0;
			state.VanillaEntering = false;

			if (LogEnabled())
			{
				auto const pTransportCell = pTransport->GetCell();

				Debug::Log("[Passengers.EnterDirections] assign transport=%s(%u) passenger=%u cell=(%d,%d) relative=%d facing=%d front=%d frame=%d\n",
					pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID,
					pCell->MapCoords.X, pCell->MapCoords.Y, relative, facing,
					IsFrontCell(pTransport, pCell, static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections), facing) ? 1 : 0,
					Unsorted::CurrentFrame);

				Debug::Log("[Passengers.EnterDirections] transport-state transport=%s(%u) mission=%d destination=%p target=%p facing=%d cell=(%d,%d) frame=%d\n",
					pTransport->Type->ID, pTransport->UniqueID, static_cast<int>(pTransport->CurrentMission),
					static_cast<void*>(pTransport->Destination), static_cast<void*>(pTransport->Target),
					static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>()),
					pTransportCell ? pTransportCell->MapCoords.X : -1,
					pTransportCell ? pTransportCell->MapCoords.Y : -1,
					Unsorted::CurrentFrame);
			}
		}

		bool IsAtApproach(UnitClass* pTransport, FootClass* pPassenger, EntryState& state, int flags,
			CellClass*& pApproach)
		{
			pApproach = nullptr;

			if (state.ApproachCell == CellStruct::Empty)
				return false;

			pApproach = MapClass::Instance.TryGetCellAt(state.ApproachCell);

			if (!pApproach)
				return false;

			// One-time approach only. Do not re-evaluate facing or transport
			// movement here: that is what caused the group to keep re-routing.
			return pPassenger->GetCell() == pApproach;
		}

		bool StartVanillaEnter(UnitClass* pTransport, FootClass* pPassenger, EntryState& state)
		{
			if (!pTransport || !pPassenger)
				return false;

			if (state.VanillaEntering)
				return true;

			if (!CanEnterNow(pTransport, pPassenger, true))
			{
				if (LogEnabled())
				{
					auto const pCell = pPassenger->GetCell();

					Debug::Log("[Passengers.EnterDirections] cannot-start transport=%s(%u) passenger=%u cell=(%d,%d) last=(%d,%d) passengers=%d/%d onBridge=%d size=%d limit=%d frame=%d\n",
						pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID,
						pCell ? pCell->MapCoords.X : -1, pCell ? pCell->MapCoords.Y : -1,
						pPassenger->LastMapCoords.X, pPassenger->LastMapCoords.Y,
						pTransport->Passengers.NumPassengers, pTransport->Type->Passengers,
						pTransport->OnBridge ? 1 : 0,
						static_cast<int>(pPassenger->GetTechnoType()->Size),
						static_cast<int>(pTransport->Type->SizeLimit),
						Unsorted::CurrentFrame);
				}

				return false;
			}

			// Hand the final step back to the engine. From here on the
			// controller must not touch the passenger's destination or mission:
			// Mission::Enter / per-cell / EnterTransport do the real walk-in.
			state.VanillaEntering = true;
			pPassenger->QueueUpToEnter = pTransport;
			pPassenger->SetDestination(pTransport, true);
			pPassenger->QueueMission(Mission::Enter, false);
			pPassenger->NextMission();

			if (LogEnabled())
			{
				auto const pCell = pPassenger->GetCell();

				Debug::Log("[Passengers.EnterDirections] vanilla-enter transport=%s(%u) passenger=%u cell=(%d,%d) last=(%d,%d) passengers=%d/%d queue=%p nthLink=%p target=%p destination=%p mission=%d frame=%d\n",
					pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID,
					pCell ? pCell->MapCoords.X : -1, pCell ? pCell->MapCoords.Y : -1,
					pPassenger->LastMapCoords.X, pPassenger->LastMapCoords.Y,
					pTransport->Passengers.NumPassengers, pTransport->Type->Passengers,
					static_cast<void*>(pPassenger->QueueUpToEnter),
					static_cast<void*>(pTransport->GetNthLink()),
					static_cast<void*>(pPassenger->Target),
					static_cast<void*>(pPassenger->Destination),
					static_cast<int>(pPassenger->CurrentMission),
					Unsorted::CurrentFrame);
			}

			return true;
		}

		CellClass* FindRearApproachCell(UnitClass* pTransport, FootClass* pPassenger, int flags)
		{
			auto const pTransportCell = pTransport ? pTransport->GetCell() : nullptr;

			if (!pTransportCell)
				return nullptr;

			auto const pPassengerCell = pPassenger ? pPassenger->GetCell() : nullptr;
			const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());

			CellClass* pBest = nullptr;
			int bestScore = std::numeric_limits<int>::max();

			for (int relative = 0; relative < MaxDirections; ++relative)
			{
				if (!(flags & (1 << relative)))
					continue;

				const int world = (facing + relative) % MaxDirections;
				auto const pCell = pTransportCell->GetNeighbourCell(static_cast<FacingType>(world));

				if (!IsValidApproachCell(pTransport, pPassenger, pCell))
					continue;

				const int score = pPassengerCell
					? std::abs(pCell->MapCoords.X - pPassengerCell->MapCoords.X)
						+ std::abs(pCell->MapCoords.Y - pPassengerCell->MapCoords.Y)
					: relative;

				if (score < bestScore)
				{
					bestScore = score;
					pBest = pCell;
				}
			}

			// No reservation and no gating: several infantry may share the same
			// rear cell. They all walk and hand over to vanilla concurrently.
			return pBest;
		}

		bool IsNearTransport(UnitClass* pTransport, FootClass* pPassenger, int distance)
		{
			if (!pTransport || !pPassenger)
				return false;

			return std::abs(pPassenger->Location.X - pTransport->Location.X) <= distance
				&& std::abs(pPassenger->Location.Y - pTransport->Location.Y) <= distance
				&& std::abs(pPassenger->Location.Z - pTransport->Location.Z) < Unsorted::CellHeight;
		}

		void HandleEntry(UnitClass* pTransport, FootClass* pPassenger, int flags)
		{
			if (!pTransport || !pPassenger)
				return;

			if (pPassenger->WhatAmI() != AbstractType::Infantry)
				return;

			if (IsPassengerInside(pTransport, pPassenger))
			{
				ClearPassengerState(pPassenger);
				return;
			}

			if (flags == None)
			{
				ClearPassengerState(pPassenger);
				return;
			}

			auto& state = s_entryStates[pPassenger->UniqueID];

			if (state.TransportID != pTransport->UniqueID)
			{
				ReleaseReservation(state.TransportID, pPassenger->UniqueID);
				state = EntryState {};
				state.TransportID = pTransport->UniqueID;
			}

			auto const pPassengerCell = pPassenger->GetCell();
			auto const pTransportCell = pTransport->GetCell();

			if (!pPassengerCell || !pTransportCell)
				return;

			// Progress watchdog: cancel a boarding attempt that has not changed
			// cells for a long time (blocked path, full transport, ...). This
			// also releases the reserved approach cell for the next passenger.
			if (state.LastProgressFrame == 0
				|| state.LastPassengerCell != pPassengerCell->MapCoords)
			{
				if (state.LastPassengerCell != CellStruct::Empty)
					state.PreviousCell = state.LastPassengerCell;

				state.LastPassengerCell = pPassengerCell->MapCoords;
				state.LastProgressFrame = Unsorted::CurrentFrame;
			}
			else if (Unsorted::CurrentFrame - state.LastProgressFrame > 1800)
			{
				CancelPassenger(pPassenger, "no-progress");
				return;
			}

			auto& active = s_activeEntrant[pTransport->UniqueID];

			if (active != 0 && active != pPassenger->UniqueID)
			{
				auto const it = s_entryStates.find(active);

				if (it == s_entryStates.end() || !s_liveTechnos.contains(active))
					active = 0;
			}

			CellClass* pApproach = nullptr;
			const bool atApproach = IsAtApproach(pTransport, pPassenger, state, flags, pApproach);
			const bool atTransport = pPassengerCell == pTransportCell;
			const bool nearTransport = IsNearTransport(pTransport, pPassenger, 320);
			const int stagger = GetStagger(pTransport->Type, false);
			const int frame = Unsorted::CurrentFrame;
			auto const lastIt = s_lastEnterFrame.find(pTransport->UniqueID);
			const bool timerReady = stagger <= 0
				|| lastIt == s_lastEnterFrame.end()
				|| frame - lastIt->second >= stagger;

			auto const MoveToTransportCell = [&]()
			{
				if (pPassenger->Destination != pTransportCell)
					pPassenger->SetDestination(pTransportCell, true);

				if (pPassenger->CurrentMission != Mission::Move
					&& (state.LastMoveFrame == 0
						|| Unsorted::CurrentFrame - state.LastMoveFrame >= 30))
				{
					state.LastMoveFrame = Unsorted::CurrentFrame;
					pPassenger->QueueMission(Mission::Move, false);
					pPassenger->NextMission();
				}
			};

			auto const RejectWrongSide = [&]()
			{
				active = 0;
				state.ApproachCell = CellStruct::Empty;
				state.Relative = -1;
				state.Facing = -1;

				if (pPassenger->Destination)
					pPassenger->SetDestination(nullptr, true);

				if (LogEnabled())
				{
					Debug::Log("[Passengers.EnterDirections] wrong-side entry rejected transport=%s(%u) passenger=%u cell=(%d,%d) last=(%d,%d) frame=%d\n",
						pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID,
						pPassengerCell->MapCoords.X, pPassengerCell->MapCoords.Y,
						pPassenger->LastMapCoords.X, pPassenger->LastMapCoords.Y,
						Unsorted::CurrentFrame);
				}
			};

			auto const AddAtTransportCell = [&]() -> bool
			{
				if (!IsEntryApproachAllowed(pTransport, pPassenger))
				{
					RejectWrongSide();
					return false;
				}

				if (!timerReady)
					return false;

				if (!CanEnterNow(pTransport, pPassenger, true))
					return false;

				if (!DoEnterNow(pTransport, pPassenger))
					return false;

				// Never leave a limboed passenger behind if the add failed.
				if (!IsPassengerInside(pTransport, pPassenger))
				{
					if (LogEnabled())
					{
						Debug::Log("[Passengers.EnterDirections][cargo] add failed after limbo transport=%s(%u) passenger=%u frame=%d\n",
							pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID,
							Unsorted::CurrentFrame);
					}

					active = 0;
					return false;
				}

				if (pTransport->Type->EnterTransportSound >= 0)
					VoxClass::PlayAtPos(pTransport->Type->EnterTransportSound, &pTransport->Location);

				s_lastEnterFrame[pTransport->UniqueID] = Unsorted::CurrentFrame;
				active = 0;
				ReleaseReservation(state.TransportID, pPassenger->UniqueID);
				s_entryStates.erase(pPassenger->UniqueID);
				return true;
			};

			// The admitted passenger: walk to the transport cell, then add.
			if (active == pPassenger->UniqueID)
			{
				if (atTransport)
				{
					AddAtTransportCell();
					return;
				}

				if (!nearTransport || Unsorted::CurrentFrame - state.LastProgressFrame > 120)
				{
					// Could not reach the vehicle cell. Drop the slot and let the
					// passenger re-approach; do not add from the entry cell.
					active = 0;
					state.ApproachCell = CellStruct::Empty;
					state.Relative = -1;
					state.Facing = -1;

					if (pPassenger->Destination)
						pPassenger->SetDestination(nullptr, true);

					return;
				}

				MoveToTransportCell();
				return;
			}

			// Someone already at the vehicle cell but not the admitted one.
			if (atTransport)
			{
				if (active != 0 || !timerReady)
					return;

				if (!IsEntryApproachAllowed(pTransport, pPassenger))
				{
					RejectWrongSide();
					return;
				}

				active = pPassenger->UniqueID;
				AddAtTransportCell();
				return;
			}

			// Waiting at the rear entry cell.
			if (atApproach)
			{
				if (active != 0 || !timerReady)
					return;

				if (!nearTransport)
				{
					state.ApproachCell = CellStruct::Empty;
					state.Relative = -1;
					state.Facing = -1;
					return;
				}

				active = pPassenger->UniqueID;
				state.LastMoveFrame = 0;
				MoveToTransportCell();
				return;
			}

			if (state.ApproachCell == CellStruct::Empty)
			{
				pApproach = FindRearApproachCell(pTransport, pPassenger, flags);

				if (!pApproach)
				{
					if (pPassenger->Destination)
						pPassenger->SetDestination(nullptr, true);

					if (LogEnabled() && Unsorted::CurrentFrame % 60 == 0)
					{
						Debug::Log("[Passengers.EnterDirections] waiting for rear cell transport=%s(%u) passenger=%u flags=0x%02X cell=(%d,%d)\n",
							pTransport->Type->ID, pTransport->UniqueID, pPassenger->UniqueID, flags,
							pPassengerCell->MapCoords.X, pPassengerCell->MapCoords.Y);
					}

					return;
				}

				const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
				const int relative = RelativeFromCell(pTransport, pApproach, facing);
				AssignApproachCell(pTransport, pPassenger, pApproach, relative, facing, state);

				pPassenger->SetDestination(pApproach, true);

				if (pPassenger->CurrentMission != Mission::Move)
				{
					pPassenger->QueueMission(Mission::Move, false);
					pPassenger->NextMission();
				}

				return;
			}

			if (!pPassenger->Destination)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(state.ApproachCell))
					pPassenger->SetDestination(pCell, true);
			}
		}

	bool IsPassengerInside(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		// Never use Passengers.IndexOf here: if a previous bug created a
		// self-referencing cargo chain it would loop forever. Walk with a hard
		// bound and treat an overflowing chain as "inside" so no further add
		// can make the corruption worse.
		int guard = pTransport->Passengers.NumPassengers + 8;

		for (auto pCurrent = pTransport->Passengers.GetFirstPassenger();
			pCurrent;
			pCurrent = abstract_cast<FootClass*>(pCurrent->NextObject))
		{
			if (pCurrent == pPassenger)
				return true;

			if (--guard <= 0)
				break;
		}

		if (guard <= 0)
		{
			static int s_lastCargoLogFrame = -1000;

			if (Unsorted::CurrentFrame - s_lastCargoLogFrame >= 60)
			{
				s_lastCargoLogFrame = Unsorted::CurrentFrame;
				Debug::Log("[Passengers.EnterDirections][cargo] chain overflow transport=%s(%u) passengers=%d looking=%p id=%u frame=%d\n",
					pTransport->Type->ID, pTransport->UniqueID, pTransport->Passengers.NumPassengers,
					static_cast<void*>(pPassenger), pPassenger->UniqueID, Unsorted::CurrentFrame);
			}

			return true;
		}

		return false;
	}

	bool IsEntryAllowedByDirection(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return true;

		if (pPassenger->WhatAmI() != AbstractType::Infantry)
			return true;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

		if (flags == All)
			return true;

		if (flags == None)
			return false;

		auto const pPassengerCell = pPassenger->GetCell();
		auto const pTransportCell = pTransport->GetCell();

		if (!pPassengerCell || !pTransportCell)
			return false;

		// Standing on the transport itself is always accepted; the approach
		// side was validated before the passenger stepped onto the cell.
		if (pPassengerCell == pTransportCell)
			return true;

		const int relative = RelativeFromCell(pTransport, pPassengerCell);
		return relative >= 0 && (flags & (1 << relative)) != 0;
	}

	bool IsEntryApproachAllowed(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return true;

		if (pPassenger->WhatAmI() != AbstractType::Infantry)
			return true;

		if (IsLoading())
			return true;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

		if (flags == All)
			return true;

		if (flags == None)
			return false;

		auto const it = s_entryStates.find(pPassenger->UniqueID);

		// Not handled by the boarding controller: leave vanilla/script entry
		// alone. Loading was already accepted above.
		if (it == s_entryStates.end())
			return true;

		auto const pTransportCell = pTransport->GetCell();
		auto const pPassengerCell = pPassenger->GetCell();

		if (!pTransportCell || !pPassengerCell)
			return false;

		const int facing = it->second.Facing >= 0
			? it->second.Facing
			: static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());

		// Still away from the vehicle: only enforce the side once the passenger
		// is adjacent, otherwise normal pathing would be blocked.
		if (pPassengerCell != pTransportCell)
		{
			const int relative = RelativeFromCell(pTransport, pPassengerCell, facing);
			return relative < 0 || (flags & (1 << relative)) != 0;
		}

		// On the transport cell: the current cell no longer carries the
		// approach side. Prefer the controller's previous-cell history, then
		// the engine's LastMapCoords, then the assigned approach cell.
		CellStruct approach = it->second.PreviousCell;

		if (approach == CellStruct::Empty || approach == pTransportCell->MapCoords)
			approach = pPassenger->LastMapCoords;

		if (approach == CellStruct::Empty || approach == pTransportCell->MapCoords)
			approach = it->second.ApproachCell;

		if (approach == CellStruct::Empty)
			return true;

		auto const pApproachCell = MapClass::Instance.TryGetCellAt(approach);

		if (!pApproachCell)
			return true;

		const int relative = RelativeFromCell(pTransport, pApproachCell, facing);
		return relative < 0 || (flags & (1 << relative)) != 0;
	}

	bool TryBeginUnload(UnitClass* pTransport)
	{
		if (!pTransport)
			return false;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

		// None means "no entry side"; unloading is still allowed.
		if (flags == All || flags == None)
			return true;

		const int stagger = GetStagger(pTransport->Type, true);

		if (stagger <= 0)
			return true;

		const int frame = Unsorted::CurrentFrame;
		auto const it = s_lastUnloadFrame.find(pTransport->UniqueID);

		if (it != s_lastUnloadFrame.end() && frame - it->second < stagger)
			return false;

		s_lastUnloadFrame[pTransport->UniqueID] = frame;
		return true;
	}

	void FinishUnload(UnitClass* pTransport)
	{
		if (pTransport)
			s_lastUnloadFrame.erase(pTransport->UniqueID);
	}

	bool HasValidUnloadCell(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

		if (flags == All || flags == None)
			return true;

		auto const pCell = pTransport->GetCell();

		if (!pCell)
			return false;

		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());

		for (int relative = 0; relative < MaxDirections; ++relative)
		{
			if (!(flags & (1 << relative)))
				continue;

			const int world = (facing + relative) % MaxDirections;

			if (IsValidUnloadCell(pTransport, pPassenger,
				pCell->GetNeighbourCell(static_cast<FacingType>(world))))
				return true;
		}

		return false;
	}

	int ChooseUnloadDirection(UnitClass* pTransport, FootClass* pPassenger, int direction)
	{
		if (!pTransport || !pPassenger)
			return direction;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

		if (flags == All || flags == None)
			return direction;

		auto const pCell = pTransport->GetCell();

		if (!pCell)
			return direction;

		const int facing = static_cast<int>(pTransport->PrimaryFacing.Current().GetFacing<8>());
		const int relative = (direction - facing + MaxDirections) % MaxDirections;

		if ((flags & (1 << relative))
			&& IsValidUnloadCell(pTransport, pPassenger,
				pCell->GetNeighbourCell(static_cast<FacingType>(direction))))
		{
			return direction;
		}

		for (int offset = 0; offset < MaxDirections; ++offset)
		{
			if (!(flags & (1 << offset)))
				continue;

			const int world = (facing + offset) % MaxDirections;

			if (IsValidUnloadCell(pTransport, pPassenger,
				pCell->GetNeighbourCell(static_cast<FacingType>(world))))
			{
				return world;
			}
		}

		return direction;
	}

	void ClearAll()
	{
		s_entryStates.clear();
		s_reservations.clear();
		s_lastEnterFrame.clear();
		s_lastUnloadFrame.clear();
		s_activeEntrant.clear();
		s_liveTechnos.clear();
		s_liveFrame = -1;
	}

	void ClearPassenger(FootClass* pPassenger)
	{
		ClearPassengerState(pPassenger);
	}

	void EnsureScenario()
	{
		const int frame = Unsorted::CurrentFrame;

		if (ScenarioClass::Instance != s_lastScenario || frame == 0)
		{
			ClearAll();
			s_lastScenario = ScenarioClass::Instance;
		}

		// Rebuild the live Techno lookup once per frame. GetTrackedTransport
		// used to scan TechnoClass::Array for every boarding passenger, which
		// became O(passengers * technos) with large groups.
		if (s_liveFrame != frame)
		{
			s_liveTechnos.clear();
			s_liveTechnos.reserve(static_cast<size_t>(TechnoClass::Array.Count));

			for (auto const pTechno : TechnoClass::Array)
			{
				if (pTechno)
					s_liveTechnos[pTechno->UniqueID] = pTechno;
			}

			s_liveFrame = frame;

			// Drop state for passengers/transports that died while boarding.
			if (frame != 0 && frame % 300 == 0)
				PruneStates();
		}
	}

	TechnoClass* GetTrackedTransport(FootClass* pPassenger)
	{
		if (!pPassenger)
			return nullptr;

		auto const it = s_entryStates.find(pPassenger->UniqueID);

		if (it == s_entryStates.end() || !it->second.TransportID)
			return nullptr;

		auto const live = s_liveTechnos.find(it->second.TransportID);
		return live != s_liveTechnos.end() ? live->second : nullptr;
	}

	void TrackTransport(FootClass* pPassenger, UnitClass* pTransport)
	{
		if (!pPassenger || !pTransport)
			return;

		auto& state = s_entryStates[pPassenger->UniqueID];

		if (state.TransportID != pTransport->UniqueID)
		{
			ReleaseReservation(state.TransportID, pPassenger->UniqueID);
			state = EntryState {};
			state.TransportID = pTransport->UniqueID;
		}
	}

	bool IsLoading()
	{
		return Unsorted::CurrentFrame == 0;
	}

	bool IsTracked(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		auto const it = s_entryStates.find(pPassenger->UniqueID);
		return it != s_entryStates.end()
			&& it->second.TransportID == pTransport->UniqueID;
	}

	bool IsVanillaEntering(FootClass* pPassenger)
	{
		if (!pPassenger)
			return false;

		auto const it = s_entryStates.find(pPassenger->UniqueID);
		return it != s_entryStates.end() && it->second.VanillaEntering;
	}

	bool IsAtFrontCell(UnitClass* pTransport, FootClass* pPassenger)
	{
		if (!pTransport || !pPassenger)
			return false;

		auto const pCell = pPassenger->GetCell();

		if (!pCell)
			return false;

		const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);
		return IsFrontCell(pTransport, pCell, flags);
	}

	bool IsManagedMove(FootClass* pPassenger)
	{
		if (!pPassenger)
			return false;

		auto const it = s_entryStates.find(pPassenger->UniqueID);

		if (it == s_entryStates.end() || it->second.ApproachCell == CellStruct::Empty)
			return false;

		auto const pCell = MapClass::Instance.TryGetCellAt(it->second.ApproachCell);
		return pCell && (pPassenger->Destination == pCell || pPassenger->GetCell() == pCell);
	}

	bool HasApproachCell(FootClass* pPassenger)
	{
		if (!pPassenger)
			return false;

		auto const it = s_entryStates.find(pPassenger->UniqueID);
		return it != s_entryStates.end() && it->second.ApproachCell != CellStruct::Empty;
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

	PassengerDirections::EnsureScenario();

	// Passengers already inside (or limboed) must never run the entry logic again.
	if (pThis->Transporter)
	{
		PassengerDirections::ClearPassenger(pThis);
		return 0;
	}

	auto const pTracked = PassengerDirections::GetTrackedTransport(pThis);

	// A Move order that is not the controller's own approach move replaces the
	// boarding order. A Move to the transport itself is the engine's approach
	// and must not cancel the controller.
	if (pThis->CurrentMission == Mission::Move && pTracked
		&& PassengerDirections::HasApproachCell(pThis))
	{
		auto const pDestination = pThis->Destination;

		if (pDestination && pDestination != pTracked
			&& abstract_cast<CellClass*>(pDestination)
			&& !PassengerDirections::IsManagedMove(pThis))
		{
			PassengerDirections::ClearPassenger(pThis);
			return 0;
		}
	}

	// A new Move/Stop/Guard order must be able to cancel the boarding attempt.
	// The controller's tracked state is authoritative. The engine queue may be
	// blocked (NoMove) and therefore empty while the passenger is still
	// walking to the boarding position.
	const bool entering = pTracked != nullptr
		|| pThis->CurrentMission == Mission::Enter
		|| pThis->QueueUpToEnter != nullptr;

	if (!entering)
	{
		PassengerDirections::ClearPassenger(pThis);
		return 0;
	}

	TechnoClass* pEntryTarget = pThis->CurrentMission == Mission::Enter
		? pThis->GetNthLink() : pThis->QueueUpToEnter;

	if (!pEntryTarget)
		pEntryTarget = pThis->GetNthLink();

	if (!pEntryTarget)
		pEntryTarget = abstract_cast<TechnoClass*>(pThis->Target);

	if (!pEntryTarget)
		pEntryTarget = pTracked;

	if (!pEntryTarget)
	{
		PassengerDirections::ClearPassenger(pThis);
		return 0;
	}

	if (auto const pDest = abstract_cast<UnitClass*>(pEntryTarget))
	{
		if (PassengerDirections::IsPassengerInside(pDest, pThis))
		{
			PassengerDirections::ClearPassenger(pThis);
			return 0;
		}

		const auto pType = pDest->Type;
		const int enterDirections = static_cast<int>(TechnoTypeExt::ExtMap.Find(pType)->Passengers_EnterDirections);

		// Restricted transports are an infantry-only feature. Never let a
		// Unit/transport be treated as a passenger by this controller.
		if (enterDirections != PassengerDirections::All
			&& pThis->WhatAmI() != AbstractType::Infantry)
			return 0;

		if (pType->Passengers > 0
			&& pThis->WhatAmI() == AbstractType::Infantry
			&& enterDirections != PassengerDirections::All)
		{
			PassengerDirections::HandleEntry(pDest, pThis, enterDirections);
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
				}
				else if (!pThis->Destination && !pDest->OnBridge && !pDest->Destination)
				{
					// Move to enter position, prevent other passengers from
					// waiting for a call and not moving early.
					auto cell = CellStruct::Empty;
					pThis->NearbyLocation(&cell, pDest);

					if (cell != CellStruct::Empty)
					{
						pThis->SetDestination(MapClass::Instance.GetCellAt(cell), true);
						pThis->QueueMission(Mission::Move, false);
						pThis->NextMission();
					}
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
		|| static_cast<int>(TechnoTypeExt::ExtMap.Find(pType)->Passengers_EnterDirections) != PassengerDirections::All;
}

// The unload loop computes the world direction in ECX and then indexes
// Unsorted::AdjacentCell with it. Rewriting ECX is safe here (6-byte
// instruction boundary), unlike a 2-byte hook that would corrupt the loop.
DEFINE_HOOK(0x73D934, UnitClass_Mission_Unload_DirectionGate, 0x7)
{
	GET(UnitClass*, pThis, ESI);
	GET(int, direction, ECX);

	auto const pPassenger = pThis->Passengers.GetFirstPassenger();

	if (!pPassenger)
		return 0;

	const int chosen = PassengerDirections::ChooseUnloadDirection(pThis, pPassenger, direction);

	if (chosen != direction)
		R->ECX(chosen);

	if (Phobos::Config::DebugGCDiagnostics)
	{
		Debug::Log("[Passengers.EnterDirections][unload] transport=%s(%u) dir=%d chosen=%d frame=%d\n",
			pThis->Type->ID, pThis->UniqueID, direction, chosen, Unsorted::CurrentFrame);
	}

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
			PassengerDirections::FinishUnload(pThis);

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

		// Direction restricted transports wait until an allowed exit cell is
		// free instead of ejecting the passenger into water or a wall.
		if (!PassengerDirections::HasValidUnloadCell(pThis, pPassenger))
		{
			pThis->MissionStatus = 0; // Retry next frame
			return NoUnloadReturn;
		}

		// Restricted transports release one passenger per configured interval.
		if (!PassengerDirections::TryBeginUnload(pThis))
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
			const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

			if (flags != PassengerDirections::All)
			{
				PassengerDirections::TrackTransport(pThis, pTransport);

				// The controller drives the approach and the manual 2-frame
				// admission. Loading must stay vanilla so cargo is restored.
				if (PassengerDirections::IsLoading())
					return 0;

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
			const int flags = static_cast<int>(TechnoTypeExt::ExtMap.Find(pTransport->Type)->Passengers_EnterDirections);

			if (flags != PassengerDirections::All)
			{
				PassengerDirections::TrackTransport(pThis, pTransport);

				// Controller-owned entry; no vanilla queue/link.
				if (PassengerDirections::IsLoading())
					return 0;

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
