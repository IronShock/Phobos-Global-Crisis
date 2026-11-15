#include "RecallCarrierAircraft.h"

#include <AircraftClass.h>
#include <HouseClass.h>
#include <ObjectClass.h>
#include <RulesClass.h>
#include <UnitClass.h>
#include <Utilities/GeneralUtils.h>

#include <Ext/Unit/Footprint.h>

namespace
{
	void Recall(TechnoClass* pTechno)
	{
		if (!pTechno || !pTechno->IsAlive)
			return;

		if (auto const pAircraft = abstract_cast<AircraftClass*>(pTechno))
		{
			if (Footprint::GetCarrierPayloadOwner(pAircraft))
				pAircraft->QueueMission(Mission::Enter, false);

			return;
		}

		auto const pUnit = abstract_cast<UnitClass*>(pTechno);

		if (!pUnit)
			return;

		auto const pState = Footprint::GetState(pUnit);

		if (!pState || !pState->CarrierPayloadsInitialized)
			return;

		for (auto const pAircraft : pState->CarrierPayloads)
		{
			if (pAircraft && pAircraft->IsAlive && !pAircraft->InLimbo)
				pAircraft->QueueMission(Mission::Enter, false);
		}
	}
}

const char* RecallCarrierAircraftCommandClass::GetName() const
{
	return "Recall Carrier Aircraft";
}

const wchar_t* RecallCarrierAircraftCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_RECALL_CARRIER_AIRCRAFT", L"Recall Carrier Aircraft");
}

const wchar_t* RecallCarrierAircraftCommandClass::GetUICategory() const
{
	return CATEGORY_SELECTION;
}

const wchar_t* RecallCarrierAircraftCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_RECALL_CARRIER_AIRCRAFT_DESC",
		L"Order the selected carrier's aircraft (or a selected payload) to land back on the deck.");
}

void RecallCarrierAircraftCommandClass::Execute(WWKey eInput) const
{
	for (int i = 0; i < ObjectClass::CurrentObjects.Count; ++i)
	{
		if (auto const pTechno = abstract_cast<TechnoClass*>(ObjectClass::CurrentObjects.GetItem(i)))
			Recall(pTechno);
	}

	MessageListClass::Instance.PrintMessage(
		L"Carrier aircraft recalled.",
		RulesClass::Instance->MessageDelay,
		HouseClass::CurrentPlayer->ColorSchemeIndex,
		true
	);
}
