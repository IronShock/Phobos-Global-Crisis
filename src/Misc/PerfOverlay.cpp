#include "PerfOverlay.h"
#include "PerfProbe.h"

// Must come before Surface.h: without UNICODE, Windows maps DrawText to
// DrawTextA, which is the name Surface's text method is declared with.
#include <Windows.h>

#include <Phobos.h>

#include <AnimClass.h>
#include <BuildingClass.h>
#include <Drawing.h>
#include <FootClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TagClass.h>
#include <TechnoClass.h>
#include <TerrainClass.h>
#include <Unsorted.h>

#include <Misc/ZoomManager.h>

#include <UI/UnitStatusPanel.h>

#include <Utilities/Debug.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

PerfOverlay::Mode PerfOverlay::CurrentMode = PerfOverlay::Mode::Off;

extern int LogicAIStats_LoopACount;

namespace
{
	constexpr int RingSize = 600;
	constexpr int IntervalFrames = 300;
	constexpr int RecentFrames = 60;

	float g_frameMs[RingSize] {};
	int g_head = 0;
	int g_count = 0;
	double g_ringSum = 0.0;

	double g_intervalMs = 0.0;
	int g_intervalFrames = 0;
	unsigned long long g_frameCounter = 0;
	double g_totalMs = 0.0;

	double g_lastFps = 0.0;
	double g_lastFrameMs = 0.0;
	double g_lastAverageMs = 0.0;
	double g_lastOnePercentLowMs = 0.0;
	double g_lastMaxMs = 0.0;

	double g_buildingDrawMsPerFrame = 0.0;
	double g_buildingAIMsPerFrame = 0.0;
	double g_buildingAIHookMsPerFrame = 0.0;
	double g_unitAIMsPerFrame = 0.0;
	double g_infantryAIMsPerFrame = 0.0;
	double g_aircraftAIMsPerFrame = 0.0;
	double g_animAIMsPerFrame = 0.0;
	double g_logicUpdateMsPerFrame = 0.0;
	double g_firstLoopMsPerFrame = 0.0;
	double g_terrainLoopAIMsPerFrame = 0.0;
	double g_buildingLoopAIMsPerFrame = 0.0;
	double g_unitLoopAIMsPerFrame = 0.0;
	double g_infantryLoopAIMsPerFrame = 0.0;
	double g_aircraftLoopAIMsPerFrame = 0.0;
	double g_animLoopAIMsPerFrame = 0.0;
	double g_otherLoopAIMsPerFrame = 0.0;
	double g_lightAnimMsPerFrame = 0.0;
	double g_tailTotalMsPerFrame = 0.0;
	double g_raiseEventMsPerFrame = 0.0;
	double g_cellDrawMsPerFrame = 0.0;
	double g_cellScanMsPerFrame = 0.0;
	double g_arrayAI1MsPerFrame = 0.0;
	double g_lightSourceMsPerFrame = 0.0;
	double g_animUpdateAllMsPerFrame = 0.0;
	double g_mapLightMsPerFrame = 0.0;
	double g_factoryAIMsPerFrame = 0.0;
	double g_houseAIMsPerFrame = 0.0;
	double g_drawObjectMsPerFrame = 0.0;
	double g_renderMsPerFrame = 0.0;
	double g_logicMsPerFrame = 0.0;
	int g_buildingDrawCallsPerFrame = 0;
	int g_unitAICallsPerFrame = 0;
	int g_infantryAICallsPerFrame = 0;
	int g_aircraftAICallsPerFrame = 0;
	int g_animAICallsPerFrame = 0;
	int g_raiseEventCallsPerFrame = 0;
	int g_cellDrawCallsPerFrame = 0;

	std::ofstream g_csv;
	bool g_csvOpened = false;
	bool g_csvHeaderWritten = false;

	double AverageRecent(int frames)
	{
		const int count = std::min(frames, g_count);

		if (count <= 0)
			return 0.0;

		double sum = 0.0;

		for (int i = 1; i <= count; ++i)
		{
			const int index = (g_head - i + RingSize) % RingSize;
			sum += g_frameMs[index];
		}

		return sum / static_cast<double>(count);
	}

	void UpdateExtremes()
	{
		if (g_count < 2)
		{
			g_lastOnePercentLowMs = 0.0;
			g_lastMaxMs = 0.0;
			return;
		}

		std::array<float, RingSize> sorted {};
		std::copy(std::begin(g_frameMs), std::begin(g_frameMs) + g_count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + g_count);

		g_lastMaxMs = sorted[g_count - 1];
		const int index = std::min(g_count - 1, static_cast<int>(std::floor((g_count - 1) * 0.99)));
		g_lastOnePercentLowMs = sorted[index];
	}

	void OpenCsvIfNeeded()
	{
		if (g_csvOpened || !Phobos::Config::DebugPerfLog)
			return;

		g_csvOpened = true;
		g_csvHeaderWritten = GetFileAttributesA("debug\\perf.csv") != INVALID_FILE_ATTRIBUTES;
		g_csv.open("debug\\perf.csv", std::ios::app);

		if (g_csv)
			Debug::Log("[Perf] writing debug\\perf.csv\n");

		if (g_csv && !g_csvHeaderWritten)
		{
			g_csv << "time_s,frame,fps,frame_ms,logic_ms,render_ms,"
				"bld_draw_calls,bld_draw_ms,bld_ai_calls,bld_ai_ms,bld_ai_hook_ms,"
				"unit_ai_calls,unit_ai_ms,inf_ai_calls,inf_ai_ms,"
				"air_ai_calls,air_ai_ms,anim_ai_calls,anim_ai_ms,"
				"logic_update_ms,first_loop_ms,terrain_ai_ms,building_ai_ms,unit_ai_ms,inf_ai_ms,air_ai_ms,anim_ai_ms,other_ai_ms,light_anim_ms,tail_total_ms,array_ai1_ms,light_source_ms,anim_update_all_ms,map_light_ms,factory_ai_ms,house_ai_ms,"
				"raise_event_calls,raise_event_ms,logic_loop_count,tag_total,"
				"cell_draw_calls,cell_draw_ms,"
				"draw_obj_calls,draw_obj_ms,"
				"bld_total,anim_total,terrain_total,techno_total,logic_objects,"
				"loopA_count,loopB_count,loopC_count,loopD_count,loopE_count,"
				"cellscan_ms,valid_cells,map_w,map_h,zoom,cam_x,cam_y\n";
			g_csvHeaderWritten = true;
		}
	}

	void WriteCsvRow(const PerfProbe::Snapshot& snapshot, double intervalFrames, double intervalMs)
	{
		if (!Phobos::Config::DebugPerfLog)
			return;

		OpenCsvIfNeeded();

		if (!g_csv)
			return;

		const double averageMs = intervalMs / intervalFrames;
		const double fps = intervalFrames * 1000.0 / std::max(intervalMs, 0.001);
		const double drawPerFrame = snapshot.Get(PerfProbe::Id::BuildingDraw).Calls / intervalFrames;
		const double drawMs = snapshot.Get(PerfProbe::Id::BuildingDraw).Ms / intervalFrames;
		const double aiPerFrame = snapshot.Get(PerfProbe::Id::BuildingAI).Calls / intervalFrames;
		const double aiMs = snapshot.Get(PerfProbe::Id::BuildingAI).Ms / intervalFrames;
		const double aiHookMs = snapshot.Get(PerfProbe::Id::BuildingAIHook).Ms / intervalFrames;
		const double unitPerFrame = snapshot.Get(PerfProbe::Id::UnitAI).Calls / intervalFrames;
		const double unitMs = snapshot.Get(PerfProbe::Id::UnitAI).Ms / intervalFrames;
		const double infantryPerFrame = snapshot.Get(PerfProbe::Id::InfantryAI).Calls / intervalFrames;
		const double infantryMs = snapshot.Get(PerfProbe::Id::InfantryAI).Ms / intervalFrames;
		const double aircraftPerFrame = snapshot.Get(PerfProbe::Id::AircraftAI).Calls / intervalFrames;
		const double aircraftMs = snapshot.Get(PerfProbe::Id::AircraftAI).Ms / intervalFrames;
		const double animPerFrame = snapshot.Get(PerfProbe::Id::AnimAI).Calls / intervalFrames;
		const double animMs = snapshot.Get(PerfProbe::Id::AnimAI).Ms / intervalFrames;
		const double logicUpdateMs = snapshot.Get(PerfProbe::Id::LogicUpdate).Ms / intervalFrames;
		const double eventPerFrame = snapshot.Get(PerfProbe::Id::RaiseEvent).Calls / intervalFrames;
		const double eventMs = snapshot.Get(PerfProbe::Id::RaiseEvent).Ms / intervalFrames;
		const int logicLoopCount = *reinterpret_cast<int*>(0x8B40D8);
		const int tagTotal = TagClass::Array.Count;
		const double cellPerFrame = snapshot.Get(PerfProbe::Id::CellDraw).Calls / intervalFrames;
		const double cellMs = snapshot.Get(PerfProbe::Id::CellDraw).Ms / intervalFrames;
		const double objectPerFrame = snapshot.Get(PerfProbe::Id::DrawObject).Calls / intervalFrames;
		const double objectMs = snapshot.Get(PerfProbe::Id::DrawObject).Ms / intervalFrames;
		const double cellScanMs = snapshot.Get(PerfProbe::Id::CellScan).Ms / intervalFrames;
		const int validCells = MapClass::Instance.ValidMapCellCount;
		const int mapWidth = MapClass::Instance.MapRect.Width;
		const int mapHeight = MapClass::Instance.MapRect.Height;
		const double renderMs = snapshot.Get(PerfProbe::Id::TacticalRender).Ms / intervalFrames;
		const double logicMs = std::max(0.0, averageMs - renderMs);
		const double timeSeconds = static_cast<double>(g_frameCounter) * averageMs / 1000.0;

		const auto pTactical = TacticalClass::Instance;
		const int camX = pTactical ? pTactical->TacticalPos.X : 0;
		const int camY = pTactical ? pTactical->TacticalPos.Y : 0;

		g_csv
			<< timeSeconds << ','
			<< Unsorted::CurrentFrame << ','
			<< fps << ','
			<< averageMs << ','
			<< logicMs << ','
			<< renderMs << ','
			<< static_cast<int>(std::lround(drawPerFrame)) << ','
			<< drawMs << ','
			<< static_cast<int>(std::lround(aiPerFrame)) << ','
			<< aiMs << ','
			<< aiHookMs << ','
			<< static_cast<int>(std::lround(unitPerFrame)) << ','
			<< unitMs << ','
			<< static_cast<int>(std::lround(infantryPerFrame)) << ','
			<< infantryMs << ','
			<< static_cast<int>(std::lround(aircraftPerFrame)) << ','
			<< aircraftMs << ','
			<< static_cast<int>(std::lround(animPerFrame)) << ','
			<< animMs << ','
			<< logicUpdateMs << ','
			<< g_firstLoopMsPerFrame << ','
			<< g_terrainLoopAIMsPerFrame << ','
			<< g_buildingLoopAIMsPerFrame << ','
			<< g_unitLoopAIMsPerFrame << ','
			<< g_infantryLoopAIMsPerFrame << ','
			<< g_aircraftLoopAIMsPerFrame << ','
			<< g_animLoopAIMsPerFrame << ','
			<< g_otherLoopAIMsPerFrame << ','
			<< g_lightAnimMsPerFrame << ','
			<< g_tailTotalMsPerFrame << ','
			<< g_arrayAI1MsPerFrame << ','
			<< g_lightSourceMsPerFrame << ','
			<< g_animUpdateAllMsPerFrame << ','
			<< g_mapLightMsPerFrame << ','
			<< g_factoryAIMsPerFrame << ','
			<< g_houseAIMsPerFrame << ','
			<< static_cast<int>(std::lround(eventPerFrame)) << ','
			<< eventMs << ','
			<< logicLoopCount << ','
			<< tagTotal << ','
			<< static_cast<int>(std::lround(cellPerFrame)) << ','
			<< cellMs << ','
			<< static_cast<int>(std::lround(objectPerFrame)) << ','
			<< objectMs << ','
			<< BuildingClass::Array.Count << ','
			<< AnimClass::Array.Count << ','
			<< TerrainClass::Array.Count << ','
			<< TechnoClass::Array.Count << ','
			<< LogicClass::Instance.Count << ','
			<< LogicAIStats_LoopACount << ','
			<< *reinterpret_cast<int*>(0x8A0218) << ','
			<< *reinterpret_cast<int*>(0xB04BE0) << ','
			<< *reinterpret_cast<int*>(0x87F778 + 0x10) << ','
			<< *reinterpret_cast<int*>(0xA83E10) << ','
			<< cellScanMs << ','
			<< validCells << ','
			<< mapWidth << ','
			<< mapHeight << ','
			<< ZoomManager::CurrentZoom << ','
			<< camX << ','
			<< camY << '\n';

	}

	void DrawTextLine(Surface* pSurface, const RectangleStruct& bounds, const wchar_t* text,
		int x, int y, COLORREF color)
	{
		constexpr TextPrintType printType =
			TextPrintType::FullShadow | TextPrintType::Point8 | TextPrintType::Background;

		Point2D position { x, y };
		Point2D ret {};
		Fancy_Text_Print_Wide(ret, text, pSurface, bounds, position, color, 0, printType);
	}
}

void PerfOverlay::InitFromConfig(bool show)
{
	// OptionsClass::LoadSettings can run more than once (startup + options
	// dialog); do not reset a mode the player picked with the hotkey.
	static bool initialized = false;

	if (initialized)
		return;

	initialized = true;
	CurrentMode = show ? Mode::Simple : Mode::Off;

	Debug::Log("[Perf] overlay mode=%d probe=%d log=%d\n",
		static_cast<int>(CurrentMode), PerfProbe::Enabled ? 1 : 0,
		Phobos::Config::DebugPerfLog ? 1 : 0);
}

void PerfOverlay::Toggle()
{
	switch (CurrentMode)
	{
	case Mode::Off:
		CurrentMode = Mode::Simple;
		break;
	case Mode::Simple:
		CurrentMode = Mode::Detailed;
		break;
	default:
		CurrentMode = Mode::Off;
		break;
	}
}

void PerfOverlay::Reset()
{
	g_head = 0;
	g_count = 0;
	g_ringSum = 0.0;
	g_intervalMs = 0.0;
	g_intervalFrames = 0;
	g_frameCounter = 0;
	g_totalMs = 0.0;
	g_lastFps = 0.0;
	g_lastFrameMs = 0.0;
	g_lastAverageMs = 0.0;
	g_lastOnePercentLowMs = 0.0;
	g_lastMaxMs = 0.0;
	g_buildingDrawMsPerFrame = 0.0;
	g_buildingAIMsPerFrame = 0.0;
	g_buildingAIHookMsPerFrame = 0.0;
	g_unitAIMsPerFrame = 0.0;
	g_infantryAIMsPerFrame = 0.0;
	g_aircraftAIMsPerFrame = 0.0;
	g_animAIMsPerFrame = 0.0;
	g_logicUpdateMsPerFrame = 0.0;
	g_firstLoopMsPerFrame = 0.0;
	g_terrainLoopAIMsPerFrame = 0.0;
	g_buildingLoopAIMsPerFrame = 0.0;
	g_unitLoopAIMsPerFrame = 0.0;
	g_infantryLoopAIMsPerFrame = 0.0;
	g_aircraftLoopAIMsPerFrame = 0.0;
	g_animLoopAIMsPerFrame = 0.0;
	g_otherLoopAIMsPerFrame = 0.0;
	g_lightAnimMsPerFrame = 0.0;
	g_tailTotalMsPerFrame = 0.0;
	g_raiseEventMsPerFrame = 0.0;
	g_cellDrawMsPerFrame = 0.0;
	g_cellScanMsPerFrame = 0.0;
	g_arrayAI1MsPerFrame = 0.0;
	g_lightSourceMsPerFrame = 0.0;
	g_animUpdateAllMsPerFrame = 0.0;
	g_mapLightMsPerFrame = 0.0;
	g_factoryAIMsPerFrame = 0.0;
	g_houseAIMsPerFrame = 0.0;
	g_drawObjectMsPerFrame = 0.0;
	g_renderMsPerFrame = 0.0;
	g_logicMsPerFrame = 0.0;
	g_buildingDrawCallsPerFrame = 0;
	g_unitAICallsPerFrame = 0;
	g_infantryAICallsPerFrame = 0;
	g_aircraftAICallsPerFrame = 0;
	g_animAICallsPerFrame = 0;
	g_cellDrawCallsPerFrame = 0;
}

void PerfOverlay::OnFrame()
{
	static LARGE_INTEGER frequency {};
	static LARGE_INTEGER last {};
	static bool initialized = false;
	static DWORD lastGameFrame = static_cast<DWORD>(-1);

	if (!initialized)
	{
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&last);
		initialized = true;
		return;
	}

	// GScreenClass_DrawText can be reached more than once per frame (zoom passes);
	// sample one frame per logic frame.
	const DWORD gameFrame = Unsorted::CurrentFrame;

	if (gameFrame == lastGameFrame)
		return;

	lastGameFrame = gameFrame;

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	double milliseconds = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0
		/ static_cast<double>(frequency.QuadPart);
	last = now;

	if (milliseconds <= 0.0)
		milliseconds = 0.001;

	// Clamp loading hitches so one 3-second stall does not hide the real frame time.
	if (milliseconds > 2000.0)
		milliseconds = 2000.0;

	if (Phobos::Config::DebugPerfProbe && milliseconds >= 33.0)
	{
		static DWORD lastHitchFrame = 0;
		const DWORD hitchFrame = Unsorted::CurrentFrame;

		if (hitchFrame - lastHitchFrame >= 15)
		{
			lastHitchFrame = hitchFrame;
			Debug::Log("[Perf] hitch frame=%d ms=%.1f logicMs=%.1f renderMs=%.1f anim=%d techno=%d logicObj=%d zoom=%.2f\n",
				static_cast<int>(hitchFrame), milliseconds,
				PerfProbe::LastLogicMs, PerfProbe::LastRenderMs,
				AnimClass::Array.Count, TechnoClass::Array.Count,
				LogicClass::Instance.Count, ZoomManager::CurrentZoom);
		}
	}

	const float previous = g_frameMs[g_head];
	g_frameMs[g_head] = static_cast<float>(milliseconds);
	g_ringSum += milliseconds - previous;
	g_head = (g_head + 1) % RingSize;

	if (g_count < RingSize)
		++g_count;

	g_lastFrameMs = milliseconds;
	g_intervalMs += milliseconds;
	++g_intervalFrames;
	++g_frameCounter;
	g_totalMs += milliseconds;

	if (g_intervalFrames >= IntervalFrames)
	{
		const auto snapshot = PerfProbe::TakeSnapshot();
		const double frames = static_cast<double>(g_intervalFrames);

		g_buildingDrawMsPerFrame = snapshot.Get(PerfProbe::Id::BuildingDraw).Ms / frames;
		g_buildingAIMsPerFrame = snapshot.Get(PerfProbe::Id::BuildingAI).Ms / frames;
		g_buildingAIHookMsPerFrame = snapshot.Get(PerfProbe::Id::BuildingAIHook).Ms / frames;
		g_unitAIMsPerFrame = snapshot.Get(PerfProbe::Id::UnitAI).Ms / frames;
		g_infantryAIMsPerFrame = snapshot.Get(PerfProbe::Id::InfantryAI).Ms / frames;
		g_aircraftAIMsPerFrame = snapshot.Get(PerfProbe::Id::AircraftAI).Ms / frames;
		g_animAIMsPerFrame = snapshot.Get(PerfProbe::Id::AnimAI).Ms / frames;
		g_logicUpdateMsPerFrame = snapshot.Get(PerfProbe::Id::LogicUpdate).Ms / frames;
		g_firstLoopMsPerFrame = 0.0;
		g_terrainLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::TerrainLoopAI).Ms / frames;
		g_buildingLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::BuildingLoopAI).Ms / frames;
		g_unitLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::UnitLoopAI).Ms / frames;
		g_infantryLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::InfantryLoopAI).Ms / frames;
		g_aircraftLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::AircraftLoopAI).Ms / frames;
		g_animLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::AnimLoopAI).Ms / frames;
		g_otherLoopAIMsPerFrame = snapshot.Get(PerfProbe::Id::OtherLoopAI).Ms / frames;
		g_lightAnimMsPerFrame = snapshot.Get(PerfProbe::Id::LightAnim).Ms / frames;
		g_tailTotalMsPerFrame = snapshot.Get(PerfProbe::Id::TailTotal).Ms / frames;
		g_raiseEventMsPerFrame = snapshot.Get(PerfProbe::Id::RaiseEvent).Ms / frames;
		g_cellDrawMsPerFrame = snapshot.Get(PerfProbe::Id::CellDraw).Ms / frames;
		g_cellScanMsPerFrame = snapshot.Get(PerfProbe::Id::CellScan).Ms / frames;
		g_arrayAI1MsPerFrame = snapshot.Get(PerfProbe::Id::ArrayAI1).Ms / frames;
		g_lightSourceMsPerFrame = snapshot.Get(PerfProbe::Id::LightSourceUpdate).Ms / frames;
		g_animUpdateAllMsPerFrame = snapshot.Get(PerfProbe::Id::AnimUpdateAll).Ms / frames;
		g_mapLightMsPerFrame = snapshot.Get(PerfProbe::Id::MapLightUpdate).Ms / frames;
		g_factoryAIMsPerFrame = snapshot.Get(PerfProbe::Id::FactoryAI).Ms / frames;
		g_houseAIMsPerFrame = snapshot.Get(PerfProbe::Id::HouseAI).Ms / frames;
		g_drawObjectMsPerFrame = snapshot.Get(PerfProbe::Id::DrawObject).Ms / frames;
		g_renderMsPerFrame = snapshot.Get(PerfProbe::Id::TacticalRender).Ms / frames;
		g_buildingDrawCallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::BuildingDraw).Calls / frames));
		g_unitAICallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::UnitAI).Calls / frames));
		g_infantryAICallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::InfantryAI).Calls / frames));
		g_aircraftAICallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::AircraftAI).Calls / frames));
		g_animAICallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::AnimAI).Calls / frames));
		g_raiseEventCallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::RaiseEvent).Calls / frames));
		g_cellDrawCallsPerFrame = static_cast<int>(std::lround(
			snapshot.Get(PerfProbe::Id::CellDraw).Calls / frames));

		WriteCsvRow(snapshot, frames, g_intervalMs);

		g_intervalMs = 0.0;
		g_intervalFrames = 0;
	}

	if ((g_frameCounter % 60) == 0)
	{
		g_lastAverageMs = AverageRecent(RecentFrames);
		g_lastFps = 1000.0 / std::max(g_lastAverageMs, 0.001);
		g_logicMsPerFrame = std::max(0.0, g_lastAverageMs - g_renderMsPerFrame);
		UpdateExtremes();
	}
}

void PerfOverlay::Draw()
{
	if (CurrentMode == Mode::Off)
		return;

	if (!DSurface::Composite || !TacticalClass::Instance)
		return;

	if (!ScenarioClass::Instance || !ScenarioClass::Instance->FileName[0])
		return;

	auto pSurface = DSurface::Composite;
	const auto bounds = pSurface->GetRect();

	if (bounds.Width <= 0 || bounds.Height <= 0)
		return;

	std::vector<std::wstring> lines;
	lines.reserve(8);

	wchar_t buffer[0x120] {};

	const double sessionAverage = (g_totalMs > 0.0 && g_frameCounter > 0)
		? static_cast<double>(g_frameCounter) * 1000.0 / g_totalMs
		: 0.0;

	if (CurrentMode == Mode::Simple)
	{
		swprintf(buffer, std::size(buffer), L"FPS: %d   Avg: %.1f   %.1f ms",
			static_cast<int>(std::lround(g_lastFps)), sessionAverage, g_lastFrameMs);
		lines.emplace_back(buffer);
	}
	else
	{
		swprintf(buffer, std::size(buffer), L"FPS %d | Avg %.1f | %.1f ms | 1%% low %d FPS | max %.1f ms",
			static_cast<int>(std::lround(g_lastFps)), sessionAverage, g_lastFrameMs,
			g_lastOnePercentLowMs > 0.0
				? static_cast<int>(std::lround(1000.0 / g_lastOnePercentLowMs))
				: 0,
			g_lastMaxMs);
		lines.emplace_back(buffer);

		swprintf(buffer, std::size(buffer), L"Logic %.1f | Render %.1f | LogicObj %d",
			g_logicMsPerFrame, g_renderMsPerFrame, LogicClass::Instance.Count);
		lines.emplace_back(buffer);

		swprintf(buffer, std::size(buffer), L"Bld %d | Anim %d | Terr %d | Cells %d | Zoom %.2f",
			BuildingClass::Array.Count, AnimClass::Array.Count,
			TerrainClass::Array.Count, MapClass::Instance.ValidMapCellCount,
			ZoomManager::CurrentZoom);
		lines.emplace_back(buffer);

		if (PerfProbe::Enabled)
		{
			swprintf(buffer, std::size(buffer), L"BldAI %.1f (gc %.1f) | Draw %d, %.1f | Event %.1f",
				g_buildingAIMsPerFrame, g_buildingAIHookMsPerFrame,
				g_buildingDrawCallsPerFrame, g_buildingDrawMsPerFrame,
				g_raiseEventMsPerFrame);
			lines.emplace_back(buffer);
		}
	}

	constexpr int left = 5;
	constexpr int padding = 4;
	constexpr int lineGap = 1;

	int textWidth = 0;
	int lineHeight = 0;

	for (const auto& line : lines)
	{
		const auto dimensions = Drawing::GetTextDimensions(line.c_str(), { 0, 0 }, 0, 2, 0);
		textWidth = std::max(textWidth, dimensions.Width);
		lineHeight = std::max(lineHeight, dimensions.Height);
	}

	const int blockWidth = textWidth + padding * 2;
	const int blockHeight = static_cast<int>(lines.size()) * lineHeight
		+ static_cast<int>(lines.size() - 1) * lineGap + padding * 2;

	// Anchor the counter to the right edge of the selected-unit status panel when
	// that panel is visible; otherwise keep the historical bottom-left position.
	int blockX = left;
	int blockY = std::max(5, bounds.Height - 65 - blockHeight - 6);
	{
		const auto panel = UnitStatusPanel::GetPanelRect();

		if (panel.Width > 0 && panel.Height > 0)
		{
			constexpr int gap = 8;
			blockX = panel.X + panel.Width + gap;
			blockY = panel.Y + panel.Height - blockHeight - 6;
		}
	}

	blockX = std::clamp(blockX, 5, std::max(5, bounds.Width - blockWidth - 5));
	blockY = std::clamp(blockY, 5, std::max(5, bounds.Height - blockHeight - 5));

	RectangleStruct background { blockX, blockY, blockWidth, blockHeight };
	ColorStruct black { 0, 0, 0 };
	pSurface->FillRectTrans(&background, &black, 45);

	COLORREF color = Drawing::RGB_To_Int(ColorStruct(255, 255, 255));

	if (g_lastFps > 0.0 && g_lastFps < 30.0)
		color = Drawing::RGB_To_Int(ColorStruct(255, 96, 96));
	else if (g_lastFps > 0.0 && g_lastFps < 50.0)
		color = Drawing::RGB_To_Int(ColorStruct(255, 220, 96));

	int y = blockY + padding;

	for (const auto& line : lines)
	{
		DrawTextLine(pSurface, bounds, line.c_str(), blockX + padding, y, color);
		y += lineHeight + lineGap;
	}

	if (CurrentMode == Mode::Detailed)
	{
		constexpr int graphWidth = 180;
		constexpr int graphHeight = 24;
		constexpr int graphSamples = 60;
		constexpr int barWidth = 2;
		constexpr int barStep = 3;
		constexpr float graphMaxMs = 50.0f;

		const int graphX = blockX;
		const int graphY = std::max(5, blockY - graphHeight - 6);

		RectangleStruct graphBackground { graphX, graphY, graphWidth + 2, graphHeight + 2 };
		pSurface->FillRectTrans(&graphBackground, &black, 35);

		const int samples = std::min(graphSamples, g_count);

		for (int i = 0; i < samples; ++i)
		{
			const int index = (g_head - samples + i + RingSize * 2) % RingSize;
			const float milliseconds = g_frameMs[index];
			const int height = std::clamp(
				static_cast<int>(milliseconds / graphMaxMs * graphHeight), 1, graphHeight);

			COLORREF barColor = Drawing::RGB_To_Int(ColorStruct(96, 220, 96));

			if (milliseconds > 33.0f)
				barColor = Drawing::RGB_To_Int(ColorStruct(255, 96, 96));
			else if (milliseconds > 16.7f)
				barColor = Drawing::RGB_To_Int(ColorStruct(255, 220, 96));

			RectangleStruct bar {
				graphX + 1 + i * barStep,
				graphY + 1 + graphHeight - height,
				barWidth,
				height
			};
			pSurface->FillRect(&bar, barColor);
		}
	}
}
