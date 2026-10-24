#include "FootprintLocomotionClass.h"

#include <CellClass.h>
#include <MapClass.h>
#include <Unsorted.h>

#include <Ext/Techno/Body.h>
#include <Ext/Unit/Footprint.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>

namespace
{
	std::vector<CellStruct> UnionSpans(const std::vector<CellStruct>& a, const std::vector<CellStruct>& b)
	{
		std::vector<CellStruct> result = a;

		for (const auto& cell : b)
		{
			if (std::find(result.begin(), result.end(), cell) == result.end())
				result.push_back(cell);
		}

		return result;
	}

	bool SameCell(const CoordStruct& left, const CoordStruct& right)
	{
		return CellClass::Coord2Cell(left) == CellClass::Coord2Cell(right);
	}

	int DirectionFromDelta(int dx, int dy)
	{
		static const int dirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const int dirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

		for (int dir = 0; dir < 8; ++dir)
		{
			if (dirX[dir] == dx && dirY[dir] == dy)
				return dir;
		}

		return -1;
	}
}

HRESULT FootprintLocomotionClass::Load(IStream* pStm)
{
	const HRESULT hr = this->LocomotionClass::Load(pStm);

	if (FAILED(hr))
		return hr;

	this->ClearPath();
	this->Moving = false;
	this->Turning = false;
	this->HeadToCoord = CoordStruct::Empty;
	this->TargetCoord = CoordStruct::Empty;
	this->Powered = true;
	this->UnLocked = true;

	return hr;
}

HRESULT FootprintLocomotionClass::Save(IStream* pStm, BOOL fClearDirty)
{
	return this->LocomotionClass::Save(pStm, fClearDirty);
}

CoordStruct FootprintLocomotionClass::Head_To_Coord()
{
	if (this->HeadToCoord != CoordStruct::Empty)
		return this->HeadToCoord;

	return this->LinkedTo ? this->LinkedTo->Location : CoordStruct::Empty;
}

Move FootprintLocomotionClass::Can_Enter_Cell(CellStruct cell)
{
	if (!this->LinkedTo || !Footprint::IsMultiCell(this->LinkedTo))
		return this->LocomotionClass::Can_Enter_Cell(cell);

	auto const pUnit = abstract_cast<UnitClass*>(this->LinkedTo);

	if (!pUnit)
		return Move::No;

	const auto pState = Footprint::GetState(pUnit);
	const int bucket = pState && pState->Initialized
		? pState->Bucket
		: Footprint::BucketFromFacing(pUnit->PrimaryFacing.Current());
	const auto span = Footprint::GetSpan(pUnit, cell, bucket);

	return Footprint::CanOccupy(pUnit, span, pUnit->OnBridge, cell) ? Move::OK : Move::No;
}

bool FootprintLocomotionClass::Process()
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked || !pLinked->IsAlive)
		return false;

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);

	if (!pUnit)
		return false;

	auto const pState = Footprint::GetState(pUnit);

	if (!pState)
		return false;

	const CellStruct anchor = pState->Initialized ? pState->Anchor : pLinked->GetMapCoords();

	if (this->Turning)
	{
		if (pState->Initialized)
		{
			const int currentBucket = Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());

			if (currentBucket != pState->Bucket)
			{
				const auto hull = Footprint::GetSpan(pUnit, pState->Anchor, currentBucket);
				Footprint::CommitSpan(pUnit, hull, currentBucket, pState->Alt, pState->Anchor);
			}
		}

		if (pLinked->PrimaryFacing.IsRotating())
			return true;

		this->FinishTurn(anchor);
		return true;
	}

	if (this->HeadToCoord != CoordStruct::Empty)
		return this->TranslateOneFrame();

	if (this->PathIndex >= this->PathLength)
	{
		this->Moving = false;
		pLinked->SetSpeedPercentage(0.0);
		return false;
	}

	const FootprintPathStep step = this->Path[this->PathIndex];

	if (step.TurnOnly)
	{
		if (this->BeginTurn(step.Bucket, anchor))
			return true;

		this->Moving = false;
		return false;
	}

	if (this->BeginMove(step, anchor))
		return true;

	this->Moving = false;
	return false;
}

void FootprintLocomotionClass::Move_To(CoordStruct to)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked || !pLinked->IsAlive)
		return;

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);

	if (!pUnit || !Footprint::IsMultiCell(pUnit))
		return;

	if (to == CoordStruct::Empty)
	{
		this->Stop_Moving();
		return;
	}

	// The mission layer can call Move_To every frame with the same destination.
	if (this->Moving && this->TargetCoord == to)
		return;

	auto const pState = Footprint::GetState(pUnit);
	const CellStruct start = pState && pState->Initialized
		? pState->Anchor
		: pLinked->GetMapCoords();
	const CellStruct goal = CellClass::Coord2Cell(to);

	// Our custom mover does not consume the vanilla PathDirections/Waypoint
	// fields. Clearing them prevents FootClass::UpdatePosition(End) from applying
	// a stale waypoint delta and shifting the hull sideways when it reaches a
	// cell centre.
	pLinked->PathDirections[0] = -1;
	pLinked->WaypointCell = CellStruct { -1, -1 };
	pLinked->WaypointNearbyAccessibleCellDelta = CellStruct { -1, -1 };

	Debug::Log("[FootprintMove] %s Move_To (%d,%d) from cell (%d,%d)\n",
		pLinked->GetTechnoType()->ID, to.X, to.Y, start.X, start.Y);

	if (!MapClass::Instance.CellExists(goal))
	{
		this->ClearPath();
		this->Moving = false;
		return;
	}

	std::vector<FootprintPathStep> path;
	const int startBucket = pState && pState->Initialized
		? pState->Bucket
		: Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());

	if (!FootprintPathfinder::FindPath(pUnit, start, goal, startBucket, path))
	{
		this->ClearPath();
		this->Moving = false;
		pLinked->SetSpeedPercentage(0.0);
		return;
	}

	this->PathLength = static_cast<int>(std::min<size_t>(path.size(), 64));
	this->PathIndex = 0;

	for (int i = 0; i < this->PathLength; ++i)
		this->Path[i] = path[static_cast<size_t>(i)];

	this->TargetCoord = to;
	this->HeadToCoord = CoordStruct::Empty;
	this->Moving = true;
	pLinked->SetSpeedPercentage(1.0);
}

void FootprintLocomotionClass::Stop_Moving()
{
	this->ClearPath();
	this->Moving = false;
	this->Turning = false;
	this->HeadToCoord = CoordStruct::Empty;

	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	pLinked->SetSpeedPercentage(0.0);

	if (auto const pUnit = abstract_cast<UnitClass*>(pLinked))
	{
		if (auto const pState = Footprint::GetState(pUnit))
		{
			pState->Locked = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;
		}

		Footprint::ReleaseReservation(pUnit);
		Footprint::Mark(pUnit, pLinked->Location);
	}
}

void FootprintLocomotionClass::Do_Turn(DirStruct dir)
{
	if (this->LinkedTo)
		this->LinkedTo->PrimaryFacing.SetDesired(dir);
}

void FootprintLocomotionClass::Unlimbo()
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	if (auto const pUnit = abstract_cast<UnitClass*>(pLinked))
		Footprint::Mark(pUnit, pLinked->Location);
}

int FootprintLocomotionClass::Apparent_Speed()
{
	return this->LinkedTo ? this->LinkedTo->GetTechnoType()->Speed : 0;
}

void FootprintLocomotionClass::Mark_All_Occupation_Bits(MarkType mark)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	if (mark == MarkType::Up)
		pLinked->MarkAllOccupationBits(pLinked->Location);
	else
		pLinked->UnmarkAllOccupationBits(pLinked->Location);
}

bool FootprintLocomotionClass::Is_Moving_Here(CoordStruct to)
{
	if (this->HeadToCoord != CoordStruct::Empty && SameCell(this->HeadToCoord, to))
		return true;

	if (this->Turning && SameCell(CellClass::Cell2Coord(this->TurnTargetCell), to))
		return true;

	return false;
}

void FootprintLocomotionClass::Limbo()
{
	this->Stop_Moving();

	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	if (auto const pUnit = abstract_cast<UnitClass*>(pLinked))
		Footprint::Release(pUnit);
}

bool FootprintLocomotionClass::BeginTurn(int targetBucket, const CellStruct& anchor)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return false;

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);
	auto const pState = Footprint::GetState(pUnit);

	if (!pUnit || !pState)
		return false;

	if (pState->ReconcileActive)
	{
		const auto currentSpan = Footprint::GetSpan(pUnit, pState->Anchor, pState->Bucket);
		Footprint::CommitSpan(pUnit, currentSpan, pState->Bucket, pState->Alt, pState->Anchor);
		pState->ReconcileActive = false;
		pState->ReconcileTarget = -1;
		pState->Locked = false;
	}

	const int from = pState->Initialized ? pState->Bucket : Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());
	const int to = targetBucket & 7;

	if (from == to)
	{
		++this->PathIndex;
		return true;
	}

	const int delta = (to - from + 8) & 7;
	const bool tryClockwise = delta <= 4;
	const bool tryCounterClockwise = delta >= 4;

	auto tryDirection = [&](bool clockwise) -> bool
	{
		const auto sweep = Footprint::GetSweepAt(pUnit, anchor, from, to, clockwise);

		if (sweep.empty()
			|| !Footprint::CanOccupy(pUnit, sweep, pState->Alt, anchor))
		{
			return false;
		}

		Footprint::ReleaseReservation(pUnit);

		if (!Footprint::ReserveSweep(pUnit, sweep, pState->Alt))
			return false;

		pState->Locked = true;
		pLinked->PrimaryFacing.SetDesired(DirStruct(to * 0x2000));
		pLinked->SetSpeedPercentage(0.0);

		this->Turning = true;
		this->TurnFromBucket = from;
		this->TurnTargetBucket = to;
		this->TurnTargetCell = anchor;

		Debug::Log("[FootprintTurn] %s cell=(%d,%d) bucket %d -> %d (%s), sweep=%d cells\n",
			pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, from, to,
			clockwise ? "cw" : "ccw", static_cast<int>(sweep.size()));

		return true;
	};

	if (tryClockwise && tryDirection(true))
		return true;

	if (tryCounterClockwise && tryDirection(false))
		return true;

	this->LogBlocked("turn", anchor);
	return false;
}

void FootprintLocomotionClass::FinishTurn(const CellStruct& anchor)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
	{
		this->Turning = false;
		return;
	}

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);
	auto const pState = Footprint::GetState(pUnit);

	if (pUnit && pState)
	{
		const auto span = Footprint::GetSpan(pUnit, anchor, this->TurnTargetBucket);
		Footprint::CommitSpan(pUnit, span, this->TurnTargetBucket, pState->Alt, anchor);
		Footprint::ReleaseReservation(pUnit);
		pState->Locked = false;
	}

	this->Turning = false;
	++this->PathIndex;
	pLinked->SetSpeedPercentage(0.0);

	Debug::Log("[FootprintTurn] finished cell=(%d,%d) bucket %d\n",
		anchor.X, anchor.Y, this->TurnTargetBucket);
}

bool FootprintLocomotionClass::BeginMove(const FootprintPathStep& step, const CellStruct& anchor)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return false;

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);
	auto const pState = Footprint::GetState(pUnit);

	if (!pUnit || !pState)
		return false;

	const int bucket = pState->Initialized ? pState->Bucket : Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());
	const CellStruct next = step.Cell;
	const int dx = next.X - anchor.X;
	const int dy = next.Y - anchor.Y;
	const int direction = DirectionFromDelta(dx, dy);
	const bool forward = direction == bucket;
	const bool reverse = direction == ((bucket + 4) & 7);

	if (step.Bucket != bucket || direction < 0 || (!forward && !reverse))
	{
		Debug::Log("[FootprintStep] %s invalid step from=(%d,%d) bucket=%d next=(%d,%d) dir=%d stepBucket=%d\n",
			pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
			next.X, next.Y, direction, step.Bucket);

		return false;
	}

	if (step.Reverse != reverse)
	{
		Debug::Log("[FootprintStep] %s reverse flag mismatch from=(%d,%d) bucket=%d next=(%d,%d) flag=%d actual=%d\n",
			pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
			next.X, next.Y, step.Reverse ? 1 : 0, reverse ? 1 : 0);
	}

	Debug::Log("[FootprintStep] %s from=(%d,%d) bucket=%d next=(%d,%d) %s\n",
		pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
		next.X, next.Y, forward ? "forward" : "reverse");

	if (pState->ReconcileActive)
	{
		Footprint::ReleaseReservation(pUnit);
		const auto currentSpan = Footprint::GetSpan(pUnit, pState->Anchor, pState->Bucket);
		Footprint::CommitSpan(pUnit, currentSpan, pState->Bucket, pState->Alt, pState->Anchor);
		pState->ReconcileActive = false;
		pState->ReconcileTarget = -1;
		pState->Locked = false;
	}
	else if (pState->Locked)
	{
		Footprint::ReleaseReservation(pUnit);
		pState->Locked = false;
	}

	const auto nextSpan = Footprint::GetSpan(pUnit, next, bucket);

	if (!Footprint::CanOccupy(pUnit, nextSpan, pState->Alt, next))
	{
		this->LogBlocked("move", next);
		return false;
	}

	const auto currentSpan = Footprint::GetSpan(pUnit, anchor, bucket);
	Footprint::CommitSpan(pUnit, UnionSpans(currentSpan, nextSpan), bucket, pState->Alt, anchor);

	this->MoveTargetCell = next;
	this->HeadToCoord = CellClass::Cell2Coord(next, pLinked->Location.Z);
	pLinked->SetSpeedPercentage(1.0);

	return true;
}

bool FootprintLocomotionClass::TranslateOneFrame()
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return false;

	auto const pUnit = abstract_cast<UnitClass*>(pLinked);
	auto const pState = Footprint::GetState(pUnit);

	if (!pUnit || !pState)
		return false;

	const int speed = std::max(1, pLinked->GetTechnoType()->Speed);
	const CoordStruct current = pLinked->Location;
	CoordStruct delta
	{
		this->HeadToCoord.X - current.X,
		this->HeadToCoord.Y - current.Y,
		this->HeadToCoord.Z - current.Z
	};

	const double distance = std::sqrt(
		static_cast<double>(delta.X) * delta.X
		+ static_cast<double>(delta.Y) * delta.Y
		+ static_cast<double>(delta.Z) * delta.Z);

	if (distance <= 0.001)
	{
		this->HeadToCoord = CoordStruct::Empty;
		this->PathIndex++;
		return true;
	}

	const int step = std::min(speed, static_cast<int>(distance));

	pLinked->Mark(MarkType::Up);

	if (distance <= speed)
	{
		pLinked->SetLocation(this->HeadToCoord);
	}
	else
	{
		CoordStruct nextPos
		{
			current.X + static_cast<int>(delta.X * step / distance),
			current.Y + static_cast<int>(delta.Y * step / distance),
			current.Z + static_cast<int>(delta.Z * step / distance)
		};

		pLinked->SetLocation(nextPos);
	}

	pLinked->Mark(MarkType::Down);

	if (distance <= speed)
	{
		pLinked->LastMapCoords = pState->Anchor;
		pLinked->CurrentMapCoords = this->MoveTargetCell;
		pLinked->PathDirections[0] = -1;
		pLinked->WaypointCell = CellStruct { -1, -1 };
		pLinked->WaypointNearbyAccessibleCellDelta = CellStruct { -1, -1 };
		pLinked->UpdatePosition(PCPType::End);

		const auto nextSpan = Footprint::GetSpan(pUnit, this->MoveTargetCell, pState->Bucket);
		Footprint::CommitSpan(pUnit, nextSpan, pState->Bucket, pState->Alt, this->MoveTargetCell);

		this->HeadToCoord = CoordStruct::Empty;
		this->PathIndex++;
	}

	return true;
}

void FootprintLocomotionClass::ClearPath()
{
	this->PathLength = 0;
	this->PathIndex = 0;
}

void FootprintLocomotionClass::LogBlocked(const char* reason, const CellStruct& cell)
{
	if (Unsorted::CurrentFrame - this->LastLogFrame < 30)
		return;

	this->LastLogFrame = Unsorted::CurrentFrame;

	Debug::Log("[FootprintMove] blocked %s at cell=(%d,%d) unit=%s\n",
		reason, cell.X, cell.Y,
		this->LinkedTo ? this->LinkedTo->GetTechnoType()->ID : "?");
}
