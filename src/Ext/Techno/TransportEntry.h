#pragma once

#include <cstring>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

class FootClass;
class TechnoClass;
class UnitClass;

// Passenger boarding direction masks. The bit index is the direction relative
// to the transport's current body facing, matching FacingType / AdjacentCell:
// 0 front, 1 frontright, 2 right, 3 backright, 4 back, 5 backleft, 6 left,
// 7 frontleft.
namespace PassengerDirections
{
	constexpr int Front = 1 << 0;
	constexpr int FrontRight = 1 << 1;
	constexpr int Right = 1 << 2;
	constexpr int BackRight = 1 << 3;
	constexpr int Back = 1 << 4;
	constexpr int BackLeft = 1 << 5;
	constexpr int Left = 1 << 6;
	constexpr int FrontLeft = 1 << 7;
	constexpr int All = Front | FrontRight | Right | BackRight | Back | BackLeft | Left | FrontLeft;
	constexpr int None = 0;

	// Parses a comma separated direction list. "all" and "none" win
	// immediately; unknown tokens are ignored. An empty/absent value means
	// vanilla behaviour.
	inline int Parse(const char* text)
	{
		if (!text || !*text)
			return All;

		int flags = 0;
		bool sawDirection = false;

		for (auto&& part : std::string_view { text } | std::views::split(','))
		{
			std::string_view token { part.begin(), part.end() };
			const auto first = token.find_first_not_of(" \t\r");

			if (first == std::string_view::npos)
				continue;

			const auto last = token.find_last_not_of(" \t\r");
			token = token.substr(first, last - first + 1);

			const std::string name { token };

			if (_stricmp(name.c_str(), "all") == 0)
				return All;

			if (_stricmp(name.c_str(), "none") == 0)
				return None;

			static constexpr std::pair<const char*, int> Names[] =
			{
				{ "front", Front },
				{ "frontright", FrontRight },
				{ "right", Right },
				{ "backright", BackRight },
				{ "back", Back },
				{ "backleft", BackLeft },
				{ "left", Left },
				{ "frontleft", FrontLeft },
			};

			for (auto const& [pName, bit] : Names)
			{
				if (_stricmp(name.c_str(), pName) == 0)
				{
					flags |= bit;
					sawDirection = true;
					break;
				}
			}
		}

		return sawDirection ? flags : All;
	}

	// True when the passenger is in the transport's cargo list. Uses the
	// engine's exact list rather than Transporter/InLimbo flags.
	bool IsPassengerInside(UnitClass* pTransport, FootClass* pPassenger);

	// Cell based direction check. Used as a stateless safety net by the
	// engine's own entry paths; it never paces or mutates entry state.
	bool IsEntryAllowedByDirection(UnitClass* pTransport, FootClass* pPassenger);

	// Full approach check for a tracked boarding passenger. When the passenger
	// already stands on the transport cell it uses LastMapCoords/ApproachCell
	// to determine which side it actually came from.
	bool IsEntryApproachAllowed(UnitClass* pTransport, FootClass* pPassenger);

	// Per-transport unload pacing, only used by the fast unload loop.
	bool TryBeginUnload(UnitClass* pTransport);
	void FinishUnload(UnitClass* pTransport);

	// True when at least one allowed adjacent cell can receive the passenger.
	bool HasValidUnloadCell(UnitClass* pTransport, FootClass* pPassenger);

	// Returns the world direction (0..7) to unload to. Keeps the requested
	// direction when it is allowed and valid, otherwise picks the first
	// allowed valid direction in fixed relative order.
	int ChooseUnloadDirection(UnitClass* pTransport, FootClass* pPassenger, int direction);

	// Drops transient boarding state. Called on scenario load and whenever an
	// order no longer wants to board.
	void ClearAll();
	void ClearPassenger(FootClass* pPassenger);
	void EnsureScenario();

	// Resolves the transport tracked for this passenger by UniqueID. Used when
	// the engine link has already been cleared while our own Move is active.
	TechnoClass* GetTrackedTransport(FootClass* pPassenger);

	// True while the scenario/savegame is being restored. All custom entry
	// gates must step aside so pre-placed cargo is not dropped.
	bool IsLoading();

	// True when the controller currently tracks this passenger for this
	// transport. Used to apply the direction check without breaking load.
	bool IsTracked(UnitClass* pTransport, FootClass* pPassenger);

	// True after the final step has been handed to Mission::Enter. The
	// controller must not touch the passenger again until it is inside.
	bool IsVanillaEntering(FootClass* pPassenger);

	// True when the passenger stands on an adjacent cell that is an allowed
	// entry side. At that point the engine may perform the final walk-in.
	bool IsAtFrontCell(UnitClass* pTransport, FootClass* pPassenger);

	// Records the transport as soon as the enter order is issued, before the
	// engine link is available. Does not assign an approach cell yet.
	void TrackTransport(FootClass* pPassenger, UnitClass* pTransport);

	// True when the passenger's current Move mission points at its assigned
	// approach cell, i.e. the move was issued by the boarding controller and
	// not by a new player order.
	bool IsManagedMove(FootClass* pPassenger);

	// True once the controller has assigned an approach cell.
	bool HasApproachCell(FootClass* pPassenger);
}
