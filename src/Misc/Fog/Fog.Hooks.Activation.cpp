#include "Fog.h"

// Scenario / game-mode fog decision. Campaign maps keep their own setting (TUTO4
// writes [SpecialFlags] FogOfWar=no and must stay fully revealed); skirmish and
// multiplayer may turn fog on through the game-mode option.
DEFINE_HOOK(0x687C56, Fog_ReadScenario_Decide, 0x5)
{
	bool fog = ScenarioClass::Instance->SpecialFlags.FogOfWar;

	if (!SessionClass::IsCampaign())
		fog = fog || GameModeOptionsClass::Instance.FogOfWar;

	if (Phobos::Config::TSFog_ForceAll)
		fog = true;

	fog = fog && Phobos::Config::TSFog;

	GameModeOptionsClass::Instance.FogOfWar = fog;
	ScenarioClass::Instance->SpecialFlags.FogOfWar = fog;

	Fog::ResetExplored();
	Fog::ResetProducers();
	Fog::ResetFirerReveals();

	if (Phobos::Config::TSFog_AllowAnim)
		Fog::RebuildWarheadAnimTypes();

	if (fog)
		Fog::InstallIsLocationFogged();

	return 0;
}

// A "Reveal all" trigger clears shroud and fog on every cell (including the three
// special cells the native loop skips).
DEFINE_HOOK(0x577EE9, Fog_RevealWholeMap_ClearCell, 0x5)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass* const, pCell, EAX);

	pCell->Flags = (pCell->Flags & ~CellFlags::Fogged) | CellFlags::CenterRevealed | CellFlags::EdgeRevealed;
	pCell->Visibility = -1;
	pCell->Foggedness = -1;
	pCell->ClearFoggedObjects();

	return 0;
}

// End of DisplayClass::RevealWholeMap. A map that reveals everything is meant to
// stay revealed, so lift fog for the rest of the scenario instead of letting the
// next spread pass re-fog it.
DEFINE_HOOK(0x577F14, Fog_RevealWholeMap_After, 0x5)
{
	if (!Fog::Active() || !Phobos::Config::TSFog_RevealAllClear)
		return 0;

	GameModeOptionsClass::Instance.FogOfWar = false;
	ScenarioClass::Instance->SpecialFlags.FogOfWar = false;

	return 0;
}

// Defeat / observer: reveal everything, same as the native accept-defeat path.
DEFINE_HOOK(0x4FC200, Fog_AcceptDefeat_Reveal, 0x5)
{
	if (!Fog::Active())
		return 0;

	Fog::ClearAllFog();
	return 0;
}

// --- Per-frame fog driver ---
//
// FogProducer coverage is refreshed and newly revealed cells are remembered.
// No cell fields are touched from this hook, so the normal fog path stays
// exactly as the engine renders it.
DEFINE_HOOK(0x55B4E1, Fog_PerFrame, 0x5)
{
	if (!Fog::Active())
		return 0;

	// FogProducer coverage / radius growth. This also updates AnyGlobalProducer,
	// which decides whether the engine's normal fog logic is forced on.
	Fog::UpdateProducers();

	// Only the custom AutoSpread=no renderer needs the independent sight map.
	// Global mode runs the engine's native fog and must not rebuild it.
	if (!Fog::UsesNativeFog())
	{
		if (Fog::SightDirty || (Unsorted::CurrentFrame % 3) == 0)
		{
			Fog::UpdateSightMap();
			Fog::SightDirty = false;
		}
	}

	// Remember revealed cells while their reveal flags are still set.
	if ((Unsorted::CurrentFrame % 15) == 0)
		Fog::ScanExploredCells();

	Fog::UpdateFirerReveals();

	return 0;
}

// ScenarioClass::LoadGame success epilogue (0x67E440). Continue Game and the
// CnCNet client call LoadGame directly, bypassing the in-game load dialog's
// return point at 0x559DCA, so the restore has to live on the function itself.
DEFINE_HOOK(0x67E720, Fog_LoadGame_RestoreState, 0x6)
{
	if ((R->EBX() & 0xFF) == 0)
		return 0;

	Fog::OnGameLoaded();

	return 0; // replay pop edi; pop esi; mov al,bl; pop ebp; pop ebx
}
