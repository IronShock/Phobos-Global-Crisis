#pragma once

#include <BasicStructures.h>

class CCINIClass;

// CoH-style tactical map: a full-screen overlay that shows the sidebar minimap
// enlarged. Toggled by a hotkey (default Ctrl+M), drawn over the composited
// frame, and left/right clicks on it move the camera exactly like a minimap
// click does. The overlay is read-only: it never issues orders and never writes
// game state, so it is safe in multiplayer.
class TacticalMap
{
public:
	struct SettingsStruct
	{
		bool Enabled = true;
		int Scale = 0;            // 0 = auto-fit to FitPercent, otherwise integer multiplier
		int FitPercent = 90;      // auto-fit limit, percent of the tactical viewport
		bool Smooth = false;      // false = nearest neighbour, true = 2x2 box filter
		int BackdropOpacity = 60;  // 0-100, darkens the viewport behind the map
		int BorderR = 220;
		int BorderG = 220;
		int BorderB = 220;
		bool Toolbar = true;
		int BrushR = 255;
		int BrushG = 240;
		int BrushB = 80;
		int BrushWidth = 2;
		int EraserRadius = 12;
		int RulerR = 120;
		int RulerG = 220;
		int RulerB = 255;
		bool ShareWithTeam = false; // multiplayer: share annotations with allies
	};

	static SettingsStruct Settings;
	static bool Opened;

	static void LoadFromINI(CCINIClass* pINI);
	static void Toggle();
	static void Close();

	// Draws the overlay onto DSurface::Composite. Called once per frame from
	// GScreenClass::Render, after the UI has been composited.
	static void Draw();

	// Handles a click at a screen-space point. Returns true when the point landed
	// on the enlarged map and the camera was moved.
	static bool HandleClick(const Point2D& point);

	// Same as HandleClick but de-duplicated per game frame. Both input paths (the
	// window message handler and the gadget hit test) feed into this one.
	static void HandleClickAt(const Point2D& point);

	// Diagnostic: logs the cell our mapping produces for a radar-local pixel.
	static void DebugComparePixel(int radarX, int radarY);

	// ESC: switches back to the camera tool first, closes the overlay otherwise.
	static bool OnEscape();

	// True while the inline text editor is active (game hotkeys are blocked then).
	static bool IsEditingText();

	// Window-procedure hook used for the inline editor (IME + keyboard).
	static bool HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

	// Clears annotations and measurements (new scenario).
	static void ResetCanvas();

	// Called from the gadget hit-test hook on every input frame. Detects a fresh
	// left/right press and moves the camera, giving the overlay an input path that
	// does not depend on the window message handler.
	static void ProcessGadgetInput(const Point2D& point);

	// Called at the start of GScreenClass::Render. Edge scrolling moves the camera
	// during the input update, i.e. before the frame is rendered; restoring the
	// pinned position here freezes the view without any visible jitter.
	static void PinCamera();
};
