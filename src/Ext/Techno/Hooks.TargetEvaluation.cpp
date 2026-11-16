#include "Body.h"

#include <New/Entity/FilterButtonClass.h>
#include <Ext/Rules/Body.h>
#include <Ext/Building/Body.h>
#include <InputManagerClass.h>
#include <Utilities/Debug.h>
#include <Utilities/CrashTrace.h>
#include <Utilities/PointerSanity.h>
#include <Phobos.h>
#include <Unsorted.h>
#include <CellClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <WWMouseClass.h>
#include <algorithm>  // std::find

// Per-weapon ammo: a slot with its own pool cannot fire when the pool is empty,
// and the vanilla Ammo check must not block it while the unit Ammo field is full
// or belongs to another (legacy) weapon. Used by the four GetFireError wrappers.
inline FireError ApplyWeaponAmmoFireError(TechnoClass* pThis, int nWeaponIndex, FireError result)
{
	if (!pThis || nWeaponIndex < 0 || nWeaponIndex >= TechnoTypeClass::MaxWeapons)
		return result;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	TechnoExt::RefreshWeaponAmmoSlot(pThis, nWeaponIndex);

	// The extension can already be gone while the object is being destroyed.
	// RefreshWeaponAmmoSlot handles that case, so only read the runtime state
	// when the extension still exists.
	if (!pExt)
		return result;

	auto const& runtime = pExt->WeaponAmmo[nWeaponIndex];

	if (!runtime.HasPool)
	{
		// An explicit Ammo=0 weapon is independent of the unit Ammo field; do not let
		// vanilla report AMMO just because the unit pool belongs to another weapon.
		if (runtime.Infinite && result == FireError::AMMO)
			return FireError::OK;

		return result;
	}

	if (!TechnoExt::CanFireWeaponSlot(pThis, nWeaponIndex, true))
		return FireError::AMMO;

	// The vanilla check saw the unit Ammo field, which is not this weapon's pool.
	if (result == FireError::AMMO)
		return FireError::OK;

	return result;
}

// ============================================================================
// GetFireError context guard + crash tracing
// ============================================================================
//
// TechnoClass::GetFireError dereferences several raw pointers:
//   - the target object (WhatAmI and type)
//   - this->Locomotor (ILocomotion COM interface: Is_Moving_Now / Can_Fire)
// If the AI target scan or a type conversion left one of them stale, the
// virtual call reads a bogus vtable slot and executes whatever value is there
// (observed once as "EIP=0x4E0"). The helpers below validate the structure of
// those objects without dereferencing their internals, reject the call when
// something is obviously broken, and keep a small trace so the exception
// handler can report the exact object that was being processed.
namespace FireErrorSanity
{
	enum class Result : int
	{
		Ok = 0,
		BadThis = 1,
		BadWeaponIndex = 2,
		BadTarget = 3,
		BadLocomotor = 4,
	};

	inline TechnoTypeClass* SafeType(TechnoClass* pThis)
	{
		if (!PointerSanity::IsSaneObject(pThis))
			return nullptr;

		auto const pType = pThis->GetTechnoType();

		return PointerSanity::IsSaneObject(pType) ? pType : nullptr;
	}

	inline const char* SafeObjectId(ObjectClass* pObj)
	{
		if (!PointerSanity::IsSaneObject(pObj))
			return nullptr;

		if (auto const pTechno = abstract_cast<TechnoClass*>(pObj))
		{
			auto const pType = SafeType(pTechno);

			return pType ? pType->get_ID() : nullptr;
		}

		return nullptr;
	}

	inline ILocomotion* GetLocomotor(TechnoClass* pThis)
	{
		if (!PointerSanity::IsSaneObject(pThis))
			return nullptr;

		if (auto const pFoot = abstract_cast<FootClass*>(pThis))
			return pFoot->Locomotor.GetInterfacePtr();

		return nullptr;
	}

	inline Result Validate(TechnoClass* pThis, ObjectClass* pObj, int nWeaponIndex)
	{
		if (!PointerSanity::IsSaneObject(pThis))
			return Result::BadThis;

		if (nWeaponIndex >= TechnoTypeClass::MaxWeapons)
			return Result::BadWeaponIndex;

		if (!PointerSanity::IsSaneObject(pObj))
			return Result::BadTarget;

		if (auto const pLoco = GetLocomotor(pThis))
		{
			if (!PointerSanity::IsSaneLocomotor(pLoco))
				return Result::BadLocomotor;
		}

		return Result::Ok;
	}

	inline void Record(uint32_t event, TechnoClass* pThis, ObjectClass* pObj,
		int nWeaponIndex, uint32_t phase = 0, uint32_t flags = 0)
	{
		if (!CrashTrace::Enabled)
			return;

		auto const pType = SafeType(pThis);
		auto const pLoco = GetLocomotor(pThis);

		CrashTrace::Push(event, pThis, pObj, pType,
			pType ? pType->get_ID() : nullptr, SafeObjectId(pObj), pLoco,
			pLoco ? CrashTrace::GetVTable(pLoco) : nullptr,
			nWeaponIndex, phase, flags);
	}

	inline void Reject(TechnoClass* pThis, ObjectClass* pObj, int nWeaponIndex,
		Result reason, const char* pWho)
	{
		Record(CrashTrace::FireErrorRejected, pThis, pObj, nWeaponIndex, 0,
			static_cast<uint32_t>(reason));

		// A rejected context usually repeats every frame for the same unit.
		// One line per 30 frames keeps the log readable.
		static int lastLoggedFrame = -1000;
		const int frame = Unsorted::CurrentFrame;

		if (frame - lastLoggedFrame >= 30)
		{
			lastLoggedFrame = frame;

			auto const pType = SafeType(pThis);
			const char* const pId = pType ? pType->get_ID() : "?";
			const char* const pTargetId = SafeObjectId(pObj);

			Debug::Log("[FireErrorGuard] %s rejected (reason=%d) this=%p(%s) target=%p(%s) weapon=%d\n",
				pWho, static_cast<int>(reason), pThis, pId, pObj, pTargetId ? pTargetId : "?", nWeaponIndex);
		}
	}

	inline bool Guard(TechnoClass* pThis, ObjectClass* pObj, int nWeaponIndex, const char* pWho)
	{
		if (!Phobos::Config::GetFireErrorGuard)
			return true;

		auto const result = Validate(pThis, pObj, nWeaponIndex);

		if (result == Result::Ok)
			return true;

		Reject(pThis, pObj, nWeaponIndex, result, pWho);
		return false;
	}
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
// check for a contested UC building.
//
// Original engine code:
//   6FC296  call HouseClass::IsAlliedWith   ; __thiscall, ret 4
//   6FC29B  test al, al
//   6FC29D  je   0x6FCD29                   ; not allied -> CANT
//   6FC2A3                                  ; allied -> continue
//
// The previous implementation hooked the call at 0x6FC296 and simulated the
// callee's `ret 4` with R->ESP(R->ESP() + 4). That only works when the hook
// engine applies ESP modifications to the real stack. The deployed Syringe
// 0.7.3.0 does not: its hook stub restores the registers with `popad`, which
// discards the modified ESP (SyringeFeatures::ESPModification stays false).
// The pushed argument therefore stayed on the stack, the base GetFireError's
// `ret 0xC` popped a value from the wrong slot and jumped to a garbage address
// (observed as EIP=0x4E0, EAX=6, ESP exactly one dword above the normal value,
// for an AI spy scanning the neutral NEGLAMP building).
//
// Hook the instruction after the call instead: IsAlliedWith runs normally and
// pops its own argument, so the stack stays balanced. We only pick the branch
// that the removed `test/je` would have taken.
DEFINE_HOOK(0x6FC29B, TechnoClass_GetFireError_UCAllowDefender, 0x8)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(TechnoClass* const, pTarget, EBP);

	// AL still holds IsAlliedWith's result. A defender-side unit must be able
	// to attack the contested building, so force the "not allied" branch.
	const bool allied = UrbanCombatTargetHelper::CanDefenderAttackUC(pThis, pTarget)
		? false
		: R->AL() != 0;

	return allied ? 0x6FC2A3 : 0x6FCD29;
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

	// ExtraFire mounts are purely visual attachment children; ExtraFire itself does the
	// firing, so a mount must never acquire a target of its own.
	if (TechnoExt::IsExtraFireMount(pThis))
		return DisallowedObject;

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
	if (!FireErrorSanity::Guard(pThis, pObj, nWeaponIndex, "Unit"))
		return FireError::ILLEGAL;

	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	FireErrorSanity::Record(CrashTrace::FireErrorEnter, pThis, pObj, nWeaponIndex);
	AresScheme::Prefix(pThis, pObj, nWeaponIndex, false);
	FireErrorSanity::Record(CrashTrace::FireErrorBeforeEngine, pThis, pObj, nWeaponIndex);
	auto const result = pThis->UnitClass::GetFireError(pObj, nWeaponIndex, ignoreRange);
	FireErrorSanity::Record(CrashTrace::FireErrorAfterEngine, pThis, pObj, nWeaponIndex,
		static_cast<uint32_t>(result));
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

	return ApplyWeaponAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F6030, UnitClass__GetFireError_Wrapper)

FireError __fastcall InfantryClass__GetFireError_Wrapper(InfantryClass* pThis, void* _, ObjectClass* pObj, int nWeaponIndex, bool ignoreRange)
{
	if (!FireErrorSanity::Guard(pThis, pObj, nWeaponIndex, "Infantry"))
		return FireError::ILLEGAL;

	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	FireErrorSanity::Record(CrashTrace::FireErrorEnter, pThis, pObj, nWeaponIndex);
	AresScheme::Prefix(pThis, pObj, nWeaponIndex, false);
	FireErrorSanity::Record(CrashTrace::FireErrorBeforeEngine, pThis, pObj, nWeaponIndex);
	auto const result = pThis->InfantryClass::GetFireError(pObj, nWeaponIndex, ignoreRange);
	FireErrorSanity::Record(CrashTrace::FireErrorAfterEngine, pThis, pObj, nWeaponIndex,
		static_cast<uint32_t>(result));
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

	return ApplyWeaponAmmoFireError(pThis, nWeaponIndex, result);
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
	if (!FireErrorSanity::Guard(pThis, pObj, nWeaponIndex, "Building"))
		return FireError::ILLEGAL;

	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	// UrbanCombat: while an indoor battle is running the building's occupants
	// fight only inside - they stop firing at outside units.
	if (auto const pBuildingExt = BuildingExt::ExtMap.Find(pThis))
	{
		if (pBuildingExt->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::None)
		{
			if (pThis->Target == pObj)
				pThis->Target = nullptr;

			return FireError::CANT;
		}
	}

	FireErrorSanity::Record(CrashTrace::FireErrorEnter, pThis, pObj, nWeaponIndex);
	FireErrorSanity::Record(CrashTrace::FireErrorBeforeEngine, pThis, pObj, nWeaponIndex);
	auto const result = GetOriginalBuildingGetFireError()(pThis, nullptr, pObj, nWeaponIndex, ignoreRange);
	FireErrorSanity::Record(CrashTrace::FireErrorAfterEngine, pThis, pObj, nWeaponIndex,
		static_cast<uint32_t>(result));

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

	return ApplyWeaponAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E427C, BuildingClass__GetFireError_Wrapper)

// AircraftClass::GetFireError (0x41A9E0) adds aircraft-specific checks then
// falls through to the base TechnoClass::GetFireError (0x6FC0B0).

Action __fastcall BuildingClass__WhatAction_Wrapper(BuildingClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	// The carrier's hidden airport proxies must never intercept the cursor or
	// a click. They remain fully functional as native docks.
	if (pThis->Type == BuildingTypeClass::Find("AIRCARRDOCK"))
		return Action::None;

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
	if (!FireErrorSanity::Guard(pThis, pObj, nWeaponIndex, "Aircraft"))
		return FireError::ILLEGAL;

	// TA.VirtualUnit: cannot attack virtual units.
	if (IsUnattackableTarget(pObj))
		return FireError::ILLEGAL;

	FireErrorSanity::Record(CrashTrace::FireErrorEnter, pThis, pObj, nWeaponIndex);
	FireErrorSanity::Record(CrashTrace::FireErrorBeforeEngine, pThis, pObj, nWeaponIndex);
	auto const result = GetOriginalAircraftGetFireError()(pThis, nullptr, pObj, nWeaponIndex, ignoreRange);
	FireErrorSanity::Record(CrashTrace::FireErrorAfterEngine, pThis, pObj, nWeaponIndex,
		static_cast<uint32_t>(result));

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

	return ApplyWeaponAmmoFireError(pThis, nWeaponIndex, result);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2664, AircraftClass__GetFireError_Wrapper)

Action __fastcall AircraftClass__WhatAction_Wrapper(AircraftClass* pThis, void* _, ObjectClass* pObj, bool ignoreForce)
{
	auto const result = GetOriginalAircraftMouseOverObject()(pThis, nullptr, pObj, ignoreForce);

	// Carrier payload interactions:
	//  - hovering the home carrier with an airborne payload shows the return
	//    (Enter) cursor, so clicking it sends the aircraft back to its slot;
	//  - hovering a ground cell with a parked payload temporarily marks it as
	//    airborne so the click assigns the real clicked destination instead of
	//    clamping it to the aircraft's own cell.
	if (auto const pCarrier = Footprint::GetCarrierPayloadOwner(pThis))
	{
		if (pObj == pCarrier)
			return Action::Enter;

		// The carrier is a multi-cell hull; hovering a peripheral deck cell
		// has no object under the cursor, so the anchor-only check above showed
		// a plain Move cursor. Resolve the hovered cell and treat every own
		// hull cell as the return target.
		if (!pObj && WWMouseClass::Instance && TacticalClass::Instance)
		{
			Point2D mouse { 0, 0 };
			WWMouseClass::Instance->GetCoords(&mouse);

			if (mouse.X != 0 || mouse.Y != 0)
			{
				const auto world = TacticalClass::Instance->ClientToCoords(mouse);
				const auto cell = CellClass::Coord2Cell(world);

				if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
				{
					if (Footprint::IsOwnCell(pCarrier, pCell, false))
						return Action::Enter;
				}
			}
		}

		if (!pObj && result == Action::Move && Footprint::IsCarrierPayloadParked(pThis))
			pThis->InAir = true;
	}

	return result;
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
