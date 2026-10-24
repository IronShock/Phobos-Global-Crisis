#include "Phobos.h"
#include "Utilities/Macro.h"
#include "Ext/Techno/Body.h"
#include "Ext/TechnoType/Body.h"
#include "Ext/Unit/Footprint.h"

#include <CellClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <HouseClass.h>
#include <UnitClass.h>
#include <Unsorted.h>

class ExtSelection
{
public:
	using callback_type = bool(__fastcall*)(ObjectClass*);

	static inline class TacticalSelectablesHelper
	{
	public:
		inline size_t size()
		{
			return TacticalClass::Instance->SelectableCount;
		}

		inline TacticalSelectableStruct* begin()
		{
			return &Unsorted::TacticalSelectables[0];
		}

		inline TacticalSelectableStruct* end()
		{
			return &Unsorted::TacticalSelectables[size()];
		}
	} Array {};

	// Reversed from Is_Selectable, w/o Select call
	static bool ObjectClass_IsSelectable(ObjectClass* pThis)
	{
		const auto pOwner = pThis->GetOwningHouse();
		return pOwner && pOwner->IsControlledByCurrentPlayer()
			&& pThis->CanBeSelected() && pThis->CanBeSelectedNow()
			&& !pThis->InLimbo;
	}

	static bool Tactical_PointInRect(const LTRBStruct& rect, const Point2D& point)
	{
		return point.X >= rect.Left && point.X < rect.Left + rect.Right
			&& point.Y >= rect.Top && point.Y < rect.Top + rect.Bottom;
	}

	static bool Tactical_PointInQuad(const Point2D& point, const Point2D (&quad)[4])
	{
		int sign = 0;

		for (int i = 0; i < 4; ++i)
		{
			const auto& a = quad[i];
			const auto& b = quad[(i + 1) & 3];
			const long long cross = static_cast<long long>(b.X - a.X) * (point.Y - a.Y)
				- static_cast<long long>(b.Y - a.Y) * (point.X - a.X);

			if (cross == 0)
				continue;

			const int current = cross > 0 ? 1 : -1;

			if (sign && current != sign)
				return false;

			sign = current;
		}

		return true;
	}

	// Drag selection normally tests a single screen point per unit. For a
	// multi-cell unit a box can cover a hull cell without covering its anchor,
	// so test every authoritative footprint cell instead. Cells are projected
	// into the same client-space rectangle used by Tactical::MakeSelection.
	static bool Tactical_FootprintInSelectionRect(TacticalClass* pThis, LTRBStruct* pRect,
		const FootprintState* pState)
	{
		static constexpr int leptonsPerCell = 256;

		const Point2D rectCorners[4]
		{
			{ pRect->Left, pRect->Top },
			{ pRect->Left + pRect->Right, pRect->Top },
			{ pRect->Left + pRect->Right, pRect->Top + pRect->Bottom },
			{ pRect->Left, pRect->Top + pRect->Bottom }
		};

		for (const auto& offset : pState->Span)
		{
			const CellStruct cell
			{
				static_cast<short>(pState->Anchor.X + offset.X),
				static_cast<short>(pState->Anchor.Y + offset.Y)
			};

			const auto pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!pCell)
				continue;

			const auto center = pCell->GetCoordsWithBridge();
			const int z = center.Z;
			const int x = cell.X * leptonsPerCell;
			const int y = cell.Y * leptonsPerCell;

			const CoordStruct corners[4]
			{
				{ x, y, z },
				{ x + leptonsPerCell, y, z },
				{ x + leptonsPerCell, y + leptonsPerCell, z },
				{ x, y + leptonsPerCell, z }
			};

			Point2D quad[4];

			for (int i = 0; i < 4; ++i)
				quad[i] = pThis->CoordsToClient(corners[i]).first;

			const auto centerPoint = pThis->CoordsToClient(center).first;

			if (Tactical_PointInRect(*pRect, centerPoint))
				return true;

			for (const auto& point : quad)
			{
				if (Tactical_PointInRect(*pRect, point))
					return true;
			}

			// A narrow band can cut through a cell without containing any of its
			// corners. The reverse test catches that case as well.
			for (const auto& point : rectCorners)
			{
				if (Tactical_PointInQuad(point, quad))
					return true;
			}
		}

		return false;
	}

	// Reversed from Tactical::Select
	static bool Tactical_IsInSelectionRect(TacticalClass* pThis, LTRBStruct* pRect, const TacticalSelectableStruct& selectable)
	{
		if (selectable.Object && selectable.Object->IsAlive)
		{
			if (const auto pUnit = abstract_cast<UnitClass*>(selectable.Object))
			{
				if (Footprint::IsMultiCell(pUnit))
				{
					if (const auto pState = Footprint::GetState(pUnit);
						pState && pState->Initialized)
					{
						return Tactical_FootprintInSelectionRect(pThis, pRect, pState);
					}
				}
			}

			const int nLocalX = selectable.X - pThis->TacticalPos.X;
			const int nLocalY = selectable.Y - pThis->TacticalPos.Y;

			if ((nLocalX >= pRect->Left && nLocalX < pRect->Right + pRect->Left)
				&& (nLocalY >= pRect->Top && nLocalY < pRect->Bottom + pRect->Top))
			{
				return true;
			}
		}
		return false;
	}

	static bool Tactical_IsHighPriorityInRect(TacticalClass* pThis, LTRBStruct* rect)
	{
		for (const auto& selected : Array)
			if (Tactical_IsInSelectionRect(pThis, rect, selected) && ObjectClass_IsSelectable(selected.Object))
			{
				if ((selected.Object->AbstractFlags & AbstractFlags::Techno) != AbstractFlags::None)
				{
					auto const& pExt = TechnoExt::ExtMap.Find(static_cast<TechnoClass*>(selected.Object));
					auto const& pTypeExt = TechnoTypeExt::ExtMap.Find(selected.Object->GetTechnoType());

					// TA.VirtualUnit: virtual units are never selected, so they
					// must not influence the high-priority selection decision.
					bool isVirtualUnit = TechnoExt::IsVirtualUnit(static_cast<TechnoClass*>(selected.Object));
					bool isLowPriorityByAttachment = !isVirtualUnit && pExt->ParentAttachment && pExt->ParentAttachment->GetType()->LowSelectionPriority;
					if (!pTypeExt->LowSelectionPriority && !isLowPriorityByAttachment)
						return true;
				}
			}

		return false;
	}

	// Reversed from Tactical::Select
	static void Tactical_SelectFiltered(TacticalClass* pThis, LTRBStruct* pRect, callback_type check_callback, bool bPriorityFiltering)
	{
		Unsorted::MoveFeedback = true;

		if (pRect->Right <= 0 || pRect->Bottom <= 0 || pThis->SelectableCount <= 0)
			return;

		for (const auto& selected : Array)
		{
			if (Tactical_IsInSelectionRect(pThis, pRect, selected))
			{
				const auto pObject = selected.Object;
				const auto pTechnoType = pObject->GetTechnoType(); // Returns nullptr on non techno objects

				if (auto const pTypeExt = TechnoTypeExt::ExtMap.TryFind(pTechnoType)) // If pTechnoType is nullptr so will be pTypeExt
				{
					// TA.VirtualUnit: virtual units are never selectable.
					if (TechnoExt::IsVirtualUnit(static_cast<TechnoClass*>(pObject)))
						continue;

					if (bPriorityFiltering)
					{
						auto const& pExt = TechnoExt::ExtMap.Find(static_cast<TechnoClass*>(pObject));
						// Attached units shouldn't be selected regardless of the setting
						bool isLowPriorityByAttachment = pExt->ParentAttachment && pExt->ParentAttachment->GetType()->LowSelectionPriority;
						bool isLowPriorityByTechno = Phobos::Config::PrioritySelectionFiltering && pTypeExt->LowSelectionPriority;

						if (isLowPriorityByAttachment || isLowPriorityByTechno)
							continue;
					}

					if (Game::IsTypeSelecting())
					{
						Game::UICommands_TypeSelect_7327D0(pTypeExt->GetSelectionGroupID());
						continue;
					}
				}

				if (check_callback)
				{
					(*check_callback)(pObject);
				}
				else
				{
					const auto pBldType = abstract_cast<BuildingTypeClass*, true>(pTechnoType);
					const auto pOwner = pObject->GetOwningHouse();

					if (pOwner && pOwner->IsControlledByCurrentPlayer() && pObject->CanBeSelected()
						&& (!pBldType || (pBldType->UndeploysInto && pBldType->IsVehicle())))
					{
						Unsorted::MoveFeedback = !pObject->Select();
					}
				}
			}
		}

		Unsorted::MoveFeedback = true;
	}

	// Reversed from Tactical::MakeSelection
	static void __fastcall Tactical_MakeFilteredSelection(TacticalClass* pThis, void* _, callback_type check_callback)
	{
		if (pThis->Band.Left || pThis->Band.Top)
		{
			int nLeft = pThis->Band.Left;
			int nRight = pThis->Band.Right;
			int nTop = pThis->Band.Top;
			int nBottom = pThis->Band.Bottom;

			if (nLeft > nRight)
				std::swap(nLeft, nRight);
			if (nTop > nBottom)
				std::swap(nTop, nBottom);

			LTRBStruct rect { nLeft , nTop, nRight - nLeft + 1, nBottom - nTop + 1 };

			bool bPriorityFiltering = Tactical_IsHighPriorityInRect(pThis, &rect);
			Tactical_SelectFiltered(pThis, &rect, check_callback, bPriorityFiltering);

			pThis->Band.Left = 0;
			pThis->Band.Top = 0;
		}
	}
};

// Replace single call
DEFINE_JUMP(CALL, 0x4ABCEB, GET_OFFSET(ExtSelection::Tactical_MakeFilteredSelection))

// Replace vanilla function. For in case another module tries to call the vanilla function at offset
DEFINE_JUMP(LJMP, 0x6D9FF0, GET_OFFSET(ExtSelection::Tactical_MakeFilteredSelection))
