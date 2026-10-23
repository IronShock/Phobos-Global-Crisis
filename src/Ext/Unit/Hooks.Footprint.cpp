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

		return Footprint::OwnerAt(pThis, alt);
	}

	DEFINE_FUNCTION_JUMP(LJMP, 0x47EBA0, CellClass_GetUnit_Reimpl);

