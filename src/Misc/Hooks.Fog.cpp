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

#include <AnimTypeClass.h>
#include <BuildingTypeClass.h>
#include <ColorScheme.h>
#include <FileSystem.h>
#include <Memory.h>
#include <OverlayClass.h>
#include <SmudgeClass.h>
#include <Surface.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>

#include <Ext/AnimType/Body.h>
#include <Ext/TerrainType/Body.h>

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

	// Real MapClass::IsLocationFogged. gamemd ships a stub there (`xor al,al; ret 4`)
	// that always reports "not fogged", which is why units are still drawn inside
	// fog. This is installed as a 5-byte JMP over the stub once the port is active.
	bool __fastcall IsLocationFoggedImpl(MapClass* pThis, void* /*edx*/, const CoordStruct& coords)
	{
		if (!Active())
			return false;

		const int level = coords.Z / Unsorted::LevelHeight;
		const int extra = (level & 1) ? ((level >> 1) + 1) : (level >> 1);
		const CellStruct cell { static_cast<short>((coords.X >> 8) - extra),
			static_cast<short>((coords.Y >> 8) - extra) };
		const auto pCell = MapClass::Instance.GetCellAt(cell);

		if (!(pCell->Flags & CellFlags::Fogged) || pCell->Foggedness == -1)
			return false;

		// Objects overlapping a fully visible neighbour keep drawing, otherwise
		// they pop out at the fog border.
		for (auto const& offset : Unsorted::AdjacentCell)
		{
			if (MapClass::Instance.GetCellAt(cell + offset)->Foggedness == -1)
				return false;
		}

		return true;
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

	Debug::Log("[Fog] scenario read: map=%d mode=%d force=%d tsfog=%d -> %d\n",
		static_cast<int>(ScenarioClass::Instance->SpecialFlags.FogOfWar),
		static_cast<int>(GameModeOptionsClass::Instance.FogOfWar),
		static_cast<int>(Phobos::Config::TSFog_ForceAll),
		static_cast<int>(Phobos::Config::TSFog), static_cast<int>(fog));

	GameModeOptionsClass::Instance.FogOfWar = fog;
	ScenarioClass::Instance->SpecialFlags.FogOfWar = fog;

	if (fog)
		Fog::InstallIsLocationFogged();

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

// --- Stage B: hide terrain objects / overlays / smudges while fully occluded ---
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

	const int level = pCoord->Z / Unsorted::LevelHeight;
	const int extra = (level & 1) ? ((level >> 1) + 1) : (level >> 1);
	const CellStruct cell { static_cast<short>((pCoord->X >> 8) - extra),
		static_cast<short>((pCoord->Y >> 8) - extra) };

	if (MapClass::Instance.GetCellAt(cell)->Visibility == -2)
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


// ============================================================================
// Stage C2: frozen fog snapshots (buildings and static objects keep their last
// visible state while inside fog; units are hidden by IsLocationFogged).
// ============================================================================

namespace FoggedObjectHelper
{
	struct FoggedObjectClassFake
	{
		char _[0x40];
		RectangleStruct RenderDimension;
		char __[0x24];
		bool Visible;
	};
	static_assert(sizeof(FoggedObjectClassFake) == 0x78u);
	bool SetVectorCapacity(DynamicVectorClass<FoggedObjectClass*>* pVector, int capacity)
	{
		return reinterpret_cast<bool(__thiscall*)(DynamicVectorClass<FoggedObjectClass*>*, int, FoggedObjectClass **)>(0x45A680)(pVector, capacity, nullptr);
	}
	bool ObjectUnlimbo(ObjectClass* pObject, const CoordStruct& position, DirType face)
	{
		return reinterpret_cast<bool(__thiscall*)(ObjectClass*, const CoordStruct&, DirType)>(0x5F4EC0)(pObject, position, face);
	}
	void AddItemToVector(DynamicVectorClass<FoggedObjectClass*>* pVector, FoggedObjectClass* pItem)
	{
		if(pVector->Count < pVector->Capacity
			|| (pVector->IsAllocated || !pVector->Capacity)
				&& pVector->CapacityIncrement > 0
				&& FoggedObjectHelper::SetVectorCapacity(pVector, pVector->Capacity + pVector->CapacityIncrement))
		{
			pVector->Items[pVector->Count++] = std::move(pItem);
		}
	}
	DynamicVectorClass<FoggedObjectClass*>* CreateGameDynamicVector()
	{
		void* raw = YRMemory::Allocate(sizeof(DynamicVectorClass<FoggedObjectClass*>));
		*reinterpret_cast<void**>(raw) = reinterpret_cast<void*>(0x7E44F4u);
		auto pVector = static_cast<DynamicVectorClass<FoggedObjectClass*>*>(raw);
		pVector->Items = nullptr;
		pVector->Capacity = 0;
		pVector->IsInitialized = true;
		pVector->IsAllocated = false;
		pVector->Count = 0;
		pVector->CapacityIncrement = 1;
		FoggedObjectHelper::SetVectorCapacity(pVector, 1);
		return pVector;
	}
	void ClearAllFog()
	{
		MapClass::Instance.CellIteratorReset();
		for (auto pCell = MapClass::Instance.CellIteratorNext(); pCell; pCell = MapClass::Instance.CellIteratorNext())
		{
			pCell->ShroudCounter = 0;
			pCell->GapsCoveringThisCell = 0;
			pCell->AltFlags |= (AltCellFlags::Mapped | AltCellFlags::NoFog);
			pCell->Flags |= (CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
			pCell->Flags &= ~CellFlags::Fogged;
			pCell->ClearFoggedObjects();
		}
		MapClass::Instance.sub_657CE0();
		MapClass::Instance.MarkNeedsRedraw(2);
	}
	static FoggedObjectClass* __fastcall CreateFoggedOverlay(void* pThis, void* _, const CoordStruct& coords, int OverlayTypeIndex, int OverlayData) JMP_THIS(0x4D0980);
	static FoggedObjectClass* __fastcall CreateFoggedSmudge(void* pThis, void* _, const CoordStruct& coords, int SmudgeTypeIndex, int SmudgeData) JMP_THIS(0x4D0C40);
	static FoggedObjectClass* __fastcall CreateFoggedTerrain(void* pThis, void* _, TerrainClass* pTerrain) JMP_THIS(0x4D1370);
	static void __fastcall TechnoClass_RevealLastSight(TechnoClass* pThis, void* _, bool OnlyOutline, bool RevealByHeight, bool specifiedHouse, HouseClass *pHouse)
	{
		if (!pThis->IsInPlayfield || !pThis->unknown_bool_250)
			return;

		const auto pOwner = pThis->Owner;
		if (pOwner->Type->MultiplayPassive)
			return;

		pThis->unknown_bool_250 = false;

		const int lastSightRange = pThis->LastSightRange;
		if (!lastSightRange)
			return;

		// 只更新计数，不更新视野
		Fog::NoReveal = true;
		MapClass::Instance.RevealArea2(&pThis->LastSightCoords, lastSightRange, pOwner, false, false, false, true, true);
		Fog::NoReveal = false;
	}
	class CellClassFake final : public CellClass
	{
	public:
		FoggedObjectClass* FreezeOverlay(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedOverlay(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this->GetCoords(),
				this->OverlayTypeIndex,
				static_cast<unsigned int>(this->OverlayData)
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
		FoggedObjectClass* FreezeSmudge(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedSmudge(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this->GetCoords(),
				this->SmudgeTypeIndex,
				static_cast<unsigned int>(this->SmudgeData)
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
	};
	class TerrainClassFake final : public TerrainClass
	{
	public:
		FoggedObjectClass* FreezeTerrain(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedTerrain(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
		bool TerrainClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (this->IsAlive && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					this->FreezeTerrain(pCell->FoggedObjects, false);
				}
			}
			return result;
		};
	};
	class OverlayClassFake final : public OverlayClass
	{
	public:
		bool OverlayClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (pCell->OverlayTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeOverlay(pCell->FoggedObjects, false);
				}
			}
			return result;
		}
	};
	class SmudgeClassFake final : public SmudgeClass
	{
	public:
		bool SmudgeClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (pCell->SmudgeTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeSmudge(pCell->FoggedObjects, false);
				}
			}
			return result;
		}
	};

	// FoggedObjectClass 关键字段布局（依据 0x4D0980/0x4D0EF0 的构造与 0x486C50 的清理逻辑）
	struct FoggedObjectFields
	{
		char _[0x24];
		int OverlayTypeIndex;           // +0x24 覆盖物：类型索引
		HouseClass* Owner;              // +0x28 建筑：冻结时的所有者 House（0x4D0EF0 从 BuildingClass+0x21C 写入，不是建筑实例）
		int OverlayData;                // +0x2C 覆盖物：数量
		int FoggedType;                 // +0x30 类别：6=建筑 0x14=覆盖物 0x1D=弹坑
		CoordStruct Position;           // +0x34 冻结时的位置
		RectangleStruct RenderDimension;// +0x40 渲染矩形
	};
	static_assert(sizeof(FoggedObjectFields) == 0x50u);

	// 全局绘制数组（0x8B3CC0 元素 / 0x8B3CC4 数量）的元素：{类别排序键, 冻结对象}
	struct FoggedDrawRecord
	{
		int SortKey;
		FoggedObjectClass* Object;
	};
	static_assert(sizeof(FoggedDrawRecord) == 0x8u);

	// FoggedObjectClass 内部 DynamicVectorClass<DrawRecord>（位于 +0x5C）。元素是虚表
	// 指针可被虚拟调用的类型对象（每项 0x10 字节），这里只需要判空与数量上限
	struct FoggedObjectDrawRecords
	{
		void* VTable;                  // +0x5C
		void* Items;                   // +0x60
		int Capacity;                  // +0x64
		bool IsInitialized;            // +0x68
		bool IsAllocated;              // +0x69
		char _padding[2];              // +0x6A
		int Count;                     // +0x6C
		int CapacityIncrement;         // +0x70
	};
	static_assert(sizeof(FoggedObjectDrawRecords) == 0x18u);

	// FoggedObjectClass 主虚表地址（0x7E8B38，前四个虚表指针依次为主表/
	// INoticeSource/INoticeSink/IRTTITypeInfo，与 0x4D255E 处的构造一致）
	constexpr uintptr_t FoggedObjectClassVTable = 0x7E8B38u;

	// 析构函数（0x4D1650）在 0x4D1722 处按“格子坐标 + FoggedType”算出的排序键做一次
	// 二分查找来摘除绘制记录。同一格、同一类别存在多个冻结对象时排序键相同，可能摘错
	// 记录，结果是被销毁的对象仍留在绘制数组（0x8B3CC0）里：下一帧 0x4D1890 会拿已
	// 释放的对象去绘制——析构里 +0x60（DrawRecord 向量）已被释放置空，+0x6C（数量）
	// 却保持原值，建筑分支 0x4D1C6B 便以空指针为基址读取而崩溃。
	// 这里在析构释放向量之前按对象指针精确清理一遍，保证数组里不残留指向自己的记录。
	void RemoveDrawRecordsOf(const FoggedObjectClass* pObject)
	{
		auto const pItems = *reinterpret_cast<FoggedDrawRecord**>(0x8B3CC0);
		auto const pCount = reinterpret_cast<int*>(0x8B3CC4);

		if (!pItems || *pCount <= 0)
			return;

		const int count = *pCount;
		int write = 0;

		for (int read = 0; read < count; ++read)
		{
			if (pItems[read].Object != pObject)
				pItems[write++] = pItems[read];
		}

		if (write == count)
			return;

		for (int i = write; i < count; ++i)
			pItems[i] = FoggedDrawRecord { 0, nullptr };

		*pCount = write;
		*reinterpret_cast<char*>(0x8B3CCC) = 0; // 数组已变动，下次使用时重新排序
	}

	// 游戏内部的坐标→格子换算（与 0x486CA7 处的实现一致）
	CellStruct CoordToCell(const CoordStruct& coords)
	{
		const int x = (coords.X + (coords.X < 0 ? 255 : 0)) >> 8;
		const int y = (coords.Y + (coords.Y < 0 ? 255 : 0)) >> 8;
		return CellStruct { static_cast<short>(x), static_cast<short>(y) };
	}

	// 从格子的迷雾对象向量中移除指定项（保持其余项顺序）
	void RemoveFromVector(DynamicVectorClass<FoggedObjectClass*>* pVector, FoggedObjectClass* pItem)
	{
		for (int i = 0; i < pVector->Count; ++i)
		{
			if (pVector->Items[i] == pItem)
			{
				for (int j = i + 1; j < pVector->Count; ++j)
					pVector->Items[j - 1] = pVector->Items[j];

				--pVector->Count;
				return;
			}
		}
	}

	// 移除一个迷雾冻结对象：按其覆盖范围同步清理所有引用它的格子，标记脏区域后销毁
	void RemoveFoggedObject(CellClass* pOwnerCell, FoggedObjectClass* pItem)
	{
		const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);
		const auto baseCell = CoordToCell(pFields->Position);

		if (pFields->FoggedType == 6)
		{
			// 建筑冻结对象同时被其覆盖的每个格子引用（0x4D2790 返回其覆盖范围）
			const auto pOffsets = reinterpret_cast<CellStruct* (__thiscall*)(FoggedObjectClass*)>(0x4D2790)(pItem);

			if (pOffsets)
			{
				for (auto pOffset = pOffsets; ; ++pOffset)
				{
					if (*pOffset == CellStruct { 0x7FFF, 0x7FFF })

						break;

					if (const auto pCell = MapClass::Instance.TryGetCellAt(baseCell + *pOffset))
					{
						if (pCell->FoggedObjects)
							RemoveFromVector(pCell->FoggedObjects, pItem);
}

				}
			}
		}
		else if (pOwnerCell && pOwnerCell->FoggedObjects)
		{
			// 非建筑冻结对象只被单个格子引用
			RemoveFromVector(pOwnerCell->FoggedObjects, pItem);
		}

		auto rect = pFields->RenderDimension;
		rect.X -= TacticalClass::Instance->TacticalPos.X;
		rect.Y -= TacticalClass::Instance->TacticalPos.Y;
		TacticalClass::Instance->RegisterDirtyArea(rect, 0);

		// 虚表 +0x20 处为标量删除析构（与 0x486D5D 处一致）
		using DeletingDtor_t = void(__thiscall*)(FoggedObjectClass*, bool);
		const auto pDtor = reinterpret_cast<DeletingDtor_t*>(*reinterpret_cast<void**>(pItem))[8];
		pDtor(pItem, true);
	}
}


using FoggedObjectFields = FoggedObjectHelper::FoggedObjectFields;
using FoggedObjectHelper::RemoveFoggedObject;

DEFINE_HOOK(0x4D20FA, FoggedObjectClass_DrawFoggedObjects_AnimDrawOffset, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(AnimTypeClass* const, pType, ESI);
	R->ref_Stack<int>(0x38) += AnimTypeExt::ExtMap.Find(pType)->XDrawOffset;
	R->EDX(static_cast<int>(R->EDX()) + pType->YDrawOffset);
	return 0; // replay MOV CL,[ESI+35D] once; native code stores the adjusted Y
}

DEFINE_HOOK(0x4D1EE5, FoggedObjectClass_DrawFoggedObjects_BuildingCustomPalette, 0x8)
{
	if (!Fog::Active())
		return 0;

	GET(BuildingTypeClass*, pType, ESI);
	GET(void*, pFogged, EBX);

	const auto pSchemes = *reinterpret_cast<DynamicVectorClass<ColorScheme*>**>(
		reinterpret_cast<char*>(pType) + 0xDF0);

	if (pSchemes)
	{
		const auto pOwner = *reinterpret_cast<HouseClass**>(reinterpret_cast<char*>(pFogged) + 0x28);
		const auto index = *reinterpret_cast<int*>(reinterpret_cast<char*>(pOwner) + 0x16054);
		const auto pScheme = pSchemes->GetItem(index);
		const auto pConvert = *reinterpret_cast<void**>(reinterpret_cast<char*>(pScheme) + 0x30C);

		if (pConvert)
		{
			R->ref_Stack<int>(0x14) = reinterpret_cast<int>(pConvert);
			return 0x4D1EED; // 跳过原版默认色盘写入，否则原指令会覆盖本钩子的写入
		}
	}

	return 0; // 重放原指令：无自定义色盘时维持原版阵营默认色盘
}

DEFINE_HOOK(0x486B21, CellClass_FogCell_FreezeObjects, 0x6)
{
	if (!Fog::Active())
		return 0;

	enum { SkipGameCode = 0x486BAB };

	GET(FoggedObjectHelper::CellClassFake*, pCell, EBP);
	GET(DynamicVectorClass<FoggedObjectClass*>*, pVector, EDI);

    pCell->Flags |= CellFlags::Fogged;
	for (auto pObject = pCell->FirstObject; pObject; pObject = pObject->NextObject)
	{
		const auto absType = pObject->WhatAmI();
		if (absType == AbstractType::Building)
		{
			const auto& pBuilding = static_cast<BuildingClass*>(pObject);
			if (!pBuilding->IsFogged && pBuilding->IsAllFogged())
			{
				pBuilding->FreezeInFog(pVector, pCell, (!pBuilding->IsStrange() && pBuilding->Translucency != 15));
			}
		}
		else if (absType == AbstractType::Unit
			|| absType == AbstractType::Aircraft
			|| absType == AbstractType::Infantry)
		{
			pObject->Deselect();
		}
		else if (absType == AbstractType::Terrain)
		{
			static_cast<FoggedObjectHelper::TerrainClassFake*>(pObject)->FreezeTerrain(pVector, true);
		}
	}

	if (pCell->OverlayTypeIndex != -1)
		pCell->FreezeOverlay(pVector, true);

	if (pCell->SmudgeTypeIndex != -1)
		pCell->FreezeSmudge(pVector, true);

	return SkipGameCode;
}

DEFINE_HOOK(0x4D1855, FoggedObjectClass_Destructor_CleanDrawRecords, 0x7)
{
	if (!Fog::Active())
		return 0;

	GET(FoggedObjectClass*, pThis, EDI);

	FoggedObjectHelper::RemoveDrawRecordsOf(pThis);

	return 0; // 重新执行被覆盖的 mov [edi+5Ch], 7E8BC0h
}

DEFINE_HOOK(0x4D18F3, FoggedObjectClass_DrawFoggedObjects_SkipInvalidRecord, 0x7)
{
	if (!Fog::Active())
		return 0;

	enum { SkipThisObject = 0x4D2311 };

	GET(FoggedObjectHelper::FoggedDrawRecord*, pRecords, ECX);
	GET(int, index, ESI);

	auto const pObject = pRecords[index].Object;

	if (!pObject || reinterpret_cast<uintptr_t*>(pObject)[0] != FoggedObjectHelper::FoggedObjectClassVTable)
		return SkipThisObject;

	auto const pVector = reinterpret_cast<FoggedObjectHelper::FoggedObjectDrawRecords*>(
		reinterpret_cast<char*>(pObject) + 0x5C);

	if (!pVector->Items || pVector->Count > pVector->Capacity)
		return SkipThisObject;

	return 0;
}

DEFINE_HOOK(0x4D1B2E, FoggedObjectClass_DrawFoggedObjects_DrawTerrain, 0x6)
{
	if (!Fog::Active())
		return 0;

	enum { SkipGameCode = 0x4D2311 };

	GET(SHPStruct*, pImage, EAX);
	GET(const CoordStruct*, pCoord, ESI);
	GET(const RectangleStruct*, pBounds, EDI);
	GET(CellClass*, pCell, EBP);
	// 不能在这里读 ECX 当 Type：0x4D1B28 的 call [edx+9Ch]（Type 的虚函数，返回 SHP）已经执行过，
	// ECX 是易失寄存器、可能已被它破坏，之前那样取到的 Type 可能是垃圾；TerrainTypeExt::Fetch
	// 会顺着垃圾指针得到无效色盘并传给 DrawSHP，底层 blitter 会做出野跳转（EIP 落到非代码地址）
	// 而崩溃。改为从栈上取游戏在 call 之前保存的 Type 指针（即当时的 ECX，[esp+18h]）。
	GET_STACK(TerrainTypeClass*, pType, STACK_OFFSET(0x18, 0x0));
	GET_STACK(int, frame, STACK_OFFSET(0x154, -0x134));

	// 参数校验：任一指针不可信、或该对象并非地形时退回原版绘制。返回 0 会重新执行被覆盖的原
	// 指令（mov ebx,eax / mov eax,esi / mov ecx,[eax]），它们依赖进入钩子时的 EAX=SHP 与
	// ESI=坐标指针。Syringe 会保存并恢复整组寄存器，这里再把 EAX 显式写回 SHP 作为双保险。
	if (!pImage || !pCoord || !pBounds || !pCell || !pType || pType->WhatAmI() != AbstractType::TerrainType)
	{
		R->EAX(pImage);
		return 0;
	}

	if (pImage)
	{
		// 帧号来自被冻结对象内部字段，动画地形换帧后可能已经越界。越界会让位块
		// 传送读到 SHP 帧表之外（yr-simd 等加速器上表现为访问违例），因此按实际
		// 帧数取模，正常情况（帧号合法）下结果与原值相同。
		const int frameCount = pImage->Frames;

		if (frameCount <= 0)
		{
			R->EAX(pImage);
			return 0;
		}

		int drawFrame = frame % frameCount;

		if (drawFrame < 0)
			drawFrame += frameCount;

		const int shadowFrame = (drawFrame + frameCount / 2) % frameCount;

		auto point = TacticalClass::Instance->CoordsToClient(*pCoord).first;
		point.X += (DSurface::ViewBounds.X - pBounds->X);
		point.Y += (DSurface::ViewBounds.Y - pBounds->Y);
		int zAdjust = -TacticalClass::Instance->AdjustForZ(pCoord->Z);

		if (!pCell->LightConvert)
			pCell->InitLightConvert();

		ConvertClass* pPalette = pCell->LightConvert;
		int intensity = static_cast<int>(pCell->Intensity_Terrain);
		auto const pTypeExt = TerrainTypeExt::ExtMap.Find(pType);

		if (const auto pPalettes = pTypeExt->Palette)
		{
			// This fork has no PaletteRemap/PlainPalette split: always use the
			// colour-scheme remap, like the native terrain drawer.
			const int wallOwnerIndex = pCell->WallOwnerIndex;
			int colorSchemeIndex = HouseClass::CurrentPlayer->ColorSchemeIndex;

			if (wallOwnerIndex >= 0)
				colorSchemeIndex = HouseClass::Array[wallOwnerIndex]->ColorSchemeIndex;

			pPalette = pPalettes->Items[colorSchemeIndex]->LightConvert;

			intensity = static_cast<int>(pCell->Intensity_Normal);

			if (pType->SpawnsTiberium)
				point.Y -= 16;
		}
		else if (pType->SpawnsTiberium)
		{
			pPalette = FileSystem::GRFTXT_TIBERIUM_PAL;
			intensity = static_cast<int>(pCell->Intensity_Normal);
			point.Y -= 16;
		}

		DSurface::Temp->DrawSHP(pPalette, pImage, drawFrame, &point, pBounds, BlitterFlags(0x4E00), 0, zAdjust - 4, ZGradient::Deg90, intensity, 0, 0, 0, 0, 0);
		if (Game::bDrawShadow)
			DSurface::Temp->DrawSHP(pPalette, pImage, shadowFrame, &point, pBounds, BlitterFlags(0x4601), 0, zAdjust - 2, ZGradient::Ground, 1000, 0, 0, 0, 0, 0);
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x4D1E46, FoggedObjectClass_DrawFoggedObjects_NullBuildingZSHA, 0x7)
{
	if (!Fog::Active())
		return 0;

	R->EAX(0); // 原本存入 BUILDINGZ_SHA，改为空指针
	return 0;
}

DEFINE_HOOK(0x441F60, BuildingClass_AfterDestruction_RemoveFoggedImage, 0x5)
{
	if (!Fog::Active())
		return 0;

	GET(BuildingClass* const, pThis, ECX);

	if (ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		const auto& position = pThis->Location;
		const auto pFoundation = pThis->GetFoundationData(false);
		const CellStruct foundationEnd { 0x7FFF, 0x7FFF };

		// 建筑的迷雾冻结对象只有一个，被注册在其覆盖的每个格子中；
		// 以两种可能的基准格并集扫描，找到后由 RemoveFoggedObject 统一清理全部引用
		const CellStruct bases[2]
		{
			pThis->GetMapCoords(),
			FoggedObjectHelper::CoordToCell(position)
		};

		for (const auto& base : bases)
		{
			for (auto pOffset = pFoundation; *pOffset != foundationEnd; ++pOffset)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(base + *pOffset);

				if (!pCell || !pCell->FoggedObjects)
					continue;

				const auto& pVector = pCell->FoggedObjects;

				for (int i = pVector->Count - 1; i >= 0; --i)
				{
					const auto pItem = pVector->Items[i];
					const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);

					// +0x28 存的是所有者 House 而非建筑实例（此前拿它比对建筑指针，
					// 条件永远不成立、清理从未生效）；按类别 + 冻结位置匹配即可唯一
					// 确定该建筑的冻结贴图
					if (pFields->FoggedType == 6 && pFields->Position == position)
					{
						RemoveFoggedObject(pCell, pItem);
						return 0;
					}
				}
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x480C67, CellClass_ReduceTiberium_SyncFoggedOverlay, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass* const, pRawCell, ESI);

	// 耗尽路径中 ESI 已被调整为 cell+0x24（&MapCoords），此处还原
	auto pCell = pRawCell;
	if (MapClass::Instance.TryGetCellAt(pRawCell->MapCoords) != pRawCell)
		pCell = reinterpret_cast<CellClass*>(reinterpret_cast<char*>(pRawCell) - 0x24);

	if (!ScenarioClass::Instance->SpecialFlags.FogOfWar
		|| !pCell->FoggedObjects
		|| !(pCell->Flags & CellFlags::Fogged))
	{
		return 0;
	}

	const auto& pVector = pCell->FoggedObjects;

	for (int i = pVector->Count - 1; i >= 0; --i)
	{
		const auto pItem = pVector->Items[i];
		const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);

		if (pFields->FoggedType != 0x14) // 只处理覆盖物冻结对象
			continue;

		if (pFields->OverlayTypeIndex != pCell->OverlayTypeIndex)
		{
			RemoveFoggedObject(pCell, pItem);

			// 覆盖物仍存在时重新冻结
			if (pCell->OverlayTypeIndex != -1)
			{
				if (!pCell->FoggedObjects)
					pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

				static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeOverlay(pCell->FoggedObjects, true);
			}
		}
		else
		{
			// 数量变化：就地更新（绘制侧直接读取这两个字段）
			pFields->OverlayData = static_cast<int>(pCell->OverlayData);
		}
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x4D246E, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
DEFINE_HOOK(0x4D2430, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(uintptr_t, flagsAddr, EAX);

	auto const pCell = reinterpret_cast<CellClass*>(flagsAddr - 0x140);

	if (pCell->Foggedness == -1)
		return R->Origin() == 0x4D2430 ? 0x4D2436 : 0x4D2474;

	return 0;
}

DEFINE_FUNCTION_JUMP(CALL6, 0x415672, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4157F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x416C6D, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4CD43F, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4DA6F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x51A8D7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x54C93C, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x73AC5F, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL, 0x5FC4B1, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x5FD2D2, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x6B4B14, FoggedObjectHelper::SmudgeClassFake::SmudgeClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x71D012, FoggedObjectHelper::TerrainClassFake::TerrainClass_Unlimbo_CheckFog);
