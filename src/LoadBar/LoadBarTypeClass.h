#pragma once

#include "LoadBarSource.h"

#include <BasicStructures.h>
#include <Utilities/Anchor.h>
#include <Utilities/Enum.h>
#include <Utilities/Template.h>

#include <string>
#include <vector>

class CCINIClass;
class TechnoClass;

enum class LoadBarFillMode : int
{
	Remaining = 0,
	Elapsed = 1
};

enum class LoadBarDirection : int
{
	LeftToRight = 0,
	RightToLeft = 1,
	TopToBottom = 2,
	BottomToTop = 3
};

enum class LoadBarStackDirection : int
{
	Up = 0,
	Down = 1,
	Left = 2,
	Right = 3
};

enum class LoadBarEntrySort : int
{
	Remaining = 0,
	Total = 1,
	Type = 2
};

struct LoadBarColorStop
{
	double At = 0.0;
	ColorStruct Color {};
};

// One named bar definition from [LoadBarTypes]. Instances are owned by
// LoadBarManager and are never serialized.
class LoadBarTypeClass
{
public:
	std::string Name;

	// Source
	LoadBarSource Source = LoadBarSource::None;
	int SourceIndex = 0;
	std::vector<std::string> AETypes;
	std::vector<std::string> AEGroups;
	int MaxEntries = 0; // 0 = unlimited (the source decides how many it can produce)
	LoadBarEntrySort EntrySort = LoadBarEntrySort::Remaining;

	// Placement
	Anchor AnchorType { HorizontalPosition::Right, VerticalPosition::Top };
	Valueable<BuildingSelectBracketPosition> AnchorBuilding { BuildingSelectBracketPosition::Top };
	Valueable<TextAlign> AlignHorizontal { TextAlign::Right };
	Valueable<VerticalPosition> AlignVertical { VerticalPosition::Bottom };
	Valueable<Point2D> Offset { Point2D::Empty };
	Nullable<Point2D> OffsetShieldDelta {};
	Valueable<bool> Stack { true };
	Valueable<int> StackSpacing { 1 };
	LoadBarStackDirection StackDirection = LoadBarStackDirection::Up;
	Valueable<int> Priority { 0 };
	Valueable<bool> ScaleWithZoom { true };

	// Size and shape
	Valueable<int> Width { 24 };
	Valueable<int> Height { 3 };
	Valueable<int> Segments { 0 };
	Valueable<int> SegmentSpacing { 1 };
	Valueable<bool> SegmentPartial { true };

	// Fill
	LoadBarFillMode Fill = LoadBarFillMode::Remaining;
	LoadBarDirection Direction = LoadBarDirection::LeftToRight;

	// Colours
	Damageable<ColorStruct> Color { ColorStruct { 64, 220, 64 }, ColorStruct { 255, 220, 64 }, ColorStruct { 255, 64, 64 } };
	Valueable<bool> ColorGradient { false };
	std::vector<LoadBarColorStop> ColorStops;
	Valueable<ColorStruct> Background { ColorStruct { 0, 0, 0 } };
	Valueable<int> BackgroundAlpha { 60 };
	Valueable<ColorStruct> Border { ColorStruct { 0, 0, 0 } };
	Valueable<int> BorderWidth { 0 };

	// Text
	Valueable<bool> Text { false };
	Valueable<ColorStruct> TextColor { ColorStruct { 255, 255, 255 } };
	Valueable<TextAlign> TextAlignH { TextAlign::Center };
	Valueable<bool> TextPercentage { false };
	Valueable<bool> TextAsTimer { false };
	Valueable<bool> TextHideMaxValue { false };
	Valueable<bool> TextLabel { false };

	// Visibility
	Valueable<DisplayShowType> ShowType { DisplayShowType::Select };
	Valueable<AffectedHouse> VisibleToHouses { AffectedHouse::All };
	Valueable<bool> VisibleToHousesObserver { true };
	Valueable<bool> VisibleInSpecialState { true };
	Valueable<bool> HideWhenFull { false };
	Valueable<bool> HideWhenEmpty { false };
	Valueable<bool> HideWhenIndefinite { true };

	LoadBarTypeClass() = default;
	explicit LoadBarTypeClass(const char* pName) : Name(pName ? pName : "") { }

	void LoadFromINI(CCINIClass* pINI, const char* pSection);
	bool CanShow(TechnoClass* pThis) const;
	ColorStruct GetBarColor(double ratio) const;
	void Draw(const RectangleStruct& rect, const LoadBarEntry& entry, double fraction, double scale) const;

private:
	void LoadColorFromINI(CCINIClass* pINI, const char* pSection);
	void BuildAutoColorStops();
};
