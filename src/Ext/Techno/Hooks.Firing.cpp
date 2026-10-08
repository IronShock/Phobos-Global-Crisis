#include "Body.h"

#include <OverlayTypeClass.h>
#include <ScenarioClass.h>
#include <TerrainClass.h>

#include <Ext/Anim/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/Bullet/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Utilities/EnumFunctions.h>

#pragma region TechnoClass_SelectWeapon

DEFINE_HOOK(0x6F3339, TechnoClass_WhatWeaponShouldIUse_Interceptor, 0x8)
{
	enum { SkipGameCode = 0x6F3341, ReturnValue = 0x6F3406 };

	GET(TechnoClass*, pThis, ESI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));

	auto const pType = pThis->GetTechnoType();

	if (pTarget && pTarget->WhatAmI() == AbstractType::Bullet)
	{
		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

		if (pTypeExt->InterceptorType)
		{
			R->EAX(pTypeExt->InterceptorType->Weapon);
			return ReturnValue;
		}
	}

	// Restore overridden instructions.
	R->EAX(pType);

	return SkipGameCode;
}

DEFINE_HOOK(0x6F3360, TechnoClass_WhatWeaponShouldIUse_MultiWeapon, 0x6)
{
	GET(TechnoTypeClass*, pType, EAX);
	enum { SkipGameCode = 0x6F3379 };

	if (TechnoTypeExt::ExtMap.Find(pType)->MultiWeapon.Get()
		&& (pType->WhatAmI() != AbstractType::UnitType || !pType->Gunner))
	{
		return SkipGameCode;
	}

	return 0;
}

DEFINE_HOOK(0x6F33CD, TechnoClass_WhatWeaponShouldIUse_ForceFire, 0x6)
{
	enum { ReturnWeaponIndex = 0x6F37AF };

	GET(TechnoClass*, pThis, ESI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));

	if (const auto pCell = abstract_cast<CellClass*>(pTarget))
	{
		auto const pWeaponPrimary = pThis->GetWeapon(0)->WeaponType;
		auto const pWeaponSecondary = pThis->GetWeapon(1)->WeaponType;
		auto const pPrimaryExt = WeaponTypeExt::ExtMap.Find(pWeaponPrimary);

		if (pWeaponSecondary
			&& !pPrimaryExt->SkipWeaponPicking
			&& (!EnumFunctions::IsCellEligible(pCell, pPrimaryExt->CanTarget, true, true)
				|| (pPrimaryExt->AttachEffect_CheckOnFirer
					&& !pPrimaryExt->HasRequiredAttachedEffects(pThis, pThis))))
		{
			R->EAX(1);
			return ReturnWeaponIndex;
		}
		else if (pCell->OverlayTypeIndex != -1)
		{
			auto const pOverlayType = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex);

			if (pOverlayType->Wall && pCell->OverlayData >> 4 != pOverlayType->DamageLevels)
			{
				R->EAX(TechnoExt::GetWeaponIndexAgainstWall(pThis, pOverlayType));
				return ReturnWeaponIndex;
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x6F3428, TechnoClass_WhatWeaponShouldIUse_ForceWeapon, 0x6)
{
	enum { UseWeaponIndex = 0x6F37AF };

	GET(TechnoClass*, pThis, ECX);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));

	auto const pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;

	// Force weapon
	const int forceWeaponIndex = pTypeExt->SelectForceWeapon(pThis, pTarget);

	if (forceWeaponIndex >= 0)
	{
		R->EAX(forceWeaponIndex);
		return UseWeaponIndex;
	}

	// Multi weapon
	const int multiWeaponIndex = pTypeExt->SelectMultiWeapon(pThis, pTarget);

	if (multiWeaponIndex >= 0)
	{
		R->EAX(multiWeaponIndex);
		return UseWeaponIndex;
	}

	return 0;
}

DEFINE_HOOK(0x6F36DB, TechnoClass_WhatWeaponShouldIUse, 0x8)
{
	GET(TechnoClass*, pThis, ESI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));

	enum { Primary = 0x6F37AD, Secondary = 0x6F3745, OriginalCheck = 0x6F36E3 };

	const auto pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
	const auto pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;
	const bool allowFallback = !pTypeExt->NoSecondaryWeaponFallback;
	const bool allowAAFallback = allowFallback ? true : pTypeExt->NoSecondaryWeaponFallback_AllowAA;
	const int weaponIndex = TechnoExt::PickWeaponIndex(pThis, pTargetTechno, pTarget, 0, 1, allowFallback, allowAAFallback);

	if (weaponIndex != -1)
		return weaponIndex == 1 ? Secondary : Primary;

	if (!pTargetTechno)
		return Primary;

	const auto pTargetExt = TechnoExt::ExtMap.Find(pTargetTechno);

	if (const auto pShield = pTargetExt->Shield.get())
	{
		if (pShield->IsActive())
		{
			const auto secondary = pThis->GetWeapon(1)->WeaponType;
			const bool secondaryIsAA = pTargetTechno && pTargetTechno->IsInAir() && secondary && secondary->Projectile->AA;

			if (secondary && (allowFallback || (allowAAFallback && secondaryIsAA) || TechnoExt::CanFireNoAmmoWeapon(pThis, 1)))
			{
				if (!pShield->CanBeTargeted(pThis->GetWeapon(0)->WeaponType))
					return Secondary;
			}
			else
			{
				return Primary;
			}
		}
	}

	return OriginalCheck;
}

DEFINE_HOOK(0x6F37EB, TechnoClass_WhatWeaponShouldIUse_AntiAir, 0x6)
{
	enum { Primary = 0x6F37AD, Secondary = 0x6F3807 };

	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));
	GET_STACK(WeaponTypeClass*, pWeapon, STACK_OFFSET(0x18, -0x4));
	GET(WeaponTypeClass*, pSecWeapon, EAX);

	if (!pWeapon->Projectile->AA && pSecWeapon->Projectile->AA)
	{
		const auto pTargetTechno = abstract_cast<TechnoClass*>(pTarget);

		if (pTargetTechno && pTargetTechno->IsInAir())
			return Secondary;
	}

	return Primary;
}

DEFINE_HOOK(0x6F3432, TechnoClass_WhatWeaponShouldIUse_Gattling, 0xA)
{
	enum { UseWeaponIndex = 0x6F37AF };

	GET(TechnoClass*, pThis, ESI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x18, 0x4));

	auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
	const int oddWeaponIndex = 2 * pThis->CurrentGattlingStage;
	const int evenWeaponIndex = oddWeaponIndex + 1;
	int chosenWeaponIndex = oddWeaponIndex;
	const int eligibleWeaponIndex = TechnoExt::PickWeaponIndex(pThis, pTargetTechno, pTarget, oddWeaponIndex, evenWeaponIndex, true);

	if (eligibleWeaponIndex != -1)
	{
		chosenWeaponIndex = eligibleWeaponIndex;
	}
	else if (pTargetTechno)
	{
		auto const pTargetExt = TechnoExt::ExtMap.Find(pTargetTechno);
		auto const pWeaponOdd = pThis->GetWeapon(oddWeaponIndex)->WeaponType;
		auto const pWeaponEven = pThis->GetWeapon(evenWeaponIndex)->WeaponType;
		bool skipRemainingChecks = false;

		if (const auto pShield = pTargetExt->Shield.get())
		{
			if (pShield->IsActive() && !pShield->CanBeTargeted(pWeaponOdd))
			{
				chosenWeaponIndex = evenWeaponIndex;
				skipRemainingChecks = true;
			}
		}

		if (!skipRemainingChecks)
		{
			if (GeneralUtils::GetWarheadVersusArmor(pWeaponOdd->Warhead, pTargetTechno->GetTechnoType()->Armor) == 0.0)
			{
				chosenWeaponIndex = evenWeaponIndex;
			}
			else
			{
				auto const landType = pTargetTechno->GetCell()->LandType;
				const bool isOnWater = (landType == LandType::Water || landType == LandType::Beach) && !pTargetTechno->IsInAir();

				if (!pTargetTechno->OnBridge && isOnWater && pThis->SelectNavalTargeting(pTargetTechno) == 2)
				{
					chosenWeaponIndex = evenWeaponIndex;
				}
				else if (pTargetTechno->IsInAir() && !pWeaponOdd->Projectile->AA && pWeaponEven->Projectile->AA)
				{
					chosenWeaponIndex = evenWeaponIndex;
				}
				else if (pThis->GetTechnoType()->LandTargeting == LandTargetingType::Land_Secondary)
				{
					chosenWeaponIndex = evenWeaponIndex;
				}
			}
		}
	}

	R->EAX(chosenWeaponIndex);
	return UseWeaponIndex;
}

DEFINE_HOOK(0x5218F3, InfantryClass_WhatWeaponShouldIUse_DeployFireWeapon, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	GET(TechnoTypeClass*, pType, ECX);

	if (pType->DeployFireWeapon == -1)
		return 0x52194E;

	if (pType->IsGattling || TechnoTypeExt::ExtMap.Find(pType)->MultiWeapon.Get())
		return !pThis->IsDeployed() ? 0x52194E : 0x52190D;

	return 0;
}

#pragma endregion

#pragma region TechnoClass_GetFireError
DEFINE_HOOK(0x6FC339, TechnoClass_CanFire, 0x6)
{
	enum { CannotFire = 0x6FCB7E };

	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EDI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x20, 0x4));
	GET(TechnoClass*, pTargetTechno, EBP);

	// Checking for nullptr is not required here, since the game has already executed them before calling the hook  -- Belonit
	const auto pWH = pWeapon->Warhead;
	const auto pWHExt = WarheadTypeExt::ExtMap.Find(pWH);
	const int nMoney = pWHExt->TransactMoney;

	if (nMoney < 0 && pThis->Owner->Available_Money() < -nMoney)
		return CannotFire;

	// AAOnly doesn't need to be checked if LandTargeting=1.
	if (pThis->GetTechnoType()->LandTargeting != LandTargetingType::Land_Not_OK && pWeapon->Projectile->AA
		&& pTarget && !pTarget->IsInAir() && BulletTypeExt::ExtMap.Find(pWeapon->Projectile)->AAOnly)
	{
		return CannotFire;
	}

	CellClass* pTargetCell = nullptr;

	if (pTarget)
	{
		if (const auto pObject = abstract_cast<ObjectClass*, true>(pTarget))
		{
			// Ignore target cell for technos that are in air.
			if ((pTargetTechno && !pTargetTechno->IsInAir()) || pObject != pTargetTechno)
				pTargetCell = pObject->GetCell();
		}
		else if (const auto pCell = abstract_cast<CellClass*, true>(pTarget))
		{
			pTargetCell = pCell;
		}
	}

	const auto pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);

	if (!pWeaponExt->SkipWeaponPicking && pTargetCell && !EnumFunctions::IsCellEligible(pTargetCell, pWeaponExt->CanTarget, true, true))
		return CannotFire;

	if (pTargetTechno)
	{
		if (pThis->Berzerk
			&& !EnumFunctions::CanTargetHouse(RulesExt::Global()->BerzerkTargeting, pThis->Owner, pTargetTechno->Owner))
		{
			return CannotFire;
		}

		if (!pWeaponExt->SkipWeaponPicking)
		{
			if (!EnumFunctions::IsTechnoEligible(pTargetTechno, pWeaponExt->CanTarget)
				|| !EnumFunctions::CanTargetHouse(pWeaponExt->CanTargetHouses, pThis->Owner, pTargetTechno->Owner)
				|| !pWeaponExt->IsHealthInThreshold(pTargetTechno)
				|| !pWeaponExt->HasRequiredAttachedEffects(pTargetTechno, pThis))
			{
				return CannotFire;
			}
		}

		if (pWH->Airstrike)
		{
			if (!EnumFunctions::IsTechnoEligible(pTargetTechno, pWHExt->AirstrikeTargets))
				return CannotFire;

			if (!TechnoExt::ExtMap.Find(pTargetTechno)->TypeExtData->AllowAirstrike.Get(pTargetTechno->AbstractFlags & AbstractFlags::Foot ? true : static_cast<BuildingClass*>(pTargetTechno)->Type->CanC4))
				return CannotFire;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6FC0C5, TechnoClass_CanFire_DisableWeapons, 0x6)
{
	enum { OutOfRange = 0x6FC0DF, Illegal = 0x6FC86A, Continue = 0x6FC0D3 };

	GET(TechnoClass*, pThis, ESI);

	if (pThis->SlaveOwner)
		return Illegal;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (pExt->AE.DisableWeapons)
		return OutOfRange;

	return Continue;
}

DEFINE_HOOK(0x6FC5C7, TechnoClass_CanFire_OpenTopped, 0x6)
{
	enum { Illegal = 0x6FC86A, OutOfRange = 0x6FC0DF, Continue = 0x6FC5D5 };

	GET(TechnoClass*, pTransport, EAX);

	auto const pTypeExt = TechnoExt::ExtMap.Find(pTransport)->TypeExtData;

	if (pTransport->Deactivated && !pTypeExt->OpenTopped_AllowFiringIfDeactivated)
		return Illegal;

	if (pTransport->Transporter)
		return Illegal;

	if (pTypeExt->OpenTopped_CheckTransportDisableWeapons && TechnoExt::ExtMap.Find(pTransport)->AE.DisableWeapons)
		return OutOfRange;

	return Continue;
}

DEFINE_HOOK(0x6FC689, TechnoClass_CanFire_LandNavalTarget, 0x6)
{
	enum { DisallowFiring = 0x6FC86A };

	GET(TechnoClass*, pThis, ESI);
	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0x20, 0x4));

	const auto pType = pThis->GetTechnoType();

	if (const auto pCell = abstract_cast<CellClass*>(pTarget))
	{
		const auto landType = pCell->LandType;

		if (pType->NavalTargeting == NavalTargetingType::Naval_None
			&& (landType == LandType::Water || landType == LandType::Beach))
		{
			return DisallowFiring;
		}
	}
	else if (const auto pTerrain = abstract_cast<TerrainClass*>(pTarget))
	{
		const auto landType = pTerrain->GetCell()->LandType;

		if (pType->LandTargeting == LandTargetingType::Land_Not_OK
			&& landType != LandType::Water && landType != LandType::Beach)
		{
			return DisallowFiring;
		}
		else if (pType->NavalTargeting == NavalTargetingType::Naval_None
			&& (landType == LandType::Water || landType == LandType::Beach))
		{
			return DisallowFiring;
		}
	}

	return 0;
}

// Skips bridge-related coord checks to allow AA to target air units while both are on a bridge.
DEFINE_HOOK(0x6FCBE6, TechnoClass_CanFire_BridgeAAFix, 0x6)
{
	enum { SkipChecks = 0x6FCCBD };

	GET(TechnoClass*, pTarget, EBP);

	if (pTarget->IsInAir())
		return SkipChecks;

	return 0;
}

DEFINE_HOOK(0x6FC94F, TechnoClass_CanFire_Rearm_OccupyFire, 0x6)
{
	enum { ApplyTimer = 0x6FC95B };

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
		return 0;

	GET(TechnoClass*, pThis, ESI);

	if (!pThis->CanOccupyFire())
		return 0;

	const auto pBuilding = static_cast<BuildingClass*>(pThis);
	const auto pOccupier = BuildingExt::GetFiringOccupant(pBuilding);
	const auto& timer = pOccupier ? pOccupier->RearmTimer : pBuilding->RearmTimer;
	R->EDX(timer.StartTime);
	R->EAX(timer.TimeLeft);
	return ApplyTimer;
}

DEFINE_HOOK(0x447F25, BuildingClass_CanFire_OccupyFire, 0x6)
{
	enum { FireErrorIllegal = 0x44805A, ApplyResult = 0x448052 };

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
		return 0;

	if (BuildingExt::ConsideringSpecificOccupant)
		return 0;

	GET(BuildingClass*, pThis, ESI);
	GET(BuildingTypeClass*, pType, EAX);

	if (!pType->CanOccupyFire || !pThis->Occupants.Count)
		return FireErrorIllegal;

	const auto mission = pThis->GetCurrentMission();

	if (mission == Mission::Construction || mission == Mission::Selling)
		return FireErrorIllegal;

	GET_STACK(AbstractClass*, pTarget, STACK_OFFSET(0xC, 0x4));

	if (!BuildingExt::CanOccupantsFire(pThis, pTarget))
		return FireErrorIllegal;

	R->EBP(FireError::OK);
	return ApplyResult;
}

#pragma endregion

#pragma region TechnoClass_Fire
DEFINE_HOOK(0x6FDD7D, TechnoClass_FireAt_UpdateWeaponType, 0x5)
{
	enum { CanNotFire = 0x6FDE03 };

	GET(TechnoClass* const, pThis, ESI);

	if (const auto pExt = TechnoExt::ExtMap.TryFind(pThis))
	{
		GET(WeaponTypeClass* const, pWeapon, EBX);

		if (pThis->CurrentBurstIndex && pWeapon != pExt->LastWeaponType && pExt->TypeExtData->RecountBurst.Get(RulesExt::Global()->RecountBurst))
		{
			if (pExt->LastWeaponType && pExt->LastWeaponType->Burst)
			{
				const auto ratio = static_cast<double>(pThis->CurrentBurstIndex) / pExt->LastWeaponType->Burst;
				const auto rof = static_cast<int>(ratio * pExt->LastWeaponType->ROF * pExt->AE.ROFMultiplier) - (Unsorted::CurrentFrame - pThis->LastFireBulletFrame);

				if (rof > 0)
				{
					pThis->ChargeTurretDelay = rof;
					pThis->RearmTimer.Start(rof);
					pThis->CurrentBurstIndex = 0;
					pExt->LastWeaponType = pWeapon;

					return CanNotFire;
				}
			}

			pThis->CurrentBurstIndex = 0;
		}

		pExt->LastWeaponType = pWeapon;
	}

	return 0;
}

DEFINE_HOOK(0x6FDDC0, TechnoClass_FireAt_BeforeTruelyFire, 0x6)
{
	enum { SkipFiring = 0x6FDE03 };

	GET(TechnoClass* const, pThis, ESI);
//	GET(AbstractClass* const, pTarget, EDI);
	GET(WeaponTypeClass* const, pWeapon, EBX);
	GET_BASE(int, weaponIndex, 0xC);

	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto& timer = pExt->DelayedFireTimer;

	if (pExt->DelayedFireWeaponIndex >= 0 && pExt->DelayedFireWeaponIndex != weaponIndex)
		pExt->ResetDelayedFireTimer();

	if (pWeaponExt->DelayedFire_Duration.isset() && (!pThis->Transporter || !pWeaponExt->DelayedFire_SkipInTransport))
	{
		auto const rtti = pThis->WhatAmI();

		if (pWeaponExt->DelayedFire_PauseFiringSequence && (rtti == AbstractType::Infantry
			|| (rtti == AbstractType::Unit && !pThis->HasTurret() && !pThis->GetTechnoType()->Voxel)))
		{
			return 0;
		}

		if (pWeapon->Burst <= 1 || !pWeaponExt->DelayedFire_OnlyOnInitialBurst || pThis->CurrentBurstIndex == 0)
		{
			if (timer.InProgress())
				return SkipFiring;

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

				return SkipFiring;
			}
			else
			{
				pExt->ResetDelayedFireTimer();
			}
		}
	}

	if (pExt->AE.HasOnFireDiscardables)
	{
		for (const auto& attachEffect : pExt->AttachedEffects)
		{
			if ((attachEffect->GetType()->DiscardOn & DiscardCondition::Firing) != DiscardCondition::None)
				attachEffect->ShouldBeDiscarded = true;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6FE43B, TechnoClass_FireAt_OpenToppedDmgMult, 0x8)
{
	enum { ApplyDamageMult = 0x6FE45A, ContinueCheck = 0x6FE460 };

	GET(TechnoClass* const, pThis, ESI);
	GET_STACK(const int, nDamage, STACK_OFFSET(0xB0, -0x84));

	float nDamageMult = 1.0f;

	//replacing whole check due to `fild`
	if (pThis->InOpenToppedTransport)
	{
		nDamageMult = RulesClass::Instance->OpenToppedDamageMultiplier;

		if (auto const pTransport = pThis->Transporter)
		{
			auto const pExt = TechnoExt::ExtMap.Find(pTransport)->TypeExtData;

			//it is float isnt it YRPP ? , check tomson26 YR-IDB !
			nDamageMult = pExt->OpenTopped_DamageMultiplier.Get(nDamageMult);
		}
	}

	// Fire-on-the-move: a shot fired by an infantry during a normal Move mission
	// (see AttackMove=yes) deals AttackMove.FireMulti (default 0.8) damage.
	if (pThis->WhatAmI() == AbstractType::Infantry
		&& (pThis->GetCurrentMission() == Mission::Move || pThis->GetCurrentMission() == Mission::Patrol))
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());

		if (pTypeExt->AttackMove_FireOnTheMove)
			nDamageMult *= static_cast<float>(pTypeExt->AttackMove_FireMulti);
	}

	if (nDamageMult != 1.0f)
	{
		R->EAX(static_cast<int>(nDamage * nDamageMult));
		return ApplyDamageMult;
	}

	return ContinueCheck;
}

DEFINE_HOOK(0x6FE19A, TechnoClass_FireAt_AreaFire, 0x6)
{
	enum { DoNotFire = 0x6FE4E7, SkipSetTarget = 0x6FE1D5 };

	GET(TechnoClass* const, pThis, ESI);
	GET(CellClass* const, pCell, EAX);
	GET_STACK(WeaponTypeClass*, pWeaponType, STACK_OFFSET(0xB0, -0x70));

	if (const auto pExt = WeaponTypeExt::ExtMap.TryFind(pWeaponType))
	{
		const auto canTarget = pExt->CanTarget;
		const auto canTargetHouses = pExt->CanTargetHouses;
		const bool skipWeaponPicking = pExt->SkipWeaponPicking;
		const bool onBridge = pThis->OnBridge;
		const int level = pThis->GetCell()->Level;

		if (pExt->AreaFire_Target == AreaFireTarget::Random)
		{
			const auto range = WeaponTypeExt::GetRangeWithModifiers(pWeaponType, pThis) / static_cast<double>(Unsorted::LeptonsPerCell);
			const auto pOwner = pThis->Owner;
			const auto mapCoords = pCell->MapCoords;
			// Clamp negative/special ranges before the cast to size_t: otherwise a
			// negative range becomes huge and AdjacentCellsInRange attempts a
			// massive allocation and enumeration.
			const double clampedRange = range > 0.0 ? range : 0.0;
			std::vector<CellStruct> adjacentCells = GeneralUtils::AdjacentCellsInRange(static_cast<size_t>(clampedRange + 0.99));
			const size_t size = adjacentCells.size();

			for (unsigned int i = 0; i < size; i++)
			{
				const int rand = ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(size) - 1);
				const unsigned int cellIndex = (i + rand) % size;
				const CellStruct tgtPos = mapCoords + adjacentCells[cellIndex];
				CellClass* tgtCell = MapClass::Instance.TryGetCellAt(tgtPos);
				const bool allowBridges = tgtCell && tgtCell->ContainsBridge() && (onBridge || tgtCell->Level + CellClass::BridgeLevels == level);

				if (skipWeaponPicking || EnumFunctions::AreCellAndObjectsEligible(tgtCell, canTarget, canTargetHouses, pOwner, true, false, allowBridges))
				{
					R->EAX(tgtCell);
					return 0;
				}
			}

			return DoNotFire;
		}
		else if (pExt->AreaFire_Target == AreaFireTarget::Self)
		{
			if (!skipWeaponPicking && !EnumFunctions::AreCellAndObjectsEligible(pThis->GetCell(), canTarget, canTargetHouses, nullptr, false, false, pThis->OnBridge))
				return DoNotFire;

			R->EAX(pThis);
			return SkipSetTarget;
		}

		const bool allowBridges = pCell->ContainsBridge() && (onBridge || pCell->Level + CellClass::BridgeLevels == level);

		if (!skipWeaponPicking && !EnumFunctions::AreCellAndObjectsEligible(pCell, canTarget, canTargetHouses, nullptr, false, false, allowBridges))
			return DoNotFire;
	}

	return 0;
}

DEFINE_HOOK(0x6FE54B, TechnoClass_FireAt_CreateBullet_OccupyFire, 0x7)
{
	enum { SkipCreateBullet = 0x6FE562 };

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
		return 0;

	GET(TechnoClass*, pThis, ESI);
	GET(int, damage, EDI);
	GET(int, speed, EAX);
	GET(WeaponTypeClass*, pWeapon, EBX);
	GET(WarheadTypeClass*, pWH, EDX);
	GET_BASE(AbstractClass*, pTarget, 0x8);
	const bool bridge = R->CL();
	TechnoClass* pOwner = pThis;

	if (pThis->CanOccupyFire())
	{
		if (auto const pOccupier = BuildingExt::GetFiringOccupant(static_cast<BuildingClass*>(pThis)))
			pOwner = pOccupier;
	}

	R->EAX(pWeapon->Projectile->CreateBullet(pTarget, pOwner, damage, pWH, speed, bridge));
	return SkipCreateBullet;
}

DEFINE_HOOK(0x6FF43F, TechnoClass_FireAt_FeedbackWeapon, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);

	if (auto const pWeaponExt = WeaponTypeExt::ExtMap.TryFind(pWeapon))
	{
		if (auto const pWeaponFeedback = pWeaponExt->FeedbackWeapon)
		{
			if (pThis->InOpenToppedTransport && !pWeaponFeedback->FireInTransport)
				return 0;

			WeaponTypeExt::DetonateAt(pWeaponFeedback, pThis, pThis);
		}
	}

	return 0;
}

DEFINE_HOOK(0x6FF031, TechnoClass_FireAt_OccupyFire, 0xA)
{
	GET(TechnoClass*, pThis, ESI);

	R->AL(!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange && pThis->CanOccupyFire());
	return 0x6FF03B;
}

DEFINE_HOOK(0x6FF0DD, TechnoClass_FireAt_TurretRecoil, 0x6)
{
	enum { SkipGameCode = 0x6FF15B };

	GET_STACK(WeaponTypeClass* const, pWeapon, STACK_OFFSET(0xB0, -0x70));

	return WeaponTypeExt::ExtMap.Find(pWeapon)->TurretRecoil_Suppress ? SkipGameCode : 0;
}

DEFINE_HOOK(0x6FF2AA, TechnoClass_FireAt_RearmTimer_OccupyFire, 0x6)
{
	enum { ApplyTimer = 0x6FF2B0 };

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
		return 0;

	GET(TechnoClass*, pThis, ESI);

	if (!pThis->CanOccupyFire())
		return 0;

	const auto pBuilding = static_cast<BuildingClass*>(pThis);
	const auto pOccupier = BuildingExt::GetFiringOccupant(pBuilding);
	R->EDX(&(pOccupier ? pOccupier->RearmTimer : pBuilding->RearmTimer));
	return ApplyTimer;
}

DEFINE_HOOK(0x6FF905, TechnoClass_FireAt_FireOnce, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);

	if (auto const pInf = abstract_cast<InfantryClass*>(pThis))
	{
		if (!WeaponTypeExt::ExtMap.Find(pWeapon)->FireOnce_ResetSequence)
			TechnoExt::ExtMap.Find(pInf)->SkipTargetChangeResetSequence = true;
	}

	return 0;
}

DEFINE_HOOK(0x6FF923, TechnoClass_FireAt_FireOnce_OccupyFire, 0x6)
{
	enum { SkipResetTarget = 0x6FF92F };

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
		return 0;

	GET(TechnoClass*, pThis, ESI);
	return pThis->CanOccupyFire() ? SkipResetTarget : 0;
}

DEFINE_HOOK(0x6FF660, TechnoClass_FireAt_Interceptor, 0x6)
{
	GET(TechnoClass* const, pSource, ESI);
	GET_BASE(AbstractClass* const, pTarget, 0x8);
	GET_STACK(BulletClass* const, pBullet, STACK_OFFSET(0xB0, -0x74));
	GET(WeaponTypeClass* const, pWeapon, EBX);

	const auto pSourceTypeExt = TechnoExt::ExtMap.Find(pSource)->TypeExtData;

	if (const auto pInterceptorType = pSourceTypeExt->InterceptorType.get())
	{
		if (const auto pTargetBullet = abstract_cast<BulletClass*, true>(pTarget))
		{
			const auto pTargetExt = BulletExt::ExtMap.Find(pTargetBullet);

			if (!pTargetExt->TypeExtData->Armor.isset())
				pTargetExt->InterceptedStatus |= InterceptedStatus::Locked;

			const auto pBulletExt = BulletExt::ExtMap.Find(pBullet);

			pBulletExt->InterceptorTechnoType = pSourceTypeExt;
			pBulletExt->InterceptedStatus |= InterceptedStatus::Targeted;

			if (!pInterceptorType->ApplyFirepowerMult)
				pBullet->Health = pWeapon->Damage;
		}
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6FF660, TechnoClass_FireAt_ToggleLaserWeaponIndex, 0x6)
DEFINE_HOOK(0x6FF4CC, TechnoClass_FireAt_ToggleLaserWeaponIndex, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(WeaponTypeClass* const, pWeapon, EBX);
	GET_BASE(const int, weaponIndex, 0xC);

	if (pWeapon->IsLaser)
	{
		if (auto const pExt = BuildingExt::ExtMap.TryFind(abstract_cast<BuildingClass*, true>(pThis)))
		{
			if (!pExt->CurrentLaserWeaponIndex.has_value())
				pExt->CurrentLaserWeaponIndex = weaponIndex;
			else
				pExt->CurrentLaserWeaponIndex.reset();
		}
	}

	return 0;
}

static inline void SetChargeTurretDelay(TechnoClass* pThis, int rearmDelay, WeaponTypeClass* pWeapon)
{
	pThis->ChargeTurretDelay = rearmDelay;
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);

	if (pWeaponExt->ChargeTurret_Delays.size() > 0)
	{
		const size_t burstIndex = pWeapon->Burst > 1 ? pThis->CurrentBurstIndex - 1 : 0;
		const size_t index = burstIndex < pWeaponExt->ChargeTurret_Delays.size() ? burstIndex : pWeaponExt->ChargeTurret_Delays.size() - 1;
		const int delay = pWeaponExt->ChargeTurret_Delays[index];

		if (delay <= 0)
			return;

		pThis->ChargeTurretDelay = delay;
		TechnoExt::ExtMap.Find(pThis)->ChargeTurretTimer.Start(delay);
	}
}

DEFINE_HOOK(0x6FE4A4, TechnoClass_FireAt_ChargeTurret1, 0x6)
{
	enum { SkipGameCode = 0x6FE4AA };

	GET(TechnoClass*, pThis, ESI);
	GET(const int, rearmDelay, EAX);
	GET_STACK(WeaponTypeClass*, pWeapon, STACK_OFFSET(0xB0, -0x70));

	SetChargeTurretDelay(pThis, rearmDelay, pWeapon);

	return SkipGameCode;
}

DEFINE_HOOK(0x6FF29E, TechnoClass_FireAt_ChargeTurret2, 0x6)
{
	enum { SkipGameCode = 0x6FF2A4 };

	GET(TechnoClass*, pThis, ESI);
	GET(const int, rearmDelay, EAX);
	GET(WeaponTypeClass*, pWeapon, EBX);

	SetChargeTurretDelay(pThis, rearmDelay, pWeapon);

	return SkipGameCode;
}

#pragma endregion

#pragma region TechnoClass_GetFLH
// Feature: Allow Units using AlternateFLHs - by Trsdy
// I don't want to rewrite something new, so I use the Infantry one directly
// afaik it has no check for infantry-specific stuff here so far
// and neither Ares nor Phobos has touched it, even that crawling flh one was in TechnoClass
DEFINE_JUMP(VTABLE, 0x7F5D20, 0x523250);// Redirect UnitClass::GetFLH to InfantryClass::GetFLH (used to be TechnoClass::GetFLH)

// Apr 4, 2025 - Starkku: Consolidated all the FLH hooks into single one & using TechnoExt::GetFLHAbsoluteCoord() to get the actual coordinate.
DEFINE_HOOK(0x6F3AEB, TechnoClass_GetFLH, 0x6)
{
	enum { SkipGameCode = 0x6F3D50 };

	GET(TechnoClass*, pThis, EBX);
	GET(TechnoTypeClass*, pType, EAX);
	GET(const int, weaponIndex, ESI);
	GET_STACK(CoordStruct*, pCoords, STACK_OFFSET(0xD8, 0x4));

	bool allowOnTurret = true;
	bool useBurstMirroring = true;
	CoordStruct flh = CoordStruct::Empty;

	if (weaponIndex >= 0)
	{
		bool found = false;
		flh = TechnoExt::GetBurstFLH(pThis, weaponIndex, found);
		if (!found)
		{
			if (auto const pInf = abstract_cast<InfantryClass*>(pThis))
				flh = TechnoExt::GetSimpleFLH(pInf, weaponIndex, found);

			if (!found)
				flh = pThis->GetWeapon(weaponIndex)->FLH;
		}
		else
		{
			useBurstMirroring = false;
		}
	}
	else
	{
		int index = -weaponIndex - 1;
		useBurstMirroring = false;
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

		if (index < static_cast<int>(pTypeExt->AlternateFLHs.size()))
			flh = pTypeExt->AlternateFLHs[index];

		if (!pTypeExt->AlternateFLH_OnTurret)
			allowOnTurret = false;
	}

	if (useBurstMirroring && pThis->CurrentBurstIndex % 2 != 0)
		flh.Y = -flh.Y;

	*pCoords = TechnoExt::GetFLHAbsoluteCoords(pThis, flh, allowOnTurret);
	R->EAX(pCoords);

	return SkipGameCode;
}

#pragma endregion

// Basically a hack to make game and Ares pick laser properties from non-Primary weapons.
DEFINE_HOOK(0x70E1A0, TechnoClass_GetTurretWeapon_LaserWeapon, 0x5)
{
	enum { ReturnResult = 0x70E1C8 };

	GET(TechnoClass* const, pThis, ECX);

	if (auto const pBuilding = abstract_cast<BuildingClass*>(pThis))
	{
		auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

		if (pExt->CurrentLaserWeaponIndex.has_value())
		{
			auto const weaponStruct = pThis->GetWeapon(*pExt->CurrentLaserWeaponIndex);
			R->EAX(weaponStruct);
			return ReturnResult;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6FCFE0, TechnoClass_RearmDelay_CanCloakDuringRearm, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EDI);

	TechnoExt::ExtMap.Find(pThis)->CanCloakDuringRearm = !pWeapon->DecloakToFire;

	return 0;
}

DEFINE_HOOK(0x6FD0B5, TechnoClass_RearmDelay_ROF, 0x6)
{
	enum { SkipGameCode = 0x6FD0BB };

	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EDI);

	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto const range = pWeaponExt->ROF_RandomDelay.Get(RulesExt::Global()->ROF_RandomDelay);
	const double rof = pWeapon->ROF * pExt->AE.ROFMultiplier;
	pExt->LastRearmWasFullDelay = true;

	R->EAX(GeneralUtils::GetRangedRandomOrSingleValue(range));
	__asm { fld rof };

	return SkipGameCode;
}

DEFINE_HOOK(0x6FD054, TechnoClass_RearmDelay_ForceFullDelay, 0x6)
{
	enum { ApplyFullRearmDelay = 0x6FD09E };

	GET(TechnoClass*, pThis, ESI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	pExt->LastRearmWasFullDelay = false;

	if (pExt->ForceFullRearmDelay)
		return ApplyFullRearmDelay;

	return 0;
}

// Issue #271: Separate burst delay for weapon type
// Author: Starkku
DEFINE_HOOK(0x6FD05E, TechnoClass_RearmDelay_BurstDelays, 0x7)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EDI);
	GET(const int, idxCurrentBurst, ECX);

	const auto pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	const int burstDelay = pWeaponExt->GetBurstDelay(pThis->CurrentBurstIndex);

	if (burstDelay >= 0)
	{
		R->EAX(burstDelay);
		return 0x6FD099;
	}

	// Restore overridden instructions
	return idxCurrentBurst <= 0 || idxCurrentBurst > 4 ? 0x6FD084 : 0x6FD067;
}

// Update ammo rounds
DEFINE_HOOK(0x6FB086, TechnoClass_Reload_ReloadAmount, 0x8)
{
	GET(TechnoClass* const, pThis, ECX);

	TechnoExt::UpdateSharedAmmo(pThis);

	return 0;
}

// Ammo.Secondary: record which weapon is being fired. All Fire calls (Unit/Infantry/
// Building/Aircraft) funnel through the base TechnoClass::Fire (0x6FDD50).
DEFINE_HOOK(0x6FDD50, TechnoClass_Fire_SecondaryAmmoWeapon, 0x6)
{
	GET(TechnoClass* const, pThis, ECX);
	GET_STACK(int, nWeaponIndex, STACK_OFFSET(0x0, 0x8));

	TechnoExt::ExtMap.Find(pThis)->CurrentFireWeaponIndex = nWeaponIndex;

	return 0;
}

// Ammo.Secondary: redirect ammo consumption so the secondary weapon drains AmmoSecondary
// instead of Ammo. Ares calls TechnoClass::DecreaseAmmo (0x70D670) in a loop for every
// ammo unit consumed; intercept that and split by weapon.
DEFINE_HOOK(0x70D670, TechnoClass_DecreaseAmmo_Secondary, 0x6)
{
	enum { SkipDecrement = 0x70D681 };

	GET(TechnoClass* const, pThis, ECX);

	const auto pExt = TechnoExt::ExtMap.Find(pThis);

	if (TechnoExt::HasSecondaryAmmo(pThis) && pExt->CurrentFireWeaponIndex == 1)
	{
		TechnoExt::DecreaseSecondaryAmmo(pThis);
		return SkipDecrement;
	}

	// Replicate the overwritten `mov eax, dword ptr [ecx + 0x2fc]` then continue.
	R->EAX(pThis->Ammo);
	return 0;
}

// Author: Otamaa
DEFINE_HOOK(0x5223B3, InfantryClass_Approach_Target_DeployFireWeapon, 0x6)
{
	GET(InfantryClass*, pThis, ESI);

	const int deployFireWeapon = pThis->Type->DeployFireWeapon;

	R->EDI(deployFireWeapon == -1 ? pThis->SelectWeapon(pThis->Target) : deployFireWeapon);
	return 0x5223B9;
}

DEFINE_HOOK(0x6FD15E, TechnoClass_GetROF_OccupyFire, 0xA)
{
	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
	{
		GET(BuildingClass*, pThis, ESI);

		R->EAX(pThis->GetOccupantCount());
		return 0x6FD168;
	}

	return 0x6FD183;
}

DEFINE_HOOK(0x6F7288, TechnoClass_InRange_OccupyFire, 0xA)
{
	enum { SkipGameCode = 0x6F72A2 };

	GET(BuildingClass*, pThis, ESI);

	if (!RulesExt::Global()->FixOccupyFire && RulesExt::Global()->UseGlobalOccupyRange)
	{
		R->EDI((RulesClass::Instance->OccupyWeaponRange + pThis->GetOccupyRangeBonus()) << 8);
	}
	else
	{
		GET(int, range, EDI);
		range += pThis->GetOccupyRangeBonus() << 8;
		R->EDI(range);
	}

	return SkipGameCode;
}
