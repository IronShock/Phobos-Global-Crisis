#include "Fog.h"

#include <intrin.h>

namespace Fog
{
	// Real MapClass::IsLocationFogged. gamemd ships a stub there (`xor al,al; ret 4`)
	// that always reports "not fogged", which is why units are still drawn inside
	// fog. This is installed as a 5-byte JMP over the stub once the port is active.
	bool __fastcall IsLocationFoggedImpl(MapClass* pThis, void* /*edx*/, const CoordStruct& coords)
	{
		// Consume the per-object context once, before any early return.
		const DWORD drawUniqueID = CurrentDrawUniqueID;
		CurrentDrawUniqueID = 0;

		if (!Active())
			return false;

		const CellStruct cell = CoordsToCellWithLevel(coords);
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			return false;

		// DisplayClass::ProcessClickCoords (0x6924E4) and the cursor/action
		// resolver (0x692A8C) treat a fogged cell as "no target" and fall back to
		// a move order. Fogged buildings are drawn as frozen snapshots and units
		// revealed by RevealFirer are visible, so let those two call sites resolve
		// a real target. Other IsLocationFogged callers keep the normal result.
		const auto returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

		if ((returnAddress == 0x6924E4u || returnAddress == 0x692A8Cu)
			&& (IsCellFirerRevealed(cell) || IsCellVisibleBuilding(pCell)))
		{
			return false;
		}

		// RevealFirer: draw only the firing enemy itself, even when other objects
		// share the cell.
		if (drawUniqueID != 0 && IsFirerRevealed(drawUniqueID, cell))
			return false;

		const bool producer = IsProducerCell(pCell);
		bool hide = false;

		if (!UsesNativeFog())
		{
			// Pure AutoSpread=no: only producer fog hides objects.
			if (producer && !InSightAt(cell))
			{
				const bool hasBuilding = (pCell->AltFlags & AltCellFlags::ContainsBuilding) ? true : false;
				const bool hasSnapshot = (pCell->Flags & CellFlags::Fogged) ? true : false;

				if (!hasBuilding || hasSnapshot)
				{
					hide = true;

					for (auto const& offset : Unsorted::AdjacentCell)
					{
						if (InSightAt(cell + offset))
						{
							hide = false;
							break;
						}
					}
				}
			}
		}
		else
		{
			// Native engine visibility (AutoSpread=yes and Global producers).
			// Foggedness is the authoritative fog state; CellFlags::Fogged only
			// tracks whether a frozen snapshot exists.
			if (pCell->Foggedness != -1)
			{
				hide = true;

				for (auto const& offset : Unsorted::AdjacentCell)
				{
					const auto pAdjacent = MapClass::Instance.TryGetCellAt(cell + offset);

					if (pAdjacent && pAdjacent->Foggedness == -1)
					{
						hide = false;
						break;
					}
				}
			}
		}

		return hide;
	}

	void InstallIsLocationFogged()
	{
		static bool installed = false;

		if (installed)
			return;

		installed = true;

		constexpr DWORD address = 0x5865E0;
		DWORD oldProtect = 0;

		if (VirtualProtect(reinterpret_cast<void*>(address), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			*reinterpret_cast<unsigned char*>(address) = 0xE9;
			const auto relative = static_cast<int>(reinterpret_cast<uintptr_t>(&IsLocationFoggedImpl)
				- (address + 5));
			*reinterpret_cast<int*>(address + 1) = relative;
			VirtualProtect(reinterpret_cast<void*>(address), 5, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 5);
			Debug::Log("[Fog] IsLocationFogged installed\n");
		}
		else
		{
			Debug::Log("[Fog] IsLocationFogged patch failed\n");
		}
	}
}

// --- Terrain objects / overlays / smudges: hide while fully occluded ---
//
// The original hooks at 0x6D9313 / 0x6D70BC / 0x48049E stole relative branches
// (je), which Syringe cannot relocate; replaying them in the trampoline jumped
// into unmapped memory. These sites are the straight-line instructions after the
// native "no such thing" branches, so the original checks stay intact.

// Terrain objects: skip the real draw only when the cell is fully occluded.
DEFINE_HOOK(0x6D9313, Fog_DrawObjects_SkipTerrain, 0x6)
{
	if (!Fog::Active())
		return 0;

	enum { SkipDraw = 0x6D940C };

	GET(const CoordStruct*, pCoord, EAX);

	const CellStruct cell = Fog::CoordsToCellWithLevel(*pCoord);
	const auto pCell = MapClass::Instance.TryGetCellAt(cell);

	if (pCell && pCell->Visibility == -2)
		return SkipDraw;

	return 0;
}

// Overlay (walls, tiberium, ...). Reached only when the cell has an overlay, the
// native "no overlay" branch at 0x6D70BC is left untouched.
DEFINE_HOOK(0x6D70C6, Fog_DrawCellOverlay_Skip, 0x7)
{
	if (!Fog::Active())
		return 0;

	enum { SkipDraw = 0x6D71A4 };

	GET(const CellClass*, pCell, EBX);

	if (pCell->Visibility == -2)
		return SkipDraw;

	return 0;
}

// Smudge (craters). Reached only when the cell has a smudge, the native branch at
// 0x48049E is left untouched.
DEFINE_HOOK(0x4804A4, Fog_DrawCellSmudge_Skip, 0x6)
{
	if (!Fog::Active())
		return 0;

	enum { SkipDraw = 0x4804FB };

	GET(const CellClass*, pCell, ESI);

	if (pCell->Visibility == -2)
		return SkipDraw;

	return 0;
}

// FogProducer / AutoSpread visual override. These hooks only run in the custom
// AutoSpread=no mode with no Global producer. With AutoSpread=yes or a Global
// producer, UsesNativeFog() is true and the engine's normal per-cell fog
// frames are left untouched (this is the "normal fog of war" look).

DEFINE_HOOK(0x480223, Fog_Producer_VisibilityFrame, 0x8)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass*, pCell, ESI);

	// Global fog: only override the shroud *drawing* for genuinely mapped
	// cells, so the engine's own fog layer is visible. Cells still inside the
	// black shroud transition keep their native shroud frames. No state writes.
	if (Fog::UsesNativeFog())
		return 0;

	if (!Fog::IsExploredCell(pCell) || !Fog::IsMappedCell(pCell)
		|| pCell->Visibility == -1)
	{
		return 0;
	}

	if (Fog::IsProducerCell(pCell))
	{
		// Producer fog is rendered by the fog layer. Clear any residual shroud
		// so an explored cell never shows black under the producer fog.
		R->EAX(0);
		return 0;
	}

	// Non-producer cells keep the engine's native shroud edge frames.
	return 0;
}

DEFINE_HOOK(0x480273, Fog_Producer_FogFrame, 0x5)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass*, pCell, ESI);

	if (Fog::UsesNativeFog())
		return 0;

	if (!Fog::IsExploredCell(pCell) || !Fog::IsMappedCell(pCell))
		return 0;

	if (Fog::IsProducerCell(pCell))
	{
		R->EAX(Fog::IsCellInSight(pCell) ? 0 : Fog::FogFrame());
		return 0;
	}

	if (pCell->Foggedness == -1)
		return 0;

	R->EAX(0);

	return 0;
}

// Normal fog frame path: Foggedness in 0..48 jumps past 0x480273 and reaches the
// draw call through 0x48028B. Only used by the custom AutoSpread=no producer mode.
DEFINE_HOOK(0x48028B, Fog_Producer_FogFrameNormal, 0x5)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass*, pCell, ESI);

	if (Fog::UsesNativeFog())
		return 0;

	if (!Fog::IsExploredCell(pCell) || !Fog::IsMappedCell(pCell))
		return 0;

	if (Fog::IsProducerCell(pCell))
	{
		R->EAX(Fog::IsCellInSight(pCell) ? 0 : Fog::FogFrame());
		return 0;
	}

	if (pCell->Foggedness == -1)
		return 0;

	R->EAX(0);

	return 0;
}

// --- AutoSpread=no: stop the engine fog logic, not just the drawing ---
//
// 0x6D8700 (GetOcclusion) recomputes Visibility/Foggedness every frame from the
// neighbours' reveal flags and writes them back into the cell. Overriding only
// the draw frame leaves that logic running, so cells keep fading to 34/48/-2 and
// FogCell() freezes buildings and hides objects. These two exit hooks return -1
// for explored cells on the fog layer, which keeps the cell fully live.
//
// AutoSpread=yes and an active Global producer both fall through to the engine's
// native result: the global path must not keep a second visibility model.
DEFINE_HOOK(0x6D8AC6, Fog_GetOcclusion_ClearExplored, 0x7)
{
	enum { ContinueAfterLookup = 0x6D8ACD };

	if (!Fog::Active())
		return 0;

	GET_STACK(char, fogLayer, STACK_OFFSET(0x0, 0x18));

	if (!fogLayer)
		return 0;

	GET(CellStruct*, pCoords, ESI);

	if (!pCoords)
		return 0;

	if (Fog::UsesNativeFog())
		return 0;

	if (!Fog::IsExploredCoords(*pCoords))
		return 0;

	R->EAX(-1);

	return ContinueAfterLookup;
}

DEFINE_HOOK(0x6D8AD4, Fog_GetOcclusion_ClearExplored_Early, 0x5)
{
	if (!Fog::Active())
		return 0;

	GET_STACK(char, fogLayer, STACK_OFFSET(0x0, 0x18));

	if (!fogLayer)
		return 0;

	GET(CellStruct*, pCoords, ESI);

	if (!pCoords)
		return 0;

	if (Fog::UsesNativeFog())
		return 0;

	if (!Fog::IsExploredCoords(*pCoords))
		return 0;

	R->EDI(-1);

	return 0;
}

// The periodic fog-spread pass calls this for cells that are edge-revealed but no
// longer centered on sight: it clears the reveal flags, forces Foggedness=-2 and
// creates the frozen snapshots (FogCell). With AutoSpread=no explored cells must
// never enter it, otherwise the engine keeps "fogging" them logically even though
// the overlay is drawn transparent.
//
// Hook after the prologue (sub esp,0x10; push ebx; push edi) and jump to the
// routine's own epilogue at 0x4ACD89, so the stack stays balanced. Emulating
// `ret 4` by hand at 0x4ACC50 leaked 8 bytes per call and corrupted the cell
// iterator's caller.
DEFINE_HOOK(0x4ACC55, Fog_AutoSpread_SkipFogCell, 0x6)
{
	enum { SkipToEpilogue = 0x4ACD89 };

	if (!Fog::Active() || Fog::UsesNativeFog())
		return 0;

	GET_STACK(CellStruct*, pCellStruct, STACK_OFFSET(0x0, 0x1C));

	if (!pCellStruct || !Fog::IsExploredCoords(*pCellStruct))
		return 0;

	return SkipToEpilogue;
}
