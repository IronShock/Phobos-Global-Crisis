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
#include <IndexClass.h>
#include <TargetClass.h>
#include <FlyLocomotionClass.h>
#include <JumpjetLocomotionClass.h>
#include <Ext/TechnoType/Body.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

FormationManager FormationManager::Instance;

// Minimum drag distance in leptons (about 0.2 cells)
static const int MIN_DRAG_LEPTONS = 50;

// 1 cell = 256 leptons
static const int LEPTONS_PER_CELL = 256;

namespace
{
	bool IsAirUnit(const FootClass* pFoot);

	// Formation preview is a client-side drawing path. The VXL cache belongs to
	// the type and is reused by the engine's normal unit drawing, so drawing the
	// preview must not allocate a fresh cache (and leak its buffers) every frame.
	VoxelIndexKey MakeFormationMainKey(int facing)
	{
		VoxelIndexKey key(0);
		key.MainVoxel.FrameIndex = 0;
		key.MainVoxel.BodyFacing = static_cast<unsigned>(facing);
		key.MainVoxel.RampType = 0;
		key.MainVoxel.AuxVoxel = 0;
		return key;
	}

	VoxelIndexKey MakeFormationAircraftKey(int facing)
	{
		VoxelIndexKey key(0);
		key.AircraftVoxel.BodyFacing = static_cast<unsigned>(facing);
		key.AircraftVoxel.RampType = 0;
		key.AircraftVoxel.FrameIndex = 0;
		return key;
	}

	VoxelIndexKey MakeFormationTurretKey(int bodyFacing, int turretFacing)
	{
		VoxelIndexKey key(0);
		key.MinorVoxel.TurretFacing = static_cast<unsigned>(turretFacing);
		key.MinorVoxel.BodyFacing = static_cast<unsigned>(bodyFacing);
		key.MinorVoxel.RampType = 0;
		key.MinorVoxel.TurretFrameIndex = 0;
		key.MinorVoxel.TurretWeaponIndex = 0;
		return key;
	}

	// Draw a voxel unit into the tactical surface at the drag preview position.
	// The engine's VXL matrix uses a four-step offset compared with the facing
	// key; the same convention is used by the unit status-panel voxel renderer.
	bool DrawVoxelUnitPreview(FootClass* pFoot, TechnoTypeClass* pType,
		const Point2D& screenPos, const DirStruct& formationDir, DSurface* pSurface)
	{
		if (!pFoot || !pType || !pSurface || !pType->Voxel)
			return false;

		if (!pType->MainVoxel.VXL || !pType->MainVoxel.HVA)
			return false;

		if (!pFoot->GetDrawer())
			return false;

		const int desiredFacing = static_cast<int>(formationDir.GetFacing<32>());
		const bool aircraft = pType->WhatAmI() == AbstractType::AircraftType;

		// The engine's ground UnitClass Draw_Matrix lowers the facing by 8
		// before building the Z rotation, which is exactly GetRadian<32>() on
		// the desired facing. Do not apply the old UnitPortrait -4 offset here:
		// that turned every ground VXL 45 degrees away from the arrow.
		Matrix3D facingMtx = Matrix3D::GetIdentity();
		facingMtx.RotateZ(static_cast<float>(
			DirStruct(desiredFacing * 2048).GetRadian<32>()));
		Matrix3D bodyMtx = Matrix3D::VoxelDefaultMatrix * facingMtx;

		RectangleStruct bounds = DSurface::ViewBounds;
		Point2D center = screenPos;
		const int bodyKey = (aircraft
			? MakeFormationAircraftKey(desiredFacing)
			: MakeFormationMainKey(desiredFacing)).Value;

		// Voxel blitting is reliable with the engine's normal alpha/flat path.
		// (The SHP preview can use TransLucent25, but VXL transparency is not
		// consistently supported by the engine's voxel blitters.)
		constexpr BlitterFlags flags = BlitterFlags::Alpha | BlitterFlags::Flat;

		pFoot->Draw_A_VXL(&pType->MainVoxel, 0, bodyKey, &pType->VoxelMainCache,
			&bounds, &center, &bodyMtx, 1000, flags, 0);

		if (pType->Turret && pType->TurretVoxel.VXL && pType->TurretVoxel.HVA)
		{
			const int turretKey = MakeFormationTurretKey(desiredFacing, desiredFacing).Value;

			pFoot->Draw_A_VXL(&pType->TurretVoxel, 0, turretKey,
				&pType->VoxelTurretWeaponCache, &bounds, &center,
				&bodyMtx, 1000, flags, 0);

			if (pType->BarrelVoxel.VXL && pType->BarrelVoxel.HVA)
			{
				pFoot->Draw_A_VXL(&pType->BarrelVoxel, 0, turretKey,
					&pType->VoxelTurretBarrelCache, &bounds, &center,
					&bodyMtx, 1000, flags, 0);
			}
		}

		return true;
	}

	// Formation facing is a private EventClass type. It is queued, sent and
	// executed by the engine exactly like an order, so every peer sees the same
	// event stream. The payload is batched to avoid filling EventClass::OutList
	// with one event per selected unit.
	constexpr unsigned char FormationNetType = 0x42;
	constexpr size_t FormationNetPayloadSize = 64;
	constexpr unsigned char FormationNetVersion = 1;
	constexpr size_t FormationNetMaxEntries = 8;

	struct FormationFacingRequest
	{
		FootClass* Unit = nullptr;
		DirStruct Facing {};
	};

#pragma pack(push, 1)
	struct FormationFacingEntry
	{
		TargetClass Whom;
		unsigned short Facing;
	};

	struct FormationFacingBatch
	{
		char Magic[2];
		unsigned char Version;
		unsigned char Count;
		FormationFacingEntry Entries[FormationNetMaxEntries];
	};
#pragma pack(pop)

	static_assert(sizeof(FormationFacingEntry) == 7, "Unexpected TargetClass layout");
	static_assert(sizeof(FormationFacingBatch) <= FormationNetPayloadSize, "Formation payload too large");

	bool IsAirUnit(const FootClass* pFoot)
	{
		if (!pFoot)
			return false;

		if (pFoot->WhatAmI() == AbstractType::Aircraft)
			return true;

		auto const pLoco = pFoot->Locomotor;

		if (!pLoco)
			return false;

		return locomotion_cast<FlyLocomotionClass*>(pLoco)
			|| locomotion_cast<JumpjetLocomotionClass*>(pLoco);
	}

	// A compact, deterministic 50% shadow for the formation preview. Drawing
	// the VXL body a second time with Darken produced a solid black silhouette
	// on some blitters; blending a small ellipse gives the requested opacity.
	void DrawPreviewShadow(DSurface* pSurface, const Point2D& center,
		int radiusX, int radiusY)
	{
		if (!pSurface || radiusX <= 0 || radiusY <= 0)
			return;

		const RectangleStruct& view = DSurface::ViewBounds;
		const int rx2 = radiusX * radiusX;
		const int ry2 = radiusY * radiusY;
		const int limit = rx2 * ry2;

		for (int y = -radiusY; y <= radiusY; ++y)
		{
			for (int x = -radiusX; x <= radiusX; ++x)
			{
				if (x * x * ry2 + y * y * rx2 > limit)
					continue;

				Point2D point { center.X + x, center.Y + y };

				if (point.X < view.X || point.Y < view.Y
					|| point.X >= view.X + view.Width
					|| point.Y >= view.Y + view.Height)
					continue;

				BYTE r = 0, g = 0, b = 0;
				Drawing::Int_To_RGB(pSurface->GetPixel(&point), r, g, b);
				pSurface->SetPixel(&point,
					Drawing::RGB_To_Int(r / 2, g / 2, b / 2));
			}
		}
	}

	void SendFormationFacing(const std::vector<FormationFacingRequest>& requests)
	{
		if (requests.empty() || !HouseClass::CurrentPlayer)
			return;

		const int house = HouseClass::CurrentPlayer->ArrayIndex;
		static int queueFullLogCount = 0;

		for (size_t offset = 0; offset < requests.size(); offset += FormationNetMaxEntries)
		{
			const size_t count = std::min(FormationNetMaxEntries, requests.size() - offset);

			EventClass event(house, EventType::Empty);
			event.Type = static_cast<EventType>(FormationNetType);
			event.IsExecuted = false;
			event.HouseIndex = static_cast<char>(house);
			std::memset(event.DataBuffer, 0, sizeof(event.DataBuffer));

			auto* data = reinterpret_cast<unsigned char*>(event.DataBuffer);
			data[0] = 'F';
			data[1] = 'M';
			data[2] = FormationNetVersion;
			data[3] = static_cast<unsigned char>(count);

			for (size_t i = 0; i < count; ++i)
			{
				const auto& request = requests[offset + i];
				FormationFacingEntry entry;
				entry.Whom = TargetClass(request.Unit);
				entry.Facing = request.Facing.Raw;
				std::memcpy(data + 4 + i * sizeof(entry), &entry, sizeof(entry));
			}

			if (!EventClass::OutList.Add(event) && queueFullLogCount < 20)
			{
				++queueFullLogCount;
				Debug::Log("[Phobos::Formation] facing event queue full, count=%d\n",
					static_cast<int>(count));
			}
		}
	}

	void ReceiveFormationFacing(const EventClass& event)
	{
		auto const* data = reinterpret_cast<const unsigned char*>(event.DataBuffer);

		if (data[0] != 'F' || data[1] != 'M' || data[2] != FormationNetVersion)
			return;

		const unsigned count = std::min<unsigned>(data[3], FormationNetMaxEntries);

		for (unsigned i = 0; i < count; ++i)
		{
			FormationFacingEntry entry;
			std::memcpy(&entry, data + 4 + i * sizeof(entry), sizeof(entry));

			auto const pFoot = entry.Whom.As_Foot();

			if (!pFoot || pFoot->Health <= 0 || pFoot->InLimbo)
				continue;

			const DirStruct dir { static_cast<int>(entry.Facing) };
			pFoot->PrimaryFacing.SetDesired(dir);
			pFoot->SecondaryFacing.SetDesired(dir);
		}
	}
}

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

	auto pSurface = DSurface::Temp;
	if (!pSurface)
		return;

	const auto& view = DSurface::ViewBounds;
	bool drawShadow = false;
	Point2D shadowScreenPos { 0, 0 };

	if (IsAirUnit(pFoot) && pFoot->GetRenderCoords().Z > worldPos.Z + 8)
	{
		auto [groundScreen, visible] = TacticalClass::Instance->CoordsToClient(worldPos);
		const bool inView = groundScreen.X >= view.X
			&& groundScreen.Y >= view.Y
			&& groundScreen.X < view.X + view.Width
			&& groundScreen.Y < view.Y + view.Height;

		if (visible && inView)
		{
			shadowScreenPos = groundScreen;
			drawShadow = true;
		}
	}

	if (drawShadow)
	{
		const int maxDimension = std::max(16, static_cast<int>(pType->MaxDimension));
		const int radiusX = std::clamp(maxDimension / 3, 8, 32);
		const int radiusY = std::max(4, radiusX / 2);
		DrawPreviewShadow(pSurface, shadowScreenPos, radiusX, radiusY);
	}

	if (pType->Voxel)
	{
		// VXL units use their own engine draw path. If the type has no usable
		// voxel/HVA, fall through to the SHP path when an image exists.
		if (DrawVoxelUnitPreview(pFoot, pType, screenPos, formationDir, pSurface))
			return;
	}

	SHPStruct* pSHP = pType->GetImage();
	if (!pSHP || pSHP->Frames <= 0)
		return;

	ConvertClass* pPalette = pFoot->GetDrawer();
	if (!pPalette)
		return;

	RectangleStruct bounds = DSurface::ViewBounds;
	BlitterFlags flags = BlitterFlags::TransLucent25 | BlitterFlags::Centered;

	auto whatAmI = pFoot->WhatAmI();

	if (whatAmI == AbstractType::Unit)
	{
		auto pUnitType = static_cast<UnitTypeClass*>(pType);
		int facings = pUnitType->Facings;
		if (facings <= 0)
			facings = 32;

		const bool jumpjet = IsAirUnit(pFoot);

		// YR SHP frames are offset by 45 degrees from DirStruct convention for
		// normal vehicles. Jumpjet helicopters use the walk-frame block and the
		// art's frame zero is north/east, so they must not get that offset.
		int facingOffset = jumpjet ? 0 : facings / 8;
		int facing = 0;
		switch (facings)
		{
		case 8:  facing = static_cast<int>(formationDir.GetFacing<8>(facingOffset));  break;
		case 16: facing = static_cast<int>(formationDir.GetFacing<16>(facingOffset)); break;
		case 32: facing = static_cast<int>(formationDir.GetFacing<32>(facingOffset)); break;
		default: facing = static_cast<int>(formationDir.GetFacing<32>(jumpjet ? 0 : 4)); break;
		}

		const int blockStart = jumpjet ? pUnitType->StartWalkFrame : pUnitType->StartStandFrame;
		const int blockFrames = jumpjet
			? std::max(1, static_cast<int>(pUnitType->WalkFrames))
			: std::max(1, pUnitType->StandingFrames);
		int bodyFrame = blockStart + facing * blockFrames;

		if (bodyFrame >= 0 && bodyFrame < pSHP->Frames)
		{
			pSurface->DrawSHP(pPalette, pSHP, bodyFrame, &screenPos, &bounds,
				flags, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		}

		// Draw turret if the unit has one and SHP has enough frames
		if (pType->Turret)
		{
			int turretStart = blockStart + facings * blockFrames;
			int turretFrame = turretStart + facing * blockFrames;

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
		// Aircraft SHP art starts at the north/east frame without the 45-degree
		// offset used by vehicle body frames.
		int facing = static_cast<int>(formationDir.GetFacing<32>(0));
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

	const RectangleStruct view = DSurface::ViewBounds;
	std::vector<Point2D> screenPositions;
	std::vector<CoordStruct> previewPositions;
	std::vector<bool> drawPositions;
	screenPositions.reserve(positions.size());
	previewPositions.reserve(positions.size());
	drawPositions.reserve(positions.size());

	for (size_t i = 0; i < positions.size(); ++i)
	{
		CoordStruct previewPos = positions[i];

		// Formation destinations stay on the ground, but air units and jumpjets
		// must be previewed at their current altitude instead of at Z = ground.
		// Ground units must stay on the destination plane even though their
		// render coordinates can contain a non-zero height offset.
		if (i < units.size() && IsAirUnit(units[i]))
			previewPos.Z = units[i]->GetRenderCoords().Z;

		auto [screenPos, visible] = TacticalClass::Instance->CoordsToClient(previewPos);

		const bool inView = screenPos.X >= view.X
			&& screenPos.Y >= view.Y
			&& screenPos.X < view.X + view.Width
			&& screenPos.Y < view.Y + view.Height;

		screenPositions.push_back(screenPos);
		previewPositions.push_back(previewPos);
		drawPositions.push_back(visible && inView);
	}

	// Draw semi-transparent unit images at each position. Skipping invisible
	// positions also keeps preview pixels out of the native bottom command bar.
	for (size_t i = 0; i < screenPositions.size() && i < units.size(); i++)
	{
		if (drawPositions[i])
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

		if ((startVis || endVis)
			&& Line_In_Bounds(&startScreen, &endScreen, &DSurface::ViewBounds))
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
	std::vector<FormationFacingRequest> facingRequests;
	facingRequests.reserve(units.size());

	for (size_t i = 0; i < units.size() && i < positions.size(); i++)
	{
		auto pFoot = units[i];
		if (!pFoot)
			continue;

		const auto& destCoord = positions[i];

		// Queue move command
		EventClass e(
			houseIndex,
			TargetClass(pFoot),
			Mission::Move,
			TargetClass(),
			TargetClass(destCoord),
			TargetClass()
		);

		if (!EventClass::OutList.Add(e))
		{
			Debug::Log("[Phobos::Formation] move event queue full, unit=%p\n",
				static_cast<void*>(pFoot));
		}

		// Facing is applied by a private network event on every client. Never
		// change it locally here: doing so desynchronises multiplayer.
		facingRequests.push_back({ pFoot, formationDir });

		// Re-apply the formation facing after arrival through the same event.
		m_pendingFacings[pFoot] = { formationDir, 0, false };
	}

	SendFormationFacing(facingRequests);
}

void FormationManager::ProcessPendingFacings()
{
	std::vector<FormationFacingRequest> facingRequests;

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
				facingRequests.push_back({ pFoot, pending.Facing });
				it = m_pendingFacings.erase(it);
				continue;
			}
		}
		else
		{
			if (pFoot->CurrentMission != Mission::Move)
			{
				facingRequests.push_back({ pFoot, pending.Facing });
				it = m_pendingFacings.erase(it);
				continue;
			}
		}

		if (pending.FramesPending > 600)
		{
			facingRequests.push_back({ pFoot, pending.Facing });
			it = m_pendingFacings.erase(it);
			continue;
		}

		++it;
	}

	SendFormationFacing(facingRequests);
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

// Formation facing is sent as a private EventClass type. It is a normal queued
// event (so command counts stay identical on every peer), but the handler only
// changes the units' desired facing. All peers must run the same Phobos build
// because older clients do not know the 0x42 payload size.
DEFINE_HOOK(0x4C6CC8, EventClass_Execute_FormationFacing, 0x5)
{
	GET(EventClass*, pEvent, ESI);

	if (static_cast<unsigned char>(pEvent->Type) == FormationNetType)
		ReceiveFormationFacing(*pEvent);

	return 0;
}

// The three engine event-size paths must agree on the fixed payload length.
// TacticalMap installs its own hooks at these addresses for type 0x41; both
// handlers return 0 for event types they do not own.
DEFINE_HOOK(0x64B6FE, EventClass_Size_FormationFacing, 0x6)
{
	GET(uint32_t, type, EDI);

	if ((type & 0xFF) == FormationNetType)
	{
		R->EDX(static_cast<int>(FormationNetPayloadSize));
		R->EBP(static_cast<int>(FormationNetPayloadSize));
		return 0x64B71D;
	}

	return 0;
}

DEFINE_HOOK(0x64BE7D, EventClass_ReadSize_FormationFacing, 0x6)
{
	GET(uint32_t, type, EDI);

	if ((type & 0xFF) == FormationNetType)
	{
		REF_STACK(int, eventSize, STACK_OFFSET(0xAC, -0x8C));
		eventSize = static_cast<int>(FormationNetPayloadSize);
		R->ECX(static_cast<int>(FormationNetPayloadSize));
		R->EBP(static_cast<int>(FormationNetPayloadSize));
		return 0x64BE97;
	}

	return 0;
}

DEFINE_HOOK(0x64C30E, EventClass_ParseSize_FormationFacing, 0x6)
{
	GET(uint32_t, type, ESI);

	if ((type & 0xFF) == FormationNetType)
	{
		R->ECX(static_cast<int>(FormationNetPayloadSize));
		R->EBP(static_cast<int>(FormationNetPayloadSize));
		return 0x64C321;
	}

	return 0;
}
