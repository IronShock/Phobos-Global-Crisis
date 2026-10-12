#include <Phobos.h>

#include <LoadOptionsClass.h>
#include <CCINIClass.h>
#include <Utilities/Stream.h>
#include <Misc/INIDiagnostics.h>
#include <intrin.h>

#include <Ext/Aircraft/Body.h>
#include <Ext/AnimType/Body.h>
#include <Ext/Anim/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/Bullet/Body.h>
#include <Ext/BulletType/Body.h>
#include <Ext/Cell/Body.h>
#include <Ext/EBolt/Body.h>
#include <Ext/House/Body.h>
#include <Ext/OverlayType/Body.h>
#include <Ext/ParticleSystemType/Body.h>
#include <Ext/RadSite/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/Script/Body.h>
#include <Ext/Side/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/SWType/NewSWType/NewSWType.h>
#include <Ext/SmudgeType/Body.h>
#include <Ext/TAction/Body.h>
#include <Ext/Team/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/Tiberium/Body.h>
#include <Ext/VoxelAnim/Body.h>
#include <Ext/VoxelAnimType/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Ext/WeaponType/Body.h>

#include <New/Type/BannerTypeClass.h>
#include <New/Type/DigitalDisplayTypeClass.h>
#include <New/Type/LaserTrailTypeClass.h>
#include <New/Type/RadTypeClass.h>

#include <New/Entity/AttachmentClass.h>
#include <New/Entity/BannerClass.h>
#include <New/Type/SelectBoxTypeClass.h>

#include <Commands/ReverseMove.h>

#include <utility>
#include <unordered_map>

#pragma region Implementation details

#pragma region Concepts

// a hack to check if some type can be used as a specialization of a template
template <template <class...> class Template, class... Args>
void DerivedFromSpecialization(const Template<Args...>&);

template <class T, template <class...> class Template>
concept DerivedFromSpecializationOf =
	requires(const T & t) { DerivedFromSpecialization<Template>(t); };

template<typename TExt>
concept HasExtMap = requires { { TExt::ExtMap } -> DerivedFromSpecializationOf<Container>; };

template<typename TExt>
concept ExtDataConsiderPointerInvalidation = HasExtMap<TExt> && requires{
	{ TExt::ShouldConsiderInvalidatePointer }->std::convertible_to<const bool>;
}&& TExt::ShouldConsiderInvalidatePointer == true;

template <typename T>
concept Clearable = requires { T::Clear(); };

template <typename T>
concept PointerInvalidationSubscribable =
	requires (void* ptr, bool removed) { T::PointerGotInvalid(ptr, removed); };

template <typename T>
concept GlobalSaveLoadable = requires
{
	T::LoadGlobals(std::declval<PhobosStreamReader&>());
	T::SaveGlobals(std::declval<PhobosStreamWriter&>());
};

template <typename TAction, typename TProcessed, typename... ArgTypes>
concept DispatchesAction =
	requires (ArgTypes... args) { TAction::template Process<TProcessed>(args...); };

#pragma endregion

// calls:
// T::Clear()
// T::ExtMap.Clear()
struct ClearAction
{
	template <typename T>
	static bool Process()
	{
		if constexpr (Clearable<T>)
			T::Clear();
		else if constexpr (HasExtMap<T>)
			T::ExtMap.Clear();

		return true;
	}
};

// calls:
// T::PointerGotInvalid(void*, bool)
// T::ExtMap.PointerGotInvalid(void*, bool)
struct InvalidatePointerAction
{
	template <typename T>
	static bool Process(void* ptr, bool removed)
	{
		if constexpr (PointerInvalidationSubscribable<T>)
			T::PointerGotInvalid(ptr, removed);
		else if constexpr (ExtDataConsiderPointerInvalidation<T>)
			T::ExtMap.PointerGotInvalid(ptr, removed);

		return true;
	}
};

// calls:
// T::LoadGlobals(PhobosStreamReader&)
struct LoadGlobalsAction
{
	template <typename T>
	static bool Process(IStream* pStm)
	{
		if constexpr (GlobalSaveLoadable<T>)
		{
			PhobosByteStream stm(0);
			stm.ReadBlockFromStream(pStm);
			PhobosStreamReader reader(stm);

			return T::LoadGlobals(reader) && reader.ExpectEndOfBlock();
		}
		else
		{
			return true;
		}
	}
};

// calls:
// T::SaveGlobals(PhobosStreamWriter&)
struct SaveGlobalsAction
{
	template <typename T>
	static bool Process(IStream* pStm)
	{
		if constexpr (GlobalSaveLoadable<T>)
		{
			PhobosByteStream stm;
			PhobosStreamWriter writer(stm);

			return T::SaveGlobals(writer) && stm.WriteBlockToStream(pStm);
		}
		else
		{
			return true;
		}
	}
};

// Diagnostic: T::ExtMap.ValidateCachedPointers()
struct ValidateCachedPointersAction
{
	template <typename T>
	static bool Process()
	{
		if constexpr (HasExtMap<T>)
			T::ExtMap.ValidateCachedPointers();

		return true;
	}
};

// this is a complicated thing that calls methods on classes. add types to the
// instantiation of this type, and the most appropriate method for each type
// will be called with no overhead of virtual functions.
template <typename... RegisteredTypes>
struct TypeRegistry
{
	__forceinline static void Clear()
	{
		dispatch_mass_action<ClearAction>();
	}

	__forceinline static void InvalidatePointer(void* ptr, bool removed)
	{
		dispatch_mass_action<InvalidatePointerAction>(ptr, removed);
	}

	__forceinline static bool LoadGlobals(IStream* pStm)
	{
		return dispatch_mass_action<LoadGlobalsAction>(pStm);
	}

	__forceinline static bool SaveGlobals(IStream* pStm)
	{
		return dispatch_mass_action<SaveGlobalsAction>(pStm);
	}

	__forceinline static void ValidateCachedPointers()
	{
		dispatch_mass_action<ValidateCachedPointersAction>();
	}

private:
	// TAction: the method dispatcher class to call with each type
	// ArgTypes: the argument types to call the method dispatcher's Process() method
	template <typename TAction, typename... ArgTypes>
		requires (DispatchesAction<TAction, RegisteredTypes, ArgTypes...> && ...)
	__forceinline static bool dispatch_mass_action(ArgTypes... args)
	{
		// (pack expression op ...) is a fold expression which
		// unfolds the parameter pack into a full expression
		return (TAction::template Process<RegisteredTypes>(args...) && ...);
	}
};

#pragma endregion

// Add more class names as you like
using PhobosTypeRegistry = TypeRegistry <
	// Ext classes
	AircraftExt,
	AnimTypeExt,
	AnimExt,
	BuildingExt,
	BuildingTypeExt,
	BulletExt,
	BulletTypeExt,
	CellExt,
	EBoltExt,
	HouseExt,
	OverlayTypeExt,
	ParticleSystemTypeExt,
	RadSiteExt,
	RulesExt,
	ScenarioExt,
	ScriptExt,
	SideExt,
	SWTypeExt,
	SmudgeTypeExt,
	TActionExt,
	TeamExt,
	TechnoExt,
	TechnoTypeExt,
	TerrainTypeExt,
	TiberiumExt,
	VoxelAnimExt,
	VoxelAnimTypeExt,
	WarheadTypeExt,
	WeaponTypeExt,
	// New classes
	ShieldTypeClass,
	LaserTrailTypeClass,
	RadTypeClass,
	ShieldClass,
	DigitalDisplayTypeClass,
	BannerTypeClass,
	BannerClass,
	AttachEffectTypeClass,
	AttachEffectClass,
	NewSWType,
	SelectBoxTypeClass,
	AttachmentClass,
	AttachmentTypeClass
	// other classes
> ;

DEFINE_HOOK(0x7258D0, AnnounceInvalidPointer, 0x6)
{
	GET(AbstractClass* const, pInvalid, ECX);
	GET(bool const, removed, EDX);

	PhobosTypeRegistry::InvalidatePointer(pInvalid, removed);

	return 0;
}

// -----------------------------------------------------------------------------
// [GC-diag] INI structure validation + map-pack read logging.
// -----------------------------------------------------------------------------
namespace Diagnostics
{
	inline bool IsReadable(const void* p, size_t len)
	{
		if (!p)
			return false;

		MEMORY_BASIC_INFORMATION mbi {};

		if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
			return false;

		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
			return false;

		const auto start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

		if (reinterpret_cast<uintptr_t>(p) + len > start + mbi.RegionSize)
			return false;

		return true;
	}

	inline bool IsPlainCStr(const char* s)
	{
		if (!s)
			return true;

		// A wild small pointer (e.g. 0x21) is never a valid string.
		if (reinterpret_cast<uintptr_t>(s) < 0x10000)
			return false;

		MEMORY_BASIC_INFORMATION mbi {};

		if (VirtualQuery(s, &mbi, sizeof(mbi)) == 0)
			return false;

		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
			return false;

		const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		size_t maxLen = regionEnd - reinterpret_cast<uintptr_t>(s);

		if (maxLen > 4096)
			maxLen = 4096;

		// Bytes >= 0x80 are fine (localized text such as 0xC2 0xA0); only reject
		// embedded C0 control bytes.
		for (size_t i = 0; i < maxLen; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);

			if (c == 0)
				return true;

			if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D)
				return false;
		}

		return false;
	}

	// [GC-diag] Ring of recently destroyed INIEntry pointers, WITH the caller that
	// freed them and a short stack so we can identify WHO freed an entry that is
	// later found still referenced (the map INI UAF). ValidateINI prints it.
	struct FreedRecord
	{
		void* Ptr;
		void* Caller;
		unsigned KeyHash;
		unsigned short NumFrames;
		void* Frames[8];
	};

	constexpr unsigned FreedRingSize = 1u << 14;
	FreedRecord FreedRing[FreedRingSize] = {};
	volatile LONG FreedRingPos = 0;

	typedef USHORT(WINAPI* FnCaptureBt)(ULONG, ULONG, PVOID*, PULONG);

	inline FnCaptureBt GetCaptureBt()
	{
		static FnCaptureBt p = reinterpret_cast<FnCaptureBt>(
			GetProcAddress(GetModuleHandleA("kernel32.dll"), "CaptureStackBackTrace"));
		return p;
	}

	void RecordFreedEntry(void* p, void* caller, unsigned keyHash)
	{
		if (!p)
			return;

		const LONG i = InterlockedIncrement(&FreedRingPos);
		FreedRecord& r = FreedRing[static_cast<unsigned>(i) & (FreedRingSize - 1)];
		r.Ptr = p;
		r.Caller = caller;
		r.KeyHash = keyHash;
		r.NumFrames = 0;

		if (FnCaptureBt pCap = GetCaptureBt())
			r.NumFrames = pCap(0, 8, r.Frames, nullptr);
	}

	const FreedRecord* FindFreed(void* p)
	{
		if (!p)
			return nullptr;

		for (unsigned i = 0; i < FreedRingSize; ++i)
		{
			if (FreedRing[i].Ptr == p)
				return &FreedRing[i];
		}

		return nullptr;
	}

	bool IsFreedEntry(void* p)
	{
		return FindFreed(p) != nullptr;
	}

	// [GC-diag] Freed-entry tracking. Fixed, zero-initialized POD table so the
	// destructor probe performs NO heap allocation and cannot perturb the heap
	// layout (an earlier unordered_map+mutex version shifted the heap and made a
	// latent corruption crash much earlier). Keyed by pointer + key hash.
	struct FreedRec
	{
		void* Ptr;
		unsigned Hash;
		void* Ret;   // caller that performed the free
		unsigned Gen; // generation; stale slots are ignored instead of cleared
	};

	constexpr unsigned FreedTableSize = 1u << 18;
	FreedRec FreedTable[FreedTableSize] = {};
	volatile LONG g_freedGen = 0;

	// [GC-diag] Log rate limits so a teardown that hammers the probe cannot flood
	// the log (a genuine double-free loop once wrote 123 MB in 40 s).
	volatile LONG g_iniDfCount = 0;
	volatile LONG g_iniUafCount = 0;
	volatile LONG g_dangleChecks = 0;
	volatile LONG g_dangleCount = 0;
	volatile LONG g_guardCount = 0;

	inline unsigned HashKeyString(const char* s)
	{
		if (!s || !IsReadable(s, 1))
			return 0;

		unsigned h = 2166136261u;

		for (int i = 0; i < 40 && s[i]; ++i)
		{
			h ^= static_cast<unsigned char>(s[i]);
			h *= 16777619u;
		}

		return h;
	}

	// [GC-diag] Per-scenario reset: bumping the generation invalidates all slots
	// in O(1), so a heap address reused across scenarios is not mistaken for a
	// repeat free.
	void ResetFreedHash()
	{
		InterlockedIncrement(&g_freedGen);
		g_dangleChecks = 0;
		g_dangleCount = 0;
	}

	// Returns true iff this exact entry (pointer + key hash) was already
	// destroyed in the current generation. Records it otherwise. Single-threaded
	// (INI parsing runs on the engine main thread), so no lock is needed.
	bool NoteFreedEntry(void* pThis, unsigned keyHash, void* callerRet, void** firstRet)
	{
		const unsigned gen = static_cast<unsigned>(g_freedGen);
		unsigned i = (static_cast<unsigned>(reinterpret_cast<uintptr_t>(pThis) >> 4)) & (FreedTableSize - 1);

		for (unsigned n = 0; n < FreedTableSize; ++n)
		{
			FreedRec& r = FreedTable[i];

			if (r.Gen != gen)
			{
				r.Ptr = pThis;
				r.Hash = keyHash;
				r.Ret = callerRet;
				r.Gen = gen;
				return false;
			}

			if (r.Ptr == pThis)
			{
				if (r.Hash == keyHash)
				{
					if (firstRet)
						*firstRet = r.Ret;

					return true; // same object freed again
				}

				r.Hash = keyHash; // heap address reused by a different entry
				r.Ret = callerRet;
				return false;
			}

			i = (i + 1) & (FreedTableSize - 1);
		}

		return false;
	}

	// The map INI currently being parsed (armed by CCINIClass_Load_Inheritance so
	// the INIEntry destructor probe can spot a still-indexed entry being freed).
	CCINIClass* CurrentMapINI = nullptr;

	void SetCurrentMapINI(CCINIClass* pINI)
	{
		CurrentMapINI = pINI;
	}

	void ClearCurrentMapINI()
	{
		CurrentMapINI = nullptr;
	}

	// [GC-diag] Does a live INIClass still reference pEnt in one of its section
	// entry indexes? This is the REAL defect probe: if an INIEntry is destroyed
	// while still indexed, the section later dereferences freed memory (the
	// "C0000005 ... Key=0x21" popup). Scanning is bounded by budget + the caller
	// only invoking it for UU entries, so it cannot become O(all entries) per
	// destroy.
	struct IndexHit
	{
		CCINIClass* Ini;
		INIClass::INISection* Sec;
	};

	IndexHit FindIndexedEntry(INIClass::INIEntry* pEnt, long& budget)
	{
		CCINIClass* const candidates[] =
		{
			CurrentMapINI,
			CCINIClass::INI_Rules,
			&CCINIClass::INI_Art,
			&CCINIClass::INI_AI,
			&CCINIClass::INI_RA2MD,
			&CCINIClass::INI_UIMD
		};

		for (CCINIClass* pINI : candidates)
		{
			if (!pINI || !IsReadable(pINI, sizeof(void*)))
				continue;

			const auto& idx = pINI->SectionIndex;

			if (idx.IndexCount < 0 || idx.IndexCount > 0x100000)
				continue;

			if (idx.IndexCount && !IsReadable(idx.IndexTable, sizeof(INIClass::IndexType::NodeElement) * idx.IndexCount))
				continue;

			for (int i = 0; i < idx.IndexCount; ++i)
			{
				if (--budget < 0)
					return {};

				INIClass::INISection* pSec = idx.IndexTable[i].Data;

				if (!pSec || !IsReadable(pSec, sizeof(INIClass::INISection)))
					continue;

				const auto& eidx = pSec->EntryIndex;

				if (eidx.IndexCount < 0 || eidx.IndexCount > 0x100000)
					continue;

				if (eidx.IndexCount && !IsReadable(eidx.IndexTable, sizeof(INIClass::IndexType::NodeElement) * eidx.IndexCount))
					continue;

				for (int j = 0; j < eidx.IndexCount; ++j)
				{
					if (--budget < 0)
						return {};

					if (eidx.IndexTable[j].Data == pEnt)
						return { pINI, pSec };
				}
			}
		}

		return {};
	}

	// [GC-diag] Same idea as FindIndexedEntry but for the INTRUSIVE LIST the
	// engine actually walks when it crashes at 0x52ABB4 (it follows Node Next/
	// Prev at [node+4]/[node+8], not the IndexClass array). A correctly freed
	// entry is unlinked from its section's Entries list, so still finding pEnt
	// linked here means the free left a dangling list node. Every hop is guarded
	// and bounded because the target may already be freed/overwritten.
	struct ListHit
	{
		CCINIClass* Ini;
		INIClass::INISection* Sec;
		const char* Where; // "Sections" or "Entries"
	};

	ListHit FindListEntry(INIClass::INIEntry* pEnt, long& budget)
	{
		CCINIClass* const candidates[] =
		{
			CurrentMapINI,
			CCINIClass::INI_Rules,
			&CCINIClass::INI_Art,
			&CCINIClass::INI_AI,
			&CCINIClass::INI_RA2MD,
			&CCINIClass::INI_UIMD
		};

		GenericNode* const target = reinterpret_cast<GenericNode*>(pEnt);

		for (CCINIClass* pINI : candidates)
		{
			if (!pINI || !IsReadable(pINI, 0x40))
				continue;

			auto* secList = reinterpret_cast<GenericList*>(&pINI->Sections);

			for (GenericNode* secNode = secList->First();
				secNode && IsReadable(secNode, 0xC);
				secNode = secNode->Next())
			{
				if (--budget < 0)
					return {};

				if (secNode == target)
					return { pINI, nullptr, "Sections" };

				if (!secNode->IsValid())
					break;

				auto* pSec = reinterpret_cast<INIClass::INISection*>(secNode);

				if (!IsReadable(pSec, 0x10))
					break;

				auto* entList = reinterpret_cast<GenericList*>(&pSec->Entries);

				for (GenericNode* entNode = entList->First();
					entNode && IsReadable(entNode, 0xC);
					entNode = entNode->Next())
				{
					if (--budget < 0)
						return {};

					if (entNode == target)
						return { pINI, pSec, "Entries" };

					if (!entNode->IsValid())
						break;
				}
			}
		}

		return {};
	}

	// [GC-diag] Real stack backtrace (frames left unresolved; addresses are
	// mapped offline against gamemd.exe / Phobos.pdb).
	void LogBacktrace(const char* tag, int maxFrames)
	{
		typedef USHORT(WINAPI* FnCapture)(ULONG, ULONG, PVOID*, PULONG);
		static FnCapture pCapture = reinterpret_cast<FnCapture>(
			GetProcAddress(GetModuleHandleA("kernel32.dll"), "CaptureStackBackTrace"));

		if (!pCapture)
			return;

		if (maxFrames > 40)
			maxFrames = 40;

		void* frames[40];
		const USHORT n = pCapture(0, static_cast<ULONG>(maxFrames), frames, nullptr);

		for (USHORT i = 0; i < n; ++i)
			Debug::Log("%s   #%02u %p\n", tag, i, frames[i]);
	}

	// [GC-diag] Hexdump a region so a corrupted INIEntry can be inspected: the
	// first dwords show vtable(0)/Next(4)/Prev(8)/Key(0xC)/Value(0x10), which
	// reveals what a stray write left behind.
	void DumpBytes(const char* tag, const void* p, size_t len)
	{
		if (!p || !IsReadable(p, len))
		{
			Debug::Log("[INI-diag] %s unreadable @ %p len=%u\n", tag, p, static_cast<unsigned>(len));
			return;
		}

		static const char hexd[] = "0123456789ABCDEF";
		const unsigned char* b = static_cast<const unsigned char*>(p);
		char line[16 * 3 + 1];

		for (size_t i = 0; i < len; i += 16)
		{
			int pos = 0;

			for (size_t k = 0; k < 16 && i + k < len; ++k)
			{
				const unsigned char c = b[i + k];
				line[pos++] = hexd[c >> 4];
				line[pos++] = hexd[c & 0xF];
				line[pos++] = ' ';
			}

			line[pos] = 0;
			Debug::Log("[INI-diag] %s %p: %s\n", tag, static_cast<const void*>(b + i), line);
		}
	}

	// A live INIEntry's vtable slot0 is the (deleting) destructor. Phobos itself
	// redirects that slot into Phobos.dll, so it is NOT always inside gamemd -
	// accept any executable page (gamemd, Phobos, Syringe trampolines, ...). Only
	// a slot0 that points into non-executable memory (e.g. reused heap) is
	// corrupt. This is the criterion the guard and the scan share.
	inline bool IsExecutablePtr(const void* p)
	{
		if (!p)
			return false;

		MEMORY_BASIC_INFORMATION mbi {};

		if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
			return false;

		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
			return false;

		const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return (mbi.Protect & exec) != 0;
	}

	// [GC-diag] Live vtable pointers observed in this build:
	//   INIEntry      -> 0x7EB734   (Phobos "deleting dtor" slot is patched here)
	//   GenericNode / GenericList (list sentinels) -> 0x7E1B0C / 0x7E1B04
	// A section's Entries list contains INIEntry nodes; its sentinel (LastNode) is
	// a GenericNode. So: only a node whose vtable is exactly INIEntry counts, and
	// walking must stop at the sentinel (IsValid()==false). Any other vtable on an
	// Entries node means the node's vtable was overwritten (freed/reused) - the
	// exact node the teardown `call [edx]` jumps into the heap on.
	constexpr uintptr_t kINIEntryVtable = 0x7EB734;

	// [GC] Rebuild a section's `Entries` intrusive list from its (usually intact)
	// EntryIndex. Used as a REPAIR when the list is found corrupted (heap OOB
	// write), so every later engine walk - INISection teardown, Read_Smudge,
	// INIClass::Reset, ReadString - sees a valid, self-consistent list.
	// Layout: INISection: Entries(List) at +0x10, FirstNode.Next/Prev at
	// +0x18/+0x1C, LastNode.Next/Prev at +0x24/+0x28; INIEntry Node.Next/Prev at
	// +0x4/+0x8.
	void RepairSectionEntries(INIClass::INISection* pSec)
	{
		if (!pSec || !IsReadable(pSec, 0x40))
			return;

		const auto& eidx = pSec->EntryIndex;

		if (eidx.IndexCount < 0 || eidx.IndexCount > 0x100000)
			return;

		if (eidx.IndexCount && !IsReadable(eidx.IndexTable, sizeof(INIClass::IndexType::NodeElement) * eidx.IndexCount))
			return;

		char* const secBytes = reinterpret_cast<char*>(pSec);
		void* const sentinelFirst = secBytes + 0x14; // FirstNode (GenericNode)
		void* const sentinelLast = secBytes + 0x20;  // LastNode

		void* prev = sentinelFirst;
		int linked = 0;

		for (int j = 0; j < eidx.IndexCount; ++j)
		{
			auto* pEnt = reinterpret_cast<INIClass::INIEntry*>(eidx.IndexTable[j].Data);

			if (!pEnt || !IsReadable(pEnt, 0x14))
				continue;

			if (*reinterpret_cast<uintptr_t const*>(pEnt) != kINIEntryVtable)
				continue;

			if (pEnt->Key && !IsReadable(pEnt->Key, 1))
				continue;

			*reinterpret_cast<void**>(reinterpret_cast<char*>(pEnt) + 8) = prev; // e.Prev = prev
			*reinterpret_cast<void**>(reinterpret_cast<char*>(prev) + 4) = pEnt; // prev.Next = e
			prev = pEnt;
			++linked;
		}

		*reinterpret_cast<void**>(reinterpret_cast<char*>(prev) + 4) = sentinelLast;    // last.Next = &LastNode
		*reinterpret_cast<void**>(reinterpret_cast<char*>(sentinelLast) + 8) = prev;    // LastNode.Prev = last
		*reinterpret_cast<void**>(reinterpret_cast<char*>(sentinelFirst) + 8) = nullptr; // FirstNode.Prev = 0
		*reinterpret_cast<void**>(reinterpret_cast<char*>(sentinelLast) + 4) = nullptr;  // LastNode.Next = 0

		Debug::Log("[INI-REPAIR] section='%s' relinked %d entries (list was corrupt)\n",
			IsPlainCStr(pSec->Name) ? pSec->Name : "?", linked);
	}

	void ScanINIListNodes(const char* tag, CCINIClass* pINI)
	{
		if (!pINI || !IsReadable(pINI, 0x40))
			return;

		unsigned badEntry = 0, badLink = 0, badSec = 0;
		long budget = 100000;

		auto* secList = reinterpret_cast<GenericList*>(&pINI->Sections);

		for (GenericNode* secNode = secList->First();
			secNode && IsReadable(secNode, 0xC) && secNode->IsValid();
			secNode = secNode->Next())
		{
			if (--budget < 0)
				break;

			auto* pSec = reinterpret_cast<INIClass::INISection*>(secNode);

			if (!IsReadable(pSec, 0x2C))
			{
				++badSec;
				break;
			}

			const bool secNameOk = IsPlainCStr(pSec->Name);

			if (!secNameOk)
				++badSec;

			auto* entList = reinterpret_cast<GenericList*>(&pSec->Entries);
			unsigned secBadE = 0, secBadL = 0;

			for (GenericNode* entNode = entList->First();
				entNode && IsReadable(entNode, 0xC) && entNode->IsValid();
				entNode = entNode->Next())
			{
				if (--budget < 0)
					break;

				GenericNode* nx = entNode->Next();

				if (nx && IsReadable(nx, 0xC) && nx->IsValid() && nx->Prev() != entNode)
				{
					++badLink;
					++secBadL;

					if (badLink <= 8)
						Debug::Log("[INI-VALID] %s section='%s' broken link node=%p next=%p next->prev=%p\n",
							tag, secNameOk ? pSec->Name : "?", static_cast<void*>(entNode),
							static_cast<void*>(nx), static_cast<void*>(nx->Prev()));
				}

				const uintptr_t vt = IsReadable(entNode, 4) ? *reinterpret_cast<uintptr_t const*>(entNode) : 0;
				void* slot0 = (vt && IsReadable(reinterpret_cast<void*>(vt), 4)) ? *reinterpret_cast<void* const*>(vt) : nullptr;

				if (vt != kINIEntryVtable || !IsExecutablePtr(slot0))
				{
					++badEntry;
					++secBadE;

					if (badEntry <= 16)
					{
						Debug::Log("[INI-VALID] %s section='%s' bad INIEntry node=%p vt=%p slot0=%p\n",
							tag, secNameOk ? pSec->Name : "?", static_cast<void*>(entNode),
							reinterpret_cast<void*>(vt), slot0);

						// Surface WHO freed this node if it is one of our destroyed
						// INIEntries: this is the smoking gun for the list UAF.
						if (const FreedRecord* fr = FindFreed(entNode))
						{
							Debug::Log("[INI-LISTFREE] section='%s' node=%p was FREED by caller=%p keyHash=%08X; still linked in Entries (UAF)\n",
								secNameOk ? pSec->Name : "?", static_cast<void*>(entNode), fr->Caller, fr->KeyHash);

							for (unsigned k = 0; k < fr->NumFrames && k < 8; ++k)
								Debug::Log("[INI-LISTFREE]   freed from frame[%u] %p\n", k, fr->Frames[k]);
						}
					}
				}
			}

			// [GC] REPAIR: rebuild the Entries list from the EntryIndex so the
			// engine's later walks (teardown / Read_Smudge / Reset) cannot hit the
			// garbage nodes.
			if (secBadE || secBadL)
				RepairSectionEntries(pSec);
		}

		if (badEntry || badLink || badSec)
			Debug::Log("[INI-VALID] %s: %u bad entries, %u broken links, %u bad sections\n",
				tag, badEntry, badLink, badSec);
	}

	inline bool CStrContains(const char* s, const char* sub)
	{
		if (!s || !sub || !IsReadable(s, 1))
			return false;

		for (int i = 0; i < 64 && s[i]; ++i)
		{
			int j = 0;

			while (sub[j] && s[i + j] == sub[j])
				++j;

			if (!sub[j])
				return true;
		}

		return false;
	}

	inline bool CStrEqualsI(const char* a, const char* b)
	{
		if (!a || !b)
			return false;

		for (int i = 0; i < 64; ++i)
		{
			char ca = a[i];
			char cb = b[i];

			if (ca >= 'A' && ca <= 'Z') ca += 32;
			if (cb >= 'A' && cb <= 'Z') cb += 32;

			if (ca != cb)
				return false;

			if (!ca)
				return true;
		}

		return false;
	}

	bool IniHasSection(CCINIClass* pINI, const char* name)
	{
		if (!pINI || !IsReadable(pINI, 0x40) || !name)
			return false;

		auto* secList = reinterpret_cast<GenericList*>(&pINI->Sections);
		long budget = 4096;

		for (GenericNode* n = secList->First();
			n && IsReadable(n, 0xC) && n->IsValid();
			n = n->Next())
		{
			if (--budget < 0)
				break;

			auto* pSec = reinterpret_cast<INIClass::INISection*>(n);

			if (IsReadable(pSec, 0x2C) && IsPlainCStr(pSec->Name) && pSec->Name && CStrEqualsI(pSec->Name, name))
				return true;
		}

		return false;
	}

	// [GC] True if the named section's Entries list contains any corrupt node.
	// Used to make ReadUUBlock skip decoding a section whose packed data was
	// destroyed by the heap corruption, instead of feeding garbage to the LCW
	// decoder (which overruns and crashes).
	bool SectionListCorrupt(CCINIClass* pINI, const char* name)
	{
		if (!pINI || !IsReadable(pINI, 0x40) || !name)
			return false;

		auto* secList = reinterpret_cast<GenericList*>(&pINI->Sections);

		for (GenericNode* n = secList->First(); n && IsReadable(n, 0xC) && n->IsValid(); n = n->Next())
		{
			auto* pSec = reinterpret_cast<INIClass::INISection*>(n);

			if (!IsReadable(pSec, 0x2C) || !IsPlainCStr(pSec->Name) || !pSec->Name || !CStrEqualsI(pSec->Name, name))
				continue;

			auto* entList = reinterpret_cast<GenericList*>(&pSec->Entries);
			long budget = 200000;

			for (GenericNode* e = entList->First(); e && IsReadable(e, 0xC) && e->IsValid(); e = e->Next())
			{
				if (--budget < 0)
					break;

				const uintptr_t vt = IsReadable(e, 4) ? *reinterpret_cast<uintptr_t const*>(e) : 0;
				void* slot0 = (vt && IsReadable(reinterpret_cast<void*>(vt), 4)) ? *reinterpret_cast<void* const*>(vt) : nullptr;

				if (vt != kINIEntryVtable || !IsExecutablePtr(slot0))
					return true;
			}

			return false;
		}

		return false;
	}

	// [GC-diag] Validate a single named section's Entries list (Phase-0 rules).
	// Used right BEFORE INIClass::WriteString populates IsoMapPack5, to decide
	// whether the map INIClass was already corrupt before this load wrote it
	// (carried-over/stale) or only gets corrupted by the writes.
	void ValidateSectionEntries(CCINIClass* pINI, const char* wantName, const char* tag)
	{
		if (!pINI || !IsReadable(pINI, 0x40) || !wantName || !IsReadable(wantName, 1))
			return;

		auto* secList = reinterpret_cast<GenericList*>(&pINI->Sections);

		for (GenericNode* secNode = secList->First();
			secNode && IsReadable(secNode, 0xC) && secNode->IsValid();
			secNode = secNode->Next())
		{
			auto* pSec = reinterpret_cast<INIClass::INISection*>(secNode);

			if (!IsReadable(pSec, 0x2C))
				break;

			const bool nameOk = IsPlainCStr(pSec->Name);

			if (!nameOk || !pSec->Name || !CStrEqualsI(pSec->Name, wantName))
				continue;

			unsigned total = 0, badEntry = 0, badLink = 0;
			long budget = 100000;
			auto* entList = reinterpret_cast<GenericList*>(&pSec->Entries);

			for (GenericNode* entNode = entList->First();
				entNode && IsReadable(entNode, 0xC) && entNode->IsValid();
				entNode = entNode->Next())
			{
				if (--budget < 0)
					break;

				++total;
				GenericNode* nx = entNode->Next();

				if (nx && IsReadable(nx, 0xC) && nx->IsValid() && nx->Prev() != entNode)
					++badLink;

				const uintptr_t vt = IsReadable(entNode, 4) ? *reinterpret_cast<uintptr_t const*>(entNode) : 0;
				void* slot0 = (vt && IsReadable(reinterpret_cast<void*>(vt), 4)) ? *reinterpret_cast<void* const*>(vt) : nullptr;

				if (vt != kINIEntryVtable || !IsExecutablePtr(slot0))
					++badEntry;
			}

			Debug::Log("[INI-VALID] %s PRE section='%s' ini=%p: %u nodes, %u bad entries, %u broken links\n",
				tag, pSec->Name, static_cast<void*>(pINI), total, badEntry, badLink);
			return;
		}

		Debug::Log("[INI-VALID] %s PRE section='%s' ini=%p: absent\n", tag, wantName, static_cast<void*>(pINI));
	}

	void ValidateINI(const char* tag, CCINIClass* pINI)
	{
		if (!pINI || !IsReadable(pINI, sizeof(void*)))
			return;

		ScanINIListNodes(tag, pINI);

		const auto& idx = pINI->SectionIndex;

		if (idx.IndexCount < 0 || idx.IndexCount > 0x100000)
		{
			Debug::Log("[INI-diag] %s: insane section count %d (table %p)\n", tag, idx.IndexCount, idx.IndexTable);
			return;
		}

		if (idx.IndexCount && !IsReadable(idx.IndexTable, sizeof(INIClass::IndexType::NodeElement) * idx.IndexCount))
		{
			Debug::Log("[INI-diag] %s: SectionIndex table unreadable (%p cnt=%d)\n", tag, idx.IndexTable, idx.IndexCount);
			return;
		}

		unsigned int badSec = 0, badEnt = 0;

		for (int i = 0; i < idx.IndexCount; ++i)
		{
			INIClass::INISection* pSec = idx.IndexTable[i].Data;

			if (!pSec || !IsReadable(pSec, sizeof(INIClass::INISection)))
			{
				++badSec;
				Debug::Log("[INI-diag] %s: bad section ptr idx=%d = %p\n", tag, i, pSec);
				continue;
			}

			if (!IsPlainCStr(pSec->Name))
			{
				++badSec;
				Debug::Log("[INI-diag] %s: section[%d] corrupt Name=%p\n", tag, i, pSec->Name);
			}

			const auto& eidx = pSec->EntryIndex;

			if (eidx.IndexCount < 0 || eidx.IndexCount > 0x100000)
			{
				Debug::Log("[INI-diag] %s: section '%s' insane entry count %d\n", tag, pSec->Name ? pSec->Name : "?", eidx.IndexCount);
				continue;
			}

			if (eidx.IndexCount && !IsReadable(eidx.IndexTable, sizeof(INIClass::IndexType::NodeElement) * eidx.IndexCount))
			{
				Debug::Log("[INI-diag] %s: section '%s' entry table unreadable\n", tag, pSec->Name ? pSec->Name : "?");
				continue;
			}

			for (int j = 0; j < eidx.IndexCount; ++j)
			{
				INIClass::INIEntry* pEnt = eidx.IndexTable[j].Data;

				if (!pEnt || !IsReadable(pEnt, sizeof(INIClass::INIEntry)))
				{
					++badEnt;
					continue;
				}

				if (!IsPlainCStr(pEnt->Key) || !IsPlainCStr(pEnt->Value))
				{
					++badEnt;
					Debug::Log("[INI-diag] %s: section '%s' corrupt entry[%d] pEnt=%p Key=%p Value=%p\n",
						tag, pSec->Name ? pSec->Name : "?", j, pEnt, pEnt->Key, pEnt->Value);

					if (const FreedRecord* fr = FindFreed(pEnt))
					{
						Debug::Log("[INI-diag]   ^ pEnt %p was FREED by caller=%p keyHash=%08X and is still referenced (UAF)\n",
							pEnt, fr->Caller, fr->KeyHash);

						for (unsigned k = 0; k < fr->NumFrames && k < 8; ++k)
							Debug::Log("[INI-diag]       freed from frame[%u] %p\n", k, fr->Frames[k]);
					}

					if (badEnt <= 3)
					{
						DumpBytes("  pre ", reinterpret_cast<const char*>(pEnt) - 0x20, 0x20);
						DumpBytes("  ent ", pEnt, sizeof(INIClass::INIEntry));
						DumpBytes("  post", reinterpret_cast<const char*>(pEnt) + sizeof(INIClass::INIEntry), 0x20);
					}
				}
			}
		}

		if (badSec || badEnt)
			Debug::Log("[INI-diag] %s: %u bad sections, %u bad entries\n", tag, badSec, badEnt);
	}

	void ValidateAllINIs(const char* where)
	{
		Debug::Log("[INI-diag] ==== validate (%s) ====\n", where);
		ValidateINI("Rules", CCINIClass::INI_Rules);
		ValidateINI("Art", &CCINIClass::INI_Art);
		ValidateINI("AI", &CCINIClass::INI_AI);
		ValidateINI("RA2MD", &CCINIClass::INI_RA2MD);
		ValidateINI("UIMD", &CCINIClass::INI_UIMD);
	}

	void ValidateMapINI(const char* stage)
	{
		Debug::Log("[INI-VALID] ==== stage: %s ====\n", stage);
		ValidateINI(stage, CurrentMapINI);
	}
}

// [GC-diag] ReadUUBlock probe. ScenarioClass::Read_INI decodes IsoMapPack5 /
// OverlayPack / OverlayDataPack through INIClass::ReadUUBlock; re-target those
// call sites so the owning INI is validated (and a bad entry dumped with its
// section name) right before the engine decodes the block. This is a rel32 data
// patch on the call, not a code hook - hooking 0x526FB0 itself broke Syringe's
// hook creation.
extern "C" int __cdecl INIClass_ReadUUBlock_Probe_Helper(CCINIClass* pThis, const char* pSection)
{
	if (!pSection || !Diagnostics::IsReadable(pSection, 1))
		return 0;

	// Decide corrupt-vs-not BEFORE ValidateINI, because ValidateINI repairs the
	// list (making the later SectionListCorrupt test return false). If the section
	// is corrupt we skip the decode: feeding garbage to the LCW decoder overruns
	// and crashes. Skipping loses that map layer but keeps the process alive.
	const int corrupt = Diagnostics::SectionListCorrupt(pThis, pSection) ? 1 : 0;

	Diagnostics::ValidateINI(pSection, pThis);

	return corrupt;
}

// Naked tail-thunk: validates (preserving ECX/args); if the section is corrupt
// it returns 0 (skip the decode) with correct __thiscall cleanup (ret 0xC),
// otherwise it jumps to the original ReadUUBlock (which does its own ret 0xC).
__declspec(naked) void INIClass_ReadUUBlock_Probe_Thunk()
{
	__asm
	{
		push ebx
		mov ebx, ecx
		mov eax, [esp + 8]      // pSection
		push eax
		push ebx                // pThis
		call INIClass_ReadUUBlock_Probe_Helper
		add esp, 8
		test eax, eax
		jne skip
		mov ecx, ebx
		pop ebx
		mov eax, 0x526FB0
		jmp eax
	skip:
		xor eax, eax
		pop ebx
		ret 0xC
	}
}

DEFINE_FUNCTION_JUMP(CALL, 0x4AD3BB, INIClass_ReadUUBlock_Probe_Thunk); // map UU blocks
DEFINE_FUNCTION_JUMP(CALL, 0x4AD46E, INIClass_ReadUUBlock_Probe_Thunk);
DEFINE_FUNCTION_JUMP(CALL, 0x4AD521, INIClass_ReadUUBlock_Probe_Thunk);
DEFINE_FUNCTION_JUMP(CALL, 0x4AD5D4, INIClass_ReadUUBlock_Probe_Thunk);
DEFINE_FUNCTION_JUMP(CALL, 0x4AD687, INIClass_ReadUUBlock_Probe_Thunk);
DEFINE_FUNCTION_JUMP(CALL, 0x5FD37B, INIClass_ReadUUBlock_Probe_Thunk); // OverlayPack
DEFINE_FUNCTION_JUMP(CALL, 0x5FD580, INIClass_ReadUUBlock_Probe_Thunk); // OverlayDataPack

// [GC-diag] INIEntry destructor probe. SAFE / non-iterating: does not walk any
// INIClass (walking one that is being torn down was what broke startup). It only
// logs when the entry being destroyed looks like an IsoMapPack5 UU entry: a
// 1..5 digit numeric Key and a long (>64 char) Value. The corresponding stack
// shows whoever frees an IsoMapPack5 entry during map parsing.
// [GC] INIEntry destructor probe + double-free detection. WinDbg classified the
// restart crash as HEAP_CORRUPTION ... DOUBLE_FREE of an INIEntry. We record the
// (pointer, key hash) of each destroy together with the caller address and log
// the first few genuine second frees so the two call sites can be compared.
//
// IMPORTANT: the original destructor is ALWAYS run. The engine teardown loop at
// 0x52AB9B re-reads its list head and relies on the node's destructor unlinking
// it; returning early makes that loop destroy the same node forever (it wrote
// 123 MB of log and hung the game). The double free is the real bug and has to
// be fixed where the entry is registered/freed twice, not masked here.
void* __fastcall INIEntry_Dtor_Probe(INIClass::INIEntry* pThis, void* /*edx*/, unsigned int flags)
{
	const bool fieldsReadable = pThis && pThis->Key && pThis->Value
		&& Diagnostics::IsReadable(pThis->Key, 1) && Diagnostics::IsReadable(pThis->Value, 65);

	const bool keyReadable = pThis && pThis->Key && Diagnostics::IsReadable(pThis->Key, 1);
	const unsigned keyHash = keyReadable ? Diagnostics::HashKeyString(pThis->Key) : 0;

	void* const callerRet = *reinterpret_cast<void**>(_AddressOfReturnAddress());

	// Record WHO frees this entry (+ short stack) so a later dangling reference
	// can report the exact free site.
	Diagnostics::RecordFreedEntry(pThis, callerRet, keyHash);

	void* firstRet = nullptr;
	const bool secondFree = Diagnostics::NoteFreedEntry(pThis, keyHash, callerRet, &firstRet);

	// Classify UU entries (1..5 digit numeric Key, >64 char Value) - the
	// IsoMapPack5 / OverlayPack entries that end up dangling.
	bool isUU = false;

	if (fieldsReadable)
	{
		const char* k = pThis->Key;
		int len = 0;
		bool numeric = true;

		for (; k[len]; ++len)
		{
			if (len >= 5 || k[len] < '0' || k[len] > '9')
			{
				numeric = false;
				break;
			}
		}

		if (numeric && len >= 1 && len <= 5)
		{
			isUU = true;
			const auto* v = reinterpret_cast<const unsigned char*>(pThis->Value);

			for (int i = 0; i < 64; ++i)
			{
				if (v[i] == 0)
				{
					isUU = false;
					break;
				}
			}
		}
	}

	// Secondary (noisy) signals, kept for reference only. A "repeat free" here is
	// usually just INIClass::WriteString replacing the same key + address reuse.
	if (secondFree && InterlockedIncrement(&Diagnostics::g_iniDfCount) <= 6)
	{
		Debug::Log("[INI-DF] repeat free %p Key='%s' hash=%08X mapINI=%p first=%p second=%p\n",
			pThis, keyReadable ? pThis->Key : "?", keyHash, Diagnostics::CurrentMapINI, firstRet, callerRet);

		// Frames past 0x5288CC (WriteString) reveal who writes this key twice.
		Diagnostics::LogBacktrace("[INI-DF]", 20);
	}

	if (isUU && InterlockedIncrement(&Diagnostics::g_iniUafCount) <= 8)
	{
		Debug::Log("[INI-UAF] dtor UU-entry %p Key='%s' caller=%p\n", pThis, pThis->Key, callerRet);
	}

	// Preserve the key across the destructor (it frees Key).
	char keyCopy[16] = {};

	if (keyReadable)
	{
		int i = 0;

		while (i < 15 && Diagnostics::IsReadable(pThis->Key + i, 1) && pThis->Key[i])
		{
			keyCopy[i] = pThis->Key[i];
			++i;
		}

		keyCopy[i] = 0;
	}

	using Fn = void*(__thiscall*)(INIClass::INIEntry*, unsigned int);
	void* const ret = reinterpret_cast<Fn>(0x52AD50)(pThis, flags);

	// [GC-diag] PRIMARY evidence, checked AFTER the destructor ran. A correct
	// destroy removes the entry from its section index/list, so a live container
	// still referencing pThis here means the destroy left a dangling node and the
	// section/section-dtor later dereferences freed memory (the popup).
	//
	// The CCINIClass/INISection teardown loop (caller 0x52ABB6) is SKIPPED: it
	// destroys entries precisely to unlink them, so post-dtor the entry is still
	// in the not-yet-cleared index array -> false positive, AND walking a
	// half-destroyed INI here dereferences freed nodes and crashed startup
	// (black screen). Real diagnosis happens at the discrete, safe points via
	// ScanINIListNodes instead.
	if (isUU && callerRet != reinterpret_cast<void*>(0x52ABB6)
		&& InterlockedIncrement(&Diagnostics::g_dangleChecks) <= 4000)
	{
		long ibudget = 40000;
		const auto ihit = Diagnostics::FindIndexedEntry(pThis, ibudget);

		if (ihit.Ini && InterlockedIncrement(&Diagnostics::g_dangleCount) <= 24)
		{
			Debug::Log("[INI-DANGLE] post-dtor still-indexed pEnt=%p Key='%s' ini=%p section='%s' caller=%p\n",
				pThis, keyCopy, ihit.Ini,
				(ihit.Sec && Diagnostics::IsPlainCStr(ihit.Sec->Name)) ? ihit.Sec->Name : "?",
				callerRet);

			Diagnostics::LogBacktrace("[INI-DANGLE]", 32);
		}

		long lbudget = 40000;
		const auto lhit = Diagnostics::FindListEntry(pThis, lbudget);

		if (lhit.Ini && InterlockedIncrement(&Diagnostics::g_dangleCount) <= 24)
		{
			Debug::Log("[INI-DANGLE] post-dtor in-%s pEnt=%p Key='%s' ini=%p section='%s' caller=%p\n",
				lhit.Where, pThis, keyCopy, lhit.Ini,
				(lhit.Sec && Diagnostics::IsPlainCStr(lhit.Sec->Name)) ? lhit.Sec->Name : "?",
				callerRet);

			Diagnostics::LogBacktrace("[INI-DANGLE]", 32);
		}
	}

	return ret;
}

DEFINE_FUNCTION_JUMP(VTABLE, 0x7EB734, INIEntry_Dtor_Probe);

// [GC] NOTE: hooking the engine teardown function 0x52AB80 (L1/L2 guards) made
// the game fail to start - that destructor runs during startup on temporary
// INIs, so NO engine hook may be added on the startup path. Removed. Protection
// is done only from existing map-load hooks (list repair + skip bad decode).

DEFINE_HOOK(0x685659, Scenario_ClearClasses, 0xa)
{
	// [GC-diag] Drop the previous scenario's INIEntry double-free records so a
	// heap address merely reused by the next scenario's map INI is not mistaken
	// for a second free.
	Diagnostics::ResetFreedHash();

	// [GC] Reset Phobos's cross-load INI include/inheritance caches and the
	// diagnostics' current-map pointer. No new engine hook; the caches are just
	// re-derived on the next INI read.
	ResetINIInheritanceCaches();
	Diagnostics::ClearCurrentMapINI();

	// [GC-diag] Arm the swizzle old->new tracker for the upcoming scenario load
	// so a double registration ("declared change to both") is dumped with a stack.
	Savegame::ResetSwizzleDiag();

	const bool heapOk = HeapValidate(GetProcessHeap(), 0, nullptr) != FALSE;

	Debug::Log("[GC-diag] Scenario_ClearClasses: heapOk=%d Bld=%d Veh=%d Inf=%d Air=%d Ovl=%d Ani=%d War=%d Wpn=%d Sfx=%d\n",
		heapOk,
		BuildingTypeClass::Array.Count, UnitTypeClass::Array.Count, InfantryTypeClass::Array.Count,
		AircraftTypeClass::Array.Count, OverlayTypeClass::Array.Count, AnimTypeClass::Array.Count,
		WarheadTypeClass::Array.Count, WeaponTypeClass::Array.Count, SmudgeTypeClass::Array.Count);

	Diagnostics::ValidateAllINIs("Scenario_ClearClasses");

	// The map/scenario INI is not one of the static ones above; validate the INI
	// the $Include handler last saw (the map INI), where the IsoMapPack5/Smudge
	// corruption shows up.
	Diagnostics::ValidateINI("ScenarioMap", Diagnostics::CurrentMapINI);

	PhobosTypeRegistry::ValidateCachedPointers();
	PhobosTypeRegistry::Clear();
	BulletExt::UCPassThrough_SavedTargets.clear();

	// Drop every session-scoped registry that holds scenario object pointers.
	// The referenced objects belong to the scenario being torn down, so keeping
	// these entries would let the next scenario dereference dangling objects.
	BuildingExt::ClearActiveUrbanCombats();
	TechnoExt::ClearPathfindingCaches();
	TeamExt::ClearOffMapState();
	WarheadTypeExt::UCPassThrough_IgnoreSet.clear();
	ReverseMoveCommandClass::LockedUnits.clear();

	return 0;
}

// Ares saves its things at the end of the save
// Phobos will save the things at the beginning of the save
// Considering how DTA gets the scenario name, I decided to save it after Rules - secsome

DEFINE_HOOK(0x67D32C, SaveGame_Phobos, 0x5)
{
	GET(IStream*, pStm, ESI);
	Diagnostics::ValidateMapINI("at SaveGame_Phobos");
	PhobosTypeRegistry::SaveGlobals(pStm);
	return 0;
}

DEFINE_HOOK(0x67E826, LoadGame_Phobos, 0x6)
{
	GET(IStream*, pStm, ESI);

	Diagnostics::ValidateMapINI("at LoadGame_Phobos");

	// NOTE: CreatePendingChildren() is NOT called here - this hook (0x67E826)
	// runs BEFORE the techno objects (and their attachment slots) are
	// deserialized, so AttachmentClass::Array is still empty at this point.
	// It is called from LoadGame_UnsetFlag (0x67E68A) after the objects have
	// been loaded and the fixups have run. See Phobos.cpp.
	Savegame::ResetSwizzleDiag();
	PhobosTypeRegistry::LoadGlobals(pStm);

	return 0;
}

DEFINE_HOOK(0x67D04E, GameSave_SavegameInformation, 0x7)
{
	REF_STACK(SavegameInformation, Info, STACK_OFFSET(0x4A4, -0x3F4));

	Info.InternalVersion = Info.InternalVersion + SAVEGAME_ID;
	strncat(Info.ExecutableName.data(),
		" + Phobos " FILE_VERSION_STR,
		Info.ExecutableName.Size - sizeof(" + Phobos " FILE_VERSION_STR)
	);

	return 0;
}

DEFINE_HOOK_AGAIN(0x67FD9D, LoadOptionsClass_GetFileInfo, 0x7)
DEFINE_HOOK(0x67FDB1, LoadOptionsClass_GetFileInfo, 0x7)
{
	GET(SavegameInformation*, Info, ESI);
	Info->InternalVersion = Info->InternalVersion - SAVEGAME_ID;
	return 0;
}

#ifdef DEBUG

#pragma warning (disable : 4091)
#pragma warning (disable : 4245)

#include <Dbghelp.h>
#include <tlhelp32.h>

bool Phobos::DetachFromDebugger()
{
	auto GetDebuggerProcessId = [](DWORD dwSelfProcessId) -> DWORD
		{
			DWORD dwParentProcessId = -1;
			HANDLE hSnapshot = CreateToolhelp32Snapshot(2, 0);
			PROCESSENTRY32 pe32;
			pe32.dwSize = sizeof(PROCESSENTRY32);
			Process32First(hSnapshot, &pe32);
			do
			{
				if (pe32.th32ProcessID == dwSelfProcessId)
				{
					dwParentProcessId = pe32.th32ParentProcessID;
					break;
				}
			}
			while (Process32Next(hSnapshot, &pe32));
			CloseHandle(hSnapshot);
			return dwParentProcessId;
		};

	HMODULE hModule = LoadLibrary("ntdll.dll");
	if (hModule != NULL)
	{
		auto const NtRemoveProcessDebug =
			(NTSTATUS(__stdcall*)(HANDLE, HANDLE))GetProcAddress(hModule, "NtRemoveProcessDebug");
		auto const NtSetInformationDebugObject =
			(NTSTATUS(__stdcall*)(HANDLE, ULONG, PVOID, ULONG, PULONG))GetProcAddress(hModule, "NtSetInformationDebugObject");
		auto const NtQueryInformationProcess =
			(NTSTATUS(__stdcall*)(HANDLE, ULONG, PVOID, ULONG, PULONG))GetProcAddress(hModule, "NtQueryInformationProcess");
		auto const NtClose =
			(NTSTATUS(__stdcall*)(HANDLE))GetProcAddress(hModule, "NtClose");

		HANDLE hDebug;
		HANDLE hCurrentProcess = GetCurrentProcess();
		NTSTATUS status = NtQueryInformationProcess(hCurrentProcess, 30, &hDebug, sizeof(HANDLE), 0);
		if (0 <= status)
		{
			ULONG killProcessOnExit = FALSE;
			status = NtSetInformationDebugObject(
				hDebug,
				1,
				&killProcessOnExit,
				sizeof(ULONG),
				NULL
			);
			if (0 <= status)
			{
				const auto pid = GetDebuggerProcessId(GetProcessId(hCurrentProcess));
				status = NtRemoveProcessDebug(hCurrentProcess, hDebug);
				if (0 <= status)
				{
					HANDLE hDbgProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
					if (INVALID_HANDLE_VALUE != hDbgProcess)
					{
						BOOL ret = TerminateProcess(hDbgProcess, EXIT_SUCCESS);
						CloseHandle(hDbgProcess);
						return ret;
					}
				}
			}
			NtClose(hDebug);
		}
		FreeLibrary(hModule);
	}

	return false;
}
#endif
