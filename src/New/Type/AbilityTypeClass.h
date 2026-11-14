#pragma once

#include <Utilities/Constructs.h>
#include <Utilities/Enumerable.h>
#include <Utilities/Enum.h>
#include <Utilities/TemplateDef.h>

#include <New/Type/AttachEffectTypeClass.h>

class AnimTypeClass;
class WeaponTypeClass;

class AbilityTypeClass final : public Enumerable<AbilityTypeClass>
{
public:
	// UI
	Valueable<CSFText> UIName;
	Nullable<CSFText> Description;
	PhobosPCXFile Icon;

	// Activation / targeting
	Valueable<AbilityActivation> Activation;
	Valueable<AbilityTarget> Target;
	Valueable<int> Range; // cells
	Valueable<AffectedHouse> AffectHouses;

	// Active ability limits
	Valueable<int> Cooldown;
	Valueable<int> Charges; // -1 = infinite
	Valueable<int> Recharge;

	// Passive trigger
	Valueable<AbilityPassiveTrigger> PassiveTrigger;
	Valueable<int> Interval;

	// AttachEffect duration override (0 = use AE defaults, -1 = permanent)
	Valueable<int> Duration;

	// Weapon action
	Nullable<WeaponTypeClass*> Weapon;
	Valueable<bool> Weapon_Instant;
	Valueable<bool> Weapon_ReleaseAtSelf;
	Nullable<CoordStruct> Weapon_FLH;
	Valueable<bool> Weapon_UseFirepowerMultiplier;
	Valueable<bool> Weapon_UseUnitAmmo;
	Valueable<bool> Weapon_PlayFiringEffects;

	// Optional AttachEffect action
	ValueableVector<AttachEffectTypeClass*> AttachEffects;

	// Direct effects
	Valueable<int> Heal_Amount;
	Valueable<double> Heal_Percent;
	Valueable<int> Repair_Amount;
	Valueable<double> Repair_Percent;
	Nullable<AnimTypeClass*> Animation;
	Nullable<int> Sound;

	AbilityTypeClass(const char* const pTitle) : Enumerable<AbilityTypeClass>(pTitle)
		, UIName {}
		, Description {}
		, Icon {}
		, Activation { AbilityActivation::Active }
		, Target { AbilityTarget::Self }
		, Range { 0 }
		, AffectHouses { AffectedHouse::All }
		, Cooldown { 0 }
		, Charges { -1 }
		, Recharge { 0 }
		, PassiveTrigger { AbilityPassiveTrigger::Always }
		, Interval { 0 }
		, Duration { 0 }
		, Weapon {}
		, Weapon_Instant { false }
		, Weapon_ReleaseAtSelf { false }
		, Weapon_FLH {}
		, Weapon_UseFirepowerMultiplier { true }
		, Weapon_UseUnitAmmo { false }
		, Weapon_PlayFiringEffects { true }
		, AttachEffects {}
		, Heal_Amount { 0 }
		, Heal_Percent { 0.0 }
		, Repair_Amount { 0 }
		, Repair_Percent { 0.0 }
		, Animation {}
		, Sound {}
	{ }

	virtual void LoadFromINI(CCINIClass* pINI);
	virtual void LoadFromStream(PhobosStreamReader& stm);
	virtual void SaveToStream(PhobosStreamWriter& stm);

private:
	template <typename T>
	void Serialize(T& Stm);
};
