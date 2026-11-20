#pragma once

#include "Commands.h"

// Sends every airborne payload of the selected carrier back to its deck, or
// the selected payload itself. Used to test/return the custom deck landing,
// which no longer goes through a native airport dock.
class RecallCarrierAircraftCommandClass : public CommandClass
{
public:
	// Registered instance, used to take over the vanilla Stop hotkey.
	static RecallCarrierAircraftCommandClass* Instance;

	// Rebinds the vanilla "StopObject" hotkey to this command once the
	// keyboard configuration has been loaded.
	static void EnsureStopRebind();

	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;
	virtual void Execute(WWKey eInput) const override;
};
