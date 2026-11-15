#include "Footprint.h"

#include <AircraftClass.h>
#include <BuildingClass.h>
#include <InfantryClass.h>
#include <MapClass.h>
#include <OverlayTypeClass.h>
#include <TerrainClass.h>
#include <Unsorted.h>

#include <Ext/BuildingType/Body.h>
#include <Ext/Cell/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Locomotion/FootprintLocomotionClass.h>

#include <SpawnManagerClass.h>
#include <Utilities/Constructs.h>

#include <Phobos.h>
#include <SessionClass.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kDegToRad = kPi / 180.0;
	constexpr int kMapSide = 512;
	constexpr int kMaxCells = MapClass::MaxCells;

	std::vector<UnitClass*> s_ownerNormal;
	std::vector<UnitClass*> s_ownerAlt;
	std::vector<UnitClass*> s_barrierNormal;
	std::vector<UnitClass*> s_barrierAlt;
	bool s_gridReady = false;

	std::unordered_map<TechnoTypeClass*, FootprintGeometry> s_geometries;
	std::unordered_map<TechnoClass*, UnitClass*> s_carrierPayloads;

	// Fallback deck coordinates if this mod's art file fails to provide the
	// DockingOffset tags before the carrier is initialized.
	constexpr CoordStruct kDefaultCarrierDockOffsets[8] =
	{
		{ 640, 768, 0 },
		{ 256, 768, 0 },
		{ -2176, 768, 0 },
		{ -2560, 768, 0 },
		{ -3328, 640, 0 },
		{ -3328, -640, 0 },
		{ -2944, -768, 0 },
		{ -2560, -896, 0 }
	};

	CoordStruct GetCarrierDockOffset(TechnoTypeExt::ExtData* pTypeExt, size_t index)
	{
		if (pTypeExt && index < pTypeExt->AircraftCarrier_DockingOffsets.size())
		{
			const auto& offset = pTypeExt->AircraftCarrier_DockingOffsets[index];

			if (offset.X != 0 || offset.Y != 0 || offset.Z != 0)
				return offset;
		}

		return index < std::size(kDefaultCarrierDockOffsets)
			? kDefaultCarrierDockOffsets[index]
			: CoordStruct::Empty;
	}

	CoordStruct GetCarrierDockSlotCoords(UnitClass* pCarrier, TechnoTypeExt::ExtData* pTypeExt, size_t index)
	{
		auto offset = GetCarrierDockOffset(pTypeExt, index);
		offset.Z += Unsorted::CellHeight;

		// The deck offsets are authored directly relative to the hull centre.
		// GetFLHAbsoluteCoords() is meant for FLH/voxel offsets and mirrors Y,
		// which put every proxy on the wrong side of the ship. Rotate the raw
		// offset with the carrier facing here instead.
		auto mtx = Matrix3D::GetIdentity();
		mtx.RotateZ(static_cast<float>(pCarrier->PrimaryFacing.Current().GetRadian<32>()));
		mtx.Translate(static_cast<float>(offset.X), static_cast<float>(offset.Y), static_cast<float>(offset.Z));

		const auto result = mtx.GetTranslation();
		return pCarrier->GetRenderCoords()
			+ CoordStruct { static_cast<int>(result.X), static_cast<int>(result.Y), static_cast<int>(result.Z) };
	}

	// Legacy / pre-rewrite carrier builds used one real 1x1 BuildingClass per
	// deck slot as a native airport proxy. They are no longer needed; release
	// any that a loaded save restored so they cannot keep blocking cells.
	void ReleaseDockProxies(FootprintState* pState)
	{
		if (!pState)
			return;

		for (auto const pDock : pState->CarrierDockBuildings)
		{
			if (!pDock)
				continue;

			if (!pDock->InLimbo)
				pDock->Limbo();

			BuildingClass::Array.Remove(pDock);
			pDock->UnInit();
		}

		pState->CarrierDockBuildings.clear();
		pState->CarrierDockBuilding = nullptr;
	}

	void EnsureGrid()
	{
		if (s_gridReady)
			return;

		s_ownerNormal.assign(kMaxCells, nullptr);
		s_ownerAlt.assign(kMaxCells, nullptr);
		s_barrierNormal.assign(kMaxCells, nullptr);
		s_barrierAlt.assign(kMaxCells, nullptr);
		s_gridReady = true;
	}

	bool IsValidCell(const CellStruct& cell)
	{
		return cell.X >= 0 && cell.Y >= 0 && cell.X < kMapSide && cell.Y < kMapSide;
	}

	bool HasForeignOwner(UnitClass* pThis, const std::vector<CellStruct>& cells, bool alt)
	{
		if (!pThis)
			return true;

		for (const auto& cell : cells)
		{
			if (auto const pOwner = Footprint::OwnerAt(cell, alt); pOwner && pOwner != pThis)
				return true;

			if (auto const pBarrier = Footprint::BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
				return true;
		}

		return false;
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

		// MakeDiscreteSpan puts an even-sized hull at offsets -L/2+1 .. L/2,
		// so its geometric centre is +0.5 forward, not 0. Sampling around 0
		// added an extra cell at one end and biased the sweep. The diagonal
		// length margin used by the old 5x1 prototype must not be applied to a
		// 36x9 hull: it expands the 45-degree endpoints by several cells.
		const double centreForward = (config.Length % 2 == 0) ? 0.5 : 0.0;
		const double centreLateral = (config.Width % 2 == 0) ? 0.5 : 0.0;
		const double halfLength = config.Length * 0.5;
		const double halfWidth = config.Width * 0.5;
		const double radius = std::sqrt(halfLength * halfLength + halfWidth * halfWidth) + margin + 1.0;
		const int range = static_cast<int>(std::ceil(radius));

		for (int dy = -range; dy <= range; ++dy)
		{
			for (int dx = -range; dx <= range; ++dx)
			{
				const double forward = dx * forwardX + dy * forwardY
					- config.PivotForward - centreForward;
				const double lateral = dx * lateralX + dy * lateralY
					- config.PivotLateral - centreLateral;

				if (std::abs(forward) <= halfLength + margin + 1.0e-9
					&& std::abs(lateral) <= halfWidth + margin + 1.0e-9)
				{
					result.push_back(CellStruct { static_cast<short>(dx), static_cast<short>(dy) });
				}
			}
		}

		return result;
	}

	std::vector<CellStruct> MakeDiscreteSpan(const FootprintConfig& config, int bucket)
	{
		static const int forwardX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const int forwardY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
		static const int lateralX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static const int lateralY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

		const int b = bucket & 7;
		const int fMin = (config.Length % 2 == 1) ? -(config.Length / 2) : -(config.Length / 2) + 1;
		const int fMax = config.Length / 2;
		const int lMin = (config.Width % 2 == 1) ? -(config.Width / 2) : -(config.Width / 2) + 1;
		const int lMax = config.Width / 2;

		std::vector<CellStruct> result;
		result.reserve(static_cast<size_t>(config.Length * config.Width));

		for (int f = fMin; f <= fMax; ++f)
		{
			for (int l = lMin; l <= lMax; ++l)
			{
				result.push_back(CellStruct
				{
					static_cast<short>(f * forwardX[b] + l * lateralX[b]),
					static_cast<short>(f * forwardY[b] + l * lateralY[b])
				});
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

		Debug::CarrierLog("[FootprintBlock] %s cell=(%d,%d) reason=%s"
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

	std::vector<CellStruct> MakeSweepArc(const FootprintConfig& config, int from, int to, bool clockwise)
	{
		if (!config.Rotates || from == to)
			return MakeDiscreteSpan(config, from);

		std::set<std::pair<int, int>> cells;
		const int target = to & 7;
		const int step = clockwise ? 1 : -1;
		int bucket = from & 7;

		for (;;)
		{
			for (const auto& cell : MakeDiscreteSpan(config, bucket))
				cells.emplace(cell.X, cell.Y);

			if (bucket == target)
				break;

			bucket = (bucket + step + 8) & 7;
		}

		if (cells.empty())
			return {};

		// Return the actual swept hull cells, not the enclosing rectangle.
		// The locomotor reserves this result one 45-degree step at a time.
		std::vector<CellStruct> result;
		result.reserve(cells.size());

		for (const auto& cell : cells)
		{
			result.push_back(CellStruct
			{
				static_cast<short>(cell.first),
				static_cast<short>(cell.second)
			});
		}

		return result;
	}

	std::vector<CellStruct> MakeTurnStepSweep(const FootprintConfig& config,
		const CellStruct& anchor, int from, bool clockwise)
	{
		std::set<std::pair<int, int>> cells;
		const int step = clockwise ? 1 : -1;
		const int startBucket = from & 7;
		const int nextBucket = (startBucket + step + 8) & 7;
		const double startAngle = startBucket * 45.0;
		const double endAngle = startAngle + step * 45.0;

		// Sample the continuous 45-degree arc. Sampling only the two discrete
		// endpoint spans misses the end-cap cells swept while the hull rotates
		// between them, which is exactly the hollow area visible in issue1.
		constexpr int kSamples = 16;

		for (int i = 0; i <= kSamples; ++i)
		{
			const double t = static_cast<double>(i) / static_cast<double>(kSamples);
			const double angle = startAngle + (endAngle - startAngle) * t;

			for (const auto& cell : MakeSpanAtAngle(config, angle, 0.0))
				cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);
		}

		// Keep the exact discrete footprint at both ends. The point-sampled arc
		// can under-fill a cell whose square is only clipped at the endpoint.
		for (const auto& cell : MakeDiscreteSpan(config, startBucket))
			cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);

		for (const auto& cell : MakeDiscreteSpan(config, nextBucket))
			cells.emplace(cell.X + anchor.X, cell.Y + anchor.Y);

		std::vector<CellStruct> result;
		result.reserve(cells.size());

		for (const auto& [x, y] : cells)
			result.push_back(CellStruct { static_cast<short>(x), static_cast<short>(y) });

		return result;
	}

	void FillSingleCellGaps(std::set<std::pair<int, int>>& cells)
	{
		if (cells.empty())
			return;

		int minX = cells.begin()->first;
		int maxX = minX;
		int minY = cells.begin()->second;
		int maxY = minY;

		for (const auto& [x, y] : cells)
		{
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}

		bool changed = true;

		while (changed)
		{
			changed = false;

			for (int y = minY; y <= maxY; ++y)
			{
				for (int x = minX; x <= maxX; ++x)
				{
					const std::pair<int, int> cell { x, y };

					if (cells.find(cell) != cells.end())
						continue;

					const bool horizontalGap = cells.find({ x - 1, y }) != cells.end()
						&& cells.find({ x + 1, y }) != cells.end();
					const bool verticalGap = cells.find({ x, y - 1 }) != cells.end()
						&& cells.find({ x, y + 1 }) != cells.end();

					if (horizontalGap || verticalGap)
					{
						cells.insert(cell);
						changed = true;
					}
				}
			}
		}
	}

	void FillEnclosedHoles(std::set<std::pair<int, int>>& cells)
	{
		if (cells.empty())
			return;

		int minX = cells.begin()->first;
		int maxX = minX;
		int minY = cells.begin()->second;
		int maxY = minY;

		for (const auto& [x, y] : cells)
		{
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}

		const int x0 = minX - 1;
		const int y0 = minY - 1;
		const int x1 = maxX + 1;
		const int y1 = maxY + 1;
		std::set<std::pair<int, int>> outside;
		std::queue<std::pair<int, int>> queue;

		auto pushOutside = [&](int x, int y)
		{
			if (x < x0 || y < y0 || x > x1 || y > y1)
				return;

			const std::pair<int, int> cell { x, y };

			if (cells.find(cell) != cells.end() || outside.find(cell) != outside.end())
				return;

			outside.insert(cell);
			queue.push(cell);
		};

		for (int x = x0; x <= x1; ++x)
		{
			pushOutside(x, y0);
			pushOutside(x, y1);
		}

		for (int y = y0; y <= y1; ++y)
		{
			pushOutside(x0, y);
			pushOutside(x1, y);
		}

		while (!queue.empty())
		{
			const auto [x, y] = queue.front();
			queue.pop();

			pushOutside(x - 1, y);
			pushOutside(x + 1, y);
			pushOutside(x, y - 1);
			pushOutside(x, y + 1);
		}

		for (int y = y0; y <= y1; ++y)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const std::pair<int, int> cell { x, y };

				if (cells.find(cell) == cells.end() && outside.find(cell) == outside.end())
					cells.insert(cell);
			}
		}
	}

	std::vector<CellStruct> MakeStepReservation(
		const std::vector<CellStruct>& currentHull,
		const std::vector<CellStruct>& currentBarriers,
		const std::vector<CellStruct>& sweep)
	{
		std::set<std::pair<int, int>> current;
		std::set<std::pair<int, int>> occupied;

		for (const auto& cell : currentHull)
		{
			current.emplace(cell.X, cell.Y);
			occupied.emplace(cell.X, cell.Y);
		}

		for (const auto& cell : currentBarriers)
		{
			current.emplace(cell.X, cell.Y);
			occupied.emplace(cell.X, cell.Y);
		}

		for (const auto& cell : sweep)
			occupied.emplace(cell.X, cell.Y);

		// Close single-cell sampling jumps first (the two missing cells at the
		// 45-degree tips), then close any larger enclosed holes. A hollow ring
		// of swept cells would otherwise draw as an empty white box and let
		// normal units sit inside the reservation.
		FillSingleCellGaps(occupied);
		FillEnclosedHoles(occupied);

		std::vector<CellStruct> result;
		result.reserve(occupied.size());

		for (const auto& [x, y] : occupied)
		{
			if (current.find({ x, y }) == current.end())
				result.push_back(CellStruct { static_cast<short>(x), static_cast<short>(y) });
		}

		return result;
	}
}

void FootprintGeometry::Build(const FootprintConfig& config)
{
	if (Built)
		return;

	const auto primary = MakeDiscreteSpan(config, 0);

	for (int bucket = 0; bucket < 8; ++bucket)
	{
		Span[bucket] = config.Rotates
			? MakeDiscreteSpan(config, bucket)
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

			SweepCW[index] = MakeSweepArc(config, from, to, true);
			SweepCCW[index] = MakeSweepArc(config, from, to, false);
		}
	}

	Built = true;
}

bool Footprint::MultiCellAllowed()
{
	if (Phobos::Config::MultiCellDisabled)
		return false;

	return !(Phobos::Config::MultiCellDisabledInMultiplayer
		&& SessionClass::IsMultiplayer());
}

bool Footprint::IsMultiCell(TechnoTypeClass* pType)
{
	if (!pType || !MultiCellAllowed())
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

bool Footprint::ReserveTurnStep(UnitClass* pThis, const CellStruct& anchor,
	int from, int to, bool clockwise, bool alt)
{
	if (!pThis)
		return false;

	const auto pState = GetState(pThis);

	if (!pState)
		return false;

	const int start = from & 7;

	if ((to & 7) == start)
		return true;

	const int step = clockwise ? 1 : -1;
	const int next = (start + step + 8) & 7;
	const auto& config = GetConfig(pThis->GetTechnoType());

	// Reserve the actual hull area swept while rotating from the current
	// 45-degree bucket to the next one. This includes temporary end-cap cells
	// that are not part of either endpoint footprint and must stay occupied
	// until the turn passes them. It is not an axis-aligned envelope, and the
	// next bucket's barrier cells are committed separately when the turn ends.
	const auto sweep = MakeTurnStepSweep(config, anchor, start, clockwise);
	const auto reservation = MakeStepReservation(
		pState->Span, pState->BarrierSpan, sweep);

	if (reservation.empty() || !CanOccupy(pThis, reservation, alt, anchor))
		return false;

	ReleaseReservation(pThis);

	if (!ReserveSweep(pThis, reservation, alt))
		return false;

	Debug::CarrierLog("[FootprintReserve] %s cell=(%d,%d) %d -> %d (%s) next=%d reserved=%d\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, start, to & 7,
		clockwise ? "cw" : "ccw", next, static_cast<int>(pState->ReservedSpan.size()));

	return true;
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

UnitClass* Footprint::BarrierOwnerAt(const CellStruct& cell, bool alt)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return nullptr;

	return alt ? s_barrierAlt[index] : s_barrierNormal[index];
}

UnitClass* Footprint::BarrierOwnerAt(const CellClass* pCell, bool alt)
{
	return pCell ? BarrierOwnerAt(pCell->MapCoords, alt) : nullptr;
}

void Footprint::SetBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	(alt ? s_barrierAlt : s_barrierNormal)[index] = pOwner;
}

void Footprint::ClearBarrierOwner(const CellStruct& cell, bool alt, UnitClass* pOwner)
{
	EnsureGrid();

	const int index = CellIndex(cell);

	if (index < 0)
		return;

	auto& owner = (alt ? s_barrierAlt : s_barrierNormal)[index];

	if (owner == pOwner)
		owner = nullptr;
}

std::vector<CellStruct> Footprint::ComputeBarriers(const std::vector<CellStruct>& span)
{
	std::set<std::pair<int, int>> hull;
	std::set<std::pair<int, int>> barriers;

	for (const auto& cell : span)
		hull.emplace(cell.X, cell.Y);

	for (const auto& cell : span)
	{
		for (int dx : { -1, 1 })
		{
			for (int dy : { -1, 1 })
			{
				if (hull.find({ cell.X + dx, cell.Y + dy }) == hull.end())
					continue;

				const std::pair<int, int> sideA { cell.X + dx, cell.Y };
				const std::pair<int, int> sideB { cell.X, cell.Y + dy };

				if (hull.find(sideA) == hull.end())
					barriers.insert(sideA);

				if (hull.find(sideB) == hull.end())
					barriers.insert(sideB);
			}
		}
	}

	std::vector<CellStruct> result;
	result.reserve(barriers.size());

	for (const auto& cell : barriers)
	{
		result.push_back(CellStruct
		{
			static_cast<short>(cell.first),
			static_cast<short>(cell.second)
		});
	}

	return result;
}

bool Footprint::IsOwnCell(TechnoClass* pThis, const CellClass* pCell, bool alt)
{
	if (!pThis || !pCell)
		return false;

	if (OwnerAt(pCell, alt) == pThis)
		return true;

	if (BarrierOwnerAt(pCell, alt) == pThis)
		return true;

	return false;
}

bool Footprint::HasObstacleNear(TechnoClass* pThis, int radius)
{
	if (!pThis || radius <= 0)
		return false;

	const auto center = pThis->GetMapCoords();

	for (int dy = -radius; dy <= radius; ++dy)
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			const CellStruct cell
			{
				static_cast<short>(center.X + dx),
				static_cast<short>(center.Y + dy)
			};

			if (const auto pOwner = OwnerAt(cell, false); pOwner && pOwner != pThis)
				return true;

			if (const auto pBarrier = BarrierOwnerAt(cell, false); pBarrier && pBarrier != pThis)
				return true;

			if (const auto pOwner = OwnerAt(cell, true); pOwner && pOwner != pThis)
				return true;

			if (const auto pBarrier = BarrierOwnerAt(cell, true); pBarrier && pBarrier != pThis)
				return true;
		}
	}

	return false;
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
	const auto pAnchorCell = MapClass::Instance.TryGetCellAt(anchor);

	if (!pAnchorCell)
		return false;

	const int level = pAnchorCell->GetLevel();
	const bool bridge = pAnchorCell->ContainsBridge();
	const int maxClimb = std::max(0, config.MaxClimb);

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

		const auto pBarrierOwner = BarrierOwnerAt(cell, alt);

		if (pBarrierOwner && pBarrierOwner != pThis)
		{
			LogBlockedOccupy(pThis, pCell, cell, "barrier", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCell->ContainsBridge() != bridge)
		{
			LogBlockedOccupy(pThis, pCell, cell, "level-bridge", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		const int cellLevel = pCell->GetLevel();

		if (std::abs(cellLevel - level) > maxClimb)
		{
			LogBlockedOccupy(pThis, pCell, cell, "slope", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		const auto pCellBuilding = pCell->GetBuilding();
		const auto pState = GetState(pThis);

		// The carrier's hidden AIRCARRDOCK proxies sit on its deck. They must
		// not block the carrier that owns them, including the occupation flags
		// checked by IsClearToMove(), otherwise the ship can never move.
		const bool ownDock = pState && pCellBuilding
			&& (pCellBuilding == pState->CarrierDockBuilding
				|| std::find(pState->CarrierDockBuildings.begin(),
					pState->CarrierDockBuildings.end(), pCellBuilding) != pState->CarrierDockBuildings.end());

		// Ignore infantry and vehicles here; vehicles are checked below so that
		// the unit's own footprint cells stay passable, and infantry is handled
		// by the crush setting.
		if (!ownDock
			&& !pCell->IsClearToMove(pType->SpeedType, true, true, -1, movementZone, cellLevel, bridge))
		{
			LogBlockedOccupy(pThis, pCell, cell, "clear-to-move", level, bridge, alt);

			if (pBlocked)
				*pBlocked = cell;

			return false;
		}

		if (pCellBuilding && !ownDock)
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
			// footprint, a barrier cell, or a vanilla unit whose cell lookup is
			// unavailable. Own barriers are passable for the hull itself.
			if (!pBarrierOwner || pBarrierOwner != pThis)
			{
				LogBlockedOccupy(pThis, pCell, cell, "stale-flag", level, bridge, alt);

				if (pBlocked)
					*pBlocked = cell;

				return false;
			}
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

bool Footprint::IsClimbAllowed(UnitClass* pThis, const CellStruct& from, const CellStruct& to)
{
	if (!pThis)
		return false;

	const auto pFromCell = MapClass::Instance.TryGetCellAt(from);
	const auto pToCell = MapClass::Instance.TryGetCellAt(to);

	if (!pFromCell || !pToCell || pFromCell->ContainsBridge() != pToCell->ContainsBridge())
		return false;

	const auto& config = GetConfig(pThis->GetTechnoType());
	return std::abs(pToCell->GetLevel() - pFromCell->GetLevel()) <= std::max(0, config.MaxClimb);
}

void Footprint::EnsureAircraftCarrierSpawns(UnitClass* pThis)
{
	if (!pThis || !pThis->Owner)
		return;

	const auto pType = pThis->GetTechnoType();
	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

	if (!pTypeExt || !TechnoTypeExt::IsAircraftCarrier(pType))
		return;

	const auto pState = GetState(pThis);

	if (!pState)
		return;

	// Old saves / pre-placed units may still carry a SpawnManager created by
	// the earlier carrier implementation. It creates a second set of J35s
	// behind the normal payload objects, so remove it completely.
	if (pThis->SpawnManager)
	{
		pThis->SpawnManager->KillNodes();
		GameDelete(pThis->SpawnManager);
		pThis->SpawnManager = nullptr;
		pState->CarrierPayloadsInitialized = false;
		pState->CarrierPayloads.clear();
		pState->CarrierPayloadDocked.clear();
		pState->CarrierPayloadLastCoords.clear();
		pState->CarrierPayloadReloadTimer.clear();

		Debug::CarrierLog("[AircraftCarrier] removed legacy SpawnManager for %s\n", pType->ID);
	}

	// The deck no longer uses native airport proxies. Saves made before the
	// rewrite may still restore them; release them once.
	if (pState->CarrierDockBuilding || !pState->CarrierDockBuildings.empty())
	{
		ReleaseDockProxies(pState);
		Debug::CarrierLog("[AircraftCarrier] released legacy dock proxies for %s\n", pType->ID);
	}

	if (pState->CarrierPayloadsInitialized)
	{
		// The payload lookup is only a cache; rebuild it after a load.
		for (auto const pAircraft : pState->CarrierPayloads)
		{
			if (pAircraft)
				s_carrierPayloads[pAircraft] = pThis;
		}

		const size_t count = pState->CarrierPayloads.size();

		// Keep the parallel arrays in step with saves that predate a field.
		if (pState->CarrierPayloadDocked.size() < count)
			pState->CarrierPayloadDocked.resize(count, 1);

		if (pState->CarrierPayloadLastCoords.size() < count)
			pState->CarrierPayloadLastCoords.resize(count, CoordStruct::Empty);

		if (pState->CarrierPayloadReloadTimer.size() < count)
			pState->CarrierPayloadReloadTimer.resize(count, 0);

		const bool moved = pState->CarrierLastCoords.X != pThis->Location.X
			|| pState->CarrierLastCoords.Y != pThis->Location.Y
			|| pState->CarrierLastCoords.Z != pThis->Location.Z
			|| pState->CarrierLastBucket != pState->Bucket;

		for (size_t i = 0; i < count; ++i)
		{
			const auto pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft || !pAircraft->IsAlive || pAircraft->InLimbo)
				continue;

			// Airborne payloads are owned by UpdateCarrierPayload / the engine.
			if (!pState->CarrierPayloadDocked[i])
				continue;

			const auto expected = GetCarrierDockSlotCoords(pThis, pTypeExt, i);
			const bool unchanged = !moved
				&& pState->CarrierPayloadLastCoords[i].X == expected.X
				&& pState->CarrierPayloadLastCoords[i].Y == expected.Y
				&& pState->CarrierPayloadLastCoords[i].Z == expected.Z;

			if (unchanged)
				continue;

			const int dockDir = i < pTypeExt->AircraftCarrier_DockingDirs.size()
				? pTypeExt->AircraftCarrier_DockingDirs[i]
				: 0;

			pAircraft->SetLocation(expected);
			pAircraft->InAir = false;
			pAircraft->PrimaryFacing.SetCurrent(dockDir != 0
				? DirStruct(static_cast<DirType>(dockDir & 0xFF))
				: pThis->PrimaryFacing.Current());

			pState->CarrierPayloadLastCoords[i] = expected;
		}

		pState->CarrierLastCoords = pThis->Location;
		pState->CarrierLastBucket = pState->Bucket;
		return;
	}

	const int docks = std::max(0, pTypeExt->AircraftCarrier_NumberOfDocks.Get());

	if (!pTypeExt->AircraftCarrier_Payloads.empty() && docks > 0)
	{
		std::vector<AircraftTypeClass*> slotTypes;
		slotTypes.reserve(docks);

		for (size_t i = 0; i < pTypeExt->AircraftCarrier_Payloads.size() && static_cast<int>(slotTypes.size()) < docks; ++i)
		{
			const auto pPayload = pTypeExt->AircraftCarrier_Payloads[i];

			if (!pPayload)
				continue;

			int count = 1;

			if (i < pTypeExt->AircraftCarrier_PayloadsNum.size())
				count = pTypeExt->AircraftCarrier_PayloadsNum[i];
			else if (pTypeExt->AircraftCarrier_PayloadsNum.size() == 1)
				count = pTypeExt->AircraftCarrier_PayloadsNum[0];

			count = std::clamp(count, 0, docks - static_cast<int>(slotTypes.size()));

			for (int j = 0; j < count; ++j)
				slotTypes.push_back(pPayload);
		}

		for (size_t i = 0; i < slotTypes.size(); ++i)
		{
			auto const pAircraft = static_cast<AircraftClass*>(slotTypes[i]->CreateObject(pThis->Owner));

			if (!pAircraft)
				continue;

			const auto coords = GetCarrierDockSlotCoords(pThis, pTypeExt, i);

			pAircraft->Spawned = false;
			pAircraft->SpawnOwner = nullptr;
			pAircraft->SetLocation(coords);

			if (!pAircraft->Unlimbo(coords, pThis->PrimaryFacing.Current().GetDir()))
			{
				Debug::Log("[AircraftCarrier] failed to Unlimbo payload %s for %s\n",
					slotTypes[i]->ID, pType->ID);
				continue;
			}

			// Unlimbo does not reliably preserve the aircraft's parked facing.
			const int dockDir = i < pTypeExt->AircraftCarrier_DockingDirs.size()
				? pTypeExt->AircraftCarrier_DockingDirs[i]
				: 0;

			pAircraft->PrimaryFacing.SetCurrent(dockDir != 0
				? DirStruct(static_cast<DirType>(dockDir & 0xFF))
				: pThis->PrimaryFacing.Current());

			// Parked payloads are real units, but they must not be treated as
			// airborne targets while they sit on the deck.
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->SetDestination(nullptr, true);
			pAircraft->InAir = false;
			pAircraft->QueueMission(Mission::Sleep, true);

			pState->CarrierPayloads.push_back(pAircraft);
			pState->CarrierPayloadDocked.push_back(1);
			pState->CarrierPayloadLastCoords.push_back(pAircraft->Location);
			pState->CarrierPayloadReloadTimer.push_back(0);
			s_carrierPayloads[pAircraft] = pThis;
		}
	}

	pState->CarrierPayloadsInitialized = true;
	pState->CarrierLastCoords = pThis->Location;
	pState->CarrierLastBucket = pState->Bucket;

	Debug::CarrierLog("[AircraftCarrier] initialized %d payload aircraft for %s\n",
		static_cast<int>(pState->CarrierPayloads.size()), pType->ID);
}

UnitClass* Footprint::GetCarrierPayloadOwner(TechnoClass* pPayload)
{
	auto const it = s_carrierPayloads.find(pPayload);
	return it == s_carrierPayloads.end() ? nullptr : it->second;
}

bool Footprint::IsCarrierPayloadParked(TechnoClass* pPayload)
{
	if (!pPayload || pPayload->WhatAmI() != AbstractType::Aircraft)
		return false;

	const auto pCarrier = GetCarrierPayloadOwner(pPayload);

	if (!pCarrier || !pCarrier->IsAlive)
		return false;

	const auto pState = GetState(pCarrier);

	if (!pState)
		return false;

	const auto pAircraft = static_cast<AircraftClass*>(pPayload);
	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return false;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	return index < pState->CarrierPayloadDocked.size()
		&& pState->CarrierPayloadDocked[index] != 0;
}

void Footprint::UpdateCarrierPayload(AircraftClass* pAircraft)
{
	if (!pAircraft || !pAircraft->IsAlive)
		return;

	const auto pCarrier = GetCarrierPayloadOwner(pAircraft);

	if (!pCarrier || !pCarrier->IsAlive)
	{
		s_carrierPayloads.erase(pAircraft);
		return;
	}

	const auto pState = GetState(pCarrier);

	if (!pState)
		return;

	const auto it = std::find(pState->CarrierPayloads.begin(), pState->CarrierPayloads.end(), pAircraft);

	if (it == pState->CarrierPayloads.end())
		return;

	const size_t index = static_cast<size_t>(std::distance(pState->CarrierPayloads.begin(), it));

	if (index >= pState->CarrierPayloadDocked.size())
		return;

	if (pState->CarrierPayloadReloadTimer.size() < pState->CarrierPayloads.size())
		pState->CarrierPayloadReloadTimer.resize(pState->CarrierPayloads.size(), 0);

	const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pCarrier->GetTechnoType());

	if (!pTypeExt)
		return;

	const auto mission = pAircraft->GetCurrentMission();
	auto& docked = pState->CarrierPayloadDocked[index];

	if (docked)
	{
		// AirportBound aircraft keep trying to enter a dock that no longer
		// exists. Enter is noise here, not a player order, but the mission must
		// still be reset every time the engine queues it or the aircraft would
		// stop rearming.
		if (mission == Mission::Enter)
		{
			pAircraft->DockNowHeadingTo = nullptr;
			pAircraft->SetDestination(nullptr, true);
			pAircraft->QueueMission(Mission::Sleep, true);
		}

		const bool ordered = pAircraft->Destination != nullptr
			|| pAircraft->Target != nullptr
			|| mission == Mission::Move
			|| mission == Mission::QMove
			|| mission == Mission::Attack
			|| mission == Mission::AttackMove;

		if (ordered)
		{
			// Launch: leave the deck and let the engine fly the ordered mission.
			docked = 0;

			CoordStruct lift = pAircraft->Location;
			lift.Z += 192;
			pAircraft->SetLocation(lift);
			pAircraft->InAir = true;

			if (index < pState->CarrierPayloadReloadTimer.size())
				pState->CarrierPayloadReloadTimer[index] = 0;

			Debug::CarrierLog("[AircraftCarrier] %s launched from slot %d\n",
				pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index));
			return;
		}

		// Rearm on deck.
		const int maxAmmo = pAircraft->Type ? pAircraft->Type->Ammo : 0;

		if (maxAmmo > 0 && pAircraft->Ammo < maxAmmo)
		{
			auto& timer = pState->CarrierPayloadReloadTimer[index];

			if (++timer >= 30)
			{
				timer = 0;
				++pAircraft->Ammo;
			}
		}
		else if (index < pState->CarrierPayloadReloadTimer.size())
		{
			pState->CarrierPayloadReloadTimer[index] = 0;
		}

		// Keep the parked aircraft pinned to the moving deck.
		const auto expected = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);

		if (pAircraft->Location.X != expected.X
			|| pAircraft->Location.Y != expected.Y
			|| pAircraft->Location.Z != expected.Z)
		{
			pAircraft->SetLocation(expected);
		}

		pAircraft->InAir = false;

		return;
	}

	// Airborne. Only intervene when the aircraft wants to come home. Aircraft
	// with unlimited ammo (Ammo < 0) never return on their own.
	const bool outOfAmmo = pAircraft->Type && pAircraft->Type->Ammo > 0
		&& pAircraft->Ammo <= 0;
	const bool wantsReturn = mission == Mission::Enter || outOfAmmo;

	if (!wantsReturn)
		return;

	const auto slotCoords = GetCarrierDockSlotCoords(pCarrier, pTypeExt, index);
	const auto current = pAircraft->Location;
	const int dx = slotCoords.X - current.X;
	const int dy = slotCoords.Y - current.Y;
	const int dz = slotCoords.Z - current.Z;
	const double horizontal = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
	const double distance = std::sqrt(
		static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + static_cast<double>(dz) * dz);

	// Far away: let the native fly locomotor bring the aircraft to the ship.
	if (horizontal > 768.0)
	{
		if (pAircraft->Destination != pCarrier)
		{
			pAircraft->SetDestination(pCarrier, true);
			pAircraft->QueueMission(Mission::Move, true);
		}

		return;
	}

	// Final approach: stop the locomotor and glide onto the live deck slot by
	// hand. Native landing requires a BuildingClass dock, which this design no
	// longer has, so the last few hundred leptons are driven here.
	if (pAircraft->Destination)
		pAircraft->SetDestination(nullptr, true);

	if (mission != Mission::Sleep)
		pAircraft->QueueMission(Mission::Sleep, true);

	if (distance <= 48.0)
	{
		pAircraft->SetLocation(slotCoords);
		pAircraft->SetDestination(nullptr, true);
		pAircraft->InAir = false;
		pAircraft->QueueMission(Mission::Sleep, true);

		const int dockDir = index < pTypeExt->AircraftCarrier_DockingDirs.size()
			? pTypeExt->AircraftCarrier_DockingDirs[index]
			: 0;

		pAircraft->PrimaryFacing.SetCurrent(dockDir != 0
			? DirStruct(static_cast<DirType>(dockDir & 0xFF))
			: pCarrier->PrimaryFacing.Current());

		docked = 1;

		if (index < pState->CarrierPayloadReloadTimer.size())
			pState->CarrierPayloadReloadTimer[index] = 0;

		Debug::CarrierLog("[AircraftCarrier] %s landed in slot %d\n",
			pAircraft->Type ? pAircraft->Type->ID : "?", static_cast<int>(index));

		return;
	}

	constexpr double kGlideSpeed = 64.0;
	const double t = std::min(1.0, kGlideSpeed / distance);

	pAircraft->SetLocation(CoordStruct
	{
		current.X + static_cast<int>(std::lround(dx * t)),
		current.Y + static_cast<int>(std::lround(dy * t)),
		current.Z + static_cast<int>(std::lround(dz * t))
	});
}

bool Footprint::Mark(UnitClass* pThis, const CoordStruct& coords)
{
	if (!pThis || !IsMultiCell(pThis))
		return false;

	const auto pState = GetState(pThis);

	if (!pState)
		return false;

	// Pre-placed map units may never enter the normal Unlimbo/Process path.
	// Mark() is guaranteed for every footprint unit, so initialize the
	// SpawnManager and its payload nodes here as well.
	EnsureAircraftCarrierSpawns(pThis);

	// While the turn controller owns the hull, or the locomotor is between
	// cell centres, the engine's mid-rotation/mid-move Mark calls must not
	// retarget the footprint to the cell currently under the model.
	if (pState->Locked || pState->Translating)
		return true;

	const bool wasInitialized = pState->Initialized;
	const auto anchor = AnchorFromCoords(coords);
	int bucket = wasInitialized
		? pState->Bucket
		: BucketFromFacing(pThis->PrimaryFacing.Current());
	auto span = GetSpan(pThis, anchor, bucket);
	const auto pAnchorCell = MapClass::Instance.TryGetCellAt(anchor);

	if (!pAnchorCell)
		return false;

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

				Debug::CarrierLog("[Footprint] spawn reorient %s anchor=(%d,%d) bucket=%d\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
			}
			else
			{
				Debug::Log("[Footprint] spawn invalid %s anchor=(%d,%d): no facing fits, blocked=(%d,%d)\n",
					pThis->GetTechnoType()->ID, anchor.X, anchor.Y, blocked.X, blocked.Y);
			}
		}
	}

	const auto barriers = ComputeBarriers(span);

	if (pState->Initialized
		&& pState->Anchor == anchor
		&& pState->Bucket == bucket
		&& pState->Alt == alt
		&& pState->Span == span
		&& pState->BarrierSpan == barriers)
	{
		return true;
	}

	// Check the complete new hull and barrier set before clearing the old
	// footprint. Two hulls moving side by side must never overwrite each
	// other's cells halfway through a Mark.
	if (HasForeignOwner(pThis, span, alt) || HasForeignOwner(pThis, barriers, alt))
	{
		Debug::CarrierLog("[Footprint] mark blocked %s anchor=(%d,%d) bucket=%d by another hull/barrier\n",
			pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
		return false;
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

	for (const auto& cell : barriers)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		// Never overwrite another hull or another unit's barrier.
		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
			continue;

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
	}

	pState->Initialized = true;
	pState->Anchor = anchor;
	pState->Bucket = bucket;
	pState->Alt = alt;
	pState->Span = span;
	pState->BarrierSpan = barriers;

	Debug::CarrierLog("[Footprint] span %s anchor=(%d,%d) bucket=%d cells=%d barriers=%d alt=%d%s\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
		static_cast<int>(span.size()), static_cast<int>(barriers.size()), alt ? 1 : 0,
		wasInitialized ? "" : " (initial)");

	return true;
}

bool Footprint::Unmark(UnitClass* pThis, const CoordStruct* pCoords)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState || !pState->Initialized)
		return false;

	// A locked/translating hull is mid-turn or between cell centres; the
	// controller commits the final footprint when it reaches the cell centre.
	if (pState->Locked || pState->Translating)
		return true;

	if (!pState->ReservedSpan.empty())
		ReleaseReservation(pThis);

	// The engine routinely unmarks and re-marks a stationary unit every frame.
	// Clearing the footprint here would make the hull and its barriers flicker,
	// so other units would plan through a momentarily free cell and then stall.
	// Only a real relocation (different anchor) or Limbo/Release force-clears.
	if (pCoords && pThis->IsAlive && !pThis->InLimbo)
	{
		const auto coordsAnchor = AnchorFromCoords(*pCoords);

		if (pState->Anchor == coordsAnchor)
			return true;
	}

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

	for (const auto& cell : pState->BarrierSpan)
	{
		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (BarrierOwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, false, pThis);
		}

		if (BarrierOwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, true, pThis);
		}
	}

	pState->Initialized = false;
	pState->Translating = false;
	pState->Span.clear();
	pState->BarrierSpan.clear();
	pState->ReconcileActive = false;
	pState->ReconcileTarget = -1;

	return true;
}

bool Footprint::CommitSpan(UnitClass* pThis, const std::vector<CellStruct>& span, int bucket, bool alt, const CellStruct& anchor)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState)
		return false;

	const auto& oldSpan = pState->Span;
	const auto& oldBarriers = pState->BarrierSpan;
	const auto newBarriers = ComputeBarriers(span);

	// Validate the whole target footprint first. This prevents two units from
	// interleaving their clears/sets and stealing each other's ownership.
	if (HasForeignOwner(pThis, span, alt) || HasForeignOwner(pThis, newBarriers, alt))
	{
		Debug::CarrierLog("[Footprint] commit blocked %s anchor=(%d,%d) bucket=%d by another hull/barrier\n",
			pThis->GetTechnoType()->ID, anchor.X, anchor.Y, bucket);
		return false;
	}

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

	for (const auto& cell : oldBarriers)
	{
		if (std::find(newBarriers.begin(), newBarriers.end(), cell) != newBarriers.end())
			continue;

		// A cell that is part of the active turn reservation must stay blocked
		// until the reservation is released, even if it is no longer a gap
		// barrier of the current hull bucket.
		if (std::find(pState->ReservedSpan.begin(), pState->ReservedSpan.end(), cell) != pState->ReservedSpan.end())
			continue;

		if (const auto pCell = MapClass::Instance.TryGetCellAt(cell))
		{
			if (BarrierOwnerAt(cell, false) == pThis)
			{
				pCell->OccupationFlags &= ~0x20;
				ClearBarrierOwner(cell, false, pThis);
			}

			if (BarrierOwnerAt(cell, true) == pThis)
			{
				pCell->AltOccupationFlags &= ~0x20;
				ClearBarrierOwner(cell, true, pThis);
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

	for (const auto& cell : newBarriers)
	{
		if (std::find(oldBarriers.begin(), oldBarriers.end(), cell) != oldBarriers.end())
			continue;

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
			continue;

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
			continue;

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
	}

	pState->Initialized = true;
	pState->Bucket = bucket;
	pState->Anchor = anchor;
	pState->Alt = alt;
	pState->Span = span;
	pState->BarrierSpan = newBarriers;

	return true;
}

bool Footprint::ReserveSweep(UnitClass* pThis, const std::vector<CellStruct>& sweep, bool alt)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState)
		return false;

	std::vector<CellStruct> added;
	added.reserve(sweep.size());

	auto rollback = [&]()
	{
		for (const auto& cell : added)
		{
			const auto pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!pCell)
				continue;

			if (BarrierOwnerAt(cell, alt) == pThis)
			{
				if (alt)
					pCell->AltOccupationFlags &= ~0x20;
				else
					pCell->OccupationFlags &= ~0x20;

				ClearBarrierOwner(cell, alt, pThis);
			}
		}

		pState->ReservedSpan.erase(
			std::remove_if(pState->ReservedSpan.begin(), pState->ReservedSpan.end(),
				[&added](const CellStruct& cell)
				{
					return std::find(added.begin(), added.end(), cell) != added.end();
				}),
			pState->ReservedSpan.end());
	};

	for (const auto& cell : sweep)
	{
		if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
			continue;

		if (std::find(pState->BarrierSpan.begin(), pState->BarrierSpan.end(), cell) != pState->BarrierSpan.end())
			continue;

		if (std::find(pState->ReservedSpan.begin(), pState->ReservedSpan.end(), cell) != pState->ReservedSpan.end())
			continue;

		if (const auto pOwner = OwnerAt(cell, alt); pOwner && pOwner != pThis)
		{
			rollback();
			return false;
		}

		if (const auto pBarrier = BarrierOwnerAt(cell, alt); pBarrier && pBarrier != pThis)
		{
			rollback();
			return false;
		}

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
		{
			rollback();
			return false;
		}

		if (alt)
			pCell->AltOccupationFlags |= 0x20;
		else
			pCell->OccupationFlags |= 0x20;

		SetBarrierOwner(cell, alt, pThis);
		pState->ReservedSpan.push_back(cell);
		added.push_back(cell);
	}

	return true;
}

void Footprint::ReleaseReservation(UnitClass* pThis)
{
	const auto pState = GetState(pThis);

	if (!pThis || !pState || pState->ReservedSpan.empty())
		return;

	for (const auto& cell : pState->ReservedSpan)
	{
		const bool inHull = std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end();
		const bool inBarrier = std::find(pState->BarrierSpan.begin(), pState->BarrierSpan.end(), cell) != pState->BarrierSpan.end();

		if (inHull || inBarrier)
		{
			// The hull keeps its own 0x20; a reservation marker on a hull cell is
			// redundant and should not linger as a barrier owner.
			if (inHull && !inBarrier)
				ClearBarrierOwner(cell, pState->Alt, pThis);

			continue;
		}

		const auto pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			continue;

		if (BarrierOwnerAt(cell, false) == pThis)
		{
			pCell->OccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, false, pThis);
		}

		if (BarrierOwnerAt(cell, true) == pThis)
		{
			pCell->AltOccupationFlags &= ~0x20;
			ClearBarrierOwner(cell, true, pThis);
		}
	}

	pState->ReservedSpan.clear();
}

void Footprint::Release(UnitClass* pThis)
{
	if (auto const pState = GetState(pThis))
		pState->Locked = false;

	ReleaseReservation(pThis);
	Unmark(pThis);
}

void Footprint::ClearAll()
{
	EnsureGrid();
	std::fill(s_ownerNormal.begin(), s_ownerNormal.end(), nullptr);
	std::fill(s_ownerAlt.begin(), s_ownerAlt.end(), nullptr);
	std::fill(s_barrierNormal.begin(), s_barrierNormal.end(), nullptr);
	std::fill(s_barrierAlt.begin(), s_barrierAlt.end(), nullptr);

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
			pState->Translating = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			if (pState->BarrierSpan.empty())
				pState->BarrierSpan = ComputeBarriers(pState->Span);

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

			for (const auto& cell : pState->BarrierSpan)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (const auto pOwner = OwnerAt(cell, pState->Alt); pOwner && pOwner != pUnit)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetBarrierOwner(cell, pState->Alt, pUnit);
			}

			for (const auto& cell : pState->ReservedSpan)
			{
				if (std::find(pState->Span.begin(), pState->Span.end(), cell) != pState->Span.end())
					continue;

				const auto pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell)
					continue;

				if (const auto pOwner = OwnerAt(cell, pState->Alt); pOwner && pOwner != pUnit)
					continue;

				if (pState->Alt)
					pCell->AltOccupationFlags |= 0x20;
				else
					pCell->OccupationFlags |= 0x20;

				SetBarrierOwner(cell, pState->Alt, pUnit);
			}
		}
		else
		{
			Mark(pUnit, pUnit->Location);
		}
	}

	// The payload lookup is a cache, not authoritative state: rebuild it from
	// the live carrier states so it can never keep pointers from a previous
	// scenario or a stale save.
	s_carrierPayloads.clear();

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !IsMultiCell(pUnit))
			continue;

		if (auto const pState = GetState(pUnit); pState && pState->CarrierPayloadsInitialized)
		{
			for (auto const pAircraft : pState->CarrierPayloads)
			{
				if (pAircraft)
					s_carrierPayloads[pAircraft] = pUnit;
			}
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

		// Turn reservations are transient. A save made mid-turn restores the
		// cells it blocked, so release them explicitly before rebuilding the
		// hull; otherwise they would stay occupied until the unit moves again.
		if (auto const pState = GetState(pUnit))
		{
			pState->Locked = false;
			pState->Translating = false;
			ReleaseReservation(pUnit);
		}

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

	Debug::CarrierLog("[Footprint] replaced locomotor for %s\n", pThis->GetTechnoType()->ID);
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

void Footprint::ReconcileFacing(UnitClass* pThis)
{
	if (!pThis || !IsMultiCell(pThis))
		return;

	const auto pState = GetState(pThis);

	if (!pState || !pState->Initialized)
		return;

	// The locomotor owns the hull while it is moving or executing its own turn.
	if (pThis->Locomotor && pThis->Locomotor->Is_Moving_Now())
		return;

	if (pState->Locked && !pState->ReconcileActive)
		return;

	const int currentBucket = BucketFromFacing(pThis->PrimaryFacing.Current());

	if (pState->ReconcileActive)
	{
		if (currentBucket != pState->Bucket)
		{
			const auto currentHull = GetSpan(pThis, pState->Anchor, currentBucket);
			CommitSpan(pThis, currentHull, currentBucket, pState->Alt, pState->Anchor);

			if (currentBucket != pState->ReconcileTarget)
			{
				if (!ReserveTurnStep(pThis, pState->Anchor, currentBucket,
					pState->ReconcileTarget, pState->ReconcileClockwise, pState->Alt))
				{
					ReleaseReservation(pThis);
					pState->Locked = false;
					pState->ReconcileActive = false;
					pState->ReconcileTarget = -1;
					pThis->PrimaryFacing.SetCurrent(DirStruct(currentBucket * 0x2000));

					Debug::CarrierLog("[FootprintAim] %s blocked at bucket %d, aborted at (%d,%d)\n",
						pThis->GetTechnoType()->ID, currentBucket,
						pState->Anchor.X, pState->Anchor.Y);
					return;
				}
			}
		}

		if (currentBucket == pState->ReconcileTarget)
		{
			const auto targetSpan = GetSpan(pThis, pState->Anchor, pState->ReconcileTarget);
			CommitSpan(pThis, targetSpan, pState->ReconcileTarget, pState->Alt, pState->Anchor);
			ReleaseReservation(pThis);
			pState->Locked = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			Debug::CarrierLog("[FootprintAim] %s finished bucket %d at (%d,%d)\n",
				pThis->GetTechnoType()->ID, currentBucket,
				pState->Anchor.X, pState->Anchor.Y);
		}
		else if (!pThis->PrimaryFacing.IsRotating())
		{
			// The desired direction changed (or the rotation was interrupted)
			// and the old target will never be reached. Commit whatever bucket
			// the hull actually reached and release the sweep.
			const auto currentSpan = GetSpan(pThis, pState->Anchor, currentBucket);
			CommitSpan(pThis, currentSpan, currentBucket, pState->Alt, pState->Anchor);
			ReleaseReservation(pThis);
			pState->Locked = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;

			Debug::CarrierLog("[FootprintAim] %s interrupted, committed bucket %d at (%d,%d)\n",
				pThis->GetTechnoType()->ID, currentBucket,
				pState->Anchor.X, pState->Anchor.Y);
		}

		return;
	}

	if (currentBucket == pState->Bucket)
		return;

	const int from = pState->Bucket;
	const int desiredBucket = BucketFromFacing(pThis->PrimaryFacing.Desired());
	int target = desiredBucket != from ? desiredBucket : currentBucket;

	if (target == from)
		return;

	const int delta = (target - from + 8) & 7;
	const bool tryClockwise = delta <= 4;
	const bool tryCounterClockwise = delta >= 4;

	auto tryDirection = [&](bool clockwise) -> bool
	{
		if (!ReserveTurnStep(pThis, pState->Anchor, from, target,
			clockwise, pState->Alt))
		{
			return false;
		}

		pState->Locked = true;
		pState->ReconcileActive = true;
		pState->ReconcileTarget = target;
		pState->ReconcileClockwise = clockwise;

		Debug::CarrierLog("[FootprintAim] %s bucket %d -> %d (%s) at (%d,%d), reserved=%d\n",
			pThis->GetTechnoType()->ID, from, target,
			clockwise ? "cw" : "ccw",
			pState->Anchor.X, pState->Anchor.Y,
			static_cast<int>(pState->ReservedSpan.size()));

		return true;
	};

	if (tryClockwise && tryDirection(true))
		return;

	if (tryCounterClockwise && tryDirection(false))
		return;

	// The hull cannot sweep to the aim direction. Keep the visual hull aligned
	// with the collision footprint instead of showing a turn that never happens.
	pThis->PrimaryFacing.SetCurrent(DirStruct(from * 0x2000));

	static int lastLogFrame = -1000;

	if (Unsorted::CurrentFrame - lastLogFrame >= 30)
	{
		lastLogFrame = Unsorted::CurrentFrame;

		Debug::CarrierLog("[FootprintAim] %s blocked %d -> %d at (%d,%d), snapped back\n",
			pThis->GetTechnoType()->ID, from, target,
			pState->Anchor.X, pState->Anchor.Y);
	}
}

void Footprint::DumpCarrierState()
{
	Debug::Log("[CarrierDump] begin: units=%d payloadMap=%d\n",
		static_cast<int>(UnitClass::Array.Count),
		static_cast<int>(s_carrierPayloads.size()));

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || !pUnit->IsAlive || !IsMultiCell(pUnit))
			continue;

		const auto pType = pUnit->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
		const auto pState = GetState(pUnit);

		if (!pState)
			continue;

		Debug::Log("[CarrierDump] unit=%s ptr=%p loc=(%d,%d,%d) cell=(%d,%d)"
			" init=%d locked=%d translating=%d bucket=%d alt=%d"
			" span=%d barrier=%d reserved=%d reconcile=%d/%d\n",
			pType->ID, static_cast<void*>(pUnit),
			pUnit->Location.X, pUnit->Location.Y, pUnit->Location.Z,
			pUnit->GetMapCoords().X, pUnit->GetMapCoords().Y,
			pState->Initialized ? 1 : 0, pState->Locked ? 1 : 0,
			pState->Translating ? 1 : 0, pState->Bucket, pState->Alt ? 1 : 0,
			static_cast<int>(pState->Span.size()),
			static_cast<int>(pState->BarrierSpan.size()),
			static_cast<int>(pState->ReservedSpan.size()),
			pState->ReconcileActive ? 1 : 0, pState->ReconcileTarget);

		if (!TechnoTypeExt::IsAircraftCarrier(pType) || !pTypeExt)
			continue;

		for (size_t i = 0; i < pState->CarrierDockBuildings.size(); ++i)
		{
			auto const pDock = pState->CarrierDockBuildings[i];

			if (!pDock)
			{
				Debug::Log("[CarrierDump]   slot %d dock=<null>\n", static_cast<int>(i));
				continue;
			}

			const auto dockCoords = pDock->GetDockCoords(nullptr);
			Debug::Log("[CarrierDump]   slot %d dock=%p type=%s alive=%d limbo=%d loc=(%d,%d,%d)"
				" linkCapacity=%d dockCoords=(%d,%d,%d)\n",
				static_cast<int>(i), static_cast<void*>(pDock),
				pDock->Type ? pDock->Type->ID : "?",
				pDock->IsAlive ? 1 : 0, pDock->InLimbo ? 1 : 0,
				pDock->Location.X, pDock->Location.Y, pDock->Location.Z,
				static_cast<int>(pDock->RadioLinks.Capacity),
				dockCoords.X, dockCoords.Y, dockCoords.Z);
		}

		for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
		{
			auto const pAircraft = pState->CarrierPayloads[i];

			if (!pAircraft)
			{
				Debug::Log("[CarrierDump]   payload %d = <null>\n", static_cast<int>(i));
				continue;
			}

			const auto expected = GetCarrierDockSlotCoords(pUnit, pTypeExt, i);
			Debug::Log("[CarrierDump]   payload %d ptr=%p type=%s alive=%d limbo=%d mission=%d"
				" inAir=%d ammo=%d health=%d loc=(%d,%d,%d) expected=(%d,%d,%d)"
				" pushed=%d parked=%d\n",
				static_cast<int>(i), static_cast<void*>(pAircraft),
				pAircraft->Type ? pAircraft->Type->ID : "?",
				pAircraft->IsAlive ? 1 : 0, pAircraft->InLimbo ? 1 : 0,
				static_cast<int>(pAircraft->GetCurrentMission()),
				pAircraft->IsInAir() ? 1 : 0, pAircraft->Ammo,
				pAircraft->Health,
				pAircraft->Location.X, pAircraft->Location.Y, pAircraft->Location.Z,
				expected.X, expected.Y, expected.Z,
				(pAircraft->Destination ? 1 : 0),
				IsCarrierPayloadParked(pAircraft) ? 1 : 0);
		}
	}

	Debug::Log("[CarrierDump] end\n");
}

void Footprint::OnPointerInvalidated(void* ptr)
{
	if (!ptr || s_carrierPayloads.empty())
		return;

	for (auto it = s_carrierPayloads.begin(); it != s_carrierPayloads.end(); )
	{
		if (it->first == ptr || it->second == ptr)
			it = s_carrierPayloads.erase(it);
		else
			++it;
	}
}

void Footprint::OnUnitDestroyed(UnitClass* pThis)
{
	if (!pThis)
		return;

	auto const pState = GetState(pThis);

	if (!pState)
		return;

	// Parked aircraft are part of the deck: they die with the ship. Airborne
	// payloads simply lose their home and continue as normal aircraft.
	for (size_t i = 0; i < pState->CarrierPayloads.size(); ++i)
	{
		auto const pAircraft = pState->CarrierPayloads[i];

		if (!pAircraft)
			continue;

		s_carrierPayloads.erase(pAircraft);

		const bool parked = i < pState->CarrierPayloadDocked.size()
			&& pState->CarrierPayloadDocked[i] != 0;

		if (parked && pAircraft->IsAlive && !pAircraft->InLimbo)
		{
			pAircraft->Limbo();
			pAircraft->UnInit();
		}
	}

	pState->CarrierPayloads.clear();
	pState->CarrierPayloadDocked.clear();
	pState->CarrierPayloadLastCoords.clear();
	pState->CarrierPayloadReloadTimer.clear();
	pState->CarrierPayloadsInitialized = false;

	// Legacy / pre-rewrite dock proxies cannot outlive the carrier: they are
	// immune to damage, so nothing else would ever remove them from the map.
	ReleaseDockProxies(pState);

	Debug::Log("[AircraftCarrier] released deck of destroyed %s\n",
		pThis->GetTechnoType() ? pThis->GetTechnoType()->ID : "?");
}
