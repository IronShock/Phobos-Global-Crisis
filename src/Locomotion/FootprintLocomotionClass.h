#pragma once

#include <LocomotionClass.h>

#include <Misc/FootprintPathfinder.h>

// Cell-to-cell mover for multi-cell units. It deliberately does not use the
// engine's TurnTrack tables: the hull's footprint is too large for the vanilla
// one-cell turning arcs. Movement is split into "translate to the next cell
// centre" and "turn in place with the swept area reserved".
class __declspec(uuid("7E2B1C64-3A9D-4F6B-8E1A-5C7D9B2F4A31"))
	FootprintLocomotionClass : public LocomotionClass
{
public:
	// IPersist
	virtual HRESULT __stdcall GetClassID(CLSID* pClassID) override
	{
		if (!pClassID)
			return E_POINTER;

		*pClassID = __uuidof(this);
		return S_OK;
	}

	// IPersistStream
	virtual HRESULT __stdcall Load(IStream* pStm) override;
	virtual HRESULT __stdcall Save(IStream* pStm, BOOL fClearDirty) override;

	virtual int Size() override { return sizeof(*this); }

	// ILocomotion
	virtual bool __stdcall Is_Moving() override { return this->Moving || this->Turning || this->HeadToCoord != CoordStruct::Empty; }
	virtual CoordStruct __stdcall Destination() override { return this->TargetCoord; }
	virtual CoordStruct __stdcall Head_To_Coord() override;
	virtual Move __stdcall Can_Enter_Cell(CellStruct cell) override;
	virtual Matrix3D __stdcall Draw_Matrix(VoxelIndexKey* key) override { return this->LocomotionClass::Draw_Matrix(key); }
	virtual Matrix3D __stdcall Shadow_Matrix(VoxelIndexKey* key) override { return this->LocomotionClass::Shadow_Matrix(key); }
	virtual int __stdcall Z_Adjust() override { return 0; }
	virtual ZGradient __stdcall Z_Gradient() override { return ZGradient::Deg90; }
	virtual bool __stdcall Process() override;
	virtual void __stdcall Move_To(CoordStruct to) override;
	virtual void __stdcall Stop_Moving() override;
	virtual void __stdcall Do_Turn(DirStruct dir) override;
	virtual void __stdcall Unlimbo() override;
	virtual bool __stdcall Power_On() override { this->Powered = true; return true; }
	virtual bool __stdcall Power_Off() override { this->Powered = false; return true; }
	virtual bool __stdcall Is_Powered() override { return this->Powered; }
	virtual Layer __stdcall In_Which_Layer() override { return Layer::Ground; }
	virtual bool __stdcall Is_Moving_Now() override { return this->Moving || this->Turning || this->HeadToCoord != CoordStruct::Empty; }
	virtual int __stdcall Apparent_Speed() override;
	virtual void __stdcall Mark_All_Occupation_Bits(MarkType mark) override;
	virtual bool __stdcall Is_Moving_Here(CoordStruct to) override;
	virtual void __stdcall Limbo() override;
	virtual void __stdcall Lock() override { this->UnLocked = false; }
	virtual void __stdcall Unlock() override { this->UnLocked = true; }

	FootprintLocomotionClass() : LocomotionClass() { }
	virtual ~FootprintLocomotionClass() override = default;

private:
	// The read-only carrier diagnostic lives in Footprint.cpp and needs the
	// private path/turn fields to correlate an engine position jump with the
	// exact locomotion state. It never mutates them.
	friend class Footprint;

	bool BeginTurn(int targetBucket, const CellStruct& anchor);
	void FinishTurn(const CellStruct& anchor);
	bool BeginMove(const FootprintPathStep& step, const CellStruct& anchor);
	// Keep the engine's map-coordinate bookkeeping in sync with the custom
	// mover when a turn ends / the next cell step starts. A stale
	// LastMapCoords / PathDirections pair made FootClass::UpdatePosition apply
	// its lateral correction on the first move after a turn.
	void ResetEngineMapState(const CellStruct& anchor);
	bool TranslateOneFrame();
	void ClearPath();
	void RecordFailure(const CellStruct& goal);
	bool IsFailureCooldown(const CellStruct& goal) const;
	void ResetRetry();
	bool RetryExpired() const;
	void LogBlocked(const char* reason, const CellStruct& cell);

	FootprintPathStep Path[64] = {};
	int PathLength = 0;
	int PathIndex = 0;

	CellStruct MoveTargetCell { 0, 0 };
	CellStruct TurnTargetCell { 0, 0 };
	CoordStruct TargetCoord = CoordStruct::Empty;
	// A new order received while a cell-to-cell interpolation is in flight.
	// Applying it immediately would snap the model to the next cell centre and
	// make rapid clicks look like teleporting; consume it when the step ends.
	CoordStruct PendingTarget = CoordStruct::Empty;
	CoordStruct HeadToCoord = CoordStruct::Empty;

	bool Moving = false;
	bool Turning = false;
	bool Powered = true;
	bool UnLocked = true;
	// Diagnostic-only: ensures one TranslateStart log per cell-to-cell step.
	bool DiagTranslateLogged = false;

	int TurnFromBucket = 0;
	int TurnTargetBucket = 0;
	bool TurnClockwise = true;
	int LastLogFrame = -1000;

	// A failed path/move must not be retried every frame. Large multi-cell
	// hulls can make one path attempt expensive, and the mission layer may
	// reissue the same destination immediately.
	CellStruct LastFailedCell { -1, -1 };
	DWORD LastFailFrame = 0;
	int RetryCount = 0;
	DWORD RetryWindowStart = 0;
};
