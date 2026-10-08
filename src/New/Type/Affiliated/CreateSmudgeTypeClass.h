#pragma once

#include <Utilities/Template.h>

class SmudgeTypeClass;

// AnimType "CreateSmudge" feature: when the anim finishes, place smudge(s) on its
// cell. Multiple types may be given (comma-separated): the first goes to the
// native smudge slot (bottom), the rest are stacked in CellExt::ExtraSmudges in
// order (each drawn above the previous). Modeled after CreateUnitTypeClass.
class CreateSmudgeTypeClass
{
public:
	NullableVector<SmudgeTypeClass*> Type;
	Valueable<bool> Overwrite { false }; // Replace the whole smudge stack on the cell.

	CreateSmudgeTypeClass() = default;

	void LoadFromINI(CCINIClass* pINI, const char* pSection);
	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:

	template <typename T>
	bool Serialize(T& stm);
};
