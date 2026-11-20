#include "RecallCarrierAircraft.h"

#include <AircraftClass.h>
#include <CommandClass.h>
#include <HouseClass.h>
#include <ObjectClass.h>
#include <RulesClass.h>
#include <UnitClass.h>
#include <Utilities/GeneralUtils.h>

#include <Ext/Unit/Footprint.h>

#include <cstring>

RecallCarrierAircraftCommandClass* RecallCarrierAircraftCommandClass::Instance = nullptr;

namespace
{
	CommandClass* s_pStopCommand = nullptr;

	// Returns true when the order was actually applied to a carrier payload.
	bool Recall(TechnoClass* pTechno)
	{
		if (!pTechno || !pTechno->IsAlive)
			return false;

		if (auto const pAircraft = abstract_cast<AircraftClass*>(pTechno))
		{
			if (!Footprint::GetCarrierPayloadOwner(pAircraft))
				return false;

			// Parked/returning payloads are already under the deck controller.
			// Only a payload that is still in normal flight needs the order.
			if (Footprint::IsCarrierPayloadDeckControlled(pAircraft))
				return false;

			pAircraft->QueueMission(Mission::Enter, true);
			return true;
		}

		auto const pUnit = abstract_cast<UnitClass*>(pTechno);

		if (!pUnit)
			return false;

		auto const pState = Footprint::GetState(pUnit);

		if (!pState || !pState->CarrierPayloadsInitialized)
			return false;

		bool recalled = false;

		for (auto const pAircraft : pState->CarrierPayloads)
		{
			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			if (Footprint::IsCarrierPayloadDeckControlled(pAircraft))
				continue;

			pAircraft->QueueMission(Mission::Enter, true);
			recalled = true;
		}

		return recalled;
	}
}

void RecallCarrierAircraftCommandClass::EnsureStopRebind()
{
	static bool rebound = false;

	if (rebound || !Instance)
		return;

	// Wait until KeyboardMD.ini has been parsed into the hotkey table.
	if (CommandClass::Hotkeys.Count() == 0)
		return;

	CommandClass* pStop = nullptr;

	for (auto const pCommand : CommandClass::Array)
	{
		if (!pCommand || pCommand == Instance)
			continue;

		const char* pName = pCommand->GetName();

		if (pName && std::strcmp(pName, "StopObject") == 0)
		{
			pStop = pCommand;
			break;
		}
	}

	if (!pStop)
		return;

	s_pStopCommand = pStop;

	// Take over every key currently bound to the vanilla Stop command. The
	// wrapper below forwards to it, so non-carrier units keep their normal
	// stop behaviour.
	for (auto& node : CommandClass::Hotkeys)
	{
		if (node.Data == pStop)
			node.Data = Instance;
	}

	rebound = true;
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
	// Preserve the vanilla Stop order for every other selected object.
	if (s_pStopCommand)
		s_pStopCommand->Execute(eInput);

	bool recalled = false;

	for (int i = 0; i < ObjectClass::CurrentObjects.Count; ++i)
	{
		if (auto const pTechno = abstract_cast<TechnoClass*>(ObjectClass::CurrentObjects.GetItem(i)))
			recalled |= Recall(pTechno);
	}

	if (recalled)
	{
		MessageListClass::Instance.PrintMessage(
			L"Carrier aircraft recalled.",
			RulesClass::Instance->MessageDelay,
			HouseClass::CurrentPlayer->ColorSchemeIndex,
			true
		);
	}
}

// Runs once per logic frame from LogicClass::Update. Rebinds the vanilla Stop
// hotkey after KeyboardMD.ini has been loaded.
DEFINE_HOOK(0x55B4E1, RecallCarrierAircraft_RebindStopHotkey, 0x5)
{
	RecallCarrierAircraftCommandClass::EnsureStopRebind();
	return 0;
}
