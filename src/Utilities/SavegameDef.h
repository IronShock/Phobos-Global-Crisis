#pragma once

// include this file whenever something is to be saved.

#include "Savegame.h"
#include <optional>
#include <vector>
#include <map>
#include <bitset>
#include <memory>

#include <ArrayClasses.h>
#include <FileSystem.h>
#include <FileFormats/SHP.h>
#include <RulesClass.h>
#include <SidebarClass.h>
#include <Utilities/Constructs.h>

#include "Swizzle.h"
#include "Debug.h"

namespace Savegame
{
	template <typename T>
	concept ImplementsUpperCaseSaveLoad = requires (PhobosStreamWriter & stmWriter, PhobosStreamReader & stmReader, T & value, bool registerForChange)
	{
		value.Save(stmWriter);
		value.Load(stmReader, registerForChange);
	};

	template <typename T>
	concept ImplementsLowerCaseSaveLoad = requires (PhobosStreamWriter & stmWriter, PhobosStreamReader & stmReader, T & value, bool registerForChange)
	{
		value.save(stmWriter);
		value.load(stmReader, registerForChange);
	};

	#pragma warning(push)
	#pragma warning(disable: 4702) // MSVC isn't smart enough and yells about unreachable code

	template <typename T>
	bool ReadPhobosStream(PhobosStreamReader& stm, T& value, bool registerForChange)
	{
		if constexpr (ImplementsUpperCaseSaveLoad<T>)
			return value.Load(stm, registerForChange);

		else if constexpr (ImplementsLowerCaseSaveLoad<T>)
			return value.load(stm, registerForChange);

		PhobosStreamObject<T> item;
		return item.ReadFromStream(stm, value, registerForChange);
	}

	template <typename T>
	bool WritePhobosStream(PhobosStreamWriter& stm, const T& value)
	{
		if constexpr (ImplementsUpperCaseSaveLoad<T>)
			return value.Save(stm);

		if constexpr (ImplementsLowerCaseSaveLoad<T>)
			return value.save(stm);

		PhobosStreamObject<T> item;
		return item.WriteToStream(stm, value);
	}

	#pragma warning(pop)

	template <typename T>
	T* RestoreObject(PhobosStreamReader& Stm, bool RegisterForChange)
	{
		T* ptrOld = nullptr;
		if (!Stm.Load(ptrOld))
			return nullptr;

		if (ptrOld)
		{
			std::unique_ptr<T> ptrNew = ObjectFactory<T>()(Stm);

			if (Savegame::ReadPhobosStream(Stm, *ptrNew, RegisterForChange))
			{
				PhobosSwizzle::RegisterChange(ptrOld, ptrNew.get());
				return ptrNew.release();
			}
		}

		return nullptr;
	}

	template <typename T>
	bool PersistObject(PhobosStreamWriter& Stm, const T* pValue)
	{
		if (!Savegame::WritePhobosStream(Stm, pValue))
			return false;

		if (pValue)
			return Savegame::WritePhobosStream(Stm, *pValue);

		return true;
	}

	template <typename T>
	bool PhobosStreamObject<T>::ReadFromStream(PhobosStreamReader& Stm, T& Value, bool RegisterForChange) const
	{
		bool ret = Stm.Load(Value);

		if (RegisterForChange)
			Swizzle swizzle(Value);

		return ret;
	}

	template <typename T>
	bool PhobosStreamObject<T>::WriteToStream(PhobosStreamWriter& Stm, const T& Value) const
	{
		Stm.Save(Value);
		return true;
	}


	// specializations

	template <typename T>
	struct Savegame::PhobosStreamObject<VectorClass<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, VectorClass<T>& Value, bool RegisterForChange) const
		{
			Value.Clear();
			int Capacity = 0;

			if (!Stm.Load(Capacity))
				return false;

			Value.Reserve(Capacity);

			for (auto ix = 0; ix < Capacity; ++ix)
			{
				if (!Savegame::ReadPhobosStream(Stm, Value.Items[ix], RegisterForChange))
					return false;
			}

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const VectorClass<T>& Value) const
		{
			int capacity = Value.Capacity;

			if (capacity < 0 || capacity > 0x1000000)
			{
				Debug::Log("Savegame - Corrupt VectorClass while saving (capacity=%d)%s%s. Writing empty to avoid freeze.\n",
					capacity,
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");
				capacity = 0;
			}

			Stm.Save(capacity);

			for (auto ix = 0; ix < capacity; ++ix)
			{
				if (!Savegame::WritePhobosStream(Stm, Value.Items[ix]))
					return false;
			}

			return true;
		}
	};

	template <typename T>
	struct Savegame::PhobosStreamObject<DynamicVectorClass<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, DynamicVectorClass<T>& Value, bool RegisterForChange) const
		{
			Value.Clear();
			int Capacity = 0;

			if (!Stm.Load(Capacity))
				return false;

			Value.Reserve(Capacity);

			if (!Stm.Load(Value.Count) || !Stm.Load(Value.CapacityIncrement))
				return false;

			for (auto ix = 0; ix < Value.Count; ++ix)
			{
				if (!Savegame::ReadPhobosStream(Stm, Value.Items[ix], RegisterForChange))
					return false;
			}

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const DynamicVectorClass<T>& Value) const
		{
			int capacity = Value.Capacity;
			int count = Value.Count;

			if (count < 0 || capacity < 0 || count > 0x1000000 || count > capacity)
			{
				Debug::Log("Savegame - Corrupt DynamicVectorClass while saving (count=%d cap=%d)%s%s. Writing empty to avoid freeze.\n",
					count, capacity,
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");
				count = 0;
				capacity = 0;
			}

			Stm.Save(capacity);
			Stm.Save(count);
			Stm.Save(Value.CapacityIncrement);

			for (auto ix = 0; ix < count; ++ix)
			{
				if (!Savegame::WritePhobosStream(Stm, Value.Items[ix]))
					return false;
			}

			return true;
		}
	};

	template <typename T>
	struct Savegame::PhobosStreamObject<TypeList<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, TypeList<T>& Value, bool RegisterForChange) const
		{
			if (!Savegame::ReadPhobosStream<DynamicVectorClass<T>>(Stm, Value, RegisterForChange))
				return false;

			return Stm.Load(Value.unknown_18);
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const TypeList<T>& Value) const
		{
			if (!Savegame::WritePhobosStream<DynamicVectorClass<T>>(Stm, Value))
				return false;

			Stm.Save(Value.unknown_18);
			return true;
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<CounterClass>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, CounterClass& Value, bool RegisterForChange) const
		{
			if (!Savegame::ReadPhobosStream<VectorClass<int>>(Stm, Value, RegisterForChange))
				return false;

			return Stm.Load(Value.Total);
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const CounterClass& Value) const
		{
			if (!Savegame::WritePhobosStream<VectorClass<int>>(Stm, Value))
				return false;

			Stm.Save(Value.Total);
			return true;
		}
	};

	template <size_t Size>
	struct Savegame::PhobosStreamObject<std::bitset<Size>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::bitset<Size>& Value, bool RegisterForChange) const
		{
			unsigned char value = 0;
			for (auto i = 0u; i < Size; ++i)
			{
				auto pos = i % 8;

				if (pos == 0 && !Stm.Load(value))
					return false;

				Value.set(i, ((value >> pos) & 1) != 0);
			}

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::bitset<Size>& Value) const
		{
			unsigned char value = 0;
			for (auto i = 0u; i < Size; ++i)
			{
				auto pos = i % 8;

				if (Value[i])
					value |= 1 << pos;

				if (pos == 7 || i == Size - 1)
				{
					Stm.Save(value);
					value = 0;
				}
			}

			return true;
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<std::string>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::string& Value, bool RegisterForChange) const
		{
			size_t size = 0;

			if (Stm.Load(size))
			{
				std::vector<char> buffer(size);

				if (!size || Stm.Read(reinterpret_cast<byte*>(buffer.data()), size))
				{
					Value.assign(buffer.begin(), buffer.end());
					return true;
				}
			}
			return false;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::string& Value) const
		{
			size_t size = Value.size();
			Stm.Save(size);

			if (size > 0 && !Savegame::IsBufferReadable(Value.c_str(), size))
			{
				Debug::Log("Savegame - Corrupt std::string during save (size=%u data=%p)%s%s. Writing zeroes to avoid crash.\n",
					static_cast<unsigned int>(size), Value.c_str(),
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");
				WriteZeroes(Stm, size);
			}
			else
			{
				Stm.Write(reinterpret_cast<const byte*>(Value.c_str()), size);
			}

			return true;
		}

	private:
		static void WriteZeroes(PhobosStreamWriter& Stm, size_t size)
		{
			static const std::vector<byte> zeros(0x10000, 0);
			size_t left = size;
			while (left > 0)
			{
				const size_t chunk = left < zeros.size() ? left : zeros.size();
				Stm.Write(zeros.data(), chunk);
				left -= chunk;
			}
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<std::wstring>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::wstring& Value, bool RegisterForChange) const
		{
			size_t size = 0;

			if (Stm.Load(size))
			{
				std::vector<wchar_t> buffer(size);

				if (!size || Stm.Read(reinterpret_cast<byte*>(buffer.data()), size * sizeof(wchar_t)))
				{
					Value.assign(buffer.begin(), buffer.end());
					return true;
				}
			}

			return false;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::wstring& Value) const
		{
			size_t size = Value.size();
			Stm.Save(size);

			if (size > 0 && !Savegame::IsBufferReadable(Value.c_str(), size * sizeof(wchar_t)))
			{
				Debug::Log("Savegame - Corrupt std::wstring during save (size=%u data=%p)%s%s. Writing zeroes to avoid crash.\n",
					static_cast<unsigned int>(size), Value.c_str(),
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");

				static const std::vector<wchar_t> zeros(0x10000, 0);
				size_t left = size;
				while (left > 0)
				{
					const size_t chunk = left < zeros.size() ? left : zeros.size();
					Stm.Write(reinterpret_cast<const byte*>(zeros.data()), chunk * sizeof(wchar_t));
					left -= chunk;
				}
			}
			else
			{
				Stm.Write(reinterpret_cast<const byte*>(Value.c_str()), size * sizeof(wchar_t));
			}

			return true;
		}
	};

	template <typename T>
	struct Savegame::PhobosStreamObject<std::unique_ptr<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::unique_ptr<T>& Value, bool RegisterForChange) const
		{
			Value.reset(RestoreObject<T>(Stm, RegisterForChange));
			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::unique_ptr<T>& Value) const
		{
			return PersistObject(Stm, Value.get());
		}
	};

	template <typename T>
	struct Savegame::PhobosStreamObject<std::optional<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::optional<T>& Value, bool RegisterForChange) const
		{
			bool hasValue = false;
			if (!Stm.Load(hasValue))
				return false;

			if (hasValue)
				return Savegame::ReadPhobosStream(Stm, *Value, RegisterForChange);
			else
				Value.reset();

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::optional<T>& Value) const
		{
			Stm.Save(Value.has_value());

			if (Value.has_value())
				return Savegame::WritePhobosStream(Stm, *Value);

			return true;
		}
	};

	template <typename T>
	struct Savegame::PhobosStreamObject<std::vector<T>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::vector<T>& Value, bool RegisterForChange) const
		{
			Value.clear();

			size_t Capacity = 0;

			if (!Stm.Load(Capacity))
				return false;

			Value.reserve(Capacity);

			size_t Count = 0;

			if (!Stm.Load(Count))
				return false;

			Value.resize(Count);

			for (auto ix = 0u; ix < Count; ++ix)
			{
				if (!Savegame::ReadPhobosStream(Stm, Value[ix], RegisterForChange))
					return false;
			}

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::vector<T>& Value) const
		{
			size_t capacity = Value.capacity();
			size_t count = Value.size();

			if (count > capacity || count > 0x1000000)
			{
				Debug::Log("Savegame - Corrupt std::vector while saving (size=%u cap=%u)%s%s. Writing empty to avoid freeze.\n",
					static_cast<unsigned>(count), static_cast<unsigned>(capacity),
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");
				capacity = 0;
				count = 0;
			}

			Stm.Save(capacity);
			Stm.Save(count);

			for (auto ix = 0u; ix < count; ++ix)
			{
				if (!Savegame::WritePhobosStream(Stm, Value[ix]))
					return false;
			}

			return true;
		}
	};

	template <typename TKey, typename TValue>
	struct Savegame::PhobosStreamObject<std::map<TKey, TValue>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::map<TKey, TValue>& Value, bool RegisterForChange) const
		{
			Value.clear();
			static_assert(!std::is_pointer_v<TKey> && !std::is_pointer_v<TValue>);
			static_assert(std::is_trivially_constructible_v<TKey> && std::is_trivially_constructible_v<TValue>);
			static_assert(std::is_trivially_destructible_v<TKey> && std::is_trivially_destructible_v<TValue>);
			size_t Count = 0;
			if (!Stm.Load(Count))
			{
				return false;
			}

			for (auto ix = 0u; ix < Count; ++ix)
			{
				std::pair<TKey, TValue> buffer;
				if (!Savegame::ReadPhobosStream(Stm, buffer, RegisterForChange))
				{
					return false;
				}
				Value.insert(buffer);
			}

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const std::map<TKey, TValue>& Value) const
		{
			Stm.Save(Value.size());

			for (const auto& item : Value)
			{
				if (!Savegame::WritePhobosStream(Stm, item))
				{
					return false;
				}
			}
			return true;
		}
	};

	template <typename TKey, typename TValue> // Why are you doing this, choom
	struct Savegame::PhobosStreamObject<std::map<TKey, std::vector<TValue>>>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, std::map<TKey, std::vector<TValue>>& Value, bool RegisterForChange) const
		{
			Value.clear();

			size_t Count = 0;
			if (!Stm.Load(Count))
			{
				return false;
			}

			for (auto ix = 0u; ix < Count; ++ix)
			{
				TKey key;
				std::vector<TValue> vals;
				if (!(Savegame::ReadPhobosStream(Stm, key, RegisterForChange)&& Savegame::ReadPhobosStream(Stm, vals, RegisterForChange)))
				{
					return false;
				}
				Value[key] = std::move(vals);
			}

			return true;
		}
		bool WriteToStream(PhobosStreamWriter& Stm, const std::map<TKey, std::vector<TValue>>& Value) const
		{
			Stm.Save(Value.size());

			for (const auto& [key,vals] : Value)
			{
				if (!(Savegame::WritePhobosStream(Stm, key) && Savegame::WritePhobosStream(Stm,vals)))
				{
					return false;
				}
			}
			return true;
		};
	};

	template <>
	struct Savegame::PhobosStreamObject<SHPStruct*>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, SHPStruct*& Value, bool RegisterForChange) const
		{
			if (Value && !Value->IsReference())
				Debug::Log("Value contains SHP file data. Possible leak.\n");

			Value = nullptr;

			bool hasValue = true;
			if (Savegame::ReadPhobosStream(Stm, hasValue) && hasValue)
			{
				std::string name;
				if (Savegame::ReadPhobosStream(Stm, name))
				{
					if (auto pSHP = FileSystem::LoadSHPFile(name.c_str()))
					{
						Value = pSHP;
						return true;
					}
				}
			}

			return !hasValue;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, SHPStruct* const& Value) const
		{
			const char* filename = nullptr;
			if (Value)
			{
				if (auto pRef = Value->AsReference())
					filename = pRef->Filename;
				else
					Debug::Log("Cannot save SHPStruct, because it isn't a reference.\n");
			}

			if (Savegame::WritePhobosStream(Stm, filename != nullptr))
			{
				if (filename)
				{
					std::string file(filename);
					return Savegame::WritePhobosStream(Stm, file);
				}
			}

			return filename == nullptr;
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<RocketStruct>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, RocketStruct& Value, bool RegisterForChange) const
		{
			if (!Stm.Load(Value))
				return false;

			if (RegisterForChange)
				Swizzle swizzle(Value.Type);

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const RocketStruct& Value) const
		{
			Stm.Save(Value);
			return true;
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<BuildType>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, BuildType& Value, bool RegisterForChange) const
		{
			if (!Stm.Load(Value))
				return false;

			if (RegisterForChange)
				Swizzle swizzle(Value.CurrentFactory);

			return true;
		}

		bool WriteToStream(PhobosStreamWriter& Stm, const BuildType& Value) const
		{
			Stm.Save(Value);
			return true;
		}
	};

	template <>
	struct Savegame::PhobosStreamObject<TranslucencyLevel*>
	{
		bool ReadFromStream(PhobosStreamReader& Stm, TranslucencyLevel*& Value, bool RegisterForChange) const
		{
			return Value->Load(Stm, RegisterForChange);
		}

		bool WriteToStream(PhobosStreamWriter& Stm, TranslucencyLevel* const& Value) const
		{
			return Value->Save(Stm);
		}
	};
}
