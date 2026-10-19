#pragma once

#include "LoadBarSource.h"

class AttachEffectClass;
class LoadBarTypeClass;
class TechnoClass;

// Read-only value extraction for LoadBar. Nothing here writes to the
// simulation, allocates persistent state, or participates in savegames.
class LoadBarValue
{
public:
	// Fills out[0..maxOut) and returns the number of entries written.
	static int Get(TechnoClass* pThis, const LoadBarTypeClass& type,
		LoadBarEntry* out, int maxOut);

	// Total duration of an AttachEffect instance. Mirrors the initial/reapply
	// calculation in AttachEffectClass without adding any stored state.
	static int GetAETotal(AttachEffectClass* pAE, TechnoClass* pTechno);
};
