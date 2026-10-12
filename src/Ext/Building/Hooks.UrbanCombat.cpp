#include "Body.h"

#include <algorithm>

#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <UnitClass.h>
#include <AircraftClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <TechnoTypeClass.h>
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>
#include <SpecificStructures.h>

#include <Ext/Building/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Rules/Body.h>
#include <Utilities/Debug.h>

// ============================================================================
// UrbanCombat
//
// An infantry with [InfantryType]UrbanCombat=yes does not auto-attack garrisoned
// enemy buildings. A normal right-click on a garrisoned enemy building makes the
// infantry run inside and start an indoor battle with the building's occupants.
// Force-firing still damages the building normally.
//
// During the battle the building's occupants stop firing at outside units and
// the battle is simulated in real time with focus-fire: every frame the attacker's
// combat power (sum of current HP * weapon DPS) is applied to one defender at a
// time, so the garrison takes casualties one by one; the defenders fight back the
// same way against the UC attackers. [General]UrbanCombat.AttackBuff= multiplies
// the attacker's power, so it shifts the win chance in the attacker's favor.
// [General]UrbanCombat.Rate= scales the battle speed. The building itself never
// takes damage from the indoor fight (it is fully simulated).
//
//   - attacker wins  : defenders are killed, the building is captured by the
//                      attacker's house and the surviving attackers become the
//                      new occupants.
//   - defender wins  : the (limboed) attackers are killed, the building stays
//                      with the defenders.
// ============================================================================

namespace UrbanCombatHelper
{
	static double GetWeaponDPS(WeaponTypeClass* pWeapon)
	{
		if (!pWeapon || pWeapon->ROF <= 0)
			return 0.0;

		return static_cast<double>(pWeapon->Damage) / static_cast<double>(pWeapon->ROF);
	}

	static double GetOccupyDPS(InfantryClass* pInf)
	{
		auto const pType = pInf->Type;
		WeaponTypeClass* pWeapon = pInf->Veterancy.IsElite() ? pType->EliteOccupyWeapon.WeaponType : pType->OccupyWeapon.WeaponType;
		return GetWeaponDPS(pWeapon);
	}

	static double GetPrimaryDPS(InfantryClass* pInf)
	{
		if (auto const pWeaponStruct = pInf->GetWeapon(0))
			return GetWeaponDPS(pWeaponStruct->WeaponType);

		return 0.0;
	}

	// DPS used by a DEFENDING occupant. A garrison defender need not carry the
	// UrbanCombat tag, and its indoor (Occupy) weapon may be weak or missing, so
	// fall back to the stronger of the occupy weapon and the primary weapon.
	// Otherwise a non-UC garrison (e.g. a bazooka anti-tank soldier) deals no
	// damage and the attacker wins the indoor fight for free.
	static double GetDefenderDPS(InfantryClass* pInf)
	{
		return Math::max(GetOccupyDPS(pInf), GetPrimaryDPS(pInf));
	}
}

// Registry of buildings that currently have an indoor battle running. The
// per-frame update and the defender-order scan only need to touch contested
// buildings; iterating the whole building array (and, for the order scan, the
// whole army) every frame was a major source of lag.
static std::vector<BuildingClass*> ActiveUrbanCombats;

static void RegisterUrbanCombat(BuildingClass* pBuilding)
{
	if (!pBuilding)
		return;

	for (auto const pExisting : ActiveUrbanCombats)
	{
		if (pExisting == pBuilding)
			return;
	}

	ActiveUrbanCombats.push_back(pBuilding);
}

static void UnregisterUrbanCombat(BuildingClass* pBuilding)
{
	for (auto it = ActiveUrbanCombats.begin(); it != ActiveUrbanCombats.end(); ++it)
	{
		if (*it == pBuilding)
		{
			ActiveUrbanCombats.erase(it);
			return;
		}
	}
}

void BuildingExt::UnregisterActiveUrbanCombat(BuildingClass* pBuilding)
{
	UnregisterUrbanCombat(pBuilding);
}

// Called on scenario clear: the registered buildings belong to the scenario
// being torn down, so drop every entry or the next scenario's per-frame update
// would dereference dangling BuildingClass pointers.
void BuildingExt::ClearActiveUrbanCombats()
{
	ActiveUrbanCombats.clear();
}

bool BuildingExt::HasActiveUrbanCombats()
{
	return !ActiveUrbanCombats.empty();
}

void BuildingExt::UpdateAllUrbanCombats()
{
	// Copy the list: UpdateUrbanCombat may resolve/abort a battle, which
	// unregisters (erases) the entry from ActiveUrbanCombats and would otherwise
	// invalidate the iterator.
	auto const battles = ActiveUrbanCombats;

	for (auto const pBuilding : battles)
	{
		if (pBuilding)
			BuildingExt::UpdateUrbanCombat(pBuilding);
	}
}

// Runs once per logic frame (LogicClass::Update, before all). Only the buildings
// with an indoor battle are simulated - the registry is empty in the common case,
// so this costs nothing when no UrbanCombat is in progress.
DEFINE_HOOK(0x55B4E1, LogicClass_Update_UrbanCombat, 0x5)
{
	if (BuildingExt::HasActiveUrbanCombats())
		BuildingExt::UpdateAllUrbanCombats();

	// Fallback: pick up a UC infantry that became an occupant of an enemy
	// garrisoned building without the entry hooks having started the battle.
	BuildingExt::ScanForNewUrbanCombats();

	return 0;
}

bool BuildingExt::IsUrbanCombatInfantry(TechnoClass* pTechno)
{
	if (!pTechno || pTechno->WhatAmI() != AbstractType::Infantry)
		return false;

	return TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType())->UrbanCombat;
}

bool BuildingExt::IsGarrisonedEnemyBuilding(TechnoClass* pOwner, BuildingClass* pBuilding)
{
	if (!pBuilding || !pOwner || !pBuilding->IsAlive)
		return false;

	if (!pBuilding->Type->CanBeOccupied)
		return false;

	if (pBuilding->Occupants.Count <= 0)
		return false;

	// Respect the building's garrison capacity limit - a full building cannot be
	// entered by a UC attacker, so no indoor battle on a full garrison.
	if (pBuilding->Occupants.Count >= pBuilding->Type->MaxNumberOccupants)
		return false;

	if (pBuilding->Owner->IsAlliedWith(pOwner->Owner))
		return false;

	return true;
}

// A defender-side UrbanCombat infantry may enter its own building while an
// indoor battle is running, to reinforce the garrison. The building owner is
// the defender side, so the entering infantry is allied with the owner. The
// building must be occupiable and not full.
bool BuildingExt::IsDefenderReinforce(TechnoClass* pOwner, BuildingClass* pBuilding)
{
	if (!pBuilding || !pOwner || !pBuilding->IsAlive)
		return false;

	if (!pBuilding->Type->CanBeOccupied)
		return false;

	if (pBuilding->Occupants.Count <= 0)
		return false;

	// A full building cannot accept more defenders.
	if (pBuilding->Occupants.Count >= pBuilding->Type->MaxNumberOccupants)
		return false;

	// The entering infantry must be on the building owner's side (defender).
	if (!pBuilding->Owner->IsAlliedWith(pOwner->Owner))
		return false;

	// Must be an UrbanCombat infantry entering a building under indoor combat.
	if (!BuildingExt::IsUrbanCombatInfantry(pOwner))
		return false;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	if (!pExt || pExt->UrbanCombat_State != ExtData::UrbanCombatState::Active)
		return false;

	return true;
}

// Attackers and defenders are split by alliance with the BUILDING OWNER, which
// is unambiguous and cannot be reversed by whichever unit was picked as the
// initiating attacker. Defenders are the building owner's garrison (UC-tagged or
// not); attackers are the UC invaders (enemy of the owner). Only UC-tagged
// infantry may be attackers - a stray non-UC enemy garrison (e.g. a GGI) must
// never be enrolled as an attacker, it is cleared up by the resolution instead.
static bool IsAttacker(TechnoClass* pTech, HouseClass* pOwnerHouse)
{
	return pTech && pOwnerHouse && BuildingExt::IsUrbanCombatInfantry(pTech) && !pTech->Owner->IsAlliedWith(pOwnerHouse);
}

static bool IsDefender(TechnoClass* pTech, HouseClass* pOwnerHouse)
{
	return pTech && pOwnerHouse && pTech->Owner->IsAlliedWith(pOwnerHouse);
}

// An "orphan occupant" is an infantry the game still treats as garrisoned
// (Transporter == this building) but that is missing from the building's
// Occupants list. This desync happens in vanilla (the game even logs
// "Infantry X was garrisoned in building Y, but building didn't find it. WTF?").
// Every part of the indoor-battle code iterates Occupants only, so an orphan is
// invisible to it: the battle concludes "no defender" and instantly resolves as
// an attacker win, while the orphan stays attached to the captured building and
// fires for its new owner. Re-adopt orphans so they are tracked again.
static bool IsOrphanOccupant(BuildingClass* pBuilding, InfantryClass* pInf)
{
	return pBuilding && pInf && pInf->IsAlive && pInf->Transporter == pBuilding
		&& pBuilding->Occupants.FindItemIndex(pInf) == -1;
}

static int AdoptOrphanOccupants(BuildingClass* pBuilding, const char* where)
{
	if (!pBuilding)
		return 0;

	const int maxOcc = pBuilding->Type->MaxNumberOccupants;
	int adopted = 0;

	for (auto const pInf : InfantryClass::Array)
	{
		if (!IsOrphanOccupant(pBuilding, pInf))
			continue;

		// Respect the capacity. If the list is already full we cannot re-adopt;
		// log it so the desync is at least visible.
		if (maxOcc > 0 && pBuilding->Occupants.Count >= maxOcc)
		{
			Debug::Log("[Phobos::UrbanCombat] Orphan NOT adopted (full %d/%d): inf=%s owner=%d bld=%s (%s)\n",
				pBuilding->Occupants.Count, maxOcc, pInf->Type->get_ID(),
				pInf->Owner ? pInf->Owner->ArrayIndex : -1, pBuilding->Type->get_ID(), where);
			continue;
		}

		pBuilding->Occupants.AddItem(pInf);
		++adopted;

		Debug::Log("[Phobos::UrbanCombat] Adopted orphan occupant: inf=%s owner=%d bld=%s (now %d/%d, %s)\n",
			pInf->Type->get_ID(), pInf->Owner ? pInf->Owner->ArrayIndex : -1,
			pBuilding->Type->get_ID(), pBuilding->Occupants.Count, maxOcc, where);
	}

	return adopted;
}

// Absorb a UC attacker into the indoor battle: limbo it and hand it to the
// battle simulation. Shared by the entry hooks and by the capture interception
// (which stops vanilla from transferring the garrison before the fight).
static void AbsorbUrbanCombatAttacker(BuildingClass* pBuilding, InfantryClass* pAttacker)
{
	if (!pBuilding || !pAttacker)
		return;

	pAttacker->Limbo();
	pAttacker->OnBridge = false;
	pAttacker->NextObject = nullptr;
	pAttacker->SetDestination(nullptr, true);
	pAttacker->SetTarget(nullptr);
	pAttacker->ShouldEnterAbsorber = false;
	pAttacker->ShouldEnterOccupiable = false;
	pAttacker->ShouldGarrisonStructure = false;

	BuildingExt::StartUrbanCombat(pBuilding, pAttacker);
	pAttacker->QueueMission(Mission::None, false);

	Debug::Log("[Phobos::UrbanCombat] Absorbed attacker: inf=%s owner=%d bld=%s owner=%d occupants=%d\n",
		pAttacker->Type->get_ID(), pAttacker->Owner ? pAttacker->Owner->ArrayIndex : -1,
		pBuilding->Type->get_ID(), pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1,
		pBuilding->Occupants.Count);
}

// Find the UrbanCombat infantry that is "grabbing" this building. Vanilla does
// not hand the capturing unit to BuildingClass::Captured (the new owner is the
// only argument), so locate it by scanning the infantry array for a UC unit on
// the new owner's side that has this building as its destination (or is right
// next to it, in case the destination was already cleared) and is on the
// Capture/Enter mission. Returns the closest such unit, or nullptr.
static InfantryClass* FindUrbanCombatCapturer(BuildingClass* pBuilding, HouseClass* pNewOwner)
{
	if (!pBuilding || !pNewOwner)
		return nullptr;

	InfantryClass* pBest = nullptr;
	int bestDist = 1 << 30;

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || !pInf->IsAlive || !BuildingExt::IsUrbanCombatInfantry(pInf))
			continue;

		if (!pInf->Owner || !pInf->Owner->IsAlliedWith(pNewOwner))
			continue;

		const int dist = pInf->DistanceFrom(pBuilding);

		// The building must be the target it is heading to; if the destination
		// was cleared, accept a unit standing right next to the building.
		if (pInf->Destination != pBuilding && dist > 2 * Unsorted::LeptonsPerCell)
			continue;

		const auto mission = pInf->GetCurrentMission();

		if (mission != Mission::Capture && mission != Mission::Enter)
			continue;

		if (dist < bestDist)
		{
			bestDist = dist;
			pBest = pInf;
		}
	}

	return pBest;
}

// Defined after ClampFiringOccupantIndex; declared here because the per-frame
// update / start / scan helpers below use it before the definition.
static int SanitizeBuildingOccupants(BuildingClass* pBuilding);

void BuildingExt::StartUrbanCombat(BuildingClass* pBuilding, InfantryClass* pAttacker)
{
	if (!pBuilding || !pAttacker)
		return;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	// Never enumerate a garrison that may contain a destroyed/freed pointer.
	SanitizeBuildingOccupants(pBuilding);

	if (pExt->UrbanCombat_State == ExtData::UrbanCombatState::None)
	{
		pExt->UrbanCombat_State = ExtData::UrbanCombatState::Active;
		pExt->UrbanCombat_AttackerHouse = pAttacker->Owner;
		RegisterUrbanCombat(pBuilding);

		// Re-adopt any garrison unit that vanilla left out of Occupants; without
		// this the defenders below are invisible and the battle resolves instantly.
		const int adopted = AdoptOrphanOccupants(pBuilding, "start");

		double defenderHP = 0.0;
		int defenderCount = 0;

		for (int i = 0; i < pBuilding->Occupants.Count; ++i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			// Attackers stay in Occupants now (removing them broke the garrison
			// tracking), so only the building owner's allied occupants count as
			// defenders - enemy garrisons may use the UC-tagged type too.
			if (IsDefender(pOcc, pBuilding->Owner))
			{
				defenderHP += pOcc->Health;

				Debug::Log("[Phobos::UrbanCombat]   defender[%d]: %s owner=%d hp=%d dps=%.2f (occupy=%.2f primary=%.2f)\n",
					defenderCount, pOcc->Type->get_ID(), pOcc->Owner ? pOcc->Owner->ArrayIndex : -1,
					pOcc->Health, UrbanCombatHelper::GetDefenderDPS(pOcc),
					UrbanCombatHelper::GetOccupyDPS(pOcc), UrbanCombatHelper::GetPrimaryDPS(pOcc));

				++defenderCount;
			}
		}

		pExt->UrbanCombat_DefenderHP = Math::max(defenderHP, 0.0);
		pExt->UrbanCombat_AttackerHP = 0.0;

		pExt->UrbanCombat_DefenderFocus = nullptr;
		pExt->UrbanCombat_DefenderFocusAccum = 0.0;
		pExt->UrbanCombat_AttackerFocus = nullptr;
		pExt->UrbanCombat_AttackerFocusAccum = 0.0;

		pBuilding->Target = nullptr;
		pBuilding->Mark(MarkType::Change);

		Debug::Log("[Phobos::UrbanCombat] Start: bld=%s owner=%d attacker=%s owner=%d occupants=%d defenders=%d adopted=%d defHP=%.0f\n",
			pBuilding->Type->get_ID(), pBuilding->Owner->ArrayIndex,
			pAttacker->Type->get_ID(), pAttacker->Owner->ArrayIndex,
			pBuilding->Occupants.Count, defenderCount, adopted, pExt->UrbanCombat_DefenderHP);
	}

	// Avoid double-enrolling the same attacker (the entry hooks may fire twice
	// for the same infantry on repeated orders).
	for (auto const pExisting : pExt->UrbanCombat_Attackers)
	{
		if (pExisting == pAttacker)
			return;
	}

	pExt->UrbanCombat_Attackers.push_back(pAttacker);
	pExt->UrbanCombat_AttackerHP += Math::max(pAttacker->Health, 1);
}

// Returns true if the building has an occupant other than the given attacker that
// is not allied with the attacker (a defender). The defender can be any infantry.
static bool HasEnemyOccupant(BuildingClass* pBuilding, InfantryClass* pAttacker)
{
	for (int i = 0; i < pBuilding->Occupants.Count; ++i)
	{
		auto const pOcc = pBuilding->Occupants.GetItem(i);

		// Address-only membership test first: never dereference a freed pointer.
		if (pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1 && pOcc->IsAlive
			&& pOcc != pAttacker && !pOcc->Owner->IsAlliedWith(pAttacker->Owner))
		{
			return true;
		}
	}

	return false;
}

// Fallback detection. The entry hooks can be bypassed, and a garrisoned infantry
// is hidden in limbo inside its building (so an infantry-list scan cannot see
// it). Walk the building list on a coarse, staggered schedule and start the
// indoor battle for any building that holds a UC attacker alongside an enemy
// garrison but has no battle running. This mirrors the (otherwise unreachable)
// auto-detect in UpdateUrbanCombat and does not depend on any entry path.
void BuildingExt::ScanForNewUrbanCombats()
{
	// Keep it off the critical path: once every 15 logic frames.
	if (Unsorted::CurrentFrame % 15 != 0)
		return;

	for (auto const pBuilding : BuildingClass::Array)
	{
		if (!pBuilding || !pBuilding->IsAlive)
			continue;

		// Cheap gates first: most buildings have no garrison.
		if (pBuilding->Occupants.Count <= 0 || !pBuilding->Type->CanBeOccupied)
			continue;

		auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

		if (!pExt || pExt->UrbanCombat_State != ExtData::UrbanCombatState::None)
			continue;

		SanitizeBuildingOccupants(pBuilding);

		for (int i = 0; i < pBuilding->Occupants.Count; ++i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			// The initiating attacker is a UC infantry that is NOT on the
			// building owner's side, with an enemy garrison present.
			if (!pOcc || InfantryClass::Array.FindItemIndex(pOcc) == -1
				|| !pOcc->IsAlive || !BuildingExt::IsUrbanCombatInfantry(pOcc))
			{
				continue;
			}

			if (pBuilding->Owner->IsAlliedWith(pOcc->Owner))
				continue;

			if (!HasEnemyOccupant(pBuilding, pOcc))
				continue;

			Debug::Log("[Phobos::UrbanCombat] Fallback start: inf[%s] inside enemy building[%s] (occupants=%d)\n",
				pOcc->Type->get_ID(), pBuilding->Type->get_ID(), pBuilding->Occupants.Count);

			BuildingExt::StartUrbanCombat(pBuilding, pOcc);
			break;
		}
	}
}

// Kill an infantry: pass a damage of at least 1 so ReceiveDamage actually
// triggers death (0 damage is a no-op, which would leave the unit alive with
// Health 0 and stall the battle).
static void KillInfantry(InfantryClass* pOcc, TechnoClass* pAssaulter)
{
	if (!pOcc)
		return;

	int damage = Math::max(pOcc->Health, 1);
	pOcc->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead, pAssaulter, true, false, nullptr);
}

// Keep the building's firing index inside the (possibly shrunken) occupant list.
// The game reads Occupants[FiringOccupantIndex] without a bounds check (see
// Hooks.Firing.cpp and BuildingExt::CanOccupantsFire), so a stale index left
// behind after the UrbanCombat code removes occupants yields a garbage Techno
// pointer and the following virtual call (call [vtable+0x3F8]) jumps into the
// weeds. Always re-clamp after mutating Occupants.
static void ClampFiringOccupantIndex(BuildingClass* pBuilding)
{
	if (!pBuilding)
		return;

	const int count = pBuilding->Occupants.Count;

	if (count <= 0)
	{
		pBuilding->FiringOccupantIndex = 0;
		return;
	}

	// Keep the current index if it still points at a valid entry, otherwise fall
	// back to the first non-null one. A null/garbage entry selected here is what
	// the occupy-fire code later dereferences as a TechnoClass*.
	if (pBuilding->FiringOccupantIndex >= 0 && pBuilding->FiringOccupantIndex < count
		&& pBuilding->Occupants[pBuilding->FiringOccupantIndex])
	{
		return;
	}

	for (int i = 0; i < count; ++i)
	{
		if (pBuilding->Occupants[i])
		{
			pBuilding->FiringOccupantIndex = i;
			return;
		}
	}

	pBuilding->FiringOccupantIndex = 0;
}

// Remove only Occupants entries whose object is no longer a registered infantry
// (i.e. a destroyed/freed pointer). This is the last line of defence against the
// use-after-free that crashed the vanilla occupant-eject code
// (BuildingClass::RemoveOccupants at 0x457DE0 dereferences occupant->Owner).
//
// The test is deliberately limited to InfantryClass::Array membership: a live
// garrison infantry is always registered there, and a freed one never is. The
// old test also required Transporter == building, which is NOT true for normal
// garrison entry/capture at the moment this runs, and wrongly evicted live,
// registered infantry (units entering a building were being lost).
//
// The membership test compares addresses only, so a possibly-freed pointer is
// never dereferenced.
static int SanitizeBuildingOccupants(BuildingClass* pBuilding)
{
	if (!pBuilding)
		return 0;

	auto& occupants = pBuilding->Occupants;
	int removed = 0;

	for (int i = occupants.Count - 1; i >= 0; --i)
	{
		InfantryClass* const pOcc = occupants.GetItem(i);

		// Only drop objects that are gone from the game's live infantry list.
		if (pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1)
			continue;

		Debug::Log("[Phobos::UrbanCombat] Sanitize: dropped DEAD occupant[%d] ptr=%p bld=%s (count=%d)\n",
			i, static_cast<void*>(pOcc),
			pBuilding->Type ? pBuilding->Type->get_ID() : "<none>", occupants.Count);

		occupants.RemoveItem(i);
		++removed;
	}

	if (removed)
		ClampFiringOccupantIndex(pBuilding);

	return removed;
}

// The vanilla occupant-eject routine dereferences occupant->Owner before
// ejecting, which crashes (C0000005 reading +0x1EC) if a destroyed infantry
// pointer was left behind in Occupants. Purge invalid entries before the vanilla
// body runs, then continue normally.
DEFINE_HOOK(0x457DE0, BuildingClass_RemoveOccupants_Sanitize, 0x6)
{
	GET(BuildingClass* const, pThis, ECX);

	SanitizeBuildingOccupants(pThis);

	return 0;
}

// Kill an occupant killed by the indoor fight - remove it from Occupants
// (clearing Transporter avoids the game's "garrisoned but didn't find it" eject
// check) and finish it off.
static void KillOccupant(BuildingClass* pBuilding, InfantryClass* pOcc, TechnoClass* pAssaulter)
{
	if (!pBuilding || !pOcc)
		return;

	const int idx = pBuilding->Occupants.FindItemIndex(pOcc);

	if (idx != -1)
		pBuilding->Occupants.RemoveItem(idx);

	ClampFiringOccupantIndex(pBuilding);

	pOcc->Transporter = nullptr;
	KillInfantry(pOcc, pAssaulter);
}

// Drop a (possibly dead/freed) attacker from the tracked attacker list so the
// vector never keeps a dangling pointer to a destroyed infantry.
static void RemoveAttackerFromList(BuildingExt::ExtData* pExt, InfantryClass* pAtt)
{
	if (!pExt || !pAtt)
		return;

	auto& attackers = pExt->UrbanCombat_Attackers;
	for (auto it = attackers.begin(); it != attackers.end(); ++it)
	{
		if (*it == pAtt)
		{
			attackers.erase(it);
			break;
		}
	}
}

void BuildingExt::UpdateUrbanCombat(BuildingClass* pBuilding)
{
	if (!pBuilding)
		return;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	// Defensive: bail out if the extension was freed/recycled (the building was
	// destroyed while an indoor battle was still tracked) - the vector member
	// may be corrupted and any use would be a use-after-free.
	if (!pExt || pExt->OwnerObject() != pBuilding)
		return;

	// Purge dead/freed entries before any of the loops below dereference an
	// occupant (a battle in progress can kill garrison units from outside).
	SanitizeBuildingOccupants(pBuilding);

	// A UC infantry may have garrisoned this building (via any entry path) as a
	// normal occupant alongside enemy defenders. Detect that and start the indoor
	// battle - the defender does not need to be a UC infantry itself.
	//
	// The initiating attacker is the UC infantry that is NOT on the building
	// owner's side (the side that entered the building). The building owner is
	// always the defender side. So an owner-side UC garrison must never be
	// picked as the attacker - otherwise attacker/defender would be inverted.
	if (pExt->UrbanCombat_State == ExtData::UrbanCombatState::None)
	{
		InfantryClass* pInitiator = nullptr;

		for (int i = 0; i < pBuilding->Occupants.Count; ++i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			if (!pOcc || !pOcc->IsAlive || !BuildingExt::IsUrbanCombatInfantry(pOcc) || !HasEnemyOccupant(pBuilding, pOcc))
				continue;

			// Only a UC infantry from OUTSIDE the building owner's side may be
			// the initiating attacker - the owner's own UC (or an allied UC
			// garrison) is a defender. Compare by alliance, not by equality, so
			// an allied UC garrison can never be picked and invert the roles.
			if (pBuilding->Owner->IsAlliedWith(pOcc->Owner))
				continue;

			pInitiator = pOcc;
			break;
		}

		if (pInitiator)
		{
			BuildingExt::StartUrbanCombat(pBuilding, pInitiator);
			return;
		}
	}

	if (pExt->UrbanCombat_State != ExtData::UrbanCombatState::Active)
		return;

	if (pBuilding->GetCurrentMission() == Mission::Selling)
	{
		AbortUrbanCombat(pBuilding);
		return;
	}

	// Building died (or defenders vanished) - attackers take over what is left.
	if (!pBuilding->IsAlive || pBuilding->Occupants.Count <= 0)
	{
		Debug::Log("[Phobos::UrbanCombat] Resolve trigger: ATTACKER WINS bld=%s (dead=%d occupants=%d)\n",
			pBuilding->Type->get_ID(), pBuilding->IsAlive ? 0 : 1, pBuilding->Occupants.Count);

		ResolveUrbanCombat(pBuilding, true);
		return;
	}

	if (pExt->UrbanCombat_Attackers.empty())
	{
		Debug::Log("[Phobos::UrbanCombat] Abort trigger: no tracked attackers bld=%s occupants=%d\n",
			pBuilding->Type->get_ID(), pBuilding->Occupants.Count);

		AbortUrbanCombat(pBuilding);
		return;
	}

	// Periodically order idle defender-side units near the contested building to
	// attack it, so the defending side actively fights the invaders from outside,
	// and re-adopt any garrison unit that vanilla dropped out of Occupants after
	// the battle started. The scan walks the whole army, so run it rarely and
	// stagger it per building (by its pointer) to avoid every contested building
	// spiking the same frame.
	if (((Unsorted::CurrentFrame + static_cast<int>(reinterpret_cast<uintptr_t>(pBuilding) >> 4)) % 45) == 0)
	{
		BuildingExt::OrderDefendersToAttackUC(pBuilding);
		AdoptOrphanOccupants(pBuilding, "update");

		// Drop any dead/freed infantry pointer that found its way into the
		// garrison while the battle is running (the per-frame update would
		// otherwise keep dereferencing it).
		SanitizeBuildingOccupants(pBuilding);
	}

	// Any infantry of the attacker side that are occupants but not yet tracked
	// as attackers join the battle (they may have entered after it started).
	// Enemy UC-tagged garrisons must NOT be enrolled - they are defenders.
	for (int i = 0; i < pBuilding->Occupants.Count; ++i)
	{
		auto const pOcc = pBuilding->Occupants.GetItem(i);

		if (!pOcc || !pOcc->IsAlive || !IsAttacker(pOcc, pBuilding->Owner))
			continue;

		// An enrolled attacker is a regular occupant, so it must expose the
		// garrison invariant as well.
		if (pOcc->Transporter != pBuilding)
			pOcc->Transporter = pBuilding;

		bool inList = false;

		for (auto const pAtt : pExt->UrbanCombat_Attackers)
		{
			if (pAtt == pOcc)
			{
				inList = true;
				break;
			}
		}

		if (!inList)
		{
			pExt->UrbanCombat_Attackers.push_back(pOcc);
			pExt->UrbanCombat_AttackerHP += Math::max(pOcc->Health, 1);
		}
	}

	pBuilding->Target = nullptr;

	double defenderPower = 0.0;

	for (int i = 0; i < pBuilding->Occupants.Count; ++i)
	{
		auto const pOcc = pBuilding->Occupants.GetItem(i);

		// Defenders are the enemy (non-allied) occupants - including any that
		// happen to carry the UrbanCombat tag. Use the stronger of the occupy
		// and primary weapons so a non-UC garrison can actually fight back.
		if (IsDefender(pOcc, pBuilding->Owner))
			defenderPower += UrbanCombatHelper::GetDefenderDPS(pOcc) * pOcc->Health;
	}

	// Defenders may reinforce mid-battle (defender-side UC infantry entering
	// their own contested building). Track the current total defender HP and
	// raise the DefenderHP pool whenever new defenders have joined, so the pool
	// always reflects the actual garrison. The pool only ever rises by the
	// amount of freshly-entered HP - it still drains normally from combat.
	double currentDefenderHP = 0.0;

	for (int i = 0; i < pBuilding->Occupants.Count; ++i)
	{
		auto const pOcc = pBuilding->Occupants.GetItem(i);

		if (IsDefender(pOcc, pBuilding->Owner))
			currentDefenderHP += pOcc->Health;
	}

	pExt->UrbanCombat_DefenderHP = Math::max(pExt->UrbanCombat_DefenderHP, currentDefenderHP);

	double attackerPower = 0.0;

	for (auto const pAtt : pExt->UrbanCombat_Attackers)
	{
		if (pAtt && pAtt->IsAlive)
			attackerPower += UrbanCombatHelper::GetPrimaryDPS(pAtt) * pAtt->Health;
	}

	auto const pRules = RulesExt::Global();

	if (!pRules)
		return;

	const double buff = Math::max(static_cast<double>(pRules->UrbanCombat_AttackBuff), 0.0);
	const double rate = Math::max(static_cast<double>(pRules->UrbanCombat_Rate), 0.0);
	// Indoor battle time base is fixed at 36000 frames (Rate / 36000 = per-frame
	// sim scale). Larger Rate = faster battle.
	const double scale = rate / 36000.0;

	attackerPower *= (1.0 + buff);

	// The attacker's indoor firepower focuses one defender at a time, so the
	// garrison takes casualties one by one during the battle. Fractional damage
	// accumulates so Health drains smoothly regardless of the per-frame rate.
	const double dmgToDefenders = attackerPower * scale;

	if (dmgToDefenders > 0.0)
	{
		InfantryClass* pDef = nullptr;

		for (int i = 0; i < pBuilding->Occupants.Count; ++i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			if (IsDefender(pOcc, pBuilding->Owner))
			{
				pDef = pOcc;
				break;
			}
		}

		if (pDef != pExt->UrbanCombat_DefenderFocus)
		{
			pExt->UrbanCombat_DefenderFocus = pDef;
			pExt->UrbanCombat_DefenderFocusAccum = 0.0;
		}

		if (pDef)
		{
			pExt->UrbanCombat_DefenderFocusAccum += dmgToDefenders;
			const int step = static_cast<int>(pExt->UrbanCombat_DefenderFocusAccum);
			pExt->UrbanCombat_DefenderFocusAccum -= step;

			if (step > 0)
			{
				const int healthBefore = pDef->Health;
				pDef->Health = Math::max(pDef->Health - step, 0);

				// Drain the force-HP pool by the health actually lost only -
				// overkill on the focus-fired defender is discarded, otherwise the
				// pool can reach zero while other defenders are still alive.
				pExt->UrbanCombat_DefenderHP -= Math::min(step, healthBefore);

				if (pDef->Health <= 0)
				{
					KillOccupant(pBuilding, pDef,
						pExt->UrbanCombat_Attackers.empty() ? nullptr : pExt->UrbanCombat_Attackers.front());
					pExt->UrbanCombat_DefenderFocusAccum = 0.0;
				}
			}
		}
	}

	// The garrison fights back focusing one UC attacker at a time, so the
	// attackers also fall one by one.
	const double dmgToAttackers = defenderPower * scale;

	if (dmgToAttackers > 0.0)
	{
		InfantryClass* pAtt = nullptr;

		for (auto const pA : pExt->UrbanCombat_Attackers)
		{
			if (pA && pA->IsAlive)
			{
				pAtt = pA;
				break;
			}
		}

		if (pAtt != pExt->UrbanCombat_AttackerFocus)
		{
			pExt->UrbanCombat_AttackerFocus = pAtt;
			pExt->UrbanCombat_AttackerFocusAccum = 0.0;
		}

		if (pAtt)
		{
			pExt->UrbanCombat_AttackerFocusAccum += dmgToAttackers;
			const int step = static_cast<int>(pExt->UrbanCombat_AttackerFocusAccum);
			pExt->UrbanCombat_AttackerFocusAccum -= step;

			if (step > 0)
			{
				const int healthBefore = pAtt->Health;
				pAtt->Health = Math::max(pAtt->Health - step, 0);

				// Same as the defender side: only the health actually lost may
				// drain the pool, never the discarded overkill.
				pExt->UrbanCombat_AttackerHP -= Math::min(step, healthBefore);

				if (pAtt->Health <= 0)
				{
					KillOccupant(pBuilding, pAtt, nullptr);
					RemoveAttackerFromList(pExt, pAtt);
					pExt->UrbanCombat_AttackerFocusAccum = 0.0;
				}
			}
		}
	}

	// Any defenders still alive?
	bool anyDefender = false;

	for (int i = 0; i < pBuilding->Occupants.Count; ++i)
	{
		auto const pOcc = pBuilding->Occupants.GetItem(i);

		if (pOcc && pOcc->IsAlive && IsDefender(pOcc, pBuilding->Owner))
		{
			anyDefender = true;
			break;
		}
	}

	// Any attackers still alive?
	bool anyAttacker = false;

	for (auto const pAtt : pExt->UrbanCombat_Attackers)
	{
		if (pAtt && pAtt->IsAlive)
		{
			anyAttacker = true;
			break;
		}
	}

	// The winner is decided purely by whether a side still has living units.
	// Do NOT also test the force-HP pools here: the indoor fight focus-fires one
	// unit at a time, so a single frame's damage can overkill that unit while
	// other units are still alive. The pools are drained by the full frame damage
	// (see below), so testing them caused an early "attacker wins" while the
	// garrison was not actually wiped out.
	if (!anyDefender)
	{
		Debug::Log("[Phobos::UrbanCombat] Decision: ATTACKER WINS bld=%s owner=%d occupants=%d trackedAttackers=%d (no living defender)\n",
			pBuilding->Type->get_ID(), pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1,
			pBuilding->Occupants.Count, static_cast<int>(pExt->UrbanCombat_Attackers.size()));

		ResolveUrbanCombat(pBuilding, true);
	}
	else if (!anyAttacker)
	{
		Debug::Log("[Phobos::UrbanCombat] Decision: DEFENDER WINS bld=%s owner=%d occupants=%d (no living attacker)\n",
			pBuilding->Type->get_ID(), pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1,
			pBuilding->Occupants.Count);

		ResolveUrbanCombat(pBuilding, false);
	}
}

// After an indoor battle ends, make any unit that is allied with the building's
// current owner stop attacking it. During the battle the building may have been
// targeted by its own side (e.g. ordered by OrderDefendersToAttackUC, or shot at
// before it was captured), so once the battle is over - and the building may have
// changed hands to the attacker - those units must drop the target instead of
// firing at a now-friendly building.
static void ClearFriendlyAttacksOnBuilding(BuildingClass* pBuilding)
{
	if (!pBuilding || !pBuilding->Owner)
		return;

	auto const ClearTarget = [&](TechnoClass* pTechno)
	{
		if (pTechno && pTechno->IsAlive && pTechno->Target == pBuilding
			&& pTechno->Owner && pTechno->Owner->IsAlliedWith(pBuilding->Owner))
			pTechno->SetTarget(nullptr);
	};

	for (auto const pUnit : UnitClass::Array)
		ClearTarget(pUnit);

	for (auto const pInf : InfantryClass::Array)
		ClearTarget(pInf);

	for (auto const pAircraft : AircraftClass::Array)
		ClearTarget(pAircraft);
}

// Set while ResolveUrbanCombat itself invokes BuildingClass::Captured, so the
// Captured hook can tell our own intentional capture apart from a vanilla
// capture that stole a contested building before the indoor battle started.
static bool InUrbanCombatResolve = false;

void BuildingExt::ResolveUrbanCombat(BuildingClass* pBuilding, bool attackerWins)
{
	if (!pBuilding)
		return;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	// Defensive: if the extension was freed/recycled (building destroyed), its
	// UrbanCombat_Attackers vector may be corrupted. Bail out to avoid a UAF in
	// the std::vector cleanup (crash in vector::_Tidy).
	if (!pExt || pExt->OwnerObject() != pBuilding)
		return;

	if (pExt->UrbanCombat_State == ExtData::UrbanCombatState::None)
		return;

	// Bring any orphan garrison unit back into Occupants before the cleanup
	// below - otherwise it is skipped, survives the resolution and stays
	// attached to the building (firing for whoever ends up owning it).
	const int adopted = AdoptOrphanOccupants(pBuilding, "resolve");

	Debug::Log("[Phobos::UrbanCombat] Resolve: bld=%s attackerWins=%d owner=%d occupantsBefore=%d trackedAttackers=%d adopted=%d\n",
		pBuilding->Type->get_ID(), attackerWins, pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1,
		pBuilding->Occupants.Count, static_cast<int>(pExt->UrbanCombat_Attackers.size()), adopted);

	auto const attackers = std::move(pExt->UrbanCombat_Attackers);
	pExt->UrbanCombat_Attackers.clear();
	pExt->UrbanCombat_State = ExtData::UrbanCombatState::None;
	UnregisterUrbanCombat(pBuilding);

	auto const pHouse = pExt->UrbanCombat_AttackerHouse;
	pExt->UrbanCombat_AttackerHouse = nullptr;
	pExt->UrbanCombat_DefenderFocus = nullptr;
	pExt->UrbanCombat_DefenderFocusAccum = 0.0;
	pExt->UrbanCombat_AttackerFocus = nullptr;
	pExt->UrbanCombat_AttackerFocusAccum = 0.0;

	// An occupant on the initiating attacker's side (same house / allied) is a
	// friendly unit of the assault, never an enemy defender - it must survive the
	// resolution. Without this a UC=no infantry that entered alongside the UC=yes
	// attacker was treated as a stray enemy and killed by its own side.
	auto const IsAttackerSide = [&](TechnoClass* pTech) -> bool
	{
		return pHouse && pTech && pTech->Owner && pTech->Owner->IsAlliedWith(pHouse);
	};

	// Attackers/defenders are split by alliance with the initiating attacker's
	// house - an enemy UC-tagged garrison is a defender, never an attacker.

	if (attackerWins)
	{
		// Assault wins. Rebuild the garrison from scratch:
		//   1. collect the units that may survive (surviving UC attackers plus any
		//      attacker-side units that were already inside);
		//   2. drop and finish off every other occupant - INCLUDING dead/stale
		//      entries, which the old loop skipped and left dangling in Occupants;
		//   3. detach the survivors, change owner, then re-attach them.
		// Every object is validated against InfantryClass::Array (address-only)
		// before it is dereferenced, so a freed pointer can never be reused here.
		TechnoClass* pFirstAliveAttacker = nullptr;
		for (auto const pAtt : attackers)
		{
			if (pAtt && pAtt->IsAlive)
			{
				pFirstAliveAttacker = pAtt;
				break;
			}
		}

		auto const IsLiveInfantry = [](InfantryClass* pInf) -> bool
		{
			return pInf
				&& InfantryClass::Array.FindItemIndex(pInf) != -1
				&& pInf->IsAlive;
		};

		std::vector<InfantryClass*> keep;
		keep.reserve(attackers.size() + pBuilding->Occupants.Count);

		auto const TryKeep = [&](InfantryClass* pInf)
		{
			if (IsLiveInfantry(pInf)
				&& std::find(keep.begin(), keep.end(), pInf) == keep.end())
			{
				keep.push_back(pInf);
			}
		};

		for (auto const pAtt : attackers)
			TryKeep(pAtt);

		for (int i = 0; i < pBuilding->Occupants.Count; ++i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			if (IsLiveInfantry(pOcc) && IsAttackerSide(pOcc))
				TryKeep(pOcc);
		}

		// Clear the whole list, killing every live occupant that is not kept.
		// Dead or freed entries are simply dropped (never dereferenced).
		for (int i = pBuilding->Occupants.Count - 1; i >= 0; --i)
		{
			auto const pOcc = pBuilding->Occupants.GetItem(i);

			pBuilding->Occupants.RemoveItem(i);

			if (!IsLiveInfantry(pOcc))
				continue;

			if (std::find(keep.begin(), keep.end(), pOcc) != keep.end())
				continue;

			pOcc->Transporter = nullptr;
			KillInfantry(pOcc, pFirstAliveAttacker);
		}

		// Detach the survivors before the ownership change so the vanilla
		// Captured path never observes them as a foreign garrison, then
		// re-attach them once the building belongs to the attacker house.
		for (auto const pAtt : keep)
		{
			if (pAtt->Transporter == pBuilding)
				pAtt->Transporter = nullptr;

			const int idx = pBuilding->Occupants.FindItemIndex(pAtt);

			if (idx != -1)
				pBuilding->Occupants.RemoveItem(idx);
		}

		if (!attackers.empty() && pHouse && pBuilding->Owner != pHouse)
		{
			InUrbanCombatResolve = true;

			// BuildingClass::Captured is a __thiscall that cleans up 2 stack args
			// (ret 8); the 2nd argument is not meaningful to the function.
			reinterpret_cast<void(__thiscall*)(BuildingClass*, HouseClass*, void*)>(0x448260)(pBuilding, pHouse, nullptr);

			InUrbanCombatResolve = false;
		}

		for (auto const pAtt : keep)
		{
			if (pBuilding->Occupants.FindItemIndex(pAtt) == -1
				&& !pBuilding->Occupants.AddItem(pAtt))
			{
				// AddItem fails when the occupant list is at capacity. Do not
				// leave the unit attached (Transporter == building) while it is
				// missing from Occupants - that orphan invariant break is what
				// the game later trips over ("garrisoned but didn't find it").
				Debug::Log("[Phobos::UrbanCombat] Cannot house surviving attacker %s in %s (list full, %d)\n",
					pAtt->Type->get_ID(), pBuilding->Type->get_ID(), pBuilding->Occupants.Count);

				pAtt->Transporter = nullptr;
				KillInfantry(pAtt, nullptr);
				continue;
			}

			pAtt->Transporter = pBuilding;
		}

		ClampFiringOccupantIndex(pBuilding);
		pBuilding->FiringOccupantIndex = 0;
	}
	else
	{
		// Defender wins - kill every occupant that is NOT allied with the building
		// owner (the UC attackers and any stray enemy garrison such as a GGI), so
		// the building keeps only its own garrison. Those that were regular
		// occupants are removed first so the building does not keep dead occupants
		// (clearing Transporter avoids the game's "garrisoned but didn't find it"
		// eject check).
		for (int i = pBuilding->Occupants.Count - 1; i >= 0; --i)
		{
			InfantryClass* const pOcc = pBuilding->Occupants.GetItem(i);

			// Address-only validity test first so a freed pointer is never
			// dereferenced; only live, registered defenders are kept.
			const bool inArray = pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1;
			const bool liveDefender = inArray && pOcc->IsAlive && IsDefender(pOcc, pBuilding->Owner);

			if (liveDefender)
				continue;

			pBuilding->Occupants.RemoveItem(i);

			if (inArray && pOcc->IsAlive)
			{
				pOcc->Transporter = nullptr;
				KillInfantry(pOcc, nullptr);
			}
		}

		for (auto const pAtt : attackers)
		{
			if (!pAtt || InfantryClass::Array.FindItemIndex(pAtt) == -1 || !pAtt->IsAlive)
				continue;

			const int idx = pBuilding->Occupants.FindItemIndex(pAtt);

			if (idx != -1)
				pBuilding->Occupants.RemoveItem(idx);

			if (pAtt->Transporter == pBuilding)
				pAtt->Transporter = nullptr;

			KillInfantry(pAtt, nullptr);
		}

		ClampFiringOccupantIndex(pBuilding);
	}

	// Orphan purge: units the game still treats as garrisoned (Transporter ==
	// building) but that were never in Occupants (e.g. the list was full, so
	// AdoptOrphanOccupants could not take them). They are invisible to the loops
	// above; on an attacker win they would stay attached to the captured building
	// and fire for its new owner. Kill every one that does not belong to the
	// winning side.
	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || !pInf->IsAlive || pInf->Transporter != pBuilding)
			continue;

		if (pBuilding->Occupants.FindItemIndex(pInf) != -1)
			continue; // already handled by the Occupants cleanup above

		if (attackerWins)
		{
			bool isSurvivingAttacker = false;
			for (auto const pAtt : attackers)
			{
				if (pAtt == pInf)
				{
					isSurvivingAttacker = true;
					break;
				}
			}

			if (isSurvivingAttacker || IsAttackerSide(pInf))
				continue;
		}
		else if (IsDefender(pInf, pBuilding->Owner))
		{
			continue;
		}

		Debug::Log("[Phobos::UrbanCombat] Purged orphan occupant: inf=%s owner=%d bld=%s attackerWins=%d\n",
			pInf->Type->get_ID(), pInf->Owner ? pInf->Owner->ArrayIndex : -1,
			pBuilding->Type->get_ID(), attackerWins);

		pInf->Transporter = nullptr;
		KillInfantry(pInf, nullptr);
	}

	SanitizeBuildingOccupants(pBuilding);
	ClampFiringOccupantIndex(pBuilding);

	Debug::Log("[Phobos::UrbanCombat] Resolved: bld=%s attackerWins=%d ownerAfter=%d occupantsAfter=%d\n",
		pBuilding->Type->get_ID(), attackerWins,
		pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1, pBuilding->Occupants.Count);

	pBuilding->Target = nullptr;
	pBuilding->Mark(MarkType::Change);

	ClearFriendlyAttacksOnBuilding(pBuilding);
}

void BuildingExt::AbortUrbanCombat(BuildingClass* pBuilding)
{
	if (!pBuilding)
		return;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	// Defensive: same guard as ResolveUrbanCombat — avoid a UAF in the vector
	// cleanup if the extension was freed/recycled.
	if (!pExt || pExt->OwnerObject() != pBuilding)
		return;

	if (pExt->UrbanCombat_State != ExtData::UrbanCombatState::Active)
		return;

	Debug::Log("[Phobos::UrbanCombat] Abort: bld=%s owner=%d occupants=%d trackedAttackers=%d\n",
		pBuilding->Type->get_ID(), pBuilding->Owner ? pBuilding->Owner->ArrayIndex : -1,
		pBuilding->Occupants.Count, static_cast<int>(pExt->UrbanCombat_Attackers.size()));

	auto const attackers = std::move(pExt->UrbanCombat_Attackers);
	pExt->UrbanCombat_Attackers.clear();
	pExt->UrbanCombat_State = ExtData::UrbanCombatState::None;
	UnregisterUrbanCombat(pBuilding);

	pExt->UrbanCombat_AttackerHouse = nullptr;
	pExt->UrbanCombat_DefenderFocus = nullptr;
	pExt->UrbanCombat_DefenderFocusAccum = 0.0;
	pExt->UrbanCombat_AttackerFocus = nullptr;
	pExt->UrbanCombat_AttackerFocusAccum = 0.0;

	for (auto const pAtt : attackers)
	{
		if (!pAtt || InfantryClass::Array.FindItemIndex(pAtt) == -1 || !pAtt->IsAlive)
			continue;

		const int idx = pBuilding->Occupants.FindItemIndex(pAtt);

		if (idx != -1)
			pBuilding->Occupants.RemoveItem(idx);

		if (pAtt->Transporter == pBuilding)
			pAtt->Transporter = nullptr;

		KillInfantry(pAtt, nullptr);
	}

	SanitizeBuildingOccupants(pBuilding);
	ClampFiringOccupantIndex(pBuilding);

	pBuilding->Mark(MarkType::Change);

	ClearFriendlyAttacksOnBuilding(pBuilding);
}

// ============================================================================
// BuildingExt::IsDefenderSide / OrderDefendersToAttackUC - make the defending
// side actively engage a contested (indoor-battle) building from outside.
// A unit is on the defender side if it is allied with the building owner
// (the building's own side, including its allies).
// ============================================================================

bool BuildingExt::IsDefenderSide(HouseClass* pHouse, BuildingClass* pBld)
{
	if (!pHouse || !pBld || !pBld->IsAlive)
		return false;

	// The ExtData may already be gone while the building is being destroyed.
	auto const pExt = BuildingExt::ExtMap.Find(pBld);

	if (!pExt)
		return false;

	if (pExt->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::Active)
		return false;

	// The building owner's side may always engage the contested building, no
	// matter who the initiating attacker house is (it can equal the owner when
	// the battle was started by the owner's own UC garrison).
	return pHouse->IsAlliedWith(pBld->Owner);
}

void BuildingExt::OrderDefendersToAttackUC(BuildingClass* pBuilding)
{
	if (!pBuilding || !pBuilding->IsAlive)
		return;

	auto const pExt = BuildingExt::ExtMap.Find(pBuilding);

	if (!pExt || pExt->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::Active)
		return;

	// Hoist the building owner out of the per-unit loop: the battle is already
	// known to be active, so the defender-side test is a plain alliance check
	// (no per-unit ExtMap lookup).
	auto const pOwner = pBuilding->Owner;

	if (!pOwner)
		return;

	const int maxDist = 12 * Unsorted::LeptonsPerCell;

	auto TryOrder = [&](TechnoClass* pUnit)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
			return;

		if (!pUnit->Owner || !pUnit->Owner->IsAlliedWith(pOwner))
			return;

		// UrbanCombat infantry on the defender side reinforce by entering the
		// contested building, not by shooting it from outside - leave them alone
		// so the player can order them inside (they become garrison defenders).
		if (BuildingExt::IsUrbanCombatInfantry(pUnit))
			return;

		if (pUnit->DistanceFrom(pBuilding) > maxDist)
			return;

		// Only idle units that are not already engaging the building.
		const auto mission = pUnit->GetCurrentMission();

		if (mission != Mission::Guard && mission != Mission::Area_Guard
			&& mission != Mission::Sleep && mission != Mission::None
			&& mission != Mission::Stop && mission != Mission::Hunt)
			return;

		if (pUnit->Target == pBuilding)
			return;

		if (!pUnit->GetWeapon(0))
			return;

		pUnit->SetTarget(pBuilding);
		pUnit->QueueMission(Mission::Attack, false);
	};

	for (auto const pUnit : UnitClass::Array)
		TryOrder(pUnit);

	for (auto const pInf : InfantryClass::Array)
		TryOrder(pInf);

	for (auto const pAircraft : AircraftClass::Array)
		TryOrder(pAircraft);
}

// ============================================================================
// BuildingClass::CanBeOccupiedBy - allow an UrbanCombat attacker to enter an
// enemy garrisoned building (used by the click-mission setup and other gates).
// ============================================================================

// Hooked at 0x457CF3 (`mov eax,[esi+0x520]`, 6 bytes) - a full instruction, so the
// 5-byte hook jump does not overflow into following code. At this point the game has
// set EDI=pInf and ESI=this; reading them from registers is reliable.
// Only UrbanCombat=yes infantry may enter an enemy garrisoned building - the vanilla
// game otherwise lets ANY infantry in (the "occupy" cursor + right-click entry), which
// would start the indoor battle and instantly kill the non-UC infantry. Non-UC infantry
// are rejected with the function's FALSE exit (0x457DA3: `pop edi; xor al,al; ...`).
DEFINE_HOOK(0x457CF3, BuildingClass_CanBeOccupiedBy_UrbanCombat, 0x6)
{
	enum { ReturnTrue = 0x457DD5, ReturnFalse = 0x457DA3 };

	GET(BuildingClass* const, pBuilding, ESI);
	GET(InfantryClass* const, pInf, EDI);

	// A defender-side UC infantry may reinforce its own contested building.
	if (pBuilding && pInf && BuildingExt::IsDefenderReinforce(pInf, pBuilding))
		return ReturnTrue;

	const bool isGarrEnemy = pBuilding && pInf && BuildingExt::IsGarrisonedEnemyBuilding(pInf, pBuilding);

	if (isGarrEnemy)
	{
		if (BuildingExt::IsUrbanCombatInfantry(pInf))
			return ReturnTrue;

		return ReturnFalse;
	}

	return 0;
}

// ============================================================================
// BuildingClass::ReceiveCommand - RadioCommand::QueryCanEnter (15) handler.
// Let the UrbanCombat attacker pass the entry query so it can run inside an
// enemy garrisoned building.
// ============================================================================

DEFINE_HOOK(0x43C7E9, BuildingClass_ReceiveCommand_QueryCanEnter_UrbanCombat, 0x8)
{
	enum { ReturnRoger = 0x43CCF2 };

	GET(BuildingClass* const, pThis, ESI);
	GET_STACK(TechnoClass* const, pFrom, 0x54);

	// Defender-side UC infantry may reinforce its own contested building.
	if (pThis && pFrom && BuildingExt::IsDefenderReinforce(pFrom, pThis))
		return ReturnRoger;

	const bool isUC = BuildingExt::IsUrbanCombatInfantry(pFrom);
	const bool isGarr = isUC && BuildingExt::IsGarrisonedEnemyBuilding(pFrom, pThis);

	if (isGarr)
		return ReturnRoger;

	return 0;
}

// ============================================================================
// BuildingClass::ReceiveCommand - RadioCommand::RequestCompleteEnter (21)
// handler. Answer "Roger" and skip the vanilla EnterBuilding call for an
// UrbanCombat attacker - the entry is performed by our own interception hook.
// Hooked at 0x43C788 (`mov cl,[eax+0x16b3]`, 6 bytes, full instruction).
// ============================================================================

DEFINE_HOOK(0x43C788, BuildingClass_ReceiveCommand_RequestCompleteEnter_UrbanCombat, 0x6)
{
	enum { ReturnRoger = 0x43CCF2 };

	GET(BuildingClass* const, pThis, ESI);
	GET_STACK(TechnoClass* const, pFrom, 0x54);

	// Defender-side UC infantry may reinforce its own contested building.
	if (pThis && pFrom && BuildingExt::IsDefenderReinforce(pFrom, pThis))
		return ReturnRoger;

	const bool isUC = BuildingExt::IsUrbanCombatInfantry(pFrom);
	const bool isGarr = isUC && BuildingExt::IsGarrisonedEnemyBuilding(pFrom, pThis);

	if (isGarr)
		return ReturnRoger;

	return 0;
}

// ============================================================================
// InfantryClass::UpdatePosition - right before the vanilla Occupants.AddItem.
// For an UrbanCombat attacker entering a garrisoned enemy building, absorb the
// infantry into the indoor battle instead of making it a regular occupant.
// ============================================================================

DEFINE_HOOK(0x51A34F, InfantryClass_UpdatePosition_UrbanCombatEntry, 0x6)
{
	enum { AfterAddItem = 0x51A38F };

	GET(InfantryClass* const, pThis, ESI);
	GET(BuildingClass* const, pBuilding, EDI);

	const bool isUC = BuildingExt::IsUrbanCombatInfantry(pThis);
	const bool isGarr = isUC && BuildingExt::IsGarrisonedEnemyBuilding(pThis, pBuilding);

	// Observation: report every UC infantry UpdatePosition near a building, so we
	// can confirm whether this entry path is reached.
	if (isUC)
	{
		Debug::Log("[Phobos::UrbanCombat] UpdatePosition hit: inf=%s owner=%d bld=%s ownerAllied=%d occ=%d/%d isGarr=%d\n",
			pThis->Type->get_ID(), pThis->Owner ? pThis->Owner->ArrayIndex : -1,
			pBuilding ? pBuilding->Type->get_ID() : "<none>",
			pBuilding ? pBuilding->Owner->IsAlliedWith(pThis->Owner) : -1,
			pBuilding ? pBuilding->Occupants.Count : -1,
			pBuilding ? pBuilding->Type->MaxNumberOccupants : -1, isGarr ? 1 : 0);
	}

	if (isGarr)
	{
		AbsorbUrbanCombatAttacker(pBuilding, pThis);

		// Resume right after the vanilla Occupants.AddItem (which we skip) so the
		// normal post-entry flag cleanup still runs.
		return AfterAddItem;
	}

	// A defender-side UC infantry reinforcing its own contested building takes
	// the normal entry path (vanilla AddItem) - it becomes a garrison defender.
	return 0;
}

// ============================================================================
// FootClass::EnterBuilding (0x4D4280) - the unified infantry building-entry
// function (called by both the garrison radio path and Mission_Capture).
// Hooked at 0x4D4290 (`call [eax+0x31c]`, 6 bytes; frame already set up,
// ESI=this). An UrbanCombat attacker entering a garrisoned enemy building starts
// the indoor battle instead of the normal entry / owner capture.
// ============================================================================

DEFINE_HOOK(0x4D4290, FootClass_EnterBuilding_UrbanCombat, 0x6)
{
	enum { Exit = 0x4D4597 };

	GET(FootClass* const, pThis, ESI);

	auto const pBld = specific_cast<BuildingClass*>(pThis->Destination);
	const bool isUC = BuildingExt::IsUrbanCombatInfantry(pThis);
	const bool isGarr = isUC && pBld && BuildingExt::IsGarrisonedEnemyBuilding(pThis, pBld);

	// Observation: report every EnterBuilding into a contested garrison, whether
	// or not it is a UC attacker, so we can confirm if this entry path is reached.
	if (pBld && pBld->Type->CanBeOccupied && pBld->Occupants.Count > 0)
	{
		Debug::Log("[Phobos::UrbanCombat] EnterBuilding hit: inf=%s owner=%d isUC=%d bld=%s ownerAllied=%d occ=%d/%d isGarr=%d\n",
			pThis->GetTechnoType()->get_ID(), pThis->Owner ? pThis->Owner->ArrayIndex : -1, isUC ? 1 : 0,
			pBld->Type->get_ID(), pBld->Owner->IsAlliedWith(pThis->Owner) ? 1 : 0,
			pBld->Occupants.Count, pBld->Type->MaxNumberOccupants, isGarr ? 1 : 0);
	}

	if (isGarr)
	{
		auto const pInf = abstract_cast<InfantryClass*>(pThis);
		AbsorbUrbanCombatAttacker(pBld, pInf);

		// Jump to EnterBuilding's clean exit, skipping the normal entry.
		return Exit;
	}

	// A defender-side UC infantry reinforcing its own contested building takes
	// the normal entry path (vanilla garrison) - no indoor-battle absorption.
	return 0;
}

// ============================================================================
// BuildingClass::KillOccupants - if the building dies while an indoor battle is
// running, the limboed attackers die along with the building.
// ============================================================================

DEFINE_HOOK(0x4585C0, BuildingClass_KillOccupants_UrbanCombat, 0x5)
{
	GET(BuildingClass* const, pThis, ECX);

	if (pThis)
	{
		auto const pExt = BuildingExt::ExtMap.Find(pThis);
		if (pExt && pExt->UrbanCombat_State == BuildingExt::ExtData::UrbanCombatState::Active)
			BuildingExt::AbortUrbanCombat(pThis);
	}

	return 0;
}

// ============================================================================
// BuildingClass::Captured - if the building is captured by someone else while
// an indoor battle is running, abort the battle.
//
// Also a robust guard against the game requesting an owner-change with a NULL
// target house (observed right after saving): BuildingClass::Captured is a
// __thiscall with 2 stack args (ret 8), so when arg1 (the new owner) is NULL we
// jump to a tiny no-op thunk that returns AL=0 and cleans the 2 args - the whole
// capture body (including the ChangeOwner call) is skipped. This bypasses the
// broken game path regardless of which caller reaches Captured.
//
// Hooked at the function entry with size 0x9: the saved bytes
// `sub esp,0x50; push ebx; mov ebx,[esp+0x58]; push esi` (0x448260-0x448268) are a
// full contiguous run, so the 5-byte hook jump does not overflow into later code.
// ============================================================================

static void __declspec(naked) CapturedNoOp_Thunk()
{
	_asm { xor al, al }
	_asm { ret 8 }
}

DEFINE_HOOK(0x448260, BuildingClass_Captured_UrbanCombat, 0x9)
{
	GET(BuildingClass* const, pThis, ECX);
	GET_STACK(HouseClass* const, pNewOwner, 0x4);

	if (!pNewOwner)
		return reinterpret_cast<uintptr_t>(&CapturedNoOp_Thunk);

	if (pThis)
	{
		auto const pExt = BuildingExt::ExtMap.Find(pThis);

		if (pExt && pExt->UrbanCombat_State == BuildingExt::ExtData::UrbanCombatState::Active)
		{
			BuildingExt::AbortUrbanCombat(pThis);
		}
		else if (!InUrbanCombatResolve && pThis->Occupants.Count > 0)
		{
			// UrbanCombat "grab": a UC=yes infantry of the new owner is taking a
			// garrisoned enemy building. The indoor battle must run first - never
			// let vanilla transfer the garrison (owner flip), otherwise the
			// defenders would silently become the attacker's units and then die
			// for the wrong side. Detect the grabber and absorb it instead.
			auto const pCapturer = FindUrbanCombatCapturer(pThis, pNewOwner);

			// Only a real enemy-owned garrison may be "grabbed". A neutral or
			// civilian building (MultiplayPassive owner) is not an enemy - entering
			// it is a normal capture/garrison. Treating it as an enemy grab wrongly
			// turned friendly entries into indoor battles, killed friendly UC=no
			// infantry and corrupted the garrison bookkeeping (crashes).
			const bool enemyOwned = pThis->Owner && pThis->Owner->Type
				&& !pThis->Owner->Type->MultiplayPassive;

			// A real defender is an alive occupant on the CURRENT owner's side that
			// is not the grabber itself. Without one there is nothing to fight, so
			// the capture must stay a normal capture.
			bool hasDefenderOccupant = false;

			if (enemyOwned)
			{
				for (int i = 0; i < pThis->Occupants.Count; ++i)
				{
					auto const pOcc = pThis->Occupants.GetItem(i);

					if (pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1 && pOcc->IsAlive
						&& pOcc != pCapturer && pOcc->Owner && pOcc->Owner->IsAlliedWith(pThis->Owner))
					{
						hasDefenderOccupant = true;
						break;
					}
				}
			}

			if (pCapturer && enemyOwned && hasDefenderOccupant && BuildingExt::IsGarrisonedEnemyBuilding(pCapturer, pThis))
			{
				Debug::Log("[Phobos::UrbanCombat] Intercepted UC capture: bld=%s owner=%d newOwner=%d occupants=%d capturer=%s owner=%d dest=%p dist=%d\n",
					pThis->Type->get_ID(), pThis->Owner ? pThis->Owner->ArrayIndex : -1,
					pNewOwner ? pNewOwner->ArrayIndex : -1, pThis->Occupants.Count,
					pCapturer->Type->get_ID(), pCapturer->Owner ? pCapturer->Owner->ArrayIndex : -1,
					(void*)pCapturer->Destination, pCapturer->DistanceFrom(pThis));

				AbsorbUrbanCombatAttacker(pThis, pCapturer);

				// Skip the whole vanilla capture body: the building must stay with
				// the defender until the indoor battle is resolved.
				return reinterpret_cast<uintptr_t>(&CapturedNoOp_Thunk);
			}

			// A capture (engineer, etc.) of a garrisoned building while no indoor
			// battle is running and no UC grabber was found. Observation log only.
			bool hasUCAttacker = false;

			for (int i = 0; i < pThis->Occupants.Count; ++i)
			{
				auto const pOcc = pThis->Occupants.GetItem(i);

				if (pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1 && pOcc->IsAlive
					&& BuildingExt::IsUrbanCombatInfantry(pOcc)
					&& pNewOwner && !pOcc->Owner->IsAlliedWith(pNewOwner))
				{
					hasUCAttacker = true;
					break;
				}
			}

			Debug::Log("[Phobos::UrbanCombat] Captured with state=None: bld=%s owner=%d newOwner=%d occupants=%d ucAttacker=%d capturer=%s\n",
				pThis->Type->get_ID(), pThis->Owner ? pThis->Owner->ArrayIndex : -1,
				pNewOwner ? pNewOwner->ArrayIndex : -1, pThis->Occupants.Count, hasUCAttacker ? 1 : 0,
				pCapturer ? pCapturer->Type->get_ID() : "<none>");
		}
	}

	return 0;
}

// ============================================================================
// Source-level fix for a broken game call:
// The game invokes BuildingClass::Captured (vtable slot 0x360) from this single
// site WITHOUT passing a new-owner argument - the new owner is uninitialized
// stack garbage (NULL in this mod's runtime), which crashes the whole
// owner-change chain (Phobos house-tracking hook, the game's ChangeOwner body,
// and Ares' internal validation). Skip the broken call for buildings.
// ============================================================================

DEFINE_HOOK(0x6FC018, TechnoClass_Check_SkipBrokenBuildingCapture, 0x6)
{
	enum { Skip = 0x6FC01E };

	GET(TechnoClass* const, pThis, ESI);

	const bool isBuilding = pThis && pThis->WhatAmI() == AbstractType::Building;

	if (isBuilding)
		return Skip;

	return 0;
}

// ============================================================================
// BuildingClass::ReceiveDamage - while an indoor battle is running, outside
// damage is propagated to the opposing side inside the building.
//
// Hooked at 0x442230 (the function entry, a full `sub esp,0x8c` instruction).
// At this point ECX=this and the args_ReceiveDamage struct sits at [esp+0x4],
// so both are read before the stack frame is set up.
//
// The damage source house decides which side takes the hit:
//   - source allied with the attacker house  -> the defenders (enemy garrison)
//   - source allied with the building owner  -> the attackers
//   - a 3rd party (allied with neither)      -> both sides
//
// The building hit is shared evenly among the affected side and scaled down by
// [General]UrbanCombat.OutsideDamageMultiplier, then per side by
// [General]UrbanCombat.DefOutsideMulti (defenders) / AttOutsideMulti
// (attackers): each unit takes (damage * defMulti|attMulti / affected count)
// through its own ReceiveDamage (so warhead Verses/armor/shields apply
// normally), which prevents a single high-damage shell from one-shotting the
// whole garrison. The actual HP lost is subtracted from the matching simulated
// force-HP pool so the indoor battle resolves correctly. A recursion guard
// prevents re-entry when a killed unit's death effect damages the building
// again.
// ============================================================================

static bool PropagatingUrbanCombatDamage = false;

DEFINE_HOOK(0x442230, BuildingClass_ReceiveDamage_UrbanCombat, 0x6)
{
	GET(BuildingClass* const, pThis, ECX);
	LEA_STACK(args_ReceiveDamage* const, args, 0x4);

	if (PropagatingUrbanCombatDamage || !pThis || !args || !args->Damage || !args->WH)
		return 0;

	auto const pExt = BuildingExt::ExtMap.Find(pThis);

	if (pExt->UrbanCombat_State != BuildingExt::ExtData::UrbanCombatState::Active)
		return 0;

	auto const pAttackerHouse = pExt->UrbanCombat_AttackerHouse;
	auto const pDefenderHouse = pThis->Owner;

	if (!pAttackerHouse || !pDefenderHouse)
		return 0;

	auto const pSourceHouse = args->SourceHouse ? args->SourceHouse
		: (args->Attacker ? args->Attacker->Owner : nullptr);

	if (!pSourceHouse)
		return 0;

	const bool fromAttackerSide = pSourceHouse->IsAlliedWith(pAttackerHouse);
	const bool fromDefenderSide = pSourceHouse->IsAlliedWith(pDefenderHouse);
	const bool thirdParty = !fromAttackerSide && !fromDefenderSide;

	const int damage = Math::max(*args->Damage, 0);

	if (damage <= 0)
		return 0;

	auto const pRules = RulesExt::Global();

	if (!pRules)
		return 0;

	// Base outside-damage share, then per side:
	//   defenders -> OutsideDamageMultiplier * DefOutsideMulti
	//   attackers -> OutsideDamageMultiplier * AttOutsideMulti
	const double defMulti = Math::max(pRules->UrbanCombat_OutsideDamageMultiplier, 0.0)
		* Math::max(pRules->UrbanCombat_DefOutsideMulti, 0.0);
	const double attMulti = Math::max(pRules->UrbanCombat_OutsideDamageMultiplier, 0.0)
		* Math::max(pRules->UrbanCombat_AttOutsideMulti, 0.0);

	PropagatingUrbanCombatDamage = true;

	if (fromAttackerSide || thirdParty)
	{
		// Outside damage hits ONE random defender (scaled only by the
		// [General] UrbanCombat multipliers, not split across the count).
		const int perDefender = static_cast<int>(damage * defMulti);

		if (perDefender > 0)
		{
			// Collect the alive defenders, then pick one at random.
			DynamicVectorClass<InfantryClass*> candidates;
			candidates.Reserve(pThis->Occupants.Count);

			for (int i = 0; i < pThis->Occupants.Count; ++i)
			{
				auto const pOcc = pThis->Occupants.GetItem(i);

				if (pOcc && InfantryClass::Array.FindItemIndex(pOcc) != -1
					&& pOcc->IsAlive && IsDefender(pOcc, pThis->Owner))
				{
					candidates.AddItem(pOcc);
				}
			}

			if (candidates.Count > 0)
			{
				auto const pOcc = candidates[ScenarioClass::Instance->Random.RandomRanged(0, candidates.Count - 1)];
				const int hpBefore = pOcc->Health;

				// Cap the per-hit damage so a single shell can never wipe a
				// defender (at most 5% of its strength per outside hit).
				int occDamage = Math::min(perDefender,
					Math::max(pOcc->Type->Strength / 20, 1));

				pOcc->ReceiveDamage(&occDamage, args->DistanceToEpicenter, args->WH,
					args->Attacker, args->IgnoreDefenses, false, pSourceHouse);

				pExt->UrbanCombat_DefenderHP -= Math::max(hpBefore - pOcc->Health, 0);

				if (!pOcc->IsAlive)
				{
					// The game may already have dropped the dead occupant from
					// Occupants during ReceiveDamage, so resolve the index by
					// lookup instead of trusting a loop counter.
					const int idx = pThis->Occupants.FindItemIndex(pOcc);

					if (idx != -1)
					{
						pThis->Occupants.RemoveItem(idx);
						pOcc->Transporter = nullptr;
						ClampFiringOccupantIndex(pThis);
					}
				}
			}
		}
	}

	// Damage the attackers only from true defender-side fire. When the attacker
	// house equals the building owner, the owner's outside fire is attacker-side
	// (it must not friendly-fire the owner's own UC garrison inside).
	if ((fromDefenderSide && !fromAttackerSide) || thirdParty)
	{
		// Outside damage hits ONE random attacker (scaled only by the
		// [General] UrbanCombat multipliers, not split across the count).
		const int perAttacker = static_cast<int>(damage * attMulti);

		if (perAttacker > 0)
		{
			// Collect the alive attackers, then pick one at random.
			DynamicVectorClass<InfantryClass*> candidates;
			candidates.Reserve(pExt->UrbanCombat_Attackers.size());

			for (auto const pAtt : pExt->UrbanCombat_Attackers)
			{
				if (pAtt && InfantryClass::Array.FindItemIndex(pAtt) != -1 && pAtt->IsAlive)
					candidates.AddItem(pAtt);
			}

			if (candidates.Count > 0)
			{
				auto const pAtt = candidates[ScenarioClass::Instance->Random.RandomRanged(0, candidates.Count - 1)];
				const int hpBefore = pAtt->Health;

				// Cap the per-hit damage so a single shell can never wipe an
				// attacker (at most 5% of its strength per outside hit).
				int attDamage = Math::min(perAttacker,
					Math::max(pAtt->Type->Strength / 20, 1));

				pAtt->ReceiveDamage(&attDamage, args->DistanceToEpicenter, args->WH,
					args->Attacker, args->IgnoreDefenses, false, pSourceHouse);

				pExt->UrbanCombat_AttackerHP -= Math::max(hpBefore - pAtt->Health, 0);

				if (!pAtt->IsAlive)
				{
					const int idx = pThis->Occupants.FindItemIndex(pAtt);

					if (idx != -1)
					{
						pThis->Occupants.RemoveItem(idx);
						pAtt->Transporter = nullptr;
						ClampFiringOccupantIndex(pThis);
					}

					RemoveAttackerFromList(pExt, pAtt);
				}
			}
		}
	}

	PropagatingUrbanCombatDamage = false;

	return 0;
}
