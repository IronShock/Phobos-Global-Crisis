#include <Ext/Unit/Footprint.h>

#include <Windows.h>

#include <CellClass.h>
#include <ObjectClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <AircraftClass.h>
#include <Unsorted.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <WWMouseClass.h>

#include <Locomotion/FootprintLocomotionClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

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
	static int s_cleanReleaseFrame = -1000000;

	const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

	Point2D current { 0, 0 };

	if (WWMouseClass::Instance)
		WWMouseClass::Instance->GetCoords(&current);

	if (down && !s_wasDown)
	{
		// Do not issue anything on press: a press may still become a drag
		// (selection box / map scroll). The command is captured on release.
		s_press = current;
		s_dragged = false;
		s_cleanReleaseFrame = -1000000;
	}
	else if (down)
	{
		const int dx = current.X > s_press.X ? current.X - s_press.X : s_press.X - current.X;
		const int dy = current.Y > s_press.Y ? current.Y - s_press.Y : s_press.Y - current.Y;

		if (dx > 4 || dy > 4)
		{
			s_dragged = true;
			s_cleanReleaseFrame = -1000000;
		}
	}
	else if (s_wasDown)
	{
		// Release of a clean click. Keep a frame-based window instead of a
		// call-count window: ProcessClickCoords is not called exactly the
		// same number of times every frame and a low frame rate could expire
		// the old 3-call window before the engine executed the command.
		s_cleanReleaseFrame = s_dragged ? -1000000 : Unsorted::CurrentFrame;

		if (!s_dragged)
			Footprint::MarkCommandClick();
	}

	s_wasDown = down;

	return s_cleanReleaseFrame >= 0
		&& Unsorted::CurrentFrame <= s_cleanReleaseFrame + 3;
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

	auto const ppTarget = reinterpret_cast<ObjectClass**>(R->EDI());
	auto pTarget = *ppTarget;

	// TacticalClass::SelectAt only knows the object's single selectable point
	// (the anchor/SHP centre). A click on any other hull cell returns null and
	// the multi-cell unit cannot be selected by a single click. Resolve the
	// footprint owner from the click coordinate and feed it to the normal
	// selection filter below.
	if (!pTarget && TacticalClass::Instance)
	{
		Point2D clickPoint { 0, 0 };
		bool hasClickPoint = false;

		if (auto const pClick = reinterpret_cast<Point2D*>(R->EBP()))
		{
			clickPoint = *pClick;
			hasClickPoint = true;
		}
		else if (WWMouseClass::Instance)
		{
			WWMouseClass::Instance->GetCoords(&clickPoint);
			hasClickPoint = true;
		}

		if (hasClickPoint)
		{
			const auto world = TacticalClass::Instance->ClientToCoords(clickPoint);
			const auto cell = CellClass::Coord2Cell(world);

			if (auto const pFootprint = Footprint::FindMultiCellOwnerAt(cell))
			{
				*ppTarget = pFootprint;
				R->EAX(pFootprint);
				pTarget = pFootprint;

				Debug::CarrierLog("[DiagClick] hull-cell fallback f=%d cell=(%d,%d) owner=%s\n",
					Unsorted::CurrentFrame, cell.X, cell.Y,
					pFootprint->GetTechnoType() ? pFootprint->GetTechnoType()->ID : "?");
			}
		}
	}

	// The engine may resolve a click on a carrier hull cell to the carrier
	// itself, hiding an enemy aircraft parked on that cell. Prefer the parked
	// payload as the target so ground units can attack it directly.
	if (TacticalClass::Instance)
	{
		Point2D clickPoint { 0, 0 };
		bool hasClickPoint = false;

		if (auto const pClick = reinterpret_cast<Point2D*>(R->EBP()))
		{
			clickPoint = *pClick;
			hasClickPoint = true;
		}
		else if (WWMouseClass::Instance)
		{
			WWMouseClass::Instance->GetCoords(&clickPoint);
			hasClickPoint = true;
		}

		if (hasClickPoint)
		{
			bool preferPresent = !pTarget;

			if (pTarget && pTarget->WhatAmI() == AbstractType::Unit)
				preferPresent = Footprint::IsMultiCell(static_cast<UnitClass*>(pTarget));

			if (preferPresent
				&& (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
			{
				// Only override the engine's carrier target while the player is
				// force-attacking; a normal attack must still hit the carrier.
				const auto world = TacticalClass::Instance->ClientToCoords(clickPoint);
				const auto cell = CellClass::Coord2Cell(world);

				if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
				{
					bool found = false;

					for (int pass = 0; pass < 2 && !found; ++pass)
					{
						ObjectClass* pObject = pass == 0 ? pCell->FirstObject : pCell->AltObject;

						for (; pObject; pObject = pObject->NextObject)
						{
							auto const pAircraft = abstract_cast<AircraftClass*>(pObject);

							if (!pAircraft || !Footprint::IsCarrierPayloadParked(pAircraft))
								continue;

							auto const pOwner = pAircraft->Owner;

							if (!pOwner || !HouseClass::CurrentPlayer)
								continue;

							*ppTarget = pAircraft;
							R->EAX(pAircraft);
							pTarget = pAircraft;
							found = true;

							Debug::CarrierLog("[DiagClick] prefer parked payload %s at cell=(%d,%d)\n",
								pAircraft->Type ? pAircraft->Type->ID : "?", cell.X, cell.Y);

							break;
						}
					}

					// The deck pin moves payloads with SetLocation, so the cell
					// object list can be stale. Find the same parked aircraft by
					// its current Location cell.
					if (!found)
					{
						for (int i = 0; i < AircraftClass::Array.Count; ++i)
						{
							auto const pAircraft = AircraftClass::Array.GetItem(i);

							if (!pAircraft || !pAircraft->IsAlive
								|| !Footprint::IsCarrierPayloadParked(pAircraft))
							{
								continue;
							}

							if (CellClass::Coord2Cell(pAircraft->Location) != cell)
								continue;

							auto const pOwner = pAircraft->Owner;

							if (!pOwner || !HouseClass::CurrentPlayer)
								continue;

							*ppTarget = pAircraft;
							R->EAX(pAircraft);
							pTarget = pAircraft;
							found = true;

							Debug::CarrierLog("[DiagClick] prefer parked payload %s by location at cell=(%d,%d)\n",
								pAircraft->Type ? pAircraft->Type->ID : "?", cell.X, cell.Y);

							break;
						}
					}
				}
			}
		}
	}

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

	// A command click on a parked carrier payload is an explicit attack order.
	// Record the player's attacker -> target pairs so only these (and not
	// AI/auto-acquired shots) may bypass the engine's weapon checks.
	if (commandClick && pTarget)
	{
		if (auto const pTechnoTarget = abstract_cast<TechnoClass*>(pTarget);
			pTechnoTarget && Footprint::IsCarrierPayloadParked(pTechnoTarget))
		{
			Footprint::RecordParkedPayloadAttackClick(pTechnoTarget);
		}
	}

	if (commandClick && !pTarget && WWMouseClass::Instance && TacticalClass::Instance)
	{
		Point2D mouse { 0, 0 };
		WWMouseClass::Instance->GetCoords(&mouse);

		if (mouse.X != 0 || mouse.Y != 0)
		{
			const auto world = TacticalClass::Instance->ClientToCoords(mouse);
			Footprint::CaptureClickDestination(CellClass::Coord2Cell(world));
		}
	}

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

	// UnitClass::DrawAsSHP has just finished computing the body frame in EBX.
	// Remember it so the island overlay reuses exactly the same facing frame.
	// ECX still holds the type pointer for the instruction we return to.
	DEFINE_HOOK(0x73C725, UnitClass_DrawAsSHP_CaptureCarrierIslandFrame, 0x6)
	{
		GET(UnitClass*, pThis, EBP);
		GET(int, frame, EBX);

		Footprint::CaptureCarrierIslandFrame(pThis, frame);
		R->ECX(pThis->GetTechnoType());

		return 0;
	}

	static void __fastcall DrawCarrierIslandForUnit(UnitClass* pThis)
	{
		Footprint::DrawCarrierIslandOverlay(pThis);
	}

	// Shared epilogue of UnitClass::Draw_It. The original bytes restore the
	// callee-saved registers; replicate them after painting the island so the
	// overlay sits immediately behind the hull in the world draw order.
	static void __declspec(naked) UnitClass_Draw_It_IslandEpilogue()
	{
		__asm
		{
			pushad
			mov ecx, esi
			call DrawCarrierIslandForUnit
			popad
			pop edi
			pop esi
			pop ebp
			pop ebx
			add esp, 0x40
			ret 8
		}
	}

	DEFINE_HOOK(0x73D43F, UnitClass_Draw_It_CarrierIsland, 0xA)
	{
		return reinterpret_cast<uintptr_t>(&UnitClass_Draw_It_IslandEpilogue);
	}


