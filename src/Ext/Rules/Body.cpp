#include "Body.h"
#include <Ext/Side/Body.h>
#include <Utilities/TemplateDef.h>
#include <FPSCounter.h>
#include <GameOptionsClass.h>

#include <Ext/BulletType/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Scenario/Body.h>
#include <New/Type/RadTypeClass.h>
#include <New/Type/ShieldTypeClass.h>
#include <New/Type/LaserTrailTypeClass.h>
#include <New/Type/DigitalDisplayTypeClass.h>
#include <New/Type/AttachEffectTypeClass.h>
#include <New/Type/BannerTypeClass.h>
#include <New/Type/InsigniaTypeClass.h>
#include <New/Type/SelectBoxTypeClass.h>
#include <New/Type/AttachmentTypeClass.h>
#include <Utilities/Patch.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TacticalClass.h>
#include <Utilities/Debug.h>
#include <Helpers/Enumerators.h>
#include <SessionClass.h>
#include <TechnoClass.h>
#include <HouseClass.h>
#include <algorithm>

std::unique_ptr<RulesExt::ExtData> RulesExt::Data = nullptr;

void RulesExt::Allocate(RulesClass* pThis)
{
	Data = std::make_unique<RulesExt::ExtData>(pThis);
}

void RulesExt::Remove(RulesClass* pThis)
{
	Data = nullptr;
}

void RulesExt::LoadFromINIFile(RulesClass* pThis, CCINIClass* pINI)
{
	Data->LoadFromINI(pINI);
}

void RulesExt::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	DigitalDisplayTypeClass::LoadFromINIList(pINI);
	SelectBoxTypeClass::LoadFromINIList(pINI);
	RadTypeClass::LoadFromINIList(pINI);
	ShieldTypeClass::LoadFromINIList(pINI);
	LaserTrailTypeClass::LoadFromINIList(&CCINIClass::INI_Art);
	AttachEffectTypeClass::LoadFromINIList(pINI);
	BannerTypeClass::LoadFromINIList(pINI);
	InsigniaTypeClass::LoadFromINIList(pINI);
	AttachmentTypeClass::LoadFromINIList(pINI);

	Data->LoadBeforeTypeData(pThis, pINI);
}

void RulesExt::LoadAfterTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	for (const auto& pTechnoType : TechnoTypeClass::Array)
	{
		if (const auto pTechnoTypeExt = TechnoTypeExt::ExtMap.TryFind(pTechnoType))
		{
			// Spawner range
			if (pTechnoTypeExt->Spawner_LimitRange)
				pTechnoTypeExt->CalculateSpawnerRange();
		}
	}

	if (pINI == CCINIClass::INI_Rules)
		Data->InitializeAfterTypeData(pThis);

	Data->LoadAfterTypeData(pThis, pINI);
}

bool RulesExt::ExtData::IsTargetInFilter(int filterMask, TechnoClass* pTarget) const
{
	if (!pTarget || filterMask == 0)
		return false;

	const int whatAmI = static_cast<int>(pTarget->WhatAmI());
	bool matched = false;

	// Check each active filter bit
	if (filterMask & static_cast<int>(TargetFilter::Infantry))
		matched |= (whatAmI == static_cast<int>(AbstractType::Infantry));

	if (filterMask & static_cast<int>(TargetFilter::Vehicle))
		matched |= (whatAmI == static_cast<int>(AbstractType::Unit));

	if (filterMask & static_cast<int>(TargetFilter::Building))
		matched |= (whatAmI == static_cast<int>(AbstractType::Building));

	if (filterMask & static_cast<int>(TargetFilter::Fighter))
		matched |= (whatAmI == static_cast<int>(AbstractType::Aircraft));

	if (filterMask & static_cast<int>(TargetFilter::Artillery))
	{
		const auto& list = this->FilterArtillery;
		matched |= (!list.empty()
			&& std::find(list.begin(), list.end(), pTarget->GetTechnoType()) != list.end());
	}

	if (filterMask & static_cast<int>(TargetFilter::Bomber))
	{
		const auto& list = this->FilterBomber;
		matched |= (!list.empty()
			&& std::find(list.begin(), list.end(), pTarget->GetTechnoType()) != list.end());
	}

	return matched;
}

void RulesExt::ExtData::InitializeConstants()
{

}

// earliest loader - can't really do much because nothing else is initialized yet, so lookups won't work
void RulesExt::ExtData::LoadFromINIFile(CCINIClass* pINI)
{

}

void RulesExt::ExtData::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	INI_EX exINI(pINI);

	this->Storage_TiberiumIndex.Read(exINI, GameStrings::General, "Storage.TiberiumIndex");
	this->HarvesterDumpAmount.Read(exINI, GameStrings::General, "HarvesterDumpAmount");
	this->InfantryGainSelfHealCap.Read(exINI, GameStrings::General, "InfantryGainSelfHealCap");
	this->UnitsGainSelfHealCap.Read(exINI, GameStrings::General, "UnitsGainSelfHealCap");
	this->GainSelfHealAllowMultiplayPassive.Read(exINI, GameStrings::General, "GainSelfHealAllowMultiplayPassive");
	this->GainSelfHealFromPlayerControl.Read(exINI, GameStrings::General, "GainSelfHealFromPlayerControl");
	this->GainSelfHealFromAllies.Read(exINI, GameStrings::General, "GainSelfHealFromAllies");
	this->EnemyInsignia.Read(exINI, GameStrings::General, "EnemyInsignia");
	this->DisguiseBlinkingVisibility.Read(exINI, GameStrings::General, "DisguiseBlinkingVisibility");
	this->ChronoSparkleDisplayDelay.Read(exINI, GameStrings::General, "ChronoSparkleDisplayDelay");
	this->ChronoSparkleBuildingDisplayPositions.Read(exINI, GameStrings::General, "ChronoSparkleBuildingDisplayPositions");
	this->ChronoSpherePreDelay.Read(exINI, GameStrings::General, "ChronoSpherePreDelay");
	this->ChronoSphereDelay.Read(exINI, GameStrings::General, "ChronoSphereDelay");
	this->AIChronoSphereSW.Read(exINI, GameStrings::General, "AIChronoSphereSW");
	this->AIChronoWarpSW.Read(exINI, GameStrings::General, "AIChronoWarpSW");

	exINI.ReadSpeed(GameStrings::General, "SubterraneanSpeed", &this->SubterraneanSpeed);
	this->SubterraneanHeight.Read(exINI, GameStrings::General, "SubterraneanHeight");
	this->AISuperWeaponDelay.Read(exINI, GameStrings::General, "AISuperWeaponDelay");
	this->UseGlobalRadApplicationDelay.Read(exINI, GameStrings::Radiation, "UseGlobalRadApplicationDelay");
	this->RadApplicationDelay_Building.Read(exINI, GameStrings::Radiation, "RadApplicationDelay.Building");
	this->RadBuildingDamageMaxCount.Read(exINI, GameStrings::Radiation, "RadBuildingDamageMaxCount");
	this->RadSiteWarhead_Detonate.Read(exINI, GameStrings::Radiation, "RadSiteWarhead.Detonate");
	this->RadSiteWarhead_Detonate_Full.Read(exINI, GameStrings::Radiation, "RadSiteWarhead.Detonate.Full");
	this->RadHasOwner.Read(exINI, GameStrings::Radiation, "RadHasOwner");
	this->RadHasInvoker.Read(exINI, GameStrings::Radiation, "RadHasInvoker");
	this->VeinholeWarhead.Read<true>(exINI, GameStrings::CombatDamage, "VeinholeWarhead");
	this->MissingCameo.Read(pINI, GameStrings::AudioVisual, "MissingCameo");

	this->PlacementGrid_Translucency.Read(exINI, GameStrings::AudioVisual, "PlacementGrid.Translucency");
	this->PlacementGrid_TranslucencyWithPreview.Read(exINI, GameStrings::AudioVisual, "PlacementGrid.TranslucencyWithPreview");
	this->PlacementPreview.Read(exINI, GameStrings::AudioVisual, "PlacementPreview");
	this->PlacementPreview_Translucency.Read(exINI, GameStrings::AudioVisual, "PlacementPreview.Translucency");

	this->SuperWeaponSidebar_AllowByDefault.Read(exINI, GameStrings::AudioVisual, "SuperWeaponSidebar.AllowByDefault");

	this->ConditionYellow_Terrain.Read(exINI, GameStrings::AudioVisual, "ConditionYellow.Terrain");
	this->Shield_ConditionYellow.Read(exINI, GameStrings::AudioVisual, "Shield.ConditionYellow");
	this->Shield_ConditionRed.Read(exINI, GameStrings::AudioVisual, "Shield.ConditionRed");
	this->Pips_Shield.Read(exINI, GameStrings::AudioVisual, "Pips.Shield");
	this->Pips_Shield_Background.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Background");
	this->Pips_Shield_Building.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Building");
	this->Pips_Shield_Building_Empty.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Building.Empty");
	this->Pips_SelfHeal_Infantry.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Infantry");
	this->Pips_SelfHeal_Units.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Units");
	this->Pips_SelfHeal_Buildings.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Buildings");
	this->Pips_SelfHeal_Infantry_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Infantry.Offset");
	this->Pips_SelfHeal_Units_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Units.Offset");
	this->Pips_SelfHeal_Buildings_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Buildings.Offset");
	this->Pips_Generic_Size.Read(exINI, GameStrings::AudioVisual, "Pips.Generic.Size");
	this->Pips_Generic_Buildings_Size.Read(exINI, GameStrings::AudioVisual, "Pips.Generic.Buildings.Size");
	this->Pips_Ammo_Size.Read(exINI, GameStrings::AudioVisual, "Pips.Ammo.Size");
	this->Pips_Ammo_Buildings_Size.Read(exINI, GameStrings::AudioVisual, "Pips.Ammo.Buildings.Size");
	this->Pips_Tiberiums_Frames.Read(exINI, GameStrings::AudioVisual, "Pips.Tiberiums.Frames");
	this->Pips_Tiberiums_EmptyFrame.Read(exINI, GameStrings::AudioVisual, "Pips.Tiberiums.EmptyFrame");
	this->Pips_Tiberiums_DisplayOrder.Read(exINI, GameStrings::AudioVisual, "Pips.Tiberiums.DisplayOrder");
	this->Pips_Tiberiums_WeedFrame.Read(exINI, GameStrings::AudioVisual, "Pips.Tiberiums.WeedFrame");
	this->Pips_Tiberiums_WeedEmptyFrame.Read(exINI, GameStrings::AudioVisual, "Pips.Tiberiums.WeedEmptyFrame");
	this->ToolTip_Background_Color.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.Color");
	this->ToolTip_Background_Opacity.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.Opacity");
	this->ToolTip_Background_BlurSize.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.BlurSize");
	this->RadialIndicatorVisibility.Read(exINI, GameStrings::AudioVisual, "RadialIndicatorVisibility");
	this->DrawTurretShadow.Read(exINI, GameStrings::AudioVisual, "DrawTurretShadow");
	this->AnimRemapDefaultColorScheme.Read(exINI, GameStrings::AudioVisual, "AnimRemapDefaultColorScheme");
	this->TimerBlinkColorScheme.Read(exINI, GameStrings::AudioVisual, "TimerBlinkColorScheme");
	this->ShowDesignatorRange.Read(exINI, GameStrings::AudioVisual, "ShowDesignatorRange");
	Nullable<double>AirShadowBaseScale;
	AirShadowBaseScale.Read(exINI, GameStrings::AudioVisual, "AirShadowBaseScale");
	if (AirShadowBaseScale.isset() && AirShadowBaseScale.Get() > 0)
		this->AirShadowBaseScale_log = -std::log(std::min(AirShadowBaseScale.Get(), 1.0));

	this->HeightShadowScaling.Read(exINI, GameStrings::AudioVisual, "HeightShadowScaling");
	if (AirShadowBaseScale.isset() && AirShadowBaseScale.Get() > 0.98 && this->HeightShadowScaling.Get())
		this->HeightShadowScaling = false;
	this->HeightShadowScaling_MinScale.Read(exINI, GameStrings::AudioVisual, "HeightShadowScaling.MinScale");

	this->RemoveShroudGlobally.Read(exINI, GameStrings::General, "RemoveShroudGlobally");

	this->ExtendedAircraftMissions.Read(exINI, GameStrings::General, "ExtendedAircraftMissions");
	this->AmphibiousEnter.Read(exINI, GameStrings::General, "AmphibiousEnter");
	this->AmphibiousUnload.Read(exINI, GameStrings::General, "AmphibiousUnload");
	this->NoQueueUpToEnter.Read(exINI, GameStrings::General, "NoQueueUpToEnter");
	this->NoQueueUpToUnload.Read(exINI, GameStrings::General, "NoQueueUpToUnload");

	this->BuildingProductionQueue.Read(exINI, GameStrings::General, "BuildingProductionQueue");

	this->AllowParallelAIQueues.Read(exINI, "GlobalControls", "AllowParallelAIQueues");
	this->ForbidParallelAIQueues_Aircraft.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Aircraft");
	this->ForbidParallelAIQueues_Building.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Building");
	this->ForbidParallelAIQueues_Infantry.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Infantry");
	this->ForbidParallelAIQueues_Navy.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Navy");
	this->ForbidParallelAIQueues_Vehicle.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Vehicle");

	this->EnablePowerSurplus.Read(exINI, GameStrings::AI, "EnablePowerSurplus");

	this->IronCurtain_KeptOnDeploy.Read(exINI, GameStrings::CombatDamage, "IronCurtain.KeptOnDeploy");
	this->IronCurtain_EffectOnOrganics.Read(exINI, GameStrings::CombatDamage, "IronCurtain.EffectOnOrganics");
	this->IronCurtain_KillOrganicsWarhead.Read<true>(exINI, GameStrings::CombatDamage, "IronCurtain.KillOrganicsWarhead");
	this->ForceShield_KeptOnDeploy.Read(exINI, GameStrings::CombatDamage, "ForceShield.KeptOnDeploy");
	this->ForceShield_EffectOnOrganics.Read(exINI, GameStrings::CombatDamage, "ForceShield.EffectOnOrganics");
	this->ForceShield_KillOrganicsWarhead.Read<true>(exINI, GameStrings::CombatDamage, "ForceShield.KillOrganicsWarhead");
	this->AllowWeaponSelectAgainstWalls.Read(exINI, GameStrings::CombatDamage, "AllowWeaponSelectAgainstWalls");

	this->IronCurtain_ExtraTintIntensity.Read(exINI, GameStrings::AudioVisual, "IronCurtain.ExtraTintIntensity");
	this->ForceShield_ExtraTintIntensity.Read(exINI, GameStrings::AudioVisual, "ForceShield.ExtraTintIntensity");
	this->ColorAddUse8BitRGB.Read(exINI, GameStrings::AudioVisual, "ColorAddUse8BitRGB");
	this->AirstrikeLineColor.Read(exINI, GameStrings::AudioVisual, "AirstrikeLineColor");
	this->AirstrikeLineZAdjust.Read(exINI, GameStrings::AudioVisual, "AirstrikeLineZAdjust");

	this->CrateOnlyOnLand.Read(exINI, GameStrings::CrateRules, "CrateOnlyOnLand");
	this->UnitCrateVehicleCap.Read(exINI, GameStrings::CrateRules, "UnitCrateVehicleCap");
	this->FreeMCV_CreditsThreshold.Read(exINI, GameStrings::CrateRules, "FreeMCV.CreditsThreshold");

	this->ROF_RandomDelay.Read(exINI, GameStrings::CombatDamage, "ROF.RandomDelay");

	this->DisplayIncome.Read(exINI, GameStrings::AudioVisual, "DisplayIncome");
	this->DisplayIncome_Houses.Read(exINI, GameStrings::AudioVisual, "DisplayIncome.Houses");
	this->DisplayIncome_AllowAI.Read(exINI, GameStrings::AudioVisual, "DisplayIncome.AllowAI");

	this->IsVoiceCreatedGlobal.Read(exINI, GameStrings::AudioVisual, "IsVoiceCreatedGlobal");
	this->SelectionFlashDuration.Read(exINI, GameStrings::AudioVisual, "SelectionFlashDuration");
	this->DrawInsignia_OnlyOnSelected.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.OnlyOnSelected");
	this->DrawInsignia_AdjustPos_Infantry.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.AdjustPos.Infantry");
	this->DrawInsignia_AdjustPos_Buildings.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.AdjustPos.Buildings");
	this->DrawInsignia_AdjustPos_BuildingsAnchor.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.AdjustPos.BuildingsAnchor");
	this->DrawInsignia_AdjustPos_Units.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.AdjustPos.Units");
	this->DrawInsignia_UsePixelSelectionBracketDelta.Read(exINI, GameStrings::AudioVisual, "DrawInsignia.UsePixelSelectionBracketDelta");
	this->Promote_VeteranAnimation.Read(exINI, GameStrings::AudioVisual, "Promote.VeteranAnimation");
	this->Promote_EliteAnimation.Read(exINI, GameStrings::AudioVisual, "Promote.EliteAnimation");

	this->DropPodTrailer.Read(exINI, GameStrings::General, "DropPodTrailer");
	this->DropPodDefaultTrailer = AnimTypeClass::Find("SMOKEY");
	this->PodImage = FileSystem::LoadSHPFile("POD.SHP");

	this->BuildingWaypoints.Read(exINI, GameStrings::General, "BuildingWaypoints");

	this->VisualScatter_Min.Read(exINI, GameStrings::AudioVisual, "VisualScatter.Min");
	this->VisualScatter_Max.Read(exINI, GameStrings::AudioVisual, "VisualScatter.Max");

	this->Buildings_DefaultDigitalDisplayTypes.Read(exINI, GameStrings::AudioVisual, "Buildings.DefaultDigitalDisplayTypes");
	this->Infantry_DefaultDigitalDisplayTypes.Read(exINI, GameStrings::AudioVisual, "Infantry.DefaultDigitalDisplayTypes");
	this->Vehicles_DefaultDigitalDisplayTypes.Read(exINI, GameStrings::AudioVisual, "Vehicles.DefaultDigitalDisplayTypes");
	this->Aircraft_DefaultDigitalDisplayTypes.Read(exINI, GameStrings::AudioVisual, "Aircraft.DefaultDigitalDisplayTypes");

	this->DefaultInfantrySelectBox.Read(exINI, GameStrings::AudioVisual, "DefaultInfantrySelectBox");
	this->DefaultUnitSelectBox.Read(exINI, GameStrings::AudioVisual, "DefaultUnitSelectBox");

	this->JumpjetClimbPredictHeight.Read(exINI, GameStrings::General, "JumpjetClimbPredictHeight");
	this->JumpjetClimbWithoutCutOut.Read(exINI, GameStrings::General, "JumpjetClimbWithoutCutOut");
	this->MissileSpawnAttackCell.Read(exINI, GameStrings::General, "MissileSpawnAttackCell");

	this->DamageOwnerMultiplier.Read(exINI, GameStrings::CombatDamage, "DamageOwnerMultiplier");
	this->DamageAlliesMultiplier.Read(exINI, GameStrings::CombatDamage, "DamageAlliesMultiplier");
	this->DamageEnemiesMultiplier.Read(exINI, GameStrings::CombatDamage, "DamageEnemiesMultiplier");
	this->DamageOwnerMultiplier_NotAffectsEnemies.Read(exINI, GameStrings::CombatDamage, "DamageOwnerMultiplier.NotAffectsEnemies");
	this->DamageAlliesMultiplier_NotAffectsEnemies.Read(exINI, GameStrings::CombatDamage, "DamageAlliesMultiplier.NotAffectsEnemies");

	this->Armor_FrontMultiplier.Read(exINI, GameStrings::CombatDamage, "Armor.FrontMultiplier");
	this->Armor_SideMultiplier.Read(exINI, GameStrings::CombatDamage, "Armor.SideMultiplier");
	this->Armor_RearMultiplier.Read(exINI, GameStrings::CombatDamage, "Armor.RearMultiplier");

	this->FixOccupyFire.Read(exINI, GameStrings::CombatDamage, "FixOccupyFire");
	this->UseGlobalOccupyRange.Read(exINI, GameStrings::CombatDamage, "UseGlobalOccupyRange");

	this->AIDirectionalArmor.Read(exINI, GameStrings::General, "AIDirectionalArmor");
	this->AIDirectionalArmor_Chance.Read(exINI, GameStrings::General, "AIDirectionalArmor.Chance");
	this->AIDirectionalArmor_Delay.Read(exINI, GameStrings::General, "AIDirectionalArmor.Delay");

	this->AircraftLevelLightMultiplier.Read(exINI, GameStrings::AudioVisual, "AircraftLevelLightMultiplier");
	this->JumpjetLevelLightMultiplier.Read(exINI, GameStrings::AudioVisual, "JumpjetLevelLightMultiplier");

	this->VoxelLightSource.Read(exINI, GameStrings::AudioVisual, "VoxelLightSource");
	// this->VoxelShadowLightSource.Read(exINI, GameStrings::AudioVisual, "VoxelShadowLightSource");

	this->CombatAlert.Read(exINI, GameStrings::AudioVisual, "CombatAlert");
	this->CombatAlert_Default.Read(exINI, GameStrings::AudioVisual, "CombatAlert.Default");
	this->CombatAlert_IgnoreBuilding.Read(exINI, GameStrings::AudioVisual, "CombatAlert.IgnoreBuilding");
	this->CombatAlert_SuppressIfInScreen.Read(exINI, GameStrings::AudioVisual, "CombatAlert.SuppressIfInScreen");
	this->CombatAlert_Interval.Read(exINI, GameStrings::AudioVisual, "CombatAlert.Interval");
	this->CombatAlert_SuppressIfAllyDamage.Read(exINI, GameStrings::AudioVisual, "CombatAlert.SuppressIfAllyDamage");
	this->CombatAlert_MakeAVoice.Read(exINI, GameStrings::AudioVisual, "CombatAlert.MakeAVoice");
	this->CombatAlert_UseFeedbackVoice.Read(exINI, GameStrings::AudioVisual, "CombatAlert.UseFeedbackVoice");
	this->CombatAlert_UseAttackVoice.Read(exINI, GameStrings::AudioVisual, "CombatAlert.UseAttackVoice");
	this->CombatAlert_UseEVA.Read(exINI, GameStrings::AudioVisual, "CombatAlert.UseEVA");

	this->ReplaceVoxelLightSources();

	this->UseFixedVoxelLighting.Read(exINI, GameStrings::AudioVisual, "UseFixedVoxelLighting");

	this->AIAutoDeployMCV.Read(exINI, GameStrings::AI, "AIAutoDeployMCV");
	this->AISetBaseCenter.Read(exINI, GameStrings::AI, "AISetBaseCenter");
	this->AIBiasSpawnCell.Read(exINI, GameStrings::AI, "AIBiasSpawnCell");
	this->AIForbidConYard.Read(exINI, GameStrings::AI, "AIForbidConYard");
	this->AINodeWallsOnly.Read(exINI, GameStrings::AI, "AINodeWallsOnly");
	this->AICleanWallNode.Read(exINI, GameStrings::AI, "AICleanWallNode");

	this->AttackMove_Aggressive.Read(exINI, GameStrings::General, "AttackMove.Aggressive");
	this->AttackMove_UpdateTarget.Read(exINI, GameStrings::General, "AttackMove.UpdateTarget");
	this->SmoothMove.Read(exINI, GameStrings::General, "SmoothMove");
	this->TA_VirtualUnit.Read(exINI, GameStrings::General, "TA.VirtualUnit");
	this->BetterPerformance.Read(exINI, GameStrings::General, "BetterPerformance");
	this->Tiberium_CanBeBuiltOn.Read(exINI, GameStrings::General, "Tiberium.CanBeBuiltOn");
	this->Wall_CanBeBuiltOn.Read(exINI, GameStrings::General, "Wall.CanBeBuiltOn");
	this->Rock_CanBeBuiltOn.Read(exINI, GameStrings::General, "Rock.CanBeBuiltOn");
	this->CanBeBuiltOnOverlay_Remove.Read(exINI, GameStrings::General, "CanBeBuiltOnOverlay.Remove");

	this->MindControl_ThreatDelay.Read(exINI, GameStrings::General, "MindControl.ThreatDelay");

	this->RecountBurst.Read(exINI, GameStrings::General, "RecountBurst");
	this->NoRearm_UnderEMP.Read(exINI, GameStrings::General, "NoRearm.UnderEMP");
	this->NoRearm_Temporal.Read(exINI, GameStrings::General, "NoRearm.Temporal");
	this->NoReload_UnderEMP.Read(exINI, GameStrings::General, "NoReload.UnderEMP");
	this->NoReload_Temporal.Read(exINI, GameStrings::General, "NoReload.Temporal");
	this->NoTurret_TrackTarget.Read(exINI, GameStrings::General, "NoTurret.TrackTarget");

	this->GatherWhenMCVDeploy.Read(exINI, GameStrings::General, "GatherWhenMCVDeploy");
	this->AIFireSale.Read(exINI, GameStrings::General, "AIFireSale");
	this->AIFireSaleDelay.Read(exINI, GameStrings::General, "AIFireSaleDelay");
	this->AIAllToHunt.Read(exINI, GameStrings::General, "AIAllToHunt");
	this->RepairBaseNodes.Read(exINI, GameStrings::Basic, "RepairBaseNodes");

	this->WarheadParticleAlphaImageIsLightFlash.Read(exINI, GameStrings::AudioVisual, "WarheadParticleAlphaImageIsLightFlash");
	this->CombatLightDetailLevel.Read(exINI, GameStrings::AudioVisual, "CombatLightDetailLevel");
	this->LightFlashAlphaImageDetailLevel.Read(exINI, GameStrings::AudioVisual, "LightFlashAlphaImageDetailLevel");
	this->BuildingTypeSelectable.Read(exINI, GameStrings::General, "BuildingTypeSelectable");

	this->ProneSpeed_Crawls.Read(exINI, GameStrings::General, "ProneSpeed.Crawls");
	this->ProneSpeed_NoCrawls.Read(exINI, GameStrings::General, "ProneSpeed.NoCrawls");

	this->DamagedSpeed.Read(exINI, GameStrings::General, "DamagedSpeed");

	this->HarvesterScanAfterUnload.Read(exINI, GameStrings::General, "HarvesterScanAfterUnload");

	this->AnimCraterDestroyTiberium.Read(exINI, GameStrings::General, "AnimCraterDestroyTiberium");

	this->BerzerkTargeting.Read(exINI, GameStrings::CombatDamage, "BerzerkTargeting");

	this->AttackMove_IgnoreWeaponCheck.Read(exINI, GameStrings::General, "AttackMove.IgnoreWeaponCheck");
	this->AttackMove_StopWhenTargetAcquired.Read(exINI, GameStrings::General, "AttackMove.StopWhenTargetAcquired");

	this->UrbanCombat_AttackBuff.Read(exINI, GameStrings::General, "UrbanCombat.AttackBuff");
	this->UrbanCombat_Rate.Read(exINI, GameStrings::General, "UrbanCombat.Rate");
	this->UrbanCombat_OutsideDamageMultiplier.Read(exINI, GameStrings::General, "UrbanCombat.OutsideDamageMultiplier");
	this->UrbanCombat_DefOutsideMulti.Read(exINI, GameStrings::General, "UrbanCombat.DefOutsideMulti");
	this->UrbanCombat_AttOutsideMulti.Read(exINI, GameStrings::General, "UrbanCombat.AttOutsideMulti");

	this->Parasite_GrappleAnim.Read(exINI, GameStrings::AudioVisual, "Parasite.GrappleAnim");

	this->AINormalTargetingDelay.Read(exINI, GameStrings::General, "AINormalTargetingDelay");
	this->PlayerNormalTargetingDelay.Read(exINI, GameStrings::General, "PlayerNormalTargetingDelay");
	this->AIGuardAreaTargetingDelay.Read(exINI, GameStrings::General, "AIGuardAreaTargetingDelay");
	this->PlayerGuardAreaTargetingDelay.Read(exINI, GameStrings::General, "PlayerGuardAreaTargetingDelay");
	this->AIAttackMoveTargetingDelay.Read(exINI, GameStrings::General, "AIAttackMoveTargetingDelay");
	this->PlayerAttackMoveTargetingDelay.Read(exINI, GameStrings::General, "PlayerAttackMoveTargetingDelay");
	this->DistributeTargetingFrame.Read(exINI, GameStrings::General, "DistributeTargetingFrame");
	this->DistributeTargetingFrame_AIOnly.Read(exINI, GameStrings::General, "DistributeTargetingFrame.AIOnly");

	this->InfantryAutoDeploy.Read(exINI, GameStrings::General, "InfantryAutoDeploy");
	
	this->BattlePoints.Read(exINI, GameStrings::General, "BattlePoints");
	this->BattlePoints_DefaultValue.Read(exINI, GameStrings::General, "BattlePoints.DefaultValue");
	this->BattlePoints_DefaultFriendlyValue.Read(exINI, GameStrings::General, "BattlePoints.DefaultFriendlyValue");
	
	this->PenetratesTransport_Level.Read(exINI, GameStrings::CombatDamage, "PenetratesTransport.Level");
	
	// === Target Filter System - read manual filter lists from [General] ===
	// Infantry/Vehicle/Building/Fighter use automatic type-based filtering (no INI needed).
	// === Target Filter System - read filter lists from [ArtilleryTypes] and [BomberTypes] sections ===
   	// Infantry/Vehicle/Building/Fighter use automatic type-based filtering (no INI needed).
	// Artillery/Bomber: read registered type names from dedicated INI sections.
	
	this->MovePositionIndicator.Read(exINI, GameStrings::AudioVisual, "MovePositionIndicator");
	this->MovePositionIndicatorColor.Read(exINI, GameStrings::AudioVisual, "MovePositionIndicatorColor");
	this->MovePositionIndicator_ShowEnemy.Read(exINI, GameStrings::General, "MovePositionIndicator.ShowEnemy");

	auto ReadDifficultyList = [&exINI](const char* pSection, const char* pKey, std::vector<int>& out)
	{
		std::vector<std::string> names;
		if (!exINI.ParseStringList(names, pSection, pKey))
			return; // key absent - keep previously parsed values (rules reloads must not clear them)

		out.clear();
		for (auto const& name : names)
		{
			if (name.empty() || _stricmp(name.c_str(), "none") == 0)
				continue;
			if (_stricmp(name.c_str(), "easy") == 0)
				out.push_back(0);
			else if (_stricmp(name.c_str(), "normal") == 0)
				out.push_back(1);
			else if (_stricmp(name.c_str(), "hard") == 0)
				out.push_back(2);
		}
	};
	ReadDifficultyList(GameStrings::General, "DisableSaveGame.Difficulty", this->DisableSaveGame);
	ReadDifficultyList(GameStrings::General, "DisableLoadGame.Difficulty", this->DisableLoadGame);
	ReadDifficultyList(GameStrings::General, "LockGameSpeed.Difficulty", this->LockGameSpeed);
	ReadDifficultyList(GameStrings::General, "TacticalPause.BlockActions.Difficulty", this->TacticalPause_BlockActions);
	Debug::Log("[Phobos::TacticalPause] BlockActions.Difficulty count=%d\n", static_cast<int>(this->TacticalPause_BlockActions.size()));

	for (int d : this->TacticalPause_BlockActions)
		Debug::Log("[Phobos::TacticalPause] BlockActions.Difficulty entry=%d\n", d);

	// Section AITargetTypes
	int itemsCount = pINI->GetKeyCount("AITargetTypes");
	for (int i = 0; i < itemsCount; ++i)
	{
		std::vector<TechnoTypeClass*> objectsList;
		char* context = nullptr;
		pINI->ReadString("AITargetTypes", pINI->GetKeyName("AITargetTypes", i), "", Phobos::readBuffer);

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			TechnoTypeClass* buffer;
			if (Parser<TechnoTypeClass*>::TryParse(cur, &buffer))
				objectsList.emplace_back(buffer);
			else
				Debug::Log("[Developer warning] AITargetTypes (Count: %d): Error parsing [%s]\n", this->AITargetTypesLists.size(), cur);
		}

		this->AITargetTypesLists.emplace_back(std::move(objectsList));
	}
	
	this->AttachmentTopLayerMinHeight.Read(exINI, GameStrings::General, "AttachmentTopLayerMinHeight");
	this->AttachmentUndergroundLayerMaxHeight.Read(exINI, GameStrings::General, "AttachmentUndergroundLayerMaxHeight");

	// Section AIScriptsList
	int scriptitemsCount = pINI->GetKeyCount("AIScriptsList");
	for (int i = 0; i < scriptitemsCount; ++i)
	{
		std::vector<ScriptTypeClass*> objectsList;

		char* context = nullptr;
		pINI->ReadString("AIScriptsList", pINI->GetKeyName("AIScriptsList", i), "", Phobos::readBuffer);

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			ScriptTypeClass* pNewScript = ScriptTypeClass::FindOrAllocate(cur);
			objectsList.emplace_back(pNewScript);
		}

		this->AIScriptsLists.emplace_back(std::move(objectsList));
	}
}

// this should load everything that TypeData is not dependant on
// i.e. InfantryElectrocuted= can go here since nothing refers to it
// but [GenericPrerequisites] have to go earlier because they're used in parsing TypeData
void RulesExt::ExtData::LoadAfterTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	INI_EX exINI(pINI);

	// === Target Filter System - read filter lists from [ArtilleryTypes] and [BomberTypes] sections ===
	// Must be loaded here (after all TechnoTypes are registered) so that
	// Parser<TechnoTypeClass*>::TryParse can resolve type names correctly.
	if (pINI == CCINIClass::INI_Rules)
	{
		this->FilterArtillery.clear();
		{
			const char* pSection = "ArtilleryTypes";
			const int nCount = pINI->GetKeyCount(pSection);
			for (int i = 0; i < nCount; ++i)
			{
				pINI->ReadString(pSection, pINI->GetKeyName(pSection, i), "", Phobos::readBuffer);

				char* context = nullptr;
				for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
				{
					TechnoTypeClass* buffer = nullptr;
					if (Parser<TechnoTypeClass*>::TryParse(cur, &buffer))
						this->FilterArtillery.push_back(buffer);
					else
						Debug::Log("[Developer warning] ArtilleryTypes: Error parsing [%s]\n", cur);
				}
			}
		}

		this->FilterBomber.clear();
		{
			const char* pSection = "BomberTypes";
			const int nCount = pINI->GetKeyCount(pSection);
			for (int i = 0; i < nCount; ++i)
			{
				pINI->ReadString(pSection, pINI->GetKeyName(pSection, i), "", Phobos::readBuffer);

				char* context = nullptr;
				for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
				{
					TechnoTypeClass* buffer = nullptr;
					if (Parser<TechnoTypeClass*>::TryParse(cur, &buffer))
						this->FilterBomber.push_back(buffer);
					else
						Debug::Log("[Developer warning] BomberTypes: Error parsing [%s]\n", cur);
				}
			}
		}
	}
}

// this runs between the before and after type data loading methods for rules ini
void RulesExt::ExtData::InitializeAfterTypeData(RulesClass* const pThis)
{

}

void RulesExt::ExtData::InitializeAfterAllLoaded()
{
	const auto pRules = RulesClass::Instance;

	// tint color
	this->TintColorIronCurtain = GeneralUtils::GetColorFromColorAdd(pRules->IronCurtainColor);
	this->TintColorForceShield = GeneralUtils::GetColorFromColorAdd(pRules->ForceShieldColor);
	this->TintColorBerserk = GeneralUtils::GetColorFromColorAdd(pRules->BerserkColor);

	// Init master bullet
	ScenarioExt::Global()->MasterDetonationBullet = BulletTypeExt::GetDefaultBulletType()->CreateBullet(nullptr, nullptr, 0, nullptr, 0, false);
}

// =============================
// load / save

template <typename T>
void RulesExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->AITargetTypesLists)
		.Process(this->AIScriptsLists)
		.Process(this->Storage_TiberiumIndex)
		.Process(this->HarvesterDumpAmount)
		.Process(this->InfantryGainSelfHealCap)
		.Process(this->UnitsGainSelfHealCap)
		.Process(this->GainSelfHealAllowMultiplayPassive)
		.Process(this->GainSelfHealFromPlayerControl)
		.Process(this->GainSelfHealFromAllies)
		.Process(this->EnemyInsignia)
		.Process(this->DisguiseBlinkingVisibility)
		.Process(this->ChronoSparkleDisplayDelay)
		.Process(this->ChronoSparkleBuildingDisplayPositions)
		.Process(this->ChronoSpherePreDelay)
		.Process(this->ChronoSphereDelay)
		.Process(this->AIChronoSphereSW)
		.Process(this->AIChronoWarpSW)
		.Process(this->SubterraneanSpeed)
		.Process(this->SubterraneanHeight)
		.Process(this->AISuperWeaponDelay)
		.Process(this->UseGlobalRadApplicationDelay)
		.Process(this->RadApplicationDelay_Building)
		.Process(this->RadBuildingDamageMaxCount)
		.Process(this->RadSiteWarhead_Detonate)
		.Process(this->RadSiteWarhead_Detonate_Full)
		.Process(this->RadHasOwner)
		.Process(this->RadHasInvoker)
		.Process(this->JumpjetCrash)
		.Process(this->JumpjetNoWobbles)
		.Process(this->VeinholeWarhead)
		.Process(this->MissingCameo)
		.Process(this->PlacementGrid_Translucency)
		.Process(this->PlacementGrid_TranslucencyWithPreview)
		.Process(this->PlacementPreview)
		.Process(this->PlacementPreview_Translucency)
		.Process(this->SuperWeaponSidebar_AllowByDefault)
		.Process(this->ConditionYellow_Terrain)
		.Process(this->Shield_ConditionYellow)
		.Process(this->Shield_ConditionRed)
		.Process(this->Pips_Shield)
		.Process(this->Pips_Shield_Background)
		.Process(this->Pips_Shield_Building)
		.Process(this->Pips_Shield_Building_Empty)
		.Process(this->Pips_SelfHeal_Infantry)
		.Process(this->Pips_SelfHeal_Units)
		.Process(this->Pips_SelfHeal_Buildings)
		.Process(this->Pips_SelfHeal_Infantry_Offset)
		.Process(this->Pips_SelfHeal_Units_Offset)
		.Process(this->Pips_SelfHeal_Buildings_Offset)
		.Process(this->Pips_Generic_Size)
		.Process(this->Pips_Generic_Buildings_Size)
		.Process(this->Pips_Ammo_Size)
		.Process(this->Pips_Ammo_Buildings_Size)
		.Process(this->Pips_Tiberiums_Frames)
		.Process(this->Pips_Tiberiums_EmptyFrame)
		.Process(this->Pips_Tiberiums_DisplayOrder)
		.Process(this->Pips_Tiberiums_WeedFrame)
		.Process(this->Pips_Tiberiums_WeedEmptyFrame)
		.Process(this->AirShadowBaseScale_log)
		.Process(this->HeightShadowScaling)
		.Process(this->HeightShadowScaling_MinScale)
		.Process(this->RemoveShroudGlobally)
		.Process(this->ExtendedAircraftMissions)
		.Process(this->AmphibiousEnter)
		.Process(this->AmphibiousUnload)
		.Process(this->NoQueueUpToEnter)
		.Process(this->NoQueueUpToUnload)
		.Process(this->BuildingProductionQueue)
		.Process(this->AllowParallelAIQueues)
		.Process(this->ForbidParallelAIQueues_Aircraft)
		.Process(this->ForbidParallelAIQueues_Building)
		.Process(this->ForbidParallelAIQueues_Infantry)
		.Process(this->ForbidParallelAIQueues_Navy)
		.Process(this->ForbidParallelAIQueues_Vehicle)
		.Process(this->EnablePowerSurplus)
		.Process(this->IronCurtain_KeptOnDeploy)
		.Process(this->IronCurtain_EffectOnOrganics)
		.Process(this->IronCurtain_KillOrganicsWarhead)
		.Process(this->ForceShield_KeptOnDeploy)
		.Process(this->ForceShield_EffectOnOrganics)
		.Process(this->ForceShield_KillOrganicsWarhead)
		.Process(this->IronCurtain_ExtraTintIntensity)
		.Process(this->ForceShield_ExtraTintIntensity)
		.Process(this->AllowWeaponSelectAgainstWalls)
		.Process(this->ColorAddUse8BitRGB)
		.Process(this->AirstrikeLineColor)
		.Process(this->AirstrikeLineZAdjust)
		.Process(this->ROF_RandomDelay)
		.Process(this->ToolTip_Background_Color)
		.Process(this->ToolTip_Background_Opacity)
		.Process(this->ToolTip_Background_BlurSize)
		.Process(this->DisplayIncome)
		.Process(this->DisplayIncome_AllowAI)
		.Process(this->DisplayIncome_Houses)
		.Process(this->CrateOnlyOnLand)
		.Process(this->UnitCrateVehicleCap)
		.Process(this->FreeMCV_CreditsThreshold)
		.Process(this->RadialIndicatorVisibility)
		.Process(this->DrawTurretShadow)
		.Process(this->IsVoiceCreatedGlobal)
		.Process(this->SelectionFlashDuration)
		.Process(this->DrawInsignia_OnlyOnSelected)
		.Process(this->DrawInsignia_AdjustPos_Infantry)
		.Process(this->DrawInsignia_AdjustPos_Buildings)
		.Process(this->DrawInsignia_AdjustPos_BuildingsAnchor)
		.Process(this->DrawInsignia_AdjustPos_Units)
		.Process(this->DrawInsignia_UsePixelSelectionBracketDelta)
		.Process(this->Promote_VeteranAnimation)
		.Process(this->Promote_EliteAnimation)
		.Process(this->AnimRemapDefaultColorScheme)
		.Process(this->TimerBlinkColorScheme)
		.Process(this->Buildings_DefaultDigitalDisplayTypes)
		.Process(this->Infantry_DefaultDigitalDisplayTypes)
		.Process(this->Vehicles_DefaultDigitalDisplayTypes)
		.Process(this->Aircraft_DefaultDigitalDisplayTypes)
		.Process(this->DefaultInfantrySelectBox)
		.Process(this->DefaultUnitSelectBox)
		.Process(this->VisualScatter_Min)
		.Process(this->VisualScatter_Max)
		.Process(this->ShowDesignatorRange)
		.Process(this->DropPodTrailer)
		.Process(this->DropPodDefaultTrailer)
		.Process(this->PodImage)
		.Process(this->JumpjetClimbPredictHeight)
		.Process(this->JumpjetClimbWithoutCutOut)
		.Process(this->DamageOwnerMultiplier)
		.Process(this->DamageAlliesMultiplier)
		.Process(this->DamageEnemiesMultiplier)
		.Process(this->DamageOwnerMultiplier_NotAffectsEnemies)
		.Process(this->DamageAlliesMultiplier_NotAffectsEnemies)
		.Process(this->Armor_FrontMultiplier)
		.Process(this->Armor_SideMultiplier)
		.Process(this->Armor_RearMultiplier)
		.Process(this->AIDirectionalArmor)
		.Process(this->AIDirectionalArmor_Chance)
		.Process(this->AIDirectionalArmor_Delay)
		.Process(this->AircraftLevelLightMultiplier)
		.Process(this->JumpjetLevelLightMultiplier)
		.Process(this->VoxelLightSource)
		// .Process(this->VoxelShadowLightSource)
		.Process(this->BuildingWaypoints)
		.Process(this->CombatAlert)
		.Process(this->CombatAlert_Default)
		.Process(this->CombatAlert_IgnoreBuilding)
		.Process(this->CombatAlert_SuppressIfInScreen)
		.Process(this->CombatAlert_Interval)
		.Process(this->CombatAlert_SuppressIfAllyDamage)
		.Process(this->CombatAlert_MakeAVoice)
		.Process(this->CombatAlert_UseFeedbackVoice)
		.Process(this->CombatAlert_UseAttackVoice)
		.Process(this->CombatAlert_UseEVA)
		.Process(this->UseFixedVoxelLighting)
		.Process(this->AIAutoDeployMCV)
		.Process(this->AISetBaseCenter)
		.Process(this->AIBiasSpawnCell)
		.Process(this->AIForbidConYard)
		.Process(this->AINodeWallsOnly)
		.Process(this->AICleanWallNode)
		.Process(this->AttackMove_Aggressive)
		.Process(this->AttackMove_UpdateTarget)
		.Process(this->MindControl_ThreatDelay)
		.Process(this->RecountBurst)
		.Process(this->NoRearm_UnderEMP)
		.Process(this->NoRearm_Temporal)
		.Process(this->NoReload_UnderEMP)
		.Process(this->NoReload_Temporal)
		.Process(this->NoTurret_TrackTarget)
		.Process(this->GatherWhenMCVDeploy)
		.Process(this->AIFireSale)
		.Process(this->AIFireSaleDelay)
		.Process(this->AIAllToHunt)
		.Process(this->RepairBaseNodes)
		.Process(this->WarheadParticleAlphaImageIsLightFlash)
		.Process(this->CombatLightDetailLevel)
		.Process(this->LightFlashAlphaImageDetailLevel)
		.Process(this->AINormalTargetingDelay)
		.Process(this->PlayerNormalTargetingDelay)
		.Process(this->AIGuardAreaTargetingDelay)
		.Process(this->PlayerGuardAreaTargetingDelay)
		.Process(this->AIAttackMoveTargetingDelay)
		.Process(this->PlayerAttackMoveTargetingDelay)
		.Process(this->DistributeTargetingFrame)
		.Process(this->DistributeTargetingFrame_AIOnly)
		.Process(this->BuildingTypeSelectable)
		.Process(this->ProneSpeed_Crawls)
		.Process(this->ProneSpeed_NoCrawls)
		.Process(this->DamagedSpeed)
		.Process(this->HarvesterScanAfterUnload)
		.Process(this->AnimCraterDestroyTiberium)
		.Process(this->BerzerkTargeting)
		.Process(this->TintColorIronCurtain)
		.Process(this->TintColorForceShield)
		.Process(this->TintColorBerserk)
		.Process(this->AttackMove_IgnoreWeaponCheck)
		.Process(this->AttackMove_StopWhenTargetAcquired)
		.Process(this->SmoothMove)
		.Process(this->TA_VirtualUnit)
		.Process(this->BetterPerformance)
		.Process(this->Tiberium_CanBeBuiltOn)
		.Process(this->Wall_CanBeBuiltOn)
		.Process(this->Rock_CanBeBuiltOn)
		.Process(this->CanBeBuiltOnOverlay_Remove)
		.Process(this->UrbanCombat_AttackBuff)
		.Process(this->UrbanCombat_Rate)
		.Process(this->UrbanCombat_OutsideDamageMultiplier)
		.Process(this->UrbanCombat_DefOutsideMulti)
		.Process(this->UrbanCombat_AttOutsideMulti)
		.Process(this->Parasite_GrappleAnim)
		.Process(this->InfantryAutoDeploy)
		.Process(this->AttachmentTopLayerMinHeight)
		.Process(this->AttachmentUndergroundLayerMaxHeight)
		.Process(this->BattlePoints)
		.Process(this->BattlePoints_DefaultValue)
		.Process(this->BattlePoints_DefaultFriendlyValue)
		.Process(this->PenetratesTransport_Level)
		
		.Process(this->FilterArtillery)
		.Process(this->FilterBomber)
		
		.Process(this->MovePositionIndicator)
		.Process(this->MovePositionIndicatorColor)
		.Process(this->MovePositionIndicator_ShowEnemy)
		.Process(this->DisableSaveGame)
		.Process(this->DisableLoadGame)
		.Process(this->TacticalPause_BlockActions)
		.Process(this->LockGameSpeed)
		.Process(this->FixOccupyFire)
		.Process(this->UseGlobalOccupyRange)
		;
}

bool RulesExt::ExtData::IsSaveGameDisabled() const
{
	if (this->DisableSaveGame.empty())
		return false;
	const int difficulty = GameOptionsClass::Instance.Difficulty;
	return std::find(this->DisableSaveGame.begin(), this->DisableSaveGame.end(), difficulty)
		!= this->DisableSaveGame.end();
}

bool RulesExt::ExtData::IsLoadGameDisabled() const
{
	if (this->DisableLoadGame.empty())
		return false;
	const int difficulty = GameOptionsClass::Instance.Difficulty;
	return std::find(this->DisableLoadGame.begin(), this->DisableLoadGame.end(), difficulty)
		!= this->DisableLoadGame.end();
}

bool RulesExt::ExtData::IsTacticalPauseBlockActionsEnabled() const
{
	if (this->TacticalPause_BlockActions.empty())
		return false;
	const int difficulty = GameOptionsClass::Instance.Difficulty;
	return std::find(this->TacticalPause_BlockActions.begin(), this->TacticalPause_BlockActions.end(), difficulty)
		!= this->TacticalPause_BlockActions.end();
}

bool RulesExt::ExtData::IsGameSpeedLocked() const
{
	if (this->LockGameSpeed.empty())
		return false;
	const int difficulty = GameOptionsClass::Instance.Difficulty;
	return std::find(this->LockGameSpeed.begin(), this->LockGameSpeed.end(), difficulty)
		!= this->LockGameSpeed.end();
}

void RulesExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<RulesClass>::LoadFromStream(Stm);
	this->Serialize(Stm);

	this->ReplaceVoxelLightSources();
}

void RulesExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<RulesClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

void RulesExt::ExtData::ReplaceVoxelLightSources()
{
	bool needCacheFlush = false;

	if (this->VoxelLightSource.isset())
	{
		needCacheFlush = true;
		auto source = this->VoxelLightSource.Get().Normalized();
		Game::VoxelLightSource = Matrix3D::VoxelDefaultMatrix * source;
	}

	/*
	// doesn't really impact anything from my testing - Kerbiter
	if (this->VoxelShadowLightSource.isset())
	{
		needCacheFlush = true;
		auto source = this->VoxelShadowLightSource.Get().Normalized();
		Game::VoxelShadowLightSource = Matrix3D::VoxelDefaultMatrix * source;
	}
	*/

	if (needCacheFlush)
		Game::DestroyVoxelCaches();
}

// =============================
// container hooks

DEFINE_HOOK(0x667A1D, RulesClass_CTOR, 0x5)
{
	GET(RulesClass*, pItem, ESI);

	RulesExt::Allocate(pItem);

	return 0;
}

DEFINE_HOOK(0x667A30, RulesClass_DTOR, 0x5)
{
	GET(RulesClass*, pItem, ECX);

	RulesExt::Remove(pItem);

	return 0;
}

IStream* RulesExt::g_pStm = nullptr;

DEFINE_HOOK_AGAIN(0x674730, RulesClass_SaveLoad_Prefix, 0x6)
DEFINE_HOOK(0x675210, RulesClass_SaveLoad_Prefix, 0x5)
{
	//GET(RulesClass*, pItem, ECX);
	GET_STACK(IStream*, pStm, 0x4);

	RulesExt::g_pStm = pStm;

	return 0;
}

DEFINE_HOOK(0x678841, RulesClass_Load_Suffix, 0x7)
{
	auto buffer = RulesExt::Global();

	PhobosByteStream Stm(0);
	if (Stm.ReadBlockFromStream(RulesExt::g_pStm))
	{
		PhobosStreamReader Reader(Stm);

		if (Reader.Expect(RulesExt::Canary) && Reader.RegisterChange(buffer))
			buffer->LoadFromStream(Reader);
	}

	return 0;
}

DEFINE_HOOK(0x675205, RulesClass_Save_Suffix, 0x8)
{
	auto buffer = RulesExt::Global();
	PhobosByteStream saver(sizeof(*buffer));
	PhobosStreamWriter writer(saver);

	writer.Expect(RulesExt::Canary);
	writer.RegisterChange(buffer);

	buffer->SaveToStream(writer);
	saver.WriteBlockToStream(RulesExt::g_pStm);

	return 0;
}

// DEFINE_HOOK(0x52D149, InitRules_PostInit, 0x5)
// {
// 	LaserTrailTypeClass::LoadFromINIList(&CCINIClass::INI_Art.get());
// 	return 0;
// }

DEFINE_HOOK(0x668BF0, RulesClass_Addition, 0x5)
{
	GET(RulesClass*, pItem, ECX);
	GET_STACK(CCINIClass*, pINI, 0x4);

	//	RulesClass::Initialized = false;
	RulesExt::LoadFromINIFile(pItem, pINI);

	return 0;
}

DEFINE_HOOK(0x679A15, RulesData_LoadBeforeTypeData, 0x6)
{
	GET(RulesClass*, pItem, ECX);
	GET_STACK(CCINIClass*, pINI, 0x4);

	//	RulesClass::Initialized = true;
	RulesExt::LoadBeforeTypeData(pItem, pINI);

	return 0;
}

DEFINE_HOOK(0x679CAF, RulesData_LoadAfterTypeData, 0x5)
{
	RulesClass* pItem = RulesClass::Instance;
	GET(CCINIClass*, pINI, ESI);

	RulesExt::LoadAfterTypeData(pItem, pINI);

	return 0;
}

DEFINE_HOOK(0x668F6A, RulesData_InitializeAfterAllLoaded, 0x5)
{
	RulesExt::Global()->InitializeAfterAllLoaded();
	return 0;
}

// Reenable obsolete [JumpjetControls] in RA2/YR
// Author: Uranusian
DEFINE_HOOK(0x7115AE, TechnoTypeClass_CTOR_JumpjetControls, 0xA)
{
	GET(TechnoTypeClass*, pThis, ESI);
	auto pRules = RulesClass::Instance;
	auto pRulesExt = RulesExt::Global();

	pThis->JumpjetTurnRate = pRules->TurnRate;
	pThis->JumpjetSpeed = pRules->Speed;
	pThis->JumpjetClimb = static_cast<float>(pRules->Climb);
	pThis->JumpjetCrash = static_cast<float>(pRulesExt->JumpjetCrash);
	pThis->JumpjetHeight = pRules->CruiseHeight;
	pThis->JumpjetAccel = static_cast<float>(pRules->Acceleration);
	pThis->JumpjetWobbles = static_cast<float>(pRules->WobblesPerSecond);
	pThis->JumpjetNoWobbles = pRulesExt->JumpjetNoWobbles;
	pThis->JumpjetDeviation = pRules->WobbleDeviation;

	return 0x711601;
}

DEFINE_HOOK(0x6744E4, RulesClass_ReadJumpjetControls_Extra, 0x7)
{
	auto pRulesExt = RulesExt::Global();
	if (!pRulesExt)
		return 0;

	GET(CCINIClass*, pINI, EDI);
	INI_EX exINI(pINI);

	pRulesExt->JumpjetCrash.Read(exINI, GameStrings::JumpjetControls, "Crash");
	pRulesExt->JumpjetNoWobbles.Read(exINI, GameStrings::JumpjetControls, "NoWobbles");

	return 0;
}

// skip vanilla JumpjetControls and make it earlier load
// DEFINE_JUMP(LJMP, 0x668EB5, 0x668EBD); // RulesClass_Process_SkipJumpjetControls // Really necessary? won't hurt to read again

void RulesExt::ApplyRemoveShroudGlobally()
{
	if (!RulesExt::Global() || !RulesExt::Global()->RemoveShroudGlobally)
		return;

	// Global shroud removal - use CellRangeIterator like the warhead does
	auto const& mapRect = MapClass::Instance.MapRect;

	// Calculate map center
	CellStruct mapCenter = {
		static_cast<short>(mapRect.X + mapRect.Width / 2),
		static_cast<short>(mapRect.Y + mapRect.Height / 2)
	};

	// Use a very large radius to cover entire map (max map size is around 200x200)
	const int radius = 200;

	CellRangeIterator<CellClass>{}(mapCenter, radius + 0.5, [](CellClass* pCell)
	{
		if (pCell)
		{
			// Use the exact same logic as the RemoveShroudOnly warhead
			while (pCell->ShroudCounter > 0)
				pCell->ReduceShroudCounter();

			// Clear gap coverage
			pCell->GapsCoveringThisCell = 0;

			// Update visibility without touching fog
			char visibility = TacticalClass::Instance->GetOcclusion(pCell->MapCoords, false);
			if (pCell->Visibility != visibility)
			{
				pCell->Visibility = visibility;
				TacticalClass::Instance->RegisterCellAsVisible(pCell);
			}
		}
		return true;
	});

	// Force full map redraw
	MapClass::Instance.MarkNeedsRedraw(2);
}
