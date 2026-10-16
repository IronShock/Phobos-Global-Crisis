#include "MOEE.h"

#include <Phobos.h>

// Port of MO_ExperienceEnhancements/src/Observer.cpp.
//
// Multi-observer fix + observer panel ordering.
// Observer (0xAC1198) is a global singleton holding only the last observer
// house; aligning the local CurrentPlayer to it makes the vanilla observer
// branch trigger. The panel list is refilled each frame with human houses
// first, AI after, and no observer slots at all.
//
// MP-safe: only the local CurrentPlayer and the pure-UI panel list are written;
// the Observer singleton is read-only.
namespace MOEE::Observer
{
	namespace
	{
		constexpr unsigned int ADDR_CURRENT = 0xA83D4Cu;      // HouseClass::CurrentPlayer (local, writable)
		constexpr unsigned int ADDR_OBSERVER = 0xAC1198u;     // HouseClass::Observer (read-only)
		constexpr unsigned int ADDR_SCENARIO = 0xA8B230u;     // ScenarioClass::Instance
		constexpr unsigned int ADDR_HOUSES_ITEMS = 0xA8022Cu; // HouseClass::Array items
		constexpr unsigned int ADDR_HOUSES_COUNT = 0xA80238u; // HouseClass::Array count
		constexpr unsigned int ADDR_NODES_ITEMS = 0xA8DA78u;  // MP node table items
		constexpr unsigned int ADDR_NODES_COUNT = 0xA8DA84u;  // MP node table count
		constexpr unsigned int ADDR_PANEL_TABLE = 0x884B94u;  // panel house list
		constexpr unsigned int ADDR_PANEL_COUNT = 0x884CF8u;  // panel house count
		constexpr unsigned int PANEL_MAX_SLOTS = 64u;

		constexpr unsigned int OFF_HOUSEINDICES = 0x1180u;    // ScenarioClass::HouseIndices[16]
		constexpr unsigned int OFF_HOUSE_INDEX = 0x30u;       // HouseClass::ArrayIndex
		constexpr unsigned int OFF_HOUSE_TYPE = 0x34u;        // HouseClass::Type
		constexpr unsigned int OFF_IS_HUMAN = 0x1ECu;         // HouseClass::IsHumanPlayer
		constexpr unsigned int OFF_HTYPE_PASSIVE = 0x1A6u;    // HouseTypeClass::MultiplayPassive
		constexpr unsigned int OFF_NODE_SPECT = 0x6Bu;        // NodeNameType::SpectatorFlag (-1 = spectator)
		constexpr unsigned int OFF_NODE_HOUSEIDX = 0x6Fu;     // NodeNameType::HouseIndex

		constexpr int NODE_MAX = 16;

		bool NodesReady()
		{
			return IsPlausiblePtr(*reinterpret_cast<void**>(ADDR_NODES_ITEMS))
				&& *reinterpret_cast<int*>(ADDR_NODES_COUNT) > 0
				&& *reinterpret_cast<int*>(ADDR_NODES_COUNT) <= NODE_MAX;
		}

		// Does a spectator node point at this house?
		bool NodeClaimsHouse(void* house)
		{
			if (!NodesReady())
				return false;

			auto items = *reinterpret_cast<void***>(ADDR_NODES_ITEMS);
			const int count = *reinterpret_cast<int*>(ADDR_NODES_COUNT);
			const int arrayIndex =
				*reinterpret_cast<int*>(static_cast<char*>(house) + OFF_HOUSE_INDEX);

			for (int i = 0; i < count; ++i)
			{
				void* node = items[i];

				if (!IsPlausiblePtr(node))
					continue;

				if (*reinterpret_cast<int*>(static_cast<char*>(node) + OFF_NODE_SPECT) != -1)
					continue;

				if (*reinterpret_cast<int*>(static_cast<char*>(node) + OFF_NODE_HOUSEIDX) == arrayIndex)
					return true;
			}

			return false;
		}

		// House has no spawn point (ArrayIndex absent from ScenarioClass::HouseIndices).
		bool HouseHasNoSpawn(void* house)
		{
			auto idx = reinterpret_cast<const int*>(
				*reinterpret_cast<char**>(ADDR_SCENARIO) + OFF_HOUSEINDICES);

			if (!IsPlausiblePtr(idx))
				return false;

			const int arrayIndex =
				*reinterpret_cast<int*>(static_cast<char*>(house) + OFF_HOUSE_INDEX);

			if (arrayIndex < 0 || arrayIndex > 1024)
				return false;

			for (int i = 0; i < 16; ++i)
			{
				if (idx[i] == arrayIndex)
					return false;
			}

			return true;
		}

		// Any observer slot (not just the global true-observer house).
		bool IsAnyObserverSlot(void* house)
		{
			return NodeClaimsHouse(house) || HouseHasNoSpawn(house);
		}

		bool LocalHouseIsObserver(void* house)
		{
			if (!*reinterpret_cast<unsigned char*>(static_cast<char*>(house) + OFF_IS_HUMAN))
				return false;

			return IsAnyObserverSlot(house);
		}

		bool IsNeutral(void* house)
		{
			auto type = *reinterpret_cast<unsigned char**>(
				static_cast<char*>(house) + OFF_HOUSE_TYPE);

			if (!IsPlausiblePtr(type))
				return true;

			return *reinterpret_cast<unsigned char*>(
				reinterpret_cast<char*>(type) + OFF_HTYPE_PASSIVE) == 1;
		}

		// Exclude neutral / any observer slot / stale pointers; humans first, then
		// AI, preserving array order within each group. Rebuilt every frame, so
		// it is idempotent with the engine's own refresh.
		void FillPanelList()
		{
			auto items = *reinterpret_cast<void***>(ADDR_HOUSES_ITEMS);
			const int count = *reinterpret_cast<int*>(ADDR_HOUSES_COUNT);
			auto table = reinterpret_cast<void**>(ADDR_PANEL_TABLE);

			if (!IsPlausiblePtr(items) || count <= 0 || count > 64)
				return;

			int n = 0;

			for (int pass = 0; pass < 2; ++pass)
			{
				const unsigned char wantHuman = static_cast<unsigned char>(pass == 0 ? 1 : 0);

				for (int i = 0; i < count && n < static_cast<int>(PANEL_MAX_SLOTS); ++i)
				{
					void* house = items[i];

					if (!IsPlausiblePtr(house))
						continue;

					if (*reinterpret_cast<unsigned char*>(static_cast<char*>(house) + OFF_IS_HUMAN) != wantHuman)
						continue;

					if (IsNeutral(house))
						continue;

					if (IsAnyObserverSlot(house))
						continue;

					table[n++] = house;
				}
			}

			*reinterpret_cast<int*>(ADDR_PANEL_COUNT) = n;
		}
	}

	void Poll()
	{
		if (!Phobos::Config::MOEE_Observer)
			return;

		void* house = *reinterpret_cast<void**>(ADDR_CURRENT);

		if (!IsPlausiblePtr(house))
			return;

		void* observer = *reinterpret_cast<void**>(ADDR_OBSERVER);

		if (IsPlausiblePtr(observer) && house == observer)
		{
			FillPanelList();
			return;
		}

		// Local player is an observer but did not get the real observer house:
		// align to the global one.
		if (!IsPlausiblePtr(observer))
			return;

		if (!LocalHouseIsObserver(house))
			return;

		*reinterpret_cast<void**>(ADDR_CURRENT) = observer;
	}
}
