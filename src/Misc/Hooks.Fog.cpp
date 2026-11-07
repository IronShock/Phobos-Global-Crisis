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

#include <unordered_set>
#include <cstring>

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

	// Explored-once bitmap + list: an explored cell must never show the black
	// shroud again, no matter which engine path writes it.
	std::vector<unsigned char> Explored;
	std::vector<int> ExploredList;

	// Friendly line-of-sight bitmap. AutoSpread=no stops the engine from clearing
	// its own reveal flags, so the engine's occlusion result cannot tell us what
	// the player can actually see. This map is rebuilt from friendly/allied
	// Technos every frame and drives both producer fog and object hiding.
	std::vector<unsigned char> InSight;


	// Set while the post-render pass is replaying an allowed animation. The
	// DrawIt gate must let this replay through even though the animation is in
	// fog; normal object drawing is skipped for the same animation.
	bool FogAnimPostRenderInProgress = false;

	// Global mode only refreshes the sight map every few frames; this flag forces
	// an immediate rebuild when the producer state changes (placed/removed/reset).
	bool SightDirty = true;

	void EnsureExplored()
	{
		if (Explored.empty())
			Explored.assign(512 * 512, 0);

		if (InSight.empty())
			InSight.assign(512 * 512, 0);

	}

	void ResetExplored()
	{
		Explored.assign(512 * 512, 0);
		ExploredList.clear();
		InSight.assign(512 * 512, 0);
		FogAnimPostRenderInProgress = false;
		SightDirty = true;
	}

	void MarkExplored(const CellStruct& cell, int index)
	{
		EnsureExplored();

		if (index < 0 || index >= 512 * 512 || Explored[index])
			return;

		Explored[index] = 1;
		ExploredList.push_back(index);
	}

	// Remember every revealed cell before a gap/reshroud clears its flags.
	void ScanExploredCells()
	{
		EnsureExplored();

		auto& map = MapClass::Instance;
		map.CellIteratorReset();

		for (auto pCell = map.CellIteratorNext(); pCell; pCell = map.CellIteratorNext())
		{
			if (pCell->Flags & (CellFlags::CenterRevealed | CellFlags::EdgeRevealed))
				MarkExplored(pCell->MapCoords, MapClass::GetCellIndex(pCell->MapCoords));
		}
	}

	int FogFrame()
	{
		return Phobos::Config::TSFog_FogFrame;
	}

	// --- FogProducer -------------------------------------------------------
	// Buildings with FogProducer=yes spread fog outward from themselves. The
	// vanilla gap generator is left completely untouched.
	struct ProducerState
	{
		int Radius = 0;
		int NextStep = 0;
		int MaxRange = 0;
		int Speed = 1;
		int Step = 1;
		DWORD UniqueID = 0;
		DWORD SeenSerial = 0;
		CellStruct Center {};
		bool Global = false;
		bool GlobalFull = false;
		bool Grow = false;
		bool Powered = false;
	};

	std::unordered_map<BuildingClass*, ProducerState> Producers;
	std::vector<unsigned char> ProducerMask;

	// Producer discovery is throttled: the building array is scanned only when
	// its size changes (place/destroy) or every ProducerScanInterval frames.
	// Radius growth still runs every frame from the cached state, without
	// dereferencing a building that may already be freed.
	constexpr int ProducerScanInterval = 10;
	DWORD ProducerScanSerial = 0;
	int LastProducerBuildingCount = -1;
	int NextProducerScanFrame = 0;

	// A Global=yes producer with Grow=no covers every map cell, so it does not
	// need the 512x512 mask at all.
	bool AnyGlobalProducer = false;

	// The first producer scan after a scenario reset establishes the initial
	// state without running the full-map conversion pass: a preplaced global
	// producer is handled by the engine from the first frame anyway.
	bool GlobalStateKnown = false;

	void ConvertToGlobalFog();
	void ConvertFromGlobalFog();

	// AutoSpread=yes or an active Global producer both mean "let the engine's
	// normal fog logic run". The custom AutoSpread=no path (InSight map, forced
	// cell clearing) is only used when no global producer is active.
	bool AutoSpreadEnabled()
	{
		return Phobos::Config::TSFog_AutoSpread || AnyGlobalProducer;
	}

	void EnsureProducerState()
	{
		if (ProducerMask.empty())
			ProducerMask.assign(512 * 512, 0);
	}

	void ResetProducers()
	{
		Producers.clear();
		ProducerMask.assign(512 * 512, 0);
		AnyGlobalProducer = false;
		GlobalStateKnown = false;
		ProducerScanSerial = 0;
		LastProducerBuildingCount = -1;
		NextProducerScanFrame = 0;
	}

	// A radius large enough to reach every cell of any supported (<= 512x512) map
	// from any cell. The fill loops clamp themselves to the map index range.
	constexpr int GlobalProducerRange = 1024;

	void FillProducerCircle(std::vector<unsigned char>& mask, const CellStruct& center, int radius)
	{
		if (radius <= 0)
			return;

		const int radius2 = radius * radius;
		const int x0 = std::max<int>(0, center.X - radius);
		const int x1 = std::min<int>(511, center.X + radius);
		const int y0 = std::max<int>(0, center.Y - radius);
		const int y1 = std::min<int>(511, center.Y + radius);

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const int dx = x - center.X;
				const int dy = y - center.Y;

				if (dx * dx + dy * dy <= radius2)
					mask[(y << 9) + x] = 1;
			}
		}
	}

	void RebuildProducerMask()
	{
		EnsureProducerState();
		std::fill(ProducerMask.begin(), ProducerMask.end(), static_cast<unsigned char>(0));

		// Global + Grow=no is handled by AnyGlobalProducer and needs no mask.
		if (AnyGlobalProducer)
			return;

		for (auto const& [pBuilding, state] : Producers)
			FillProducerCircle(ProducerMask, state.Center, state.Radius);
	}

	// Render-only: mark the current view dirty so a Global producer's fog appears
	// immediately instead of only in the next scrolled/dirty strip. This does not
	// write any cell state.
	void MarkVisibleCellsDirty()
	{
		auto const& rect = MapClass::Instance.VisibleRect;

		for (int y = rect.Y; y < rect.Y + rect.Height; ++y)
		{
			for (int x = rect.X; x < rect.X + rect.Width; ++x)
			{
				const CellStruct cell { static_cast<short>(x), static_cast<short>(y) };

				if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
					TacticalClass::Instance->RegisterCellAsVisible(pCell);
			}
		}
	}

	void UpdateProducers()
	{
		// FogProducer is a no-op in normal automatic fog mode: the engine already
		// covers the whole map. Skipping the scan keeps the per-frame cost at zero
		// for the common case.
		if (Phobos::Config::TSFog_AutoSpread)
			return;

		EnsureProducerState();

		const bool wasGlobal = AnyGlobalProducer;
		bool dirty = false;

		if (!Active() || !HouseClass::CurrentPlayer)
		{
			if (!Producers.empty() || AnyGlobalProducer)
			{
				const bool hadGlobal = AnyGlobalProducer;
				Producers.clear();
				AnyGlobalProducer = false;
				GlobalStateKnown = false;
				ProducerScanSerial = 0;
				LastProducerBuildingCount = -1;
				NextProducerScanFrame = 0;
				std::fill(ProducerMask.begin(), ProducerMask.end(), static_cast<unsigned char>(0));

				if (hadGlobal)
				{
					SightDirty = true;
					MarkVisibleCellsDirty();
				}

				MapClass::Instance.MarkNeedsRedraw(2);
			}

			return;
		}

		// Rescan the building array only when its size changes (place/destroy) or
		// after a short interval. In a steady state this removes the per-frame
		// array scan and the temporary hash set allocation entirely.
		const bool countChanged = BuildingClass::Array.Count != LastProducerBuildingCount;
		const bool scanDue = !GlobalStateKnown || countChanged
			|| Unsorted::CurrentFrame >= NextProducerScanFrame;
		bool anyGlobal = AnyGlobalProducer;

		if (scanDue)
		{
			LastProducerBuildingCount = BuildingClass::Array.Count;
			NextProducerScanFrame = Unsorted::CurrentFrame + ProducerScanInterval;
			anyGlobal = false;

			++ProducerScanSerial;

			if (ProducerScanSerial == 0)
			{
				ProducerScanSerial = 1;

				for (auto& [pProducer, state] : Producers)
					state.SeenSerial = 0;
			}

			for (auto pBuilding : BuildingClass::Array)
			{
				if (!pBuilding || !pBuilding->IsAlive || pBuilding->InLimbo || !pBuilding->Type)
					continue;

				auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pBuilding->Type);

				if (!pTypeExt || !pTypeExt->FogProducer.Get())
					continue;

				auto it = Producers.find(pBuilding);

				if (it == Producers.end() || it->second.UniqueID != pBuilding->UniqueID)
				{
					it = Producers.insert_or_assign(pBuilding, ProducerState {}).first;
					it->second.UniqueID = pBuilding->UniqueID;
					dirty = true;
				}

				auto& state = it->second;
				state.SeenSerial = ProducerScanSerial;

				const bool global = pTypeExt->FogProducer_Global.Get();
				// Global=yes always covers the whole map immediately; Range, Grow and
				// power are ignored.
				const bool grow = global ? false : pTypeExt->FogProducer_Grow.Get();
				const int range = global ? GlobalProducerRange
					: std::clamp(pTypeExt->FogProducer_Range.Get(), 0, 511);
				const int speed = std::max(1, pTypeExt->FogProducer_Speed.Get());
				const int step = std::max(1, pTypeExt->FogProducer_Step.Get());
				const auto center = pBuilding->GetMapCoords();

				if (state.Global != global || state.Grow != grow || state.MaxRange != range
					|| state.Speed != speed || state.Step != step
					|| state.Center.X != center.X || state.Center.Y != center.Y)
				{
					dirty = true;
				}

				state.Global = global;
				state.Grow = grow;
				state.MaxRange = range;
				state.Speed = speed;
				state.Step = step;
				state.Center = center;
				state.Powered = pBuilding->IsPowerOnline();

				if (!grow)
					state.Radius = range;
				else if (state.Radius > range)
					state.Radius = range;

				state.GlobalFull = global && !grow;

				if (state.GlobalFull)
					anyGlobal = true;
			}

			// Drop states of buildings that are gone or no longer producers. Do not
			// dereference the stored pointers: the building may already be freed.
			for (auto it = Producers.begin(); it != Producers.end(); )
			{
				if (it->second.SeenSerial != ProducerScanSerial)
				{
					it = Producers.erase(it);
					dirty = true;
				}
				else
				{
					++it;
				}
			}
		}

		// Radius growth runs every frame from the cached state, without touching
		// the building pointers (which may become stale between scans).
		for (auto& [pProducer, state] : Producers)
		{
			if (!state.Grow || !state.Powered || state.Radius >= state.MaxRange
				|| Unsorted::CurrentFrame < state.NextStep)
			{
				continue;
			}

			const int oldRadius = state.Radius;

			state.Radius = std::min(state.MaxRange, state.Radius + state.Step);
			state.NextStep = Unsorted::CurrentFrame + state.Speed;

			if (state.Radius != oldRadius)
				dirty = true;
		}

		if (scanDue)
		{
			if (!GlobalStateKnown)
			{
				// First scan after a scenario reset: a preplaced global producer is
				// handled by the engine from the first frame, so no conversion pass is
				// needed (and no explored cells exist yet anyway).
				GlobalStateKnown = true;
				AnyGlobalProducer = anyGlobal;
			}
			else if (AnyGlobalProducer != anyGlobal)
			{
				AnyGlobalProducer = anyGlobal;
				SightDirty = true;
				dirty = true;

				// Switching between the custom AutoSpread=no renderer and the engine's
				// native fog is a one-time state conversion. Doing it here (logic tick)
				// keeps GetOcclusion and the draw hooks pure.
				if (AnyGlobalProducer)
					ConvertToGlobalFog();
				else
					ConvertFromGlobalFog();
			}
		}

		if (dirty)
		{
			RebuildProducerMask();

			if (wasGlobal != AnyGlobalProducer)
				MarkVisibleCellsDirty();

			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}

	bool IsExploredCoords(const CellStruct& cell)
	{
		EnsureExplored();

		const int key = MapClass::GetCellIndex(cell);

		return key >= 0 && key < 512 * 512 && Explored[key] != 0;
	}

	bool InSightAt(const CellStruct& cell)
	{
		EnsureExplored();

		const int key = MapClass::GetCellIndex(cell);

		return key >= 0 && key < 512 * 512 && InSight[key] != 0;
	}

	void MarkSightCircle(const CellStruct& center, int sight)
	{
		if (sight <= 0)
			return;

		const int sight2 = sight * sight;
		const int x0 = std::max<int>(0, center.X - sight);
		const int x1 = std::min<int>(511, center.X + sight);
		const int y0 = std::max<int>(0, center.Y - sight);
		const int y1 = std::min<int>(511, center.Y + sight);

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const int dx = x - center.X;
				const int dy = y - center.Y;

				if (dx * dx + dy * dy <= sight2)
					InSight[(y << 9) + x] = 1;
			}
		}
	}

	// AutoSpread=no must not rely on the engine's reveal flags: skipping FogCell
	// leaves every explored cell marked as revealed, so GetOcclusion always says
	// "visible". Rebuild real friendly line of sight from the Technos instead.
	void UpdateSightMap()
	{
		EnsureExplored();
		std::fill(InSight.begin(), InSight.end(), static_cast<unsigned char>(0));

		if (!Active() || !HouseClass::CurrentPlayer)
			return;

		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			const auto pTechno = TechnoClass::Array.Items[i];

			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->IsOnMap)
				continue;

			const auto pOwner = pTechno->Owner;

			if (!pOwner)
				continue;

			const bool own = pOwner->IsControlledByCurrentPlayer();
			const bool allied = RulesClass::Instance->AllyReveal
				&& pOwner->IsAlliedWith(HouseClass::CurrentPlayer);

			if (!own && !allied)
				continue;

			int sight = pTechno->LastSightRange;
			const auto pType = pTechno->GetTechnoType();
			const int typeSight = pType ? pType->Sight : 0;
			sight = std::max(sight, typeSight);

			if (sight <= 0)
				continue;

			MarkSightCircle(pTechno->GetMapCoords(), std::min(sight, 511));
		}
	}

	// One-time conversion from the custom AutoSpread=no renderer to the engine's
	// native fog. Called when a global producer appears (usually released by a
	// custom option mid-game). This must never run from GetOcclusion or a draw
	// hook: it writes cell state.
	void ConvertToGlobalFog()
	{
		ScanExploredCells();
		UpdateSightMap();
		SightDirty = false;

		auto& map = MapClass::Instance;
		map.CellIteratorReset();

		for (auto pCell = map.CellIteratorNext(); pCell; pCell = map.CellIteratorNext())
		{
			const int index = MapClass::GetCellIndex(pCell->MapCoords);

			if (index < 0 || index >= 512 * 512 || !Explored[index])
				continue;

			// Explored cells never fall back to the black shroud; the native fog
			// layer is what covers them when they are not in real sight.
			pCell->AltFlags |= AltCellFlags::Mapped;
			pCell->Visibility = -1;

			if (InSightAt(pCell->MapCoords))
			{
				pCell->Flags |= (CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
				pCell->Foggedness = -1;

				if (pCell->Flags & CellFlags::Fogged)
					pCell->CleanFog();
			}
			else
			{
				pCell->Flags &= ~(CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
				pCell->Foggedness = -2;

				// Create the frozen building/terrain image exactly like the engine
				// does when a cell leaves sight. FogCell() ignores cells that are
				// already flagged as fogged, so this also repairs cells that carry
				// Foggedness=-2 without a snapshot flag.
				if (!(pCell->Flags & CellFlags::Fogged))
					pCell->FogCell();
			}
		}

		map.MarkNeedsRedraw(2);
	}

	// One-time conversion back to AutoSpread=no after the global producer is
	// gone: explored cells become fully live again and engine snapshots are
	// cleaned up.
	void ConvertFromGlobalFog()
	{
		auto& map = MapClass::Instance;
		map.CellIteratorReset();

		for (auto pCell = map.CellIteratorNext(); pCell; pCell = map.CellIteratorNext())
		{
			// Clean snapshots even on cells that were only revealed by the native
			// fog after the conversion, so no frozen image can survive the switch.
			if (pCell->Flags & CellFlags::Fogged)
				pCell->CleanFog();

			const int index = MapClass::GetCellIndex(pCell->MapCoords);

			if (index < 0 || index >= 512 * 512 || !Explored[index])
				continue;

			pCell->Flags |= (CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
			pCell->Visibility = -1;
			pCell->Foggedness = -1;
		}

		map.MarkNeedsRedraw(2);
	}

	bool IsExploredCell(CellClass* pCell)
	{
		return IsExploredCoords(pCell->MapCoords);
	}

	// True only for cells whose black shroud has actually been lifted. The
	// Explored bitmap can also contain edge cells (EdgeRevealed) that are still
	// inside the shroud transition; those must keep their native shroud frames.
	bool IsMappedCell(CellClass* pCell)
	{
		return (pCell->AltFlags & (AltCellFlags::Mapped | AltCellFlags::NoFog))
			!= static_cast<AltCellFlags>(0);
	}

	// AnimTypeClass pointers used by combat effects (warhead, weapon and bullet
	// animations) and all animations linked from them. Built after rules/scenario
	// load so the lists are complete before the first draw.
	std::unordered_set<AnimTypeClass*> WarheadAnimTypes;
	bool WarheadAnimTypesBuilt = false;

	// Explicit [FogAnim] registry: animation names listed there are allowed even
	// when they are not reached through a warhead/weapon/bullet field.
	std::unordered_set<AnimTypeClass*> FogAnimRegistry;

	void AddFogAnimName(const char* pName)
	{
		if (!pName || !*pName)
			return;

		if (auto const pType = AnimTypeClass::Find(pName))
			FogAnimRegistry.insert(pType);
		else
			Debug::Log("[FogAnim] unknown animation '%s'\n", pName);
	}

	void ParseFogAnimSection(CCINIClass* pINI)
	{
		if (!pINI)
			return;

		const int count = pINI->GetKeyCount("FogAnim");

		for (int i = 0; i < count; ++i)
		{
			const char* pKey = pINI->GetKeyName("FogAnim", i);

			if (!pKey)
				continue;

			char value[2048] {};

			if (pINI->ReadString("FogAnim", pKey, "", value, sizeof(value)) <= 0)
				continue;

			char* pContext = nullptr;

			for (char* pToken = strtok_s(value, ", \t", &pContext); pToken;
				pToken = strtok_s(nullptr, ", \t", &pContext))
			{
				AddFogAnimName(pToken);
			}
		}
	}

	void AddCombatAnimType(AnimTypeClass* pType)
	{
		if (!pType || !WarheadAnimTypes.insert(pType).second)
			return;

		AddCombatAnimType(pType->Next);
		AddCombatAnimType(pType->Spawns);
		AddCombatAnimType(pType->BounceAnim);
		AddCombatAnimType(pType->ExpireAnim);
		AddCombatAnimType(pType->TrailerAnim);

		if (auto const pExt = AnimTypeExt::ExtMap.TryFind(pType))
		{
			for (auto const pAnim : pExt->SplashAnims)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->WakeAnim)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->SmallFireAnims)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->LargeFireAnims)
				AddCombatAnimType(pAnim);
		}
	}

	void RebuildWarheadAnimTypes()
	{
		WarheadAnimTypes.clear();

		// Global damage-based splash animations used when a warhead has no
		// SplashList override.
		for (auto const pAnim : RulesClass::Instance->SplashList)
			AddCombatAnimType(pAnim);

		for (auto const pWH : WarheadTypeClass::Array)
		{
			if (!pWH)
				continue;

			for (auto const pAnim : pWH->AnimList)
				AddCombatAnimType(pAnim);

			if (auto const pExt = WarheadTypeExt::ExtMap.TryFind(pWH))
			{
				for (auto const pAnim : pExt->SplashList)
					AddCombatAnimType(pAnim);

				for (auto const pAnim : pExt->Crit_AnimList)
					AddCombatAnimType(pAnim);
			}
		}

		// Weapon launch/occupant/assault animations and their linked trails.
		for (auto const pWeapon : WeaponTypeClass::Array)
		{
			if (!pWeapon)
				continue;

			for (auto const pAnim : pWeapon->Anim)
				AddCombatAnimType(pAnim);

			AddCombatAnimType(pWeapon->OccupantAnim);
			AddCombatAnimType(pWeapon->AssaultAnim);
			AddCombatAnimType(pWeapon->OpenToppedAnim);
		}

		// Projectile trails.
		for (auto const pBullet : BulletTypeClass::Array)
		{
			if (pBullet)
				AddCombatAnimType(pBullet->Trailer);
		}

		FogAnimRegistry.clear();
		ParseFogAnimSection(CCINIClass::INI_Rules);
		ParseFogAnimSection(&CCINIClass::INI_Art);

		// Deprecated per-AnimType alias; keep old INIs working.
		for (auto const pType : AnimTypeClass::Array)
		{
			if (auto const pExt = AnimTypeExt::ExtMap.TryFind(pType))
			{
				if (pExt->AllowAnim)
					FogAnimRegistry.insert(pType);
			}
		}

		WarheadAnimTypesBuilt = true;
	}

	bool IsWarheadAnim(AnimClass* pAnim)
	{
		if (!pAnim || !pAnim->Type)
			return false;

		if (!WarheadAnimTypesBuilt)
			RebuildWarheadAnimTypes();

		return WarheadAnimTypes.find(pAnim->Type) != WarheadAnimTypes.end();
	}

	bool IsRegistryAnim(AnimClass* pAnim)
	{
		if (!pAnim || !pAnim->Type)
			return false;

		if (!WarheadAnimTypesBuilt)
			RebuildWarheadAnimTypes();

		return FogAnimRegistry.find(pAnim->Type) != FogAnimRegistry.end();
	}

	// Real sight, not the forced AutoSpread=no state. In AutoSpread=yes the live
	// Foggedness is authoritative; with AutoSpread=no every explored cell is
	// forced to -1, so the independently built InSight map is used instead.
	bool IsCellInSight(CellClass* pCell)
	{
		if (AutoSpreadEnabled())
			return pCell->Foggedness == -1;

		return InSightAt(pCell->MapCoords);
	}

	bool IsProducerCell(CellClass* pCell)
	{
		EnsureProducerState();

		const int key = MapClass::GetCellIndex(pCell->MapCoords);

		if (key < 0 || key >= 512 * 512)
			return false;

		return ProducerMask[key] != 0;
	}

	// A warhead/registry animation inside explored, mapped fog that the player
	// cannot currently see. These are drawn once by the post-render pass.
	bool IsFoggedAllowedAnim(AnimClass* pAnim, CellClass* pCell, bool requireOnScreen)
	{
		if (!pAnim || !pAnim->Type || !pAnim->IsAlive || pAnim->Invisible)
			return false;

		if (!Active() || !Phobos::Config::TSFog_AllowAnim)
			return false;

		if (!pCell || !IsExploredCell(pCell) || !IsMappedCell(pCell))
			return false;

		if (!IsWarheadAnim(pAnim) && !IsRegistryAnim(pAnim))
			return false;

		if (IsCellInSight(pCell))
			return false;

		if (pCell->Foggedness == -1 && !pAnim->IsFogged && !IsProducerCell(pCell))
			return false;

		if (requireOnScreen
			&& !TacticalClass::Instance->CoordsToClient(pAnim->GetRenderCoords()).second)
		{
			return false;
		}

		return true;
	}

	// Draw allowed fog animations after TacticalClass::Render(..., 2) has
	// completed. The fog layer was drawn earlier, in mode 1; this pass therefore
	// places the animation above fog without touching any fog/cell state.
	void DrawAllowedAnimsAfterRender()
	{
		if (!Active() || !Phobos::Config::TSFog_AllowAnim)
			return;

		// The object pass passes this same rectangle to ObjectClass::DrawIt.
		auto const pBounds = reinterpret_cast<RectangleStruct*>(0x886FA0);
		constexpr int MaxDraw = 128;
		int drawn = 0;

		for (auto const pAnim : AnimClass::Array)
		{
			if (drawn >= MaxDraw)
				break;

			auto const pCell = pAnim ? pAnim->GetCell() : nullptr;

			if (!IsFoggedAllowedAnim(pAnim, pCell, true))
				continue;

			auto [location, visible] = TacticalClass::Instance
				->CoordsToClient(pAnim->GetRenderCoords());

			if (!visible)
				continue;

			const bool oldFogged = pAnim->IsFogged;
			const bool oldVisible = pAnim->IsVisible;
			const bool oldInProgress = FogAnimPostRenderInProgress;

			pAnim->IsFogged = false;
			pAnim->IsVisible = true;
			FogAnimPostRenderInProgress = true;
			pAnim->DrawIt(&location, pBounds);
			FogAnimPostRenderInProgress = oldInProgress;
			pAnim->IsVisible = oldVisible;
			pAnim->IsFogged = oldFogged;

			++drawn;
		}
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
		const bool producer = IsProducerCell(pCell);
		bool hide = false;

		if (!AutoSpreadEnabled())
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
					if (MapClass::Instance.GetCellAt(cell + offset)->Foggedness == -1)
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

	if (Phobos::Config::TSFog_AllowAnim)
		Fog::RebuildWarheadAnimTypes();

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
			&& (Fog::Explored[exploredIndex] || (flags & (CellFlags::CenterRevealed | CellFlags::EdgeRevealed)));

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
	if (!Fog::AutoSpreadEnabled())
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

	return 0;
}

// AnimClass::DrawIt skips drawing when AnimClass::IsFogged (offset 0x19D) is
// set. Allow warhead animations and explicitly opted-in AnimTypes to keep
// drawing inside explored fog. Unexplored black shroud stays hidden.
// Hook the `mov al, [esi+19Dh]` instead of the following `jne`: Syringe cannot
// relocate the relative branch, but a plain mov can be replayed safely.
DEFINE_HOOK(0x422FEC, AnimClass_DrawIt_AllowAnim, 0x6)
{
	enum { DrawAnyway = 0x422FFA, SkipDraw = 0x4238A3 };

	if (!Fog::Active() || !Phobos::Config::TSFog_AllowAnim)
		return 0;

	GET(AnimClass*, pThis, ESI);

	if (!pThis || !pThis->Type)
		return 0;

	// The post-render pass calls DrawIt directly; let that one through.
	if (Fog::FogAnimPostRenderInProgress)
		return 0;

	auto const pCell = pThis->GetCell();
	const bool explored = pCell && Fog::IsExploredCell(pCell);
	const bool mapped = pCell && Fog::IsMappedCell(pCell);

	const bool combat = Fog::IsWarheadAnim(pThis);
	const bool registry = Fog::IsRegistryAnim(pThis);
	const bool allow = explored && mapped && (combat || registry);
	const bool fogged = Fog::IsFoggedAllowedAnim(pThis, pCell, false);

	// Skip the normal object pass for fogged allowed animations; the
	// post-render pass draws them above fog instead, exactly once.
	if (fogged)
		return SkipDraw;

	return allow ? DrawAnyway : 0;
}

// GScreenClass::Render calls TacticalClass::Render three times:
//   0x4F44DF eMode=0, 0x4F44F4 eMode=1 (terrain + fog), 0x4F4515 eMode=2
//   (objects + animations). The instruction at 0x4F451B is immediately after
// the mode-2 call, so this is a genuine post-fog/post-object point. Replaying
// the allowed animations here puts them above fog without modifying any cell.
DEFINE_HOOK(0x4F451B, GScreenClass_Render_AfterTacticalObjects_AllowAnim, 0x5)
{
	if (Fog::Active() && Phobos::Config::TSFog_AllowAnim)
		Fog::DrawAllowedAnimsAfterRender();

	return 0;
}

// FogProducer / AutoSpread visual override. These hooks only run in the custom
// AutoSpread=no mode with no Global producer. With AutoSpread=yes or a Global
// producer, AutoSpreadEnabled() is true and the engine's normal per-cell fog
// frames are left untouched (this is the "normal fog of war" look).

DEFINE_HOOK(0x480223, Fog_Producer_VisibilityFrame, 0x8)
{
	if (!Fog::Active())
		return 0;

	GET(CellClass*, pCell, ESI);

	// Global fog: only override the shroud *drawing* for genuinely mapped
	// cells, so the engine's own fog layer is visible. Cells still inside the
	// black shroud transition keep their native shroud frames. No state writes.
	if (Fog::AutoSpreadEnabled())
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

	if (Fog::AutoSpreadEnabled())
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

	if (Fog::AutoSpreadEnabled())
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

	if (Fog::AutoSpreadEnabled())
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

	if (Fog::AutoSpreadEnabled())
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

	if (!Fog::Active() || Fog::AutoSpreadEnabled())
		return 0;

	GET_STACK(CellStruct*, pCellStruct, STACK_OFFSET(0x0, 0x1C));

	if (!pCellStruct || !Fog::IsExploredCoords(*pCellStruct))
		return 0;

	return SkipToEpilogue;
}
