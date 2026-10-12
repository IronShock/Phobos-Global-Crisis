#include "FilterButtonClass.h"

#include <Phobos.h>

#include <GScreenClass.h>
#include <ObjectClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <MessageListClass.h>
#include <VocClass.h>
#include <MouseClass.h>
#include <Unsorted.h>
#include <Drawing.h>
#include <HouseClass.h>
#include <RulesClass.h>
#include <PCX.h>
#include <FileSystem.h>

#include <Ext/Techno/Body.h>
#include <Ext/Rules/Body.h>
#include <Commands/TacticalPause.h>

#include <Utilities/GeneralUtils.h>

#include <FPSCounter.h>
#include <Phobos.h>
#include <algorithm>
#include <cmath>

// Static array of the 6 filter buttons.
FilterButtonClass* FilterButtonClass::Buttons[FilterButtonClass::ButtonCount] = {};

// PCX filenames for each button.
static const char* FilterPCXNames[] = {
	"buttonfilter01.pcx",
	"buttonfilter02.pcx",
	"buttonfilter03.pcx",
	"buttonfilter04.pcx",
	"buttonfilter05.pcx",
	"buttonfilter06.pcx"
};

// Cached PCX surfaces (loaded once).
static BSurface* FilterPCXSurfaces[6] = {};

// ============================================================================
// Construction / Destruction
// ============================================================================

FilterButtonClass::FilterButtonClass(TargetFilter filterType, int x, int y, int width, int height)
	: GadgetClass(x, y, width, height, (GadgetFlag::LeftPress | GadgetFlag::LeftRelease), false)
	, FilterType(filterType)
{
}

FilterButtonClass::~FilterButtonClass()
{
	if (this == Make_Global<GadgetClass*>(0x8B3E94))
		this->OnMouseLeave();
}

// ============================================================================
// Drawing
// ============================================================================

bool FilterButtonClass::Draw(bool forced)
{
	if (Phobos::UI::CinemaMode)
		return true;

	const auto pSurface = DSurface::Composite;
	auto bounds = pSurface->GetRect();
	const Point2D location = { this->X, this->Y };
	const RectangleStruct rect = { location.X, location.Y, this->Width, this->Height };

	const int idx = static_cast<int>(std::log2(static_cast<int>(this->FilterType)));
	if (idx < 0 || idx >= ButtonCount)
		return true;

	// Determine if this filter is active on the current selection.
	const bool active = this->IsFilterActiveOnSelection();

	// Try to draw PCX icon if available.
	const auto pPCX = FilterPCXSurfaces[idx];
	if (pPCX)
	{
		RectangleStruct destRect = { location.X, location.Y, this->Width, this->Height };
		PCX::Instance.BlitToSurface(&destRect, pSurface, pPCX);

		// Draw highlight border if active.
		if (active)
		{
			const COLORREF activeBorder = Drawing::RGB_To_Int(ColorStruct(255, 255, 0));
			pSurface->DrawRect(&destRect, activeBorder);
		}
		else if (this->IsHovering)
		{
			const COLORREF hoverBorder = Drawing::RGB_To_Int(Drawing::TooltipColor);
			pSurface->DrawRect(&destRect, hoverBorder);
		}
	}
	else
	{
		// Fallback: colored rectangle with label.
		const COLORREF bgColors[] = {
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Infantry - dark green
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Vehicle  - dark yellow
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Artillery- dark orange
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Building - dark gray
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Fighter  - dark blue
			Drawing::RGB_To_Int(ColorStruct(96,  96,  96 )),  // Bomber   - dark purple
		};

		const wchar_t* labels[] = {
			L"I", L"V", L"A", L"B", L"F", L"b"
		};

		// Fill background.
		RectangleStruct fillRect = rect;
		pSurface->FillRect(&fillRect, bgColors[idx]);

		// Draw border.
		COLORREF borderColor;
		if (active)
			borderColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 0));  // Yellow when active
		else if (this->IsHovering)
			borderColor = Drawing::RGB_To_Int(Drawing::TooltipColor);
		else
			borderColor = Drawing::RGB_To_Int(ColorStruct(64, 64, 64));

		RectangleStruct borderRect = rect;
		pSurface->DrawRect(&borderRect, borderColor);

		// Draw label text.
		Point2D textPos = { location.X + this->Width / 2, location.Y + 1 };
		const COLORREF textColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 255));
		constexpr TextPrintType printType =
			TextPrintType::FullShadow | TextPrintType::Point8 |
			TextPrintType::Background | TextPrintType::Center;

		pSurface->DrawTextA(labels[idx], &bounds, &textPos, textColor, 0, printType);
	}

	// Draw FPS overlay after button content (ensures FPS is on top of all buttons).
	// Uses a frame counter so it only draws once per frame regardless of how many
	// buttons call this method.
	DrawFPSOverlay();

	return true;
}

// ============================================================================
// Mouse handling
// ============================================================================

void FilterButtonClass::OnMouseEnter()
{
	if (Phobos::UI::CinemaMode)
		return;

	this->IsHovering = true;
}

void FilterButtonClass::OnMouseLeave()
{
	this->IsHovering = false;
}

bool FilterButtonClass::Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier)
{
	if (TacticalPauseCommandClass::IsBlockingActions())
		return true;

	if (Phobos::UI::CinemaMode)
		return false;

	if (flags & GadgetFlag::LeftPress)
	{
		MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);
		VocClass::PlayGlobal(RulesClass::Instance->GUIBuildSound, 0x2000, 1.0f);
		this->ApplyFilterToSelection();
	}

	this->GadgetClass::Action(flags, pKey, KeyModifier::None);
	return true;
}

// ============================================================================
// Filter logic (bitmask: toggle individual bits)
// ============================================================================

void FilterButtonClass::ApplyFilterToSelection() const
{
	const auto& currentObjects = ObjectClass::CurrentObjects;
	const int count = currentObjects.Count;

	if (count == 0)
		return;

	const int filterBit = static_cast<int>(this->FilterType);

	// Check if ALL selected Technos already have this bit set.
	// If so, toggle it off. Otherwise, toggle it on.
	bool allHaveBit = true;

	for (int i = 0; i < count; ++i)
	{
		auto const pObject = currentObjects.GetItem(i);
		auto const pTechno = abstract_cast<TechnoClass*>(pObject);

		if (!pTechno)
			continue;

		auto const pExt = TechnoExt::ExtMap.Find(pTechno);
		if (!pExt || !(pExt->CurrentTargetFilter & filterBit))
		{
			allHaveBit = false;
			break;
		}
	}

	// Toggle: if all have it, remove it; otherwise add it.
	const int newMask = allHaveBit
		? (~filterBit)  // Remove this bit
		: filterBit;     // Add this bit

	for (int i = 0; i < count; ++i)
	{
		auto const pObject = currentObjects.GetItem(i);
		auto const pTechno = abstract_cast<TechnoClass*>(pObject);

		if (!pTechno)
			continue;

		if (auto const pExt = TechnoExt::ExtMap.Find(pTechno))
		{
			if (allHaveBit)
				pExt->CurrentTargetFilter &= ~filterBit;  // Remove bit
			else
				pExt->CurrentTargetFilter |= filterBit;   // Add bit

			// If filter is active, clear target if it doesn't match.
			if (pExt->CurrentTargetFilter != 0)
			{
				auto const pTarget = abstract_cast<TechnoClass*>(pTechno->Target);
				if (pTarget)
				{
					if (!RulesExt::Global()->IsTargetInFilter(
						pExt->CurrentTargetFilter, pTarget))
					{
						pTechno->SetTarget(nullptr);
					}
				}
			}
		}
	}
}

bool FilterButtonClass::IsFilterActiveOnSelection() const
{
	const auto& currentObjects = ObjectClass::CurrentObjects;
	const int count = currentObjects.Count;
	const int filterBit = static_cast<int>(this->FilterType);

	for (int i = 0; i < count; ++i)
	{
		auto const pObject = currentObjects.GetItem(i);
		auto const pTechno = abstract_cast<TechnoClass*>(pObject);

		if (!pTechno)
			continue;

		if (auto const pExt = TechnoExt::ExtMap.Find(pTechno))
		{
			if (pExt->CurrentTargetFilter & filterBit)
				return true;
		}
	}

	return false;
}

// ============================================================================
// Background + FPS display
// ============================================================================

// Static frame counter to ensure FPS is drawn only once per frame.
static int s_LastFPSDrawFrame = 0;

void FilterButtonClass::DrawBackground()
{
	const auto pSurface = DSurface::Composite;

	// Geometry must match InitButtons():
	//   buttonSize = 28, gap = 3, startX = 5, startY = screenHeight - 65
	constexpr int buttonSize = 28;
	constexpr int gap = 3;
	constexpr int startX = 5;
	constexpr int padding = 4;  // padding around the button group

	const int screenHeight = pSurface->GetHeight();
	const int startY = screenHeight - 65;

	// Total width of all 6 buttons + 5 gaps
	const int buttonsWidth = ButtonCount * buttonSize + (ButtonCount - 1) * gap;

	// Background rect with padding
	RectangleStruct bgRect = {
		startX - padding,
		startY - padding,
		buttonsWidth + padding * 2,
		buttonSize + padding * 2
	};

	// Draw semi-transparent black background
	ColorStruct black { 0x0, 0x0, 0x0 };
	pSurface->FillRectTrans(&bgRect, &black, 40);

	// Draw a thin border around the background
	const COLORREF borderColor = Drawing::RGB_To_Int(ColorStruct(100, 100, 100));
	pSurface->DrawRect(&bgRect, borderColor);
}

void FilterButtonClass::DrawFPSOverlay()
{
	if (!Phobos::Config::ShowFPS)
		return;

	// Only draw once per frame (buttons call Draw() in sequence)
	const int currentFrame = Unsorted::CurrentFrame;
	if (s_LastFPSDrawFrame == currentFrame)
		return;
	s_LastFPSDrawFrame = currentFrame;

	const auto pSurface = DSurface::Composite;

	// Geometry must match InitButtons() and DrawBackground():
	constexpr int buttonSize = 28;
	constexpr int gap = 3;
	constexpr int startX = 5;
	constexpr int padding = 4;

	const int screenHeight = pSurface->GetHeight();
	const int startY = screenHeight - 65;
	const int buttonsWidth = ButtonCount * buttonSize + (ButtonCount - 1) * gap;

	// Background rect (same as DrawBackground)
	const int bgX = startX - padding;
	const int bgY = startY - padding;
	const int bgW = buttonsWidth + padding * 2;

	// Format FPS text
	wchar_t fpsBuffer[0x20] {};
	const unsigned int fps = FPSCounter::CurrentFrameRate;
	swprintf(fpsBuffer, std::size(fpsBuffer), L"FPS: %d", static_cast<int>(fps));

	auto bounds = pSurface->GetRect();
	auto textSize = Drawing::GetTextDimensions(fpsBuffer, { 0, 0 }, 0, 2, 0);

	// Position FPS text centered above the background box.
	// Using Center flag so X is the center point of the text.
	const int fpsX = bgX + bgW / 2;
	const int fpsY = bgY - textSize.Height - 2;

	// Semi-transparent background for FPS text
	ColorStruct black { 0x0, 0x0, 0x0 };
	RectangleStruct fpsRect = {
		fpsX - textSize.Width / 2 - 3,
		fpsY - 1,
		textSize.Width + 6,
		textSize.Height + 2
	};
	pSurface->FillRectTrans(&fpsRect, &black, 40);

	// Draw FPS text using the same rendering method as button labels
	Point2D fpsPos { fpsX, fpsY };
	const COLORREF textColor = Drawing::RGB_To_Int(ColorStruct(255, 255, 255));
	constexpr TextPrintType printType =
		TextPrintType::FullShadow | TextPrintType::Point8 |
		TextPrintType::Background | TextPrintType::Center;
	pSurface->DrawTextA(fpsBuffer, &bounds, &fpsPos, textColor, 0, printType);
}

// ============================================================================
// Static manager
// ============================================================================

void FilterButtonClass::InitButtons()
{
	ClearButtons();

	constexpr int buttonSize = 28;
	constexpr int gap = 3;
	constexpr int startX = 5;

	const int screenHeight = DSurface::Composite->GetHeight();
	const int startY = screenHeight - 65;

	const TargetFilter types[] = {
		TargetFilter::Infantry,
		TargetFilter::Vehicle,
		TargetFilter::Artillery,
		TargetFilter::Building,
		TargetFilter::Fighter,
		TargetFilter::Bomber
	};

	// Load PCX icons for each button.
	for (int i = 0; i < ButtonCount; ++i)
	{
		FilterPCXSurfaces[i] = nullptr;
		PCX::Instance.LoadFile(FilterPCXNames[i]);
		FilterPCXSurfaces[i] = PCX::Instance.GetSurface(FilterPCXNames[i]);
	}

	for (int i = 0; i < ButtonCount; ++i)
	{
		const int x = startX + i * (buttonSize + gap);
		auto pButton = GameCreate<FilterButtonClass>(
			types[i], x, startY, buttonSize, buttonSize);

		pButton->Zap();
		GScreenClass::Instance.AddButton(pButton);

		Buttons[i] = pButton;
	}
}

void FilterButtonClass::ClearButtons()
{
	for (int i = 0; i < ButtonCount; ++i)
	{
		if (Buttons[i])
		{
			GScreenClass::Instance.RemoveButton(Buttons[i]);
			GameDelete(Buttons[i]);
			Buttons[i] = nullptr;
		}
	}
}

bool FilterButtonClass::IsAnyHovering()
{
	for (int i = 0; i < ButtonCount; ++i)
	{
		if (Buttons[i] && Buttons[i]->IsHovering)
			return true;
	}
	return false;
}

void FilterButtonClass::ToggleFilterByHotkey(int buttonIndex)
{
	if (buttonIndex < 0 || buttonIndex >= ButtonCount)
		return;

	if (Buttons[buttonIndex])
	{
		VocClass::PlayGlobal(RulesClass::Instance->GUIBuildSound, 0x2000, 1.0f);
		Buttons[buttonIndex]->ApplyFilterToSelection();
	}
}

// ============================================================================
// Hotkey command class template implementations
// ============================================================================

template <int N>
const char* FilterHotkeyCommandClass<N>::GetName() const
{
	static const char* names[] = {
		"Filter Infantry",
		"Filter Vehicle",
		"Filter Artillery",
		"Filter Building",
		"Filter Fighter",
		"Filter Bomber"
	};
	return names[N];
}

template <int N>
const wchar_t* FilterHotkeyCommandClass<N>::GetUIName() const
{
	static const char* keys[] = {
		"TXT_TARGET_FILTER_INFANTRY",
		"TXT_TARGET_FILTER_VEHICLE",
		"TXT_TARGET_FILTER_ARTILLERY",
		"TXT_TARGET_FILTER_BUILDING",
		"TXT_TARGET_FILTER_FIGHTER",
		"TXT_TARGET_FILTER_BOMBER"
	};
	static const wchar_t* defaults[] = {
		L"Target Filter: Infantry",
		L"Target Filter: Vehicle",
		L"Target Filter: Artillery",
		L"Target Filter: Building",
		L"Target Filter: Fighter",
		L"Target Filter: Bomber"
	};
	return GeneralUtils::LoadStringUnlessMissing(keys[N], defaults[N]);
}

template <int N>
const wchar_t* FilterHotkeyCommandClass<N>::GetUICategory() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TARGET_FILTER_CATEGORY", L"Target Filters");
}

template <int N>
const wchar_t* FilterHotkeyCommandClass<N>::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TARGET_FILTER_DESC",
		L"Toggle target filter for selected units.");
}

template <int N>
void FilterHotkeyCommandClass<N>::Execute(WWKey eInput) const
{
	FilterButtonClass::ToggleFilterByHotkey(N);
}

// Explicit template instantiations
template class FilterHotkeyCommandClass<0>;
template class FilterHotkeyCommandClass<1>;
template class FilterHotkeyCommandClass<2>;
template class FilterHotkeyCommandClass<3>;
template class FilterHotkeyCommandClass<4>;
template class FilterHotkeyCommandClass<5>;
