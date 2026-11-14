#include "Body.h"

#include <AircraftClass.h>
#include <UnitClass.h>
#include <Phobos.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <JumpjetLocomotionClass.h>

#include <Ext/Anim/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/WeaponType/Body.h>

#include <Utilities/AresFunctions.h>
#include <Utilities/CrashTrace.h>
#include <Utilities/PointerSanity.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;
UnitClass* TechnoExt::Deployer = nullptr;
TechnoClass* TechnoExt::DeployTransferSource = nullptr;

TechnoExt::ExtData::~ExtData()
{
	auto const pTypeExt = this->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	auto const pThis = this->OwnerObject();
	// Besides BuildingClass, calling pThis->WhatAmI() here will only result in AbstractType::None
	auto const whatAmI = pType->WhatAmI();

	if (pTypeExt->AutoDeath_Behavior.isset())
	{
		auto& vec = ScenarioExt::Global()->AutoDeathObjects;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (whatAmI != AbstractType::AircraftType && whatAmI != AbstractType::BuildingType
		&& pType->Ammo > 0 && pTypeExt->ReloadInTransport)
	{
		auto& vec = ScenarioExt::Global()->TransportReloaders;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (this->AnimRefCount > 0)
		AnimExt::InvalidateTechnoPointers(pThis);

	if (pTypeExt->Harvester_Counted)
	{
		auto& vec = HouseExt::ExtMap.Find(pThis->Owner)->OwnedCountedHarvesters;
		vec.erase(std::remove(vec.begin(), vec.end(), pThis), vec.end());
	}

	for (auto const pBolt : this->ElectricBolts)
	{
		pBolt->Owner = nullptr;
	}

	this->ElectricBolts.clear();
}

bool TechnoExt::IsActiveIgnoreEMP(TechnoClass* pThis)
{
	return pThis
		&& pThis->IsAlive
		&& pThis->Health > 0
		&& !pThis->InLimbo
		&& !pThis->TemporalTargetingMe
		&& !pThis->BeingWarpedOut
		;
}

bool TechnoExt::IsActive(TechnoClass* pThis)
{
	return TechnoExt::IsActiveIgnoreEMP(pThis)
		&& !pThis->Deactivated
		&& !pThis->IsUnderEMP()
		;
}

bool TechnoExt::IsHarvesting(TechnoClass* pThis)
{
	if (!TechnoExt::IsActive(pThis))
		return false;

	auto const pSlaveManager = pThis->SlaveManager;

	if (pSlaveManager && pSlaveManager->State != SlaveManagerStatus::Ready)
		return true;

	if (pThis->WhatAmI() == AbstractType::Building)
		return pThis->IsPowerOnline();

	if (TechnoExt::HasAvailableDock(pThis))
	{
		switch (pThis->GetCurrentMission())
		{
		case Mission::Harvest:
			if (auto const pUnit = abstract_cast<UnitClass*, true>(pThis))
			{
				if (pUnit->HasAnyLink() && !TechnoExt::HasRadioLinkWithDock(pUnit)) // Probably still in factory.
					return false;

				if (pUnit->IsUseless) // Harvesters currently sitting without purpose are idle even if they are on harvest mission.
					return false;
			}
			return true;
		case Mission::Unload:
			return true;
		case Mission::Enter:
			if (pThis->HasAnyLink())
			{
				auto const pLink = pThis->GetNthLink(0);

				if (pLink->WhatAmI() != AbstractType::Building) // Enter mission + non-building link = not trying to unload
					return false;
			}
			return true;
		case Mission::Guard:
			if (auto pUnit = abstract_cast<UnitClass*, true>(pThis))
			{
				if (pUnit->ArchiveTarget && pUnit->GetStoragePercentage() > 0.0 && pUnit->Locomotor->Is_Moving()) // Edge-case, waiting to be able to unload.
					return true;
			}
			return false;
		default:
			return false;
		}
	}

	return false;
}

bool TechnoExt::HasAvailableDock(TechnoClass* pThis)
{
	for (auto const pBld : pThis->GetTechnoType()->Dock)
	{
		if (pThis->Owner->CountOwnedAndPresent(pBld))
			return true;
	}

	return false;
}

bool TechnoExt::HasRadioLinkWithDock(TechnoClass* pThis)
{
	if (pThis->HasAnyLink())
	{
		auto const pLink = abstract_cast<BuildingClass*, true>(pThis->GetNthLink(0));

		if (pLink && pThis->GetTechnoType()->Dock.FindItemIndex(pLink->Type) >= 0)
			return true;
	}

	return false;
}

// Syncs Iron Curtain or Force Shield timer to another techno.
void TechnoExt::SyncInvulnerability(TechnoClass* pFrom, TechnoClass* pTo)
{
	if (pFrom->IsIronCurtained())
	{
		const auto pTypeExt = TechnoExt::ExtMap.Find(pFrom)->TypeExtData;
		const bool isForceShielded = pFrom->ForceShielded;
		const bool allowSyncing = !isForceShielded
			? pTypeExt->IronCurtain_KeptOnDeploy.Get(RulesExt::Global()->IronCurtain_KeptOnDeploy)
			: pTypeExt->ForceShield_KeptOnDeploy.Get(RulesExt::Global()->ForceShield_KeptOnDeploy);

		if (allowSyncing)
		{
			pTo->IronCurtainTimer = pFrom->IronCurtainTimer;
			pTo->IronTintStage = pFrom->IronTintStage;
			pTo->ForceShielded = isForceShielded;
		}
	}
}

double TechnoExt::GetCurrentSpeedMultiplier(FootClass* pThis)
{
	double houseMultiplier = 1.0;
	auto const whatAmI = pThis->WhatAmI();

	if (whatAmI == AbstractType::Aircraft)
		houseMultiplier = pThis->Owner->Type->SpeedAircraftMult;
	else if (whatAmI == AbstractType::Infantry)
		houseMultiplier = pThis->Owner->Type->SpeedInfantryMult;
	else
		houseMultiplier = pThis->Owner->Type->SpeedUnitsMult;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	return pThis->SpeedMultiplier * houseMultiplier * pExt->AE.SpeedMultiplier *
		(pThis->HasAbility(Ability::Faster) ? RulesClass::Instance->VeteranSpeed : 1.0);
}

double TechnoExt::GetCurrentFirepowerMultiplier(TechnoClass* pThis)
{
	return pThis->FirepowerMultiplier * TechnoExt::ExtMap.Find(pThis)->AE.FirepowerMultiplier *
		(pThis->HasAbility(Ability::Firepower) ? RulesClass::Instance->VeteranCombat : 1.0);
}

CoordStruct TechnoExt::PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts = 1)
{
	if (!pThis || !pPassenger)
		return CoordStruct::Empty;

	if (maxAttempts < 1)
		maxAttempts = 1;

	const auto pTypePassenger = pPassenger->GetTechnoType();
	auto placeCoords = CellStruct::Empty;
	short extraDistance = 1;
	auto speedType = pTypePassenger->SpeedType;
	auto movementZone = pTypePassenger->MovementZone;

	if (pTypePassenger->WhatAmI() == AbstractType::AircraftType)
	{
		speedType = SpeedType::Track;
		movementZone = MovementZone::Normal;
	}
	do
	{
		placeCoords = pThis->GetMapCoords() - CellStruct { static_cast<short>(extraDistance / 2), static_cast<short>(extraDistance / 2) };
		placeCoords = MapClass::Instance.NearByLocation(placeCoords, speedType, -1, movementZone, false, extraDistance, extraDistance, true, false, false, false, CellStruct::Empty, false, false);

		if (placeCoords == CellStruct::Empty)
			return CoordStruct::Empty;

		const auto pCell = MapClass::Instance.GetCellAt(placeCoords);

		if (pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) == Move::OK)
			break;

		extraDistance++;
	}
	while (extraDistance <= maxAttempts);

	if (const auto pCell = MapClass::Instance.TryGetCellAt(placeCoords))
		return pCell->GetCoordsWithBridge();

	return CoordStruct::Empty;
}

bool TechnoExt::AllowedTargetByZone(TechnoClass* pThis, TechnoClass* pTarget, TargetZoneScanType zoneScanType, WeaponTypeClass* pWeapon, bool useZone, int zone)
{
	if (!pThis || !pTarget)
		return false;

	if (pThis->WhatAmI() == AbstractType::Aircraft)
		return true;

	auto const pType = pThis->GetTechnoType();
	auto const mZone = pType->MovementZone;
	int currentZone = useZone ? zone : MapClass::Instance.GetMovementZoneType(pThis->GetMapCoords(), mZone, pThis->OnBridge);

	if (currentZone != -1)
	{
		if (zoneScanType == TargetZoneScanType::Any)
			return true;

		int targetZone = MapClass::Instance.GetMovementZoneType(pTarget->GetMapCoords(), mZone, pTarget->OnBridge);

		if (zoneScanType == TargetZoneScanType::Same)
		{
			if (currentZone != targetZone)
				return false;
		}
		else
		{
			if (currentZone == targetZone)
				return true;

			auto const speedType = pType->SpeedType;
			auto const cellStruct = MapClass::Instance.NearByLocation(CellClass::Coord2Cell(pTarget->Location),
				speedType, -1, mZone, false, 1, 1, true,
				false, false, speedType != SpeedType::Float, CellStruct::Empty, false, false);

			if (cellStruct == CellStruct::Empty)
				return false;

			auto const pCell = MapClass::Instance.TryGetCellAt(cellStruct);

			if (!pCell)
				return false;

			if (!pWeapon)
			{
				int weaponIndex = pThis->SelectWeapon(pTarget);

				if (weaponIndex < 0)
					return false;

				pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
			}

			const double distance = pCell->GetCoordsWithBridge().DistanceFrom(pTarget->GetCenterCoords());

			if (distance > pWeapon->Range)
				return false;
		}
	}

	return true;
}

// Feature for common usage : TechnoType conversion -- Trsdy
// BTW, who said it was merely a Type pointer replacement and he could make a better one than Ares?
bool TechnoExt::ConvertToType(FootClass* pThis, TechnoTypeClass* pToType)
{
	const auto pType = pThis->GetTechnoType();

	auto const pLocoBefore = pThis->Locomotor.GetInterfacePtr();
	CrashTrace::Push(CrashTrace::TypeConvertEnter, pThis, pToType, pType,
		pType ? pType->get_ID() : nullptr, pToType ? pToType->get_ID() : nullptr, pLocoBefore,
		pLocoBefore ? CrashTrace::GetVTable(pLocoBefore) : nullptr, -1);

	// It really should be at the beginning.
	if (pType == pToType || pType->WhatAmI() != pToType->WhatAmI())
	{
		Debug::Log("Incompatible types between %s and %s\n", pThis->get_ID(), pToType->get_ID());
		return false;
	}

	if (AresFunctions::ConvertTypeTo)
	{
		const int oldHealth = pThis->Health;

		if (AresFunctions::ConvertTypeTo(pThis, pToType))
		{
			// Fixed an issue where morphing could result in -1 health.
			const double ratio = static_cast<double>(pToType->Strength) / pType->Strength;
			pThis->Health = static_cast<int>(oldHealth * ratio + 0.5);

			auto const pTypeExt = TechnoExt::ExtMap.Find(pThis);
			pTypeExt->UpdateTypeData(pToType);
			pTypeExt->UpdateTypeData_Foot();

			auto const pLocoAfter = pThis->Locomotor.GetInterfacePtr();
			CrashTrace::Push(CrashTrace::TypeConvertLeave, pThis, pToType, pToType,
				pToType->get_ID(), pToType->get_ID(), pLocoAfter,
				pLocoAfter ? CrashTrace::GetVTable(pLocoAfter) : nullptr, -1, 1);

			return true;
		}

		return false;
	}

	// In case not using Ares 3.0. Only update necessary vanilla properties
	AbstractType rtti;
	TechnoTypeClass** nowTypePtr;

	// Different types prohibited
	switch (pThis->WhatAmI())
	{
	case AbstractType::Infantry:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<InfantryClass*>(pThis)->Type));
		rtti = AbstractType::InfantryType;
		break;
	case AbstractType::Unit:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<UnitClass*>(pThis)->Type));
		rtti = AbstractType::UnitType;
		break;
	case AbstractType::Aircraft:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<AircraftClass*>(pThis)->Type));
		rtti = AbstractType::AircraftType;
		break;
	default:
		Debug::Log("%s is not FootClass, conversion not allowed\n", pToType->get_ID());
		return false;
	}

	// Detach CLEG targeting
	auto const tempUsing = pThis->TemporalImUsing;
	if (tempUsing && tempUsing->Target)
		tempUsing->LetGo();

	auto const pOwner = pThis->Owner;

	// Remove tracking of old techno
	if (!pThis->InLimbo)
		pOwner->RegisterLoss(pThis, false);
	pOwner->RemoveTracking(pThis);

	const int oldHealth = pThis->Health;

	// Generic type-conversion
	auto const prevType = *nowTypePtr;
	*nowTypePtr = pToType;

	// Readjust health according to percentage
	pThis->SetHealthPercentage((double)(oldHealth) / (double)prevType->Strength);
	pThis->EstimatedHealth = pThis->Health;

	// Add tracking of new techno
	pOwner->AddTracking(pThis);
	if (!pThis->InLimbo)
		pOwner->RegisterGain(pThis, false);
	pOwner->RecheckTechTree = true;

	// Update Ares AttachEffects -- skipped
	// Ares RecalculateStats -- skipped

	// Adjust ammo
	pThis->Ammo = Math::min(pThis->Ammo, pToType->Ammo);
	// Ares ResetSpotlights -- skipped

	// Adjust ROT
	if (rtti == AbstractType::AircraftType)
		pThis->SecondaryFacing.SetROT(pToType->ROT);
	else
		pThis->PrimaryFacing.SetROT(pToType->ROT);
	// Adjust Ares TurretROT -- skipped
	//  pThis->SecondaryFacing.SetROT(TechnoTypeExt::ExtMap.Find(pToType)->TurretROT.Get(pToType->ROT));

	// Locomotor change, referenced from Ares 0.A's abduction code, not sure if correct, untested
	CLSID nowLocoID;
	ILocomotion* iloco = pThis->Locomotor;
	const auto& toLoco = pToType->Locomotor;

	// A stale locomotor is one of the few things this function cannot recover
	// from: calling GetClassID/End_Piggyback on it jumps through a dead vtable.
	// Detect it, leave the pointer alone and let the GetFireError guard keep the
	// object from using it.
	const bool locoSane = iloco && PointerSanity::IsSaneLocomotor(iloco);

	if (iloco && !locoSane)
	{
		CrashTrace::Push(CrashTrace::LocomotorSuspect, pThis, nullptr, pType,
			pType ? pType->get_ID() : nullptr, nullptr, iloco,
			CrashTrace::GetVTable(iloco), -1);
		Debug::Log("[Locomotor] %s has a corrupt locomotor %p; conversion will not touch it\n",
			pThis->get_ID(), iloco);
	}

	if (locoSane
		&& SUCCEEDED(static_cast<LocomotionClass*>(iloco)->GetClassID(&nowLocoID))
		&& nowLocoID != toLoco)
	{
		// because we are throwing away the locomotor in a split second, piggybacking
		// has to be stopped. otherwise the object might remain in a weird state.
		while (LocomotionClass::End_Piggyback(pThis->Locomotor));
		// throw away the current locomotor and instantiate
		// a new one of the default type for this unit.
		if (auto const newLoco = LocomotionClass::CreateInstance(toLoco))
		{
			newLoco->Link_To_Object(pThis);
			auto const pNewLoco = newLoco.GetInterfacePtr();

			Debug::Log("[Locomotor] %s converted %s -> %s (%p -> %p)\n",
				pThis->get_ID(), pType->get_ID(), pToType->get_ID(), iloco, pNewLoco);

			pThis->Locomotor = std::move(newLoco);

			CrashTrace::Push(CrashTrace::LocomotorReplaced, pThis, nullptr, pToType,
				pToType->get_ID(), nullptr, pNewLoco,
				pNewLoco ? CrashTrace::GetVTable(pNewLoco) : nullptr, -1);
		}
	}

	const auto& jjLoco = LocomotionClass::CLSIDs::Jumpjet;
	if (pToType->BalloonHover && pToType->DeployToLand && prevType->Locomotor != jjLoco && toLoco == jjLoco)
	{
		if (auto const pLoco = pThis->Locomotor.GetInterfacePtr();
			pLoco && PointerSanity::IsSaneLocomotor(pLoco))
		{
			pThis->Locomotor->Move_To(pThis->Location);
		}
	}

	auto const pTypeExt = TechnoExt::ExtMap.Find(pThis);
	pTypeExt->UpdateTypeData(pToType);
	pTypeExt->UpdateTypeData_Foot();

	auto const pLocoAfter = pThis->Locomotor.GetInterfacePtr();
	CrashTrace::Push(CrashTrace::TypeConvertLeave, pThis, pToType, pToType,
		pToType->get_ID(), pToType->get_ID(), pLocoAfter,
		pLocoAfter ? CrashTrace::GetVTable(pLocoAfter) : nullptr, -1, 1);

	return true;
}

// Checks if vehicle can deploy into a building at its current location. If unit has no DeploysInto set returns noDeploysIntoDefaultValue (def = false) instead.
bool TechnoExt::CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue)
{
	if (!pThis)
		return false;

	auto const pDeployType = pThis->Type->DeploysInto;

	if (!pDeployType)
		return noDeploysIntoDefaultValue;

	auto mapCoords = CellClass::Coord2Cell(pThis->GetCoords());

	if (pDeployType->GetFoundationWidth() > 2 || pDeployType->GetFoundationHeight(false) > 2)
		mapCoords += CellStruct { -1, -1 };

	// The vanilla game used an inappropriate approach here, resulting in potential risk of desync.
	// Now, through additional checks, we can directly exclude the unit who want to deploy.
	TechnoExt::Deployer = pThis;
	const bool canDeploy = pDeployType->CanCreateHere(mapCoords, pThis->Owner);
	TechnoExt::Deployer = nullptr;

	return canDeploy;
}

bool TechnoExt::IsTypeImmune(TechnoClass* pThis, TechnoClass* pSource)
{
	if (!pThis || !pSource)
		return false;

	auto const pType = pThis->GetTechnoType();

	if (!pType->TypeImmune)
		return false;

	if (pType == pSource->GetTechnoType() && pThis->Owner == pSource->Owner)
		return true;

	return false;
}

/// <summary>
/// Gets whether or not techno has listed AttachEffect types active on it
/// </summary>
/// <param name="attachEffectTypes">Attacheffect types.</param>
/// <param name="requireAll">Whether or not to require all listed types to be present or if only one will satisfy the check.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>True if techno has active AttachEffects that satisfy the source, false if not.</returns>
bool TechnoExt::ExtData::HasAttachedEffects(std::vector<AttachEffectTypeClass*> attachEffectTypes, bool requireAll, bool ignoreSameSource,
	TechnoClass* pInvoker, AbstractClass* pSource, std::vector<int> const* minCounts, std::vector<int> const* maxCounts) const
{
	unsigned int foundCount = 0;
	unsigned int typeCounter = 1;
	const bool checkSource = ignoreSameSource && pInvoker && pSource;

	for (auto const& type : attachEffectTypes)
	{
		for (auto const& attachEffect : this->AttachedEffects)
		{
			if (attachEffect->GetType() == type && attachEffect->IsActive())
			{
				if (checkSource && attachEffect->IsFromSource(pInvoker, pSource))
					continue;

				const unsigned int minSize = minCounts ? minCounts->size() : 0;
				const unsigned int maxSize = maxCounts ? maxCounts->size() : 0;

				if (type->Cumulative && (minSize > 0 || maxSize > 0))
				{
					const int cumulativeCount = this->GetAttachedEffectCumulativeCount(type, ignoreSameSource, pInvoker, pSource);

					if (minSize > 0)
					{
						if (cumulativeCount < minCounts->at(typeCounter - 1 >= minSize ? minSize - 1 : typeCounter - 1))
							continue;
					}
					if (maxSize > 0)
					{
						if (cumulativeCount > maxCounts->at(typeCounter - 1 >= maxSize ? maxSize - 1 : typeCounter - 1))
							continue;
					}
				}

				// Only need to find one match, can stop here.
				if (!requireAll)
					return true;

				foundCount++;
				break;
			}
		}

		// One of the required types was not found, can stop here.
		if (requireAll && foundCount < typeCounter)
			return false;

		typeCounter++;
	}

	if (requireAll && foundCount == attachEffectTypes.size())
		return true;

	return false;
}

/// <summary>
/// Gets how many counts of same cumulative AttachEffect type instance techno has active on it.
/// </summary>
/// <param name="pAttachEffectType">AttachEffect type.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>Number of active cumulative AttachEffect type instances on the techno. 0 if the AttachEffect type is not cumulative.</returns>
int TechnoExt::ExtData::GetAttachedEffectCumulativeCount(AttachEffectTypeClass* pAttachEffectType, bool ignoreSameSource, TechnoClass* pInvoker, AbstractClass* pSource) const
{
	if (!pAttachEffectType->Cumulative)
		return 0;

	unsigned int foundCount = 0;
	const bool checkSource = ignoreSameSource && pInvoker && pSource;

	for (auto const& attachEffect : this->AttachedEffects)
	{
		if (attachEffect->GetType() == pAttachEffectType && attachEffect->IsActive())
		{
			if (checkSource && attachEffect->IsFromSource(pInvoker, pSource))
				continue;

			foundCount++;
		}
	}

	return foundCount;
}

bool TechnoExt::IsOutOfAmmoOrSpawns(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	// Spawner with nothing docked: the missile is in flight or not yet reloaded.
	if (pThis->SpawnManager && pThis->SpawnManager->CountDockedSpawns() == 0)
		return true;

	// Limited-ammo unit that has fired everything.
	auto const pType = pThis->GetTechnoType();
	return pType && pType->Ammo > 0 && pThis->Ammo <= 0;
}

UnitTypeClass* TechnoExt::GetUnitTypeExtra(UnitClass* pUnit)
{
	auto const pData = TechnoTypeExt::ExtMap.Find(pUnit->Type);
	const bool onWater = pUnit->GetCell()->LandType == LandType::Water && !pUnit->OnBridge;

	// While empty, use Image.Empty / WaterImage.Empty and never fall through to
	// the damaged images (empty takes priority). If no empty image is configured
	// return nullptr, so the SHP draw hook can fall back to Ares' NoSpawnAlt
	// AltImage for this unit.
	if (IsOutOfAmmoOrSpawns(pUnit))
	{
		if (onWater)
		{
			if (auto const imageEmpty = pData->WaterImage_Empty)
				return imageEmpty;
		}
		else if (auto const imageEmpty = pData->Image_Empty)
		{
			return abstract_cast<UnitTypeClass*, true>(imageEmpty);
		}

		return nullptr;
	}

	if (pUnit->IsGreenHP())
	{
		return nullptr;
	}
	else if (pUnit->IsYellowHP())
	{
		if (onWater)
		{
			if (auto const imageYellow = pData->WaterImage_ConditionYellow)
				return imageYellow;
		}
		else if (auto const imageYellow = pData->Image_ConditionYellow)
		{
			return abstract_cast<UnitTypeClass*, true>(imageYellow);
		}
	}
	else
	{
		if (onWater)
		{
			if (auto const imageRed = pData->WaterImage_ConditionRed)
				return imageRed;
			else if (auto const imageYellow = pData->WaterImage_ConditionYellow)
				return imageYellow;
		}
		else if (auto const imageRed = pData->Image_ConditionRed)
		{
			return abstract_cast<UnitTypeClass*, true>(imageRed);
		}
		else if (auto const imageYellow = pData->Image_ConditionYellow)
		{
			return abstract_cast<UnitTypeClass*, true>(imageYellow);
		}
	}

	return nullptr;
}

AircraftTypeClass* TechnoExt::GetAircraftTypeExtra(AircraftClass* pAircraft)
{
	auto const pData = TechnoTypeExt::ExtMap.Find(pAircraft->Type);

	// Empty aircraft use Image.Empty (no WaterImage counterpart). If not
	// configured, keep the normal type so callers never receive a null type.
	if (IsOutOfAmmoOrSpawns(pAircraft))
	{
		if (auto const imageEmpty = pData->Image_Empty)
			return abstract_cast<AircraftTypeClass*, true>(imageEmpty);

		return pAircraft->Type;
	}

	if (pAircraft->IsGreenHP())
	{
		return pAircraft->Type;
	}
	else if (pAircraft->IsYellowHP())
	{
		if (auto const imageYellow = pData->Image_ConditionYellow)
			return abstract_cast<AircraftTypeClass*, true>(imageYellow);
	}
	else
	{
		if (auto const imageRed = pData->Image_ConditionRed)
			return abstract_cast<AircraftTypeClass*, true>(imageRed);
		else if (auto const imageYellow = pData->Image_ConditionYellow)
			return abstract_cast<AircraftTypeClass*, true>(imageYellow);
	}

	return pAircraft->Type;
}

void TechnoExt::ExtData::ResetDelayedFireTimer()
{
	this->DelayedFireTimer.Stop();
	this->DelayedFireWeaponIndex = -1;
	this->DelayedFireSequencePaused = false;

	if (this->CurrentDelayedFireAnim)
	{
		if (AnimExt::ExtMap.Find(this->CurrentDelayedFireAnim)->DelayedFireRemoveOnNoDelay)
			this->CurrentDelayedFireAnim->UnInit();
	}
}

void TechnoExt::CreateDelayedFireAnim(TechnoClass* pThis, AnimTypeClass* pAnimType, int weaponIndex, bool attach, bool center, bool removeOnNoDelay, bool onTurret, CoordStruct firingCoords)
{
	if (pAnimType)
	{
		auto coords = pThis->GetCenterCoords();

		if (!center)
			coords = TechnoExt::GetFLHAbsoluteCoords(pThis, firingCoords, onTurret);

		auto const pAnim = GameCreate<AnimClass>(pAnimType, coords);

		if (attach)
			pAnim->SetOwnerObject(pThis);

		auto const pAnimExt = AnimExt::ExtMap.Find(pAnim);
		pAnim->Owner = pThis->Owner;
		pAnimExt->SetInvoker(pThis);

		if (attach)
		{
			pAnimExt->DelayedFireRemoveOnNoDelay = removeOnNoDelay;
			TechnoExt::ExtMap.Find(pThis)->CurrentDelayedFireAnim = pAnim;
		}
	}
}

bool TechnoExt::HandleDelayedFireWithPauseSequence(TechnoClass* pThis, int weaponIndex, int firingFrame)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto& timer = pExt->DelayedFireTimer;
	auto const pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);

	if (pExt->DelayedFireWeaponIndex >= 0 && pExt->DelayedFireWeaponIndex != weaponIndex)
	{
		pExt->ResetDelayedFireTimer();
		pExt->DelayedFireSequencePaused = false;
	}

	if (pWeaponExt->DelayedFire_PauseFiringSequence && pWeaponExt->DelayedFire_Duration.isset() && (!pThis->Transporter || !pWeaponExt->DelayedFire_SkipInTransport))
	{
		if (pWeapon->Burst <= 1 || !pWeaponExt->DelayedFire_OnlyOnInitialBurst || pThis->CurrentBurstIndex == 0)
		{
			if (pThis->Animation.Value == firingFrame)
				pExt->DelayedFireSequencePaused = true;

			if (!timer.HasStarted())
			{
				pExt->DelayedFireWeaponIndex = weaponIndex;
				timer.Start(Math::max(GeneralUtils::GetRangedRandomOrSingleValue(pWeaponExt->DelayedFire_Duration), 0));
				auto pAnimType = pWeaponExt->DelayedFire_Animation;

				if (pThis->Transporter && pWeaponExt->DelayedFire_OpenToppedAnimation.isset())
					pAnimType = pWeaponExt->DelayedFire_OpenToppedAnimation;

				auto firingCoords = pThis->GetWeapon(weaponIndex)->FLH;

				if (pWeaponExt->DelayedFire_AnimOffset.isset())
					firingCoords = pWeaponExt->DelayedFire_AnimOffset;

				TechnoExt::CreateDelayedFireAnim(pThis, pAnimType, weaponIndex, pWeaponExt->DelayedFire_AnimIsAttached, pWeaponExt->DelayedFire_CenterAnimOnFirer,
					pWeaponExt->DelayedFire_RemoveAnimOnNoDelay, pWeaponExt->DelayedFire_AnimOnTurret, firingCoords);

				return true;
			}
			else if (timer.InProgress())
			{
				return true;
			}

			if (timer.Completed())
				pExt->ResetDelayedFireTimer();
		}

		pExt->DelayedFireSequencePaused = false;
	}

	return false;
}

bool TechnoExt::IsHealthInThreshold(TechnoClass* pObject, double min, double max)
{
	const double hp = pObject->GetHealthPercentage();
	return hp <= max && hp >= min;
}

bool TechnoExt::CannotMove(UnitClass* pThis)
{
	const auto pType = pThis->Type;

	if (pType->Speed == 0)
		return true;

	if (!locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
	{
		LandType landType = pThis->GetCell()->LandType;
		const LandType movementRestrictedTo = pType->MovementRestrictedTo;

		if (pThis->OnBridge
			&& (landType == LandType::Water || landType == LandType::Beach))
		{
			landType = LandType::Road;
		}

		if (movementRestrictedTo != LandType::None
			&& movementRestrictedTo != landType
			&& landType != LandType::Tunnel)
		{
			return true;
		}
	}

	return false;
}

bool TechnoExt::HasAmmoToDeploy(TechnoClass* pThis)
{
	const auto pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;

	const int min = pTypeExt->Ammo_DeployUnlockMinimumAmount;
	const int max = pTypeExt->Ammo_DeployUnlockMaximumAmount;

	if (min < 0 && max < 0)
		return true;

	const int ammo = pThis->Ammo;

	if ((min < 0 || ammo >= min) && (max < 0 || ammo <= max))
		return true;

	return false;
}

void TechnoExt::HandleOnDeployAmmoChange(TechnoClass* pThis, int maxAmmoOverride)
{
	const auto pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;
	int add = pTypeExt->Ammo_AddOnDeploy;

	if (add != 0)
	{
		int maxAmmo = pTypeExt->OwnerObject()->Ammo;

		if (maxAmmoOverride >= 0)
			maxAmmo = maxAmmoOverride;

		int originalAmmo = pThis->Ammo;
		pThis->Ammo = std::clamp(pThis->Ammo + add, 0, maxAmmo);

		if (originalAmmo != pThis->Ammo)
		{
			pThis->StartReloading();
			pThis->Mark(MarkType::Change);
		}
	}

}

// =============================
// per-weapon ammo

void TechnoExt::ResolveWeaponAmmoConfig(TechnoClass* pThis, int slot, WeaponAmmoConfig& out, WeaponTypeClass* pWeapon)
{
	out = WeaponAmmoConfig {};

	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
	{
		out.Infinite = true;
		return;
	}

	auto const pType = pThis->GetTechnoType();

	if (!pWeapon)
		pWeapon = TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite());

	out.Weapon = pWeapon;

	if (!pWeapon)
	{
		out.Infinite = true;
		return;
	}

	// [General] AresWeaponAmmo=no disables the per-weapon ammo pools and falls
	// back to the vanilla single unit Ammo pool. ExtraFire entries are forced
	// unlimited in RefreshExtraFireAmmoEntry.
	if (!Phobos::Config::AresWeaponAmmo)
		return;

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	auto const& override = pTypeExt->WeaponAmmo[slot];

	// Resolve the maximum first: > 0 means a pool, 0 means explicit unlimited,
	// unset/-1 falls back to the unit Ammo when the type has one.
	int maxAmmo = -1;

	if (override.Ammo.isset())
		maxAmmo = override.Ammo.Get();
	else if (pWeaponExt->Ammo.isset())
		maxAmmo = pWeaponExt->Ammo.Get();

	if (maxAmmo > 0)
	{
		out.HasPool = true;
		out.Max = maxAmmo;
	}
	else if (maxAmmo == 0)
	{
		out.Infinite = true;
		return;
	}
	else if (pType->Ammo > 0)
	{
		// No per-weapon pool: keep the vanilla unit Ammo behavior.
		return;
	}
	else
	{
		out.Infinite = true;
		return;
	}

	auto const pickInt = [](const Nullable<int>& overrideValue, const Nullable<int>& weaponValue, int fallback) -> int
	{
		if (overrideValue.isset())
			return overrideValue.Get();

		if (weaponValue.isset())
			return weaponValue.Get();

		return fallback;
	};

	out.Initial = pickInt(override.Initial, pWeaponExt->Ammo_Initial, -1);
	out.Reload = pickInt(override.Reload, pWeaponExt->Ammo_Reload, -1);
	out.EmptyReload = pickInt(override.EmptyReload, pWeaponExt->Ammo_Reload_Empty, -1);
	out.ReloadAmount = override.ReloadAmount.isset()
		? override.ReloadAmount.Get()
		: pWeaponExt->Ammo_Reload_Amount.Get();
	out.Cost = override.Cost.isset()
		? override.Cost.Get()
		: pWeaponExt->Ammo_Cost.Get();

	if (out.ReloadAmount < 1)
		out.ReloadAmount = 1;

	if (out.Cost < 1)
		out.Cost = 1;

	// Vanilla unit ammo semantics: EmptyReload <= 0 means "use Reload when empty".
	if (out.EmptyReload < 0)
		out.EmptyReload = out.Reload;

	if (override.PerBurst.isset())
		out.PerBurst = override.PerBurst.Get();
	else
		out.PerBurst = pWeaponExt->Ammo_PerBurst.Get();

}

void TechnoExt::RefreshWeaponAmmoSlot(TechnoClass* pThis, int slot)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return;

	auto& runtime = pExt->WeaponAmmo[slot];
	WeaponAmmoConfig config;
	ResolveWeaponAmmoConfig(pThis, slot, config);

	const bool hadPool = runtime.HasPool;

	runtime.HasPool = config.HasPool;
	runtime.Infinite = config.Infinite;
	runtime.Max = config.Max;
	runtime.Reload = config.Reload;
	runtime.EmptyReload = config.EmptyReload;
	runtime.ReloadAmount = config.ReloadAmount;
	runtime.Cost = config.Cost;
	runtime.PerBurst = config.PerBurst;
	runtime.Weapon = config.Weapon;

	const bool wasInitialized = runtime.Initialized;
	const bool preserveLoaded = pExt->WeaponAmmoLoadedFromSave && !runtime.Initialized;

	if (preserveLoaded)
	{
		// Counts and timers came from a savegame; only clamp them to the resolved pool.
		runtime.Initialized = true;

		if (runtime.HasPool)
			pExt->WeaponAmmoCount[slot] = Math::clamp(pExt->WeaponAmmoCount[slot], 0, runtime.Max);
		else
			pExt->WeaponAmmoCount[slot] = 0;
	}
	else if (!runtime.Initialized || (!hadPool && runtime.HasPool))
	{
		if (runtime.HasPool)
		{
			const int initial = config.Initial < 0 ? runtime.Max : config.Initial;
			pExt->WeaponAmmoCount[slot] = Math::clamp(initial, 0, runtime.Max);
		}
		else
		{
			pExt->WeaponAmmoCount[slot] = 0;
		}

		pExt->WeaponAmmoReloadTimer[slot].Stop();
		runtime.Initialized = true;
	}
	else if (runtime.HasPool)
	{
		pExt->WeaponAmmoCount[slot] = Math::clamp(pExt->WeaponAmmoCount[slot], 0, runtime.Max);

		if (pExt->WeaponAmmoCount[slot] >= runtime.Max)
			pExt->WeaponAmmoReloadTimer[slot].Stop();
	}
	else
	{
		pExt->WeaponAmmoCount[slot] = 0;
		pExt->WeaponAmmoReloadTimer[slot].Stop();
	}

	if (runtime.HasPool)
		StartWeaponAmmoReload(pThis, slot);

	// Only log slots that actually own a pool; empty slots would flood the log.
	if (!wasInitialized && runtime.HasPool)
	{
		Debug::Log("[WeaponAmmo] [%s] slot=%d weapon=%s initialized=%d/%d preserve=%d\n",
			pThis->GetTechnoType()->ID, slot,
			runtime.Weapon ? runtime.Weapon->ID : "<none>",
			pExt->WeaponAmmoCount[slot], runtime.Max,
			preserveLoaded ? 1 : 0);
	}
}

void TechnoExt::RefreshAllWeaponAmmo(TechnoClass* pThis)
{
	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return;

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
		RefreshWeaponAmmoSlot(pThis, slot);

	// Resolve every ExtraFire pool while the "counts loaded" flag is still false on a
	// fresh unit, so each entry starts at its configured initial amount.
	const auto pType = pThis->GetTechnoType();
	const bool isElite = pThis->Veterancy.IsElite();

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
	{
		auto const pParentWeapon = TechnoTypeExt::GetWeaponType(pType, slot, isElite);

		if (!pParentWeapon)
			continue;

		const auto pParentExt = WeaponTypeExt::ExtMap.Find(pParentWeapon);
		const int count = static_cast<int>(pParentExt->ExtraFire_Weapons.size());

		for (int entry = 0; entry < count; ++entry)
			RefreshExtraFireAmmoEntry(pThis, slot, entry);
	}

	pExt->WeaponAmmoLoadedFromSave = false;
	pExt->ExtraFireAmmoLoadedFromSave = false;
	pExt->WeaponAmmoInitialized = true;
	pExt->WeaponAmmoElite = isElite;
	UpdateAircraftAmmoCompat(pThis);
}

bool TechnoExt::CanFireWeaponSlot(TechnoClass* pThis, int slot, bool allowMidBurst, int costOverride)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return true;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return true;

	RefreshWeaponAmmoSlot(pThis, slot);
	auto const& runtime = pExt->WeaponAmmo[slot];

	if (runtime.HasPool)
	{
		if (allowMidBurst && runtime.PerBurst && pThis->CurrentBurstIndex > 0)
			return true;

		const int cost = costOverride >= 0 ? costOverride : runtime.Cost;

		return pExt->WeaponAmmoCount[slot] >= Math::max(1, cost);
	}

	if (runtime.Infinite)
		return true;

	// Vanilla unit Ammo fallback (weapon has no Ammo and the type has Ammo > 0).
	auto const pType = pThis->GetTechnoType();

	return pType->Ammo <= 0 || pThis->Ammo > 0;
}

bool TechnoExt::ConsumeWeaponAmmo(TechnoClass* pThis, int slot, int amount)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return false;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return false;

	RefreshWeaponAmmoSlot(pThis, slot);
	auto const& runtime = pExt->WeaponAmmo[slot];

	if (!runtime.HasPool)
		return false;

	const int cost = Math::max(1, amount >= 0 ? amount : runtime.Cost);
	pExt->WeaponAmmoCount[slot] = Math::max(0, pExt->WeaponAmmoCount[slot] - cost);
	StartWeaponAmmoReload(pThis, slot);
	UpdateAircraftAmmoCompat(pThis);

	if (pExt->WeaponAmmoCount[slot] == 0)
	{
		Debug::Log("[WeaponAmmo] [%s] slot=%d weapon=%s reached 0\n",
			pThis->GetTechnoType()->ID, slot,
			runtime.Weapon ? runtime.Weapon->ID : "<none>");
	}

	return true;
}

void TechnoExt::StartWeaponAmmoReload(TechnoClass* pThis, int slot)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return;

	auto& runtime = pExt->WeaponAmmo[slot];
	auto& timer = pExt->WeaponAmmoReloadTimer[slot];

	if (!runtime.HasPool || pExt->WeaponAmmoCount[slot] >= runtime.Max)
	{
		timer.Stop();
		return;
	}

	if (timer.InProgress())
		return;

	const int delay = pExt->WeaponAmmoCount[slot] <= 0 ? runtime.EmptyReload : runtime.Reload;

	if (delay < 0)
	{
		timer.Stop();
		return;
	}

	timer.Start(delay);
}

void TechnoExt::UpdateWeaponAmmoReload(TechnoClass* pThis)
{
	if (!pThis)
		return;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return;

	const bool atAirfield = IsAtAirfield(pThis);
	const auto pType = pThis->GetTechnoType();

	int padReload = -1;
	int padEmptyReload = -1;
	int padIncrement = 1;

	if (atAirfield)
	{
		padReload = pType->Reload > 0
			? pType->Reload
			: Math::max(1, static_cast<int>(RulesClass::Instance->ReloadRate));
		padEmptyReload = pType->EmptyReload > 0 ? pType->EmptyReload : padReload;
		padIncrement = pType->ReloadIncrement > 0 ? pType->ReloadIncrement : 1;
	}

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
	{
		auto& runtime = pExt->WeaponAmmo[slot];

		if (!runtime.HasPool)
			continue;

		auto& timer = pExt->WeaponAmmoReloadTimer[slot];
		int& ammo = pExt->WeaponAmmoCount[slot];

		if (ammo >= runtime.Max)
		{
			timer.Stop();
			continue;
		}

		const int increment = atAirfield ? padIncrement : runtime.ReloadAmount;

		if (increment <= 0)
			continue;

		if (timer.InProgress())
			continue;

		const int delay = atAirfield
			? (ammo <= 0 ? padEmptyReload : padReload)
			: (ammo <= 0 ? runtime.EmptyReload : runtime.Reload);

		if (!timer.IsTicking())
		{
			if (delay >= 0)
				timer.Start(delay);

			continue;
		}

		// The timer has expired; restore one increment and start the next tick.
		ammo = Math::min(runtime.Max, ammo + increment);

		if (ammo >= runtime.Max)
			timer.Stop();
		else if (delay >= 0)
			timer.Start(delay);
		else
			timer.Stop();
	}

	UpdateExtraFireAmmoReload(pThis);
	UpdateAircraftAmmoCompat(pThis);
}

int TechnoExt::GetWeaponAmmoRemaining(TechnoClass* pThis, int slot)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return 0;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return 0;

	RefreshWeaponAmmoSlot(pThis, slot);
	auto const& runtime = pExt->WeaponAmmo[slot];

	if (runtime.HasPool)
		return pExt->WeaponAmmoCount[slot];

	if (runtime.Infinite)
		return -1;

	return pThis->Ammo;
}

int TechnoExt::GetWeaponAmmoMax(TechnoClass* pThis, int slot)
{
	if (!pThis || slot < 0 || slot >= TechnoTypeClass::MaxWeapons)
		return 0;

	auto const pExt = ExtMap.Find(pThis);

	if (!pExt)
		return 0;

	RefreshWeaponAmmoSlot(pThis, slot);
	auto const& runtime = pExt->WeaponAmmo[slot];

	if (runtime.HasPool)
		return runtime.Max;

	if (runtime.Infinite)
		return -1;

	return pThis->GetTechnoType()->Ammo;
}

int TechnoExt::GetCurrentDisplayWeaponSlot(TechnoClass* pThis)
{
	if (!pThis)
		return 0;

	auto const pExt = ExtMap.Find(pThis);

	if (pExt && pExt->LastFiredWeaponSlot >= 0 && pExt->LastFiredWeaponSlot < TechnoTypeClass::MaxWeapons)
		return pExt->LastFiredWeaponSlot;

	const int selected = pThis->SelectWeapon(pThis->Target);

	if (selected >= 0 && selected < TechnoTypeClass::MaxWeapons)
		return selected;

	return 0;
}

bool TechnoExt::HasAnyWeaponAmmo(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	const auto pType = pThis->GetTechnoType();

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
	{
		auto const pWeapon = TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite());

		if (!pWeapon)
			continue;

		auto const pExt = ExtMap.Find(pThis);
		RefreshWeaponAmmoSlot(pThis, slot);
		auto const& runtime = pExt->WeaponAmmo[slot];

		if (runtime.HasPool)
		{
			if (pExt->WeaponAmmoCount[slot] > 0)
				return true;
		}
		else if (runtime.Infinite)
		{
			return true;
		}
		else if (pType->Ammo <= 0 || pThis->Ammo > 0)
		{
			return true;
		}
	}

	return false;
}

bool TechnoExt::HasLegacyWeaponAmmo(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	const auto pType = pThis->GetTechnoType();

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
	{
		auto const pWeapon = TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite());

		if (!pWeapon)
			continue;

		auto const pExt = ExtMap.Find(pThis);
		RefreshWeaponAmmoSlot(pThis, slot);
		auto const& runtime = pExt->WeaponAmmo[slot];

		if (!runtime.HasPool && !runtime.Infinite)
			return true;
	}

	return false;
}

bool TechnoExt::HasOwnWeaponAmmoPool(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	const auto pType = pThis->GetTechnoType();

	for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
	{
		auto const pWeapon = TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite());

		if (!pWeapon)
			continue;

		auto const pExt = ExtMap.Find(pThis);
		RefreshWeaponAmmoSlot(pThis, slot);

		if (pExt->WeaponAmmo[slot].HasPool)
			return true;
	}

	return false;
}

// Aircraft RTB / rearm checks in the executable read TechnoClass::Ammo directly.
// For aircraft whose weapons all use their own pools, keep that field as a small
// compatibility mirror (max > 0 while any weapon can fire, 0 when everything is dry).
// The pool remains the source of truth; legacy weapons are never touched.
void TechnoExt::UpdateAircraftAmmoCompat(TechnoClass* pThis)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Aircraft)
		return;

	if (!HasOwnWeaponAmmoPool(pThis) || HasLegacyWeaponAmmo(pThis))
		return;

	const auto pType = pThis->GetTechnoType();
	pThis->Ammo = HasAnyWeaponAmmo(pThis) ? Math::max(1, pType->Ammo) : 0;
}

double TechnoExt::ApplyAccelerationCurve(double progress, int curve)
{
	switch (curve)
	{
	case 1: // ease-in
		return progress * progress;

	case 2: // ease-out
		return 1.0 - (1.0 - progress) * (1.0 - progress);

	case 3: // smoothstep
		return progress * progress * (3.0 - 2.0 * progress);

	default:
		return progress;
	}
}

double TechnoExt::GetTerrainSpeedFactor(TechnoTypeExt::ExtData* pTypeExt, LandType landType)
{
	switch (landType)
	{
	case LandType::Road:
		return pTypeExt->TerrainSpeed_Road.Get();

	case LandType::Water:
		return pTypeExt->TerrainSpeed_Water.Get();

	case LandType::Rock:
		return pTypeExt->TerrainSpeed_Rock.Get();

	case LandType::Wall:
		return pTypeExt->TerrainSpeed_Wall.Get();

	case LandType::Tiberium:
		return pTypeExt->TerrainSpeed_Tiberium.Get();

	case LandType::Beach:
		return pTypeExt->TerrainSpeed_Beach.Get();

	case LandType::Rough:
		return pTypeExt->TerrainSpeed_Rough.Get();

	case LandType::Ice:
		return pTypeExt->TerrainSpeed_Ice.Get();

	case LandType::Railroad:
		return pTypeExt->TerrainSpeed_Railroad.Get();

	case LandType::Tunnel:
		return pTypeExt->TerrainSpeed_Tunnel.Get();

	case LandType::Weeds:
		return pTypeExt->TerrainSpeed_Weeds.Get();

	case LandType::Clear:
	default:
		return pTypeExt->TerrainSpeed_Clear.Get();
	}
}

void TechnoExt::ResetAccelerationState(FootClass* pThis)
{
	if (!pThis || !pThis->IsAlive || pThis->InLimbo)
		return;

	auto const pExt = TechnoExt::ExtMap.TryFind(pThis);

	if (!pExt)
		return;

	pExt->AccelActive = false;
	pExt->AccelWasMoving = false;
	pExt->AccelStartSpeed = 0.0;
	pExt->AccelTargetSpeed = 0.0;
	pExt->AccelStartFrame = 0;
}

double TechnoExt::UpdateEnhancedAcceleration(FootClass* pThis, double targetSpeed, int distanceToTarget)
{
	if (!pThis || !pThis->IsAlive || pThis->InLimbo)
		return targetSpeed;

	auto const pType = pThis->GetTechnoType();

	if (!pType)
		return targetSpeed;

	auto const pTypeExt = TechnoTypeExt::ExtMap.TryFind(pType);
	auto const pExt = TechnoExt::ExtMap.TryFind(pThis);

	if (!pTypeExt || !pExt
		|| reinterpret_cast<uintptr_t>(pTypeExt) < 0x10000
		|| reinterpret_cast<uintptr_t>(pExt) < 0x10000
		|| pTypeExt->Acceleration_Mode.Get() != 1)
	{
		return targetSpeed;
	}

	targetSpeed = Math::clamp(targetSpeed, 0.0, 1.0);
	const double beforeSpeed = pThis->SpeedPercentage;

	auto const pCell = pThis->GetCell();
	double factor = 1.0;

	if (pCell)
	{
		const LandType landType = pThis->OnBridge ? LandType::Road : pCell->LandType;
		factor *= GetTerrainSpeedFactor(pTypeExt, landType);

		if (pCell->SlopeIndex != 0)
			factor *= pTypeExt->SlopeSpeedFactor.Get();
	}

	if (pThis->PrimaryFacing.IsRotating())
		factor *= pTypeExt->TurnSpeedFactor.Get();

	if (pType->Passengers > 0)
	{
		const double load = Math::clamp(static_cast<double>(pThis->Passengers.NumPassengers) / pType->Passengers, 0.0, 1.0);
		factor *= 1.0 - (1.0 - pTypeExt->LoadSpeedFactor.Get()) * load;
	}

	if (pType->Strength > 0)
	{
		const double health = Math::clamp(static_cast<double>(pThis->Health) / pType->Strength, 0.0, 1.0);
		const double threshold = Math::clamp(pTypeExt->HealthSpeedThreshold.Get(), 0.0, 1.0);

		if (health < threshold)
			factor *= pTypeExt->HealthSpeedFactor.Get() + (1.0 - pTypeExt->HealthSpeedFactor.Get()) * (health / Math::max(0.001, threshold));
	}

	if (pThis->WhatAmI() == AbstractType::Unit && static_cast<UnitClass*>(pThis)->IsCrushingSomething
		&& !pTypeExt->SkipCrushSlowdown)
	{
		factor = Math::min(factor, pTypeExt->CrushSlowdownMultiplier.Get());
	}

	if (pThis->IsSinking)
		factor = Math::min(factor, 0.1);

	targetSpeed = Math::clamp(targetSpeed * factor, 0.0, 1.0);

	int accelerationTime = pTypeExt->Acceleration_Time.isset()
		? pTypeExt->Acceleration_Time.Get()
		: 60;
	int decelerationTime = pTypeExt->Deceleration_Time.isset()
		? pTypeExt->Deceleration_Time.Get()
		: 30;

	accelerationTime = Math::max(1, accelerationTime);
	decelerationTime = Math::max(1, decelerationTime);

	const int stopDistance = Math::max(0, pTypeExt->StopDistance.Get());

	if (distanceToTarget >= 0)
	{
		const double defaultSpeed = Math::max(0, pThis->GetDefaultSpeed());
		const double deceleration = defaultSpeed / static_cast<double>(decelerationTime);
		const double currentSpeed = Math::max(0, pThis->GetCurrentSpeed());
		const double autoBrake = deceleration > 0.0
			? (currentSpeed * currentSpeed) / (2.0 * deceleration)
			: 0.0;
		const double brakeDistance = pTypeExt->BrakeDistance.Get() >= 0
			? static_cast<double>(pTypeExt->BrakeDistance.Get())
			: autoBrake * Math::max(0.0, pTypeExt->BrakeDistance_Multiplier.Get());

		if (distanceToTarget <= brakeDistance || distanceToTarget <= stopDistance)
			targetSpeed = Math::min(targetSpeed, Math::max(0.0001, pTypeExt->MinimumSpeed.Get()));
	}

	targetSpeed = Math::clamp(targetSpeed, 0.0, 1.0);

	const bool moving = targetSpeed > 0.001;
	const bool starting = moving && (!pExt->AccelWasMoving || (!pExt->AccelActive && pThis->SpeedPercentage <= 0.001));
	const double startDelta = targetSpeed - pExt->AccelTargetSpeed;

	if (starting)
	{
		// A stationary unit can still carry SpeedPercentage == 1.0 from an earlier
		// move. Start a new ramp at the configured InitialSpeed instead.
		pExt->AccelStartSpeed = Math::clamp(pTypeExt->InitialSpeed.Get(), 0.0, targetSpeed);
		pExt->AccelTargetSpeed = targetSpeed;
		pExt->AccelStartFrame = static_cast<int>(Unsorted::CurrentFrame);
		pExt->AccelActive = true;
	}
	else if (!pExt->AccelActive || std::abs(startDelta) > 0.01)
	{
		pExt->AccelStartSpeed = pThis->SpeedPercentage;
		pExt->AccelTargetSpeed = targetSpeed;
		pExt->AccelStartFrame = static_cast<int>(Unsorted::CurrentFrame);
		pExt->AccelActive = true;
	}
	else
	{
		pExt->AccelTargetSpeed = targetSpeed;
	}

	pExt->AccelWasMoving = moving;

	const double delta = pExt->AccelTargetSpeed - pExt->AccelStartSpeed;

	if (std::abs(delta) < 0.0001)
	{
		pExt->AccelActive = false;
		return Math::clamp(pExt->AccelTargetSpeed, 0.0, 1.0);
	}

	const int baseTime = delta > 0.0 ? accelerationTime : decelerationTime;
	const double duration = Math::max(1.0, static_cast<double>(baseTime) * std::abs(delta));
	const double progress = Math::clamp(
		static_cast<double>(static_cast<int>(Unsorted::CurrentFrame) - pExt->AccelStartFrame) / duration, 0.0, 1.0);
	const int curve = delta > 0.0 ? pTypeExt->Acceleration_Curve.Get() : pTypeExt->Deceleration_Curve.Get();
	const double speed = pExt->AccelStartSpeed + delta * ApplyAccelerationCurve(progress, curve);

	if (progress >= 1.0)
		pExt->AccelActive = false;

	const int elapsed = static_cast<int>(Unsorted::CurrentFrame) - pExt->AccelStartFrame;

	// Diagnostic for tuning: the first 120 frames of every ramp are logged even
	// without Debug.GCDiagnostics, then the periodic log is opt-in.
	if (starting || elapsed < 120)
	{
		if (starting || (elapsed % 10) == 0)
		{
			Debug::Log("[Accel] [%s] before=%.3f after=%.3f target=%.3f start=%.3f p=%.2f dist=%d time=%d wasMoving=%d\n",
				pThis->GetTechnoType()->ID, beforeSpeed, speed,
				pExt->AccelTargetSpeed, pExt->AccelStartSpeed, progress,
				distanceToTarget, baseTime, pExt->AccelWasMoving ? 1 : 0);
		}
	}
	else if (Phobos::Config::DebugGCDiagnostics && (Unsorted::CurrentFrame % 10) == 0)
	{
		Debug::Log("[Accel] [%s] speed=%.3f target=%.3f start=%.3f p=%.2f dist=%d time=%d\n",
			pThis->GetTechnoType()->ID, speed, pExt->AccelTargetSpeed,
			pExt->AccelStartSpeed, progress, distanceToTarget, baseTime);
	}

	return Math::clamp(speed, 0.0, 1.0);
}

bool TechnoExt::IsAtAirfield(TechnoClass* pThis)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Aircraft)
		return false;

	auto const pAircraft = static_cast<AircraftClass*>(pThis);

	if (pAircraft->IsInAir())
		return false;

	auto const pCell = pAircraft->GetCell();

	if (!pCell)
		return false;

	auto const pBuilding = pCell->GetBuilding();

	if (!pBuilding)
		return false;

	return pBuilding->Type->UnitReload || pBuilding->Type->Helipad;
}

// ExtraFire pools are implemented in Body.Update.cpp next to the firing loop.

// =============================
// load / save

template <typename T>
void TechnoExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->TypeExtData)
		.Process(this->Shield)
		.Process(this->LaserTrails)
		.Process(this->AttachedEffects)
		.Process(this->Abilities)
		.Process(this->AE)
		.Process(this->PreviousType)
		.Process(this->AnimRefCount)
		.Process(this->ReceiveDamage)
		.Process(this->LastKillWasTeamTarget)
		.Process(this->PassengerDeletionTimer)
		.Process(this->CurrentShieldType)
		.Process(this->LastWarpDistance)
		.Process(this->ChargeTurretTimer)
		.Process(this->AutoDeathTimer)
		.Process(this->MindControlRingAnimType)
		.Process(this->DamageNumberOffset)
		.Process(this->Strafe_BombsDroppedThisRound)
		.Process(this->Strafe_TargetCell)
		.Process(this->CurrentAircraftWeaponIndex)
		.Process(this->IsInTunnel)
		.Process(this->IsBurrowed)
		.Process(this->HasBeenPlacedOnMap)
		.Process(this->DeployFireTimer)
		.Process(this->SkipTargetChangeResetSequence)
		.Process(this->ForceFullRearmDelay)
		.Process(this->LastRearmWasFullDelay)
		.Process(this->CanCloakDuringRearm)
		.Process(this->WHAnimRemainingCreationInterval)
		.Process(this->LastWeaponType)
		.Process(this->FiringObstacleCell)
		.Process(this->IsDetachingForCloak)
		.Process(this->BeControlledThreatFrame)
		.Process(this->LastTargetID)
		.Process(this->AIDirectionalArmorDelayTimer)
		.Process(this->AIDirectionalArmorDelayTargetID)
		.Process(this->AccumulatedGattlingValue)
		.Process(this->ShouldUpdateGattlingValue)
		.Process(this->OriginalPassengerOwner)
		.Process(this->HasRemainingWarpInDelay)
		.Process(this->LastWarpInDelay)
		.Process(this->IsBeingChronoSphered)
		.Process(this->KeepTargetOnMove)
		.Process(this->LastSensorsMapCoords)
		.Process(this->TiberiumEater_Timer)
		.Process(this->AirstrikeTargetingMe)
		.Process(this->FiringAnimationTimer)
		.Process(this->SimpleDeployerAnimationTimer)
		.Process(this->DelayedFireSequencePaused)
		.Process(this->DelayedFireTimer)
		.Process(this->DelayedFireWeaponIndex)
		.Process(this->CurrentDelayedFireAnim)
		.Process(this->AttachedEffectInvokerCount)
		.Process(this->TintColorOwner)
		.Process(this->TintColorAllies)
		.Process(this->TintColorEnemies)
		.Process(this->TintIntensityOwner)
		.Process(this->TintIntensityAllies)
		.Process(this->TintIntensityEnemies)
		.Process(this->AttackMoveFollowerTempCount)
		.Process(this->ParentAttachment)
		.Process(this->ChildAttachments)
		.Process(this->DormantAttachments)
		.Process(this->AltOccupation)
		.Process(this->LastHurtFrame)
		;
	// Note: CurrentTargetFilter is NOT serialized - it is a transient UI state.
	// On load it defaults to 0 (no filter) from the constructor initializer list.
	// Serializing it would break save compatibility with saves made before the
	// filter feature was added, as the extra 4 bytes would misalign the stream.

	for (int i = 0; i < TechnoTypeClass::MaxWeapons; ++i)
		Stm.Process(this->NextWeaponCooldowns[i]);

	Stm.Process(this->NextChainBurstRearm);
	Stm.Process(this->ExtraFireCooldowns);
	Stm.Process(this->ExtraFireBurstIndex);
	Stm.Process(this->ExtraFireMountFacing);

	for (int i = 0; i < TechnoTypeClass::MaxWeapons; ++i)
	{
		Stm.Process(this->WeaponAmmoCount[i]);
		Stm.Process(this->WeaponAmmoReloadTimer[i]);
	}

	Stm.Process(this->ExtraFireAmmoCount);
	Stm.Process(this->ExtraFireAmmoReloadTimer);
	Stm.Process(this->GiftBox_Initialized);
	Stm.Process(this->GiftBox_Opened);
	Stm.Process(this->GiftBox_Timer);
	Stm.Process(this->AccelStartSpeed);
	Stm.Process(this->AccelTargetSpeed);
	Stm.Process(this->AccelStartFrame);
	Stm.Process(this->AccelActive);
	Stm.Process(this->AccelWasMoving);
}

void TechnoExt::ExtData::InvalidatePointer(void* ptr, bool bRemoved)
{
	AbilityClass::OnOwnerRemoved(this->OwnerObject());
	AnnounceInvalidPointer(this->AirstrikeTargetingMe, ptr);
	AnnounceInvalidPointer(this->PendingAttackTarget, ptr);

	for (auto& pTarget : this->ExtraFireOwnTargets)
		AnnounceInvalidPointer(pTarget, ptr);
	
	for (auto const& pAttachment : ChildAttachments)
		pAttachment->InvalidatePointer(ptr);

	for (auto& [key, vec] : DormantAttachments)
		for (auto const& pAttachment : vec)
			pAttachment->InvalidatePointer(ptr);
}

void TechnoExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoClass>::LoadFromStream(Stm);
	this->Serialize(Stm);

	// Counts and timers were just restored from the save. The next RefreshAll must
	// keep them instead of resetting every pool to its Ammo.Initial value.
	this->WeaponAmmoLoadedFromSave = true;
	this->ExtraFireAmmoLoadedFromSave = true;

	// A moving unit restored mid-ramp must not be treated as starting from rest.
	if (auto const pFoot = abstract_cast<FootClass*>(this->OwnerObject()))
	{
		if (!this->AccelWasMoving && pFoot->SpeedPercentage > 0.01)
			this->AccelWasMoving = true;
	}
}

void TechnoExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool TechnoExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm
		.Success();
}

bool TechnoExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm
		.Success();
}

// =============================
// container

TechnoExt::ExtContainer::ExtContainer() : Container("TechnoClass") { }

TechnoExt::ExtContainer::~ExtContainer() = default;


// =============================
// container hooks

DEFINE_HOOK(0x6F3260, TechnoClass_CTOR, 0x5)
{
	GET(TechnoClass*, pItem, ESI);

	TechnoExt::ExtMap.TryAllocate(pItem);

	return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR, 0x5)
{
	GET(TechnoClass*, pItem, ECX);

	TechnoExt::ExtMap.Remove(pItem);

	return 0;
}

DEFINE_HOOK_AGAIN(0x70C250, TechnoClass_SaveLoad_Prefix, 0x8)
DEFINE_HOOK(0x70BF50, TechnoClass_SaveLoad_Prefix, 0x5)
{
	GET_STACK(TechnoClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);

	TechnoExt::ExtMap.PrepareStream(pItem, pStm);

	return 0;
}

DEFINE_HOOK(0x70C249, TechnoClass_Load_Suffix, 0x5)
{
	TechnoExt::ExtMap.LoadStatic();

	return 0;
}

DEFINE_HOOK(0x70C264, TechnoClass_Save_Suffix, 0x5)
{
	TechnoExt::ExtMap.SaveStatic();

	return 0;
}

DEFINE_HOOK(0x710415, TechnoClass_DetachAnim, 0x6)
{
	GET(TechnoClass*, pThis, ECX);
	GET(AbstractClass*, pTarget, EAX);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (pExt->CurrentDelayedFireAnim == pTarget)
		pExt->CurrentDelayedFireAnim = nullptr;

	return 0;
}
