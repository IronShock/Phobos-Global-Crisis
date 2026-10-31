#include "MOEE.Commands.h"

#include <Commands/Commands.h>
#include <CommandClass.h>
#include <Phobos.h>
#include <Utilities/GeneralUtils.h>

#include <Windows.h>
#include <cstring>

namespace
{
	struct CommandInfo
	{
		const char* name;
		const char* txtName;
		const wchar_t* uiFallback;
		const char* txtDescription;
		const wchar_t* descriptionFallback;
	};

	const CommandInfo CommandInfos[] =
	{
		{
			MOEE::CommandNames::AttackMove,
			"TXT_MOEE_ATTACK_MOVE", L"Attack Move",
			"TXT_MOEE_ATTACK_MOVE_DESC",
			L"Fixed mouse button: X2 (side button). The keyboard part is optional."
		},
		{
			MOEE::CommandNames::TreeChop,
			"TXT_MOEE_TREE_CHOP", L"Tree Chop",
			"TXT_MOEE_TREE_CHOP_DESC",
			L"Fixed mouse button: Middle. Hold the keyboard part (default Ctrl+Z) and click the middle button."
		},
		{
			MOEE::CommandNames::FuzzyAttack,
			"TXT_MOEE_FUZZY_ATTACK", L"Fuzzy Attack",
			"TXT_MOEE_FUZZY_ATTACK_DESC",
			L"Fixed mouse button: Middle. Hold the keyboard part (default Ctrl) and click the middle button."
		},
		{
			MOEE::CommandNames::SmartOrder,
			"TXT_MOEE_SMART_ORDER", L"Smart Order",
			"TXT_MOEE_SMART_ORDER_DESC",
			L"Fixed mouse button: Middle. The keyboard part is empty by default."
		},
		{
			MOEE::CommandNames::PatrolCircle,
			"TXT_MOEE_PATROL_CIRCLE", L"Patrol Circle",
			"TXT_MOEE_PATROL_CIRCLE_DESC",
			L"Fixed mouse button: Middle. Hold the keyboard part (default Shift+Z) and drag the middle button."
		},
		{
			MOEE::CommandNames::SmartArea,
			"TXT_MOEE_SMART_AREA", L"Smart Area",
			"TXT_MOEE_SMART_AREA_DESC",
			L"Fixed mouse button: Middle. Hold the keyboard part (default Shift) and click the middle button."
		},
		{
			MOEE::CommandNames::DeployPreview,
			"TXT_MOEE_DEPLOY_PREVIEW", L"Deploy Preview",
			"TXT_MOEE_DEPLOY_PREVIEW_DESC",
			L"Fixed mouse button: Middle. The keyboard part is empty by default."
		},
		{
			MOEE::CommandNames::WeaponRange,
			"TXT_MOEE_WEAPON_RANGE", L"Weapon Range",
			"TXT_MOEE_WEAPON_RANGE_DESC",
			L"Hold the keyboard part (default Ctrl+Shift) to display the selected unit's weapon range."
		},
		{
			MOEE::CommandNames::RepairAll,
			"TXT_MOEE_REPAIR_ALL", L"Repair All",
			"TXT_MOEE_REPAIR_ALL_DESC",
			L"Hold the keyboard part (default Ctrl+Alt) and use the repair button to repair every damaged building."
		},
	};

	template <int Index>
	class MOEECommandClass : public CommandClass
	{
	public:
		const char* GetName() const override
		{
			return CommandInfos[Index].name;
		}

		const wchar_t* GetUIName() const override
		{
			return GeneralUtils::LoadStringUnlessMissing(
				CommandInfos[Index].txtName, CommandInfos[Index].uiFallback);
		}

		const wchar_t* GetUICategory() const override
		{
			return L"MOEE";
		}

		const wchar_t* GetUIDescription() const override
		{
			return GeneralUtils::LoadStringUnlessMissing(
				CommandInfos[Index].txtDescription, CommandInfos[Index].descriptionFallback);
		}

		// The commands are binding holders only: the actual actions stay in the
		// per-frame MOEE modules, which combine the binding with a fixed mouse
		// button. Nothing to execute here.
		void Execute(WWKey) const override { }
	};

	// Mouse-based combos that participate in the "more specific binding wins"
	// rule. Weapon Range and Repair All are keyboard-only and excluded.
	const char* const MouseComboNames[] =
	{
		MOEE::CommandNames::TreeChop,
		MOEE::CommandNames::FuzzyAttack,
		MOEE::CommandNames::PatrolCircle,
		MOEE::CommandNames::SmartArea,
	};
}

void MOEE::RegisterCommands()
{
	MakeCommand<MOEECommandClass<0>>();
	MakeCommand<MOEECommandClass<1>>();
	MakeCommand<MOEECommandClass<2>>();
	MakeCommand<MOEECommandClass<3>>();
	MakeCommand<MOEECommandClass<4>>();
	MakeCommand<MOEECommandClass<5>>();
	MakeCommand<MOEECommandClass<6>>();
	MakeCommand<MOEECommandClass<7>>();
	MakeCommand<MOEECommandClass<8>>();
}

MOEE::KeyPart MOEE::GetKeyPart(const char* commandName)
{
	// Cache the result: the hotkey table is fixed for the process lifetime and a
	// per-frame GetPrivateProfileIntA() fallback would hit the disk constantly.
	struct CacheEntry
	{
		char name[64];
		KeyPart part;
	};

	static CacheEntry cache[16] {};
	static int cacheCount = 0;

	for (int i = 0; i < cacheCount; ++i)
	{
		if (_stricmp(cache[i].name, commandName) == 0)
			return cache[i].part;
	}

	unsigned int raw = 0;
	bool found = false;

	for (auto const& node : CommandClass::Hotkeys)
	{
		if (node.Data && node.Data->GetName()
			&& _stricmp(node.Data->GetName(), commandName) == 0)
		{
			raw = node.ID;
			found = true;
			break;
		}
	}

	if (!found)
	{
		// Relative path resolves against the game directory (the process CWD).
		raw = static_cast<unsigned int>(
			GetPrivateProfileIntA("Hotkey", commandName, 0, "KeyboardMD.ini"));
	}

	KeyPart part;

	if (raw & 0x100u) part.mods |= 2u; // WWKey::Shift
	if (raw & 0x200u) part.mods |= 1u; // WWKey::Ctrl
	if (raw & 0x400u) part.mods |= 4u; // WWKey::Alt

	unsigned int vk = raw & 0xFFu;

	// A pure modifier can be stored either as a WWKey flag or as its VK code.
	switch (vk)
	{
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL:
		part.mods |= 1u;
		vk = 0;
		break;
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT:
		part.mods |= 2u;
		vk = 0;
		break;
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU:
		part.mods |= 4u;
		vk = 0;
		break;
	default:
		break;
	}

	part.vk = vk;

	if (cacheCount < static_cast<int>(sizeof(cache) / sizeof(cache[0])))
	{
		strncpy_s(cache[cacheCount].name, commandName, _TRUNCATE);
		cache[cacheCount].part = part;
		++cacheCount;
	}

	return part;
}

bool MOEE::KeyPartHeld(const KeyPart& part)
{
	if ((part.mods & 1u) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
		return false;

	if ((part.mods & 2u) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
		return false;

	if ((part.mods & 4u) && !(GetAsyncKeyState(VK_MENU) & 0x8000))
		return false;

	if (part.vk && !(GetAsyncKeyState(static_cast<int>(part.vk)) & 0x8000))
		return false;

	return true;
}

bool MOEE::KeyPartHeldExact(const KeyPart& part)
{
	const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

	const unsigned int held = (ctrl ? 1u : 0u) | (shift ? 2u : 0u) | (alt ? 4u : 0u);

	if (held != part.mods)
		return false;

	if (part.vk && !(GetAsyncKeyState(static_cast<int>(part.vk)) & 0x8000))
		return false;

	return true;
}

bool MOEE::MouseComboClaimed(const char* selfName)
{
	const KeyPart self = GetKeyPart(selfName);

	for (const char* name : MouseComboNames)
	{
		if (_stricmp(name, selfName) == 0)
			continue;

		const KeyPart other = GetKeyPart(name);

		if (other.Empty() || !KeyPartHeld(other))
			continue;

		// Only a strictly more specific binding claims the combo. For example,
		// Ctrl+Z+Middle (Tree Chop) claims Ctrl+Middle (Fuzzy Attack), but not
		// the other way around.
		if ((other.mods & self.mods) != self.mods)
			continue;

		if (self.vk && other.vk != self.vk)
			continue;

		if (other.mods != self.mods || other.vk != self.vk)
			return true;
	}

	return false;
}
