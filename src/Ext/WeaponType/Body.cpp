#include "Body.h"
#include <Ext/Bullet/Body.h>
#include <Ext/Techno/Body.h>
#include <SpawnManagerClass.h>

WeaponTypeExt::ExtContainer WeaponTypeExt::ExtMap;

bool WeaponTypeExt::ExtData::HasRequiredAttachedEffects(TechnoClass* pTarget, TechnoClass* pFirer) const
{
	const bool hasRequiredTypes = this->AttachEffect_RequiredTypes.size() > 0;
	const bool hasDisallowedTypes = this->AttachEffect_DisallowedTypes.size() > 0;
	const bool hasRequiredGroups = this->AttachEffect_RequiredGroups.size() > 0;
	const bool hasDisallowedGroups = this->AttachEffect_DisallowedGroups.size() > 0;

	if (hasRequiredTypes || hasDisallowedTypes || hasRequiredGroups || hasDisallowedGroups)
	{
		auto pTechno = pTarget;

		if (this->AttachEffect_CheckOnFirer)
			pTechno = pFirer;

		if (!pTechno)
			return true;

		auto const pTechnoExt = TechnoExt::ExtMap.Find(pTechno);
		auto const pWH = this->OwnerObject()->Warhead;

		if (hasDisallowedTypes && pTechnoExt->HasAttachedEffects(this->AttachEffect_DisallowedTypes, false, this->AttachEffect_IgnoreFromSameSource, pFirer, pWH, &this->AttachEffect_DisallowedMinCounts, &this->AttachEffect_DisallowedMaxCounts))
			return false;

		if (hasDisallowedGroups && pTechnoExt->HasAttachedEffects(AttachEffectTypeClass::GetTypesFromGroups(this->AttachEffect_DisallowedGroups), false, this->AttachEffect_IgnoreFromSameSource, pFirer, pWH, &this->AttachEffect_DisallowedMinCounts, &this->AttachEffect_DisallowedMaxCounts))
			return false;

		if (hasRequiredTypes && !pTechnoExt->HasAttachedEffects(this->AttachEffect_RequiredTypes, true, this->AttachEffect_IgnoreFromSameSource, pFirer, pWH, &this->AttachEffect_RequiredMinCounts, &this->AttachEffect_RequiredMaxCounts))
			return false;

		if (hasRequiredGroups && !pTechnoExt->HasAttachedEffects(AttachEffectTypeClass::GetTypesFromGroups(this->AttachEffect_RequiredGroups), true, this->AttachEffect_IgnoreFromSameSource, pFirer, pWH, &this->AttachEffect_RequiredMinCounts, &this->AttachEffect_RequiredMaxCounts))
			return false;
	}

	return true;
}

bool WeaponTypeExt::ExtData::IsHealthInThreshold(TechnoClass* pTarget) const
{
	return TechnoExt::IsHealthInThreshold(pTarget, this->CanTarget_MinHealth, this->CanTarget_MaxHealth);
}

void WeaponTypeExt::ExtData::Initialize()
{
	this->RadType = RadTypeClass::FindOrAllocate(GameStrings::Radiation);
}

int WeaponTypeExt::ExtData::GetBurstDelay(int burstIndex) const
{
	int burstDelay = -1;

	if (burstIndex == 0)
		return 0;
	else if (this->Burst_Delays.size() > (unsigned)burstIndex)
		burstDelay = this->Burst_Delays[burstIndex - 1];
	else if (this->Burst_Delays.size() > 0)
		burstDelay = this->Burst_Delays[this->Burst_Delays.size() - 1];

	return burstDelay;
}

// =============================
// load / save

void WeaponTypeExt::ExtData::LoadFromINIFile(CCINIClass* const pINI)
{
	auto pThis = this->OwnerObject();
	const char* pSection = pThis->ID;
	INI_EX exINI(pINI);
	char tempBuffer[0x40];

	this->DiskLaser_Radius.Read(exINI, pSection, "DiskLaser.Radius");
	this->ProjectileRange.Read(exINI, pSection, "ProjectileRange");

	for (int idx = 0; idx < 3; ++idx)
	{
		_snprintf_s(tempBuffer, _TRUNCATE, "Bolt.Color%d", idx + 1);
		this->Bolt_Color[idx].Read(exINI, pSection, tempBuffer);

		_snprintf_s(tempBuffer, _TRUNCATE, "Bolt.Disable%d", idx + 1);
		this->Bolt_Disable[idx].Read(exINI, pSection, tempBuffer);
	}

	this->Bolt_ParticleSystem.Read(exINI, pSection, "Bolt.ParticleSystem");
	this->Bolt_Arcs.Read(exINI, pSection, "Bolt.Arcs");
	this->Bolt_Duration.Read(exINI, pSection, "Bolt.Duration");
	this->Bolt_FollowFLH.Read(exINI, pSection, "Bolt.FollowFLH");

	this->RadType.Read<true>(exINI, pSection, "RadType");

	this->Strafing.Read(exINI, pSection, "Strafing");
	this->Strafing_Shots.Read(exINI, pSection, "Strafing.Shots");
	this->Strafing_SimulateBurst.Read(exINI, pSection, "Strafing.SimulateBurst");
	this->Strafing_UseAmmoPerShot.Read(exINI, pSection, "Strafing.UseAmmoPerShot");
	this->Strafing_TargetCell.Read(exINI, pSection, "Strafing.TargetCell");
	this->Strafing_EndDelay.Read(exINI, pSection, "Strafing.EndDelay");
	this->CanTarget.Read(exINI, pSection, "CanTarget");
	this->CanTargetHouses.Read(exINI, pSection, "CanTargetHouses");
	this->CanTarget_MaxHealth.Read(exINI, pSection, "CanTarget.MaxHealth");
	this->CanTarget_MinHealth.Read(exINI, pSection, "CanTarget.MinHealth");
	this->Burst_Delays.Read(exINI, pSection, "Burst.Delays");
	this->Burst_FireWithinSequence.Read(exINI, pSection, "Burst.FireWithinSequence");
	this->Burst_NoDelay.Read(exINI, pSection, "Burst.NoDelay");
	this->AreaFire_Target.Read(exINI, pSection, "AreaFire.Target");
	this->FeedbackWeapon.Read<true>(exINI, pSection, "FeedbackWeapon");
	this->Next.Read<true>(exINI, pSection, "Next");

	// ExtraFire0, ExtraFire1, ... (ExtraFire is an alias for ExtraFire0). Read contiguous
	// entries until the first missing index (safety-capped at MaxExtraFire).
	for (int i = 0; i < WeaponTypeExt::MaxExtraFire; ++i)
	{
		Nullable<WeaponTypeClass*> extra;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d", i);
		extra.Read<true>(exINI, pSection, tempBuffer);

		if (!extra.isset() && i == 0)
			extra.Read<true>(exINI, pSection, "ExtraFire");

		if (!extra.isset())
		{
			// Distinguish "there is no such index" (end of the contiguous block, stop) from
			// "the index exists but names a weapon that does not" (a typo, which must not
			// silently drop this and every later entry). The latter is reported and kept as
			// an empty slot so the parallel arrays stay aligned.
			char keyBuffer[0x40];
			_snprintf_s(keyBuffer, _TRUNCATE, "ExtraFire%d", i);

			bool keyPresent = exINI.ReadString(pSection, keyBuffer) && exINI.value()[0] != '\0';

			if (!keyPresent && i == 0)
				keyPresent = exINI.ReadString(pSection, "ExtraFire") && exINI.value()[0] != '\0';

			if (!keyPresent)
				break;

			Debug::Log("[ExtraFire] [%s]ExtraFire%d names a weapon that does not exist; the entry is skipped.\n",
				pSection, i);
		}

		this->ExtraFire_Weapons.push_back(extra.Get());

		Valueable<bool> ownTarget { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.OwnTarget", i);
		ownTarget.Read(exINI, pSection, tempBuffer);
		if (i == 0) ownTarget.Read(exINI, pSection, "ExtraFire.OwnTarget");
		this->ExtraFire_OwnTarget.push_back(ownTarget.Get());

		Nullable<CoordStruct> flh;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.FLH", i);
		flh.Read(exINI, pSection, tempBuffer);
		if (!flh.isset() && i == 0) flh.Read(exINI, pSection, "ExtraFire.FLH");
		this->ExtraFire_FLHs.push_back(flh);

		Nullable<Leptons> extraRange;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Range", i);
		extraRange.Read(exINI, pSection, tempBuffer);
		if (!extraRange.isset() && i == 0) extraRange.Read(exINI, pSection, "ExtraFire.Range");
		this->ExtraFire_Ranges.push_back(extraRange);

		Valueable<bool> onTurret { true };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.OnTurret", i);
		onTurret.Read(exINI, pSection, tempBuffer);
		if (i == 0) onTurret.Read(exINI, pSection, "ExtraFire.OnTurret");
		this->ExtraFire_OnTurret.push_back(onTurret.Get());

		Valueable<bool> useAmmo { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.UseAmmo", i);
		useAmmo.Read(exINI, pSection, tempBuffer);
		if (i == 0) useAmmo.Read(exINI, pSection, "ExtraFire.UseAmmo");
		this->ExtraFire_UseAmmo.push_back(useAmmo.Get());

		Valueable<bool> checkParentRange { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.CheckParentRange", i);
		checkParentRange.Read(exINI, pSection, tempBuffer);
		if (i == 0) checkParentRange.Read(exINI, pSection, "ExtraFire.CheckParentRange");
		this->ExtraFire_CheckParentRange.push_back(checkParentRange.Get());

		Valueable<bool> checkFacing { true };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.CheckFacing", i);
		checkFacing.Read(exINI, pSection, tempBuffer);
		if (i == 0) checkFacing.Read(exINI, pSection, "ExtraFire.CheckFacing");
		this->ExtraFire_CheckFacing.push_back(checkFacing.Get());

		Valueable<bool> onlyWhenParentFires { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.OnlyWhenParentFires", i);
		onlyWhenParentFires.Read(exINI, pSection, tempBuffer);
		if (i == 0) onlyWhenParentFires.Read(exINI, pSection, "ExtraFire.OnlyWhenParentFires");
		this->ExtraFire_OnlyWhenParentFires.push_back(onlyWhenParentFires.Get());

		Valueable<bool> autoTarget { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.AutoTarget", i);
		autoTarget.Read(exINI, pSection, tempBuffer);
		if (i == 0) autoTarget.Read(exINI, pSection, "ExtraFire.AutoTarget");
		this->ExtraFire_AutoTarget.push_back(autoTarget.Get());

		Valueable<bool> useAmmo2 { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.UseAmmo2", i);
		useAmmo2.Read(exINI, pSection, tempBuffer);
		if (i == 0) useAmmo2.Read(exINI, pSection, "ExtraFire.UseAmmo2");
		this->ExtraFire_UseAmmo2.push_back(useAmmo2.Get());

		Valueable<bool> turnToTarget { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.TurnToTarget", i);
		turnToTarget.Read(exINI, pSection, tempBuffer);
		if (i == 0) turnToTarget.Read(exINI, pSection, "ExtraFire.TurnToTarget");
		this->ExtraFire_TurnToTarget.push_back(turnToTarget.Get());

		Valueable<int> facingTolerance { WeaponTypeExt::DefaultFacingTolerance };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.FacingTolerance", i);
		facingTolerance.Read(exINI, pSection, tempBuffer);
		if (i == 0) facingTolerance.Read(exINI, pSection, "ExtraFire.FacingTolerance");
		this->ExtraFire_FacingTolerances.push_back(facingTolerance.Get());

		Valueable<double> damageMultiplier { 1.0 };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.DamageMultiplier", i);
		damageMultiplier.Read(exINI, pSection, tempBuffer);
		if (i == 0) damageMultiplier.Read(exINI, pSection, "ExtraFire.DamageMultiplier");
		this->ExtraFire_DamageMultipliers.push_back(damageMultiplier.Get());

		Valueable<double> chance { 1.0 };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Chance", i);
		chance.Read(exINI, pSection, tempBuffer);
		if (i == 0) chance.Read(exINI, pSection, "ExtraFire.Chance");
		this->ExtraFire_Chances.push_back(chance.Get());

		Valueable<int> turretROT { 0 };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Turret.ROT", i);
		turretROT.Read(exINI, pSection, tempBuffer);
		if (i == 0) turretROT.Read(exINI, pSection, "ExtraFire.Turret.ROT");
		this->ExtraFire_TurretROTs.push_back(turretROT.Get());

		Valueable<bool> turret { false };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Turret", i);
		turret.Read(exINI, pSection, tempBuffer);
		if (i == 0) turret.Read(exINI, pSection, "ExtraFire.Turret");
		this->ExtraFire_Turrets.push_back(turret.Get());

		Valueable<bool> imageHouseColor { true };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.HouseColor", i);
		imageHouseColor.Read(exINI, pSection, tempBuffer);
		if (i == 0) imageHouseColor.Read(exINI, pSection, "ExtraFire.Image.HouseColor");
		this->ExtraFire_Image_HouseColors.push_back(imageHouseColor.Get());

		Valueable<Point2D> imageOffset { { 0, 0 } };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.Offset", i);
		imageOffset.Read(exINI, pSection, tempBuffer);
		if (i == 0) imageOffset.Read(exINI, pSection, "ExtraFire.Image.Offset");
		this->ExtraFire_Image_Offsets.push_back(imageOffset.Get());

		// The model is referenced by UnitType ID and resolved lazily, because weapon sections
		// are parsed before the techno type arrays are populated.
		std::string imageID;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image", i);

		if (exINI.ReadString(pSection, tempBuffer) && exINI.value()[0])
			imageID = exINI.value();
		else if (i == 0 && exINI.ReadString(pSection, "ExtraFire.Image") && exINI.value()[0])
			imageID = exINI.value();

		this->ExtraFire_ImageIDs.push_back(imageID);

		Nullable<CoordStruct> imageFLH;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.FLH", i);
		imageFLH.Read(exINI, pSection, tempBuffer);
		if (!imageFLH.isset() && i == 0) imageFLH.Read(exINI, pSection, "ExtraFire.Image.FLH");
		this->ExtraFire_Image_FLHs.push_back(imageFLH);

		Nullable<bool> imageOnTurret;
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.IsOnTurret", i);
		imageOnTurret.Read(exINI, pSection, tempBuffer);
		if (!imageOnTurret.isset() && i == 0) imageOnTurret.Read(exINI, pSection, "ExtraFire.Image.IsOnTurret");
		this->ExtraFire_Image_OnTurrets.push_back(imageOnTurret);

		Valueable<int> imageRotationAdjust { 0 };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.RotationAdjust", i);
		imageRotationAdjust.Read(exINI, pSection, tempBuffer);
		if (i == 0) imageRotationAdjust.Read(exINI, pSection, "ExtraFire.Image.RotationAdjust");
		this->ExtraFire_Image_RotationAdjusts.push_back(imageRotationAdjust.Get());

		Valueable<bool> imageMuzzle { true };
		_snprintf_s(tempBuffer, _TRUNCATE, "ExtraFire%d.Image.Muzzle", i);
		imageMuzzle.Read(exINI, pSection, tempBuffer);
		if (i == 0) imageMuzzle.Read(exINI, pSection, "ExtraFire.Image.Muzzle");
		this->ExtraFire_Image_Muzzles.push_back(imageMuzzle.Get());
	}
	this->Laser_IsSingleColor.Read(exINI, pSection, "IsSingleColor");
	this->VisualScatter.Read(exINI, pSection, "VisualScatter");
	this->ROF_RandomDelay.Read(exINI, pSection, "ROF.RandomDelay");
	this->ChargeTurret_Delays.Read(exINI, pSection, "ChargeTurret.Delays");
	this->OmniFire_TurnToTarget.Read(exINI, pSection, "OmniFire.TurnToTarget");
	this->FireOnce_ResetSequence.Read(exINI, pSection, "FireOnce.ResetSequence");
	this->TurretRecoil_Suppress.Read(exINI, pSection, "TurretRecoil.Suppress");
	this->ExtraWarheads.Read(exINI, pSection, "ExtraWarheads");
	this->ExtraWarheads_DamageOverrides.Read(exINI, pSection, "ExtraWarheads.DamageOverrides");
	this->ExtraWarheads_DetonationChances.Read(exINI, pSection, "ExtraWarheads.DetonationChances");
	this->ExtraWarheads_FullDetonation.Read(exINI, pSection, "ExtraWarheads.FullDetonation");
	this->AmbientDamage_Warhead.Read<true>(exINI, pSection, "AmbientDamage.Warhead");
	this->AmbientDamage_IgnoreTarget.Read(exINI, pSection, "AmbientDamage.IgnoreTarget");
	this->AttachEffect_RequiredTypes.Read(exINI, pSection, "AttachEffect.RequiredTypes");
	this->AttachEffect_DisallowedTypes.Read(exINI, pSection, "AttachEffect.DisallowedTypes");
	exINI.ParseStringList(this->AttachEffect_RequiredGroups, pSection, "AttachEffect.RequiredGroups");
	exINI.ParseStringList(this->AttachEffect_DisallowedGroups, pSection, "AttachEffect.DisallowedGroups");
	this->AttachEffect_RequiredMinCounts.Read(exINI, pSection, "AttachEffect.RequiredMinCounts");
	this->AttachEffect_RequiredMaxCounts.Read(exINI, pSection, "AttachEffect.RequiredMaxCounts");this->DelayedFire_Duration.Read(exINI, pSection, "DelayedFire.Duration");
	this->AttachEffect_DisallowedMinCounts.Read(exINI, pSection, "AttachEffect.DisallowedMinCounts");
	this->AttachEffect_DisallowedMaxCounts.Read(exINI, pSection, "AttachEffect.DisallowedMaxCounts");
	this->AttachEffect_CheckOnFirer.Read(exINI, pSection, "AttachEffect.CheckOnFirer");
	this->AttachEffect_IgnoreFromSameSource.Read(exINI, pSection, "AttachEffect.IgnoreFromSameSource");
	this->KeepRange.Read(exINI, pSection, "KeepRange");
	this->KeepRange_AllowAI.Read(exINI, pSection, "KeepRange.AllowAI");
	this->KeepRange_AllowPlayer.Read(exINI, pSection, "KeepRange.AllowPlayer");
	this->KeepRange_EarlyStopFrame.Read(exINI, pSection, "KeepRange.EarlyStopFrame");
	this->KickOutPassengers.Read(exINI, pSection, "KickOutPassengers");
	this->Beam_Color.Read(exINI, pSection, "Beam.Color");
	this->Beam_Duration.Read(exINI, pSection, "Beam.Duration");
	this->Beam_Amplitude.Read(exINI, pSection, "Beam.Amplitude");
	this->Beam_IsHouseColor.Read(exINI, pSection, "Beam.IsHouseColor");
	this->LaserThickness.Read(exINI, pSection, "LaserThickness");
	this->DelayedFire_Duration.Read(exINI, pSection, "DelayedFire.Duration");
	this->DelayedFire_SkipInTransport.Read(exINI, pSection, "DelayedFire.SkipInTransport");
	this->DelayedFire_Animation.Read(exINI, pSection, "DelayedFire.Animation");
	this->DelayedFire_OpenToppedAnimation.Read(exINI, pSection, "DelayedFire.OpenToppedAnimation");
	this->DelayedFire_AnimIsAttached.Read(exINI, pSection, "DelayedFire.AnimIsAttached");
	this->DelayedFire_CenterAnimOnFirer.Read(exINI, pSection, "DelayedFire.CenterAnimOnFirer");
	this->DelayedFire_RemoveAnimOnNoDelay.Read(exINI, pSection, "DelayedFire.RemoveAnimOnNoDelay");
	this->DelayedFire_PauseFiringSequence.Read(exINI, pSection, "DelayedFire.PauseFiringSequence");
	this->DelayedFire_OnlyOnInitialBurst.Read(exINI, pSection, "DelayedFire.OnlyOnInitialBurst");
	this->DelayedFire_AnimOffset.Read(exINI, pSection, "DelayedFire.AnimOffset");
	this->DelayedFire_AnimOnTurret.Read(exINI, pSection, "DelayedFire.AnimOnTurret");

	// handle SkipWeaponPicking
	if (this->CanTarget != AffectedTarget::All || this->CanTargetHouses != AffectedHouse::All
		|| this->CanTarget_MaxHealth < 1.0 || this->CanTarget_MinHealth > 0.0
		|| this->AttachEffect_RequiredTypes.size() || this->AttachEffect_RequiredGroups.size()
		|| this->AttachEffect_DisallowedTypes.size() || this->AttachEffect_DisallowedGroups.size())
	{
		this->SkipWeaponPicking = false;
	}
}

template <typename T>
void WeaponTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->DiskLaser_Radius)
		.Process(this->ProjectileRange)
		.Process(this->Bolt_Color)
		.Process(this->Bolt_Disable)
		.Process(this->Bolt_ParticleSystem)
		.Process(this->Bolt_Arcs)
		.Process(this->Bolt_Duration)
		.Process(this->Bolt_FollowFLH)
		.Process(this->Strafing)
		.Process(this->Strafing_Shots)
		.Process(this->Strafing_SimulateBurst)
		.Process(this->Strafing_UseAmmoPerShot)
		.Process(this->Strafing_TargetCell)
		.Process(this->Strafing_EndDelay)
		.Process(this->CanTarget)
		.Process(this->CanTargetHouses)
		.Process(this->CanTarget_MaxHealth)
		.Process(this->CanTarget_MinHealth)
		.Process(this->RadType)
		.Process(this->Burst_Delays)
		.Process(this->Burst_FireWithinSequence)
		.Process(this->Burst_NoDelay)
		.Process(this->AreaFire_Target)
		.Process(this->FeedbackWeapon)
		.Process(this->Next)
		.Process(this->ExtraFire_Weapons)
		.Process(this->ExtraFire_FLHs)
		.Process(this->ExtraFire_Ranges)
		.Process(this->ExtraFire_OwnTarget)
		.Process(this->ExtraFire_OnTurret)
		.Process(this->ExtraFire_UseAmmo)
		.Process(this->ExtraFire_CheckParentRange)
		.Process(this->ExtraFire_CheckFacing)
		.Process(this->Laser_IsSingleColor)
		.Process(this->VisualScatter)
		.Process(this->ROF_RandomDelay)
		.Process(this->ChargeTurret_Delays)
		.Process(this->OmniFire_TurnToTarget)
		.Process(this->FireOnce_ResetSequence)
		.Process(this->TurretRecoil_Suppress)
		.Process(this->ExtraWarheads)
		.Process(this->ExtraWarheads_DamageOverrides)
		.Process(this->ExtraWarheads_DetonationChances)
		.Process(this->ExtraWarheads_FullDetonation)
		.Process(this->AmbientDamage_Warhead)
		.Process(this->AmbientDamage_IgnoreTarget)
		.Process(this->AttachEffect_RequiredTypes)
		.Process(this->AttachEffect_DisallowedTypes)
		.Process(this->AttachEffect_RequiredGroups)
		.Process(this->AttachEffect_DisallowedGroups)
		.Process(this->AttachEffect_RequiredMinCounts)
		.Process(this->AttachEffect_RequiredMaxCounts)
		.Process(this->AttachEffect_DisallowedMinCounts)
		.Process(this->AttachEffect_DisallowedMaxCounts)
		.Process(this->AttachEffect_CheckOnFirer)
		.Process(this->AttachEffect_IgnoreFromSameSource)
		.Process(this->KeepRange)
		.Process(this->KeepRange_AllowAI)
		.Process(this->KeepRange_AllowPlayer)
		.Process(this->KeepRange_EarlyStopFrame)
		.Process(this->KickOutPassengers)
		.Process(this->Beam_Color)
		.Process(this->Beam_Duration)
		.Process(this->Beam_Amplitude)
		.Process(this->Beam_IsHouseColor)
		.Process(this->LaserThickness)
		.Process(this->SkipWeaponPicking)
		.Process(this->DelayedFire_Duration)
		.Process(this->DelayedFire_SkipInTransport)
		.Process(this->DelayedFire_Animation)
		.Process(this->DelayedFire_OpenToppedAnimation)
		.Process(this->DelayedFire_AnimIsAttached)
		.Process(this->DelayedFire_CenterAnimOnFirer)
		.Process(this->DelayedFire_RemoveAnimOnNoDelay)
		.Process(this->DelayedFire_PauseFiringSequence)
		.Process(this->DelayedFire_OnlyOnInitialBurst)
		.Process(this->DelayedFire_AnimOffset)
		.Process(this->DelayedFire_AnimOnTurret)
		// NOTE: new fields must always be appended at the very end of this chain. The
		// reader compares the block size it consumed against the stored one, so inserting
		// anywhere but the end invalidates every pre-existing savegame.
		.Process(this->ExtraFire_OnlyWhenParentFires)
		.Process(this->ExtraFire_AutoTarget)
		.Process(this->ExtraFire_UseAmmo2)
		.Process(this->ExtraFire_TurnToTarget)
		.Process(this->ExtraFire_FacingTolerances)
		.Process(this->ExtraFire_DamageMultipliers)
		.Process(this->ExtraFire_Chances)
		.Process(this->ExtraFire_Turrets)
		.Process(this->ExtraFire_TurretROTs)
		.Process(this->ExtraFire_Image_HouseColors)
		.Process(this->ExtraFire_Image_Offsets)
		.Process(this->ExtraFire_ImageIDs)
		.Process(this->ExtraFire_Image_FLHs)
		.Process(this->ExtraFire_Image_OnTurrets)
		.Process(this->ExtraFire_Image_RotationAdjusts)
		.Process(this->ExtraFire_Image_Muzzles)
		;
};

// ExtraFire mount model: resolve the UnitType whose art this entry draws. Deferred to first
// use because weapon types are parsed before techno types exist, and cached afterwards since
// TechnoTypeClass instances are stable for the session. A non-UnitType or unknown ID logs
// once and yields nullptr, so no model is drawn for that entry.
TechnoTypeClass* WeaponTypeExt::ExtData::GetExtraFireImageType(int index) const
{
	if (index < 0 || index >= static_cast<int>(this->ExtraFire_ImageIDs.size()))
		return nullptr;

	if (index >= static_cast<int>(this->ExtraFire_ImageResolved.size()))
	{
		this->ExtraFire_ImageResolved.resize(this->ExtraFire_ImageIDs.size(), 0);
		this->ExtraFire_ImageTypes.resize(this->ExtraFire_ImageIDs.size(), nullptr);
	}

	if (!this->ExtraFire_ImageResolved[index])
	{
		this->ExtraFire_ImageResolved[index] = 1;

		auto const& imageID = this->ExtraFire_ImageIDs[index];

		if (!imageID.empty())
		{
			if (auto const pType = TechnoTypeClass::Find(imageID.c_str()))
			{
				// Only UnitTypes carry Facings / StandingFrames / StartStandFrame, which is
				// how the frame for a given facing is derived. Anything else has no model.
				if (pType->WhatAmI() == AbstractType::UnitType)
					this->ExtraFire_ImageTypes[index] = pType;
				else
					Debug::Log("[ExtraFire] Image=%s is not a UnitType, no model will be drawn.\n", imageID.c_str());
			}
			else
			{
				Debug::Log("[ExtraFire] Image=%s could not be resolved to a TechnoType.\n", imageID.c_str());
			}
		}
	}

	return this->ExtraFire_ImageTypes[index];
}




void WeaponTypeExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<WeaponTypeClass>::LoadFromStream(Stm);
	this->Serialize(Stm);

}

void WeaponTypeExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<WeaponTypeClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool WeaponTypeExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm
		.Process(OldRadius)
		.Success();
}

bool WeaponTypeExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm
		.Process(OldRadius)
		.Success();
}

void WeaponTypeExt::DetonateAt(WeaponTypeClass* pThis, AbstractClass* pTarget, TechnoClass* pOwner, HouseClass* pFiringHouse)
{
	WeaponTypeExt::DetonateAt(pThis, pTarget->GetCoords(), pOwner, pThis->Damage, pFiringHouse, pTarget);
}

void WeaponTypeExt::DetonateAt(WeaponTypeClass* pThis, AbstractClass* pTarget, TechnoClass* pOwner, int damage, HouseClass* pFiringHouse)
{
	WeaponTypeExt::DetonateAt(pThis, pTarget->GetCoords(), pOwner, damage, pFiringHouse, pTarget);
}

void WeaponTypeExt::DetonateAt(WeaponTypeClass* pThis, const CoordStruct& coords, TechnoClass* pOwner, HouseClass* pFiringHouse, AbstractClass* pTarget)
{
	WeaponTypeExt::DetonateAt(pThis, coords, pOwner, pThis->Damage, pFiringHouse, pTarget);
}

void WeaponTypeExt::DetonateAt(WeaponTypeClass* pThis, const CoordStruct& coords, TechnoClass* pOwner, int damage, HouseClass* pFiringHouse, AbstractClass* pTarget)
{
	BulletExt::Detonate(coords, pOwner, damage, pFiringHouse, pTarget, pThis->Bright, pThis, pThis->Warhead);
}

int WeaponTypeExt::GetRangeWithModifiers(WeaponTypeClass* pThis, TechnoClass* pFirer)
{
	int range = 0;

	if (!pThis && !pFirer)
		return range;
	else if (pFirer && pFirer->CanOccupyFire())
		range = RulesClass::Instance->OccupyWeaponRange * Unsorted::LeptonsPerCell;
	else if (pThis && pFirer)
		range = pThis->Range;
	else
		return range;

	if (range == -512)
		return range;

	return WeaponTypeExt::GetRangeWithModifiers(pThis, pFirer, range);
}

int WeaponTypeExt::GetRangeWithModifiers(WeaponTypeClass* pThis, TechnoClass* pFirer, int range)
{
	auto pTechno = pFirer;

	if (pTechno->Transporter)
	{
		auto const pType = pTechno->Transporter->GetTechnoType();

		if (pType->OpenTopped)
		{
			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

			if (pTypeExt->OpenTopped_UseTransportRangeModifiers)
				pTechno = pTechno->Transporter;
		}
	}

	auto const pTechnoExt = TechnoExt::ExtMap.Find(pTechno);

	if (!pTechnoExt->AE.HasRangeModifier)
		return range;

	int extraRange = 0;

	for (auto const& attachEffect : pTechnoExt->AttachedEffects)
	{
		if (!attachEffect->IsActive())
			continue;

		auto const type = attachEffect->GetType();

		if (type->WeaponRange_Multiplier == 1.0 && type->WeaponRange_ExtraRange == 0.0)
			continue;

		if (type->WeaponRange_AllowWeapons.size() > 0 && !type->WeaponRange_AllowWeapons.Contains(pThis))
			continue;

		if (type->WeaponRange_DisallowWeapons.size() > 0 && type->WeaponRange_DisallowWeapons.Contains(pThis))
			continue;

		range = static_cast<int>(range * Math::max(type->WeaponRange_Multiplier, 0.0));
		extraRange += static_cast<int>(type->WeaponRange_ExtraRange * Unsorted::LeptonsPerCell);
	}

	range += extraRange;

	return Math::max(range, 0);
}

int WeaponTypeExt::GetTechnoKeepRange(WeaponTypeClass* pThis, TechnoClass* pFirer, bool isMinimum)
{
	if (!pThis)
		return 0;

	const auto pExt = WeaponTypeExt::ExtMap.Find(pThis);
	const auto keepRange = pExt->KeepRange.Get();

	if (!keepRange || !pFirer || pFirer->Transporter)
		return 0;

	const auto absType = pFirer->WhatAmI();

	if (absType != AbstractType::Infantry && absType != AbstractType::Unit)
		return 0;

	const auto pHouse = pFirer->Owner;

	if (pHouse && pHouse->IsControlledByHuman())
	{
		if (!pExt->KeepRange_AllowPlayer)
			return 0;
	}
	else if (!pExt->KeepRange_AllowAI)
	{
		return 0;
	}

	if (pFirer->RearmTimer.GetTimeLeft() < pExt->KeepRange_EarlyStopFrame)
		return 0;

	if (!pFirer->RearmTimer.InProgress())
	{
		const auto spawnManager = pFirer->SpawnManager;

		if (!spawnManager || spawnManager->Status != SpawnManagerStatus::CoolDown)
			return 0;

		const auto spawnsNumber = pFirer->GetTechnoType()->SpawnsNumber;

		for (int i = 0; i < spawnsNumber; i++)
		{
			const auto status = spawnManager->SpawnedNodes[i]->Status;

			if (status == SpawnNodeStatus::TakeOff || status == SpawnNodeStatus::Returning)
				return 0;
		}
	}

	if (isMinimum)
		return (keepRange > 0) ? keepRange : 0;

	if (keepRange > 0)
		return 0;

	const auto checkRange = -keepRange - 128;
	const auto pTarget = pFirer->Target;

	if (pTarget && pFirer->DistanceFrom(pTarget) >= checkRange)
		return (checkRange > 443) ? checkRange : 443; // 1.73 * Unsorted::LeptonsPerCell

	return -keepRange;
}

// =============================
// container

WeaponTypeExt::ExtContainer::ExtContainer() : Container("WeaponTypeClass") { }

WeaponTypeExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x771EE9, WeaponTypeClass_CTOR, 0x5)
{
	GET(WeaponTypeClass*, pItem, ESI);

	WeaponTypeExt::ExtMap.TryAllocate(pItem);

	return 0;
}

DEFINE_HOOK(0x77311D, WeaponTypeClass_SDDTOR, 0x6)
{
	GET(WeaponTypeClass*, pItem, ESI);

	WeaponTypeExt::ExtMap.Remove(pItem);

	return 0;
}

DEFINE_HOOK_AGAIN(0x772EB0, WeaponTypeClass_SaveLoad_Prefix, 0x5)
DEFINE_HOOK(0x772CD0, WeaponTypeClass_SaveLoad_Prefix, 0x7)
{
	GET_STACK(WeaponTypeClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);

	WeaponTypeExt::ExtMap.PrepareStream(pItem, pStm);

	return 0;
}

DEFINE_HOOK(0x772EA6, WeaponTypeClass_Load_Suffix, 0x6)
{
	WeaponTypeExt::ExtMap.LoadStatic();

	return 0;
}

DEFINE_HOOK(0x772F8C, WeaponTypeClass_Save, 0x5)
{
	WeaponTypeExt::ExtMap.SaveStatic();

	return 0;
}

//DEFINE_HOOK_AGAIN(0x7729D6, WeaponTypeClass_LoadFromINI, 0x5)// Section dont exist!
DEFINE_HOOK_AGAIN(0x7729C7, WeaponTypeClass_LoadFromINI, 0x5)
DEFINE_HOOK(0x7729B0, WeaponTypeClass_LoadFromINI, 0x5)
{
	GET(WeaponTypeClass*, pItem, ESI);
	GET_STACK(CCINIClass*, pINI, 0xE4);

	WeaponTypeExt::ExtMap.LoadFromINI(pItem, pINI);

	return 0;
}
