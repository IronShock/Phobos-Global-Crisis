#include "Body.h"

#include <SuperClass.h>
#include <StringTable.h>
#include <Unsorted.h>
#include <HouseClass.h>
#include <Surface.h>
#include <Drawing.h>

#include <Ext/House/Body.h>

#include <Misc/TextScale.h>

//this hook just for phobos NewSWType
DEFINE_HOOK(0x6CC390, SuperClass_Launch, 0x6)
{
	GET(SuperClass* const, pSuper, ECX);
	GET_STACK(CellStruct const* const, pCell, 0x4);
	GET_STACK(bool const, isPlayer, 0x8);

	Debug::Log("[Phobos Launch] %s\n", pSuper->Type->get_ID());

	auto const handled = SWTypeExt::Activate(pSuper, *pCell, isPlayer);

	return handled ? 0x6CDE40 : 0;
}

// Ares hooked at 0x6CC390 and jumped to 0x6CDE40
// If a super is not handled by Ares however, we do it at the original entry point
DEFINE_HOOK_AGAIN(0x6CC390, SuperClass_Place_FireExt, 0x6)
DEFINE_HOOK(0x6CDE40, SuperClass_Place_FireExt, 0x5)
{
	GET(SuperClass* const, pSuper, ECX);
	GET_STACK(CellStruct const* const, pCell, 0x4);
	// GET_STACK(bool const, isPlayer, 0x8);

	// Check if the SuperClass pointer is valid and not corrupted.
	if (pSuper && VTable::Get(pSuper) == SuperClass::AbsVTable)
		SWTypeExt::FireSuperWeaponExt(pSuper, *pCell);
	else
		Debug::Log(__FUNCTION__": Hook entered with an invalid or corrupt SuperClass pointer.");

	return 0;
}

DEFINE_HOOK(0x6CB5EB, SuperClass_Grant_ShowTimer, 0x5)
{
	GET(SuperClass*, pThis, ESI);

	if (SuperClass::ShowTimers.AddItem(pThis))
	{
		std::sort(SuperClass::ShowTimers.begin(), SuperClass::ShowTimers.end(),
			[](SuperClass* a, SuperClass* b)
			{
				const auto aExt = SWTypeExt::ExtMap.Find(a->Type);
				const auto bExt = SWTypeExt::ExtMap.Find(b->Type);
				return aExt->ShowTimer_Priority.Get() > bExt->ShowTimer_Priority.Get();
			}
		);
	}

	return 0x6CB63E;
}

DEFINE_HOOK(0x6DBE74, Tactical_SuperLinesCircles_ShowDesignatorRange, 0x7)
{
	if (!Phobos::Config::ShowDesignatorRange || !(RulesExt::Global()->ShowDesignatorRange) || Unsorted::CurrentSWType == -1)
		return 0;

	const auto pSuperType = SuperWeaponTypeClass::Array.GetItem(Unsorted::CurrentSWType);
	const auto pExt = SWTypeExt::ExtMap.Find(pSuperType);

	if (!pExt->ShowDesignatorRange)
		return 0;

	for (const auto pCurrentTechno : TechnoClass::Array)
	{
		const auto pCurrentTechnoType = pCurrentTechno->GetTechnoType();
		const auto pOwner = pCurrentTechno->Owner;

		if (!pCurrentTechno->IsAlive
			|| pCurrentTechno->InLimbo
			|| (pOwner != HouseClass::CurrentPlayer && pOwner->IsAlliedWith(HouseClass::CurrentPlayer))                  // Ally objects are never designators or inhibitors
			|| (pOwner == HouseClass::CurrentPlayer && !pExt->SW_Designators.Contains(pCurrentTechnoType))               // Only owned objects can be designators
			|| (!pOwner->IsAlliedWith(HouseClass::CurrentPlayer) && !pExt->SW_Inhibitors.Contains(pCurrentTechnoType)))  // Only enemy objects can be inhibitors
		{
			continue;
		}

		const auto pTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pCurrentTechnoType);

		const float radius = pOwner == HouseClass::CurrentPlayer
			? (float)(pTechnoTypeExt->DesignatorRange.Get(pCurrentTechnoType->Sight))
			: (float)(pTechnoTypeExt->InhibitorRange.Get(pCurrentTechnoType->Sight));

		CoordStruct coords = pCurrentTechno->GetCenterCoords();
		coords.Z = MapClass::Instance.GetCellFloorHeight(coords);
		const auto color = pOwner->Color;
		Game::DrawRadialIndicator(false, true, coords, color, radius, false, true);
	}

	return 0;
}

DEFINE_HOOK(0x6CBEF4, SuperClass_AnimStage_UseWeeds, 0x6)
{
	enum
	{
		Ready = 0x6CBFEC,
		NotReady = 0x6CC064,
		ProgressInEax = 0x6CC066
	};

	constexpr int maxCounterFrames = 54;

	GET(SuperClass*, pSuper, ECX);
	GET(SuperWeaponTypeClass*, pSWType, EBX);

	const auto pExt = SWTypeExt::ExtMap.Find(pSWType);

	if (pExt->UseWeeds)
	{
		if (pSuper->IsReady)
			return Ready;

		if (pExt->UseWeeds_StorageTimer)
		{
			int progress = static_cast<int>(pSuper->Owner->OwnedWeed.GetTotalAmount() * maxCounterFrames / pExt->UseWeeds_Amount);
			if (progress > maxCounterFrames)
				progress = maxCounterFrames;

			R->EAX(progress);
			return ProgressInEax;
		}
		else
		{
			return NotReady;
		}
	}

	return 0;
}

// This hook intercepts the "je NothingChanged" at 0x6CBD1B which fires when
// IsReady=true AND UseChargeDrain=false. For charge system SWs that are ready,
// we must NOT skip the charge update, so we bypass this jump and fall through
// to the IsSuspended check and our main charge hook at 0x6CBD2C.
DEFINE_HOOK(0x6CBD1B, SuperClass_AI_SkipReadyForCharge, 0x6)
{
	GET(SuperClass*, pSuper, ESI);

	const auto pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

	if (pExt->IsChargeSystemActive() && pSuper->IsReady)
	{
		return 0x6CBD21;
	}

	// Original behavior: if UseChargeDrain is false, jump to NothingChanged
	if (!pSuper->Type->UseChargeDrain)
		return 0x6CBE9D;  // NothingChanged

	return 0x6CBD21;  // Continue to IsSuspended check
}

DEFINE_HOOK(0x6CBD2C, SuperClass_AI_ChargeSystem, 0x6)
{
	enum
	{
		NothingChanged = 0x6CBE9D,
		SomethingChanged = 0x6CBD48,
		Charged = 0x6CBD73
	};

	enum
	{
		SWReadyTimer = 0,
		SWAlmostReadyTimer = 15,
		SWNotReadyTimer = 915
	};

	GET(SuperClass*, pSuper, ESI);

	const auto pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

	// UseWeeds takes priority
	if (pExt->UseWeeds)
	{
		pSuper->Type->ShowTimer = false;
		const float totalAmount = pSuper->Owner->OwnedWeed.GetTotalAmount();
		const int weedAmount = pExt->UseWeeds_Amount;

		if (totalAmount >= weedAmount)
		{
			pSuper->Owner->OwnedWeed.RemoveAmount(static_cast<float>(weedAmount), 0);
			pSuper->RechargeTimer.Start(SWReadyTimer); // The Armageddon is here
			return Charged;
		}

		if (totalAmount >= pExt->UseWeeds_ReadinessAnimationPercentage * weedAmount)
		{
			pSuper->RechargeTimer.Start(SWAlmostReadyTimer); // The end is nigh!
		}
		else
		{
			pSuper->RechargeTimer.Start(SWNotReadyTimer); // 61 seconds > 60 seconds (animation activation threshold)
		}

		const int animStage = pSuper->AnimStage();
		if (pSuper->CameoChargeState != animStage)
		{
			pSuper->CameoChargeState = animStage;
			return SomethingChanged;
		}

		return NothingChanged;
	}

	// Charge system
	if (pExt->IsChargeSystemActive())
	{
		auto pHouseExt = HouseExt::ExtMap.Find(pSuper->Owner);
		auto& swExt = pHouseExt->SuperExts[pSuper->Type->ArrayIndex];

		// Initialize charge system on first call
		if (!swExt.Charge_Initialized)
			pExt->InitializeChargeState(pSuper);

		const int maxCharge = pExt->GetMaxCharge(pSuper->Owner);

		// Periodic debug log (every ~5 seconds at 15fps)
		static int chargeLogCounter = 0;
		if (chargeLogCounter++ % 75 == 0)
		{
			Debug::Log("[Phobos::Charge::AI] SW[%s] Ready=%d/%d RechargeAll=%d Slots=%d IsReady=%d TimerStart=%d TimerLeft=%d\n",
				pSuper->Type->get_ID(), swExt.Charge_CurrentReady, maxCharge,
				pExt->Charge_RechargeAll.Get(), swExt.Charge_RechargeSlots.size(), pSuper->IsReady,
				pSuper->RechargeTimer.StartTime, pSuper->RechargeTimer.TimeLeft);
		}
		const int rechargeTime = pSuper->GetRechargeTime();
		const int currentFrame = Unsorted::CurrentFrame;

		// Cap ready charges if max decreased (e.g. Charger units lost)
		if (swExt.Charge_CurrentReady > maxCharge)
			swExt.Charge_CurrentReady = maxCharge;

		bool becameReady = false;

		if (pExt->Charge_RechargeAll)
		{
			// === Parallel mode: each slot charges independently ===

			// Resize slots if maxCharge changed
			if (static_cast<int>(swExt.Charge_RechargeSlots.size()) != maxCharge)
			{
				int oldSize = static_cast<int>(swExt.Charge_RechargeSlots.size());
				swExt.Charge_RechargeSlots.resize(maxCharge, currentFrame); // new slots start charging
				Debug::Log("[Phobos::Charge] SW[%s] slots resized %d -> %d (new slots charging)\n",
					pSuper->Type->get_ID(), oldSize, maxCharge);
			}

			// Check each slot for completion
			for (int i = 0; i < maxCharge; i++)
			{
				if (swExt.Charge_RechargeSlots[i] >= 0)
				{
					int elapsed = currentFrame - swExt.Charge_RechargeSlots[i];
					if (elapsed >= rechargeTime)
					{
						swExt.Charge_RechargeSlots[i] = -1; // Mark as ready
						swExt.Charge_CurrentReady++;
						becameReady = true;
					}
				}
			}

			// Sync game timer for cameo animation with the FASTEST charging slot
			// (smallest start frame = least remaining time). This way the
			// displayed cooldown always shows the charge that will be ready
			// soonest, even right after firing when a consumed slot restarts.
			bool anyCharging = false;
			int fastestStart = 0x7FFFFFFF;
			for (int i = 0; i < maxCharge; i++)
			{
				if (swExt.Charge_RechargeSlots[i] >= 0
					&& swExt.Charge_RechargeSlots[i] < fastestStart)
				{
					fastestStart = swExt.Charge_RechargeSlots[i];
				}
			}
			if (fastestStart != 0x7FFFFFFF)
			{
				pSuper->RechargeTimer.StartTime = fastestStart;
				pSuper->RechargeTimer.TimeLeft = rechargeTime;
				anyCharging = true;
			}
			if (!anyCharging && swExt.Charge_CurrentReady > 0)
			{
				// All slots ready - set timer to "completed" state (not Stop).
				// Keep TimeLeft == 0 (raw) exactly like a normally recharged SW,
				// because the game's SPECIAL_PLACE firing path reads TimeLeft
				// directly and rejects the SW while it's a positive value.
				// Use the current frame as StartTime so it stays valid regardless
				// of RechargeTime (a large RechargeTime would otherwise make
				// StartTime a big negative number that the game rejects).
				pSuper->RechargeTimer.StartTime = currentFrame;
				pSuper->RechargeTimer.TimeLeft = 0;
			}
		}
		else
		{
			// === Sequential mode: charges complete one at a time ===
			// Uses frame-based tracking via Charge_RechargeSlots[0] instead of
			// relying on RechargeTimer.Completed(), which may be invalidated
			// when the game processes the Charged return value.

			if (swExt.Charge_RechargeSlots.empty())
				swExt.Charge_RechargeSlots.assign(1, -1);

			if (swExt.Charge_CurrentReady < maxCharge)
			{
				if (swExt.Charge_RechargeSlots[0] < 0)
				{
					// No slot currently charging - start one
					swExt.Charge_RechargeSlots[0] = currentFrame;
					pSuper->RechargeTimer.Start(rechargeTime);
				}
				else
				{
					int elapsed = currentFrame - swExt.Charge_RechargeSlots[0];
					if (elapsed >= rechargeTime)
					{
						swExt.Charge_CurrentReady++;
						becameReady = true;
						// Mark slot as done; next slot starts on next AI tick
						swExt.Charge_RechargeSlots[0] = -1;
					}
					else
					{
						// Still charging - sync timer for cameo animation
						pSuper->RechargeTimer.StartTime = swExt.Charge_RechargeSlots[0];
						pSuper->RechargeTimer.TimeLeft = rechargeTime;
					}
				}
			}

			// If all charges ready, set timer to completed state.
			// TimeLeft must be 0 (raw) like a normally recharged SW so the
			// game's SPECIAL_PLACE firing path accepts the SW as ready.
			// Use the current frame as StartTime so it stays valid regardless
			// of RechargeTime.
			if (swExt.Charge_CurrentReady >= maxCharge && swExt.Charge_RechargeSlots[0] < 0)
			{
				pSuper->RechargeTimer.StartTime = currentFrame;
				pSuper->RechargeTimer.TimeLeft = 0;
			}
		}

		// Keep IsReady in sync with available charges.
	pSuper->IsReady = (swExt.Charge_CurrentReady > 0);

	// Return Charged whenever any charge becomes ready
	if (becameReady)
	{
		pSuper->CameoChargeState = pSuper->AnimStage();
		return Charged;
	}

	// Update cameo if animation stage changed
		const int animStage = pSuper->AnimStage();
		if (pSuper->CameoChargeState != animStage)
		{
			pSuper->CameoChargeState = animStage;
			return SomethingChanged;
		}

		return NothingChanged;
	}

	return 0;
}

// This is pointless for SWs using weeds because their charge is tied to weed storage.
DEFINE_HOOK(0x6CC1E6, SuperClass_SetSWCharge_ChargeSystem, 0x5)
{
	enum { Skip = 0x6CC251 };

	GET(SuperClass*, pSuper, EDI);

	const auto pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

	if (pExt->UseWeeds || pExt->IsChargeSystemActive())
		return Skip;

	return 0;
}

#pragma region SW TabIndex
DEFINE_HOOK(0x6A5F6E, SidebarClass_6A5F20_TabIndex, 0x8)
{
	enum { ApplyTabIndex = 0x6A5FD3 };

	GET(AbstractType const, absType, ESI);
	GET(int const, typeIdx, EAX);

	R->EAX(SidebarClass::GetObjectTabIdx(absType, typeIdx, 0));
	return ApplyTabIndex;
}

DEFINE_HOOK(0x6A614D, SidebarClass_6A6140_TabIndex, 0x5)
{
	enum { ApplyTabIndex = 0x6A61B1 };

	GET(AbstractType const, absType, EDI);
	GET(int const, typeIdx, EBP);

	R->EAX(SidebarClass::GetObjectTabIdx(absType, typeIdx, 0));
	return ApplyTabIndex;
}

DEFINE_HOOK(0x6A633D, SidebarClass_AddCameo_TabIndex, 0x5)
{
	enum { ApplyTabIndex = 0x6A63B7 };

	GET(AbstractType const, absType, ESI);
	GET(int const, typeIdx, EBP);

	R->Stack(STACK_OFFSET(0x14, 0x4), SidebarClass::GetObjectTabIdx(absType, typeIdx, 0));
	return ApplyTabIndex;
}

DEFINE_HOOK(0x6ABC9D, SidebarClass_GetObjectTabIndex_Super, 0x5)
{
	enum { ApplyTabIndex = 0x6ABCA2 };

	GET(int const, typeIdx, EDX);

	if (typeIdx < 0 || typeIdx >= SuperWeaponTypeClass::Array.Count)
		return 0;

	const auto pSWType = SuperWeaponTypeClass::Array[typeIdx];
	const auto pSWTypExt = SWTypeExt::ExtMap.Find(pSWType);

	R->EAX(pSWTypExt->TabIndex);
	return ApplyTabIndex;
}

DEFINE_HOOK(0x6AC67A, SidebarClass_6AC5F0_TabIndex, 0x5)
{
	enum { ApplyTabIndex = 0x6AC6D9 };

	GET(AbstractType const, absType, EAX);
	GET(int const, typeIdx, ESI);

	R->EAX(SidebarClass::GetObjectTabIdx(absType, typeIdx, 0));
	return ApplyTabIndex;
}

DEFINE_JUMP(LJMP, 0x6A8D07, 0x6A8D17) // Skip tabIndex check
#pragma endregion

// Full rewrite
DEFINE_HOOK(0x6CC367, SuperClass_IsReady_BattlePoints, 0xD)
{
	GET(SuperClass*, pSuper, ECX);

	enum{ ReturnIsReady = 0x6CC37D, ReturnZero = 0x6CC381, SkipAll = 0x6CC383};

	if (pSuper->IsSuspended)
		return ReturnZero;

	if (pSuper->Type->UseChargeDrain)
	{
		R->AL(pSuper->ChargeDrainState != ChargeDrainState::Charging);
		return SkipAll;
	}

	const auto pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

	// Charge system: can fire if ready charges available
	if (pExt->IsChargeSystemActive())
	{
		pExt->InitializeChargeState(pSuper);

		auto pHouseExt = HouseExt::ExtMap.Find(pSuper->Owner);
		auto& swExt = pHouseExt->SuperExts[pSuper->Type->ArrayIndex];

		// Respect the BattlePoints limit: cannot fire if the player can't
		// afford one fire's BattlePoints cost (AtOnce may still fire partially).
		if (pExt->BattlePoints_Amount < 0
			&& pHouseExt->BattlePoints < std::abs(pExt->BattlePoints_Amount))
		{
			return ReturnZero;
		}

		const bool shouldBeReady = (swExt.Charge_CurrentReady > 0);
		if (pSuper->IsReady != shouldBeReady)
		{
			pSuper->IsReady = shouldBeReady;
			pSuper->CameoChargeState = pSuper->AnimStage();
		}

		return shouldBeReady ? ReturnIsReady : ReturnZero;
	}

	if (pExt->BattlePoints_Amount != 0)
	{
		const auto pOwnerExt = HouseExt::ExtMap.Find(pSuper->Owner);

		if (pExt->BattlePoints_Amount < 0)
		{
			if (pOwnerExt->BattlePoints < std::abs(pExt->BattlePoints_Amount))
				return ReturnZero;
		}
	}

	return ReturnIsReady;
}

// Executed before the Ares hook for launching AI super weapons, SW->IsReady property won't be updated anymore
DEFINE_HOOK(0x4FD77C, ExpertAI_SuperWeaponAI_RecheckIsReady, 0x5)
{
	GET(HouseClass*, pHouse, EBX);
	if (!SessionClass::IsCampaign() || pHouse->IQLevel2 >= RulesClass::Instance->SuperWeapons)
	{
		for (auto const& pSuper : pHouse->Supers)
		{
			if (pSuper->IsReady)
				pSuper->IsReady = pSuper->CanFire();
		}
	}

	return 0;
}


// Hook NameReadiness() to append charge count to the displayed text
// Replaces the entire function at 0x6CC2B0
// Original function logic:
//   if (IsSuspended) return LoadString("TXT_HOLD");
//   if (Type->UseChargeDrain) { switch(ChargeDrainState) { ... } }
//   if (IsReady) return LoadString("TXT_READY");
//   return nullptr;
DEFINE_HOOK(0x6CC2B0, SuperClass_NameReadiness_ChargeCount, 0x5)
{
	GET(SuperClass*, pSuper, ECX);

	// Replicate original NameReadiness() logic
	const wchar_t* originalText = nullptr;

	if (pSuper->IsSuspended)
	{
		originalText = StringTable::LoadString("TXT_HOLD");
	}
	else if (pSuper->Type && pSuper->Type->UseChargeDrain)
	{
		switch (pSuper->ChargeDrainState)
		{
		case ChargeDrainState::Charging:
			originalText = StringTable::LoadString("TXT_CHARGING");
			break;
		case ChargeDrainState::Ready:
			originalText = StringTable::LoadString("TXT_READY");
			break;
		case ChargeDrainState::Draining:
			originalText = StringTable::LoadString("TXT_FIRESTORM_ON");
			break;
		default:
			break;
		}
	}
	else if (pSuper->IsReady)
	{
		originalText = StringTable::LoadString("TXT_READY");
	}

	// If charge system is active, append charge count to the text
	const auto pSWExt = SWTypeExt::ExtMap.Find(pSuper->Type);
	if (pSWExt && pSWExt->IsChargeSystemActive())
	{
		auto pHouseExt = HouseExt::ExtMap.Find(pSuper->Owner);
		if (pHouseExt && pSuper->Type->ArrayIndex >= 0
			&& pSuper->Type->ArrayIndex < static_cast<int>(pHouseExt->SuperExts.size()))
		{
			pSWExt->InitializeChargeState(pSuper);
			auto& swExt = pHouseExt->SuperExts[pSuper->Type->ArrayIndex];

			if (swExt.Charge_Initialized)
			{
				// Override: show "Ready" text when charges are available,
				// even if IsReady is false (which happens when CurrentReady
				// < maxCharge and slots are still charging)
				if (swExt.Charge_CurrentReady > 0 && !pSuper->IsSuspended
					&& !(pSuper->Type && pSuper->Type->UseChargeDrain))
				{
					originalText = StringTable::LoadString("TXT_READY");
				}

				static wchar_t buffer[256];
				if (originalText && wcslen(originalText) > 0)
					swprintf_s(buffer, L"%s\n%d", originalText, swExt.Charge_CurrentReady);
				else
					swprintf_s(buffer, L"%d", swExt.Charge_CurrentReady);

				R->EAX(buffer);
				return 0x6CC352; // Jump to the RET instruction at end of original function
			}
		}
	}

	// Return original result
	R->EAX(originalText);
	return 0x6CC352; // Jump to the RET instruction at end of original function
}

#pragma region VanillaSidebarChargeDisplay
// ============================================================================
// Vanilla Sidebar Charge Counter & Text Display
// ============================================================================
// When SuperWeaponSidebar.Allow=no, super weapons are displayed in the vanilla
// sidebar (StripClass::DrawStrip) instead of the Phobos custom sidebar.
// The vanilla sidebar does not render the NameReadiness text or the charge
// counter for super weapons, because:
//   1. bl is cleared (xor bl, bl at 0x6A99B5), which skips the text drawing
//      code at 0x6A9AFB (test bl, bl; je 0x6A9B4B).
//   2. The charge counter (white number in top-left corner) is only implemented
//      in SWButtonClass::Draw, which is exclusive to the Phobos sidebar.
//
// These hooks add charge counter and readiness text rendering to the vanilla
// sidebar by:
//   - Hook 1 (0x6A996B): Stores the SW type index when DrawStrip enters the
//     super weapon code path. At this point, [esp+0x12] is set to 1 (text flag)
//     and edi contains the SW type index.
//   - Hook 2 (0x6A99BE): Clears the stored SW type index when DrawStrip enters
//     the non-SW code path ([esp+0x12] set to 0).
//   - Hook 3 (0x6A9BF3): After all cameo/overlay rendering is complete, draws
//     the charge counter (white number) and NameReadiness text (with charge
//     count appended) on the cameo. At this point, ESI=destX and EBP=destY
//     are still valid (callee-saved registers preserved through function calls).
// ============================================================================

namespace VanillaSidebarChargeTemp
{
	// Stores the SW type index during vanilla sidebar DrawStrip processing.
	// Set to >= 0 when processing a super weapon cameo, -1 otherwise.
	int CurrentSWTypeIndex = -1;
}

// Hook 1: Store SW type index when entering the SW drawing path in vanilla sidebar.
// Address 0x6A996B: mov byte [esp+0x12], 1  (5 bytes)
// This instruction is only reached in the SW code path (jumped to from 0x6A975B).
// At this point, edi = SW type index (set at 0x6A9936: mov edi, [eax+0x58]).
DEFINE_HOOK(0x6A996B, StripClass_DrawStrip_StoreSWIndex, 0x5)
{
	GET(int, swTypeIdx, EDI);
	VanillaSidebarChargeTemp::CurrentSWTypeIndex = swTypeIdx;

	// Preserve original instruction: mov byte [esp+0x12], 1
	R->Stack8(0x12, 1);

	return 0;
}

// Hook 2: Clear SW type index when entering the non-SW drawing path.
// Address 0x6A99BE: mov byte [esp+0x12], 0  (5 bytes)
// This instruction is reached for non-SW items (jumped to from 0x6A9727).
DEFINE_HOOK(0x6A99BE, StripClass_DrawStrip_ClearSWIndex, 0x5)
{
	VanillaSidebarChargeTemp::CurrentSWTypeIndex = -1;

	// Preserve original instruction: mov byte [esp+0x12], 0
	R->Stack8(0x12, 0);

	return 0;
}

// Hook 3: Draw charge counter and readiness text after cameo rendering.
// Address 0x6A9BF3: mov ax, word ptr [0xB0FA1C]  (6 bytes)
// This point is reached after ALL cameo/overlay/text rendering is complete
// for both SW and non-SW items. ESI=destX, EBP=destY are preserved
// (callee-saved registers, not clobbered by the drawing function calls).
DEFINE_HOOK(0x6A9BF3, StripClass_DrawStrip_DrawCharge, 0x6)
{
	// Keep the super weapon readiness/charge text at the original size so it
	// fits the cameo and is not scaled together with the font.
	TextScale::ScopedExclusion unscaledText;

	// Preserve original instruction: mov ax, word ptr [0xB0FA1C]
	// This hook replaces 6 bytes (the entire instruction), so we must
	// replicate it to load TooltipColor into AX for subsequent code.
	GET(DWORD, origEAX, EAX);
	const WORD colorWord = *reinterpret_cast<const WORD*>(0xB0FA1C);
	R->EAX((origEAX & 0xFFFF0000) | colorWord);

	if (VanillaSidebarChargeTemp::CurrentSWTypeIndex >= 0)
	{
		const auto pHouse = HouseClass::CurrentPlayer;
		const auto& supers = pHouse->Supers;

		if (supers.ValidIndex(VanillaSidebarChargeTemp::CurrentSWTypeIndex))
		{
			const auto pSuper = supers[VanillaSidebarChargeTemp::CurrentSWTypeIndex];

			if (pSuper && pSuper->IsPresent && pSuper->Type)
			{
				const auto pSWExt = SWTypeExt::ExtMap.Find(pSuper->Type);

				if (pSWExt && pSWExt->IsChargeSystemActive())
				{
					// Ensure charge state is initialized
					pSWExt->InitializeChargeState(pSuper);

					const auto pHouseExt = HouseExt::ExtMap.Find(pHouse);
					const int typeIdx = pSuper->Type->ArrayIndex;

					if (pHouseExt && typeIdx >= 0
						&& typeIdx < static_cast<int>(pHouseExt->SuperExts.size()))
					{
						const auto& swChargeExt = pHouseExt->SuperExts[typeIdx];

						if (swChargeExt.Charge_Initialized)
						{
							GET(int, destX, ESI);
							GET(int, destY, EBP);

							const auto pSurface = DSurface::Sidebar;

							if (pSurface)
							{
								RectangleStruct bounds = { 0, 0, pSurface->Width, pSurface->Height };

								// Draw charge counter (white number) in top-left corner
								wchar_t chargeBuffer[8];
								swprintf_s(chargeBuffer, L"%d", swChargeExt.Charge_CurrentReady);

								Point2D chargeLoc = { destX + 3, destY + 1 };
								const COLORREF whiteColor = Drawing::RGB_To_Int(255, 255, 255);
								constexpr TextPrintType chargePrintType =
									TextPrintType::FullShadow | TextPrintType::Point8 | TextPrintType::Background;

								pSurface->DrawTextA(chargeBuffer, &bounds, &chargeLoc, whiteColor, 0, chargePrintType);

								// Draw NameReadiness text (with charge count) at top-center
								// NameReadiness() is hooked at 0x6CC2B0 to append the charge count.
								// The vanilla sidebar calls NameReadiness at 0x6A9994 and stores
								// the result at [esp+0x40], but never draws it (bl=0 skips text).
								// We call it again here and draw the result ourselves.
								if (const auto text = pSuper->NameReadiness())
								{
									Point2D textLoc = { destX + 30, destY };
									const COLORREF foreColor = Drawing::RGB_To_Int(Drawing::TooltipColor);
									constexpr TextPrintType printType =
										TextPrintType::FullShadow | TextPrintType::Point8
										| TextPrintType::Background | TextPrintType::Center;

									pSurface->DrawTextA(text, &bounds, &textLoc, foreColor, 0, printType);
								}
							}
						}
					}
				}
			}
		}

		// Clear the stored index to prevent stale state
		VanillaSidebarChargeTemp::CurrentSWTypeIndex = -1;
	}

	return 0;
}

// Hook 4: Darken the cameo when a ready super weapon cannot afford its
// BattlePoints cost, mirroring Ares' money-based darkening at 0x6A99B7.
// Address 0x6A9AFB: test bl, bl  (6 bytes: test + je 0x6A9B4B + xor ecx,ecx)
// BL is the darken flag. Ares sets it for insufficient money; we OR in the
// BattlePoints condition. The SW type index is available via
// VanillaSidebarChargeTemp::CurrentSWTypeIndex (set at 0x6A996B for SW items,
// cleared at 0x6A99BE for non-SW items).
DEFINE_HOOK(0x6A9AFB, StripClass_DrawStrip_DarkenBP, 0x6)
{
	unsigned char darken = R->BL();

	if (VanillaSidebarChargeTemp::CurrentSWTypeIndex >= 0)
	{
		const auto pHouse = HouseClass::CurrentPlayer;
		const auto& supers = pHouse->Supers;

		if (supers.ValidIndex(VanillaSidebarChargeTemp::CurrentSWTypeIndex))
		{
			const auto pSuper = supers[VanillaSidebarChargeTemp::CurrentSWTypeIndex];

			if (pSuper && pSuper->IsReady && pSuper->Type)
			{
				const auto pSWExt = SWTypeExt::ExtMap.Find(pSuper->Type);

				if (pSWExt && pSWExt->BattlePoints_Amount < 0)
				{
					const auto pOwnerExt = HouseExt::ExtMap.Find(pSuper->Owner);

					if (pOwnerExt
						&& pOwnerExt->BattlePoints < std::abs(pSWExt->BattlePoints_Amount))
					{
						darken = 1;
					}
				}
			}
		}
	}

	R->BL(darken);

	if (darken)
	{
		// Replicate the replaced xor ecx,ecx (0x6A9AFF) before continuing
		// the darken drawing code at 0x6A9B01.
		R->ECX(0);
		return 0x6A9B01;
	}

	// Skip the darken drawing, as the original je 0x6A9B4B would.
	return 0x6A9B4B;
}
#pragma endregion
