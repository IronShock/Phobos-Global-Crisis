#pragma once
#include <Windows.h>

// Port of MagicShiba/MO_ExperienceEnhancements into Phobos.
//
// Every module keeps its original hook/patch addresses (both projects target the
// same YR 1.001 gamemd.exe build), but the code is rewritten to use Phobos'
// hook, config and logging infrastructure instead of MO_EE's mingw inline-asm /
// custom ini layer. Per-module toggles live in RA2MD.ini -> [Phobos] with the
// "MOEE." prefix (see Phobos::Config).
namespace MOEE
{
	// Shared helpers, replacing MO_EE's GdiText.h / OrderUtil.h equivalents.
	bool IsPlausiblePtr(const void* p);
	bool IsMultiplayer();
	void Log(const char* pTag, const char* pFormat, ...);

	// Per-frame tick, driven from Hooks.MOEE.cpp (LogicClass::Update 0x55B4E1).
	void Poll();

	// Render-phase overlay pass, driven from Hooks.MOEE.cpp
	// (TacticalClass::Draw 0x6D4684). Only preview lines for now.
	void Draw();

	// Modules. Each is self-gating on its own Phobos::Config flag.
	namespace MegaListCap { void Poll(); }   // EventClass::MegaMissionList 256 -> 4096
	namespace Observer { void Poll(); }      // multi-observer fix + panel sort
	namespace WaypointCap                     // single-player waypoint limits (16/128)
	{
		void Poll();
		void LogRefuse(int kind, unsigned int count, unsigned int cap);
	}
	namespace AttackMove { void Poll(); }     // custom attack-move hotkey
	namespace RepairAll                        // Ctrl/double-click/automatic repair
	{
		void Poll();
		bool TriggerHook(bool fromButton);
	}
	namespace PlaceScatter { void Poll(); }   // clear units out of the building footprint
	namespace TreeChop { void Poll(); void Draw(); } // Ctrl+Z+Middle quick tree chopping
	namespace FuzzyAttack { void Poll(); void Draw(); } // Ctrl+Middle fuzzy force-attack
	namespace SmartOrder { void Poll(); void Draw(); }  // bare Middle context-sensitive order / scatter
	namespace PatrolCircle { void Poll(); void Draw(); } // Z+Middle patrol ring
	namespace SmartArea { int ClaimsMiddle(); void Poll(); void Draw(); } // Shift+Middle area-context order
	namespace DeployPreview { int ClaimsMiddle(); void Poll(); void Draw(); } // Middle base-deploy preview
	namespace WeaponRange { void Draw(); }   // Ctrl + single unit: weapon range ellipse
	namespace GridView { void Poll(); void Draw(); } // toggleable tactical ground grid
	namespace ScaleBar { void Draw(); }      // bottom-bar zoom scale bar (ZoomManager)
	namespace VecText { void EnsureInit(); }  // global vector text
}
