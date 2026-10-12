#include "TacticalPause.h"

#include <Utilities/GeneralUtils.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Unsorted.h>

#include <cstring>

#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SuperClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <WWMouseClass.h>

#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>

bool TacticalPauseCommandClass::IsActive = false;

// Freeze the Phobos custom (multi-)charge timers while time is stopped. Their
// charge is derived from "Unsorted::CurrentFrame - Charge_RechargeSlots[i]", and
// CurrentFrame keeps advancing during the time-stop, so we advance every charging
// slot in lock-step, keeping the elapsed time (and thus the charge) constant.
static void FreezeChargeSlots()
{
	for (auto const pHouse : HouseClass::Array)
	{
		if (auto* const pExt = HouseExt::ExtMap.Find(pHouse))
		{
			for (auto& swExt : pExt->SuperExts)
			{
				for (auto& slot : swExt.Charge_RechargeSlots)
				{
					if (slot >= 0)
						++slot;
				}
			}
		}
	}

	// The cameo countdown is driven by RechargeTimer, which the custom-charge AI
	// (not running while paused) keeps in sync with the slots. Advance its start
	// frame in lock-step too, so the displayed cooldown also stays frozen.
	for (auto const pSuper : SuperClass::Array)
	{
		if (pSuper->RechargeTimer.IsTicking())
			++pSuper->RechargeTimer.StartTime;
	}
}

// Pause/resume the Scenario frame-based timers (mission countdown, environment)
// while time is stopped. ScenarioClass::ElapsedTimer is deliberately NOT touched:
// the top-right "Operation Time Elapsed" is derived from it plus SystemTimer
// (real time) and must keep running. (.ElapsedTimer is not in the list below.)
static void SetScenarioTimersPaused(bool paused)
{
	const auto handle = [paused](CDTimerClass& timer)
	{
		if (paused)
		{
			if (timer.IsTicking())
				timer.Pause();
		}
		else if (timer.StartTime == -1 && timer.TimeLeft > 0)
		{
			timer.Resume();
		}
	};

	if (auto* const pScenario = ScenarioClass::Instance)
	{
		handle(pScenario->MissionTimer);
		handle(pScenario->ShroudRegrowTimer);
		handle(pScenario->FogTimer);
		handle(pScenario->IceTimer);
		handle(pScenario->unknown_timer_123c);
		handle(pScenario->AmbientTimer);
	}
}

const char* TacticalPauseCommandClass::GetName() const
{
	return "TacticalPause";
}

const wchar_t* TacticalPauseCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_PAUSE", L"Tactical Time Stop");
}

const wchar_t* TacticalPauseCommandClass::GetUICategory() const
{
	return CATEGORY_CONTROL;
}

const wchar_t* TacticalPauseCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_PAUSE_DESC",
		L"Freeze the battlefield while still issuing orders. Press again to resume.");
}

void TacticalPauseCommandClass::Execute(WWKey eInput) const
{
	if (!SessionClass::IsSingleplayer())
		return;

	IsActive = !IsActive;
}

void TacticalPauseCommandClass::Reset()
{
	IsActive = false;
}

bool TacticalPauseCommandClass::IsBlockingActions()
{
	if (!IsActive)
		return false;

	auto const pRules = RulesExt::Global();
	const bool enabled = pRules && pRules->IsTacticalPauseBlockActionsEnabled();

	// Diagnostic (throttled): confirm the difficulty gate at runtime.
	static int s_lastLogFrame = -100000;

	if (Unsorted::CurrentFrame - s_lastLogFrame >= 120)
	{
		s_lastLogFrame = Unsorted::CurrentFrame;
		Debug::Log("[Phobos::TacticalPause] BlockActions: IsActive=%d difficulty=%d enabled=%d\n",
			IsActive ? 1 : 0, GameOptionsClass::Instance.Difficulty, enabled ? 1 : 0);
	}

	return enabled;
}

// [General] TacticalPause.BlockActions: while the time-stop is active and the
// option is on, no command may execute from the keyboard. Hooked right before
// the virtual CommandClass::Execute call (0x55E015) in Game::KeyboardProcess;
// ESI holds the resolved command. The TacticalPause command itself is allowed so
// the player can resume; keys that are not commands (ESC/menu, camera, etc.) do
// not reach this site and keep working.
DEFINE_HOOK(0x55E012, Game_KeyboardProcess_TacticalPauseBlock, 0x6)
{
	enum { SkipExecute = 0x55E018 };

	GET(CommandClass* const, pCommand, ESI);

	if (TacticalPauseCommandClass::IsBlockingActions() && pCommand)
	{
		const char* const pName = pCommand->GetName();

		// Allow the time-stop toggle itself and ESC (the vanilla "Options" command,
		// Keyboard.ini Options=27) so the player can resume and open the menu.
		const DWORD key = R->EAX();
		const bool isEscape = (key & 0xFF) == 0x1B; // VK_ESCAPE

		if (!isEscape && (!pName || std::strcmp(pName, "TacticalPause") != 0))
		{
			static int s_lastKeyLog = -100000;

			if (Unsorted::CurrentFrame - s_lastKeyLog >= 120)
			{
				s_lastKeyLog = Unsorted::CurrentFrame;
				Debug::Log("[Phobos::TacticalPause] keyboard block: key=%X cmd=%s\n",
					key, pName ? pName : "<null>");
			}

			return SkipExecute;
		}
	}

	return 0;
}

// [General] TacticalPause.BlockActions: while blocking, make every default
// gadget "not clicked" so its Action never runs. GadgetClass::Clicked (0x4E13F0)
// is the per-gadget hit test that runs AFTER GadgetClass::Input has read the
// input, so the keyboard/hotkeys (and the ability to leave the time-stop) keep
// working. It is __thiscall with 5 stack args (ret 0x14) returning bool in AL,
// so the thunk returns false and cleans the arguments. This disables the vanilla
// sidebar build/production cameos, the vanilla and Phobos superweapon sidebars,
// the command bar and the message column. Game::SpecialDialog != 0 lets gadgets
// pass through when a dialog is open.
static void __declspec(naked) GadgetClass_Clicked_BlockThunk()
{
	_asm { xor al, al }
	_asm { ret 0x14 }
}

// Size 0xA covers the two whole instructions at the entry
// (`mov eax,[esp+8]` 4 bytes + `mov edx,[0x8b3e88]` 6 bytes) so returning 0
// re-executes them and resumes on an instruction boundary.
DEFINE_HOOK(0x4E13F0, GadgetClass_Clicked_TacticalPauseBlock, 0xA)
{
	if (TacticalPauseCommandClass::IsBlockingActions() && Game::SpecialDialog == 0)
	{
		static int s_lastGadgetLog = -100000;

		if (Unsorted::CurrentFrame - s_lastGadgetLog >= 120)
		{
			s_lastGadgetLog = Unsorted::CurrentFrame;
			Debug::Log("[Phobos::TacticalPause] GadgetClicked blocked\n");
		}

		return reinterpret_cast<uintptr_t>(&GadgetClass_Clicked_BlockThunk);
	}

	return 0;
}

// Clear the toggle whenever a scenario is (re)loaded so that quitting while
// time-stopped, or loading a save, cannot leave the game frozen.
DEFINE_HOOK(0x68AD2F, ScenarioClass_LoadFromINI_TacticalPauseReset, 0x5)
{
	TacticalPauseCommandClass::Reset();
	return 0;
}

// Also clear it when the mission is won, so the score screen and the following
// missions never start up already frozen.
DEFINE_HOOK(0x685D80, DoWin_TacticalPauseReset, 0x6)
{
	TacticalPauseCommandClass::Reset();
	return 0;
}

// Tactical view fields (offsets verified against gamemd.exe). FocusOn (0x6D2420)
// writes +0xC8/+0xCC (start) and +0xD0/+0xD4 (destination) and TacticalClass::Update
// (0x6D2540) interpolates between them using +0xD8 (rate) and +0xDC (progress),
// writing the result to +0xD64/+0xD68 (current view centre).
static Point2D* Tactical_Start(TacticalClass* pTactical)
{
	return reinterpret_cast<Point2D*>(reinterpret_cast<char*>(pTactical) + 0xC8);
}

static Point2D* Tactical_Destination(TacticalClass* pTactical)
{
	return reinterpret_cast<Point2D*>(reinterpret_cast<char*>(pTactical) + 0xD0);
}

static float* Tactical_ScrollRate(TacticalClass* pTactical)
{
	return reinterpret_cast<float*>(reinterpret_cast<char*>(pTactical) + 0xD8);
}

static float* Tactical_ScrollProgress(TacticalClass* pTactical)
{
	return reinterpret_cast<float*>(reinterpret_cast<char*>(pTactical) + 0xDC);
}

static Point2D* Tactical_CurrentCenter(TacticalClass* pTactical)
{
	return reinterpret_cast<Point2D*>(reinterpret_cast<char*>(pTactical) + 0xD64);
}

// The "pending" view offset that TacticalClass::Scroll (0x6D8530) adds its delta
// to and TacticalClass::Update reconciles back into +0xD64.
static Point2D* Tactical_PendingCenter(TacticalClass* pTactical)
{
	return reinterpret_cast<Point2D*>(reinterpret_cast<char*>(pTactical) + 0xD74);
}

// Move the view by an explicit screen-space delta (used for the arrow keys).
static void MoveViewByScreenDelta(TacticalClass* pTactical, int dx, int dy)
{
	Point2D target = *Tactical_CurrentCenter(pTactical);
	target.X += dx;
	target.Y += dy;

	reinterpret_cast<bool(__thiscall*)(TacticalClass*, Point2D*)>(0x6D8640)(pTactical, &target);

	*Tactical_CurrentCenter(pTactical) = target;
	*Tactical_PendingCenter(pTactical) = target;
	*Tactical_Start(pTactical) = target;
	*Tactical_Destination(pTactical) = target;
	*Tactical_ScrollRate(pTactical) = 0.0f;
	*Tactical_ScrollProgress(pTactical) = 0.0f;

	reinterpret_cast<void(__thiscall*)(TacticalClass*)>(0x6D8B30)(pTactical);
	*reinterpret_cast<byte*>(reinterpret_cast<char*>(pTactical) + 0xD7D) = 1;
}

// While time is stopped the engine's own camera scrolling does not run (it is
// driven by the logic frame), so drive it ourselves.
static void ApplyTimeStopCameraScroll()
{
	auto* const pTactical = TacticalClass::Instance;

	if (!pTactical)
		return;

	const int rate = GameOptionsClass::Instance.ScrollRate;
	const int step = 8 + rate * 2;
	int dx = 0;
	int dy = 0;

	// Keyboard (arrow keys)
	if (GetAsyncKeyState(VK_LEFT) & 0x8000)  dx -= step;
	if (GetAsyncKeyState(VK_RIGHT) & 0x8000) dx += step;
	if (GetAsyncKeyState(VK_UP) & 0x8000)    dy -= step;
	if (GetAsyncKeyState(VK_DOWN) & 0x8000)  dy += step;

	// Right mouse drag: bind the view to the cursor 1:1 (the camera moves exactly
	// as fast as the player drags), so the position is well defined and never
	// drifts. The camera is then locked to it below.
	static Point2D s_lastMouse {};
	static bool s_lastMouseValid = false;
	static bool s_dragging = false;
	bool dragging = false;

	if (WWMouseClass::Instance)
	{
		// The cursor position is only refreshed once per frame after the map render;
		// refresh it now so we use the current position.
		reinterpret_cast<void(__thiscall*)(WWMouseClass*)>(0x7BA090)(WWMouseClass::Instance);

		const int mx = WWMouseClass::Instance->GetX();
		const int my = WWMouseClass::Instance->GetY();

		const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

		// Edge scrolling: push the cursor against a screen edge (measured against the
		// whole window so the right/bottom portions include the sidebar area).
		if (GameOptionsClass::Instance.AutoScroll && !rightDown)
		{
			RectangleStruct win = DSurface::WindowBounds;

			if (win.Width <= 0 || win.Height <= 0)
				win = DSurface::ViewBounds;

			const int margin = 16;

			if (mx <= win.X + margin)                    dx -= step;
			else if (mx >= win.X + win.Width - margin)   dx += step;

			if (my <= win.Y + margin)                    dy -= step;
			else if (my >= win.Y + win.Height - margin)  dy += step;
		}

		dragging = rightDown;
		if (rightDown)
		{
			if (s_lastMouseValid && s_dragging)
			{
				dx += mx - s_lastMouse.X;
				dy += my - s_lastMouse.Y;
			}
			s_dragging = true;
		}
		else
		{
			s_dragging = false;
		}

		s_lastMouse.X = mx;
		s_lastMouse.Y = my;
		s_lastMouseValid = true;
	}

	// Re-assert (lock) the camera every paused frame while dragging, even if the
	// cursor did not move this frame, so nothing can shift it out from under the
	// player and no lag/drift can build up.
	if (dx || dy || dragging)
		MoveViewByScreenDelta(pTactical, dx, dy);
}

// Apply the camera pan right after the game has processed this frame's input
// (MapClass::GetInputAndUpdate / Game::KeyboardProcess, 0x55D8AB..0x55D8B4) and
// immediately before the tactical map render (0x55D8F2 -> GScreenClass::Render).
// Reading the mouse here - instead of at the top of the frame - gives the smallest
// possible input-to-display latency, so the view tracks the cursor without the
// visible lag it had when updated from the LogicClass::Update redirect (0x55DC9E,
// which runs after the render).
DEFINE_HOOK(0x55D8ED, TacticalPause_ApplyCameraScrollBeforeRender, 0x5)
{
	if (TacticalPauseCommandClass::IsActive)
	{
		FreezeChargeSlots();
		ApplyTimeStopCameraScroll();
	}

	return 0;
}

// Core of the tactical time-stop: replace the single LogicClass::Update() call
// in Game::MainLoop (0x55DC9E: call 0055AFB0) with a redirect that simply skips
// the update while active. Everything else in the vanilla frame keeps running
// (input, camera, selection, action lines, radar, sidebar, messages), so only
// the simulation is frozen.
//
// NOTE: a plain DEFINE_HOOK at this site is not safe because the instruction is
// a rel32 `call`; Syringe copies the 5 bytes into a trampoline without fixing
// up the relative target, which crashes. Patching the call target directly via
// DEFINE_FUNCTION_JUMP avoids that entirely.
static void __fastcall TacticalPause_LogicUpdateRedirect(void* pThis)
{
	// Pause/resume the superweapon recharge timers on the toggle.
	static bool s_WasActive = false;
	if (TacticalPauseCommandClass::IsActive != s_WasActive)
	{
		s_WasActive = TacticalPauseCommandClass::IsActive;
		SetScenarioTimersPaused(s_WasActive);
	}

	if (!TacticalPauseCommandClass::IsActive)
	{
		reinterpret_cast<void(__fastcall*)(void*)>(0x55AFB0)(pThis);
		return;
	}

	for (auto const pTechno : TechnoClass::Array)
	{
		// Unsorted::CurrentFrame keeps advancing every frame (it is incremented in
		// Game::MainLoop at 0x55DE73, which is not skipped), and FacingClass::Current()
		// interpolates a rotating hull/turret from a frame-based timer. Pin each facing
		// to its current interpolated value so units can neither finish an
		// in-progress turn nor start a new one while time is stopped. AI re-issues the
		// desired facing once the time-stop is released.
		//
		// NOTE: FacingClass::SetCurrent(x) only updates DesiredFacing/StartFacing when
		// x differs from Current(); calling SetCurrent(Current()) therefore just stops
		// the timer and makes Current() fall through to the old DesiredFacing, which
		// snapped a rotating turret straight to its target. Assign the members directly.
		const auto freezeFacing = [](FacingClass& facing)
		{
			const DirStruct current = facing.Current();
			facing.DesiredFacing = current;
			facing.StartFacing = current;
			facing.RotationTimer.Start(0);
		};

		freezeFacing(pTechno->PrimaryFacing);
		freezeFacing(pTechno->SecondaryFacing);
		freezeFacing(pTechno->BarrelFacing);

		// TechnoClass::AI (normally run from LogicClass::Update) is what clears the
		// per-object mouse-hover flag every frame. Without it, a hovered unit's
		// health bar would linger after the cursor leaves it, so clear it here. The
		// engine's hover detection re-sets it for the object under the cursor, which
		// happens before the tactical view is drawn.
		pTechno->IsMouseHovering = false;
	}

	// NOTE: do NOT force a full map redraw here. MapClass::MarkNeedsRedraw(2) every
	// paused frame halved the frame rate (60 -> 30 fps), which is what made the
	// camera pan stutter. The camera move itself marks the view dirty (0x6D8B30 +
	// +0xD7D) and the engine redraws the changed regions on its own.
}
DEFINE_FUNCTION_JUMP(CALL, 0x55DC9E, TacticalPause_LogicUpdateRedirect);
