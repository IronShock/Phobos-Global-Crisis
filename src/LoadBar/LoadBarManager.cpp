#include "LoadBarManager.h"

#include "LoadBarTypeClass.h"
#include "LoadBarValue.h"

#include <CCINIClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <New/Entity/AttachEffectClass.h>
#include <New/Type/AttachEffectTypeClass.h>

#include <Ext/Techno/Body.h>

#include <Misc/ZoomManager.h>

#include <Phobos.h>
#include <Utilities/Debug.h>
#include <Utilities/INIParser.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	struct LoadBarDrawRequest
	{
		RectangleStruct Rect;
		LoadBarEntry Entry;
		const LoadBarTypeClass* Type;
		double Fraction;
		double Scale;
		int Priority;
	};

	std::vector<std::unique_ptr<LoadBarTypeClass>> g_types;
	std::unordered_map<std::string, LoadBarTypeClass*> g_typesByName;

	// 0 = Infantry, 1 = Unit, 2 = Aircraft, 3 = Building.
	std::vector<LoadBarTypeClass*> g_defaults[4];
	std::unordered_map<std::string, std::vector<LoadBarTypeClass*>> g_assignments;
	std::unordered_map<std::string, LoadBarTypeClass*> g_aeAssignments;

	std::unordered_map<TechnoTypeClass*, const std::vector<LoadBarTypeClass*>*> g_typeCache;
	std::unordered_map<AttachEffectTypeClass*, LoadBarTypeClass*> g_aeCache;

	std::vector<LoadBarDrawRequest> g_requests;

	bool g_enabled = true;
	int g_maxPerFrame = 256;
	bool g_loaded = false;
	RulesClass* g_loadedForRules = nullptr;

	int CategoryIndex(AbstractType whatAmI)
	{
		switch (whatAmI)
		{
		case AbstractType::Infantry: return 0;
		case AbstractType::Unit: return 1;
		case AbstractType::Aircraft: return 2;
		case AbstractType::Building: return 3;
		default: return -1;
		}
	}

	LoadBarTypeClass* FindBar(const std::string& name)
	{
		auto const it = g_typesByName.find(name);
		return it == g_typesByName.end() ? nullptr : it->second;
	}

	void AppendBarList(const std::vector<std::string>& names, std::vector<LoadBarTypeClass*>& out)
	{
		for (auto const& name : names)
		{
			if (name.empty())
				continue;

			if (auto const pBar = FindBar(name))
				out.push_back(pBar);
			else
				Debug::Log("[LoadBar] Unknown bar type '%s' referenced by configuration.\n", name.c_str());
		}
	}

	void ResetConfig()
	{
		g_typesByName.clear();
		g_assignments.clear();
		g_aeAssignments.clear();
		g_typeCache.clear();
		g_aeCache.clear();

		for (auto& list : g_defaults)
			list.clear();

		g_requests.clear();
		g_types.clear();

		g_enabled = true;
		g_maxPerFrame = 256;
		g_loaded = false;
	}

	void LoadConfig(CCINIClass* pINI)
	{
		ResetConfig();

		if (!pINI)
			return;

		constexpr const char* typeSection = "LoadBarTypes";
		const int typeCount = pINI->GetKeyCount(typeSection);

		for (int i = 0; i < typeCount; ++i)
		{
			const char* pKey = pINI->GetKeyName(typeSection, i);

			if (!pINI->ReadString(typeSection, pKey, "", Phobos::readBuffer))
				continue;

			const std::string name = Phobos::readBuffer;

			if (name.empty() || g_typesByName.contains(name))
				continue;

			auto pType = std::make_unique<LoadBarTypeClass>(name.c_str());
			pType->LoadFromINI(pINI, name.c_str());

			if (pType->Source == LoadBarSource::None)
				Debug::Log("[LoadBar] Bar type '%s' has no valid Source and will never appear.\n", name.c_str());

			g_typesByName.emplace(name, pType.get());
			g_types.push_back(std::move(pType));
		}

		g_enabled = pINI->ReadBool("LoadBar", "Enable", true);
		g_maxPerFrame = std::max(1, pINI->ReadInteger("LoadBar", "MaxPerFrame", 256));

		INI_EX exINI(pINI);
		static const char* defaultKeys[4] =
		{
			"Infantry.Default",
			"Vehicles.Default",
			"Aircraft.Default",
			"Buildings.Default"
		};

		for (int i = 0; i < 4; ++i)
		{
			std::vector<std::string> names;

			if (exINI.ParseStringList(names, "LoadBar", defaultKeys[i]))
				AppendBarList(names, g_defaults[i]);
		}

		constexpr const char* assignmentSection = "LoadBarAssignments";
		const int assignmentCount = pINI->GetKeyCount(assignmentSection);

		for (int i = 0; i < assignmentCount; ++i)
		{
			const char* pKey = pINI->GetKeyName(assignmentSection, i);
			std::vector<std::string> names;

			if (!exINI.ParseStringList(names, assignmentSection, pKey))
				continue;

			auto& list = g_assignments[pKey];
			AppendBarList(names, list);
		}

		constexpr const char* aeSection = "LoadBarAttachEffects";
		const int aeCount = pINI->GetKeyCount(aeSection);

		for (int i = 0; i < aeCount; ++i)
		{
			const char* pKey = pINI->GetKeyName(aeSection, i);
			std::vector<std::string> names;

			if (!exINI.ParseStringList(names, aeSection, pKey) || names.empty())
				continue;

			if (auto const pBar = FindBar(names[0]))
				g_aeAssignments[pKey] = pBar;
			else
				Debug::Log("[LoadBar] Unknown bar type '%s' for AttachEffect '%s'.\n", names[0].c_str(), pKey);
		}

		Debug::Log("[LoadBar] Loaded %d bar types, %d assignments, %d AttachEffect mappings.\n",
			static_cast<int>(g_types.size()), static_cast<int>(g_assignments.size()),
			static_cast<int>(g_aeAssignments.size()));

		g_loaded = true;
	}

	const std::vector<LoadBarTypeClass*>* GetBarsFor(TechnoClass* pThis)
	{
		auto const pType = pThis->GetTechnoType();

		if (!pType)
			return nullptr;

		auto const cached = g_typeCache.find(pType);

		if (cached != g_typeCache.end())
			return cached->second;

		const std::vector<LoadBarTypeClass*>* pResult = nullptr;
		auto const assigned = g_assignments.find(pType->ID);

		if (assigned != g_assignments.end())
		{
			pResult = &assigned->second;
		}
		else
		{
			const int category = CategoryIndex(pThis->WhatAmI());

			if (category >= 0)
				pResult = &g_defaults[category];
		}

		g_typeCache.emplace(pType, pResult);
		return pResult;
	}

	LoadBarTypeClass* GetAETypeBar(AttachEffectTypeClass* pType)
	{
		if (!pType)
			return nullptr;

		auto const cached = g_aeCache.find(pType);

		if (cached != g_aeCache.end())
			return cached->second;

		LoadBarTypeClass* pResult = nullptr;
		const char* pName = pType->Name;
		auto const assigned = g_aeAssignments.find(pName);

		if (assigned != g_aeAssignments.end())
			pResult = assigned->second;

		g_aeCache.emplace(pType, pResult);
		return pResult;
	}

	bool Intersects(const RectangleStruct& a, const RectangleStruct& b)
	{
		return a.X < b.X + b.Width
			&& a.X + a.Width > b.X
			&& a.Y < b.Y + b.Height
			&& a.Y + a.Height > b.Y;
	}

	// Actual world-to-window scale for the current zoom level. Uses the same
	// source/destination rectangles as TacticalToScreen so the bar geometry and
	// its anchor stay visually consistent at every zoom step.
	double GetZoomScale()
	{
		if (!ZoomManager::IsZoomed())
			return 1.0;

		RectangleStruct srcRect { 0, 0, 0, 0 };
		RectangleStruct dstRect { 0, 0, 0, 0 };
		ZoomManager::GetBlitRects(srcRect, dstRect);

		if (srcRect.Width <= 0 || srcRect.Height <= 0 || dstRect.Width <= 0 || dstRect.Height <= 0)
			return 1.0;

		const double scaleX = static_cast<double>(dstRect.Width) / static_cast<double>(srcRect.Width);
		const double scaleY = static_cast<double>(dstRect.Height) / static_cast<double>(srcRect.Height);

		return std::max(0.01, (scaleX + scaleY) * 0.5);
	}

	int ScaleValue(int value, double scale)
	{
		return static_cast<int>(std::lround(static_cast<double>(value) * scale));
	}

	RectangleStruct MakeRect(TechnoClass* pThis, TechnoExt::ExtData* pExt, const LoadBarTypeClass& type, double scale)
	{
		Point2D anchor = pThis->WhatAmI() == AbstractType::Building
			? TechnoExt::GetBuildingSelectBracketPosition(pThis, type.AnchorBuilding)
			: TechnoExt::GetFootSelectBracketPosition(pThis, type.AnchorType);

		anchor.Y += pThis->GetTechnoType()->PixelSelectionBracketDelta;

		// GetFoot/GetBuildingSelectBracketPosition return tactical client space
		// (CoordsToScreen - TacticalPos). LoadBar draws on the window composite, so
		// map to screen space. ZoomManager keeps its viewport cache at the enlarged
		// zoom-out size (R), which is why the anchor drifts without this mapping.
		if (ZoomManager::IsZoomed())
		{
			anchor = ZoomManager::TacticalToScreen(anchor);
		}
		else
		{
			// TacticalToScreen early-outs at 1.0x; add the viewport origin manually.
			anchor.X += DSurface::ViewBounds.X;
			anchor.Y += DSurface::ViewBounds.Y;
		}

		// Screen-space UI offsets, scaled with the world so they keep the same
		// visual distance from the anchor at every zoom level.
		if (pExt->Shield && pExt->Shield->IsAvailable() && type.OffsetShieldDelta.isset())
		{
			Point2D shieldDelta = type.OffsetShieldDelta.Get();
			shieldDelta.X = ScaleValue(shieldDelta.X, scale);
			shieldDelta.Y = ScaleValue(shieldDelta.Y, scale);
			anchor += shieldDelta;
		}

		Point2D offset = type.Offset.Get();
		offset.X = ScaleValue(offset.X, scale);
		offset.Y = ScaleValue(offset.Y, scale);
		anchor += offset;

		const int width = std::max(1, ScaleValue(type.Width.Get(), scale));
		const int height = std::max(1, ScaleValue(type.Height.Get(), scale));
		int x = anchor.X;
		int y = anchor.Y;

		switch (type.AlignHorizontal.Get())
		{
		case TextAlign::Center:
			x -= width / 2;
			break;
		case TextAlign::Right:
			x -= width;
			break;
		default:
			break;
		}

		switch (type.AlignVertical.Get())
		{
		case VerticalPosition::Center:
			y -= height / 2;
			break;
		case VerticalPosition::Bottom:
			y -= height;
			break;
		default:
			break;
		}

		return RectangleStruct { x, y, width, height };
	}

	void AddRequest(TechnoClass* pThis, TechnoExt::ExtData* pExt, const LoadBarTypeClass* pType,
		const LoadBarEntry& entry, double scale, int& stackOffset, int& count)
	{
		if (!pType || count >= g_maxPerFrame)
			return;

		const double fraction = pType->Fill == LoadBarFillMode::Elapsed
			? 1.0 - entry.Remaining
			: entry.Remaining;

		if (fraction <= 0.0 && pType->HideWhenEmpty)
			return;

		if (fraction >= 1.0 && pType->HideWhenFull)
			return;

		RectangleStruct rect = MakeRect(pThis, pExt, *pType, scale);

		if (pType->Stack)
		{
			const bool vertical = pType->StackDirection == LoadBarStackDirection::Up
				|| pType->StackDirection == LoadBarStackDirection::Down;

			switch (pType->StackDirection)
			{
			case LoadBarStackDirection::Down:
				rect.Y += stackOffset;
				break;
			case LoadBarStackDirection::Left:
				rect.X -= stackOffset;
				break;
			case LoadBarStackDirection::Right:
				rect.X += stackOffset;
				break;
			default:
				rect.Y -= stackOffset;
				break;
			}

			const int spacing = std::max(0, ScaleValue(pType->StackSpacing.Get(), scale));
			const int step = std::max(1, (vertical ? rect.Height : rect.Width) + spacing);
			stackOffset += step;
		}

		if (!Intersects(rect, DSurface::ViewBounds))
			return;

		g_requests.push_back(LoadBarDrawRequest { rect, entry, pType, fraction, scale, pType->Priority.Get() });
		++count;
	}

	void CollectBar(TechnoClass* pThis, TechnoExt::ExtData* pExt, const LoadBarTypeClass* pType,
		int& stackOffset, int& count)
	{
		if (!pType || !pType->CanShow(pThis))
			return;

		LoadBarEntry entries[8] {};
		const int written = LoadBarValue::Get(pThis, *pType, entries, 8);

		if (written <= 0)
			return;

		const double scale = pType->ScaleWithZoom ? GetZoomScale() : 1.0;

		for (int i = 0; i < written; ++i)
			AddRequest(pThis, pExt, pType, entries[i], scale, stackOffset, count);
	}

	void CollectTechno(TechnoClass* pThis, int& count)
	{
		if (!pThis || !pThis->GetTechnoType() || !pThis->IsAlive || pThis->InLimbo || !pThis->IsOnMap || pThis->Health <= 0)
			return;

		auto const pExt = TechnoExt::ExtMap.Find(pThis);

		if (!pExt)
			return;

		auto const pBars = GetBarsFor(pThis);
		const bool hasTechnoBars = pBars && !pBars->empty();
		const bool hasAEBars = !pExt->AttachedEffects.empty() && !g_aeAssignments.empty();

		if (!hasTechnoBars && !hasAEBars)
			return;

		if (!HouseClass::IsCurrentPlayerObserver() && !pThis->IsClearlyVisibleTo(HouseClass::CurrentPlayer))
			return;

		if (pThis->CloakState == CloakState::Cloaked || pThis->CloakState == CloakState::Cloaking)
		{
			if (!HouseClass::IsCurrentPlayerObserver()
				&& pThis->Owner != HouseClass::CurrentPlayer
				&& !pThis->Owner->IsAlliedWith(HouseClass::CurrentPlayer))
			{
				return;
			}
		}

		if (!HouseClass::IsCurrentPlayerObserver() && MapClass::Instance.IsLocationShrouded(pThis->GetCoords()))
			return;

		int stackOffset = 0;

		if (hasTechnoBars)
		{
			for (auto const pType : *pBars)
				CollectBar(pThis, pExt, pType, stackOffset, count);
		}

		// AttachEffect-owned bars. Each active AE instance gets its own bar.
		int aeIndex = 0;

		for (auto const& pAE : pExt->AttachedEffects)
		{
			++aeIndex;

			if (!pAE || !pAE->IsActive())
				continue;

			auto const pAEtype = pAE->GetType();
			auto const pType = GetAETypeBar(pAEtype);

			if (!pType || !pType->CanShow(pThis))
				continue;

			const int total = LoadBarValue::GetAETotal(pAE.get(), pThis);
			const int remaining = std::max(0, pAE->GetRemainingDuration());

			if (total <= 0 && pType->HideWhenIndefinite)
				continue;

			const double fraction = total > 0
				? std::clamp(static_cast<double>(remaining) / static_cast<double>(total), 0.0, 1.0)
				: 1.0;

			const double scale = pType->ScaleWithZoom ? GetZoomScale() : 1.0;
			const LoadBarEntry entry { fraction, remaining, total, pAEtype->Name, aeIndex };
			AddRequest(pThis, pExt, pType, entry, scale, stackOffset, count);
		}
	}
}

void LoadBarManager::DrawAll()
{
	if (!DSurface::Composite || !TacticalClass::Instance || !HouseClass::CurrentPlayer)
		return;

	if (!ScenarioClass::Instance || !ScenarioClass::Instance->FileName[0])
		return;

	if (!g_loaded || g_loadedForRules != RulesClass::Instance)
	{
		if (!CCINIClass::INI_Rules || !RulesClass::Instance)
			return;

		LoadConfig(CCINIClass::INI_Rules);
		g_loadedForRules = RulesClass::Instance;
	}

	if (!g_enabled)
		return;

	g_requests.clear();

	int count = 0;

	for (auto const pTechno : TechnoClass::Array)
	{
		if (count >= g_maxPerFrame)
			break;

		CollectTechno(pTechno, count);
	}

	std::stable_sort(g_requests.begin(), g_requests.end(),
		[](const LoadBarDrawRequest& a, const LoadBarDrawRequest& b)
		{
			if (a.Priority != b.Priority)
				return a.Priority < b.Priority;

			return a.Rect.Y < b.Rect.Y;
		});

	for (auto const& request : g_requests)
		request.Type->Draw(request.Rect, request.Entry, request.Fraction, request.Scale);

	g_requests.clear();
}
