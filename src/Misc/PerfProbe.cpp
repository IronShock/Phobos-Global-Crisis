#include "PerfProbe.h"

#include <Utilities/Macro.h>
#include <Windows.h>

bool PerfProbe::Enabled = false;
double PerfProbe::LastLogicMs = 0.0;
double PerfProbe::LastRenderMs = 0.0;

namespace
{
	struct Timer
	{
		PerfProbe::Stat Stat;
		double Start;
		int Depth;
	};

	Timer g_timers[static_cast<int>(PerfProbe::Id::Count)];

	Timer& GetTimer(PerfProbe::Id id)
	{
		return g_timers[static_cast<int>(id)];
	}
}

double PerfProbe::NowMs()
{
	static LARGE_INTEGER frequency {};
	static LARGE_INTEGER origin {};
	static bool initialized = false;

	if (!initialized)
	{
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&origin);
		initialized = true;
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	return static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0
		/ static_cast<double>(frequency.QuadPart);
}

void PerfProbe::Enter(Id id)
{
	const bool alwaysOn = (id == Id::LogicUpdate || id == Id::TacticalRender);

	if (!Enabled && !alwaysOn)
		return;

	auto& timer = GetTimer(id);

	if (timer.Depth++ == 0)
		timer.Start = NowMs();
}

void PerfProbe::Exit(Id id)
{
	const bool alwaysOn = (id == Id::LogicUpdate || id == Id::TacticalRender);

	if (!Enabled && !alwaysOn)
		return;

	auto& timer = GetTimer(id);

	if (timer.Depth <= 0)
		return;

	if (--timer.Depth == 0)
	{
		const double ms = NowMs() - timer.Start;
		timer.Stat.Ms += ms;
		++timer.Stat.Calls;

		if (id == Id::LogicUpdate)
			LastLogicMs = ms;
		else if (id == Id::TacticalRender)
			LastRenderMs = ms;
	}
}

void PerfProbe::Add(Id id, double ms)
{
	if (!Enabled)
		return;

	auto& timer = GetTimer(id);
	timer.Stat.Ms += ms;
	++timer.Stat.Calls;
}

PerfProbe::Snapshot PerfProbe::TakeSnapshot()
{
	Snapshot snapshot {};

	for (int i = 0; i < static_cast<int>(Id::Count); ++i)
	{
		snapshot.Stats[i] = g_timers[i].Stat;
		g_timers[i].Stat = {};
	}

	return snapshot;
}

