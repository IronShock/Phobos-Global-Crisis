#include "DumpCarrierState.h"

#include <Ext/Unit/Footprint.h>

#include <HouseClass.h>
#include <RulesClass.h>
#include <Utilities/GeneralUtils.h>

const char* DumpCarrierStateCommandClass::GetName() const
{
	return "Dump Carrier State";
}

const wchar_t* DumpCarrierStateCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_DUMP_CARRIER_STATE", L"Dump Carrier State");
}

const wchar_t* DumpCarrierStateCommandClass::GetUICategory() const
{
	return CATEGORY_DEVELOPMENT;
}

const wchar_t* DumpCarrierStateCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_DUMP_CARRIER_STATE_DESC",
		L"Write the footprint and deck state of every multi-cell unit to debug.log.");
}

void DumpCarrierStateCommandClass::Execute(WWKey eInput) const
{
	Footprint::DumpCarrierState();

	MessageListClass::Instance.PrintMessage(
		L"Carrier state dumped to debug.log.",
		RulesClass::Instance->MessageDelay,
		HouseClass::CurrentPlayer->ColorSchemeIndex,
		true
	);
}
