#include "Fog.h"

namespace Fog
{
	// AnimTypeClass pointers used by combat effects (warhead, weapon and bullet
	// animations) and all animations linked from them. Built after rules/scenario
	// load so the lists are complete before the first draw.
	std::unordered_set<AnimTypeClass*> WarheadAnimTypes;
	bool WarheadAnimTypesBuilt = false;

	// Explicit [FogAnim] registry: animation names listed there are allowed even
	// when they are not reached through a warhead/weapon/bullet field.
	std::unordered_set<AnimTypeClass*> FogAnimRegistry;

	void AddFogAnimName(const char* pName)
	{
		if (!pName || !*pName)
			return;

		if (auto const pType = AnimTypeClass::Find(pName))
			FogAnimRegistry.insert(pType);
		else
			Debug::Log("[FogAnim] unknown animation '%s'\n", pName);
	}

	void ParseFogAnimSection(CCINIClass* pINI)
	{
		if (!pINI)
			return;

		const int count = pINI->GetKeyCount("FogAnim");

		for (int i = 0; i < count; ++i)
		{
			const char* pKey = pINI->GetKeyName("FogAnim", i);

			if (!pKey)
				continue;

			char value[2048] {};

			if (pINI->ReadString("FogAnim", pKey, "", value, sizeof(value)) <= 0)
				continue;

			char* pContext = nullptr;

			for (char* pToken = strtok_s(value, ", \t", &pContext); pToken;
				pToken = strtok_s(nullptr, ", \t", &pContext))
			{
				AddFogAnimName(pToken);
			}
		}
	}

	void AddCombatAnimType(AnimTypeClass* pType)
	{
		if (!pType || !WarheadAnimTypes.insert(pType).second)
			return;

		AddCombatAnimType(pType->Next);
		AddCombatAnimType(pType->Spawns);
		AddCombatAnimType(pType->BounceAnim);
		AddCombatAnimType(pType->ExpireAnim);
		AddCombatAnimType(pType->TrailerAnim);

		if (auto const pExt = AnimTypeExt::ExtMap.TryFind(pType))
		{
			for (auto const pAnim : pExt->SplashAnims)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->WakeAnim)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->SmallFireAnims)
				AddCombatAnimType(pAnim);

			for (auto const pAnim : pExt->LargeFireAnims)
				AddCombatAnimType(pAnim);
		}
	}

	void RebuildWarheadAnimTypes()
	{
		WarheadAnimTypes.clear();

		// Global damage-based splash animations used when a warhead has no
		// SplashList override.
		for (auto const pAnim : RulesClass::Instance->SplashList)
			AddCombatAnimType(pAnim);

		for (auto const pWH : WarheadTypeClass::Array)
		{
			if (!pWH)
				continue;

			for (auto const pAnim : pWH->AnimList)
				AddCombatAnimType(pAnim);

			if (auto const pExt = WarheadTypeExt::ExtMap.TryFind(pWH))
			{
				for (auto const pAnim : pExt->SplashList)
					AddCombatAnimType(pAnim);

				for (auto const pAnim : pExt->Crit_AnimList)
					AddCombatAnimType(pAnim);
			}
		}

		// Weapon launch/occupant/assault animations and their linked trails.
		for (auto const pWeapon : WeaponTypeClass::Array)
		{
			if (!pWeapon)
				continue;

			for (auto const pAnim : pWeapon->Anim)
				AddCombatAnimType(pAnim);

			AddCombatAnimType(pWeapon->OccupantAnim);
			AddCombatAnimType(pWeapon->AssaultAnim);
			AddCombatAnimType(pWeapon->OpenToppedAnim);
		}

		// Projectile trails.
		for (auto const pBullet : BulletTypeClass::Array)
		{
			if (pBullet)
				AddCombatAnimType(pBullet->Trailer);
		}

		FogAnimRegistry.clear();
		ParseFogAnimSection(CCINIClass::INI_Rules);
		ParseFogAnimSection(&CCINIClass::INI_Art);

		// Deprecated per-AnimType alias; keep old INIs working.
		for (auto const pType : AnimTypeClass::Array)
		{
			if (auto const pExt = AnimTypeExt::ExtMap.TryFind(pType))
			{
				if (pExt->AllowAnim)
					FogAnimRegistry.insert(pType);
			}
		}

		WarheadAnimTypesBuilt = true;
	}

	bool IsWarheadAnim(AnimClass* pAnim)
	{
		if (!pAnim || !pAnim->Type)
			return false;

		if (!WarheadAnimTypesBuilt)
			RebuildWarheadAnimTypes();

		return WarheadAnimTypes.find(pAnim->Type) != WarheadAnimTypes.end();
	}

	bool IsRegistryAnim(AnimClass* pAnim)
	{
		if (!pAnim || !pAnim->Type)
			return false;

		if (!WarheadAnimTypesBuilt)
			RebuildWarheadAnimTypes();

		return FogAnimRegistry.find(pAnim->Type) != FogAnimRegistry.end();
	}
	// A warhead/registry animation inside explored, mapped fog that the player
	// cannot currently see. These are drawn once by the post-render pass.
	bool IsFoggedAllowedAnim(AnimClass* pAnim, CellClass* pCell, bool requireOnScreen)
	{
		if (!pAnim || !pAnim->Type || !pAnim->IsAlive || pAnim->Invisible)
			return false;

		if (!Active() || !Phobos::Config::TSFog_AllowAnim)
			return false;

		if (!pCell || !IsExploredCell(pCell) || !IsMappedCell(pCell))
			return false;

		if (!IsWarheadAnim(pAnim) && !IsRegistryAnim(pAnim))
			return false;

		if (IsCellInSight(pCell))
			return false;

		if (pCell->Foggedness == -1 && !pAnim->IsFogged && !IsProducerCell(pCell))
			return false;

		if (requireOnScreen
			&& !TacticalClass::Instance->CoordsToClient(pAnim->GetRenderCoords()).second)
		{
			return false;
		}

		return true;
	}

	// Draw allowed fog animations after TacticalClass::Render(..., 2) has
	// completed. The fog layer was drawn earlier, in mode 1; this pass therefore
	// places the animation above fog without touching any fog/cell state.
	void DrawAllowedAnimsAfterRender()
	{
		if (!Active() || !Phobos::Config::TSFog_AllowAnim)
			return;

		// The object pass passes this same rectangle to ObjectClass::DrawIt.
		auto const pBounds = reinterpret_cast<RectangleStruct*>(0x886FA0);
		constexpr int MaxDraw = 128;
		int drawn = 0;

		for (auto const pAnim : AnimClass::Array)
		{
			if (drawn >= MaxDraw)
				break;

			auto const pCell = pAnim ? pAnim->GetCell() : nullptr;

			if (!IsFoggedAllowedAnim(pAnim, pCell, true))
				continue;

			auto [location, visible] = TacticalClass::Instance
				->CoordsToClient(pAnim->GetRenderCoords());

			if (!visible)
				continue;

			const bool oldFogged = pAnim->IsFogged;
			const bool oldVisible = pAnim->IsVisible;
			const bool oldInProgress = FogAnimPostRenderInProgress;

			pAnim->IsFogged = false;
			pAnim->IsVisible = true;
			FogAnimPostRenderInProgress = true;
			pAnim->DrawIt(&location, pBounds);
			FogAnimPostRenderInProgress = oldInProgress;
			pAnim->IsVisible = oldVisible;
			pAnim->IsFogged = oldFogged;

			++drawn;
		}
	}
}

// AnimClass::DrawIt skips drawing when AnimClass::IsFogged (offset 0x19D) is
// set. Allow warhead animations and explicitly opted-in AnimTypes to keep
// drawing inside explored fog. Unexplored black shroud stays hidden.
// Hook the `mov al, [esi+19Dh]` instead of the following `jne`: Syringe cannot
// relocate the relative branch, but a plain mov can be replayed safely.
DEFINE_HOOK(0x422FEC, AnimClass_DrawIt_AllowAnim, 0x6)
{
	enum { DrawAnyway = 0x422FFA, SkipDraw = 0x4238A3 };

	if (!Fog::Active() || !Phobos::Config::TSFog_AllowAnim)
		return 0;

	GET(AnimClass*, pThis, ESI);

	if (!pThis || !pThis->Type)
		return 0;

	// The post-render pass calls DrawIt directly; let that one through.
	if (Fog::FogAnimPostRenderInProgress)
		return 0;

	auto const pCell = pThis->GetCell();
	const bool explored = pCell && Fog::IsExploredCell(pCell);
	const bool mapped = pCell && Fog::IsMappedCell(pCell);

	const bool combat = Fog::IsWarheadAnim(pThis);
	const bool registry = Fog::IsRegistryAnim(pThis);
	const bool allow = explored && mapped && (combat || registry);
	const bool fogged = Fog::IsFoggedAllowedAnim(pThis, pCell, false);

	// Skip the normal object pass for fogged allowed animations; the
	// post-render pass draws them above fog instead, exactly once.
	if (fogged)
		return SkipDraw;

	return allow ? DrawAnyway : 0;
}

// GScreenClass::Render calls TacticalClass::Render three times:
//   0x4F44DF eMode=0, 0x4F44F4 eMode=1 (terrain + fog), 0x4F4515 eMode=2
//   (objects + animations). The instruction at 0x4F451B is immediately after
// the mode-2 call, so this is a genuine post-fog/post-object point. Replaying
// the allowed animations here puts them above fog without modifying any cell.
DEFINE_HOOK(0x4F451B, GScreenClass_Render_AfterTacticalObjects_AllowAnim, 0x5)
{
	if (Fog::Active() && Phobos::Config::TSFog_AllowAnim)
		Fog::DrawAllowedAnimsAfterRender();

	return 0;
}
