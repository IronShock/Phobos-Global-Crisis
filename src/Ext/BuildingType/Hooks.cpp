#include "Body.h"

#include <TacticalClass.h>
#include <DisplayClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <GameModeOptionsClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <Unsorted.h>
#include <Ext/Rules/Body.h>
#include <Misc/Fog/Fog.h>
#include <Misc/ZoomManager.h>

#include <Utilities/Macro.h>
#include <Utilities/EnumFunctions.h>
#include <Utilities/GeneralUtils.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

DEFINE_HOOK(0x460285, BuildingTypeClass_LoadFromINI_Muzzle, 0x6)
{
	enum { Skip = 0x460388, Read = 0x460299 };

	GET(BuildingTypeClass*, pThis, EBP);

	// Restore overriden instructions
	R->Stack(STACK_OFFSET(0x368, -0x358), 0);
	R->EDX(0);

	// Disable Vanilla Muzzle flash when MaxNumberOccupants is 0 or more than 10
	return !pThis->MaxNumberOccupants || pThis->MaxNumberOccupants > 10
		? Skip : Read;
}

DEFINE_HOOK(0x44043D, BuildingClass_AI_Temporaled_Chronosparkle_MuzzleFix, 0x8)
{
	GET(const int, nFiringIndex, EBX);
	GET(BuildingClass*, pThis, ESI);

	auto const pType = pThis->Type;

	if (pType->MaxNumberOccupants > 10)
	{
		auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pType);
		R->EAX(&pTypeExt->OccupierMuzzleFlashes[nFiringIndex]);
	}

	return 0;
}

DEFINE_HOOK(0x45387A, BuildingClass_FireOffset_Replace_MuzzleFix, 0xA)
{
	GET(BuildingClass*, pThis, ESI);

	auto const pType = pThis->Type;

	if (pType->MaxNumberOccupants > 10)
	{
		auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pType);
		R->EDX(&pTypeExt->OccupierMuzzleFlashes[pThis->FiringOccupantIndex]);
	}

	return 0;
}

DEFINE_HOOK(0x458623, BuildingClass_KillOccupiers_Replace_MuzzleFix, 0x7)
{
	GET(const int, nFiringIndex, EDI);
	GET(BuildingClass*, pThis, ESI);

	auto const pType = pThis->Type;

	if (pType->MaxNumberOccupants > 10)
	{
		auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pType);
		R->ECX(&pTypeExt->OccupierMuzzleFlashes[nFiringIndex]);
	}

	return 0;
}

namespace
{
	enum class BuildRangeCellType : BYTE
	{
		None = 0,
		Legal = 1,
		Blocked = 2,
		Occupied = 3
	};

	struct BuildRangeEdge
	{
		CellStruct A;
		CellStruct B;
		int Z = 0;
	};

	struct BuildRangeCellEdges
	{
		BuildRangeEdge Edges[4] {};
		BYTE Count = 0;
	};

	struct BuildRangeData
	{
		int MinX = 0;
		int MinY = 0;
		int Width = 0;
		int Height = 0;
		std::vector<BuildRangeCellType> Cells;
		std::vector<BYTE> Fogged;
		bool HasFogged = false;
		std::vector<int> Heights;
		std::vector<BuildRangeEdge> Edges;
		std::vector<BuildRangeCellEdges> CellEdges;
		std::vector<BYTE> HoleMask;
		std::vector<int> ChangedCells;
	};

	struct BuildRangeCandidate
	{
		CellStruct Anchor;
		bool Valid = false;
	};

	struct BuildRangeCellSnapshot
	{
		bool Exists = false;
		int IsoTileTypeIndex = -1;
		int OverlayTypeIndex = -1;
		int SmudgeTypeIndex = -1;
		int WallOwnerIndex = -1;
		int InfantryOwnerIndex = -1;
		int AltInfantryOwnerIndex = -1;
		int LandType = 0;
		int Level = 0;
		BYTE SlopeIndex = 0;
		BYTE OverlayData = 0;
		BYTE SmudgeData = 0;
		BYTE Height = 0;
		BYTE BlockedNeighbours = 0;
		DWORD Passability = 0;
		DWORD OccupationFlags = 0;
		DWORD AltOccupationFlags = 0;
		DWORD Flags = 0;
		DWORD AltFlags = 0;
		DWORD BaseSpacerOfHouses = 0;
		DWORD GapsCoveringThisCell = 0;
		ObjectClass* FirstObject = nullptr;
		ObjectClass* AltObject = nullptr;
		FootClass* Jumpjet = nullptr;
		BuildingTypeClass* Rubble = nullptr;
		CellClass* BridgeOwnerCell = nullptr;

		bool operator==(const BuildRangeCellSnapshot& other) const
		{
			return Exists == other.Exists
				&& IsoTileTypeIndex == other.IsoTileTypeIndex
				&& OverlayTypeIndex == other.OverlayTypeIndex
				&& SmudgeTypeIndex == other.SmudgeTypeIndex
				&& WallOwnerIndex == other.WallOwnerIndex
				&& InfantryOwnerIndex == other.InfantryOwnerIndex
				&& AltInfantryOwnerIndex == other.AltInfantryOwnerIndex
				&& LandType == other.LandType
				&& Level == other.Level
				&& SlopeIndex == other.SlopeIndex
				&& OverlayData == other.OverlayData
				&& SmudgeData == other.SmudgeData
				&& Height == other.Height
				&& BlockedNeighbours == other.BlockedNeighbours
				&& Passability == other.Passability
				&& OccupationFlags == other.OccupationFlags
				&& AltOccupationFlags == other.AltOccupationFlags
				&& Flags == other.Flags
				&& AltFlags == other.AltFlags
				&& BaseSpacerOfHouses == other.BaseSpacerOfHouses
				&& GapsCoveringThisCell == other.GapsCoveringThisCell
				&& FirstObject == other.FirstObject
				&& AltObject == other.AltObject
				&& Jumpjet == other.Jumpjet
				&& Rubble == other.Rubble
				&& BridgeOwnerCell == other.BridgeOwnerCell;
		}

		bool operator!=(const BuildRangeCellSnapshot& other) const
		{
			return !(*this == other);
		}
	};

	struct BuildRangeStatic
	{
		int MinX = 0;
		int MinY = 0;
		int Width = 0;
		int Height = 0;
		std::vector<BYTE> Range;
		std::vector<BYTE> Red;
		std::vector<std::uint32_t> LegalCount;
		std::vector<CellStruct> Foundation;
		std::vector<BuildRangeCandidate> Candidates;
		std::vector<std::vector<std::uint32_t>> CellCandidates;
		std::vector<BuildRangeCellSnapshot> Snapshots;
	};

	struct BuildRangeSpan
	{
		int Y;
		int X0;
		int X1;
		BYTE Type;
	};

	struct BuildRangeFillRect
	{
		int X;
		int Y;
		int Width;
		int Height;
		BYTE Type;
	};

	struct BuildRangeFillCache
	{
		std::vector<BuildRangeFillRect> Rects;
		std::vector<BYTE> Mask;
		int MaskWidth = 0;
		int MaskHeight = 0;
		unsigned int Revision = 0;
		Point2D TacticalPos {};
		double Zoom = 0.0;
		RectangleStruct View {};
		bool Valid = false;
	};

	struct BuildRangeScreenEdge
	{
		Point2D A;
		Point2D B;
	};

	struct BuildRangeOutlineCache
	{
		std::vector<BuildRangeScreenEdge> Edges;
		unsigned int Revision = 0;
		Point2D TacticalPos {};
		double Zoom = 0.0;
		RectangleStruct View {};
		bool Valid = false;
	};

	struct BuildRangeLutCache
	{
		WORD Lut[4][65536] {};
		ColorStruct Colors[4] {};
		int Opacity = -1;
		bool Valid = false;
	};

	BuildRangeData CachedBuildRangeData;
	BuildRangeStatic CachedBuildRangeStatic;
	BuildingTypeClass* CachedBuildRangeType = nullptr;
	HouseClass* CachedBuildRangeOwner = nullptr;
	unsigned int CachedBuildRangeFrame = 0;
	std::uint64_t CachedBuildRangeBaseSignature = 0;
	std::uint64_t CachedBuildRangeFoundationHash = 0;
	int CachedBuildRangeBaseBuildingCount = -1;
	unsigned int CachedBuildRangeBaseCheckFrame = 0;
	unsigned int CachedBuildRangeRevision = 0;
	unsigned int CachedBuildRangeEdgeRevision = 0;
	BuildRangeFillCache CachedBuildRangeFill;
	BuildRangeOutlineCache CachedBuildRangeOutline;
	BuildRangeLutCache CachedBuildRangeLut;

	bool IsFoundationSentinel(const CellStruct& cell)
	{
		return cell.X == 0x7FFF && cell.Y == 0x7FFF;
	}

	void MergeBuildRangeEdges(std::vector<BuildRangeEdge>& edges)
	{
		struct Segment
		{
			int Line;
			int Start;
			int End;
			int Z;
			bool Vertical;
		};

		std::vector<Segment> segments;
		segments.reserve(edges.size());

		for (auto const& edge : edges)
		{
			if (edge.A.Y == edge.B.Y)
			{
				segments.push_back({
					edge.A.Y,
					std::min(edge.A.X, edge.B.X),
					std::max(edge.A.X, edge.B.X),
					edge.Z,
					false
				});
			}
			else if (edge.A.X == edge.B.X)
			{
				segments.push_back({
					edge.A.X,
					std::min(edge.A.Y, edge.B.Y),
					std::max(edge.A.Y, edge.B.Y),
					edge.Z,
					true
				});
			}
		}

		std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b)
		{
			if (a.Vertical != b.Vertical)
				return a.Vertical < b.Vertical;

			if (a.Line != b.Line)
				return a.Line < b.Line;

			if (a.Z != b.Z)
				return a.Z < b.Z;

			return a.Start < b.Start;
		});

		edges.clear();

		for (auto const& segment : segments)
		{
			if (!edges.empty())
			{
				auto& last = edges.back();
				const bool sameVertical = last.A.X == last.B.X;

				if (sameVertical == segment.Vertical)
				{
					const int lastLine = sameVertical ? last.A.X : last.A.Y;
					const int lastStart = sameVertical
						? std::min(last.A.Y, last.B.Y)
						: std::min(last.A.X, last.B.X);
					const int lastEnd = sameVertical
						? std::max(last.A.Y, last.B.Y)
						: std::max(last.A.X, last.B.X);

					if (lastLine == segment.Line && last.Z == segment.Z
						&& segment.Start <= lastEnd)
					{
						if (sameVertical)
						{
							last.A.Y = static_cast<short>(lastStart);
							last.B.Y = static_cast<short>(std::max(lastEnd, segment.End));
						}
						else
						{
							last.A.X = static_cast<short>(lastStart);
							last.B.X = static_cast<short>(std::max(lastEnd, segment.End));
						}

						continue;
					}
				}
			}

			BuildRangeEdge edge {};

			if (segment.Vertical)
			{
				edge.A = { static_cast<short>(segment.Line), static_cast<short>(segment.Start) };
				edge.B = { static_cast<short>(segment.Line), static_cast<short>(segment.End) };
			}
			else
			{
				edge.A = { static_cast<short>(segment.Start), static_cast<short>(segment.Line) };
				edge.B = { static_cast<short>(segment.End), static_cast<short>(segment.Line) };
			}

			edge.Z = segment.Z;
			edges.push_back(edge);
		}
	}

	static int BuildRangeIndex(const BuildRangeData& data, int x, int y)
	{
		return (y - data.MinY) * data.Width + (x - data.MinX);
	}

	static int BuildRangeIndex(const BuildRangeStatic& data, int x, int y)
	{
		return (y - data.MinY) * data.Width + (x - data.MinX);
	}

	void RebuildBuildRangeEdges(BuildRangeData& result);

	static bool IsBuildRangeCellBlocked(
		CellClass* pCell,
		BuildingTypeClass* pType,
		HouseClass* pOwner)
	{
		if (!pCell)
			return true;

		// Use the engine's cell-level rule for every building type. The old
		// hand-written naval shortcut omitted constraints such as WaterBound,
		// foundation placement, bridges and shoreline rules, which made some
		// actually invalid cells appear as legal range.
		const SpeedType buildSpeed = pType->SpeedType;
		return !pCell->CanThisExistHere(buildSpeed, pType, pOwner);
	}

	static BuildRangeCellSnapshot CaptureBuildRangeCellSnapshot(CellClass* pCell)
	{
		BuildRangeCellSnapshot snapshot;

		if (!pCell)
			return snapshot;

		snapshot.Exists = true;
		snapshot.IsoTileTypeIndex = pCell->IsoTileTypeIndex;
		snapshot.OverlayTypeIndex = pCell->OverlayTypeIndex;
		snapshot.SmudgeTypeIndex = pCell->SmudgeTypeIndex;
		snapshot.WallOwnerIndex = pCell->WallOwnerIndex;
		snapshot.InfantryOwnerIndex = pCell->InfantryOwnerIndex;
		snapshot.AltInfantryOwnerIndex = pCell->AltInfantryOwnerIndex;
		snapshot.LandType = static_cast<int>(pCell->LandType);
		snapshot.Level = static_cast<int>(pCell->Level);
		snapshot.SlopeIndex = pCell->SlopeIndex;
		snapshot.OverlayData = pCell->OverlayData;
		snapshot.SmudgeData = pCell->SmudgeData;
		snapshot.Height = static_cast<BYTE>(pCell->Height);
		snapshot.BlockedNeighbours = pCell->BlockedNeighbours;
		snapshot.Passability = static_cast<DWORD>(pCell->Passability);
		snapshot.OccupationFlags = pCell->OccupationFlags;
		snapshot.AltOccupationFlags = pCell->AltOccupationFlags;
		snapshot.Flags = static_cast<DWORD>(pCell->Flags);
		snapshot.AltFlags = static_cast<DWORD>(pCell->AltFlags);
		snapshot.BaseSpacerOfHouses = pCell->BaseSpacerOfHouses;
		snapshot.GapsCoveringThisCell = pCell->GapsCoveringThisCell;
		snapshot.FirstObject = pCell->FirstObject;
		snapshot.AltObject = pCell->AltObject;
		snapshot.Jumpjet = pCell->Jumpjet;
		snapshot.Rubble = pCell->Rubble;
		snapshot.BridgeOwnerCell = pCell->BridgeOwnerCell;

		return snapshot;
	}

	static BuildRangeCellType GetBuildRangeDisplayType(
		bool range,
		bool red,
		std::uint32_t legalCount,
		bool disableRed,
		bool disableYellow)
	{
		if (disableRed)
			return range ? BuildRangeCellType::Legal : BuildRangeCellType::None;

		if (red)
			return BuildRangeCellType::Occupied;

		if (legalCount > 0)
			return BuildRangeCellType::Legal;

		if (!range)
			return BuildRangeCellType::None;

		return disableYellow
			? BuildRangeCellType::Occupied
			: BuildRangeCellType::Blocked;
	}

	static bool ValidateBuildRangeCandidate(
		BuildingTypeClass* pType,
		HouseClass* pOwner,
		const CellStruct& anchor,
		const std::vector<CellStruct>& foundation)
	{
		// Reject candidates with a blocked foundation cell before paying for
		// the full engine placement check. This is the common case when a unit
		// or building moves into one cell of the foundation.
		for (auto const& offset : foundation)
		{
			const CellStruct cell {
				static_cast<short>(anchor.X + offset.X),
				static_cast<short>(anchor.Y + offset.Y)
			};

			if (!MapClass::Instance.CellExists(cell)
				|| IsBuildRangeCellBlocked(
					MapClass::Instance.TryGetCellAt(cell), pType, pOwner))
			{
				return false;
			}
		}

		CellStruct candidate = anchor;
		return pType->CanPlaceHere(&candidate, pOwner);
	}

	static std::uint64_t HashBuildRangeFoundation(const CellStruct* pData)
	{
		std::uint64_t hash = 1469598103934665603ull;
		auto mix = [&hash](std::uint64_t value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		};

		if (pData)
		{
			for (; !IsFoundationSentinel(*pData); ++pData)
			{
				mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(pData->X)));
				mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(pData->Y)));
			}
		}

		mix(0x7FFFull);
		mix(0x7FFFull);

		return hash;
	}

	static std::uint64_t HashBuildRangeBaseSignature(HouseClass* pOwner)
	{
		std::uint64_t hash = 1469598103934665603ull;
		auto mix = [&hash](std::uint64_t value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		};

		const auto& mapRect = MapClass::Instance.MapRect;
		mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(mapRect.X)));
		mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(mapRect.Y)));
		mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(mapRect.Width)));
		mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(mapRect.Height)));

		if (!pOwner)
			return hash;

		for (auto const pAnchor : BuildingClass::Array)
		{
			if (!pAnchor || !pAnchor->IsAlive || pAnchor->InLimbo
				|| !pAnchor->Type || !pAnchor->Owner || !pAnchor->Type->BaseNormal
				|| !pOwner->IsAlliedWith(pAnchor->Owner))
			{
				continue;
			}

			const CellStruct anchor = pAnchor->GetMapCoords();
			mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(anchor.X)));
			mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(anchor.Y)));
			mix(reinterpret_cast<std::uintptr_t>(pAnchor->Type));
			mix(static_cast<std::uint64_t>(pAnchor->Owner->ArrayIndex));
		}

		return hash;
	}

	BuildRangeData BuildBuildRangeData(
		BuildingTypeClass* pType,
		HouseClass* pOwner,
		BuildRangeStatic& staticOut)
	{
		BuildRangeData result;
		staticOut = BuildRangeStatic {};

		if (!pType || !DisplayClass::Instance.CurrentFoundation_Data)
			return result;

		if (!pOwner || pType->Adjacent < 0)
			return result;

		// Copy the foundation data so the placement checks cannot disturb the
		// active foundation used by the game.
		std::vector<CellStruct> foundationData;
		for (auto pCell = DisplayClass::Instance.CurrentFoundation_Data;
			!IsFoundationSentinel(*pCell); ++pCell)
		{
			foundationData.push_back(*pCell);
		}

		if (foundationData.empty())
			return result;

		foundationData.push_back(CellStruct { 0x7FFF, 0x7FFF });

		std::vector<CellStruct> foundation(
			foundationData.begin(), foundationData.end() - 1);

		staticOut.Foundation = foundation;

		const int adjacent = std::clamp(static_cast<int>(pType->Adjacent), 0, 30);
		const int foundationWidth = std::max(1, static_cast<int>(pType->GetFoundationWidth()));
		const int foundationHeight = std::max(1, static_cast<int>(pType->GetFoundationHeight(false)));

		int minX = std::numeric_limits<int>::max();
		int minY = std::numeric_limits<int>::max();
		int maxX = std::numeric_limits<int>::min();
		int maxY = std::numeric_limits<int>::min();

		auto accumulateAnchorBounds = [&](bool strict)
		{
			for (auto const pAnchor : BuildingClass::Array)
			{
				if (!pAnchor || !pAnchor->Type)
					continue;

				if (strict)
				{
					if (!pAnchor->IsAlive || pAnchor->InLimbo || !pAnchor->Owner
						|| !pAnchor->Type->BaseNormal
						|| !pOwner->IsAlliedWith(pAnchor->Owner))
					{
						continue;
					}
				}
				else if (!pAnchor->Owner
					|| (pAnchor->Owner != pOwner && !pOwner->IsAlliedWith(pAnchor->Owner)))
				{
					continue;
				}

				const CellStruct anchor = pAnchor->GetMapCoords();
				const int anchorWidth = std::max(1, static_cast<int>(pAnchor->Type->GetFoundationWidth()));
				const int anchorHeight = std::max(1, static_cast<int>(pAnchor->Type->GetFoundationHeight(false)));
				const int margin = adjacent + foundationWidth + foundationHeight
					+ anchorWidth + anchorHeight + 2;

				minX = std::min(minX, static_cast<int>(anchor.X) - margin);
				minY = std::min(minY, static_cast<int>(anchor.Y) - margin);
				maxX = std::max(maxX, static_cast<int>(anchor.X) + anchorWidth + margin);
				maxY = std::max(maxY, static_cast<int>(anchor.Y) + anchorHeight + margin);
			}
		};

		accumulateAnchorBounds(true);

		// The proximity check below is the authority on which cells belong to
		// the range. If no strict base structure was found (for example because
		// a mod flags its conyard as BaseNormal=no), widen the scan so the
		// overlay can still be drawn; cells outside the real range stay empty.
		if (minX > maxX || minY > maxY)
			accumulateAnchorBounds(false);

		if (minX > maxX || minY > maxY)
			return result;

		// MapRect is not always the plain map rectangle in large-map setups
		// (MapSizeExt et al). Only clamp when it actually overlaps the range
		// computed from the base; otherwise a bogus/offset rect collapses the
		// whole grid to a single column.
		const auto& mapRect = MapClass::Instance.MapRect;
		const int mapMaxX = mapRect.X + mapRect.Width - 1;
		const int mapMaxY = mapRect.Y + mapRect.Height - 1;

		if (mapRect.Width > 0 && mapRect.Height > 0
			&& mapRect.X <= maxX && mapMaxX >= minX
			&& mapRect.Y <= maxY && mapMaxY >= minY)
		{
			minX = std::clamp(minX, static_cast<int>(mapRect.X), mapMaxX);
			minY = std::clamp(minY, static_cast<int>(mapRect.Y), mapMaxY);
			maxX = std::clamp(maxX, static_cast<int>(mapRect.X), mapMaxX);
			maxY = std::clamp(maxY, static_cast<int>(mapRect.Y), mapMaxY);
		}

		result.MinX = minX;
		result.MinY = minY;
		result.Width = maxX - minX + 1;
		result.Height = maxY - minY + 1;

		if (result.Width <= 0 || result.Height <= 0)
			return result;

		constexpr BYTE FlagRange = 0x01;
		constexpr BYTE FlagLegal = 0x02;
		constexpr BYTE FlagOccupied = 0x04;
		constexpr BYTE FlagFogged = 0x08;

		const size_t cellCount = static_cast<size_t>(result.Width)
			* static_cast<size_t>(result.Height);

		std::vector<BYTE> flags(cellCount, 0);
		std::vector<BuildRangeCandidate> candidates;

		auto markFlag = [&](int x, int y, BYTE flag)
		{
			if (x < result.MinX || x >= result.MinX + result.Width
				|| y < result.MinY || y >= result.MinY + result.Height)
			{
				return;
			}

			flags[BuildRangeIndex(result, x, y)] |= flag;
		};

		// The yellow base is the engine's actual Adjacent neighbourhood. A 1x1
		// proximity check gives us every cell in that neighbourhood, including
		// cells the current building's larger foundation cannot cover alone.
		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				CellStruct candidate { static_cast<short>(x), static_cast<short>(y) };
				CellStruct singleCellFoundation[] = {
					{ 0, 0 },
					{ 0x7FFF, 0x7FFF }
				};

				if (MapClass::Instance.CellExists(candidate)
					&& DisplayClass::Instance.PassesProximityCheck(
						pType, pOwner->ArrayIndex, singleCellFoundation, &candidate))
				{
					markFlag(x, y, FlagRange);
				}
			}
		}

		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				CellStruct candidate { static_cast<short>(x), static_cast<short>(y) };

				// Proximity is the cheaper rejection test; only run the full
				// terrain/occupancy check for candidates that are near a base.
				if (!DisplayClass::Instance.PassesProximityCheck(
					pType, pOwner->ArrayIndex, foundationData.data(), &candidate))
				{
					continue;
				}

				std::vector<CellStruct> cells;
				cells.reserve(foundation.size());

				bool allInBounds = true;

				for (auto const& offset : foundation)
				{
					const CellStruct cell {
						static_cast<short>(candidate.X + offset.X),
						static_cast<short>(candidate.Y + offset.Y)
					};

					if (!MapClass::Instance.CellExists(cell)
						|| cell.X < result.MinX || cell.X >= result.MinX + result.Width
						|| cell.Y < result.MinY || cell.Y >= result.MinY + result.Height)
					{
						allInBounds = false;
						break;
					}

					cells.push_back(cell);
				}

				if (!allInBounds)
					continue;

				for (auto const& cell : cells)
					markFlag(cell.X, cell.Y, FlagRange);

				// Always use the engine's actual placement check. Naval
				// buildings are no longer validated with a reduced custom
				// check, so green range now matches what the engine accepts.
				const bool canPlace = pType->CanPlaceHere(&candidate, pOwner);

				BuildRangeCandidate candidateEntry;
				candidateEntry.Anchor = candidate;
				candidateEntry.Valid = canPlace;
				candidates.push_back(candidateEntry);

				if (canPlace)
				{
					for (auto const& cell : cells)
						markFlag(cell.X, cell.Y, FlagLegal);
				}
			}
		}

		result.Heights.assign(cellCount, 0);
		result.Fogged.assign(cellCount, 0);

		// Red is the cell-level actual unbuildable test. It covers terrain,
		// trees/rocks, water, cliffs, overlays, walls, buildings and ground
		// units/infantry in one check.
		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				const int index = BuildRangeIndex(result, x, y);

				if ((flags[index] & FlagRange) == 0)
					continue;

				const auto pCell = MapClass::Instance.TryGetCellAt(
					CellStruct { static_cast<short>(x), static_cast<short>(y) });

				if (pCell)
				{
					result.Heights[index] = pCell->GetCellCoords().Z;

					// Hide native fog and black/unexplored shroud cells. Normal
					// visible cells have Foggedness == -1 and are mapped.
					if (pCell->Foggedness != -1 || !Fog::IsMappedCell(pCell))
					{
						flags[index] |= FlagFogged;
						result.Fogged[index] = 1;
						result.HasFogged = true;
					}
				}

				if (IsBuildRangeCellBlocked(pCell, pType, pOwner))
					flags[index] |= FlagOccupied;
			}
		}

		staticOut.MinX = result.MinX;
		staticOut.MinY = result.MinY;
		staticOut.Width = result.Width;
		staticOut.Height = result.Height;
		staticOut.Range.assign(cellCount, 0);
		staticOut.Red.assign(cellCount, 0);
		staticOut.LegalCount.assign(cellCount, 0);
		staticOut.Candidates = std::move(candidates);
		staticOut.CellCandidates.assign(cellCount, {});
		staticOut.Snapshots.resize(cellCount);

		for (std::uint32_t candidateIndex = 0;
			candidateIndex < staticOut.Candidates.size(); ++candidateIndex)
		{
			const auto& candidate = staticOut.Candidates[candidateIndex];

			for (auto const& offset : staticOut.Foundation)
			{
				const CellStruct cell {
					static_cast<short>(candidate.Anchor.X + offset.X),
					static_cast<short>(candidate.Anchor.Y + offset.Y)
				};

				const int index = BuildRangeIndex(result, cell.X, cell.Y);

				if (candidate.Valid)
					++staticOut.LegalCount[index];

				// A changed cell can affect placement of a nearby foundation
				// through bibs, shore rules or adjacent occupancy. Map the
				// candidate into its 3x3 neighbourhood so those candidates are
				// revalidated as well.
				for (int dy = -1; dy <= 1; ++dy)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = cell.X + dx;
						const int ny = cell.Y + dy;

						if (nx < result.MinX || nx >= result.MinX + result.Width
							|| ny < result.MinY || ny >= result.MinY + result.Height)
						{
							continue;
						}

						staticOut.CellCandidates[
							BuildRangeIndex(result, nx, ny)].push_back(candidateIndex);
					}
				}
			}
		}

		for (size_t i = 0; i < cellCount; ++i)
		{
			if ((flags[i] & FlagRange) == 0)
				continue;

			staticOut.Range[i] = 1;
			staticOut.Red[i] = (flags[i] & FlagOccupied) != 0;

			const int x = result.MinX + static_cast<int>(i % result.Width);
			const int y = result.MinY + static_cast<int>(i / result.Width);
			staticOut.Snapshots[i] = CaptureBuildRangeCellSnapshot(
				MapClass::Instance.TryGetCellAt(
					CellStruct { static_cast<short>(x), static_cast<short>(y) }));
		}

		const auto pRules = RulesExt::Global();
		const bool disableRed = pRules->BuildRange_DisableRed;
		const bool disableYellow = pRules->BuildRange_DisableYellow;

		result.Cells.resize(cellCount);

		for (size_t i = 0; i < cellCount; ++i)
		{
			if (disableRed)
			{
				// With red disabled the whole actual range is presented as
				// buildable green. This also overrides DisableYellow.
				result.Cells[i] = (flags[i] & FlagRange) != 0
					? BuildRangeCellType::Legal
					: BuildRangeCellType::None;
			}
			else if (flags[i] & FlagOccupied)
				result.Cells[i] = BuildRangeCellType::Occupied;
			else if (flags[i] & FlagLegal)
				result.Cells[i] = BuildRangeCellType::Legal;
			else if (flags[i] & FlagRange)
				result.Cells[i] = disableYellow
					? BuildRangeCellType::Occupied
					: BuildRangeCellType::Blocked;
			else
				result.Cells[i] = BuildRangeCellType::None;
		}

		RebuildBuildRangeEdges(result);

		return result;
	}

	void RebuildBuildRangeEdges(BuildRangeData& result)
	{
		result.Edges.clear();

		const size_t cellCount = static_cast<size_t>(result.Width)
			* static_cast<size_t>(result.Height);
		result.CellEdges.assign(cellCount, {});
		result.HoleMask.assign(cellCount, 0);

		// The white outline follows the green legal set. Non-green cells that
		// are enclosed by the green set are holes: their inner edges are not
		// part of the outer contour and are deliberately not drawn.
		const int holeWidth = result.Width + 2;
		const int holeHeight = result.Height + 2;
		std::vector<BYTE> exterior(static_cast<size_t>(holeWidth) * holeHeight, 0);
		std::vector<int> queue;
		queue.reserve(static_cast<size_t>(holeWidth) * holeHeight);

		auto isGreen = [&](int x, int y)
		{
			if (x < result.MinX || x >= result.MinX + result.Width
				|| y < result.MinY || y >= result.MinY + result.Height)
			{
				return false;
			}

			const int index = BuildRangeIndex(result, x, y);
			return result.Cells[index] == BuildRangeCellType::Legal;
		};

		auto pushExterior = [&](int hx, int hy)
		{
			if (hx < 0 || hx >= holeWidth || hy < 0 || hy >= holeHeight)
				return;

			const int x = result.MinX + hx - 1;
			const int y = result.MinY + hy - 1;

			if (isGreen(x, y))
				return;

			const int index = hy * holeWidth + hx;

			if (exterior[index])
				return;

			exterior[index] = 1;
			queue.push_back(index);
		};

		for (int x = 0; x < holeWidth; ++x)
		{
			pushExterior(x, 0);
			pushExterior(x, holeHeight - 1);
		}

		for (int y = 0; y < holeHeight; ++y)
		{
			pushExterior(0, y);
			pushExterior(holeWidth - 1, y);
		}

		for (size_t i = 0; i < queue.size(); ++i)
		{
			const int index = queue[i];
			const int hx = index % holeWidth;
			const int hy = index / holeWidth;

			pushExterior(hx - 1, hy);
			pushExterior(hx + 1, hy);
			pushExterior(hx, hy - 1);
			pushExterior(hx, hy + 1);
		}

		auto isHole = [&](int x, int y)
		{
			if (x < result.MinX || x >= result.MinX + result.Width
				|| y < result.MinY || y >= result.MinY + result.Height)
			{
				return false;
			}

			const int index = BuildRangeIndex(result, x, y);

			if (result.Cells[index] == BuildRangeCellType::Legal)
				return false;

			return !exterior[(y - result.MinY + 1) * holeWidth + (x - result.MinX + 1)];
		};

		auto isColoredNonGreen = [&](int x, int y)
		{
			if (x < result.MinX || x >= result.MinX + result.Width
				|| y < result.MinY || y >= result.MinY + result.Height)
			{
				return false;
			}

			const auto value = result.Cells[BuildRangeIndex(result, x, y)];

			return value == BuildRangeCellType::Blocked
				|| value == BuildRangeCellType::Occupied;
		};

		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				if (isHole(x, y))
					result.HoleMask[BuildRangeIndex(result, x, y)] = 1;
			}
		}

		static constexpr CellStruct neighbours[] = {
			{ 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 }
		};

		auto addEdge = [&](int x, int y, const BuildRangeEdge& edge)
		{
			result.Edges.push_back(edge);

			auto& cellEdges = result.CellEdges[BuildRangeIndex(result, x, y)];

			if (cellEdges.Count < 4)
				cellEdges.Edges[cellEdges.Count++] = edge;
		};

		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				if (!isGreen(x, y))
					continue;

				const int cellZ = result.Heights[BuildRangeIndex(result, x, y)];

				for (auto const& offset : neighbours)
				{
					const int nx = x + offset.X;
					const int ny = y + offset.Y;

					if (isGreen(nx, ny))
						continue;

					// Keep the outer contour and draw the colour boundaries
					// between green and red/yellow. Edges around empty enclosed
					// holes remain hidden.
					if (!isColoredNonGreen(nx, ny) && isHole(nx, ny))
						continue;

					BuildRangeEdge edge {};

					if (offset.Y < 0)
					{
						edge.A = { static_cast<short>(x), static_cast<short>(y) };
						edge.B = { static_cast<short>(x + 1), static_cast<short>(y) };
					}
					else if (offset.X > 0)
					{
						edge.A = { static_cast<short>(x + 1), static_cast<short>(y) };
						edge.B = { static_cast<short>(x + 1), static_cast<short>(y + 1) };
					}
					else if (offset.Y > 0)
					{
						edge.A = { static_cast<short>(x), static_cast<short>(y + 1) };
						edge.B = { static_cast<short>(x + 1), static_cast<short>(y + 1) };
					}
					else
					{
						edge.A = { static_cast<short>(x), static_cast<short>(y) };
						edge.B = { static_cast<short>(x), static_cast<short>(y + 1) };
					}

					edge.Z = cellZ;
					addEdge(x, y, edge);
				}
			}
		}

		MergeBuildRangeEdges(result.Edges);
	}

	static void RebuildBuildRangeCellEdges(BuildRangeData& result, int x, int y)
	{
		if (x < result.MinX || x >= result.MinX + result.Width
			|| y < result.MinY || y >= result.MinY + result.Height)
		{
			return;
		}

		const int index = BuildRangeIndex(result, x, y);
		auto& cellEdges = result.CellEdges[index];
		cellEdges.Count = 0;

		if (result.Cells[index] != BuildRangeCellType::Legal)
			return;

		auto isGreen = [&](int nx, int ny)
		{
			if (nx < result.MinX || nx >= result.MinX + result.Width
				|| ny < result.MinY || ny >= result.MinY + result.Height)
			{
				return false;
			}

			return result.Cells[BuildRangeIndex(result, nx, ny)]
				== BuildRangeCellType::Legal;
		};

		auto isColoredNonGreen = [&](int nx, int ny)
		{
			if (nx < result.MinX || nx >= result.MinX + result.Width
				|| ny < result.MinY || ny >= result.MinY + result.Height)
			{
				return false;
			}

			const auto value = result.Cells[BuildRangeIndex(result, nx, ny)];
			return value == BuildRangeCellType::Blocked
				|| value == BuildRangeCellType::Occupied;
		};

		auto isHole = [&](int nx, int ny)
		{
			if (nx < result.MinX || nx >= result.MinX + result.Width
				|| ny < result.MinY || ny >= result.MinY + result.Height)
			{
				return false;
			}

			return result.HoleMask[BuildRangeIndex(result, nx, ny)] != 0;
		};

		static constexpr CellStruct neighbours[] = {
			{ 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 }
		};

		const int cellZ = result.Heights[index];

		for (auto const& offset : neighbours)
		{
			const int nx = x + offset.X;
			const int ny = y + offset.Y;

			if (isGreen(nx, ny))
				continue;

			if (!isColoredNonGreen(nx, ny) && isHole(nx, ny))
				continue;

			BuildRangeEdge edge {};

			if (offset.Y < 0)
			{
				edge.A = { static_cast<short>(x), static_cast<short>(y) };
				edge.B = { static_cast<short>(x + 1), static_cast<short>(y) };
			}
			else if (offset.X > 0)
			{
				edge.A = { static_cast<short>(x + 1), static_cast<short>(y) };
				edge.B = { static_cast<short>(x + 1), static_cast<short>(y + 1) };
			}
			else if (offset.Y > 0)
			{
				edge.A = { static_cast<short>(x), static_cast<short>(y + 1) };
				edge.B = { static_cast<short>(x + 1), static_cast<short>(y + 1) };
			}
			else
			{
				edge.A = { static_cast<short>(x), static_cast<short>(y) };
				edge.B = { static_cast<short>(x), static_cast<short>(y + 1) };
			}

			edge.Z = cellZ;

			if (cellEdges.Count < 4)
				cellEdges.Edges[cellEdges.Count++] = edge;
		}
	}

	static void RebuildBuildRangeEdgesLocal(
		BuildRangeData& result,
		const std::vector<int>& dirtyCells)
	{
		const size_t cellCount = static_cast<size_t>(result.Width)
			* static_cast<size_t>(result.Height);

		if (result.CellEdges.size() != cellCount
			|| result.HoleMask.size() != cellCount
			|| dirtyCells.empty())
		{
			RebuildBuildRangeEdges(result);
			return;
		}

		std::vector<BYTE> dirty(cellCount, 0);

		for (auto const index : dirtyCells)
		{
			if (index < 0 || static_cast<size_t>(index) >= cellCount)
				continue;

			const int x = result.MinX + index % result.Width;
			const int y = result.MinY + index / result.Width;

			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = x + dx;
					const int ny = y + dy;

					if (nx < result.MinX || nx >= result.MinX + result.Width
						|| ny < result.MinY || ny >= result.MinY + result.Height)
					{
						continue;
					}

					dirty[BuildRangeIndex(result, nx, ny)] = 1;
				}
			}
		}

		for (int y = result.MinY; y < result.MinY + result.Height; ++y)
		{
			for (int x = result.MinX; x < result.MinX + result.Width; ++x)
			{
				if (dirty[BuildRangeIndex(result, x, y)])
					RebuildBuildRangeCellEdges(result, x, y);
			}
		}

		result.Edges.clear();

		for (auto const& cellEdges : result.CellEdges)
		{
			for (BYTE i = 0; i < cellEdges.Count; ++i)
				result.Edges.push_back(cellEdges.Edges[i]);
		}

		MergeBuildRangeEdges(result.Edges);
	}

	bool RefreshBuildRangeData(
		BuildingTypeClass* pType,
		HouseClass* pOwner,
		BuildRangeStatic& rangeStatic,
		BuildRangeData& rangeData)
	{
		if (!pType || !pOwner
			|| rangeStatic.Width <= 0 || rangeStatic.Height <= 0)
		{
			return false;
		}

		const size_t cellCount = static_cast<size_t>(rangeStatic.Width)
			* static_cast<size_t>(rangeStatic.Height);

		if (rangeStatic.Range.size() != cellCount
			|| rangeStatic.Red.size() != cellCount
			|| rangeStatic.LegalCount.size() != cellCount
			|| rangeStatic.Snapshots.size() != cellCount
			|| rangeData.Cells.size() != cellCount
			|| rangeData.Heights.size() != cellCount)
		{
			return false;
		}

		std::vector<BYTE> cellChanged(cellCount, 0);
		std::vector<BYTE> candidateDirty(rangeStatic.Candidates.size(), 0);
		std::vector<std::uint32_t> dirtyCandidates;
		dirtyCandidates.reserve(64);
		rangeData.ChangedCells.clear();

		bool visualChanged = false;
		bool greenChanged = false;
		std::vector<int> edgeDirtyCells;
		auto touchesGreen = [&](int index)
		{
			if (rangeData.Cells[index] == BuildRangeCellType::Legal)
				return true;

			const int x = rangeStatic.MinX + index % rangeStatic.Width;
			const int y = rangeStatic.MinY + index / rangeStatic.Width;

			if (x > rangeStatic.MinX
				&& rangeData.Cells[BuildRangeIndex(rangeStatic, x - 1, y)]
					== BuildRangeCellType::Legal)
			{
				return true;
			}

			if (x + 1 < rangeStatic.MinX + rangeStatic.Width
				&& rangeData.Cells[BuildRangeIndex(rangeStatic, x + 1, y)]
					== BuildRangeCellType::Legal)
			{
				return true;
			}

			if (y > rangeStatic.MinY
				&& rangeData.Cells[BuildRangeIndex(rangeStatic, x, y - 1)]
					== BuildRangeCellType::Legal)
			{
				return true;
			}

			if (y + 1 < rangeStatic.MinY + rangeStatic.Height
				&& rangeData.Cells[BuildRangeIndex(rangeStatic, x, y + 1)]
					== BuildRangeCellType::Legal)
			{
				return true;
			}

			return false;
		};

		for (int y = rangeStatic.MinY; y < rangeStatic.MinY + rangeStatic.Height; ++y)
		{
			for (int x = rangeStatic.MinX; x < rangeStatic.MinX + rangeStatic.Width; ++x)
			{
				const int index = BuildRangeIndex(rangeStatic, x, y);

				if (!rangeStatic.Range[index])
					continue;

				const auto pCell = MapClass::Instance.TryGetCellAt(
					CellStruct { static_cast<short>(x), static_cast<short>(y) });
				auto snapshot = CaptureBuildRangeCellSnapshot(pCell);
				const auto previousSnapshot = rangeStatic.Snapshots[index];

				if (snapshot == previousSnapshot)
					continue;

				rangeStatic.Snapshots[index] = snapshot;
				cellChanged[index] = 1;

				// Height is only queried when the cell's height fields changed.
				// The old code called GetCellCoords() for every cell on every
				// refresh, which was one of the largest remaining costs.
				if (snapshot.Level != previousSnapshot.Level
					|| snapshot.Height != previousSnapshot.Height
					|| snapshot.IsoTileTypeIndex != previousSnapshot.IsoTileTypeIndex
					|| snapshot.OverlayTypeIndex != previousSnapshot.OverlayTypeIndex
					|| snapshot.LandType != previousSnapshot.LandType
					|| snapshot.SlopeIndex != previousSnapshot.SlopeIndex)
				{
					const int newZ = pCell ? pCell->GetCellCoords().Z : 0;

					if (rangeData.Heights[index] != newZ)
					{
						rangeData.Heights[index] = newZ;
						visualChanged = true;

						if (touchesGreen(index))
							edgeDirtyCells.push_back(index);
					}
				}

				const bool red = IsBuildRangeCellBlocked(pCell, pType, pOwner);

				if (rangeStatic.Red[index] != static_cast<BYTE>(red ? 1 : 0))
					rangeStatic.Red[index] = red ? 1 : 0;

				for (auto const candidateIndex : rangeStatic.CellCandidates[index])
				{
					if (candidateDirty[candidateIndex])
						continue;

					candidateDirty[candidateIndex] = 1;
					dirtyCandidates.push_back(candidateIndex);
				}
			}
		}

		for (auto const candidateIndex : dirtyCandidates)
		{
			auto& candidate = rangeStatic.Candidates[candidateIndex];
			const bool valid = ValidateBuildRangeCandidate(
				pType, pOwner, candidate.Anchor, rangeStatic.Foundation);

			if (valid == candidate.Valid)
				continue;

			candidate.Valid = valid;

			for (auto const& offset : rangeStatic.Foundation)
			{
				const CellStruct cell {
					static_cast<short>(candidate.Anchor.X + offset.X),
					static_cast<short>(candidate.Anchor.Y + offset.Y)
				};

				const int index = BuildRangeIndex(rangeStatic, cell.X, cell.Y);
				cellChanged[index] = 1;

				if (valid)
				{
					++rangeStatic.LegalCount[index];
				}
				else if (rangeStatic.LegalCount[index] > 0)
				{
					--rangeStatic.LegalCount[index];
				}
			}
		}

		const auto pRules = RulesExt::Global();
		const bool disableRed = pRules->BuildRange_DisableRed;
		const bool disableYellow = pRules->BuildRange_DisableYellow;

		for (size_t i = 0; i < cellCount; ++i)
		{
			if (!cellChanged[i])
				continue;

			const auto newType = GetBuildRangeDisplayType(
				rangeStatic.Range[i] != 0,
				rangeStatic.Red[i] != 0,
				rangeStatic.LegalCount[i],
				disableRed,
				disableYellow);

			if (rangeData.Cells[i] != newType)
			{
				const auto oldType = rangeData.Cells[i];

				if (oldType == BuildRangeCellType::Legal
					|| newType == BuildRangeCellType::Legal)
				{
					greenChanged = true;
				}
				else if (touchesGreen(static_cast<int>(i)))
				{
					edgeDirtyCells.push_back(static_cast<int>(i));
				}

				rangeData.Cells[i] = newType;
				rangeData.ChangedCells.push_back(static_cast<int>(i));
				visualChanged = true;
			}
		}

		if (!visualChanged)
			return false;

		if (greenChanged)
		{
			RebuildBuildRangeEdges(rangeData);
			++CachedBuildRangeEdgeRevision;
		}
		else if (!edgeDirtyCells.empty())
		{
			RebuildBuildRangeEdgesLocal(rangeData, edgeDirtyCells);
			++CachedBuildRangeEdgeRevision;
		}

		return true;
	}

	CoordStruct CellCornerToWorld(const CellStruct& corner, int z)
	{
		return CoordStruct {
			static_cast<int>(corner.X) * Unsorted::LeptonsPerCell,
			static_cast<int>(corner.Y) * Unsorted::LeptonsPerCell,
			z
		};
	}

	CoordStruct CellCornerToWorldFromCorner(const CellStruct& corner)
	{
		CoordStruct result {
			static_cast<int>(corner.X) * Unsorted::LeptonsPerCell,
			static_cast<int>(corner.Y) * Unsorted::LeptonsPerCell,
			0
		};

		result.Z = MapClass::Instance.GetCellFloorHeight(result);
		return result;
	}

	static bool RasterizeBuildRangeCell(
		std::vector<BYTE>& mask,
		const RectangleStruct& clip,
		const Point2D (&points)[4],
		BYTE value)
	{
		if (clip.Width <= 0 || clip.Height <= 0 || value == 0)
			return false;

		float minY = static_cast<float>(points[0].Y);
		float maxY = minY;

		for (int i = 1; i < 4; ++i)
		{
			minY = std::min(minY, static_cast<float>(points[i].Y));
			maxY = std::max(maxY, static_cast<float>(points[i].Y));
		}

		const int firstY = std::max(clip.Y, static_cast<int>(std::ceil(minY)));
		const int lastY = std::min(clip.Y + clip.Height - 1, static_cast<int>(std::floor(maxY)));

		if (firstY > lastY)
			return false;

		bool written = false;

		for (int y = firstY; y <= lastY; ++y)
		{
			const float scanY = static_cast<float>(y) + 0.5f;
			float intersections[4];
			int count = 0;

			for (int i = 0; i < 4; ++i)
			{
				const Point2D& a = points[i];
				const Point2D& b = points[(i + 1) & 3];
				const float ay = static_cast<float>(a.Y);
				const float by = static_cast<float>(b.Y);

				if ((ay <= scanY && by > scanY) || (by <= scanY && ay > scanY))
				{
					const float ax = static_cast<float>(a.X);
					const float bx = static_cast<float>(b.X);
					intersections[count++] = ax + (scanY - ay) * (bx - ax) / (by - ay);
				}
			}

			if (count < 2)
				continue;

			std::sort(intersections, intersections + count);

			for (int i = 0; i + 1 < count; i += 2)
			{
				const int firstX = std::max(clip.X, static_cast<int>(std::ceil(intersections[i])));
				const int lastX = std::min(clip.X + clip.Width - 1,
					static_cast<int>(std::floor(intersections[i + 1])));

				if (firstX > lastX)
					continue;

				const int row = (y - clip.Y) * clip.Width;

				for (int x = firstX; x <= lastX; ++x)
				{
					BYTE& pixel = mask[row + x - clip.X];

					if (value > pixel)
						pixel = value;
				}

				written = true;
			}
		}

		return written;
	}

	static bool ProjectBuildRangeCell(int x, int y, Point2D (&points)[4])
	{
		// The cells come from the range grid and are already validated against
		// the map, so no MapRect-based clipping here (MapRect can be remapped
		// by large-map extensions and would otherwise discard every cell).
		const CellStruct corners[4] = {
			{ static_cast<short>(x), static_cast<short>(y) },
			{ static_cast<short>(x + 1), static_cast<short>(y) },
			{ static_cast<short>(x + 1), static_cast<short>(y + 1) },
			{ static_cast<short>(x), static_cast<short>(y + 1) }
		};

		for (int i = 0; i < 4; ++i)
			points[i] = TacticalClass::Instance->CoordsToClient(CellCornerToWorldFromCorner(corners[i])).first;

		return true;
	}

	ColorStruct GetBuildRangeColor(BuildRangeCellType type)
	{
		const auto pRules = RulesExt::Global();

		switch (type)
		{
		case BuildRangeCellType::Legal:
			return pRules->BuildRange_LegalColor;
		case BuildRangeCellType::Blocked:
			return pRules->BuildRange_BlockedColor;
		case BuildRangeCellType::Occupied:
			return pRules->BuildRange_OccupiedColor;
		default:
			return ColorStruct { 0, 0, 0 };
		}
	}

	static bool ShouldLogBuildRangeDiag()
	{
		static unsigned int lastFrame = 0;
		const unsigned int frame = Unsorted::CurrentFrame;

		if (frame - lastFrame < 120)
			return false;

		lastFrame = frame;
		return true;
	}

	void DrawBuildRangeFill(const BuildRangeData& data)
	{
		const auto pRules = RulesExt::Global();

		if (!pRules->BuildRange_Fill || data.Width <= 0 || data.Height <= 0)
			return;

		const int opacity = std::clamp(static_cast<int>(pRules->BuildRange_FillOpacity), 0, 100);

		if (opacity <= 0)
			return;

		auto const pSurface = DSurface::Temp;

		if (!pSurface || !TacticalClass::Instance)
			return;

		const RectangleStruct rect = pSurface->GetRect();
		const RectangleStruct clip = Drawing::Intersect(DSurface::ViewBounds, rect);

		if (clip.Width <= 0 || clip.Height <= 0)
			return;

		const Point2D tacticalPos = TacticalClass::Instance->TacticalPos;
		const double zoom = ZoomManager::CurrentZoom;
		const RectangleStruct view = DSurface::ViewBounds;

		const bool sameView =
			CachedBuildRangeFill.Valid
			&& CachedBuildRangeFill.TacticalPos.X == tacticalPos.X
			&& CachedBuildRangeFill.TacticalPos.Y == tacticalPos.Y
			&& CachedBuildRangeFill.Zoom == zoom
			&& CachedBuildRangeFill.View.X == view.X
			&& CachedBuildRangeFill.View.Y == view.Y
			&& CachedBuildRangeFill.View.Width == view.Width
			&& CachedBuildRangeFill.View.Height == view.Height
			&& CachedBuildRangeFill.MaskWidth == clip.Width
			&& CachedBuildRangeFill.MaskHeight == clip.Height;

		bool rebuildRects = false;

		if (!sameView)
		{
			CachedBuildRangeFill.Mask.assign(
				static_cast<size_t>(clip.Width) * clip.Height, 0);
			CachedBuildRangeFill.MaskWidth = clip.Width;
			CachedBuildRangeFill.MaskHeight = clip.Height;

			for (int y = data.MinY; y < data.MinY + data.Height; ++y)
			{
				for (int x = data.MinX; x < data.MinX + data.Width; ++x)
				{
					const BYTE value = static_cast<BYTE>(data.Cells[BuildRangeIndex(data, x, y)]);

					if (value == 0)
						continue;

					Point2D points[4];

					if (ProjectBuildRangeCell(x, y, points))
						RasterizeBuildRangeCell(CachedBuildRangeFill.Mask, clip, points, value);
				}
			}

			rebuildRects = true;
		}
		else if (CachedBuildRangeFill.Revision != CachedBuildRangeRevision
			&& !data.ChangedCells.empty())
		{
			auto& mask = CachedBuildRangeFill.Mask;

			for (auto const index : data.ChangedCells)
			{
				const int x = data.MinX + index % data.Width;
				const int y = data.MinY + index / data.Width;
				Point2D points[4];

				if (!ProjectBuildRangeCell(x, y, points))
					continue;

				int minX = static_cast<int>(points[0].X);
				int maxX = minX;
				int minY = static_cast<int>(points[0].Y);
				int maxY = minY;

				for (int i = 1; i < 4; ++i)
				{
					minX = std::min(minX, static_cast<int>(points[i].X));
					maxX = std::max(maxX, static_cast<int>(points[i].X));
					minY = std::min(minY, static_cast<int>(points[i].Y));
					maxY = std::max(maxY, static_cast<int>(points[i].Y));
				}

				const int clearX0 = std::max(clip.X, minX - 2);
				const int clearX1 = std::min(clip.X + clip.Width - 1, maxX + 2);
				const int clearY0 = std::max(clip.Y, minY - 2);
				const int clearY1 = std::min(clip.Y + clip.Height - 1, maxY + 2);

				for (int cy = clearY0; cy <= clearY1; ++cy)
				{
					BYTE* const row = mask.data()
						+ static_cast<size_t>(cy - clip.Y) * clip.Width;

					for (int cx = clearX0; cx <= clearX1; ++cx)
						row[cx - clip.X] = 0;
				}

				// Re-rasterize the changed cell and its neighbours because
				// projected cell quads can overlap by one pixel.
				for (int dy = -1; dy <= 1; ++dy)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = x + dx;
						const int ny = y + dy;

						if (nx < data.MinX || nx >= data.MinX + data.Width
							|| ny < data.MinY || ny >= data.MinY + data.Height)
						{
							continue;
						}

						const BYTE value = static_cast<BYTE>(
							data.Cells[BuildRangeIndex(data, nx, ny)]);

						if (value == 0)
							continue;

						Point2D neighbourPoints[4];

						if (ProjectBuildRangeCell(nx, ny, neighbourPoints))
						{
							RasterizeBuildRangeCell(
								CachedBuildRangeFill.Mask, clip, neighbourPoints, value);
						}
					}
				}
			}

			rebuildRects = true;
		}

		if (rebuildRects)
		{
			auto const& mask = CachedBuildRangeFill.Mask;
			std::vector<BuildRangeSpan> spans;
			spans.reserve(static_cast<size_t>(clip.Height) * 4);

			for (int y = 0; y < clip.Height; ++y)
			{
				const int row = y * clip.Width;
				int x = 0;

				while (x < clip.Width)
				{
					const BYTE value = mask[row + x];

					if (value == 0)
					{
						++x;
						continue;
					}

					const int firstX = x;

					while (x < clip.Width && mask[row + x] == value)
						++x;

					spans.push_back({
						clip.Y + y,
						clip.X + firstX,
						clip.X + x - 1,
						value
					});
				}
			}

			// Merge horizontal spans vertically into larger rectangles so the
			// engine blitter is called for blocks instead of every screen row.
			using RectKey = std::uint64_t;
			std::unordered_map<RectKey, BuildRangeFillRect> open;

			auto keyFor = [](const BuildRangeSpan& span)
			{
				return (static_cast<RectKey>(static_cast<uint16_t>(span.X0)) << 32)
					| (static_cast<RectKey>(static_cast<uint16_t>(span.X1)) << 16)
					| span.Type;
			};

			CachedBuildRangeFill.Rects.clear();
			CachedBuildRangeFill.Rects.reserve(spans.size() / 4 + 1);

			for (auto const& span : spans)
			{
				const RectKey key = keyFor(span);
				auto it = open.find(key);

				if (it == open.end())
				{
					open.emplace(key, BuildRangeFillRect {
						span.X0,
						span.Y,
						span.X1 - span.X0 + 1,
						1,
						span.Type
					});
				}
				else if (it->second.Y + it->second.Height == span.Y)
				{
					++it->second.Height;
				}
				else
				{
					CachedBuildRangeFill.Rects.push_back(it->second);
					it->second = {
						span.X0,
						span.Y,
						span.X1 - span.X0 + 1,
						1,
						span.Type
					};
				}
			}

			for (auto const& [key, fillRect] : open)
			{
				(void)key;
				CachedBuildRangeFill.Rects.push_back(fillRect);
			}

			CachedBuildRangeFill.Revision = CachedBuildRangeRevision;
			CachedBuildRangeFill.TacticalPos = tacticalPos;
			CachedBuildRangeFill.Zoom = zoom;
			CachedBuildRangeFill.View = view;
			CachedBuildRangeFill.Valid = true;
		}

		CachedBuildRangeFill.Revision = CachedBuildRangeRevision;

		if (CachedBuildRangeFill.Rects.empty())
		{
			if (ShouldLogBuildRangeDiag())
			{
				Debug::Log("[BuildRange] fill skipped: no rects (surface=%p clip=%dx%d mask=%dx%d)\n",
					pSurface, clip.Width, clip.Height,
					CachedBuildRangeFill.MaskWidth, CachedBuildRangeFill.MaskHeight);
			}

			return;
		}

		const ColorStruct colors[4] = {
			ColorStruct { 0, 0, 0 },
			GetBuildRangeColor(BuildRangeCellType::Legal),
			GetBuildRangeColor(BuildRangeCellType::Blocked),
			GetBuildRangeColor(BuildRangeCellType::Occupied)
		};

		if (!pSurface->CanLock())
		{
			if (ShouldLogBuildRangeDiag())
				Debug::Log("[BuildRange] fill skipped: surface %p cannot be locked\n", pSurface);

			return;
		}

		BYTE* const base = static_cast<BYTE*>(pSurface->Lock(0, 0));

		if (!base)
		{
			if (ShouldLogBuildRangeDiag())
				Debug::Log("[BuildRange] fill skipped: Lock(%p) returned null\n", pSurface);

			return;
		}

		const int pitch = pSurface->GetPitch();
		const int bpp = pSurface->GetBytesPerPixel();

		if (bpp != 2 && bpp != 4)
		{
			if (ShouldLogBuildRangeDiag())
				Debug::Log("[BuildRange] fill skipped: unsupported bpp=%d\n", bpp);

			pSurface->Unlock();
			return;
		}

		const int inverseOpacity = 100 - opacity;

		if (bpp == 2
			&& (!CachedBuildRangeLut.Valid
				|| CachedBuildRangeLut.Opacity != opacity
				|| CachedBuildRangeLut.Colors[1] != colors[1]
				|| CachedBuildRangeLut.Colors[2] != colors[2]
				|| CachedBuildRangeLut.Colors[3] != colors[3]))
		{
			for (int type = 1; type < 4; ++type)
			{
				const auto& color = colors[type];
				const int srcR = (static_cast<int>(color.R) * 31 + 127) / 255;
				const int srcG = (static_cast<int>(color.G) * 63 + 127) / 255;
				const int srcB = (static_cast<int>(color.B) * 31 + 127) / 255;

				// Blend in the destination's own 5/6/5 channel space. Rounding
				// (instead of truncating after an 8-bit round trip) keeps the
				// rendered colour faithful, and nudging by one step when the
				// rounded result equals the source keeps low opacities visible
				// instead of silently doing nothing.
				auto blend = [opacity, inverseOpacity](int src, int dst)
				{
					const int out = (src * opacity + dst * inverseOpacity + 50) / 100;

					if (opacity > 0 && src != dst && out == dst)
						return dst + (src > dst ? 1 : -1);

					return out;
				};

				for (int value = 0; value < 65536; ++value)
				{
					const int r = blend(srcR, (value >> 11) & 0x1F);
					const int g = blend(srcG, (value >> 5) & 0x3F);
					const int b = blend(srcB, value & 0x1F);

					CachedBuildRangeLut.Lut[type][value] = static_cast<WORD>(
						(r << 11) | (g << 5) | b);
				}

				CachedBuildRangeLut.Colors[type] = color;
			}

			CachedBuildRangeLut.Opacity = opacity;
			CachedBuildRangeLut.Valid = true;
		}

		for (auto const& fillRect : CachedBuildRangeFill.Rects)
		{
			for (int y = fillRect.Y; y < fillRect.Y + fillRect.Height; ++y)
			{
				BYTE* const row = base + static_cast<size_t>(y) * pitch;

				for (int x = fillRect.X; x < fillRect.X + fillRect.Width; ++x)
				{
					BYTE* const pixel = row + static_cast<size_t>(x) * bpp;

					if (bpp == 2)
					{
						WORD& destination = *reinterpret_cast<WORD*>(pixel);
						destination = CachedBuildRangeLut.Lut[fillRect.Type][destination];
					}
					else
					{
						DWORD& destination = *reinterpret_cast<DWORD*>(pixel);
						const int dr = static_cast<int>((destination >> 16) & 0xFF);
						const int dg = static_cast<int>((destination >> 8) & 0xFF);
						const int db = static_cast<int>(destination & 0xFF);
						const auto& color = colors[fillRect.Type];

						const int r = (static_cast<int>(color.R) * opacity + dr * inverseOpacity) / 100;
						const int g = (static_cast<int>(color.G) * opacity + dg * inverseOpacity) / 100;
						const int b = (static_cast<int>(color.B) * opacity + db * inverseOpacity) / 100;

						destination = (destination & 0xFF000000u)
							| (static_cast<DWORD>(r) << 16)
							| (static_cast<DWORD>(g) << 8)
							| static_cast<DWORD>(b);
					}
				}
			}
		}

		pSurface->Unlock();
	}

	void DrawBuildRangeOutline(const BuildRangeData& data)
	{
		auto const pSurface = DSurface::Temp;

		if (!pSurface || !TacticalClass::Instance)
			return;

		const Point2D tacticalPos = TacticalClass::Instance->TacticalPos;
		const double zoom = ZoomManager::CurrentZoom;
		RectangleStruct view = DSurface::ViewBounds;

		const bool sameView =
			CachedBuildRangeOutline.Valid
			&& CachedBuildRangeOutline.Revision == CachedBuildRangeEdgeRevision
			&& CachedBuildRangeOutline.TacticalPos.X == tacticalPos.X
			&& CachedBuildRangeOutline.TacticalPos.Y == tacticalPos.Y
			&& CachedBuildRangeOutline.Zoom == zoom
			&& CachedBuildRangeOutline.View.X == view.X
			&& CachedBuildRangeOutline.View.Y == view.Y
			&& CachedBuildRangeOutline.View.Width == view.Width
			&& CachedBuildRangeOutline.View.Height == view.Height;

		if (!sameView)
		{
			CachedBuildRangeOutline.Edges.clear();
			CachedBuildRangeOutline.Edges.reserve(data.Edges.size());

			for (auto const& edge : data.Edges)
			{
				// Edges are generated from the green cells in the range grid and
				// are already inside the map; no MapRect-based clip here.
				BuildRangeEdge clipped = edge;

				const CoordStruct worldA = CellCornerToWorld(clipped.A, clipped.Z);
				const CoordStruct worldB = CellCornerToWorld(clipped.B, clipped.Z);
				auto const [screenA, visibleA] = TacticalClass::Instance->CoordsToClient(worldA);
				auto const [screenB, visibleB] = TacticalClass::Instance->CoordsToClient(worldB);

				if (!visibleA && !visibleB)
					continue;

				Point2D start = screenA;
				Point2D end = screenB;

				if (!Line_In_Bounds(&start, &end, &view))
					continue;

				CachedBuildRangeOutline.Edges.push_back({ start, end });
			}

			CachedBuildRangeOutline.Revision = CachedBuildRangeEdgeRevision;
			CachedBuildRangeOutline.TacticalPos = tacticalPos;
			CachedBuildRangeOutline.Zoom = zoom;
			CachedBuildRangeOutline.View = view;
			CachedBuildRangeOutline.Valid = true;
		}

		const int color = Drawing::RGB_To_Int(255, 255, 255);

		for (auto const& edge : CachedBuildRangeOutline.Edges)
		{
			Point2D start = edge.A;
			Point2D end = edge.B;
			pSurface->DrawDashed(&start, &end, color, 0);
		}
	}

	void DrawBuildRange()
	{
		auto const pDisplay = &DisplayClass::Instance;
		auto const pBuilding = specific_cast<BuildingClass*>(pDisplay->CurrentBuilding);

		// The engine draws its placement grid from CurrentBuildingType and
		// CurrentBuildingOwnerArrayIndex alone. The preview BuildingClass is
		// optional in some mods, so mirror the engine's state instead of
		// requiring the preview object here.
		auto const pType = pBuilding && pBuilding->Type
			? pBuilding->Type
			: specific_cast<BuildingTypeClass*>(pDisplay->CurrentBuildingType);

		if (!pType || !pDisplay->CurrentFoundation_Data)
			return;

		HouseClass* pOwner = pBuilding ? pBuilding->Owner : nullptr;

		if (!pOwner)
		{
			const int ownerIndex = pDisplay->CurrentBuildingOwnerArrayIndex;

			if (ownerIndex >= 0 && ownerIndex < HouseClass::Array.Count)
				pOwner = HouseClass::Array.GetItem(ownerIndex);
		}

		if (!pOwner)
			return;

		const unsigned int frame = Unsorted::CurrentFrame;
		const auto pRules = RulesExt::Global();
		const int fillOpacity = std::clamp(static_cast<int>(pRules->BuildRange_FillOpacity), 0, 100);
		const ColorStruct legalColor = pRules->BuildRange_LegalColor;
		const unsigned int refreshFrames = static_cast<unsigned int>(
			std::clamp(static_cast<int>(pRules->BuildRange_RefreshFrames), 1, 60));
		const std::uint64_t foundationHash = HashBuildRangeFoundation(
			DisplayClass::Instance.CurrentFoundation_Data);

		bool rebuildStatic = !CachedBuildRangeType
			|| CachedBuildRangeType != pType
			|| CachedBuildRangeOwner != pOwner
			|| CachedBuildRangeFoundationHash != foundationHash
			|| CachedBuildRangeData.Cells.empty();

		if (!rebuildStatic && frame - CachedBuildRangeFrame >= refreshFrames)
		{
			const int buildingCount = BuildingClass::Array.Count;
			const bool checkBase = CachedBuildRangeBaseBuildingCount != buildingCount
				|| frame - CachedBuildRangeBaseCheckFrame >= refreshFrames * 4;

			if (checkBase)
			{
				const std::uint64_t baseSignature = HashBuildRangeBaseSignature(pOwner);
				CachedBuildRangeBaseBuildingCount = buildingCount;
				CachedBuildRangeBaseCheckFrame = frame;

				if (baseSignature != CachedBuildRangeBaseSignature)
					rebuildStatic = true;
			}

			if (!rebuildStatic
				&& RefreshBuildRangeData(
					pType, pOwner, CachedBuildRangeStatic, CachedBuildRangeData))
			{
				++CachedBuildRangeRevision;
			}

			CachedBuildRangeFrame = frame;
		}

		if (rebuildStatic)
		{
			BuildRangeData newData = BuildBuildRangeData(
				pType, pOwner, CachedBuildRangeStatic);
			CachedBuildRangeData = std::move(newData);
			CachedBuildRangeType = pType;
			CachedBuildRangeOwner = pOwner;
			CachedBuildRangeFoundationHash = foundationHash;
			CachedBuildRangeBaseSignature = HashBuildRangeBaseSignature(pOwner);
			CachedBuildRangeBaseBuildingCount = BuildingClass::Array.Count;
			CachedBuildRangeBaseCheckFrame = frame;
			CachedBuildRangeFrame = frame;
			CachedBuildRangeFill.Valid = false;
			CachedBuildRangeOutline.Valid = false;
			++CachedBuildRangeRevision;
			++CachedBuildRangeEdgeRevision;

			// Diagnostic: emitted when the range is (re)built, throttled so an
			// empty result cannot flood the log. If the overlay still does not
			// show, this line tells whether the preview object was missing or
			// whether the generated range contained no cells.
			static unsigned int lastDiagFrame = 0;

			if (frame - lastDiagFrame >= 120)
			{
				lastDiagFrame = frame;

				int legal = 0;
				int blocked = 0;
				int occupied = 0;

				for (auto const cell : CachedBuildRangeData.Cells)
				{
					switch (cell)
					{
					case BuildRangeCellType::Legal: ++legal; break;
					case BuildRangeCellType::Blocked: ++blocked; break;
					case BuildRangeCellType::Occupied: ++occupied; break;
					default: break;
					}
				}

				Debug::Log("[BuildRange] type=%s preview=%p owner=%p idx=%d temp=%p view=%d,%d,%dx%d maprect=%d,%d,%dx%d range=%dx%d cells=%d legal=%d blocked=%d occupied=%d edges=%d fill=%d opacity=%d color=%d,%d,%d\n",
					pType->ID,
					pBuilding, CachedBuildRangeOwner, pDisplay->CurrentBuildingOwnerArrayIndex,
					static_cast<void*>(DSurface::Temp),
					DSurface::ViewBounds.X, DSurface::ViewBounds.Y,
					DSurface::ViewBounds.Width, DSurface::ViewBounds.Height,
					MapClass::Instance.MapRect.X, MapClass::Instance.MapRect.Y,
					MapClass::Instance.MapRect.Width, MapClass::Instance.MapRect.Height,
					CachedBuildRangeData.Width, CachedBuildRangeData.Height,
					static_cast<int>(CachedBuildRangeData.Cells.size()),
					legal, blocked, occupied,
					static_cast<int>(CachedBuildRangeData.Edges.size()),
					static_cast<int>(pRules->BuildRange_Fill),
					fillOpacity, legalColor.R, legalColor.G, legalColor.B);
			}
		}

		DrawBuildRangeFill(CachedBuildRangeData);
		DrawBuildRangeOutline(CachedBuildRangeData);
	}
}

DEFINE_HOOK(0x6D528A, TacticalClass_DrawPlacement_PlacementPreview, 0x6)
{
	DrawBuildRange();

	if (!RulesExt::Global()->PlacementPreview || !Phobos::Config::ShowPlacementPreview)
		return 0;

	const auto pBuilding = specific_cast<BuildingClass*>(DisplayClass::Instance.CurrentBuilding);
	const auto pType = pBuilding ? pBuilding->Type : nullptr;
	const auto pTypeExt = BuildingTypeExt::ExtMap.TryFind(pType);
	const bool isShow = pTypeExt && pTypeExt->PlacementPreview;

	if (isShow)
	{
		CellClass* pCell = nullptr;
		{
			const CellStruct nDisplayCell = Make_Global<CellStruct>(0x88095C);
			const CellStruct nDisplayCell_Offset = Make_Global<CellStruct>(0x880960);

			pCell = MapClass::Instance.TryGetCellAt(nDisplayCell + nDisplayCell_Offset);
			if (!pCell)
				return 0;
		}

		int nImageFrame = 0;
		SHPStruct* pImage = pTypeExt->PlacementPreview_Shape.GetSHP();
		{
			if (!pImage)
			{
				pImage = pType->LoadBuildup();
				if (pImage)
					nImageFrame = ((pImage->Frames / 2) - 1);
				else
					pImage = pType->GetImage();

				if (!pImage)
					return 0;
			}

			nImageFrame = Math::clamp(pTypeExt->PlacementPreview_ShapeFrame.Get(nImageFrame), 0, (int)pImage->Frames);
		}


		Point2D point;
		{
			const CoordStruct offset = pTypeExt->PlacementPreview_Offset;
			const int nHeight = offset.Z + pCell->GetFloorHeight({ 0, 0 });
			const CoordStruct coords = CellClass::Cell2Coord(pCell->MapCoords, nHeight);

			point = TacticalClass::Instance->CoordsToClient(coords).first;
			point.X += offset.X;
			point.Y += offset.Y;
		}

		const BlitterFlags blitFlags = pTypeExt->PlacementPreview_Translucency.Get(RulesExt::Global()->PlacementPreview_Translucency) |
			BlitterFlags::Centered | BlitterFlags::Nonzero | BlitterFlags::MultiPass;

		ConvertClass* pPalette = pTypeExt->PlacementPreview_Remap.Get()
			? pBuilding->GetDrawer()
			: pTypeExt->PlacementPreview_Palette.GetOrDefaultConvert(FileSystem::UNITx_PAL);

		DSurface* pSurface = DSurface::Temp;
		RectangleStruct rect = pSurface->GetRect();
		rect.Height -= 32; // account for bottom bar

		CC_Draw_Shape(pSurface, pPalette, pImage, nImageFrame, &point, &rect, blitFlags,
			0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
	}

	return 0;
}

DEFINE_HOOK(0x47EFAE, CellClass_Draw_It_SetPlacementGridTranslucency, 0x6)
{
	const BlitterFlags translucency = (RulesExt::Global()->PlacementPreview && Phobos::Config::ShowPlacementPreview)
		? RulesExt::Global()->PlacementGrid_TranslucencyWithPreview.Get(RulesExt::Global()->PlacementGrid_Translucency)
		: RulesExt::Global()->PlacementGrid_Translucency;

	if (translucency != BlitterFlags::None)
	{
		LEA_STACK(BlitterFlags*, blitFlags, STACK_OFFSET(0x68, -0x58));
		*blitFlags |= translucency;
	}

	return 0;
}

// Rewritten
DEFINE_HOOK(0x465D40, BuildingTypeClass_IsVehicle, 0x6)
{
	enum { ReturnFromFunction = 0x465D6A };

	GET(BuildingTypeClass*, pThis, ECX);

	const auto pExt = BuildingTypeExt::ExtMap.Find(pThis);

	if (pExt->ConsideredVehicle.isset())
	{
		R->EAX(pExt->ConsideredVehicle.Get());
		return ReturnFromFunction;
	}

	return 0;
}

DEFINE_HOOK(0x5F5416, ObjectClass_ReceiveDamage_CanC4DamageRounding, 0x6)
{
	enum { SkipGameCode = 0x5F5456 };

	GET(ObjectClass*, pThis, ESI);
	GET(int*, pDamage, EDI);

	if (*pDamage == 0 && pThis->WhatAmI() == AbstractType::Building)
	{
		auto const pType = static_cast<BuildingClass*>(pThis)->Type;

		if (!pType->CanC4)
		{
			auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pType);

			if (!pTypeExt->CanC4_AllowZeroDamage)
				*pDamage = 1;
		}
	}

	return SkipGameCode;
}

#pragma region BuildingProximity

namespace ProximityTemp
{
	BuildingTypeClass* pType = nullptr;
}

DEFINE_HOOK(0x4A8F20, DisplayClass_BuildingProximityCheck_SetContext, 0x5)
{
	GET(BuildingTypeClass*, pType, ESI);

	ProximityTemp::pType = pType;

	return 0;
}

DEFINE_HOOK(0x4A8FD7, DisplayClass_BuildingProximityCheck_BuildArea, 0x6)
{
	enum { SkipBuilding = 0x4A902C };

	GET(BuildingClass*, pCellBuilding, ESI);

	auto const pTypeExt = BuildingTypeExt::ExtMap.Find(pCellBuilding->Type);

	if (pTypeExt->NoBuildAreaOnBuildup && pCellBuilding->CurrentMission == Mission::Construction)
		return SkipBuilding;

	auto const pTmpTypeExt = BuildingTypeExt::ExtMap.Find(ProximityTemp::pType);
	auto const& pBuildingsAllowed = pTmpTypeExt->Adjacent_Allowed;

	if (pBuildingsAllowed.size() > 0 && !pBuildingsAllowed.Contains(pCellBuilding->Type))
		return SkipBuilding;

	auto const& pBuildingsDisallowed = pTmpTypeExt->Adjacent_Disallowed;

	if (pBuildingsDisallowed.size() > 0 && pBuildingsDisallowed.Contains(pCellBuilding->Type))
		return SkipBuilding;

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x6FE3F1, TechnoClass_FireAt_OccupyDamageBonus, 0xB)
{
	enum { ApplyDamageBonus = 0x6FE405 };

	GET(TechnoClass* const, pThis, ESI);

	if (const auto Building = specific_cast<BuildingClass*>(pThis))
	{
		GET_STACK(const int, damage, STACK_OFFSET(0xC8, -0x9C));
		R->EAX(GeneralUtils::SafeDoubleToInt(static_cast<double>(damage) * BuildingTypeExt::ExtMap.Find(Building->Type)->BuildingOccupyDamageMult.Get(RulesClass::Instance->OccupyDamageMultiplier)));
		return ApplyDamageBonus;
	}

	return 0;
}

DEFINE_HOOK(0x6FE421, TechnoClass_FireAt_BunkerDamageBonus, 0xB)
{
	enum { ApplyDamageBonus = 0x6FE435 };

	GET(TechnoClass* const, pThis, ESI);

	if (const auto Building = specific_cast<BuildingClass*>(pThis->BunkerLinkedItem))
	{
		GET_STACK(const int, damage, STACK_OFFSET(0xC8, -0x9C));
		R->EAX(GeneralUtils::SafeDoubleToInt(static_cast<double>(damage) * BuildingTypeExt::ExtMap.Find(Building->Type)->BuildingBunkerDamageMult.Get(RulesClass::Instance->OccupyDamageMultiplier)));
		return ApplyDamageBonus;
	}

	return 0;
}

DEFINE_HOOK(0x6FD183, TechnoClass_RearmDelay_BuildingOccupyROFMult, 0xC)
{
	enum { ApplyRofMod = 0x6FD1AB, SkipRofMod = 0x6FD1B1 };

	GET(TechnoClass*, pThis, ESI);

	if (const auto Building = specific_cast<BuildingClass*>(pThis))
	{
		const auto multiplier = BuildingTypeExt::ExtMap.Find(Building->Type)->BuildingOccupyROFMult.Get(RulesClass::Instance->OccupyROFMultiplier);

		if (multiplier > 0.0f)
		{
			GET_STACK(const int, rof, STACK_OFFSET(0x10, 0x4));
			R->EAX(Game::F2I(static_cast<double>(rof) / multiplier));
			return ApplyRofMod;
		}

		return SkipRofMod;
	}

	return 0;
}

DEFINE_HOOK(0x6FD1C7, TechnoClass_RearmDelay_BuildingBunkerROFMult, 0xC)
{
	enum { ApplyRofMod = 0x6FD1EF, SkipRofMod = 0x6FD1F1 };

	GET(TechnoClass*, pThis, ESI);

	if (const auto Building = specific_cast<BuildingClass*>(pThis->BunkerLinkedItem))
	{
		const auto multiplier = BuildingTypeExt::ExtMap.Find(Building->Type)->BuildingBunkerROFMult.Get(RulesClass::Instance->BunkerROFMultiplier);

		if (multiplier > 0.0f)
		{
			GET_STACK(const int, rof, STACK_OFFSET(0x10, 0x4));
			R->EAX(Game::F2I(static_cast<double>(rof) / multiplier));
			return ApplyRofMod;
		}

		return SkipRofMod;
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x45933D, BuildingClass_BunkerSound, 0x5)
DEFINE_HOOK_AGAIN(0x4595D9, BuildingClass_BunkerSound, 0x5)
DEFINE_HOOK(0x459494, BuildingClass_BunkerSound, 0x5)
{
	enum
	{
		BunkerWallUpSound = 0x45933D,
		BunkerWallUpSound_Handled_ret = 0x459374,

		BunkerWallDownSound_01 = 0x4595D9,
		BunkerWallDownSound_01_Handled_ret = 0x459612,

		BunkerWallDownSound_02 = 0x459494,
		BunkerWallDownSound_02_Handled_ret = 0x4594CD
	};

	BuildingClass const* pThis = R->Origin() == BunkerWallDownSound_01
		? R->EDI<BuildingClass*>() : R->ESI<BuildingClass*>();

	BuildingTypeExt::PlayBunkerSound(pThis, R->Origin() == BunkerWallUpSound);

	switch (R->Origin())
	{
	case BunkerWallUpSound:
		return BunkerWallUpSound_Handled_ret;
	case BunkerWallDownSound_01:
		return BunkerWallDownSound_01_Handled_ret;
	case BunkerWallDownSound_02:
		return BunkerWallDownSound_02_Handled_ret;
	default:
		__assume(0); // Just avoiding compilation warnings, shouldn't go this way
	}
}

DEFINE_HOOK_AGAIN(0x4426DB, BuildingClass_ReceiveDamage_DisableDamageSound, 0x8)
DEFINE_HOOK_AGAIN(0x702777, BuildingClass_ReceiveDamage_DisableDamageSound, 0x8)
DEFINE_HOOK(0x70272E, BuildingClass_ReceiveDamage_DisableDamageSound, 0x8)
{
	enum
	{
		BuildingClass_TakeDamage_DamageSound = 0x4426DB,
		BuildingClass_TakeDamage_DamageSound_Handled_ret = 0x44270B,

		TechnoClass_TakeDamage_Building_DamageSound_01 = 0x702777,
		TechnoClass_TakeDamage_Building_DamageSound_01_Handled_ret = 0x7027AE,

		TechnoClass_TakeDamage_Building_DamageSound_02 = 0x70272E,
		TechnoClass_TakeDamage_Building_DamageSound_02_Handled_ret = 0x702765,
	};

	GET(TechnoClass*, pThis, ESI);

	if (auto const pBuilding = specific_cast<BuildingClass*>(pThis))
	{
		if (BuildingTypeExt::ExtMap.Find(pBuilding->Type)->DisableDamageSound)
		{
			switch (R->Origin())
			{
			case BuildingClass_TakeDamage_DamageSound:
				return BuildingClass_TakeDamage_DamageSound_Handled_ret;
			case TechnoClass_TakeDamage_Building_DamageSound_01:
				return TechnoClass_TakeDamage_Building_DamageSound_01_Handled_ret;
			case TechnoClass_TakeDamage_Building_DamageSound_02:
				return TechnoClass_TakeDamage_Building_DamageSound_02_Handled_ret;
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x44E85F, BuildingClass_Power_DamageFactor, 0x7)
{
	enum { Handled = 0x44E86F };

	GET(BuildingClass*, pThis, ESI);
	GET_STACK(const int, powerMultiplier, STACK_OFFSET(0xC, -0x4));

	const double factor = BuildingTypeExt::ExtMap.Find(pThis->Type)->PowerPlant_DamageFactor;

	if (factor == 1.0)
		R->EAX(Game::F2I(powerMultiplier * pThis->GetHealthPercentage()));
	else if (factor == 0.0)
		R->EAX(powerMultiplier);
	else
		R->EAX(Math::max(Game::F2I(powerMultiplier * (1.0 - factor + factor * pThis->GetHealthPercentage())), 0));

	return Handled;
}

#pragma region WeaponFactoryPath

DEFINE_HOOK(0x73F5A7, UnitClass_IsCellOccupied_UnlimboDirection, 0x8)
{
	enum { NextObject = 0x73FA87, ContinueCheck = 0x73F5AF };

	GET(const bool, notPassable, EAX);

	if (notPassable)
		return ContinueCheck;

	GET(BuildingClass* const, pBuilding, ESI);

	const auto pType = pBuilding->Type;

	if (!pType->WeaponsFactory)
		return NextObject;

	GET(CellClass* const, pCell, EDI);

	return pCell->MapCoords.Y == pBuilding->Location.Y / Unsorted::LeptonsPerCell + pType->FoundationOutside[10].Y ? NextObject : ContinueCheck;
}

#pragma endregion
