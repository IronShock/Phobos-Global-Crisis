#pragma once

#include <GeneralStructures.h>

class DSurface;
class REGISTERS;

class ZoomManager
{
public:
	static bool Enabled;
	static bool ScrollEnabled;
	static bool KeyEnabled;
	static double CurrentZoom;
	static double TargetZoom;
	static double MinZoom;
	static double MaxZoom;
	static int ZoomOutRepaint;
	static bool ZoomOutKeepText; // keep the superweapon list / timers 1:1 (see 0x6D4941)
	static bool ZoomOutBandFix; // clamp the selection band against R (patched operands)
	static double Step;
	static bool Smooth;
	static double SmoothRate;
	static double ActiveSmoothRate;
	static bool BlitAppliedThisFrame;

	// Performs high-performance CPU software stretch blit directly in RAM, bypassing DirectDraw Blt stalls
	static void FastStretchBlit(DSurface* pDst, const RectangleStruct& dstRect, DSurface* pSrc, const RectangleStruct& srcRect);

	// Redirects hardcoded DSurface::Composite references to DSurface::Temp for tactical rendering
	static void ApplySurfacePatches();

	// Checks whether player manual zoom interaction is allowed
	static bool CanPlayerZoom();

	// Applies scripted tactical zoom from map triggers with optional resolution clamping
	static void SetScriptZoom(double targetZoom, int transitionRate, int minWidth, int minHeight);

	// Adjusts target zoom level incrementally
	static void ZoomIn();
	static void ZoomOut();

	// Resets zoom to default 1.0x scale
	static void ResetZoom();

	// Smoothly interpolates current zoom toward target zoom each frame
	static void Update();

	// Calculates source crop and destination blit rectangles maintaining strict center parity
	static void GetBlitRects(RectangleStruct& srcRect, RectangleStruct& dstRect);

	// Blits tactical surface to composite buffer with centering and scaling
	static void ApplyTacticalBlit();

	// Checks whether zoom level is currently greater than 1.0x
	static bool IsZoomed();

	// Directional predicates. Magnifying = 1.0x < zoom <= MaxZoom (the window shows a
	// centred crop of the already-drawn frame stretched up). ZoomingOut = MinZoom <=
	// zoom < 1.0x (the engine draws a viewport larger than the window into offscreen
	// surfaces, which are then downsampled back into the window).
	static bool IsMagnifying();
	static bool IsZoomingOut();

	// Zoom-out pipeline. PrepareZoomOut() runs once per frame (render entry) to keep
	// the engine's viewport cache and the offscreen surfaces / Z- and ABuffers in sync
	// with the current zoom level. RenderZoomOutScene() performs the three tactical
	// passes against the offscreen surfaces and downsamples the result into the window
	// composite. Returns false when the pipeline is unavailable (caller falls back).
	// Per-frame maintenance for the zoom-out pipeline (called from the render entry).
	static void PrepareZoomOut();
	static void ReleaseZoomOut();
	static bool RenderZoomOutScene(bool bFlag);

	// Shadow of ViewBounds for DisplayClass::UpdateDragBand (0x4AC380): the engine clamps the
	// drag band's live corner to ViewBounds, but that runs during the input phase where
	// ViewBounds is still A - so a drag reaching past A got pinned to A's edge and the band
	// rect degenerated (no border drawn). Four operands are patched to read this instead:
	// R while zoomed out, A otherwise.
	static RectangleStruct DragBandBounds;

	// The superweapon list (PowerBarClass::Draw) and the on-screen timers are drawn inside the
	// third tactical pass, so zooming out used to shrink them along with the map. The list's
	// two call sites report their argument, the list draw itself is suppressed for the R pass,
	// and both are drawn 1:1 after the downsample (see RenderZoomOutTextPhase).
	static void NoteSuperWeaponListDraw(int arg);
	static void NoteSuperWeaponListCall(REGISTERS* R);
	static void RenderZoomOutTextPhase();
	// Replays the on-screen timer draws that the enlarged pass was told to skip (0x6D4B50).
	static void ReplayZoomOutTimers();
	// Chained onto 0x6D4B50 by the hook: records the call while the enlarged pass runs.
	static bool NoteTimerDraw(REGISTERS* R);
	// Like ScreenToTactical but without the early-out for points outside the window viewport
	// (used by the selection band, whose ends may sit over the bottom bar / sidebar).
	static Point2D ScreenToTacticalClamped(const Point2D& screenPoint);
	// Diagnostic: logs the band's two ends and the geometry behind them (rate limited).
	static void LogBandDiag(const char* pTag, const Point2D& raw, const Point2D& out);
	// Presents the map from inside the third pass and switches the *current surface* to the
	// window (leaving Composite/Tile alone) so the text drawn right after is 1:1.
	static void PresentZoomOutTextMidPass();

	// Restricts camera coordinates to playable map boundaries, scaled by current zoom
	static bool ClampTacticalPos(Point2D* pPoint);

	// Transforms screen coordinates to tactical composite coordinates for mouse interactions
	static Point2D ScreenToTactical(const Point2D& screenPoint);

	// Same, but safe to call *while the tactical passes are running* (ViewBounds is R then, so
	// the plain version would derive the wrong source rect). Used by the selection band, which
	// reads the live mouse position inside the third pass.
	static Point2D ScreenToTacticalDuringRender(const Point2D& screenPoint);

	// Transforms tactical coordinates back to screen coordinates
	static Point2D TacticalToScreen(const Point2D& virtualPoint);
};
