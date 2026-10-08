#include "CreateSmudgeTypeClass.h"

#include <SmudgeTypeClass.h>
#include <Utilities/INIParser.h>
#include <Utilities/TemplateDef.h>

void CreateSmudgeTypeClass::LoadFromINI(CCINIClass* pINI, const char* pSection)
{
	INI_EX exINI(pINI);

	this->Type.Read(exINI, pSection, "CreateSmudge");
	this->Overwrite.Read(exINI, pSection, "CreateSmudge.Overwrite");
}

#pragma region(save/load)

template <class T>
bool CreateSmudgeTypeClass::Serialize(T& stm)
{
	return stm
		.Process(this->Type)
		.Process(this->Overwrite)
		.Success();
}

bool CreateSmudgeTypeClass::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool CreateSmudgeTypeClass::Save(PhobosStreamWriter& stm) const
{
	return const_cast<CreateSmudgeTypeClass*>(this)->Serialize(stm);
}

#pragma endregion(save/load)
