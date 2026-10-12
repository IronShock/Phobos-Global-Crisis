#include <Helpers/Macro.h>
#include <InfantryClass.h>
#include <Unsorted.h>

#include <CCINIClass.h>
#include <map>
#include <set>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Utilities/Debug.h>

namespace FiringAITemp
{
	bool CanFire;
	int WeaponIndex;
	bool IsSecondary;
	WeaponTypeClass* WeaponType;
	FireError FireErrorResult;
	bool WalkFireActive; // fire-on-the-move WalkFire animation is being driven this frame
}

// Last fire frame per fire-on-the-move infantry. The vanilla RearmTimer is pinned
// to the "pending fire" value (5) while the infantry cannot complete the shot, so
// it cannot be used as a ready check - we track the weapon ROF ourselves instead.
static std::map<const InfantryClass*, int> s_fireOnMoveLastFrame;

// WalkFire sequence support. The vanilla SequenceStruct has only 42 slots and no
// WalkFire, and extending it via a binary patch proved unstable. Instead we read
// the [<Sequence>]=WalkFire entry from the unit's art and stash it into a sequence
// slot the unit does not actually use (preferring rarely-used ones), then play it
// with a normal PlayAnim call. Slot index per unit is cached in s_walkFireSlot.
static std::map<const InfantryTypeClass*, int> s_walkFireSlot;

// Per-infantry WalkFire animation frame counter (方案 B: manually driven so the
// state machine's per-frame Walk override cannot freeze it on the first frame).
static std::map<const InfantryClass*, int> s_walkFireFrame;

// FireUp-style discharge frame for fire-on-the-move: [<InfantryArt>]WalkFire=<frame>
// makes the walk-fire shot fire only when the WalkFire animation reaches that frame.
// -1 (default) means no alignment - the shot fires on the weapon ROF as before.
static std::map<const InfantryTypeClass*, int> s_walkFireFireFrame;

// WalkFire animation speed multiplier: [<InfantryArt>]WalkFire.Rate=<double>.
// Default 1.0 = one animation frame per game frame; 0.5 = half speed, 2.0 = double.
// Only the animation is slowed - the weapon ROF is unaffected.
static std::map<const InfantryTypeClass*, double> s_walkFireRate;

// Per-infantry frame accumulator for WalkFire.Rate.
static std::map<const InfantryClass*, double> s_walkFireRateAccum;

// Infantry on a plain Move mission whose Target was acquired by the fire-on-the-move
// hook itself. A normal Move never owns a vanilla Target, so leaving such a
// self-acquired Target behind when the fire-on-the-move context ends (mission
// changed, destination reached/cleared, or the infantry went prone) can leave a
// stale pointer that the engine's firing/AI code dereferences later. We only ever
// clear a Target that we ourselves set.
static std::set<const InfantryClass*> s_fireOnMoveSelfTarget;

// Cached target for a UC infantry walking to a garrisoned building it was ordered
// to seize (Mission::Enter/Capture). pThis->Target must NOT be written during that
// transition (it disturbs the entry/limbo handoff), so the acquired target lives
// here instead and is only refreshed on a staggered schedule - a full GreatestThreat
// scan every frame for every UC infantry was a major source of lag.
static std::map<const InfantryClass*, TechnoClass*> s_ucWalkTarget;

// Erase every per-infantry entry tracked by the fire-on-the-move / WalkFire logic.
// Called when the infantry is destroyed (see InfantryFiring_ClearTracking).
void InfantryFiring_ClearTracking(TechnoClass* pThis)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Infantry)
		return;

	auto const pInf = static_cast<InfantryClass*>(pThis);

	s_fireOnMoveLastFrame.erase(pInf);
	s_walkFireFrame.erase(pInf);
	s_walkFireRateAccum.erase(pInf);
	s_fireOnMoveSelfTarget.erase(pInf);
	s_ucWalkTarget.erase(pInf);
}

static void ReleaseSelfTarget(InfantryClass* pThis)
{
	if (!pThis)
		return;

	s_ucWalkTarget.erase(pThis);

	if (s_fireOnMoveSelfTarget.erase(pThis) && pThis->Target)
		pThis->Target = nullptr;
}

// Reads [<Sequence>]=WalkFire (Start,Count,FacingMultiplier[,Facing]) from the
// infantry type's named sequence section and writes it into a free sequence slot.
// Returns the slot index used, or -1 if WalkFire is not defined / no free slot.
static int LoadWalkFireSequence(InfantryTypeClass* pType)
{
	if (!pType || !pType->Sequence)
		return -1;

	const char* artSection = pType->ImageFile;
	if (!artSection || !artSection[0])
		artSection = pType->ID;

	char seqSection[0x80];
	if (CCINIClass::INI_Art.ReadString(artSection, "Sequence", "", seqSection) <= 0 || !seqSection[0])
		return -1;

	char walkFire[0x40];
	if (CCINIClass::INI_Art.ReadString(seqSection, "WalkFire", "", walkFire) <= 0 || !walkFire[0])
		return -1;

	int start = 0, count = 0, mult = 0;
	if (std::sscanf(walkFire, "%d,%d,%d", &start, &count, &mult) < 3)
		return -1;

	// Prefer rarely-used plain sequence slots that ground infantry normally leave
	// empty. Avoid Hover/Fly/FireFly which are handled specially for air units.
	constexpr Sequence preferred[] = {
		Sequence::Shovel, Sequence::Carry, Sequence::Tumble,
		Sequence::AirDeathStart, Sequence::AirDeathFalling, Sequence::AirDeathFinish
	};

	int slot = -1;
	for (const auto s : preferred)
	{
		if (pType->Sequence->GetSequence(s).CountFrames == 0)
		{
			slot = static_cast<int>(s);
			break;
		}
	}

	if (slot < 0)
		return -1;

	auto& seq = pType->Sequence->GetSequence(static_cast<Sequence>(slot));
	seq.StartFrame = start;
	seq.CountFrames = count;
	seq.FacingMultiplier = mult;

	return slot;
}

// Runs at the very end of InfantryTypeClass::LoadFromINI (success path), after the
// engine parsed the standard sequences, so the SequenceStruct is ready.
DEFINE_HOOK(0x524741, InfantryTypeClass_LoadFromINI_WalkFire, 0xA)
{
	GET(InfantryTypeClass*, pThis, ESI);

	s_walkFireSlot[pThis] = LoadWalkFireSequence(pThis);

	const char* artSection = pThis->ImageFile;
	if (!artSection || !artSection[0])
		artSection = pThis->ID;

	s_walkFireFireFrame[pThis] = CCINIClass::INI_Art.ReadInteger(artSection, "WalkFire", -1);

	s_walkFireRate[pThis] = Math::max(CCINIClass::INI_Art.ReadDouble(artSection, "WalkFire.Rate", 1.0), 0.01);

	return 0;
}


// Returns true if pTarget lies within a +/-15 degree cone facing the infantry's
// current direction (while walking, the infantry faces its movement direction).
static bool IsInFaceCone(InfantryClass* pThis, AbstractClass* pTarget)
{
	if (!pThis || !pTarget)
		return false;

	const auto own = pThis->GetCoords();
	const auto tgt = pTarget->GetCoords();
	const auto desired = DirStruct(Math::atan2(own.Y - tgt.Y, tgt.X - own.X));
	const auto facing = pThis->PrimaryFacing.Current();
	const auto diff = static_cast<short>(desired.Raw) - static_cast<short>(facing.Raw);

	return std::abs(static_cast<short>(diff)) <= 2731; // 2731 ~= 15 degrees (65536 per full turn)
}

DEFINE_HOOK(0x520AD2, InfantryClass_FiringAI_NoTarget, 0x7)
{
	GET(InfantryClass*, pThis, EBP);

	if (pThis->Type->IsGattling)
		pThis->GattlingRateDown(1);

	FiringAITemp::CanFire = false;
	return 0;
}

DEFINE_HOOK(0x5206D2, InfantryClass_FiringAI_SetContext, 0x6)
{
	enum { SkipGameCode = 0x5209A6 };

	GET(InfantryClass*, pThis, EBP);
	GET(int, weaponIndex, EDI);

	auto const pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;

	if (!pWeapon)
	{
		if (pThis->Type->IsGattling)
			pThis->GattlingRateDown(1);

		R->AL(false);
		FiringAITemp::CanFire = false;
		return SkipGameCode;
	}

	const auto pTarget = pThis->Target;
	FiringAITemp::WeaponIndex = weaponIndex;
	FiringAITemp::IsSecondary = TechnoTypeExt::ExtMap.Find(pThis->Type)->IsSecondary(weaponIndex);
	FiringAITemp::WeaponType = pWeapon;
	FiringAITemp::FireErrorResult = pThis->GetFireError(pTarget, weaponIndex, true);
	FiringAITemp::CanFire = true;

	return 0;
}

// Fire-on-the-move target acquisition. A normal Move mission does not auto-acquire
// targets, so while walking we look for the nearest enemy in weapon range and
// assign it as the target; the vanilla FiringAI then fires it (see the promotion in
// InfantryClass_FiringAI_SetContext above). Hooked in InfantryClass::Update right
// before the FiringAI call gate (0x51BF10: `mov eax,[esi+0x5a4]`, ESI = this). The
// vanilla AttackMove (Q) command is left untouched.
DEFINE_HOOK(0x51BF10, InfantryClass_Update_FireOnTheMove, 0x6)
{
	GET(InfantryClass* const, pThis, ESI);

	FiringAITemp::WalkFireActive = false;

	if (!pThis || !pThis->IsAlive || !pThis->Owner)
	{
		ReleaseSelfTarget(pThis);
		return 0;
	}

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->Type);

	if (!pTypeExt->AttackMove_FireOnTheMove)
	{
		ReleaseSelfTarget(pThis);
		return 0;
	}

	// Fire-on-the-move during a normal walking Move, or while a UC infantry walks
	// to a garrisoned building it was ordered to seize (Mission::Enter/Capture).
	// The vanilla AttackMove (Q) command stays vanilla.
	const auto mission = pThis->GetCurrentMission();
	const bool enteringUCBuilding = pTypeExt->UrbanCombat
		&& (mission == Mission::Enter || mission == Mission::Capture);

	if (((mission != Mission::Move && mission != Mission::Patrol) && !enteringUCBuilding) || !pThis->Destination)
	{
		ReleaseSelfTarget(pThis);
		return 0;
	}

	// Crawling (prone) infantry cannot fire while moving.
	if (pThis->Crawling)
	{
		ReleaseSelfTarget(pThis);
		return 0;
	}

	// Resolve the target to fire at. During a normal Move the target is stored in
	// pThis->Target (the vanilla FiringAI may also use it). While a UC infantry walks
	// to a garrisoned building (Mission::Enter/Capture) we must NOT touch pThis->Target
	// - mutating it disturbs the entry/limbo transition and crashes - so we keep the
	// target in a local and fire directly.
	AbstractClass* pTarget = nullptr;

	if (enteringUCBuilding)
	{
		// Reuse the cached target while it is still a valid in-range enemy; only
		// re-scan on a staggered schedule. A full GreatestThreat scan every frame
		// for every UC infantry walking in was far too expensive.
		auto it = s_ucWalkTarget.find(pThis);
		TechnoClass* pCur = (it != s_ucWalkTarget.end()) ? it->second : nullptr;

		bool curValid = pCur && pCur->IsAlive && !pCur->InLimbo
			&& !pThis->Owner->IsAlliedWith(pCur->Owner)
			&& pThis->IsCloseEnoughToAttack(pCur)
			&& (!pTypeExt->AttackMove_FireOnTheMove_Face || IsInFaceCone(pThis, pCur));

		if (!curValid)
		{
			if (it != s_ucWalkTarget.end())
				s_ucWalkTarget.erase(it);

			pCur = nullptr;

			// Throttle the threat scan (staggered per infantry) to keep the cost low.
			if (((Unsorted::CurrentFrame + static_cast<int>(reinterpret_cast<uintptr_t>(pThis) >> 4)) & 7) == 0)
			{
				auto const pEnemy = abstract_cast<TechnoClass*>(pThis->GreatestThreat(ThreatType::Range, &pThis->Location, false));

				if (pEnemy && !pThis->Owner->IsAlliedWith(pEnemy->Owner) && pThis->IsCloseEnoughToAttack(pEnemy)
					&& (!pTypeExt->AttackMove_FireOnTheMove_Face || IsInFaceCone(pThis, pEnemy)))
				{
					s_ucWalkTarget[pThis] = pEnemy;
					pCur = pEnemy;
				}
			}
		}

		pTarget = pCur;
	}
	else
	{
		// Normal Move: keep the current target if it is still a valid in-range enemy
		// (and inside the facing cone for AttackMove.Face=yes), otherwise re-acquire.
		auto const pCur = pThis->Target;
		auto const pCurTech = abstract_cast<TechnoClass*>(pCur);
		bool curValid = pCurTech && !pThis->Owner->IsAlliedWith(pCurTech->Owner) && pThis->IsCloseEnoughToAttack(pCur);

		if (pTypeExt->AttackMove_FireOnTheMove_Face)
			curValid = curValid && IsInFaceCone(pThis, pCur);

		if (curValid)
		{
			pTarget = pCur;
		}
		else
		{
			pThis->Target = nullptr;
			s_fireOnMoveSelfTarget.erase(pThis);

			// Throttle the threat scan (staggered per infantry) to keep the cost low.
			if (((Unsorted::CurrentFrame + static_cast<int>(reinterpret_cast<uintptr_t>(pThis) >> 4)) & 7) == 0)
			{
				auto const pEnemy = abstract_cast<TechnoClass*>(pThis->GreatestThreat(ThreatType::Range, &pThis->Location, false));

				if (pEnemy && !pThis->Owner->IsAlliedWith(pEnemy->Owner) && pThis->IsCloseEnoughToAttack(pEnemy)
					&& (!pTypeExt->AttackMove_FireOnTheMove_Face || IsInFaceCone(pThis, pEnemy)))
				{
					pThis->Target = pEnemy;
					s_fireOnMoveSelfTarget.insert(pThis);
					pTarget = pEnemy;
				}
			}
		}
	}

	// Fire directly while the locomotor is actually moving, throttled by the weapon
	// ROF and respecting Ammo. The WalkFire animation override is applied during both
	// a normal Move and while a UC infantry approaches a garrisoned building it was
	// ordered to seize (Mission::Enter/Capture).
	if (pTarget && pThis->Locomotor->Is_Moving())
	{
		if (!pTypeExt->AttackMove_FireOnTheMove_Face || IsInFaceCone(pThis, pTarget))
		{
			// Mark that the WalkFire animation should be driven this frame. The actual
			// override happens at the very end of InfantryClass::Update (0x51BF80), so
			// the FiringAI / animation-advance functions that run between here and there
			// cannot overwrite it. The epilogue skips limboed infantry, so the override
			// never clobbers the garrison/limbo transition when the UC infantry is
			// absorbed into the indoor battle.
		const auto it = s_walkFireSlot.find(pThis->Type);
		const int walkFireSlot = (it != s_walkFireSlot.end()) ? it->second : -1;

		const int idx = pThis->SelectWeapon(pTarget);
		auto const pWeapon = pThis->GetWeapon(idx)->WeaponType;

		// Respect Ammo: a unit with an ammo pool cannot fire when it is empty.
		// When out of ammo the WalkFire override is not armed either, so the normal
		// Walk animation plays until the ammo pool is refilled.
		if (pWeapon && pThis->Type->Ammo > 0 && pThis->Ammo <= 0)
			return 0;

		if (walkFireSlot >= 0
			&& pThis->Type->Sequence->GetSequence(static_cast<Sequence>(walkFireSlot)).CountFrames > 0)
		{
			FiringAITemp::WalkFireActive = true;
		}

			const int rof = Math::max(pWeapon->ROF, 1);

			// AttackMove.FireRate scales the fire-on-the-move rate of fire:
			// effective interval = weapon ROF / FireRate (default 0.8 -> slower).
			const double fireRate = Math::max(pTypeExt->AttackMove_FireRate, 0.0);
			const int effectiveRof = (fireRate > 0.0)
				? Math::max(static_cast<int>(rof / fireRate), 1)
				: rof;

			auto& lastFrame = s_fireOnMoveLastFrame[pThis];

			if (lastFrame < 0 || Unsorted::CurrentFrame - lastFrame >= effectiveRof)
			{
				// FireUp-style WalkFire discharge frame: [<InfantryArt>]WalkFire=<frame>
				// fires only when the WalkFire animation reaches that frame. Only applies
				// when a WalkFire sequence is actually defined.
				const auto fit = s_walkFireFireFrame.find(pThis->Type);
				const int dischargeFrame = (fit != s_walkFireFireFrame.end()) ? fit->second : -1;

				if (dischargeFrame >= 0 && walkFireSlot >= 0)
				{
					auto const wfIt = s_walkFireFrame.find(pThis);
					const int animFrame = (wfIt != s_walkFireFrame.end()) ? wfIt->second : 0;

					if (animFrame != dischargeFrame)
						return 0;
				}

				lastFrame = Unsorted::CurrentFrame;

					// Revalidate the target right before the shot: it was picked
					// earlier this frame, so it may have just died/entered limbo
					// (e.g. another volley of this squad resolved first). Firing
					// into a limboed object poisons the engine's weapon/target
					// bookkeeping, so skip the shot instead.
					if (pTarget && !abstract_cast<ObjectClass*>(pTarget)->InLimbo && pThis->Fire(pTarget, idx))
					{
						if (walkFireSlot < 0)
							pThis->PlayAnim(Sequence::FireUp);
					}
			}
		}
	}

	return 0;
}

// Runs at the very end of InfantryClass::Update (0x51BF80, its epilogue) - after
// the FiringAI, the animation-advance and 0x520F40 all ran - so this is the last
// write before the draw and cannot be overwritten by the animation state machine.
// Applies the fire-on-the-move WalkFire override: forces SequenceAnim and manually
// advances the animation frame counter (Infantry +0xF8, compared against the
// sequence's CountFrames) so WalkFire loops 0..CountFrames-1. ESI = this.
DEFINE_HOOK(0x51BF80, InfantryClass_Update_ApplyWalkFire, 0x7)
{
	GET(InfantryClass* const, pThis, ESI);

	if (!FiringAITemp::WalkFireActive || !pThis || !pThis->Type || pThis->InLimbo)
		return 0;

	const auto it = s_walkFireSlot.find(pThis->Type);
	const int walkFireSlot = (it != s_walkFireSlot.end()) ? it->second : -1;

	if (walkFireSlot < 0)
		return 0;

	const int count = pThis->Type->Sequence->GetSequence(static_cast<Sequence>(walkFireSlot)).CountFrames;
	if (count <= 0)
		return 0;

	pThis->SequenceAnim = static_cast<Sequence>(walkFireSlot);

	auto& frame = s_walkFireFrame[pThis];

	// WalkFire.Rate scales the animation speed: the frame counter advances by
	// the accumulated rate, so 1.0 = one frame per game frame, 0.5 = half speed,
	// 2.0 = double speed. Hold frames keep writing the same frame, so the
	// animation freezes in place while walking.
	const auto rit = s_walkFireRate.find(pThis->Type);
	const double rate = (rit != s_walkFireRate.end()) ? Math::max(rit->second, 0.01) : 1.0;

	double& acc = s_walkFireRateAccum[pThis];
	acc += rate;

	int steps = static_cast<int>(acc);
	if (steps > 0)
	{
		acc -= steps;
		frame = (frame + steps) % count;
	}
	*reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + 0xF8) = frame;

	return 0;
}

// avoid repeatedly calling GetFireError().
DEFINE_HOOK_AGAIN(0x5209D2, InfantryClass_FiringAI_SetFireError, 0x6)
DEFINE_HOOK(0x5206E4, InfantryClass_FiringAI_SetFireError, 0x6)
{
	R->EAX(FiringAITemp::FireErrorResult);
	return R->Origin() == 0x5206E4 ? 0x5206F9 : 0x5209E4;
}

// determine if it is the second.
DEFINE_HOOK_AGAIN(0x520968, InfantryClass_UpdateFiring_IsSecondary, 0x6)
DEFINE_HOOK(0x520888, InfantryClass_UpdateFiring_IsSecondary, 0x8)
{
	GET(InfantryClass*, pThis, EBP);
	const bool isSecondary = FiringAITemp::IsSecondary;

	if (R->Origin() == 0x520888)
	{
		R->AL(pThis->Crawling);
		return isSecondary ? 0x520890 : 0x5208DC;
	}
	else if (isSecondary)
	{
		return pThis->Crawling ? 0x520970 : 0x52098A;
	}
	else
	{
		return 0x5209A0;
	}
}

DEFINE_HOOK(0x5209AF, InfantryClass_FiringAI, 0x6)
{
	enum { Continue = 0x5209CD, ReturnFromFunction = 0x520AD9 };

	GET(InfantryClass*, pThis, EBP);
	GET(int, firingFrame, EDX);

	int cumulativeDelay = 0;
	int projectedDelay = 0;
	const int weaponIndex = FiringAITemp::WeaponIndex;
	const auto pWeaponExt = WeaponTypeExt::ExtMap.Find(FiringAITemp::WeaponType);
	const bool allowBurst = pWeaponExt->Burst_FireWithinSequence;

	// Calculate cumulative burst delay as well cumulative delay after next shot (projected delay).
	if (allowBurst)
	{
		for (int i = 0; i <= pThis->CurrentBurstIndex; i++)
		{
			const int burstDelay = pWeaponExt->GetBurstDelay(i);
			int delay = (burstDelay > -1) ? burstDelay : ScenarioClass::Instance->Random.RandomRanged(3, 5);

			// Other than initial delay, treat 0 frame delays as 1 frame delay due to per-frame processing.
			if (i != 0)
				delay = Math::max(delay, 1);

			cumulativeDelay += delay;

			if (i == pThis->CurrentBurstIndex)
				projectedDelay = cumulativeDelay + delay;
		}
	}

	if (TechnoExt::HandleDelayedFireWithPauseSequence(pThis, weaponIndex, firingFrame + cumulativeDelay))
		return ReturnFromFunction;

	if (pThis->Animation.Value == firingFrame + cumulativeDelay)
	{
		if (allowBurst)
		{
			int frameCount = pThis->Type->Sequence->GetSequence(pThis->SequenceAnim).CountFrames;

			// If projected frame for firing next shot goes beyond the sequence frame count, cease firing after this shot and start rearm timer.
			if (firingFrame + projectedDelay > frameCount)
				TechnoExt::ExtMap.Find(pThis)->ForceFullRearmDelay = true;
		}

		R->EAX(weaponIndex); // Reuse the weapon index to save some time.
		return Continue;
	}

	return ReturnFromFunction;
}

DEFINE_HOOK(0x520AD9, InfantryClass_FiringAI_IsGattling, 0x5)
{
	GET(InfantryClass*, pThis, EBP);

	if (FiringAITemp::CanFire)
	{
		if (pThis->Type->IsGattling)
		{
			const FireError fireError = FiringAITemp::FireErrorResult;

			switch (fireError)
			{
			case FireError::OK:
			case FireError::REARM:
			case FireError::FACING:
			case FireError::ROTATING:
			{
				if (pThis->IsDeployed())
					pThis->GattlingRateDown(1);
				else
					pThis->GattlingRateUp(1);

				break;
			}
			default:
				pThis->GattlingRateDown(1);
				break;
			}
		}

		FiringAITemp::CanFire = false;
	}

	return 0;
}

DEFINE_HOOK(0x5209EE, InfantryClass_UpdateFiring_BurstNoDelay, 0x5)
{
	enum { SkipVanillaFire = 0x520A57 };

	GET(InfantryClass* const, pThis, EBP);
	GET(const int, wpIdx, ESI);
	GET(AbstractClass* const, pTarget, EAX);

	if (const auto pWeapon = pThis->GetWeapon(wpIdx)->WeaponType)
	{
		if (pWeapon->Burst > 1)
		{
			const auto pExt = WeaponTypeExt::ExtMap.Find(pWeapon);

			if (pExt->Burst_NoDelay && (!pExt->DelayedFire_Duration.isset() || pExt->DelayedFire_OnlyOnInitialBurst))
			{
				if (pThis->Fire(pTarget, wpIdx))
				{
					if (!pThis->CurrentBurstIndex)
						return SkipVanillaFire;

					auto rof = pThis->RearmTimer.TimeLeft;
					pThis->RearmTimer.Start(0);

					for (auto i = pThis->CurrentBurstIndex; i < pWeapon->Burst && pThis->GetFireError(pTarget, wpIdx, true) == FireError::OK && pThis->Fire(pTarget, wpIdx); ++i)
					{
						rof = pThis->RearmTimer.TimeLeft;
						pThis->RearmTimer.Start(0);
					}

					pThis->RearmTimer.Start(rof);
				}

				return SkipVanillaFire;
			}
		}
	}

	return 0;
}
