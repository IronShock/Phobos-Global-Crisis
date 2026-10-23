#pragma once

#include <UnitClass.h>

#include <vector>

struct FootprintPathStep
{
	CellStruct Cell { 0, 0 };
	int Bucket = 0;
	bool TurnOnly = false;
	bool Reverse = false;
};

class FootprintPathfinder
{
public:
	// Finds a path for a multi-cell unit. The state is (cell, facing bucket), so
	// turning clearance is part of the search rather than a post-process.
	static bool FindPath(
		UnitClass* pUnit,
		const CellStruct& start,
		const CellStruct& goal,
		int startBucket,
		std::vector<FootprintPathStep>& outPath);
};
