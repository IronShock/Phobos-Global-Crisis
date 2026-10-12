#pragma once
#include <SmudgeTypeClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Macro.h>
#include <Utilities/Constructs.h>

class SmudgeTypeExt
{
public:
	using base_type = SmudgeTypeClass;

	static constexpr DWORD Canary = 0x5A4D5544;
	// NOTE: deliberately no ExtPointerOffset. The game frees SmudgeTypeClass
	// objects during scenario teardown without invoking the hooked DTOR, so a
	// pointer cached on the object goes stale and is then reused by whatever
	// object takes that memory next (observed as heap corruption / random
	// C0000005 on the 2nd-3rd scenario load). Keep the extension in the map.

	class ExtData final : public Extension<SmudgeTypeClass>
	{
	public:
		CustomPalette Palette;
		// When set, this smudge is not drawn in the cell ground pass (where overlays
		// cover it) but deferred until after the overlay pass, so it renders above
		// overlays (below units/buildings). See Ext/Smudge/Hooks.cpp.
		Valueable<bool> DrawObject;

		ExtData(SmudgeTypeClass* OwnerObject) : Extension<SmudgeTypeClass>(OwnerObject)
			, Palette {}
			, DrawObject { false }
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<SmudgeTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;
};
