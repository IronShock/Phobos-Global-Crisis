#pragma once
#include <GadgetClass.h>
#include <CommandClass.h>

// ============================================================================
// Target Filter System - FilterButtonClass
//
// 6 small clickable buttons at the bottom-left of the game screen.
// Allows multiple simultaneous filters (bitmask). Each click toggles one
// filter on/off for all selected Technos. Supports PCX icons and hotkeys.
// ============================================================================

// Target filter categories as bit flags for multi-select support.
enum class TargetFilter : int
{
	None      = 0,
	Infantry  = 1 << 0,  // 1
	Vehicle   = 1 << 1,  // 2
	Artillery = 1 << 2,  // 4
	Building  = 1 << 3,  // 8
	Fighter   = 1 << 4,  // 16
	Bomber    = 1 << 5,  // 32
};
MAKE_ENUM_FLAGS(TargetFilter);

class FilterButtonClass : public GadgetClass
{
public:
	FilterButtonClass() = default;
	FilterButtonClass(TargetFilter filterType, int x, int y, int width, int height);

	~FilterButtonClass();

	virtual bool Draw(bool forced) override;
	virtual void OnMouseEnter() override;
	virtual void OnMouseLeave() override;
	virtual bool Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier) override;

	// Toggle this filter bit on/off for all selected Technos.
	void ApplyFilterToSelection() const;

	// Returns true if any selected Techno has this filter bit set.
	bool IsFilterActiveOnSelection() const;

public:
	TargetFilter FilterType { TargetFilter::None };
	bool IsHovering { false };

	// ---- Static manager helpers ----
	static void InitButtons();
	static void ClearButtons();
	static bool IsAnyHovering();

	// Draw the semi-transparent background behind the filter buttons.
	// Called from GScreenClass_DrawText (before buttons are drawn).
	static void DrawBackground();

	// Draw the FPS counter above the background box.
	// Called from FilterButtonClass::Draw (after buttons are drawn, so FPS is on top).
	static void DrawFPSOverlay();

	// Toggle a filter via hotkey (same as clicking the button).
	static void ToggleFilterByHotkey(int buttonIndex);

private:
	static constexpr int ButtonCount = 6;
	static FilterButtonClass* Buttons[ButtonCount];
};

// ============================================================================
// Hotkey command class for filter buttons
// ============================================================================
template <int N>
class FilterHotkeyCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;
	virtual void Execute(WWKey eInput) const override;
};