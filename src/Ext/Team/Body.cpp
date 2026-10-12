#include "Body.h"

#include <CellClass.h>
#include <MapClass.h>
#include <Unsorted.h>
#include <unordered_map>

#include <Utilities/Debug.h>

TeamExt::ExtContainer TeamExt::ExtMap;

// =============================
// load / save

template <typename T>
void TeamExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->WaitNoTargetAttempts)
		.Process(this->NextSuccessWeightAward)
		.Process(this->IdxSelectedObjectFromAIList)
		.Process(this->CloseEnough)
		.Process(this->Countdown_RegroupAtLeader)
		.Process(this->MoveMissionEndMode)
		.Process(this->WaitNoTargetCounter)
		.Process(this->WaitNoTargetTimer)
		.Process(this->ForceJump_Countdown)
		.Process(this->ForceJump_InitialCountdown)
		.Process(this->ForceJump_RepeatMode)
		.Process(this->TeamLeader)
		.Process(this->PreviousScriptList)
		;
}

void TeamExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TeamClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TeamExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TeamClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

void TeamExt::ExtData::InvalidatePointer(void* ptr, bool bRemoved)
{
	AnnounceInvalidPointer(TeamLeader, ptr);
}

// =============================
// container

TeamExt::ExtContainer::ExtContainer() : Container("TeamClass") { }
TeamExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

//Everything InitEd beside the Vector below this address
DEFINE_HOOK(0x6E8B46, TeamClass_CTOR, 0x7)
{
	GET(TeamClass*, pThis, ESI);

	TeamExt::ExtMap.TryAllocate(pThis);

	return 0;
}

//before `test` i hope not crash the game ,..
DEFINE_HOOK(0x6E8EC6, TeamClass_DTOR, 0x9)
{
	GET(TeamClass*, pThis, ESI);

	TeamExt::ExtMap.Remove(pThis);

	return 0;
}

DEFINE_HOOK_AGAIN(0x6EC450, TeamClass_SaveLoad_Prefix, 0x5)
DEFINE_HOOK(0x6EC540, TeamClass_SaveLoad_Prefix, 0x8)
{
	GET_STACK(TeamClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);

	TeamExt::ExtMap.PrepareStream(pItem, pStm);

	return 0;
}

DEFINE_HOOK(0x6EC52F, TeamClass_Load_Suffix, 0x6)
{
	TeamExt::ExtMap.LoadStatic();

	return 0;
}

DEFINE_HOOK(0x6EC55A, TeamClass_Save_Suffix, 0x5)
{
	TeamExt::ExtMap.SaveStatic();
	return 0;
}

// =============================
// leaving members cleanup

// { last beyond-map cell, frame it was first seen there }
// File scope so it can be cleared on scenario reset; otherwise keys from the
// previous scenario linger and can alias newly allocated feet.
static std::unordered_map<FootClass*, std::pair<CellStruct, int>> s_offMap;

void TeamExt::ClearOffMapState()
{
	s_offMap.clear();
}

void TeamExt::RemoveStuckLeavingMembers()
{
	const int frame = Unsorted::CurrentFrame;

	if (s_offMap.size() > 1024)
	{
		for (auto it = s_offMap.begin(); it != s_offMap.end();)
		{
			bool alive = false;
			for (auto const pTeam : TeamClass::Array)
			{
				for (auto pUnit = pTeam ? pTeam->FirstUnit : nullptr; pUnit; pUnit = pUnit->NextTeamMember)
				{
					if (pUnit == it->first) { alive = true; break; }
				}
				if (alive) break;
			}
			if (!alive)
				it = s_offMap.erase(it);
			else
				++it;
		}
	}

	for (auto const pTeam : TeamClass::Array)
	{
		if (!pTeam)
			continue;

		for (auto pUnit = pTeam->FirstUnit; pUnit; pUnit = pUnit->NextTeamMember)
		{
			if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive)
				continue;

			// Aircraft legitimately fly beyond the map during ordinary attacks and
			// return, so they are only culled when their team is actually leaving;
			// ground feet are culled whenever they have crossed the boundary.
			const auto what = pUnit->WhatAmI();

			if (what == AbstractType::Aircraft && !pTeam->IsLeavingMap)
				continue;

			if (what != AbstractType::Infantry && what != AbstractType::Unit && what != AbstractType::Aircraft)
				continue;

			const auto cell = CellClass::Coord2Cell(pUnit->Location);

			// Only units that truly crossed the map boundary have left the map.
			// Cells within the map but outside the usable area (the border strip)
			// are legitimate positions and must not be touched.
			if (MapClass::Instance.CoordinatesLegal(cell))
			{
				s_offMap.erase(pUnit);
				continue;
			}

			// Give the unit a short grace period once beyond the edge, then cull
			// it so leaving teams finish leaving instead of lingering off-map and
			// spamming pathfinding against an unreachable destination.
			auto& track = s_offMap[pUnit];

			if (track.first.X != cell.X || track.first.Y != cell.Y)
			{
				track.first = cell;
				track.second = frame;
				continue;
			}

			if (frame - track.second < 30)
				continue;

			s_offMap.erase(pUnit);

			Debug::Log("[LeaveFix] %s (%X) removed at (%d,%d)\n",
				pUnit->get_ID(), pUnit, cell.X, cell.Y);

			pUnit->Limbo();
		}
	}
}
