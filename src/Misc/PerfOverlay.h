#pragma once

// On-screen FPS / frame-time monitor plus the CSV perf log.
//
// Toggled by the "New FPS Counter" hotkey (Options -> Keyboard -> Interface):
//   Off -> Simple (FPS + frame ms) -> Detailed (+ 1% low, logic/render split,
//   building draw/AI time, object counts, frame-time graph) -> Off
//
// Purely client-side rendering: no simulation state, no RNG, no sync impact.

class PerfOverlay
{
public:
	enum class Mode
	{
		Off = 0,
		Simple = 1,
		Detailed = 2
	};

	static Mode CurrentMode;

	// Called once after [Phobos] has been read. `show` is the legacy ShowFPS key.
	static void InitFromConfig(bool show);

	// Cycles Off -> Simple -> Detailed -> Off.
	static void Toggle();

	// Once per rendered frame, from the GScreenClass_DrawText hook.
	static void OnFrame();

	// Draws the overlay (no-op when Off).
	static void Draw();

	// Drops the ring buffer so a scenario load hitch does not pollute the stats.
	static void Reset();
};
