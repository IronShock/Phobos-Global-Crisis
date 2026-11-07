#include "Fog.h"

// DisplayClass::ProcessClickCoords resolves the clicked object and then marks
// the cell as "no target" when the building in it has IsFogged set (0x6e7).
// Fogged buildings are still drawn as frozen snapshots, so a click on one must
// resolve to the building instead of becoming a move order.
DEFINE_HOOK(0x6924CB, Fog_ProcessClickCoords_FoggedBuilding, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(BuildingClass*, pBuilding, EAX);

	if (pBuilding && pBuilding->IsFogged)
	{
		R->CL(0);
		return 0x6924D1;
	}

	return 0;
}
