#include "AbilityTypeClass.h"

#include <Utilities/Debug.h>

template<>
const char* Enumerable<AbilityTypeClass>::GetMainSection()
{
	return "AbilityTypes";
}

void AbilityTypeClass::LoadFromINI(CCINIClass* pINI)
{
	const char* section = this->Name;

	if (!pINI->GetSection(section))
		return;

	INI_EX exINI(pINI);

	this->UIName.Read(exINI, section, "UIName");
	this->Description.Read(exINI, section, "Description");
	this->Icon.Read(pINI, section, "Icon");

	this->Activation.Read(exINI, section, "Activation");
	this->Target.Read(exINI, section, "Target");
	this->Range.Read(exINI, section, "Range");
	this->AffectHouses.Read(exINI, section, "AffectHouses");

	this->Cooldown.Read(exINI, section, "Cooldown");
	this->Charges.Read(exINI, section, "Charges");
	this->Recharge.Read(exINI, section, "Recharge");

	this->PassiveTrigger.Read(exINI, section, "Passive.Trigger");
	this->Interval.Read(exINI, section, "Interval");
	this->Duration.Read(exINI, section, "Duration");

	this->Weapon.Read<true>(exINI, section, "Weapon");
	this->Weapon_Instant.Read(exINI, section, "Weapon.Instant");
	this->Weapon_ReleaseAtSelf.Read(exINI, section, "Weapon.ReleaseAtSelf");
	this->Weapon_FLH.Read(exINI, section, "Weapon.FLH");
	this->Weapon_UseFirepowerMultiplier.Read(exINI, section, "Weapon.UseFirepowerMultiplier");
	this->Weapon_UseUnitAmmo.Read(exINI, section, "Weapon.UseUnitAmmo");
	this->Weapon_PlayFiringEffects.Read(exINI, section, "Weapon.PlayFiringEffects");

	this->AttachEffects.Read(exINI, section, "AttachEffects");

	this->Heal_Amount.Read(exINI, section, "Heal.Amount");
	this->Heal_Percent.Read(exINI, section, "Heal.Percent");
	this->Repair_Amount.Read(exINI, section, "Repair.Amount");
	this->Repair_Percent.Read(exINI, section, "Repair.Percent");

	this->Animation.Read(exINI, section, "Animation");
	this->Sound.Read(exINI, section, "Sound");

	if (this->Target == AbilityTarget::Cell && !this->Weapon.isset())
	{
		Debug::Log("[AbilityTypes] [%s] Target=Cell requires a Weapon; the ability will not activate.\n",
			section);
	}
}

template <typename T>
void AbilityTypeClass::Serialize(T& Stm)
{
	Stm
		.Process(this->UIName)
		.Process(this->Description)
		.Process(this->Icon)
		.Process(this->Activation)
		.Process(this->Target)
		.Process(this->Range)
		.Process(this->AffectHouses)
		.Process(this->Cooldown)
		.Process(this->Charges)
		.Process(this->Recharge)
		.Process(this->PassiveTrigger)
		.Process(this->Interval)
		.Process(this->Duration)
		.Process(this->Weapon)
		.Process(this->Weapon_Instant)
		.Process(this->Weapon_ReleaseAtSelf)
		.Process(this->Weapon_FLH)
		.Process(this->Weapon_UseFirepowerMultiplier)
		.Process(this->Weapon_UseUnitAmmo)
		.Process(this->Weapon_PlayFiringEffects)
		.Process(this->AttachEffects)
		.Process(this->Heal_Amount)
		.Process(this->Heal_Percent)
		.Process(this->Repair_Amount)
		.Process(this->Repair_Percent)
		.Process(this->Animation)
		.Process(this->Sound)
		;
}

void AbilityTypeClass::LoadFromStream(PhobosStreamReader& Stm)
{
	this->Serialize(Stm);
}

void AbilityTypeClass::SaveToStream(PhobosStreamWriter& Stm)
{
	this->Serialize(Stm);
}
