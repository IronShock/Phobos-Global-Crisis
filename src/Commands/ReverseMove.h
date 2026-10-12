#pragma once

#include "Commands.h"
#include <set>

// Hold-to-reverse command: when the bound key is held down, units with
// AdvancedDrive.Reverse will drive backwards instead of turning around.
// The key binding is configurable in Options -> Keyboard -> Control.
// Once a unit starts reversing due to the hotkey, it will continue
// reversing until it reaches its destination, even after the key is released.
class ReverseMoveCommandClass : public CommandClass
{
public:
	// Tracks whether the bound key is currently held.
	static inline bool IsHeld = false;

	// The last game frame the key was observed down. Used together with a
	// grace period so that a quick release right after issuing a move order
	// still counts as "held while commanding".
	static inline int LastHeldFrame = -1;

	// The virtual key code bound to this command, discovered on first press.
	static inline int BoundVK = 0;

	// Units that started reversing due to the hotkey and should continue
	// reversing until they reach their destination, even after key release.
	static inline std::set<void*> LockedUnits;

	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;

	virtual void Execute(WWKey eInput) const override;

	// Queries the actual Windows key state and updates IsHeld.
	// Call this every frame before relying on IsHeld.
	static void UpdateHeldState();

	// Returns true when the key is held now, or was held within the last
	// `graceFrames` frames. This prevents the reverse order from being lost
	// when the player releases the key right after commanding a move.
	static bool IsEffectivelyHeld(int graceFrames = 15);
};
