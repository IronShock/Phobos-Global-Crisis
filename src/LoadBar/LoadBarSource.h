#pragma once

// LoadBar is a self-contained, client-side progress bar renderer.
//
// It deliberately owns its own source enum instead of extending
// DisplayInfoType: the feature must not add fields to, or change the
// serialization of, any existing class. Everything here is read-only with
// respect to the simulation and is never written to a savegame.

enum class LoadBarSource : int
{
	None = 0,
	Health,
	Shield,
	Ammo,
	MindControl,
	Spawns,
	Passengers,
	Tiberium,
	Experience,
	Occupants,
	GattlingStage,
	ROF,
	Reload,
	SpawnTimer,
	GattlingTimer,
	ProduceCash,
	PassengerKill,
	AutoDeath,
	SuperWeapon,
	IronCurtain,
	TemporalLife,
	FactoryProcess,
	WeaponCooldown,
	AttachEffect
};

// A single bar instance produced by a source.
struct LoadBarEntry
{
	double Remaining = 0.0; // 0..1, 1 = full / just started, 0 = empty / ready
	int Value = 0;          // remaining frames or count, for text
	int Total = 0;          // total frames or count, for text
	const char* Label = nullptr; // optional, e.g. AttachEffect type name
	int Key = 0;            // stable sort key
};

const char* LoadBarSourceToString(LoadBarSource source);
LoadBarSource LoadBarSourceFromString(const char* text);
