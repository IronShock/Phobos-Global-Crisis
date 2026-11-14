#pragma once

#include <GeneralDefinitions.h>
#include <Timer.h>

#include <New/Type/AbilityTypeClass.h>

class AbstractClass;
class CellClass;
class ObjectClass;
class TechnoClass;

class AbilityClass
{
public:
	AbilityTypeClass* Type = nullptr;
	CDTimerClass Cooldown;
	CDTimerClass RechargeTimer;
	CDTimerClass IntervalTimer;
	int Charges = -1;
	bool PassiveApplied = false;

	AbilityClass() = default;
	explicit AbilityClass(AbilityTypeClass* pType);

	void Update(TechnoClass* pOwner);

	bool CanActivate(TechnoClass* pOwner, AbstractClass* pTarget,
		const CellStruct& cell, bool selfRelease) const;

	bool Activate(TechnoClass* pOwner, AbstractClass* pTarget,
		const CellStruct& cell, bool selfRelease);

	bool ApplyEffects(TechnoClass* pOwner, AbstractClass* pTarget,
		const CellStruct& cell, bool selfRelease);

	// UI / targeting entry points.
	static void OnSlotClicked(TechnoClass* pOwner, int slot);
	static bool HasPending();
	static void CancelTargeting();
	static void OnOwnerRemoved(TechnoClass* pOwner);
	static bool HandleTargetClick(ObjectClass* pTarget, const CellStruct& cell);
	static void PollTargeting();
	static void OnDamaged(TechnoClass* pOwner);

	bool Load(PhobosStreamReader& Stm, bool registerForChange);
	bool Save(PhobosStreamWriter& Stm) const;

private:
	template <typename T>
	bool Serialize(T& Stm);

	bool ValidateTarget(TechnoClass* pOwner, AbstractClass* pTarget,
		const CellStruct& cell, bool selfRelease) const;

	bool ApplyWeapon(TechnoClass* pOwner, AbstractClass* pTarget,
		const CellStruct& cell, bool selfRelease);
	void ApplyAttachEffects(TechnoClass* pOwner, AbstractClass* pTarget, bool selfRelease);
	void ApplyHealRepair(TechnoClass* pOwner, AbstractClass* pTarget, bool selfRelease);

	void ConsumeUse(TechnoClass* pOwner);
};
