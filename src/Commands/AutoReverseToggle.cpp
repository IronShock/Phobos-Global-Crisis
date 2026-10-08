#include "AutoReverseToggle.h"

#include <Utilities/GeneralUtils.h>
#include <CommandClass.h>

const char* AutoReverseToggleCommandClass::GetName() const
{
	return "Auto Reverse Toggle";
}

const wchar_t* AutoReverseToggleCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_AUTO_REVERSE", L"Toggle Auto-Reverse");
}

const wchar_t* AutoReverseToggleCommandClass::GetUICategory() const
{
	return CATEGORY_CONTROL;
}

const wchar_t* AutoReverseToggleCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_AUTO_REVERSE_DESC", L"Toggle automatic reversing for your vehicles.");
}

void AutoReverseToggleCommandClass::Execute(WWKey eInput) const
{
	AutoReverseEnabled = !AutoReverseEnabled;
}
