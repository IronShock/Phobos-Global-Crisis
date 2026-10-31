#include "ToggleTacticalMap.h"

#include <Utilities/GeneralUtils.h>
#include <Misc/TacticalMap.h>

const char* ToggleTacticalMapCommandClass::GetName() const
{
	return "Tactical Map";
}

const wchar_t* ToggleTacticalMapCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_MAP", L"Tactical Map");
}

const wchar_t* ToggleTacticalMapCommandClass::GetUICategory() const
{
	return CATEGORY_INTERFACE;
}

const wchar_t* ToggleTacticalMapCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_MAP_DESC",
		L"Open or close the enlarged minimap overlay. Click it to move the camera.");
}

void ToggleTacticalMapCommandClass::Execute(WWKey eInput) const
{
	TacticalMap::Toggle();
}
