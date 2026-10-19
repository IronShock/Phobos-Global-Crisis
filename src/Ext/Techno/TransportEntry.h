#pragma once

class FootClass;
class UnitClass;

namespace PassengerEnterDirections
{
	// Returns false when the transport restricts entry directions and the
	// passenger is not approaching from an allowed side. Used by the vanilla
	// per-cell entry path so a disallowed approach cannot bypass the fast-entry
	// direction check.
	bool IsEntryAllowedByDirection(UnitClass* pTransport, FootClass* pPassenger,
		const char* gate = "entry");

	// Exact cargo-list membership check. Transporter/InLimbo are not reliable
	// for passengers in the engine's CargoClass, so re-entry must use this.
	bool IsPassengerInside(UnitClass* pTransport, FootClass* pPassenger);

	// Boarding gate shared by the fast-entry and engine entry paths. It
	// enforces the "at vehicle" rule and the per-transport entry interval.
	bool TryBeginEnter(UnitClass* pTransport, FootClass* pPassenger);

	// Per-transport unloading interval used by the fast-unload loop.
	bool TryBeginUnload(UnitClass* pTransport);
	void FinishUnload(UnitClass* pTransport);

}

namespace TechnoAttachmentTemp
{
	// The engine's normal passenger attachment path uses single-object mode.
	// Direct CargoClass::AddPassenger calls must use the same mode, otherwise
	// the passenger's NextObject chain is attached as cargo as well.
	void SetSingleObjectAttach(bool single);
}
