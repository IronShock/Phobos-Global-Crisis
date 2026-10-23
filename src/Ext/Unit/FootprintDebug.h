#pragma once

// Client-side debug overlay: draws the authoritative occupied cells of every
// multi-cell unit on the tactical map. It reads FootprintState directly, so the
// outline is exactly what the collision layer sees.
namespace FootprintDebug
{
	void Draw();
}
