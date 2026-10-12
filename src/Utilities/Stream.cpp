#include "Stream.h"
#include "Debug.h"

#include <SwizzleManagerClass.h>

#include <Objidl.h>
#include <unordered_map>
#include <intrin.h>

namespace
{
	// [GC-diag] Tracks old->new pointer registrations within one load so the same
	// old pointer being registered to two different new objects (which Ares logs as
	// "declared change to both") can be reported together with a stack trace.
	std::unordered_map<long, void*> s_swizzleReg;
	bool s_swizzleDiagActive = false;

	void DumpSwizzleStack()
	{
		void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());

		for (int i = 0; i < 24; ++i)
		{
			void* v = sp[i];

			if (v)
				Debug::Log("[SWZ-diag]   +%02X %p\n", i * 4, v);
		}
	}
}

namespace Savegame
{
	// Called before Phobos global load so each load starts with a clean tracker.
	void ResetSwizzleDiag()
	{
		s_swizzleReg.clear();
		s_swizzleDiagActive = true;
	}
}

namespace Savegame
{
	char CurrentSaveContext[0x100] = { 0 };

	bool IsBufferReadable(const void* p, size_t Size)
	{
		if (Size == 0)
			return true;

		if (!p)
			return false;

		const auto* base = reinterpret_cast<const unsigned char*>(p);
		size_t remaining = Size;

		while (remaining > 0)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0)
				return false;

			const DWORD protect = mbi.Protect & 0xFF;

			if (mbi.State != MEM_COMMIT
				|| (mbi.Protect & PAGE_GUARD)
				|| protect == PAGE_NOACCESS
				|| protect == PAGE_EXECUTE)
			{
				return false;
			}

			const size_t regionSize = mbi.RegionSize
				- (reinterpret_cast<size_t>(base) - reinterpret_cast<size_t>(mbi.BaseAddress));

			if (regionSize >= remaining)
				break;

			base += regionSize;
			remaining -= regionSize;
		}

		return true;
	}
}

PhobosByteStream::PhobosByteStream(size_t Reserve) : Data(), CurrentOffset(0)
{
	this->Data.reserve(Reserve);
}

PhobosByteStream::~PhobosByteStream() = default;

bool PhobosByteStream::ReadFromStream(IStream* pStm, const size_t Length)
{
	auto size = this->Data.size();
	this->Data.resize(size + Length);
	auto pv = reinterpret_cast<void*>(this->Data.data());

	ULONG out = 0;
	auto success = pStm->Read(pv, Length, &out);
	bool result(SUCCEEDED(success) && out == Length);

	if (!result)
		this->Data.resize(size);

	return result;
}

bool PhobosByteStream::WriteToStream(IStream* pStm) const
{
	const size_t Length(this->Data.size());
	auto pcv = reinterpret_cast<const void*>(this->Data.data());

	ULONG out = 0;
	auto success = pStm->Write(pcv, Length, &out);

	return SUCCEEDED(success) && out == Length;
}

bool PhobosByteStream::Read(data_t* Value, size_t Size)
{
	bool ret = false;

	if (this->Data.size() >= this->CurrentOffset + Size)
	{
		auto Position = &this->Data[this->CurrentOffset];
		std::memcpy(Value, Position, Size);
		ret = true;
	}

	this->CurrentOffset += Size;
	return ret;
}

void PhobosByteStream::Write(const data_t* Value, size_t Size)
{
	if (Size > 0)
	{
		if (!Savegame::IsBufferReadable(Value, Size))
		{
			static bool logged = false;
			if (!logged)
			{
				logged = true;
				Debug::Log("PhobosByteStream::Write - Unreadable source pointer %p, size %u at stream position %u%s%s. Writing zeroes to avoid crash.\n",
					Value, static_cast<unsigned int>(Size), static_cast<unsigned int>(this->Data.size()),
					Savegame::CurrentSaveContext[0] ? " while saving " : "",
					Savegame::CurrentSaveContext[0] ? Savegame::CurrentSaveContext : "");
			}

			// keep the stream aligned so the block stays loadable; clamp absurd sizes
			// so a corrupted huge Size cannot freeze the game in the zero-fill loop
			constexpr size_t MaxZeroFill = 0x10000;
			static const std::vector<data_t> zeros(0x10000, 0);
			size_t left = Size > MaxZeroFill ? MaxZeroFill : Size;
			while (left > 0)
			{
				const size_t chunk = left < zeros.size() ? left : zeros.size();
				this->Data.insert(this->Data.end(), zeros.begin(), zeros.begin() + chunk);
				left -= chunk;
			}
			return;
		}

		this->Data.insert(this->Data.end(), Value, Value + Size);
	}
}

size_t PhobosByteStream::ReadBlockFromStream(IStream* pStm)
{
	ULONG out = 0;
	size_t Length = 0;

	if (SUCCEEDED(pStm->Read(&Length, sizeof(Length), &out)))
	{
		if (this->ReadFromStream(pStm, Length))
			return Length;
	}

	return 0;
}

bool PhobosByteStream::WriteBlockToStream(IStream* pStm) const
{
	ULONG out = 0;
	const size_t Length = this->Data.size();

	if (SUCCEEDED(pStm->Write(&Length, sizeof(Length), &out)))
		return this->WriteToStream(pStm);

	return false;
}

bool PhobosStreamReader::RegisterChange(void* newPtr)
{
	static_assert(sizeof(long) == sizeof(void*), "long and void* need to be of same size.");

	long oldPtr = 0;
	if (this->Load(oldPtr))
	{
		// [GC-diag] Report the same old pointer being mapped to two different new
		// objects. This is the "declared change to both" corruption at its source.
		if (s_swizzleDiagActive && oldPtr)
		{
			auto const it = s_swizzleReg.find(oldPtr);
			if (it == s_swizzleReg.end())
			{
				s_swizzleReg[oldPtr] = newPtr;
			}
			else if (it->second != newPtr)
			{
				Debug::Log("[SWZ-diag] old=%p -> %p AND %p (ctx='%s', streamOffset=%X/%X)\n",
					(void*)oldPtr, it->second, newPtr, Savegame::CurrentSaveContext,
					this->stream->Offset(), this->stream->Size());

				DumpSwizzleStack();
			}
		}

		if (SUCCEEDED(SwizzleManagerClass::Instance.Here_I_Am(oldPtr, newPtr)))
			return true;

		this->EmitSwizzleWarning(oldPtr, newPtr, stream_debugging_t());
	}

	return false;
}

void PhobosStreamReader::EmitExpectEndOfBlockWarning(std::true_type) const
{
	Debug::Log("PhobosStreamReader - Read %X bytes instead of %X!\n",
		this->stream->Offset(), this->stream->Size());
}

void PhobosStreamReader::EmitLoadWarning(size_t size, std::true_type) const
{
	Debug::Log("PhobosStreamReader - Could not read data of length %u at %X of %X.\n",
		size, this->stream->Offset() - size, this->stream->Size());
}

void PhobosStreamReader::EmitExpectWarning(unsigned int found, unsigned int expect, std::true_type) const
{
	Debug::Log("PhobosStreamReader - Found %X, expected %X\n", found, expect);
}

void PhobosStreamReader::EmitSwizzleWarning(long id, void* pointer, std::true_type) const
{
	Debug::Log("PhobosStreamReader - Could not register change from %X to %p\n", id, pointer);
}
