#include <Phobos.h>

#include <GameOptionsClass.h>

#include <Ext/Rules/Body.h>
#include <Utilities/Macro.h>

// Locks the in-game game speed adjustment at the difficulties configured in
// [General] -> LockGameSpeed.Difficulty. The game speed is stored in
// GameOptionsClass::Instance.GameSpeed (0xA8EB60). The player changes it via
// the pause-menu / Game Controls speed sliders, which queue a message that is
// later applied by writing GameSpeed. Because the change can arrive through
// several paths, we do a per-frame check in the main loop (before
// LogicClass::Update): while the speed is locked, any change to GameSpeed is
// reverted back to the speed captured when the lock became active. This keeps
// the speed fully frozen regardless of how the player tries to change it.

namespace GameSpeedLockTemp
{
	static int LockedGameSpeed = -1;
	static bool LockActive = false;
}

// Main loop, right before LogicClass::Update - runs every frame. The patched
// `mov ecx, 0x87F778h` (5 bytes) is replicated on return.
DEFINE_HOOK(0x55DC99, GameSpeedLock_PerFrame, 0x5)
{
	if (RulesExt::Global()->IsGameSpeedLocked())
	{
		if (!GameSpeedLockTemp::LockActive)
		{
			GameSpeedLockTemp::LockActive = true;
			GameSpeedLockTemp::LockedGameSpeed = GameOptionsClass::Instance.GameSpeed;
		}
		else if (GameOptionsClass::Instance.GameSpeed != GameSpeedLockTemp::LockedGameSpeed)
		{
			GameOptionsClass::Instance.GameSpeed = GameSpeedLockTemp::LockedGameSpeed;
		}
	}
	else
	{
		GameSpeedLockTemp::LockActive = false;
		GameSpeedLockTemp::LockedGameSpeed = -1;
	}

	R->ECX(0x87F778);
	return 0;
}
