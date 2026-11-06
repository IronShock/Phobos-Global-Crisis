#include <Phobos.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Misc/FoggedVoxel.h>

#include <AnimTypeClass.h>
#include <BuildingClass.h>
#include <CellClass.h>
#include <DisplayClass.h>
#include <EBolt.h>
#include <FileSystem.h>
#include <GameModeOptionsClass.h>
#include <HouseClass.h>
#include <LaserDrawClass.h>
#include <MapClass.h>
#include <OverlayClass.h>
#include <RadBeam.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SmudgeClass.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TerrainClass.h>
#include <WaveClass.h>

#include <Ext/AnimType/Body.h>
#include <Ext/TerrainType/Body.h>

#include <algorithm>
#include <cstdint>

// Fog of War overhaul, ported from wenrui1245/Phobos-Recya (GPL-3.0).
// Fog owner used by optional observer/external integration; null = normal.
HouseClass* PhobosFogOwner = nullptr;

namespace FoggedObjectHelper
{
	bool NoReveal = false;
	struct FoggedObjectClassFake
	{
		char _[0x40];
		RectangleStruct RenderDimension;
		char __[0x24];
		bool Visible;
	};
	static_assert(sizeof(FoggedObjectClassFake) == 0x78u);
	bool SetVectorCapacity(DynamicVectorClass<FoggedObjectClass*>* pVector, int capacity)
	{
		return reinterpret_cast<bool(__thiscall*)(DynamicVectorClass<FoggedObjectClass*>*, int, FoggedObjectClass **)>(0x45A680)(pVector, capacity, nullptr);
	}
	bool ObjectUnlimbo(ObjectClass* pObject, const CoordStruct& position, DirType face)
	{
		return reinterpret_cast<bool(__thiscall*)(ObjectClass*, const CoordStruct&, DirType)>(0x5F4EC0)(pObject, position, face);
	}
	void AddItemToVector(DynamicVectorClass<FoggedObjectClass*>* pVector, FoggedObjectClass* pItem)
	{
		if(pVector->Count < pVector->Capacity
			|| (pVector->IsAllocated || !pVector->Capacity)
				&& pVector->CapacityIncrement > 0
				&& FoggedObjectHelper::SetVectorCapacity(pVector, pVector->Capacity + pVector->CapacityIncrement))
		{
			pVector->Items[pVector->Count++] = std::move(pItem);
		}
	}
	DynamicVectorClass<FoggedObjectClass*>* CreateGameDynamicVector()
	{
		void* raw = YRMemory::Allocate(sizeof(DynamicVectorClass<FoggedObjectClass*>));
		*reinterpret_cast<void**>(raw) = reinterpret_cast<void*>(0x7E44F4u);
		auto pVector = static_cast<DynamicVectorClass<FoggedObjectClass*>*>(raw);
		pVector->Items = nullptr;
		pVector->Capacity = 0;
		pVector->IsInitialized = true;
		pVector->IsAllocated = false;
		pVector->Count = 0;
		pVector->CapacityIncrement = 1;
		FoggedObjectHelper::SetVectorCapacity(pVector, 1);
		return pVector;
	}
	void ClearAllFog()
	{
		MapClass::Instance.CellIteratorReset();
		for (auto pCell = MapClass::Instance.CellIteratorNext(); pCell; pCell = MapClass::Instance.CellIteratorNext())
		{
			pCell->ShroudCounter = 0;
			pCell->GapsCoveringThisCell = 0;
			pCell->AltFlags |= (AltCellFlags::Mapped | AltCellFlags::NoFog);
			pCell->Flags |= (CellFlags::CenterRevealed | CellFlags::EdgeRevealed);
			pCell->Flags &= ~CellFlags::Fogged;
			pCell->ClearFoggedObjects();
		}
		MapClass::Instance.sub_657CE0();
		MapClass::Instance.MarkNeedsRedraw(2);
	}
	static FoggedObjectClass* __fastcall CreateFoggedOverlay(void* pThis, void* _, const CoordStruct& coords, int OverlayTypeIndex, int OverlayData) JMP_THIS(0x4D0980);
	static FoggedObjectClass* __fastcall CreateFoggedSmudge(void* pThis, void* _, const CoordStruct& coords, int SmudgeTypeIndex, int SmudgeData) JMP_THIS(0x4D0C40);
	static FoggedObjectClass* __fastcall CreateFoggedTerrain(void* pThis, void* _, TerrainClass* pTerrain) JMP_THIS(0x4D1370);
	static void __fastcall TechnoClass_RevealLastSight(TechnoClass* pThis, void* _, bool OnlyOutline, bool RevealByHeight, bool specifiedHouse, HouseClass *pHouse)
	{
		if (!pThis->IsInPlayfield || !pThis->unknown_bool_250)
			return;

		const auto pOwner = pThis->Owner;
		if (pOwner->Type->MultiplayPassive)
			return;

		pThis->unknown_bool_250 = false;

		const int lastSightRange = pThis->LastSightRange;
		if (!lastSightRange)
			return;

		// 只更新计数，不更新视野
		FoggedObjectHelper::NoReveal = true;
		MapClass::Instance.RevealArea2(&pThis->LastSightCoords, lastSightRange, pOwner, false, false, false, true, true);
		FoggedObjectHelper::NoReveal = false;
	}
	class CellClassFake final : public CellClass
	{
	public:
		FoggedObjectClass* FreezeOverlay(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedOverlay(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this->GetCoords(),
				this->OverlayTypeIndex,
				static_cast<unsigned int>(this->OverlayData)
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
		FoggedObjectClass* FreezeSmudge(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedSmudge(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this->GetCoords(),
				this->SmudgeTypeIndex,
				static_cast<unsigned int>(this->SmudgeData)
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
	};
	class TerrainClassFake final : public TerrainClass
	{
	public:
		FoggedObjectClass* FreezeTerrain(DynamicVectorClass<FoggedObjectClass*>* pVector, bool visible)
		{
			const auto pObject = FoggedObjectHelper::CreateFoggedTerrain(
				YRMemory::Allocate(sizeof(FoggedObjectClassFake)),
				0,
				this
			);
			reinterpret_cast<FoggedObjectClassFake*>(pObject)->Visible = visible;
			auto rect = reinterpret_cast<FoggedObjectClassFake*>(pObject)->RenderDimension;
			rect.X -= TacticalClass::Instance->TacticalPos.X;
			rect.Y -= TacticalClass::Instance->TacticalPos.Y;
			TacticalClass::Instance->RegisterDirtyArea(rect, 0);
			FoggedObjectHelper::AddItemToVector(pVector, pObject);
			return pObject;
		}
		bool TerrainClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (this->IsAlive && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					this->FreezeTerrain(pCell->FoggedObjects, false);
				}
			}
			return result;
		};
	};
	class OverlayClassFake final : public OverlayClass
	{
	public:
		bool OverlayClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (pCell->OverlayTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeOverlay(pCell->FoggedObjects, false);
				}
			}
			return result;
		}
	};
	class SmudgeClassFake final : public SmudgeClass
	{
	public:
		bool SmudgeClass_Unlimbo_CheckFog(const CoordStruct& coords, DirType dir)
		{
			const bool result = FoggedObjectHelper::ObjectUnlimbo(this, coords, dir);
			if (result && ScenarioClass::Instance->SpecialFlags.FogOfWar)
			{
				const auto pCell = MapClass::Instance.GetCellAt(coords);
				if (pCell->SmudgeTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
				{
					if (!pCell->FoggedObjects)
						pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

					static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeSmudge(pCell->FoggedObjects, false);
				}
			}
			return result;
		}
	};

	// FoggedObjectClass 关键字段布局（依据 0x4D0980/0x4D0EF0 的构造与 0x486C50 的清理逻辑）
	struct FoggedObjectFields
	{
		char _[0x24];
		int OverlayTypeIndex;           // +0x24 覆盖物：类型索引
		HouseClass* Owner;              // +0x28 建筑：冻结时的所有者 House（0x4D0EF0 从 BuildingClass+0x21C 写入，不是建筑实例）
		int OverlayData;                // +0x2C 覆盖物：数量
		int FoggedType;                 // +0x30 类别：6=建筑 0x14=覆盖物 0x1D=弹坑
		CoordStruct Position;           // +0x34 冻结时的位置
		RectangleStruct RenderDimension;// +0x40 渲染矩形
	};
	static_assert(sizeof(FoggedObjectFields) == 0x50u);

	// 全局绘制数组（0x8B3CC0 元素 / 0x8B3CC4 数量）的元素：{类别排序键, 冻结对象}
	struct FoggedDrawRecord
	{
		int SortKey;
		FoggedObjectClass* Object;
	};
	static_assert(sizeof(FoggedDrawRecord) == 0x8u);

	// FoggedObjectClass 内部 DynamicVectorClass<DrawRecord>（位于 +0x5C）。元素是虚表
	// 指针可被虚拟调用的类型对象（每项 0x10 字节），这里只需要判空与数量上限
	struct FoggedObjectDrawRecords
	{
		void* VTable;                  // +0x5C
		void* Items;                   // +0x60
		int Capacity;                  // +0x64
		bool IsInitialized;            // +0x68
		bool IsAllocated;              // +0x69
		char _padding[2];              // +0x6A
		int Count;                     // +0x6C
		int CapacityIncrement;         // +0x70
	};
	static_assert(sizeof(FoggedObjectDrawRecords) == 0x18u);

	// FoggedObjectClass 主虚表地址（0x7E8B38，前四个虚表指针依次为主表/
	// INoticeSource/INoticeSink/IRTTITypeInfo，与 0x4D255E 处的构造一致）
	constexpr uintptr_t FoggedObjectClassVTable = 0x7E8B38u;

	// 析构函数（0x4D1650）在 0x4D1722 处按“格子坐标 + FoggedType”算出的排序键做一次
	// 二分查找来摘除绘制记录。同一格、同一类别存在多个冻结对象时排序键相同，可能摘错
	// 记录，结果是被销毁的对象仍留在绘制数组（0x8B3CC0）里：下一帧 0x4D1890 会拿已
	// 释放的对象去绘制——析构里 +0x60（DrawRecord 向量）已被释放置空，+0x6C（数量）
	// 却保持原值，建筑分支 0x4D1C6B 便以空指针为基址读取而崩溃。
	// 这里在析构释放向量之前按对象指针精确清理一遍，保证数组里不残留指向自己的记录。
	void RemoveDrawRecordsOf(const FoggedObjectClass* pObject)
	{
		auto const pItems = *reinterpret_cast<FoggedDrawRecord**>(0x8B3CC0);
		auto const pCount = reinterpret_cast<int*>(0x8B3CC4);

		if (!pItems || *pCount <= 0)
			return;

		const int count = *pCount;
		int write = 0;

		for (int read = 0; read < count; ++read)
		{
			if (pItems[read].Object != pObject)
				pItems[write++] = pItems[read];
		}

		if (write == count)
			return;

		for (int i = write; i < count; ++i)
			pItems[i] = FoggedDrawRecord { 0, nullptr };

		*pCount = write;
		*reinterpret_cast<char*>(0x8B3CCC) = 0; // 数组已变动，下次使用时重新排序
	}

	// 游戏内部的坐标→格子换算（与 0x486CA7 处的实现一致）
	CellStruct CoordToCell(const CoordStruct& coords)
	{
		const int x = (coords.X + (coords.X < 0 ? 255 : 0)) >> 8;
		const int y = (coords.Y + (coords.Y < 0 ? 255 : 0)) >> 8;
		return CellStruct { static_cast<short>(x), static_cast<short>(y) };
	}

	// 从格子的迷雾对象向量中移除指定项（保持其余项顺序）
	void RemoveFromVector(DynamicVectorClass<FoggedObjectClass*>* pVector, FoggedObjectClass* pItem)
	{
		for (int i = 0; i < pVector->Count; ++i)
		{
			if (pVector->Items[i] == pItem)
			{
				for (int j = i + 1; j < pVector->Count; ++j)
					pVector->Items[j - 1] = pVector->Items[j];

				--pVector->Count;
				return;
			}
		}
	}

	// 移除一个迷雾冻结对象：按其覆盖范围同步清理所有引用它的格子，标记脏区域后销毁
	void RemoveFoggedObject(CellClass* pOwnerCell, FoggedObjectClass* pItem)
	{
		const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);
		const auto baseCell = CoordToCell(pFields->Position);

		if (pFields->FoggedType == 6)
		{
			// 建筑冻结对象同时被其覆盖的每个格子引用（0x4D2790 返回其覆盖范围）
			const auto pOffsets = reinterpret_cast<CellStruct* (__thiscall*)(FoggedObjectClass*)>(0x4D2790)(pItem);

			if (pOffsets)
			{
				for (auto pOffset = pOffsets; ; ++pOffset)
				{
					if (*pOffset == CellStruct { 0x7FFF, 0x7FFF })

						break;

					if (const auto pCell = MapClass::Instance.TryGetCellAt(baseCell + *pOffset))
					{
						if (pCell->FoggedObjects)
							RemoveFromVector(pCell->FoggedObjects, pItem);
}

				}
			}
		}
		else if (pOwnerCell && pOwnerCell->FoggedObjects)
		{
			// 非建筑冻结对象只被单个格子引用
			RemoveFromVector(pOwnerCell->FoggedObjects, pItem);
		}

		auto rect = pFields->RenderDimension;
		rect.X -= TacticalClass::Instance->TacticalPos.X;
		rect.Y -= TacticalClass::Instance->TacticalPos.Y;
		TacticalClass::Instance->RegisterDirtyArea(rect, 0);

		// 虚表 +0x20 处为标量删除析构（与 0x486D5D 处一致）
		using DeletingDtor_t = void(__thiscall*)(FoggedObjectClass*, bool);
		const auto pDtor = reinterpret_cast<DeletingDtor_t*>(*reinterpret_cast<void**>(pItem))[8];
		pDtor(pItem, true);
	}
}

using FoggedObjectFields = FoggedObjectHelper::FoggedObjectFields;
using FoggedObjectHelper::RemoveFoggedObject;

DEFINE_HOOK(0x687C56, INIClass_ReadScenario_EnableFog, 0x5)
{
	// Follow the scenario / game-mode setting instead of forcing the rules value:
	// campaign maps such as TUTO4 write [SpecialFlags] FogOfWar=no and must stay
	// fully revealed. The rules switch only decides whether the port is allowed.
	bool fog = ScenarioClass::Instance->SpecialFlags.FogOfWar;

	if (!SessionClass::IsCampaign())
		fog = fog || GameModeOptionsClass::Instance.FogOfWar;

	fog = fog && Phobos::Config::TSFog;

	GameModeOptionsClass::Instance.FogOfWar = fog;
	ScenarioClass::Instance->SpecialFlags.FogOfWar = fog;
	return 0;
}

// 像黑幕那样自动刷新周围格子的绘制状态，防止边缘出现锯齿
DEFINE_HOOK(0x4A9CA0, DisplayClass_RevealFogShroud_Reimplement, 0x8)
{
	enum { SkipGameCode = 0x4A9DC6 };

	GET(DisplayClass*, pThis, ECX);
	GET_STACK(CellStruct*, pCellStruct, STACK_OFFSET(0x0, 0x4));
	GET_STACK(HouseClass*, pHouse, STACK_OFFSET(0x0, 0x8));
	GET_STACK(bool, increase, STACK_OFFSET(0x0, 0xC));
	// Kaleidoscope fog owner: only the owning house updates the fog
	if (PhobosFogOwner && pHouse != PhobosFogOwner)
		return SkipGameCode;

	const auto pCell = pThis->GetCellAt(*pCellStruct);
	const auto flags = pCell->Flags;

	const bool edgeNotRevealed = !(flags & CellFlags::EdgeRevealed);
	const bool shouldRadarRedraw = edgeNotRevealed || !(pCell->AltFlags & AltCellFlags::Mapped);
	bool shouldRedraw = shouldRadarRedraw;
	pCell->Flags = FoggedObjectHelper::NoReveal ? (flags & ~CellFlags::IsPlot) : (flags & ~CellFlags::IsPlot | CellFlags::EdgeRevealed);

	if (increase)
		pCell->IncreaseShroudCounter();
	else
		pCell->ReduceShroudCounter();

	char newVisibility = TacticalClass::Instance->GetOcclusion(*pCellStruct, false);
	char newFoggedness = TacticalClass::Instance->GetOcclusion(*pCellStruct, true);

	if (newVisibility != pCell->Visibility)
	{
		shouldRedraw = true;
		pCell->Visibility = newVisibility;
	}

	if (pCell->Visibility == -1)
		pCell->AltFlags |= AltCellFlags::NoFog;

	if (newFoggedness != pCell->Foggedness)
	{
		shouldRedraw = true;
		pCell->Foggedness = newFoggedness;
	}

	if (pCell->Foggedness == -1)
	{
		pCell->Flags |= CellFlags::CenterRevealed;

		// 修复：完全可见的格子不应保留 Fogged 标记。间谍卫星揭幕后格子带有
		// EdgeRevealed，而 CleanFog 只在格子“新变为 EdgeRevealed”时触发；此后
		// FogCell（迷雾扩散/下坡遮挡）会重新给这些格子打上 Fogged 却不更新
		// Foggedness，形成“逻辑迷雾/视觉无雾”的错位——单位走入后被当作处于
		// 迷雾中（不绘制且被取消选中）。凡视野更新到完全可见的格子即清理。
		if (pCell->Flags & CellFlags::Fogged)
			pCell->CleanFog();
	}

	if (shouldRedraw)
		TacticalClass::Instance->RegisterCellAsVisible(pCell);

	if ((pCell->AltFlags & AltCellFlags::Mapped) && ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		for (size_t i = 0; i < 8; ++i)
		{
			auto adjCell = Unsorted::AdjacentCell[i & 7] + *pCellStruct;
			const auto pAdjCell = pThis->GetCellAt(adjCell);

			if (adjCell != *pCellStruct && !(pAdjCell->Flags & CellFlags::CenterRevealed))
			{
				const char adjNewFoggedness = TacticalClass::Instance->GetOcclusion(adjCell, true);
				if (adjNewFoggedness == -1)
				{
					if (pAdjCell->Flags & CellFlags::EdgeRevealed)
					{
						pAdjCell->Flags |= CellFlags::CenterRevealed;
						TacticalClass::Instance->RegisterCellAsVisible(pAdjCell);

						for (size_t j = 0; j < 8; ++j)
						{
							const auto nextAdjCell = Unsorted::AdjacentCell[j & 7] + adjCell;
							const auto pNextAdjCell = pThis->GetCellAt(nextAdjCell);

							const char nextAdjNewFoggedness = TacticalClass::Instance->GetOcclusion(nextAdjCell, true);
							if (nextAdjNewFoggedness != pNextAdjCell->Foggedness)
							{
								pNextAdjCell->Foggedness = nextAdjNewFoggedness;
								TacticalClass::Instance->RegisterCellAsVisible(pNextAdjCell);
							}
						}

						continue;
					}
				}
				else
				{
					if (adjNewFoggedness == -2 || (pAdjCell->Flags & CellFlags::EdgeRevealed))
					{
						if (adjNewFoggedness >= 0 && adjNewFoggedness != pAdjCell->Foggedness)
						{
							pAdjCell->Foggedness = adjNewFoggedness;
							TacticalClass::Instance->RegisterCellAsVisible(pAdjCell);
						}

						continue;
					}
				}

				pThis->MapCellFoggedness(&adjCell, pHouse);
				continue;
			}
		}
	}

	if (shouldRedraw)
		MapClass::Instance.RevealCheck(pCell, pHouse, shouldRadarRedraw);

	if ((pCell->Flags & CellFlags::EdgeRevealed) && edgeNotRevealed && ScenarioClass::Instance->SpecialFlags.FogOfWar)
		pCell->CleanFog();

	return SkipGameCode;
}

DEFINE_HOOK(0x4A9DF1, DisplayClass_MapCellFoggedness_NoUpdate, 0x6)
{
	enum { SkipGameCode = 0x4A9DFE };

	GET(CellFlags, flags, EAX);

	R->ECX(!(flags & CellFlags::EdgeRevealed));

	flags &= ~CellFlags::IsPlot;
	if (!FoggedObjectHelper::NoReveal)
		flags |= CellFlags::EdgeRevealed;

	R->EAX(flags);

	return SkipGameCode;
}

// 像黑幕刷新那样考虑飞行单位，确保未移动的飞行单位的视野不会被刷新掉，并且友军视野不只看建筑，避免频繁刷新
DEFINE_HOOK(0x4ADFF0, MapClass_AllToSee_Reimplement, 0x5)
{
	enum { SkipGameCode = 0x4AE0A5 };

	GET_STACK(bool, notForBuilding, STACK_OFFSET(0x0, 0x4));
	GET_STACK(bool, revealFlag, STACK_OFFSET(0x0, 0x8));

	const auto& vec = TechnoClass::Array;
	for (int i = 0; i < vec.Count; ++i)
	{
		const auto pTechno = vec.Items[i];
		if (pTechno && !pTechno->InLimbo && pTechno->IsOnMap)
		{
			if (pTechno->WhatAmI() != AbstractType::Building || !notForBuilding)
			{
				const auto pOwner = pTechno->Owner;
				if (pOwner->IsControlledByCurrentPlayer())
				{
					if (pTechno->DiscoveredByCurrentPlayer)
						pTechno->See(false, revealFlag);
				}
				else if (RulesClass::Instance->AllyReveal)
				{
					if (pOwner->IsAlliedWith(HouseClass::CurrentPlayer))
						pTechno->See(revealFlag, false);
				}
			}
		}
	}

	return SkipGameCode;
}

// 增加一种立即刷新迷雾的机制，甚至！？性能提升？！
DEFINE_HOOK(0x4ACBC4, MapClass_FogSpread_SkipWithSpySat, 0x5)
{
	enum { SkipGameCode = 0x4ACC4B, SpreadFogOfWar = 0x4ACBF8 };

	GET(MapClass*, pThis, ECX);

	const auto pPlayer = HouseClass::CurrentPlayer;
	if (pPlayer->Defeated)
		return SkipGameCode;

	pThis->CellIteratorReset();
	if (RulesClass::Instance->ShadowGrow)
	{
		for (auto pCell = pThis->CellIteratorNext(); pCell; pCell = pThis->CellIteratorNext())
			pCell->Flags |= CellFlags::IsPlot;
	}
	else
	{
		for (auto pCell = pThis->CellIteratorNext(); pCell; pCell = pThis->CellIteratorNext())
		{
			const auto flags = pCell->Flags;
			if (!(flags & CellFlags::CenterRevealed) && (flags & CellFlags::EdgeRevealed))
				pCell->Flags = flags | CellFlags::IsPlot;
		}
	}

	return SpreadFogOfWar;
}

// Frozen animation segments bypass AnimClass::DrawIt. At 4D20FA ESI is
// still the validated AnimTypeClass (WhatAmI == 5 at 4D205C). X is the
// local at ESP+38, and Y is still in EDX, committed to ESP+3C at 4D2102.
// Do not use 4D21E4: both animation branches clear ESI before that shared
// DrawSHP site; the building overlay SHP also reaches it with another type.
DEFINE_HOOK(0x4D20FA, FoggedObjectClass_DrawFoggedObjects_AnimDrawOffset, 0x6)
{
	GET(AnimTypeClass* const, pType, ESI);
	R->ref_Stack<int>(0x38) += AnimTypeExt::ExtMap.Find(pType)->XDrawOffset;
	R->EDX(static_cast<int>(R->EDX()) + pType->YDrawOffset);
	return 0; // replay MOV CL,[ESI+35D] once; native code stores the adjusted Y
}

// 半透明迷雾中的建筑快照默认取所有者阵营色盘（0x4D1C9B-0x4D1CB0 算出后于本处
// 写入帧槽 [esp+0x14]，三处 CC_Draw_Shape 均从该槽取色盘）。但正常绘制经
// TechnoClass::GetDrawer(0x705D70) 会优先使用 TechnoTypeClass+0xDF0 缓存的自定义
// 色盘方案族（Art 图像节 Palette= 标签：LoadFromINI 0x71582B 将文件名读入 +0xDD0，
// 0x717820 经 ColorScheme::GeneratePalette 生成方案族缓存到 +0xDF0）。快照绘制
// 漏掉该分支，导致带自定义 Palette 的建筑快照错误显示为阵营默认色盘。
// 此处按 GetDrawer 的自定义分支补齐：FoggedObject+0x28 为快照所有者 House，
// 其 +0x16054 为 ColorScheme 索引；方案族 +4 为方案指针数组，方案 +0x30C 为
// 其 LightConvert。TerrainPalette 分支在钩点之前已处理，优先级与正常绘制一致。
DEFINE_HOOK(0x4D1EE5, FoggedObjectClass_DrawFoggedObjects_BuildingCustomPalette, 0x8)
{
	GET(BuildingTypeClass*, pType, ESI);
	GET(void*, pFogged, EBX);

	const auto pSchemes = *reinterpret_cast<DynamicVectorClass<ColorScheme*>**>(
		reinterpret_cast<char*>(pType) + 0xDF0);

	if (pSchemes)
	{
		const auto pOwner = *reinterpret_cast<HouseClass**>(reinterpret_cast<char*>(pFogged) + 0x28);
		const auto index = *reinterpret_cast<int*>(reinterpret_cast<char*>(pOwner) + 0x16054);
		const auto pScheme = pSchemes->GetItem(index);
		const auto pConvert = *reinterpret_cast<void**>(reinterpret_cast<char*>(pScheme) + 0x30C);

		if (pConvert)
		{
			R->ref_Stack<int>(0x14) = reinterpret_cast<int>(pConvert);
			return 0x4D1EED; // 跳过原版默认色盘写入，否则原指令会覆盖本钩子的写入
		}
	}

	return 0; // 重放原指令：无自定义色盘时维持原版阵营默认色盘
}

// 让单位在更新视野的时候不会多更新一次之前的视野
DEFINE_FUNCTION_JUMP(CALL6, 0x415672, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4157F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x416C6D, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4CD43F, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4DA6F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x51A8D7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x54C93C, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x73AC5F, FoggedObjectHelper::TechnoClass_RevealLastSight);

// 迷雾遮蔽时增加地形对象、覆盖物、弹坑
DEFINE_HOOK(0x486B21, CellClass_FogCell_FreezeObjects, 0x6)
{
	enum { SkipGameCode = 0x486BAB };

	GET(FoggedObjectHelper::CellClassFake*, pCell, EBP);
	GET(DynamicVectorClass<FoggedObjectClass*>*, pVector, EDI);

    pCell->Flags |= CellFlags::Fogged;
	for (auto pObject = pCell->FirstObject; pObject; pObject = pObject->NextObject)
	{
		const auto absType = pObject->WhatAmI();
		if (absType == AbstractType::Building)
		{
			const auto& pBuilding = static_cast<BuildingClass*>(pObject);
			if (!pBuilding->IsFogged && pBuilding->IsAllFogged())
			{
				pBuilding->FreezeInFog(pVector, pCell, (!pBuilding->IsStrange() && pBuilding->Translucency != 15));
			}
		}
		else if (absType == AbstractType::Unit
			|| absType == AbstractType::Aircraft
			|| absType == AbstractType::Infantry)
		{
			pObject->Deselect();
		}
		else if (absType == AbstractType::Terrain)
		{
			static_cast<FoggedObjectHelper::TerrainClassFake*>(pObject)->FreezeTerrain(pVector, true);
		}
	}

	if (pCell->OverlayTypeIndex != -1)
		pCell->FreezeOverlay(pVector, true);

	if (pCell->SmudgeTypeIndex != -1)
		pCell->FreezeSmudge(pVector, true);

	return SkipGameCode;
}

// 每帧快照逻辑（0x4D2370）会按“该格存在迷雾快照”重新给格子打上 CellFlags::Fogged，
// 即使该格早已重新完全可见（Visibility == -1）。标志残留后，已揭开格子里的单位会被
// 绘制循环当作雾中单位隐藏（表现为移动途中凭空消失），迷雾边缘的真实地形也会被跳过。
// 这里在两个打标点跳过完全可见的格子；快照本身保留，下次入雾时照常显示，避免动到
// 快照数据（清理快照会让建筑部件数据与全局绘制列表脱节而崩溃）。
DEFINE_HOOK_AGAIN(0x4D246E, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
DEFINE_HOOK(0x4D2430, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
{
	GET(uintptr_t, flagsAddr, EAX);

	auto const pCell = reinterpret_cast<CellClass*>(flagsAddr - 0x140);

	if (pCell->Foggedness == -1)
		return R->Origin() == 0x4D2430 ? 0x4D2436 : 0x4D2474;

	return 0;
}

// 地形对象仅在格子被完全遮挡（黑幕，Visibility == -2）时跳过真实绘制、由迷雾
// 快照接管。半透明迷雾边缘（Visibility 0..48）必须照常绘制真实地形，否则边缘
// 的真实地形与快照都不绘制，表现为迷雾边缘的地形对象整体消失。
DEFINE_HOOK(0x6D9313, TacticalClass_DrawObjectsInLayers_SkipTerrainInFog, 0x6)
{
	enum { SkipDraw = 0x6D940C };

	GET(const CoordStruct*, pCoord, EAX);

	if (ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		const int level = pCoord->Z / Unsorted::LevelHeight;
		const int extra = (level & 1) ? ((level >> 1) + 1) : (level >> 1);
		const CellStruct cell { static_cast<short>((pCoord->X >> 8) - extra), static_cast<short>((pCoord->Y >> 8) - extra) };

		if (MapClass::Instance.GetCellAt(cell)->Visibility == -2)
			return SkipDraw;
	}

	return 0;
}

// 覆盖物（围墙）仅在格子被完全遮挡时跳过真实绘制、由迷雾快照接管，理由同上。
// 注意原版此处是 cmp [ebx+44h],-1 / je 跳过：没有覆盖物的格子必须照旧跳过，
// 不能因为不满足迷雾条件就返回 Draw，否则会绕过原版的 -1 检查。
DEFINE_HOOK(0x6D70BC, TacticalClass_DrawCellOverlay_SkipOverlayInFog, 0xA)
{
	enum { Draw = 0x6D70C6, SkipDraw = 0x6D71A4 };

	GET(const CellClass*, pCell, EBX);

	if (pCell->OverlayTypeIndex == -1)
		return SkipDraw;

	if (ScenarioClass::Instance->SpecialFlags.FogOfWar && pCell->Visibility == -2)
		return SkipDraw;

	return Draw;
}

// 弹坑仅在格子被完全遮挡时跳过真实绘制、由迷雾快照接管，理由同上。
// 原版此处是 cmp [esi+48h],-1 / je 跳过：没有弹坑的格子必须照旧跳过，否则会拿
// -1 去索引 SmudgeTypeClass::Array（读到数组前面的垃圾指针）并在绘制时崩溃。
DEFINE_HOOK(0x48049E, CellClass_DrawCellSmudge_SkipSmudgeInFog, 0x6)
{
	enum { Draw = 0x4804A4, SkipDraw = 0x4804FB };

	GET(const CellClass*, pThis, ESI);

	if (pThis->SmudgeTypeIndex == -1)
		return SkipDraw;

	if (ScenarioClass::Instance->SpecialFlags.FogOfWar && pThis->Visibility == -2)
		return SkipDraw;

	return Draw;
}

// 被击败或是观察者时揭开所有迷雾
DEFINE_HOOK(0x4FC200, HouseClass_AcceptDefeat_RevealFog, 0x5)
{
	if (ScenarioClass::Instance->SpecialFlags.FogOfWar)
		FoggedObjectHelper::ClearAllFog();

	return 0;
}

// 修复间谍卫星与迷雾的交互：原版的整图揭幕只清除黑幕（清 ShroudCounter/Gaps、
// 置 Mapped|NoFog），完全不处理迷雾层——被揭示的格子迷雾标记仍在，却又没有迷雾
// 快照可画，表现为“看起来全开了”，但真实单位走入这些格子时会因迷雾中不绘制
// 真实物体而消失。按选定行为，揭示时连同迷雾一并彻底清除。
// 注意除了下述标记外，还必须重置格子的 Visibility/Foggedness（绘制与选中管线
// 依据这两个字段判断格子是否可见），未探索过的格子中它们是“完全遮挡”值。
// 钩点改在 0x577EE9（下一格迭代调用处，含被原版跳过的 3 个特殊格子）：
// 这些格子若保留 !CenterRevealed&&EdgeRevealed 状态，会成为迷雾扩散的种子，
// 从它们出发逐圈重新迷雾整张地图并经 FogCell 打回 Fogged 标记。此处对全部
// 格子（含特殊格子）补齐 CenterRevealed|EdgeRevealed 并清理迷雾状态，黑幕
// 的跳过逻辑（ShroudCounter/Gaps 不动）保持原版行为不变。
DEFINE_HOOK(0x577EE9, DisplayClass_RevealWholeMap_ClearFog, 0x5)
{
	GET(CellClass* const, pCell, EAX);

	if (Phobos::Config::TSFog && ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		pCell->Flags = (pCell->Flags & ~CellFlags::Fogged) | CellFlags::CenterRevealed | CellFlags::EdgeRevealed;
		pCell->Visibility = -1;
		pCell->Foggedness = -1;
		pCell->ClearFoggedObjects();
	}

	return 0;
}

// End of DisplayClass::RevealWholeMap (the push before the final call, after the
// cell loop). A map that reveals everything is meant to stay revealed, so drop the
// fog flag for the rest of the scenario and release the frozen images; otherwise
// the next FogSpread pass would re-fog the map and recreate snapshots we just freed.
DEFINE_HOOK(0x577F14, DisplayClass_RevealWholeMap_AfterReveal, 0x5)
{
	if (Phobos::Config::TSFog && Phobos::Config::TSFog_RevealAllClear
		&& ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		GameModeOptionsClass::Instance.FogOfWar = false;
		ScenarioClass::Instance->SpecialFlags.FogOfWar = false;
		FoggedVoxel::Clear();
	}

	return 0;
}

// 地形对象、覆盖物、弹坑出现时检查是否被迷雾遮蔽，虽然感觉没必要，但既然建筑也这么做了那就蛮做
DEFINE_FUNCTION_JUMP(CALL, 0x5FC4B1, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x5FD2D2, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x6B4B14, FoggedObjectHelper::SmudgeClassFake::SmudgeClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x71D012, FoggedObjectHelper::TerrainClassFake::TerrainClass_Unlimbo_CheckFog);


// FreezeInFog is also called by Building AI at 440C03 (Visible=false), not
// only CellClass::FogCell. Prepare at the common entry before native drawing
// state changes, and never sample a building that was not actually visible.
DEFINE_HOOK(0x457AA0, BuildingClass_FreezeInFog_PrepareVoxel, 0x6)
{
	GET(BuildingClass*, building, ECX);
	GET_STACK(bool, visible, 0xC);
	if (visible)
		FoggedVoxel::Prepare(building);
	return 0;
}

// Copy the native visible voxel render, including barrel/turret draw order.
// 70750E: ECX = selected converter, EDI = Techno, [ESP] = flags.
DEFINE_HOOK(0x70750E, TechnoClass_BlitVoxel_RememberFogPalette, 0x5)
{
	GET(TechnoClass*, pThis, EDI);
	GET(ConvertClass*, palette, ECX);
	GET_STACK(unsigned, flags, 0x0);
	FoggedVoxel::SelectPalette(pThis, palette, flags);
	return 0;
}

// All 13 native RLE_Blit arguments have already been pushed. In particular
// [ESP+14/18/1C/30] = Z adjustment/gradient/intensity/tint. Resume the call once.
DEFINE_HOOK(0x707736, TechnoClass_BlitVoxel_RememberFogImage, 0x5)
{
	GET(TechnoClass*, pThis, EDI);
	GET(const VoxelCacheStruct*, cache, ESI);
	GET(const RectangleStruct*, clip, EDX);
	LEA_STACK(const unsigned*, arguments, 0x0);
	FoggedVoxel::Capture(pThis, cache, clip, arguments);
	return 0;
}

// Cache misses and recoiling barrels render through a plain 8-bit surface.
// EBP = source Techno, EDX = converter, EBX = flags; 11 blit args on the stack.
DEFINE_HOOK(0x707235, TechnoClass_RenderVoxel_RememberFogImage, 0x5)
{
	GET(TechnoClass*, pThis, EBP);
	GET(ConvertClass*, palette, EDX);
	GET(unsigned, flags, EBX);
	LEA_STACK(const unsigned*, arguments, 0x0);
	FoggedVoxel::CapturePlain(pThis, palette, flags, arguments);
	return 0;
}

// Bind before the animation loop: EBX is still the original building, EDI
// is its snapshot and the main SHP bounds are initialized. The later animation
// union preserves our expanded bounds. Do not read ESP+54 at 4D1310: 4D119A
// reuses that argument slot for AnimTypeClass*, so animated buildings lose VXLs.
DEFINE_HOOK(0x4D10D3, FoggedObjectClass_CTOR_FreezeVoxel, 0x9)
{
	GET(FoggedObjectClass*, snapshot, EDI);
	GET(BuildingClass*, building, EBX);
	FoggedVoxel::Freeze(building, snapshot);
	return 0; // mov ecx,ebx; mov byte ptr [ebx+6E7h],1 (IsFogged)
}

// Only the building branch reaches this jump after its SHP records. Other
// skip/invalid-record paths must never interpret EBX as a valid snapshot.
DEFINE_HOOK(0x4D2203, FoggedObjectClass_DrawFrozenVoxel, 0x5)
{
	GET(FoggedObjectClass*, snapshot, EBX);
	GET(const RectangleStruct*, bounds, EDI);
	FoggedVoxel::Draw(snapshot, bounds);
	return 0;
}

// FoggedObjectClass 析构（0x4D1650）里摘除全局绘制记录的二分查找可能摘错对象
// （见 FoggedObjectHelper::RemoveDrawRecordsOf 的说明）。这里在向量被释放之前，
// 按对象指针把绘制数组里指向自己的记录全部清掉。
DEFINE_HOOK(0x4D1855, FoggedObjectClass_Destructor_CleanDrawRecords, 0x7)
{
	GET(FoggedObjectClass*, pThis, EDI);

	FoggedVoxel::Remove(pThis);
	FoggedObjectHelper::RemoveDrawRecordsOf(pThis);

	return 0; // 重新执行被覆盖的 mov [edi+5Ch], 7E8BC0h
}

// 绘制迷雾快照（0x4D1890）逐条遍历全局绘制数组（0x8B3CC0 / 数量 0x8B3CC4），
// 取出的记录里只有 {类别排序键, FoggedObjectClass*}。记录是在冻结对象析构时
// （0x4D1650，二分查找后移位摘除）才被移除的，一旦对象先被销毁而记录残留，
// 后续每帧都会拿已释放的对象去绘制：
//   - 建筑分支 0x4D1C6B 直接以 [对象+0x60]（内部 DrawRecord 向量）为基址读
//     [基址 + i*0x10]，向量已释放时即访问违例（EAX=数量、ECX=NULL、EBP=0）；
//   - 地形分支 0x4D1AD9 同样按 [对象+0x60] 取值，空指针直接崩。
// 这里在取出记录后校验对象身份（主虚表必须是 FoggedObjectClass）与向量一致性，
// 不一致就跳过该记录，交给下一帧；正常对象的检查恒通过，不改变绘制结果。
DEFINE_HOOK(0x4D18F3, FoggedObjectClass_DrawFoggedObjects_SkipInvalidRecord, 0x7)
{
	enum { SkipThisObject = 0x4D2311 };

	GET(FoggedObjectHelper::FoggedDrawRecord*, pRecords, ECX);
	GET(int, index, ESI);

	auto const pObject = pRecords[index].Object;

	if (!pObject || reinterpret_cast<uintptr_t*>(pObject)[0] != FoggedObjectHelper::FoggedObjectClassVTable)
		return SkipThisObject;

	auto const pVector = reinterpret_cast<FoggedObjectHelper::FoggedObjectDrawRecords*>(
		reinterpret_cast<char*>(pObject) + 0x5C);

	if (!pVector->Items || pVector->Count > pVector->Capacity)
		return SkipThisObject;

	return 0;
}

DEFINE_HOOK(0x4D1B2E, FoggedObjectClass_DrawFoggedObjects_DrawTerrain, 0x6)
{
	enum { SkipGameCode = 0x4D2311 };

	GET(SHPStruct*, pImage, EAX);
	GET(const CoordStruct*, pCoord, ESI);
	GET(const RectangleStruct*, pBounds, EDI);
	GET(CellClass*, pCell, EBP);
	// 不能在这里读 ECX 当 Type：0x4D1B28 的 call [edx+9Ch]（Type 的虚函数，返回 SHP）已经执行过，
	// ECX 是易失寄存器、可能已被它破坏，之前那样取到的 Type 可能是垃圾；TerrainTypeExt::Fetch
	// 会顺着垃圾指针得到无效色盘并传给 DrawSHP，底层 blitter 会做出野跳转（EIP 落到非代码地址）
	// 而崩溃。改为从栈上取游戏在 call 之前保存的 Type 指针（即当时的 ECX，[esp+18h]）。
	GET_STACK(TerrainTypeClass*, pType, STACK_OFFSET(0x18, 0x0));
	GET_STACK(int, frame, STACK_OFFSET(0x154, -0x134));

	// 参数校验：任一指针不可信、或该对象并非地形时退回原版绘制。返回 0 会重新执行被覆盖的原
	// 指令（mov ebx,eax / mov eax,esi / mov ecx,[eax]），它们依赖进入钩子时的 EAX=SHP 与
	// ESI=坐标指针。Syringe 会保存并恢复整组寄存器，这里再把 EAX 显式写回 SHP 作为双保险。
	if (!pImage || !pCoord || !pBounds || !pCell || !pType || pType->WhatAmI() != AbstractType::TerrainType)
	{
		R->EAX(pImage);
		return 0;
	}

	if (pImage)
	{
		// 帧号来自被冻结对象内部字段，动画地形换帧后可能已经越界。越界会让位块
		// 传送读到 SHP 帧表之外（yr-simd 等加速器上表现为访问违例），因此按实际
		// 帧数取模，正常情况（帧号合法）下结果与原值相同。
		const int frameCount = pImage->Frames;

		if (frameCount <= 0)
		{
			R->EAX(pImage);
			return 0;
		}

		int drawFrame = frame % frameCount;

		if (drawFrame < 0)
			drawFrame += frameCount;

		const int shadowFrame = (drawFrame + frameCount / 2) % frameCount;

		auto point = TacticalClass::Instance->CoordsToClient(*pCoord).first;
		point.X += (DSurface::ViewBounds.X - pBounds->X);
		point.Y += (DSurface::ViewBounds.Y - pBounds->Y);
		int zAdjust = -TacticalClass::Instance->AdjustForZ(pCoord->Z);

		if (!pCell->LightConvert)
			pCell->InitLightConvert();

		ConvertClass* pPalette = pCell->LightConvert;
		int intensity = static_cast<int>(pCell->Intensity_Terrain);
		auto const pTypeExt = TerrainTypeExt::ExtMap.Find(pType);

		if (const auto pPalettes = pTypeExt->Palette)
		{
			// This fork has no PaletteRemap/PlainPalette split: always use the
			// color-scheme remap, same as the native terrain drawer.
			const int wallOwnerIndex = pCell->WallOwnerIndex;
			int colorSchemeIndex = HouseClass::CurrentPlayer->ColorSchemeIndex;

			if (wallOwnerIndex >= 0)
				colorSchemeIndex = HouseClass::Array[wallOwnerIndex]->ColorSchemeIndex;

			pPalette = pPalettes->Items[colorSchemeIndex]->LightConvert;

			intensity = static_cast<int>(pCell->Intensity_Normal);

			if (pType->SpawnsTiberium)
				point.Y -= 16;
		}
		else if (pType->SpawnsTiberium)
		{
			pPalette = FileSystem::GRFTXT_TIBERIUM_PAL;
			intensity = static_cast<int>(pCell->Intensity_Normal);
			point.Y -= 16;
		}

		DSurface::Temp->DrawSHP(pPalette, pImage, drawFrame, &point, pBounds, BlitterFlags(0x4E00), 0, zAdjust - 4, ZGradient::Deg90, intensity, 0, 0, 0, 0, 0);
		if (Game::bDrawShadow)
			DSurface::Temp->DrawSHP(pPalette, pImage, shadowFrame, &point, pBounds, BlitterFlags(0x4601), 0, zAdjust - 2, ZGradient::Ground, 1000, 0, 0, 0, 0, 0);
	}

	return SkipGameCode;
}

#pragma region FoggedEffects

namespace FoggedEffectHelper
{
	// 特效可见性判定：两端点都在迷雾中时才视为不可见，任一端可见即绘制
	__forceinline bool IsCoordFogged(const CoordStruct& coords)
	{
		// gamemd's MapClass::IsLocationFogged (0x5865E0) is a stub that always
		// returns false in this build, so evaluate the cell flags directly.
		const int level = coords.Z / Unsorted::LevelHeight;
		const int extra = (level & 1) ? ((level >> 1) + 1) : (level >> 1);
		const CellStruct cell { static_cast<short>((coords.X >> 8) - extra),
			static_cast<short>((coords.Y >> 8) - extra) };
		const auto pCell = MapClass::Instance.GetCellAt(cell);

		if (!(pCell->Flags & CellFlags::Fogged) || pCell->Foggedness == -1)
			return false;

		// Keep objects that overlap a fully visible neighbour drawn, otherwise
		// they pop out at the fog edge.
		for (auto const& offset : Unsorted::AdjacentCell)
		{
			if (MapClass::Instance.GetCellAt(cell + offset)->Foggedness == -1)
				return false;
		}

		return true;
	}

	__forceinline bool IsBeamFogged(const CoordStruct& source, const CoordStruct& target)
	{
		return ScenarioClass::Instance->SpecialFlags.FogOfWar
			&& IsCoordFogged(source) && IsCoordFogged(target);
	}
}

// 激光（含飞碟激光分段、激光型尾迹）两端都在迷雾中时跳过绘制
DEFINE_HOOK(0x550274, LaserDrawClass_Draw_SkipInFog, 0x5)
{
	GET(LaserDrawClass* const, pThis, EBX);

	if (FoggedEffectHelper::IsBeamFogged(pThis->Source, pThis->Target))
		return 0x5509CA;

	return 0;
}

// 电弧（含电弧型尾迹）两端都在迷雾中时跳过绘制，寿命照常折半递减
DEFINE_HOOK(0x4C27D5, EBolt_DrawAll_SkipInFog_1, 0x7)
{
	GET(EBolt* const, pThis, ESI);
	GET(int, visible, EAX);

	if ((visible & 0xFF) && FoggedEffectHelper::IsBeamFogged(pThis->Point1, pThis->Point2))
		return 0x4C281A;

	return 0;
}

DEFINE_HOOK(0x4C28F9, EBolt_DrawAll_SkipInFog_2, 0x7)
{
	GET(EBolt* const, pThis, ESI);
	GET(int, visible, EAX);

	if ((visible & 0xFF) && FoggedEffectHelper::IsBeamFogged(pThis->Point1, pThis->Point2))
		return 0x4C293D;

	return 0;
}

// 辐射波（含辐射型尾迹）两端都在迷雾中时跳过绘制，存活计时照常递增
DEFINE_HOOK(0x65978C, RadBeam_AI_SkipInFog_1, 0x5)
{
	GET(RadBeam* const, pThis, ESI);

	if (*(bool*)((char*)pThis + 9) && FoggedEffectHelper::IsBeamFogged(pThis->SourceLocation, pThis->TargetLocation))
		return 0x659968;

	return 0;
}

DEFINE_HOOK(0x659CB7, RadBeam_AI_SkipInFog_2, 0x5)
{
	GET(RadBeam* const, pThis, EDI);

	if (*(bool*)((char*)pThis + 9) && FoggedEffectHelper::IsBeamFogged(pThis->SourceLocation, pThis->TargetLocation))
		return 0x659E09;

	return 0;
}

// 声波/磁波两端都在迷雾中时跳过绘制，波动推进与伤害逻辑照常执行
DEFINE_HOOK(0x762C4E, WaveClass_UpdateWave_SkipInFog, 0x6)
{
	struct WaveEndpointsFake
	{
		char _[0xAC];
		AbstractClass* SourceObj;  // +0xAC
		char __[0x12C - 0xB0];
		bool DrawFlag;             // +0x12C
		char ___[0x1D4 - 0x12D];
		AbstractClass* TargetObj;  // +0x1D4
	};

	GET(WaveClass* const, pThis, ESI);
	const auto pFake = reinterpret_cast<WaveEndpointsFake*>(pThis);

	if (pFake->SourceObj && pFake->TargetObj)
	{
		CoordStruct source;
		CoordStruct target;
		pFake->SourceObj->GetCoords(&source);
		pFake->TargetObj->GetCoords(&target);

		if (FoggedEffectHelper::IsBeamFogged(source, target))
			return 0x762D57;
	}

	return 0;
}

#pragma endregion

// 修复迷雾中建筑绘制崩溃：原分支在绘制建筑遮挡时会使用 BUILDINGZ.SHA 的缓存指针
// （FileSystem::BUILDINGZ_SHA），但该缓冲在进入游戏时已被释放，读取已释放内存导致崩溃。
// 原版迷雾逻辑从不执行到此处，故原版不崩溃。将遮挡图参数置空以跳过遮挡绘制，
// 与同分支的无遮挡子路径传 0 的行为一致，迷雾中的建筑不需要遮挡效果。
DEFINE_HOOK(0x4D1E46, FoggedObjectClass_DrawFoggedObjects_NullBuildingZSHA, 0x7)
{
	R->EAX(0); // 原本存入 BUILDINGZ_SHA，改为空指针
	return 0;
}

// Fogged building AlphaImage uses the existing native AlphaShape pipeline.
// Hooks.AlphaImage.cpp preserves the last visible native source while all
// foundation cells are fogged; do not duplicate its light as a frozen SHP.

// 迷雾中的建筑被摧毁后，移除其冻结在迷雾中的贴图，防止残留
DEFINE_HOOK(0x441F60, BuildingClass_AfterDestruction_RemoveFoggedImage, 0x5)
{
	GET(BuildingClass* const, pThis, ECX);

	if (ScenarioClass::Instance->SpecialFlags.FogOfWar)
	{
		const auto& position = pThis->Location;
		const auto pFoundation = pThis->GetFoundationData(false);
		const CellStruct foundationEnd { 0x7FFF, 0x7FFF };

		// 建筑的迷雾冻结对象只有一个，被注册在其覆盖的每个格子中；
		// 以两种可能的基准格并集扫描，找到后由 RemoveFoggedObject 统一清理全部引用
		const CellStruct bases[2]
		{
			pThis->GetMapCoords(),
			FoggedObjectHelper::CoordToCell(position)
		};

		for (const auto& base : bases)
		{
			for (auto pOffset = pFoundation; *pOffset != foundationEnd; ++pOffset)
			{
				const auto pCell = MapClass::Instance.TryGetCellAt(base + *pOffset);

				if (!pCell || !pCell->FoggedObjects)
					continue;

				const auto& pVector = pCell->FoggedObjects;

				for (int i = pVector->Count - 1; i >= 0; --i)
				{
					const auto pItem = pVector->Items[i];
					const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);

					// +0x28 存的是所有者 House 而非建筑实例（此前拿它比对建筑指针，
					// 条件永远不成立、清理从未生效）；按类别 + 冻结位置匹配即可唯一
					// 确定该建筑的冻结贴图
					if (pFields->FoggedType == 6 && pFields->Position == position)
					{
						RemoveFoggedObject(pCell, pItem);
						return 0;
					}
				}
			}
		}
	}

	return 0;
}

// 迷雾中矿石被采集后，同步冻结的覆盖物贴图：
// 数量变化就地更新，类型变化或耗尽则移除后重建（0x480C67 为两条扣减路径的汇合点）
DEFINE_HOOK(0x480C67, CellClass_ReduceTiberium_SyncFoggedOverlay, 0x6)
{
	GET(CellClass* const, pRawCell, ESI);

	// 耗尽路径中 ESI 已被调整为 cell+0x24（&MapCoords），此处还原
	auto pCell = pRawCell;
	if (MapClass::Instance.TryGetCellAt(pRawCell->MapCoords) != pRawCell)
		pCell = reinterpret_cast<CellClass*>(reinterpret_cast<char*>(pRawCell) - 0x24);

	if (!ScenarioClass::Instance->SpecialFlags.FogOfWar
		|| !pCell->FoggedObjects
		|| !(pCell->Flags & CellFlags::Fogged))
	{
		return 0;
	}

	const auto& pVector = pCell->FoggedObjects;

	for (int i = pVector->Count - 1; i >= 0; --i)
	{
		const auto pItem = pVector->Items[i];
		const auto pFields = reinterpret_cast<FoggedObjectFields*>(pItem);

		if (pFields->FoggedType != 0x14) // 只处理覆盖物冻结对象
			continue;

		if (pFields->OverlayTypeIndex != pCell->OverlayTypeIndex)
		{
			RemoveFoggedObject(pCell, pItem);

			// 覆盖物仍存在时重新冻结
			if (pCell->OverlayTypeIndex != -1)
			{
				if (!pCell->FoggedObjects)
					pCell->FoggedObjects = FoggedObjectHelper::CreateGameDynamicVector();

				static_cast<FoggedObjectHelper::CellClassFake*>(pCell)->FreezeOverlay(pCell->FoggedObjects, true);
			}
		}
		else
		{
			// 数量变化：就地更新（绘制侧直接读取这两个字段）
			pFields->OverlayData = static_cast<int>(pCell->OverlayData);
		}
	}

	return 0;
}

#pragma endregion

#pragma endregion
