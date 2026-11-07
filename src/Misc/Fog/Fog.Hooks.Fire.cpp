#include "Fog.h"

#include <ObjectClass.h>

// RevealFirer -----------------------------------------------------------------
//
// The vanilla fire path reveals shroud around the firer twice:
//   * MapClass::RevealArea1 (radius 3) when WeaponType.RevealOnFire is set;
//   * MapClass::RevealArea3 (radius 4) unconditionally in one Fire overload.
// With RevealFirer enabled both are skipped. Instead, an enemy firing from an
// explored fog cell becomes visible to the local player for a short time while
// the fog layer itself is left untouched.

namespace
{
	bool RevealFirerActive()
	{
		return Phobos::Config::TSFog_RevealFirer && Fog::Active();
	}
}

// TechnoClass::Fire overload A: skip the conditional RevealArea1 (0x6FDF30).
// Steals `mov edx,[esi]; push 0; push 1` (6 bytes) so replaying on the inactive
// path lands on the next instruction rather than inside Syringe's 5-byte jump.
DEFINE_HOOK(0x6FDF11, Fog_RevealFirer_SkipArea1_A, 0x6)
{
	if (!RevealFirerActive())
		return 0;

	return 0x6FDF35;
}

// TechnoClass::Fire overload A: RevealArea3 follows unconditionally. The same
// address is the join target when RevealOnFire is false, so this also handles
// the "no conditional reveal" path.
DEFINE_HOOK(0x6FDF35, Fog_RevealFirer_SkipArea3_A, 0x6)
{
	if (!RevealFirerActive())
		return 0;

	GET(TechnoClass*, pFirer, ESI);
	Fog::RevealFirer(pFirer);

	return 0x6FDF52;
}

// TechnoClass::Fire overload B: skip the conditional RevealArea1 (0x6FF71B).
DEFINE_HOOK(0x6FF6FC, Fog_RevealFirer_SkipArea1_B, 0x6)
{
	if (!RevealFirerActive())
		return 0;

	return 0x6FF720;
}

// TechnoClass::Fire overload B: skip RevealArea3 (0x6FF738).
DEFINE_HOOK(0x6FF720, Fog_RevealFirer_SkipArea3_B, 0x6)
{
	if (!RevealFirerActive())
		return 0;

	return 0x6FF73D;
}

// TechnoClass::Fire overload B: join point after both reveals. Reachable both
// from the skipped block and from the early "no reveal" branches, so this is the
// single place that installs the fog-aware reveal.
DEFINE_HOOK(0x6FF73D, Fog_RevealFirer_Reveal_B, 0x6)
{
	if (!RevealFirerActive())
		return 0;

	GET(TechnoClass*, pFirer, ESI);
	Fog::RevealFirer(pFirer);

	return 0; // replay `mov edx, [0xa8ed84]`
}

// MapClass::IsLocationFogged only receives coordinates, so the object being
// drawn is recorded right before the three draw-loop calls. IsLocationFoggedImpl
// consumes and clears it, which lets RevealFirer show the firing unit itself
// while other enemies sharing the cell stay hidden.
DEFINE_HOOK(0x6D8FD4, Fog_RevealFirer_DrawContext_A, 0x5)
{
	if (!RevealFirerActive())
		return 0;

	GET(ObjectClass*, pObject, ESI);
	Fog::CurrentDrawUniqueID = pObject ? pObject->UniqueID : 0;

	return 0;
}

DEFINE_HOOK(0x6D9209, Fog_RevealFirer_DrawContext_B, 0x5)
{
	if (!RevealFirerActive())
		return 0;

	GET(ObjectClass*, pObject, ESI);
	Fog::CurrentDrawUniqueID = pObject ? pObject->UniqueID : 0;

	return 0;
}

DEFINE_HOOK(0x6D929A, Fog_RevealFirer_DrawContext_C, 0x5)
{
	if (!RevealFirerActive())
		return 0;

	GET(ObjectClass*, pObject, ESI);
	Fog::CurrentDrawUniqueID = pObject ? pObject->UniqueID : 0;

	return 0;
}
