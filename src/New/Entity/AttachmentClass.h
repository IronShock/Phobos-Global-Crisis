#pragma once

#include <algorithm>

#include <vector>

#include <GeneralStructures.h>

#include <New/Type/AttachmentTypeClass.h>
#include <Ext/TechnoType/Body.h>

class TechnoClass;

class AttachmentClass
{
public:
	static std::vector<AttachmentClass*> Array;

	static std::vector<AttachmentClass*> PendingDataFixup;

	TechnoTypeExt::ExtData::AttachmentDataEntry* Data;
	TechnoClass* Parent;
	TechnoClass* Child;
	CDTimerClass RespawnTimer;

	int DataIndex { -1 }; // Serialized index for restoring Data pointer after load

	bool bIsPendingChildCreation { false }; // True for attachments created during save loading whose OnCreated() was deferred


	AttachmentClass(TechnoTypeExt::ExtData::AttachmentDataEntry* data,
		TechnoClass* pParent, TechnoClass* pChild = nullptr) :
		Data { data },
		Parent { pParent },
		Child { pChild },
		RespawnTimer { }
	{
		Array.push_back(this);
	}

	AttachmentClass() :
		Data { },
		Parent { },
		Child { },
		RespawnTimer { }
	{
		Array.push_back(this);
	}

	~AttachmentClass();

	AttachmentTypeClass* GetType();
	TechnoTypeClass* GetChildType();
	CoordStruct GetChildLocation();

	void OnCreated();
	void CreateChild();
	void AI();
	void Destroy(TechnoClass* pSource);
	void ChildDestroyed();

	void Unlimbo();
	void Limbo();

	bool AttachChild(TechnoClass* pChild);
	bool DetachChild();

	// Core link/unlink without side effects (locomotor changes, missions, owner resets)
	void AttachChildCore(TechnoClass* pChild);
	void DetachChildCore();

	void InvalidatePointer(void* ptr);

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

	static void FixupDataPointers();

	// Restores ParentAttachment pointers on children and ensures child locomotors
	// are AttachmentLocomotionClass after save/load. Called after FixupDataPointers.
	static void FixupParentAttachments();

	// Creates children for attachments that were created during save loading
	// but had OnCreated() skipped. Called after FixupParentAttachments.
	static void CreatePendingChildren();

private:
	template <typename T>
	bool Serialize(T& stm);
};
