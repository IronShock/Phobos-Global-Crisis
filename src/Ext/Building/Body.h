#pragma once
#include <BuildingClass.h>
#include <HouseClass.h>
#include <TiberiumClass.h>
#include <FactoryClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <Misc/FlyingStrings.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/BuildingType/Body.h>

class BuildingExt
{
public:
	using base_type = BuildingClass;

	static constexpr DWORD Canary = 0x87654321;
	static constexpr size_t ExtPointerOffset = 0x6FC;
	static constexpr bool ShouldConsiderInvalidatePointer = true;

	class ExtData final : public Extension<BuildingClass>
	{
	public:
		enum class UrbanCombatState : byte
		{
			None = 0,
			Active = 1
		};

		BuildingTypeExt::ExtData* TypeExtData;
		TechnoExt::ExtData* TechnoExtData;
		bool DeployedTechno;
		bool IsCreatedFromMapFile;
		int LimboID;
		int GrindingWeapon_LastFiredFrame;
		int GrindingWeapon_AccumulatedCredits;
		BuildingClass* CurrentAirFactory;
		int AccumulatedIncome;
		std::optional<int> CurrentLaserWeaponIndex;
		int PoweredUpToLevel; // Distinct from UpgradeLevel, and set to highest PowersUpToLevel out of applied upgrades regardless of how many are currently applied to this building.
		SuperClass* CurrentEMPulseSW;

		// UrbanCombat - indoor battle state machine
		UrbanCombatState UrbanCombat_State;
		std::vector<InfantryClass*> UrbanCombat_Attackers; // attacker infantry, kept in Limbo during the battle
		double UrbanCombat_AttackerHP;                    // total remaining attacker force HP (double to keep fractional damage)
		double UrbanCombat_DefenderHP;                    // total remaining defender (occupant) force HP
		HouseClass* UrbanCombat_AttackerHouse;            // the initiating attacker's house - attackers/defenders are split by alliance with it
		InfantryClass* UrbanCombat_DefenderFocus;         // current focus-fired defender + fractional damage accumulator
		double UrbanCombat_DefenderFocusAccum;
		InfantryClass* UrbanCombat_AttackerFocus;         // current focus-fired attacker + fractional damage accumulator
		double UrbanCombat_AttackerFocusAccum;

		CDTimerClass BattlePoints_ProduceTimer;
		HouseClass* BattlePoints_ProduceOwner;

		ExtData(BuildingClass* OwnerObject) : Extension<BuildingClass>(OwnerObject)
			, TypeExtData { nullptr }
			, TechnoExtData { nullptr }
			, DeployedTechno { false }
			, IsCreatedFromMapFile { false }
			, LimboID { -1 }
			, GrindingWeapon_LastFiredFrame { 0 }
			, GrindingWeapon_AccumulatedCredits { 0 }
			, CurrentAirFactory { nullptr }
			, AccumulatedIncome { 0 }
			, CurrentLaserWeaponIndex {}
			, PoweredUpToLevel { 0 }
			, CurrentEMPulseSW {}
			, BattlePoints_ProduceTimer {}
			, BattlePoints_ProduceOwner { nullptr }
			, UrbanCombat_State { UrbanCombatState::None }
			, UrbanCombat_Attackers {}
			, UrbanCombat_AttackerHP { 0.0 }
			, UrbanCombat_DefenderHP { 0.0 }
			, UrbanCombat_AttackerHouse { nullptr }
			, UrbanCombat_DefenderFocus { nullptr }
			, UrbanCombat_DefenderFocusAccum { 0.0 }
			, UrbanCombat_AttackerFocus { nullptr }
			, UrbanCombat_AttackerFocusAccum { 0.0 }
		{ }

		void DisplayIncomeString();
		void UpdateBattlePointsProduction();
		void ApplyPoweredKillSpawns();
		bool HasSuperWeapon(int index, bool withUpgrades) const;
		bool HandleInfiltrate(HouseClass* pInfiltratorHouse, int moneybefore);
		void UpdatePrimaryFactoryAI();
		virtual ~ExtData();

		// virtual void LoadFromINIFile(CCINIClass* pINI) override;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override
		{
			AnnounceInvalidPointer(CurrentAirFactory, ptr);
			AnnounceInvalidPointer(BattlePoints_ProduceOwner, ptr);

			bool wasUrbanCombatAttacker = false;

			for (auto& pAttacker : UrbanCombat_Attackers)
			{
				if (pAttacker == ptr)
					wasUrbanCombatAttacker = true;

				AnnounceInvalidPointer(pAttacker, ptr);
			}

			AnnounceInvalidPointer(UrbanCombat_AttackerHouse, ptr);
			AnnounceInvalidPointer(UrbanCombat_DefenderFocus, ptr);
			AnnounceInvalidPointer(UrbanCombat_AttackerFocus, ptr);

			// Diagnostic: a destroyed UrbanCombat attacker must not still be
			// referenced by the building's Occupants list. Log it so a dangling
			// pointer can be traced to its source.
			if (wasUrbanCombatAttacker)
				this->LogInvalidatedUrbanCombatPtr(ptr, bRemoved);
		}

		void LogInvalidatedUrbanCombatPtr(void* ptr, bool bRemoved);

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<BuildingExt>
	{
	public:
		ExtContainer();
		~ExtContainer();

		virtual bool InvalidateExtDataIgnorable(void* const ptr) const override
		{
			auto const abs = static_cast<AbstractClass*>(ptr)->WhatAmI();

			switch (abs)
			{
			case AbstractType::Building:
			case AbstractType::Infantry:
				// Buildings own the ExtData's building pointers; infantry are
				// tracked in UrbanCombat_Attackers and must be nulled out when
				// destroyed (otherwise the vector keeps a dangling pointer).
				return false;
			default:
				return true;
			}
		}
	};

	static ExtContainer ExtMap;

	static bool ConsideringSpecificOccupant;

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static void StoreTiberium(BuildingClass* pThis, float amount, int idxTiberiumType, int idxStorageTiberiumType);

	static int CountOccupiedDocks(BuildingClass* pBuilding);
	static bool HasFreeDocks(BuildingClass* pBuilding);
	static bool IsUrbanCombatInfantry(TechnoClass* pTechno);
	static bool IsGarrisonedEnemyBuilding(TechnoClass* pOwner, BuildingClass* pBuilding);
	static bool IsDefenderReinforce(TechnoClass* pOwner, BuildingClass* pBuilding);
	static void StartUrbanCombat(BuildingClass* pBuilding, InfantryClass* pAttacker);
	static void UpdateUrbanCombat(BuildingClass* pBuilding);
	static bool HasActiveUrbanCombats();
	static void UpdateAllUrbanCombats();
	static void ScanForNewUrbanCombats();
	static void UnregisterActiveUrbanCombat(BuildingClass* pBuilding);
	static void ClearActiveUrbanCombats();
	static void ResolveUrbanCombat(BuildingClass* pBuilding, bool attackerWins);
	static void AbortUrbanCombat(BuildingClass* pBuilding);
	static bool IsDefenderSide(HouseClass* pHouse, BuildingClass* pBld);
	static void OrderDefendersToAttackUC(BuildingClass* pBuilding);
	static bool CanGrindTechno(BuildingClass* pBuilding, TechnoClass* pTechno);
	static bool DoGrindingExtras(BuildingClass* pBuilding, TechnoClass* pTechno, int refund);
	static bool CanUndeployOnSell(BuildingClass* pThis);
	static void KickOutStuckUnits(BuildingClass* pThis);
	static const std::vector<CellStruct> GetFoundationCells(BuildingClass* pThis, CellStruct baseCoords, bool includeOccupyHeight = false);
	static bool CanOccupantsFire(BuildingClass* pThis, AbstractClass* pTarget);
	static int GetOccupantsRange(BuildingClass* pThis);

	// Returns Occupants[FiringOccupantIndex] only when the index is in range and
	// the entry is a valid pointer; otherwise nullptr. The occupy-fire code reads
	// Occupants[FiringOccupantIndex] without any bounds/validity check, so after
	// the UrbanCombat code mutates Occupants a stale index yields a garbage Techno
	// pointer and the following virtual call jumps into the weeds.
	static TechnoClass* GetFiringOccupant(BuildingClass* pBuilding);
};
