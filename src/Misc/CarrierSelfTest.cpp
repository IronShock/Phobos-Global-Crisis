#include "CarrierSelfTest.h"

#include <cmath>

#include <Phobos.h>

#include <CellClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

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
	CellStruct s_slotCell { -1, -1 };

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

					if (!pCell || pCell->LandType != LandType::Water)
						continue;

					if (pCell->GetBuilding() || pCell->ContainsBridge())
						continue;

					out = cell;
					return true;
				}
			}
		}

		return false;
	}

	FootprintState* State()
	{
		if (!s_carrier || !s_carrier->IsAlive)
			return nullptr;

		return Footprint::GetState(s_carrier);
	}

	void LogDeck(const char* tag)
	{
		auto const pState = State();

		if (!pState)
			return;

		Debug::Log("[CarrierSelfTest] %s frame=%d carrier=(%d,%d) bucket=%d initialized=%d payloads=%d\n",
			tag, Unsorted::CurrentFrame, s_carrier->Location.X, s_carrier->Location.Y,
			pState->Bucket, pState->CarrierPayloadsInitialized ? 1 : 0,
			static_cast<int>(pState->CarrierPayloads.size()));

		for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft)
			{
				Debug::Log("[CarrierSelfTest]   slot %d null\n", static_cast<int>(i));
				continue;
			}

			const unsigned char state = i < pState->CarrierPayloadDocked.size()
				? pState->CarrierPayloadDocked[i]
				: 255;

			Debug::Log("[CarrierSelfTest]   slot %d state=%d mission=%d inAir=%d loc=(%d,%d,%d)"
				" facing=%u carrierFacing=%u ammo=%d hasAmmo=%d\n",
				static_cast<int>(i), static_cast<int>(state),
				static_cast<int>(pAircraft->GetCurrentMission()),
				pAircraft->IsInAir() ? 1 : 0,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				static_cast<unsigned>(pAircraft->PrimaryFacing.Current().Raw),
				static_cast<unsigned>(s_carrier->PrimaryFacing.Current().Raw),
				pAircraft->Ammo, TechnoExt::HasAnyWeaponAmmo(pAircraft) ? 1 : 0);
		}
	}
}

void CarrierSelfTest::Start()
{
	s_enabled = Phobos::Config::DebugCarrierSelfTest;

	if (!s_enabled)
		return;

	s_carrier = nullptr;
	s_phase = 0;
	s_phaseFrame = Unsorted::CurrentFrame;
	s_startFrame = Unsorted::CurrentFrame;

	auto const pType = UnitTypeClass::Find("AIRCARR");

	if (!pType)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: UnitType AIRCARR not found\n");
		return;
	}

	CellStruct cell { -1, -1 };

	if (!FindWaterCell(cell))
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: no water cell on this map\n");
		return;
	}

	auto const pCell = MapClass::Instance.TryGetCellAt(cell);

	if (!pCell)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: water cell (%d,%d) unavailable\n", cell.X, cell.Y);
		return;
	}

	s_slotCell = cell;

	auto const pUnit = static_cast<UnitClass*>(pType->CreateObject(HouseClass::CurrentPlayer));

	if (!pUnit)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: CreateObject returned null\n");
		return;
	}

	const auto coords = CellClass::Cell2Coord(cell, pCell->GetCoords().Z);
	pUnit->SetLocation(coords);

	++Unsorted::ScenarioInit;
	const bool placed = pUnit->Unlimbo(coords, DirType(0));
	--Unsorted::ScenarioInit;

	if (!placed)
	{
		Debug::Log("[CarrierSelfTest] RESULT CREATE FAIL: Unlimbo failed at (%d,%d)\n", cell.X, cell.Y);
		pUnit->UnInit();
		return;
	}

	s_carrier = pUnit;

	Debug::Log("[CarrierSelfTest] RESULT CREATE OK: AIRCARR at (%d,%d) coords=(%d,%d,%d)\n",
		cell.X, cell.Y, coords.X, coords.Y, coords.Z);
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
		if (s_phase < 90 && Unsorted::CurrentFrame - s_startFrame < 120)
			return;

		Debug::Log("[CarrierSelfTest] RESULT DECK FAIL: carrier lost or state missing\n");
		s_enabled = false;
		return;
	}

	if ((Unsorted::CurrentFrame - s_startFrame) % 30 == 0)
		LogDeck("tick");

	if (!pState->CarrierPayloadsInitialized)
	{
		if (Unsorted::CurrentFrame - s_startFrame > 180)
		{
			Debug::Log("[CarrierSelfTest] RESULT DECK FAIL: payloads not initialized\n");
			s_enabled = false;
		}

		return;
	}

	if (pState->CarrierPayloads.empty())
	{
		Debug::Log("[CarrierSelfTest] RESULT DECK FAIL: payload list empty\n");
		s_enabled = false;
		return;
	}

	if (s_phase == 0)
	{
		if (Unsorted::CurrentFrame - s_startFrame < 90)
			return;

		auto const pAircraft = pState->CarrierPayloads[0];

		if (!pAircraft || !pAircraft->IsAlive)
		{
			Debug::Log("[CarrierSelfTest] RESULT LANDING FAIL: payload 0 missing\n");
			s_enabled = false;
			return;
		}

		// Facing check: slots without an explicit AircraftDockingDir should
		// match the carrier facing.
		LogDeck("before-return");

		// Force the return path directly: state 2 is what UpdateCarrierPayload
		// uses for the manual glide back to the live deck slot.
		if (pState->CarrierPayloadDocked.size() < 1)
			pState->CarrierPayloadDocked.resize(1, 0);

		pState->CarrierPayloadDocked[0] = 2;

		CoordStruct away = pAircraft->Location;
		away.X += 2500;
		away.Y += 600;
		away.Z += 700;

		pAircraft->SetLocation(away);
		pAircraft->InAir = true;
		pAircraft->SetDestination(nullptr, true);
		pAircraft->QueueMission(Mission::Sleep, true);

		Debug::Log("[CarrierSelfTest] forced return: slot0 moved to (%d,%d,%d), state=2\n",
			away.X, away.Y, away.Z);

		s_phase = 1;
		s_phaseFrame = Unsorted::CurrentFrame;
		return;
	}

	if (s_phase == 1)
	{
		const unsigned char state = pState->CarrierPayloadDocked[0];

		if (state == 1)
		{
			Debug::Log("[CarrierSelfTest] RESULT LANDING PASS in %d frames\n",
				Unsorted::CurrentFrame - s_phaseFrame);

			LogDeck("after-landing");
			s_phase = 2;
			s_phaseFrame = Unsorted::CurrentFrame;
			return;
		}

		if (Unsorted::CurrentFrame - s_phaseFrame > 900)
		{
			auto const pAircraft = pState->CarrierPayloads[0];

			Debug::Log("[CarrierSelfTest] RESULT LANDING FAIL after %d frames: state=%d mission=%d inAir=%d loc=(%d,%d,%d)\n",
				Unsorted::CurrentFrame - s_phaseFrame, static_cast<int>(state),
				pAircraft ? static_cast<int>(pAircraft->GetCurrentMission()) : -1,
				pAircraft ? (pAircraft->IsInAir() ? 1 : 0) : -1,
				pAircraft ? pAircraft->Location.X : 0,
				pAircraft ? pAircraft->Location.Y : 0,
				pAircraft ? pAircraft->Location.Z : 0);

			LogDeck("landing-timeout");
			s_enabled = false;
		}

		return;
	}

	if (s_phase == 2)
	{
		if (Unsorted::CurrentFrame - s_phaseFrame < 30)
			return;

		auto const pAircraft = pState->CarrierPayloads[0];

		if (!pAircraft || !pAircraft->IsAlive)
		{
			Debug::Log("[CarrierSelfTest] RESULT LAUNCH FAIL: payload 0 missing\n");
			s_enabled = false;
			return;
		}

		const CellStruct targetCell
		{
			static_cast<short>(s_slotCell.X + 4),
			static_cast<short>(s_slotCell.Y)
		};

		const auto pTargetCell = MapClass::Instance.TryGetCellAt(targetCell);

		if (!pTargetCell)
		{
			Debug::Log("[CarrierSelfTest] RESULT LAUNCH SKIP: target cell unavailable\n");
			s_enabled = false;
			return;
		}

		pAircraft->SetDestination(pTargetCell, true);
		pAircraft->QueueMission(Mission::Move, true);

		Debug::Log("[CarrierSelfTest] forced launch: Move to (%d,%d)\n", targetCell.X, targetCell.Y);

		s_phase = 3;
		s_phaseFrame = Unsorted::CurrentFrame;
		return;
	}

	if (s_phase == 3)
	{
		auto const pAircraft = pState->CarrierPayloads[0];

		if (!pAircraft)
		{
			Debug::Log("[CarrierSelfTest] RESULT LAUNCH FAIL: payload 0 missing\n");
			s_enabled = false;
			return;
		}

		const unsigned char state = pState->CarrierPayloadDocked[0];

		if (state == 0 && pAircraft->IsInAir())
		{
			Debug::Log("[CarrierSelfTest] RESULT LAUNCH PASS in %d frames loc=(%d,%d,%d)\n",
				Unsorted::CurrentFrame - s_phaseFrame,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);

			LogDeck("after-launch");
			s_phase = 4;
			s_enabled = false;
			Debug::Log("[CarrierSelfTest] RESULT SELFTEST COMPLETE\n");
			return;
		}

		if (Unsorted::CurrentFrame - s_phaseFrame > 120)
		{
			Debug::Log("[CarrierSelfTest] RESULT LAUNCH FAIL after %d frames: state=%d mission=%d inAir=%d loc=(%d,%d,%d)\n",
				Unsorted::CurrentFrame - s_phaseFrame, static_cast<int>(state),
				static_cast<int>(pAircraft->GetCurrentMission()),
				pAircraft->IsInAir() ? 1 : 0,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z);

			LogDeck("launch-timeout");
			s_enabled = false;
		}
	}
}
