// Anim-to--Unit
// Author: Otamaa, revisions by Starkku

#include "Body.h"

#include <Phobos.h>
#include <BulletClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <CellClass.h>
#include <OverlayClass.h>
#include <OverlayTypeClass.h>
#include <SmudgeTypeClass.h>
#include <BasicStructures.h>

#include <Ext/Cell/Body.h>
#include <Ext/Bullet/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/AnimType/Body.h>

// Add a smudge to a cell: the native slot (CellClass::SmudgeTypeIndex) holds the
// first one, further ones (up to [General] SmudgeCapacity in total) are stacked in
// CellExt::ExtraSmudges and rendered alongside the native smudge.
static void AddSmudgeToCell(CellClass* pCell, SmudgeTypeClass* pType)
{
	if (!pCell || !pType)
		return;

	if (pCell->SmudgeTypeIndex == -1)
	{
		pCell->SmudgeTypeIndex = pType->ArrayIndex;
		return;
	}

	// The native smudge counts as one of the capacity; the rest go to ExtraSmudges.
	auto const pCellExt = CellExt::ExtMap.Find(pCell);
	const int extraCapacity = Phobos::Config::SmudgeCapacity - 1 > 0 ? Phobos::Config::SmudgeCapacity - 1 : 0;
	if (pCellExt && static_cast<int>(pCellExt->ExtraSmudges.size()) < extraCapacity)
		pCellExt->ExtraSmudges.push_back(pType);
}

DEFINE_HOOK(0x737F6D, UnitClass_TakeDamage_Destroy, 0x7)
{
	GET(UnitClass* const, pThis, ESI);
	REF_STACK(args_ReceiveDamage const, Receivedamageargs, STACK_OFFSET(0x44, 0x4));

	R->ECX(R->ESI());
	TechnoExt::ExtMap.Find(pThis)->ReceiveDamage = true;
	AnimTypeExt::ProcessDestroyAnims(pThis, Receivedamageargs.Attacker);
	pThis->Destroy();

	return 0x737F74;
}

DEFINE_HOOK(0x738807, UnitClass_Destroy_DestroyAnim, 0x8)
{
	GET(UnitClass* const, pThis, ESI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (!pExt->ReceiveDamage)
		AnimTypeExt::ProcessDestroyAnims(pThis);

	return 0x73887E;
}

// Performance tweak, mark once instead of every frame.
// DEFINE_HOOK(0x423BC8, AnimClass_AI_CreateUnit_MarkOccupationBits, 0x6)
DEFINE_HOOK(0x4226F0, AnimClass_CTOR_CreateUnit_MarkOccupationBits, 0x6)
{
	GET(AnimClass* const, pThis, ESI);

	auto const pTypeExt = AnimTypeExt::ExtMap.Find(pThis->Type);

	if (pTypeExt->CreateUnitType)
		pThis->MarkAllOccupationBits(pThis->GetCell()->GetCoordsWithBridge());

	return 0; //return (pThis->Type->MakeInfantry != -1) ? 0x423BD6 : 0x423C03;
}

DEFINE_HOOK(0x424932, AnimClass_AI_CreateUnit_ActualEffects, 0x6)
{
	GET(AnimClass* const, pThis, ESI);

	auto const pType = pThis->Type;
	auto const pTypeExt = AnimTypeExt::ExtMap.Find(pType);

	if (auto const pCreateUnit = pTypeExt->CreateUnitType.get())
	{
		auto const pUnitType = pCreateUnit->Type;
		auto const pExt = AnimExt::ExtMap.Find(pThis);
		pThis->UnmarkAllOccupationBits(pThis->GetCell()->GetCoordsWithBridge());

		auto const facing = pCreateUnit->RandomFacing
			? static_cast<DirType>(ScenarioClass::Instance->Random.RandomRanged(0, 255)) : pCreateUnit->Facing;

		auto const primaryFacing = pCreateUnit->InheritDeathFacings && pExt->FromDeathUnit ? pExt->DeathUnitFacing : facing;
		DirType* secondaryFacing = nullptr;

		if (pUnitType->WhatAmI() == AbstractType::UnitType && pUnitType->Turret && pExt->FromDeathUnit && pExt->DeathUnitHasTurret && pCreateUnit->InheritTurretFacings)
		{
			auto dir = pExt->DeathUnitTurretFacing.GetDir();
			secondaryFacing = &dir;
			Debug::Log("CreateUnit: Using stored turret facing %d from anim [%s]\n", pExt->DeathUnitTurretFacing.GetFacing<256>(), pType->get_ID());
		}

		TechnoTypeExt::CreateUnit(pCreateUnit, primaryFacing, secondaryFacing, pThis->Location, pThis->Owner, pExt->Invoker, pExt->InvokerHouse);
	}

	// CreateOverlay: place the configured overlay on the anim's cell when it ends.
	// Note: the OverlayClass constructor's 3rd parameter is a placement flag, not
	// the frame. The game draws a cell's overlay using CellClass::OverlayData as
	// the frame (see the wall/overlay drawing hooks), so the frame is applied to
	// the cell's OverlayData instead.
	if (auto const pCreateOverlay = pTypeExt->CreateOverlayType.get())
	{
		if (auto const pCell = pThis->GetCell())
		{
			if (pCreateOverlay->Overwrite || pCell->OverlayTypeIndex == -1)
			{
				GameCreate<OverlayClass>(pCreateOverlay->Type, pCell->MapCoords, -1);

				// CreateOverlay.Frame: select the overlay's frame (-1 = the
				// type's default frame, OverlayData stays 0).
				if (pCreateOverlay->Frame >= 0)
					pCell->OverlayData = static_cast<unsigned char>(pCreateOverlay->Frame & 0xFF);
			}
		}
	}

	// CreateSmudge: add the configured smudge(s) to the anim's cell when it ends.
	// The cell's native slot (CellClass::SmudgeTypeIndex) takes the first smudge
	// (bottom); further ones (up to CreateSmudge.Capacity in total) are stacked in
	// CellExt::ExtraSmudges and rendered above the previous ones, in list order.
	// Overwrite=yes replaces the whole stack with just these smudges.
	if (auto const pCreateSmudge = pTypeExt->CreateSmudgeType.get())
	{
		if (auto const pCell = pThis->GetCell())
		{
			if (pCreateSmudge->Overwrite)
			{
				if (auto const pCellExt = CellExt::ExtMap.Find(pCell))
					pCellExt->ExtraSmudges.clear();

				pCell->SmudgeTypeIndex = -1;
			}

			for (auto const pSmudge : pCreateSmudge->Type)
				AddSmudgeToCell(pCell, pSmudge);
		}
	}

	return (pType->MakeInfantry != -1) ? 0x42493E : 0x424B31;
}
