#pragma once

// Reverse engineering and coding by: CrimRecya

#include <ArrayClasses.h>
#include <GeneralStructures.h>
#include <FootClass.h>

class TechnoClass;
struct AStarClass_PathNodeBuffer;
struct AStarClass_PathQueueBuffer;
struct AStarClass_HierarchicalBuffer;
struct PriorityQueueClass_PathQueueNode;
struct PriorityQueueClass_HierarchicalNode;

struct AStarClass_PassabilityData
{
	unsigned short Indices[500];
};
static_assert(sizeof(AStarClass_PassabilityData) == 0x3E8);

class PathFinderData
{
	CellStruct StartCell;
	int TotalDistance;
	int PathLength;
	int* Directions;
	int unknown_int_10;
	int* Levels;
	CellStruct unknown_cellstruct_18;
	int unknown_int_1C;
};
static_assert(sizeof(PathFinderData) == 0x20);

class AStarClass
{
public:
	//Static
	DEFINE_REFERENCE(AStarClass, Instance, 0x87E8B8u)

	int AttemptPath(
		CellStruct* pFromMapCrd,
		CellStruct* pToMapCrd,
		FootClass* pFoot,
		bool bFromAlt,
		bool bToAlt,
		MovementZone nMovementZone = MovementZone::None)
		{ JMP_THIS(0x42D170); }

	PathFinderData* FindPath(
		CellStruct* pStart,
		CellStruct* pEnd,
		FootClass* pFoot,
		int* pDirs,
		int maxSteps,
		MovementZone movementZone,
		int mode)
	{
		JMP_THIS(0x42C900);
	}

	char unknown_byte_0;
	bool FindBridgeDir;
	char unknown_byte_2;
	bool CanFindPath;
	float PathCostFactor;
	bool IsAlt;
	PROTECTED_PROPERTY(BYTE, padding_9_B[3]);
	AStarClass_PathNodeBuffer* PathNodeBuffer;
	AStarClass_PathQueueBuffer* PathQueueBuffer;
	PriorityQueueClass_PathQueueNode* PathQueue;
	int* VisitCounts;
	int* AltVisitCounts;
	float* AltDistances;
	float* Distances;
	int SearchID;
	SpeedType FinderSpeedType;
	int StartLevel;
	int EndLevel;
	bool IsSearching;
	PROTECTED_PROPERTY(BYTE, padding_39_3B[3]);
	int FindMode;
	int* LevelVisitedMarkers[3];
	int* OpenSetMarkers[3];
	float* GCostArray[3];
	AStarClass_HierarchicalBuffer* HierarchyBuffer;
	PriorityQueueClass_HierarchicalNode* HierarchyQueue;
	int PathLength;
	CellStruct CellStructBuffer;
	DynamicVectorClass<unsigned int> ZoneIndices[3];
	AStarClass_PassabilityData PassabilityData[3];
	int PassabilityCounts[3];
};

static_assert(sizeof(AStarClass) == 0xC80);
