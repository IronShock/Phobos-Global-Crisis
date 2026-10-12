#include <AircraftClass.h>
#include <EventClass.h>
#include <FlyLocomotionClass.h>
#include <Unsorted.h>
#include <AircraftTypeClass.h>
#include <HouseClass.h>
#include <SuperWeaponTypeClass.h>
#include <ScenarioClass.h>
#include <CellClass.h>

#include <Ext/Aircraft/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/Anim/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/BulletType/Body.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

// Ares creates para-drop planes through the exe AircraftClass ctor (0x413D20)
// and never touches the vanilla HouseClass::SendParadropPlanes funnel, so the
// funnel-based SpawnPoints hook never fires. This ctor hook instead identifies
// para-drop aircraft by their type (Ares' ParaDrop.Aircraft) and queues them
// for relocation to a SpawnPoints waypoint cell.
DEFINE_HOOK(0x413D20, AircraftClass_Ctor_ParaDropSpawn, 0x7)
{
	GET(AircraftClass* const, pAircraft, ECX);
	GET_STACK(AircraftTypeClass*, pType, 0x4);
	GET_STACK(HouseClass*, pOwner, 0x8);

	if (!pType || !pOwner)
		return 0;

	// Identify the firing ParaDrop SW: the one whose ParaDrop.Aircraft matches
	// the plane being created AND has SpawnPoints/Delay configured.
	SuperWeaponTypeClass* pSW = nullptr;
	int matchCount = 0;

	for (auto const& pSuper : pOwner->Supers)
	{
		if (pSuper->Type->Type != SuperWeaponType::ParaDrop)
			continue;

		auto const pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

		if (!pExt->ParaDropAircraft || pExt->ParaDropAircraft != pType)
			continue;

		if (pExt->SpawnPoints.empty()
			&& !(pExt->SpawnPoints_Delay.isset() && pExt->SpawnPoints_Delay.Get() > 0))
		{
			continue;
		}

		pSW = pSuper->Type;
		++matchCount;
	}

	if (matchCount != 1 || !pSW)
		return 0;

	auto const pExt = SWTypeExt::ExtMap.Find(pSW);

	// Resolve the configured waypoints into cells (skip missing / zero cells).
	std::vector<CellStruct> cells;

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
				cells.push_back(waypoints[waypoint]);
			}
		}
	}

	if (cells.empty())
		return 0;

	// Pick a waypoint cell for this plane.
	const CellStruct cell = cells[ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(cells.size()) - 1)];

	// SpawnPoints.Delay: hold the plane hidden until this frame, then it
	// enters from the waypoint in the air.
	const int delay = pExt->SpawnPoints_Delay.Get(0);

	ParaDropSpawnPointTemp::PendingReloc.push_back({
		pAircraft, cell, 0,
		delay > 0 ? Unsorted::CurrentFrame + delay : 0
	});

	return 0;
}

#pragma region Mission_Attack

DEFINE_HOOK(0x417FF1, AircraftClass_Mission_Attack_StrafeShots, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	AirAttackStatus const state = (AirAttackStatus)pThis->MissionStatus;

	// Re-evaluate weapon choice due to potentially changing targeting conditions here
	// only when aircraft is adjusting position or picking attack location.
	// This choice is also re-evaluated again every time just before firing UNLESS mid strafing run.
	// See AircraftClass_SelectWeapon_Wrapper and the call redirects below for this.
	if (state > AirAttackStatus::ValidateAZ && state < AirAttackStatus::FireAtTarget)
		pExt->CurrentAircraftWeaponIndex = Math::max(pThis->SelectWeapon(pThis->Target), 0);

	if (state < AirAttackStatus::FireAtTarget2_Strafe
		|| state > AirAttackStatus::FireAtTarget5_Strafe)
	{
		pExt->Strafe_BombsDroppedThisRound = 0;
	}

	// No need to evaluate this before any strafing shots have been fired.
	if (pExt->Strafe_BombsDroppedThisRound)
	{
		auto const pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;
		auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
		int const strafingShots = pWeaponExt->Strafing_Shots.Get(5);

		if (strafingShots > 5)
		{
			if (state == AirAttackStatus::FireAtTarget3_Strafe)
			{
				int const remainingShots = strafingShots - 3 - pExt->Strafe_BombsDroppedThisRound;

				if (remainingShots > 0)
					pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget2_Strafe;
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x4197FC, AircraftClass_GetFireLocation_WeaponRange, 0x6)
{
	enum { SkipGameCode = 0x419808 };

	GET(AircraftClass*, pThis, EDI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	R->EAX(pThis->GetWeaponRange(pExt->CurrentAircraftWeaponIndex));

	return SkipGameCode;
}

// If strafing weapon target is in air, consider the cell it is on as the firing position instead of the object itself if can fire at it.
DEFINE_HOOK(0x4197F3, AircraftClass_GetFireLocation_Strafing, 0x5)
{
	GET(AircraftClass*, pThis, EDI);
	GET(AbstractClass*, pTarget, EAX);

	// pTarget can be nullptr
	auto const pObject = abstract_cast<ObjectClass*>(pTarget);

	if (!pObject || !pObject->IsInAir())
		return 0;

	auto const fireError = pThis->GetFireError(pTarget, TechnoExt::ExtMap.Find(pThis)->CurrentAircraftWeaponIndex, false);

	if (fireError == FireError::ILLEGAL || fireError == FireError::CANT)
		return 0;

	R->EAX(MapClass::Instance.GetCellAt(pObject->GetCoords()));

	return 0;
}

long __stdcall AircraftClass_IFlyControl_IsStrafe(IFlyControl const* ifly)
{
	__assume(ifly != nullptr);

	auto const pThis = static_cast<AircraftClass const*>(ifly);
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	WeaponTypeClass* pWeapon = nullptr;

	pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;

	if (pWeapon)
	{
		auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
		auto const pBulletType = pWeapon->Projectile;
		return pWeaponExt->Strafing.Get(pBulletType->ROT <= 1 && !pBulletType->Inviso && !BulletTypeExt::ExtMap.Find(pBulletType)->TrajectoryType);
	}

	return false;
}

DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2268, AircraftClass_IFlyControl_IsStrafe);

DEFINE_HOOK(0x4180F4, AircraftClass_Mission_Attack_WeaponRange, 0x5)
{
	enum { SkipGameCode = 0x4180FF };

	GET(AircraftClass*, pThis, ESI);

	R->EAX(pThis->GetWeapon(TechnoExt::ExtMap.Find(pThis)->CurrentAircraftWeaponIndex));

	return SkipGameCode;
}

DEFINE_HOOK(0x418403, AircraftClass_Mission_Attack_FireAtTarget_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	const auto pExt = TechnoExt::ExtMap.Find(pThis);

	// Ammo.Secondary: when the secondary weapon is about to fire and a secondary
	// ammo pool is configured, consume AmmoSecondary directly and clear
	// ShouldLoseAmmo so the vanilla state machine (which drains Ammo at
	// 0x418042/0x4180AD/0x418BFE/0x415071) never touches the primary pool.
	if (TechnoExt::HasSecondaryAmmo(pThis) && pExt->CurrentAircraftWeaponIndex == 1)
	{
		TechnoExt::DecreaseSecondaryAmmo(pThis);
		pThis->ShouldLoseAmmo = false;
	}
	else
	{
		pThis->ShouldLoseAmmo = true;
	}

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418478;
}

DEFINE_HOOK(0x4186B6, AircraftClass_Mission_Attack_FireAtTarget2_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x4186D7;
}

DEFINE_HOOK(0x418805, AircraftClass_Mission_Attack_FireAtTarget2Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418826;
}

DEFINE_HOOK(0x418914, AircraftClass_Mission_Attack_FireAtTarget3Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418935;
}

DEFINE_HOOK(0x418A23, AircraftClass_Mission_Attack_FireAtTarget4Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418A44;
}

DEFINE_HOOK(0x418B1F, AircraftClass_Mission_Attack_FireAtTarget5Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418B40;
}

int __fastcall AircraftClass_SelectWeapon_Wrapper(AircraftClass* pThis, void* _, AbstractClass* pTarget)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	// Re-evaluate weapon selection only if not mid-strafing run before firing.
	if (!pExt->Strafe_BombsDroppedThisRound)
		pExt->CurrentAircraftWeaponIndex = Math::max(pThis->SelectWeapon(pTarget), 0);

	return pExt->CurrentAircraftWeaponIndex;
}

DEFINE_FUNCTION_JUMP(CALL6, 0x41831E, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4185F5, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4187C4, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4188D3, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4189E2, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x418AF1, AircraftClass_SelectWeapon_Wrapper);

DEFINE_HOOK_AGAIN(0x41874E, AircraftClass_Mission_Attack_StrafingDestinationFix, 0x6)
DEFINE_HOOK(0x418544, AircraftClass_Mission_Attack_StrafingDestinationFix, 0x6)
{
	GET(FireError, fireError, EAX);
	GET(AircraftClass*, pThis, ESI);

	// The aircraft managed by the spawn manager will not update destination after changing target
	if (fireError == FireError::RANGE && pThis->Is_Strafe())
		pThis->SetDestination(pThis->Target, true);

	return 0;
}

#pragma region After_Shot_Delays

static int GetDelay(AircraftClass* pThis, bool isLastShot)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto const pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	int delay = pWeapon->ROF;

	if (isLastShot || pExt->Strafe_BombsDroppedThisRound == pWeaponExt->Strafing_Shots.Get(5) || (pWeaponExt->Strafing_UseAmmoPerShot && !pThis->Ammo))
	{
		pExt->Strafe_TargetCell = nullptr;
		pThis->MissionStatus = (int)AirAttackStatus::FlyToPosition;
		delay = pWeaponExt->Strafing_EndDelay.Get((pWeapon->Range + (Unsorted::LeptonsPerCell * 4)) / pThis->Type->Speed);
	}

	return delay;
}

DEFINE_HOOK(0x4184CC, AircraftClass_Mission_Attack_Delay1A, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (WeaponTypeExt::ExtMap.Find(pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType)->Strafing_TargetCell)
		pExt->Strafe_TargetCell = MapClass::Instance.GetCellAt(pThis->Target->GetCoords());

	pThis->IsLocked = true;
	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget2_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4184F1;
}

DEFINE_HOOK(0x418506, AircraftClass_Mission_Attack_Delay1B, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->IsLocked = true;
	pThis->MissionStatus = pThis->Ammo > 0 ? (int)AirAttackStatus::PickAttackLocation : (int)AirAttackStatus::ReturnToBase;
	R->EAX(GetDelay(pThis, false));

	return 0x418539;
}

DEFINE_HOOK(0x418883, AircraftClass_Mission_Attack_Delay2, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget3_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4188A1;
}

DEFINE_HOOK(0x418992, AircraftClass_Mission_Attack_Delay3, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget4_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4189B0;
}

DEFINE_HOOK(0x418AA1, AircraftClass_Mission_Attack_Delay4, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget5_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x418ABF;
}

DEFINE_HOOK(0x418B8A, AircraftClass_Mission_Attack_Delay5, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	R->EAX(GetDelay(pThis, true));

	return 0x418BBA;
}

// Ammo.Secondary: aircraft do not go through TechnoClass::DecreaseAmmo (their
// override at 0x41B900 is an empty stub). Instead they drain Ammo directly here at
// the end of an attack run when ShouldLoseAmmo ([esi+0x6C8]) is set. Redirect that
// drain so the secondary weapon consumes AmmoSecondary instead of Ammo.
DEFINE_HOOK(0x418BFE, AircraftClass_LoseAmmo_Secondary, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	if (TechnoExt::HasSecondaryAmmo(pThis))
	{
		const auto pExt = TechnoExt::ExtMap.Find(pThis);

		if (pExt->CurrentFireWeaponIndex == 1)
		{
			TechnoExt::DecreaseSecondaryAmmo(pThis);

			// EAX = 0 so the `cmp eax, ebx / jle` at 0x418C0A-0x418C0C skips the
			// primary Ammo decrement. Continue at 0x418C04 so ShouldLoseAmmo
			// ([esi+0x6C8]) still gets cleared.
			R->EAX(0);

			return 0;
		}
	}

	// Replicate the overwritten `mov eax, dword ptr [esi + 0x2fc]`.
	R->EAX(pThis->Ammo);

	return 0;
}

#pragma endregion

#pragma region StrafeCell

DEFINE_HOOK_AGAIN(0x4188AC, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK_AGAIN(0x4189BB, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK_AGAIN(0x418ACA, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK(0x41879D, AircraftClass_Mission_Attack_StrafeCell, 0x6)
{
	enum { CannotFireNow = 0x418BC5, SkipGameCode = 0x418BBA };

	GET(AircraftClass*, pThis, ESI);

	const auto pExt = TechnoExt::ExtMap.Find(pThis);

	if (const auto pTargetCell = pExt->Strafe_TargetCell)
	{
		switch (pThis->GetFireError(pTargetCell, pExt->CurrentAircraftWeaponIndex, true))
		{
		case FireError::OK:
		case FireError::FACING:
		case FireError::CLOAKED:
		case FireError::RANGE:
			break;
		default:
			return CannotFireNow;
		}

		AircraftExt::FireWeapon(pThis, pTargetCell);

		if (pExt->TypeExtData->FiringForceScatter)
			pTargetCell->ScatterContent(pThis->Location, true, false, false);

		pThis->SetDestination(pTargetCell, true);
		pThis->MissionStatus++;

		R->EAX(GetDelay(pThis, pThis->MissionStatus > static_cast<int>(AirAttackStatus::FireAtTarget5_Strafe)));
		return SkipGameCode;
	}

	return 0;
}

#pragma endregion

#pragma region ScatterCell

DEFINE_HOOK_AGAIN(0x41882C, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x41893B, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x418A4A, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x418B46, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK(0x41847E, AircraftClass_MissionAttack_ScatterCell1, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	return TechnoTypeExt::ExtMap.Find(pThis->Type)->FiringForceScatter ? 0 : (R->Origin() + 0x44);
}

DEFINE_HOOK(0x4186DD, AircraftClass_MissionAttack_ScatterCell2, 0x5)
{
	GET(AircraftClass*, pThis, ESI);
	return TechnoTypeExt::ExtMap.Find(pThis->Type)->FiringForceScatter ? 0 : (R->Origin() + 0x43);
}

#pragma endregion

DEFINE_HOOK(0x414F10, AircraftClass_AI_Trailer, 0x5)
{
	enum { SkipGameCode = 0x414F47 };

	GET(AircraftClass*, pThis, ESI);
	REF_STACK(const CoordStruct, coords, STACK_OFFSET(0x40, -0xC));

	auto const pTrailerAnim = GameCreate<AnimClass>(pThis->Type->Trailer, coords, 1, 1);
	auto const pTrailerAnimExt = AnimExt::ExtMap.Find(pTrailerAnim);
	AnimExt::SetAnimOwnerHouseKind(pTrailerAnim, pThis->Owner, nullptr, false, true);
	pTrailerAnimExt->SetInvoker(pThis);
	pTrailerAnimExt->IsTechnoTrailerAnim = true;

	return SkipGameCode;
}

DEFINE_HOOK(0x414C0B, AircraftClass_ChronoSparkleDelay, 0x5)
{
	R->ECX(RulesExt::Global()->ChronoSparkleDisplayDelay);
	return 0x414C10;
}

#pragma region LandingDir

DEFINE_HOOK(0x4CF31C, FlyLocomotionClass_FlightUpdate_LandingDir, 0x9)
{
	enum { SkipGameCode = 0x4CF3D0, SetSecondaryFacing = 0x4CF351 };

	GET(FootClass** const, pFootPtr, ESI);
	GET_STACK(IFlyControl* const, iFly, STACK_OFFSET(0x48, -0x38));
	REF_STACK(unsigned int, dir, STACK_OFFSET(0x48, 0x8));

	const auto pFoot = *pFootPtr;
	dir = 0;

	if (!iFly)
		return SetSecondaryFacing;

	if (iFly->Is_Locked())
		return SkipGameCode;

	if (const auto pAircraft = abstract_cast<AircraftClass*, true>(pFoot))
		dir = DirStruct(AircraftExt::GetLandingDir(pAircraft)).Raw;
	else
		dir = (iFly->Landing_Direction() << 13);

	return SetSecondaryFacing;
}

namespace SeparateAircraftTemp
{
	BuildingClass* pBuilding = nullptr;
}

DEFINE_HOOK(0x446F57, BuildingClass_GrandOpening_PoseDir_SetContext, 0x6)
{
	GET(BuildingClass*, pThis, EBP);

	SeparateAircraftTemp::pBuilding = pThis;

	return 0;
}

DirType __fastcall AircraftClass_PoseDir_Wrapper(AircraftClass* pThis)
{
	return AircraftExt::GetLandingDir(pThis, SeparateAircraftTemp::pBuilding);
}
DEFINE_FUNCTION_JUMP(CALL, 0x446F67, AircraftClass_PoseDir_Wrapper); // BuildingClass_GrandOpening

DEFINE_HOOK(0x443FC7, BuildingClass_ExitObject_PoseDir1, 0x8)
{
	GET(BuildingClass*, pThis, ESI);
	GET(AircraftClass*, pAircraft, EBP);

	R->EAX(AircraftExt::GetLandingDir(pAircraft, pThis));

	return 0;
}

DEFINE_HOOK(0x44402E, BuildingClass_ExitObject_PoseDir2, 0x5)
{
	GET(BuildingClass*, pThis, ESI);
	GET(AircraftClass*, pAircraft, EBP);

	auto const dir = DirStruct(AircraftExt::GetLandingDir(pAircraft, pThis));

	if (RulesExt::Global()->ExtendedAircraftMissions)
		pAircraft->PrimaryFacing.SetCurrent(dir);

	pAircraft->SecondaryFacing.SetCurrent(dir);

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x415EEE, AircraftClass_Fire_KickOutPassengers, 0x6)
{
	enum { SkipKickOutPassengers = 0x415F08 };

	GET(AircraftClass*, pThis, EDI);
	GET_BASE(const int, weaponIdx, 0xC);

	auto const pWeapon = pThis->GetWeapon(weaponIdx)->WeaponType;

	if (!pWeapon)
		return 0;

	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);

	if (pWeaponExt->KickOutPassengers)
		return 0;

	return SkipKickOutPassengers;
}

// Aircraft mission hard code are all disposable that no ammo, target died or arrived destination all will call the aircraft return airbase
#pragma region ExtendedAircraftMissions

// Waypoint: enable and smooth moving action
bool __fastcall AircraftTypeClass_CanUseWaypoint(AircraftTypeClass* pThis)
{
	return RulesExt::Global()->ExtendedAircraftMissions.Get();
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2908, AircraftTypeClass_CanUseWaypoint)

static __forceinline int GetTurningRadius(AircraftClass* pThis)
{
	constexpr double epsilon = 1e-10;
	constexpr double raw2Radian = Math::TwoPi / 65536;
	// GetRadian<65536>() is an incorrect method
	const double rotRadian = std::abs(static_cast<double>(pThis->PrimaryFacing.ROT.Raw) * raw2Radian);
	return rotRadian > epsilon ? static_cast<int>(static_cast<double>(pThis->Type->Speed) / rotRadian) : 0;
}

// Move: smooth the planning paths and returning route
DEFINE_HOOK_AGAIN(0x4168C7, AircraftClass_Mission_Move_SmoothMoving, 0x5)
DEFINE_HOOK(0x416A0A, AircraftClass_Mission_Move_SmoothMoving, 0x5)
{
	enum { EnterIdleAndReturn = 0x416AC0, ContinueMoving1 = 0x416908, ContinueMoving2 = 0x416A47 };

	GET(AircraftClass* const, pThis, ESI);
	GET(CoordStruct const* const, pCoords, EAX);

	if (pThis->Team || pThis->Airstrike || pThis->Spawned)
		return 0;

	const auto pType = pThis->Type;

	if (!pType->AirportBound)
		return 0;

	const auto extendedMissions = RulesExt::Global()->ExtendedAircraftMissions;

	if (!TechnoTypeExt::ExtMap.Find(pType)->ExtendedAircraftMissions_SmoothMoving.Get(extendedMissions))
		return 0;

	const int distance = static_cast<int>(Point2D { pCoords->X, pCoords->Y }.DistanceFrom(Point2D { pThis->Location.X, pThis->Location.Y }));

	// When the horizontal distance between the aircraft and its destination is greater than half of its deceleration distance
	// or its turning radius, continue to move forward, otherwise return to airbase or execute the next planning waypoint
	const auto rotRadian = std::abs(pThis->PrimaryFacing.ROT.Raw * (Math::TwoPi / 65536)); // GetRadian<65536>() is an incorrect method
	const auto turningRadius = rotRadian > 1e-10 ? static_cast<int>(pType->Speed / rotRadian) : 0;

	if (distance > std::max((pType->SlowdownDistance / 2), turningRadius))
		return (R->Origin() == 0x4168C7 ? ContinueMoving1 : ContinueMoving2);

	// Try next planning waypoint first, then return to air base if it does not exist or cannot be taken
	if (!extendedMissions || (!pThis->TryNextPlanningTokenNode() && pThis->QueuedMission != Mission::Area_Guard))
		pThis->EnterIdleMode(false, true);

	return EnterIdleAndReturn;
}

DEFINE_HOOK(0x4DDD66, FootClass_IsLandZoneClear_ReplaceHardcode, 0x6) // To avoid that the aircraft cannot fly towards the water surface normally
{
	enum { SkipGameCode = 0x4DDD8A };

	GET(FootClass* const, pThis, EBP);
	GET_STACK(CellStruct, cell, STACK_OFFSET(0x20, 0x4));

	const auto pType = pThis->GetTechnoType();

	// In vanilla, only aircrafts or `foots with fly locomotion` will call this virtual function
	// So I don't know why WW use hard-coded `SpeedType::Track` and `MovementZone::Normal` to check this
	R->AL(MapClass::Instance.GetCellAt(cell)->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, true));
	return SkipGameCode;
}

// Skip duplicated aircraft check
DEFINE_PATCH(0x4CF033, 0x8B, 0x06, 0xEB, 0x18); // mov eax, [esi] ; jmp short loc_4CF04F ;
DEFINE_JUMP(LJMP, 0x4179E2, 0x417B44);

// Fix enter mission
DEFINE_HOOK(0x419EF6, AircraftClass_Mission_Enter_FixNotCarryall, 0x7)
{
	enum { SkipGameCode = 0x419EFD };

	GET(AircraftClass* const, pThis, ESI);

	return pThis->Type->Carryall ? 0 : SkipGameCode;
}

// Skip set chaotic ArchiveTarget
void __fastcall AircraftClass_SetArchiveTarget_Wrapper(AircraftClass* pThis, void* _, AbstractClass* pTarget)
{
	if (!RulesExt::Global()->ExtendedAircraftMissions)
		pThis->SetArchiveTarget(pTarget);
}
DEFINE_FUNCTION_JUMP(CALL, 0x41AB09, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC0A, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC2E, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC45, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC68, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41ACB9, AircraftClass_SetArchiveTarget_Wrapper);

DEFINE_HOOK(0x4CF190, FlyLocomotionClass_FlightUpdate_SetPrimaryFacing, 0x6) // Make aircraft not to fly directly to the airport before starting to land
{
	enum { SkipGameCode = 0x4CF29A };

	GET(IFlyControl* const, iFly, EAX);

	if (!iFly || !iFly->Is_Locked())
	{
		GET(FootClass** const, pFootPtr, ESI);
		GET(const int, distance, EBX);

		// No const because it also need to be used by SecondaryFacing
		REF_STACK(CoordStruct, destination, STACK_OFFSET(0x48, 0x8));

		const auto pFoot = *pFootPtr;
		const auto pAircraft = abstract_cast<AircraftClass*, true>(pFoot);

		// Rewrite vanilla implement
		if (!pAircraft || !TechnoTypeExt::ExtMap.Find(pAircraft->Type)->ExtendedAircraftMissions_RearApproach.Get(RulesExt::Global()->ExtendedAircraftMissions))
		{
			const auto footCoords = pFoot->GetCoords();
			const auto desired = DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X));

			if (!iFly || !iFly->Is_Strafe() || distance > 768 // I don't know why it's 3 cells' length, but its vanilla, keep it
				|| std::abs(static_cast<short>(static_cast<short>(desired.Raw) - static_cast<short>(pFoot->PrimaryFacing.Current().Raw))) <= 8192)
			{
				pFoot->PrimaryFacing.SetDesired(desired);
			}
		}
		else
		{
			const auto footCoords = pAircraft->GetCoords();
			const auto landingDir = DirStruct(AircraftExt::GetLandingDir(pAircraft));

			// Try to land from the rear
			if (pAircraft->Destination && (pAircraft->DockNowHeadingTo == pAircraft->Destination || pAircraft->SpawnOwner == pAircraft->Destination))
			{
				const auto pType = pAircraft->Type;

				// Like smooth moving
				const auto rotRadian = std::abs(pAircraft->PrimaryFacing.ROT.Raw * (Math::TwoPi / 65536));
				const auto turningRadius = rotRadian > 1e-10 ? static_cast<int>(pType->Speed / rotRadian) : 0;

				// diameter = 2 * radius
				const auto cellCounts = Math::max((pType->SlowdownDistance / Unsorted::LeptonsPerCell), (turningRadius / 128));

				// The direction of the airport
				const auto currentDir = DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X));

				// Included angle's raw
				const auto difference = static_cast<short>(static_cast<short>(currentDir.Raw) - static_cast<short>(landingDir.Raw));

				// Land from this direction of the airport
				const auto landingFace = landingDir.GetFacing<8>(4);
				auto cellOffset = Unsorted::AdjacentCoord[landingFace];

				// When the direction is opposite, moving to the side first, then automatically shorten based on the current distance
				if (std::abs(difference) >= 12288) // 12288 -> 3/16 * 65536 (1/8 < 3/16 < 1/4, so the landing can begin at the appropriate location)
					cellOffset = (cellOffset + Unsorted::AdjacentCoord[((difference > 0) ? (landingFace + 2) : (landingFace - 2)) & 7]) * cellCounts;
				else // 724 -> 512√2
					cellOffset *= Math::min(cellCounts, ((landingFace & 1) ? (distance / 724) : (distance / 512)));

				// On the way back, increase the offset value of the destination so that it looks like a real airplane
				destination.X += cellOffset.X;
				destination.Y += cellOffset.Y;
			}

			if (footCoords.Y != destination.Y || footCoords.X != destination.X)
				pAircraft->PrimaryFacing.SetDesired(DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X)));
			else
				pAircraft->PrimaryFacing.SetDesired(landingDir);
		}
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x4CF3D0, FlyLocomotionClass_FlightUpdate_SetFlightLevel, 0x7) // Make aircraft not have to fly directly above the airport before starting to descend
{
	GET(FootClass** const, pFootPtr, ESI);

	const auto pAircraft = abstract_cast<AircraftClass*, true>(*pFootPtr);

	if (!pAircraft)
		return 0;

	const auto pType = pAircraft->Type;

	// Ares hook
	if (pType->HunterSeeker)
		return 0;

	if (!TechnoTypeExt::ExtMap.Find(pType)->ExtendedAircraftMissions_EarlyDescend.Get(RulesExt::Global()->ExtendedAircraftMissions))
		return 0;

	enum { SkipGameCode = 0x4CF4D2 };

	GET_STACK(FlyLocomotionClass* const, pThis, STACK_OFFSET(0x48, -0x28));
	GET(const int, distance, EBX);

	// Restore skipped code
	R->EBP(pThis);

	// Same as vanilla
	if (pThis->IsElevating && distance < 768)
	{
		// Fast descent
		const auto floorHeight = MapClass::Instance.GetCellFloorHeight(pThis->MovingDestination);
		pThis->FlightLevel = pThis->MovingDestination.Z - floorHeight;

		// Bug fix
		if (MapClass::Instance.GetCellAt(pAircraft->Location)->ContainsBridge() && pThis->FlightLevel >= CellClass::BridgeHeight)
			pThis->FlightLevel -= CellClass::BridgeHeight;

		return SkipGameCode;
	}

	const auto flightLevel = pType->GetFlightLevel();

	// Check returning actions
	if (distance < pType->SlowdownDistance && pAircraft->Destination
		&& (pAircraft->DockNowHeadingTo == pAircraft->Destination || pAircraft->SpawnOwner == pAircraft->Destination)
		&& (!TechnoTypeExt::ExtMap.Find(pType)->ExtendedAircraftMissions_RearApproach.Get(RulesExt::Global()->ExtendedAircraftMissions)
			|| std::abs(static_cast<short>(static_cast<short>(DirStruct(AircraftExt::GetLandingDir(pAircraft)).Raw) - static_cast<short>(pAircraft->PrimaryFacing.Current().Raw))) < 16384))
	{
		// Slow descent
		const auto floorHeight = MapClass::Instance.GetCellFloorHeight(pThis->MovingDestination);
		const auto destinationHeight = pThis->MovingDestination.Z - floorHeight + 1;
		pThis->FlightLevel = static_cast<int>((flightLevel - destinationHeight) * (static_cast<double>(distance) / pType->SlowdownDistance)) + destinationHeight;
	}
	else
	{
		// Horizontal flight
		pThis->FlightLevel = flightLevel;
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x4CE42A, FlyLocomotionClass_StateUpdate_NoLanding, 0x6) // Prevent aircraft from hovering due to cyclic enter Guard and AreaGuard missions when above buildings
{
	enum { SkipGameCode = 0x4CE441 };

	GET(FootClass* const, pLinkTo, EAX);

	const auto pAircraft = abstract_cast<AircraftClass*, true>(pLinkTo);

	if (!pAircraft || !RulesExt::Global()->ExtendedAircraftMissions)
		return 0;

	if (pAircraft->Airstrike || pAircraft->Spawned || pAircraft->GetCurrentMission() == Mission::Enter)
		return 0;

	return SkipGameCode;
}

DEFINE_HOOK(0x414DA8, AircraftClass_Update_UnlandableDamage, 0x6) // After FootClass_Update
{
	GET(AircraftClass* const, pThis, ESI);

	const auto pType = pThis->Type;

	if (pThis->IsAlive && pType->AirportBound && !pThis->Airstrike && !pThis->Spawned)
	{
		if (RulesExt::Global()->ExtendedAircraftMissions)
		{
			// Check area guard range
			if (const auto pArchive = pThis->ArchiveTarget)
			{
				if (pThis->Target && !pThis->IsFiring && !pThis->IsLocked
					&& pThis->DistanceFrom3D(pArchive) > static_cast<int>(pThis->GetGuardRange(1) * 1.1))
				{
					pThis->SetTarget(nullptr);
					pThis->SetDestination(pArchive, true);
				}
			}

			// Patrol target search: for team-controlled aircraft in Move mission,
			// periodically search for nearby ground targets to attack along the patrol route.
			// This makes patrol triggers work like attack-move for aircraft.
			if (pThis->Team && pThis->IsInAir() && pThis->Ammo && pThis->IsArmed()
				&& !pThis->Target && !pThis->IsLocked
				&& (pThis->GetCurrentMission() == Mission::Move || pThis->GetCurrentMission() == Mission::Patrol)
				&& pThis->Destination)
			{
				if (!pThis->TargetingTimer.HasTimeLeft())
				{
					auto coords = pThis->GetCoords();

					if (pThis->TargetAndEstimateDamage(coords, ThreatType::Area))
					{
						// Save current destination so aircraft can return to patrol route after attack
						pThis->SetArchiveTarget(pThis->Destination);
						pThis->QueueMission(Mission::Attack, false);
					}
				}
			}

			// Check dock building
			pThis->TryNearestDockBuilding(&pType->Dock, 0, 0);
		}

		if (pThis->DockNowHeadingTo)
		{
			// Exit the aimless hovering state and return to the new airport
			if (pThis->GetCurrentMission() == Mission::Area_Guard && pThis->MissionStatus)
			{
				pThis->SetArchiveTarget(nullptr);
				pThis->EnterIdleMode(false, true);
			}
		}
	}

	return 0;
}

// Guard: restart area guard
DEFINE_HOOK(0x41A5C7, AircraftClass_Mission_Guard_StartAreaGuard, 0x6)
{
	enum { SkipGameCode = 0x41A6AC };

	GET(AircraftClass* const, pThis, ESI);

	if (!RulesExt::Global()->ExtendedAircraftMissions
		|| pThis->Team || !pThis->IsArmed() || pThis->Airstrike || pThis->Spawned)
	{
		return 0;
	}

	const auto pArchive = pThis->ArchiveTarget;

	if (!pArchive || !pThis->Ammo)
		return 0;

	pThis->SetDestination(pArchive, true);
	pThis->QueueMission(Mission::Area_Guard, false);
	return SkipGameCode;
}

// AreaGuard: Hover over to guard, or return when no ammo
DEFINE_HOOK_AGAIN(0x41A982, AircraftClass_Mission_AreaGuard, 0x6)
DEFINE_HOOK(0x41A96C, AircraftClass_Mission_AreaGuard, 0x6)
{
	enum { SkipGameCode = 0x41A9DA };

	GET(AircraftClass* const, pThis, ESI);

	if (!RulesExt::Global()->ExtendedAircraftMissions
		|| pThis->Team || !pThis->IsArmed() || pThis->Airstrike || pThis->Spawned)
	{
		return 0;
	}

	auto enterIdleMode = [pThis]() -> bool
	{
		// Avoid duplicate checks in Update
		if (pThis->MissionStatus)
			return false;

		pThis->EnterIdleMode(false, true);

		if (pThis->DockNowHeadingTo)
			return true;

		// Hovering state without any airport
		pThis->MissionStatus = 1;
		return false;
	};
	auto hoverOverArchive = [pThis](const CoordStruct& coords, AbstractClass* pDest)
	{
		const auto& location = pThis->Location;
		const int turningRadius = GetTurningRadius(pThis);
		const double distance = Math::max(1.0, Point2D { coords.X, coords.Y }.DistanceFrom(Point2D { location.X, location.Y }));

		// Random hovering direction
		const double ratio = (((pThis->LastFireBulletFrame + pThis->UniqueID) & 1) ? turningRadius : -turningRadius) / distance;

		// Fly sideways towards the target, and extend the distance to ensure no deceleration
		const CoordStruct destination
		{
			(static_cast<int>(coords.X - ratio * (location.Y - coords.Y)) - location.X) * 4 + location.X,
			(static_cast<int>(coords.Y + ratio * (location.X - coords.X)) - location.Y) * 4 + location.Y,
			coords.Z
		};

		pThis->Locomotor->Move_To(destination);
		pThis->IsLocked = distance < turningRadius;
	};

	if (const auto pArchive = pThis->ArchiveTarget)
	{
		if (pThis->Ammo)
		{
			auto coords = pArchive->GetCoords();

			if (!pThis->TargetingTimer.HasTimeLeft() && pThis->TargetAndEstimateDamage(coords, ThreatType::Area))
			{
				// Without an airport, there is no need to record the previous location
				if (pThis->MissionStatus)
					pThis->SetArchiveTarget(nullptr);

				pThis->QueueMission(Mission::Attack, false);
			}
			else
			{
				// Check dock building
				if (!pThis->MissionStatus && !pThis->TryNearestDockBuilding(&pThis->Type->Dock, 0, 0))
					pThis->MissionStatus = 1;

				hoverOverArchive(coords, pArchive);
			}
		}
		else if (!enterIdleMode() && pThis->IsAlive)
		{
			// continue circling
			hoverOverArchive(pArchive->GetCoords(), pArchive);
		}
	}
	else if (!pThis->Destination)
	{
		enterIdleMode();
	}

	R->EAX(1);
	return SkipGameCode;
}

DEFINE_HOOK(0x7001B0, TechnoClass_MouseOverObject_EnableGuardObject, 0x7)
{
	enum { SkipCheckCanGuard = 0x7001E9, ContinueCheckCanGuard = 0x7001BC };

	GET(TechnoClass* const, pThis, ESI);

	return pThis->WhatAmI() == AbstractType::Aircraft
		&& !RulesExt::Global()->ExtendedAircraftMissions
		? SkipCheckCanGuard : ContinueCheckCanGuard;
}

// Sleep: return to airbase if in incorrect sleep status
int __fastcall AircraftClass_Mission_Sleep(AircraftClass* pThis)
{
	if (!pThis->Destination || pThis->Destination == pThis->DockNowHeadingTo)
		return 450; // Vanilla MissionClass_Mission_Sleep value

	pThis->EnterIdleMode(false, true);
	return 1;
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E24A8, AircraftClass_Mission_Sleep)

// AttackMove: return when no ammo or arrived destination
bool __fastcall AircraftTypeClass_CanAttackMove(AircraftTypeClass* pThis)
{
	return RulesExt::Global()->ExtendedAircraftMissions.Get();
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E290C, AircraftTypeClass_CanAttackMove)

DEFINE_HOOK(0x6FA68B, TechnoClass_Update_AttackMovePaused, 0xA) // To make aircrafts not search for targets while resting at the airport, this is designed to adapt to loop waypoint
{
	enum { SkipGameCode = 0x6FA6F5 };

	GET(TechnoClass* const, pThis, ESI);

	const bool skip = RulesExt::Global()->ExtendedAircraftMissions
		&& pThis->WhatAmI() == AbstractType::Aircraft
		&& (!pThis->Ammo || !pThis->IsInAir());

	return skip ? SkipGameCode : 0;
}

DEFINE_HOOK(0x4DF3BA, FootClass_UpdateAttackMove_AircraftHoldAttackMoveTarget1, 0x6) // When it have MegaDestination
{
	enum { LoseTarget = 0x4DF3D3, HoldTarget = 0x4DF4AB };

	GET(FootClass* const, pThis, ESI);

	// The aircraft is constantly moving, which may cause its target to constantly enter and leave its range, so it is fixed to hold the target.
	if (RulesExt::Global()->ExtendedAircraftMissions && pThis->WhatAmI() == AbstractType::Aircraft)
		return HoldTarget;

	return pThis->InAuxiliarySearchRange(pThis->Target) ? HoldTarget : LoseTarget;
}

DEFINE_HOOK(0x4DF42A, FootClass_UpdateAttackMove_AircraftHoldAttackMoveTarget2, 0x6) // When it have MegaTarget
{
	enum { ContinueCheck = 0x4DF462, HoldTarget = 0x4DF4AB };

	GET(FootClass* const, pThis, ESI);

	// Although if the target selected by CS is an object rather than cell.
	return (RulesExt::Global()->ExtendedAircraftMissions && pThis->WhatAmI() == AbstractType::Aircraft) ? HoldTarget : ContinueCheck;
}

DEFINE_HOOK(0x418CD1, AircraftClass_Mission_Attack_ContinueFlyToDestination, 0x6)
{
	enum { Continue = 0x418C43, Return = 0x418CE8 };

	GET(AircraftClass* const, pThis, ESI);

	if (!pThis->Target)
	{
		if (!RulesExt::Global()->ExtendedAircraftMissions || pThis->Airstrike || pThis->Spawned)
			return Continue;

		if (pThis->MegaMissionIsAttackMove() && pThis->MegaDestination)
		{
			pThis->SetArchiveTarget(nullptr);
			pThis->SetDestination(pThis->MegaDestination, false);
			pThis->QueueMission(Mission::Move, false);
			pThis->HaveAttackMoveTarget = false;
		}
		else if (pThis->ArchiveTarget)
		{
			// Return to the saved destination (patrol route waypoint or guard area)
			// after target is destroyed. Works for both team and non-team aircraft.
			pThis->SetDestination(pThis->ArchiveTarget, true);

			if (pThis->Team)
			{
				// Team aircraft: use Patrol so the unit keeps patrolling the
				// route until the Team AI overrides it with the next waypoint.
				pThis->SetArchiveTarget(nullptr);
				pThis->QueueMission(Mission::Patrol, false);
			}
			else
			{
				pThis->QueueMission(Mission::Area_Guard, false);
			}
		}
		else
		{
			return Continue;
		}
	}
	else
	{
		pThis->MissionStatus = 1;
	}

	R->EAX(1);
	return Return;
}

// Jun 10, 2025 - Starkku: This is a bandaid fix to AI scripting problem that causes more issues than it solves so I have disabled it.
/*
// Idle: clear the target if no ammo
DEFINE_HOOK(0x414D4D, AircraftClass_Update_ClearTargetIfNoAmmo, 0x6)
{
	enum { ClearTarget = 0x414D3F };

	GET(AircraftClass* const, pThis, ESI);

	if (RulesExt::Global()->ExtendedAircraftMissions && !pThis->Ammo && !pThis->Airstrike && !pThis->Spawned)
	{
		if (!SessionClass::IsCampaign()) // To avoid AI's aircrafts team repeatedly attempting to attack the target when no ammo
		{
			if (const auto pTeam = pThis->Team)
				pTeam->LiberateMember(pThis);
		}

		return ClearTarget;
	}

	return 0;
}*/

// Idle: should not crash immediately
DEFINE_HOOK_AGAIN(0x417B82, AircraftClass_EnterIdleMode_NoCrash, 0x6)
DEFINE_HOOK(0x4179F7, AircraftClass_EnterIdleMode_NoCrash, 0x6)
{
	enum { SkipGameCode = 0x417B69 };

	GET(AircraftClass* const, pThis, ESI);

	if (pThis->Airstrike || pThis->Spawned)
		return 0;

	if (!pThis->Team && (pThis->CurrentMission != Mission::Area_Guard || !pThis->ArchiveTarget))
	{
		const auto pCell = reinterpret_cast<CellClass*(__thiscall*)(AircraftClass*)>(0x41A160)(pThis);
		pThis->SetDestination(pCell, true);
		pThis->SetArchiveTarget(pCell);
		pThis->QueueMission(Mission::Area_Guard, true);
	}
	else if (!pThis->Destination)
	{
		const auto pCell = reinterpret_cast<CellClass*(__thiscall*)(AircraftClass*)>(0x41A160)(pThis);
		pThis->SetDestination(pCell, true);
	}

	return SkipGameCode;
}

// Stop: clear the mega mission and return to airbase immediately
// (StopEventFix's DEFINE_HOOK(0x4C75DA, EventClass_RespondToEvent_Stop, 0x6) in Hooks.BugFixes.cpp)

// TakeOff: Emergency takeoff when the airport is destroyed
DEFINE_HOOK(0x4425B6, BuildingClass_ReceiveDamage_NoDestroyLink, 0xA)
{
	enum { LetTakeOff = 0x44259D };

	GET(BuildingClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (!pThis->Type->Helipad)
		return 0;

	const auto pAircraft = abstract_cast<AircraftClass*, true>(pTechno);

	if (!pAircraft)
		return 0;

	return RulesExt::Global()->ExtendedAircraftMissions ? LetTakeOff : 0;
}

// GreatestThreat: for all the mission that should let the aircraft auto select a target
AbstractClass* __fastcall AircraftClass_GreatestThreat(AircraftClass* pThis, void* _, ThreatType threatType, CoordStruct* pSelectCoords, bool onlyTargetHouseEnemy)
{
	// Always add weapon allowed threats so that aircraft (including team-controlled patrol aircraft)
	// can find ground targets during GreatestThreat calls from any code path.
	if (RulesExt::Global()->ExtendedAircraftMissions && pThis->Ammo && !pThis->Airstrike && !pThis->Spawned)
	{
		if (const auto pPrimaryWeapon = pThis->GetWeapon(0)->WeaponType)
			threatType |= pPrimaryWeapon->AllowedThreats();

		if (const auto pSecondaryWeapon = pThis->GetWeapon(1)->WeaponType)
			threatType |= pSecondaryWeapon->AllowedThreats();
	}

	return pThis->FootClass::GreatestThreat(threatType, pSelectCoords, onlyTargetHouseEnemy);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2668, AircraftClass_GreatestThreat)

// Handle assigning area guard mission to aircraft.
DEFINE_HOOK(0x4C7403, EventClass_Execute_AircraftAreaGuard, 0x6)
{
	enum { SkipGameCode = 0x4C7426 };

	GET(EventClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (pTechno->WhatAmI() == AbstractType::Aircraft
		&& RulesExt::Global()->ExtendedAircraftMissions
		&& !pTechno->Ammo)
	{
		// Skip assigning destination / target here.
		R->ESI(&pThis->MegaMission.Target);
		return SkipGameCode;
	}

	return 0;
}

// Do not untether aircraft when assigning area guard mission by default.
DEFINE_HOOK(0x4C72F2, EventClass_Execute_AircraftAreaGuard_Unlink, 0x6)
{
	enum { SkipGameCode = 0x4C7349 };

	GET(EventClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (RulesExt::Global()->ExtendedAircraftMissions
		&& pTechno->WhatAmI() == AbstractType::Aircraft
		&& pThis->MegaMission.Mission == (char)Mission::Area_Guard
		&& !pTechno->Ammo)
	{
		// If we're on dock reloading but have ammo, untether from dock and try to scan for targets.
		return SkipGameCode;
	}

	return 0;
}

DEFINE_HOOK(0x418CF3, AircraftClass_Mission_Attack_PlanningFix, 0x5)
{
	enum { SkipIdle = 0x418D00 };

	GET(AircraftClass*, pThis, ESI);

	return pThis->Ammo <= 0 || !pThis->TryNextPlanningTokenNode() ? 0 : SkipIdle;
}

#pragma endregion

static __forceinline bool CheckSpyPlaneCameraCount(AircraftClass* pThis)
{
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pThis->GetWeapon(0)->WeaponType);

	if (!pWeaponExt->Strafing_Shots.isset())
		return true;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (pExt->Strafe_BombsDroppedThisRound >= pWeaponExt->Strafing_Shots)
		return false;

	pExt->Strafe_BombsDroppedThisRound++;

	return true;
}

DEFINE_HOOK(0x415666, AircraftClass_Mission_SpyPlaneApproach_MaxCount, 0x6)
{
	enum { Skip = 0x41570C };

	GET(AircraftClass*, pThis, ESI);

	if (!CheckSpyPlaneCameraCount(pThis))
		return Skip;

	return 0;
}

DEFINE_HOOK(0x4157EB, AircraftClass_Mission_SpyPlaneOverfly_MaxCount, 0x6)
{
	enum { Skip = 0x415863 };

	GET(AircraftClass*, pThis, ESI);

	if (!CheckSpyPlaneCameraCount(pThis))
		return Skip;

	return 0;
}
