#pragma once

// [GC-diag] Structural validation for CCINIClass objects. Implemented in
// Phobos.Ext.cpp; used from existing scenario/overlay hooks to report an INI
// whose section/entry nodes were corrupted (e.g. a Value pointer turned into a
// small wild value such as 0x21) before it crashes the engine.
class CCINIClass;

namespace Diagnostics
{
	void ValidateINI(const char* tag, CCINIClass* pINI);
	void ValidateAllINIs(const char* where);

	// [GC-diag] Validate the map/scenario INI currently being parsed (armed via
	// SetCurrentMapINI) at a named stage of map loading, to localize which step
	// first corrupts its section/entry lists.
	void ValidateMapINI(const char* stage);

	// Arms the INIEntry destructor probe with the INI currently being parsed.
	void SetCurrentMapINI(CCINIClass* pINI);

	// [GC] Clears the current-map INI pointer held by the diagnostics.
	void ClearCurrentMapINI();

	// [GC] True if the INI currently has a section with this name (bounded walk,
	// name compared case-insensitively). Used as a gate so a checkpoint in a hook
	// that also runs during startup only fires for a map INI (which has
	// "IsoMapPack5"), never for Rules/Art/...
	bool IniHasSection(CCINIClass* pINI, const char* name);

	// [GC] True if the named section's Entries list has any corrupt node.
	bool SectionListCorrupt(CCINIClass* pINI, const char* name);
}

// [GC] Defined in Hooks.INIInheritance.cpp. Resets Phobos's static, cross-load
// INI include/inheritance caches so a reused CCINIClass address or stale
// $Inherits/$Include state from a previous scenario cannot corrupt the next
// scenario's map INI (observed as deterministic crash on the 2nd restart).
void ResetINIInheritanceCaches();
