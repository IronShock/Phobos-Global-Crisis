#include "Fog.h"

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

	char newVisibility = TacticalClass::Instance->GetOcclusion(*pCellStruct, false);
	char newFoggedness = TacticalClass::Instance->GetOcclusion(*pCellStruct, true);

	// Once explored, a cell must never fall back to the black shroud layer: clamp
	// the computed occlusion to fog. This is what keeps temporary reveals (fire,
	// gap generators, attack impact) from turning into black shroud afterwards.
	{
		Fog::EnsureExplored();

		const int exploredIndex = MapClass::GetCellIndex(*pCellStruct);
		const bool inRange = exploredIndex >= 0 && exploredIndex < 512 * 512;
		const bool explored = inRange
			&& (Fog::IsExploredIndex(exploredIndex) || (flags & (CellFlags::CenterRevealed | CellFlags::EdgeRevealed)));

		if (explored)
		{
			Fog::MarkExplored(*pCellStruct, exploredIndex);

			if (!Fog::AutoSpreadEnabled())
			{
				// AutoSpread=no: an explored cell never re-fogs. Keep it exactly as
				// if it were in sight so no snapshot/freeze logic kicks in.
				newFoggedness = -1;

				if (newVisibility == -2)
					newVisibility = -1;
			}
			else if (newVisibility == -2)
			{
				// Engine-driven fog (AutoSpread=yes / Global): explored cells must
				// never fall back to the black shroud, but keep the engine's own
				// Foggedness (including -2 = dense fog).
				newVisibility = -1;
			}
		}
	}

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
