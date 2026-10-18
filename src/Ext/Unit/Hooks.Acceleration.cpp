#include <DriveLocomotionClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// The original call to SetSpeedPercentage has already completed at 0x4B1218.
// Override its result here instead of hooking the call itself, which keeps the
// original stack/argument cleanup untouched and avoids corrupting the guest stack.
DEFINE_HOOK(0x4B1218, DriveLocomotionClass_SetSpeed_Enhanced, 0x5)
{
	enum { Continue = 0x4B121D };

	GET(DriveLocomotionClass* const, pLoco, EBP);

	auto const pLinked = pLoco->LinkedTo;

	if (!pLinked)
		return Continue;

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pLinked->GetTechnoType());

	if (!pTypeExt || pTypeExt->Acceleration_Mode.Get() != 1)
	{
		R->ECX(pLinked);
		R->EDX(*reinterpret_cast<DWORD*>(pLinked));
		return Continue;
	}

	// An empty Destination (e.g. between path segments) would make the distance
	// nonsense; leave the engine's own value in place.
	if (pLoco->Destination == CoordStruct::Empty)
	{
		R->ECX(pLinked);
		R->EDX(*reinterpret_cast<DWORD*>(pLinked));
		return Continue;
	}

	CoordStruct coords = pLoco->Destination;
	coords.Z = MapClass::Instance.GetCellFloorHeight(coords);

	// GetCellAt can return null for coordinates outside the map.
	if (auto const pCell = MapClass::Instance.GetCellAt(coords))
	{
		if (pCell->ContainsBridge())
			coords.Z += CellClass::BridgeHeight;
	}

	const int distance = Game::F2I((pLinked->Location - coords).Magnitude());
	// Match the stock target: movementspeed_50 is the engine's own value for this
	// unit/cell. Using 1.0 overshoots vanilla; using anything lower is slower.
	const double speed = TechnoExt::UpdateEnhancedAcceleration(pLinked, pLoco->movementspeed_50, distance);
	pLinked->SetSpeedPercentage(speed);

	// Recreate the two instructions skipped by this 5-byte hook after any C++
	// calls: mov ecx, [ebp + 0xC] / mov edx, [ecx].
	R->ECX(pLinked);
	R->EDX(*reinterpret_cast<DWORD*>(pLinked));

	return Continue;
}
