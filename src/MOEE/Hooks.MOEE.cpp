#include "MOEE.h"

#include <Utilities/Macro.h>
#include <Unsorted.h>

// Per-frame tick for the ported MO_ExperienceEnhancements modules.
// LogicClass::Update (0x55B4E1) is the same address Phobos already uses for its
// other per-frame hooks; Syringe chains multiple hooks at one address.
DEFINE_HOOK(0x55B4E1, MOEE_PerFrame, 0x5)
{
	MOEE::Poll();
	return 0;
}

// Draw phase (same address Phobos uses for FlyingStrings / formation preview).
DEFINE_HOOK(0x6D4684, MOEE_DrawOverlays, 0x6)
{
	MOEE::Draw();
	return 0;
}
