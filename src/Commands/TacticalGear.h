#pragma once

#include "Commands.h"

#include <SessionClass.h>
#include <GameOptionsClass.h>
#include <Utilities/GeneralUtils.h>

// Tactical Gear: in-game game speed shifter. Direction +1 = slower (downshift),
// -1 = faster (upshift). The speed value ranges 0 (fastest) .. 6 (slowest) and
// is clamped at the bounds (no wrap-around). Singleplayer only.
template<int Direction>
class TacticalGearCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override
	{
		return Direction > 0 ? "Tactical Gear Down" : "Tactical Gear Up";
	}

	virtual const wchar_t* GetUIName() const override
	{
		return Direction > 0
			? GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_GEAR_DOWN", L"Tactical Gear: Slow Down")
			: GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_GEAR_UP", L"Tactical Gear: Speed Up");
	}

	virtual const wchar_t* GetUICategory() const override
	{
		return CATEGORY_CONTROL;
	}

	virtual const wchar_t* GetUIDescription() const override
	{
		return Direction > 0
			? GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_GEAR_DOWN_DESC", L"Shift the game speed one step slower.")
			: GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_GEAR_UP_DESC", L"Shift the game speed one step faster.");
	}

	virtual void Execute(WWKey eInput) const override
	{
		if (!SessionClass::IsSingleplayer())
			return;

		auto& speed = GameOptionsClass::Instance.GameSpeed;
		const int next = speed + Direction;

		if (next >= 0 && next <= 6)
			speed = next;
	}
};
