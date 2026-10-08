#include "Body.h"

#include <CellClass.h>
#include <Ext/Aircraft/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/SWType/Body.h>
#include "Ext/Techno/Body.h"
#include "Ext/TechnoType/Body.h"
#include "Ext/Team/Body.h"
#include "Ext/Building/Body.h"
#include <Unsorted.h>
#include <unordered_map>

// Resolves the SpyPlane superweapon's SpawnPoints waypoints into the shared
// temporary before the aircraft placement hook uses them. Hooking the
// HouseClass::SendSpyPlanes entry (the funnel every SpyPlane launch goes
// through) works for both the vanilla SpyPlane case and Ares' SW_SpyPlane
// handling, which calls this function directly and thereby bypasses Phobos'
// SuperClass::Launch hook.
DEFINE_HOOK(0x65EAB0, HouseClass_SendSpyPlanes_ResolveSpawnPoints, 0x8)
{
	GET(HouseClass* const, pHouse, ECX);
	GET(int const, aircraftTypeIdx, EDX);

	SpyPlaneSpawnPointTemp::SpawnPoints.clear();
	SpyPlaneSpawnPointTemp::UsedSpawnPointIndices.clear();

	// Identify the firing SpyPlane SW. The SendSpyPlanes funnel only exposes
	// the aircraft type, not the firing SuperClass, so identification combines
	// two signals: which SpyPlane SW declared the fired plane (SpyPlane.Type,
	// resolved through Phobos' own inheritance-aware INI reading) and which
	// SpyPlane SW actually configured SpawnPoints/SpawnPoints.Delay. Prefer a
	// SW that matches both; otherwise the single SpawnPoints-configured
	// SpyPlane SW is treated as the firing one.
	SuperWeaponTypeClass* pFiringSW = nullptr;
	SuperWeaponTypeClass* pMatchedConfigured = nullptr;
	int matchedConfiguredCount = 0;
	SuperWeaponTypeClass* pConfigured = nullptr;
	int configuredCount = 0;

	for (auto const& pSuper : pHouse->Supers)
	{
		if (pSuper->Type->Type != SuperWeaponType::SpyPlane)
			continue;

		auto const pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

		const bool hasSpawnPoints = !pExt->SpawnPoints.empty()
			|| (pExt->SpawnPoints_Delay.isset() && pExt->SpawnPoints_Delay.Get() > 0);

		if (hasSpawnPoints)
		{
			pConfigured = pSuper->Type;
			++configuredCount;
		}

		if (pExt->SpyPlaneType && pExt->SpyPlaneType->ArrayIndex == aircraftTypeIdx
			&& hasSpawnPoints)
		{
			pMatchedConfigured = pSuper->Type;
			++matchedConfiguredCount;
		}
	}

	// Best signal: exactly one SpyPlane SW both fired the plane and wants
	// SpawnPoints applied.
	if (matchedConfiguredCount == 1)
	{
		pFiringSW = pMatchedConfigured;
	}
	// Otherwise, if exactly one SpyPlane SW configured SpawnPoints at all, it
	// must be the one being fired.
	else if (configuredCount == 1)
	{
		pFiringSW = pConfigured;
	}
	// Ambiguous: more than one SpyPlane SW wants SpawnPoints and none can be
	// tied to the fired plane, so the summon stays vanilla.
	else
	{
		pFiringSW = nullptr;
	}

	// The firing SW must actually have SpawnPoints/Delay configured, otherwise
	// there is nothing to apply and the summon stays vanilla.
	if (!pFiringSW)
		return 0;

	auto const pExt = SWTypeExt::ExtMap.Find(pFiringSW);

	if (pExt->SpawnPoints.empty()
		&& !(pExt->SpawnPoints_Delay.isset() && pExt->SpawnPoints_Delay.Get() > 0))
	{
		return 0;
	}

	if (const auto pScenarioExt = ScenarioExt::Global())
	{
		auto& waypoints = pScenarioExt->Waypoints;

		for (auto const waypoint : pExt->SpawnPoints)
		{
			if (waypoint >= 0
				&& waypoints.find(waypoint) != waypoints.end()
				&& waypoints[waypoint].X
				&& waypoints[waypoint].Y)
			{
				SpyPlaneSpawnPointTemp::SpawnPoints.push_back(waypoints[waypoint]);
			}
		}
	}

	const int delay = pExt->SpawnPoints_Delay.Get(0);

	Debug::Log("[Phobos] SpyPlane %s resolved %u SpawnPoints, delay %d.\n",
		pFiringSW->ID, SpyPlaneSpawnPointTemp::SpawnPoints.size(), delay);

	// A deferred re-invocation of SendSpyPlanes must not re-schedule itself.
	if (SpyPlaneSpawnPointTemp::InDeferredCall)
		return 0;

	if (delay > 0)
	{
		// Capture the call parameters and schedule a deferred re-invocation.
		const int count = R->Stack32(0x4);
		const int mission = R->Stack32(0x8);
		AbstractClass* const pTarget = reinterpret_cast<AbstractClass*>(R->Stack32(0xC));
		AbstractClass* const pDestination = reinterpret_cast<AbstractClass*>(R->Stack32(0x10));

		SpyPlaneSpawnPointTemp::Pending.push_back({
			Unsorted::CurrentFrame + delay,
			pHouse, aircraftTypeIdx, count, mission, pTarget, pDestination
		});

		// Turn this call into a no-op by zeroing the aircraft count, so
		// SendSpyPlanes returns immediately through its own early-exit path
		// without spawning anything and without touching the caller's stack.
		R->Stack(0x4, 0);
	}

	return 0;
}

// Fires due delayed SpyPlane summons and LimboDelivery deliveries on their
// expiry frame.
DEFINE_HOOK(0x55B4E1, LogicClass_Update_BeforeAll_ProcessDeferredEffects, 0x5)
{
	// Remove team members that have driven beyond the map boundary so leaving
	// teams (including teams sent to off-map waypoints) finish leaving.
	TeamExt::RemoveStuckLeavingMembers();

	auto& pending = SpyPlaneSpawnPointTemp::Pending;

	for (auto it = pending.begin(); it != pending.end();)
	{
		if (it->ExpireFrame > Unsorted::CurrentFrame)
		{
			++it;
			continue;
		}

		auto entry = *it;
		it = pending.erase(it);

		if (!entry.pHouse || entry.pHouse->Defeated)
			continue;

		SpyPlaneSpawnPointTemp::InDeferredCall = true;

		typedef void(__fastcall* SendSpyPlanesFunc)(HouseClass*, int, int, int, AbstractClass*, AbstractClass*);
		reinterpret_cast<SendSpyPlanesFunc>(0x65EAB0)(
			entry.pHouse, entry.AircraftTypeIdx, entry.Count, entry.Mission,
			entry.pTarget, entry.pDestination);

		SpyPlaneSpawnPointTemp::InDeferredCall = false;
	}

	auto& limboPending = LimboDeliveryTemp::Pending;

	for (auto it = limboPending.begin(); it != limboPending.end();)
	{
		if (it->ExpireFrame > Unsorted::CurrentFrame)
		{
			++it;
			continue;
		}

		auto entry = *it;
		it = limboPending.erase(it);

		if (!entry.pHouse || entry.pHouse->Defeated)
			continue;

		if (entry.SWTypeIndex < 0 || entry.SWTypeIndex >= SuperWeaponTypeClass::Array.Count)
			continue;

		auto const pSWType = SuperWeaponTypeClass::Array.GetItem(entry.SWTypeIndex);
		SWTypeExt::ExtMap.Find(pSWType)->ApplyLimboDelivery(entry.pHouse);
	}

	// Relocate para-drop aircraft to their SpawnPoints waypoint cell. The planes
	// are created and placed by Ares at the map edge; once they are on-map we
	// move them to the recorded waypoint before their locomotion starts driving
	// them, so they visibly enter from the configured waypoints instead. With
	// SpawnPoints.Delay the relocation is deferred until the expiry frame, then
	// the plane enters from the waypoint in the air (like Airstrike).
	auto& paraDropReloc = ParaDropSpawnPointTemp::PendingReloc;

	// Iterate by index (bounded by size()) and collect completed indices, then
	// erase them afterwards - safer than mutating the vector while iterating.
	std::vector<size_t> finished;

	for (size_t i = 0; i < paraDropReloc.size(); i++)
	{
		auto& entry = paraDropReloc[i];
		auto* const pAircraft = entry.Aircraft;

		// The plane may not have been placed yet in the same frame it was
		// created; give it a few frames, then drop stale entries.
		if (!pAircraft)
		{
			if (++entry.WaitFrames > 10)
				finished.push_back(i);
			continue;
		}

		// SpawnPoints.Delay: defer the relocation until the expiry frame.
		if (entry.ExpireFrame > Unsorted::CurrentFrame)
			continue;

		auto const pType = pAircraft->Type;
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
		auto const height = pTypeExt->SpawnHeight.Get(pType->GetFlightLevel());

		// Place the plane at the waypoint cell and lift it to flight altitude so
		// the drop happens from the air (like Airstrike), not on the ground.
		pAircraft->SetLocation(CellClass::Cell2Coord(entry.Cell));
		pAircraft->SetHeight(height);

		finished.push_back(i);
	}

	// Remove the completed entries (highest index first keeps positions valid).
	for (auto rit = finished.rbegin(); rit != finished.rend(); ++rit)
		paraDropReloc.erase(paraDropReloc.begin() + static_cast<ptrdiff_t>(*rit));

	return 0;
}

DEFINE_HOOK(0x508C30, HouseClass_UpdatePower_UpdateCounter, 0x5)
{
	GET(HouseClass*, pThis, ECX);
	auto const pHouseExt = HouseExt::ExtMap.Find(pThis);

	pHouseExt->PowerPlantEnhancers.clear();

	// This pre-iterating ensure our process to be done in O(NM) instead of O(N^2),
	// as M should be much less than N, this will be a great improvement. - secsome
	for (auto const pBld : pThis->Buildings)
	{
		if (TechnoExt::IsActive(pBld) && pBld->IsOnMap && pBld->HasPower)
		{
			const auto pType = pBld->Type;
			const auto pExt = BuildingTypeExt::ExtMap.Find(pType);

			if (pExt->PowerPlantEnhancer_Buildings.size()
				&& (pExt->PowerPlantEnhancer_Amount != 0 || pExt->PowerPlantEnhancer_Factor != 1.0f))
			{
				++pHouseExt->PowerPlantEnhancers[pType->ArrayIndex];
			}
		}
	}

	return 0;
}

// Power Plant Enhancer #131
DEFINE_HOOK(0x508CF2, HouseClass_UpdatePower_PowerOutput, 0x7)
{
	GET(HouseClass*, pThis, ESI);
	GET(BuildingClass*, pBld, EDI);

	pThis->PowerOutput += BuildingTypeExt::GetEnhancedPower(pBld, pThis);

	return 0x508D07;
}

// Trigger power recalculation on gain/loss of any techno, not just buildings.
DEFINE_HOOK_AGAIN(0x5025F0, HouseClass_RegisterGain, 0x5) // RegisterLoss
DEFINE_HOOK(0x502A80, HouseClass_RegisterGain, 0x8)
{
	if (!Phobos::Config::UnitPowerDrain)
		return 0;

	GET(HouseClass*, pThis, ECX);

	pThis->RecheckPower = true;

	return 0;
}

DEFINE_HOOK(0x508D8D, HouseClass_UpdatePower_Techno, 0x6)
{
	if (!Phobos::Config::UnitPowerDrain)
		return 0;

	GET(HouseClass*, pThis, ESI);

	auto updateDrainForThisType = [pThis](const TechnoTypeClass* pType)
	{
			const int count = pThis->CountOwnedAndPresent(pType);
			if (count == 0)
				return;
			const auto pExt = TechnoTypeExt::ExtMap.Find(pType);
			if (pExt->Power > 0)
				pThis->PowerOutput += pExt->Power * count;
			else
				pThis->PowerDrain -= pExt->Power * count;
	};

	for (const auto pType : InfantryTypeClass::Array)
		updateDrainForThisType(pType);
	for (const auto pType : UnitTypeClass::Array)
		updateDrainForThisType(pType);
	for (const auto pType : AircraftTypeClass::Array)
		updateDrainForThisType(pType);
	// Don't do this for buildings, they've already been counted.

	return 0;
}

DEFINE_HOOK(0x73E474, UnitClass_Unload_Storage, 0x6)
{
	GET(BuildingClass* const, pBuilding, EDI);
	GET(int const, idxTiberium, EBP);
	REF_STACK(float, amount, 0x1C);

	auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pBuilding->Type);

	auto const storageTiberiumIndex = RulesExt::Global()->Storage_TiberiumIndex;

	if (pTypeExt->Refinery_UseStorage && storageTiberiumIndex >= 0)
	{
		BuildingExt::StoreTiberium(pBuilding, amount, idxTiberium, storageTiberiumIndex);
		amount = 0.0f;
	}

	return 0;
}

namespace RecalcCenterTemp
{
	HouseExt::ExtData* pExtData;
}

DEFINE_HOOK(0x4FD166, HouseClass_RecalcCenter_SetContext, 0x5)
{
	GET(HouseClass* const, pThis, EDI);

	RecalcCenterTemp::pExtData = HouseExt::ExtMap.Find(pThis);

	return 0;
}

DEFINE_HOOK_AGAIN(0x4FD463, HouseClass_RecalcCenter_LimboDelivery, 0x6)
DEFINE_HOOK(0x4FD1CD, HouseClass_RecalcCenter_LimboDelivery, 0x6)
{
	enum { SkipBuilding1 = 0x4FD23B, SkipBuilding2 = 0x4FD4D5 };

	GET(BuildingClass* const, pBuilding, ESI);

	if (!MapClass::Instance.CoordinatesLegal(pBuilding->GetMapCoords()))
		return R->Origin() == 0x4FD1CD ? SkipBuilding1 : SkipBuilding2;

	auto const pExt = RecalcCenterTemp::pExtData;

	if (pExt && pExt->OwnsLimboDeliveredBuilding(pBuilding))
		return R->Origin() == 0x4FD1CD ? SkipBuilding1 : SkipBuilding2;

	return 0;
}

DEFINE_HOOK(0x4AC534, DisplayClass_ComputeStartPosition_IllegalCoords, 0x6)
{
	enum { SkipTechno = 0x4AC55B };

	GET(TechnoClass* const, pTechno, ECX);

	if (!MapClass::Instance.CoordinatesLegal(pTechno->GetMapCoords()))
		return SkipTechno;

	return 0;
}

#pragma region LimboTracking

// These hooks handle tracking objects that are limboed e.g not physically on the map or engaged in game logic updates.
// The objects are manually updated once after pre-placed objects have been parsed, buildings are ignored as the limboed pre-placed buildings
// are not relevant (walls that will be converted into overlays etc), after which automatic update on limbo/unlimbo and uninit is enabled.

namespace LimboTrackingTemp
{
	bool Enabled = false;
	bool IsBeingDeleted = false;
}

DEFINE_HOOK(0x687B18, ScenarioClass_ReadINI_StartTracking, 0x7)
{
	for (auto const pTechno : TechnoClass::Array)
	{
		auto const pType = pTechno->GetTechnoType();

		if (!pType->Insignificant && !pType->DontScore && pTechno->WhatAmI() != AbstractType::Building && pTechno->InLimbo)
		{
			auto const pOwnerExt = HouseExt::ExtMap.Find(pTechno->Owner);
			pOwnerExt->AddToLimboTracking(pType);
		}
	}

	LimboTrackingTemp::Enabled = true;

	return 0;
}

void __fastcall TechnoClass_UnInit_Wrapper(TechnoClass* pThis)
{
	InfantryFiring_ClearTracking(pThis);

	if (LimboTrackingTemp::Enabled && pThis->InLimbo)
	{
		auto const pType = pThis->GetTechnoType();

		if (!pType->Insignificant && !pType->DontScore)
			HouseExt::ExtMap.Find(pThis->Owner)->RemoveFromLimboTracking(pType);
	}

	LimboTrackingTemp::IsBeingDeleted = true;
	pThis->ObjectClass::UnInit();
	LimboTrackingTemp::IsBeingDeleted = false;
}

DEFINE_FUNCTION_JUMP(CALL, 0x4DE60B, TechnoClass_UnInit_Wrapper);   // FootClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E3FB4, TechnoClass_UnInit_Wrapper); // BuildingClass

DEFINE_HOOK(0x6F6BC9, TechnoClass_Limbo_AddTracking, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	auto const pType = pThis->GetTechnoType();

	if (LimboTrackingTemp::Enabled && !pType->Insignificant && !pType->DontScore && !LimboTrackingTemp::IsBeingDeleted)
	{
		auto const pOwnerExt = HouseExt::ExtMap.Find(pThis->Owner);
		pOwnerExt->AddToLimboTracking(pType);
	}

	return 0;
}

DEFINE_HOOK(0x6F6D85, TechnoClass_Unlimbo_RemoveTracking, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	auto const pType = pThis->GetTechnoType();
	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (LimboTrackingTemp::Enabled && !pType->Insignificant && !pType->DontScore && pExt->HasBeenPlacedOnMap)
	{
		auto const pOwnerExt = HouseExt::ExtMap.Find(pThis->Owner);
		pOwnerExt->RemoveFromLimboTracking(pType);
	}
	else if (!pExt->HasBeenPlacedOnMap)
	{
		pExt->HasBeenPlacedOnMap = true;

		if (pExt->TypeExtData->AutoDeath_Behavior.isset())
			ScenarioExt::Global()->AutoDeathObjects.push_back(pExt);
	}

	return 0;
}

DEFINE_HOOK(0x7015C9, TechnoClass_Captured_UpdateTracking, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(HouseClass* const, pNewOwner, EBP);

	// Defensive fix + diagnostic: the game can request an owner-change with a NULL
	// target house (observed during saving). ChangeOwner's body would dereference
	// the NULL house and crash, so skip the whole function by jumping straight to
	// its clean early-exit path (0x70188C: pop esi; xor al,al; pop ebp;
	// add esp,0xc; ret 8). This covers every ChangeOwner caller.
	if (!pNewOwner)
		return 0x70188C;

	auto const pType = pThis->GetTechnoType();
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto const pTypeExt = pExt->TypeExtData;
	auto const pOwnerExt = HouseExt::ExtMap.Find(pThis->Owner);
	auto const pNewOwnerExt = HouseExt::ExtMap.Find(pNewOwner);

	if (LimboTrackingTemp::Enabled && !pType->Insignificant && !pType->DontScore && pThis->InLimbo)
	{
		pOwnerExt->RemoveFromLimboTracking(pType);
		pNewOwnerExt->AddToLimboTracking(pType);
	}

	if (pTypeExt->Harvester_Counted)
	{
		auto& vec = pOwnerExt->OwnedCountedHarvesters;
		vec.erase(std::remove(vec.begin(), vec.end(), pThis), vec.end());

		pNewOwnerExt->OwnedCountedHarvesters.push_back(pThis);
	}

	if (const auto pMe = generic_cast<FootClass*, true>(pThis))
	{
		const bool I_am_human = pThis->Owner->IsControlledByHuman();

		if (I_am_human != pNewOwner->IsControlledByHuman())
		{
			if (const auto pConvertTo = I_am_human
				? pTypeExt->Convert_HumanToComputer.Get()
				: pTypeExt->Convert_ComputerToHuman.Get())
			{
				if (pConvertTo->WhatAmI() == pType->WhatAmI())
					TechnoExt::ConvertToType(pMe, pConvertTo);
			}

			if (!I_am_human)
				TechnoExt::ChangeOwnerMissionFix(pMe);
		}
	}

	for (const auto& pTrail : pExt->LaserTrails)
	{
		if (pTrail->Type->IsHouseColor)
			pTrail->CurrentColor = pNewOwner->LaserColor;
	}

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x65EB8D, HouseClass_SendSpyPlanes_PlaceAircraft, 0x6)
{
	enum { SkipGameCode = 0x65EBE5, SkipGameCodeNoSuccess = 0x65EC12 };

	GET(AircraftClass* const, pAircraft, ESI);
	GET(CellStruct const, edgeCell, EDI);

	// SpyPlane superweapon SpawnPoints override: pick a random waypoint cell
	// resolved at HouseClass::SendSpyPlanes entry instead of the default map
	// edge cell. Each aircraft of the summon gets a distinct waypoint whenever
	// enough remain, so two planes never appear from the same one.
	auto cell = edgeCell;

	auto const spawnCount = SpyPlaneSpawnPointTemp::SpawnPoints.size();

	if (spawnCount > 0)
	{
		auto& used = SpyPlaneSpawnPointTemp::UsedSpawnPointIndices;
		std::vector<int> available;
		available.reserve(spawnCount);

		for (size_t i = 0; i < spawnCount; ++i)
		{
			if (std::find(used.begin(), used.end(), static_cast<int>(i)) == used.end())
				available.push_back(static_cast<int>(i));
		}

		// More aircraft than distinct waypoints: duplicates are unavoidable,
		// so fall back to picking among all of them.
		if (available.empty())
		{
			available.resize(spawnCount);

			for (size_t i = 0; i < spawnCount; ++i)
				available[i] = static_cast<int>(i);
		}

		const int index = available[
			ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(available.size()) - 1)];

		used.push_back(index);
		cell = SpyPlaneSpawnPointTemp::SpawnPoints[index];
	}

	const bool result = AircraftExt::PlaceReinforcementAircraft(pAircraft, cell);

	return result ? SkipGameCode : SkipGameCodeNoSuccess;
}

DEFINE_HOOK(0x65E997, HouseClass_SendAirstrike_PlaceAircraft, 0x6)
{
	enum { SkipGameCode = 0x65E9EE, SkipGameCodeNoSuccess = 0x65EA8B };

	GET(AircraftClass* const, pAircraft, ESI);
	GET(CellStruct const, edgeCell, EDI);

	const bool result = AircraftExt::PlaceReinforcementAircraft(pAircraft, edgeCell);

	return result ? SkipGameCode : SkipGameCodeNoSuccess;
}

// Vanilla and Ares all only hardcoded to find factory with BuildCat::DontCare...
static inline bool CheckShouldDisableDefensesCameo(HouseClass* pHouse, TechnoTypeClass* pType)
{
	if (const auto pBuildingType = abstract_cast<BuildingTypeClass*>(pType))
	{
		if (pBuildingType->BuildCat == BuildCat::Combat)
		{
			auto count = 0;

			if (const auto pFactory = pHouse->Primary_ForDefenses)
			{
				count = pFactory->CountTotal(pBuildingType);

				if (pFactory->Object && pFactory->Object->GetType() == pBuildingType && pBuildingType->BuildLimit > 0)
					--count;
			}

			auto buildLimitRemaining = [](HouseClass* pHouse, BuildingTypeClass* pBldType)
			{
				const auto BuildLimit = pBldType->BuildLimit;

				if (BuildLimit >= 0)
					return BuildLimit - BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pBldType, pHouse);
				else
					return -BuildLimit - pHouse->CountOwnedEver(pBldType);
			};

			if (buildLimitRemaining(pHouse, pBuildingType) - count <= 0)
				return true;
		}
	}

	return false;
}

DEFINE_HOOK(0x50B669, HouseClass_ShouldDisableCameo_GreyCameo, 0x5)
{
	GET(HouseClass*, pThis, ECX);
	GET_STACK(TechnoTypeClass*, pType, 0x4);
	GET(const bool, aresDisable, EAX);

	if (aresDisable || !pType)
		return 0;

	if (CheckShouldDisableDefensesCameo(pThis, pType) || HouseExt::ReachedBuildLimit(pThis, pType, false))
		R->EAX(true);

	return 0;
}

DEFINE_HOOK(0x4FD77C, HouseClass_ExpertAI_Superweapons, 0x5)
{
	enum { SkipSWProcess = 0x4FD7A0 };

	if (RulesExt::Global()->AISuperWeaponDelay.isset())
		return SkipSWProcess;

	return 0;
}

DEFINE_HOOK(0x4F9038, HouseClass_AI_Superweapons, 0x5)
{
	GET(HouseClass*, pThis, ESI);

	if (!RulesExt::Global()->AISuperWeaponDelay.isset() || pThis->IsControlledByHuman() || pThis->Type->MultiplayPassive)
		return 0;

	const int delay = RulesExt::Global()->AISuperWeaponDelay.Get();

	if (delay > 0)
	{
		auto const pExt = HouseExt::ExtMap.Find(pThis);

		if (pExt->AISuperWeaponDelayTimer.HasTimeLeft())
			return 0;

		pExt->AISuperWeaponDelayTimer.Start(delay);
	}

	if (!SessionClass::IsCampaign() || pThis->IQLevel2 >= RulesClass::Instance->SuperWeapons)
		pThis->AI_TryFireSW();

	return 0;
}

DEFINE_HOOK_AGAIN(0x4FFA99, HouseClass_ExcludeFromMultipleFactoryBonus, 0x6)
DEFINE_HOOK(0x4FF9C9, HouseClass_ExcludeFromMultipleFactoryBonus, 0x6)
{
	GET(BuildingClass*, pBuilding, ESI);

	auto const pType = pBuilding->Type;

	if (BuildingTypeExt::ExtMap.Find(pType)->ExcludeFromMultipleFactoryBonus)
	{
		GET(HouseClass*, pThis, EDI);
		GET(const bool, isNaval, ECX);

		auto const pExt = HouseExt::ExtMap.Find(pThis);
		pExt->UpdateNonMFBFactoryCounts(pType->Factory, R->Origin() == 0x4FF9C9, isNaval);
	}

	return 0;
}

DEFINE_HOOK(0x500910, HouseClass_GetFactoryCount, 0x5)
{
	enum { SkipGameCode = 0x50095D };

	GET(HouseClass*, pThis, ECX);
	GET_STACK(AbstractType, rtti, 0x4);
	GET_STACK(const bool, isNaval, 0x8);

	auto const pExt = HouseExt::ExtMap.Find(pThis);
	R->EAX(pExt->GetFactoryCountWithoutNonMFB(rtti, isNaval));

	return SkipGameCode;
}

// Sell all and all in.
DEFINE_HOOK(0x4FD8F7, HouseClass_UpdateAI_OnLastLegs, 0x10)
{
	enum { ret = 0x4FD907 };

	GET(HouseClass*, pThis, EBX);

	if (RulesExt::Global()->AIFireSale)
	{
		auto const pExt = HouseExt::ExtMap.Find(pThis);

		if (RulesExt::Global()->AIFireSaleDelay <= 0 || pExt->AIFireSaleDelayTimer.Completed())
			pThis->Fire_Sale();
		else if (!pExt->AIFireSaleDelayTimer.HasStarted())
			pExt->AIFireSaleDelayTimer.Start(RulesExt::Global()->AIFireSaleDelay);
	}

	if (RulesExt::Global()->AIAllToHunt)
		pThis->All_To_Hunt();

	return ret;
}
