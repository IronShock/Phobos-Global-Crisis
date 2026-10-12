#pragma once

#include "Commands.h"

// Toggle the automatic reverse behavior for the current player's
// AdvancedDrive vehicles. When disabled, the player's vehicles always
// drive forward instead of auto-reversing toward a rear target.
class AutoReverseToggleCommandClass : public CommandClass
{
public:
	// The live toggle state, flipped by the hotkey at runtime. Its initial
	// value is set from [General] ReverseMove when the rules are loaded
	// (see Phobos.INI.cpp), so that setting only changes the default.
	static inline bool AutoReverseEnabled = false;

	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;

	virtual void Execute(WWKey eInput) const override;
};
