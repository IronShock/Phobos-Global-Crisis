#pragma once

#include <Utilities/Template.h>

class OverlayTypeClass;

// AnimType "CreateOverlay" feature: when the anim finishes, place an overlay on
// its cell. Modeled after CreateUnitTypeClass.
class CreateOverlayTypeClass
{
public:
	Valueable<OverlayTypeClass*> Type { nullptr };
	Valueable<bool> Overwrite { false }; // Replace an overlay already on the cell.
	Valueable<int> Frame { -1 }; // Overlay frame to use; -1 = the type's default frame.

	CreateOverlayTypeClass() = default;

	void LoadFromINI(CCINIClass* pINI, const char* pSection);
	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:

	template <typename T>
	bool Serialize(T& stm);
};
