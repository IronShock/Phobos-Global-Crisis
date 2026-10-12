#include "CreateOverlayTypeClass.h"

#include <OverlayTypeClass.h>
#include <Utilities/INIParser.h>
#include <Utilities/TemplateDef.h>

void CreateOverlayTypeClass::LoadFromINI(CCINIClass* pINI, const char* pSection)
{
	INI_EX exINI(pINI);

	this->Type.Read(exINI, pSection, "CreateOverlay");
	this->Overwrite.Read(exINI, pSection, "CreateOverlay.Overwrite");
	this->Frame.Read(exINI, pSection, "CreateOverlay.Frame");
}

#pragma region(save/load)

template <class T>
bool CreateOverlayTypeClass::Serialize(T& stm)
{
	return stm
		.Process(this->Type)
		.Process(this->Overwrite)
		.Process(this->Frame)
		.Success();
}

bool CreateOverlayTypeClass::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool CreateOverlayTypeClass::Save(PhobosStreamWriter& stm) const
{
	return const_cast<CreateOverlayTypeClass*>(this)->Serialize(stm);
}

#pragma endregion(save/load)
