#pragma once

// Keyboard-side bindings for the MOEE modules.
//
// The mouse button of each combo stays fixed in code (Middle for most modules,
// X2 for Attack Move). Only the keyboard part is exposed as a CommandClass so
// it shows up in the in-game keyboard settings (Options -> Keyboard) and is
// stored in KeyboardMD.ini -> [Hotkey].
//
// A binding can be empty (mouse only), a modifier-only binding (e.g. Ctrl), or
// a full combination (e.g. Ctrl+Z). The modules read the binding every frame
// and combine it with their fixed mouse button.

namespace MOEE
{
	namespace CommandNames
	{
		constexpr const char* AttackMove = "MOEE Attack Move";
		constexpr const char* TreeChop = "MOEE Tree Chop";
		constexpr const char* FuzzyAttack = "MOEE Fuzzy Attack";
		constexpr const char* SmartOrder = "MOEE Smart Order";
		constexpr const char* PatrolCircle = "MOEE Patrol Circle";
		constexpr const char* SmartArea = "MOEE Smart Area";
		constexpr const char* DeployPreview = "MOEE Deploy Preview";
		constexpr const char* WeaponRange = "MOEE Weapon Range";
		constexpr const char* RepairAll = "MOEE Repair All";
	}

	struct KeyPart
	{
		unsigned int mods = 0; // 1 = Ctrl, 2 = Shift, 4 = Alt
		unsigned int vk = 0;   // extra non-modifier key, 0 = none

		bool Empty() const { return mods == 0 && vk == 0; }
	};

	// Registers the MOEE commands so they appear in the in-game keyboard UI.
	void RegisterCommands();

	// Reads the keyboard part of a command binding (KeyboardMD.ini -> [Hotkey]).
	// Falls back to reading the ini directly for modifier-only bindings that the
	// engine may not put into its hotkey table.
	KeyPart GetKeyPart(const char* commandName);

	// All required keys/modifiers are held (extra modifiers are allowed).
	bool KeyPartHeld(const KeyPart& part);

	// The held modifiers match the binding exactly and its extra key is held.
	bool KeyPartHeldExact(const KeyPart& part);

	// True when another, strictly more specific MOEE mouse combo is fully held
	// (e.g. Ctrl+Z claims Ctrl+Middle for Tree Chop).
	bool MouseComboClaimed(const char* selfName);
}
