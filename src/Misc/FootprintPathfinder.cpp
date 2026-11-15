#include "FootprintPathfinder.h"

#include <Ext/Unit/Footprint.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace
{
	constexpr int kMapSide = 512;
	constexpr int kMaxExpansions = 20000;

	const int kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	const int kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

	struct OpenNode
	{
		float F;
		uint32_t Key;

		bool operator>(const OpenNode& other) const
		{
			if (this->F != other.F)
				return this->F > other.F;

			return this->Key > other.Key;
		}
	};

	int CellIndex(const CellStruct& cell)
	{
		return cell.Y * kMapSide + cell.X;
	}

	bool InBounds(int x, int y)
	{
		return x >= 0 && y >= 0 && x < kMapSide && y < kMapSide;
	}

	CellStruct CellFromIndex(int index)
	{
		return CellStruct
		{
			static_cast<short>(index % kMapSide),
			static_cast<short>(index / kMapSide)
		};
	}

	uint32_t MakeKey(const CellStruct& cell, int bucket)
	{
		return static_cast<uint32_t>(CellIndex(cell)) * 8u + static_cast<uint32_t>(bucket & 7);
	}

	void DecodeKey(uint32_t key, CellStruct& cell, int& bucket)
	{
		const int cellIndex = static_cast<int>(key / 8u);
		bucket = static_cast<int>(key % 8u);
		cell = CellFromIndex(cellIndex);
	}

	float OctileDistance(const CellStruct& from, const CellStruct& goal)
	{
		const int dx = std::abs(from.X - goal.X);
		const int dy = std::abs(from.Y - goal.Y);
		const int diagonal = std::min(dx, dy);
		const int straight = std::max(dx, dy) - diagonal;

		return static_cast<float>(straight) + diagonal * 1.41421356f;
	}

}

bool FootprintPathfinder::FindPath(
	UnitClass* pUnit,
	const CellStruct& start,
	const CellStruct& goal,
	int startBucket,
	std::vector<FootprintPathStep>& outPath)
{
	outPath.clear();

	if (!pUnit || !Footprint::IsMultiCell(pUnit))
		return false;

	if (!InBounds(start.X, start.Y) || !InBounds(goal.X, goal.Y))
		return false;

	if (start == goal)
		return true;

	const auto pType = pUnit->GetTechnoType();
	const bool alt = pUnit->OnBridge;
	const int rot = std::max(pType->ROT, 1);
	const float framesPerCell = 256.0f / static_cast<float>(std::max(pType->Speed, 1));
	const float turnFramesPer45 = 32.0f / static_cast<float>(rot);

	const auto& config = Footprint::GetConfig(pType);
	const int footprintArea = std::max(1, config.Length * config.Width);
	const bool largeHull = footprintArea > 64;
	// A 36x9 hull has a 1936-cell turn sweep and millions of cell checks can
	// easily exceed a frame. Keep the search bounded for large footprints;
	// failures are also put on a cooldown by the locomotor.
	const int maxExpansions = largeHull
		? std::clamp(200000 / footprintArea, 400, 4000)
		: kMaxExpansions;

	const uint32_t startKey = MakeKey(start, startBucket);
	const uint32_t goalIndex = static_cast<uint32_t>(CellIndex(goal));

	std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<OpenNode>> open;
	std::unordered_map<uint32_t, float> gScore;
	std::unordered_map<uint32_t, uint32_t> cameFrom;
	std::unordered_set<uint32_t> closed;

	gScore.reserve(4096);
	cameFrom.reserve(4096);
	closed.reserve(4096);

	gScore[startKey] = 0.0f;
	open.push(OpenNode { OctileDistance(start, goal) * framesPerCell, startKey });

	int expansions = 0;
	uint32_t goalKey = 0;
	bool found = false;

	while (!open.empty() && expansions < maxExpansions)
	{
		const OpenNode current = open.top();
		open.pop();

		if (closed.find(current.Key) != closed.end())
			continue;

		closed.insert(current.Key);
		++expansions;

		CellStruct cell;
		int bucket = 0;
		DecodeKey(current.Key, cell, bucket);

		if (current.Key != startKey && static_cast<uint32_t>(CellIndex(cell)) == goalIndex)
		{
			goalKey = current.Key;
			found = true;
			break;
		}

		const float baseCost = gScore[current.Key];

		// Turn in place. The sweep is checked against the full continuous
		// rotation, not just the target facing.
		for (int newBucket = 0; newBucket < 8; ++newBucket)
		{
			if (newBucket == bucket)
				continue;

			const int delta = (newBucket - bucket + 8) & 7;
			const int steps = std::min(delta, 8 - delta);

			// For very large hulls, turning several buckets at once produces
			// enormous swept areas. Chain adjacent turns instead so the path
			// search stays tractable while still allowing any final facing.
			if (largeHull && steps > 1)
				continue;

			const float turnCost = steps * turnFramesPer45 + 1.0f;

			const bool tryClockwise = delta <= 4;
			const bool tryCounterClockwise = delta >= 4;

			for (int pass = 0; pass < 2; ++pass)
			{
				const bool clockwise = pass == 0;

				if ((clockwise && !tryClockwise) || (!clockwise && !tryCounterClockwise))
					continue;

				const auto sweep = Footprint::GetSweepAt(pUnit, cell, bucket, newBucket, clockwise);

				if (!Footprint::CanOccupy(pUnit, sweep, alt, cell))
					continue;

				const uint32_t newKey = MakeKey(cell, newBucket);
				const float newCost = baseCost + turnCost;

				auto it = gScore.find(newKey);

				if (it == gScore.end() || newCost < it->second)
				{
					gScore[newKey] = newCost;
					cameFrom[newKey] = current.Key;
					open.push(OpenNode { newCost + OctileDistance(cell, goal) * framesPerCell, newKey });
				}
			}
		}

		// Move one cell, always in the direction the hull is facing. Reverse
		// movement is deliberately not generated: a hull that needs to go
		// backwards must plan a turn first instead of backing up.
		{
			const int dir = bucket;
			const int nx = cell.X + kDirX[dir];
			const int ny = cell.Y + kDirY[dir];

			if (!InBounds(nx, ny))
				continue;

			const CellStruct next { static_cast<short>(nx), static_cast<short>(ny) };

			if (!Footprint::IsClimbAllowed(pUnit, cell, next))
				continue;

			const auto span = Footprint::GetSpan(pUnit, next, bucket);

			if (!Footprint::CanOccupy(pUnit, span, alt, next))
				continue;

			const bool diagonal = (kDirX[dir] != 0 && kDirY[dir] != 0);
			const float moveCost = framesPerCell * (diagonal ? 1.41421356f : 1.0f);
			const uint32_t newKey = MakeKey(next, bucket);
			const float newCost = baseCost + moveCost;

			auto it = gScore.find(newKey);

			if (it == gScore.end() || newCost < it->second)
			{
				gScore[newKey] = newCost;
				cameFrom[newKey] = current.Key;
				open.push(OpenNode { newCost + OctileDistance(next, goal) * framesPerCell, newKey });
			}
		}
	}

	if (!found)
	{
		Debug::CarrierLog("[FootprintPath] no path from (%d,%d) bucket %d to (%d,%d), expansions=%d\n",
			start.X, start.Y, startBucket, goal.X, goal.Y, expansions);
		return false;
	}

	std::vector<uint32_t> keys;
	keys.reserve(64);
	uint32_t key = goalKey;

	for (;;)
	{
		keys.push_back(key);

		if (key == startKey)
			break;

		auto it = cameFrom.find(key);

		if (it == cameFrom.end())
			break;

		key = it->second;
	}

	std::reverse(keys.begin(), keys.end());

	for (size_t i = 1; i < keys.size(); ++i)
	{
		CellStruct previousCell;
		int previousBucket = 0;
		CellStruct currentCell;
		int currentBucket = 0;

		DecodeKey(keys[i - 1], previousCell, previousBucket);
		DecodeKey(keys[i], currentCell, currentBucket);

		FootprintPathStep step;

		if (previousCell == currentCell)
		{
			step.Cell = currentCell;
			step.Bucket = currentBucket;
			step.TurnOnly = true;
		}
		else
		{
			step.Cell = currentCell;
			step.Bucket = currentBucket;
			step.TurnOnly = false;
			step.Reverse = false;
		}

		outPath.push_back(step);

		if (outPath.size() >= 64)
			break;
	}

	Debug::CarrierLog("[FootprintPath] path %d steps, expansions=%d, goal bucket %d\n",
		static_cast<int>(outPath.size()), expansions, startBucket);

	return !outPath.empty();
}
