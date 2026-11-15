#pragma once

// Debug-only automated check for the aircraft carrier deck, enabled with
// Debug.CarrierSelfTest=yes in RA2MD.ini [Phobos]. It spawns a carrier on the
// first water cell it finds, forces a payload into the return path and writes
// a PASS/FAIL verdict to debug.log. It exists so the deck can be tested without
// manual play; it never runs unless the flag is set.
namespace CarrierSelfTest
{
	void Start();
	void Tick();
}
