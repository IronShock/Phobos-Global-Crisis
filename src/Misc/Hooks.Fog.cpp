#include <Phobos.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <GameModeOptionsClass.h>
#include <DisplayClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <Unsorted.h>

// Fog of war, stage A: activation + reveal model only.
//
// Ported from wenrui1245/Phobos-Recya (GPL-3.0). Rewritten so that:
//   * every hook body starts with Fog::Active(); when fog is off it returns 0 and
//     the trampoline replays the original instructions unchanged;
//   * no hook steals a relative branch or relative call (Syringe does not relocate
//     those, so replaying them in the trampoline jumps into unmapped memory).
//
// Later stages (draw skips, frozen objects, voxel snapshots) are added only after
// this stage is verified against a fog-less mission (TUTO4) and a fogged skirmish.

namespace Fog
{
	bool NoReveal = false;

	inline bool Active()
	{
		return Phobos::Config::TSFog && ScenarioClass::Instance->SpecialFlags.FogOfWar;
	}

	void ClearAllFog()
	{
		auto& map = MapClass::Instance;
		map.CellIteratorReset();

		for (auto pCell = map.CellIteratorNext(); pCell; pCell = map.CellIteratorNext())
		{
			pCell->ShroudCounter = 0;
			pCell->GapsCoveringThisCell = 0;
			pCell->AltFlags |= (AltCellFlags::Mapped | AltCellFlags::NoFog);
			pCell->Flags |= (CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
			pCell->Flags &= ~CellFlags::Fogged;
			pCell->Visibility = -1;
			pCell->Foggedness = -1;
			pCell->ClearFoggedObjects();
		}

		map.sub_657CE0();
		map.MarkNeedsRedraw(2);
	}
}

// Scenario / game-mode fog decision. Campaign maps keep their own setting (TUTO4
// writes [SpecialFlags] FogOfWar=no and must stay fully revealed); skirmish and
// multiplayer may turn fog on through the game-mode option.
DEFINE_HOOK(0x687C56, Fog_ReadScenario_Decide, 0x5)
{
	bool fog = ScenarioClass::Instance->SpecialFlags.FogOfWar;

	if (!SessionClass::IsCampaign())
		fog = fog || GameModeOptionsClass::Instance.FogOfWar;

	fog = fog && Phobos::Config::TSFog;

	Debug::Log("[Fog] scenario read: map=%d mode=%d tsfog=%d -> %d\n",
		static_cast<int>(ScenarioClass::Instance->SpecialFlags.FogOfWar),
		static_cast<int>(GameModeOptionsClass::Instance.FogOfWar),
		static_cast<int>(Phobos::Config::TSFog), static_cast<int>(fog));

	GameModeOptionsClass::Instance.FogOfWar = fog;
	ScenarioClass::Instance->SpecialFlags.FogOfWar = fog;
	return 0;
}

// DisplayClass::RevealFogShroud reimplementation. Refresh neighbours like the
// original so fog borders do not alias, and clear the Fogged flag on cells that
// became fully visible.
DEFINE_HOOK(0x4A9CA0, Fog_RevealFogShroud, 0x8)
{
	if (!Fog::Active())
		return 0;

	enum { SkipGameCode = 0x4A9DC6 };

	GET(DisplayClass*, pThis, ECX);
	GET_STACK(CellStruct*, pCellStruct, STACK_OFFSET(0x0, 0x4));
	GET_STACK(HouseClass*, pHouse, STACK_OFFSET(0x0, 0x8));
	GET_STACK(bool, increase, STACK_OFFSET(0x0, 0xC));

	const auto pCell = pThis->GetCellAt(*pCellStruct);
	const auto flags = pCell->Flags;

	const bool edgeNotRevealed = !(flags & CellFlags::EdgeRevealed);
	const bool shouldRadarRedraw = edgeNotRevealed || !(pCell->AltFlags & AltCellFlags::Mapped);
	bool shouldRedraw = shouldRadarRedraw;
	pCell->Flags = Fog::NoReveal ? (flags & ~CellFlags::IsPlot) : (flags & ~CellFlags::IsPlot | CellFlags::EdgeRevealed);

	if (increase)
		pCell->IncreaseShroudCounter();
	else
		pCell->ReduceShroudCounter();

	const char newVisibility = TacticalClass::Instance->GetOcclusion(*pCellStruct, false);
	const char newFoggedness = TacticalClass::Instance->GetOcclusion(*pCellStruct, true);

	if (newVisibility != pCell->Visibility)
	{
		shouldRedraw = true;
		pCell->Visibility = newVisibility;
	}

	if (pCell->Visibility == -1)
		pCell->AltFlags |= AltCellFlags::NoFog;

	if (newFoggedness != pCell->Foggedness)
	{
		shouldRedraw = true;
		pCell->Foggedness = newFoggedness;
	}

	if (pCell->Foggedness == -1)
	{
		pCell->Flags |= CellFlags::CenterRevealed;

		// A fully visible cell must not keep the Fogged flag: CleanFog only runs when
		// EdgeRevealed first appears, so FogCell can re-fog such a cell later without
		// updating Foggedness, which hides units standing in a visibly clear cell.
		if (pCell->Flags & CellFlags::Fogged)
			pCell->CleanFog();
	}

	if (shouldRedraw)
		TacticalClass::Instance->RegisterCellAsVisible(pCell);

	if ((pCell->AltFlags & AltCellFlags::Mapped) && ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		for (size_t i = 0; i < 8; ++i)
		{
			auto adjCell = Unsorted::AdjacentCell[i & 7] + *pCellStruct;
			const auto pAdjCell = pThis->GetCellAt(adjCell);

			if (adjCell != *pCellStruct && !(pAdjCell->Flags & CellFlags::CenterRevealed))
			{
				const char adjNewFoggedness = TacticalClass::Instance->GetOcclusion(adjCell, true);

				if (adjNewFoggedness == -1)
				{
					if (pAdjCell->Flags & CellFlags::EdgeRevealed)
					{
						pAdjCell->Flags |= CellFlags::CenterRevealed;
						TacticalClass::Instance->RegisterCellAsVisible(pAdjCell);

						for (size_t j = 0; j < 8; ++j)
						{
							const auto nextAdjCell = Unsorted::AdjacentCell[j & 7] + adjCell;
							const auto pNextAdjCell = pThis->GetCellAt(nextAdjCell);
							const char nextAdjNewFoggedness = TacticalClass::Instance->GetOcclusion(nextAdjCell, true);

							if (nextAdjNewFoggedness != pNextAdjCell->Foggedness)
							{
								pNextAdjCell->Foggedness = nextAdjNewFoggedness;
								TacticalClass::Instance->RegisterCellAsVisible(pNextAdjCell);
							}
						}

						continue;
					}
				}
				else
				{
					if (adjNewFoggedness == -2 || (pAdjCell->Flags & CellFlags::EdgeRevealed))
					{
						if (adjNewFoggedness >= 0 && adjNewFoggedness != pAdjCell->Foggedness)
						{
							pAdjCell->Foggedness = adjNewFoggedness;
							TacticalClass::Instance->RegisterCellAsVisible(pAdjCell);
						}

						continue;
					}
				}

				pThis->MapCellFoggedness(&adjCell, pHouse);
				continue;
			}
		}
	}

	if (shouldRedraw)
		MapClass::Instance.RevealCheck(pCell, pHouse, shouldRadarRedraw);

	if ((pCell->Flags & CellFlags::EdgeRevealed) && edgeNotRevealed && ScenarioClass::Instance->SpecialFlags.FogOfWar)
		pCell->CleanFog();

	return SkipGameCode;
}

// DisplayClass::MapCellFoggedness: honour Fog::NoReveal while keeping the native
// edge flag bookkeeping.
DEFINE_HOOK(0x4A9DF1, Fog_MapCellFoggedness, 0x6)
{
	if (!Fog::Active())
		return 0;

	enum { SkipGameCode = 0x4A9DFE };

	GET(CellFlags, flags, EAX);

	R->ECX(!(flags & CellFlags::EdgeRevealed));

	flags &= ~CellFlags::IsPlot;

	if (!Fog::NoReveal)
		flags |= CellFlags::EdgeRevealed;

	R->EAX(flags);

	return SkipGameCode;
}

// MapClass::AllToSee: count flying/stationary friendlies like the shroud refresh
// does, otherwise their sight is dropped every refresh.
DEFINE_HOOK(0x4ADFF0, Fog_AllToSee, 0x5)
{
	if (!Fog::Active())
		return 0;

	enum { SkipGameCode = 0x4AE0A5 };

	GET_STACK(bool, notForBuilding, STACK_OFFSET(0x0, 0x4));
	GET_STACK(bool, revealFlag, STACK_OFFSET(0x0, 0x8));

	const auto& vec = TechnoClass::Array;

	for (int i = 0; i < vec.Count; ++i)
	{
		const auto pTechno = vec.Items[i];

		if (pTechno && !pTechno->InLimbo && pTechno->IsOnMap)
		{
			if (pTechno->WhatAmI() != AbstractType::Building || !notForBuilding)
			{
				const auto pOwner = pTechno->Owner;

				if (pOwner->IsControlledByCurrentPlayer())
				{
					if (pTechno->DiscoveredByCurrentPlayer)
						pTechno->See(false, revealFlag);
				}
				else if (RulesClass::Instance->AllyReveal)
				{
					if (pOwner->IsAlliedWith(HouseClass::CurrentPlayer))
						pTechno->See(revealFlag, false);
				}
			}
		}
	}

	return SkipGameCode;
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
