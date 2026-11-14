#include "UnitStatusPanel.h"
#include "UnitPortrait.h"

#include <Phobos.h>

#include <BuildingTypeClass.h>
#include <CCINIClass.h>
#include <Drawing.h>
#include <GadgetClass.h>
#include <GScreenClass.h>
#include <GameStrings.h>
#include <HouseClass.h>
#include <Memory.h>
#include <PCX.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <StringTable.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Unsorted.h>
#include <WWMouseClass.h>

#include <Ext/Techno/Body.h>
#include <Ext/Sidebar/SWSidebar/SWSidebarClass.h>

#include <New/Entity/AbilityClass.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr int ReferenceWidth = 420;
	constexpr int ReferenceHeight = 176;
	constexpr int NarrowContentWidth = 326;
	constexpr int WideContentWidth = 392;

	// Internal layout, expressed in reference pixels (420x176).
	namespace Ref
	{
		constexpr int TitleX = 14;
		constexpr int TitleY = 8;
		constexpr int TitleW = 326;
		constexpr int TitleH = 18;

		constexpr int HealthX = 14;
		constexpr int HealthY = 30;
		constexpr int HealthW = 326;
		constexpr int HealthH = 12;

		constexpr int PortraitX = 14;
		constexpr int PortraitY = 46;
		constexpr int PortraitSize = 94;

		constexpr int InfoX = 116;
		constexpr int InfoY = 46;
		constexpr int InfoW = 232;
		constexpr int InfoH = 94;

		constexpr int InfoPadX = 6;
		constexpr int InfoPadY = 4;
		constexpr int InfoColumnGap = 8;
		constexpr int InfoRowHeight = 11;
		constexpr int InfoRows = 6;
		constexpr int InfoColumns = 2;
		constexpr int InfoMaxFields = InfoRows * InfoColumns;

		constexpr int BottomX = 14;
		constexpr int BottomY = 144;
		constexpr int BottomW = 326;
		constexpr int BottomH = 24;
		constexpr int BottomMaxFields = 4;

		constexpr int SkillSlotX = 356;
		constexpr int SkillSlotY = 22;
		constexpr int SkillSlotW = 40;
		constexpr int SkillSlotH = 40;
		constexpr int SkillSlotGap = 6;
		constexpr int SkillSlotCount = 3;
	}

	struct PanelMetrics
	{
		RectangleStruct Rect;
		double Scale;
		int ContentWidth = NarrowContentWidth;
		int InfoX = Ref::InfoX;
		int InfoWidth = 224;

		int X(int value) const { return this->Rect.X + static_cast<int>(std::lround(value * this->Scale)); }
		int Y(int value) const { return this->Rect.Y + static_cast<int>(std::lround(value * this->Scale)); }
		int W(int value) const { return static_cast<int>(std::lround(value * this->Scale)); }
		int H(int value) const { return static_cast<int>(std::lround(value * this->Scale)); }
	};

	struct SelectionInfo
	{
		std::vector<TechnoClass*> Units;
		TechnoClass* Representative = nullptr;
		bool IsMulti = false;
		bool AllSameType = false;
	};

	TechnoClass* GetSelectedTechno(int& selectedCount);
	SelectionInfo GetSelectionInfo();
	int GetPortraitTargetSize(TechnoClass* pThis, int size);

	bool IsEnemySelection(TechnoClass* pThis)
	{
		if (!pThis || !pThis->Owner || !HouseClass::CurrentPlayer)
			return false;

		return !pThis->Owner->IsControlledByCurrentPlayer()
			&& !pThis->Owner->IsAlliedWith(HouseClass::CurrentPlayer);
	}

	int GetVisibleSkillCount(TechnoClass* pThis)
	{
		if (!pThis)
			return 0;

		auto const pExt = TechnoExt::ExtMap.Find(pThis);

		if (!pExt)
			return 0;

		return std::min(Ref::SkillSlotCount, static_cast<int>(pExt->Abilities.size()));
	}

	bool HasAbilities(TechnoClass* pThis)
	{
		return GetVisibleSkillCount(pThis) > 0;
	}

	RectangleStruct GetSkillSlotRect(const RectangleStruct& panel, int index)
	{
		const PanelMetrics m { panel, panel.Width / static_cast<double>(ReferenceWidth) };
		const int y = Ref::SkillSlotY + index * (Ref::SkillSlotH + Ref::SkillSlotGap);

		return { m.X(Ref::SkillSlotX), m.Y(y), m.W(Ref::SkillSlotW), m.H(Ref::SkillSlotH) };
	}

	COLORREF Color(int r, int g, int b)
	{
		return Drawing::RGB_To_Int(r, g, b);
	}

	RectangleStruct Inset(RectangleStruct rect, int amount)
	{
		rect.X += amount;
		rect.Y += amount;
		rect.Width = std::max(0, rect.Width - amount * 2);
		rect.Height = std::max(0, rect.Height - amount * 2);
		return rect;
	}

	// The superweapon sidebar is anchored to the left edge by default and can
	// grow down over the unit panel when there are many superweapons. Return its
	// occupied rectangle so the panel can be moved out of its way.
	RectangleStruct GetSWSidebarRect()
	{
		if (!SWSidebarClass::IsEnabled())
			return { 0, 0, 0, 0 };

		int left = INT_MAX;
		int top = INT_MAX;
		int right = INT_MIN;
		int bottom = INT_MIN;
		bool found = false;

		auto include = [&](GadgetClass* pGadget)
		{
			if (!pGadget || pGadget->Width <= 0 || pGadget->Height <= 0)
				return;

			left = std::min(left, pGadget->X);
			top = std::min(top, pGadget->Y);
			right = std::max(right, pGadget->X + pGadget->Width);
			bottom = std::max(bottom, pGadget->Y + pGadget->Height);
			found = true;
		};

		for (auto* pColumn : SWSidebarClass::Instance.Columns)
		{
			if (!pColumn)
				continue;

			include(pColumn);

			for (auto* pButton : pColumn->Buttons)
				include(pButton);
		}

		include(SWSidebarClass::Instance.ToggleButton);

		if (!found)
			return { 0, 0, 0, 0 };

		return { left, top, right - left, bottom - top };
	}

	void Fill(DSurface* pSurface, RectangleStruct rect, int r, int g, int b)
	{
		if (rect.Width > 0 && rect.Height > 0)
			pSurface->FillRect(&rect, Color(r, g, b));
	}

	void FillTrans(DSurface* pSurface, RectangleStruct rect, int r, int g, int b, int opacity)
	{
		if (rect.Width > 0 && rect.Height > 0)
		{
			ColorStruct color { static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b) };
			pSurface->FillRectTrans(&rect, &color, opacity);
		}
	}

	void Border(DSurface* pSurface, RectangleStruct rect, int r, int g, int b)
	{
		if (rect.Width > 0 && rect.Height > 0)
			pSurface->DrawRect(&rect, Color(r, g, b));
	}

	void HLine(DSurface* pSurface, int x, int y, int width, int r, int g, int b)
	{
		Fill(pSurface, { x, y, width, 1 }, r, g, b);
	}

	void VLine(DSurface* pSurface, int x, int y, int height, int r, int g, int b)
	{
		Fill(pSurface, { x, y, 1, height }, r, g, b);
	}

	bool IsChineseLanguage()
	{
		static int cached = -1;

		if (cached < 0)
		{
			cached = 0;
			const wchar_t* text = StringTable::LoadString(GameStrings::TXT_CANCEL);

			if (text)
			{
				for (const wchar_t* p = text; *p; ++p)
				{
					if (*p >= 0x4E00 && *p <= 0x9FFF)
					{
						cached = 1;
						break;
					}
				}
			}
		}

		return cached == 1;
	}

	const wchar_t* Localized(const char* key, const wchar_t* english, const wchar_t* chinese)
	{
		return StringTable::TryFetchString(key, IsChineseLanguage() ? chinese : english);
	}

	// ---------------------------------------------------------------------
	// Info field model
	// ---------------------------------------------------------------------

	enum class InfoType
	{
		None = 0,
		Health,
		Shield,
		Ammo,
		MindControl,
		Spawns,
		Passengers,
		Tiberium,
		Experience,
		Occupants,
		GattlingStage,
		ROF,
		Reload,
		SpawnTimer,
		GattlingTimer,
		ProduceCash,
		PassengerKill,
		AutoDeath,
		SuperWeapon,
		IronCurtain,
		TemporalLife,
		FactoryProcess,
		Speed,
		Armor,
		Cost,
		Power
	};

	InfoType ParseInfoType(const std::string& value)
	{
		static const std::pair<const char*, InfoType> names[] = {
			{ "Health", InfoType::Health },
			{ "Shield", InfoType::Shield },
			{ "Ammo", InfoType::Ammo },
			{ "MindControl", InfoType::MindControl },
			{ "Spawns", InfoType::Spawns },
			{ "Passengers", InfoType::Passengers },
			{ "Tiberium", InfoType::Tiberium },
			{ "Experience", InfoType::Experience },
			{ "Occupants", InfoType::Occupants },
			{ "GattlingStage", InfoType::GattlingStage },
			{ "ROF", InfoType::ROF },
			{ "Reload", InfoType::Reload },
			{ "SpawnTimer", InfoType::SpawnTimer },
			{ "GattlingTimer", InfoType::GattlingTimer },
			{ "ProduceCash", InfoType::ProduceCash },
			{ "PassengerKill", InfoType::PassengerKill },
			{ "AutoDeath", InfoType::AutoDeath },
			{ "SuperWeapon", InfoType::SuperWeapon },
			{ "IronCurtain", InfoType::IronCurtain },
			{ "TemporalLife", InfoType::TemporalLife },
			{ "FactoryProcess", InfoType::FactoryProcess },
			{ "Speed", InfoType::Speed },
			{ "Armor", InfoType::Armor },
			{ "Cost", InfoType::Cost },
			{ "Power", InfoType::Power }
		};

		for (const auto& entry : names)
		{
			if (_stricmp(value.c_str(), entry.first) == 0)
				return entry.second;
		}

		return InfoType::None;
	}

	bool ToDisplayInfoType(InfoType type, DisplayInfoType& out)
	{
		switch (type)
		{
		case InfoType::Health: out = DisplayInfoType::Health; return true;
		case InfoType::Shield: out = DisplayInfoType::Shield; return true;
		case InfoType::Ammo: out = DisplayInfoType::Ammo; return true;
		case InfoType::MindControl: out = DisplayInfoType::MindControl; return true;
		case InfoType::Spawns: out = DisplayInfoType::Spawns; return true;
		case InfoType::Passengers: out = DisplayInfoType::Passengers; return true;
		case InfoType::Tiberium: out = DisplayInfoType::Tiberium; return true;
		case InfoType::Experience: out = DisplayInfoType::Experience; return true;
		case InfoType::Occupants: out = DisplayInfoType::Occupants; return true;
		case InfoType::GattlingStage: out = DisplayInfoType::GattlingStage; return true;
		case InfoType::ROF: out = DisplayInfoType::ROF; return true;
		case InfoType::Reload: out = DisplayInfoType::Reload; return true;
		case InfoType::SpawnTimer: out = DisplayInfoType::SpawnTimer; return true;
		case InfoType::GattlingTimer: out = DisplayInfoType::GattlingTimer; return true;
		case InfoType::ProduceCash: out = DisplayInfoType::ProduceCash; return true;
		case InfoType::PassengerKill: out = DisplayInfoType::PassengerKill; return true;
		case InfoType::AutoDeath: out = DisplayInfoType::AutoDeath; return true;
		case InfoType::SuperWeapon: out = DisplayInfoType::SuperWeapon; return true;
		case InfoType::IronCurtain: out = DisplayInfoType::IronCurtain; return true;
		case InfoType::TemporalLife: out = DisplayInfoType::TemporalLife; return true;
		case InfoType::FactoryProcess: out = DisplayInfoType::FactoryProcess; return true;
		default: return false;
		}
	}

	bool IsTimerInfo(InfoType type)
	{
		switch (type)
		{
		case InfoType::ROF:
		case InfoType::Reload:
		case InfoType::SpawnTimer:
		case InfoType::GattlingTimer:
		case InfoType::ProduceCash:
		case InfoType::PassengerKill:
		case InfoType::AutoDeath:
		case InfoType::SuperWeapon:
		case InfoType::IronCurtain:
		case InfoType::TemporalLife:
		case InfoType::FactoryProcess:
			return true;
		default:
			return false;
		}
	}

	const wchar_t* InfoLabel(InfoType type)
	{
		switch (type)
		{
		case InfoType::Health: return Localized("TXT_UNIT_STATUS_HEALTH", L"HP", L"生命");
		case InfoType::Shield: return Localized("TXT_UNIT_STATUS_SHIELD", L"SH", L"护盾");
		case InfoType::Ammo: return Localized("TXT_UNIT_STATUS_AMMO", L"AMMO", L"弹药");
		case InfoType::MindControl: return Localized("TXT_UNIT_STATUS_MINDCONTROL", L"MC", L"控制");
		case InfoType::Spawns: return Localized("TXT_UNIT_STATUS_SPAWNS", L"SPAWN", L"子机");
		case InfoType::Passengers: return Localized("TXT_UNIT_STATUS_PASSENGERS", L"PASS", L"乘客");
		case InfoType::Tiberium: return Localized("TXT_UNIT_STATUS_TIBERIUM", L"TIB", L"矿");
		case InfoType::Experience: return Localized("TXT_UNIT_STATUS_EXPERIENCE", L"EXP", L"经验");
		case InfoType::Occupants: return Localized("TXT_UNIT_STATUS_OCCUPANTS", L"OCC", L"占领");
		case InfoType::GattlingStage: return Localized("TXT_UNIT_STATUS_GATTLINGSTAGE", L"GATT", L"加特林");
		case InfoType::ROF: return Localized("TXT_UNIT_STATUS_ROF", L"ROF", L"射速");
		case InfoType::Reload: return Localized("TXT_UNIT_STATUS_RELOAD", L"RELOAD", L"装填");
		case InfoType::SpawnTimer: return Localized("TXT_UNIT_STATUS_SPAWNTIMER", L"SPT", L"子机计时");
		case InfoType::GattlingTimer: return Localized("TXT_UNIT_STATUS_GATTLINGTIMER", L"GTT", L"加特林计时");
		case InfoType::ProduceCash: return Localized("TXT_UNIT_STATUS_PRODUCECASH", L"CASH", L"产钱");
		case InfoType::PassengerKill: return Localized("TXT_UNIT_STATUS_PASSENGERKILL", L"PKILL", L"乘客删除");
		case InfoType::AutoDeath: return Localized("TXT_UNIT_STATUS_AUTODEATH", L"AUTO", L"自毁");
		case InfoType::SuperWeapon: return Localized("TXT_UNIT_STATUS_SUPERWEAPON", L"SW", L"超武");
		case InfoType::IronCurtain: return Localized("TXT_UNIT_STATUS_IRONCURTAIN", L"IC", L"铁幕");
		case InfoType::TemporalLife: return Localized("TXT_UNIT_STATUS_TEMPORALLIFE", L"TIME", L"时间");
		case InfoType::FactoryProcess: return Localized("TXT_UNIT_STATUS_FACTORYPROCESS", L"FACT", L"工厂");
		case InfoType::Speed: return Localized("TXT_UNIT_STATUS_SPEED", L"SPD", L"速度");
		case InfoType::Armor: return Localized("TXT_UNIT_STATUS_ARMOR", L"ARM", L"装甲");
		case InfoType::Cost: return Localized("TXT_UNIT_STATUS_COST", L"COST", L"造价");
		case InfoType::Power: return Localized("TXT_UNIT_STATUS_POWER", L"PWR", L"电力");
		default: return L"";
		}
	}

	const wchar_t* ArmorLabel(Armor armor)
	{
		switch (armor)
		{
		case Armor::None: return Localized("TXT_UNIT_STATUS_ARMOR_NONE", L"None", L"无");
		case Armor::Flak: return Localized("TXT_UNIT_STATUS_ARMOR_FLAK", L"Flak", L"防空");
		case Armor::Plate: return Localized("TXT_UNIT_STATUS_ARMOR_PLATE", L"Plate", L"装甲");
		case Armor::Light: return Localized("TXT_UNIT_STATUS_ARMOR_LIGHT", L"Light", L"轻");
		case Armor::Medium: return Localized("TXT_UNIT_STATUS_ARMOR_MEDIUM", L"Medium", L"中");
		case Armor::Heavy: return Localized("TXT_UNIT_STATUS_ARMOR_HEAVY", L"Heavy", L"重");
		case Armor::Wood: return Localized("TXT_UNIT_STATUS_ARMOR_WOOD", L"Wood", L"木");
		case Armor::Steel: return Localized("TXT_UNIT_STATUS_ARMOR_STEEL", L"Steel", L"钢");
		case Armor::Concrete: return Localized("TXT_UNIT_STATUS_ARMOR_CONCRETE", L"Concrete", L"混凝土");
		case Armor::Special_1: return Localized("TXT_UNIT_STATUS_ARMOR_SPECIAL1", L"Special", L"特殊");
		case Armor::Special_2: return Localized("TXT_UNIT_STATUS_ARMOR_SPECIAL2", L"Special", L"特殊");
		default: return Localized("TXT_UNIT_STATUS_ARMOR_NONE", L"None", L"无");
		}
	}

	struct InfoField
	{
		InfoType Type;
		std::wstring Label;
		std::wstring Value;
	};

	bool BuildInfoField(TechnoClass* pThis, InfoType type, InfoField& out)
	{
		const auto pType = pThis->GetTechnoType();

		out.Type = type;
		out.Label = InfoLabel(type);
		out.Value.clear();

		if (out.Label.empty())
			return false;

		switch (type)
		{
		case InfoType::Speed:
		{
			// The INI Speed value is converted by ReadSpeed to leptons/frame:
			// internal = min(rulesSpeed, 100) * 256 / 100. Invert it for display
			// so the panel shows the value the modder wrote in the rules INI.
			const int rulesSpeed = (pType->Speed * 100 + 128) / 256;

			wchar_t buffer[32] {};
			swprintf(buffer, std::size(buffer), L"%d", rulesSpeed);
			out.Value = buffer;
			return true;
		}
		case InfoType::Armor:
		{
			out.Value = ArmorLabel(pType->Armor);
			return !out.Value.empty();
		}
		case InfoType::Cost:
		{
			wchar_t buffer[32] {};
			swprintf(buffer, std::size(buffer), L"$%d", pType->GetCost());
			out.Value = buffer;
			return true;
		}
		case InfoType::Power:
		{
			if (pThis->WhatAmI() != AbstractType::Building)
				return false;

			const auto pBuildingType = static_cast<BuildingTypeClass*>(pType);
			const int net = pBuildingType->PowerBonus - pBuildingType->PowerDrain;

			wchar_t buffer[32] {};
			swprintf(buffer, std::size(buffer), L"%+d", net);
			out.Value = buffer;
			return true;
		}
		case InfoType::Experience:
		{
			const int cost = pType->GetCost();
			const double ratio = RulesClass::Instance->VeteranRatio;

			if (cost <= 0 || ratio <= 0.0)
				return false;

			const double levelExp = ratio * cost;
			const float veterancy = std::clamp(pThis->Veterancy.Veterancy, 0.0f, 2.0f);
			const double next = (veterancy < 1.0f ? 1.0 : 2.0) * levelExp;
			const double current = std::min(static_cast<double>(veterancy), 2.0) * levelExp;

			const int currentValue = static_cast<int>(std::lround(current));
			const int nextValue = static_cast<int>(std::lround(next));

			if (nextValue <= 0)
				return false;

			wchar_t buffer[64] {};
			swprintf(buffer, std::size(buffer), L"%d/%d", currentValue, nextValue);
			out.Value = buffer;
			return true;
		}
		default:
			break;
		}

		DisplayInfoType displayType {};

		if (!ToDisplayInfoType(type, displayType))
			return false;

		int value = -1;
		int maxValue = 0;
		TechnoExt::GetValuesForDisplay(pThis, displayType, value, maxValue, 0);

		if (value < 0)
			return false;

		wchar_t buffer[64] {};

		if (IsTimerInfo(type))
		{
			if (maxValue > 0)
				swprintf(buffer, std::size(buffer), L"%d/%d", value, maxValue);
			else if (value == 0)
				out.Value = Localized("TXT_UNIT_STATUS_READY", L"Ready", L"就绪");
			else
				swprintf(buffer, std::size(buffer), L"%d", value);
		}
		else
		{
			if (maxValue <= 0)
				return false;

			swprintf(buffer, std::size(buffer), L"%d/%d", value, maxValue);
		}

		if (out.Value.empty())
			out.Value = buffer;

		return !out.Value.empty();
	}

	std::vector<InfoField> BuildInfoFields(TechnoClass* pThis, const std::vector<InfoType>& types, size_t maxFields)
	{
		std::vector<InfoField> result;

		if (!pThis)
			return result;

		result.reserve(std::min(types.size(), maxFields));

		for (InfoType type : types)
		{
			if (result.size() >= maxFields)
				break;

			InfoField field;

			if (BuildInfoField(pThis, type, field))
				result.push_back(std::move(field));
		}

		return result;
	}

	// ---------------------------------------------------------------------
	// Config: field lists come from rules [General]
	// ---------------------------------------------------------------------

	std::vector<InfoType> CenterInfoTypes;
	std::vector<InfoType> BottomInfoTypes;
	bool InfoConfigLoaded = false;

	struct TypeInfoConfig
	{
		std::vector<InfoType> Center;
		std::vector<InfoType> Bottom;
	};

	std::unordered_map<TechnoTypeClass*, TypeInfoConfig> TypeInfoCache;

	std::string TrimToken(const std::string& token)
	{
		size_t first = token.find_first_not_of(" \t\r\n");

		if (first == std::string::npos)
			return {};

		size_t last = token.find_last_not_of(" \t\r\n");
		return token.substr(first, last - first + 1);
	}

	void ParseInfoList(const char* text, std::vector<InfoType>& out, size_t maxFields)
	{
		out.clear();

		if (!text)
			return;

		const char* p = text;

		while (*p && out.size() < maxFields)
		{
			const char* comma = strchr(p, ',');
			const size_t length = comma ? static_cast<size_t>(comma - p) : strlen(p);

			std::string token = TrimToken(std::string(p, length));

			if (!token.empty())
			{
				const auto type = ParseInfoType(token);

				if (type == InfoType::None)
					Debug::Log("[UnitStatusPanel] Unknown info type '%s'\n", token.c_str());
				else
					out.push_back(type);
			}

			if (!comma)
				break;

			p = comma + 1;
		}
	}

	void LoadInfoConfig()
	{
		if (InfoConfigLoaded)
			return;

		InfoConfigLoaded = true;

		auto load = [](const char* key, const char* fallback, std::vector<InfoType>& out, size_t maxFields)
		{
			char buffer[2048] {};

			if (CCINIClass::INI_Rules)
				CCINIClass::INI_Rules->ReadString(GameStrings::General, key, fallback, buffer);
			else
				strcpy_s(buffer, fallback);

			ParseInfoList(buffer, out, maxFields);

			if (out.empty())
				ParseInfoList(fallback, out, maxFields);
		};

		load("UnitStatusPanel.CenterInfo", "Ammo,Passengers,Experience",
			CenterInfoTypes, Ref::InfoMaxFields);
		load("UnitStatusPanel.BottomInfo", "Health,Speed,Armor",
			BottomInfoTypes, Ref::BottomMaxFields);
	}

	const TypeInfoConfig& GetTypeInfoConfig(TechnoTypeClass* pType)
	{
		static const TypeInfoConfig emptyConfig {};

		if (!pType)
			return emptyConfig;

		auto it = TypeInfoCache.find(pType);

		if (it != TypeInfoCache.end())
			return it->second;

		TypeInfoConfig config { CenterInfoTypes, BottomInfoTypes };

		if (CCINIClass::INI_Rules)
		{
			auto readOverride = [pType](const char* key, std::vector<InfoType>& out,
				const std::vector<InfoType>& fallback, size_t maxFields)
			{
				char buffer[2048] {};

				// "<none>" means the key is absent -> inherit the global list.
				// An explicitly empty value means "show no fields for this unit".
				CCINIClass::INI_Rules->ReadString(pType->ID, key, NONE_STR, buffer);

				if (_stricmp(buffer, NONE_STR) == 0)
				{
					out = fallback;
					return;
				}

				ParseInfoList(buffer, out, maxFields);
			};

			readOverride("UnitStatusPanel.CenterInfo", config.Center,
				CenterInfoTypes, Ref::InfoMaxFields);
			readOverride("UnitStatusPanel.BottomInfo", config.Bottom,
				BottomInfoTypes, Ref::BottomMaxFields);
		}

		auto result = TypeInfoCache.emplace(pType, std::move(config));
		return result.first->second;
	}

	// ---------------------------------------------------------------------
	// PCX assets
	// ---------------------------------------------------------------------

	struct PanelAsset
	{
		std::string Path;
		BSurface* Surface = nullptr;
		BSurface* Scaled = nullptr;
		int ScaledWidth = 0;
		int ScaledHeight = 0;
		int ScaledSlice = -1;
		bool ScaledFailed = false;
		bool Loaded = false;

		void ReleaseScaled()
		{
			if (this->Scaled)
			{
				GameDelete(this->Scaled);
				this->Scaled = nullptr;
			}

			this->ScaledWidth = 0;
			this->ScaledHeight = 0;
			this->ScaledSlice = -1;
			this->ScaledFailed = false;
		}

		void Reset()
		{
			this->ReleaseScaled();
			this->Path.clear();
			this->Surface = nullptr;
			this->Loaded = false;
		}

		void Load(const std::string& path)
		{
			if (this->Loaded && this->Path == path)
				return;

			this->ReleaseScaled();
			this->Loaded = true;
			this->Path = path;
			this->Surface = nullptr;

			if (path.empty())
				return;

			if (PCX::Instance.LoadFile(path.c_str()))
				this->Surface = PCX::Instance.GetSurface(path.c_str());

			if (!this->Surface)
				Debug::Log("[UnitStatusPanel] PCX not loaded: %s\n", path.c_str());
		}
	};

	PanelAsset BackgroundAsset;
	PanelAsset PortraitAsset;
	PanelAsset PortraitFrameAsset;
	PanelAsset InfoFrameAsset;
	PanelAsset BottomFrameAsset;
	PanelAsset SkillSlotAsset;
	PanelAsset SkillIconAsset;

	void EnsureAssets()
	{
		BackgroundAsset.Load(Phobos::Config::UnitStatusPanel_Background);
		PortraitAsset.Load(Phobos::Config::UnitStatusPanel_Portrait);
		PortraitFrameAsset.Load(Phobos::Config::UnitStatusPanel_PortraitFrame);
		InfoFrameAsset.Load(Phobos::Config::UnitStatusPanel_InfoFrame);
		BottomFrameAsset.Load(Phobos::Config::UnitStatusPanel_BottomFrame);
		SkillSlotAsset.Load(Phobos::Config::UnitStatusPanel_SkillSlot);
		SkillIconAsset.Load(Phobos::Config::UnitStatusPanel_SkillIcon);
	}

	bool BlitAsset(DSurface* pSurface, const PanelAsset& asset, const RectangleStruct& rect, bool center)
	{
		if (!asset.Surface || rect.Width <= 0 || rect.Height <= 0)
			return false;

		RectangleStruct target = rect;

		if (center && asset.Surface->Width <= rect.Width && asset.Surface->Height <= rect.Height)
		{
			target.X += (rect.Width - asset.Surface->Width) / 2;
			target.Y += (rect.Height - asset.Surface->Height) / 2;
			target.Width = asset.Surface->Width;
			target.Height = asset.Surface->Height;
		}
		else
		{
			target.Width = std::min(rect.Width, static_cast<int>(asset.Surface->Width));
			target.Height = std::min(rect.Height, static_cast<int>(asset.Surface->Height));
		}

		PCX::Instance.BlitToSurface(&target, pSurface, asset.Surface);
		return true;
	}

	// PCX::BlitToSurface draws a PCX at its native size. Ability icons are
	// commonly 60x48 cameo-sized PCXs while the skill slot is much smaller, so
	// blitting them directly makes the icon spill out of the slot. Scale to fit
	// and center the result inside the slot instead.
	void BlitIconFit(DSurface* pSurface, const RectangleStruct& rect, BSurface* pIcon)
	{
		if (!pSurface || !pIcon || rect.Width <= 0 || rect.Height <= 0)
			return;

		const int sourceWidth = pIcon->Width;
		const int sourceHeight = pIcon->Height;

		if (sourceWidth <= 0 || sourceHeight <= 0)
			return;

		if (sourceWidth <= rect.Width && sourceHeight <= rect.Height)
		{
			RectangleStruct target = rect;
			target.X += (rect.Width - sourceWidth) / 2;
			target.Y += (rect.Height - sourceHeight) / 2;
			target.Width = sourceWidth;
			target.Height = sourceHeight;
			PCX::Instance.BlitToSurface(&target, pSurface, pIcon);
			return;
		}

		const double scale = std::min(
			rect.Width / static_cast<double>(sourceWidth),
			rect.Height / static_cast<double>(sourceHeight));

		const int drawWidth = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
		const int drawHeight = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));
		const int offsetX = rect.X + (rect.Width - drawWidth) / 2;
		const int offsetY = rect.Y + (rect.Height - drawHeight) / 2;

		for (int y = 0; y < drawHeight; ++y)
		{
			const int sourceY = std::min(sourceHeight - 1, y * sourceHeight / drawHeight);

			for (int x = 0; x < drawWidth; ++x)
			{
				const int sourceX = std::min(sourceWidth - 1, x * sourceWidth / drawWidth);

				Point2D sourcePoint { sourceX, sourceY };
				const COLORREF color = pIcon->GetPixel(&sourcePoint);

				if (color == COLOR_PURPLE)
					continue;

				Point2D targetPoint { offsetX + x, offsetY + y };
				pSurface->SetPixel(&targetPoint, color);
			}
		}
	}

	// Maps one destination coordinate through a 9-slice frame. The four
	// corners stay at their native pixel size, the edges are stretched along
	// one axis, and the centre is stretched in both directions.
	int MapSliceCoordinate(int value, int destinationSize, int sourceSize, int slice)
	{
		if (destinationSize <= 0 || sourceSize <= 0)
			return 0;

		slice = std::clamp(slice, 0, std::min(sourceSize, destinationSize) / 2);

		if (slice <= 0)
			return std::clamp(value * sourceSize / std::max(1, destinationSize), 0, sourceSize - 1);

		if (value < slice)
			return std::min(value, sourceSize - 1);

		if (value >= destinationSize - slice)
			return std::max(0, sourceSize - (destinationSize - value));

		const int destinationMiddle = destinationSize - slice * 2;
		const int sourceMiddle = sourceSize - slice * 2;

		if (destinationMiddle <= 0 || sourceMiddle <= 0)
			return std::clamp(sourceSize / 2, 0, sourceSize - 1);

		return slice + (value - slice) * sourceMiddle / destinationMiddle;
	}

	bool EnsureScaled(PanelAsset& asset, int width, int height, int slice)
	{
		if (!asset.Surface || width <= 0 || height <= 0)
			return false;

		if (asset.Scaled
			&& asset.ScaledWidth == width
			&& asset.ScaledHeight == height
			&& asset.ScaledSlice == slice)
		{
			return true;
		}

		if (asset.ScaledFailed
			&& asset.ScaledWidth == width
			&& asset.ScaledHeight == height
			&& asset.ScaledSlice == slice)
		{
			return false;
		}

		asset.ReleaseScaled();

		auto* pScaled = GameCreate<BSurface>(width, height);

		if (!pScaled)
			return false;

		RectangleStruct fill { 0, 0, width, height };
		pScaled->FillRect(&fill, COLOR_PURPLE);

		const int sourceWidth = asset.Surface->Width;
		const int sourceHeight = asset.Surface->Height;
		int written = 0;

		for (int y = 0; y < height; ++y)
		{
			const int sourceY = MapSliceCoordinate(y, height, sourceHeight, slice);

			for (int x = 0; x < width; ++x)
			{
				const int sourceX = MapSliceCoordinate(x, width, sourceWidth, slice);

				Point2D sourcePoint { sourceX, sourceY };
				const COLORREF color = asset.Surface->GetPixel(&sourcePoint);

				if (color == COLOR_PURPLE)
					continue;

				Point2D targetPoint { x, y };
				pScaled->SetPixel(&targetPoint, color);
				++written;
			}
		}

		if (written <= 0)
		{
			GameDelete(pScaled);
			asset.ScaledWidth = width;
			asset.ScaledHeight = height;
			asset.ScaledSlice = slice;
			asset.ScaledFailed = true;
			return false;
		}

		asset.Scaled = pScaled;
		asset.ScaledWidth = width;
		asset.ScaledHeight = height;
		asset.ScaledSlice = slice;
		asset.ScaledFailed = false;
		return true;
	}

	bool BlitSliced(DSurface* pSurface, PanelAsset& asset, const RectangleStruct& rect, int slice)
	{
		if (!asset.Surface || rect.Width <= 0 || rect.Height <= 0)
			return false;

		// At the reference resolution the art is already pixel-perfect.
		if (asset.Surface->Width == rect.Width && asset.Surface->Height == rect.Height)
		{
			RectangleStruct target = rect;
			PCX::Instance.BlitToSurface(&target, pSurface, asset.Surface);
			return true;
		}

		if (!EnsureScaled(asset, rect.Width, rect.Height, slice))
			return false;

		RectangleStruct target = rect;
		PCX::Instance.BlitToSurface(&target, pSurface, asset.Scaled);
		return true;
	}

	// ---------------------------------------------------------------------
	// Drawing helpers
	// ---------------------------------------------------------------------

	void DrawTextCentered(DSurface* pSurface, const wchar_t* text, int centerX, int top,
		COLORREF color, TextPrintType flags)
	{
		if (!text || !*text)
			return;

		RectangleStruct bounds = pSurface->GetRect();
		Point2D location { centerX, top };
		pSurface->DrawText(text, &bounds, &location, color, 0, flags);
	}

	void DrawTextAt(DSurface* pSurface, const wchar_t* text, int x, int top,
		COLORREF color, TextPrintType flags)
	{
		if (!text || !*text)
			return;

		RectangleStruct bounds = pSurface->GetRect();
		Point2D location { x, top };
		pSurface->DrawText(text, &bounds, &location, color, 0, flags);
	}

	void DrawTextRight(DSurface* pSurface, const wchar_t* text, int rightX, int top,
		COLORREF color, TextPrintType flags)
	{
		if (!text || !*text)
			return;

		auto wanted = Drawing::GetTextDimensions(text, { 0, 0 }, 0, 2, 0);
		DrawTextAt(pSurface, text, rightX - wanted.Width, top, color, flags);
	}

	void DrawInset(DSurface* pSurface, const RectangleStruct& rect)
	{
		FillTrans(pSurface, rect, 0, 0, 0, 55);
		Border(pSurface, rect, 150, 156, 162);

		const auto inner = Inset(rect, 2);
		Border(pSurface, inner, 50, 56, 62);

		HLine(pSurface, rect.X + 1, rect.Y + 1, rect.Width - 2, 6, 8, 10);
		VLine(pSurface, rect.X + 1, rect.Y + 1, rect.Height - 2, 6, 8, 10);
		HLine(pSurface, rect.X + 1, rect.Y + rect.Height - 2, rect.Width - 2, 96, 102, 108);
	}

	void DrawFallbackFrame(DSurface* pSurface, const RectangleStruct& outer)
	{
		Border(pSurface, outer, 150, 156, 162);
		Border(pSurface, Inset(outer, 1), 30, 34, 38);

		HLine(pSurface, outer.X + 2, outer.Y + 2, outer.Width - 4, 108, 114, 120);
		VLine(pSurface, outer.X + 2, outer.Y + 2, outer.Height - 4, 96, 102, 108);
	}

	void DrawShell(DSurface* pSurface, const PanelMetrics& m)
	{
		const auto& outer = m.Rect;

		const bool hasFrame = BackgroundAsset.Surface != nullptr;

		const RectangleStruct base = hasFrame ? Inset(outer, m.W(4)) : outer;
		FillTrans(pSurface, base, 28, 32, 36, Phobos::Config::UnitStatusPanel_Opacity);

		if (!hasFrame || !BlitSliced(pSurface, BackgroundAsset, outer,
			Phobos::Config::UnitStatusPanel_SliceSize))
		{
			DrawFallbackFrame(pSurface, outer);
		}
	}

	void DrawTitle(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis, int selectedCount)
	{
		std::wstring title;

		if (!pThis)
		{
			title = Localized("TXT_UNIT_STATUS_NO_SELECTION", L"No unit selected", L"未选择单位");
		}
		else if (selectedCount > 1)
		{
			wchar_t buffer[96] {};
			const auto format = Localized("TXT_UNIT_STATUS_MULTI_SELECTION",
				L"%d units selected", L"已选择 %d 个单位");
			swprintf(buffer, std::size(buffer), format, selectedCount);
			title = buffer;
		}
		else
		{
			const auto pType = pThis->GetTechnoType();

			if (pType->UIName && *pType->UIName)
			{
				title = pType->UIName;
			}
			else
			{
				wchar_t name[128] {};
				MultiByteToWideChar(CP_ACP, 0, pType->Name, -1, name, static_cast<int>(std::size(name)));
				title = name;
			}
		}

		const bool chinese = IsChineseLanguage();
		const auto titleFlags = (chinese ? TextPrintType::Point8 : TextPrintType::Metal12)
			| TextPrintType::Center | TextPrintType::FullShadow;

		DrawTextCentered(pSurface, title.c_str(),
			m.X(Ref::TitleX + m.ContentWidth / 2), m.Y(Ref::TitleY),
			Color(220, 226, 232), titleFlags);
	}

	void DrawHealthBar(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis)
	{
		const RectangleStruct bar {
			m.X(Ref::HealthX), m.Y(Ref::HealthY),
			m.W(m.ContentWidth), m.H(Ref::HealthH)
		};

		FillTrans(pSurface, bar, 0, 0, 0, 60);

		if (pThis)
		{
			const auto pType = pThis->GetTechnoType();

			if (pType->Strength > 0)
			{
				double fraction = static_cast<double>(pThis->Health) / pType->Strength;
				fraction = std::clamp(fraction, 0.0, 1.0);

				int r = 64, g = 180, b = 96;

				if (fraction <= RulesClass::Instance->ConditionRed)
				{
					r = 200; g = 60; b = 50;
				}
				else if (fraction <= RulesClass::Instance->ConditionYellow)
				{
					r = 220; g = 190; b = 60;
				}

				const int fillWidth = static_cast<int>(std::lround((bar.Width - 4) * fraction));

				if (fillWidth > 0)
				{
					RectangleStruct fill { bar.X + 2, bar.Y + 2, fillWidth, bar.Height - 4 };
					Fill(pSurface, fill, r, g, b);
				}
			}
		}

		Border(pSurface, bar, 140, 146, 152);
		Border(pSurface, Inset(bar, 2), 42, 46, 50);
		HLine(pSurface, bar.X + 3, bar.Y + 3, bar.Width - 6, 12, 14, 16);

		constexpr int segments = 4;
		for (int i = 1; i < segments; ++i)
		{
			const int x = bar.X + bar.Width * i / segments;
			VLine(pSurface, x, bar.Y + 2, bar.Height - 4, 24, 28, 32);
		}
	}

	constexpr COLORREF PortraitNoTint = RGB(255, 255, 255);

	// 0 = normal, 1 = yellow, 2 = red. Uses the engine's own health-bar
	// thresholds so the portrait changes colour exactly when the bar does.
	unsigned char GetHealthTintState(TechnoClass* pThis)
	{
		if (!pThis || !Phobos::Config::UnitStatusPanel_HealthTint)
			return 0;

		switch (pThis->GetHealthStatus())
		{
		case HealthState::Yellow:
			return 1;
		case HealthState::Red:
			return 2;
		default:
			return 0;
		}
	}

	COLORREF GetHealthTintColor(unsigned char state)
	{
		switch (state)
		{
		case 1:
			return Phobos::Config::UnitStatusPanel_HealthTintYellow;
		case 2:
			return Phobos::Config::UnitStatusPanel_HealthTintRed;
		default:
			return PortraitNoTint;
		}
	}

	void DrawPortraitImage(Surface* pSurface, const PortraitImage* pModel,
		const RectangleStruct& destination, COLORREF tint = PortraitNoTint)
	{
		if (!pSurface || !pModel || pModel->Pixels.empty()
			|| destination.Width <= 0 || destination.Height <= 0)
		{
			return;
		}

		const bool tinted = tint != PortraitNoTint;
		BYTE tintR = 255;
		BYTE tintG = 255;
		BYTE tintB = 255;
		BYTE tintLutR[256] {};
		BYTE tintLutG[256] {};
		BYTE tintLutB[256] {};

		if (tinted)
		{
			Drawing::Int_To_RGB(tint, tintR, tintG, tintB);

			for (int i = 0; i < 256; ++i)
			{
				tintLutR[i] = static_cast<BYTE>(i * tintR / 255);
				tintLutG[i] = static_cast<BYTE>(i * tintG / 255);
				tintLutB[i] = static_cast<BYTE>(i * tintB / 255);
			}
		}

		const bool useMask = pModel->Mask.size() == pModel->Pixels.size();
		const bool useShadow = pModel->Shadow.size() == pModel->Pixels.size();
		const int baseX = destination.X + (destination.Width - pModel->Width) / 2;
		const int baseY = destination.Y + (destination.Height - pModel->Height) / 2;

		const RectangleStruct content = pModel->ContentBounds.Width > 0
			&& pModel->ContentBounds.Height > 0
			? pModel->ContentBounds
			: RectangleStruct { 0, 0, pModel->Width, pModel->Height };

		const int startX = std::clamp(content.X, 0, pModel->Width);
		const int startY = std::clamp(content.Y, 0, pModel->Height);
		const int endX = std::clamp(content.X + content.Width, startX, pModel->Width);
		const int endY = std::clamp(content.Y + content.Height, startY, pModel->Height);

		if (useShadow)
		{
			for (int y = startY; y < endY; ++y)
			{
				for (int x = startX; x < endX; ++x)
				{
					const size_t index = static_cast<size_t>(y) * pModel->Width + x;

					if (index >= pModel->Shadow.size())
						continue;

					const BYTE alpha = pModel->Shadow[index];

					if (alpha == 0)
						continue;

					Point2D targetPoint { baseX + x, baseY + y };
					const COLORREF background = pSurface->GetPixel(&targetPoint);
					BYTE r = 0, g = 0, b = 0;
					Drawing::Int_To_RGB(background, r, g, b);

					const int inverse = 255 - static_cast<int>(alpha);
					r = static_cast<BYTE>(r * inverse / 255);
					g = static_cast<BYTE>(g * inverse / 255);
					b = static_cast<BYTE>(b * inverse / 255);

					pSurface->SetPixel(&targetPoint, Drawing::RGB_To_Int(r, g, b));
				}
			}
		}

		for (int y = startY; y < endY; ++y)
		{
			for (int x = startX; x < endX; ++x)
			{
				const size_t index = static_cast<size_t>(y) * pModel->Width + x;

				if (index >= pModel->Pixels.size())
					continue;

				if (useMask)
				{
					if (!pModel->Mask[index])
						continue;
				}
				else if (pModel->Pixels[index] == 0)
				{
					continue;
				}

				Point2D targetPoint { baseX + x, baseY + y };
				COLORREF color = pModel->Pixels[index];

				if (tinted)
				{
					BYTE gray = 0;
					BYTE grayG = 0;
					BYTE grayB = 0;
					Drawing::Int_To_RGB(color, gray, grayG, grayB);
					color = Drawing::RGB_To_Int(
						tintLutR[gray], tintLutG[gray], tintLutB[gray]);
				}

				pSurface->SetPixel(&targetPoint, color);
			}
		}
	}

	void DrawPortrait(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis)
	{
		// Keep the portrait frame explicitly square. A single size is used for
		// both axes even if the panel is scaled by a non-integer factor.
		const int portraitSize = m.W(Ref::PortraitSize);
		const RectangleStruct portrait {
			m.X(Ref::PortraitX), m.Y(Ref::PortraitY),
			portraitSize, portraitSize
		};

		if (!BlitSliced(pSurface, PortraitFrameAsset, portrait,
			Phobos::Config::UnitStatusPanel_SliceSize))
		{
			DrawInset(pSurface, portrait);
		}

		const auto inner = Inset(portrait, m.W(4));

		if (BlitAsset(pSurface, PortraitAsset, inner, true))
			return;

		if (!pThis)
			return;

		const int modelSize = GetPortraitTargetSize(pThis, inner.Width);
		const PortraitImage* pModel = UnitPortrait::Get(pThis, modelSize, modelSize);

		if (!pModel || pModel->Pixels.empty())
			return;

		static bool loggedPortraitSize = false;

		if (!loggedPortraitSize)
		{
			loggedPortraitSize = true;
			Debug::Log("[UnitStatusPanel] portrait frame=%dx%d inner=%dx%d model=%dx%d\n",
				portrait.Width, portrait.Height,
				inner.Width, inner.Height,
				pModel->Width, pModel->Height);
		}

		DrawPortraitImage(pSurface, pModel, inner,
			GetHealthTintColor(GetHealthTintState(pThis)));
	}

	int GetPortraitTargetSize(TechnoClass* pThis, int size)
	{
		if (!pThis || size <= 0 || pThis->WhatAmI() != AbstractType::Building)
			return size;

		const int percent = std::clamp(
			Phobos::Config::UnitStatusPanel_BuildingScale, 1, 100);
		return std::max(8, size * percent / 100);
	}

	RectangleStruct GetPortraitContentBounds(const PortraitImage* pModel)
	{
		if (!pModel || pModel->Pixels.empty())
			return { 0, 0, 1, 1 };

		if (pModel->ContentBounds.Width > 0 && pModel->ContentBounds.Height > 0)
			return pModel->ContentBounds;

		const int width = pModel->Width;
		const int height = pModel->Height;
		const bool useMask = pModel->Mask.size() == pModel->Pixels.size();

		int minX = width;
		int minY = height;
		int maxX = -1;
		int maxY = -1;

		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const int index = y * width + x;
				const bool opaque = useMask
					? pModel->Mask[index] != 0
					: pModel->Pixels[index] != 0;

				if (!opaque)
					continue;

				minX = std::min(minX, x);
				minY = std::min(minY, y);
				maxX = std::max(maxX, x);
				maxY = std::max(maxY, y);
			}
		}

		if (maxX < minX || maxY < minY)
			return { 0, 0, std::max(1, width / 4), std::max(1, height / 4) };

		return { minX, minY, maxX - minX + 1, maxY - minY + 1 };
	}

	BSurface* MultiPortraitCache = nullptr;
	RectangleStruct MultiPortraitCacheRect {};
	std::vector<TechnoTypeClass*> MultiPortraitCacheTypes;
	std::vector<unsigned char> MultiPortraitCacheElites;
	std::vector<unsigned char> MultiPortraitCacheStates;

	void ClearMultiPortraitCache()
	{
		if (MultiPortraitCache)
		{
			GameDelete(MultiPortraitCache);
			MultiPortraitCache = nullptr;
		}

		MultiPortraitCacheRect = { 0, 0, 0, 0 };
		MultiPortraitCacheTypes.clear();
		MultiPortraitCacheElites.clear();
		MultiPortraitCacheStates.clear();
	}

	struct MultiPortraitItem
	{
		TechnoClass* Unit = nullptr;
		const PortraitImage* Model = nullptr;
		RectangleStruct Content { 0, 0, 1, 1 };
		int X = 0;
		int Y = 0;
	};

	struct MultiPortraitPending
	{
		bool Active = false;
		BSurface* Surface = nullptr;
		RectangleStruct Rect {};
		std::vector<TechnoTypeClass*> Types;
		std::vector<unsigned char> Elites;
		std::vector<unsigned char> States;
		std::vector<TechnoClass*> Units;
		std::vector<MultiPortraitItem> Items;
		int GatherIndex = 0;
		int ComposeIndex = 0;
		int RenderSize = 64;
		bool LaidOut = false;
	};

	MultiPortraitPending MultiPortraitPendingState;

	void ClearPendingMultiPortrait()
	{
		if (MultiPortraitPendingState.Surface)
		{
			GameDelete(MultiPortraitPendingState.Surface);
			MultiPortraitPendingState.Surface = nullptr;
		}

		MultiPortraitPendingState = {};
	}

	struct MultiPortraitGrid
	{
		int Rows = 1;
		int Columns = 1;
		int CellWidth = 1;
		int CellHeight = 1;
		int CellSize = 1;
	};

	MultiPortraitGrid ChooseMultiPortraitGrid(int count, const RectangleStruct& inner)
	{
		MultiPortraitGrid best;

		if (count <= 0 || inner.Width <= 0 || inner.Height <= 0)
			return best;

		for (int rows = 1; rows <= count; ++rows)
		{
			const int columns = (count + rows - 1) / rows;
			const int cellWidth = inner.Width / columns;
			const int cellHeight = inner.Height / rows;
			const int cellSize = std::min(cellWidth, cellHeight);

			if (cellSize > best.CellSize)
			{
				best.Rows = rows;
				best.Columns = columns;
				best.CellWidth = cellWidth;
				best.CellHeight = cellHeight;
				best.CellSize = cellSize;
			}
		}

		return best;
	}

	int EstimateMultiPortraitRenderSize(int count, const RectangleStruct& inner)
	{
		const auto grid = ChooseMultiPortraitGrid(count, inner);
		const int size = std::clamp(grid.CellSize, 8, 86);

		// Quantise so selection count changes do not needlessly invalidate all
		// cached thumbnails.
		return std::max(8, size / 4 * 4);
	}

	bool LayoutMultiPortraitItems(std::vector<MultiPortraitItem>& items,
		const RectangleStruct& inner, int& contentHeight, int& maxContentWidth)
	{
		contentHeight = 0;
		maxContentWidth = 0;

		if (items.empty() || inner.Width <= 0 || inner.Height <= 0)
			return false;

		const auto grid = ChooseMultiPortraitGrid(
			static_cast<int>(items.size()), inner);

		if (grid.CellSize <= 0)
			return false;

		for (int row = 0; row < grid.Rows; ++row)
		{
			const int first = row * grid.Columns;
			const int last = std::min(
				first + grid.Columns, static_cast<int>(items.size()));

			if (first >= last)
				break;

			int rowWidth = 0;

			for (int i = first; i < last; ++i)
			{
				const int currentGap = std::max(0,
					grid.CellWidth - items[i].Content.Width);
				const int padding = std::clamp(currentGap / 3, 2, 12);
				rowWidth += items[i].Content.Width + padding;
			}

			int cursorX = inner.X + (inner.Width - rowWidth) / 2;
			const int cellY = inner.Y + row * grid.CellHeight;

			for (int i = first; i < last; ++i)
			{
				const int currentGap = std::max(0,
					grid.CellWidth - items[i].Content.Width);
				const int padding = std::clamp(currentGap / 3, 2, 12);

				items[i].X = cursorX;
				items[i].Y = cellY
					+ (grid.CellHeight - items[i].Content.Height) / 2;
				cursorX += items[i].Content.Width + padding;
			}
		}

		contentHeight = inner.Height;
		maxContentWidth = inner.Width;
		return true;
	}

	void DrawMultiPortraitGrid(DSurface* pSurface, const PanelMetrics& m,
		const SelectionInfo& selection)
	{
		if (selection.Units.empty())
			return;

		const int areaX = m.X(Ref::PortraitX);
		const int areaY = m.Y(Ref::TitleY + Ref::TitleH + 4);
		const int areaBottom = m.Y(ReferenceHeight - 8);
		const int areaWidth = m.W(m.ContentWidth);
		const int areaHeight = std::max(1, areaBottom - areaY);
		const RectangleStruct area { areaX, areaY, areaWidth, areaHeight };
		const auto inner = Inset(area, m.W(4));

		const bool areaMatches = MultiPortraitCache
			&& MultiPortraitCacheRect.X == area.X
			&& MultiPortraitCacheRect.Y == area.Y
			&& MultiPortraitCacheRect.Width == area.Width
			&& MultiPortraitCacheRect.Height == area.Height;

		size_t visibleCount = selection.Units.size();
		const int limit = Phobos::Config::UnitStatusPanel_MultiPortraitLimit;

		if (limit > 0)
			visibleCount = std::min(visibleCount, static_cast<size_t>(limit));

		std::vector<TechnoClass*> visibleUnits(
			selection.Units.begin(),
			selection.Units.begin() + static_cast<std::ptrdiff_t>(visibleCount));

		std::vector<TechnoTypeClass*> cacheTypes;
		std::vector<unsigned char> cacheElites;
		std::vector<unsigned char> cacheStates;
		cacheTypes.reserve(visibleUnits.size());
		cacheElites.reserve(visibleUnits.size());
		cacheStates.reserve(visibleUnits.size());

		for (auto const pUnit : visibleUnits)
		{
			cacheTypes.emplace_back(pUnit->GetTechnoType());
			cacheElites.emplace_back(static_cast<unsigned char>(pUnit->Veterancy.IsElite() ? 1 : 0));
			cacheStates.emplace_back(GetHealthTintState(pUnit));
		}

		auto drawFrame = [&]()
		{
			if (!BlitSliced(pSurface, PortraitFrameAsset, area,
				Phobos::Config::UnitStatusPanel_SliceSize))
			{
				DrawInset(pSurface, area);
			}
		};

		auto drawDisplayedCache = [&]()
		{
			if (areaMatches)
			{
				RectangleStruct target = area;
				PCX::Instance.BlitToSurface(&target, pSurface, MultiPortraitCache);
			}
			else
			{
				drawFrame();
			}
		};

		const bool cacheMatches = areaMatches
			&& MultiPortraitCacheTypes == cacheTypes
			&& MultiPortraitCacheElites == cacheElites
			&& MultiPortraitCacheStates == cacheStates;

		if (cacheMatches)
		{
			if (MultiPortraitPendingState.Active)
				ClearPendingMultiPortrait();

			drawDisplayedCache();
			return;
		}

		const bool pendingMatches = MultiPortraitPendingState.Active
			&& MultiPortraitPendingState.Rect.X == area.X
			&& MultiPortraitPendingState.Rect.Y == area.Y
			&& MultiPortraitPendingState.Rect.Width == area.Width
			&& MultiPortraitPendingState.Rect.Height == area.Height
			&& MultiPortraitPendingState.Types == cacheTypes
			&& MultiPortraitPendingState.Elites == cacheElites
			&& MultiPortraitPendingState.States == cacheStates;

		if (!pendingMatches)
		{
			ClearPendingMultiPortrait();

			auto& pending = MultiPortraitPendingState;
			pending.Active = true;
			pending.Rect = area;
			pending.Types = cacheTypes;
			pending.Elites = cacheElites;
			pending.States = cacheStates;
			pending.Units = visibleUnits;
			pending.Items.resize(visibleUnits.size());
			pending.GatherIndex = 0;
			pending.ComposeIndex = 0;
			pending.RenderSize = EstimateMultiPortraitRenderSize(
				static_cast<int>(visibleUnits.size()), inner);
			pending.LaidOut = false;

			// Fill every already-cached thumbnail immediately. Only model types
			// that have never been rendered need to wait for later frames.
			for (size_t i = 0; i < pending.Items.size(); ++i)
			{
				auto& item = pending.Items[i];
				item.Unit = pending.Units[i];
				const int modelSize = GetPortraitTargetSize(
					item.Unit, pending.RenderSize);
				item.Model = UnitPortrait::TryGetCached(
					item.Unit, modelSize, modelSize);

				if (item.Model && !item.Model->Pixels.empty())
					item.Content = GetPortraitContentBounds(item.Model);
			}

			// The old cache cannot be shown here: this frame's clean panel
			// background is what gets copied into the pending surface.
			drawFrame();
			pending.Surface = GameCreate<BSurface>(area.Width, area.Height);

			if (!pending.Surface)
			{
				pending.Active = false;
				return;
			}

			for (int y = 0; y < area.Height; ++y)
			{
				for (int x = 0; x < area.Width; ++x)
				{
					Point2D source { area.X + x, area.Y + y };
					Point2D target { x, y };
					pending.Surface->SetPixel(&target, pSurface->GetPixel(&source));
				}
			}

			drawDisplayedCache();
			return;
		}

		auto& pending = MultiPortraitPendingState;

		if (pending.GatherIndex < static_cast<int>(pending.Items.size()))
		{
			int rendered = 0;

			for (int i = pending.GatherIndex; i < static_cast<int>(pending.Items.size()); ++i)
			{
				auto& item = pending.Items[i];

				if (!item.Model)
				{
					const int modelSize = GetPortraitTargetSize(
						item.Unit, pending.RenderSize);
					item.Model = UnitPortrait::Get(
						item.Unit, modelSize, modelSize);

					if (item.Model && !item.Model->Pixels.empty())
						item.Content = GetPortraitContentBounds(item.Model);

					++rendered;
				}

				pending.GatherIndex = i + 1;

				if (rendered >= 2)
					break;
			}

			drawDisplayedCache();
			return;
		}

		if (!pending.LaidOut)
		{
			int contentHeight = 0;
			int maxContentWidth = 0;
			LayoutMultiPortraitItems(
				pending.Items, inner, contentHeight, maxContentWidth);

			if (contentHeight > inner.Height || maxContentWidth > inner.Width)
			{
				const double heightScale = contentHeight > 0
					? static_cast<double>(inner.Height) / contentHeight
					: 1.0;
				const double widthScale = maxContentWidth > 0
					? static_cast<double>(inner.Width) / maxContentWidth
					: 1.0;
				const double scale = std::min({ heightScale, widthScale, 1.0 });
				pending.RenderSize = std::max(8, static_cast<int>(
					pending.RenderSize * scale * 0.95));
				pending.GatherIndex = 0;

				for (auto& item : pending.Items)
				{
					item.Model = nullptr;
					item.Content = { 0, 0, 1, 1 };
				}

				drawDisplayedCache();
				return;
			}

			pending.LaidOut = true;
			pending.ComposeIndex = 0;
		}

		if (pending.ComposeIndex < static_cast<int>(pending.Items.size()))
		{
			const int end = std::min(
				static_cast<int>(pending.Items.size()),
				pending.ComposeIndex + 8);

			for (int i = pending.ComposeIndex; i < end; ++i)
			{
				auto const& item = pending.Items[i];

				if (!item.Model)
					continue;

				const RectangleStruct destination {
					item.X - item.Content.X - area.X,
					item.Y - item.Content.Y - area.Y,
					item.Model->Width,
					item.Model->Height
				};
				const unsigned char tintState = i < static_cast<int>(pending.States.size())
					? pending.States[i]
					: 0;

				DrawPortraitImage(pending.Surface, item.Model, destination,
					GetHealthTintColor(tintState));
			}

			pending.ComposeIndex = end;
			drawDisplayedCache();
			return;
		}

		// Pending surface is complete. Swap it in without another full build.
		if (MultiPortraitCache)
			GameDelete(MultiPortraitCache);

		MultiPortraitCache = pending.Surface;
		MultiPortraitCacheRect = area;
		MultiPortraitCacheTypes = pending.Types;
		MultiPortraitCacheElites = pending.Elites;
		MultiPortraitCacheStates = pending.States;

		pending.Surface = nullptr;
		ClearPendingMultiPortrait();

		drawDisplayedCache();
	}

	void DrawInfoBox(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis)
	{
		const RectangleStruct info {
			m.X(m.InfoX), m.Y(Ref::InfoY),
			m.W(m.InfoWidth), m.H(Ref::InfoH)
		};

		if (!BlitSliced(pSurface, InfoFrameAsset, info,
			Phobos::Config::UnitStatusPanel_SliceSize))
		{
			DrawInset(pSurface, info);
		}

		if (!pThis)
			return;

		if (IsEnemySelection(pThis))
		{
			const auto text = Localized("TXT_UNIT_STATUS_UNKNOWN", L"Unknown", L"未知");
			const auto wanted = Drawing::GetTextDimensions(text, { 0, 0 }, 0, 2, 0);
			const int textY = info.Y + std::max(0, (info.Height - wanted.Height) / 2);

			DrawTextCentered(pSurface, text, info.X + info.Width / 2, textY,
				Color(220, 226, 232),
				TextPrintType::Point8 | TextPrintType::FullShadow | TextPrintType::Center);
			return;
		}

		const auto& typeConfig = GetTypeInfoConfig(pThis->GetTechnoType());
		const auto fields = BuildInfoFields(pThis, typeConfig.Center, Ref::InfoMaxFields);

		if (fields.empty())
			return;

		const int innerWidth = info.Width - m.W(Ref::InfoPadX * 2);
		const int columnWidth = (innerWidth - m.W(Ref::InfoColumnGap)) / Ref::InfoColumns;
		const int columnX[Ref::InfoColumns] = {
			info.X + m.W(Ref::InfoPadX),
			info.X + m.W(Ref::InfoPadX) + columnWidth + m.W(Ref::InfoColumnGap)
		};
		const int startY = info.Y + m.H(Ref::InfoPadY);

		const auto labelFlags = TextPrintType::Point8 | TextPrintType::FullShadow;
		const auto valueFlags = TextPrintType::Point8 | TextPrintType::FullShadow;

		for (size_t i = 0; i < fields.size(); ++i)
		{
			const int row = static_cast<int>(i) / Ref::InfoColumns;
			const int col = static_cast<int>(i) % Ref::InfoColumns;
			const int x = columnX[col];
			const int y = startY + row * m.H(Ref::InfoRowHeight);

			DrawTextAt(pSurface, fields[i].Label.c_str(), x, y, Color(160, 168, 176), labelFlags);
			DrawTextRight(pSurface, fields[i].Value.c_str(), x + columnWidth, y,
				Color(220, 226, 232), valueFlags);
		}
	}

	void DrawBottomBar(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis)
	{
		const RectangleStruct bottom {
			m.X(Ref::BottomX), m.Y(Ref::BottomY),
			m.W(m.ContentWidth), m.H(Ref::BottomH)
		};

		if (!BlitSliced(pSurface, BottomFrameAsset, bottom,
			Phobos::Config::UnitStatusPanel_SliceSize))
		{
			DrawInset(pSurface, bottom);
		}

		if (!pThis)
			return;

		const auto& typeConfig = GetTypeInfoConfig(pThis->GetTechnoType());
		const auto fields = BuildInfoFields(pThis, typeConfig.Bottom, Ref::BottomMaxFields);

		if (fields.empty())
			return;

		const int maxWidth = bottom.Width - m.W(12);
		std::wstring line;
		const auto flags = TextPrintType::Point8 | TextPrintType::FullShadow;

		for (const auto& field : fields)
		{
			std::wstring token = field.Label + L" " + field.Value;

			if (!line.empty())
				token = line + L"   |   " + token;

			auto wanted = Drawing::GetTextDimensions(token.c_str(), { 0, 0 }, 0, 2, 0);

			if (wanted.Width > maxWidth)
				break;

			line = token;
		}

		if (line.empty())
			return;

		auto wanted = Drawing::GetTextDimensions(line.c_str(), { 0, 0 }, 0, 2, 0);
		const int textX = bottom.X + (bottom.Width - wanted.Width) / 2;
		const int textY = bottom.Y + std::max(0, (bottom.Height - wanted.Height) / 2);

		DrawTextAt(pSurface, line.c_str(), textX, textY,
			Color(220, 226, 232), flags);
	}

	void DrawSkillSlot(DSurface* pSurface, const PanelMetrics& m, int index, TechnoClass* pThis)
	{
		const RectangleStruct slot = GetSkillSlotRect(m.Rect, index);

		if (slot.Width <= 0 || slot.Height <= 0)
			return;

		if (!BlitSliced(pSurface, SkillSlotAsset, slot,
			Phobos::Config::UnitStatusPanel_SliceSize))
		{
			DrawInset(pSurface, slot);
		}

		const RectangleStruct iconRect = Inset(slot, m.W(3));
		AbilityClass* pAbility = nullptr;

		if (pThis)
		{
			const auto pExt = TechnoExt::ExtMap.Find(pThis);

			if (pExt && index < static_cast<int>(pExt->Abilities.size()))
				pAbility = pExt->Abilities[index].get();
		}

		if (!pAbility || !pAbility->Type)
			return;

		const auto pIcon = pAbility->Type->Icon.GetSurface();

		if (pIcon)
		{
			BlitIconFit(pSurface, iconRect, pIcon);
		}
		else
		{
			BlitAsset(pSurface, SkillIconAsset, iconRect, true);
		}

		if (pAbility->Type->Activation == AbilityActivation::Active)
		{
			const int cooldownTotal = pAbility->Type->Cooldown;
			const int cooldownLeft = pAbility->Cooldown.GetTimeLeft();

			if (cooldownTotal > 0 && cooldownLeft > 0)
			{
				const int overlayHeight = std::max(1,
					iconRect.Height * std::min(cooldownLeft, cooldownTotal) / cooldownTotal);
				const RectangleStruct overlay {
					iconRect.X, iconRect.Y, iconRect.Width, overlayHeight
				};
				FillTrans(pSurface, overlay, 0, 0, 0, 110);
			}

			if (pAbility->Charges >= 0)
			{
				wchar_t buffer[16] {};
				swprintf(buffer, std::size(buffer), L"%d", pAbility->Charges);
				DrawTextRight(pSurface, buffer,
					iconRect.X + iconRect.Width, iconRect.Y,
					Color(230, 230, 230), TextPrintType::Point8 | TextPrintType::FullShadow);
			}

			if (pAbility->Charges == 0)
				FillTrans(pSurface, iconRect, 0, 0, 0, 80);
		}
		else
		{
			DrawTextAt(pSurface, L"P",
				iconRect.X + 1, iconRect.Y + iconRect.Height - 9,
				Color(180, 200, 255), TextPrintType::Point8 | TextPrintType::FullShadow);
		}

		const bool controllable = pThis
			&& pThis->Owner
			&& HouseClass::CurrentPlayer
			&& pThis->Owner->IsControlledByCurrentPlayer();

		if (!controllable)
			FillTrans(pSurface, slot, 0, 0, 0, 80);
	}

	void DrawSkillSlots(DSurface* pSurface, const PanelMetrics& m, TechnoClass* pThis)
	{
		const int count = GetVisibleSkillCount(pThis);

		for (int i = 0; i < count; ++i)
			DrawSkillSlot(pSurface, m, i, pThis);
	}

	SelectionInfo GetSelectionInfo()
	{
		SelectionInfo info;
		const auto& objects = ObjectClass::CurrentObjects;

		info.Units.reserve(objects.Count);

		for (int i = 0; i < objects.Count; ++i)
		{
			auto pObject = objects.GetItem(i);

			if (!pObject || pObject->InLimbo)
				continue;

			const auto whatAmI = pObject->WhatAmI();

			if (whatAmI != AbstractType::Infantry
				&& whatAmI != AbstractType::Unit
				&& whatAmI != AbstractType::Aircraft
				&& whatAmI != AbstractType::Building)
			{
				continue;
			}

			auto pTechno = static_cast<TechnoClass*>(pObject);

			if (pTechno->Health > 0)
				info.Units.emplace_back(pTechno);
		}

		if (info.Units.empty())
			return info;

		info.Representative = info.Units.back();
		info.IsMulti = info.Units.size() > 1;
		info.AllSameType = true;

		auto const pType = info.Units.front()->GetTechnoType();

		for (auto const pUnit : info.Units)
		{
			if (pUnit->GetTechnoType() != pType)
			{
				info.AllSameType = false;
				break;
			}
		}

		return info;
	}

	TechnoClass* GetSelectedTechno(int& selectedCount)
	{
		const auto info = GetSelectionInfo();
		selectedCount = static_cast<int>(info.Units.size());
		return info.Representative;
	}

	class UnitStatusPanelGadget final : public GadgetClass
	{
	public:
		UnitStatusPanelGadget(int x, int y, int width, int height)
			: GadgetClass(x, y, width, height,
				GadgetFlag::LeftPress | GadgetFlag::LeftRelease
				| GadgetFlag::RightPress | GadgetFlag::RightRelease, false)
		{ }

		virtual bool Draw(bool forced) override
		{
			return false;
		}

		virtual void OnMouseEnter() override
		{
			this->Hovering = true;
		}

		virtual void OnMouseLeave() override
		{
			this->Hovering = false;
		}

		virtual bool Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier) override
		{
			if ((flags & GadgetFlag::LeftPress) && WWMouseClass::Instance)
			{
				Point2D mouse { 0, 0 };
				WWMouseClass::Instance->GetCoords(&mouse);

				const auto panel = UnitStatusPanel::GetPanelRect();

				Debug::Log("[UnitStatusPanel] Action X=%d Y=%d flags=%X panel=(%d,%d,%d,%d)\n",
					mouse.X, mouse.Y, static_cast<unsigned>(flags),
					panel.X, panel.Y, panel.Width, panel.Height);

				if (panel.Width > 0 && panel.Height > 0)
				{
					const auto selection = GetSelectionInfo();
					auto const pSelected = selection.Representative;
					const bool skillsVisible = pSelected
						&& !IsEnemySelection(pSelected)
						&& (!selection.IsMulti || selection.AllSameType);

					if (skillsVisible)
					{
						const int skillCount = GetVisibleSkillCount(pSelected);

						for (int i = 0; i < skillCount; ++i)
						{
							const auto slot = GetSkillSlotRect(panel, i);

							if (mouse.X < slot.X || mouse.Y < slot.Y
								|| mouse.X >= slot.X + slot.Width
								|| mouse.Y >= slot.Y + slot.Height)
							{
								continue;
							}

							AbilityClass::OnSlotClicked(pSelected, i);
							break;
						}
					}
				}
			}

			// Keep the base gadget's sticky/focus bookkeeping in sync and
			// consume the click so it never reaches the battlefield underneath.
			this->GadgetClass::Action(flags, pKey, modifier);
			return true;
		}

		virtual bool Clicked(DWORD* pKey, GadgetFlag flags, int X, int Y,
			KeyModifier modifier) override
		{
			// GadgetClass::Clicked dispatches to Action(), which handles the
			// ability slots and the sticky state.
			return this->GadgetClass::Clicked(pKey, flags, X, Y, modifier);
		}

		bool Hovering = false;
	};

	UnitStatusPanelGadget* Gadget = nullptr;
}

bool UnitStatusPanel::IsEnabled()
{
	return Phobos::Config::UnitStatusPanel_Enable
		&& DSurface::Composite
		&& DSurface::ViewBounds.Width > 0
		&& DSurface::ViewBounds.Height > 0;
}

namespace
{
	bool IsInGame()
	{
		return ScenarioClass::Instance
			&& ScenarioClass::Instance->FileName[0]
			&& HouseClass::CurrentPlayer
			&& TacticalClass::Instance;
	}

	void SyncGadget(const RectangleStruct& rect)
	{
		if (!Gadget || rect.Width <= 0 || rect.Height <= 0)
			return;

		if (Gadget->X == rect.X
			&& Gadget->Y == rect.Y
			&& Gadget->Width == rect.Width
			&& Gadget->Height == rect.Height)
		{
			return;
		}

		Gadget->SetPosition(rect.X, rect.Y);
		Gadget->SetDimension(rect.Width, rect.Height);
		Gadget->Zap();
	}
}

RectangleStruct UnitStatusPanel::GetPanelRect()
{
	RectangleStruct empty { 0, 0, 0, 0 };

	if (!IsEnabled())
		return empty;

	const auto& view = DSurface::ViewBounds;
	const int screenWidth = DSurface::Composite->GetWidth();
	const int screenHeight = DSurface::Composite->GetHeight();

	if (screenWidth <= 0 || screenHeight <= 0)
		return empty;

	double scale = std::min(view.Width / 1752.0, view.Height / 1048.0);
	scale *= Phobos::Config::UnitStatusPanel_Scale;
	scale = std::clamp(scale, 0.35, 2.0);

	int width = std::max(1, static_cast<int>(std::lround(ReferenceWidth * scale)));
	int height = std::max(1, static_cast<int>(std::lround(ReferenceHeight * scale)));

	int x = view.X + Phobos::Config::UnitStatusPanel_OffsetX;
	int y = view.Y + view.Height - height + Phobos::Config::UnitStatusPanel_OffsetY;

	// Avoid the superweapon sidebar. It is left-aligned by default and can
	// extend down over the panel; the panel's gadget would otherwise swallow
	// clicks intended for the lower superweapon buttons.
	const auto swRect = GetSWSidebarRect();

	if (swRect.Width > 0 && swRect.Height > 0)
	{
		// Include the filter-button strip above the panel in the hit area.
		constexpr int filterReserve = 48;
		const int avoidTop = y - filterReserve;
		const int avoidRight = x + width;
		const int avoidBottom = y + height;

		const bool overlaps = x < swRect.X + swRect.Width
			&& avoidRight > swRect.X
			&& avoidTop < swRect.Y + swRect.Height
			&& avoidBottom > swRect.Y;

		if (overlaps)
		{
			if (swRect.X < view.X + view.Width / 2)
				x = swRect.X + swRect.Width + 4;
			else
				x = swRect.X - width - 4;
		}
	}

	// Never let the panel leave the battlefield viewport.
	x = std::clamp(x, view.X, std::max(view.X, view.X + view.Width - width));
	y = std::clamp(y, view.Y, std::max(view.Y, view.Y + view.Height - height));

	static bool logged = false;

	if (!logged)
	{
		logged = true;
		Debug::Log("[UnitStatusPanel] ViewBounds=(%d,%d,%d,%d) screen=%dx%d panel=(%d,%d,%d,%d)\n",
			view.X, view.Y, view.Width, view.Height,
			screenWidth, screenHeight,
			x, y, width, height);
	}

	return { x, y, width, height };
}

void UnitStatusPanel::InitIO()
{
	InitClear();

	if (!IsEnabled())
		return;

	EnsureAssets();
	LoadInfoConfig();

	const auto rect = GetPanelRect();

	if (rect.Width <= 0 || rect.Height <= 0)
		return;

	Gadget = GameCreate<UnitStatusPanelGadget>(rect.X, rect.Y, rect.Width, rect.Height);
	Gadget->Zap();
	GScreenClass::Instance.AddButton(Gadget);
}

void UnitStatusPanel::InitClear()
{
	InfoConfigLoaded = false;
	TypeInfoCache.clear();
	UnitPortrait::Clear();
	ClearMultiPortraitCache();
	ClearPendingMultiPortrait();
	AbilityClass::CancelTargeting();

	BackgroundAsset.Reset();
	PortraitAsset.Reset();
	PortraitFrameAsset.Reset();
	InfoFrameAsset.Reset();
	BottomFrameAsset.Reset();
	SkillSlotAsset.Reset();
	SkillIconAsset.Reset();

	if (!Gadget)
		return;

	GScreenClass::Instance.RemoveButton(Gadget);
	GameDelete(Gadget);
	Gadget = nullptr;
}

bool UnitStatusPanel::IsHovering()
{
	return Gadget
		&& Gadget->Hovering
		&& IsInGame()
		&& !Phobos::UI::CinemaMode
		&& Unsorted::SpecialDialog == 0;
}

void UnitStatusPanel::Draw()
{
	// Cancel a pending ability target selection on right click, even if the
	// panel itself is not being redrawn this frame.
	AbilityClass::PollTargeting();

	// Keep the panel visible when the ESC pause menu is open; the menu is drawn
	// on top by the engine. Click blocking is still disabled while a special
	// dialog is active (see IsHovering).
	if (!IsEnabled() || !IsInGame() || Phobos::UI::CinemaMode)
		return;

	const auto rect = GetPanelRect();

	if (rect.Width <= 0 || rect.Height <= 0)
		return;

	SyncGadget(rect);

	EnsureAssets();
	LoadInfoConfig();

	DSurface* pSurface = DSurface::Composite;

	const auto selection = GetSelectionInfo();
	const int selectedCount = static_cast<int>(selection.Units.size());
	TechnoClass* pSelected = selection.Representative;
	const bool isEnemy = IsEnemySelection(pSelected);
	const bool multi = selection.IsMulti;
	const bool showSkills = !isEnemy
		&& HasAbilities(pSelected)
		&& (!multi || selection.AllSameType);
	const int contentWidth = showSkills ? NarrowContentWidth : WideContentWidth;
	const int infoWidth = contentWidth - (Ref::PortraitSize + 8);
	const PanelMetrics metrics {
		rect,
		rect.Width / static_cast<double>(ReferenceWidth),
		contentWidth,
		Ref::InfoX,
		infoWidth
	};

	DrawShell(pSurface, metrics);
	DrawTitle(pSurface, metrics, pSelected, selectedCount);

	if (multi)
	{
		// Multi-selection shows every selected unit and hides status bars.
		// Same-type selections may still expose the ability slots.
		DrawMultiPortraitGrid(pSurface, metrics, selection);

		if (showSkills)
			DrawSkillSlots(pSurface, metrics, pSelected);

		return;
	}

	// Enemy information is still redacted in DrawInfoBox, but the health bar is
	// visible for every selected unit.
	if (pSelected)
		DrawHealthBar(pSurface, metrics, pSelected);

	DrawPortrait(pSurface, metrics, pSelected);
	DrawInfoBox(pSurface, metrics, pSelected);
	DrawBottomBar(pSurface, metrics, pSelected);

	if (showSkills)
		DrawSkillSlots(pSurface, metrics, pSelected);
}
