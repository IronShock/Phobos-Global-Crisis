#pragma once

#include "Commands.h"

// Tactical time-stop: freezes the whole battlefield (all units, AI, production
// and timers) while the player can still pan the camera, select units and issue
// orders. Queued orders are executed once the time-stop is released.
// Singleplayer only (campaign / skirmish vs AI), toggled with a hotkey.
class TacticalPauseCommandClass : public CommandClass
{
public:
	// Runtime toggle, not serialized. Reset on scenario load.
	static bool IsActive;

	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;
	virtual void Execute(WWKey eInput) const override;

	static void Reset();

	// True while the time-stop is active AND the player asked to forbid all
	// operations during it ([General] TacticalPause.BlockActions=yes).
	static bool IsBlockingActions();
};
