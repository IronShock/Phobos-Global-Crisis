#include "Fog.h"

// ============================================================================
// Frozen fog snapshots: buildings and static objects keep their last visible
// state while inside fog; units are hidden by IsLocationFogged.
// ============================================================================

namespace FoggedObjectHelper
{
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
		Fog::NoReveal = true;
		MapClass::Instance.RevealArea2(&pThis->LastSightCoords, lastSightRange, pOwner, false, false, false, true, true);
		Fog::NoReveal = false;
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
				const auto pCell = MapClass::Instance.TryGetCellAt(coords);
				if (pCell && this->IsAlive && (pCell->Flags & CellFlags::Fogged))
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
				const auto pCell = MapClass::Instance.TryGetCellAt(coords);
				if (pCell && pCell->OverlayTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
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
				const auto pCell = MapClass::Instance.TryGetCellAt(coords);
				if (pCell && pCell->SmudgeTypeIndex != -1 && (pCell->Flags & CellFlags::Fogged))
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

DEFINE_HOOK(0x4D20FA, FoggedObjectClass_DrawFoggedObjects_AnimDrawOffset, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(AnimTypeClass* const, pType, ESI);
	R->ref_Stack<int>(0x38) += AnimTypeExt::ExtMap.Find(pType)->XDrawOffset;
	R->EDX(static_cast<int>(R->EDX()) + pType->YDrawOffset);
	return 0; // replay MOV CL,[ESI+35D] once; native code stores the adjusted Y
}

DEFINE_HOOK(0x4D1EE5, FoggedObjectClass_DrawFoggedObjects_BuildingCustomPalette, 0x8)
{
	if (!Fog::Active())
		return 0;

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

DEFINE_HOOK(0x486B21, CellClass_FogCell_FreezeObjects, 0x6)
{
	if (!Fog::Active())
		return 0;

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

DEFINE_HOOK(0x4D1855, FoggedObjectClass_Destructor_CleanDrawRecords, 0x7)
{
	if (!Fog::Active())
		return 0;

	GET(FoggedObjectClass*, pThis, EDI);

	FoggedObjectHelper::RemoveDrawRecordsOf(pThis);

	return 0; // 重新执行被覆盖的 mov [edi+5Ch], 7E8BC0h
}

DEFINE_HOOK(0x4D18F3, FoggedObjectClass_DrawFoggedObjects_SkipInvalidRecord, 0x7)
{
	if (!Fog::Active())
		return 0;

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
	if (!Fog::Active())
		return 0;

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
			// colour-scheme remap, like the native terrain drawer.
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

DEFINE_HOOK(0x4D1E46, FoggedObjectClass_DrawFoggedObjects_NullBuildingZSHA, 0x7)
{
	if (!Fog::Active())
		return 0;

	R->EAX(0); // 原本存入 BUILDINGZ_SHA，改为空指针
	return 0;
}

DEFINE_HOOK(0x441F60, BuildingClass_AfterDestruction_RemoveFoggedImage, 0x5)
{
	if (!Fog::Active())
		return 0;

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

DEFINE_HOOK(0x480C67, CellClass_ReduceTiberium_SyncFoggedOverlay, 0x6)
{
	if (!Fog::Active())
		return 0;

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

DEFINE_HOOK_AGAIN(0x4D246E, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
DEFINE_HOOK(0x4D2430, FoggedObjectClass_MarkFogged_SkipWhenVisible, 0x6)
{
	if (!Fog::Active())
		return 0;

	GET(uintptr_t, flagsAddr, EAX);

	auto const pCell = reinterpret_cast<CellClass*>(flagsAddr - 0x140);

	if (pCell->Foggedness == -1)
		return R->Origin() == 0x4D2430 ? 0x4D2436 : 0x4D2474;

	return 0;
}

DEFINE_FUNCTION_JUMP(CALL6, 0x415672, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4157F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x416C6D, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4CD43F, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x4DA6F7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x51A8D7, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x54C93C, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL6, 0x73AC5F, FoggedObjectHelper::TechnoClass_RevealLastSight);
DEFINE_FUNCTION_JUMP(CALL, 0x5FC4B1, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x5FD2D2, FoggedObjectHelper::OverlayClassFake::OverlayClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x6B4B14, FoggedObjectHelper::SmudgeClassFake::SmudgeClass_Unlimbo_CheckFog);
DEFINE_FUNCTION_JUMP(CALL, 0x71D012, FoggedObjectHelper::TerrainClassFake::TerrainClass_Unlimbo_CheckFog);
