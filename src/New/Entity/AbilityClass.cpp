#include "AbilityClass.h"

#include <Phobos.h>

#include <AnimClass.h>
#include <CellClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <TechnoClass.h>
#include <Unsorted.h>
#include <VocClass.h>

#include <Ext/Bullet/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/WeaponType/Body.h>

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace
{
	AbilityClass* PendingAbility = nullptr;
	TechnoClass* PendingOwner = nullptr;
}

AbilityClass::AbilityClass(AbilityTypeClass* pType)
	: Type { pType }
	, Charges { pType ? pType->Charges : -1 }
{
}

void AbilityClass::Update(TechnoClass* pOwner)
{
	if (!this->Type || !pOwner || pOwner->InLimbo || pOwner->Health <= 0)
		return;

	if (this->Type->Activation == AbilityActivation::Active)
	{
		if (this->Type->Charges >= 0
			&& this->Type->Recharge > 0
			&& this->Charges < this->Type->Charges)
		{
			if (!this->RechargeTimer.HasStarted())
			{
				this->RechargeTimer.Start(this->Type->Recharge);
			}
			else if (this->RechargeTimer.Expired())
			{
				++this->Charges;

				if (this->Charges < this->Type->Charges)
					this->RechargeTimer.Start(this->Type->Recharge);
				else
					this->RechargeTimer.Stop();
			}
		}

		return;
	}

	switch (this->Type->PassiveTrigger)
	{
	case AbilityPassiveTrigger::Always:
		if (!this->PassiveApplied)
		{
			this->ApplyEffects(pOwner, pOwner, CellStruct::Empty, true);
			this->PassiveApplied = true;
		}
		break;

	case AbilityPassiveTrigger::Interval:
		if (this->Type->Interval <= 0)
		{
			if (!this->PassiveApplied)
			{
				this->ApplyEffects(pOwner, pOwner, CellStruct::Empty, true);
				this->PassiveApplied = true;
			}
		}
		else
		{
			if (!this->IntervalTimer.HasStarted())
			{
				this->IntervalTimer.Start(this->Type->Interval);
			}
			else if (this->IntervalTimer.Expired())
			{
				this->ApplyEffects(pOwner, pOwner, CellStruct::Empty, true);
				this->IntervalTimer.Start(this->Type->Interval);
			}
		}
		break;

	case AbilityPassiveTrigger::OnDamaged:
		break;
	}
}

bool AbilityClass::CanActivate(TechnoClass* pOwner, AbstractClass* pTarget,
	const CellStruct& cell, bool selfRelease) const
{
	if (!this->Type || !pOwner || pOwner->InLimbo || pOwner->Health <= 0)
		return false;

	if (this->Type->Activation != AbilityActivation::Active)
		return false;

	if (this->Charges == 0)
		return false;

	if (this->Cooldown.InProgress())
		return false;

	if (this->Type->Weapon_UseUnitAmmo
		&& this->Type->Weapon.isset()
		&& pOwner->Ammo == 0)
	{
		return false;
	}

	if (this->Type->Target == AbilityTarget::Cell && !this->Type->Weapon.isset())
		return false;

	return this->ValidateTarget(pOwner, pTarget, cell, selfRelease);
}

bool AbilityClass::ValidateTarget(TechnoClass* pOwner, AbstractClass* pTarget,
	const CellStruct& cell, bool selfRelease) const
{
	if (selfRelease || this->Type->Target == AbilityTarget::Self)
		return true;

	if (this->Type->Target == AbilityTarget::Cell)
	{
		if (!this->Type->Weapon.isset())
			return false;

		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			return false;

		int range = 0;

		if (this->Type->Range > 0)
			range = this->Type->Range * Unsorted::LeptonsPerCell;
		else if (auto const pWeapon = this->Type->Weapon.Get())
			range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pOwner);

		return range <= 0 || pOwner->DistanceFrom(pCell) <= range;
	}

	auto const pTechno = abstract_cast<TechnoClass*>(pTarget);

	if (!pTechno || pTechno->InLimbo || pTechno->Health <= 0
		|| !pTechno->Owner || !pOwner->Owner)
	{
		return false;
	}

	const bool allied = pOwner->Owner->IsAlliedWith(pTechno->Owner);

	if (this->Type->Target == AbilityTarget::Ally && !allied)
		return false;

	if (this->Type->Target == AbilityTarget::Enemy && (allied || pTechno == pOwner))
		return false;

	const auto affected = this->Type->AffectHouses.Get();

	if (pTechno == pOwner)
	{
		if ((affected & AffectedHouse::Owner) == AffectedHouse::None)
			return false;
	}
	else if (allied)
	{
		if ((affected & AffectedHouse::Allies) == AffectedHouse::None)
			return false;
	}
	else if ((affected & AffectedHouse::Enemies) == AffectedHouse::None)
	{
		return false;
	}

	int range = 0;

	if (this->Type->Range > 0)
		range = this->Type->Range * Unsorted::LeptonsPerCell;
	else if (auto const pWeapon = this->Type->Weapon.Get())
		range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pOwner);

	return range <= 0 || pOwner->DistanceFrom(pTechno) <= range;
}

bool AbilityClass::Activate(TechnoClass* pOwner, AbstractClass* pTarget,
	const CellStruct& cell, bool selfRelease)
{
	if (!this->CanActivate(pOwner, pTarget, cell, selfRelease))
		return false;

	if (!this->ApplyEffects(pOwner, pTarget, cell, selfRelease))
		return false;

	this->ConsumeUse(pOwner);
	return true;
}

void AbilityClass::ConsumeUse(TechnoClass* pOwner)
{
	if (this->Type->Charges >= 0 && this->Charges > 0)
	{
		--this->Charges;

		if (this->Type->Recharge > 0 && !this->RechargeTimer.HasStarted())
			this->RechargeTimer.Start(this->Type->Recharge);
	}

	if (this->Type->Cooldown > 0)
		this->Cooldown.Start(this->Type->Cooldown);

	(void)pOwner;
}

bool AbilityClass::ApplyEffects(TechnoClass* pOwner, AbstractClass* pTarget,
	const CellStruct& cell, bool selfRelease)
{
	if (!this->Type || !pOwner || !pOwner->Owner)
		return false;

	if (!this->ApplyWeapon(pOwner, pTarget, cell, selfRelease))
		return false;

	this->ApplyAttachEffects(pOwner, pTarget, selfRelease);
	this->ApplyHealRepair(pOwner, pTarget, selfRelease);

	CoordStruct coords = pOwner->GetCoords();

	if (selfRelease || this->Type->Target == AbilityTarget::Self)
	{
		coords = pOwner->GetCoords();
	}
	else if (this->Type->Target == AbilityTarget::Cell)
	{
		if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
			coords = pCell->GetCoords();
	}
	else if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
	{
		coords = pTechno->GetCoords();
	}

	if (this->Type->Animation.isset())
	{
		if (auto const pAnim = this->Type->Animation.Get())
			GameCreate<AnimClass>(pAnim, coords);
	}

	if (this->Type->Sound.isset())
		VocClass::PlayAt(this->Type->Sound.Get(), pOwner->GetCoords());

	return true;
}

bool AbilityClass::ApplyWeapon(TechnoClass* pOwner, AbstractClass* pTarget,
	const CellStruct& cell, bool selfRelease)
{
	if (!this->Type->Weapon.isset())
		return true;

	auto const pWeapon = this->Type->Weapon.Get();

	if (!pWeapon)
		return false;

	if (this->Type->Weapon_UseUnitAmmo && pOwner->Ammo == 0)
		return false;

	const bool releaseAtSelf = selfRelease || this->Type->Weapon_ReleaseAtSelf;

	CoordStruct targetCoords = pOwner->GetCoords();
	AbstractClass* pBulletTarget = pTarget;

	if (releaseAtSelf || this->Type->Target == AbilityTarget::Self)
	{
		if (this->Type->Weapon_FLH.isset())
		{
			targetCoords = TechnoExt::GetFLHAbsoluteCoords(
				pOwner, this->Type->Weapon_FLH.Get(), false);
		}
		else
		{
			targetCoords = pOwner->GetCoords();
		}

		pBulletTarget = pOwner;
	}
	else if (this->Type->Target == AbilityTarget::Cell)
	{
		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			return false;

		targetCoords = pCell->GetCoords();
		pBulletTarget = pCell;
	}
	else if (pTarget)
	{
		targetCoords = pTarget->GetCoords();
	}

	// ReleaseAtSelf controls where the weapon is aimed/released, not whether
	// the weapon's projectile is skipped. Only Weapon.Instant (or a weapon with
	// no projectile at all) may collapse the shot into a direct warhead
	// detonation.
	const bool instant = this->Type->Weapon_Instant
		|| !pWeapon->Projectile;

	Debug::Log("[Ability] Weapon=%s projectile=%d instant=%d releaseAtSelf=%d\n",
		pWeapon->get_ID(), pWeapon->Projectile ? 1 : 0, instant ? 1 : 0, releaseAtSelf ? 1 : 0);

	if (instant)
	{
		WeaponTypeExt::DetonateAt(pWeapon, targetCoords, pOwner, pOwner->Owner, pBulletTarget);
	}
	else
	{
		int damage = pWeapon->Damage;

		if (this->Type->Weapon_UseFirepowerMultiplier && damage > 0)
		{
			damage = static_cast<int>(
				damage * TechnoExt::GetCurrentFirepowerMultiplier(pOwner));
		}

		CoordStruct source = pOwner->GetFLH(0, CoordStruct::Empty);

		if (this->Type->Weapon_FLH.isset())
		{
			source = TechnoExt::GetFLHAbsoluteCoords(
				pOwner, this->Type->Weapon_FLH.Get(), false);
		}

		if (!pBulletTarget)
			return false;

		if (auto const pBullet = pWeapon->Projectile->CreateBullet(
				pBulletTarget, pOwner, damage, pWeapon->Warhead,
				pWeapon->Speed, pWeapon->Bright))
		{
			BulletExt::SimulatedFiringUnlimbo(
				pBullet, pOwner->Owner, pWeapon, source, false);

			BulletExt::SimulatedFiringEffects(
				pBullet, pOwner->Owner, nullptr,
				this->Type->Weapon_PlayFiringEffects, true);
		}
		else
		{
			return false;
		}
	}

	if (this->Type->Weapon_UseUnitAmmo && pOwner->Ammo > 0)
		--pOwner->Ammo;

	return true;
}

void AbilityClass::ApplyAttachEffects(TechnoClass* pOwner, AbstractClass* pTarget,
	bool selfRelease)
{
	if (this->Type->AttachEffects.empty())
		return;

	TechnoClass* pEffectTarget = selfRelease
		? pOwner
		: abstract_cast<TechnoClass*>(pTarget);

	if (this->Type->Target == AbilityTarget::Cell && !selfRelease)
		pEffectTarget = nullptr;

	if (!pEffectTarget)
		return;

	AEAttachInfoTypeClass info;
	info.AttachTypes = this->Type->AttachEffects;

	if (this->Type->Duration > 0)
	{
		info.DurationOverrides.assign(
			this->Type->AttachEffects.size(), this->Type->Duration);
	}
	else if (this->Type->Duration < 0)
	{
		// Negative duration is permanent for AttachEffectClass.
		info.DurationOverrides.assign(this->Type->AttachEffects.size(), -1);
	}

	AttachEffectClass::Attach(
		pEffectTarget, pOwner->Owner, pOwner, pOwner, info);
}

void AbilityClass::ApplyHealRepair(TechnoClass* pOwner, AbstractClass* pTarget,
	bool selfRelease)
{
	if (this->Type->Heal_Amount == 0 && this->Type->Heal_Percent == 0.0
		&& this->Type->Repair_Amount == 0 && this->Type->Repair_Percent == 0.0)
	{
		return;
	}

	TechnoClass* pEffectTarget = selfRelease
		? pOwner
		: abstract_cast<TechnoClass*>(pTarget);

	if (this->Type->Target == AbilityTarget::Cell && !selfRelease)
		pEffectTarget = nullptr;

	if (!pEffectTarget || pEffectTarget->Health <= 0)
		return;

	auto const pType = pEffectTarget->GetTechnoType();

	if (!pType || pType->Strength <= 0)
		return;

	const double amount = this->Type->Heal_Amount
		+ this->Type->Heal_Percent * pType->Strength / 100.0
		+ this->Type->Repair_Amount
		+ this->Type->Repair_Percent * pType->Strength / 100.0;

	if (amount <= 0.0)
		return;

	pEffectTarget->Health = std::min(
		pType->Strength,
		pEffectTarget->Health + static_cast<int>(std::lround(amount)));
}

bool AbilityClass::HasPending()
{
	return PendingAbility != nullptr && PendingOwner != nullptr;
}

void AbilityClass::CancelTargeting()
{
	PendingAbility = nullptr;
	PendingOwner = nullptr;
}

void AbilityClass::OnOwnerRemoved(TechnoClass* pOwner)
{
	if (PendingOwner == pOwner)
		CancelTargeting();
}

void AbilityClass::OnSlotClicked(TechnoClass* pOwner, int slot)
{
	if (!pOwner || slot < 0)
		return;

	// Active abilities are player input. Never allow the local player to fire
	// an allied/AI unit's ability just because its panel is visible.
	if (!pOwner->Owner || !HouseClass::CurrentPlayer
		|| !pOwner->Owner->IsControlledByCurrentPlayer())
	{
		Debug::Log("[Ability] Slot click rejected owner=%s slot=%d (not controlled by current player)\n",
			pOwner->GetTechnoType()->get_ID(), slot);
		return;
	}

	if (HasPending())
		CancelTargeting();

	auto const pExt = TechnoExt::ExtMap.Find(pOwner);

	if (!pExt || slot >= static_cast<int>(pExt->Abilities.size()))
		return;

	auto& pAbility = pExt->Abilities[slot];

	if (!pAbility || !pAbility->Type
		|| pAbility->Type->Activation != AbilityActivation::Active)
	{
		Debug::Log("[Ability] Slot click owner=%s slot=%d invalid-or-passive\n",
			pOwner->GetTechnoType()->get_ID(), slot);
		return;
	}

	const bool selfRelease = pAbility->Type->Target == AbilityTarget::Self
		|| pAbility->Type->Weapon_ReleaseAtSelf;

	bool activated = false;

	if (selfRelease)
	{
		activated = pAbility->Activate(pOwner, pOwner, CellStruct::Empty, true);
	}
	else
	{
		PendingAbility = pAbility.get();
		PendingOwner = pOwner;
		activated = true;
	}

	Debug::Log("[Ability] Slot click owner=%s slot=%d type=%s self=%d activated=%d\n",
		pOwner->GetTechnoType()->get_ID(), slot,
		pAbility->Type->Name.data(), selfRelease ? 1 : 0, activated ? 1 : 0);
}

bool AbilityClass::HandleTargetClick(ObjectClass* pTarget, const CellStruct& cell)
{
	if (!HasPending())
		return false;

	auto* pAbility = PendingAbility;
	auto* pOwner = PendingOwner;

	if (!pOwner || pOwner->InLimbo || pOwner->Health <= 0
		|| !pOwner->Owner || !HouseClass::CurrentPlayer
		|| !pOwner->Owner->IsControlledByCurrentPlayer())
	{
		CancelTargeting();
		return true;
	}

	pAbility->Activate(pOwner, pTarget, cell, false);
	CancelTargeting();
	return true;
}

void AbilityClass::PollTargeting()
{
	if (!HasPending())
		return;

	if (!PendingOwner || PendingOwner->InLimbo || PendingOwner->Health <= 0
		|| !PendingOwner->Owner || !HouseClass::CurrentPlayer
		|| !PendingOwner->Owner->IsControlledByCurrentPlayer())
	{
		CancelTargeting();
		return;
	}

	if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
		CancelTargeting();
}

void AbilityClass::OnDamaged(TechnoClass* pOwner)
{
	if (!pOwner)
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pOwner);

	if (!pExt)
		return;

	for (auto& pAbility : pExt->Abilities)
	{
		if (!pAbility || !pAbility->Type
			|| pAbility->Type->Activation != AbilityActivation::Passive
			|| pAbility->Type->PassiveTrigger != AbilityPassiveTrigger::OnDamaged)
		{
			continue;
		}

		if (pAbility->Cooldown.InProgress())
			continue;

		pAbility->Cooldown.Start(std::max(1, pAbility->Type->Cooldown.Get()));
		pAbility->ApplyEffects(pOwner, pOwner, CellStruct::Empty, true);
	}
}

template <typename T>
bool AbilityClass::Serialize(T& Stm)
{
	return Stm
		.Process(this->Type)
		.Process(this->Cooldown)
		.Process(this->RechargeTimer)
		.Process(this->IntervalTimer)
		.Process(this->Charges)
		.Process(this->PassiveApplied)
		.Success();
}

bool AbilityClass::Load(PhobosStreamReader& Stm, bool registerForChange)
{
	return this->Serialize(Stm);
}

bool AbilityClass::Save(PhobosStreamWriter& Stm) const
{
	return const_cast<AbilityClass*>(this)->Serialize(Stm);
}
