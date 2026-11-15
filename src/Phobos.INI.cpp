#include "Phobos.h"

#include <CCINIClass.h>
#include <Drawing.h>
#include <Utilities/CrashTrace.h>
#include <StringTable.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <MessageListClass.h>
#include <sstream>
#include <HouseClass.h>
#include <GameOptionsClass.h>

#include <Utilities/Parser.h>
#include <Utilities/GeneralUtils.h>
#include <Utilities/Patch.h>
#include <Utilities/Macro.h>

#include "Misc/BlittersFix.h"
#include "Misc/ZoomManager.h"
#include "Misc/TacticalMap.h"
#include "Misc/PerfProbe.h"
#include "Misc/PerfOverlay.h"

#include <algorithm>

#include <Commands/AutoReverseToggle.h>

bool Phobos::UI::DisableEmptySpawnPositions = false;
bool Phobos::UI::ExtendedToolTips = false;
int Phobos::UI::MaxToolTipWidth = 0;
bool Phobos::UI::HarvesterCounter_Show = false;
double Phobos::UI::HarvesterCounter_ConditionYellow = 0.99;
double Phobos::UI::HarvesterCounter_ConditionRed = 0.5;
bool Phobos::UI::ProducingProgress_Show = false;
const wchar_t* Phobos::UI::CostLabel = L"";
const wchar_t* Phobos::UI::PowerLabel = L"";
const wchar_t* Phobos::UI::PowerBlackoutLabel = L"";
const wchar_t* Phobos::UI::TimeLabel = L"";
const wchar_t* Phobos::UI::HarvesterLabel = L"";
const wchar_t* Phobos::UI::ShowBriefingResumeButtonLabel = L"";
const wchar_t* Phobos::UI::SWShotsFormat = L"";
const wchar_t* Phobos::UI::ChargeLabel = L"";
const wchar_t* Phobos::UI::BattlePoints_Label = L"";
const wchar_t* Phobos::UI::BattlePointsSidebar_Label = L"";
bool Phobos::UI::BattlePointsSidebar_Label_InvertPosition = false;
char Phobos::UI::ShowBriefingResumeButtonStatusLabel[32];
bool Phobos::UI::PowerDelta_Show = false;
double Phobos::UI::PowerDelta_ConditionYellow = 0.75;
double Phobos::UI::PowerDelta_ConditionRed = 1.0;
bool Phobos::UI::CenterPauseMenuBackground = false;
bool Phobos::UI::SuperWeaponSidebar = false;
bool Phobos::UI::SuperWeaponSidebar_Pyramid = true;
int Phobos::UI::SuperWeaponSidebar_Interval = 0;
int Phobos::UI::SuperWeaponSidebar_LeftOffset = 0;
int Phobos::UI::SuperWeaponSidebar_CameoHeight = 48;
int Phobos::UI::SuperWeaponSidebar_Max = 0;
int Phobos::UI::SuperWeaponSidebar_MaxColumns = INT32_MAX;
bool Phobos::UI::WeedsCounter_Show = false;
bool Phobos::UI::AnchoredToolTips = false;
bool Phobos::UI::CinemaMode = false;

bool Phobos::Config::ToolTipDescriptions = true;
bool Phobos::Config::ToolTipBlur = false;
bool Phobos::Config::PrioritySelectionFiltering = true;
bool Phobos::Config::DevelopmentCommands = true;
bool Phobos::Config::SuperWeaponSidebarCommands = false;
bool Phobos::Config::ReverseMove = false;
bool Phobos::Config::TacticalGear = false;
bool Phobos::Config::TacticalPause = true;
int Phobos::Config::SmudgeCapacity = 1;
bool Phobos::Config::AresWeaponAmmo = true;
bool Phobos::Config::TSFog = true;
bool Phobos::Config::TSFog_RevealAllClear = true;
bool Phobos::Config::TSFog_ForceAll = false;
int Phobos::Config::TSFog_FogFrame = 34;
bool Phobos::Config::TSFog_AllowAnim = false;
bool Phobos::Config::TSFog_AutoSpread = true;
bool Phobos::Config::TSFog_RevealFirer = false;
int Phobos::Config::TSFog_RevealFirer_Frames = 45;
bool Phobos::Config::ShowPlanningPath = false;
bool Phobos::Config::ArtImageSwap = false;
bool Phobos::Config::ShowPlacementPreview = false;
bool Phobos::Config::EnableSelectBox = false;
bool Phobos::Config::DigitalDisplay_Enable = false;
bool Phobos::Config::GetFireErrorGuard = true;
bool Phobos::Config::GetFireErrorTrace = true;
bool Phobos::Config::AirHeightLine_Enable = true;
bool Phobos::Config::AirHeightLine_Dashed = false;
bool Phobos::Config::AirHeightLine_GroundMarker = true;
bool Phobos::Config::AirHeightLine_ShowCoordinates = false;
int Phobos::Config::AirHeightLine_Thickness = 2;
COLORREF Phobos::Config::AirHeightLine_Color = RGB(192, 192, 192);
bool Phobos::Config::UnitStatusPanel_Enable = true;
double Phobos::Config::UnitStatusPanel_Scale = 1.0;
int Phobos::Config::UnitStatusPanel_OffsetX = 0;
int Phobos::Config::UnitStatusPanel_OffsetY = 0;
int Phobos::Config::UnitStatusPanel_MultiPortraitLimit = 60;
bool Phobos::Config::UnitStatusPanel_MessageAvoid = true;
bool Phobos::Config::UnitStatusPanel_FilterAvoid = true;
int Phobos::Config::UnitStatusPanel_Opacity = 50;
int Phobos::Config::UnitStatusPanel_SliceSize = 9;
int Phobos::Config::UnitStatusPanel_ShadowOpacity = 35;
bool Phobos::Config::UnitStatusPanel_HealthTint = true;
COLORREF Phobos::Config::UnitStatusPanel_HealthTintYellow = RGB(255, 255, 0);
COLORREF Phobos::Config::UnitStatusPanel_HealthTintRed = RGB(255, 64, 64);
int Phobos::Config::UnitStatusPanel_TurretOffsetMode = 6;
int Phobos::Config::UnitStatusPanel_TurretOffsetX = 0;
int Phobos::Config::UnitStatusPanel_TurretOffsetY = 0;
int Phobos::Config::UnitStatusPanel_BuildingScale = 100;
bool Phobos::Config::UnitStatusPanel_SHPShadow = false;
std::vector<std::string> Phobos::Config::UnitStatusPanel_TurretOffsetNoOffset;
std::unordered_map<std::string, int> Phobos::Config::UnitStatusPanel_TurretOffsetModeOverrides;
bool Phobos::Config::UnitStatusPanel_NativeDraw = false;
bool Phobos::Config::UnitStatusPanel_VoxelNativeDraw = true;
std::string Phobos::Config::UnitStatusPanel_Background = "ustatus.pcx";
std::string Phobos::Config::UnitStatusPanel_Portrait;
std::string Phobos::Config::UnitStatusPanel_PortraitFrame;
std::string Phobos::Config::UnitStatusPanel_InfoFrame;
std::string Phobos::Config::UnitStatusPanel_BottomFrame;
std::string Phobos::Config::UnitStatusPanel_SkillSlot;
std::string Phobos::Config::UnitStatusPanel_SkillIcon;
bool Phobos::Config::MessageApplyHoverState = false;
bool Phobos::Config::MessageDisplayInCenter = false;
int Phobos::Config::MessageDisplayInCenter_LabelsCount = 6;
int Phobos::Config::MessageDisplayInCenter_RecordsCount = 12;
bool Phobos::Config::RealTimeTimers = false;
bool Phobos::Config::RealTimeTimers_Adaptive = false;
int Phobos::Config::CampaignDefaultGameSpeed = 2;
bool Phobos::Config::SkirmishUnlimitedColors = false;
bool Phobos::Config::ShowDesignatorRange = false;
bool Phobos::Config::SaveVariablesOnScenarioEnd = false;
bool Phobos::Config::SaveGameOnScenarioStart = true;
bool Phobos::Config::ContinueGameOnExit = true;
bool Phobos::Config::ShowBriefing = true;
bool Phobos::Config::ShowHarvesterCounter = false;
bool Phobos::Config::ShowPowerDelta = true;
bool Phobos::Config::ShowWeedsCounter = false;
bool Phobos::Config::HideLightFlashEffects = true;
bool Phobos::Config::ShowFlashOnSelecting = false;
bool Phobos::Config::UnitPowerDrain = false;
int Phobos::Config::SuperWeaponSidebar_RequiredSignificance = 0;
bool Phobos::Config::ShowGameTime = true;
int Phobos::Config::ShowGameTime_BoardOpacity = 40;
bool Phobos::Config::TacticalZoom = false;
bool Phobos::Config::TacticalZoom_Scroll = true;
bool Phobos::Config::TacticalZoom_KeyEnabled = true;
double Phobos::Config::TacticalZoom_Max = 3.6;
double Phobos::Config::TacticalZoom_Min = 1.0;
int Phobos::Config::TacticalZoom_ZoomOutRepaint = 1;
bool Phobos::Config::TacticalZoom_ZoomOutKeepText = true;
bool Phobos::Config::TacticalZoom_ZoomOutBandFix = false;
bool Phobos::Config::DebugGCDiagnostics = false;
bool Phobos::Config::DebugPerfProbe = false;
bool Phobos::Config::DebugPerfLog = false;
bool Phobos::Config::MultiCellDisabled = false;
bool Phobos::Config::MultiCellDisabledInMultiplayer = true;
bool Phobos::Config::DebugCarrierSelfTest = false;
double Phobos::Config::TacticalZoom_Step = 0.2;
bool Phobos::Config::TacticalZoom_Smooth = true;
bool Phobos::Config::ShowFPS = false;
double Phobos::Config::TextScale = 1.0;
bool Phobos::Config::TextScale_Smooth = true;
bool Phobos::Config::FormationEnabled = true;
int Phobos::Config::FormationNearThreshold = 1536;
int Phobos::Config::FormationFarThreshold = 3072;
int Phobos::Config::FormationSpacing = 512;
int Phobos::Config::FormationSpacingInfantry = 256;

bool Phobos::Config::RightClickCommands = false;
bool Phobos::Config::RightClickCommands_Debug = false;

// MO_ExperienceEnhancements port (src/MOEE/).
bool Phobos::Config::MOEE_Disabled = false;
bool Phobos::Config::MOEE_MegaListCap = true;
bool Phobos::Config::MOEE_Observer = true;
int Phobos::Config::MOEE_WaypointCap = 128;
int Phobos::Config::MOEE_RepairAll = 0;
int Phobos::Config::MOEE_RepairAll_Interval = 30;
int Phobos::Config::MOEE_RepairAll_Batch = 24;
bool Phobos::Config::MOEE_RepairAll_DoubleClick = true;
int Phobos::Config::MOEE_RepairAll_DblFrames = 12;
int Phobos::Config::MOEE_PlaceScatter = 1;
int Phobos::Config::MOEE_PlaceScatter_Interval = 30;
int Phobos::Config::MOEE_PlaceScatter_Batch = 16;
int Phobos::Config::MOEE_PlaceScatter_Dist = 2;
int Phobos::Config::MOEE_OrderThrottle = 1;
int Phobos::Config::MOEE_TreeChop_Range = 4;
int Phobos::Config::MOEE_FuzzyAttack_Range = 4;
int Phobos::Config::MOEE_FuzzyAttack_SquadSize = 4;
int Phobos::Config::MOEE_SmartOrder_NodeStep = 2;
int Phobos::Config::MOEE_PatrolCircle_Size = 4;
bool Phobos::Config::MOEE_PatrolCircle_Commit = true;
int Phobos::Config::MOEE_SmartArea_Range = 4;
int Phobos::Config::MOEE_SmartArea_SquadSize = 4;
bool Phobos::Config::MOEE_DeployPreview = true;
bool Phobos::Config::MOEE_DeployPreview_AutoDeploy = true;
int Phobos::Config::MOEE_DeployPreview_Timeout = 675;
bool Phobos::Config::MOEE_MapCenter = true;
bool Phobos::Config::MOEE_WeaponRange = true;
bool Phobos::Config::MOEE_VecText = true;
char Phobos::Config::MOEE_VecText_FontFace[0x40] = "HarmonyOS_Sans_SC_Regular.ttf";
char Phobos::Config::MOEE_VecText_FontFaceLatin[0x40] = "Roboto_SemiCondensed-Medium.ttf";
char Phobos::Config::MOEE_VecText_FontFaceBold[0x40] = "HarmonyOS_Sans_SC_Bold.ttf";
char Phobos::Config::MOEE_VecText_FontFaceLatinBold[0x40] = "Roboto_SemiCondensed-Bold.ttf";
int Phobos::Config::MOEE_VecText_FontPx = 0;
bool Phobos::Config::MOEE_VecText_Bold = false;
int Phobos::Config::MOEE_VecText_DigitTrack = 0;
bool Phobos::Config::MOEE_VecText_CjkOnly = true;
bool Phobos::Config::MOEE_VecText_MenuTake = true;
int Phobos::Config::MOEE_VecText_CreditPx = 0;
int Phobos::Config::MOEE_VecText_CreditDY = 0;
bool Phobos::Config::MOEE_VecText_Brief = true;
int Phobos::Config::MOEE_VecText_BriefLine = 125;
bool Phobos::Config::MOEE_VecText_BriefOwn = true;
int Phobos::Config::MOEE_VecText_BriefTrack = 0;
bool Phobos::Config::MOEE_VecText_BriefMark = true;
int Phobos::Config::MOEE_VecText_BriefWidth = 580;
bool Phobos::Config::MOEE_VecText_BriefAdv = true;

bool Phobos::Misc::CustomGS = false;
int Phobos::Misc::CustomGS_ChangeInterval[7] = { -1, -1, -1, -1, -1, -1, -1 };
int Phobos::Misc::CustomGS_ChangeDelay[7] = { 0, 1, 2, 3, 4, 5, 6 };
int Phobos::Misc::CustomGS_DefaultDelay[7] = { 0, 1, 2, 3, 4, 5, 6 };

static void ParseHotkeyString(const std::string& s, int& mainKey, int& modifier)
{
	mainKey = 0;
	modifier = 0;

	size_t start = 0;
	while (start < s.size())
	{
		std::string token;
		size_t plus = s.find('+', start);
		if (plus == std::string::npos)
		{
			token = s.substr(start);
			start = s.size();
		}
		else
		{
			token = s.substr(start, plus - start);
			start = plus + 1;
		}

		// Trim whitespace
		size_t first = token.find_first_not_of(" \t");
		if (first == std::string::npos)
			continue;
		size_t last = token.find_last_not_of(" \t");
		token = token.substr(first, last - first + 1);

		// Check modifiers
		if (_stricmp(token.c_str(), "Ctrl") == 0 || _stricmp(token.c_str(), "Control") == 0)
		{
			modifier |= 1;
		}
		else if (_stricmp(token.c_str(), "Shift") == 0)
		{
			modifier |= 2;
		}
		else if (_stricmp(token.c_str(), "Alt") == 0)
		{
			modifier |= 4;
		}
		else
		{
			// Function keys F1-F24
			if (token.size() >= 2 && (token[0] == 'F' || token[0] == 'f'))
			{
				int fnum = atoi(token.c_str() + 1);
				if (fnum >= 1 && fnum <= 24)
					mainKey = VK_F1 + fnum - 1;
			}
			// Single character A-Z or 0-9
			else if (token.size() == 1)
			{
				char c = static_cast<char>(toupper(static_cast<unsigned char>(token[0])));
				if (c >= 'A' && c <= 'Z')
					mainKey = static_cast<int>(c);
				else if (c >= '0' && c <= '9')
					mainKey = static_cast<int>(c);
			}
			// Special keys
			else if (_stricmp(token.c_str(), "Space") == 0) mainKey = VK_SPACE;
			else if (_stricmp(token.c_str(), "Enter") == 0 || _stricmp(token.c_str(), "Return") == 0) mainKey = VK_RETURN;
			else if (_stricmp(token.c_str(), "Tab") == 0) mainKey = VK_TAB;
			else if (_stricmp(token.c_str(), "Esc") == 0 || _stricmp(token.c_str(), "Escape") == 0) mainKey = VK_ESCAPE;
			else if (_stricmp(token.c_str(), "Backspace") == 0 || _stricmp(token.c_str(), "Back") == 0) mainKey = VK_BACK;
			else if (_stricmp(token.c_str(), "Delete") == 0 || _stricmp(token.c_str(), "Del") == 0) mainKey = VK_DELETE;
			else if (_stricmp(token.c_str(), "Insert") == 0 || _stricmp(token.c_str(), "Ins") == 0) mainKey = VK_INSERT;
			else if (_stricmp(token.c_str(), "Home") == 0) mainKey = VK_HOME;
			else if (_stricmp(token.c_str(), "End") == 0) mainKey = VK_END;
			else if (_stricmp(token.c_str(), "PageUp") == 0 || _stricmp(token.c_str(), "PgUp") == 0) mainKey = VK_PRIOR;
			else if (_stricmp(token.c_str(), "PageDown") == 0 || _stricmp(token.c_str(), "PgDn") == 0) mainKey = VK_NEXT;
			else if (_stricmp(token.c_str(), "Up") == 0) mainKey = VK_UP;
			else if (_stricmp(token.c_str(), "Down") == 0) mainKey = VK_DOWN;
			else if (_stricmp(token.c_str(), "Left") == 0) mainKey = VK_LEFT;
			else if (_stricmp(token.c_str(), "Right") == 0) mainKey = VK_RIGHT;
			else
			{
				// Try parsing as numeric VK code (backward compatibility)
				int num = atoi(token.c_str());
				if (num > 0)
					mainKey = num;
			}
		}
	}
}
DEFINE_HOOK(0x5FACDF, OptionsClass_LoadSettings_LoadPhobosSettings, 0x5)
{
	const auto phobosSection = "Phobos";

	TacticalMap::LoadFromINI(&CCINIClass::INI_RA2MD);

	Phobos::Config::ToolTipDescriptions = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ToolTipDescriptions", true);
	Phobos::Config::DebugGCDiagnostics = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "Debug.GCDiagnostics", false);
	Phobos::Config::DebugPerfProbe = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "Debug.PerfProbe", false);
	Phobos::Config::DebugPerfLog = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "Debug.PerfLog", false);
	Phobos::Config::MultiCellDisabled = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MultiCell.Disabled", false);
	Phobos::Config::MultiCellDisabledInMultiplayer = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MultiCell.DisabledInMultiplayer", true);
	Debug::VerboseCarrier = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "Debug.AircraftCarrier", false);
	Phobos::Config::DebugCarrierSelfTest = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "Debug.CarrierSelfTest", false);
	PerfProbe::Enabled = Phobos::Config::DebugPerfProbe;
	Phobos::Config::ToolTipBlur = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ToolTipBlur", false);
	Phobos::Config::PrioritySelectionFiltering = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "PrioritySelectionFiltering", true);
	Phobos::Config::ShowPlacementPreview = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowPlacementPreview", true);
	Phobos::Config::MessageApplyHoverState = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MessageApplyHoverState", false);
	Phobos::Config::MessageDisplayInCenter = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MessageDisplayInCenter", false);
	Phobos::Config::MessageDisplayInCenter_LabelsCount = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MessageDisplayInCenter.LabelsCount", 6);
	Phobos::Config::MessageDisplayInCenter_RecordsCount = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MessageDisplayInCenter.RecordsCount", 12);
	Phobos::Config::RealTimeTimers = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "RealTimeTimers", false);
	Phobos::Config::RealTimeTimers_Adaptive = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "RealTimeTimers.Adaptive", false);
	Phobos::Config::EnableSelectBox = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "EnableSelectBox", false);
	Phobos::Config::DigitalDisplay_Enable = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "DigitalDisplay.Enable", false);
	Phobos::Config::GetFireErrorGuard = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "GetFireError.Guard", true);
	Phobos::Config::GetFireErrorTrace = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "GetFireError.Trace", true);
	CrashTrace::Enabled = Phobos::Config::GetFireErrorTrace;
	Phobos::Config::AirHeightLine_Enable = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "AirHeightLine.Enable", true);
	Phobos::Config::AirHeightLine_Dashed = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "AirHeightLine.Dashed", false);
	Phobos::Config::AirHeightLine_GroundMarker = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "AirHeightLine.GroundMarker", true);
	Phobos::Config::AirHeightLine_ShowCoordinates = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "AirHeightLine.ShowCoordinates", false);
	Phobos::Config::AirHeightLine_Thickness = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "AirHeightLine.Thickness", 2), 1, 4);
	{
		const ColorStruct airLineColor = CCINIClass::INI_RA2MD.ReadColor(
			phobosSection, "AirHeightLine.Color", ColorStruct(192, 192, 192));
		Phobos::Config::AirHeightLine_Color = Drawing::RGB_To_Int(airLineColor);
	}
	Phobos::Config::UnitStatusPanel_Enable = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.Enable", true);
	Phobos::Config::UnitStatusPanel_Scale = CCINIClass::INI_RA2MD.ReadDouble(phobosSection, "UnitStatusPanel.Scale", 1.0);
	Phobos::Config::UnitStatusPanel_OffsetX = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.OffsetX", 0);
	Phobos::Config::UnitStatusPanel_OffsetY = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.OffsetY", 0);
	Phobos::Config::UnitStatusPanel_MultiPortraitLimit = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.MultiPortraitLimit", 60), 0, 256);
	Phobos::Config::UnitStatusPanel_MessageAvoid = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.MessageAvoid", true);
	Phobos::Config::UnitStatusPanel_FilterAvoid = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.FilterAvoid", true);
	Phobos::Config::UnitStatusPanel_Opacity = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.Opacity", 50), 0, 100);
	Phobos::Config::UnitStatusPanel_SliceSize = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.SliceSize", 9), 0, 64);
	Phobos::Config::UnitStatusPanel_ShadowOpacity = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.ShadowOpacity", 35), 0, 100);
	Phobos::Config::UnitStatusPanel_HealthTint =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.HealthTint", true);
	{
		const ColorStruct yellow = CCINIClass::INI_RA2MD.ReadColor(
			phobosSection, "UnitStatusPanel.HealthTint.Yellow", ColorStruct(255, 255, 0));
		const ColorStruct red = CCINIClass::INI_RA2MD.ReadColor(
			phobosSection, "UnitStatusPanel.HealthTint.Red", ColorStruct(255, 64, 64));

		Phobos::Config::UnitStatusPanel_HealthTintYellow = Drawing::RGB_To_Int(yellow);
		Phobos::Config::UnitStatusPanel_HealthTintRed = Drawing::RGB_To_Int(red);
	}
	Phobos::Config::UnitStatusPanel_TurretOffsetMode = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.TurretOffsetMode", 6), 0, 6);
	Phobos::Config::UnitStatusPanel_TurretOffsetX =
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.TurretOffsetX", 0);
	Phobos::Config::UnitStatusPanel_TurretOffsetY =
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.TurretOffsetY", 0);
	Phobos::Config::UnitStatusPanel_BuildingScale = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "UnitStatusPanel.BuildingScale", 100), 1, 100);
	Phobos::Config::UnitStatusPanel_SHPShadow =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.SHPShadow", false);

	Phobos::Config::UnitStatusPanel_NativeDraw =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.NativeDraw", false);
	Phobos::Config::UnitStatusPanel_VoxelNativeDraw =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "UnitStatusPanel.VoxelNativeDraw", true);

	Phobos::Config::UnitStatusPanel_TurretOffsetModeOverrides.clear();
	{
		constexpr const char* prefix = "UnitStatusPanel.TurretOffsetMode.";
		const int keyCount = CCINIClass::INI_RA2MD.GetKeyCount(phobosSection);

		for (int i = 0; i < keyCount; ++i)
		{
			const char* key = CCINIClass::INI_RA2MD.GetKeyName(phobosSection, i);

			if (!key || std::strncmp(key, prefix, std::strlen(prefix)) != 0)
				continue;

			const int value = std::clamp(
				CCINIClass::INI_RA2MD.ReadInteger(phobosSection, key, 1), 0, 6);
			Phobos::Config::UnitStatusPanel_TurretOffsetModeOverrides.emplace(
				key + std::strlen(prefix), value);
		}
	}

	Phobos::Config::UnitStatusPanel_TurretOffsetNoOffset.clear();
	{
		char buffer[Phobos::readLength] {};
		CCINIClass::INI_RA2MD.ReadString(
			phobosSection, "UnitStatusPanel.TurretOffsetNoOffset", "", buffer);
		std::stringstream stream(buffer);
		std::string item;

		while (std::getline(stream, item, ','))
		{
			const auto first = item.find_first_not_of(" 	");
			const auto last = item.find_last_not_of(" 	");

			if (first != std::string::npos)
				Phobos::Config::UnitStatusPanel_TurretOffsetNoOffset.emplace_back(
					item.substr(first, last - first + 1));
		}
	}

	auto readPanelPath = [&](const char* key, std::string& target, bool keepOnMissing)
	{
		char buffer[Phobos::readLength] {};

		CCINIClass::INI_RA2MD.ReadString(phobosSection, key, "", buffer);

		if (!buffer[0])
		{
			if (!keepOnMissing)
				target.clear();

			return;
		}

		if (_stricmp(buffer, NONE_STR) == 0)
			target.clear();
		else
			target = buffer;
	};

	readPanelPath("UnitStatusPanel.Background", Phobos::Config::UnitStatusPanel_Background, true);
	readPanelPath("UnitStatusPanel.Portrait", Phobos::Config::UnitStatusPanel_Portrait, false);
	readPanelPath("UnitStatusPanel.PortraitFrame", Phobos::Config::UnitStatusPanel_PortraitFrame, false);
	readPanelPath("UnitStatusPanel.InfoFrame", Phobos::Config::UnitStatusPanel_InfoFrame, false);
	readPanelPath("UnitStatusPanel.BottomFrame", Phobos::Config::UnitStatusPanel_BottomFrame, false);
	readPanelPath("UnitStatusPanel.SkillSlot", Phobos::Config::UnitStatusPanel_SkillSlot, false);
	readPanelPath("UnitStatusPanel.SkillIcon", Phobos::Config::UnitStatusPanel_SkillIcon, false);
	// [DISABLED] SaveGameOnScenarioStart 已强制关闭：任务开始时不再写入阻塞式自动存档
	// （约 8MB），否则开局会冻结 2-3 秒。客户端重写 RA2MD.ini 时会丢弃该配置项，
	// 因此无法通过配置文件可靠地关闭，这里直接在代码中禁用。
	Phobos::Config::SaveGameOnScenarioStart = false;
	Phobos::Config::ContinueGameOnExit = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ContinueGameOnExit", true);
	Phobos::Config::ShowBriefing = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowBriefing", true);
	Phobos::Config::ShowPowerDelta = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowPowerDelta", true);
	Phobos::Config::ShowHarvesterCounter = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowHarvesterCounter", true);
	Phobos::Config::ShowWeedsCounter = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowWeedsCounter", true);
	Phobos::Config::HideLightFlashEffects = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "HideLightFlashEffects", false);
	Phobos::Config::ShowFlashOnSelecting = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowFlashOnSelecting", false);
	Phobos::Config::SuperWeaponSidebar_RequiredSignificance = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "SuperWeaponSidebar.RequiredSignificance", 0);
	Phobos::Config::ShowGameTime = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowGameTime", true);
	Phobos::Config::ShowGameTime_BoardOpacity = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "ShowGameTime.BoardOpacity", 40);
	// FPS / frame-time monitor. The hotkey ("New FPS Counter") cycles
	// Off -> Simple -> Detailed -> Off; ShowFPS selects the initial state.
	Phobos::Config::ShowFPS = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowFPS", false);
	PerfOverlay::InitFromConfig(Phobos::Config::ShowFPS);

	// In-game text scale. Allowed values: 1.0, 1.25, 1.5, 1.75, 2.0
	// (step 0.25, min 1.0, max 2.0). 1.0 keeps the original size.
	{
		double textScale = CCINIClass::INI_RA2MD.ReadDouble(phobosSection, "TextScale", 1.0);
		textScale = std::round(textScale * 4.0) / 4.0;
		Phobos::Config::TextScale = std::clamp(textScale, 1.0, 2.0);
		Phobos::Config::TextScale_Smooth = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "TextScale.Smooth", true);
	}

	Phobos::Config::FormationEnabled = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "FormationEnabled", true);
	Phobos::Config::FormationNearThreshold = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "FormationNearThreshold", 1536);
	Phobos::Config::FormationFarThreshold = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "FormationFarThreshold", 3072);
	Phobos::Config::FormationSpacing = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "FormationSpacing", 512);
	{
		// Infantry formations pack tighter than vehicles. If the key is absent,
		// derive it from the vehicle spacing (half); always clamp to a sane min.
		int infantrySpacing = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "FormationSpacingInfantry", -1);

		if (infantrySpacing < 0)
			infantrySpacing = Phobos::Config::FormationSpacing / 2;

		if (infantrySpacing < 64)
			infantrySpacing = 64;

		Phobos::Config::FormationSpacingInfantry = infantrySpacing;
	}

	// Mainstream-RTS control scheme (left select / right command). Written by the
	// client options panel, off by default.
	Phobos::Config::RightClickCommands = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "RightClickCommands", false);
	Phobos::Config::RightClickCommands_Debug = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "RightClickCommands.Debug", false);

	// MO_ExperienceEnhancements port toggles (see src/MOEE/). Defaults match the
	// upstream plugin (enabled).
	Phobos::Config::MOEE_MegaListCap = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.MegaListCap", true);
	Phobos::Config::MOEE_Observer = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.Observer", true);
	// 0 = unlimited (single player), 1..4096 = cap. Multiplayer always uses vanilla 16/128.
	Phobos::Config::MOEE_WaypointCap = CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.WaypointCap", 128);
	// Repair simplification: 0 = off, 1 = manual (Ctrl/double-click), 2 = automatic.
	Phobos::Config::MOEE_RepairAll = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.RepairAll", 0), 0, 2);
	Phobos::Config::MOEE_RepairAll_Interval = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.RepairAll.Interval", 30), 5, 600);
	Phobos::Config::MOEE_RepairAll_Batch = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.RepairAll.Batch", 24), 1, 96);
	Phobos::Config::MOEE_RepairAll_DoubleClick =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.RepairAll.DoubleClick", true);
	Phobos::Config::MOEE_RepairAll_DblFrames = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.RepairAll.DblFrames", 12), 3, 60);
	// Clear own ground units out of the building footprint when placing:
	// 0 = off, 1 = on click, 2 = also chase the hovering ghost.
	Phobos::Config::MOEE_PlaceScatter = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.PlaceScatter", 1), 0, 2);
	Phobos::Config::MOEE_PlaceScatter_Interval = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.PlaceScatter.Interval", 30), 5, 300);
	Phobos::Config::MOEE_PlaceScatter_Batch = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.PlaceScatter.Batch", 16), 1, 64);
	Phobos::Config::MOEE_PlaceScatter_Dist = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.PlaceScatter.Dist", 2), 1, 4);
	// Single-player order cap removal (direct DoList write); multiplayer always
	// keeps vanilla behaviour. 0 = disabled, 1 = enabled.
	Phobos::Config::MOEE_OrderThrottle = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.OrderThrottle", 1), 0, 1);
	// Smart middle-click / hotkey modules.
	Phobos::Config::MOEE_TreeChop_Range = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.TreeChop.Range", 4), 2, 16);
	Phobos::Config::MOEE_FuzzyAttack_Range = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.FuzzyAttack.Range", 4), 2, 16);
	Phobos::Config::MOEE_FuzzyAttack_SquadSize = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.FuzzyAttack.SquadSize", 4), 2, 16);
	Phobos::Config::MOEE_SmartOrder_NodeStep = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.SmartOrder.NodeStep", 2), 1, 8);
	Phobos::Config::MOEE_PatrolCircle_Size = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.PatrolCircle.Size", 4), 2, 16);
	Phobos::Config::MOEE_PatrolCircle_Commit =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.PatrolCircle.Commit", true);
	Phobos::Config::MOEE_SmartArea_Range = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.SmartArea.Range", 4), 2, 16);
	Phobos::Config::MOEE_SmartArea_SquadSize = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.SmartArea.SquadSize", 4), 1, 16);
	Phobos::Config::MOEE_DeployPreview =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.DeployPreview", true);
	Phobos::Config::MOEE_DeployPreview_AutoDeploy =
		CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.DeployPreview.AutoDeploy", true);
	Phobos::Config::MOEE_DeployPreview_Timeout = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.DeployPreview.Timeout", 675), 30, 4500);
	// High-resolution map centering: pin the camera to the map centre when the
	// viewport is larger than the map (fixes the left/right jitter).
	Phobos::Config::MOEE_MapCenter = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.MapCenter", true);
	// Hold the configured key part (default Ctrl+Shift) to draw the weapon range.
	Phobos::Config::MOEE_WeaponRange = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.WeaponRange", true);

	// Global vector-text takeover (MO_EE VecText port, staged).
	Phobos::Config::MOEE_VecText = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText", true);
	CCINIClass::INI_RA2MD.ReadString(phobosSection, "MOEE.VecText.FontFace", "HarmonyOS_Sans_SC_Regular.ttf",
		Phobos::Config::MOEE_VecText_FontFace);
	CCINIClass::INI_RA2MD.ReadString(phobosSection, "MOEE.VecText.FontFaceLatin", "Roboto_SemiCondensed-Medium.ttf",
		Phobos::Config::MOEE_VecText_FontFaceLatin);
	CCINIClass::INI_RA2MD.ReadString(phobosSection, "MOEE.VecText.FontFaceBold", "HarmonyOS_Sans_SC_Bold.ttf",
		Phobos::Config::MOEE_VecText_FontFaceBold);
	CCINIClass::INI_RA2MD.ReadString(phobosSection, "MOEE.VecText.FontFaceLatinBold", "Roboto_SemiCondensed-Bold.ttf",
		Phobos::Config::MOEE_VecText_FontFaceLatinBold);
	Phobos::Config::MOEE_VecText_FontPx = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.FontPx", 0), 0, 40);
	Phobos::Config::MOEE_VecText_Bold = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.Bold", false);
	Phobos::Config::MOEE_VecText_DigitTrack = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.DigitTrack", 0), -8, 8);
	Phobos::Config::MOEE_VecText_CjkOnly = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.CjkOnly", true);
	Phobos::Config::MOEE_VecText_MenuTake = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.MenuTake", true);
	Phobos::Config::MOEE_VecText_CreditPx = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.CreditPx", 0), 0, 40);
	Phobos::Config::MOEE_VecText_CreditDY = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.CreditDY", 0), -16, 16);
	Phobos::Config::MOEE_VecText_Brief = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.Brief", true);
	Phobos::Config::MOEE_VecText_BriefLine = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.BriefLine", 125), 0, 400);
	Phobos::Config::MOEE_VecText_BriefOwn = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.BriefOwn", true);
	Phobos::Config::MOEE_VecText_BriefTrack = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.BriefTrack", 0), -8, 8);
	Phobos::Config::MOEE_VecText_BriefMark = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.BriefMark", true);
	Phobos::Config::MOEE_VecText_BriefWidth = std::clamp(
		CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "MOEE.VecText.BriefWidth", 580), 0, 4096);
	Phobos::Config::MOEE_VecText_BriefAdv = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "MOEE.VecText.BriefAdv", true);

	// Custom game speeds, 6 - i so that GS6 is index 0, just like in the engine
	Phobos::Config::CampaignDefaultGameSpeed = 6 - CCINIClass::INI_RA2MD.ReadInteger(phobosSection, "CampaignDefaultGameSpeed", 4);
	if (Phobos::Config::CampaignDefaultGameSpeed > 6 || Phobos::Config::CampaignDefaultGameSpeed < 0)
	{
		Phobos::Config::CampaignDefaultGameSpeed = 2;
	}

	{
		const byte temp = (byte)Phobos::Config::CampaignDefaultGameSpeed;

		Patch::Apply_RAW(0x55D77A, { temp }); // We overwrite the instructions that force GameSpeed to 2 (GS4)
		Patch::Apply_RAW(0x55D78D, { temp }); // when speed control is off. Doesn't need a hook.
	}

	Phobos::Config::ShowDesignatorRange = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "ShowDesignatorRange", false);

	CCINIClass ini_uimd {};
	ini_uimd.LoadFromFile(GameStrings::UIMD_INI);

	// LoadingScreen
	{
		Phobos::UI::DisableEmptySpawnPositions =
			ini_uimd.ReadBool("LoadingScreen", "DisableEmptySpawnPositions", false);
	}

	// ToolTips
	{
		Phobos::UI::ExtendedToolTips =
			ini_uimd.ReadBool(GameStrings::ToolTips, "ExtendedToolTips", false);

		Phobos::UI::AnchoredToolTips =
			ini_uimd.ReadBool(GameStrings::ToolTips, "AnchoredToolTips", false);

		Phobos::UI::MaxToolTipWidth =
			ini_uimd.ReadInteger(GameStrings::ToolTips, "MaxWidth", 0);

		ini_uimd.ReadString(GameStrings::ToolTips, "CostLabel", NONE_STR, Phobos::readBuffer);
		Phobos::UI::CostLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"$");
		
		ini_uimd.ReadString(GameStrings::ToolTips, "BattlePoints.Label", NONE_STR, Phobos::readBuffer);
		Phobos::UI::BattlePoints_Label = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u2605: "); // ¡ï: 

		ini_uimd.ReadString(GameStrings::ToolTips, "PowerLabel", NONE_STR, Phobos::readBuffer);
		Phobos::UI::PowerLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u26a1"); // âš¡

		ini_uimd.ReadString(GameStrings::ToolTips, "PowerBlackoutLabel", NONE_STR, Phobos::readBuffer);
		Phobos::UI::PowerBlackoutLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u26a1\u274c"); // âš¡âŒ

		ini_uimd.ReadString(GameStrings::ToolTips, "TimeLabel", NONE_STR, Phobos::readBuffer);
		Phobos::UI::TimeLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u231a"); // âŒš

		ini_uimd.ReadString(GameStrings::ToolTips, "SWShotsFormat", NONE_STR, Phobos::readBuffer);
		Phobos::UI::SWShotsFormat = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"Shots: %d"); // âŒš

		Phobos::UI::ChargeLabel = StringTable::TryFetchString("TXT_CHARGE", L"Charge: ");
	}

	// Sidebar
	{
		Phobos::UI::HarvesterCounter_Show =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "HarvesterCounter.Show", false);

		ini_uimd.ReadString(SIDEBAR_SECTION, "HarvesterCounter.Label", NONE_STR, Phobos::readBuffer);
		Phobos::UI::HarvesterLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u26cf"); // â›

		Phobos::UI::HarvesterCounter_ConditionYellow =
			ini_uimd.ReadDouble(SIDEBAR_SECTION, "HarvesterCounter.ConditionYellow", Phobos::UI::HarvesterCounter_ConditionYellow);

		Phobos::UI::HarvesterCounter_ConditionRed =
			ini_uimd.ReadDouble(SIDEBAR_SECTION, "HarvesterCounter.ConditionRed", Phobos::UI::HarvesterCounter_ConditionRed);

		Phobos::UI::WeedsCounter_Show =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "WeedsCounter.Show", false);

		Phobos::UI::ProducingProgress_Show =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "ProducingProgress.Show", false);

		Phobos::UI::PowerDelta_Show =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "PowerDelta.Show", false);

		Phobos::UI::PowerDelta_ConditionYellow =
			ini_uimd.ReadDouble(SIDEBAR_SECTION, "PowerDelta.ConditionYellow", Phobos::UI::PowerDelta_ConditionYellow);

		Phobos::UI::PowerDelta_ConditionRed =
			ini_uimd.ReadDouble(SIDEBAR_SECTION, "PowerDelta.ConditionRed", Phobos::UI::PowerDelta_ConditionRed);

		Phobos::UI::CenterPauseMenuBackground =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "CenterPauseMenuBackground", Phobos::UI::CenterPauseMenuBackground);

		Phobos::UI::SuperWeaponSidebar =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "SuperWeaponSidebar", Phobos::UI::SuperWeaponSidebar);

		Phobos::UI::SuperWeaponSidebar_Pyramid =
			ini_uimd.ReadBool(SIDEBAR_SECTION, "SuperWeaponSidebar.Pyramid", Phobos::UI::SuperWeaponSidebar_Pyramid);

		Phobos::UI::SuperWeaponSidebar_Interval =
			ini_uimd.ReadInteger(SIDEBAR_SECTION, "SuperWeaponSidebar.Interval", Phobos::UI::SuperWeaponSidebar_Interval);

		Phobos::UI::SuperWeaponSidebar_LeftOffset =
			ini_uimd.ReadInteger(SIDEBAR_SECTION, "SuperWeaponSidebar.LeftOffset", Phobos::UI::SuperWeaponSidebar_LeftOffset);

		Phobos::UI::SuperWeaponSidebar_LeftOffset = std::min(Phobos::UI::SuperWeaponSidebar_Interval, Phobos::UI::SuperWeaponSidebar_LeftOffset);

		Phobos::UI::SuperWeaponSidebar_CameoHeight =
			ini_uimd.ReadInteger(SIDEBAR_SECTION, "SuperWeaponSidebar.CameoHeight", Phobos::UI::SuperWeaponSidebar_CameoHeight);

		Phobos::UI::SuperWeaponSidebar_CameoHeight = std::max(48, Phobos::UI::SuperWeaponSidebar_CameoHeight);

		Phobos::UI::SuperWeaponSidebar_Max =
			ini_uimd.ReadInteger(SIDEBAR_SECTION, "SuperWeaponSidebar.Max", Phobos::UI::SuperWeaponSidebar_Max);

		const int reserveHeight = 96;
		const int screenHeight = GameOptionsClass::Instance.ScreenHeight - reserveHeight;

		if (Phobos::UI::SuperWeaponSidebar_Max > 0)
			Phobos::UI::SuperWeaponSidebar_Max = std::min(Phobos::UI::SuperWeaponSidebar_Max, screenHeight / Phobos::UI::SuperWeaponSidebar_CameoHeight);
		else
			Phobos::UI::SuperWeaponSidebar_Max = screenHeight / Phobos::UI::SuperWeaponSidebar_CameoHeight;

		Phobos::UI::SuperWeaponSidebar_MaxColumns =
			ini_uimd.ReadInteger(SIDEBAR_SECTION, "SuperWeaponSidebar.MaxColumns", Phobos::UI::SuperWeaponSidebar_MaxColumns);
			
		Phobos::UI::BattlePointsSidebar_Label_InvertPosition = ini_uimd.ReadBool(SIDEBAR_SECTION, "BattlePointsSidebar.Label.InvertPosition", false);

		ini_uimd.ReadString(SIDEBAR_SECTION, "BattlePointsSidebar.Label", NONE_STR, Phobos::readBuffer);
		Phobos::UI::BattlePointsSidebar_Label = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"\u2605"); // %d ¡ï
	}

	// UISettings
	{
		ini_uimd.ReadString(UISETTINGS_SECTION, "ShowBriefingResumeButtonLabel", "GUI:Resume", Phobos::readBuffer);
		Phobos::UI::ShowBriefingResumeButtonLabel = GeneralUtils::LoadStringOrDefault(Phobos::readBuffer, L"");

		ini_uimd.ReadString(UISETTINGS_SECTION, "ShowBriefingResumeButtonStatusLabel", "STT:BriefingButtonReturn", Phobos::readBuffer);
		strcpy_s(Phobos::UI::ShowBriefingResumeButtonStatusLabel, Phobos::readBuffer);
	}

	// TacticalZoom
	{
		const char* const section = ini_uimd.GetSection("TacticalZoom") ? "TacticalZoom" : UISETTINGS_SECTION;

		const bool modderZoomEnabled = ini_uimd.ReadBool(section, "TacticalZoom",
			ini_uimd.ReadBool(section, "Enabled", false));

		Phobos::Config::TacticalZoom_Scroll = ini_uimd.ReadBool(section, "TacticalZoom.Scroll",
			ini_uimd.ReadBool(section, "Scroll",
			ini_uimd.ReadBool(section, "TacticalZoom.Wheel",
			ini_uimd.ReadBool(section, "Wheel", true))));

		Phobos::Config::TacticalZoom_KeyEnabled = ini_uimd.ReadBool(section, "TacticalZoom.KeyEnabled",
			ini_uimd.ReadBool(section, "KeyEnabled",
			ini_uimd.ReadBool(section, "TacticalZoom.Hotkeys",
			ini_uimd.ReadBool(section, "Hotkeys", true))));

		Phobos::Config::TacticalZoom_Max = ini_uimd.ReadDouble(section, "TacticalZoom.Max",
			ini_uimd.ReadDouble(section, "Max", 3.6));

		Phobos::Config::TacticalZoom_Min = ini_uimd.ReadDouble(section, "TacticalZoom.Min",
			ini_uimd.ReadDouble(section, "Min", 1.0));

		Phobos::Config::TacticalZoom_ZoomOutRepaint = ini_uimd.ReadInteger(section, "TacticalZoom.ZoomOutRepaint",
			ini_uimd.ReadInteger(section, "ZoomOutRepaint", 1));

		Phobos::Config::TacticalZoom_ZoomOutKeepText = ini_uimd.ReadBool(section, "TacticalZoom.ZoomOutKeepText",
			ini_uimd.ReadBool(section, "ZoomOutKeepText", true));

		Phobos::Config::TacticalZoom_ZoomOutBandFix = ini_uimd.ReadBool(section, "TacticalZoom.ZoomOutBandFix",
			ini_uimd.ReadBool(section, "ZoomOutBandFix", false));

		Phobos::Config::TacticalZoom_Step = ini_uimd.ReadDouble(section, "TacticalZoom.Step",
			ini_uimd.ReadDouble(section, "Step", 0.2));

		Phobos::Config::TacticalZoom_Smooth = ini_uimd.ReadBool(section, "TacticalZoom.Smooth",
			ini_uimd.ReadBool(section, "Smooth", true));

		// Player preference overrides from RA2MD.INI [Phobos]
		const bool playerZoomEnabled = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "TacticalZoom", true);
		Phobos::Config::TacticalZoom_Smooth = CCINIClass::INI_RA2MD.ReadBool(phobosSection, "TacticalZoom.Smooth", Phobos::Config::TacticalZoom_Smooth);

		Phobos::Config::TacticalZoom = modderZoomEnabled && playerZoomEnabled;

		ZoomManager::Enabled = Phobos::Config::TacticalZoom;
		ZoomManager::ScrollEnabled = Phobos::Config::TacticalZoom_Scroll;
		ZoomManager::KeyEnabled = Phobos::Config::TacticalZoom_KeyEnabled;
		ZoomManager::MaxZoom = std::max(1.0, Phobos::Config::TacticalZoom_Max);
		// Zoom-out support: <1.0 lets the viewport grow past the window (R > A) and
		// the larger frame is downsampled back into the window (see ZoomManager).
		// Clamped to [0.2, 1.0]; 1.0 disables zoom-out entirely (vanilla behaviour).
		ZoomManager::MinZoom = std::clamp(Phobos::Config::TacticalZoom_Min, 0.2, 1.0);
		ZoomManager::ZoomOutRepaint = std::clamp(Phobos::Config::TacticalZoom_ZoomOutRepaint, 0, 3);
		ZoomManager::ZoomOutKeepText = Phobos::Config::TacticalZoom_ZoomOutKeepText;
		ZoomManager::ZoomOutBandFix = Phobos::Config::TacticalZoom_ZoomOutBandFix;
		ZoomManager::Step = std::max(0.01, Phobos::Config::TacticalZoom_Step);
		ZoomManager::Smooth = Phobos::Config::TacticalZoom_Smooth;

		ZoomManager::ApplySurfacePatches();
	}

	return 0;
}

DEFINE_HOOK(0x52D21F, InitRules_ThingsThatShouldntBeSerailized, 0x6)
{
	CCINIClass* const pINI_RULESMD = CCINIClass::INI_Rules;

	RulesClass::Instance->Read_JumpjetControls(pINI_RULESMD);

	// [General] AresWeaponAmmo: master switch for the per-weapon/ExtraFire ammo
	// pools. yes (default) keeps the custom pools; no restores the vanilla single
	// unit Ammo pool and makes ExtraFire unlimited.
	Phobos::Config::AresWeaponAmmo = pINI_RULESMD->ReadBool(GameStrings::General, "AresWeaponAmmo", true);

	// [General] MOEE.Disabled: master switch for the MO_ExperienceEnhancements
	// gameplay modules and keyboard commands. Diagnostics/crash logging stay on.
	Phobos::Config::MOEE_Disabled = pINI_RULESMD->ReadBool(GameStrings::General, "MOEE.Disabled", false);

	Phobos::Config::ArtImageSwap = pINI_RULESMD->ReadBool(GameStrings::General, "ArtImageSwap", false);
	Phobos::Config::UnitPowerDrain = pINI_RULESMD->ReadBool(GameStrings::General, "UnitPowerDrain", false);
	Phobos::Config::ReverseMove = pINI_RULESMD->ReadBool(GameStrings::General, "ReverseMove", false);
	// [General] TacticalGear controls whether the game-speed shifter hotkeys are
	// registered at all. Disabled by default; when off the commands do not exist
	// and their keyboard.ini bindings are ignored.
	Phobos::Config::TacticalGear = pINI_RULESMD->ReadBool(GameStrings::General, "TacticalGear", false);
	// [General] TacticalPause registers the tactical time-stop hotkey command.
	// Defaults to on; set to no to remove the command and its keyboard binding.
	Phobos::Config::TacticalPause = pINI_RULESMD->ReadBool(GameStrings::General, "TacticalPause", true);
	// [General] TacticalPause.BlockActions.Difficulty: while the tactical time-stop
	// is active, forbid the player from performing any operation (selection, orders,
	// production, superweapons, hotkeys) at the listed difficulties. Camera panning,
	// the pause toggle and the ESC/menu keys keep working. Empty = feature off.
	// Read as a difficulty list in RulesExt (see RulesExt::ExtData).
	// [General] ReverseMove only sets the default: the player can still flip the
	// smart-reverse state at runtime with the "Toggle Auto-Reverse" hotkey.
	AutoReverseToggleCommandClass::AutoReverseEnabled = Phobos::Config::ReverseMove;
	// Max smudges per cell (native + stacked extras), used by CreateSmudge and the
	// map editor's multi-smudge loading.
	Phobos::Config::SmudgeCapacity = pINI_RULESMD->ReadInteger(GameStrings::General, "SmudgeCapacity", 1);
	// [General] TSFog: master switch for the TS-style fog of war port. When off the
	// scenario fog flag is forced off, so the whole port is inert without a rebuild.
	Phobos::Config::TSFog = pINI_RULESMD->ReadBool(GameStrings::General, "TSFog", true);
	// [General] TSFog.RevealAll: clear (default) lifts fog permanently for the rest of
	// the scenario when a "Reveal all" trigger fires; keep only drops the frozen images.
	{
		char revealAll[16] {};
		pINI_RULESMD->ReadString(GameStrings::General, "TSFog.RevealAll", "clear", revealAll);
		Phobos::Config::TSFog_RevealAllClear = _stricmp(revealAll, "keep") != 0;
	}
	// [General] TSFog.ForceAll: test switch that enables fog even for maps whose
	// [SpecialFlags] FogOfWar=no (e.g. TUTO4). Keep it off for normal play.
	Phobos::Config::TSFog_ForceAll = pINI_RULESMD->ReadBool(GameStrings::General, "TSFog.ForceAll", false);
	// [General] TSFog.FogFrame: fog frame used by FogProducer when AutoSpread=no.
	// With AutoSpread=yes the engine's own per-cell fog frame is left untouched.
	// 34 is one of the frames the engine itself uses for visible fog; 15 can be
	// transparent depending on fog.shp.
	Phobos::Config::TSFog_FogFrame = std::clamp(
		pINI_RULESMD->ReadInteger(GameStrings::General, "TSFog.FogFrame", 34), 0, 48);
	// [General] TSFog.AllowAnim: draw warhead animations (AnimList/SplashList/
	// Crit.AnimList) and AnimType AllowAnim=yes anims inside explored fog.
	Phobos::Config::TSFog_AllowAnim = pINI_RULESMD->ReadBool(GameStrings::General, "TSFog.AllowAnim", false);
	// [General] TSFog.AutoSpread: when off, fog no longer reconceals an explored
	// area on its own; only FogProducer buildings spread fog.
	Phobos::Config::TSFog_AutoSpread = pINI_RULESMD->ReadBool(GameStrings::General, "TSFog.AutoSpread", true);
	// [General] TSFog.RevealFirer: replace the vanilla shroud reveal on fire with a
	// fog-aware reveal of enemy firers (one cell, fog layer kept).
	Phobos::Config::TSFog_RevealFirer = pINI_RULESMD->ReadBool(GameStrings::General, "TSFog.RevealFirer", false);
	Phobos::Config::TSFog_RevealFirer_Frames = std::clamp(
		pINI_RULESMD->ReadInteger(GameStrings::General, "TSFog.RevealFirer.Frames", 45), 5, 300);

	Phobos::Misc::CustomGS = pINI_RULESMD->ReadBool(GameStrings::General, "CustomGS", false);

	char tempBuffer[26];
	for (size_t i = 0; i <= 6; ++i)
	{
		int temp;
		_snprintf_s(tempBuffer, sizeof(tempBuffer), "CustomGS%d.ChangeDelay", 6 - i);
		temp = pINI_RULESMD->ReadInteger(GameStrings::General, tempBuffer, -1);
		if (temp >= 0 && temp <= 6)
			Phobos::Misc::CustomGS_ChangeDelay[i] = 6 - temp;

		_snprintf_s(tempBuffer, sizeof(tempBuffer), "CustomGS%d.DefaultDelay", 6 - i);
		temp = pINI_RULESMD->ReadInteger(GameStrings::General, tempBuffer, -1);
		if (temp >= 1)
			Phobos::Misc::CustomGS_DefaultDelay[i] = 6 - temp;

		_snprintf_s(tempBuffer, sizeof(tempBuffer), "CustomGS%d.ChangeInterval", 6 - i);
		temp = pINI_RULESMD->ReadInteger(GameStrings::General, tempBuffer, -1);
		if (temp >= 1)
			Phobos::Misc::CustomGS_ChangeInterval[i] = temp;
	}

	if (pINI_RULESMD->ReadBool(GameStrings::General, "FixTransparencyBlitters", true))
		BlittersFix::Apply();

	Phobos::Config::SkirmishUnlimitedColors = pINI_RULESMD->ReadBool(GameStrings::General, "SkirmishUnlimitedColors", false);
	// Disable Ares hook at this address so that our logic can run.
	if (Phobos::Config::SkirmishUnlimitedColors)
		Patch::Apply_RAW(0x69A310, { 0x8B, 0x44, 0x24, 0x04, 0xD1, 0xE0, 0x40 });

	Phobos::Config::SaveVariablesOnScenarioEnd = pINI_RULESMD->ReadBool(GameStrings::General, "SaveVariablesOnScenarioEnd", false);
#ifndef DEBUG
	Phobos::Config::DevelopmentCommands = pINI_RULESMD->ReadBool("GlobalControls", "DebugKeysEnabled", Phobos::Config::DevelopmentCommands);
#endif
	Phobos::Config::SuperWeaponSidebarCommands = pINI_RULESMD->ReadBool("GlobalControls", "SuperWeaponSidebarKeysEnabled", Phobos::Config::SuperWeaponSidebarCommands);
	Phobos::Config::ShowPlanningPath = pINI_RULESMD->ReadBool("GlobalControls", "DebugPlanningPaths", Phobos::Config::ShowPlanningPath);

	return 0;
}
