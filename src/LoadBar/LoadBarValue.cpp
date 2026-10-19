#include "LoadBarValue.h"

#include "LoadBarTypeClass.h"

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <New/Entity/AttachEffectClass.h>

#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	bool ToDisplayInfoType(LoadBarSource source, DisplayInfoType& out)
	{
		switch (source)
		{
		case LoadBarSource::Health: out = DisplayInfoType::Health; return true;
		case LoadBarSource::Shield: out = DisplayInfoType::Shield; return true;
		case LoadBarSource::Ammo: out = DisplayInfoType::Ammo; return true;
		case LoadBarSource::MindControl: out = DisplayInfoType::MindControl; return true;
		case LoadBarSource::Spawns: out = DisplayInfoType::Spawns; return true;
		case LoadBarSource::Passengers: out = DisplayInfoType::Passengers; return true;
		case LoadBarSource::Tiberium: out = DisplayInfoType::Tiberium; return true;
		case LoadBarSource::Experience: out = DisplayInfoType::Experience; return true;
		case LoadBarSource::Occupants: out = DisplayInfoType::Occupants; return true;
		case LoadBarSource::GattlingStage: out = DisplayInfoType::GattlingStage; return true;
		case LoadBarSource::ROF: out = DisplayInfoType::ROF; return true;
		case LoadBarSource::Reload: out = DisplayInfoType::Reload; return true;
		case LoadBarSource::SpawnTimer: out = DisplayInfoType::SpawnTimer; return true;
		case LoadBarSource::GattlingTimer: out = DisplayInfoType::GattlingTimer; return true;
		case LoadBarSource::ProduceCash: out = DisplayInfoType::ProduceCash; return true;
		case LoadBarSource::PassengerKill: out = DisplayInfoType::PassengerKill; return true;
		case LoadBarSource::AutoDeath: out = DisplayInfoType::AutoDeath; return true;
		case LoadBarSource::SuperWeapon: out = DisplayInfoType::SuperWeapon; return true;
		case LoadBarSource::IronCurtain: out = DisplayInfoType::IronCurtain; return true;
		case LoadBarSource::TemporalLife: out = DisplayInfoType::TemporalLife; return true;
		case LoadBarSource::FactoryProcess: out = DisplayInfoType::FactoryProcess; return true;
		default: return false;
		}
	}

	bool ReadyWhenNoTimer(LoadBarSource source)
	{
		return source == LoadBarSource::ROF
			|| source == LoadBarSource::Reload
			|| source == LoadBarSource::SuperWeapon
			|| source == LoadBarSource::WeaponCooldown;
	}

	bool MatchesName(const char* name, const std::vector<std::string>& list)
	{
		if (!name)
			return false;

		for (auto const& entry : list)
		{
			if (_stricmp(name, entry.c_str()) == 0)
				return true;
		}

		return false;
	}

	int LimitEntries(int count, int maxEntries, int maxOut)
	{
		int limit = std::min(count, maxOut);

		if (maxEntries > 0)
			limit = std::min(limit, maxEntries);

		return limit;
	}
}

int LoadBarValue::Get(TechnoClass* pThis, const LoadBarTypeClass& type,
	LoadBarEntry* out, int maxOut)
{
	if (!pThis || !out || maxOut <= 0 || type.Source == LoadBarSource::None)
		return 0;

	if (type.Source == LoadBarSource::WeaponCooldown)
	{
		if (!pThis->IsArmed())
			return 0;

		auto const pExt = TechnoExt::ExtMap.Find(pThis);

		if (!pExt)
			return 0;

		const auto pType = pThis->GetTechnoType();
		const int currentSlot = TechnoExt::GetCurrentDisplayWeaponSlot(pThis);
		int slots[TechnoTypeClass::MaxWeapons] {};
		int slotCount = 0;

		if (type.SourceIndex == -2)
		{
			for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
			{
				if (TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite()))
					slots[slotCount++] = slot;
			}
		}
		else
		{
			const int slot = type.SourceIndex >= 0 ? type.SourceIndex : currentSlot;

			if (slot >= 0 && slot < TechnoTypeClass::MaxWeapons
				&& TechnoTypeExt::GetWeaponType(pType, slot, pThis->Veterancy.IsElite()))
			{
				slots[slotCount++] = slot;
			}
		}

		int written = 0;

		for (int i = 0; i < slotCount && written < maxOut; ++i)
		{
			const int slot = slots[i];
			const CDTimerClass* pTimer = nullptr;

			if (pExt->NextWeaponCooldowns[slot].TimeLeft > 0)
				pTimer = &pExt->NextWeaponCooldowns[slot];
			else if (slot == currentSlot)
				pTimer = &pThis->RearmTimer;

			const int total = pTimer ? pTimer->TimeLeft : 0;
			const int value = pTimer ? pTimer->GetTimeLeft() : 0;
			const double remaining = total > 0
				? std::clamp(static_cast<double>(value) / static_cast<double>(total), 0.0, 1.0)
				: 0.0;

			out[written++] = LoadBarEntry { remaining, value, total, nullptr, slot };
		}

		return written;
	}

	if (type.Source == LoadBarSource::AttachEffect)
	{
		auto const pExt = TechnoExt::ExtMap.Find(pThis);

		if (!pExt)
			return 0;

		std::vector<LoadBarEntry> entries;
		entries.reserve(pExt->AttachedEffects.size());

		int index = 0;

		for (auto const& pAE : pExt->AttachedEffects)
		{
			++index;

			if (!pAE || !pAE->IsActive())
				continue;

			auto const pAEtype = pAE->GetType();

			if (!pAEtype)
				continue;

			const char* name = pAEtype->Name;

			if (!type.AETypes.empty() && !MatchesName(name, type.AETypes))
				continue;

			if (!type.AEGroups.empty())
			{
				bool matched = false;

				for (auto const& group : type.AEGroups)
				{
					if (pAEtype->HasGroup(group))
					{
						matched = true;
						break;
					}
				}

				if (!matched)
					continue;
			}

			const int total = GetAETotal(pAE.get(), pThis);
			const int remaining = std::max(0, pAE->GetRemainingDuration());

			if (total <= 0 && type.HideWhenIndefinite)
				continue;

			const double fraction = total > 0
				? std::clamp(static_cast<double>(remaining) / static_cast<double>(total), 0.0, 1.0)
				: 1.0;

			entries.push_back(LoadBarEntry { fraction, remaining, total, name, index });
		}

		if (entries.empty())
			return 0;

		if (type.EntrySort == LoadBarEntrySort::Remaining)
		{
			std::stable_sort(entries.begin(), entries.end(),
				[](const LoadBarEntry& a, const LoadBarEntry& b) { return a.Remaining < b.Remaining; });
		}
		else if (type.EntrySort == LoadBarEntrySort::Total)
		{
			std::stable_sort(entries.begin(), entries.end(),
				[](const LoadBarEntry& a, const LoadBarEntry& b) { return a.Total < b.Total; });
		}
		else
		{
			std::stable_sort(entries.begin(), entries.end(),
				[](const LoadBarEntry& a, const LoadBarEntry& b)
				{
					return a.Label && b.Label ? _stricmp(a.Label, b.Label) < 0 : a.Key < b.Key;
				});
		}

		if (type.SourceIndex > static_cast<int>(entries.size()))
			return 0;

		const int start = type.SourceIndex > 0 ? type.SourceIndex - 1 : 0;
		const int available = static_cast<int>(entries.size()) - start;
		const int count = LimitEntries(available, type.MaxEntries, maxOut);

		for (int i = 0; i < count; ++i)
			out[i] = entries[start + i];

		return count;
	}

	DisplayInfoType infoType {};

	if (!ToDisplayInfoType(type.Source, infoType))
		return 0;

	int value = -1;
	int total = 0;

	TechnoExt::GetValuesForDisplay(pThis, infoType, value, total, type.SourceIndex);

	if (value < 0)
		return 0;

	if (total <= 0)
	{
		if (!ReadyWhenNoTimer(type.Source))
			return 0;

		out[0] = LoadBarEntry { 0.0, value, total, nullptr, 0 };
		return 1;
	}

	const double fraction = std::clamp(static_cast<double>(value) / static_cast<double>(total), 0.0, 1.0);
	out[0] = LoadBarEntry { fraction, value, total, nullptr, 0 };

	return 1;
}

int LoadBarValue::GetAETotal(AttachEffectClass* pAE, TechnoClass* pTechno)
{
	if (!pAE || !pTechno)
		return 0;

	auto const pType = pAE->GetType();

	if (!pType)
		return 0;

	const int durationOverride = pAE->GetDurationOverride();
	int total = durationOverride != 0 ? durationOverride : pType->Duration.Get();

	if (pType->Duration_ApplyFirepowerMult.Get() && total > 0 && pAE->GetInvoker())
	{
		const double multiplier = TechnoExt::GetCurrentFirepowerMultiplier(pAE->GetInvoker());
		total = std::max(static_cast<int>(total * multiplier), 0);
	}

	if (pType->Duration_ApplyArmorMultOnTarget.Get() && total > 0)
	{
		auto const pExt = TechnoExt::ExtMap.Find(pTechno);
		const double armor = pTechno->ArmorMultiplier * (pExt ? pExt->AE.ArmorMultiplier : 1.0);

		if (armor > 0.0)
			total = std::max(static_cast<int>(total / armor), 0);
	}

	return total;
}
