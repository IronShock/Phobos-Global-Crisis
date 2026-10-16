#include "ZoomManager.h"

#include <TacticalClass.h>
#include <Surface.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <Unsorted.h>

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Utilities/Patch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include <vector>

bool ZoomManager::Enabled = false;
bool ZoomManager::ScrollEnabled = true;
bool ZoomManager::KeyEnabled = true;
double ZoomManager::CurrentZoom = 1.0;
double ZoomManager::TargetZoom = 1.0;
double ZoomManager::MinZoom = 1.0;
double ZoomManager::MaxZoom = 3.6;
int ZoomManager::ZoomOutRepaint = 1;
bool ZoomManager::ZoomOutKeepText = true;
bool ZoomManager::ZoomOutBandFix = false;
RectangleStruct ZoomManager::DragBandBounds = { 0, 0, 0, 0 };

namespace
{
	// Defined with the rest of the zoom-out helpers further down; ApplySurfacePatches (above
	// them) installs it.
	void BuildSuperWeaponListHook();
}
double ZoomManager::Step = 0.2;
bool ZoomManager::Smooth = true;
double ZoomManager::SmoothRate = 0.25;
double ZoomManager::ActiveSmoothRate = 0.25;
bool ZoomManager::BlitAppliedThisFrame = false;

static bool LastInputLockedState = false;

// Checks whether any tactical zoom (magnify or zoom-out) is currently active
bool ZoomManager::IsZoomed()
{
	return IsMagnifying() || IsZoomingOut();
}

// Magnifying: the window shows a centred crop of the already-drawn frame, stretched up
bool ZoomManager::IsMagnifying()
{
	return CurrentZoom > 1.0001;
}

// Zoom-out: the engine draws a viewport larger than the window (see the pipeline below)
bool ZoomManager::IsZoomingOut()
{
	return CurrentZoom < 0.9999;
}

// Determines if the human player is currently permitted to interact with tactical zoom
bool ZoomManager::CanPlayerZoom()
{
	if (!Enabled)
		return false;

	if (Unsorted::UserInputLocked)
		return false;

	if (ScenarioClass::Instance && ScenarioClass::Instance->UserInputLocked)
		return false;

	return true;
}

// Applies scripted tactical zoom from map triggers with resolution clamping and transition rate
void ZoomManager::SetScriptZoom(double targetZoom, int transitionRate, int minWidth, int minHeight)
{
	if (!TacticalClass::Instance)
		return;

	double clampedZoom = targetZoom;

	// Clamp zoom level to preserve minimum visible tactical viewport dimensions
	if (minWidth > 0 && DSurface::ViewBounds.Width > 0)
	{
		const double maxByWidth = static_cast<double>(DSurface::ViewBounds.Width) / static_cast<double>(minWidth);
		clampedZoom = std::min(clampedZoom, maxByWidth);
	}

	if (minHeight > 0 && DSurface::ViewBounds.Height > 0)
	{
		const double maxByHeight = static_cast<double>(DSurface::ViewBounds.Height) / static_cast<double>(minHeight);
		clampedZoom = std::min(clampedZoom, maxByHeight);
	}

	TargetZoom = std::max(1.0, clampedZoom);

	// Synchronize input lock state to prevent cutscene reset from overriding scripted target
	LastInputLockedState = Unsorted::UserInputLocked || (ScenarioClass::Instance && ScenarioClass::Instance->UserInputLocked);

	if (transitionRate <= 0)
	{
		CurrentZoom = TargetZoom;
		ActiveSmoothRate = SmoothRate;
		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
		{
			TacticalClass::Instance->SetTacticalPosition(&currentPos);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}
	else
	{
		ActiveSmoothRate = std::clamp(static_cast<double>(transitionRate) / 100.0, 0.01, 1.0);
	}
}

// Steps target zoom inward toward maximum magnification
void ZoomManager::ZoomIn()
{
	if (!CanPlayerZoom() || !TacticalClass::Instance)
		return;

	ActiveSmoothRate = SmoothRate;
	TargetZoom = std::clamp(TargetZoom + Step, MinZoom, MaxZoom);

	if (!Smooth)
	{
		CurrentZoom = TargetZoom;
		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
		{
			TacticalClass::Instance->SetTacticalPosition(&currentPos);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}
}

// Steps target zoom outward toward default 1.0x
void ZoomManager::ZoomOut()
{
	if (!CanPlayerZoom() || !TacticalClass::Instance)
		return;

	ActiveSmoothRate = SmoothRate;
	TargetZoom = std::clamp(TargetZoom - Step, MinZoom, MaxZoom);

	if (!Smooth)
	{
		CurrentZoom = TargetZoom;
		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
		{
			TacticalClass::Instance->SetTacticalPosition(&currentPos);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}
}

// Resets target zoom immediately back to 1.0x scale
void ZoomManager::ResetZoom()
{
	if (!CanPlayerZoom() || !TacticalClass::Instance)
		return;

	ActiveSmoothRate = SmoothRate;
	TargetZoom = 1.0;

	if (!Smooth)
	{
		CurrentZoom = 1.0;
		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
			TacticalClass::Instance->SetTacticalPosition(&currentPos);

		MapClass::Instance.MarkNeedsRedraw(2);
	}
}

// Smoothly interpolates current zoom toward target zoom each frame
void ZoomManager::Update()
{
	BlitAppliedThisFrame = false;

	if (!TacticalClass::Instance)
		return;

	const bool currentLocked = Unsorted::UserInputLocked || (ScenarioClass::Instance && ScenarioClass::Instance->UserInputLocked);

	if (currentLocked && !LastInputLockedState)
	{
		// Smoothly restore default view when input is locked for cutscenes without an active script override
		if (std::abs(TargetZoom - 1.0) > 0.0001)
		{
			TargetZoom = 1.0;
			ActiveSmoothRate = SmoothRate;
		}
	}

	LastInputLockedState = currentLocked;

	const double effectiveRate = (ActiveSmoothRate > 0.0) ? ActiveSmoothRate : SmoothRate;

	static bool wasZoomed = false;

	// Zoom-out changes the engine's viewport size, and the engine's incremental bookkeeping
	// cannot follow a viewport that changes every frame - each intermediate size would need a
	// full repaint of the (up to 4x) enlarged viewport, which made the transition stutter.
	// Snap those transitions instead; magnifying is unaffected (it never changes the viewport).
	const bool bZoomOutRegime = (CurrentZoom < 0.9999 || TargetZoom < 0.9999);

	if (Smooth && !bZoomOutRegime && std::abs(CurrentZoom - TargetZoom) > 0.0001)
	{
		CurrentZoom += (TargetZoom - CurrentZoom) * effectiveRate;

		if (std::abs(CurrentZoom - TargetZoom) <= 0.0001)
		{
			CurrentZoom = TargetZoom;
			ActiveSmoothRate = SmoothRate;
		}

		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
		{
			TacticalClass::Instance->SetTacticalPosition(&currentPos);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}
	else if (CurrentZoom != TargetZoom)
	{
		CurrentZoom = TargetZoom;
		ActiveSmoothRate = SmoothRate;
		Point2D currentPos = TacticalClass::Instance->TacticalCoord1;
		if (ClampTacticalPos(&currentPos))
		{
			TacticalClass::Instance->SetTacticalPosition(&currentPos);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}

	const bool isZoomedNow = IsZoomed();
	if (wasZoomed && !isZoomedNow)
		MapClass::Instance.MarkNeedsRedraw(2);

	wasZoomed = isZoomedNow;
}

// Calculates source crop and destination blit rectangles maintaining strict center parity
void ZoomManager::GetBlitRects(RectangleStruct& srcRect, RectangleStruct& dstRect)
{
	const RectangleStruct& vb = DSurface::ViewBounds;
	dstRect = vb;

	if (vb.Width <= 0 || vb.Height <= 0)
	{
		srcRect = vb;
		return;
	}

	if (!IsZoomed())
	{
		srcRect = vb;
		return;
	}

	// Zoom-out: the source region is the whole (larger) viewport the engine draws into.
	// Screen space A maps onto R, so every screen->map conversion goes through here.
	if (IsZoomingOut())
	{
		const double zoom = CurrentZoom;
		const int outW = std::clamp(static_cast<int>(std::round(vb.Width / zoom)), 2, 8192);
		const int outH = std::clamp(static_cast<int>(std::round(vb.Height / zoom)), 2, 8192);
		srcRect = { 0, 0, outW, outH };
		return;
	}

	const double zoom = CurrentZoom;
	int srcW = static_cast<int>(std::round(vb.Width / zoom));
	srcW = std::clamp(srcW, 2, vb.Width);
	if ((srcW % 2) != (vb.Width % 2))
	{
		if (vb.Width / zoom > srcW && srcW + 1 <= vb.Width)
			srcW += 1;
		else if (srcW - 1 >= 2)
			srcW -= 1;
		else
			srcW += 1;
	}

	int srcH = static_cast<int>(std::round(vb.Height / zoom));
	srcH = std::clamp(srcH, 2, vb.Height);
	if ((srcH % 2) != (vb.Height % 2))
	{
		if (vb.Height / zoom > srcH && srcH + 1 <= vb.Height)
			srcH += 1;
		else if (srcH - 1 >= 2)
			srcH -= 1;
		else
			srcH += 1;
	}

	const int cropX = vb.X + (vb.Width - srcW) / 2;
	const int cropY = vb.Y + (vb.Height - srcH) / 2;

	srcRect = { cropX, cropY, srcW, srcH };
}

// Restricts camera coordinates to playable map boundaries, scaled by current zoom
bool ZoomManager::ClampTacticalPos(Point2D* pPoint)
{
	if (!pPoint)
		return false;

	const auto& viewBounds = DSurface::ViewBounds;
	if (viewBounds.Width <= 0 || viewBounds.Height <= 0)
		return false;

	RectangleStruct srcRect, dstRect;
	GetBlitRects(srcRect, dstRect);

	const int effectiveWidth = srcRect.Width;
	const int effectiveHeight = srcRect.Height;

	const int v1 = Make_Global<int>(0x87F8E4);
	const int v2 = Make_Global<int>(0x87F8DC);
	const int v3 = Make_Global<int>(0x87F8E8);
	const int v4 = Make_Global<int>(0x87F8EC);
	const int v5 = Make_Global<int>(0x87F8F0);

	const int minX = 30 * (2 * v1 - v2) + (effectiveWidth / 2);
	const int maxX = std::max(minX, minX + (60 * v4) - effectiveWidth);

	const int minY = 15 * (v2 + 2 * v3 - 5) + (effectiveHeight / 2);
	const int maxY = std::max(minY, minY + ((60 * v5 + 270) / 2) - effectiveHeight);

	// Zoom-out can make the viewport larger than the map: vanilla's clamp then yields an
	// inverted range (min > max) and the camera would stick to one edge. Pin that axis to
	// the map centre instead. Each axis is handled independently, so a degenerate X does
	// not leave Y unclamped (and vice versa).
	const bool pinX = IsZoomingOut() && (60 * v4) < effectiveWidth;
	const bool pinY = IsZoomingOut() && ((30 * v5 + 135) < effectiveHeight);

	bool bClamped = false;

	if (pinX)
	{
		pPoint->X = (minX - effectiveWidth / 2) + (60 * v4) / 2;
		bClamped = true;
	}
	else if (pPoint->X < minX)
	{
		pPoint->X = minX;
		bClamped = true;
	}
	else if (pPoint->X > maxX)
	{
		pPoint->X = maxX;
		bClamped = true;
	}

	if (pinY)
	{
		pPoint->Y = (minY - effectiveHeight / 2) + (30 * v5 + 135) / 2;
		bClamped = true;
	}
	else if (pPoint->Y < minY)
	{
		pPoint->Y = minY;
		bClamped = true;
	}
	else if (pPoint->Y > maxY)
	{
		pPoint->Y = maxY;
		bClamped = true;
	}

	return bClamped;
}

// Same as ScreenToTactical but WITHOUT the "point outside the window viewport -> return it
// unchanged" early-out: while zoomed out the mouse is allowed to sit over the bottom bar or the
// sidebar during a drag, and returning the physical coordinate there left the selection band's
// corner in a different space (the "offset at the screen edge" bug). Used by the band only.
Point2D ZoomManager::ScreenToTacticalClamped(const Point2D& screenPoint)
{
	if (!IsZoomed())
		return screenPoint;

	RectangleStruct srcRect, dstRect;
	GetBlitRects(srcRect, dstRect);

	if (dstRect.Width <= 0 || dstRect.Height <= 0)
		return screenPoint;

	Point2D virtualPoint;
	virtualPoint.X = static_cast<int>(srcRect.X + (static_cast<double>(screenPoint.X - dstRect.X) * srcRect.Width / dstRect.Width) + 0.5);
	virtualPoint.Y = static_cast<int>(srcRect.Y + (static_cast<double>(screenPoint.Y - dstRect.Y) * srcRect.Height / dstRect.Height) + 0.5);

	virtualPoint.X = std::clamp(virtualPoint.X, srcRect.X, srcRect.X + srcRect.Width - 1);
	virtualPoint.Y = std::clamp(virtualPoint.Y, srcRect.Y, srcRect.Y + srcRect.Height - 1);

	return virtualPoint;
}

// Maps screen pixel coordinates into tactical surface space under zoom
Point2D ZoomManager::ScreenToTactical(const Point2D& screenPoint)
{
	if (!IsZoomed())
		return screenPoint;

	RectangleStruct srcRect, dstRect;
	GetBlitRects(srcRect, dstRect);

	if (dstRect.Width <= 0 || dstRect.Height <= 0)
		return screenPoint;

	if (screenPoint.X < dstRect.X || screenPoint.X >= dstRect.X + dstRect.Width ||
	    screenPoint.Y < dstRect.Y || screenPoint.Y >= dstRect.Y + dstRect.Height)
	{
		return screenPoint;
	}

	Point2D virtualPoint;
	virtualPoint.X = static_cast<int>(srcRect.X + (static_cast<double>(screenPoint.X - dstRect.X) * srcRect.Width / dstRect.Width) + 0.5);
	virtualPoint.Y = static_cast<int>(srcRect.Y + (static_cast<double>(screenPoint.Y - dstRect.Y) * srcRect.Height / dstRect.Height) + 0.5);

	virtualPoint.X = std::clamp(virtualPoint.X, srcRect.X, srcRect.X + srcRect.Width - 1);
	virtualPoint.Y = std::clamp(virtualPoint.Y, srcRect.Y, srcRect.Y + srcRect.Height - 1);

	return virtualPoint;
}

// Maps tactical surface coordinates back to screen pixel space
Point2D ZoomManager::TacticalToScreen(const Point2D& virtualPoint)
{
	if (!IsZoomed())
		return virtualPoint;

	RectangleStruct srcRect, dstRect;
	GetBlitRects(srcRect, dstRect);

	if (srcRect.Width <= 0 || srcRect.Height <= 0)
		return virtualPoint;

	Point2D screenPoint;
	screenPoint.X = static_cast<int>(dstRect.X + (static_cast<double>(virtualPoint.X - srcRect.X) * dstRect.Width / srcRect.Width) + 0.5);
	screenPoint.Y = static_cast<int>(dstRect.Y + (static_cast<double>(virtualPoint.Y - srcRect.Y) * dstRect.Height / srcRect.Height) + 0.5);

	return screenPoint;
}

// Performs high-performance CPU software stretch blit directly in RAM, bypassing DirectDraw Blt stalls
void ZoomManager::FastStretchBlit(DSurface* pDst, const RectangleStruct& dstRect, DSurface* pSrc, const RectangleStruct& srcRect)
{
	if (!pDst || !pSrc || dstRect.Width <= 0 || dstRect.Height <= 0 || srcRect.Width <= 0 || srcRect.Height <= 0)
		return;

	WORD* pSrcBase = static_cast<WORD*>(pSrc->Lock(0, 0));
	WORD* pDstBase = static_cast<WORD*>(pDst->Lock(0, 0));

	if (!pSrcBase || !pDstBase)
	{
		if (pDstBase)
			pDst->Unlock();

		if (pSrcBase)
			pSrc->Unlock();

		RectangleStruct clippedDst = dstRect;
		RectangleStruct clippedSrc = srcRect;
		pDst->CopyFrom(&clippedDst, &clippedDst, pSrc, &clippedDst, &clippedSrc, false, false);
		return;
	}

	const int srcPitch = pSrc->GetPitch();
	const int dstPitch = pDst->GetPitch();
	const BYTE* pSrcBytes = reinterpret_cast<const BYTE*>(pSrcBase);
	BYTE* pDstBytes = reinterpret_cast<BYTE*>(pDstBase);

	const int dw = dstRect.Width;
	const int dh = dstRect.Height;
	const int sw = srcRect.Width;
	const int sh = srcRect.Height;
	const int srcMaxX = pSrc->Width - 1;
	const int srcMaxY = pSrc->Height - 1;

	thread_local std::vector<WORD> xMap;
	if (static_cast<int>(xMap.size()) < dw)
		xMap.resize(dw);

	for (int dx = 0; dx < dw; ++dx)
	{
		const int sx = srcRect.X + (dx * sw) / dw;
		xMap[dx] = static_cast<WORD>(std::clamp(sx, 0, srcMaxX));
	}

	const int hexadecs = dw / 16;
	const size_t rowBytes = static_cast<size_t>(dw) * sizeof(WORD);
	int lastSy = -1;
	const WORD* pPrevDstRow = nullptr;

	for (int dy = 0; dy < dh; ++dy)
	{
		int sy = srcRect.Y + (dy * sh) / dh;
		sy = std::clamp(sy, 0, srcMaxY);

		WORD* pDstRow = reinterpret_cast<WORD*>(pDstBytes + (dstRect.Y + dy) * dstPitch) + dstRect.X;

		if (sy == lastSy && pPrevDstRow)
		{
			std::memcpy(pDstRow, pPrevDstRow, rowBytes);
			continue;
		}

		pPrevDstRow = pDstRow;
		lastSy = sy;

		const WORD* pSrcRow = reinterpret_cast<const WORD*>(pSrcBytes + sy * srcPitch);
		int dx = 0;

		for (int h = 0; h < hexadecs; ++h, dx += 16)
		{
			const WORD p0 = pSrcRow[xMap[dx + 0]];
			const WORD p1 = pSrcRow[xMap[dx + 1]];
			const WORD p2 = pSrcRow[xMap[dx + 2]];
			const WORD p3 = pSrcRow[xMap[dx + 3]];
			const WORD p4 = pSrcRow[xMap[dx + 4]];
			const WORD p5 = pSrcRow[xMap[dx + 5]];
			const WORD p6 = pSrcRow[xMap[dx + 6]];
			const WORD p7 = pSrcRow[xMap[dx + 7]];

			const __m128i val0 = _mm_setr_epi16(p0, p1, p2, p3, p4, p5, p6, p7);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(pDstRow + dx + 0), val0);

			const WORD p8 = pSrcRow[xMap[dx + 8]];
			const WORD p9 = pSrcRow[xMap[dx + 9]];
			const WORD p10 = pSrcRow[xMap[dx + 10]];
			const WORD p11 = pSrcRow[xMap[dx + 11]];
			const WORD p12 = pSrcRow[xMap[dx + 12]];
			const WORD p13 = pSrcRow[xMap[dx + 13]];
			const WORD p14 = pSrcRow[xMap[dx + 14]];
			const WORD p15 = pSrcRow[xMap[dx + 15]];

			const __m128i val1 = _mm_setr_epi16(p8, p9, p10, p11, p12, p13, p14, p15);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(pDstRow + dx + 8), val1);
		}

		for (; dx + 8 <= dw; dx += 8)
		{
			const WORD p0 = pSrcRow[xMap[dx + 0]];
			const WORD p1 = pSrcRow[xMap[dx + 1]];
			const WORD p2 = pSrcRow[xMap[dx + 2]];
			const WORD p3 = pSrcRow[xMap[dx + 3]];
			const WORD p4 = pSrcRow[xMap[dx + 4]];
			const WORD p5 = pSrcRow[xMap[dx + 5]];
			const WORD p6 = pSrcRow[xMap[dx + 6]];
			const WORD p7 = pSrcRow[xMap[dx + 7]];

			const __m128i val = _mm_setr_epi16(p0, p1, p2, p3, p4, p5, p6, p7);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(pDstRow + dx), val);
		}

		for (; dx < dw; ++dx)
			pDstRow[dx] = pSrcRow[xMap[dx]];
	}

	pDst->Unlock();
	pSrc->Unlock();
}

// Blits centered viewport crop from Alternate surface onto Composite surface
void ZoomManager::ApplyTacticalBlit()
{
	if (!IsZoomed() || !DSurface::Alternate || !DSurface::Composite)
		return;

	BlitAppliedThisFrame = true;

	RectangleStruct srcRect, dstRect;
	GetBlitRects(srcRect, dstRect);

	FastStretchBlit(DSurface::Composite, dstRect, DSurface::Alternate, srcRect);
}

// Redirects hardcoded DSurface::Composite references in world-space rendering to DSurface::Temp
void ZoomManager::ApplySurfacePatches()
{
	static bool applied = false;
	if (applied)
		return;

	applied = true;

	// EBolt::Draw: redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x4C1EF4 + 2, { 0x14 });
	Patch::Apply_RAW(0x4C24EC + 2, { 0x14 });
	Patch::Apply_RAW(0x4C2601 + 2, { 0x14 });
	Patch::Apply_RAW(0x4C26EE + 2, { 0x14 });

	// LineTrail::Draw: redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x556CDA + 2, { 0x14 });

	// FootClass::Draw_Action_Line (non-player): redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x4DC5BD + 2, { 0x14 });
	Patch::Apply_RAW(0x4DC64A + 2, { 0x14 });
	Patch::Apply_RAW(0x4DC6FE + 1, { 0x14 });

	// TechnoClass order and target action lines (0x007049C0, called by FootClass::DrawActionLines for player units):
	// redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x704AE6 + 2, { 0x14 });
	Patch::Apply_RAW(0x704B93 + 2, { 0x14 });
	Patch::Apply_RAW(0x704C13 + 2, { 0x14 });
	Patch::Apply_RAW(0x704CFD + 2, { 0x14 });
	Patch::Apply_RAW(0x704D87 + 2, { 0x14 });
	Patch::Apply_RAW(0x704DDD + 2, { 0x14 });
	Patch::Apply_RAW(0x704E1D + 2, { 0x14 });

	// TechnoClass mind control, slave, and spawn action lines (0x00704E40):
	// redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x704F76 + 2, { 0x14 });
	Patch::Apply_RAW(0x705000 + 2, { 0x14 });
	Patch::Apply_RAW(0x7051BB + 2, { 0x14 });
	Patch::Apply_RAW(0x70521F + 2, { 0x14 });

	// Planning Mode waypoint nodes and lines: redirect target surface from DSurface::Composite to DSurface::Temp
	Patch::Apply_RAW(0x6353CF + 2, { 0x14 });
	Patch::Apply_RAW(0x635530 + 2, { 0x14 });
	Patch::Apply_RAW(0x63560D + 2, { 0x14 });
	Patch::Apply_RAW(0x635758 + 2, { 0x14 });
	Patch::Apply_RAW(0x63B28D + 2, { 0x14 });
	Patch::Apply_RAW(0x63B3CA + 2, { 0x14 });
	Patch::Apply_RAW(0x63B4E6 + 2, { 0x14 });
	Patch::Apply_RAW(0x63B792 + 2, { 0x14 });
	Patch::Apply_RAW(0x63B88F + 2, { 0x14 });
	Patch::Apply_RAW(0x63B8F8 + 2, { 0x14 });
	Patch::Apply_RAW(0x63B9F4 + 2, { 0x14 });
	Patch::Apply_RAW(0x63BA68 + 2, { 0x14 });
	Patch::Apply_RAW(0x63BBA5 + 2, { 0x14 });
	Patch::Apply_RAW(0x63BC1B + 2, { 0x14 });
	Patch::Apply_RAW(0x63C14D + 2, { 0x14 });
	Patch::Apply_RAW(0x63C208 + 2, { 0x14 });
	Patch::Apply_RAW(0x63C311 + 2, { 0x14 });
	Patch::Apply_RAW(0x63C395 + 2, { 0x14 });
	Patch::Apply_RAW(0x63C476 + 2, { 0x14 });
	Patch::Apply_RAW(0x63C948 + 2, { 0x14 });
	Patch::Apply_RAW(0x63CEE3 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D44A + 2, { 0x14 });
	Patch::Apply_RAW(0x63D4D5 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D5F5 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D6E2 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D758 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D7E7 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D8C3 + 2, { 0x14 });
	Patch::Apply_RAW(0x63D8F0 + 2, { 0x14 });

	// DisplayClass::UpdateDragBand (0x4AC380) clamps the drag band's live corner to ViewBounds.
	// It runs during the input phase, where ViewBounds is still A, so a drag past A got pinned
	// to A's edge and the band rect degenerated - the selection border stopped being drawn.
	// Point those four loads at DragBandBounds instead (R while zoomed out, A otherwise).
	// Each site is byte-checked first: if another mod already rewrote those instructions we
	// leave them alone rather than risk corrupting its patch.
	if (ZoomOutBandFix)
	{
		const uintptr_t shadow = reinterpret_cast<uintptr_t>(&DragBandBounds);
		const uintptr_t sites[4] = { 0x4AC3B3u, 0x4AC3B8u, 0x4AC3BEu, 0x4AC3C4u };
		const uintptr_t offsets[4] = { 4u, 0u, 8u, 12u }; // ViewBounds.Y / .X / .Width / .Height
		const uint32_t expected[4] = { 0x00886FA4u, 0x00886FA0u, 0x00886FA8u, 0x00886FACu };

		for (int i = 0; i < 4; ++i)
		{
			uint32_t* const pOperand = reinterpret_cast<uint32_t*>(sites[i]);

			if (*pOperand != expected[i])
			{
				Debug::Log("[ZoomOut] band fix: operand %d is %p (expected %p) - skipped\n",
					i, *pOperand, expected[i]);
				continue;
			}

			DWORD oldProtect = 0;

			if (!VirtualProtect(pOperand, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect))
				continue;

			*pOperand = static_cast<uint32_t>(shadow + offsets[i]);
			VirtualProtect(pOperand, sizeof(uint32_t), oldProtect, &oldProtect);
		}
	}

	BuildSuperWeaponListHook();
}

// =====================================================================================
// Zoom-out pipeline (MinZoom <= CurrentZoom < 1.0)
// -------------------------------------------------------------------------------------
// The engine rasterises the tactical map 1:1 for whatever rectangle DSurface::ViewBounds
// holds, into DSurface::Composite. It can never show more map than the window holds, so
// "zooming out" means: temporarily enlarge the viewport to R = A / zoom (> A), let the
// engine draw that many map pixels into two offscreen surfaces of size R, then downsample
// R back into the window composite at A. That is the only way to see more map than the
// window - the extra map pixels do not exist otherwise.
//
// Only the three tactical passes see R. Outside of RenderZoomOutScene() DSurface::ViewBounds
// stays at A, so all the one-time layout code (DisplayClass::InitGUI for the sidebar/bottom
// bar, mode toggles, radar geometry) is left completely untouched. What *is* kept at R every
// frame is the engine's viewport cache ([0xB0CE28] - the copy that drives Update_Tactical_
// Position, the camera clamp, the screen<->map conversions and the dirty-area bookkeeping),
// which keeps the camera, the mouse and the incremental redraw all consistent with the
// larger viewport.
//
// Like MOEE's ScaleBar this is render-surface-only bookkeeping: it writes to whatever the
// current render target is and never touches simulation state, so it stays multiplayer safe.
// =====================================================================================

namespace
{
	constexpr uintptr_t kZoomOutDSurfCtor = 0x004BA5A0u; // DSurface::DSurface(this, w, h, 1, 1)
	constexpr uintptr_t kZoomOutOperatorNew = 0x007C8E17u; // engine operator new (cdecl, 1 arg)
	constexpr uintptr_t kZoomOutOperatorDel = 0x007C8B3Du; // engine operator delete (cdecl, 1 arg)
	constexpr uintptr_t kZoomOutZBufCtor = 0x007BC970u; // ZBufferClass ctor, rect by value, ret 0x10
	constexpr uintptr_t kZoomOutZBufDtor = 0x007BCAE0u;
	constexpr uintptr_t kZoomOutABufCtor = 0x00410CE0u; // ABufferClass ctor, rect by value, ret 0x10
	constexpr uintptr_t kZoomOutABufDtor = 0x00410E50u;
	constexpr uintptr_t kZoomOutZBufPtr = 0x00887644u; // ZBufferClass* used by Render every frame
	constexpr uintptr_t kZoomOutABufPtr = 0x0087E8A4u; // ABufferClass*
	constexpr uintptr_t kZoomOutViewCache = 0x00B0CE28u; // RectangleStruct copy of the view size
	constexpr uintptr_t kZoomOutUpdateTacPos = 0x006D8B30u; // Update_Tactical_Position(this)
	constexpr uintptr_t kZoomOutFrameNo = 0x00A8ED84u; // engine frame counter

	constexpr int kZoomOutSurfObjSize = 0x24; // sizeof(DSurface)
	constexpr int kZoomOutBufObjSize = 0x30; // sizeof(ZBufferClass / ABufferClass)
	constexpr int kZoomOutMinSide = 64;
	constexpr int kZoomOutMaxSide = 8192; // keeps the offscreen buffers inside device memory

	struct ZoomOutState
	{
		bool  surfacesActive = false;
		void* bigComp = nullptr; // offscreen stand-in for DSurface::Composite
		void* bigMap = nullptr; // offscreen stand-in for DSurface::Tile
		int   capW = 0; // largest offscreen size allocated so far (only grows)
		int   capH = 0;
		int   viewW = 0; // R currently in use
		int   viewH = 0;
		void* bigZ = nullptr; // offscreen ZBufferClass*
		void* bigA = nullptr; // offscreen ABufferClass*
		int   bufW = 0;
		int   bufH = 0;
		int   redrawFrames = 0; // frames left to force a full redraw of R
		int   redrawFrameNo = -1;
		int   appliedW = 0; // R we last wrote into the engine's viewport cache
		int   appliedH = 0;
		int   exitRedrawFrames = 0; // frames left to force a full repaint of A after leaving
		int   exitRedrawFrameNo = -1;
		Point2D lastTacPos = { 0, 0 }; // TacticalPos as of the previous frame
		bool  haveLastTacPos = false;
		bool  cameraMoved = false; // the camera moved this frame (set by PrepareZoomOut)
		RectangleStruct windowView = { 0, 0, 0, 0 }; // the window viewport (A), captured in prepare
		bool  cacheOwned = false; // we have written R into the engine's viewport cache
		void* retireComp = nullptr; // freed one frame late (the engine still caches surfaces)
		void* retireMap = nullptr;
		void* winZ = nullptr; // the engine's window Z/ABuffers while the passes run
		void* winA = nullptr;
		void* winComp = nullptr; // the engine's window composite while we are zoomed out
		void* winMap = nullptr; // ... and its paired map surface
		void* curComp = nullptr; // the offscreen pair currently installed; the order tracks the
		void* curMap = nullptr; //     engine's own Composite/Tile alternation
		bool  installed = false;
		bool  needInstall = false;
		bool  allocFailed = false; // surfaces / buffers could not be created -> stay at 1x
		bool  logged = false;
		bool  exitLogged = false;
	};

	ZoomOutState g_ZoomOut;

	// Defined further down; DestroySurfaces calls Uninstall so the engine never keeps a
	// pointer to a surface we are about to free.
	void ZoomOutInstallSurfaces();
	void ZoomOutUninstallSurfaces();
	void ZoomOutFillVoidRaw(BYTE* pBase, int pitch, int width, int height);

	// --- keeping text 1:1 while zoomed out ------------------------------------------
	unsigned char* g_SwListTrampPtr = nullptr; // executable trampoline (VirtualAlloc)
	bool  g_SwListSkip = false; // suppress the list draw while the R pass runs
	int   g_SwListCalls = 0; // how many times the pass asked for the list this frame
	uintptr_t g_SwListArg[2] = {}; // its one argument (verified: 1 stack arg, `ret 4`)
	bool  g_TextPhaseDone = false; // the frame was already presented mid-pass

	// --- on-screen timers (0x6D4B50) ------------------------------------------------
	// Verified ABI: FOUR stack arguments plus ECX and EDX, returning with `ret 0x10` (the two
	// call sites between them push four dwords: 0x6D4A3B/3F/4B/5F, and the function's own first
	// `ret 0x10` at 0x6D4DAC agrees). They are recorded at the function ENTRY - never by hooking
	// an `E8 rel32` call site, which corrupts the relative target.
	constexpr int kTimerMaxCalls = 8;
	bool  g_TimerSkip = false;
	int   g_TimerCount = 0;
	unsigned g_TimerRec[kTimerMaxCalls][6] = {}; // ECX, EDX, arg1..arg4

	// Replaces the entry of PowerBarClass::Draw. While the R pass runs the draw is suppressed
	// (the list is redrawn 1:1 afterwards, see RenderZoomOutTextPhase); otherwise it executes the
	// stolen prologue and continues in the original function.
	// Verified ABI: ONE stack argument - the call sites push a single dword (0x6D4644/0x6D46CB)
	// and the function's own entry path ends with `ret 4` (0x6DAD5B). `ret 0x10` was a mistake
	// here: 0x6DA9D0..0x6DE98B spans several functions, so "the last ret in that range" says
	// nothing about this one.
	__declspec(naked) void SuperWeaponList_EntryThunk()
	{
		__asm
		{
			cmp  byte ptr [g_SwListSkip], 0
			je   cont
			inc  dword ptr [g_SwListCalls]
			ret  4
		cont:
			jmp  dword ptr [g_SwListTrampPtr]
		}
	}

	void BuildSuperWeaponListHook()
	{
		if (!ZoomManager::ZoomOutKeepText)
			return;

		static bool done = false;

		if (done)
			return;

		const uintptr_t at = 0x006DA9D0u;
		const unsigned char* p = reinterpret_cast<const unsigned char*>(at);

		// Expected prologue: sub esp,0x64 / push ebx / push ebp (5 complete bytes).
		if (p[0] != 0x83 || p[1] != 0xEC || p[2] != 0x64 || p[3] != 0x53 || p[4] != 0x55)
			return;

		// The trampoline MUST be executable - putting these bytes in a data array made the game
		// fault with "tried to execute <address in .rdata>".
		unsigned char* tramp = static_cast<unsigned char*>(
			VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!tramp)
			return;

		memcpy(tramp, p, 5);
		tramp[5] = 0xE9;
		*reinterpret_cast<int32_t*>(&tramp[6]) = static_cast<int32_t>(
			(at + 5) - (reinterpret_cast<uintptr_t>(&tramp[5]) + 5));
		g_SwListTrampPtr = tramp;

		DWORD oldProtect = 0;

		if (!VirtualProtect(reinterpret_cast<void*>(at), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
			return;

		*reinterpret_cast<unsigned char*>(at) = 0xE9;
		*reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(
			reinterpret_cast<uintptr_t>(&SuperWeaponList_EntryThunk) - (at + 5));
		VirtualProtect(reinterpret_cast<void*>(at), 5, oldProtect, &oldProtect);
		done = true;

		Debug::Log("[ZoomOut] superweapon list hook installed (tramp=%p)\n", tramp);
	}



	void ZoomOutLog(const char* pMessage)
	{
		Debug::Log("[ZoomOut] %s\n", pMessage);
	}

	// --- offscreen surfaces ---------------------------------------------------------

	void ZoomOutDestroySurface(void* pObject)
	{
		if (!pObject)
			return;

		void** pVTable = *reinterpret_cast<void***>(pObject);

		if (pVTable && pVTable[0])
			reinterpret_cast<void(__thiscall*)(void*, int)>(pVTable[0])(pObject, 1);
	}

	void* ZoomOutCreateSurface(int width, int height)
	{
		if (width < 8 || height < 8)
			return nullptr;

		void* pObject = reinterpret_cast<void*(__cdecl*)(size_t)>(kZoomOutOperatorNew)(
			static_cast<size_t>(kZoomOutSurfObjSize));

		if (!pObject)
			return nullptr;

		reinterpret_cast<void(__thiscall*)(void*, int, int, int, int)>(kZoomOutDSurfCtor)(
			pObject, width, height, 1, 1);

		DSurface* pSurface = reinterpret_cast<DSurface*>(pObject);

		// DSurface construction fails silently when the device cannot give us the size.
		if (pSurface->GetWidth() != width || pSurface->GetHeight() != height)
		{
			ZoomOutDestroySurface(pObject);
			return nullptr;
		}

		return pObject;
	}

	void ZoomOutClearSurface(void* pObject)
	{
		if (!pObject)
			return;

		DSurface* pSurface = reinterpret_cast<DSurface*>(pObject);
		const int pitch = pSurface->GetPitch();
		const int width = pSurface->GetWidth();
		const int height = pSurface->GetHeight();

		if (pitch <= 0 || width < 8 || height < 8 || pitch < width * 2)
			return;

		BYTE* pBuffer = static_cast<BYTE*>(pSurface->Lock(0, 0));

		if (!pBuffer)
			return;

		for (int y = 0; y < height; ++y)
			memset(pBuffer + static_cast<size_t>(y) * pitch, 0, static_cast<size_t>(width) * 2);

		pSurface->Unlock();
	}

	// --- ZBuffer / ABuffer ----------------------------------------------------------

	void* ZoomOutBuildBuffer(bool isABuffer, const RectangleStruct& area)
	{
		void* pObject = reinterpret_cast<void*(__cdecl*)(size_t)>(kZoomOutOperatorNew)(
			static_cast<size_t>(kZoomOutBufObjSize));

		if (!pObject)
			return nullptr;

		if (isABuffer)
			reinterpret_cast<void(__thiscall*)(void*, RectangleStruct)>(kZoomOutABufCtor)(pObject, area);
		else
			reinterpret_cast<void(__thiscall*)(void*, RectangleStruct)>(kZoomOutZBufCtor)(pObject, area);

		// Matches the tail of DisplayClass::Set_View_Dimensions step 3.
		if (!isABuffer)
			*reinterpret_cast<unsigned*>(static_cast<BYTE*>(pObject) + 0x24) = 0x8000u;

		return pObject;
	}

	void ZoomOutDropBuffer(bool isABuffer, void* pObject)
	{
		if (!pObject)
			return;

		if (isABuffer)
			reinterpret_cast<void(__thiscall*)(void*)>(kZoomOutABufDtor)(pObject);
		else
			reinterpret_cast<void(__thiscall*)(void*)>(kZoomOutZBufDtor)(pObject);

		reinterpret_cast<void(__cdecl*)(void*)>(kZoomOutOperatorDel)(pObject);
	}

	// --- state ----------------------------------------------------------------------

	void ZoomOutDestroyBuffers()
	{
		ZoomOutDropBuffer(false, g_ZoomOut.bigZ);
		ZoomOutDropBuffer(true, g_ZoomOut.bigA);
		g_ZoomOut.bigZ = nullptr;
		g_ZoomOut.bigA = nullptr;
		g_ZoomOut.bufW = 0;
		g_ZoomOut.bufH = 0;
	}

	void ZoomOutDropSurfacePair(void*& comp, void*& map)
	{
		ZoomOutDestroySurface(comp);
		ZoomOutDestroySurface(map);
		comp = nullptr;
		map = nullptr;
	}

	// Frees the surfaces pending retirement (see ZoomOutRetireSurfaces).
	void ZoomOutFlushRetired()
	{
		ZoomOutDropSurfacePair(g_ZoomOut.retireComp, g_ZoomOut.retireMap);
	}

	void ZoomOutDestroySurfaces()
	{
		// Never free a surface the engine still holds pointers to.
		ZoomOutUninstallSurfaces();
		ZoomOutFlushRetired();
		ZoomOutDropSurfacePair(g_ZoomOut.bigComp, g_ZoomOut.bigMap);
		g_ZoomOut.surfacesActive = false;
		g_ZoomOut.capW = 0;
		g_ZoomOut.capH = 0;
		g_ZoomOut.viewW = 0;
		g_ZoomOut.viewH = 0;
		g_ZoomOut.appliedW = 0;
		g_ZoomOut.appliedH = 0;
	}

	// Move the offscreen surfaces out of use but free them one frame later: the engine
	// caches surface pointers inside its own structures and freeing them in the same frame
	// the swap ends is a use-after-free (MOEE hit this at 0x6D493E).
	void ZoomOutRetireSurfaces()
	{
		ZoomOutFlushRetired();
		g_ZoomOut.retireComp = g_ZoomOut.bigComp;
		g_ZoomOut.retireMap = g_ZoomOut.bigMap;
		g_ZoomOut.bigComp = nullptr;
		g_ZoomOut.bigMap = nullptr;
		g_ZoomOut.surfacesActive = false;
		g_ZoomOut.capW = 0;
		g_ZoomOut.capH = 0;
		g_ZoomOut.viewW = 0;
		g_ZoomOut.viewH = 0;
		g_ZoomOut.appliedW = 0;
		g_ZoomOut.appliedH = 0;
	}

	// R = A / zoom, clamped so the long side stays within the device limit. Returns the
	// size the engine must be told to draw; false when zoom is out of the sane range.
	bool ZoomOutCurrentViewSize(int& outW, int& outH)
	{
		const RectangleStruct a = DSurface::ViewBounds;

		if (a.Width < kZoomOutMinSide || a.Height < kZoomOutMinSide)
			return false;

		const double zoom = ZoomManager::CurrentZoom;

		if (zoom < 0.2 || zoom > 1.0)
			return false;

		double rw = static_cast<double>(a.Width) / zoom;
		double rh = static_cast<double>(a.Height) / zoom;
		const double longSide = rw > rh ? rw : rh;

		if (longSide > static_cast<double>(kZoomOutMaxSide))
		{
			const double scale = static_cast<double>(kZoomOutMaxSide) / longSide;
			rw *= scale;
			rh *= scale;
		}

		outW = static_cast<int>(rw + 0.5);
		outH = static_cast<int>(rh + 0.5);

		if (outW < kZoomOutMinSide || outH < kZoomOutMinSide)
			return false;

		if (outW > kZoomOutMaxSide || outH > kZoomOutMaxSide)
			return false;

		return true;
	}

	bool ZoomOutEnsureSurfaces(int width, int height)
	{
		if (g_ZoomOut.allocFailed)
			return false;

		if (g_ZoomOut.surfacesActive && width <= g_ZoomOut.capW && height <= g_ZoomOut.capH)
		{
			if (g_ZoomOut.viewW != width || g_ZoomOut.viewH != height)
			{
				g_ZoomOut.viewW = width;
				g_ZoomOut.viewH = height;
				g_ZoomOut.redrawFrames = 4;
			}

			return true;
		}

		ZoomOutDestroySurfaces();

		void* pComp = ZoomOutCreateSurface(width, height);

		if (!pComp)
		{
			g_ZoomOut.allocFailed = true;
			ZoomOutLog("offscreen composite allocation FAILED -> zoom-out disabled");
			return false;
		}

		void* pMap = ZoomOutCreateSurface(width, height);

		if (!pMap)
		{
			ZoomOutDestroySurface(pComp);
			g_ZoomOut.allocFailed = true;
			ZoomOutLog("offscreen map allocation FAILED -> zoom-out disabled");
			return false;
		}

		g_ZoomOut.bigComp = pComp;
		g_ZoomOut.bigMap = pMap;
		g_ZoomOut.curComp = pComp;
		g_ZoomOut.curMap = pMap;
		g_ZoomOut.needInstall = true;
		g_ZoomOut.surfacesActive = true;
		g_ZoomOut.capW = width;
		g_ZoomOut.capH = height;
		g_ZoomOut.viewW = width;
		g_ZoomOut.viewH = height;
		g_ZoomOut.redrawFrames = 4;

		// No initial clear: the surfaces' contents are undefined, but we immediately push a full
		// repaint of R for a few frames and repaint the out-of-map strips every frame, so every
		// visible pixel is written before it is shown. Clearing here just cost a 2 x R memset
		// (tens of MB) in the frame the surfaces are created.

		char buffer[96];
		sprintf_s(buffer, "offscreen surfaces ON %dx%d", width, height);
		ZoomOutLog(buffer);
		return true;
	}

	bool ZoomOutEnsureBuffers(int width, int height)
	{
		if (g_ZoomOut.bigZ && g_ZoomOut.bigA && g_ZoomOut.bufW == width && g_ZoomOut.bufH == height)
			return true;

		ZoomOutDestroyBuffers();

		const RectangleStruct area = { 0, 0, width, height };
		void* pZ = ZoomOutBuildBuffer(false, area);
		void* pA = ZoomOutBuildBuffer(true, area);

		if (!pZ || !pA)
		{
			ZoomOutDropBuffer(false, pZ);
			ZoomOutDropBuffer(true, pA);
			ZoomOutLog("Z/ABuffer allocation FAILED -> zoom-out disabled");
			return false;
		}

		// The buffers must really be R sized (the engine addresses them with viewport
		// coordinates every frame), so verify the object fields instead of trusting the
		// constructor call to have landed.
		const int zW = *reinterpret_cast<int*>(static_cast<BYTE*>(pZ) + 0x28);
		const int zH = *reinterpret_cast<int*>(static_cast<BYTE*>(pZ) + 0x2C);

		if (zW != width || zH != height)
		{
			ZoomOutDropBuffer(false, pZ);
			ZoomOutDropBuffer(true, pA);
			ZoomOutLog("Z/ABuffer size mismatch -> zoom-out disabled");
			return false;
		}

		g_ZoomOut.bigZ = pZ;
		g_ZoomOut.bigA = pA;
		g_ZoomOut.bufW = width;
		g_ZoomOut.bufH = height;
		return true;
	}

	// Nearest-neighbour downsample of pSrc's srcRect into pDst's dstRect (both 16bpp).
	// When bFillVoid is set, the out-of-map strips of the source are repainted first (see
	// ZoomOutFillVoidRaw) - done here so both surfaces are locked only once per frame.
	void ZoomOutDemagnify(void* pSrcRaw, const RectangleStruct& srcRect, DSurface* pDst, const RectangleStruct& dstRect,
		bool bFillVoid, const RectangleStruct* pClipDst)
	{
		DSurface* pSrc = reinterpret_cast<DSurface*>(pSrcRaw);

		if (!pSrc || !pDst || pSrc == pDst)
			return;

		const int srcPitch = pSrc->GetPitch();
		const int srcW = pSrc->GetWidth();
		const int srcH = pSrc->GetHeight();
		const int dstPitch = pDst->GetPitch();
		const int dstW = pDst->GetWidth();
		const int dstH = pDst->GetHeight();

		if (srcPitch <= 0 || srcW < 8 || srcH < 8 || srcPitch < srcW * 2)
			return;

		if (dstPitch <= 0 || dstW < 8 || dstH < 8 || dstPitch < dstW * 2)
			return;

		// Clip the source to the surface and the destination to the surface.
		int sx0 = srcRect.X, sy0 = srcRect.Y, sw = srcRect.Width, sh = srcRect.Height;

		if (sx0 < 0) { sw += sx0; sx0 = 0; }
		if (sy0 < 0) { sh += sy0; sy0 = 0; }
		if (sx0 + sw > srcW) sw = srcW - sx0;
		if (sy0 + sh > srcH) sh = srcH - sy0;

		int dx0 = dstRect.X, dy0 = dstRect.Y, dw = dstRect.Width, dh = dstRect.Height;

		if (dx0 < 0) { dw += dx0; dx0 = 0; }
		if (dy0 < 0) { dh += dy0; dy0 = 0; }
		if (dx0 + dw > dstW) dw = dstW - dx0;
		if (dy0 + dh > dstH) dh = dstH - dy0;

		if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
			return;

		// Optional destination-space clip: resample only the region that changed. The clip is
		// expressed relative to dstRect and never changes the scaling ratio.
		int clipX0 = 0, clipX1 = dstRect.Width;
		int clipY0 = 0, clipY1 = dstRect.Height;

		if (pClipDst)
		{
			clipX0 = std::max(0, pClipDst->X - dstRect.X);
			clipX1 = std::min(dstRect.Width, pClipDst->X + pClipDst->Width - dstRect.X);
			clipY0 = std::max(0, pClipDst->Y - dstRect.Y);
			clipY1 = std::min(dstRect.Height, pClipDst->Y + pClipDst->Height - dstRect.Y);

			if (clipX1 <= clipX0 || clipY1 <= clipY0)
				return;
		}

		BYTE* pSrcBase = static_cast<BYTE*>(pSrc->Lock(0, 0));

		if (!pSrcBase)
			return;

		BYTE* pDstBase = static_cast<BYTE*>(pDst->Lock(0, 0));

		if (!pDstBase)
		{
			pSrc->Unlock();
			return;
		}

		// Repaint the out-of-map strips of the source before copying, so a full copy carries
		// the (constant) void colour into the window composite.
		if (bFillVoid)
			ZoomOutFillVoidRaw(static_cast<BYTE*>(pSrcBase), srcPitch, srcW, srcH);

		// Exact 2:1 fast path (the 0.5x level): plain stride-2 gather, no division. Kept scalar
		// on purpose - a hand-written SSE2 lane extraction here produced a subtle smear once,
		// and the scalar loop is already cheap enough to matter little (see notes).
		if (sw == dw * 2 && sh == dh * 2)
		{
			for (int y = clipY0; y < clipY1; ++y)
			{
				const WORD* pSrcRow = reinterpret_cast<const WORD*>(pSrcBase + static_cast<size_t>(sy0 + y * 2) * srcPitch) + sx0;
				WORD* pDstRow = reinterpret_cast<WORD*>(pDstBase + static_cast<size_t>(dy0 + y) * dstPitch) + dx0;

				int x = clipX0;

				// SSE2: pick the even 16-bit lanes. _mm_shufflelo/hi_epi16 put the wanted words in
				// the low half of each 64-bit lane, so the high lane is brought down with
				// _mm_shuffle_epi32(.., 0xEE) before the 32-bit interleave. The lane mapping was
				// verified against the scalar loop ([0,2,4,6,8,10,12,14]) - getting this wrong
				// corrupts 4 of every 8 output pixels.
				for (; x + 8 <= clipX1; x += 8)
				{
					const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pSrcRow + x * 2));
					const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pSrcRow + x * 2 + 8));
					const __m128i a = _mm_unpacklo_epi32(_mm_shufflelo_epi16(v0, 0x88),
						_mm_shuffle_epi32(_mm_shufflehi_epi16(v0, 0x88), 0xEE));
					const __m128i b = _mm_unpacklo_epi32(_mm_shufflelo_epi16(v1, 0x88),
						_mm_shuffle_epi32(_mm_shufflehi_epi16(v1, 0x88), 0xEE));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(pDstRow + x), _mm_unpacklo_epi64(a, b));
				}

				for (; x < clipX1; ++x)
					pDstRow[x] = pSrcRow[x * 2];
			}
		}
		else
		{
			// Incremental (Bresenham style) stepping, seeded exactly at the clip origin so a
			// partial resample keeps the same mapping as a full one. No per-pixel division.
			const int ratioSW = srcRect.Width;
			const int ratioDW = dstRect.Width;
			const int ratioSH = srcRect.Height;
			const int ratioDH = dstRect.Height;

			int sy = sy0 + static_cast<int>((static_cast<long long>(clipY0) * ratioSH) / ratioDH);
			unsigned yAcc = static_cast<unsigned>((static_cast<long long>(clipY0) * ratioSH) % ratioDH);

			for (int y = clipY0; y < clipY1; ++y)
			{
				const WORD* pSrcRow = reinterpret_cast<const WORD*>(pSrcBase + static_cast<size_t>(sy) * srcPitch);
				WORD* pDstRow = reinterpret_cast<WORD*>(pDstBase + static_cast<size_t>(dy0 + y) * dstPitch) + dx0;

				int sx = sx0 + static_cast<int>((static_cast<long long>(clipX0) * ratioSW) / ratioDW);
				unsigned xAcc = static_cast<unsigned>((static_cast<long long>(clipX0) * ratioSW) % ratioDW);
				int x = clipX0;

				for (; x + 8 <= clipX1; x += 8)
				{
					pDstRow[x + 0] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 1] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 2] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 3] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 4] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 5] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 6] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
					pDstRow[x + 7] = pSrcRow[sx]; xAcc += ratioSW; while (xAcc >= static_cast<unsigned>(ratioDW)) { xAcc -= ratioDW; ++sx; }
				}

				for (; x < clipX1; ++x)
				{
					pDstRow[x] = pSrcRow[sx];
					xAcc += ratioSW;

					while (xAcc >= static_cast<unsigned>(ratioDW))
					{
						xAcc -= ratioDW;
						++sx;
					}
				}

				yAcc += ratioSH;

				while (yAcc >= static_cast<unsigned>(ratioDH))
				{
					yAcc -= ratioDH;
					++sy;
				}
			}
		}

		pDst->Unlock();
		pSrc->Unlock();
	}

	// Repaints the parts of the offscreen frame that lie outside the map, every frame.
	//
	// When the map is smaller than R (normal once the viewport is enlarged) the strips around
	// it can never be repainted by any map cell, and the engine's incremental rasteriser shifts
	// the previous frame's pixels into them when the camera moves - so the result is a stale
	// smear along one side ("residue when panning", MOEE notes 11.13 / 11.15). Filling those
	// strips each frame is the fix. Render-target only, multiplayer safe.
	void ZoomOutFillVoidRaw(BYTE* pBase, int pitch, int width, int height)
	{
		if (!pBase || !TacticalClass::Instance)
			return;

		if (pitch < width * 2 || width < 16 || height < 16)
			return;

		const int bx = *reinterpret_cast<int*>(0x87F8DCu);
		const int gx = *reinterpret_cast<int*>(0x87F8E4u);
		const int gy = *reinterpret_cast<int*>(0x87F8E8u);
		const int cw = *reinterpret_cast<int*>(0x87F8ECu);
		const int ch = *reinterpret_cast<int*>(0x87F8F0u);

		if (cw <= 0 || cw > 8192 || ch <= 0 || ch > 8192)
			return;

		if (gx < -4096 || gx > 4096 || gy < -4096 || gy > 4096)
			return;

		// Same pixel mapping the engine (and our clamp) uses: the map's top-left pixel is
		// 30*(2*gridX - fieldDC) / 15*(fieldDC + 2*gridY - 5), drawn at (mapPos - TacticalPos).
		const Point2D tacPos = TacticalClass::Instance->TacticalPos;
		const int mapLeft = 30 * (2 * gx - bx) - tacPos.X;
		const int mapTop = 15 * (bx + 2 * gy - 5) - tacPos.Y;

		int mx0 = mapLeft;
		int my0 = mapTop;
		int mx1 = mapLeft + 60 * cw;
		int my1 = mapTop + 30 * ch + 135;

		if (mx0 < 0) mx0 = 0;
		if (my0 < 0) my0 = 0;
		if (mx1 > width) mx1 = width;
		if (my1 > height) my1 = height;

		const size_t rowBytes = static_cast<size_t>(width) * 2;

		if (mx1 <= mx0 || my1 <= my0)
		{
			for (int y = 0; y < height; ++y)
				memset(pBase + static_cast<size_t>(y) * pitch, 0, rowBytes);
		}
		else
		{
			for (int y = 0; y < my0; ++y)
				memset(pBase + static_cast<size_t>(y) * pitch, 0, rowBytes);

			for (int y = my1; y < height; ++y)
				memset(pBase + static_cast<size_t>(y) * pitch, 0, rowBytes);

			for (int y = my0; y < my1; ++y)
			{
				BYTE* pRow = pBase + static_cast<size_t>(y) * pitch;

				if (mx0 > 0)
					memset(pRow, 0, static_cast<size_t>(mx0) * 2);

				if (mx1 < width)
					memset(pRow + static_cast<size_t>(mx1) * 2, 0, static_cast<size_t>(width - mx1) * 2);
			}
		}
	}

	// --- installed surface pair -----------------------------------------------------

	bool ZoomOutIsBigSurface(const void* pSurface)
	{
		return pSurface != nullptr
			&& (pSurface == g_ZoomOut.bigComp || pSurface == g_ZoomOut.bigMap
				|| pSurface == g_ZoomOut.curComp || pSurface == g_ZoomOut.curMap);
	}

	// Installs the offscreen pair as the engine's Composite/Tile.
	//
	// The engine swaps those two globals once per tactical pass (three times per frame), so the
	// pair alternates across frames and its incremental renderer relies on that phase. We must
	// therefore NOT reset them to a fixed order every frame: install once, then leave the pair
	// alone and only put the window surfaces back for the chrome pass (the next frame restores
	// our pair in exactly the order the engine left it).
	void ZoomOutInstallSurfaces()
	{
		if (!g_ZoomOut.bigComp || !g_ZoomOut.bigMap)
			return;

		const bool bCurrentIsOurs = ZoomOutIsBigSurface(DSurface::Composite)
			&& ZoomOutIsBigSurface(DSurface::Tile);

		if (bCurrentIsOurs && g_ZoomOut.installed && !g_ZoomOut.needInstall)
			return;

		if (!bCurrentIsOurs)
		{
			// First install, or the engine handed us rebuilt window surfaces.
			g_ZoomOut.winComp = DSurface::Composite;
			g_ZoomOut.winMap = DSurface::Tile;
		}

		if (!ZoomOutIsBigSurface(g_ZoomOut.curComp) || !ZoomOutIsBigSurface(g_ZoomOut.curMap))
		{
			g_ZoomOut.curComp = g_ZoomOut.bigComp;
			g_ZoomOut.curMap = g_ZoomOut.bigMap;
		}

		DSurface::Composite = reinterpret_cast<DSurface*>(g_ZoomOut.curComp);
		DSurface::Tile = reinterpret_cast<DSurface*>(g_ZoomOut.curMap);
		DSurface::Temp = DSurface::Composite;
		g_ZoomOut.installed = true;
		g_ZoomOut.needInstall = false;
	}

	void ZoomOutUninstallSurfaces()
	{
		if (g_ZoomOut.installed && g_ZoomOut.winComp && ZoomOutIsBigSurface(DSurface::Composite))
		{
			DSurface::Composite = reinterpret_cast<DSurface*>(g_ZoomOut.winComp);
			DSurface::Tile = reinterpret_cast<DSurface*>(g_ZoomOut.winMap);
			DSurface::Temp = reinterpret_cast<DSurface*>(g_ZoomOut.winComp);
		}

		g_ZoomOut.installed = false;
		g_ZoomOut.needInstall = false;
		g_ZoomOut.curComp = nullptr;
		g_ZoomOut.curMap = nullptr;
		g_ZoomOut.winComp = nullptr;
		g_ZoomOut.winMap = nullptr;
	}

	// Forces the pipeline off and restores the engine's viewport cache to A.
	void ZoomOutShutDown(bool bDeferSurfaces)
	{
		// Put the window surfaces back before the offscreen ones go away.
		ZoomOutUninstallSurfaces();
		ZoomOutDestroyBuffers();

		if (bDeferSurfaces)
			ZoomOutRetireSurfaces();
		else
			ZoomOutDestroySurfaces();

		g_ZoomOut.allocFailed = false;

		// The drag band must clamp against the window viewport again once we are not zoomed out.
		ZoomManager::DragBandBounds = DSurface::ViewBounds;

		// Only touch the engine's viewport cache if we were the ones who moved it.
		if (g_ZoomOut.cacheOwned)
		{
			g_ZoomOut.cacheOwned = false;

			if (TacticalClass::Instance)
			{
				const RectangleStruct a = DSurface::ViewBounds;
				RectangleStruct* pCache = reinterpret_cast<RectangleStruct*>(kZoomOutViewCache);

				if (pCache->Width != a.Width || pCache->Height != a.Height || pCache->X != a.X || pCache->Y != a.Y)
				{
					*pCache = a;
					reinterpret_cast<void(__thiscall*)(void*)>(kZoomOutUpdateTacPos)(TacticalClass::Instance);
				}

				// TacticalPos just jumped by (A - R) / 2. The engine's incremental rasteriser
				// shifts the render surface by (LastTacticalPos - TacticalPos) at the start of
				// the next Draw; leaving the R-space value here drags ~(R - A) / 2 pixels of
				// stale content across the screen. Zero the delta.
				TacticalClass::Instance->LastTacticalPos = TacticalClass::Instance->TacticalPos;
			}
		}
	}
}

// Called every frame from the render entry, before the tactical passes.
void ZoomManager::PrepareZoomOut()
{
	// Free last frame's retired surfaces first (see ZoomOutRetireSurfaces).
	ZoomOutFlushRetired();

	// Leaving zoom-out leaves the downsampled image in the window composite, and the engine
	// only repaints dirty cells, so it would stay on screen until the player scrolls. Force a
	// full repaint of the (restored) window viewport for a few frames.
	if (g_ZoomOut.exitRedrawFrames > 0 && TacticalClass::Instance)
	{
		const int frame = *reinterpret_cast<int*>(kZoomOutFrameNo);

		if (frame != g_ZoomOut.exitRedrawFrameNo)
		{
			g_ZoomOut.exitRedrawFrameNo = frame;
			g_ZoomOut.exitRedrawFrames--;
		}

		const RectangleStruct a = DSurface::ViewBounds;

		if (a.Width >= 16 && a.Height >= 16)
		{
			TacticalClass::Instance->RegisterDirtyArea(a, true);
			MapClass::Instance.MarkNeedsRedraw(2);
		}
	}

	const bool bZoomingOut = Enabled && MinZoom < 1.0
		&& IsZoomingOut() && TacticalClass::Instance != nullptr;

	if (!bZoomingOut)
	{
		const bool bWasActive = g_ZoomOut.cacheOwned;

		ZoomOutShutDown(bWasActive);

		if (bWasActive)
		{
			g_ZoomOut.exitRedrawFrames = 5;
			g_ZoomOut.exitRedrawFrameNo = -1;

			if (TacticalClass::Instance)
			{
				TacticalClass::Instance->RegisterDirtyArea(DSurface::ViewBounds, true);
				MapClass::Instance.MarkNeedsRedraw(2);
			}

			if (!g_ZoomOut.exitLogged)
			{
				g_ZoomOut.exitLogged = true;
				ZoomOutLog("left zoom-out -> forcing full repaint of the window viewport");
			}
		}

		return;
	}

	g_ZoomOut.exitLogged = false;

	int viewW = 0, viewH = 0;

	if (!ZoomOutCurrentViewSize(viewW, viewH)
		|| !ZoomOutEnsureSurfaces(viewW, viewH)
		|| !ZoomOutEnsureBuffers(viewW, viewH))
	{
		// Never render against a half-built pipeline: drop back to 1x this frame.
		CurrentZoom = 1.0;
		TargetZoom = 1.0;
		ActiveSmoothRate = SmoothRate;

		if (!g_ZoomOut.logged)
		{
			g_ZoomOut.logged = true;
			ZoomOutLog("pipeline unavailable -> reset to 1x");
		}

		ZoomOutShutDown(true);
		return;
	}

	g_ZoomOut.logged = false;

	// Install the offscreen pair as the engine's Composite/Tile (once; the engine's own
	// per-pass swaps keep it installed - see ZoomOutInstallSurfaces).
	ZoomOutInstallSurfaces();

	// Capture the window viewport (A) - ViewBounds still holds it here, and the selection
	// band needs it later, inside the passes, where ViewBounds has already become R.
	g_ZoomOut.windowView = DSurface::ViewBounds;

	// DisplayClass::UpdateDragBand clamps the band's live corner against this shadow (patched
	// operands) - it runs in the input phase, where ViewBounds is still A.
	ZoomManager::DragBandBounds = RectangleStruct { 0, 0, viewW, viewH };

	// Keep the engine's viewport cache (and therefore TacticalPos, the camera clamp,
	// the mouse mapping and the dirty-area bookkeeping) on the larger viewport.
	RectangleStruct* pCache = reinterpret_cast<RectangleStruct*>(kZoomOutViewCache);
	pCache->X = 0;
	pCache->Y = 0;
	pCache->Width = viewW;
	pCache->Height = viewH;
	g_ZoomOut.cacheOwned = true;

	reinterpret_cast<void(__thiscall*)(void*)>(kZoomOutUpdateTacPos)(TacticalClass::Instance);

	// Whenever R changes size TacticalPos jumps by (R - R') / 2; zero the incremental
	// shift delta so that frame is not dragged across the screen.
	if (g_ZoomOut.appliedW != viewW || g_ZoomOut.appliedH != viewH)
	{
		g_ZoomOut.appliedW = viewW;
		g_ZoomOut.appliedH = viewH;
		TacticalClass::Instance->LastTacticalPos = TacticalClass::Instance->TacticalPos;
	}

	// bFull: mark the whole of R dirty. bRedraw: ask the engine for a full redraw.
	// A fresh / resized offscreen surface is blank either way, so that case always does both.
	bool bFull = false;
	bool bRedraw = false;

	if (g_ZoomOut.redrawFrames > 0)
	{
		const int frame = *reinterpret_cast<int*>(kZoomOutFrameNo);

		if (frame != g_ZoomOut.redrawFrameNo)
		{
			g_ZoomOut.redrawFrameNo = frame;
			g_ZoomOut.redrawFrames--;
		}

		bFull = true;
		bRedraw = true;
	}

	// The engine's incremental path shifts the previous frame along the camera delta and then
	// only repaints the newly exposed strip, which leaves the rest of an enlarged frame stale
	// (rain / searchlight smears behind the pan). Counters below select how to avoid that:
	//   0 = off (fastest, but trails), 1 = both, 2 = MarkNeedsRedraw only, 3 = dirty area only.
	const Point2D tacPos = TacticalClass::Instance->TacticalPos;
	const bool bCameraMoved = !g_ZoomOut.haveLastTacPos
		|| tacPos.X != g_ZoomOut.lastTacPos.X
		|| tacPos.Y != g_ZoomOut.lastTacPos.Y;

	g_ZoomOut.lastTacPos = tacPos;
	g_ZoomOut.haveLastTacPos = true;

	if (bCameraMoved && ZoomOutRepaint != 0)
	{
		if (ZoomOutRepaint != 2)
			bFull = true;

		if (ZoomOutRepaint != 3)
			bRedraw = true;
	}

	// A frame that must be repainted wholesale (camera moved - the engine's shift copy
	// registers no dirty area - or a MarkNeedsRedraw) must not be resampled through the
	// dirty clip, or the frame would be shown half-updated.
	g_ZoomOut.cameraMoved = bCameraMoved || bRedraw;

	if (bFull)
	{
		const RectangleStruct area = { 0, 0, viewW, viewH };
		TacticalClass::Instance->RegisterDirtyArea(area, true);
	}

	if (bRedraw)
		MapClass::Instance.MarkNeedsRedraw(2);
}

// Releases the offscreen resources (called when zoom-out is left or the mod is disabled).
void ZoomManager::ReleaseZoomOut()
{
	ZoomOutShutDown(false);
}

// The three tactical passes, drawn into the offscreen surfaces at R, then downsampled
// into the window composite at A. The caller draws the chrome afterwards.
bool ZoomManager::RenderZoomOutScene(bool bFlag)
{
	if (!g_ZoomOut.surfacesActive || !g_ZoomOut.bigComp || !g_ZoomOut.bigMap
		|| !g_ZoomOut.bigZ || !g_ZoomOut.bigA)
		return false;

	if (!TacticalClass::Instance || !DSurface::Composite)
		return false;

	// Our offscreen pair is already installed as Composite/Tile (see ZoomOutInstallSurfaces),
	// in whatever order the engine's own swaps left it. The window surfaces we hand back for
	// the chrome pass are remembered by the install step.
	if (!ZoomOutIsBigSurface(DSurface::Composite) || !g_ZoomOut.winComp)
		return false;

	int viewW = 0, viewH = 0;

	if (!ZoomOutCurrentViewSize(viewW, viewH))
		return false;

	DSurface* pWindowComposite = reinterpret_cast<DSurface*>(g_ZoomOut.winComp);
	DSurface* pWindowTile = reinterpret_cast<DSurface*>(g_ZoomOut.winMap);
	void* pWindowZ = *reinterpret_cast<void**>(kZoomOutZBufPtr);
	void* pWindowA = *reinterpret_cast<void**>(kZoomOutABufPtr);
	const RectangleStruct savedBounds = DSurface::ViewBounds;

	// Swap in the bigger viewport for the duration of the three passes only. Composite/Tile and
	// Temp already point at the offscreen pair.
	DSurface::ViewBounds = RectangleStruct { 0, 0, viewW, viewH };
	*reinterpret_cast<void**>(kZoomOutZBufPtr) = g_ZoomOut.bigZ;
	*reinterpret_cast<void**>(kZoomOutABufPtr) = g_ZoomOut.bigA;

	// Suppress the superweapon list while the R pass runs (it is redrawn 1:1 later, see
	// RenderZoomOutTextPhase) and arm the text phase.
	g_ZoomOut.winZ = pWindowZ;
	g_ZoomOut.winA = pWindowA;
	g_SwListSkip = ZoomOutKeepText;
	g_SwListCalls = 0;
	g_TimerSkip = ZoomOutKeepText;
	g_TimerCount = 0;
	g_TextPhaseDone = false;

	TacticalClass::Instance->Render(DSurface::Composite, bFlag, 0);
	TacticalClass::Instance->Render(DSurface::Composite, bFlag, 1);
	TacticalClass::Instance->Render(DSurface::Composite, bFlag, 2);

	g_SwListSkip = false;
	g_TimerSkip = false;

	void* pFinal = DSurface::Temp ? DSurface::Temp : DSurface::Composite;

	// Remember the order the engine left our pair in, so next frame restores exactly that
	// order and its Composite/Tile alternation phase is preserved.
	g_ZoomOut.curComp = DSurface::Composite;
	g_ZoomOut.curMap = DSurface::Tile;

	// Hand the window surfaces back for the chrome pass.
	DSurface::ViewBounds = savedBounds;
	DSurface::Composite = pWindowComposite;
	DSurface::Tile = pWindowTile;
	DSurface::Temp = pWindowComposite;
	*reinterpret_cast<void**>(kZoomOutZBufPtr) = pWindowZ;
	*reinterpret_cast<void**>(kZoomOutABufPtr) = pWindowA;

	// Always resample the whole frame. An earlier version clipped this to the region the
	// engine reported via RegisterDirtyArea, but plenty of repaints (shroud/fog reveals,
	// large overlays, hover) do not register a rect, which left stale content on screen.
	const RectangleStruct srcRect = { 0, 0, viewW, viewH };
	ZoomOutDemagnify(pFinal, srcRect, pWindowComposite, savedBounds, true, nullptr);

	// Redraw the superweapon list that the pass was told to skip - now on the window, at 1:1.
	RenderZoomOutTextPhase();

	// And the on-screen timers, replayed from the calls the pass was told to skip.
	ReplayZoomOutTimers();
	return true;
}

// Diagnostic for the selection band: prints both ends of the transform plus the geometry, so a
// positional error near the screen edges can be traced to a mismatched mapping.
void ZoomManager::LogBandDiag(const char* pTag, const Point2D& raw, const Point2D& out)
{
	if (!IsZoomingOut())
		return;

	static int tick = 0;

	if (tick-- > 0)
		return;

	tick = 20;

	int rw = 0, rh = 0;
	ZoomOutCurrentViewSize(rw, rh);

	const RectangleStruct vb = DSurface::ViewBounds;

	Debug::Log("[BandDiag] %s raw=(%d,%d) -> (%d,%d) VB=(%d,%d,%d,%d) win=(%d,%d,%d,%d) R=%dx%d zoom=%.3f\n",
		pTag, raw.X, raw.Y, out.X, out.Y,
		vb.X, vb.Y, vb.Width, vb.Height,
		g_ZoomOut.windowView.X, g_ZoomOut.windowView.Y,
		g_ZoomOut.windowView.Width, g_ZoomOut.windowView.Height,
		rw, rh, CurrentZoom);
}

// Replays the timer draws the pass was told to skip. Runs at the end of the frame, when the
// window surfaces and ViewBounds are already back, so the text keeps its original size.
// The signature is the one Phobos itself documents in Hooks.UI.cpp: __fastcall with
// ECX = index, EDX = ColorScheme* and four stack arguments (time, text, xy1, xy2).
void ZoomManager::ReplayZoomOutTimers()
{
	using DrawTimerFunc = int(__fastcall*)(int, void*, int, void*, void*, void*);

	const int n = (g_TimerCount < kTimerMaxCalls) ? g_TimerCount : kTimerMaxCalls;
	g_TimerCount = 0;

	for (int i = 0; i < n; ++i)
	{
		const unsigned* rec = g_TimerRec[i];
		reinterpret_cast<DrawTimerFunc>(0x006D4B50u)(
			static_cast<int>(rec[0]), reinterpret_cast<void*>(rec[1]),
			static_cast<int>(rec[2]), reinterpret_cast<void*>(rec[3]),
			reinterpret_cast<void*>(rec[4]), reinterpret_cast<void*>(rec[5]));
	}
}

// Called from the chained hook on the timer draw (0x6D4B50). Returns true when the call must be
// suppressed - the arguments are stashed and replayed 1:1 at the end of the frame.
bool ZoomManager::NoteTimerDraw(REGISTERS* R)
{
	if (!R || !ZoomOutKeepText || !g_TimerSkip || !IsZoomingOut())
		return false;

	if (g_TimerCount < kTimerMaxCalls)
	{
		unsigned* rec = g_TimerRec[g_TimerCount];
		rec[0] = R->ECX();
		rec[1] = R->EDX();
		rec[2] = R->Stack<unsigned>(0x4);
		rec[3] = R->Stack<unsigned>(0x8);
		rec[4] = R->Stack<unsigned>(0xC);
		rec[5] = R->Stack<unsigned>(0x10);
		++g_TimerCount;
	}

	return true;
}

// Runs from inside the third tactical pass (0x6D4941), i.e. exactly where the magnify path blits:
// the objects are done, only UI text (superweapon list + the on-screen timers) follows. Present
// the map now and point the *current surface* and viewport at the window, so that text lands at
// 1:1. Only ViewBounds / Temp / the current surface / the Z- and ABuffers are touched - the
// Composite/Tile pair is left alone, since swapping it mid-pass is what corrupted drawing state.
void ZoomManager::PresentZoomOutTextMidPass()
{
	if (!ZoomOutKeepText)
		return;

	if (!g_ZoomOut.surfacesActive || !g_ZoomOut.bigZ || !g_ZoomOut.winComp || !g_ZoomOut.winZ)
		return;

	if (!TacticalClass::Instance)
		return;

	int viewW = 0, viewH = 0;

	if (!ZoomOutCurrentViewSize(viewW, viewH))
		return;

	void* pFinal = DSurface::Temp ? DSurface::Temp : DSurface::Composite;

	if (!ZoomOutIsBigSurface(pFinal))
		return;

	ZoomOutDemagnify(pFinal, RectangleStruct { 0, 0, viewW, viewH },
		reinterpret_cast<DSurface*>(g_ZoomOut.winComp), g_ZoomOut.windowView, true, nullptr);

	DSurface* pWindow = reinterpret_cast<DSurface*>(g_ZoomOut.winComp);

	DSurface::ViewBounds = g_ZoomOut.windowView;
	DSurface::Temp = pWindow;
	*reinterpret_cast<void**>(0x00887314u) = pWindow; // the "current surface" the draws use
	*reinterpret_cast<void**>(kZoomOutZBufPtr) = g_ZoomOut.winZ;
	*reinterpret_cast<void**>(kZoomOutABufPtr) = g_ZoomOut.winA;

	g_TextPhaseDone = true;
}

// Called from the two PowerBarClass::Draw call sites, on the call instruction itself: the single
// argument is the top stack dword. It is replayed 1:1 at the end of the frame.
void ZoomManager::NoteSuperWeaponListCall(REGISTERS* R)
{
	if (!R || !ZoomOutKeepText || !IsZoomingOut())
		return;

	const int idx = g_SwListCalls;

	if (idx < 0 || idx > 1)
		return;

	g_SwListArg[idx] = R->Stack<uintptr_t>(0);
}

// Called at the end of the frame, after the map has been presented and the window surfaces and
// ViewBounds are back: redraws the superweapon list(s) whose draw inside the enlarged pass was
// suppressed, so the text keeps its original size instead of being shrunk with the map.
// (The on-screen timers are inline code inside the pass and cannot be replayed this way yet.)
void ZoomManager::RenderZoomOutTextPhase()
{
	if (!ZoomOutKeepText)
		return;

	const int calls = g_SwListCalls;
	g_SwListCalls = 0;

	if (calls <= 0 || !TacticalClass::Instance)
		return;

	using FnPowerBarDraw = void(__thiscall*)(void*, uintptr_t);

	const int n = (calls < 2) ? calls : 2;

	// The two engine call sites push 0 and 1 respectively (verified bytes at 0x6D4644 / 0x6D46CB),
	// so replaying them in order is exact. The arguments are deliberately NOT recorded with hooks
	// on those call instructions: they are PC-relative (`E8 rel32`) and hooking one corrupted the
	// call target, which made the game jump into read-only data.
	for (int i = 0; i < n; ++i)
		reinterpret_cast<FnPowerBarDraw>(0x006DA9D0u)(TacticalClass::Instance, static_cast<uintptr_t>(i));
}

// Same as ScreenToTactical, but safe while the tactical passes are running: ViewBounds is R
// then, so GetBlitRects would derive the wrong source rect. Swap the window viewport (A) in
// for the duration of the conversion. (Defined down here because g_ZoomOut lives below.)
Point2D ZoomManager::ScreenToTacticalDuringRender(const Point2D& screenPoint)
{
	if (!IsZoomingOut())
		return ScreenToTactical(screenPoint);

	const RectangleStruct saved = DSurface::ViewBounds;

	if (g_ZoomOut.windowView.Width >= 16 && g_ZoomOut.windowView.Height >= 16)
		DSurface::ViewBounds = g_ZoomOut.windowView;

	const Point2D result = ScreenToTacticalClamped(screenPoint);
	DSurface::ViewBounds = saved;
	return result;
}


