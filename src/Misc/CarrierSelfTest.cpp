#include "CarrierSelfTest.h"

#include <cmath>

#include <CellClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <Phobos.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Footprint.h>
#include <Utilities/Debug.h>

namespace
{
	bool s_enabled = false;
	UnitClass* s_carrier = nullptr;
	int s_startFrame = 0;
	int s_phase = 0;
	int s_phaseFrame = 0;
	CoordStruct s_slot0 {};
	CoordStruct s_takeoff {};

	FootprintState* State()
	{
		if (!s_carrier || !s_carrier->IsAlive)
			return nullptr;

		return Footprint::GetState(s_carrier);
	}

	UnitClass* FindExistingCarrier()
	{
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
				continue;

			if (!Footprint::IsMultiCell(pUnit))
				continue;

			if (TechnoTypeExt::IsAircraftCarrier(pUnit->GetTechnoType()))
				return pUnit;
		}

		return nullptr;
	}

	bool FindWaterCell(CellStruct& out)
	{
		const auto& rect = MapClass::Instance.MapRect;
		const CellStruct center
		{
			static_cast<short>(rect.X + rect.Width / 2),
			static_cast<short>(rect.Y + rect.Height / 2)
		};

		for (int radius = 0; radius < 96; ++radius)
		{
			for (int dy = -radius; dy <= radius; ++dy)
			{
				for (int dx = -radius; dx <= radius; ++dx)
				{
					if (radius > 0 && std::abs(dx) != radius && std::abs(dy) != radius)
						continue;

					const CellStruct cell
					{
						static_cast<short>(center.X + dx),
						static_cast<short>(center.Y + dy)
					};

					const auto pCell = MapClass::Instance.TryGetCellAt(cell);

					if (!pCell || pCell->LandType != LandType::Water || pCell->GetBuilding() || pCell->ContainsBridge())
						continue;

					out = cell;
					return true;
				}
			}
		}

		return false;
	}

	void LogDeck(const char* tag)
	{
		auto const pState = State();

		if (!pState)
			return;

		Debug::Log("[CarrierSelfTest] %s frame=%d carrier=(%d,%d) bucket=%d payloads=%d\n",
			tag, Unsorted::CurrentFrame, s_carrier->Location.X, s_carrier->Location.Y,
			pState->Bucket, static_cast<int>(pState->CarrierPayloads.size()));

		for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft)
				continue;

			const unsigned char state = i < pState->CarrierPayloadDocked.size()
				? pState->CarrierPayloadDocked[i]
				: 255;

			Debug::Log("[CarrierSelfTest]   slot %d state=%d mission=%d inAir=%d loc=(%d,%d,%d)"
				" facing=%u ammo=%d hasAmmo=%d\n",
				static_cast<int>(i), static_cast<int>(state),
				static_cast<int>(pAircraft->GetCurrentMission()),
				pAircraft->IsInAir() ? 1 : 0,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
				pAircraft->Ammo, TechnoExt::HasAnyWeaponAmmo(pAircraft) ? 1 : 0);
		}
	}
}

void CarrierSelfTest::Start()
{
	s_enabled = Phobos::Config::DebugCarrierSelfTest;

	if (!s_enabled)
		return;

	s_carrier = FindExistingCarrier();
	s_phase = 0;
	s_startFrame = Unsorted::CurrentFrame;
	s_phaseFrame = Unsorted::CurrentFrame;

	if (s_carrier)
	{
		Debug::Log("[CarrierSelfTest] using existing AIRCARR at (%d,%d)\n",
			s_carrier->Location.X, s_carrier->Location.Y);
		return;
	}

	// No carrier on the map: create one for the test.
	auto const pType = UnitTypeClass::Find("AIRCARR");

	if (!pType)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: UnitType AIRCARR not found\n");
		return;
	}

	CellStruct cell { -1, -1 };

	if (!FindWaterCell(cell))
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: no water cell\n");
		return;
	}

	auto const pCell = MapClass::Instance.TryGetCellAt(cell);

	if (!pCell)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: cell unavailable\n");
		return;
	}

	auto const pUnit = static_cast<UnitClass*>(pType->CreateObject(HouseClass::CurrentPlayer));

	if (!pUnit)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: CreateObject null\n");
		return;
	}

	const auto coords = CellClass::Cell2Coord(cell, pCell->GetCoords().Z);
	pUnit->SetLocation(coords);

	++Unsorted::ScenarioInit;
	const bool placed = pUnit->Unlimbo(coords, DirType(0));
	--Unsorted::ScenarioInit;

	if (!placed)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: Unlimbo failed\n");
		pUnit->UnInit();
		return;
	}

	s_carrier = pUnit;
	Debug::Log("[CarrierSelfTest] RESULT CREATE OK: AIRCARR at (%d,%d)\n", cell.X, cell.Y);
}

void CarrierSelfTest::Tick()
{
	static int lastFrame = -1;

	if (lastFrame == Unsorted::CurrentFrame)
		return;

	lastFrame = Unsorted::CurrentFrame;

	if (!s_enabled || !s_carrier)
		return;

	auto const pState = State();

	if (!pState || !s_carrier->IsAlive)
	{
		Debug::Log("[CarrierSelfTest] RESULT DECK FAIL: carrier lost\n");
		s_enabled = false;
		return;
	}

	if (!pState->CarrierPayloadsInitialized || pState->CarrierPayloads.empty())
	{
		if (Unsorted::CurrentFrame - s_startFrame > 240)
		{
			Debug::Log("[CarrierSelfTest] RESULT DECK FAIL: payloads not ready\n");
			s_enabled = false;
		}

		return;
	}

	if ((Unsorted::CurrentFrame - s_startFrame) % 60 == 0)
		LogDeck("tick");

	if (s_phase == 0)
	{
		if (Unsorted::CurrentFrame - s_startFrame < 90)
			return;

		auto const pAircraft = pState->CarrierPayloads[0];

		if (!pAircraft || !pAircraft->IsAlive)
		{
			Debug::Log("[CarrierSelfTest] RESULT TAKEOFF FAIL: payload 0 missing\n");
			s_enabled = false;
			return;
		}

		s_slot0 = pAircraft->Location;
		LogDeck("before-takeoff");

		// Equivalent to a left-click move: destination far from the ship.
		const auto carrierCell = s_carrier->GetMapCoords();
		const CellStruct targetCell
		{
			static_cast<short>(carrierCell.X + 10),
			static_cast<short>(carrierCell.Y + 6)
		};

		const auto pTargetCell = MapClass::Instance.TryGetCellAt(targetCell);

		if (!pTargetCell)
		{
			Debug::Log("[CarrierSelfTest] RESULT TAKEOFF SKIP: target cell invalid\n");
			s_enabled = false;
			return;
		}

		pAircraft->SetDestination(pTargetCell, true);
		pAircraft->QueueMission(Mission::Move, true);

		// The parked-state launch gate requires a selected payload (or an AI
		// carrier). Select it and use the same deferred intent as a real
		// left-click so this test actually exercises the runway takeoff.
		pAircraft->Select();
		Footprint::QueueCarrierTakeoffIntent(pAircraft, s_carrier);

		Debug::Log("[CarrierSelfTest] issued left-click move: slot0=(%d,%d,%d) target=(%d,%d) selected=%d\n",
			s_slot0.X, s_slot0.Y, s_slot0.Z, targetCell.X, targetCell.Y,
			pAircraft->IsSelected ? 1 : 0);

		s_phase = 1;
		s_phaseFrame = Unsorted::CurrentFrame;
		return;
	}

	if (s_phase == 1)
	{
		if (Unsorted::CurrentFrame - s_phaseFrame < 4)
			return;

		auto const pAircraft = pState->CarrierPayloads[0];

		if (!pAircraft)
		{
			Debug::Log("[CarrierSelfTest] RESULT TAKEOFF FAIL: payload 0 missing\n");
			s_enabled = false;
			return;
		}

		s_takeoff = pAircraft->Location;

		const double delta = s_takeoff.DistanceFrom(s_slot0);

		Debug::Log("[CarrierSelfTest] takeoff slot0=(%d,%d,%d) actual=(%d,%d,%d) delta=%.0f state=%d mission=%d\n",
			s_slot0.X, s_slot0.Y, s_slot0.Z,
			s_takeoff.X, s_takeoff.Y, s_takeoff.Z, delta,
			pState->CarrierPayloadDocked[0],
			static_cast<int>(pAircraft->GetCurrentMission()));

		// Simulate running out of ammo: clear both the legacy field and the
		// per-weapon pools, drop the target and let the state machine decide.
		pAircraft->Ammo = 0;
		pAircraft->SetTarget(nullptr);
		pAircraft->SetDestination(nullptr, true);
		pAircraft->QueueMission(Mission::Sleep, true);

		if (auto const pExt = TechnoExt::ExtMap.Find(pAircraft))
		{
			for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
				pExt->WeaponAmmoCount[slot] = 0;
		}

		Debug::Log("[CarrierSelfTest] ammo cleared, waiting for return/landing\n");

		s_phase = 2;
		s_phaseFrame = Unsorted::CurrentFrame;
		return;
	}

	if (s_phase == 2)
	{
		auto const pAircraft = pState->CarrierPayloads[0];
		const unsigned char state = pState->CarrierPayloadDocked[0];

		if (state == 1)
		{
			const double delta = pAircraft->Location.DistanceFrom(s_slot0);

			Debug::Log("[CarrierSelfTest] RESULT LANDING PASS in %d frames slot0=(%d,%d,%d) actual=(%d,%d,%d) delta=%.0f\n",
				Unsorted::CurrentFrame - s_phaseFrame,
				s_slot0.X, s_slot0.Y, s_slot0.Z,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				delta);

			LogDeck("after-landing");
			s_enabled = false;
			return;
		}

		if (Unsorted::CurrentFrame - s_phaseFrame > 900)
		{
			Debug::Log("[CarrierSelfTest] RESULT LANDING FAIL state=%d mission=%d inAir=%d loc=(%d,%d,%d)\n",
				static_cast<int>(state),
				pAircraft ? static_cast<int>(pAircraft->GetCurrentMission()) : -1,
				pAircraft ? (pAircraft->IsInAir() ? 1 : 0) : -1,
				pAircraft ? pAircraft->Location.X : 0,
				pAircraft ? pAircraft->Location.Y : 0,
				pAircraft ? pAircraft->Location.Z : 0);

			LogDeck("landing-timeout");
			s_enabled = false;
		}
	}
}
