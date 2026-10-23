#pragma once

#include "Commands.h"

// Toggles the CoH-style tactical map overlay (enlarged minimap).
class ToggleTacticalMapCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;
	virtual void Execute(WWKey eInput) const override;
};
