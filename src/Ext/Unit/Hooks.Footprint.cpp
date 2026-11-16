#include <Ext/Unit/Footprint.h>

#include <Windows.h>

#include <CellClass.h>
#include <ObjectClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <WWMouseClass.h>

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

// DisplayClass::ProcessClickCoords is also used while the cursor is merely
// hovering over the tactical map. Capturing a "click" there made any mouse
// movement over water launch every selected parked payload. Only treat the
// mouse as commanding while the left button is down (plus a short tail so the
// engine's button-up handler still sees the command frame).
static bool IsTacticalCommandClick()
{
	static bool s_wasDown = false;
	static bool s_dragged = false;
	static Point2D s_press { 0, 0 };
	static int s_window = 0;

	const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

	Point2D current { 0, 0 };

	if (WWMouseClass::Instance)
		WWMouseClass::Instance->GetCoords(&current);

	if (down && !s_wasDown)
	{
		// Capture the press cell too: some commands execute on button-down.
		// A drag cancels the window on the next call (see below), so a clean
		// click still works both on press and on release.
		s_press = current;
		s_dragged = false;
		s_window = 1;
	}
	else if (down)
	{
		const int dx = current.X > s_press.X ? current.X - s_press.X : s_press.X - current.X;
		const int dy = current.Y > s_press.Y ? current.Y - s_press.Y : s_press.Y - current.Y;

		if (dx > 4 || dy > 4)
		{
			s_dragged = true;
			s_window = 0;
		}
	}
	else if (s_wasDown)
	{
		// Release of a clean click: keep the window for the few calls the
		// engine uses to execute the command after button-up.
		s_window = s_dragged ? 0 : 3;
	}
	else if (s_window > 0)
	{
		--s_window;
	}

	s_wasDown = down;

	return s_window > 0;
}

// DisplayClass::ProcessClickCoords applies selectability filters after the
// tactical target has been resolved. A peripheral hull cell can pass the
// target lookup but fail those filters because the object's own cell is the
// anchor. Accept a controlled multi-cell unit directly; nothing is written to
// any cell list.
DEFINE_HOOK(0x692522, DisplayClass_ProcessClickCoords_AcceptFootprintUnit, 0x5)
{
	enum { Success = 0x6925F0, Continue = 0x692527 };

	const bool commandClick = IsTacticalCommandClick();

	// DisplayClass::ProcessClickCoords already resolved the hovered cell.
	// Use that cell instead of recomputing it from WWMouse coordinates: the
	// screen/client conversion was off by TacticalPos, which made every
	// peripheral carrier cell resolve to the wrong place (only the anchor
	// cell worked) and broke normal move/takeoff clicks.
	GET_STACK(CellStruct*, pResolvedCell, 0x30);

	if (pResolvedCell && pResolvedCell->X >= 0 && pResolvedCell->Y >= 0)
		Footprint::SetLastMouseCell(*pResolvedCell);

	auto const ppTarget = reinterpret_cast<ObjectClass**>(R->EDI());
	auto const pTarget = *ppTarget;

	if (commandClick && pTarget && abstract_cast<TechnoClass*>(pTarget)
		&& !abstract_cast<AircraftClass*>(pTarget))
	{
		auto const pTechnoTarget = abstract_cast<TechnoClass*>(pTarget);
		// A click on some other techno is an attack order for a parked payload.
		Footprint::CaptureAttackClick(pTechnoTarget);

		// A click on the carrier records a deferred landing intent. Do not
		// null the target: the engine's normal Enter/Return mission is caught
		// by AircraftClass_QueueMission and converted to the same intent.
		Footprint::CaptureCarrierReturnClick(pTechnoTarget);
	}

	if (commandClick && !pTarget && pResolvedCell)
		Footprint::CaptureClickDestination(*pResolvedCell);

	if (pTarget)
	{
		auto const pTechno = abstract_cast<TechnoClass*>(pTarget);

		// Terrain and other non-Techno objects do not have a TechnoType. The
		// vanilla filter would dereference GetTechnoType() unconditionally and
		// crash (C0000005 at 0x692575). Drop such targets here.
		if (!pTechno || !pTechno->GetTechnoType())
		{
			*ppTarget = nullptr;
			return Success;
		}

		auto const pUnit = abstract_cast<UnitClass*>(pTechno);

		if (pUnit
			&& pUnit->IsAlive
			&& !pUnit->InLimbo
			&& Footprint::IsMultiCell(pUnit)
			&& pUnit->Owner
			&& pUnit->Owner->IsControlledByCurrentPlayer())
		{
			return Success;
		}
	}

	R->ESI(R->EAX());
	return Continue;
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


