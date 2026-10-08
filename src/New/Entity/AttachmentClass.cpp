#include "AttachmentClass.h"

#include <Dir.h>
#include <BulletClass.h>
#include <BulletTypeClass.h>
#include <WarheadTypeClass.h>

#include <TechnoClass.h>

#include <ObjBase.h>

#include <Utilities/SavegameDef.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Locomotion/AttachmentLocomotionClass.h>

std::vector<AttachmentClass*> AttachmentClass::Array;

std::vector<AttachmentClass*> AttachmentClass::PendingDataFixup;

AttachmentTypeClass* AttachmentClass::GetType()
{
	return AttachmentTypeClass::Array[this->Data->Type].get();
}

TechnoTypeClass* AttachmentClass::GetChildType()
{
	if (!this->Data || !this->Data->TechnoType.isset())
		return nullptr;

	const int idx = this->Data->TechnoType.Get();
	if (idx < 0 || idx >= TechnoTypeClass::Array.Count)
		return nullptr;

	return TechnoTypeClass::Array[idx];
}

CoordStruct AttachmentClass::GetChildLocation()
{
	auto& flh = this->Data->FLH.Get();
	return TechnoExt::GetFLHAbsoluteCoords(this->Parent, flh, this->Data->IsOnTurret);
}

AttachmentClass::~AttachmentClass()
{
	// clean up non-owning references
	if (this->Child)
	{
		auto const& pChildExt = TechnoExt::ExtMap.Find(Child);
		pChildExt->ParentAttachment = nullptr;
	}

	auto position = std::find(Array.begin(), Array.end(), this);
	if (position != Array.end())
		Array.erase(position);
}

void AttachmentClass::OnCreated()
{
	if (this->Child)
		return;

	if (!this->Data)
		return;

	const int dataType = this->Data->Type.Get();
	if (dataType < 0 || static_cast<size_t>(dataType) >= AttachmentTypeClass::Array.size())
		return;

	auto const pType = AttachmentTypeClass::Array[static_cast<size_t>(dataType)].get();
	if (!pType)
		return;

	if (pType->RespawnAtCreation)
		this->CreateChild();
}

void AttachmentClass::CreateChild()
{
	if (auto const pChildType = this->GetChildType())
	{
		if (pChildType->WhatAmI() != AbstractType::UnitType)
			return;

		if (const auto pTechno = static_cast<TechnoClass*>(pChildType->CreateObject(this->Parent->Owner)))
		{
			this->AttachChild(pTechno);
		}
		else
		{
			Debug::Log("[" __FUNCTION__ "] Failed to create child %s of parent %s!\n",
				pChildType->ID, this->Parent->GetTechnoType()->ID);
		}
	}
}

void AttachmentClass::AI()
{
	// Guard: if Data is null (e.g. FixupDataPointers failed), skip AI entirely
	// to prevent crash in GetType() which dereferences Data.
	if (!this->Data)
		return;

	// Safety net: if this attachment was created during save loading and
	// OnCreated() was skipped (bIsPendingChildCreation), create the child now.
	// This handles cases where CreatePendingChildren() didn't process this
	// attachment (e.g. it was added to Array after CreatePendingChildren ran,
	// or in edge cases where the load hook timing didn't cover it).
	// The existing unlimbo logic later in AI() will unlimbo the child if needed.
	if (this->bIsPendingChildCreation)
	{
		this->bIsPendingChildCreation = false;
		if (this->Parent)
			this->OnCreated();
	}

	AttachmentTypeClass* pType = this->GetType();

	if (!this->Child)
	{
		if (pType->RespawnDelay == 0)
		{
			this->CreateChild();
		}
		else if (pType->RespawnDelay > 0)
		{
			if (!this->RespawnTimer.HasStarted())
			{
				this->RespawnTimer.Start(pType->RespawnDelay);
			}
			else if (this->RespawnTimer.Completed())
			{
				this->CreateChild();
				this->RespawnTimer.Stop();
			}
		}
	}

	if (this->Child)
	{
		// Safety net: ensure the child has AttachmentLocomotionClass.
		// FixupParentAttachments() handles this on load, but edge cases such as
		// unit conversion or piggyback ending can leave the child with its default
		// locomotor, causing it to render independently of the parent.
		if (auto pChildAsFoot = abstract_cast<FootClass*>(this->Child))
		{
			if (IPersistPtr pLocoPersist = pChildAsFoot->Locomotor)
			{
				CLSID locoCLSID {};
				if (SUCCEEDED(pLocoPersist->GetClassID(&locoCLSID))
					&& locoCLSID != __uuidof(AttachmentLocomotionClass))
				{
					LocomotionClass::ChangeLocomotorTo(pChildAsFoot,
						__uuidof(AttachmentLocomotionClass));
				}
			}
		}

		// Safety net: restore ParentAttachment pointer on the child.
		// This pointer cannot be swizzled (AttachmentClass is not a game object)
		// and may become stale after load or unit conversion.
		if (auto const pChildExt = TechnoExt::ExtMap.Find(this->Child))
		{
			if (!pChildExt->ParentAttachment)
				pChildExt->ParentAttachment = this;
		}

		if (this->Child->InLimbo && !this->Parent->InLimbo)
			this->Unlimbo();
		else if (!this->Child->InLimbo && this->Parent->InLimbo)
			this->Limbo();

		this->Child->SetLocation(this->GetChildLocation());

		DirStruct childDir = this->Data->IsOnTurret
			? this->Parent->SecondaryFacing.Current() : this->Parent->PrimaryFacing.Current();

		childDir.Raw += DirStruct(this->Data->RotationAdjust).Raw; // overflow = free modulo for rotation

		this->Child->PrimaryFacing.SetCurrent(childDir);
		// TODO handle secondary facing in case the turret is idle

		FootClass* pParentAsFoot = abstract_cast<FootClass*>(this->Parent);
		FootClass* pChildAsFoot = abstract_cast<FootClass*>(this->Child);
		if (pParentAsFoot && pChildAsFoot)
		{
			pChildAsFoot->TubeIndex = pParentAsFoot->TubeIndex;
		}

		if (pType->InheritStateEffects)
		{
			this->Child->IsFallingDown = this->Parent->IsFallingDown;
			this->Child->WasFallingDown = this->Parent->WasFallingDown;
			this->Child->CloakState = this->Parent->CloakState;
			this->Child->WarpingOut = this->Parent->WarpingOut;
			this->Child->unknown_280 = this->Parent->unknown_280; // sth related to teleport
			this->Child->BeingWarpedOut = this->Parent->BeingWarpedOut;
			this->Child->Deactivated = this->Parent->Deactivated;
			//this->Child->Flash(this->Parent->Flashing.DurationRemaining);

			this->Child->IronCurtainTimer = this->Parent->IronCurtainTimer;
			this->Child->IdleActionTimer = this->Parent->IdleActionTimer;
			this->Child->IronTintTimer = this->Parent->IronTintTimer;
			this->Child->CloakDelayTimer = this->Parent->CloakDelayTimer;
			this->Child->ChronoLockRemaining = this->Parent->ChronoLockRemaining;
			this->Child->Berzerk = this->Parent->Berzerk;
			this->Child->BerzerkDurationLeft = this->Parent->BerzerkDurationLeft;
			this->Child->ChronoWarpedByHouse = this->Parent->ChronoWarpedByHouse;
			this->Child->EMPLockRemaining = this->Parent->EMPLockRemaining;
			this->Child->ShouldLoseTargetNow = this->Parent->ShouldLoseTargetNow;
		}

		if (pType->InheritOwner)
			this->Child->SetOwningHouse(this->Parent->GetOwningHouse(), false);
	}
}

// Called in Kill_Cargo, handles logics for parent destruction on children
void AttachmentClass::Destroy(TechnoClass* pSource)
{
	if (this->Child)
	{
		auto const pChildExt = TechnoExt::ExtMap.Find(this->Child);
		pChildExt->ParentAttachment = nullptr;

		auto pType = this->GetType();

		if (pType->DestructionWeapon_Child.isset())
			TechnoExt::FireWeaponAtSelf(this->Child, pType->DestructionWeapon_Child);

		if (pType->InheritDestruction && this->Child)
			TechnoExt::Kill(this->Child, pSource);
		else if (!this->Child->InLimbo && pType->ParentDestructionMission.isset())
			this->Child->QueueMission(pType->ParentDestructionMission.Get(), false);

		this->Child = nullptr;
	}
}

void AttachmentClass::ChildDestroyed()
{
	if (this->Child)
	{
		if (auto const pChildExt = TechnoExt::ExtMap.Find(this->Child))
			pChildExt->ParentAttachment = nullptr;

		AttachmentTypeClass* pType = this->GetType();
		if (pType->DestructionWeapon_Parent.isset())
			TechnoExt::FireWeaponAtSelf(this->Parent, pType->DestructionWeapon_Parent);

		this->Child = nullptr;
	}
}

void AttachmentClass::Unlimbo()
{
	if (this->Child)
	{
		CoordStruct childCoord = TechnoExt::GetFLHAbsoluteCoords(
			this->Parent, this->Data->FLH, this->Data->IsOnTurret);

		DirStruct childDir = this->Data->IsOnTurret
			? this->Parent->SecondaryFacing.Current() : this->Parent->PrimaryFacing.Current();

		childDir.Raw += DirStruct(this->Data->RotationAdjust).Raw; // overflow = free modulo for rotation

		++Unsorted::ScenarioInit;
		this->Child->Unlimbo(childCoord, childDir.GetDir());
		--Unsorted::ScenarioInit;
	}
}

void AttachmentClass::Limbo()
{
	if (this->Child)
		this->Child->Limbo();
}

bool AttachmentClass::AttachChild(TechnoClass* pChild)
{
	if (this->Child)
		return false;

	if (pChild->WhatAmI() != AbstractType::Unit)
		return false;

	if (auto const pChildAsFoot = abstract_cast<FootClass*>(pChild))
	{
		if (IPersistPtr pLocoPersist = pChildAsFoot->Locomotor)
		{
			CLSID locoCLSID { };
			if (SUCCEEDED(pLocoPersist->GetClassID(&locoCLSID))
				&& locoCLSID != __uuidof(AttachmentLocomotionClass))
			{
				LocomotionClass::ChangeLocomotorTo(pChildAsFoot,
					__uuidof(AttachmentLocomotionClass));
			}
		}
	}

	this->AttachChildCore(pChild);

	// bandaid for jitterless drawing. TODO fix properly
	// this->Child->GetTechnoType()->DisableVoxelCache = true;
	// this->Child->GetTechnoType()->DisableShadowCache = true;

	AttachmentTypeClass* pType = this->GetType();

	if (pType->InheritOwner)
	{
		if (auto pController = this->Child->MindControlledBy)
			pController->CaptureManager->FreeUnit(this->Child);
	}

	return true;
}

bool AttachmentClass::DetachChild()
{
	if (this->Child)
	{
		AttachmentTypeClass* pType = this->GetType();

		if (!this->Child->InLimbo && pType->ParentDetachmentMission.isset())
			this->Child->QueueMission(pType->ParentDetachmentMission.Get(), false);

		// FIXME this won't work probably
		if (pType->InheritOwner)
			this->Child->SetOwningHouse(this->Parent->GetOriginalOwner(), false);

		// remove the attachment locomotor manually just to be safe
		if (auto const pChildAsFoot = abstract_cast<FootClass*>(this->Child))
			LocomotionClass::End_Piggyback(pChildAsFoot->Locomotor);

		this->DetachChildCore();

		return true;
	}

	return false;
}


void AttachmentClass::AttachChildCore(TechnoClass* pChild)
{
	this->Child = pChild;
	TechnoExt::ExtMap.Find(pChild)->ParentAttachment = this;
}

void AttachmentClass::DetachChildCore()
{
	if (this->Child)
	{
		TechnoExt::ExtMap.Find(this->Child)->ParentAttachment = nullptr;
		this->Child = nullptr;
	}
}

void AttachmentClass::InvalidatePointer(void* ptr)
{
	AnnounceInvalidPointer(this->Parent, ptr);
	AnnounceInvalidPointer(this->Child, ptr);
}

#pragma region Save/Load

template <typename T>
bool AttachmentClass::Serialize(T& stm)
{
	// The Data field is a pointer into TechnoTypeExt::ExtData::AttachmentData
	// (a ValueableVector<AttachmentDataEntry>). This pointer cannot be swizzled
	// because AttachmentDataEntry is not a game object. Instead, we serialize
	// the index of the entry within the parent type's AttachmentData vector,
	// and restore the pointer in FixupDataPointers() after all loading completes.
	if constexpr (std::is_same_v<T, PhobosStreamWriter>)
	{
		// Save: compute the index from the Data pointer
		this->DataIndex = -1;
		if (this->Parent && this->Data)
		{
			auto const pType = this->Parent->GetTechnoType();
			if (auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType))
			{
				for (size_t i = 0; i < pTypeExt->AttachmentData.size(); ++i)
				{
					if (&pTypeExt->AttachmentData[i] == this->Data)
					{
						this->DataIndex = static_cast<int>(i);
						break;
					}
				}
			}
		}
	}

	stm.Process(this->DataIndex);

	if constexpr (std::is_same_v<T, PhobosStreamReader>)
	{
		// Load: Data will be restored later in FixupDataPointers()
		this->Data = nullptr;
		AttachmentClass::PendingDataFixup.push_back(this);
	}

	stm.Process(this->Parent);
	stm.Process(this->Child);
	stm.Process(this->RespawnTimer);

	return stm.Success();
}

bool AttachmentClass::Load(PhobosStreamReader& stm, bool RegisterForChange)
{
	return Serialize(stm);
}

bool AttachmentClass::Save(PhobosStreamWriter& stm) const
{
	return const_cast<AttachmentClass*>(this)->Serialize(stm);
}

void AttachmentClass::FixupDataPointers()
{
	for (auto* pAttachment : AttachmentClass::PendingDataFixup)
	{
		if (pAttachment->DataIndex >= 0 && pAttachment->Parent)
		{
			auto const pType = pAttachment->Parent->GetTechnoType();
			if (auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType))
			{
				if (static_cast<size_t>(pAttachment->DataIndex) < pTypeExt->AttachmentData.size())
				{
					pAttachment->Data = &pTypeExt->AttachmentData[pAttachment->DataIndex];
				}
				else
				{
					Debug::Log("[AttachmentClass::FixupDataPointers] DataIndex %d out of range for parent %s!\n",
						pAttachment->DataIndex, pType->ID);
				}
			}
		}
	}

	AttachmentClass::PendingDataFixup.clear();
}

void AttachmentClass::FixupParentAttachments()
{
	// After save/load, the ParentAttachment pointer stored on each child's
	// TechnoExt::ExtData is stale. AttachmentClass objects live inside
	// std::vector<unique_ptr<AttachmentClass>> on the parent's ext data;
	// when the vector is deserialized the AttachmentClass objects are recreated
	// at new addresses. The Swizzle system cannot remap AttachmentClass* because
	// it is not a game object. This function restores the correct pointers by
	// iterating the global Array.
	for (auto* pAttachment : AttachmentClass::Array)
	{
		if (!pAttachment->Child)
			continue;

		// Restore the child's ParentAttachment pointer.
		auto const pChildExt = TechnoExt::ExtMap.Find(pAttachment->Child);
		if (pChildExt)
			pChildExt->ParentAttachment = pAttachment;

		// Ensure the child is using AttachmentLocomotionClass.
		// Under normal circumstances the locomotor is saved/restored correctly,
		// but some edge cases (e.g. skill-summoned units) can leave the child
		// with its default locomotor, causing it to render at the wrong position.
		if (auto pChildAsFoot = abstract_cast<FootClass*>(pAttachment->Child))
		{
			if (IPersistPtr pLocoPersist = pChildAsFoot->Locomotor)
			{
				CLSID locoCLSID {};
				if (SUCCEEDED(pLocoPersist->GetClassID(&locoCLSID))
					&& locoCLSID != __uuidof(AttachmentLocomotionClass))
				{
					LocomotionClass::ChangeLocomotorTo(pChildAsFoot,
						__uuidof(AttachmentLocomotionClass));
				}
			}
		}
	}
}

void AttachmentClass::CreatePendingChildren()
{
	// After save loading, units created during the load process (by triggers,
	// scripts, or skills) had their attachment slots created by InitializeAttachments
	// but OnCreated() was skipped (because Phobos::IsLoadingSaveGame was true).
	// For units loaded from the save, LoadFromStream replaced those empty slots with
	// saved data (so bIsPendingChildCreation is false on the deserialized objects).
	// For new units not in the save, the empty slots remain with the flag set to true.
	// This function creates their children now that loading is complete.
	//
	// Use index-based iteration because OnCreated() may add new AttachmentClass
	// objects to Array (if the child unit type has its own attachments).
	// IsLoadingSaveGame is still true at this point, so those new attachments
	// also get bIsPendingChildCreation = true and are processed in the same loop.
	for (size_t i = 0; i < AttachmentClass::Array.size(); ++i)
	{
		auto* pAttachment = AttachmentClass::Array[i];
		if (!pAttachment->bIsPendingChildCreation)
			continue;

		pAttachment->bIsPendingChildCreation = false;

		if (!pAttachment->Data || !pAttachment->Parent)
			continue;

		// Create the child unit if RespawnAtCreation is true.
		pAttachment->OnCreated();

		// If the child was created and the parent is not in limbo, unlimbo the child
		// immediately so it renders on the next frame instead of waiting for AI().
		if (pAttachment->Child && !pAttachment->Parent->InLimbo)
			pAttachment->Unlimbo();
	}
}

#pragma endregion
