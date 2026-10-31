#include "RadarSignal.h"

#include <RadarClass.h>
#include <TechnoClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <FileSystem.h>
#include <Unsorted.h>

#include <Utilities/EnumFunctions.h>

#include <Ext/TechnoType/Body.h>

#include <algorithm>
#include <cstdint>

namespace
{
	bool IsVisible(TechnoClass* pTechno, TechnoTypeClass* pType, TechnoTypeExt::ExtData* pTypeExt)
	{
		if (pTypeExt->RadarSignal_AlwaysVisible)
			return true;

		if (MapClass::Instance.IsLocationShrouded(pTechno->GetCoords())
			&& !pTechno->Owner->IsControlledByCurrentPlayer())
		{
			return false;
		}

		if (pType->RadarInvisible
			&& EnumFunctions::CanTargetHouse(
				pTypeExt->RadarInvisibleToHouse.Get(AffectedHouse::Enemies),
				pTechno->Owner, HouseClass::CurrentPlayer))
		{
			return false;
		}

		return true;
	}

	int GetFrame(TechnoTypeExt::ExtData* pTypeExt, int frames)
	{
		int frame = pTypeExt->RadarSignal_Frame.Get(0);
		const int frameCount = std::max(1, pTypeExt->RadarSignal_FrameCount.Get(1));
		const int animRate = pTypeExt->RadarSignal_AnimRate.Get(6);

		if (frameCount > 1 && animRate > 0)
			frame += (Unsorted::CurrentFrame / animRate) % frameCount;

		if (frames > 0)
			frame = std::clamp(frame, 0, frames - 1);
		else if (frame < 0)
			frame = 0;

		return frame;
	}

	// 0x69E090 loads a SHPReference's pixel data on demand (the engine calls it
	// from CC_Draw_Shape the same way).
	constexpr uintptr_t kLoadSHPReference = 0x69E090;

	SHPFile* GetLoadedFile(SHPStruct* pImage)
	{
		if (!pImage)
			return nullptr;

		if (auto const pRef = pImage->AsReference())
		{
			if (!pRef->Loaded)
				reinterpret_cast<void(__thiscall*)(SHPReference*)>(kLoadSHPReference)(pRef);

			pImage = pRef->Data;
		}

		if (!pImage || pImage->IsReference())
			return nullptr;

		return pImage->AsFile();
	}

	// Draws one minimap marker centred on (centerX, centerY). Uses the native SHP
	// size and slides markers inside the radar image instead of clipping them.
	void DrawMiniMarker(DSurface* pSurface, const RectangleStruct& radarRect, int centerX, int centerY,
		TechnoTypeExt::ExtData* pTypeExt)
	{
		auto const pImage = pTypeExt->RadarSignal_Image.Get();

		if (!pImage)
			return;

		// SHP references report width/height 0 until their data is loaded, so the
		// loaded file is used for sizing.
		auto const pFile = GetLoadedFile(pImage);
		const int imageW = pFile ? pFile->Width : pImage->Width;
		const int imageH = pFile ? pFile->Height : pImage->Height;

		if (imageW <= 0 || imageH <= 0)
			return;

		if (imageW > radarRect.Width || imageH > radarRect.Height)
			return;

		// Keep edge markers visible by sliding them inside instead of dropping them.
		const int left = std::clamp(centerX - imageW / 2, radarRect.X, radarRect.X + radarRect.Width - imageW);
		const int top = std::clamp(centerY - imageH / 2, radarRect.Y, radarRect.Y + radarRect.Height - imageH);

		Point2D location { left, top };

		RectangleStruct bounds = pSurface->GetRect();
		auto const pConvert = pTypeExt->RadarSignal_Palette.GetOrDefaultConvert(FileSystem::SIDEBAR_PAL);

		pSurface->DrawSHP(pConvert, pImage, GetFrame(pTypeExt, pFile ? pFile->Frames : pImage->Frames), &location, &bounds,
			BlitterFlags::bf_400, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
	}

	bool PassesCommonChecks(TechnoClass* pTechno, TechnoTypeClass*& pType, TechnoTypeExt::ExtData*& pTypeExt)
	{
		if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner)
			return false;

		pType = pTechno->GetTechnoType();
		pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

		if (!pTypeExt->RadarSignal || !pTypeExt->RadarSignal_Image.Get())
			return false;

		if (!IsVisible(pTechno, pType, pTypeExt))
			return false;

		const int blinkRate = pTypeExt->RadarSignal_Blink.Get(0);

		if (blinkRate > 0 && ((Unsorted::CurrentFrame / blinkRate) & 1))
			return false;

		return true;
	}
}

void RadarSignal::DrawMarkers(DSurface* pSurface, const RectangleStruct& radarRect)
{
	if (!pSurface || radarRect.Width <= 0 || radarRect.Height <= 0)
		return;

	// Radar must be active and its deploy animation finished. The engine state
	// machine reports 0x14AC=3 with 0x14FC climbing to 31 while the radar loads, and
	// 0x14AC=1 with 0x14FC=32 once it is fully on screen; retracting/off is 2/0.
	// Drawing earlier would put markers on top of the loading animation.
	auto& radar = RadarClass::Instance;

	if (!radar.IsAvailableNow || radar.unknown_14AC != 1 || radar.unknown_14FC < 32)
		return;

	auto* const pRadar = &RadarClass::Instance;

	for (auto const pTechno : TechnoClass::Array)
	{
		TechnoTypeClass* pType = nullptr;
		TechnoTypeExt::ExtData* pTypeExt = nullptr;

		if (!PassesCommonChecks(pTechno, pType, pTypeExt))
			continue;

		CoordStruct coords = pTechno->GetCoords();
		Point2D pixel {};

		if (!pRadar->GetCrdOnRadar(&pixel, &coords, false))
			continue;

		DrawMiniMarker(pSurface, radarRect, radarRect.X + pixel.X, radarRect.Y + pixel.Y, pTypeExt);
	}
}

void RadarSignal::DrawToSidebar(RadarClass* pRadar)
{
	const auto& radarRect = pRadar->unknown_rect_149C;

	if (!DSurface::Sidebar || radarRect.Width <= 0 || radarRect.Height <= 0)
		return;

	DrawMarkers(DSurface::Sidebar, radarRect);
}

