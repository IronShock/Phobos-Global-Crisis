#pragma once

// Client-side entry point. Called once per frame from GScreenClass_DrawText.
// LoadBar owns no serialized state: all configuration is read from rulesmd.ini
// at runtime and all values are read from existing engine/Phobos timers.
class LoadBarManager
{
public:
	static void DrawAll();
};
