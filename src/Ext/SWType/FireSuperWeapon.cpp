#include "Body.h"

#include <SuperClass.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <Unsorted.h>
#include <MessageListClass.h>

#include <Utilities/EnumFunctions.h>
#include <Utilities/GeneralUtils.h>

#include "Ext/House/Body.h"
#include "Ext/WarheadType/Body.h"
#include "Ext/WeaponType/Body.h"
#include <Ext/Scenario/Body.h>

// ============= New SuperWeapon Effects================

void SWTypeExt::FireSuperWeaponExt(SuperClass* pSW, const CellStruct& cell)
{
	const auto pHouse = pSW->Owner;
	const auto pType = pSW->Type;
	auto const pTypeExt = SWTypeExt::ExtMap.Find(pType);

	auto& sw_ext = HouseExt::ExtMap.Find(pHouse)->SuperExts[pType->ArrayIndex];

	// --- Charge system: handle AtOnce ---
	// When AtOnce is active and we have multiple charges, fire all at once,
	// but only as many as the player can afford (per-charge money cost).
	// The Charge_InAtOnce flag prevents recursive re-entry.
	bool doAtOnce = false;
	int atOnceRemaining = 0;
	int atOnceFired = 0;

	if (pTypeExt->IsChargeSystemActive() && !sw_ext.Charge_InAtOnce)
	{
		if (pTypeExt->Charge_AtOnce && sw_ext.Charge_CurrentReady > 1)
		{
			doAtOnce = true;

			// Number of charges this AtOnce firing will consume, capped by
			// affordability (money and BattlePoints) so the limits are
			// respected instead of bypassed.
			int firesToFire = sw_ext.Charge_CurrentReady;
			const int perFireCost = pTypeExt->Money_Amount.Get();
			if (perFireCost < 0)
			{
				const int affordable = static_cast<int>(pHouse->Available_Money() / (-perFireCost));
				if (affordable < firesToFire)
					firesToFire = affordable;
			}
			const auto pHouseExt = HouseExt::ExtMap.Find(pHouse);
			const int bpCost = pTypeExt->BattlePoints_Amount.Get();
			if (bpCost < 0 && pHouseExt)
			{
				const int affordable = pHouseExt->BattlePoints / (-bpCost);
				if (affordable < firesToFire)
					firesToFire = affordable;
			}
			if (firesToFire <= 0)
				firesToFire = 1; // this call is the first fire

			atOnceFired = firesToFire;
			atOnceRemaining = firesToFire - 1; // this call is the first fire
			sw_ext.Charge_InAtOnce = true;

			if (atOnceFired < sw_ext.Charge_CurrentReady)
			{
				Debug::Log("[Phobos::Charge::AtOnce] SW[%s] fired %d of %d ready charges (affordability limit)\n",
					pType->get_ID(), atOnceFired, sw_ext.Charge_CurrentReady);
			}
		}
	}

	// === Apply SW effects for this fire ===
	if (pTypeExt->LimboDelivery_Types.size() > 0)
	{
		const int delay = pTypeExt->LimboDelivery_Delay.Get(0);

		if (delay > 0)
			LimboDeliveryTemp::Pending.push_back({ Unsorted::CurrentFrame + delay, pHouse, pType->ArrayIndex });
		else
			pTypeExt->ApplyLimboDelivery(pHouse);
	}

	if (pTypeExt->LimboKill_IDs.size() > 0)
		pTypeExt->ApplyLimboKill(pHouse);

	if (pTypeExt->Detonate_Warhead || pTypeExt->Detonate_Weapon)
		pTypeExt->ApplyDetonation(pHouse, cell);

	if (pTypeExt->SW_Next.size() > 0)
		pTypeExt->ApplySWNext(pSW, cell);

	if (pTypeExt->Convert_Pairs.size() > 0)
		pTypeExt->ApplyTypeConversion(pSW);

	if (pTypeExt->SW_Link.size() > 0)
		pTypeExt->ApplyLinkedSW(pSW);

	if (static_cast<int>(pType->Type) == 28 && !pTypeExt->EMPulse_TargetSelf)
		pTypeExt->HandleEMPulseLaunch(pSW, cell);

	if (pTypeExt->BattlePoints_Amount != 0)
		pTypeExt->ApplyBattlePoints(pSW);

	sw_ext.ShotCount++;

	// === Tag handling ===
	const auto pTags = &pHouse->RelatedTags;
	if (pTags->Count > 0)
	{
		int index = 0;
		int TagCount = pTags->Count;

		while (TagCount > 0 && index < TagCount)
		{
			const auto pTag = pTags->GetItem(index);

			if (pTag->RaiseEvent(static_cast<TriggerEvent>(77), nullptr, CellStruct::Empty, false, (TechnoClass*)(pSW))
				|| pTag->RaiseEvent(static_cast<TriggerEvent>(75), nullptr, CellStruct::Empty, false, (TechnoClass*)(pSW)))
			{
				if (TagCount != pTags->Count)
				{
					TagCount = pTags->Count;
					continue;
				}
			}

			++index;
		}
	}

	// === Charge system: AtOnce additional fires ===
	if (doAtOnce)
	{
		for (int i = 0; i < atOnceRemaining; i++)
		{
			// SetReadiness(true) so the game's internal Launch checks pass
			pSW->SetReadiness(true);
			// Launch triggers the SW again. Inside Launch, FireSuperWeaponExt
			// will be called recursively, but Charge_InAtOnce is true so it
			// won't re-enter the AtOnce block.
			pSW->Launch(cell, pHouse->IsCurrentPlayer());
		}

		sw_ext.Charge_InAtOnce = false;
		// Consume only the fired charges; keep the unaffordable remainder ready.
		sw_ext.Charge_CurrentReady -= atOnceFired;
		if (sw_ext.Charge_CurrentReady < 0)
			sw_ext.Charge_CurrentReady = 0;
		// Update IsReady immediately (Reset() will also set it false, but
		// this ensures correctness if Reset() is not called for any reason)
		pSW->IsReady = (sw_ext.Charge_CurrentReady > 0);

		// Restart charging for the consumed slots
		const int rechargeTime = pSW->GetRechargeTime();
		const int currentFrame = Unsorted::CurrentFrame;

		if (pTypeExt->Charge_RechargeAll)
		{
			// Parallel mode: restart the consumed ready slots (the ones that fired)
			if (static_cast<int>(sw_ext.Charge_RechargeSlots.size()) < pTypeExt->GetMaxCharge(pHouse))
				sw_ext.Charge_RechargeSlots.resize(pTypeExt->GetMaxCharge(pHouse), currentFrame);
			int restarted = 0;
			for (size_t i = 0; i < sw_ext.Charge_RechargeSlots.size() && restarted < atOnceFired; i++)
			{
				if (sw_ext.Charge_RechargeSlots[i] == -1)
				{
					sw_ext.Charge_RechargeSlots[i] = currentFrame;
					restarted++;
				}
			}

			// Sync the cooldown display to the fastest charging slot (smallest
			// start frame = least remaining time).
			int fastestStart = 0x7FFFFFFF;
			for (size_t i = 0; i < sw_ext.Charge_RechargeSlots.size(); i++)
			{
				if (sw_ext.Charge_RechargeSlots[i] >= 0
					&& sw_ext.Charge_RechargeSlots[i] < fastestStart)
				{
					fastestStart = sw_ext.Charge_RechargeSlots[i];
				}
			}
			if (fastestStart != 0x7FFFFFFF)
			{
				pSW->RechargeTimer.StartTime = fastestStart;
				pSW->RechargeTimer.TimeLeft = rechargeTime;
			}
		}
		else
		{
			// Sequential mode: keep charging an already-charging slot instead
			// of restarting it from zero, so an in-progress charge is never
			// wasted when the superweapon fires.
			if (sw_ext.Charge_RechargeSlots.empty())
				sw_ext.Charge_RechargeSlots.assign(1, currentFrame);
			else if (sw_ext.Charge_RechargeSlots[0] < 0)
				sw_ext.Charge_RechargeSlots[0] = currentFrame;

			if (sw_ext.Charge_RechargeSlots[0] >= 0)
			{
				pSW->RechargeTimer.StartTime = sw_ext.Charge_RechargeSlots[0];
				pSW->RechargeTimer.TimeLeft = rechargeTime;
			}
		}
	}
	else if (pTypeExt->IsChargeSystemActive() && !sw_ext.Charge_InAtOnce)
	{
		// Normal (non-AtOnce) fire: consume one charge
		if (sw_ext.Charge_CurrentReady > 0)
			sw_ext.Charge_CurrentReady--;

		// Restart charging for the consumed slot
		const int rechargeTime = pSW->GetRechargeTime();
		const int currentFrame = Unsorted::CurrentFrame;

		if (pTypeExt->Charge_RechargeAll)
		{
			// Parallel mode: restart one slot's timer
			// Find the first ready slot (-1) and restart it
			for (size_t i = 0; i < sw_ext.Charge_RechargeSlots.size(); i++)
			{
				if (sw_ext.Charge_RechargeSlots[i] == -1)
				{
					sw_ext.Charge_RechargeSlots[i] = currentFrame;
					break;
				}
			}

			// Sync the cooldown display to the fastest charging slot (smallest
			// start frame = least remaining time).
			int fastestStart = 0x7FFFFFFF;
			for (size_t i = 0; i < sw_ext.Charge_RechargeSlots.size(); i++)
			{
				if (sw_ext.Charge_RechargeSlots[i] >= 0
					&& sw_ext.Charge_RechargeSlots[i] < fastestStart)
				{
					fastestStart = sw_ext.Charge_RechargeSlots[i];
				}
			}
			if (fastestStart != 0x7FFFFFFF)
			{
				pSW->RechargeTimer.StartTime = fastestStart;
				pSW->RechargeTimer.TimeLeft = rechargeTime;
			}
		}
		else
		{
			// Sequential mode: keep charging an already-charging slot instead
			// of restarting it from zero, so an in-progress charge is never
			// wasted when the superweapon fires.
			if (sw_ext.Charge_RechargeSlots.empty())
				sw_ext.Charge_RechargeSlots.assign(1, currentFrame);
			else if (sw_ext.Charge_RechargeSlots[0] < 0)
				sw_ext.Charge_RechargeSlots[0] = currentFrame;

			if (sw_ext.Charge_RechargeSlots[0] >= 0)
			{
				pSW->RechargeTimer.StartTime = sw_ext.Charge_RechargeSlots[0];
				pSW->RechargeTimer.TimeLeft = rechargeTime;
			}
		}
	}
}

// ====================================================

#pragma region LimboDelivery
inline void LimboCreate(BuildingTypeClass* pType, HouseClass* pOwner, int ID)
{
	// BuildLimit check goes before creation
	if (pType->BuildLimit > 0)
	{
		int sum = pOwner->CountOwnedNow(pType);

		// copy Ares' deployable units x build limit fix
		if (auto const pUndeploy = pType->UndeploysInto)
			sum += pOwner->CountOwnedNow(pUndeploy);

		if (sum >= pType->BuildLimit)
			return;
	}

	if (auto const pBuilding = static_cast<BuildingClass*>(pType->CreateObject(pOwner)))
	{
		// All of these are mandatory
		pBuilding->InLimbo = false;
		pBuilding->IsAlive = true;
		pBuilding->IsOnMap = true;

		// Jun 3, 2023 - Starkku: For reasons beyond my comprehension, the discovery logic is checked for certain logics like power drain/output in campaign only.
		// Normally on unlimbo the buildings are revealed to current player if unshrouded or if game is a campaign and to non-player houses always.
		// Because of the unique nature of LimboDelivered buildings, this has been adjusted to always reveal to the current player in singleplayer
		// and to the owner of the building regardless, removing the shroud check from the equation since they don't physically exist
		if (SessionClass::IsCampaign())
			pBuilding->DiscoveredBy(HouseClass::CurrentPlayer);

		pBuilding->DiscoveredBy(pOwner);

		pOwner->RegisterGain(pBuilding, false);
		pOwner->RecheckTechTree = true;
		pOwner->RecheckPower = true;
		pOwner->Buildings.AddItem(pBuilding);

		// Different types of building logics
		if (pType->ConstructionYard)
			pOwner->ConYards.AddItem(pBuilding); // why would you do that????

		if (pType->SecretLab)
			pOwner->SecretLabs.AddItem(pBuilding);

		auto const pBuildingExt = BuildingExt::ExtMap.Find(pBuilding);
		auto const pOwnerExt = HouseExt::ExtMap.Find(pOwner);

		if (pType->FactoryPlant)
		{
			if (pBuildingExt->TypeExtData->FactoryPlant_AllowTypes.size() > 0 || pBuildingExt->TypeExtData->FactoryPlant_DisallowTypes.size() > 0)
			{
				pOwnerExt->RestrictedFactoryPlants.push_back(pBuilding);
			}
			else
			{
				pOwner->FactoryPlants.AddItem(pBuilding);
				pOwner->CalculateCostMultipliers();
			}
		}

		// BuildingClass::Place is already called in DiscoveredBy
		// it added OrePurifier and xxxGainSelfHeal to House counter already

		// LimboKill ID
		pBuildingExt->LimboID = ID;

		// Add building to list of owned limbo buildings
		pOwnerExt->OwnedLimboDeliveredBuildings.push_back(pBuilding);
		auto const pBldType = pBuilding->Type;

		if (!pBldType->Insignificant && !pBldType->DontScore)
			pOwnerExt->AddToLimboTracking(pBldType);

		auto const pTechnoExt = TechnoExt::ExtMap.Find(pBuilding);
		auto const pTechnoTypeExt = pTechnoExt->TypeExtData;

		if (pTechnoTypeExt->AutoDeath_Behavior.isset())
		{
			ScenarioExt::Global()->AutoDeathObjects.push_back(pTechnoExt);

			if (pTechnoTypeExt->AutoDeath_AfterDelay > 0)
				pTechnoExt->AutoDeathTimer.Start(pTechnoTypeExt->AutoDeath_AfterDelay);
		}

	}
}

void SWTypeExt::ExtData::ApplyLimboDelivery(HouseClass* pHouse)
{
	// random mode
	if (this->LimboDelivery_RandomWeightsData.size())
	{
		int id = -1;
		const size_t idsSize = this->LimboDelivery_IDs.size();
		const auto results = this->WeightedRollsHandler(&this->LimboDelivery_RollChances, &this->LimboDelivery_RandomWeightsData, this->LimboDelivery_Types.size());
		for (size_t result : results)
		{
			if (result < idsSize)
				id = this->LimboDelivery_IDs[result];

			LimboCreate(this->LimboDelivery_Types[result], pHouse, id);
		}
	}
	// no randomness mode
	else
	{
		int id = -1;
		const size_t idsSize = this->LimboDelivery_IDs.size();

		for (size_t i = 0; i < this->LimboDelivery_Types.size(); i++)
		{
			if (i < idsSize)
				id = this->LimboDelivery_IDs[i];

			LimboCreate(this->LimboDelivery_Types[i], pHouse, id);
		}
	}
}

void SWTypeExt::ExtData::ApplyLimboKill(HouseClass* pHouse)
{
	for (int limboKillID : this->LimboKill_IDs)
	{
		for (HouseClass* pTargetHouse : HouseClass::Array)
		{
			if (EnumFunctions::CanTargetHouse(this->LimboKill_Affected, pHouse, pTargetHouse))
			{
				auto const pHouseExt = HouseExt::ExtMap.Find(pTargetHouse);
				auto& vec = pHouseExt->OwnedLimboDeliveredBuildings;

				for (auto it = vec.begin(); it != vec.end(); )
				{
					BuildingClass* const pBuilding = *it;
					auto const pBuildingExt = BuildingExt::ExtMap.Find(pBuilding);

					if (pBuildingExt->LimboID == limboKillID)
					{
						it = vec.erase(it);
						auto const pBldType = pBuilding->Type;

						// Remove limbo buildings' tracking here because their are not truely InLimbo
						if (!pBldType->Insignificant && !pBldType->DontScore)
							HouseExt::ExtMap.Find(pBuilding->Owner)->RemoveFromLimboTracking(pBldType);

						pBuilding->Stun();
						pBuilding->Limbo();
						pBuilding->RegisterDestruction(nullptr);
						pBuilding->UnInit();
					}
					else
					{
						++it;
					}
				}
			}
		}
	}
}

#pragma endregion

void SWTypeExt::ExtData::ApplyDetonation(HouseClass* pHouse, const CellStruct& cell)
{
	auto coords = MapClass::Instance.GetCellAt(cell)->GetCoords();
	BuildingClass* pFirer = nullptr;

	for (auto const& pBld : pHouse->Buildings)
	{
		if (this->IsLaunchSiteEligible(cell, pBld, false))
		{
			pFirer = pBld;
			break;
		}
	}

	if (this->Detonate_AtFirer)
		coords = pFirer ? pFirer->GetCenterCoords() : CoordStruct::Empty;

	const auto pWeapon = this->Detonate_Weapon;
	auto const mapCoords = CellClass::Coord2Cell(coords);

	if (!MapClass::Instance.CoordinatesLegal(mapCoords))
	{
		auto const ID = pWeapon ? pWeapon->get_ID() : this->Detonate_Warhead->get_ID();
		Debug::Log("ApplyDetonation: Superweapon [%s] failed to detonate [%s] - cell at %d, %d is invalid.\n", this->OwnerObject()->get_ID(), ID, mapCoords.X, mapCoords.Y);
		return;
	}

	if (pWeapon)
		WeaponTypeExt::DetonateAt(pWeapon, coords, pFirer, this->Detonate_Damage.Get(pWeapon->Damage), pHouse);
	else
	{
		if (this->Detonate_Warhead_Full)
			WarheadTypeExt::DetonateAt(this->Detonate_Warhead, coords, pFirer, this->Detonate_Damage.Get(0), pHouse);
		else
			MapClass::DamageArea(coords, this->Detonate_Damage.Get(0), pFirer, this->Detonate_Warhead, true, pHouse);
	}
}

void SWTypeExt::ExtData::ApplySWNext(SuperClass* pSW, const CellStruct& cell)
{
	// SW.Next proper launching mechanic
	auto LaunchTheSW = [=](const int swIdxToLaunch)
		{
			const auto pHouse = pSW->Owner;
			if (const auto pSuper = pHouse->Supers.GetItem(swIdxToLaunch))
			{
				const auto pNextTypeExt = SWTypeExt::ExtMap.Find(pSuper->Type);
				if (!this->SW_Next_RealLaunch
					|| (pSuper->IsPresent && pSuper->IsReady && !pSuper->IsSuspended && pHouse->CanTransactMoney(pNextTypeExt->Money_Amount)))
				{
					if ((this->SW_Next_IgnoreInhibitors || !pNextTypeExt->HasInhibitor(pHouse, cell))
						&& (this->SW_Next_IgnoreDesignators || pNextTypeExt->HasDesignator(pHouse, cell)))
					{
						const int oldstart = pSuper->RechargeTimer.StartTime;
						const int oldleft = pSuper->RechargeTimer.TimeLeft;
						pSuper->SetReadiness(true);
						pSuper->Launch(cell, pHouse->IsCurrentPlayer());
						pSuper->Reset();
						if (!this->SW_Next_RealLaunch)
						{
							pSuper->RechargeTimer.StartTime = oldstart;
							pSuper->RechargeTimer.TimeLeft = oldleft;
						}
					}
				}
			}
		};

	// random mode
	if (this->SW_Next_RandomWeightsData.size())
	{
		const auto results = this->WeightedRollsHandler(&this->SW_Next_RollChances, &this->SW_Next_RandomWeightsData, this->SW_Next.size());
		for (const int result : results)
			LaunchTheSW(this->SW_Next[result]);
	}
	// no randomness mode
	else
	{
		for (const auto swType : this->SW_Next)
			LaunchTheSW(swType);
	}
}

void SWTypeExt::ExtData::ApplyTypeConversion(SuperClass* pSW)
{
	for (const auto pTargetFoot : FootClass::Array)
		TypeConvertGroup::Convert(pTargetFoot, this->Convert_Pairs, pSW->Owner);
}

void SWTypeExt::ExtData::HandleEMPulseLaunch(SuperClass* pSW, const CellStruct& cell) const
{
	auto const& pBuildings = this->GetEMPulseCannons(pSW->Owner, cell);
	auto const count = this->SW_MaxCount >= 0 ? static_cast<size_t>(this->SW_MaxCount) : std::numeric_limits<size_t>::max();

	for (size_t i = 0; i < pBuildings.size(); i++)
	{
		auto const pBuilding = pBuildings[i];
		auto const pExt = BuildingExt::ExtMap.Find(pBuilding);
		pExt->CurrentEMPulseSW = pSW;

		if (i + 1 == count)
			break;
	}

	if (this->EMPulse_SuspendOthers)
	{
		auto const pHouse = pSW->Owner;
		auto const pHouseExt = HouseExt::ExtMap.Find(pHouse);

		for (auto const& pSuper : pHouse->Supers)
		{
			if (static_cast<int>(pSuper->Type->Type) != 28 || pSuper == pSW)
				continue;

			auto const pTypeExt = SWTypeExt::ExtMap.Find(pSuper->Type);
			bool suspend = false;

			if (this->EMPulse_Cannons.empty() && pTypeExt->EMPulse_Cannons.empty())
			{
				suspend = true;
			}
			else
			{
				// Suspend if the two cannon lists share common items.
				suspend = std::find_first_of(this->EMPulse_Cannons.begin(), this->EMPulse_Cannons.end(),
					pTypeExt->EMPulse_Cannons.begin(), pTypeExt->EMPulse_Cannons.end()) != this->EMPulse_Cannons.end();
			}

			if (suspend)
			{
				pSuper->IsSuspended = true;
				const int arrayIndex = pSW->Type->ArrayIndex;

				if (pHouseExt->SuspendedEMPulseSWs.count(arrayIndex))
					pHouseExt->SuspendedEMPulseSWs[arrayIndex].push_back(arrayIndex);
				else
					pHouseExt->SuspendedEMPulseSWs.insert({ arrayIndex, std::vector<int>{pSuper->Type->ArrayIndex} });
			}
		}
	}
}

void SWTypeExt::ExtData::ApplyLinkedSW(SuperClass* pSW)
{
	const auto pHouse = pSW->Owner;
	const bool notObserver = !pHouse->IsObserver() || !pHouse->IsCurrentPlayerObserver();

	if (pHouse->Defeated || !notObserver)
		return;

	auto linkedSW = [=](const int swIdxToAdd)
	{
		if (const auto pSuper = pHouse->Supers.GetItem(swIdxToAdd))
		{
			const bool granted = this->SW_Link_Grant && !pSuper->IsPresent && pSuper->Grant(true, false, false);
			bool isActive = granted;

			if (pSuper->IsPresent)
			{
				// check SW.Link.Reset first
				if (this->SW_Link_Reset)
				{
					pSuper->Reset();
					isActive = true;
				}
				// check SW.Link.Ready, which will default to SW.InitialReady for granted superweapon
				else if (this->SW_Link_Ready || (granted && SWTypeExt::ExtMap.Find(pSuper->Type)->SW_InitialReady))
				{
					pSuper->RechargeTimer.TimeLeft = 0;
					pSuper->SetReadiness(true);
					isActive = true;
				}
				// reset granted superweapon if it doesn't meet above conditions
				else if (granted)
				{
					pSuper->Reset();
				}
			}

			if (granted && notObserver && pHouse->IsCurrentPlayer())
			{
				if (MouseClass::Instance.AddCameo(AbstractType::Special, swIdxToAdd))
					MouseClass::Instance.RepaintSidebar(1);
			}

			return isActive;
		}

		return false;
	};

	bool isActive = false;

	// random mode
	if (this->SW_Link_RandomWeightsData.size())
	{
		const auto results = this->WeightedRollsHandler(&this->SW_Link_RollChances, &this->SW_Link_RandomWeightsData, this->SW_Link.size());

		for (const int result : results)
		{
			if (linkedSW(this->SW_Link[result]))
				isActive = true;
		}
	}

	// no randomness mode
	else
	{
		for (const auto swType : this->SW_Link)
		{
			if (linkedSW(swType))
				isActive = true;
		}
	}

	if (isActive && notObserver && pHouse->IsCurrentPlayer())
	{
		if (this->EVA_LinkedSWAcquired.isset())
			VoxClass::PlayIndex(this->EVA_LinkedSWAcquired.Get(), -1, -1);

		MessageListClass::Instance.PrintMessage(this->Message_LinkedSWAcquired.Get(), RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer->ColorSchemeIndex, true);
	}
}

void SWTypeExt::ExtData::ApplyBattlePoints(SuperClass* pSW)
{
	auto pOwnerExt = HouseExt::ExtMap.Find(pSW->Owner);
	pOwnerExt->UpdateBattlePoints(this->BattlePoints_Amount);
}
