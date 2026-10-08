#include "SWButtonClass.h"
#include "SWSidebarClass.h"
#include <Phobos.h>
#include <EventClass.h>
#include <CCToolTip.h>
#include <CommandClass.h>
#include <UI.h>

#include <Ext/SWType/Body.h>
#include <Ext/House/Body.h>
#include <Utilities/AresFunctions.h>
#include <Utilities/Debug.h>
#include <Commands/TacticalPause.h>

#include <Misc/TextScale.h>

#include <set>

SWButtonClass::SWButtonClass(int superIdx, int x, int y, int width, int height)
	: GadgetClass(x, y, width, height, (GadgetFlag::LeftPress | GadgetFlag::RightPress), false)
	, SuperIndex(superIdx)
{
	if (const auto backColumn = SWSidebarClass::Instance.Columns.back())
		backColumn->Buttons.emplace_back(this);

	this->Disabled = !SWSidebarClass::IsEnabled();
}

SWButtonClass::~SWButtonClass()
{
	// The vanilla game did not consider adding/deleting buttons midway through the game,
	// so this behavior needs to be made known to the global variable
	if (this == Make_Global<GadgetClass*>(0x8B3E94))
		this->OnMouseLeave();
}

bool SWButtonClass::Draw(bool forced)
{
	if (Phobos::UI::CinemaMode)
		return true;

	if (!forced)
		return false;

	// The super weapon sidebar draws to the composite surface, so the general
	// sidebar-surface exclusion does not apply here. Keep all of its text
	// (hotkey, readiness/Text.Ready, charge count) at the original size so it
	// does not overflow the cameo and get covered by the neighbouring buttons.
	TextScale::ScopedExclusion unscaledText;

	const auto pSurface = DSurface::Composite;
	auto bounds = pSurface->GetRect();
	Point2D location = { this->X, this->Y };
	RectangleStruct destRect = { location.X, location.Y, this->Width, this->Height };

	const auto pCurrent = HouseClass::CurrentPlayer;
	const auto pSuper = pCurrent->Supers[this->SuperIndex];
	const auto pType = pSuper->Type;
	const auto pSWExt = SWTypeExt::ExtMap.Find(pType);

	// support for pcx cameos
	if (const auto pPCXCameo = pSWExt->SidebarPCX.GetSurface())
	{
		PCX::Instance.BlitToSurface(&destRect, pSurface, pPCXCameo);
	}
	else if (const auto pCameo = pType->SidebarImage) // old shp cameos, fixed palette
	{
		const auto pCameoRef = pCameo->AsReference();
		char pFilename[0x20];
		strcpy_s(pFilename, RulesExt::Global()->MissingCameo.data());
		_strlwr_s(pFilename);

		if (!_stricmp(pCameoRef->Filename, GameStrings::XXICON_SHP) && strstr(pFilename, ".pcx"))
		{
			PCX::Instance.LoadFile(pFilename);

			if (const auto CameoPCX = PCX::Instance.GetSurface(pFilename))
				PCX::Instance.BlitToSurface(&destRect, pSurface, CameoPCX);
		}
		else
		{
			const auto pConvert = pSWExt->SidebarPal.Convert ? pSWExt->SidebarPal.GetConvert() : FileSystem::CAMEO_PAL;
			pSurface->DrawSHP(pConvert, pCameo, 0, &location, &bounds, BlitterFlags::bf_400, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		}
	}

	if (this->IsHovering)
	{
		RectangleStruct cameoRect = { location.X, location.Y, this->Width, this->Height };
		const COLORREF tooltipColor = Drawing::RGB_To_Int(Drawing::TooltipColor);
		pSurface->DrawRect(&cameoRect, tooltipColor);
	}

	// For charge system: check if SW can actually fire (CurrentReady > 0)
	bool swCanFire = pSuper->IsReady;
	if (!swCanFire && pSWExt->IsChargeSystemActive())
	{
		const auto pHouseExtDF = HouseExt::ExtMap.Find(pCurrent);
		if (pHouseExtDF && pType->ArrayIndex >= 0
			&& pType->ArrayIndex < static_cast<int>(pHouseExtDF->SuperExts.size()))
		{
			const auto& swChargeDF = pHouseExtDF->SuperExts[pType->ArrayIndex];
			if (swChargeDF.Charge_Initialized && swChargeDF.Charge_CurrentReady > 0)
				swCanFire = true;
		}
	}
	if (swCanFire && !pCurrent->CanTransactMoney(pSWExt->Money_Amount)
		|| (swCanFire && pSWExt->BattlePoints_Amount < 0
			&& HouseExt::ExtMap.Find(pCurrent)->BattlePoints < std::abs(pSWExt->BattlePoints_Amount))
		|| (pSWExt->SW_UseAITargeting && AresFunctions::IsTargetConstraintsEligible && !AresFunctions::IsTargetConstraintsEligible(AresFunctions::SWTypeExtMap_Find(pSuper->Type), HouseClass::CurrentPlayer, true)))
	{
		RectangleStruct darkenBounds { 0, 0, location.X + this->Width, location.Y + this->Height };
		pSurface->DrawSHP(FileSystem::SIDEBAR_PAL, FileSystem::DARKEN_SHP, 0, &location, &darkenBounds, BlitterFlags::bf_400 | BlitterFlags::Darken, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
	}

	// For charge system SWs, consider "ready" when CurrentReady > 0
	// (IsReady may be false when CurrentReady < maxCharge)
	bool ready = !pSuper->IsSuspended && (pType->UseChargeDrain ? pSuper->ChargeDrainState == ChargeDrainState::Ready : pSuper->IsReady);
	if (!ready && pSWExt->IsChargeSystemActive())
	{
		const auto pHouseExtCheck = HouseExt::ExtMap.Find(pCurrent);
		if (pHouseExtCheck && pType->ArrayIndex >= 0
			&& pType->ArrayIndex < static_cast<int>(pHouseExtCheck->SuperExts.size()))
		{
			const auto& swChargeCheck = pHouseExtCheck->SuperExts[pType->ArrayIndex];
			if (swChargeCheck.Charge_Initialized && swChargeCheck.Charge_CurrentReady > 0)
				ready = true;
		}
	}
	bool drawReadiness = true;

	if (ready && this->ColumnIndex == 0)
	{
		auto& buttons = SWSidebarClass::Instance.Columns[this->ColumnIndex]->Buttons;
		const int buttonId = std::distance(buttons.begin(), std::find(buttons.begin(), buttons.end(), this));

		if (buttonId < 10)
		{
			unsigned short hotkey = 0;

			for (int idx = 0; idx < CommandClass::Hotkeys.IndexCount; idx++)
			{
				if (CommandClass::Hotkeys.IndexTable[idx].Data == SWSidebarClass::Commands[buttonId])
					hotkey = CommandClass::Hotkeys.IndexTable[idx].ID;
			}

			Point2D textLoc = { location.X + this->Width / 2, location.Y };
			const COLORREF foreColor = Drawing::RGB_To_Int(Drawing::TooltipColor);
			constexpr TextPrintType printType = TextPrintType::FullShadow | TextPrintType::Point8 | TextPrintType::Background | TextPrintType::Center;

			wchar_t buffer[64];
			UI::GetKeyboardKeyString(hotkey, buffer);

			if (std::wcslen(buffer))
			{
				pSurface->DrawTextA(buffer, &bounds, &textLoc, foreColor, 0, printType);
				drawReadiness = false;
			}
		}
	}

	if (drawReadiness)
	{
		if (const auto buffer = pSuper->NameReadiness())
		{
			Point2D textLoc = { location.X + this->Width / 2, location.Y };
			const COLORREF foreColor = Drawing::RGB_To_Int(Drawing::TooltipColor);
			constexpr TextPrintType printType = TextPrintType::FullShadow | TextPrintType::Point8 | TextPrintType::Background | TextPrintType::Center;

			pSurface->DrawTextA(buffer, &bounds, &textLoc, foreColor, 0, printType);
		}
	}

	// Draw charge count in top-left corner (white number)
	if (pSWExt->IsChargeSystemActive())
	{
		const auto pHouseExt = HouseExt::ExtMap.Find(pCurrent);
		const int typeIdx = pType->ArrayIndex;

		if (pHouseExt && typeIdx >= 0 && typeIdx < static_cast<int>(pHouseExt->SuperExts.size()))
		{
			const auto& swChargeExt = pHouseExt->SuperExts[typeIdx];

			// Debug: one-time log when charge system is detected
			static std::set<int> loggedChargeSWs;
			if (loggedChargeSWs.find(typeIdx) == loggedChargeSWs.end())
			{
				loggedChargeSWs.insert(typeIdx);
				Debug::Log("[Phobos::SWButton::Draw] Charge system active for SW[%s] typeIdx=%d, Initialized=%d, CurrentReady=%d\n",
					pType->get_ID(), typeIdx, swChargeExt.Charge_Initialized, swChargeExt.Charge_CurrentReady);
			}

			// Lazy initialization if not yet initialized
			if (!swChargeExt.Charge_Initialized)
			{
				pSWExt->InitializeChargeState(pSuper);
			}

			// Show number whenever charge system is initialized (even if 0)
			if (swChargeExt.Charge_Initialized)
			{
				wchar_t chargeBuffer[8];
				swprintf_s(chargeBuffer, L"%d", swChargeExt.Charge_CurrentReady);

				Point2D chargeLoc = { location.X + 3, location.Y + 1 };
				const COLORREF whiteColor = Drawing::RGB_To_Int(255, 255, 255);
				constexpr TextPrintType chargePrintType = TextPrintType::FullShadow | TextPrintType::Point8 | TextPrintType::Background;

				pSurface->DrawTextA(chargeBuffer, &bounds, &chargeLoc, whiteColor, 0, chargePrintType);
			}
		}
		else
		{
			Debug::Log("[Phobos::SWButton::Draw] Charge system active but HouseExt/SuperExts access failed: SW[%s] typeIdx=%d, SuperExts.size=%d\n",
				pType->get_ID(), typeIdx, pHouseExt ? (int)pHouseExt->SuperExts.size() : -1);
		}
	}

	if (pSuper->ShouldDrawProgress())
	{
		Point2D loc = { location.X, location.Y };
		pSurface->DrawSHP(FileSystem::SIDEBAR_PAL, FileSystem::GCLOCK2_SHP, pSuper->AnimStage() + 1, &loc, &bounds, BlitterFlags::bf_400 | BlitterFlags::TransLucent50, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
	}

	return true;
}

void SWButtonClass::OnMouseEnter()
{
	if (Phobos::UI::CinemaMode)
		return;

	if (!SWSidebarClass::IsEnabled())
		return;

	this->IsHovering = true;
	SWSidebarClass::Instance.CurrentButton = this;
	SWSidebarClass::Instance.Columns[this->ColumnIndex]->OnMouseEnter();
	CCToolTip::Instance->SaveTimerDelay();
	CCToolTip::Instance->SetTimerDelay(0);
}

void SWButtonClass::OnMouseLeave()
{
	this->IsHovering = false;
	SWSidebarClass::Instance.CurrentButton = nullptr;
	SWSidebarClass::Instance.Columns[this->ColumnIndex]->OnMouseLeave();
	CCToolTip::Instance->RestoreTimeDelay();
}

bool SWButtonClass::Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier)
{
	if (TacticalPauseCommandClass::IsBlockingActions())
		return true;

	if (Phobos::UI::CinemaMode)
		return false;

	if (!SWSidebarClass::IsEnabled())
		return false;

	if (flags & GadgetFlag::RightPress)
		DisplayClass::Instance.CurrentSWTypeIndex = -1;

	if (flags & GadgetFlag::LeftPress)
	{
		MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);
		VocClass::PlayGlobal(RulesClass::Instance->GUIBuildSound, 0x2000, 1.0);
		this->LaunchSuper();
	}

	this->GadgetClass::Action(flags, pKey, KeyModifier::None);
	return true;
}

void SWButtonClass::SetColumn(int column)
{
	this->ColumnIndex = column;
}

bool SWButtonClass::LaunchSuper() const
{
	const auto pCurrent = HouseClass::CurrentPlayer;
	const auto pSuper = pCurrent->Supers[this->SuperIndex];
	const auto pType = pSuper->Type;
	const auto pSWExt = SWTypeExt::ExtMap.Find(pType);
	const bool manual = !pSWExt->SW_ManualFire && pSWExt->SW_AutoFire;
	const bool unstoppable = pType->UseChargeDrain && pSuper->ChargeDrainState == ChargeDrainState::Draining && pSWExt->SW_Unstoppable;

	if (!pSuper->CanFire() && !manual)
	{
		VoxClass::PlayIndex(pSWExt->EVA_Impatient);
		return false;
	}

	if (!pCurrent->CanTransactMoney(pSWExt->Money_Amount))
	{
		VoxClass::PlayIndex(pSWExt->EVA_InsufficientFunds);
		pSWExt->PrintMessage(pSWExt->Message_InsufficientFunds, pCurrent);
	}
	else if (!pSWExt->SW_UseAITargeting || (AresFunctions::IsTargetConstraintsEligible && AresFunctions::IsTargetConstraintsEligible(AresFunctions::SWTypeExtMap_Find(pType), HouseClass::CurrentPlayer, true)))
	{
		if (!manual && !unstoppable)
		{
			const auto swIndex = pType->ArrayIndex;

			if (pType->Action == Action::None || pSWExt->SW_UseAITargeting)
			{
				EventClass::OutList.Add(EventClass(pCurrent->ArrayIndex, EventType::SpecialPlace, swIndex, CellStruct::Empty));
			}
			else
			{
				DisplayClass::Instance.CurrentBuilding = nullptr;
				DisplayClass::Instance.CurrentBuildingType = nullptr;
				DisplayClass::Instance.CurrentBuildingOwnerArrayIndex = -1;
				DisplayClass::Instance.SetActiveFoundation(nullptr);
				MapClass::Instance.SetRepairMode(0);
				MapClass::Instance.SetSellMode(0);
				DisplayClass::Instance.PowerToggleMode = false;
				DisplayClass::Instance.PlanningMode = false;
				DisplayClass::Instance.PlaceBeaconMode = false;
				DisplayClass::Instance.CurrentSWTypeIndex = swIndex;
				MapClass::Instance.UnselectAll();
				VoxClass::PlayIndex(pSWExt->EVA_SelectTarget);
			}

			return true;
		}
	}
	else
	{
		pSWExt->PrintMessage(pSWExt->Message_CannotFire, pCurrent);
	}

	return false;
}
