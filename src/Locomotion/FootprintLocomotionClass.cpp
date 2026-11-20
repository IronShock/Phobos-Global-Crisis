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
	constexpr DWORD kMoveFailCooldownFrames = 30;
	constexpr DWORD kMoveRetryTimeoutFrames = 300;

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
	this->PendingTarget = CoordStruct::Empty;
	this->Powered = true;
	this->UnLocked = true;
	this->ResetRetry();

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

	// Once a step has failed there is no valid path left to execute. Previously
	// Process() kept re-reading Path[0] every frame and spamming the same
	// "invalid step" failure instead of waiting for a new Move_To().
	if (!this->Moving && this->HeadToCoord == CoordStruct::Empty && !this->Turning)
	{
		// A footprint barrier can disappear when the blocking hull moves away.
		// Keep the requested destination and retry periodically instead of
		// stopping forever after the first collision.
		if (this->TargetCoord != CoordStruct::Empty)
		{
			if (this->RetryExpired())
			{
				this->Stop_Moving();
				return false;
			}

			const CellStruct goal = CellClass::Coord2Cell(this->TargetCoord);

			if (!this->IsFailureCooldown(goal))
				this->Move_To(this->TargetCoord);
		}

		return false;
	}

	if (this->Turning)
	{
		if (pState->Initialized)
		{
			const int currentBucket = Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());

			if (currentBucket != pState->Bucket)
			{
				const auto hull = Footprint::GetSpan(pUnit, pState->Anchor, currentBucket);
				Footprint::CommitSpan(pUnit, hull, currentBucket, pState->Alt, pState->Anchor);

				if (currentBucket != this->TurnTargetBucket)
				{
					if (!Footprint::ReserveTurnStep(pUnit, pState->Anchor,
						currentBucket, this->TurnTargetBucket, this->TurnClockwise, pState->Alt))
					{
						// A normal unit reached the next 45-degree slice first.
						// Abort this turn rather than overlapping it; keep the
						// destination so the normal retry path can replan.
						const CellStruct failureGoal = this->TargetCoord == CoordStruct::Empty
							? pState->Anchor
							: CellClass::Coord2Cell(this->TargetCoord);

						this->RecordFailure(failureGoal);
						Footprint::ReleaseReservation(pUnit);
						pState->Locked = false;
						pLinked->PrimaryFacing.SetCurrent(DirStruct(currentBucket * 0x2000));
						pLinked->PrimaryFacing.SetDesired(DirStruct(currentBucket * 0x2000));
						this->Turning = false;
						this->ClearPath();
						this->Moving = false;
						pLinked->SetSpeedPercentage(0.0);
						this->LogBlocked("turn-step", pState->Anchor);
						return false;
					}
				}
			}
		}

		if (pLinked->PrimaryFacing.IsRotating())
			return true;

		this->FinishTurn(anchor);
		return true;
	}

	if (this->HeadToCoord != CoordStruct::Empty)
	{
		const bool result = this->TranslateOneFrame();

		// Consume an order that arrived during the interpolation only after
		// the hull has reached the current cell centre. This keeps continuous
		// clicks continuous instead of teleporting to the next anchor.
		if (this->HeadToCoord == CoordStruct::Empty
			&& this->PendingTarget != CoordStruct::Empty)
		{
			const CoordStruct pending = this->PendingTarget;
			this->PendingTarget = CoordStruct::Empty;
			this->TargetCoord = CoordStruct::Empty;
			this->Move_To(pending);
		}

		return result;
	}

	if (this->PathIndex >= this->PathLength)
	{
		// Reaching the end of the path clears the destination so the selection
		// line does not stay on the map after the unit has arrived.
		this->Stop_Moving();
		return false;
	}

	const FootprintPathStep step = this->Path[this->PathIndex];

	if (step.TurnOnly)
	{
		if (this->BeginTurn(step.Bucket, anchor))
			return true;

		this->RecordFailure(this->TargetCoord == CoordStruct::Empty
			? anchor
			: CellClass::Coord2Cell(this->TargetCoord));
		this->ClearPath();
		this->Moving = false;
		pLinked->SetSpeedPercentage(0.0);
		return false;
	}

	if (this->BeginMove(step, anchor))
		return true;

	this->RecordFailure(this->TargetCoord == CoordStruct::Empty
		? anchor
		: CellClass::Coord2Cell(this->TargetCoord));
	this->ClearPath();
	this->Moving = false;
	pLinked->SetSpeedPercentage(0.0);
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

	// Read-only diagnostic: start a short snapshot window for the art/position
	// jump investigation. This does not alter the movement state.
	if (this->TargetCoord != to)
		Footprint::BeginCarrierArtDiag(pUnit, "Move_To");

	const bool newTarget = this->TargetCoord != to;

	if (newTarget)
	{
		this->ResetRetry();
		this->TargetCoord = to;
	}

	auto const pState = Footprint::GetState(pUnit);

	if (this->HeadToCoord != CoordStruct::Empty)
	{
		// Let the current cell-to-cell interpolation finish first. Replacing
		// the path immediately would snap the model to the next cell centre
		// on every click, which is the rapid-click teleport.
		if (newTarget)
		{
			this->PendingTarget = to;
			this->TargetCoord = to;
			this->ResetRetry();
		}

		return;
	}

	// A normal order replaces any stale deferred order.
	this->PendingTarget = CoordStruct::Empty;

	const CellStruct start = pState && pState->Initialized
		? pState->Anchor
		: pLinked->GetMapCoords();
	const CellStruct goal = CellClass::Coord2Cell(to);

	if (this->IsFailureCooldown(goal))
	{
		this->ClearPath();
		this->Moving = false;
		return;
	}

	// Our custom mover does not consume the vanilla PathDirections/Waypoint
	// fields. Clearing them prevents FootClass::UpdatePosition(End) from applying
	// a stale waypoint delta and shifting the hull sideways when it reaches a
	// cell centre.
	pLinked->PathDirections[0] = -1;
	pLinked->WaypointCell = CellStruct { -1, -1 };
	pLinked->WaypointNearbyAccessibleCellDelta = CellStruct { -1, -1 };

	Debug::CarrierLog("[FootprintMove] %s Move_To (%d,%d) from cell (%d,%d)\n",
		pLinked->GetTechnoType()->ID, to.X, to.Y, start.X, start.Y);

	if (!MapClass::Instance.CellExists(goal))
	{
		this->Stop_Moving();
		return;
	}

	// Checking only the goal anchor is not enough for a 36x9 hull: at a map
	// edge the anchor is valid while every possible facing extends outside the
	// map. Do not start a potentially 20000-expansion A* search for such a goal.
	bool goalFits = false;

	for (int bucket = 0; bucket < 8; ++bucket)
	{
		const auto goalSpan = Footprint::GetSpan(pUnit, goal, bucket);

		if (Footprint::CanOccupy(pUnit, goalSpan, pUnit->OnBridge, goal))
		{
			goalFits = true;
			break;
		}
	}

	if (!goalFits)
	{
		this->LogBlocked("goal", goal);
		this->Stop_Moving();
		return;
	}

	std::vector<FootprintPathStep> path;
	const int startBucket = pState && pState->Initialized
		? pState->Bucket
		: Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());

	if (!FootprintPathfinder::FindPath(pUnit, start, goal, startBucket, path))
	{
		this->RecordFailure(goal);
		this->ClearPath();
		this->Moving = false;
		pLinked->SetSpeedPercentage(0.0);
		return;
	}

	if (path.empty())
	{
		this->Stop_Moving();
		return;
	}

	this->ResetRetry();
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
	this->TargetCoord = CoordStruct::Empty;
	this->PendingTarget = CoordStruct::Empty;
	this->ResetRetry();

	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	// Clearing only our own TargetCoord is not enough: the engine's action-line
	// renderer reads FootClass::Destination directly. If the custom locomotor
	// reaches its path end without clearing these fields, the old destination
	// line remains visible forever even though the unit has stopped.
	pLinked->Destination = nullptr;
	pLinked->LastDestination = nullptr;
	pLinked->MegaDestination = nullptr;

	pLinked->SetSpeedPercentage(0.0);

	if (auto const pUnit = abstract_cast<UnitClass*>(pLinked))
	{
		if (auto const pState = Footprint::GetState(pUnit))
		{
			pState->Locked = false;
			pState->Translating = false;
			pState->ReconcileActive = false;
			pState->ReconcileTarget = -1;
		}

		Footprint::LogCarrierArtDiag(pUnit, "Stop");
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
	{
		Footprint::Mark(pUnit, pLinked->Location);
	}
}

int FootprintLocomotionClass::Apparent_Speed()
{
	return this->LinkedTo ? this->LinkedTo->GetCurrentSpeed() : 0;
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

	Footprint::LogCarrierArtDiag(pUnit, "TurnBegin");

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
		if (!Footprint::ReserveTurnStep(pUnit, anchor, from, to,
			clockwise, pState->Alt))
		{
			return false;
		}

		pState->Locked = true;
		pLinked->PrimaryFacing.SetDesired(DirStruct(to * 0x2000));
		pLinked->SetSpeedPercentage(0.0);

		this->Turning = true;
		this->TurnFromBucket = from;
		this->TurnTargetBucket = to;
		this->TurnClockwise = clockwise;
		this->TurnTargetCell = anchor;

		Debug::CarrierLog("[FootprintTurn] %s cell=(%d,%d) bucket %d -> %d (%s), remainingReserved=%d\n",
			pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, from, to,
			clockwise ? "cw" : "ccw",
			static_cast<int>(pState->ReservedSpan.size()));

		return true;
	};

	if (tryClockwise && tryDirection(true))
		return true;

	if (tryCounterClockwise && tryDirection(false))
		return true;

	this->LogBlocked("turn", anchor);
	return false;
}

// The custom mover owns the hull's position, but the engine still updates its
// own cell bookkeeping between frames. After a move step TranslateOneFrame
// aligns LastMapCoords/CurrentMapCoords and clears the vanilla path fields
// before UpdatePosition(End). A turn is an extra in-place action with no such
// cleanup, so the first move after the turn could make FootClass::UpdatePosition
// consume the previous step's stale values and apply a one-cell lateral
// correction. Resetting the fields at the turn boundary keeps the engine's
// idea of the hull's cell equal to the cell the hull is actually sitting on.
void FootprintLocomotionClass::ResetEngineMapState(const CellStruct& anchor)
{
	auto const pLinked = this->LinkedTo;

	if (!pLinked)
		return;

	pLinked->LastMapCoords = anchor;
	pLinked->CurrentMapCoords = anchor;
	pLinked->PathDirections[0] = -1;
	pLinked->WaypointCell = CellStruct { -1, -1 };
	pLinked->WaypointNearbyAccessibleCellDelta = CellStruct { -1, -1 };
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

	Footprint::LogCarrierArtDiag(pUnit, "TurnEndBeforeCommit");

	if (pUnit && pState)
	{
		const auto span = Footprint::GetSpan(pUnit, anchor, this->TurnTargetBucket);
		Footprint::CommitSpan(pUnit, span, this->TurnTargetBucket, pState->Alt, anchor);
		Footprint::ReleaseReservation(pUnit);
		pState->Locked = false;
	}

	Footprint::LogCarrierArtDiag(pUnit, "TurnEnd");

	// The hull is still exactly on the turn cell centre here. Align the
	// engine's map bookkeeping to that cell before the next BeginMove so the
	// first step cannot inherit the previous step's stale lateral correction.
	this->ResetEngineMapState(anchor);
	Footprint::LogCarrierArtDiag(pUnit, "TurnEndClean");

	this->Turning = false;
	++this->PathIndex;
	pLinked->SetSpeedPercentage(0.0);

	Debug::CarrierLog("[FootprintTurn] finished cell=(%d,%d) bucket %d\n",
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

	Footprint::LogCarrierArtDiag(pUnit, "MoveBeginBeforeCommit");

	const int bucket = pState->Initialized ? pState->Bucket : Footprint::BucketFromFacing(pLinked->PrimaryFacing.Current());
	const CellStruct next = step.Cell;
	const int dx = next.X - anchor.X;
	const int dy = next.Y - anchor.Y;
	const int direction = DirectionFromDelta(dx, dy);
	const bool forward = direction == bucket;

	if (step.Bucket != bucket || direction < 0 || !forward || step.Reverse)
	{
		this->LogBlocked("invalid-step", next);
		return false;
	}

	Debug::CarrierLog("[FootprintStep] %s from=(%d,%d) bucket=%d next=(%d,%d) forward\n",
		pLinked->GetTechnoType()->ID, anchor.X, anchor.Y, bucket,
		next.X, next.Y);

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

	if (!Footprint::IsClimbAllowed(pUnit, anchor, next))
	{
		this->LogBlocked("slope", next);
		return false;
	}

	if (!Footprint::CanOccupy(pUnit, nextSpan, pState->Alt, next))
	{
		this->LogBlocked("move", next);
		return false;
	}

	const auto currentSpan = Footprint::GetSpan(pUnit, anchor, bucket);

	if (!Footprint::CommitSpan(pUnit, UnionSpans(currentSpan, nextSpan),
		bucket, pState->Alt, anchor))
	{
		this->LogBlocked("commit", next);
		return false;
	}

	const auto pNextCell = MapClass::Instance.TryGetCellAt(next);
	const int nextZ = pNextCell
		? (pState->Alt ? pNextCell->GetCoordsWithBridge().Z : pNextCell->GetCoords().Z)
		: pLinked->Location.Z;

	// Safety net for the turn-end path: the hull is still at the anchor cell
	// centre when the first cell step starts. Keep the engine's map fields at
	// that cell until TranslateOneFrame commits the destination at the end of
	// the step.
	this->ResetEngineMapState(anchor);

	this->MoveTargetCell = next;
	this->HeadToCoord = CellClass::Cell2Coord(next, nextZ);
	this->DiagTranslateLogged = false;
	pLinked->SetSpeedPercentage(1.0);

	Footprint::LogCarrierArtDiag(pUnit, "MoveBegin");

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

	if (!this->DiagTranslateLogged)
	{
		Footprint::LogCarrierArtDiag(pUnit, "TranslateStart");
		this->DiagTranslateLogged = true;
	}

	const CellStruct oldAnchor = pState->Anchor;
	pState->Translating = true;

	const int speed = std::max(1, pLinked->GetCurrentSpeed());
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
		pState->Translating = false;
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
			current.X + static_cast<int>(std::lround(delta.X * static_cast<double>(step) / distance)),
			current.Y + static_cast<int>(std::lround(delta.Y * static_cast<double>(step) / distance)),
			current.Z + static_cast<int>(std::lround(delta.Z * static_cast<double>(step) / distance))
		};

		pLinked->SetLocation(nextPos);
	}

	pLinked->Mark(MarkType::Down);

	if (distance <= speed)
	{
		// Use the anchor captured before the engine's Mark/UpdatePosition
		// hooks ran. pState->Anchor may otherwise already point at the target
		// cell, and UpdatePosition would remove the wrong old cell / apply a
		// lateral correction.
		pLinked->LastMapCoords = oldAnchor;
		pLinked->CurrentMapCoords = this->MoveTargetCell;
		pLinked->PathDirections[0] = -1;
		pLinked->WaypointCell = CellStruct { -1, -1 };
		pLinked->WaypointNearbyAccessibleCellDelta = CellStruct { -1, -1 };
		pLinked->UpdatePosition(PCPType::End);

		const auto nextSpan = Footprint::GetSpan(pUnit, this->MoveTargetCell, pState->Bucket);

		if (!Footprint::CommitSpan(pUnit, nextSpan, pState->Bucket,
			pState->Alt, this->MoveTargetCell))
		{
			// Another hull won the race for this cell. Put the model back at
			// the last committed centre instead of leaving it standing in a
			// cell we do not own.
			pLinked->SetLocation(CellClass::Cell2Coord(oldAnchor, pLinked->Location.Z));
			this->Stop_Moving();
			return false;
		}

		this->HeadToCoord = CoordStruct::Empty;
		this->PathIndex++;
		pState->Translating = false;

		Footprint::LogCarrierArtDiag(pUnit, "TranslateEnd");
	}

	return true;
}

void FootprintLocomotionClass::ClearPath()
{
	this->PathLength = 0;
	this->PathIndex = 0;
}

void FootprintLocomotionClass::RecordFailure(const CellStruct& goal)
{
	this->LastFailedCell = goal;
	this->LastFailFrame = Unsorted::CurrentFrame;
	++this->RetryCount;

	if (this->RetryWindowStart == 0)
		this->RetryWindowStart = Unsorted::CurrentFrame;
}

bool FootprintLocomotionClass::IsFailureCooldown(const CellStruct& goal) const
{
	if (this->LastFailedCell.X < 0 || this->LastFailedCell != goal || this->LastFailFrame == 0)
		return false;

	return static_cast<DWORD>(Unsorted::CurrentFrame) - this->LastFailFrame < kMoveFailCooldownFrames;
}

void FootprintLocomotionClass::ResetRetry()
{
	this->LastFailedCell = CellStruct { -1, -1 };
	this->LastFailFrame = 0;
	this->RetryCount = 0;
	this->RetryWindowStart = 0;
}

bool FootprintLocomotionClass::RetryExpired() const
{
	if (this->RetryCount == 0)
		return false;

	return static_cast<DWORD>(Unsorted::CurrentFrame) - this->RetryWindowStart >= kMoveRetryTimeoutFrames;
}

void FootprintLocomotionClass::LogBlocked(const char* reason, const CellStruct& cell)
{
	if (Unsorted::CurrentFrame - this->LastLogFrame < 30)
		return;

	this->LastLogFrame = Unsorted::CurrentFrame;

	Debug::CarrierLog("[FootprintMove] blocked %s at cell=(%d,%d) unit=%s\n",
		reason, cell.X, cell.Y,
		this->LinkedTo ? this->LinkedTo->GetTechnoType()->ID : "?");
}
