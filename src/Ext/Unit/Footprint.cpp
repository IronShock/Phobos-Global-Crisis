#include "Footprint.h"

#include <BuildingClass.h>
#include <InfantryClass.h>
#include <MapClass.h>
#include <OverlayTypeClass.h>
#include <TerrainClass.h>
#include <Unsorted.h>

#include <Ext/Cell/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Locomotion/FootprintLocomotionClass.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <set>
#include <unordered_map>

namespace
{
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kDegToRad = kPi / 180.0;
	constexpr int kMapSide = 512;
	constexpr int kMaxCells = MapClass::MaxCells;

	std::vector<UnitClass*> s_ownerNormal;
	std::vector<UnitClass*> s_ownerAlt;
	bool s_gridReady = false;

	std::unordered_map<TechnoTypeClass*, FootprintGeometry> s_geometries;

	void EnsureGrid()
	{
		if (s_gridReady)
			return;

		s_ownerNormal.assign(kMaxCells, nullptr);
		s_ownerAlt.assign(kMaxCells, nullptr);
		s_gridReady = true;
	}

	bool IsValidCell(const CellStruct& cell)
	{
		return cell.X >= 0 && cell.Y >= 0 && cell.X < kMapSide && cell.Y < kMapSide;
	}

	int CellIndex(const CellStruct& cell)
	{
		if (!IsValidCell(cell))
			return -1;

		const int idx = MapClass::GetCellIndex(cell);
		return (idx >= 0 && idx < kMaxCells) ? idx : -1;
	}

	std::vector<CellStruct> MakeSpanAtAngle(const FootprintConfig& config, double angleDeg, double margin)
	{
		std::vector<CellStruct> result;

		const double angle = angleDeg * kDegToRad;
		const double forwardX = std::sin(angle);
		const double forwardY = -std::cos(angle);
		const double lateralX = std::cos(angle);
		const double lateralY = std::sin(angle);

		const double halfLength = config.Length * 0.5;
		const double halfWidth = config.Width * 0.5;
		const double radius = std::sqrt(halfLength * halfLength + halfWidth * halfWidth) + margin + 1.0;
		const int range = static_cast<int>(std::ceil(radius));

		for (int dy = -range; dy <= range; ++dy)
		{
			for (int dx = -range; dx <= range; ++dx)
			{
				const double forward = dx * forwardX + dy * forwardY - config.PivotForward;
				const double lateral = dx * lateralX + dy * lateralY - config.PivotLateral;

				if (std::abs(forward) <= halfLength + margin + 1.0e-9
					&& std::abs(lateral) <= halfWidth + margin + 1.0e-9)
				{
					result.push_back(CellStruct { static_cast<short>(dx), static_cast<short>(dy) });
				}
			}
		}

		return result;
	}

	void LogBlockedOccupy(UnitClass* pUnit, CellClass* pCell, const CellStruct& cell,
		const char* reason, int level, bool bridge, bool alt)
	{
		static int lastFrame = -100;
		static int count = 0;

		if (Unsorted::CurrentFrame != lastFrame)
		{
			lastFrame = Unsorted::CurrentFrame;
			count = 0;
		}

		if (count++ >= 8)
			return;

		const auto pType = pUnit ? pUnit->GetTechnoType() : nullptr;
		const auto pUnitContent = pCell ? pCell->GetUnit(alt) : nullptr;
		const auto pBuilding = pCell ? pCell->GetBuilding() : nullptr;
		const auto pTerrain = pCell ? pCell->GetTerrain(false) : nullptr;

		Debug::Log("[FootprintBlock] %s cell=(%d,%d) reason=%s"
			" type(SpeedType=%d MoveZone=%d) anchorLevel=%d/%d"
			" cell(level=%d bridge=%d land=%d pass=%d flags=%08X/%08X)"
			" unit=%s building=%s terrain=%s\n",
			pType ? pType->ID : "?",
			cell.X, cell.Y, reason,
			pType ? static_cast<int>(pType->SpeedType) : -1,
			pType ? static_cast<int>(pType->MovementZone) : -1,
			level, bridge ? 1 : 0,
			pCell ? pCell->GetLevel() : -1,
			pCell && pCell->ContainsBridge() ? 1 : 0,
			pCell ? static_cast<int>(pCell->LandType) : -1,
			pCell ? static_cast<int>(pCell->Passability) : -1,
			pCell ? pCell->OccupationFlags : 0,
			pCell ? pCell->AltOccupationFlags : 0,
			pUnitContent ? pUnitContent->GetTechnoType()->ID : "-",
			pBuilding ? pBuilding->Type->ID : "-",
			pTerrain ? pTerrain->Type->ID : "-");
	}

	std::vector<CellStruct> MakeSweepArc(const FootprintConfig& config, double fromDeg, double toDeg, bool clockwise)
	{		std::set<std::pair<int, int>> cells;

		double end = toDeg;

		if (clockwise)
		{
			while (end <= fromDeg)
				end += 360.0;
		}
		else
		{
			while (end >= fromDeg)
				end -= 360.0;
		}

		const double step = clockwise ? 1.0 : -1.0;

		for (double angle = fromDeg; clockwise ? angle <= end + 1.0e-9 : angle >= end - 1.0e-9; angle += step)
		{
			for (const auto& cell : MakeSpanAtAngle(config, angle, 0.0))
				cells.emplace(cell.X, cell.Y);
		}

		if (cells.empty())
			return {};

		// Use the bounding box of the exact sweep. For a 3x1 this turns the
		// seven-cell star of a 90-degree pivot into the intuitive 3x3 turning
		// square, while avoiding the 5x5 over-reservation a half-cell margin
		// would produce.
		int minX = cells.begin()->first;
		int maxX = minX;
		int minY = cells.begin()->second;
		int maxY = minY;

		for (const auto& cell : cells)
		{
			minX = std::min(minX, cell.first);
			maxX = std::max(maxX, cell.first);
			minY = std::min(minY, cell.second);
			maxY = std::max(maxY, cell.second);
		}

		std::vector<CellStruct> result;
		result.reserve(static_cast<size_t>((maxX - minX + 1) * (maxY - minY + 1)));

		for (int y = minY; y <= maxY; ++y)
		{
			for (int x = minX; x <= maxX; ++x)
				result.push_back(CellStruct { static_cast<short>(x), static_cast<short>(y) });
		}

		return result;
	}
}

void FootprintGeometry::Build(const FootprintConfig& config)
{
	if (Built)
		return;

	const auto primary = MakeSpanAtAngle(config, 0.0, 0.0);

	for (int bucket = 0; bucket < 8; ++bucket)
	{
		Span[bucket] = config.Rotates
			? MakeSpanAtAngle(config, bucket * 45.0, 0.0)
			: primary;

		CellStruct min { 0, 0 };
		CellStruct max { 0, 0 };

		for (const auto& cell : Span[bucket])
		{
			min.X = std::min(min.X, cell.X);
			min.Y = std::min(min.Y, cell.Y);
			max.X = std::max(max.X, cell.X);
			max.Y = std::max(max.Y, cell.Y);
		}

		Min[bucket] = min;
		Max[bucket] = max;
	}

	for (int from = 0; from < 8; ++from)
	{
		for (int to = 0; to < 8; ++to)
		{
			const int index = from * 8 + to;

			if (!config.Rotates || from == to)
			{
				SweepCW[index] = Span[from];
				SweepCCW[index] = Span[from];
				continue;
			}

			SweepCW[index] = MakeSweepArc(config, from * 45.0, to * 45.0, true);
			SweepCCW[index] = MakeSweepArc(config, from * 45.0, to * 45.0, false);
		}
	}

	Built = true;
}

bool Footprint::IsMultiCell(TechnoTypeClass* pType)
{
	if (!pType)
		return false;

	const auto pExt = TechnoTypeExt::ExtMap.TryFind(pType);

	if (!pExt || !pExt->MultiCell.Enabled)
		return false;

	return pExt->MultiCell.Length > 1 || pExt->MultiCell.Width > 1;
}

bool Footprint::IsMultiCell(TechnoClass* pThis)
{
	if (!pThis || pThis->WhatAmI() != AbstractType::Unit)
		return false;

	return IsMultiCell(pThis->GetTechnoType());
}

const FootprintConfig& Footprint::GetConfig(TechnoTypeClass* pType)
{
	return TechnoTypeExt::ExtMap.Find(pType)->MultiCell;
}

const FootprintGeometry& Footprint::GetGeometry(TechnoTypeClass* pType)
{
	auto it = s_geometries.find(pType);

	if (it == s_geometries.end())
	{
		FootprintGeometry geometry;
		geometry.Build(GetConfig(pType));
		it = s_geometries.emplace(pType, std::move(geometry)).first;
	}

	return it->second;
}

FootprintState* Footprint::GetState(TechnoClass* pThis)
{
	if (!pThis)
		return nullptr;

	const auto pExt = TechnoExt::ExtMap.TryFind(pThis);
	return pExt ? &pExt->Footprint : nullptr;
}

int Footprint::BucketFromFacing(const DirStruct& facing)
{
	return static_cast<int>(facing.GetValue<3>()) & 7;
}

CellStruct Footprint::AnchorFromCoords(const CoordStruct& coords)
{
	return CellStruct
	{
		static_cast<short>(coords.X / Unsorted::LeptonsPerCell),
		static_cast<short>(coords.Y / Unsorted::LeptonsPerCell)
	};
}

std::vector<CellStruct> Footprint::GetSpan(TechnoClass* pThis, const CellStruct& anchor, int bucket)
{
	std::vector<CellStruct> result;

	if (!pThis)
		return result;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);

	if (!config.Enabled)
	{
		result.push_back(anchor);
		return result;
	}

	const auto& geometry = GetGeometry(pType);
	const int index = config.Rotates ? (bucket & 7) : 0;

	for (const auto& offset : geometry.Span[index])
		result.push_back(anchor + offset);

	return result;
}

std::vector<CellStruct> Footprint::GetSweep(TechnoClass* pThis, int from, int to, bool clockwise)
{
	if (!pThis)
		return {};

	return GetSweepAt(pThis, pThis->GetMapCoords(), from, to, clockwise);
}

std::vector<CellStruct> Footprint::GetSweepAt(TechnoClass* pThis, const CellStruct& anchor, int from, int to, bool clockwise)
{
	std::vector<CellStruct> result;

	if (!pThis)
		return result;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);

	if (!config.Enabled)
		return result;

	if (!config.Rotates || from == to)
		return GetSpan(pThis, anchor, from);

	const auto& geometry = GetGeometry(pType);
	const auto& offsets = clockwise
		? geometry.SweepCW[(from & 7) * 8 + (to & 7)]
		: geometry.SweepCCW[(from & 7) * 8 + (to & 7)];

	for (const auto& offset : offsets)
		result.push_back(anchor + offset);

	return result;
}

UnitClass* Footprint::OwnerAt(const CellStruct& cell, bool alt)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return nullptr;

	return alt ? s_ownerAlt[index] : s_ownerNormal[index];
}

UnitClass* Footprint::OwnerAt(const CellClass* pCell, bool alt)
{
	return pCell ? OwnerAt(pCell->MapCoords, alt) : nullptr;
}

UnitClass* Footprint::FindOwnerByScan(const CellStruct& cell, bool alt)
{
	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
			continue;

		const auto pState = GetState(pUnit);

		if (!pState || !pState->Initialized || pState->Alt != alt)
			continue;

		if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
		{
			SetOwner(cell, alt, pUnit);
			return pUnit;
		}
	}

	return nullptr;
}

void Footprint::SetOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	(alt ? s_ownerAlt : s_ownerNormal)[index] = pOwner;
}

void Footprint::ClearOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	auto& owner = (alt ? s_ownerAlt : s_ownerNormal)[index];

	if (owner == pOwner)
		owner = nullptr;
}

bool Footprint::IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt)
{
	return pThis && pCell && OwnerAt(pCell, alt) == pThis;
}

bool Footprint::CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, CellStruct* pBlocked)
{
	return pThis
		? CanOccupy(pThis, span, alt, pThis->GetMapCoords(), pBlocked)
		: false;
}

bool Footprint::CanOccupy(UnitClass* pThis, const std::vector<CellStruct>& span, bool alt, const CellStruct& anchor, CellStruct* pBlocked)
{
	if (!pThis)
		return false;

	const auto pType = pThis->GetTechnoType();
	const auto& config = GetConfig(pType);
	const auto pAnchorCell = MapClass::Instance.GetCellAt(anchor);
	const int level = pAnchorCell->GetLevel();
	const bool bridge = pAnchorCell->ContainsBridge();

	// [CDEST] relies on inherited MovementZone; if a hull uses Float but inherited
	// Normal/None, treat it as Water instead of rejecting every water cell.
	auto movementZone = pType->MovementZone;

	if (pType->SpeedType == SpeedType::Float
		&& (movementZone == MovementZone::None || movementZone == MovementZone::Normal))
	{
		movementZone = MovementZone::Water;
	}

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
		{
			LogBlockedOccupy(pThis, nullptr, cell, "outside-map", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->GetLevel() != level || pCell->ContainsBridge() != bridge)
		{
			LogBlockedOccupy(pThis, pCell, cell, "level-bridge", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		// Ignore infantry and vehicles here; vehicles are checked below so that
		// the unit's own footprint cells stay passable, and infantry is handled
		// by the crush setting.
		if (!pCell->IsClearToMove(pType->SpeedType, true, true, -1, movementZone, level, bridge))
		{
			LogBlockedOccupy(pThis, pCell, cell, "clear-to-move", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->GetBuilding())
		{
			LogBlockedOccupy(pThis, pCell, cell, "building", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (const auto pTerrain = pCell->GetTerrain(false))
		{
			if (!TerrainTypeExt::ExtMap.Find(pTerrain->Type)->IsPassable)
			{
				LogBlockedOccupy(pThis, pCell, cell, "terrain", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}

		if (const auto pUnit = pCell->GetUnit(alt))
		{
			if (pUnit != pThis)
			{
				LogBlockedOccupy(pThis, pCell, cell, "unit", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}
		else if ((alt ? pCell->AltOccupationFlags : pCell->OccupationFlags) & 0x20)
		{
			// A vehicle bit without a matching object is either a stale
			// footprint or a vanilla unit whose cell lookup is unavailable;
			// either way it must not be treated as free.
			LogBlockedOccupy(pThis, pCell, cell, "stale-flag", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (!config.Crush && pCell->GetInfantry(false))
		{
			LogBlockedOccupy(pThis, pCell, cell, "infantry", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->OverlayTypeIndex >= 0)
		{
			const auto pOverlay = OverlayTypeClass::Array.Items[pCell->OverlayTypeIndex];

			if (pOverlay && pOverlay->Wall)
			{
				LogBlockedOccupy(pThis, pCell, cell, "wall", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
		}
	}

	return true;
}

bool Footprint::Mark(UnitClass* pThis, const CoordStruct& coords)
{
	if (!pThis || !IsMultiCell(pThis))
		return false;

	const auto pState = GetState(pThis);

	if (!pState)
		return false;

	// While the turn controller owns the hull, the reserved sweep must not be
	// replaced by the engine's mid-rotation footprint updates.
	if (pState->Locked)
		return true;

	const bool wasInitialized = pState->Initialized;
	const auto anchor = AnchorFromCoords(coords);
	int bucket = wasInitialized
		? pState->Bucket
		: BucketFromFacing(pThis->PrimaryFacing.Current());
	auto span = GetSpan(pThis, anchor, bucket);
	const auto pAnchorCell = MapClass::Instance.GetCellAt(anchor);
	const int height = MapClass::Instance.GetCellFloorHeight(coords) + CellClass::BridgeHeight;
	const bool alt = (coords.Z >= height && pAnchorCell->ContainsBridge());

	// A map-placed 1x1 ship may not have room for a 3-cell hull in its spawn
	// orientation. Try every facing before giving up; this keeps campaign
	// placements usable without editing every map.
	if (!wasInitialized)
	{
		CellStruct blocked { 0, 0 };

		if (!CanOccupy(pThis, span, alt, anchor, &blocked))
		{
			bool found = false;

			for (int candidate = 0; candidate < 8; ++candidate)
			{
				if (candidate == bucket)
					continue;

				auto candidateSpan = GetSpan(pThis, anchor, candidate);

				if (CanOccupy(pThis, candidateSpan, alt, anchor, &blocked))
				{
					bucket = candidate;
					span = std::move(candidateSpan);
					found = true;
					break;
				}
			}

			if (found)
			{
				pThis->PrimaryFacing.SetCurrent(DirStruct(bucket * 0x2000));

				Debug::Log("[Footprint] spawn reorient %s anchor=(%d,%d) bucket=%d\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
			}
			else
			{
				Debug::Log("[Footprint] spawn invalid %s anchor=(%d,%d): no facing fits, blocked=(%d,%d)\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, blocked.X, blocked.Y);
			}
		}
	}

	if (pState->Initialized
		&& pState->Anchor == anchor
		&& pState->Bucket == bucket
		&& pState->Alt == alt
		&& pState->Span == span)
	{
		return true;
	}

	if (wasInitialized)
		Unmark(pThis);

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetOwner(cell, alt, pThis);
	}

	pState->Initialized = true;
	pState->Anchor = anchor;
	pState->Bucket = bucket;
	pState->Alt = alt;
	pState->Span = span;

	Debug::Log("[Footprint] span %s anchor=(%d,%d) bucket=%d cells=%d alt=%d%s\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
		static_cast<int>(span.size()), alt ? 1 : 0,
		wasInitialized ? "" : " (initial)");

	return true;
}

bool Footprint::Unmark(UnitClass* pThis)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState || !pState->Initialized)
		return false;

	// A locked hull is mid-turn; the sweep is released by the turn controller.
	if (pState->Locked)
		return true;

	auto span = pState->Span;

	if (span.empty())
		span = GetSpan(pThis, pState->Anchor, pState->Bucket);

	for (const auto& cell : span)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (OwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearOwner(cell, false, pThis);
		}

		if (OwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearOwner(cell, true, pThis);
		}

		if (const auto pCellExt = CellExt::ExtMap.TryFind(pCell))
		{
			if (pCellExt->IncomingUnit == pThis)
				pCellExt->IncomingUnit = nullptr;

			if (pCellExt->IncomingUnitAlt == pThis)
				pCellExt->IncomingUnitAlt = nullptr;
		}
	}

	pState->Initialized = false;
	pState->Span.clear();

	return true;
}

bool Footprint::CommitSpan(UnitClass* pThis, const std::vector<CellStruct>& span, int bucket, bool alt, const CellStruct& anchor)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState)
		return false;

	const auto& oldSpan = pState->Span;

	for (const auto& cell : oldSpan)
	{
		if (std::find(span.begin(), span.end(), cell) != span.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (OwnerAt(cell, false) == pThis)
			{
				pCell->OccupationFlags &= ~0x20;
				ClearOwner(cell, false, pThis);
			}

			if (OwnerAt(cell, true) == pThis)
			{
				pCell->AltOccupationFlags &= ~0x20;
				ClearOwner(cell, true, pThis);
			}
		}
	}

	for (const auto& cell : span)
	{
		if (std::find(oldSpan.begin(), oldSpan.end(), cell) != oldSpan.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (alt)
				pCell->AltOccupationFlags |= 0x20;
			else
				pCell->OccupationFlags |= 0x20;

			SetOwner(cell, alt, pThis);
		}
	}

	pState->Initialized = true;
	pState->Bucket = bucket;
	pState->Anchor = anchor;
	pState->Alt = alt;
	pState->Span = span;

	return true;
}

void Footprint::Release(UnitClass* pThis)
{
	if (auto const pState = GetState(pThis))
		pState->Locked = false;

	Unmark(pThis);
}

void Footprint::ClearAll()
{
	EnsureGrid();
	std::fill(s_ownerNormal.begin(), s_ownerNormal.end(), nullptr);
	std::fill(s_ownerAlt.begin(), s_ownerAlt.end(), nullptr);

	// Scenario start can run after map units have already been marked. Clearing
	// only the owner grid would leave their 0x20 flags orphaned and make the
	// hull reject its own bow/stern cells. Rebuild from the live units instead.
	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !IsMultiCell(pUnit))
			continue;

		const auto pState = GetState(pUnit);

		if (pState && pState->Initialized && !pState->Span.empty())
		{
			pState->Locked = false;

			for (const auto& cell : pState->Span)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetOwner(cell, pState->Alt, pUnit);
			}
		}
		else
		{
			Mark(pUnit, pUnit->Location);
		}
	}
}

void Footprint::FixupAfterLoad()
{
	ClearAll();

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !IsMultiCell(pUnit))
			continue;

		EnsureLocomotor(pUnit);
		Mark(pUnit, pUnit->Location);
	}
}

void Footprint::EnsureLocomotor(UnitClass* pThis)
{
	if (!pThis || !IsMultiCell(pThis))
		return;

	if (locomotion_cast<FootprintLocomotionClass*>(pThis->Locomotor))
		return;

	auto pNewLoco = LocomotionClass::CreateInstance(__uuidof(FootprintLocomotionClass));

	if (!pNewLoco)
	{
		Debug::Log("[Footprint] failed to create locomotor for %s\n", pThis->GetTechnoType()->ID);
		return;
	}

	pNewLoco->Link_To_Object(pThis);
	pThis->Locomotor = pNewLoco;

	Debug::Log("[Footprint] replaced locomotor for %s\n", pThis->GetTechnoType()->ID);
}

int Footprint::GetYSort(TechnoClass* pThis)
{
	int base = pThis->ObjectClass::GetYSort();

	if (!IsMultiCell(pThis))
		return base;

	const auto pState = GetState(pThis);

	if (!pState || !pState->Initialized || pState->Span.empty())
		return base;

	short maxY = pState->Anchor.Y;

	for (const auto& offset : pState->Span)
		maxY = std::max<short>(maxY, offset.Y);

	return base + (maxY - pState->Anchor.Y) * Unsorted::LeptonsPerCell;
}
