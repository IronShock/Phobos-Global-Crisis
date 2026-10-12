#include "Formation.h"

#include <Phobos.h>
#include <TacticalClass.h>
#include <WWMouseClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <ObjectClass.h>
#include <Helpers/Cast.h>
#include <Surface.h>
#include <Drawing.h>
#include <GeneralDefinitions.h>
#include <TechnoClass.h>
#include <UnitTypeClass.h>
#include <InfantryTypeClass.h>
#include <FileSystem.h>
#include <Dir.h>
#include <Facing.h>
#include <MissionClass.h>
#include <FileFormats/SHP.h>
#include <Matrix3D.h>
#include <MapClass.h>
#include <CellClass.h>
#include <Ext/TechnoType/Body.h>

#include <cmath>
#include <algorithm>

FormationManager FormationManager::Instance;

// Minimum drag distance in leptons (about 0.2 cells)
static const int MIN_DRAG_LEPTONS = 50;

// 1 cell = 256 leptons
static const int LEPTONS_PER_CELL = 256;

// Spacing used by one unit: infantry line up tighter than vehicles/aircraft.
static int GetUnitSpacing(FootClass* pFoot)
{
	if (pFoot && pFoot->WhatAmI() == AbstractType::Infantry)
		return Phobos::Config::FormationSpacingInfantry;

	return Phobos::Config::FormationSpacing;
}

// Order the selected units so infantry slot in BETWEEN vehicles (vehicle-first
// round robin: V I V I ...). The order is then used consistently by the layout,
// the preview and the execution, which all pair positions[i] with units[i].
static void OrderUnitsForFormation(std::vector<FootClass*>& units)
{
	std::vector<FootClass*> vehicles;
	std::vector<FootClass*> infantry;
	vehicles.reserve(units.size());
	infantry.reserve(units.size());

	for (auto const pFoot : units)
	{
		if (pFoot && pFoot->WhatAmI() == AbstractType::Infantry)
			infantry.push_back(pFoot);
		else
			vehicles.push_back(pFoot);
	}

	std::vector<FootClass*> ordered;
	ordered.reserve(units.size());

	size_t vi = 0;
	size_t ii = 0;

	while (vi < vehicles.size() || ii < infantry.size())
	{
		if (vi < vehicles.size())
			ordered.push_back(vehicles[vi++]);

		if (ii < infantry.size())
			ordered.push_back(infantry[ii++]);
	}

	units.swap(ordered);
}

bool FormationManager::IsMiddleButtonPressed() const
{
	return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

Point2D FormationManager::GetMouseScreenPos() const
{
	Point2D pos;
	WWMouseClass::Instance->GetCoords(&pos);
	return pos;
}

bool FormationManager::ShouldActivate() const
{
	for (auto const pObject : ObjectClass::CurrentObjects)
	{
		if (auto const pFoot = abstract_cast<FootClass*>(pObject))
		{
			if (pFoot->Owner->IsControlledByCurrentPlayer())
				return true;
		}
	}
	return false;
}

std::vector<FootClass*> FormationManager::GetSelectedFoots() const
{
	std::vector<FootClass*> result;
	for (auto const pObject : ObjectClass::CurrentObjects)
	{
		if (auto const pFoot = abstract_cast<FootClass*>(pObject))
		{
			if (pFoot->Owner->IsControlledByCurrentPlayer())
				result.push_back(pFoot);
		}
	}
	return result;
}

int FormationManager::GetDragDistanceWorld() const
{
	Point2D currentPos = GetMouseScreenPos();
	CoordStruct currentWorld = TacticalClass::Instance->ClientToCoords(currentPos);

	int dx = currentWorld.X - m_startWorldPos.X;
	int dy = currentWorld.Y - m_startWorldPos.Y;
	return static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
}

FormationType FormationManager::DetermineFormationType(int dragWorld) const
{
	// Near (within 6 cells): Linear
	// Medium (6-12 cells): Pyramid
	// Far (beyond 12 cells): Wedge
	int nearThreshold = Phobos::Config::FormationNearThreshold;
	int farThreshold = Phobos::Config::FormationFarThreshold;

	if (dragWorld < nearThreshold)
		return FormationType::Linear;
	else if (dragWorld < farThreshold)
		return FormationType::Pyramid;
	else
		return FormationType::Wedge;
}

void FormationManager::CalculateDirectionVectors(double& fwdX, double& fwdY, double& rightX, double& rightY) const
{
	Point2D currentPos = GetMouseScreenPos();
	CoordStruct currentWorld = TacticalClass::Instance->ClientToCoords(currentPos);

	double dx = static_cast<double>(currentWorld.X - m_startWorldPos.X);
	double dy = static_cast<double>(currentWorld.Y - m_startWorldPos.Y);
	double len = std::sqrt(dx * dx + dy * dy);

	if (len < 1.0)
	{
		fwdX = 0.0;
		fwdY = 1.0;
	}
	else
	{
		fwdX = dx / len;
		fwdY = dy / len;
	}

	rightX = -fwdY;
	rightY = fwdX;
}

DirStruct FormationManager::CalculateFormationDir(double fwdX, double fwdY) const
{
	// World coords: X=east, Y=south
	// DirStruct radian: 0=East, pi/2=North (standard math convention)
	// So rad = atan2(-fwdY, fwdX) to convert from game coords to math convention
	double rad = std::atan2(-fwdY, fwdX);
	return DirStruct(rad);
}

void FormationManager::CalculateFormationPositions(std::vector<CoordStruct>& positions,
	FormationType type, int count, double fwdX, double fwdY, double rightX, double rightY,
	const std::vector<FootClass*>& units) const
{
	positions.clear();
	positions.reserve(count);

	if (count <= 0)
		return;

	const double vehicleSpacing = static_cast<double>(Phobos::Config::FormationSpacing);

	// Per-unit spacing by index (falls back to the vehicle spacing if the units
	// list is shorter than the count).
	auto const spacingAt = [&](int i) -> double
	{
		return (i >= 0 && i < static_cast<int>(units.size()))
			? static_cast<double>(GetUnitSpacing(units[i])) : vehicleSpacing;
	};

	switch (type)
	{
	case FormationType::Linear:
	{
		// Cumulative side offsets along the right vector, in unit order, so
		// infantry (smaller spacing) pack tighter and fall between vehicles.
		std::vector<double> c(count, 0.0);
		for (int i = 1; i < count; ++i)
			c[i] = c[i - 1] + (spacingAt(i - 1) + spacingAt(i)) * 0.5;

		const double center = c[count - 1] * 0.5;

		for (int i = 0; i < count; ++i)
		{
			const double sideDist = c[i] - center;
			positions.push_back({
				m_startWorldPos.X + static_cast<int>(rightX * sideDist),
				m_startWorldPos.Y + static_cast<int>(rightY * sideDist),
				m_startWorldPos.Z
			});
		}
		break;
	}
	case FormationType::Pyramid:
	{
		// Pyramid with base at front (widest row closest to drag direction).
		int maxRow = 0;
		while ((maxRow + 1) * (maxRow + 2) / 2 < count)
			maxRow++;

		// Assign units to rows in order (row 0 = widest front row).
		std::vector<std::vector<int>> rows;
		{
			int unitIdx = 0;
			for (int row = 0; row <= maxRow && unitIdx < count; ++row)
			{
				int unitsInRow = maxRow - row + 1;
				if (unitsInRow < 1)
					unitsInRow = 1;

				std::vector<int> rowUnits;
				for (int j = 0; j < unitsInRow && unitIdx < count; ++j, ++unitIdx)
					rowUnits.push_back(unitIdx);

				rows.push_back(rowUnits);
			}
		}

		const int rowCount = static_cast<int>(rows.size());

		// Row depth spacing = the widest unit spacing in the row (so all-infantry
		// rows sit closer together front-to-back).
		std::vector<double> rowSpacing(rowCount, vehicleSpacing);
		for (int r = 0; r < rowCount; ++r)
		{
			double mx = 0.0;
			for (int ui : rows[r])
				mx = std::max(mx, spacingAt(ui));
			if (mx <= 0.0)
				mx = vehicleSpacing;
			rowSpacing[r] = mx;
		}

		// Cumulative forward offsets between rows, centered on the start point.
		std::vector<double> fwdOffset(rowCount, 0.0);
		for (int r = 1; r < rowCount; ++r)
			fwdOffset[r] = fwdOffset[r - 1] + (rowSpacing[r - 1] + rowSpacing[r]) * 0.5;

		const double depthCenter = (rowCount > 0) ? fwdOffset[rowCount - 1] * 0.5 : 0.0;

		for (int r = 0; r < rowCount; ++r)
		{
			const auto& rowUnits = rows[r];
			const int n = static_cast<int>(rowUnits.size());

			// Cumulative side offsets within the row.
			std::vector<double> c(n, 0.0);
			for (int k = 1; k < n; ++k)
				c[k] = c[k - 1] + (spacingAt(rowUnits[k - 1]) + spacingAt(rowUnits[k])) * 0.5;

			const double sideCenter = (n > 0) ? c[n - 1] * 0.5 : 0.0;
			const double fwdDist = depthCenter - fwdOffset[r];

			for (int k = 0; k < n; ++k)
			{
				const double sideDist = c[k] - sideCenter;
				positions.push_back({
					m_startWorldPos.X + static_cast<int>(fwdX * fwdDist + rightX * sideDist),
					m_startWorldPos.Y + static_cast<int>(fwdY * fwdDist + rightY * sideDist),
					m_startWorldPos.Z
				});
			}
		}
		break;
	}
	case FormationType::Wedge:
	{
		// Wedge: 1 unit at the tip, then pairs expanding outward.
		std::vector<std::vector<int>> rows;
		{
			int unitIdx = 0;
			if (unitIdx < count)
				rows.push_back({ unitIdx++ });

			while (unitIdx < count)
			{
				std::vector<int> rowUnits;
				rowUnits.push_back(unitIdx++);
				if (unitIdx < count)
					rowUnits.push_back(unitIdx++);
				rows.push_back(rowUnits);
			}
		}

		const int rowCount = static_cast<int>(rows.size());

		std::vector<double> rowSpacing(rowCount, vehicleSpacing);
		for (int r = 0; r < rowCount; ++r)
		{
			double mx = 0.0;
			for (int ui : rows[r])
				mx = std::max(mx, spacingAt(ui));
			if (mx <= 0.0)
				mx = vehicleSpacing;
			rowSpacing[r] = mx;
		}

		// Cumulative forward offsets and lateral half-spread; both scale with the
		// per-row spacing so infantry wedges stay tight.
		std::vector<double> fwdOffset(rowCount, 0.0);
		std::vector<double> lateral(rowCount, 0.0);
		for (int r = 1; r < rowCount; ++r)
		{
			fwdOffset[r] = fwdOffset[r - 1] + (rowSpacing[r - 1] + rowSpacing[r]) * 0.5;
			lateral[r] = lateral[r - 1] + (rowSpacing[r - 1] + rowSpacing[r]) * 0.5;
		}

		const double depthCenter = (rowCount > 0) ? fwdOffset[rowCount - 1] * 0.5 : 0.0;

		for (int r = 0; r < rowCount; ++r)
		{
			const auto& rowUnits = rows[r];
			const int n = static_cast<int>(rowUnits.size());
			const double fwdDist = depthCenter - fwdOffset[r];

			for (int k = 0; k < n; ++k)
			{
				// Single-unit rows sit on the centre line; pairs straddle it.
				const double sideDist = (n >= 2) ? ((k == 0) ? -lateral[r] : lateral[r]) : 0.0;

				positions.push_back({
					m_startWorldPos.X + static_cast<int>(fwdX * fwdDist + rightX * sideDist),
					m_startWorldPos.Y + static_cast<int>(fwdY * fwdDist + rightY * sideDist),
					m_startWorldPos.Z
				});
			}
		}
		break;
	}
	default:
		break;
	}

	// Adjust positions that fall on impassable terrain
	for (size_t i = 0; i < positions.size() && i < units.size(); i++)
	{
		if (!IsPositionPassable(positions[i], units[i]))
		{
			positions[i] = FindNearestPassable(positions[i], units[i], fwdX, fwdY);
		}
	}
}

bool FormationManager::IsPositionPassable(const CoordStruct& pos, FootClass* pFoot) const
{
	if (!pFoot)
		return true;

	auto pType = pFoot->GetTechnoType();
	if (!pType)
		return true;

	auto pCell = MapClass::Instance.TryGetCellAt(pos);
	if (!pCell)
		return false;

	return pCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, true);
}

CoordStruct FormationManager::FindNearestPassable(const CoordStruct& pos, FootClass* pFoot,
	double fwdX, double fwdY) const
{
	// Search in expanding rings around the original position
	// Try forward/backward along formation direction first, then lateral offsets
	int cellSize = LEPTONS_PER_CELL;

	for (int radius = 1; radius <= 5; radius++)
	{
		// Try forward and backward
		for (int sign : {1, -1})
		{
			CoordStruct testPos = {
				pos.X + static_cast<int>(fwdX * radius * cellSize * sign),
				pos.Y + static_cast<int>(fwdY * radius * cellSize * sign),
				pos.Z
			};
			if (IsPositionPassable(testPos, pFoot))
				return testPos;
		}

		// Try lateral offsets (perpendicular)
		double perpX = -fwdY;
		double perpY = fwdX;
		for (int sign : {1, -1})
		{
			CoordStruct testPos = {
				pos.X + static_cast<int>(perpX * radius * cellSize * sign),
				pos.Y + static_cast<int>(perpY * radius * cellSize * sign),
				pos.Z
			};
			if (IsPositionPassable(testPos, pFoot))
				return testPos;
		}

		// Try diagonal offsets
		for (int sx : {1, -1})
		{
			for (int sy : {1, -1})
			{
				CoordStruct testPos = {
					pos.X + static_cast<int>((fwdX * sx + perpX * sy) * radius * cellSize / 2),
					pos.Y + static_cast<int>((fwdY * sx + perpY * sy) * radius * cellSize / 2),
					pos.Z
				};
				if (IsPositionPassable(testPos, pFoot))
					return testPos;
			}
		}
	}

	// Fallback: return original position
	return pos;
}

void FormationManager::DrawUnitPreview(FootClass* pFoot, const Point2D& screenPos, const CoordStruct& worldPos,
	const DirStruct& formationDir) const
{
	if (!pFoot)
		return;

	auto pType = pFoot->GetTechnoType();
	if (!pType)
		return;

	// Skip VXL units - cannot easily draw them semi-transparently
	if (pType->Voxel)
		return;

	SHPStruct* pSHP = pType->GetImage();
	if (!pSHP || pSHP->Frames <= 0)
		return;

	ConvertClass* pPalette = pFoot->GetDrawer();
	if (!pPalette)
		return;

	auto pSurface = DSurface::Temp;
	if (!pSurface)
		return;

	RectangleStruct bounds = pSurface->GetRect();
	BlitterFlags flags = BlitterFlags::TransLucent25 | BlitterFlags::Centered;

	auto whatAmI = pFoot->WhatAmI();

	if (whatAmI == AbstractType::Unit)
	{
		auto pUnitType = static_cast<UnitTypeClass*>(pType);
		int facings = pUnitType->Facings;
		if (facings <= 0)
			facings = 32;

		// YR SHP frames are offset by 45 degrees from DirStruct convention.
		// Frame i shows direction (i * 360/Facings - 45 degrees).
		// Use GetFacing with offset = Facings/8 to compensate (equals 45 degrees).
		int facingOffset = facings / 8;
		int facing = 0;
		switch (facings)
		{
		case 8:  facing = static_cast<int>(formationDir.GetFacing<8>(facingOffset));  break;
		case 16: facing = static_cast<int>(formationDir.GetFacing<16>(facingOffset)); break;
		case 32: facing = static_cast<int>(formationDir.GetFacing<32>(facingOffset)); break;
		default: facing = static_cast<int>(formationDir.GetFacing<32>(4)); break;
		}

		int standFrames = std::max(1, pUnitType->StandingFrames);
		int bodyFrame = pUnitType->StartStandFrame + facing * standFrames;

		if (bodyFrame >= 0 && bodyFrame < pSHP->Frames)
		{
			pSurface->DrawSHP(pPalette, pSHP, bodyFrame, &screenPos, &bounds,
				flags, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		}

		// Draw turret if the unit has one and SHP has enough frames
		if (pType->Turret)
		{
			int turretStart = pUnitType->StartStandFrame + facings * standFrames;
			int turretFrame = turretStart + facing * standFrames;

			if (turretFrame >= 0 && turretFrame < pSHP->Frames)
			{
				// Calculate turret screen position using the same Matrix3D approach
				// as the game's UnitClass_DrawSHP_TurretOffest hook
				Matrix3D mtx = Matrix3D::GetIdentity();
				mtx.RotateZ(static_cast<float>(formationDir.GetRadian<32>()));
				TechnoTypeExt::ApplyTurretOffset(pType, &mtx);

				const auto res = mtx.GetTranslation();
				const auto location = CoordStruct
				{
					static_cast<int>(res.X),
					static_cast<int>(-res.Y),
					static_cast<int>(res.Z)
				};

				Point2D screenDelta = TacticalClass::CoordsToScreen(location);
				Point2D turretScreenPos = { screenPos.X + screenDelta.X, screenPos.Y + screenDelta.Y };

				pSurface->DrawSHP(pPalette, pSHP, turretFrame, &turretScreenPos, &bounds,
					flags, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
			}
		}
	}
	else if (whatAmI == AbstractType::Infantry)
	{
		auto pInfType = static_cast<InfantryTypeClass*>(pType);
		if (pInfType->Sequence)
		{
			auto& readySeq = pInfType->Sequence->GetSequence(Sequence::Ready);
			// Infantry uses 8 facings, offset by 1 (= 45 degrees)
			int facing = static_cast<int>(formationDir.GetFacing<8>(1));
			int facingMult = std::max(1, readySeq.FacingMultiplier);
			int bodyFrame = readySeq.StartFrame + facing * facingMult;

			if (bodyFrame >= 0 && bodyFrame < pSHP->Frames)
			{
				pSurface->DrawSHP(pPalette, pSHP, bodyFrame, &screenPos, &bounds,
					flags, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
			}
		}
	}
	else if (whatAmI == AbstractType::Aircraft)
	{
		// SHP aircraft: use 32 facings with offset
		int facing = static_cast<int>(formationDir.GetFacing<32>(4));
		if (facing >= 0 && facing < pSHP->Frames)
		{
			pSurface->DrawSHP(pPalette, pSHP, facing, &screenPos, &bounds,
				flags, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		}
	}
}

void FormationManager::DrawFormationPreview(FormationType type, const std::vector<CoordStruct>& positions,
	const std::vector<FootClass*>& units, const DirStruct& formationDir) const
{
	auto pSurface = DSurface::Temp;
	if (!pSurface)
		return;

	// Use a single color for all formation types (no type label shown)
	COLORREF color = RGB(255, 255, 255);

	std::vector<Point2D> screenPositions;
	for (const auto& worldPos : positions)
	{
		auto [screenPos, visible] = TacticalClass::Instance->CoordsToClient(worldPos);
		screenPositions.push_back(screenPos);
	}

	// Draw semi-transparent unit images at each position
	for (size_t i = 0; i < screenPositions.size() && i < units.size(); i++)
	{
		DrawUnitPreview(units[i], screenPositions[i], positions[i], formationDir);
	}

	// Draw formation direction arrow from start position
	{
		// Calculate forward direction from formationDir
		double rad = formationDir.GetRadian<65536>();
		double fwdX = std::cos(rad);
		double fwdY = -std::sin(rad);  // Game Y = South, math Y = North

		// Arrow endpoint: 200 leptons forward from start
		int arrowLen = 200;
		CoordStruct arrowEnd = {
			m_startWorldPos.X + static_cast<int>(fwdX * arrowLen),
			m_startWorldPos.Y + static_cast<int>(fwdY * arrowLen),
			m_startWorldPos.Z
		};

		auto [startScreen, startVis] = TacticalClass::Instance->CoordsToClient(m_startWorldPos);
		auto [endScreen, endVis] = TacticalClass::Instance->CoordsToClient(arrowEnd);

		if (startVis || endVis)
		{
			// Draw arrow shaft
			pSurface->DrawLine(&startScreen, &endScreen, color);

			// Draw arrowhead
			int dx = endScreen.X - startScreen.X;
			int dy = endScreen.Y - startScreen.Y;
			double len = std::sqrt(static_cast<double>(dx * dx + dy * dy));
			if (len > 1.0)
			{
				double ux = dx / len;
				double uy = dy / len;
				int ah = 10;
				// Arrowhead: two lines going backwards at ~30 degrees from shaft
				Point2D ah1 = {
					endScreen.X - static_cast<int>(ux * ah) - static_cast<int>(uy * ah * 0.6),
					endScreen.Y - static_cast<int>(uy * ah) + static_cast<int>(ux * ah * 0.6)
				};
				Point2D ah2 = {
					endScreen.X - static_cast<int>(ux * ah) + static_cast<int>(uy * ah * 0.6),
					endScreen.Y - static_cast<int>(uy * ah) - static_cast<int>(ux * ah * 0.6)
				};
				pSurface->DrawLine(&endScreen, &ah1, color);
				pSurface->DrawLine(&endScreen, &ah2, color);
			}
		}
	}
}

void FormationManager::ExecuteFormation(const std::vector<CoordStruct>& positions,
	const std::vector<FootClass*>& units, const DirStruct& formationDir)
{
	if (positions.size() != units.size() || units.empty())
		return;

	int houseIndex = HouseClass::CurrentPlayer->ArrayIndex;

	for (size_t i = 0; i < units.size() && i < positions.size(); i++)
	{
		auto pFoot = units[i];
		if (!pFoot)
			continue;

		const auto& destCoord = positions[i];

		// Set desired facing to formation direction (DirStruct N=0 convention)
		pFoot->PrimaryFacing.SetDesired(formationDir);
		pFoot->SecondaryFacing.SetDesired(formationDir);

		// Queue move command
		EventClass e(
			houseIndex,
			TargetClass(pFoot),
			Mission::Move,
			TargetClass(),
			TargetClass(destCoord),
			TargetClass()
		);

		EventClass::OutList.Add(e);

		// Add to pending facings so we can re-apply facing after arrival
		m_pendingFacings[pFoot] = { formationDir, 0, false };
	}
}

void FormationManager::ProcessPendingFacings()
{
	for (auto it = m_pendingFacings.begin(); it != m_pendingFacings.end(); )
	{
		auto pFoot = it->first;
		auto& pending = it->second;

		// Remove dead or invalid units
		if (!pFoot || !pFoot->IsAlive || pFoot->Health <= 0)
		{
			it = m_pendingFacings.erase(it);
			continue;
		}

		pending.FramesPending++;

		if (!pending.HasStartedMoving)
		{
			if (pFoot->CurrentMission == Mission::Move)
			{
				pending.HasStartedMoving = true;
			}
			else if (pending.FramesPending > 30)
			{
				pFoot->PrimaryFacing.SetDesired(pending.Facing);
				pFoot->SecondaryFacing.SetDesired(pending.Facing);
				it = m_pendingFacings.erase(it);
				continue;
			}
		}
		else
		{
			if (pFoot->CurrentMission != Mission::Move)
			{
				pFoot->PrimaryFacing.SetDesired(pending.Facing);
				pFoot->SecondaryFacing.SetDesired(pending.Facing);
				it = m_pendingFacings.erase(it);
				continue;
			}
		}

		if (pending.FramesPending > 600)
		{
			pFoot->PrimaryFacing.SetDesired(pending.Facing);
			pFoot->SecondaryFacing.SetDesired(pending.Facing);
			it = m_pendingFacings.erase(it);
			continue;
		}

		++it;
	}
}

void FormationManager::Process()
{
	if (!Phobos::Config::FormationEnabled)
		return;

	ProcessPendingFacings();

	bool currentPressed = IsMiddleButtonPressed();

	if (currentPressed && !m_wasPressed)
	{
		m_startScreenPos = GetMouseScreenPos();
		m_startWorldPos = TacticalClass::Instance->ClientToCoords(m_startScreenPos);
		m_isActive = ShouldActivate();
	}

	if (m_isActive && currentPressed)
	{
		int dragWorld = GetDragDistanceWorld();

		if (dragWorld >= MIN_DRAG_LEPTONS)
		{
			FormationType type = DetermineFormationType(dragWorld);

			if (type != FormationType::None)
			{
				double fwdX, fwdY, rightX, rightY;
				CalculateDirectionVectors(fwdX, fwdY, rightX, rightY);

				DirStruct formationDir = CalculateFormationDir(fwdX, fwdY);

				auto units = GetSelectedFoots();
				OrderUnitsForFormation(units);
				if (!units.empty())
				{
					std::vector<CoordStruct> positions;
					CalculateFormationPositions(positions, type, static_cast<int>(units.size()),
						fwdX, fwdY, rightX, rightY, units);
					DrawFormationPreview(type, positions, units, formationDir);
				}
			}
		}
	}

	if (!currentPressed && m_wasPressed && m_isActive)
	{
		int dragWorld = GetDragDistanceWorld();

		if (dragWorld >= MIN_DRAG_LEPTONS)
		{
			FormationType type = DetermineFormationType(dragWorld);

			if (type != FormationType::None)
			{
				double fwdX, fwdY, rightX, rightY;
				CalculateDirectionVectors(fwdX, fwdY, rightX, rightY);

				DirStruct formationDir = CalculateFormationDir(fwdX, fwdY);

				auto units = GetSelectedFoots();
				OrderUnitsForFormation(units);
				if (!units.empty())
				{
					std::vector<CoordStruct> positions;
					CalculateFormationPositions(positions, type, static_cast<int>(units.size()),
						fwdX, fwdY, rightX, rightY, units);
					ExecuteFormation(positions, units, formationDir);
				}
			}
		}

		m_isActive = false;
	}

	m_wasPressed = currentPressed;
}
