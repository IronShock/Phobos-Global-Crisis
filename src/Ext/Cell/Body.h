#pragma once
#include <CellClass.h>

#include <Utilities/Container.h>
#include <Utilities/Constructs.h>
#include <Utilities/Template.h>

class SmudgeTypeClass;

class CellExt
{
public:
	using base_type = CellClass;

	static constexpr DWORD Canary = 0x13371337;

	struct RadLevel
	{
		RadSiteClass* Rad { nullptr };
		int Level { 0 };

		RadLevel() = default;
		RadLevel(RadSiteClass* pRad, int level) : Rad(pRad), Level(level)
		{ }

		bool Load(PhobosStreamReader& stm, bool registerForChange);
		bool Save(PhobosStreamWriter& stm) const;

	private:
		template <typename T>
		bool Serialize(T& stm);
	};

	class ExtData final : public Extension<CellClass>
	{
	public:
		std::vector<RadSiteClass*> RadSites {};
		std::vector<RadLevel> RadLevels { };
		UnitClass* IncomingUnit {};
		UnitClass* IncomingUnitAlt {};
		// Extra smudges on top of the cell's native one (CellClass::SmudgeTypeIndex),
		// placed by the AnimType CreateSmudge feature. Rendered alongside the native
		// smudge (see Hooks.DrawOverlay.cpp).
		std::vector<SmudgeTypeClass*> ExtraSmudges {};

		ExtData(CellClass* OwnerObject) : Extension<CellClass>(OwnerObject)
		{ }

		virtual ~ExtData() = default;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override;

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<CellExt>
	{
	public:
		ExtContainer();
		~ExtContainer();

		virtual bool InvalidateExtDataIgnorable(void* const ptr) const override;
	};

	static ExtContainer ExtMap;
};

// Diagnostics for the radiation system: a std::vector whose internal header got
// overwritten (heap corruption) shows up as size > capacity or an unreadable data
// pointer. These helpers let the rad hooks detect and report it instead of
// iterating/erasing into garbage memory.
namespace CellExtHelper
{
	template <typename T>
	bool IsVectorSane(const std::vector<T>& vec)
	{
		if (vec.size() > vec.capacity())
			return false;

		if (!vec.empty() && !Savegame::IsBufferReadable(vec.data(), vec.size() * sizeof(T)))
			return false;

		return true;
	}

	inline bool IsExtReadable(CellExt::ExtData* pExt)
	{
		return pExt && Savegame::IsBufferReadable(pExt, sizeof(CellExt::ExtData));
	}
}
