#include "Body.h"

#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <CellClass.h>
#include <OverlayTypeClass.h>
#include <SmudgeTypeClass.h>
#include <BasicStructures.h>

namespace OverlayPaletteTemp
{
	// The overlay type currently being drawn by CellClass::DrawOverlay. Set at
	// the start of the draw (ZAdjust hook) and consumed right before the shared
	// DrawSHP call so the CustomPalette override can be applied.
	OverlayTypeClass* Current;
}

DEFINE_HOOK(0x47F71D, CellClass_DrawOverlay_ZAdjust, 0x5)
{
	GET(const int, zAdjust, EDI);
	GET_STACK(OverlayTypeClass*, pOverlayType, STACK_OFFSET(0x24, -0x14));

	OverlayPaletteTemp::Current = pOverlayType;

	auto const pTypeExt = OverlayTypeExt::ExtMap.Find(pOverlayType);

	if (pTypeExt->ZAdjust != 0)
		R->EDI(zAdjust - pTypeExt->ZAdjust);

	return 0;
}

// CC_Draw_Shape (0x4AED70) receives the palette convert in EDX. For every
// non-wall overlay draw (regular, tiberium and special overlays converge here at
// the shared DrawSHP) EDX holds the cell LightConvert; substitute the overlay
// type's CustomPalette convert when one is configured.
DEFINE_HOOK(0x47FB7B, CellClass_DrawOverlay_CustomPalette, 0x6)
{
	if (auto const pOverlayType = OverlayPaletteTemp::Current)
	{
		if (auto const pConvert = OverlayTypeExt::ExtMap.Find(pOverlayType)->CustomPalette.GetConvert())
			R->EDX(reinterpret_cast<DWORD>(pConvert));
	}

	return 0;
}

// Replaces an Ares hook at 0x47F9A4
DEFINE_HOOK(0x47F974, CellClass_DrawOverlay_Walls, 0x5)
{
	enum { SkipGameCode = 0x47FB86 };

	GET(CellClass*, pThis, ESI);
	GET(SHPStruct*, pShape, EAX);
	GET(RectangleStruct*, pBounds, EBP);
	GET(const int, zAdjust, EDI);
	GET_STACK(OverlayTypeClass*, pOverlayType, STACK_OFFSET(0x24, -0x14));
	REF_STACK(Point2D, pLocation, STACK_OFFSET(0x24, -0x10));

	const int wallOwnerIndex = pThis->WallOwnerIndex;
	int colorSchemeIndex = HouseClass::CurrentPlayer->ColorSchemeIndex;

	if (wallOwnerIndex >= 0)
		colorSchemeIndex = HouseClass::Array[wallOwnerIndex]->ColorSchemeIndex;

	LightConvertClass* pConvert = nullptr;
	auto const pTypeExt = OverlayTypeExt::ExtMap.Find(pOverlayType);

	// A configured CustomPalette overrides both the palette file and the
	// per-house color scheme for walls.
	if (auto const pCustom = pTypeExt->CustomPalette.GetConvert())
	{
		pConvert = reinterpret_cast<LightConvertClass*>(pCustom);
	}
	else if (pTypeExt->Palette)
	{
		pConvert = pTypeExt->Palette->Items[colorSchemeIndex]->LightConvert;
	}
	else
	{
		pConvert = ColorScheme::Array[colorSchemeIndex]->LightConvert;
	}

	DSurface::Temp->DrawSHP(pConvert, pShape, pThis->OverlayData, &pLocation, pBounds,
		BlitterFlags(0x4E00), 0, -2 - zAdjust, ZGradient::Deg90, pThis->Intensity_Normal, 0, 0, 0, 0, 0);

	return SkipGameCode;
}

#pragma region CanBeBuiltOn

DEFINE_HOOK(0x47C9A7, CellClass_IsClearToBuild_Overlays, 0x5)
{
	enum { ReturnFromFunction = 0x47C6D1, CheckTileLandType = 0x47C9CD };

	GET(CellClass*, pThis, EDI);
	GET_STACK(BuildingTypeClass*, pBuildingType, STACK_OFFSET(0x18, 0x8));

	const int overlayTypeIndex = pThis->OverlayTypeIndex;

	if (overlayTypeIndex != -1)
	{
		if (OverlayTypeExt::CanPlaceBuildingOnOverlay(overlayTypeIndex, pBuildingType, false))
			return CheckTileLandType;
	}

	return ReturnFromFunction;
}

DEFINE_HOOK(0x45EF11, BuildingTypeClass_FlushForPlacement_Overlays, 0x6)
{
	enum { Continue = 0x45EF2C };

	GET(BuildingTypeClass*, pThis, EBX);
	GET(const int, overlayTypeIndex, ECX);

	if (OverlayTypeExt::CanPlaceBuildingOnOverlay(overlayTypeIndex, pThis, false))
		return Continue;

	return 0;
}

#pragma endregion
