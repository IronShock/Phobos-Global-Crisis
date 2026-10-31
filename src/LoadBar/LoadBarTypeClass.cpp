#include "LoadBarTypeClass.h"

#include <HouseClass.h>
#include <RulesClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <Drawing.h>
#include <Surface.h>

#include <Utilities/Debug.h>
#include <Utilities/EnumFunctions.h>
#include <Utilities/INIParser.h>

#include <Misc/TextScale.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <utility>

namespace
{
	LoadBarEntrySort ParseEntrySort(const char* text)
	{
		if (text && _stricmp(text, "total") == 0)
			return LoadBarEntrySort::Total;

		if (text && _stricmp(text, "type") == 0)
			return LoadBarEntrySort::Type;

		return LoadBarEntrySort::Remaining;
	}

	LoadBarStackDirection ParseStackDirection(const char* text)
	{
		if (text && _stricmp(text, "down") == 0)
			return LoadBarStackDirection::Down;

		if (text && _stricmp(text, "left") == 0)
			return LoadBarStackDirection::Left;

		if (text && _stricmp(text, "right") == 0)
			return LoadBarStackDirection::Right;

		return LoadBarStackDirection::Up;
	}

	LoadBarDirection ParseDirection(const char* text)
	{
		if (text && _stricmp(text, "righttoleft") == 0)
			return LoadBarDirection::RightToLeft;

		if (text && _stricmp(text, "toptobottom") == 0)
			return LoadBarDirection::TopToBottom;

		if (text && _stricmp(text, "bottomtotop") == 0)
			return LoadBarDirection::BottomToTop;

		return LoadBarDirection::LeftToRight;
	}

	LoadBarFillMode ParseFillMode(const char* text)
	{
		if (text && _stricmp(text, "elapsed") == 0)
			return LoadBarFillMode::Elapsed;

		return LoadBarFillMode::Remaining;
	}

	ColorStruct LerpColor(const ColorStruct& a, const ColorStruct& b, double t)
	{
		t = std::clamp(t, 0.0, 1.0);

		return ColorStruct {
			static_cast<BYTE>(std::lround(a.R + (b.R - a.R) * t)),
			static_cast<BYTE>(std::lround(a.G + (b.G - a.G) * t)),
			static_cast<BYTE>(std::lround(a.B + (b.B - a.B) * t))
		};
	}

	void SortAndDedupeStops(std::vector<LoadBarColorStop>& stops)
	{
		std::stable_sort(stops.begin(), stops.end(),
			[](const LoadBarColorStop& a, const LoadBarColorStop& b) { return a.At < b.At; });

		std::vector<LoadBarColorStop> unique;
		unique.reserve(stops.size());

		for (auto const& stop : stops)
		{
			if (!unique.empty() && std::abs(unique.back().At - stop.At) < 1e-6)
				unique.back() = stop; // later definition wins at the same position
			else
				unique.push_back(stop);
		}

		stops = std::move(unique);
	}

	RectangleStruct Intersect(const RectangleStruct& a, const RectangleStruct& b)
	{
		const int left = std::max(a.X, b.X);
		const int top = std::max(a.Y, b.Y);
		const int right = std::min(a.X + a.Width, b.X + b.Width);
		const int bottom = std::min(a.Y + a.Height, b.Y + b.Height);

		if (right <= left || bottom <= top)
			return RectangleStruct { 0, 0, 0, 0 };

		return RectangleStruct { left, top, right - left, bottom - top };
	}
}

const char* LoadBarSourceToString(LoadBarSource source)
{
	switch (source)
	{
	case LoadBarSource::Health: return "Health";
	case LoadBarSource::Shield: return "Shield";
	case LoadBarSource::Ammo: return "Ammo";
	case LoadBarSource::MindControl: return "MindControl";
	case LoadBarSource::Spawns: return "Spawns";
	case LoadBarSource::Passengers: return "Passengers";
	case LoadBarSource::Tiberium: return "Tiberium";
	case LoadBarSource::Experience: return "Experience";
	case LoadBarSource::Occupants: return "Occupants";
	case LoadBarSource::GattlingStage: return "GattlingStage";
	case LoadBarSource::ROF: return "ROF";
	case LoadBarSource::Reload: return "Reload";
	case LoadBarSource::SpawnTimer: return "SpawnTimer";
	case LoadBarSource::GattlingTimer: return "GattlingTimer";
	case LoadBarSource::ProduceCash: return "ProduceCash";
	case LoadBarSource::PassengerKill: return "PassengerKill";
	case LoadBarSource::AutoDeath: return "AutoDeath";
	case LoadBarSource::SuperWeapon: return "SuperWeapon";
	case LoadBarSource::IronCurtain: return "IronCurtain";
	case LoadBarSource::TemporalLife: return "TemporalLife";
	case LoadBarSource::FactoryProcess: return "FactoryProcess";
	case LoadBarSource::WeaponCooldown: return "WeaponCooldown";
	case LoadBarSource::AttachEffect: return "AttachEffect";
	default: return "None";
	}
}

LoadBarSource LoadBarSourceFromString(const char* text)
{
	static const std::pair<const char*, LoadBarSource> Names[] =
	{
		{ "health", LoadBarSource::Health },
		{ "shield", LoadBarSource::Shield },
		{ "ammo", LoadBarSource::Ammo },
		{ "mindcontrol", LoadBarSource::MindControl },
		{ "spawns", LoadBarSource::Spawns },
		{ "passengers", LoadBarSource::Passengers },
		{ "tiberium", LoadBarSource::Tiberium },
		{ "experience", LoadBarSource::Experience },
		{ "occupants", LoadBarSource::Occupants },
		{ "gattlingstage", LoadBarSource::GattlingStage },
		{ "rof", LoadBarSource::ROF },
		{ "reload", LoadBarSource::Reload },
		{ "spawntimer", LoadBarSource::SpawnTimer },
		{ "gattlingtimer", LoadBarSource::GattlingTimer },
		{ "producecash", LoadBarSource::ProduceCash },
		{ "passengerkill", LoadBarSource::PassengerKill },
		{ "autodeath", LoadBarSource::AutoDeath },
		{ "superweapon", LoadBarSource::SuperWeapon },
		{ "ironcurtain", LoadBarSource::IronCurtain },
		{ "temporallife", LoadBarSource::TemporalLife },
		{ "factoryprocess", LoadBarSource::FactoryProcess },
		{ "weaponcooldown", LoadBarSource::WeaponCooldown },
		{ "attacheffect", LoadBarSource::AttachEffect },
	};

	if (!text || !*text)
		return LoadBarSource::None;

	for (auto const& [name, source] : Names)
	{
		if (_stricmp(text, name) == 0)
			return source;
	}

	Debug::Log("[LoadBar] Unknown Source=%s\n", text);
	return LoadBarSource::None;
}

void LoadBarTypeClass::LoadFromINI(CCINIClass* pINI, const char* pSection)
{
	if (!pINI || !pSection)
		return;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	if (exINI.ReadString(pSection, "Source"))
		this->Source = LoadBarSourceFromString(exINI.value());

	exINI.ReadInteger(pSection, "SourceIndex", &this->SourceIndex);
	exINI.ParseStringList(this->AETypes, pSection, "AE.Type");
	exINI.ParseStringList(this->AEGroups, pSection, "AE.Group");
	exINI.ReadInteger(pSection, "MaxEntries", &this->MaxEntries);

	if (exINI.ReadString(pSection, "Entry.Sort"))
		this->EntrySort = ParseEntrySort(exINI.value());

	this->AnchorType.Read(exINI, pSection, "Anchor.%s");
	this->AnchorBuilding.Read(exINI, pSection, "Anchor.Building");
	this->AlignHorizontal.Read(exINI, pSection, "Align.Horizontal");
	this->AlignVertical.Read(exINI, pSection, "Align.Vertical");
	this->Offset.Read(exINI, pSection, "Offset");
	this->OffsetShieldDelta.Read(exINI, pSection, "Offset.ShieldDelta");
	this->Stack.Read(exINI, pSection, "Stack");
	this->StackSpacing.Read(exINI, pSection, "Stack.Spacing");

	if (exINI.ReadString(pSection, "Stack.Direction"))
		this->StackDirection = ParseStackDirection(exINI.value());

	this->Priority.Read(exINI, pSection, "Priority");
	this->ScaleWithZoom.Read(exINI, pSection, "ScaleWithZoom");

	this->Width.Read(exINI, pSection, "Width");
	this->Height.Read(exINI, pSection, "Height");
	this->Segments.Read(exINI, pSection, "Segments");
	this->SegmentSpacing.Read(exINI, pSection, "Segment.Spacing");
	this->SegmentPartial.Read(exINI, pSection, "Segment.Partial");

	if (exINI.ReadString(pSection, "Fill"))
		this->Fill = ParseFillMode(exINI.value());

	if (exINI.ReadString(pSection, "Direction"))
		this->Direction = ParseDirection(exINI.value());

	// [LoadBar] acts as the global colour/gradient default; the bar section
	// overrides any keys it defines. Explicit stops always apply, even when
	// Color.Gradient=no.
	this->LoadColorFromINI(pINI, "LoadBar");
	this->LoadColorFromINI(pINI, pSection);

	if (this->ColorGradient && this->ColorStops.empty())
		this->BuildAutoColorStops();

	this->Background.Read(exINI, pSection, "Background");
	this->BackgroundAlpha.Read(exINI, pSection, "Background.Alpha");
	this->Border.Read(exINI, pSection, "Border");
	this->BorderWidth.Read(exINI, pSection, "Border.Width");

	this->Text.Read(exINI, pSection, "Text");
	this->TextColor.Read(exINI, pSection, "Text.Color");
	this->TextAlignH.Read(exINI, pSection, "Text.Align");
	this->TextPercentage.Read(exINI, pSection, "Text.Percentage");
	this->TextAsTimer.Read(exINI, pSection, "Text.AsTimer");
	this->TextHideMaxValue.Read(exINI, pSection, "Text.HideMaxValue");
	this->TextLabel.Read(exINI, pSection, "Text.Label");

	this->ShowType.Read(exINI, pSection, "ShowType");
	this->VisibleToHouses.Read(exINI, pSection, "VisibleToHouses");
	this->VisibleToHousesObserver.Read(exINI, pSection, "VisibleToHouses.Observer");
	this->VisibleInSpecialState.Read(exINI, pSection, "VisibleInSpecialState");
	this->HideWhenFull.Read(exINI, pSection, "HideWhenFull");
	this->HideWhenEmpty.Read(exINI, pSection, "HideWhenEmpty");
	this->HideWhenIndefinite.Read(exINI, pSection, "HideWhenIndefinite");

	const int borderWidth = std::max(0, this->BorderWidth.Get());

	if (borderWidth > 0
		&& (this->Width.Get() <= borderWidth * 2 || this->Height.Get() <= borderWidth * 2))
	{
		Debug::Log("[LoadBar] Bar '%s': Border.Width=%d leaves no fill area at %dx%d; border will be skipped.\n",
			this->Name.c_str(), borderWidth, this->Width.Get(), this->Height.Get());
	}
}

void LoadBarTypeClass::LoadColorFromINI(CCINIClass* pINI, const char* pSection)
{
	if (!pINI || !pSection || !pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	this->Color.Read(exINI, pSection, "Color.%s");
	this->ColorGradient.Read(exINI, pSection, "Color.Gradient");

	// Stop lists are always auto-scanned: Color.Gradient.0.At / .Color,
	// Color.Gradient.1.At / .Color, ... until a pair is missing. There is no
	// stop-count key to keep in sync.
	std::vector<LoadBarColorStop> parsed;

	for (int i = 0; i < 128; ++i)
	{
		char atKey[0x40] {};
		char colorKey[0x40] {};

		_snprintf_s(atKey, _TRUNCATE, "Color.Gradient.%d.At", i);
		_snprintf_s(colorKey, _TRUNCATE, "Color.Gradient.%d.Color", i);

		double at = 0.0;
		ColorStruct stopColor {};

		const bool hasAt = exINI.ReadDouble(pSection, atKey, &at);
		const bool hasColor = exINI.Read3Bytes(pSection, colorKey, reinterpret_cast<byte*>(&stopColor));

		if (!hasAt && !hasColor)
			break; // end of the consecutive stop list

		if (!hasAt || !hasColor)
		{
			Debug::Log("[LoadBar] Section '%s': %s present without %s; stop scan stopped.\n",
				pSection, hasAt ? atKey : colorKey, hasAt ? colorKey : atKey);
			break;
		}

		if (at < 0.0 || at > 1.0)
		{
			Debug::Log("[LoadBar] Section '%s': %s=%.3f is outside 0..1; clamped.\n", pSection, atKey, at);
			at = std::clamp(at, 0.0, 1.0);
		}

		parsed.push_back(LoadBarColorStop { at, stopColor });
	}

	if (!parsed.empty())
	{
		this->ColorStops = std::move(parsed);
		SortAndDedupeStops(this->ColorStops);
	}
}

void LoadBarTypeClass::BuildAutoColorStops()
{
	this->ColorStops.clear();

	double yellowAt = 0.5;
	double redAt = 0.25;

	if (RulesClass::Instance)
	{
		yellowAt = std::clamp(RulesClass::Instance->ConditionYellow, 0.0, 1.0);
		redAt = std::clamp(RulesClass::Instance->ConditionRed, 0.0, 1.0);
	}

	this->ColorStops.push_back(LoadBarColorStop { 0.0, this->Color.Get(0.0, yellowAt, redAt) });
	this->ColorStops.push_back(LoadBarColorStop { redAt, this->Color.Get(redAt, yellowAt, redAt) });
	this->ColorStops.push_back(LoadBarColorStop { yellowAt, this->Color.Get(yellowAt, yellowAt, redAt) });
	this->ColorStops.push_back(LoadBarColorStop { 1.0, this->Color.Get(1.0, yellowAt, redAt) });

	SortAndDedupeStops(this->ColorStops);
}

bool LoadBarTypeClass::CanShow(TechnoClass* pThis) const
{
	if (!pThis)
		return false;

	if (HouseClass::IsCurrentPlayerObserver())
	{
		if (!this->VisibleToHousesObserver)
			return false;
	}
	else if (!EnumFunctions::CanTargetHouse(this->VisibleToHouses, pThis->Owner, HouseClass::CurrentPlayer))
	{
		return false;
	}

	if (!this->VisibleInSpecialState && (pThis->TemporalTargetingMe || pThis->IsIronCurtained()))
		return false;

	const DisplayShowType flags = this->ShowType;

	if (flags == DisplayShowType::All)
		return true;

	DisplayShowType current = pThis->IsMouseHovering ? DisplayShowType::CursorHover : DisplayShowType::None;

	if (pThis->IsSelected)
		current |= DisplayShowType::Selected;

	if (current != DisplayShowType::None)
		return (current & flags) != DisplayShowType::None;

	return (flags & DisplayShowType::Idle) != DisplayShowType::None;
}

ColorStruct LoadBarTypeClass::GetBarColor(double ratio) const
{
	// Explicit stops always win; Color.Gradient only controls the automatic
	// fallback when no stop list is configured.
	if (this->ColorStops.empty())
		return this->Color.Get(ratio);

	ratio = std::clamp(ratio, 0.0, 1.0);

	if (ratio <= this->ColorStops.front().At)
		return this->ColorStops.front().Color;

	if (ratio >= this->ColorStops.back().At)
		return this->ColorStops.back().Color;

	for (size_t i = 0; i + 1 < this->ColorStops.size(); ++i)
	{
		const auto& from = this->ColorStops[i];
		const auto& to = this->ColorStops[i + 1];

		if (ratio <= to.At)
		{
			const double span = to.At - from.At;
			const double t = span > 1e-9 ? (ratio - from.At) / span : 1.0;
			return LerpColor(from.Color, to.Color, t);
		}
	}

	return this->ColorStops.back().Color;
}

void LoadBarTypeClass::Draw(const RectangleStruct& rect, const LoadBarEntry& entry, double fraction, double scale) const
{
	if (!DSurface::Composite || rect.Width <= 0 || rect.Height <= 0)
		return;

	const RectangleStruct view = DSurface::ViewBounds;

	const int alpha = std::clamp(this->BackgroundAlpha.Get(), 0, 100);

	if (alpha > 0)
	{
		RectangleStruct background = Intersect(rect, view);

		if (background.Width > 0 && background.Height > 0)
		{
			ColorStruct backgroundColour = this->Background.Get();
			DSurface::Composite->FillRectTrans(&background, &backgroundColour, alpha);
		}
	}

	const double clamped = std::clamp(fraction, 0.0, 1.0);
	const ColorStruct fillColour = this->GetBarColor(clamped);
	const COLORREF fillColourRef = Drawing::RGB_To_Int(fillColour);

	const int borderWidth = std::max(0, static_cast<int>(std::lround(this->BorderWidth.Get() * scale)));
	RectangleStruct fillArea = rect;
	bool drawBorder = borderWidth > 0;

	if (drawBorder)
	{
		fillArea.X += borderWidth;
		fillArea.Y += borderWidth;
		fillArea.Width -= borderWidth * 2;
		fillArea.Height -= borderWidth * 2;

		// A 2px-high bar with a 1px border has no room for a fill. Fall back to
		// no border so the progress is always visible instead of turning black.
		if (fillArea.Width <= 0 || fillArea.Height <= 0)
		{
			fillArea = rect;
			drawBorder = false;
		}
	}

	// Border first, fill second: the border can never cover the progress.
	if (drawBorder)
	{
		const COLORREF borderColour = Drawing::RGB_To_Int(this->Border.Get());
		RectangleStruct clip = view;

		for (int i = 0; i < borderWidth; ++i)
		{
			RectangleStruct borderRect { rect.X + i, rect.Y + i, rect.Width - i * 2, rect.Height - i * 2 };

			if (borderRect.Width <= 0 || borderRect.Height <= 0)
				break;

			DSurface::Composite->DrawRectEx(&clip, &borderRect, borderColour);
		}
	}

	if (this->Segments.Get() > 1)
	{
		const int segments = std::max(1, this->Segments.Get());
		const int spacing = std::max(0, static_cast<int>(std::lround(this->SegmentSpacing.Get() * scale)));
		const bool horizontal = this->Direction == LoadBarDirection::LeftToRight
			|| this->Direction == LoadBarDirection::RightToLeft;
		const int available = horizontal ? fillArea.Width : fillArea.Height;
		const int cell = std::max(1, (available - (segments - 1) * spacing) / segments);
		const double lit = clamped * segments;
		const bool partial = this->SegmentPartial.Get();

		for (int i = 0; i < segments; ++i)
		{
			const double part = std::clamp(lit - i, 0.0, 1.0);

			if (part <= 0.0 || (!partial && part < 1.0))
				continue;

			const int length = std::max(1, static_cast<int>(std::lround(cell * part)));
			RectangleStruct cellRect { 0, 0, 0, 0 };

			if (this->Direction == LoadBarDirection::LeftToRight)
			{
				cellRect = { fillArea.X + i * (cell + spacing), fillArea.Y, length, fillArea.Height };
			}
			else if (this->Direction == LoadBarDirection::RightToLeft)
			{
				cellRect = { fillArea.X + fillArea.Width - cell - i * (cell + spacing), fillArea.Y, length, fillArea.Height };
			}
			else if (this->Direction == LoadBarDirection::TopToBottom)
			{
				cellRect = { fillArea.X, fillArea.Y + i * (cell + spacing), fillArea.Width, length };
			}
			else
			{
				cellRect = { fillArea.X, fillArea.Y + fillArea.Height - cell - i * (cell + spacing), fillArea.Width, length };
			}

			// Clip segmented cells to the bar itself so a large segment count
			// cannot spill outside the configured width at small zoom levels.
			RectangleStruct clip = Intersect(fillArea, view);
			DSurface::Composite->FillRectEx(&clip, &cellRect, fillColourRef);
		}
	}
	else
	{
		RectangleStruct fillRect { 0, 0, 0, 0 };

		if (this->Direction == LoadBarDirection::LeftToRight)
		{
			const int length = static_cast<int>(std::lround(fillArea.Width * clamped));
			fillRect = { fillArea.X, fillArea.Y, length, fillArea.Height };
		}
		else if (this->Direction == LoadBarDirection::RightToLeft)
		{
			const int length = static_cast<int>(std::lround(fillArea.Width * clamped));
			fillRect = { fillArea.X + fillArea.Width - length, fillArea.Y, length, fillArea.Height };
		}
		else if (this->Direction == LoadBarDirection::TopToBottom)
		{
			const int length = static_cast<int>(std::lround(fillArea.Height * clamped));
			fillRect = { fillArea.X, fillArea.Y, fillArea.Width, length };
		}
		else
		{
			const int length = static_cast<int>(std::lround(fillArea.Height * clamped));
			fillRect = { fillArea.X, fillArea.Y + fillArea.Height - length, fillArea.Width, length };
		}

		if (fillRect.Width > 0 && fillRect.Height > 0)
		{
			RectangleStruct clip = view;
			DSurface::Composite->FillRectEx(&clip, &fillRect, fillColourRef);
		}
	}

	if (!this->Text)
		return;

	wchar_t buffer[0x100] {};
	wchar_t labelPart[0x40] {};

	if (this->TextLabel && entry.Label && *entry.Label)
		swprintf(labelPart, std::size(labelPart), L"%hs ", entry.Label);

	if (this->TextPercentage)
	{
		swprintf(buffer, std::size(buffer), L"%ls%d%%", labelPart, static_cast<int>(std::lround(clamped * 100.0)));
	}
	else if (this->TextAsTimer)
	{
		const int seconds = std::max(0, entry.Value) / 15;
		const int minutes = seconds / 60;
		const int hours = minutes / 60;

		if (hours > 0)
			swprintf(buffer, std::size(buffer), L"%ls%d:%02d:%02d", labelPart, hours, minutes % 60, seconds % 60);
		else
			swprintf(buffer, std::size(buffer), L"%ls%d:%02d", labelPart, minutes % 60, seconds % 60);
	}
	else if (this->TextHideMaxValue)
	{
		swprintf(buffer, std::size(buffer), L"%ls%d", labelPart, entry.Value);
	}
	else
	{
		swprintf(buffer, std::size(buffer), L"%ls%d/%d", labelPart, entry.Value, entry.Total);
	}

	const auto dimensions = Drawing::GetTextDimensions(buffer, { 0, 0 }, 0, 2, 0);
	const int textWidth = dimensions.Width;
	const int textHeight = dimensions.Height;

	int textX = rect.X;
	const TextAlign align = this->TextAlignH.Get();

	if (align == TextAlign::Center)
		textX = rect.X + (rect.Width - textWidth) / 2;
	else if (align == TextAlign::Right)
		textX = rect.X + rect.Width - textWidth - 2;
	else
		textX = rect.X + 2;

	int textY = rect.Y + (rect.Height - textHeight) / 2;

	if (rect.Height < textHeight + 2)
		textY = rect.Y - textHeight - 1;

	Point2D textPos { textX, textY };
	RectangleStruct clip = view;
	const COLORREF textColour = Drawing::RGB_To_Int(this->TextColor.Get());
	const TextPrintType flags = TextPrintType::FullShadow | TextPrintType::LASTPOINT;

	TextScale::ScopedExclusion unscaled;
	DSurface::Composite->DrawTextA(buffer, &clip, &textPos, textColour, 0, flags);
}
