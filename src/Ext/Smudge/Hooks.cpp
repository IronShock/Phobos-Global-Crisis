#include <Helpers/Macro.h>
#include <CellClass.h>
#include <SmudgeTypeClass.h>
#include <OverlayTypeClass.h>
#include <CCINIClass.h>
#include <MapClass.h>
#include <Phobos.h>
#include <BasicStructures.h>

#include <Ext/Cell/Body.h>
#include <Ext/SmudgeType/Body.h>
#include <Ext/OverlayType/Body.h>
#include <Misc/INIDiagnostics.h>

#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <cstdio>

// SmudgeType "Unclearable" tag (default yes).
//
// Vanilla YR smudges are stored on the cell as a type index
// (CellClass::SmudgeTypeIndex at +0x48) and are cleared by writing -1 to that
// field (building placement clears the foundation cells, other map-wide clear
// paths, etc.). Every `mov dword ptr [cell+0x48], -1` site is guarded below so an
// Unclearable smudge is never removed.

static std::set<const SmudgeTypeClass*> s_unclearableSmudge;
static bool s_unclearableInitialized = false;

static void EnsureUnclearableInitialized()
{
	if (s_unclearableInitialized)
		return;

	s_unclearableInitialized = true;

	// Rules are fully loaded by the time a smudge is cleared in game, so reading
	// [Type]Unclearable= (default yes) from the rules INI is reliable here.
	for (int i = 0; i < SmudgeTypeClass::Array.Count; ++i)
	{
		auto const pType = SmudgeTypeClass::Array.GetItem(i);

		if (CCINIClass::INI_Rules->ReadBool(pType->ID, "Unclearable", true))
			s_unclearableSmudge.insert(pType);
	}
}

static int GetCellSmudgeTypeIndex(CellClass* pCell)
{
	// CellClass::SmudgeTypeIndex is at +0x48 in the game binary.
	return *reinterpret_cast<int*>(reinterpret_cast<char*>(pCell) + 0x48);
}

static bool IsCellSmudgeUnclearable(CellClass* pCell)
{
	if (!pCell)
		return false;

	EnsureUnclearableInitialized();

	const int index = GetCellSmudgeTypeIndex(pCell);

	if (index < 0 || index >= SmudgeTypeClass::Array.Count)
		return false;

	return s_unclearableSmudge.count(SmudgeTypeClass::Array.GetItem(index)) > 0;
}

// Guard the four `mov dword ptr [cell+0x48], -1` (clear smudge) sites.

DEFINE_HOOK(0x43F3A1, BuildingClass_PlaceFoundation_ClearSmudge_Unclearable, 0x7)
{
	enum { Skip = 0x43F3A8 };

	GET(CellClass* const, pCell, EAX);

	if (IsCellSmudgeUnclearable(pCell))
		return Skip;

	// The native smudge is being cleared - drop any stacked extras too.
	if (auto const pCellExt = CellExt::ExtMap.Find(pCell))
		pCellExt->ExtraSmudges.clear();

	return 0;
}

DEFINE_HOOK(0x43F8A6, BuildingClass_PlaceFoundation2_ClearSmudge_Unclearable, 0x7)
{
	enum { Skip = 0x43F8AD };

	GET(CellClass* const, pCell, EAX);

	if (IsCellSmudgeUnclearable(pCell))
		return Skip;

	if (auto const pCellExt = CellExt::ExtMap.Find(pCell))
		pCellExt->ExtraSmudges.clear();

	return 0;
}

DEFINE_HOOK(0x588BA9, MapClass_ClearSmudges_Unclearable, 0x7)
{
	enum { Skip = 0x588BB0 };

	GET(CellClass* const, pCell, EAX);

	if (IsCellSmudgeUnclearable(pCell))
		return Skip;

	if (auto const pCellExt = CellExt::ExtMap.Find(pCell))
		pCellExt->ExtraSmudges.clear();

	return 0;
}

DEFINE_HOOK(0x6B32FD, SmudgeTypeClass_ClearSmudges_Unclearable, 0x7)
{
	enum { Skip = 0x6B3304 };

	GET(CellClass* const, pCell, EAX);

	if (IsCellSmudgeUnclearable(pCell))
		return Skip;

	if (auto const pCellExt = CellExt::ExtMap.Find(pCell))
		pCellExt->ExtraSmudges.clear();

	return 0;
}

// Map loading: ScenarioClass::Read_Smudge (0x6B4C80) applies every [Smudge] entry
// by overwriting the cell's native slot (CellClass::SmudgeTypeIndex), so only the
// LAST entry for a cell survives. A map editor that stacks smudges on one cell
// therefore loses all but one of them. Re-read the section after the vanilla load
// and stack every non-last entry of a cell into CellExt::ExtraSmudges (up to the
// [General] SmudgeCapacity limit) so they all render.
DEFINE_HOOK(0x6B4DB7, ScenarioClass_ReadSmudge_ExtraSmudges, 0xA)
{
	GET(CCINIClass*, pINI, EBP);

	// [GC-diag] Bisection checkpoint: validate the map INI right after the
	// vanilla [Smudge] read. Runs only during map load (never at startup).
	Diagnostics::ValidateINI("after Read_Smudge", pINI);

	const int capacity = Phobos::Config::SmudgeCapacity;
	const int extraCapacity = capacity - 1 > 0 ? capacity - 1 : 0;
	if (extraCapacity <= 0)
		return 0;

	const int count = pINI->GetKeyCount("Smudge");
	if (count <= 1)
		return 0;

	struct Entry
	{
		CellStruct Cell;
		SmudgeTypeClass* Type;
		int KeyIndex;
	};

	std::vector<Entry> entries;
	std::unordered_map<int, int> lastIndex;

	char buffer[0x80];
	char typeName[0x80];
	for (int i = 0; i < count; ++i)
	{
		pINI->ReadString("Smudge", pINI->GetKeyName("Smudge", i), "", buffer, sizeof(buffer));
		const char* pValue = buffer;

		int x, y, data;
		if (sscanf_s(pValue, "%[^,],%d,%d,%d", typeName, sizeof(typeName), &x, &y, &data) != 4)
			continue;

		auto const pType = SmudgeTypeClass::Find(typeName);
		if (!pType)
			continue;

		const CellStruct cell { static_cast<short>(x), static_cast<short>(y) };
		const int cellKey = (x << 16) | (y & 0xFFFF);
		entries.push_back({ cell, pType, i });
		lastIndex[cellKey] = i;
	}

	for (auto const& entry : entries)
	{
		const int cellKey = (entry.Cell.X << 16) | (entry.Cell.Y & 0xFFFF);
		if (lastIndex[cellKey] == entry.KeyIndex)
			continue; // the last entry for this cell is its native smudge

		auto const pCell = MapClass::Instance.TryGetCellAt(entry.Cell);
		if (!pCell || pCell->SmudgeTypeIndex == -1)
			continue;

		auto const pCellExt = CellExt::ExtMap.Find(pCell);
		if (pCellExt && static_cast<int>(pCellExt->ExtraSmudges.size()) < extraCapacity)
			pCellExt->ExtraSmudges.push_back(entry.Type);
	}

	return 0;
}

// Map saving: ScenarioClass::Write_Smudge (0x6B4DD0) writes each cell's native
// smudge. Hook right after a native entry is written (0x6B4E76) and also write the
// stacked ExtraSmudges so a map saved from the game keeps every per-cell smudge.
DEFINE_HOOK(0x6B4E76, ScenarioClass_WriteSmudge_ExtraSmudges, 0x5)
{
	GET(CellClass*, pCell, ESI);
	GET_STACK(int, keyIndex, 0x10);
	// `this` (the CCINIClass) is at [esp+0x18] at this hook point (the engine
	// loads it as `mov ecx,[esp+0x1c]` one push earlier, before WriteString's
	// ret 0xC). Using 0x20 read the key buffer instead and passed a garbage INI
	// to WriteString, corrupting the heap (map INI entry UAF on the next load).
	GET_STACK(CCINIClass*, pINI, 0x18);

	auto const pCellExt = CellExt::ExtMap.Find(pCell);
	if (pCellExt && !pCellExt->ExtraSmudges.empty())
	{
		char key[0x10];
		char value[0x40];
		for (auto const pType : pCellExt->ExtraSmudges)
		{
			_snprintf_s(key, sizeof(key), "%d", keyIndex);
			_snprintf_s(value, sizeof(value), "%s,%d,%d,0",
				pType->ID, pCell->MapCoords.X, pCell->MapCoords.Y);
			pINI->WriteString("Smudge", key, value);
			++keyIndex;
		}
	}

	// The vanilla `inc dword ptr [esp+0x10]` (stolen with size 0x5) is replayed by
	// the trampoline, so write back the counter already advanced past any extras we
	// wrote; the replayed `inc` then bumps it once more for the next native smudge.
	R->Stack(0x10, keyIndex);

	return 0;
}

// SmudgeType "DrawObject" tag.
//
// Vanilla smudges are drawn in the cell ground pass (SmudgeTypeClass::DrawIt via
// vtable+0xA0 at 0x4804F5) which runs before the overlay pass, so overlays (e.g.
// tiberium) are drawn on top of them. When a smudge type has DrawObject=yes its
// exact call arguments are also queued; the queue is flushed right after the
// overlay pass and the last ground pass but still before the object layer, so the
// smudge is drawn a second time above overlays while remaining below units and
// buildings. The original ground-pass draw is kept so no register/stack surgery is
// needed. This applies to both the native cell smudge and the stacked ExtraSmudges
// (AnimType CreateSmudge feature).
namespace SmudgeDrawObject
{
	struct DeferredDraw
	{
		SmudgeTypeClass* Type;
		Point2D Point;
		RectangleStruct Rect;
		int SmudgeData;
		int Height;
		CellStruct MapCoords;
	};

	// Hard cap so a queued entry can never leak across frames if the flush point
	// is ever missed (the visible cell count is far below this).
	static constexpr size_t MaxDeferred = 0x8000;

	static std::vector<DeferredDraw> Deferred;

	static bool IsDrawObject(SmudgeTypeClass* pType)
	{
		if (!pType)
			return false;

		auto const pExt = SmudgeTypeExt::ExtMap.Find(pType);
		return pExt && pExt->DrawObject;
	}

	// True when the cell's overlay is flagged IgnoreObject=yes, in which case a
	// DrawObject smudge must not be drawn above that overlay.
	static bool CellOverlayIgnoresObject(CellClass* pCell)
	{
		if (!pCell)
			return false;

		const int index = pCell->OverlayTypeIndex;

		if (index < 0 || index >= OverlayTypeClass::Array.Count)
			return false;

		auto const pExt = OverlayTypeExt::ExtMap.Find(OverlayTypeClass::Array.GetItem(index));
		return pExt && pExt->IgnoreObject;
	}

	static void Queue(SmudgeTypeClass* pType, const Point2D& point, const RectangleStruct& rect,
		int smudgeData, int height, const CellStruct& mapCoords)
	{
		if (Deferred.size() >= MaxDeferred)
			Deferred.clear();

		Deferred.push_back({ pType, point, rect, smudgeData, height, mapCoords });
	}

	static void Flush()
	{
		for (auto const& entry : Deferred)
			entry.Type->DrawIt(entry.Point, entry.Rect, entry.SmudgeData, entry.Height, entry.MapCoords);

		Deferred.clear();
	}
}

// Native smudge draw call site. The five arguments are already pushed on the stack
// (Point2D*, RectangleStruct*, SmudgeData, Height, CellStruct*) and ECX holds the
// smudge type. For DrawObject=yes types the exact same arguments are queued so the
// smudge can be drawn again above overlays later; the original call still runs so
// the ground pass output stays intact and no register/stack surgery is needed.
DEFINE_HOOK(0x4804F5, SmudgeTypeClass_DrawIt_DrawObject, 0x6)
{
	GET(SmudgeTypeClass*, pType, ECX);
	GET(CellClass*, pCell, ESI);

	if (SmudgeDrawObject::IsDrawObject(pType) && !SmudgeDrawObject::CellOverlayIgnoresObject(pCell))
	{
		auto const pPoint = R->Stack<Point2D*>(0x0);
		auto const pRect = R->Stack<RectangleStruct*>(0x4);
		const int smudgeData = R->Stack<int>(0x8);
		const int height = R->Stack<int>(0xC);
		auto const pMapCoords = R->Stack<CellStruct*>(0x10);

		SmudgeDrawObject::Queue(pType, *pPoint, *pRect, smudgeData, height, *pMapCoords);
	}

	// Execute the original call.
	return 0;
}

// Render the extra smudges stacked on a cell alongside its native smudge. The
// vanilla native smudge is drawn at 0x48049E-0x4804F5 (SmudgeTypeClass::DrawIt,
// vtable+0xA0) in a separate cell-ground pass; hook right after that call
// (0x4804FB) and draw each ExtraSmudges entry with the exact same args the native
// just used (point at [esp+0x14], clip rect in ebp, smudge data at cell+0x11F,
// height = (signed char)cell+0x11B * [0x89E7C0], mapcoords at cell+0x24) so they
// are drawn after / on top of the native smudge instead of being covered by it.
// DrawObject=yes entries are also queued so they follow the native smudge.
DEFINE_HOOK(0x4804FB, CellClass_DrawSmudge_ExtraSmudges, 0x7)
{
	GET(CellClass*, pCell, ESI);
	GET(RectangleStruct*, pRect, EBP);
	Point2D* const pPoint = reinterpret_cast<Point2D*>(R->ESP() + 0x14);

	auto const pCellExt = CellExt::ExtMap.Find(pCell);
	if (pCellExt && !pCellExt->ExtraSmudges.empty())
	{
		const char* const pCellBytes = reinterpret_cast<const char*>(pCell);
		const int height = static_cast<int>(static_cast<signed char>(pCellBytes[0x11B]))
			* *reinterpret_cast<int*>(0x89E7C0);
		const int smudgeData = static_cast<unsigned char>(pCellBytes[0x11F]);
		const CellStruct& mapCoords = *reinterpret_cast<const CellStruct*>(pCellBytes + 0x24);

		for (auto const pType : pCellExt->ExtraSmudges)
		{
			pType->DrawIt(*pPoint, *pRect, smudgeData, height, mapCoords);

			if (SmudgeDrawObject::IsDrawObject(pType) && !SmudgeDrawObject::CellOverlayIgnoresObject(pCell))
				SmudgeDrawObject::Queue(pType, *pPoint, *pRect, smudgeData, height, mapCoords);
		}
	}

	return 0;
}

// Flush deferred DrawObject smudges. This function (0x6D3870) is called right after
// the overlay pass (0x6D3290) and the last ground pass (0x6D3040) in the terrain
// render path, but before the object layer (Tactical_RenderLayers, mode 2), so the
// deferred smudges end up above overlays and below units/buildings.
DEFINE_HOOK(0x6D3870, TacticalClass_Render_FlushDrawObjectSmudges, 0x5)
{
	SmudgeDrawObject::Flush();

	return 0;
}
