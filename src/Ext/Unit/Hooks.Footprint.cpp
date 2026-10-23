#include <Ext/Unit/Footprint.h>

#include <CellClass.h>
#include <ObjectClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <Locomotion/FootprintLocomotionClass.h>

#include <Helpers/Macro.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// The type INI is normally enough, but forcing at construction guarantees the
// hull never inherits the vanilla Ship/Drive locomotor if Ares re-reads the
// Locomotor key after the type extension is loaded. Hook after the initial
// `mov eax, [esp+8]` (pType), before the FootClass base constructor runs.
DEFINE_HOOK(0x7353C4, UnitClass_Constructor_ForceFootprintLocomotion, 0x5)
{
	GET(UnitTypeClass*, pType, EAX);

	if (Footprint::IsMultiCell(pType))
		pType->Locomotor = __uuidof(FootprintLocomotionClass);

	return 0;
}

// After UnitClass::UpdateRotation has updated the body/turret facing, keep the
// collision footprint in sync with a stationary hull turning to aim.
DEFINE_HOOK(0x736BE2, UnitClass_UpdateRotation_FootprintAim, 0x6)
{
	GET(UnitClass*, pThis, ESI);

	Footprint::ReconcileFacing(pThis);

	return 0;
}

// The engine reads the vehicle occupation bit from the cell's
	// OccupationFlags. For a multi-cell unit every hull cell has 0x20 set but
	// only the anchor cell contains the object, so IsCellOccupied would reject
	// the unit's own trailing cells. Clear the bit for the querying owner.
	DEFINE_HOOK(0x73FC38, UnitClass_IsCellOccupied_IgnoreOwnFootprint, 0xB)
	{
		enum { ContinueCheck = 0x73FC43, Proceed = 0x73FD37 };

		GET(UnitClass*, pThis, EBX);
		GET(CellClass*, pCell, EDI);

		auto* const pFlags = reinterpret_cast<byte*>(R->ESP() + 0x14);
		const bool alt = *reinterpret_cast<byte*>(R->ESP() + 0x13) != 0;

		if (Footprint::IsMultiCell(pThis) && Footprint::IsOwnCell(pThis, pCell, alt))
			*pFlags &= ~0x20;

		return (*pFlags & 0x3F) == 0 ? Proceed : ContinueCheck;
	}

	// CellClass::GetUnit only walks the cell's object list. For a multi-cell
	// unit the surrounding cells are reserved but contain no object, so return
	// the footprint owner as a fallback.
	UnitClass* __fastcall CellClass_GetUnit_Reimpl(CellClass* pThis, void*, bool alt)
	{
		// Replicates the engine's early-out flag.
		if (!Game::IsActive)
			return nullptr;

		ObjectClass* pObject = alt ? pThis->AltObject : pThis->FirstObject;

		while (pObject)
		{
			if (pObject->WhatAmI() == AbstractType::Unit)
				return static_cast<UnitClass*>(pObject);

			pObject = pObject->NextObject;
		}

		if (auto const pOwner = Footprint::OwnerAt(pThis, alt))
			return pOwner;

		// Self-heal a grid that was cleared or loaded out of sync: the cell still
		// carries the vehicle bit, so find the hull whose span contains it.
		const DWORD flags = alt ? pThis->AltOccupationFlags : pThis->OccupationFlags;

		if (flags & 0x20)
			return Footprint::FindOwnerByScan(pThis->MapCoords, alt);

		return nullptr;
	}

	DEFINE_FUNCTION_JUMP(LJMP, 0x47EBA0, CellClass_GetUnit_Reimpl);

	// Safe counterpart for mouse selection and cell-targeted attacks. The hook
	// covers exactly one 5-byte instruction (`mov al, [0xA8E9A0]`), so it cannot
	// corrupt the following branch the way the old 2-byte hook did. It only reads
	// the footprint table; nothing is inserted into the cell's object list.
	DEFINE_HOOK(0x47C5E1, CellClass_GetSomeObject_FootprintFallback, 0x5)
	{
		enum { ReturnObject = 0x47C609, Continue = 0x47C5E6 };

		GET(CellClass*, pCell, EDI);

		if (auto const pOwner = Footprint::OwnerAt(pCell, false))
		{
			R->EAX(pOwner);
			return ReturnObject;
		}

		if (auto const pOwner = Footprint::OwnerAt(pCell, true))
		{
			R->EAX(pOwner);
			return ReturnObject;
		}

		return Continue;
	}

