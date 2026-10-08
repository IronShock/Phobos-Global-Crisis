#include "Body.h"

#include <New/Entity/FilterButtonClass.h>
#include <Ext/Rules/Body.h>
#include <Ext/Building/Body.h>
#include <InputManagerClass.h>
#include <Utilities/Debug.h>
#include <Phobos.h>
#include <Unsorted.h>
#include <algorithm>  // std::find

// Ammo.Secondary: if the secondary weapon is empty, it cannot fire. Used by the
// GetFireError wrappers below (Unit / Infantry / Building / Aircraft).
inline FireError ApplySecondaryAmmoFireError(TechnoClass* pThis, int nWeaponIndex, FireError result)
{
	if (TechnoExt::HasSecondaryAmmo(pThis) && nWeaponIndex == 1)
	{
		if (TechnoExt::ExtMap.Find(pThis)->AmmoSecondary <= 0)
			return FireError::AMMO;
	}

	return result;
}

// TA.VirtualUnit: virtual TA child units cannot be attacked. Any attempt to
// target one fails as ILLEGAL.
inline bool IsUnattackableTarget(ObjectClass* pTarget)
{
	if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
		return TechnoExt::IsVirtualUnit(pTechno);

	return false;
}

// ============================================================================
// Target Filter System - Safe wrapper for IsTargetInFilter
// ============================================================================
// RulesExt::Global() returns nullptr during game restart/cleanup (when
// RulesExt::Data has been cleared). Calling IsTargetInFilter on a null
// pointer causes C0000005 access violations. This wrapper returns true
// (target is in filter / allowed) when RulesExt is unavailable, so filter
// checks are safely skipped during transitions.
static inline bool IsTargetInFilterSafe(int filterMask, TechnoClass* pTarget)
{
	auto const pRules = RulesExt::Global();
	return pRules ? pRules->IsTargetInFilter(filterMask, pTarget) : true;
}

// ============================================================================
// UrbanCombat: allow defender-side units to engage a contested (indoor-battle)
// building. A unit is "defender side" if it is allied with the building owner
// but NOT allied with the initiating attacker house. The hooks below make the
// game treat such a building as a valid (non-allied) target for those units.
// ============================================================================
namespace UrbanCombatTargetHelper
{
	BuildingClass* AsUCBuilding(ObjectClass* pTarget)
	{
		if (!pTarget)
			return nullptr;

		// Fast type gate before touching any building fields.
		if (pTarget->WhatAmI() != AbstractType::Building)
			return nullptr;

		auto const pBld = static_cast<BuildingClass*>(pTarget);

		if (!pBld->IsAlive)
			return nullptr;

		// The ExtData may already be gone while the building is being destroyed
		// (DTOR removes it), so never dereference a null extension.
		auto const pExt = BuildingExt::ExtMap.Find(pBld);

		if (!pExt)
			return nullptr;

		if (pExt->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::Active)
			return nullptr;

		return pBld;
	}

	bool CanDefenderAttackUC(TechnoClass* pUnit, ObjectClass* pTarget)
	{
		if (!pUnit || !pUnit->Owner)
			return false;

		auto const pBld = AsUCBuilding(pTarget);

		return pBld && BuildingExt::IsDefenderSide(pUnit->Owner, pBld);
	}
}

// TechnoClass::GetFireError - let a defender-side unit pass the allied-target
// check for a contested UC building. Hooked on the `call IsAlliedWith`
// (0x6FC296); when the fix applies we report "not allied" so the target is
// treated as hostile, otherwise the original alliance result is reproduced.
// The callee cleans its own argument (ret 4), so the pushed arg is popped and
// execution resumes right after the call.
DEFINE_HOOK(0x6FC296, TechnoClass_GetFireError_UCAllowDefender, 0x5)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(TechnoClass* const, pTarget, EBP);

	if (UrbanCombatTargetHelper::CanDefenderAttackUC(pThis, pTarget))
	{
		R->EAX(0);
	}
	else
	{
		R->EAX(pTarget->Owner->IsAlliedWith(pThis->Owner));
	}

	R->ESP(R->ESP() + 4);

	return 0x6FC29B;
}

#pragma region TargetAcquisition

DEFINE_HOOK(0x7098B9, TechnoClass_TargetSomethingNearby_AutoFire, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	const auto pTechnoExt = TechnoExt::ExtMap.Find(pThis);

	// Target Filter System - do NOT skip target scanning here.
	// Individual targets are filtered in EvaluateObject and CanAutoTargetObject,
	// so only matching targets will be acquired.

	const auto pExt = pTechnoExt->TypeExtData;

	if (pExt->AutoFire)
	{
		if (pExt->AutoFire_TargetSelf)
			pThis->SetTarget(pThis);
		else
			pThis->SetTarget(pThis->GetCell());

		return 0x7099B8;
	}

	return 0;
}

// Target Filter System - prevent retaliation when filter is active.
// The retaliation check is in Hooks.Misc.cpp at 0x7089E8 (AttackMindControlledDelay hook),
// where ESI still holds `this`. At 0x708B0B (the "can retaliate" exit), ESI has been
// overwritten by `mov esi,[eax]` at 0x708AD0, so the check cannot be done there.
// All code paths to 0x708B0B pass through 0x7089E8.

FireError __fastcall TechnoClass_TargetSomethingNearby_CanFire_Wrapper(TechnoClass* pThis, void* _, AbstractClass* pTarget, int weaponIndex, bool ignoreRange)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	const bool disableWeapons = pExt->AE.DisableWeapons;
	pExt->AE.DisableWeapons = false;
	auto const fireError = pThis->GetFireError(pTarget, weaponIndex, ignoreRange);
	pExt->AE.DisableWeapons = disableWeapons;
	return fireError;
}

DEFINE_FUNCTION_JUMP(CALL6, 0x7098E6, TechnoClass_TargetSomethingNearby_CanFire_Wrapper);

#pragma endregion

#pragma region MapZone

namespace MapZoneTemp
{
	TargetZoneScanType zoneScanType;
}

DEFINE_HOOK(0x6F9C67, TechnoClass_GreatestThreat_MapZoneSetContext, 0x5)
{
	GET(TechnoClass*, pThis, ESI);

	auto const pTypeExt = TechnoExt::ExtMap.Find(pThis)->TypeExtData;
	MapZoneTemp::zoneScanType = pTypeExt->TargetZoneScanType;

	return 0;
}

DEFINE_HOOK(0x6F7E47, TechnoClass_EvaluateObject_MapZone, 0x7)
{
	enum { AllowedObject = 0x6F7EA2, DisallowedObject = 0x6F894F };

	GET(TechnoClass*, pThis, EDI);
	GET(ObjectClass*, pObject, ESI);
	GET(const int, zone, EBP);

	// UrbanCombat infantry do not auto-acquire garrisoned enemy buildings. Check
	// the candidate type first: this hook runs for every candidate object during
	// target evaluation, so the UC type lookup is only paid for buildings.
	if (auto const pBuilding = abstract_cast<BuildingClass*>(pObject))
	{
		if (BuildingExt::IsUrbanCombatInfantry(pThis)
			&& BuildingExt::IsGarrisonedEnemyBuilding(pThis, pBuilding))
			return DisallowedObject;
	}

	if (auto const pTechno = abstract_cast<TechnoClass*>(pObject))
	{
		// TA.VirtualUnit: virtual units are never acquired as targets.
		if (TechnoExt::IsVirtualUnit(pTechno))
			return DisallowedObject;

		if (!TechnoExt::AllowedTargetByZone(pThis, pTechno, MapZoneTemp::zoneScanType, nullptr, true, zone))
			return DisallowedObject;

		// Target Filter System - reject only non-matching targets during auto-scanning.
		// Matching targets (e.g., tanks when tank filter is active) are allowed.
		if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
		{
			if (pExt->CurrentTargetFilter != 0
				&& !IsTargetInFilterSafe(pExt->CurrentTargetFilter, pTechno))
				return DisallowedObject;
		}
	}

	return AllowedObject;
}

// Fix the hardcode of healing weapon can't acquire in air target.
DEFINE_HOOK(0x6F9222, TechnoClass_SelectAutoTarget_HealingTargetAir, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	return pThis->CombatDamage(-1) < 0 ? 0x6F922E : 0;
}

#pragma endregion

#pragma region Walls

DEFINE_HOOK(0x70095A, TechnoClass_WhatAction_WallWeapon, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET_STACK(OverlayTypeClass*, pOverlayTypeClass, STACK_OFFSET(0x2C, -0x18));

	R->EAX(pThis->GetWeapon(TechnoExt::GetWeaponIndexAgainstWall(pThis, pOverlayTypeClass)));

	return 0;
}

DEFINE_HOOK(0x51C1F1, InfantryClass_CanEnterCell_WallWeapon, 0x5)
{
	enum { SkipGameCode = 0x51C1FE };

	GET(InfantryClass*, pThis, EBP);
	GET(OverlayTypeClass*, pOverlayTypeClass, ESI);

	R->EAX(pThis->GetWeapon(TechnoExt::GetWeaponIndexAgainstWall(pThis, pOverlayTypeClass)));

	return SkipGameCode;
}

DEFINE_HOOK(0x73F495, UnitClass_CanEnterCell_WallWeapon, 0x6)
{
	enum { SkipGameCode = 0x73F4A1 };

	GET(UnitClass*, pThis, EBX);
	GET(OverlayTypeClass*, pOverlayTypeClass, ESI);

	R->EAX(pThis->GetWeapon(TechnoExt::GetWeaponIndexAgainstWall(pThis, pOverlayTypeClass)));

	return SkipGameCode;
}

namespace CellEvalTemp
{
	int weaponIndex;
}

DEFINE_HOOK(0x6F8C9D, TechnoClass_EvaluateCell_SetContext, 0x7)
{
	GET(const int, weaponIndex, EAX);

	CellEvalTemp::weaponIndex = weaponIndex;

	return 0;
}

WeaponStruct* __fastcall TechnoClass_EvaluateCellGetWeaponWrapper(TechnoClass* pThis)
{
	return pThis->GetWeapon(CellEvalTemp::weaponIndex);
}

int __fastcall TechnoClass_EvaluateCellGetWeaponRangeWrapper(TechnoClass* pThis, void* _, int weaponIndex)
{
	return pThis->GetWeaponRange(CellEvalTemp::weaponIndex);
}

DEFINE_FUNCTION_JUMP(CALL6, 0x6F8CE3, TechnoClass_EvaluateCellGetWeaponWrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x6F8DD2, TechnoClass_EvaluateCellGetWeaponRangeWrapper);

#pragma endregion

#pragma region AggressiveAttackMove

static inline bool CheckAttackMoveCanResetTarget(FootClass* pThis)
{
	const auto pTarget = pThis->Target;

	if (!pTarget || pTarget == pThis->MegaTarget)
		return false;

	const auto pTargetTechno = abstract_cast<TechnoClass*, true>(pTarget);

	if (!pTargetTechno || pTargetTechno->IsArmed())
		return false;

	if (pThis->TargetingTimer.InProgress())
		return false;

	const auto pPrimaryWeapon = pThis->GetWeapon(0)->WeaponType;

	if (!pPrimaryWeapon)
		return false;

	const auto pNewTarget = abstract_cast<TechnoClass*>(pThis->GreatestThreat(ThreatType::Range, &pThis->Location, false));

	if (!pNewTarget || pNewTarget->GetTechnoType() == pTargetTechno->GetTechnoType())
		return false;

	const auto pSecondaryWeapon = pThis->GetWeapon(1)->WeaponType;

	if (!pSecondaryWeapon || !pSecondaryWeapon->NeverUse) // Melee unit's virtual scanner
		return true;

	return pSecondaryWeapon->Range <= pPrimaryWeapon->Range;
}

DEFINE_HOOK(0x4DF3A0, FootClass_UpdateAttackMove_SelectNewTarget, 0x6)
{
	GET(FootClass* const, pThis, ECX);

	const auto pExt = TechnoExt::ExtMap.Find(pThis);
	const auto pRules = RulesExt::Global();

	if (pExt && pRules
		&& pExt->TypeExtData->AttackMove_UpdateTarget.Get(pRules->AttackMove_UpdateTarget)
		&& CheckAttackMoveCanResetTarget(pThis))
	{
		pThis->Target = nullptr;
		pThis->HaveAttackMoveTarget = false;
		pExt->UpdateGattlingRateDownReset();
	}

	return 0;
}

DEFINE_HOOK(0x6F85AB, TechnoClass_CanAutoTargetObject_AggressiveAttackMove, 0x6)
{
	enum { ContinueCheck = 0x6F85BA, CanTarget = 0x6F8604, CannotTarget = 0x6F894F };

	GET(TechnoClass* const, pThis, EDI);
	GET(ObjectClass* const, pObject, ESI);

	const auto pExt = TechnoExt::ExtMap.Find(pThis);

	// Target Filter System - reject only non-matching targets during attack-move/area-guard.
	// Matching targets are allowed through for auto-targeting.
	if (pExt && pExt->CurrentTargetFilter != 0)
	{
		if (auto const pTarget = abstract_cast<TechnoClass*>(pObject))
		{
			if (!IsTargetInFilterSafe(pExt->CurrentTargetFilter, pTarget))
				return CannotTarget;
		}
		else
		{
			return CannotTarget;
		}
	}

	if (!pThis->Owner->IsControlledByHuman())
		return CanTarget;

	if (!pThis->MegaMissionIsAttackMove())
		return ContinueCheck;

	return pExt->TypeExtData->AttackMove_Aggressive.Get(
		RulesExt::Global() ? RulesExt::Global()->AttackMove_Aggressive : false) ? CanTarget : ContinueCheck;
}

#pragma endregion

#pragma region HealingWeapons

#pragma region TechnoClass_EvaluateObject

namespace EvaluateObjectTemp
{
	WeaponTypeClass* PickedWeapon = nullptr;
}

DEFINE_HOOK(0x6F7E24, TechnoClass_EvaluateObject_SetContext, 0x6)
{
	GET(WeaponTypeClass*, pWeapon, EBP);

	EvaluateObjectTemp::PickedWeapon = pWeapon;

	return 0;
}

double __fastcall HealthRatio_Wrapper(TechnoClass* pTechno)
{
	double result = pTechno->GetHealthPercentage();

	if (result >= 1.0)
	{
		const auto pExt = TechnoExt::ExtMap.Find(pTechno);

		if (const auto pShieldData = pExt->Shield.get())
		{
			if (pShieldData->IsActive())
			{
				const auto pWH = EvaluateObjectTemp::PickedWeapon ? EvaluateObjectTemp::PickedWeapon->Warhead : nullptr;
				const auto pFoot = abstract_cast<FootClass*>(pTechno);

				if (!pShieldData->CanBePenetrated(pWH) || ((pFoot && pFoot->ParasiteEatingMe)))
					result = pShieldData->GetHealthRatio();
			}
		}
	}

	return result;
}

DEFINE_FUNCTION_JUMP(CALL, 0x6F7F51, HealthRatio_Wrapper)

#pragma endregion

class AresScheme
{
	static inline ObjectClass* LinkedObj = nullptr;
public:
	static void __cdecl Prefix(TechnoClass* pThis, ObjectClass* pObj, int nWeaponIndex, bool considerEngineers)
	{
		if (LinkedObj)
			return;

		if (considerEngineers && CanApplyEngineerActions(pThis, pObj))
			return;

		if (nWeaponIndex < 0)
			nWeaponIndex = pThis->SelectWeapon(pObj);

		if (const auto pTechno = abstract_cast<TechnoClass*>(pObj))
		{
			const auto pExt = TechnoExt::ExtMap.Find(pTechno);

			if (const auto pShieldData = pExt->Shield.get())
			{
				if (pShieldData->IsActive())
				{
					const auto pWeapon = pThis->GetWeapon(nWeaponIndex)->WeaponType;
					const auto pFoot = abstract_cast<FootClass*>(pObj);

					if (pWeapon && (!pShieldData->CanBePenetrated(pWeapon->Warhead) || (pFoot && pFoot->ParasiteEatingMe)))
					{
						const auto shieldRatio = pExt->Shield->GetHealthRatio();

						if (shieldRatio < 1.0)
						{
							LinkedObj = pObj;
							--LinkedObj->Health;
						}
					}
				}
			}
		}
	}

	static void __cdecl Suffix()
	{
		if (LinkedObj)
		{
			++LinkedObj->Health;
			LinkedObj = nullptr;
		}
	}

private:
	static bool CanApplyEngineerActions(TechnoClass* pThis, ObjectClass* pTarget)
	{
		const auto pInf = abstract_cast<InfantryClass*>(pThis);
		const auto pBuilding = abstract_cast<BuildingClass*>(pTarget);

		if (!pInf || !pBuilding)
			return false;

		const bool allied = HouseClass::CurrentPlayer->IsAlliedWith(pBuilding);
		const auto pType = pBuilding->Type;

		if (allied && pType->Repairable)
			return true;

		if (!allied && pType->Capturable
			&& (!pBuilding->Owner->Type->MultiplayPassive
				|| !pType->CanBeOccupied
				|| pBuilding->IsBeingWarpedOut()))
		{
			return true;
		}

		return false;
	}
};

// Read the original class-specific MouseOverObject implementations. BuildingClass
// and AircraftClass DO override MouseOverObject in the exe (0x447210 and 0x417CC0),
// so reading the base/FootClass vtable slot would bypass their own cursor logic
// (e.g. the UndeploysInto gate that keeps the deploy cursor off ordinary buildings).
using MouseOverObjectPtr = Action(__fastcall*)(ObjectClass*, void*, ObjectClass*, bool);

inline MouseOverObjectPtr GetOriginalBuildingMouseOverObject()
{
	return reinterpret_cast<MouseOverObjectPtr>(0x447210);
}

inline MouseOverObjectPtr GetOriginalAircraftMouseOverObject()
{
	return reinterpret_cast<MouseOverObjectPtr>(0x417CC0);
}

// ============================================================================
// Target Filter System - GetFireError wrappers
// ============================================================================
//
// Design rationale:
//
// When a filter is active and the target does NOT match the filter:
//   - BLOCK with FireError::CANT. No exceptions.
//   - Clear the unit's Target pointer if it matches, to prevent being stuck.
//
// The filter is NEVER cleared here. The ONLY way to clear the filter is
// through the FilterButtonClass UI toggle — the player must explicitly
// turn it off. No automatic behavior (auto-targeting, retaliation, player
// attack commands on non-filtered targets) can clear the filter.
//
// Auto-targeting is additionally blocked at the source: EvaluateObject and
// CanAutoTargetObject reject non-matching targets when a filter is active.
// ============================================================================

FireError __fastcall UnitClass__GetFireError_Wrapper(UnitClass* pThis, void* _, ObjectClass* pObj, int nWeaponIndex, bool ignoreRange)
{
	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	AresScheme::Prefix(pThis, pObj, nWeaponIndex, false);
	auto const result = pThis->UnitClass::GetFireError(pObj, nWeaponIndex, ignoreRange);
	AresScheme::Suffix();

	// === Target Filter System ===
	if (result == FireError::OK)
	{
		if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
		{
			if (pExt->CurrentTargetFilter != 0)
			{
				auto const pTarget = abstract_cast<TechnoClass*>(pObj);
				if (pTarget && !IsTargetInFilterSafe(
					pExt->CurrentTargetFilter, pTarget))
				{
					if (pThis->Target == pObj)
						pThis->Target = nullptr;
					return FireError::CANT;
				}
			}
		}
	}
	// === End Target Filter ===

	return ApplySecondaryAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F6030, UnitClass__GetFireError_Wrapper)

FireError __fastcall InfantryClass__GetFireError_Wrapper(InfantryClass* pThis, void* _, ObjectClass* pObj, int nWeaponIndex, bool ignoreRange)
{
	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	AresScheme::Prefix(pThis, pObj, nWeaponIndex, false);
	auto const result = pThis->InfantryClass::GetFireError(pObj, nWeaponIndex, ignoreRange);
	AresScheme::Suffix();

	// === Target Filter System ===
	if (result == FireError::OK)
	{
		if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
		{
			if (pExt->CurrentTargetFilter != 0)
			{
				auto const pTarget = abstract_cast<TechnoClass*>(pObj);
				if (pTarget && !IsTargetInFilterSafe(
					pExt->CurrentTargetFilter, pTarget))
				{
					if (pThis->Target == pObj)
						pThis->Target = nullptr;
					return FireError::CANT;
				}
			}
		}
	}
	// === End Target Filter ===

	return ApplySecondaryAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7EB418, InfantryClass__GetFireError_Wrapper)

// ============================================================================
// Target Filter System - WhatAction wrappers
// ============================================================================
//
// The filter does NOT modify cursor behavior. When a filter is active and
// the player hovers over a non-matching enemy, the cursor shows the normal
// action (e.g., Attack), but GetFireError will block the actual firing.
// The player must turn off the filter via the UI toggle to attack
// non-filtered targets. No PendingAttackTarget or frame tracking is used.
// ============================================================================

Action __fastcall UnitClass__WhatAction_Wrapper(UnitClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	AresScheme::Prefix(pThis, pObj, -1, false);
	auto result = pThis->UnitClass::MouseOverObject(pObj, ignoreForce);
	AresScheme::Suffix();

	// UrbanCombat: a defender-side unit may order an attack on a contested
	// building, so show the Attack cursor (left-click = attack in this mod).
	if (auto const pUCBld = UrbanCombatTargetHelper::AsUCBuilding(pObj))
	{
		const bool canAttack = BuildingExt::IsDefenderSide(pThis->Owner, pUCBld);

		if (canAttack)
			return Action::Attack;
	}

	if (!pExt || !pExt->ParentAttachment)
		return result;

	switch (result)
	{
	case Action::Repair:
		result = Action::NoRepair;
		break;

	case Action::Self_Deploy:
		if (pThis->Type->DeploysInto)
			result = Action::NoDeploy;
		break;

	case Action::Sabotage:
	case Action::Capture:
	case Action::Enter:
		result = Action::NoEnter;
		break;

	case Action::GuardArea:
	case Action::AttackMoveNav:
	case Action::Move:
		result = Action::NoMove;
		break;
	}

	return result;
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F5CE4, UnitClass__WhatAction_Wrapper)

Action __fastcall InfantryClass__WhatAction_Wrapper(InfantryClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	AresScheme::Prefix(pThis, pObj, -1, pThis->Type->Engineer);
	auto const result = pThis->InfantryClass::MouseOverObject(pObj, ignoreForce);
	AresScheme::Suffix();

	// UrbanCombat infantry use the vanilla "Capture" cursor/action on garrisoned
	// enemy buildings; the actual indoor battle is started from Mission_Capture
	// (see Hooks.UrbanCombat.cpp). No cursor override here.

	// UrbanCombat: a defender-side unit may order an attack on a contested
	// building, so show the Attack cursor (left-click = attack in this mod).
	// A defender-side UC infantry that can reinforce the building instead gets
	// the vanilla Enter cursor so it can garrison the contested building.
	if (auto const pUCBld = UrbanCombatTargetHelper::AsUCBuilding(pObj))
	{
		const bool canAttack = BuildingExt::IsDefenderSide(pThis->Owner, pUCBld);

		if (canAttack && !BuildingExt::IsDefenderReinforce(pThis, pUCBld))
			return Action::Attack;
	}

	return result;
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7EB0CC, InfantryClass__WhatAction_Wrapper)

// ============================================================================
// BuildingClass and AircraftClass GetFireError wrappers
// ============================================================================
//
// BuildingClass and AircraftClass DO override GetFireError in the exe.
// BuildingClass::GetFireError (0x447F10) adds the turret FACING gate on top of
// the base TechnoClass::GetFireError (0x6FC0B0): it returns FireError::FACING
// when the turret (PrimaryFacing) is not aligned with the target, so buildings
// rotate the turret BEFORE firing. AircraftClass::GetFireError (0x41A9E0) adds
// aircraft-specific checks then falls through to the base.
//
// We must call the class-specific implementations, NOT the base one. Reading the
// FootClass vtable (0x7E8C94 + 0x3C0) would return the base 0x6FC0B0, which has
// no FACING gate and would make every building fire omnidirectionally.
using GetFireErrorPtr = FireError(__fastcall*)(TechnoClass*, void*, AbstractClass*, int, bool);

inline GetFireErrorPtr GetOriginalBuildingGetFireError()
{
	return reinterpret_cast<GetFireErrorPtr>(0x447F10);
}

inline GetFireErrorPtr GetOriginalAircraftGetFireError()
{
	return reinterpret_cast<GetFireErrorPtr>(0x41A9E0);
}

FireError __fastcall BuildingClass__GetFireError_Wrapper(BuildingClass* pThis, void* _, ObjectClass* pObj, int nWeaponIndex, bool ignoreRange)
{
	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	// UrbanCombat: while an indoor battle is running the building's occupants
	// fight only inside - they stop firing at outside units.
	if (BuildingExt::ExtMap.Find(pThis)->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::None)
	{
		if (pThis->Target == pObj)
			pThis->Target = nullptr;

		return FireError::CANT;
	}

	auto const result = GetOriginalBuildingGetFireError()(pThis, nullptr, pObj, nWeaponIndex, ignoreRange);

	if (result == FireError::OK)
	{
		if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
		{
			if (pExt->CurrentTargetFilter != 0)
			{
				auto const pTarget = abstract_cast<TechnoClass*>(pObj);
				if (pTarget && !IsTargetInFilterSafe(
					pExt->CurrentTargetFilter, pTarget))
				{
					if (pThis->Target == pObj)
						pThis->Target = nullptr;
					return FireError::CANT;
				}
			}
		}
	}

	return ApplySecondaryAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E427C, BuildingClass__GetFireError_Wrapper)

// AircraftClass::GetFireError (0x41A9E0) adds aircraft-specific checks then
// falls through to the base TechnoClass::GetFireError (0x6FC0B0).

Action __fastcall BuildingClass__WhatAction_Wrapper(BuildingClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	auto const result = GetOriginalBuildingMouseOverObject()(pThis, nullptr, pObj, ignoreForce);

	// UrbanCombat: while an indoor battle is running the occupants must not be
	// released (D-key deploy/eject) - neither attackers nor defenders. Block the
	// Self_Deploy action so the eject is simply unavailable during the fight.
	if (result == Action::Self_Deploy
		&& BuildingExt::ExtMap.Find(pThis)->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::None)
	{
		return Action::NoDeploy;
	}

	return result;
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E3F30, BuildingClass__WhatAction_Wrapper)

FireError __fastcall AircraftClass__GetFireError_Wrapper(AircraftClass* pThis, void* _, ObjectClass* pObj, int nWeaponIndex, bool ignoreRange)
{
	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	auto const result = GetOriginalAircraftGetFireError()(pThis, nullptr, pObj, nWeaponIndex, ignoreRange);

	if (result == FireError::OK)
	{
		if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
		{
			if (pExt->CurrentTargetFilter != 0)
			{
				auto const pTarget = abstract_cast<TechnoClass*>(pObj);
				if (pTarget && !IsTargetInFilterSafe(
					pExt->CurrentTargetFilter, pTarget))
				{
					if (pThis->Target == pObj)
						pThis->Target = nullptr;
					return FireError::CANT;
				}
			}
		}
	}

	return ApplySecondaryAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2664, AircraftClass__GetFireError_Wrapper)

Action __fastcall AircraftClass__WhatAction_Wrapper(AircraftClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	return GetOriginalAircraftMouseOverObject()(pThis, nullptr, pObj, ignoreForce);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2318, AircraftClass__WhatAction_Wrapper)

#pragma endregion

DEFINE_HOOK(0x6F9189, TechnoClass_GreatestThreat_OccupyWeaponRange, 0xA)
{
	enum { ApplyRange = 0x6F91A3 };

	GET(BuildingClass*, pThis, ESI);

	R->EAX((BuildingExt::GetOccupantsRange(pThis) >> 8) + 1);
	return ApplyRange;
}
