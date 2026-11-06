#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// A voxel cache is RLE data, not width * height bytes. Every row length
// includes its own two-byte prefix. Own the bytes before the cache resets.
namespace FoggedVoxelData
{
	constexpr int MaxDimension = 1024;
	constexpr unsigned MaxShapeBytes = 32 + MaxDimension * (2 * MaxDimension + 2);
	constexpr unsigned MaxParts = 8;
	constexpr unsigned MaxBuildings = 65536;
	constexpr unsigned LegacySaveMagic = 0x31585646; // FVX1: coordinate/type only
	constexpr unsigned SaveMagic = 0x32585646; // FVX2: native snapshot owner pointer

	inline unsigned Read16(const unsigned char* data)
	{
		return data[0] | (unsigned(data[1]) << 8);
	}

	inline void Write16(unsigned char* data, unsigned value)
	{
		data[0] = static_cast<unsigned char>(value);
		data[1] = static_cast<unsigned char>(value >> 8);
	}

	// With a size, validate saved data without reading beyond its block.
	// Without a size, the source is a valid native voxel cache entry.
	inline unsigned RLESize(const unsigned char* data, int width, int height,
		unsigned available = MaxShapeBytes)
	{
		if (!data || width <= 0 || height <= 0 || width > MaxDimension || height > MaxDimension)
			return 0;
		unsigned offset = 0;
		for (int y = 0; y < height; ++y)
		{
			if (offset > available || available - offset < 2)
				return 0;
			const unsigned length = Read16(data + offset);
			if (length < 3 || length > unsigned(2 * width + 2) || length > available - offset)
				return 0;
			unsigned pixels = 0;
			for (unsigned x = offset + 2; x < offset + length; ++x)
			{
				if (data[x])
					++pixels;
				else
				{
					if (++x >= offset + length || !data[x])
						return 0;
					pixels += data[x];
				}
				if (pixels > unsigned(width))
					return 0;
			}
			if (pixels != unsigned(width))
				return 0;
			offset += length;
		}
		return offset;
	}

	struct Image
	{
		int X = 0, Y = 0;
		int Width = 0, Height = 0;
		int PaletteIndex = -1;
		bool CustomPalette = false;
		unsigned Flags = 0;
		int ZAdjust = 0, Gradient = 0, Intensity = 1000, Tint = 0;
		std::vector<unsigned char> Shape;

		bool Capture(const void* buffer, int width, int height)
		{
			const auto data = static_cast<const unsigned char*>(buffer);
			const auto length = RLESize(data, width, height);
			if (!length)
				return false;
			Width = width;
			Height = height;
			Shape.assign(32 + length, 0);
			Write16(Shape.data() + 2, width);
			Write16(Shape.data() + 4, height);
			Write16(Shape.data() + 6, 1); // single-frame, inline SHP
			Write16(Shape.data() + 12, width);
			Write16(Shape.data() + 14, height);
			Shape[16] = 3; // SHP RLE, identical to the native voxel cache encoding
			Shape[28] = 32; // frame offset, little endian
			std::memcpy(Shape.data() + 32, data, length);
			return true;
		}

		bool CapturePlain(const unsigned char* pixels, int pitch, int width, int height)
		{
			if (!pixels || width <= 0 || height <= 0 || width > MaxDimension
				|| height > MaxDimension || pitch < width)
				return false;
			std::vector<unsigned char> rle;
			rle.reserve(height * (width + 2));
			for (int y = 0; y < height; ++y)
			{
				const auto start = rle.size();
				rle.resize(start + 2);
				const auto row = pixels + y * pitch;
				for (int x = 0; x < width; ++x)
				{
					rle.push_back(row[x]);
					if (!row[x])
					{
						unsigned run = 1;
						while (x + 1 < width && !row[x + 1] && run < 255) { ++x; ++run; }
						rle.push_back(static_cast<unsigned char>(run));
					}
				}
				Write16(rle.data() + start, static_cast<unsigned>(rle.size() - start));
			}
			return Capture(rle.data(), width, height);
		}

		bool Valid() const
		{
			if (Shape.size() < 32 || Shape.size() > MaxShapeBytes || PaletteIndex < 0
				|| X < -8192 || X > 8192 || Y < -8192 || Y > 8192
				|| Gradient < -1 || Gradient > 3)
				return false;
			// Verify all header bytes, including padding and the frame offset.
			unsigned char header[32] {};
			Write16(header + 2, Width); Write16(header + 4, Height);
			Write16(header + 6, 1); Write16(header + 12, Width);
			Write16(header + 14, Height); header[16] = 3; header[28] = 32;
			return std::memcmp(header, Shape.data(), 32) == 0
				&& RLESize(Shape.data() + 32, Width, Height,
					static_cast<unsigned>(Shape.size() - 32)) == Shape.size() - 32;
		}

		template <typename Writer> void Save(Writer& stm) const
		{
			stm.Save(X); stm.Save(Y); stm.Save(Width); stm.Save(Height);
			stm.Save(PaletteIndex); stm.Save(CustomPalette); stm.Save(Flags);
			stm.Save(ZAdjust); stm.Save(Gradient); stm.Save(Intensity); stm.Save(Tint);
			stm.Save(static_cast<unsigned>(Shape.size()));
			stm.Write(Shape.data(), Shape.size());
		}

		template <typename Reader> bool Load(Reader& stm)
		{
			unsigned length = 0;
			if (!stm.Load(X) || !stm.Load(Y) || !stm.Load(Width) || !stm.Load(Height)
				|| !stm.Load(PaletteIndex) || !stm.Load(CustomPalette) || !stm.Load(Flags)
				|| !stm.Load(ZAdjust) || !stm.Load(Gradient) || !stm.Load(Intensity)
				|| !stm.Load(Tint) || !stm.Load(length) || length < 32 || length > MaxShapeBytes)
				return false;
			Shape.resize(length);
			return stm.Read(Shape.data(), length) && Valid();
		}
	};
}
