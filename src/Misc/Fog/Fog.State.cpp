#include "Fog.h"

namespace Fog
{
	bool NoReveal = false;

	bool Active()
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
			Explored.assign(MapCellCount, 0);

		if (InSight.empty())
			InSight.assign(MapCellCount, 0);

	}

	void ResetExplored()
	{
		Explored.assign(MapCellCount, 0);
		ExploredList.clear();
		InSight.assign(MapCellCount, 0);
		FogAnimPostRenderInProgress = false;
		SightDirty = true;
	}

	void MarkExplored(int index)
	{
		EnsureExplored();

		if (index < 0 || index >= MapCellCount || Explored[index])
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
				MarkExplored(MapClass::GetCellIndex(pCell->MapCoords));
		}
	}

	int FogFrame()
	{
		return Phobos::Config::TSFog_FogFrame;
	}

	CellStruct CoordsToCellWithLevel(const CoordStruct& coords)
	{
		const int level = coords.Z / Unsorted::LevelHeight;
		const int extra = (level & 1) ? ((level >> 1) + 1) : (level >> 1);

		return CellStruct {
			static_cast<short>((coords.X >> 8) - extra),
			static_cast<short>((coords.Y >> 8) - extra) };
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
	bool UsesNativeFog()
	{
		return Phobos::Config::TSFog_AutoSpread || AnyGlobalProducer;
	}

	void EnsureProducerState()
	{
		if (ProducerMask.empty())
			ProducerMask.assign(MapCellCount, 0);
	}

	void ResetProducers()
	{
		Producers.clear();
		ProducerMask.assign(MapCellCount, 0);
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
		const int x1 = std::min<int>(MaxMapCoord, center.X + radius);
		const int y0 = std::max<int>(0, center.Y - radius);
		const int y1 = std::min<int>(MaxMapCoord, center.Y + radius);

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const int dx = x - center.X;
				const int dy = y - center.Y;

				if (dx * dx + dy * dy <= radius2)
					mask[(y * MapSize) + x] = 1;
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
					: std::clamp(pTypeExt->FogProducer_Range.Get(), 0, MaxMapCoord);
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

		return key >= 0 && key < MapCellCount && Explored[key] != 0;
	}

	bool IsExploredIndex(int index)
	{
		EnsureExplored();

		return index >= 0 && index < MapCellCount && Explored[index] != 0;
	}

	bool InSightAt(const CellStruct& cell)
	{
		EnsureExplored();

		const int key = MapClass::GetCellIndex(cell);

		return key >= 0 && key < MapCellCount && InSight[key] != 0;
	}

	void MarkSightCircle(const CellStruct& center, int sight)
	{
		if (sight <= 0)
			return;

		const int sight2 = sight * sight;
		const int x0 = std::max<int>(0, center.X - sight);
		const int x1 = std::min<int>(MaxMapCoord, center.X + sight);
		const int y0 = std::max<int>(0, center.Y - sight);
		const int y1 = std::min<int>(MaxMapCoord, center.Y + sight);

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const int dx = x - center.X;
				const int dy = y - center.Y;

				if (dx * dx + dy * dy <= sight2)
					InSight[(y * MapSize) + x] = 1;
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

			MarkSightCircle(pTechno->GetMapCoords(), std::min(sight, MaxMapCoord));
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

			if (index < 0 || index >= MapCellCount || !Explored[index])
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

			if (index < 0 || index >= MapCellCount || !Explored[index])
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
	// Real sight, not the forced AutoSpread=no state. In AutoSpread=yes the live
	// Foggedness is authoritative; with AutoSpread=no every explored cell is
	// forced to -1, so the independently built InSight map is used instead.
	bool IsCellInSight(CellClass* pCell)
	{
		if (UsesNativeFog())
			return pCell->Foggedness == -1;

		return InSightAt(pCell->MapCoords);
	}

	bool IsProducerCell(CellClass* pCell)
	{
		EnsureProducerState();

		const int key = MapClass::GetCellIndex(pCell->MapCoords);

		if (key < 0 || key >= MapCellCount)
			return false;

		return ProducerMask[key] != 0;
	}

	namespace
	{
		struct FirerReveal
		{
			DWORD UniqueID;
			int ExpireFrame;
			bool IsBuilding;
			std::vector<CellStruct> Cells;
		};

		std::vector<FirerReveal> FirerReveals;

		void MarkFirerCellsDirty(const std::vector<CellStruct>& cells)
		{
			for (auto const& cell : cells)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
					TacticalClass::Instance->RegisterCellAsVisible(pCell);
			}
		}

		// Native fog is authoritative in Foggedness; the custom AutoSpread=no
		// renderer keeps explored cells at -1 and draws producer fog separately.
		bool IsCellFoggedForFirerReveal(CellClass* pCell)
		{
			if (UsesNativeFog())
				return pCell->Foggedness != -1;

			return IsProducerCell(pCell) && !InSightAt(pCell->MapCoords);
		}
	}

	DWORD CurrentDrawUniqueID = 0;

	void ResetFirerReveals()
	{
		FirerReveals.clear();
		CurrentDrawUniqueID = 0;
	}

	// Loading a save restores the engine's cell fog state but not necessarily the
	// scenario fog flag or the custom bitmaps. Restore both before the first
	// frame, otherwise IsLocationFogged would consider every object visible while
	// the saved fog layer is still drawn.
	namespace
	{
		bool DetectGlobalProducerOnMap()
		{
			for (auto pBuilding : BuildingClass::Array)
			{
				if (!pBuilding || !pBuilding->IsAlive || pBuilding->InLimbo || !pBuilding->Type)
					continue;

				auto const pExt = BuildingTypeExt::ExtMap.TryFind(pBuilding->Type);

				if (pExt && pExt->FogProducer.Get() && pExt->FogProducer_Global.Get()
					&& !pExt->FogProducer_Grow.Get())
				{
					return true;
				}
			}

			return false;
		}

		// Recompute the native per-cell fog frames from the restored reveal flags.
		// The saved cells can come back with only the endpoint values (-1 / -2 /
		// FogFrame), which draws hard cell-shaped fog edges instead of the normal
		// 0..48 transition frames.
		void RebuildFogGradient()
		{
			if (!UsesNativeFog())
				return;

			EnsureExplored();

			for (int index : ExploredList)
			{
				CellStruct cell {
					static_cast<short>(index & (MapSize - 1)),
					static_cast<short>(index >> 9) };

				auto const pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				const char fog = TacticalClass::Instance->GetOcclusion(cell, true);

				if (pCell->Foggedness != fog)
				{
					pCell->Foggedness = fog;
					TacticalClass::Instance->RegisterCellAsVisible(pCell);
				}
			}
		}
	}

	void OnGameLoaded()
	{
		if (!Phobos::Config::TSFog)
			return;

		bool fog = ScenarioClass::Instance->SpecialFlags.FogOfWar;

		if (!SessionClass::IsCampaign())
			fog = fog || GameModeOptionsClass::Instance.FogOfWar;

		if (!fog && Phobos::Config::TSFog_ForceAll)
			fog = true;

		GameModeOptionsClass::Instance.FogOfWar = fog;
		ScenarioClass::Instance->SpecialFlags.FogOfWar = fog;

		ResetFirerReveals();

		if (!fog)
			return;

		// The custom bitmaps are not part of the save; rebuild them from the
		// restored cell state and the loaded objects.
		ResetExplored();
		ScanExploredCells();

		// Some saves only restore AltFlags::Mapped, not the reveal bits.
		if (ExploredList.empty())
		{
			MapClass::Instance.CellIteratorReset();

			for (auto pCell = MapClass::Instance.CellIteratorNext(); pCell;
				pCell = MapClass::Instance.CellIteratorNext())
			{
				if (pCell->AltFlags & (AltCellFlags::Mapped | AltCellFlags::NoFog))
					MarkExplored(MapClass::GetCellIndex(pCell->MapCoords));
			}
		}

		UpdateSightMap();
		SightDirty = false;

		// Detect the Global producer directly: the normal scan needs
		// HouseClass::CurrentPlayer, which may not be ready at this point.
		ResetProducers();
		AnyGlobalProducer = DetectGlobalProducerOnMap();

		if (HouseClass::CurrentPlayer)
			UpdateProducers();

		InstallIsLocationFogged();
		RebuildFogGradient();

		{
			int visible = 0, gradient = 0, dense = 0;

			for (int index : ExploredList)
			{
				CellStruct cell {
					static_cast<short>(index & (MapSize - 1)),
					static_cast<short>(index >> 9) };

				if (auto const pCell = MapClass::Instance.TryGetCellAt(cell))
				{
					if (pCell->Foggedness == -1)
						++visible;
					else if (pCell->Foggedness == -2)
						++dense;
					else
						++gradient;
				}
			}

			Debug::Log("[FogLoad] active=%d auto=%d native=%d global=%d buildings=%d explored=%d vis=%d grad=%d dense=%d\n",
				static_cast<int>(Active()), static_cast<int>(Phobos::Config::TSFog_AutoSpread),
				static_cast<int>(UsesNativeFog()), static_cast<int>(AnyGlobalProducer),
				BuildingClass::Array.Count, static_cast<int>(ExploredList.size()),
				visible, gradient, dense);
		}

		MapClass::Instance.MarkNeedsRedraw(2);
	}

	bool IsFirerRevealed(DWORD uniqueID, const CellStruct& cell)
	{
		if (uniqueID == 0)
			return false;

		for (auto const& reveal : FirerReveals)
		{
			// Buildings keep their frozen snapshot; never draw the live building.
			if (reveal.IsBuilding)
				continue;

			if (reveal.UniqueID != uniqueID || Unsorted::CurrentFrame >= reveal.ExpireFrame)
				continue;

			for (auto const& revealCell : reveal.Cells)
			{
				if (revealCell == cell)
					return true;
			}
		}

		return false;
	}

	bool IsCellFirerRevealed(const CellStruct& cell)
	{
		for (auto const& reveal : FirerReveals)
		{
			if (reveal.IsBuilding || Unsorted::CurrentFrame >= reveal.ExpireFrame)
				continue;

			if (std::find(reveal.Cells.begin(), reveal.Cells.end(), cell) != reveal.Cells.end())
				return true;
		}

		return false;
	}

	bool IsCellVisibleBuilding(CellClass* pCell)
	{
		return (pCell->Flags & CellFlags::Fogged)
			&& (pCell->AltFlags & AltCellFlags::ContainsBuilding);
	}

	void RevealFirer(TechnoClass* pFirer)
	{
		if (!Phobos::Config::TSFog_RevealFirer || !Active() || !pFirer || !HouseClass::CurrentPlayer)
			return;

		if (!pFirer->IsAlive || pFirer->InLimbo || !pFirer->IsOnMap)
			return;

		const auto pOwner = pFirer->Owner;

		if (!pOwner || pOwner->IsControlledByCurrentPlayer()
			|| pOwner->IsAlliedWith(HouseClass::CurrentPlayer))
		{
			return;
		}

		if (pFirer->CloakState == CloakState::Cloaked
			|| pFirer->CloakState == CloakState::Cloaking)
		{
			return;
		}

		if (pFirer->InWhichLayer() == Layer::Underground)
			return;

		// Submarines and other submerged objects are not revealed either.
		if (!pFirer->IsSurfaced())
			return;

		std::vector<CellStruct> candidates;
		const bool isBuilding = pFirer->WhatAmI() == AbstractType::Building;

		if (isBuilding)
		{
			const auto pBuilding = static_cast<BuildingClass*>(pFirer);
			const auto pFoundation = pBuilding->GetFoundationData(false);
			const auto base = pBuilding->GetMapCoords();
			const CellStruct foundationEnd { 0x7FFF, 0x7FFF };

			for (auto pOffset = pFoundation; pOffset && *pOffset != foundationEnd; ++pOffset)
				candidates.push_back(base + *pOffset);
		}
		else
		{
			candidates.push_back(pFirer->GetMapCoords());
		}

		// The object draw path resolves the render coordinates, which can differ
		// from the base cell for tall buildings; include both.
		candidates.push_back(CoordsToCellWithLevel(pFirer->GetCoords()));

		std::vector<CellStruct> revealCells;
		revealCells.reserve(candidates.size());

		for (auto const& cell : candidates)
		{
			if (std::find(revealCells.begin(), revealCells.end(), cell) != revealCells.end())
				continue;

			auto const pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!pCell || !IsExploredCoords(cell) || !IsCellFoggedForFirerReveal(pCell))
				continue;

			revealCells.push_back(cell);
		}

		if (revealCells.empty())
			return;

		const int expireFrame = Unsorted::CurrentFrame + Phobos::Config::TSFog_RevealFirer_Frames;
		const DWORD uniqueID = pFirer->UniqueID;

		for (auto& reveal : FirerReveals)
		{
			if (reveal.UniqueID != uniqueID)
				continue;

			MarkFirerCellsDirty(reveal.Cells);
			reveal.Cells = revealCells;
			reveal.ExpireFrame = expireFrame;
			reveal.IsBuilding = isBuilding;
			MarkFirerCellsDirty(reveal.Cells);
			return;
		}

		MarkFirerCellsDirty(revealCells);
		FirerReveals.push_back(FirerReveal { uniqueID, expireFrame, isBuilding, std::move(revealCells) });
	}

	void UpdateFirerReveals()
	{
		if (FirerReveals.empty())
			return;

		for (auto it = FirerReveals.begin(); it != FirerReveals.end(); )
		{
			if (Unsorted::CurrentFrame < it->ExpireFrame)
			{
				++it;
				continue;
			}

			MarkFirerCellsDirty(it->Cells);
			it = FirerReveals.erase(it);
		}
	}

}
