#pragma once

#include <unordered_map>

#include <CCINIClass.h>
#include <SwizzleManagerClass.h>

#include <string_view>
#include <cstdio>
#include "Debug.h"
#include "Stream.h"
#include "Swizzle.h"
#include "Phobos.h"

enum class InitState
{
	Blank = 0x0,	  // CTOR'd
	Constanted = 0x1, // values that can be set without looking at Rules (i.e. country default loadscreen)
	Ruled = 0x2,	  // Rules has been loaded and props set (i.e. country powerplants taken from [General])
	Inited = 0x3,	  // values that need the object's state (i.e. is object a secretlab? -> load default boons)
	Completed = 0x4	  // INI has been read and values set
};

namespace PhobosContainerDetail
{
	// Best-effort zeroing of an object's extension pointer. The game may have
	// already freed the key object during scenario teardown; a plain write would
	// fault, so verify the target is committed and writable first.
	inline bool TryResetExtensionPointer(void* key, size_t offset)
	{
		if (!key)
			return false;

		MEMORY_BASIC_INFORMATION mbi;

		if (VirtualQuery(key, &mbi, sizeof(mbi)) == 0)
			return false;

		if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
			return false;

		constexpr DWORD writable =
			PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

		if ((mbi.Protect & writable) == 0)
			return false;

		const auto addr = reinterpret_cast<uintptr_t>(key) + offset;
		const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

		if (addr + sizeof(void*) > regionEnd)
			return false;

		*reinterpret_cast<uintptr_t*>(addr) = 0;
		return true;
	}
}

/*
 * ==========================
 *	It's a kind of magic
 * ==========================

 * These two templates are the basis of the new class extension standard.

 * ==========================

 * Extension<T> is the parent class for the data you want to link with this instance of T
	( for example, [Warhead]MindControl.Permanent= should be stored in WarheadClassExt::ExtData
	which itself should be a derivate of Extension<WarheadTypeClass> )

 * ==========================

	Container<TX> is the storage for all the Extension<T> which share the same T,
	where TX is the containing class of the relevant derivate of Extension<T>. // complex, huh?
	( for example, there is Container<WarheadTypeExt>
	which contains all the custom data for all WarheadTypeClass instances,
	and WarheadTypeExt itself contains just statics like the Container itself )

	Requires:
	using base_type = T;
	const DWORD Extension<T>::Canary = (any dword value easily identifiable in a byte stream)
	class TX::ExtData : public Extension<T> { custom_data; }

	Complex? Yes. That's partially why you should be happy these are premade for you.
 *
 */

template <typename T>
class Extension
{
	T* AttachedToObject;
	InitState Initialized;

public:

	Extension(T* const OwnerObject) : AttachedToObject { OwnerObject }, Initialized { InitState::Blank }
	{ }

	Extension(const Extension& other) = delete;

	void operator=(const Extension& RHS) = delete;

	virtual ~Extension() = default;

	// the object this Extension expands
	T* const& OwnerObject() const
	{
		return this->AttachedToObject;
	}

	void EnsureConstanted()
	{
		if (this->Initialized < InitState::Constanted)
		{
			this->InitializeConstants();
			this->Initialized = InitState::Constanted;
		}
	}

	void LoadFromINI(CCINIClass* pINI)
	{
		if (!pINI)
			return;

		switch (this->Initialized)
		{
		case InitState::Blank:
			this->EnsureConstanted();
		case InitState::Constanted:
			this->InitializeRuled();
			this->Initialized = InitState::Ruled;
		case InitState::Ruled:
			this->Initialize();
			this->Initialized = InitState::Inited;
		case InitState::Inited:
		case InitState::Completed:
			if (pINI == CCINIClass::INI_Rules)
				this->LoadFromRulesFile(pINI);

			this->LoadFromINIFile(pINI);
			this->Initialized = InitState::Completed;
		}
	}

	virtual void InvalidatePointer(void* ptr, bool bRemoved) = 0;

	virtual inline void SaveToStream(PhobosStreamWriter& Stm)
	{
		//Stm.Save(this->AttachedToObject);
		Stm.Save(this->Initialized);
	}

	virtual inline void LoadFromStream(PhobosStreamReader& Stm)
	{
		//Stm.Load(this->AttachedToObject);
		Stm.Load(this->Initialized);
	}

protected:
	// right after construction. only basic initialization tasks possible;
	// owner object is only partially constructed! do not use global state!
	virtual void InitializeConstants() { }

	virtual void InitializeRuled() { }

	// called before the first ini file is read
	virtual void Initialize() { }

	// for things that only logically work in rules - countries, sides, etc
	virtual void LoadFromRulesFile(CCINIClass* pINI) { }

	// load any ini file: rules, game mode, scenario or map
	virtual void LoadFromINIFile(CCINIClass* pINI) { }
};

// a non-virtual base class for a pointer to pointer map.
// pointers are not owned by this map, so be cautious.
class ContainerMapBase final
{
public:
	using key_type = void*;
	using const_key_type = const void*;
	using value_type = void*;
	using map_type = std::unordered_map<const_key_type, value_type>;
	using const_iterator = map_type::const_iterator;
	using iterator = const_iterator;

	ContainerMapBase() = default;
	ContainerMapBase(ContainerMapBase const&) = delete;
	~ContainerMapBase() = default;

	ContainerMapBase& operator=(ContainerMapBase const&) = delete;
	ContainerMapBase& operator=(ContainerMapBase&&) = delete;

	value_type find(const_key_type key) const
	{
		auto const it = this->Items.find(key);
		if (it != this->Items.end())
			return it->second;

		return nullptr;
	}

	void insert(const_key_type key, value_type value)
	{
		// insert_or_assign, not emplace: Allocate() resets and overwrites the
		// object's cached pointer, so the map must be overwritten too. With
		// emplace a second Allocate() for the same key (the CTOR hooks are hit
		// twice for some types) left the map holding the OLD extension while the
		// object pointed at the new one, so they diverged and Clear() then
		// operated on the wrong object.
		this->Items.insert_or_assign(key, value);
	}

	value_type remove(const_key_type key)
	{
		auto const it = this->Items.find(key);
		if (it != this->Items.cend())
		{
			auto const value = it->second;
			this->Items.erase(it);

			return value;
		}

		return nullptr;
	}

	void clear()
	{
		// this leaks all objects inside. this case is logged.
		this->Items.clear();
	}

	size_t size() const
	{
		return this->Items.size();
	}

	const_iterator begin() const
	{
		return this->Items.cbegin();
	}

	const_iterator end() const
	{
		return this->Items.cend();
	}

private:
	map_type Items;
};

// looks like a typed map, but is really a thin wrapper around the untyped map
// pointers are not owned here either, see that each pointer is deleted
template <typename Key, typename Value>
class ContainerMap final
{
public:
	using key_type = Key*;
	using const_key_type = const Key*;
	using value_type = Value*;
	using iterator = typename std::unordered_map<key_type, value_type>::const_iterator;

	ContainerMap() = default;
	ContainerMap(ContainerMap const&) = delete;

	ContainerMap& operator=(ContainerMap const&) = delete;
	ContainerMap& operator=(ContainerMap&&) = delete;

	value_type find(const_key_type key) const
	{
		return static_cast<value_type>(this->Items.find(key));
	}

	value_type insert(const_key_type key, value_type value)
	{
		this->Items.insert(key, value);
		return value;
	}

	value_type remove(const_key_type key)
	{
		return static_cast<value_type>(this->Items.remove(key));
	}

	void clear()
	{
		this->Items.clear();
	}

	size_t size() const
	{
		return this->Items.size();
	}

	iterator begin() const
	{
		auto ret = this->Items.begin();
		return reinterpret_cast<iterator&>(ret);
	}

	iterator end() const
	{
		auto ret = this->Items.end();
		return reinterpret_cast<iterator&>(ret);
	}

private:
	ContainerMapBase Items;
};

template <class T>
concept HasOffset = requires(T) { T::ExtPointerOffset; };

template <typename T>
class Container
{
private:
	using base_type = typename T::base_type;
	using extension_type = typename T::ExtData;
	using base_type_ptr = base_type*;
	using const_base_type_ptr = const base_type*;
	using extension_type_ptr = extension_type*;
	using map_type = ContainerMap<base_type, extension_type>;

	map_type Items;

	base_type* SavingObject;
	extension_type_ptr SavingExtPointer;
	IStream* SavingStream;
	const char* Name;

public:
	explicit Container(const char* pName) :
		Items(),
		SavingObject(nullptr),
		SavingStream(nullptr),
		Name(pName)
	{ }

	virtual ~Container() = default;

	void PointerGotInvalid(void* ptr, bool bRemoved)
	{
		//this->InvalidatePointer(ptr, bRemoved);

		if (!this->InvalidateExtDataIgnorable(ptr))
			this->InvalidateExtDataPointer(ptr, bRemoved);
	}

protected:
	//virtual void InvalidatePointer(void* ptr, bool bRemoved) { }

	virtual bool InvalidateExtDataIgnorable(void* const ptr) const
	{
		return true;
	}

	void InvalidateExtDataPointer(void* const ptr, bool bRemoved) const
	{
		for (const auto& i : this->Items)
			i.second->InvalidatePointer(ptr, bRemoved);
	}

private:
	extension_type_ptr GetExtensionPointer(const_base_type_ptr key) const
	{
		return (extension_type_ptr)(*(uintptr_t*)((char*)key + T::ExtPointerOffset));
	}

	void SetExtensionPointer(base_type_ptr key, extension_type_ptr value)
	{
		(*(uintptr_t*)((char*)key + T::ExtPointerOffset)) = (uintptr_t)value;
	}

	void ResetExtensionPointer(base_type_ptr key)
	{
		(*(uintptr_t*)((char*)key + T::ExtPointerOffset)) = 0;
	}

public:
	extension_type_ptr Allocate(base_type_ptr key)
	{
		if constexpr (HasOffset<T>)
			ResetExtensionPointer(key);

		auto const val = new extension_type(key);

		val->EnsureConstanted();

		if constexpr (HasOffset<T>)
			SetExtensionPointer(key, val);

		this->Items.insert(key, val);

		return val;
	}

	extension_type_ptr TryAllocate(base_type_ptr key, bool bCond, const std::string_view& nMessage)
	{
		if (!key || (!bCond && !nMessage.empty()))
		{
			Debug::Log("%s \n", nMessage.data());
			return nullptr;
		}

		return Allocate(key);
	}

	extension_type_ptr TryAllocate(base_type_ptr key)
	{
		if (!key)
		{
			Debug::Log("Attempted to allocate %s from nullptr!\n", typeid(extension_type).name());
			return nullptr;
		}

		return Allocate(key);
	}

	extension_type_ptr TryFind(const_base_type_ptr key) const
	{
		if (!key)
			return nullptr;

		if constexpr (HasOffset<T>)
			return GetExtensionPointer(key);
		else
			return this->Items.find(key);
	}

	extension_type_ptr Find(const_base_type_ptr key) const
	{
		if constexpr (HasOffset<T>)
			return GetExtensionPointer(key);
		else
			return this->Items.find(key);
	}

	// Only used on loading, does not check if key is nullptr.
	extension_type_ptr FindOrAllocate(base_type_ptr key)
	{
		extension_type_ptr value = nullptr;

		if constexpr (HasOffset<T>)
			value = GetExtensionPointer(key);
		else
			value = this->Items.find(key);

		if (!value)
			value = Allocate(key);

		return value;
	}

	void Remove(base_type_ptr key)
	{
		if (auto Item = Find(key))
		{
			this->Items.remove(key);
			delete Item;

			if constexpr (HasOffset<T>)
				ResetExtensionPointer(key);
		}
	}

	// Diagnostic (read-only): verify every object's cached extension pointer
	// still matches the map entry. A mismatch means the object was freed and
	// its memory reused without the DTOR hook firing - the root cause of the
	// "crash on the 2nd scenario load" heap corruption.
	void ValidateCachedPointers() const
	{
		if constexpr (HasOffset<T>)
		{
			if (!this->Items.size())
				return;

			size_t stale = 0;

			for (const auto& i : this->Items)
			{
				const auto key = i.first;
				const auto val = i.second;
				MEMORY_BASIC_INFORMATION mbi {};

				if (VirtualQuery(key, &mbi, sizeof(mbi)) == 0
					|| mbi.State != MEM_COMMIT
					|| (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				{
					++stale;
					Debug::Log("[GC-diag] %s ext-cache points at uncommitted obj=%p map=%p\n",
						this->Name, (void*)key, (void*)val);
					continue;
				}

				const auto cached = GetExtensionPointer(key);

				if (cached != val)
				{
					++stale;
					Debug::Log("[GC-diag] %s ext-cache mismatch: obj=%p cached=%p map=%p\n",
						this->Name, (void*)key, (void*)cached, (void*)val);
				}
			}

			if (stale)
				Debug::Log("[GC-diag] %s: %zu/%zu cached extension pointers stale.\n",
					this->Name, stale, this->Items.size());
		}
	}

	void Clear()
	{
		if (this->Items.size())
		{
			Debug::Log("Cleared %u items from %s.\n", this->Items.size(), this->Name);

			// Deliberately does nothing to the values or the key objects.
			// The game frees the type objects before this runs, so deleting the
			// values would leave the objects' cached extension pointer dangling,
			// and resetting those pointers can hit memory already reused by an
			// unrelated object. Both corrupt the heap on later scenario loads.
			// Extension data is reclaimed when its object is destroyed, and the
			// cached pointer is overwritten on (re)creation by Allocate().
			this->Items.clear();
		}
	}

	void LoadFromINI(const_base_type_ptr key, CCINIClass* pINI)
	{
		if (auto ptr = this->Find(key))
			ptr->LoadFromINI(pINI);
	}

	void PrepareStream(base_type_ptr key, IStream* pStm)
	{
		//Debug::Log("[PrepareStream] Next is %p of type '%s'\n", key, this->Name);

		this->SavingObject = key;
		this->SavingStream = pStm;

		// Loading the base type data might override the ext pointer stored on it so it needs to be saved.
		if constexpr (HasOffset<T>)
			this->SavingExtPointer = GetExtensionPointer(key);
	}

	void SaveStatic()
	{
		if (this->SavingObject && this->SavingStream)
		{
			// The game's own Save may have overwritten the stored ext pointer (e.g.
			// CellClass offset 0x144 collides with a game field). Restore it before
			// serializing so SaveKey reads the real extension. Mirrors LoadStatic.
			if constexpr (HasOffset<T>)
				SetExtensionPointer(this->SavingObject, this->SavingExtPointer);

			//Debug::Log("[SaveStatic] Saving object %p as '%s'\n", this->SavingObject, this->Name);
			if (!this->Save(this->SavingObject, this->SavingStream))
				Debug::FatalErrorAndExit("SaveStatic - Saving object %p as '%s' failed!\n", this->SavingObject, this->Name);
		}
		else
		{
			Debug::Log("SaveStatic - Object or Stream not set for '%s': %p, %p\n",
				this->Name, this->SavingObject, this->SavingStream);
		}

		this->SavingObject = nullptr;
		this->SavingStream = nullptr;
	}

	void LoadStatic()
	{
		if (this->SavingObject && this->SavingStream)
		{
			// Restore stored ext pointer data.
			if constexpr (HasOffset<T>)
				SetExtensionPointer(this->SavingObject, this->SavingExtPointer);

			//Debug::Log("[LoadStatic] Loading object %p as '%s'\n", this->SavingObject, this->Name);
			if (!this->Load(this->SavingObject, this->SavingStream))
				Debug::FatalErrorAndExit("LoadStatic - Loading object %p as '%s' failed!\n", this->SavingObject, this->Name);
		}
		else
		{
			Debug::Log("LoadStatic - Object or Stream not set for '%s': %p, %p\n",
				this->Name, this->SavingObject, this->SavingStream);
		}

		this->SavingObject = nullptr;
		this->SavingStream = nullptr;
	}

	decltype(auto) begin() const = delete;

	decltype(auto) end() const = delete;

	size_t size() const
	{
		return this->Items.size();
	}

protected:
	// override this method to do type-specific stuff
	virtual bool Save(base_type_ptr key, IStream* pStm)
	{
		return this->SaveKey(key, pStm) != nullptr;
	}

	// override this method to do type-specific stuff
	virtual bool Load(base_type_ptr key, IStream* pStm)
	{
		return this->LoadKey(key, pStm) != nullptr;
	}

	extension_type_ptr SaveKey(base_type_ptr key, IStream* pStm)
	{
		// this really shouldn't happen
		if (!key)
		{
			Debug::Log("SaveKey - Attempted for a null pointer! WTF!\n");
			return nullptr;
		}

		// get the value data
		auto buffer = this->Find(key);
		if (!buffer)
		{
			Debug::Log("SaveKey - Could not find value.\n");
			return nullptr;
		}

		// write the current pointer, the size of the block, and the canary
		PhobosByteStream saver(sizeof(*buffer));
		PhobosStreamWriter writer(saver);

		writer.Save(T::Canary);
		writer.Save(buffer);

		// save the data
		if constexpr (requires(const base_type& obj) { obj.ID; })
			_snprintf_s(Savegame::CurrentSaveContext, sizeof(Savegame::CurrentSaveContext),
				"%s [%s] %p", this->Name, key->ID, static_cast<const void*>(key));
		else
			_snprintf_s(Savegame::CurrentSaveContext, sizeof(Savegame::CurrentSaveContext),
				"%s %p", this->Name, static_cast<const void*>(key));
		buffer->SaveToStream(writer);

		// save the block
		if (!saver.WriteBlockToStream(pStm))
		{
			Debug::Log("SaveKey - Failed to save data.\n");
			return nullptr;
		}

		//Debug::Log("[SaveKey] Save used up 0x%X bytes\n", saver.Size());

		return buffer;
	}

	extension_type_ptr LoadKey(base_type_ptr key, IStream* pStm)
	{
		// this really shouldn't happen
		if (!key)
		{
			Debug::Log("LoadKey - Attempted for a null pointer! WTF!\n");
			return nullptr;
		}

		// get or allocate the value data
		extension_type_ptr buffer = this->FindOrAllocate(key);
		if (!buffer)
		{
			Debug::Log("LoadKey - Could not find or allocate value.\n");
			return nullptr;
		}

		PhobosByteStream loader(0);
		if (!loader.ReadBlockFromStream(pStm))
		{
			Debug::Log("LoadKey - Failed to read data from save stream?!\n");
			return nullptr;
		}

		PhobosStreamReader reader(loader);
		if (reader.Expect(T::Canary) && reader.RegisterChange(buffer))
		{
			buffer->LoadFromStream(reader);
			if (reader.ExpectEndOfBlock())
				return buffer;
		}

		return nullptr;
	}

private:
	Container(const Container&) = delete;
	Container& operator = (const Container&) = delete;
	Container& operator = (Container&&) = delete;
};
