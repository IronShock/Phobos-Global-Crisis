#pragma once

// Lightweight per-frame timing probes for the GC performance work.
//
// The probes are installed on the game's hottest object paths and are inert
// unless PerfProbe::Enabled is set (from [Phobos] Debug.PerfProbe). They are
// render/timing only: nothing here changes simulation state, so it is safe for
// multiplayer as long as the setting is purely local (it is).

class PerfProbe
{
public:
	enum class Id
	{
		BuildingDraw = 0,
		BuildingAI,
		BuildingAIHook,
		UnitAI,
		InfantryAI,
		AircraftAI,
		AnimAI,
		LogicUpdate,
		RaiseEvent,
		TacticalRender,
		CellDraw,
		DrawObject,
		CellScan,
		TerrainLoopAI,
		BuildingLoopAI,
		UnitLoopAI,
		InfantryLoopAI,
		AircraftLoopAI,
		AnimLoopAI,
		OtherLoopAI,
		FirstLoop,
		LightAnim,
		TailTotal,
		ArrayAI1,
		LightSourceUpdate,
		AnimUpdateAll,
		MapLightUpdate,
		FactoryAI,
		HouseAI,
		Count
	};

	struct Stat
	{
		unsigned long long Calls;
		double Ms;
	};

	struct Snapshot
	{
		Stat Stats[static_cast<int>(Id::Count)];

		const Stat& Get(Id id) const
		{
			return Stats[static_cast<int>(id)];
		}
	};

	// Set from [Phobos] Debug.PerfProbe. Defaults to false so the probes cost
	// nothing in a normal game; the diagnostic build turns it on.
	static bool Enabled;

	// Always-on last-frame timings used by the hitch log (negligible cost).
	static double LastLogicMs;
	static double LastRenderMs;

	static void Enter(Id id);
	static void Exit(Id id);
	static void Add(Id id, double ms);

	// Returns the totals accumulated since the previous call and starts a new
	// interval. Called once per measurement interval by PerfOverlay.
	static Snapshot TakeSnapshot();

private:
	static double NowMs();
};
