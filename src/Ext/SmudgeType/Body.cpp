#include "Body.h"

#include <CCINIClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>
#include <Utilities/Patch.h>

SmudgeTypeExt::ExtContainer SmudgeTypeExt::ExtMap;

// =============================
// load / save

template <typename T>
void SmudgeTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->Palette)
		.Process(this->DrawObject)
		;
}

void SmudgeTypeExt::ExtData::LoadFromINIFile(CCINIClass* pINI)
{
	auto const pSection = this->OwnerObject()->ID;
	INI_EX exINI(pINI);

	this->Palette.LoadFromINI(pINI, pSection, "CustomPalette");
	this->DrawObject.Read(exINI, pSection, "DrawObject");
}

void SmudgeTypeExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<SmudgeTypeClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void SmudgeTypeExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<SmudgeTypeClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

SmudgeTypeExt::ExtContainer::ExtContainer() : Container("SmudgeTypeClass") { }
SmudgeTypeExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK_AGAIN(0x6B5369, SmudgeTypeClass_CTOR, 0x5)
DEFINE_HOOK(0x6B535C, SmudgeTypeClass_CTOR, 0x5)
{
	GET(SmudgeTypeClass*, pItem, EAX);

	SmudgeTypeExt::ExtMap.TryAllocate(pItem);

	return 0;
}

DEFINE_HOOK(0x6B53F0, SmudgeTypeClass_DTOR, 0x5)
{
	GET(SmudgeTypeClass*, pItem, ESI);

	SmudgeTypeExt::ExtMap.Remove(pItem);

	return 0;
}

// Per-type INI load: SmudgeTypeClass::LoadFromIniList reads every type inline,
// so hook the loop bottom where esi still holds the type being processed.
DEFINE_HOOK(0x6B552B, SmudgeTypeClass_LoadFromIniList, 0x5)
{
	GET(SmudgeTypeClass*, pItem, ESI);

	SmudgeTypeExt::ExtMap.LoadFromINI(pItem, CCINIClass::INI_Rules);

	if (auto const pExt = SmudgeTypeExt::ExtMap.Find(pItem))
	{
		if (pExt->Palette.GetConvert())
			Debug::Log("[SmudgePal] %s CustomPalette loaded (convert %X)\n",
				pItem->ID, pExt->Palette.GetConvert());
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6B5850, SmudgeTypeClass_SaveLoad_Prefix, 0xA) // Load
DEFINE_HOOK(0x6B58B0, SmudgeTypeClass_SaveLoad_Prefix, 0x8) // Save
{
	GET_STACK(SmudgeTypeClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);

	SmudgeTypeExt::ExtMap.PrepareStream(pItem, pStm);

	return 0;
}

DEFINE_HOOK(0x6B589F, SmudgeTypeClass_Load_Suffix, 0x6)
{
	SmudgeTypeExt::ExtMap.LoadStatic();

	return 0;
}

DEFINE_HOOK(0x6B58CA, SmudgeTypeClass_Save_Suffix, 0x5)
{
	SmudgeTypeExt::ExtMap.SaveStatic();

	return 0;
}

// =============================
// CustomPalette drawing hook

namespace SmudgePaletteTemp
{
	// The smudge type currently being drawn by SmudgeTypeClass::DrawIt. Set at
	// the draw entry, consumed by the redirected CC_Draw_Shape call.
	SmudgeTypeClass* Current;
}

static ConvertClass* GetDrawSmudgeConvert()
{
	if (auto const pType = SmudgePaletteTemp::Current)
	{
		if (auto const pExt = SmudgeTypeExt::ExtMap.Find(pType))
		{
			if (auto const pConvert = pExt->Palette.GetConvert())
				return pConvert;
		}
	}

	return nullptr;
}

// SmudgeTypeClass::DrawIt draws through CC_Draw_Shape (0x4AED70), which receives
// the palette convert in EDX (from the cell LightConvert). The vanilla call at
// 0x6B56B4 is redirected here (via a correctly-relative Patch::Apply_CALL, so no
// trampoline relocation is involved) so the CustomPalette convert for the type
// being drawn can be swapped in. The function is naked: the CC_Draw_Shape
// arguments are already on the stack, so we only adjust EDX and then jump (not
// call) to the real function, preserving every argument.
static void __declspec(naked) DrawItPaletteRedirect()
{
	__asm
	{
		// ECX = DSurface::Temp, EDX = cell LightConvert, stack = CC_Draw_Shape args.
		push edx
		push ecx
		call GetDrawSmudgeConvert
		test eax, eax
		jz UseVanilla
		mov edx, eax
		pop ecx
		add esp, 4
		push 0x4AED70
		ret
	UseVanilla:
		pop ecx
		pop edx
		push 0x4AED70
		ret
	}
}

// Every smudge (native and stacked extras) is drawn through SmudgeTypeClass::
// DrawIt (0x6B55F0) with `this` = the smudge type. Cache it at the entry and
// install the CC_Draw_Shape redirect once.
DEFINE_HOOK(0x6B55F0, SmudgeTypeClass_DrawIt_Begin, 0x5)
{
	static bool s_redirected = false;
	if (!s_redirected)
	{
		s_redirected = true;
		Patch::Apply_CALL(0x6B56B4, reinterpret_cast<void*>(&DrawItPaletteRedirect));
	}

	GET(SmudgeTypeClass*, pThis, ECX);

	SmudgePaletteTemp::Current = pThis;

	{
		static int s_log = 0;
		if (s_log < 5)
		{
			++s_log;
			if (auto const pExt = SmudgeTypeExt::ExtMap.Find(pThis))
			{
				if (pExt->Palette.GetConvert())
					Debug::Log("[SmudgePal] %s draw begin (convert %X)\n",
						pThis->ID, pExt->Palette.GetConvert());
			}
		}
	}

	return 0;
}
