#pragma once

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

#include <AnimClass.h>
#include <AnimTypeClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <BulletTypeClass.h>
#include <CCINIClass.h>
#include <ColorScheme.h>
#include <FileSystem.h>
#include <Memory.h>
#include <OverlayClass.h>
#include <SmudgeClass.h>
#include <Surface.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>
#include <WarheadTypeClass.h>
#include <WeaponTypeClass.h>

#include <Ext/AnimType/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/WarheadType/Body.h>

#include <cstring>
#include <unordered_set>
#include <vector>

// Shared fog-of-war state and queries. Hooks live in the Fog.Hooks.* units.
namespace Fog
{
	constexpr int MapSize = 512;
	constexpr int MaxMapCoord = MapSize - 1;
	constexpr int MapCellCount = MapSize * MapSize;

	bool Active();
	bool UsesNativeFog();
	int FogFrame();
	CellStruct CoordsToCellWithLevel(const CoordStruct& coords);

	void ClearAllFog();

	// Explored-once bookkeeping.
	void EnsureExplored();
	void ResetExplored();
	void MarkExplored(int index);
	void ScanExploredCells();
	bool IsExploredIndex(int index);
	bool IsExploredCoords(const CellStruct& cell);
	bool IsExploredCell(CellClass* pCell);
	bool IsMappedCell(CellClass* pCell);

	// Friendly line of sight (custom AutoSpread=no path).
	void UpdateSightMap();
	bool InSightAt(const CellStruct& cell);
	bool IsCellInSight(CellClass* pCell);

	// FogProducer state.
	void ResetProducers();
	void UpdateProducers();
	bool IsProducerCell(CellClass* pCell);

	// Animation visibility (defined in Fog.Hooks.Anim.cpp).
	void RebuildWarheadAnimTypes();

	// MapClass::IsLocationFogged replacement (defined in Fog.Hooks.Draw.cpp).
	void InstallIsLocationFogged();

	extern bool NoReveal;
	extern bool FogAnimPostRenderInProgress;
	extern bool SightDirty;
}
