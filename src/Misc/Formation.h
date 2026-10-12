#pragma once

#include <GeneralStructures.h>
#include <Dir.h>
#include <vector>
#include <map>

class FootClass;

enum class FormationType : int
{
	None = 0,
	Linear = 1,     // Linear formation - short drag (within 6 cells)
	Pyramid = 2,    // Pyramid formation - medium drag (6-12 cells)
	Wedge = 3       // Wedge formation - long drag (beyond 12 cells)
};

struct PendingFacing
{
	DirStruct Facing;
	int FramesPending = 0;
	bool HasStartedMoving = false;
};

class FormationManager
{
public:
	static FormationManager Instance;

	void Process();

private:
	bool m_isActive = false;
	bool m_wasPressed = false;
	Point2D m_startScreenPos = { 0, 0 };
	CoordStruct m_startWorldPos = { 0, 0, 0 };
	std::map<FootClass*, PendingFacing> m_pendingFacings;

	bool IsMiddleButtonPressed() const;
	Point2D GetMouseScreenPos() const;
	bool ShouldActivate() const;
	std::vector<FootClass*> GetSelectedFoots() const;
	int GetDragDistanceWorld() const;
	FormationType DetermineFormationType(int dragWorld) const;
	void CalculateDirectionVectors(double& fwdX, double& fwdY, double& rightX, double& rightY) const;
	DirStruct CalculateFormationDir(double fwdX, double fwdY) const;
	bool IsPositionPassable(const CoordStruct& pos, FootClass* pFoot) const;
	CoordStruct FindNearestPassable(const CoordStruct& pos, FootClass* pFoot,
		double fwdX, double fwdY) const;
	void CalculateFormationPositions(std::vector<CoordStruct>& positions, FormationType type, int count,
		double fwdX, double fwdY, double rightX, double rightY, const std::vector<FootClass*>& units) const;
	void DrawUnitPreview(FootClass* pFoot, const Point2D& screenPos, const CoordStruct& worldPos,
		const DirStruct& formationDir) const;
	void DrawFormationPreview(FormationType type, const std::vector<CoordStruct>& positions,
		const std::vector<FootClass*>& units, const DirStruct& formationDir) const;
	void ExecuteFormation(const std::vector<CoordStruct>& positions,
		const std::vector<FootClass*>& units, const DirStruct& formationDir);
	void ProcessPendingFacings();
};
