#include "UnitPortrait.h"

#include <Phobos.h>

#include <AircraftTypeClass.h>
#include <AnimClass.h>
#include <AnimTypeClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <ColorScheme.h>
#include <ConvertClass.h>
#include <Dir.h>
#include <Drawing.h>
#include <FileSystem.h>
#include <InfantryTypeClass.h>
#include <Matrix3D.h>
#include <Memory.h>
#include <ScenarioClass.h>
#include <Surface.h>
#include <Theater.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Timer.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/WeaponType/Body.h>

#include <Utilities/Debug.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	constexpr int SoutheastFacing = 12; // 32 facings: N=0, E=8, SE=12, S=16, SW=20, W=24
	constexpr int SoutheastRaw = SoutheastFacing * 2048;

	constexpr DWORD RetryDelayMs = 1000;

	struct CacheKey
	{
		TechnoTypeClass* Type = nullptr;
		bool Elite = false;
		int Width = 0;
		int Height = 0;

		bool operator==(const CacheKey& other) const
		{
			return Type == other.Type
				&& Elite == other.Elite
				&& Width == other.Width
				&& Height == other.Height;
		}
	};

	struct CacheKeyHash
	{
		size_t operator()(const CacheKey& key) const
		{
			size_t hash = std::hash<void*>()(key.Type);
			hash ^= key.Elite ? static_cast<size_t>(0x9E3779B9u) : 0u;
			hash ^= static_cast<size_t>(key.Width) * 0x85EBCA6Bu;
			hash ^= static_cast<size_t>(key.Height) * 0xC2B2AE35u;
			return hash;
		}
	};

	std::unordered_map<CacheKey, PortraitImage, CacheKeyHash> Cache;
	std::unordered_map<CacheKey, DWORD, CacheKeyHash> RetryAt;
	std::unordered_set<TechnoTypeClass*> LoggedRequests;
	std::unordered_set<TechnoTypeClass*> LoggedFailures;
	std::unordered_set<TechnoTypeClass*> LoggedModels;
	std::unordered_set<TechnoTypeClass*> LoggedBuildingAnims;
	std::unordered_set<TechnoTypeClass*> LoggedVoxelDiag;


	bool ShouldLogFailure(TechnoTypeClass* pType)
	{
		return LoggedFailures.insert(pType).second;
	}

	bool ShouldLogModel(TechnoTypeClass* pType)
	{
		return LoggedModels.insert(pType).second;
	}

	bool ShouldLogBuildingAnims(TechnoTypeClass* pType)
	{
		return LoggedBuildingAnims.insert(pType).second;
	}

	bool ShouldLogVoxelDiag(TechnoTypeClass* pType)
	{
		return LoggedVoxelDiag.insert(pType).second;
	}

	int FacingDistance(int facing, int desired)
	{
		facing %= 32;
		if (facing < 0)
			facing += 32;

		desired %= 32;
		if (desired < 0)
			desired += 32;

		const int diff = std::abs(facing - desired);
		return std::min(diff, 32 - diff);
	}

	struct SHPFrameInfo
	{
		int BodyFrame = 0;
		int TurretFrame = -1;
	};

	struct VoxelPart
	{
		VoxelCacheStruct* Entry = nullptr;
		int OffsetX = 0;
		int OffsetY = 0;
	};

	// A temporary drawing area on one of the game's DSurfaces. The surface is
	// filled with a key colour instead of black: dark vehicle pixels are valid
	// model pixels, and using black as the transparency key makes them vanish.
	struct ScratchSurface
	{
		DSurface* Surface = nullptr;
		int Width = 0;
		int Height = 0;
		RectangleStruct Rect {};
		COLORREF Key = 0;
		std::vector<COLORREF> Saved;

		bool Begin(DSurface* pSurface, int width, int height)
		{
			if (!pSurface || width <= 0 || height <= 0)
				return false;

			Surface = pSurface;
			Width = std::min(width, pSurface->Width);
			Height = std::min(height, pSurface->Height);

			if (Width <= 0 || Height <= 0)
				return false;

			Rect = { 0, 0, Width, Height };
			Saved.resize(static_cast<size_t>(Width) * Height);

			for (int y = 0; y < Height; ++y)
			{
				for (int x = 0; x < Width; ++x)
				{
					Point2D point { x, y };
					Saved[static_cast<size_t>(y) * Width + x] = Surface->GetPixel(&point);
				}
			}

			Clear();
			return true;
		}

		void End()
		{
			if (!Surface)
				return;

			for (int y = 0; y < Height; ++y)
			{
				for (int x = 0; x < Width; ++x)
				{
					Point2D point { x, y };
					Surface->SetPixel(&point, Saved[static_cast<size_t>(y) * Width + x]);
				}
			}

			Saved.clear();
			Surface = nullptr;
		}

		void Clear()
		{
			if (!Surface)
				return;

			// Fill with a fixed colour and remember what the surface actually
			// stored. This avoids relying on colour equality after a possible
			// 16-bit conversion.
			Surface->FillRect(&Rect, Drawing::RGB_To_Int(255, 0, 255));

			Point2D point { 0, 0 };
			Key = Surface->GetPixel(&point);
		}

		bool IsOpaque(COLORREF color) const
		{
			return color != Key;
		}

		PortraitImage Capture() const
		{
			PortraitImage image;

			if (!Surface || Width <= 0 || Height <= 0)
				return image;

			image.Width = Width;
			image.Height = Height;
			image.Pixels.resize(static_cast<size_t>(Width) * Height, 0);
			image.Mask.assign(static_cast<size_t>(Width) * Height, 0);

			for (int y = 0; y < Height; ++y)
			{
				for (int x = 0; x < Width; ++x)
				{
					Point2D point { x, y };
					const SIZE_T index = static_cast<size_t>(y) * Width + x;
					const COLORREF color = Surface->GetPixel(&point);
					image.Pixels[index] = color;
					image.Mask[index] = IsOpaque(color) ? 1 : 0;
				}
			}

			return image;
		}
	};

	bool FindContentBounds(const PortraitImage& source, RectangleStruct& out)
	{
		int minX = std::numeric_limits<int>::max();
		int minY = std::numeric_limits<int>::max();
		int maxX = std::numeric_limits<int>::min();
		int maxY = std::numeric_limits<int>::min();

		const bool useMask = source.Mask.size() == source.Pixels.size();
		const size_t count = static_cast<size_t>(source.Width) * source.Height;

		for (size_t index = 0; index < count; ++index)
		{
			const bool opaque = useMask ? source.Mask[index] != 0 : source.Pixels[index] != 0;

			if (!opaque)
				continue;

			const int x = static_cast<int>(index % source.Width);
			const int y = static_cast<int>(index / source.Width);
			minX = std::min(minX, x);
			minY = std::min(minY, y);
			maxX = std::max(maxX, x);
			maxY = std::max(maxY, y);
		}

		if (maxX < minX || maxY < minY)
			return false;

		out = { minX, minY, maxX - minX + 1, maxY - minY + 1 };
		return true;
	}

	bool FindVisibleBounds(const PortraitImage& source, RectangleStruct& out)
	{
		int minX = std::numeric_limits<int>::max();
		int minY = std::numeric_limits<int>::max();
		int maxX = std::numeric_limits<int>::min();
		int maxY = std::numeric_limits<int>::min();

		const bool useMask = source.Mask.size() == source.Pixels.size();
		const bool useShadow = source.Shadow.size() == source.Pixels.size();
		const size_t count = static_cast<size_t>(source.Width) * source.Height;

		for (size_t index = 0; index < count; ++index)
		{
			const bool body = useMask ? source.Mask[index] != 0 : source.Pixels[index] != 0;
			const bool shadow = useShadow && source.Shadow[index] != 0;

			if (!body && !shadow)
				continue;

			const int x = static_cast<int>(index % source.Width);
			const int y = static_cast<int>(index / source.Width);
			minX = std::min(minX, x);
			minY = std::min(minY, y);
			maxX = std::max(maxX, x);
			maxY = std::max(maxY, y);
		}

		if (maxX < minX || maxY < minY)
			return false;

		out = { minX, minY, maxX - minX + 1, maxY - minY + 1 };
		return true;
	}

	RectangleStruct UnionBounds(const RectangleStruct& a, const RectangleStruct& b)
	{
		const int x0 = std::min(a.X, b.X);
		const int y0 = std::min(a.Y, b.Y);
		const int x1 = std::max(a.X + a.Width, b.X + b.Width);
		const int y1 = std::max(a.Y + a.Height, b.Y + b.Height);

		return { x0, y0, x1 - x0, y1 - y0 };
	}

	int Luminance(COLORREF color)
	{
		BYTE r = 0, g = 0, b = 0;
		Drawing::Int_To_RGB(color, r, g, b);
		return (r * 77 + g * 150 + b * 29) >> 8;
	}

	// Translucent SHP blitters blend the SHP's transparent black pixels with
	// the scratch key colour. Those blended pixels are not equal to the key, so
	// they survive Capture() and become isolated dark specks after grayscale
	// conversion. Remove colours that lie on the key->black blend line.
	void RemoveKeyBlendSpecks(PortraitImage& image, COLORREF key)
	{
		if (image.Mask.size() != image.Pixels.size())
			return;

		BYTE keyR = 0, keyG = 0, keyB = 0;
		Drawing::Int_To_RGB(key, keyR, keyG, keyB);

		if (keyR == 0 && keyG == 0 && keyB == 0)
			return;

		for (size_t index = 0; index < image.Mask.size(); ++index)
		{
			if (!image.Mask[index])
				continue;

			BYTE r = 0, g = 0, b = 0;
			Drawing::Int_To_RGB(image.Pixels[index], r, g, b);

			const auto isNear = [](BYTE value, int expected)
			{
				return std::abs(static_cast<int>(value) - expected) <= 2;
			};

			// 50% key blend (the common black-transparent case), plus the
			// 25% and 75% variants used by the other translucent blitters.
			const bool keyHalf = isNear(r, keyR / 2) && isNear(g, keyG / 2) && isNear(b, keyB / 2);
			const bool keyQuarter = isNear(r, keyR / 4) && isNear(g, keyG / 4) && isNear(b, keyB / 4);
			const bool keyThreeQuarter = isNear(r, keyR * 3 / 4) && isNear(g, keyG * 3 / 4) && isNear(b, keyB * 3 / 4);

			if (keyHalf || keyQuarter || keyThreeQuarter)
			{
				image.Mask[index] = 0;
				image.Pixels[index] = 0;
			}
		}
	}

	// One-shot diagnostic dump. Used to compare the native voxel render and the
	// engine cache render when a model still looks wrong.
	void SavePortraitBmp(const PortraitImage& image, const char* pPath)
	{
		if (!pPath || image.Width <= 0 || image.Height <= 0
			|| image.Pixels.size() < static_cast<size_t>(image.Width) * image.Height)
		{
			return;
		}

		const int rowSize = (image.Width * 3 + 3) & ~3;
		const int dataSize = rowSize * image.Height;
		const int fileSize = 54 + dataSize;
		unsigned char header[54] {};

		header[0] = 'B';
		header[1] = 'M';

		auto put32 = [&](int offset, unsigned value)
		{
			header[offset + 0] = static_cast<unsigned char>(value & 0xFF);
			header[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
			header[offset + 2] = static_cast<unsigned char>((value >> 16) & 0xFF);
			header[offset + 3] = static_cast<unsigned char>((value >> 24) & 0xFF);
		};

		put32(2, static_cast<unsigned>(fileSize));
		put32(10, 54);
		put32(14, 40);
		put32(18, static_cast<unsigned>(image.Width));
		put32(22, static_cast<unsigned>(image.Height));
		header[26] = 1;
		header[28] = 24;
		put32(34, static_cast<unsigned>(dataSize));

		FILE* pFile = nullptr;

		if (fopen_s(&pFile, pPath, "wb") != 0 || !pFile)
			return;

		fwrite(header, 1, 54, pFile);

		std::vector<unsigned char> row(static_cast<size_t>(rowSize), 0);

		for (int y = image.Height - 1; y >= 0; --y)
		{
			for (int x = 0; x < image.Width; ++x)
			{
				BYTE r = 0, g = 0, b = 0;
				Drawing::Int_To_RGB(
					image.Pixels[static_cast<size_t>(y) * image.Width + x], r, g, b);
				row[static_cast<size_t>(x) * 3 + 0] = b;
				row[static_cast<size_t>(x) * 3 + 1] = g;
				row[static_cast<size_t>(x) * 3 + 2] = r;
			}

			fwrite(row.data(), 1, static_cast<size_t>(rowSize), pFile);
		}

		fclose(pFile);
	}

	// Scales and grayscales a captured model. The body's content rect controls
	// the scale/centering; sourceBounds may be larger (shadow) and is clipped to
	// the target frame instead of shrinking the body. The shadow is emitted as a
	// separate per-pixel alpha channel so it can be blended translucently at draw
	// time instead of being baked into the body as a solid grey slab.
	PortraitImage ScaleGrayscale(const PortraitImage& source,
		const RectangleStruct& content, const RectangleStruct& sourceBounds,
		int targetWidth, int targetHeight, bool allowUpscale,
		const PortraitImage* pShadow = nullptr, BYTE shadowOpacity = 0,
		int fillPercent = 90)
	{
		PortraitImage output;

		if (content.Width <= 0 || content.Height <= 0 || targetWidth <= 0 || targetHeight <= 0
			|| source.Width <= 0 || source.Height <= 0 || source.Pixels.empty()
			|| source.Mask.size() != source.Pixels.size())
		{
			return output;
		}

		RectangleStruct bounds = sourceBounds;
		bounds.X = std::clamp(bounds.X, 0, source.Width - 1);
		bounds.Y = std::clamp(bounds.Y, 0, source.Height - 1);
		bounds.Width = std::min(bounds.Width, source.Width - bounds.X);
		bounds.Height = std::min(bounds.Height, source.Height - bounds.Y);

		if (bounds.Width <= 0 || bounds.Height <= 0)
			return output;

		const bool hasShadow = pShadow
			&& pShadow->Width == source.Width
			&& pShadow->Height == source.Height
			&& pShadow->Mask.size() == source.Pixels.size()
			&& shadowOpacity > 0;

		const int maxWidth = std::max(1, targetWidth * fillPercent / 100);
		const int maxHeight = std::max(1, targetHeight * fillPercent / 100);

		double scale = std::min(
			static_cast<double>(maxWidth) / content.Width,
			static_cast<double>(maxHeight) / content.Height);

		// Infantry art is intentionally small. Do not blow it up.
		if (!allowUpscale)
			scale = std::min(scale, 1.0);

		scale = std::max(scale, 0.01);

		const int drawWidth = std::max(1, static_cast<int>(std::lround(content.Width * scale)));
		const int drawHeight = std::max(1, static_cast<int>(std::lround(content.Height * scale)));
		const int offsetX = (targetWidth - drawWidth) / 2;
		const int offsetY = (targetHeight - drawHeight) / 2;

		output.Width = targetWidth;
		output.Height = targetHeight;
		output.Pixels.assign(static_cast<size_t>(targetWidth) * targetHeight, 0);
		output.Mask.assign(static_cast<size_t>(targetWidth) * targetHeight, 0);
		output.Shadow.assign(static_cast<size_t>(targetWidth) * targetHeight, 0);

		const double invScale = 1.0 / scale;

		for (int ty = 0; ty < targetHeight; ++ty)
		{
			for (int tx = 0; tx < targetWidth; ++tx)
			{
				const size_t targetIndex = static_cast<size_t>(ty) * targetWidth + tx;
				int red = 0;
				int green = 0;
				int blue = 0;
				int bodySamples = 0;
				int shadowSamples = 0;
				int area = 0;

				if (scale >= 1.0)
				{
					// Nearest-neighbour upscaling keeps pixel art crisp.
					const int sx = content.X + static_cast<int>(std::lround((tx - offsetX) * invScale));
					const int sy = content.Y + static_cast<int>(std::lround((ty - offsetY) * invScale));

					if (sx >= bounds.X && sy >= bounds.Y
						&& sx < bounds.X + bounds.Width && sy < bounds.Y + bounds.Height)
					{
						const size_t sourceIndex = static_cast<size_t>(sy) * source.Width + sx;
						area = 1;

						if (sourceIndex < source.Mask.size() && source.Mask[sourceIndex])
						{
							BYTE r = 0, g = 0, b = 0;
							Drawing::Int_To_RGB(source.Pixels[sourceIndex], r, g, b);
							red = r;
							green = g;
							blue = b;
							bodySamples = 1;
						}

						if (hasShadow && sourceIndex < pShadow->Mask.size() && pShadow->Mask[sourceIndex])
							shadowSamples = 1;
					}
				}
				else
				{
					// Box filtering avoids the broken-up look caused by
					// nearest-neighbour downscaling of 320x240 vehicle art.
					const int sx0 = content.X + static_cast<int>(std::floor((tx - offsetX) * invScale));
					const int sx1 = content.X + static_cast<int>(std::ceil((tx + 1 - offsetX) * invScale));
					const int sy0 = content.Y + static_cast<int>(std::floor((ty - offsetY) * invScale));
					const int sy1 = content.Y + static_cast<int>(std::ceil((ty + 1 - offsetY) * invScale));

					const int x0 = std::clamp(sx0, bounds.X, bounds.X + bounds.Width);
					const int x1 = std::clamp(sx1, bounds.X, bounds.X + bounds.Width);
					const int y0 = std::clamp(sy0, bounds.Y, bounds.Y + bounds.Height);
					const int y1 = std::clamp(sy1, bounds.Y, bounds.Y + bounds.Height);

					area = std::max(0, x1 - x0) * std::max(0, y1 - y0);

					for (int sy = y0; sy < y1; ++sy)
					{
						for (int sx = x0; sx < x1; ++sx)
						{
							const size_t sourceIndex = static_cast<size_t>(sy) * source.Width + sx;

							if (sourceIndex < source.Mask.size() && source.Mask[sourceIndex])
							{
								BYTE r = 0, g = 0, b = 0;
								Drawing::Int_To_RGB(source.Pixels[sourceIndex], r, g, b);
								red += r;
								green += g;
								blue += b;
								++bodySamples;
							}

							if (hasShadow && sourceIndex < pShadow->Mask.size() && pShadow->Mask[sourceIndex])
								++shadowSamples;
						}
					}
				}

				if (hasShadow && bodySamples <= 0 && shadowSamples > 0 && area > 0)
				{
					const int alpha = static_cast<int>(shadowOpacity) * shadowSamples / area;
					output.Shadow[targetIndex] = static_cast<BYTE>(std::clamp(alpha, 0, 255));
				}

				if (bodySamples <= 0)
					continue;

				output.Pixels[targetIndex] = Drawing::RGB_To_Int(
					red / bodySamples, green / bodySamples, blue / bodySamples);
				output.Mask[targetIndex] = 1;
			}
		}

		// Normalise the luminance of the resulting model so dark vehicles keep
		// their detail instead of becoming a washed-out white silhouette.
		int minLuminance = 255;
		int maxLuminance = 0;
		bool any = false;

		for (size_t index = 0; index < output.Mask.size(); ++index)
		{
			if (!output.Mask[index])
				continue;

			const int luminance = Luminance(output.Pixels[index]);
			minLuminance = std::min(minLuminance, luminance);
			maxLuminance = std::max(maxLuminance, luminance);
			any = true;
		}

		if (!any)
			return {};

		const int range = maxLuminance - minLuminance;

		for (size_t index = 0; index < output.Mask.size(); ++index)
		{
			if (!output.Mask[index])
				continue;

			const int luminance = Luminance(output.Pixels[index]);
			int bright = 0;

			if (range < 24)
				bright = 30 + luminance * 225 / 255;
			else
				bright = 20 + (luminance - minLuminance) * 235 / range;

			bright = std::clamp(bright, 0, 255);
			output.Pixels[index] = Drawing::RGB_To_Int(bright, bright, bright);
		}

		FindVisibleBounds(output, output.ContentBounds);
		return output;
	}

	SHPFrameInfo SelectSHPFrames(TechnoTypeClass* pType, SHPStruct* pImage)
	{
		SHPFrameInfo info {};

		if (!pImage || pImage->Frames <= 0)
			return info;

		const auto whatAmI = pType->WhatAmI();

		switch (whatAmI)
		{
		case AbstractType::BuildingType:
			info.BodyFrame = 0;
			break;
		case AbstractType::InfantryType:
		{
			auto const pInfType = static_cast<InfantryTypeClass*>(pType);

			if (pInfType->Sequence)
			{
				const auto& ready = pInfType->Sequence->GetSequence(Sequence::Ready);

				if (ready.CountFrames > 0)
				{
					// Infantry sequences start at north and rotate
					// counter-clockwise: 0=N, 1=NW, 2=W, 3=SW, 4=S,
					// 5=SE, 6=E, 7=NE. 5 is the south-east 3/4 view.
					constexpr int facing = 5;
					info.BodyFrame = ready.StartFrame + facing * std::max(1, ready.FacingMultiplier);
				}
			}

			break;
		}
		case AbstractType::UnitType:
		{
			auto const pUnitType = static_cast<UnitTypeClass*>(pType);
			const int facings = std::max(1, pUnitType->Facings);
			int facing = 0;

			switch (facings)
			{
			case 2:
				facing = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<2>(0));
				break;
			case 4:
				facing = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<4>(0));
				break;
			case 8:
				facing = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<8>(0));
				break;
			case 16:
				facing = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<16>(0));
				break;
			case 32:
				facing = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<32>(0));
				break;
			default:
				facing = 0;
				break;
			}

			// Jumpjet units (the mod's helicopters) keep their flying pose in
			// the walk-frame block. The art's StandFrames key is not read by
			// the engine, so using StandingFrames here picks the wrong frame
			// and makes the portrait face the wrong way.
			const bool jumpjet = pUnitType->JumpJet;
			const int blockStart = jumpjet
				? pUnitType->StartWalkFrame
				: pUnitType->StartStandFrame;
			const int blockFrames = jumpjet
				? std::max(1, static_cast<int>(pUnitType->WalkFrames))
				: std::max(1, pUnitType->StandingFrames);

			info.BodyFrame = blockStart + facing * blockFrames;

			if (pType->Turret)
			{
				const int turretStart = blockStart + facings * blockFrames;
				const int turretFrame = turretStart + facing * blockFrames;

				if (turretFrame >= 0 && turretFrame < pImage->Frames)
					info.TurretFrame = turretFrame;
			}

			break;
		}
		case AbstractType::AircraftType:
			info.BodyFrame = static_cast<int>(DirStruct(SoutheastRaw).GetFacing<32>(0));
			break;
		default:
			info.BodyFrame = 0;
			break;
		}

		info.BodyFrame = std::clamp(info.BodyFrame, 0, static_cast<int>(pImage->Frames) - 1);
		return info;
	}

	AnimTypeClass* FindAnimType(const char* pID)
	{
		if (!pID || !pID[0])
			return nullptr;

		auto const& array = AnimTypeClass::Array;

		for (int i = 0; i < array.Count; ++i)
		{
			auto const pType = array.GetItem(i);

			if (pType && _stricmp(pType->get_ID(), pID) == 0)
				return pType;
		}

		return nullptr;
	}

	ConvertClass* GetTheaterPalette()
	{
		if (!ScenarioClass::Instance)
			return nullptr;

		char paletteName[0x10] {};
		std::strncpy(paletteName,
			Theater::GetTheater(ScenarioClass::Instance->Theater).PaletteFileName,
			std::size(paletteName) - 5);
		paletteName[std::size(paletteName) - 5] = 0;

		if (!paletteName[0])
			return nullptr;

		std::string filename = paletteName;
		filename += ".pal";

		static std::unordered_map<std::string, ConvertClass*> cache;
		auto it = cache.find(filename);

		if (it != cache.end())
			return it->second;

		ConvertClass* pConvert = nullptr;
		auto* pSchemes = ColorScheme::GeneratePalette(
			const_cast<char*>(filename.c_str()));

		if (pSchemes && pSchemes->Count > 0)
		{
			if (auto const pScheme = pSchemes->GetItem(0))
				pConvert = pScheme->LightConvert;
		}

		cache.emplace(filename, pConvert);
		return pConvert;
	}

	PortraitImage RenderSHP(TechnoClass* pThis, TechnoTypeClass* pType,
		int targetWidth, int targetHeight)
	{
		SHPStruct* pImage = pType->GetImage();

		if (pImage && pImage->IsReference())
			pImage->Load();

		if (!pImage || pImage->Width <= 0 || pImage->Height <= 0 || pImage->Frames <= 0)
		{
			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] No SHP image for type '%s'\n", pType->ID);

			return {};
		}

		auto frameInfo = SelectSHPFrames(pType, pImage);

		ConvertClass* pPalette = nullptr;

		if (pThis && pType->WhatAmI() == AbstractType::BuildingType)
		{
			auto const pBldType = static_cast<BuildingTypeClass*>(pType);

			if (pBldType->TerrainPalette)
			{
				// The engine draws TerrainPalette buildings with the cell's
				// light convert. That convert is built from the raw theater
				// palette, so indices 16-31 keep their terrain colours.
				// GetTheaterPalette() returns a ColorScheme LightConvert,
				// which remaps those indices to the first house colour and
				// corrupts the building texture.
				if (auto const pCell = pThis->GetCell())
				{
					if (!pCell->LightConvert)
						pCell->InitLightConvert();

					pPalette = pCell->LightConvert;
				}

				if (!pPalette)
					pPalette = GetTheaterPalette();
			}
		}

		if (!pPalette && pThis)
			pPalette = pThis->GetDrawer();

		if (!pPalette)
			pPalette = FileSystem::UNITx_PAL;

		if (!pPalette)
			pPalette = FileSystem::PALETTE_PAL;

		if (!pPalette)
		{
			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] No palette for type '%s'\n", pType->ID);

			return {};
		}

		DSurface* pSurface = DSurface::Temp ? DSurface::Temp : DSurface::Composite;

		if (!pSurface)
			return {};

		const int scratchWidth = std::min(pSurface->Width,
			std::max(static_cast<int>(pImage->Width) + 128, 256));
		const int scratchHeight = std::min(pSurface->Height,
			std::max(static_cast<int>(pImage->Height) + 128, 256));

		ScratchSurface scratch;

		if (!scratch.Begin(pSurface, scratchWidth, scratchHeight))
			return {};

		const Point2D bodyPos { scratch.Width / 2, scratch.Height / 2 };

		if (Phobos::Config::UnitStatusPanel_NativeDraw
			&& pThis && pType->WhatAmI() == AbstractType::UnitType
			&& !pType->Voxel)
		{
			auto const pUnit = static_cast<UnitClass*>(pThis);
			FacingClass oldPrimary { pUnit->PrimaryFacing };
			FacingClass oldSecondary { pUnit->SecondaryFacing };
			const bool oldNoShadow = pType->NoShadow;

			pUnit->PrimaryFacing.SetCurrent(DirStruct(SoutheastRaw));
			pUnit->SecondaryFacing.SetCurrent(DirStruct(SoutheastRaw));
			pType->NoShadow = true;
			pUnit->DrawObject(scratch.Surface, bodyPos, scratch.Rect, 1000, 0);
			pType->NoShadow = oldNoShadow;
			pUnit->PrimaryFacing = oldPrimary;
			pUnit->SecondaryFacing = oldSecondary;

			PortraitImage body = scratch.Capture();
			scratch.End();

			RectangleStruct bodyBounds {};

			if (!FindContentBounds(body, bodyBounds))
				return {};

			PortraitImage output = ScaleGrayscale(body, bodyBounds, bodyBounds,
				targetWidth, targetHeight,
				pType->WhatAmI() != AbstractType::InfantryType,
				nullptr, 0);

			if (ShouldLogModel(pType))
			{
				Debug::Log("[UnitPortrait] Native SHP type '%s' out=%dx%d content=%d,%d %dx%d\n",
					pType->ID, output.Width, output.Height,
					bodyBounds.X, bodyBounds.Y, bodyBounds.Width, bodyBounds.Height);
			}

			return output;
		}

		auto drawFrame = [&](int frame, BlitterFlags flags, const Point2D& position)
		{
			if (frame < 0 || frame >= pImage->Frames)
				return;

			scratch.Surface->DrawSHP(pPalette, pImage, frame, &position, &scratch.Rect,
				flags | BlitterFlags::Centered,
				0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		};

		// Draws a building SHP field (BibShape, door anims, ...) with the same
		// centered canvas origin the engine uses for the building body.
		auto drawBuildingSHP = [&](SHPStruct* pShape, int zAdjust, const Point2D& position)
		{
			if (!pShape)
				return;

			if (pShape->IsReference())
				pShape->Load();

			if (pShape->Frames <= 0)
				return;

			scratch.Surface->DrawSHP(pPalette, pShape, 0, &position, &scratch.Rect,
				BlitterFlags::bf_400 | BlitterFlags::Centered,
				0, zAdjust, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		};

		auto countOpaque = [&]()
		{
			int count = 0;

			for (int y = 0; y < scratch.Height; ++y)
			{
				for (int x = 0; x < scratch.Width; ++x)
				{
					Point2D point { x, y };

					if (scratch.IsOpaque(scratch.Surface->GetPixel(&point)))
						++count;
				}
			}

			return count;
		};

		// Some building SHPs use frame 0 as an empty placeholder: the helipad
		// body is transparent and its visible pad is the BibShape. If frame 0
		// carries almost nothing, scan the frames and use the most populated
		// one so the body itself still contributes.
		if (pType->WhatAmI() == AbstractType::BuildingType && pImage->Frames > 1)
		{
			scratch.Clear();
			drawFrame(frameInfo.BodyFrame, BlitterFlags::bf_400, bodyPos);
			const int baseCount = countOpaque();

			if (baseCount < 16)
			{
				int bestFrame = frameInfo.BodyFrame;
				int bestFrameCount = baseCount;

				for (int frame = 0; frame < pImage->Frames; ++frame)
				{
					scratch.Clear();
					drawFrame(frame, BlitterFlags::bf_400, bodyPos);
					const int count = countOpaque();

					if (count > bestFrameCount)
					{
						bestFrameCount = count;
						bestFrame = frame;
					}
				}

				frameInfo.BodyFrame = bestFrame;
			}
		}

		struct FlagDef
		{
			BlitterFlags Flags;
			const char* Name;
		};

		const FlagDef flags[] = {
			{ BlitterFlags::bf_400, "bf400" },
			{ BlitterFlags::Flat, "flat-only" },
			{ BlitterFlags::Flat | BlitterFlags::Alpha, "flat" },
			{ BlitterFlags::TransLucent50, "t50" },
			{ BlitterFlags::TransLucent75, "t75" },
			{ BlitterFlags::TransLucent25, "t25" },
		};

		BlitterFlags chosenFlags = BlitterFlags::bf_400;
		int bestCount = 0;
		int bf400Count = 0;
		int flatOnlyCount = 0;
		int flatAlphaCount = 0;
		const char* chosenName = "bf400";
		const int scratchPixels = scratch.Width * scratch.Height;

		for (const auto& flag : flags)
		{
			scratch.Clear();
			drawFrame(frameInfo.BodyFrame, flag.Flags, bodyPos);
			const int count = countOpaque();

			if (flag.Flags == BlitterFlags::bf_400)
				bf400Count = count;
			else if (flag.Flags == BlitterFlags::Flat)
				flatOnlyCount = count;
			else if (flag.Flags == (BlitterFlags::Flat | BlitterFlags::Alpha))
				flatAlphaCount = count;

			// An opaque blitter that fills most of the scratch pad is drawing
			// the transparent background as black. Never let that win.
			const bool opaque = flag.Flags == BlitterFlags::bf_400
				|| flag.Flags == BlitterFlags::Flat
				|| flag.Flags == (BlitterFlags::Flat | BlitterFlags::Alpha);
			const bool sane = !opaque || count < scratchPixels / 3;

			if (sane && count > bestCount)
			{
				bestCount = count;
				chosenFlags = flag.Flags;
				chosenName = flag.Name;
			}
		}

		if (bestCount <= 0)
		{
			scratch.End();

			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] SHP body rendered empty for type '%s' (frame=%d, %dx%d, %d frames)\n",
					pType->ID, frameInfo.BodyFrame, pImage->Width, pImage->Height, pImage->Frames);

			return {};
		}

		// Prefer an opaque flat pass whenever it preserves essentially all of
		// the hull. Translucent blitters blend the transparent black background
		// into the scratch key and leave isolated dark speckles after grayscale
		// conversion ("black dots").
		const bool bf400Sane = bf400Count > 0 && bf400Count < scratchPixels / 3;
		const bool flatOnlySane = flatOnlyCount > 0
			&& flatOnlyCount < scratchPixels / 3;

		if (bf400Sane && bf400Count * 100 >= bestCount * 90)
		{
			chosenFlags = BlitterFlags::bf_400;
			chosenName = "bf400";
		}
		else if (flatOnlySane && flatOnlyCount * 100 >= bestCount * 90)
		{
			chosenFlags = BlitterFlags::Flat;
			chosenName = "flat-only";
		}
		else if (flatAlphaCount * 100 >= bestCount * 90)
		{
			chosenFlags = BlitterFlags::Flat | BlitterFlags::Alpha;
			chosenName = "flat";
		}

		// SHP turret frames only bake in the turret's height: across every
		// facing their rotation pivot stays at the unit origin in the frame
		// canvas. The horizontal part of the art's TurretOffset still has to
		// be applied. Mode 6 projects it directly in the SHP canvas axes
		// (identity matrix): the preview frame faces the screen's 2:1
		// isometric down-right diagonal, so a negative offset moves the turret
		// up-left, toward the rear. Modes 1/2/4/5 keep the old engine/voxel
		// rotation for art authored in that convention; mode 0 disables the
		// offset entirely.
		Point2D turretPos = bodyPos;
		Point2D turretDelta { 0, 0 };
		int turretOffsetMode = Phobos::Config::UnitStatusPanel_TurretOffsetMode;

		if (pType)
		{
			auto const it = Phobos::Config::UnitStatusPanel_TurretOffsetModeOverrides.find(pType->ID);

			if (it != Phobos::Config::UnitStatusPanel_TurretOffsetModeOverrides.end())
				turretOffsetMode = it->second;
		}

		for (auto const& typeId : Phobos::Config::UnitStatusPanel_TurretOffsetNoOffset)
		{
			if (pType && _stricmp(typeId.c_str(), pType->ID) == 0)
			{
				turretOffsetMode = 0;
				break;
			}
		}

		if (frameInfo.TurretFrame >= 0 && turretOffsetMode != 0)
		{
			if (turretOffsetMode == 3)
			{
				turretDelta.X = Phobos::Config::UnitStatusPanel_TurretOffsetX;
				turretDelta.Y = Phobos::Config::UnitStatusPanel_TurretOffsetY;
			}
			else
			{
				Matrix3D mtx = Matrix3D::GetIdentity();

				// Mode 6: the SHP preview frame is drawn facing the canvas's
				// down-right isometric diagonal, so the offset must not be
				// rotated by the engine facing (that is the voxel convention
				// and moves the turret vertically instead of backwards).
				if (turretOffsetMode != 6)
					mtx.RotateZ(static_cast<float>(DirStruct(SoutheastRaw).GetRadian<32>()));

				TechnoTypeExt::ApplyTurretOffset(pType, &mtx);

				const auto res = mtx.GetTranslation();
				int xSign = 1;
				int ySign = -1;

				if (turretOffsetMode == 2)
					ySign = 1;
				else if (turretOffsetMode == 4)
				{
					xSign = -1;
					ySign = -1;
				}
				else if (turretOffsetMode == 5)
				{
					xSign = -1;
					ySign = 1;
				}

				const CoordStruct location {
					static_cast<int>(res.X) * xSign,
					static_cast<int>(res.Y) * ySign,
					static_cast<int>(res.Z)
				};

				turretDelta = TacticalClass::CoordsToScreen(location);
				turretDelta.X += Phobos::Config::UnitStatusPanel_TurretOffsetX;
				turretDelta.Y += Phobos::Config::UnitStatusPanel_TurretOffsetY;
			}

			turretPos.X += turretDelta.X;
			turretPos.Y += turretDelta.Y;
		}

		int shadowBody = -1;
		int shadowTurret = -1;

		if (pType->WhatAmI() == AbstractType::UnitType)
		{
			auto const pUnitType = static_cast<UnitTypeClass*>(pType);
			const int stride = std::max(1, pUnitType->Facings)
				* std::max(1, pUnitType->StandingFrames);

			// Turreted units use [body][turret][body shadow][turret shadow].
			// Units without a turret use [body][body shadow].
			shadowBody = frameInfo.BodyFrame
				+ (frameInfo.TurretFrame >= 0 ? stride * 2 : stride);

			if (shadowBody < 0 || shadowBody >= pImage->Frames)
				shadowBody = -1;

			if (frameInfo.TurretFrame >= 0)
			{
				shadowTurret = frameInfo.TurretFrame + stride * 2;

				if (shadowTurret < 0 || shadowTurret >= pImage->Frames)
					shadowTurret = -1;
			}
		}

		if (!Phobos::Config::UnitStatusPanel_SHPShadow)
		{
			shadowBody = -1;
			shadowTurret = -1;
		}

		// Shadow is rendered first as a mask. It is blended as a translucent
		// layer underneath the body, never as a second bright vehicle.
		scratch.Clear();

		if (shadowBody >= 0)
			drawFrame(shadowBody, chosenFlags, bodyPos);

		if (shadowTurret >= 0)
			drawFrame(shadowTurret, chosenFlags, turretPos);

		PortraitImage shadow = scratch.Capture();

		// Body, turret and ExtraFire mounts.
		scratch.Clear();

		if (pType->WhatAmI() == AbstractType::BuildingType)
		{
			auto const pBldType = static_cast<BuildingTypeClass*>(pType);

			// The bib carries the ground-level part of many buildings: helipad
			// pads, airfield runways and factory aprons. Without it those
			// buildings look almost empty in the portrait (GAHPAD/NAHPAD
			// rendered a single opaque pixel before this).
			drawBuildingSHP(pBldType->BibShape, 0, bodyPos);

			// The under-door art sits behind the building body.
			drawBuildingSHP(pBldType->UnderDoorAnim, 0, bodyPos);
		}

		drawFrame(frameInfo.BodyFrame, chosenFlags, bodyPos);

		if (frameInfo.TurretFrame >= 0)
			drawFrame(frameInfo.TurretFrame, chosenFlags, turretPos);

		auto drawExtraFireMounts = [&](TechnoClass* pTechno, TechnoTypeClass* pTechnoType, const Point2D& basePos)
		{
			if (!pTechno || !pTechnoType)
				return;

			const bool isElite = pTechno->Veterancy.IsElite();

			for (int slot = 0; slot < TechnoTypeClass::MaxWeapons; ++slot)
			{
				auto const pParentWeapon = TechnoTypeExt::GetWeaponType(pTechnoType, slot, isElite);

				if (!pParentWeapon)
					continue;

				auto const pParentExt = WeaponTypeExt::ExtMap.Find(pParentWeapon);
				const int count = static_cast<int>(pParentExt->ExtraFire_Weapons.size());

				if (count <= 0)
					continue;

				auto const& parentStruct = (isElite && pTechnoType->GetWeapon(slot, true).WeaponType)
					? pTechnoType->GetWeapon(slot, true)
					: pTechnoType->GetWeapon(slot, false);

				for (int i = 0; i < count; ++i)
				{
					auto const pUnitType = static_cast<UnitTypeClass*>(pParentExt->GetExtraFireImageType(i));

					if (!pUnitType)
						continue;

					auto pShape = pUnitType->Image;

					if (pShape && pShape->IsReference())
						pShape->Load();

					if (!pShape || pShape->Frames <= 0)
						continue;

					// The mount art arrays are config-driven; a malformed
					// entry should not read past the end of a vector.
					if (i >= static_cast<int>(pParentExt->ExtraFire_Image_RotationAdjusts.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_Image_Offsets.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_Turrets.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_Image_FLHs.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_FLHs.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_OnTurret.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_Image_OnTurrets.size())
						|| i >= static_cast<int>(pParentExt->ExtraFire_Image_HouseColors.size()))
					{
						continue;
					}

					const bool hasMount = pParentExt->ExtraFire_Turrets[i];
					auto const& imageFLH = pParentExt->ExtraFire_Image_FLHs[i];
					const auto flh = imageFLH.isset()
						? imageFLH.Get()
						: (pParentExt->ExtraFire_FLHs[i].isset() ? pParentExt->ExtraFire_FLHs[i].Get() : parentStruct.FLH);

					(void)hasMount;

					Matrix3D mtx = Matrix3D::GetIdentity();
					mtx.RotateZ(static_cast<float>(DirStruct(SoutheastRaw).GetRadian<32>()));

					mtx.Translate(static_cast<float>(flh.X), static_cast<float>(flh.Y), static_cast<float>(flh.Z));

					const auto result = mtx.GetTranslation();
					const CoordStruct location {
						static_cast<int>(result.X),
						static_cast<int>(-result.Y),
						static_cast<int>(result.Z)
					};
					const Point2D screenDelta = TacticalClass::CoordsToScreen(location);

					const auto& artType = *pUnitType;
					const int facings = artType.Facings > 0 ? artType.Facings : 32;
					const int standFrames = std::max(1, artType.StandingFrames);
					const int rotationAdjust = pParentExt->ExtraFire_Image_RotationAdjusts[i];
					const DirStruct frameFacing { static_cast<short>(SoutheastRaw + rotationAdjust * 256) };
					int facingIndex = 0;

					if (facings > 1)
					{
						const int facing32 = static_cast<int>(frameFacing.GetFacing<32>(0));
						facingIndex = facing32 * facings / 32;
					}

					const int frame = std::clamp(artType.StartStandFrame + facingIndex * standFrames,
						0, static_cast<int>(pShape->Frames) - 1);

					Point2D drawPos = basePos;
					drawPos.X += screenDelta.X + pParentExt->ExtraFire_Image_Offsets[i].X;
					drawPos.Y += screenDelta.Y + pParentExt->ExtraFire_Image_Offsets[i].Y;

					ConvertClass* pDrawer = FileSystem::PALETTE_PAL;

					if (pParentExt->ExtraFire_Image_HouseColors[i])
					{
						if (auto const pRemapColour = pTechno->GetRemapColour())
							pDrawer = pRemapColour;
					}

					scratch.Surface->DrawSHP(pDrawer, pShape, frame, &drawPos, &scratch.Rect,
						BlitterFlags::Flat | BlitterFlags::Alpha | BlitterFlags::Centered,
						0, -2, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
				}
			}
		};

		drawExtraFireMounts(pThis, pType, bodyPos);

		// Buildings keep their radar dish, SAM launcher, factory doors and
		// similar structure parts in separate animation slots or SHP fields.
		// Those frames are not part of the body SHP, so the portrait has to
		// composite them explicitly.
		auto drawBuildingAnims = [&]()
		{
			if (pType->WhatAmI() != AbstractType::BuildingType || !pThis)
				return;

			auto const pBldType = static_cast<BuildingTypeClass*>(pType);
			auto const pBuilding = static_cast<BuildingClass*>(pThis);

			struct Part
			{
				SHPStruct* Shape = nullptr;
				ConvertClass* Drawer = nullptr;
				Point2D Position {};
				int ZAdjust = 0;
				int Frame = 0;
			};

			std::vector<Part> parts;
			parts.reserve(24);
			const bool logThisType = ShouldLogBuildingAnims(pType);

			for (int slotIndex = static_cast<int>(BuildingAnimSlot::Upgrade1);
				slotIndex <= static_cast<int>(BuildingAnimSlot::SuperLowPower); ++slotIndex)
			{
				auto const slot = static_cast<BuildingAnimSlot>(slotIndex);

				// The PreProduction/Production slots are transient factory
				// animations (unit rolling out / helicopter taking off). They
				// are not part of the idle look of the building and must never
				// be shown in the portrait, even when the live anim exists.
				if (slot == BuildingAnimSlot::PreProduction || slot == BuildingAnimSlot::Production)
					continue;

				auto const& animInfo = pBldType->GetBuildingAnim(slot);
				AnimClass* pAnim = pBuilding->GetAnim(slot);

				// Slots outside the always-visible Active..Turret range are
				// only drawn while their animation actually exists; otherwise
				// the portrait would show upgrade/power/superweapon art that
				// is not part of the finished building.
				const bool liveOnly = slot < BuildingAnimSlot::Active
					|| slot > BuildingAnimSlot::Turret;

				SHPStruct* pShape = nullptr;
				ConvertClass* pDrawer = pPalette;
				Point2D animPos = bodyPos;
				int zAdjust = 0;
				int frame = 0;
				const char* animId = animInfo.Anim;

				if (pAnim && pAnim->Type && !pAnim->Type->Voxel)
				{
					// The engine positions and animates building turrets and
					// active anims through the live AnimClass. Using the live
					// instance gives us its exact screen position, turret
					// facing frame and palette.
					animId = pAnim->Type->get_ID();
					pShape = pAnim->Type->GetImage();

					if (pShape && pShape->IsReference())
						pShape->Load();

					if (!pShape || pShape->Frames <= 0)
						continue;

					if (pAnim->LightConvert)
						pDrawer = pAnim->LightConvert;

					const Point2D buildingScreen = TacticalClass::CoordsToScreen(
						pThis->GetRenderCoords());
					const Point2D animScreen = TacticalClass::CoordsToScreen(
						pAnim->GetRenderCoords());

					animPos.X += animScreen.X - buildingScreen.X;
					animPos.Y += animScreen.Y - buildingScreen.Y;
					zAdjust = pAnim->ZAdjust;
					frame = pAnim->Animation.Value;
				}
				else
				{
					// Fallback for buildings whose animation is currently not
					// running (no power, not spawned yet, ...). The type data
					// still places the normal animation correctly.
					if (liveOnly || !animInfo.Anim[0])
						continue;

					auto const pAnimType = FindAnimType(animInfo.Anim);

					if (!pAnimType || pAnimType->Voxel)
						continue;

					pShape = pAnimType->GetImage();

					if (pShape && pShape->IsReference())
						pShape->Load();

					if (!pShape || pShape->Frames <= 0)
						continue;

					animPos.X += animInfo.Position.X;
					animPos.Y += animInfo.Position.Y;
					zAdjust = animInfo.ZAdjust;
					frame = pAnimType->Start;
				}

				frame = std::clamp(frame, 0, static_cast<int>(pShape->Frames) - 1);

				if (logThisType)
				{
					Debug::Log("[UnitPortrait] BldAnim '%s' slot=%d id='%s' frames=%d frame=%d pos=%d,%d z=%d\n",
						pType->ID, slotIndex, animId, pShape->Frames, frame,
						animPos.X, animPos.Y, zAdjust);
				}

				parts.push_back(Part { pShape, pDrawer, animPos, zAdjust, frame });
			}

			// The engine also stores factory door/roof art in dedicated SHP
			// fields, but those are the *deploying* (open/roof retracting)
			// frames: on the war factory they show the production exit frame
			// (helicopter taking off). The closed door is already part of the
			// body / UnderDoorAnim, so they are deliberately not composited.

			std::stable_sort(parts.begin(), parts.end(),
				[](Part const& a, Part const& b) { return a.ZAdjust < b.ZAdjust; });

			for (auto const& part : parts)
			{
				scratch.Surface->DrawSHP(part.Drawer, part.Shape, part.Frame,
					&part.Position, &scratch.Rect,
					chosenFlags | BlitterFlags::Centered,
					0, part.ZAdjust, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
			}
		};

		drawBuildingAnims();

		PortraitImage body = scratch.Capture();

		const bool translucentBlit = chosenFlags == BlitterFlags::TransLucent25
			|| chosenFlags == BlitterFlags::TransLucent50
			|| chosenFlags == BlitterFlags::TransLucent75;

		if (translucentBlit)
		{
			RemoveKeyBlendSpecks(shadow, scratch.Key);
			RemoveKeyBlendSpecks(body, scratch.Key);
		}

		scratch.End();

		RectangleStruct bodyBounds {};

		if (!FindContentBounds(body, bodyBounds))
		{
			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] SHP frame body=%d turret=%d rendered empty for type '%s'\n",
					frameInfo.BodyFrame, frameInfo.TurretFrame, pType->ID);

			return {};
		}

		RectangleStruct shadowBounds {};
		const bool hasShadow = FindContentBounds(shadow, shadowBounds);

		const RectangleStruct sourceBounds = hasShadow
			? UnionBounds(bodyBounds, shadowBounds)
			: bodyBounds;

		const BYTE shadowAlpha = static_cast<BYTE>(std::clamp(
			Phobos::Config::UnitStatusPanel_ShadowOpacity, 0, 100) * 255 / 100);

		PortraitImage output = ScaleGrayscale(body, bodyBounds, sourceBounds,
			targetWidth, targetHeight,
			pType->WhatAmI() != AbstractType::InfantryType,
			hasShadow ? &shadow : nullptr, shadowAlpha);

		if (output.Empty())
			return {};

		if (ShouldLogModel(pType))
		{
			Debug::Log("[UnitPortrait] SHP type '%s' body=%d turret=%d shadow=%d flags=%s coverage=%d/%d/%d/%d turretMode=%d turretDelta=%d,%d out=%dx%d content=%d,%d %dx%d\n",
				pType->ID, frameInfo.BodyFrame, frameInfo.TurretFrame,
				shadowBody >= 0 ? shadowBody : -1, chosenName,
				bestCount, bf400Count, flatOnlyCount, flatAlphaCount,
				turretOffsetMode, turretDelta.X, turretDelta.Y,
				output.Width, output.Height,
				bodyBounds.X, bodyBounds.Y, bodyBounds.Width, bodyBounds.Height);
		}

		return output;
	}

	// ---------------------------------------------------------------------
	// Voxel cache
	// ---------------------------------------------------------------------

	VoxelIndexKey MakeMainKey(int facing)
	{
		VoxelIndexKey key(0);
		key.MainVoxel.FrameIndex = 0;
		key.MainVoxel.BodyFacing = static_cast<unsigned>(facing);
		key.MainVoxel.RampType = 0;
		key.MainVoxel.AuxVoxel = 0;
		return key;
	}

	VoxelIndexKey MakeAircraftKey(int facing)
	{
		VoxelIndexKey key(0);
		key.AircraftVoxel.BodyFacing = static_cast<unsigned>(facing);
		key.AircraftVoxel.RampType = 0;
		key.AircraftVoxel.FrameIndex = 0;
		return key;
	}

	VoxelIndexKey MakeFlyKey(int facing)
	{
		VoxelIndexKey key(0);
		key.FlyVoxel.FrameIndex = 0;
		key.FlyVoxel.RollRight = 0;
		key.FlyVoxel.RollLeft = 0;
		key.FlyVoxel.PitchState = 0;
		key.FlyVoxel.BodyFacing = static_cast<unsigned>(facing);
		return key;
	}

	VoxelIndexKey MakeRocketKey(int facing)
	{
		VoxelIndexKey key(0);
		key.RocketVoxel.FrameIndex = 0;
		key.RocketVoxel.BodyFacing = static_cast<unsigned>(facing);
		key.RocketVoxel.InitialPitch = 0;
		key.RocketVoxel.FinalPitch = 0;
		return key;
	}

	VoxelIndexKey MakeUndergroundKey(int facing)
	{
		VoxelIndexKey key(0);
		key.UndergroundVoxel.FrameIndex = 0;
		key.UndergroundVoxel.DigState = 0;
		key.UndergroundVoxel.BodyFacing = static_cast<unsigned>(facing);
		return key;
	}

	VoxelIndexKey MakeTurretKey(int bodyFacing, int turretFacing)
	{
		VoxelIndexKey key(0);
		key.MinorVoxel.TurretFacing = static_cast<unsigned>(turretFacing);
		key.MinorVoxel.BodyFacing = static_cast<unsigned>(bodyFacing);
		key.MinorVoxel.RampType = 0;
		key.MinorVoxel.TurretFrameIndex = 0;
		key.MinorVoxel.TurretWeaponIndex = 0;
		return key;
	}

	template <typename TCache>
	VoxelCacheStruct* FetchExact(TCache& cache, const VoxelIndexKey& key)
	{
		if (!cache.IsPresent(key))
			return nullptr;

		VoxelCacheStruct* pEntry = cache.FetchIndex(key);

		if (!pEntry || !pEntry->Buffer || pEntry->Width == 0 || pEntry->Height == 0)
			return nullptr;

		return pEntry;
	}

	template <typename TCache>
	VoxelCacheStruct* SelectVoxelEntry(TCache& cache, bool aircraft, bool turretPart, VoxelIndexKey* pOutKey = nullptr)
	{
		VoxelCacheStruct* best = nullptr;
		int bestScore = std::numeric_limits<int>::max();
		int bestFacing = -1;
		int bestFrame = -1;

		for (auto const& node : cache)
		{
			VoxelCacheStruct* pEntry = node.Data;

			if (!pEntry || !pEntry->Buffer || pEntry->Width == 0 || pEntry->Height == 0)
				continue;

			int facing = 0;
			int frame = 0;
			int ramp = 0;
			int aux = 0;
			int weapon = 0;
			int bodyFacing = SoutheastFacing;

			if (turretPart)
			{
				facing = static_cast<int>(node.ID.MinorVoxel.TurretFacing);
				frame = static_cast<int>(node.ID.MinorVoxel.TurretFrameIndex);
				ramp = static_cast<int>(node.ID.MinorVoxel.RampType);
				weapon = static_cast<int>(node.ID.MinorVoxel.TurretWeaponIndex);
				bodyFacing = static_cast<int>(node.ID.MinorVoxel.BodyFacing);
			}
			else if (aircraft)
			{
				facing = static_cast<int>(node.ID.AircraftVoxel.BodyFacing);
				frame = static_cast<int>(node.ID.AircraftVoxel.FrameIndex);
				ramp = static_cast<int>(node.ID.AircraftVoxel.RampType);
			}
			else
			{
				facing = static_cast<int>(node.ID.MainVoxel.BodyFacing);
				frame = static_cast<int>(node.ID.MainVoxel.FrameIndex);
				ramp = static_cast<int>(node.ID.MainVoxel.RampType);
				aux = static_cast<int>(node.ID.MainVoxel.AuxVoxel);
			}

			const int score = FacingDistance(facing, SoutheastFacing) * 100000
				+ FacingDistance(bodyFacing, SoutheastFacing) * 10000
				+ frame * 1000
				+ ramp * 100
				+ aux * 10
				+ weapon;

			if (score < bestScore)
			{
				bestScore = score;
				best = pEntry;
				bestFacing = facing;
				bestFrame = frame;

				if (pOutKey)
					*pOutKey = node.ID;
			}
		}

		if (best && (bestFacing != SoutheastFacing || bestFrame != 0))
		{
			Debug::Log("[UnitPortrait] Closest voxel %s facing=%d frame=%d (wanted %d/0)\n",
				turretPart ? "turret" : "body", bestFacing, bestFrame, SoutheastFacing);
		}

		return best;
	}

	VoxelCacheStruct* FindVoxelBody(TechnoTypeClass* pType, bool aircraft, VoxelIndexKey* pOutKey = nullptr)
	{
		auto& cache = pType->VoxelMainCache;

		auto tryKey = [&](VoxelIndexKey key) -> VoxelCacheStruct*
		{
			auto* pEntry = FetchExact(cache, key);

			if (pEntry && pOutKey)
				*pOutKey = key;

			return pEntry;
		};

		if (aircraft)
		{
			if (auto* pEntry = tryKey(MakeAircraftKey(SoutheastFacing)))
				return pEntry;
		}

		if (auto* pEntry = tryKey(MakeMainKey(SoutheastFacing)))
			return pEntry;

		if (auto* pEntry = tryKey(MakeFlyKey(SoutheastFacing)))
			return pEntry;

		if (auto* pEntry = tryKey(MakeRocketKey(SoutheastFacing)))
			return pEntry;

		if (auto* pEntry = tryKey(MakeUndergroundKey(SoutheastFacing)))
			return pEntry;

		return SelectVoxelEntry(cache, aircraft, false, pOutKey);
	}

	template <typename TCache>
	VoxelCacheStruct* FindVoxelTurret(TCache& cache, VoxelIndexKey* pOutKey = nullptr)
	{
		for (const int bodyFacing : { SoutheastFacing, 0 })
		{
			const VoxelIndexKey key = MakeTurretKey(bodyFacing, SoutheastFacing);

			if (auto* pEntry = FetchExact(cache, key))
			{
				if (pOutKey)
					*pOutKey = key;

				return pEntry;
			}
		}

		return SelectVoxelEntry(cache, false, true, pOutKey);
	}

	COLORREF DecodeVoxelColor(unsigned short value)
	{
		int red = 0;
		int green = 0;
		int blue = 0;

		if (Drawing::ColorMode == RGBMode::RGB555 || Drawing::ColorMode == RGBMode::RGB556)
		{
			red = ((value >> 10) & 0x1F) * 255 / 31;
			green = ((value >> 5) & 0x1F) * 255 / 31;
			blue = (value & 0x1F) * 255 / 31;
		}
		else
		{
			red = ((value >> 11) & 0x1F) * 255 / 31;
			green = ((value >> 5) & 0x3F) * 255 / 63;
			blue = (value & 0x1F) * 255 / 31;
		}

		return Drawing::RGB_To_Int(red, green, blue);
	}

	// Renders the model into a private cache with a fixed south-east matrix.
	// The private cache and the fixed matrix mean this never touches the live
	// unit (no facing changes) and never writes into the type's tactical cache.
	// The cache entries are intentionally leaked: the engine may keep a
	// reference to the last entry, and freeing it caused a delayed crash.
	PortraitImage RenderVoxelNative(TechnoClass* pThis, TechnoTypeClass* pType,
		int targetWidth, int targetHeight)
	{
		if (!pThis || !pType || !pType->Voxel)
			return {};

		auto const pFoot = abstract_cast<FootClass*>(pThis);

		if (!pFoot)
			return {};

		const int scratchWidth = 256;
		const int scratchHeight = 256;
		BSurface* pTarget = GameCreate<BSurface>(scratchWidth, scratchHeight);

		if (!pTarget)
			return {};

		RectangleStruct rect { 0, 0, scratchWidth, scratchHeight };
		Point2D bodyPos { scratchWidth / 2, scratchHeight / 2 };
		pTarget->FillRect(&rect, Drawing::RGB_To_Int(255, 0, 255));

		Point2D keyPoint { 0, 0 };
		const COLORREF key = pTarget->GetPixel(&keyPoint);

		// Draw into our own 16-bit BSurface rather than the 1752x1080 screen.
		// Draw_A_VXL uses the current surface for its cache blit; using the
		// screen made the blitter run past the screen buffer (crash 0x496888).
		DSurface* pOldTemp = DSurface::Temp;
		DSurface::Temp = reinterpret_cast<DSurface*>(pTarget);

		// The engine's facing convention needs one 8th less than the raw SE
		// index here; 12 produced a south-facing model, 8 produces SE.
		Matrix3D facingMtx = Matrix3D::GetIdentity();
		facingMtx.RotateZ(static_cast<float>(DirStruct(8 * 2048).GetRadian<32>()));

		Matrix3D bodyMtx = Matrix3D::VoxelDefaultMatrix * facingMtx;
		constexpr BlitterFlags blit = BlitterFlags::Alpha | BlitterFlags::Flat;
		const bool aircraft = pType->WhatAmI() == AbstractType::AircraftType;

		IndexClass<VoxelIndexKey, VoxelCacheStruct*> bodyCache;
		const int bodyKey = aircraft
			? MakeAircraftKey(SoutheastFacing).Value
			: MakeMainKey(SoutheastFacing).Value;

		pFoot->Draw_A_VXL(&pType->MainVoxel, 0, bodyKey, &bodyCache,
			&rect, &bodyPos, &bodyMtx, 1000, blit, 0);

		if (pType->Turret)
		{
			IndexClass<VoxelIndexKey, VoxelCacheStruct*> turretCache;
			IndexClass<VoxelIndexKey, VoxelCacheStruct*> barrelCache;
			const int turretKey = MakeTurretKey(SoutheastFacing, SoutheastFacing).Value;

			pFoot->Draw_A_VXL(&pType->TurretVoxel, 0, turretKey, &turretCache,
				&rect, &bodyPos, &bodyMtx, 1000, blit, 0);

			if (pType->BarrelVoxel.VXL)
			{
				pFoot->Draw_A_VXL(&pType->BarrelVoxel, 0, turretKey, &barrelCache,
					&rect, &bodyPos, &bodyMtx, 1000, blit, 0);
			}
		}

		DSurface::Temp = pOldTemp;

		PortraitImage body;
		body.Width = scratchWidth;
		body.Height = scratchHeight;
		body.Pixels.assign(static_cast<size_t>(scratchWidth) * scratchHeight, 0);
		body.Mask.assign(static_cast<size_t>(scratchWidth) * scratchHeight, 0);

		for (int y = 0; y < scratchHeight; ++y)
		{
			for (int x = 0; x < scratchWidth; ++x)
			{
				Point2D point { x, y };
				const COLORREF color = pTarget->GetPixel(&point);
				const size_t index = static_cast<size_t>(y) * scratchWidth + x;
				body.Pixels[index] = color;
				body.Mask[index] = color != key ? 1 : 0;
			}
		}

		GameDelete(pTarget);

		RemoveKeyBlendSpecks(body, key);

		RectangleStruct bodyBounds {};

		if (!FindContentBounds(body, bodyBounds))
			return {};

		return ScaleGrayscale(body, bodyBounds, bodyBounds,
			targetWidth, targetHeight, true, nullptr, 0, 78);
	}

	PortraitImage RenderVoxel(TechnoClass* pThis, TechnoTypeClass* pType,
		int targetWidth, int targetHeight)
	{
		const bool aircraft = pType->WhatAmI() == AbstractType::AircraftType;
		const bool logDiag = ShouldLogVoxelDiag(pType);

		if (Phobos::Config::UnitStatusPanel_VoxelNativeDraw)
		{
			PortraitImage native = RenderVoxelNative(pThis, pType, targetWidth, targetHeight);

			if (logDiag)
			{
				char path[96];
				std::snprintf(path, sizeof(path), "debug/portrait_%s_native.bmp", pType->ID);
				SavePortraitBmp(native, path);
				Debug::Log("[UnitPortrait] Voxel native type='%s' ok=%d out=%dx%d\n",
					pType->ID, native.Empty() ? 0 : 1, native.Width, native.Height);
			}

			if (!native.Empty())
				return native;
		}

		auto* pBody = FindVoxelBody(pType, aircraft);

		if (!pBody)
		{
			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] No body voxel cache for type '%s' (entries=%d)\n",
					pType->ID, pType->VoxelMainCache.Count());

			return {};
		}

		std::vector<VoxelPart> parts;
		parts.push_back(VoxelPart { pBody, 0, 0 });

		if (pType->Turret)
		{
			if (auto* pTurret = FindVoxelTurret(pType->VoxelTurretWeaponCache))
				parts.push_back(VoxelPart { pTurret, 0, 0 });

			if (auto* pBarrel = FindVoxelTurret(pType->VoxelTurretBarrelCache))
				parts.push_back(VoxelPart { pBarrel, 0, 0 });
		}

		int minX = std::numeric_limits<int>::max();
		int minY = std::numeric_limits<int>::max();
		int maxX = std::numeric_limits<int>::min();
		int maxY = std::numeric_limits<int>::min();

		for (auto& part : parts)
		{
			minX = std::min(minX, static_cast<int>(part.Entry->X));
			minY = std::min(minY, static_cast<int>(part.Entry->Y));
			maxX = std::max(maxX, static_cast<int>(part.Entry->X) + part.Entry->Width);
			maxY = std::max(maxY, static_cast<int>(part.Entry->Y) + part.Entry->Height);
		}

		const int width = maxX - minX;
		const int height = maxY - minY;

		if (width <= 0 || height <= 0)
			return {};

		PortraitImage canvas;
		canvas.Width = width;
		canvas.Height = height;
		canvas.Pixels.assign(static_cast<size_t>(width) * height, 0);
		canvas.Mask.assign(static_cast<size_t>(width) * height, 0);

		auto const pConvert = pThis->GetDrawer() ? pThis->GetDrawer() : pThis->GetRemapColour();
		auto const pPaletteEntries = pConvert ? pConvert->UsedPalette1 : nullptr;

		for (auto& part : parts)
		{
			const int partWidth = part.Entry->Width;
			const int partHeight = part.Entry->Height;
			const int offsetX = static_cast<int>(part.Entry->X) - minX;
			const int offsetY = static_cast<int>(part.Entry->Y) - minY;

			if (!part.Entry->Buffer)
				continue;

			// The voxel cache does *not* store 16-bit RGB pixels. The engine
			// wraps this buffer in a 1-byte-per-pixel BSurface and blits it
			// through a ConvertClass (see Draw_A_VXL at 0x707480). Reading it
			// as 16-bit produced the noisy portraits; use the 8-bit indices
			// and the unit's palette instead.
			auto const first = *reinterpret_cast<const uintptr_t*>(part.Entry->Buffer);
			Surface* pLockedSurface = nullptr;
			const unsigned char* pBase = nullptr;
			int stride = partWidth;
			int bytesPerPixel = 1;

			if (first == 0x7E2070)
			{
				pLockedSurface = static_cast<Surface*>(part.Entry->Buffer);
				pBase = static_cast<const unsigned char*>(pLockedSurface->Lock(0, 0));
				stride = pLockedSurface->GetPitch();
				bytesPerPixel = pLockedSurface->GetBytesPerPixel();
			}
			else
			{
				pBase = static_cast<const unsigned char*>(part.Entry->Buffer);
			}

			if (pBase)
			{
				for (int y = 0; y < partHeight; ++y)
				{
					auto const* row = pBase + static_cast<size_t>(y) * static_cast<size_t>(stride);

					for (int x = 0; x < partWidth; ++x)
					{
						COLORREF color = 0;

						if (bytesPerPixel <= 1)
						{
							const unsigned char index = row[x];

							if (index == 0)
								continue;

							if (pConvert && pConvert->PaletteData)
							{
								if (pConvert->BytesPerPixel == 2)
								{
									const unsigned short value =
										reinterpret_cast<const unsigned short*>(pConvert->PaletteData)[index];
									color = DecodeVoxelColor(value);
								}
								else
								{
									const unsigned char value =
										reinterpret_cast<const unsigned char*>(pConvert->PaletteData)[index];
									color = Drawing::RGB_To_Int(value, value, value);
								}
							}
							else if (pPaletteEntries)
							{
								auto const& rgb = pPaletteEntries[index];
								color = Drawing::RGB_To_Int(rgb.Red, rgb.Green, rgb.Blue);
							}
							else
							{
								color = Drawing::RGB_To_Int(index, index, index);
							}
						}
						else
						{
							const unsigned short value =
								reinterpret_cast<const unsigned short*>(row)[x];

							if (value == 0)
								continue;

							color = DecodeVoxelColor(value);
						}

						const size_t index = static_cast<size_t>(offsetY + y) * width + offsetX + x;
						canvas.Pixels[index] = color;
						canvas.Mask[index] = 1;
					}
				}
			}

			if (pLockedSurface)
				pLockedSurface->Unlock();
		}

		RectangleStruct content {};

		if (!FindContentBounds(canvas, content))
		{
			if (ShouldLogFailure(pType))
				Debug::Log("[UnitPortrait] Voxel composite empty for type '%s'\n", pType->ID);

			return {};
		}

		PortraitImage output = ScaleGrayscale(canvas, content, content,
			targetWidth, targetHeight,
			pType->WhatAmI() != AbstractType::InfantryType,
			nullptr, 0, 78);

		if (output.Empty())
			return {};

		if (logDiag)
		{
			char path[96];
			std::snprintf(path, sizeof(path), "debug/portrait_%s_cache.bmp", pType->ID);
			SavePortraitBmp(output, path);
			Debug::Log("[UnitPortrait] Voxel cache type='%s' body=%p buffer=%p first=%08X %dx%d parts=%d composite=%dx%d out=%dx%d\n",
				pType->ID, pBody, pBody->Buffer,
				pBody->Buffer ? *reinterpret_cast<const unsigned*>(pBody->Buffer) : 0,
				pBody->Width, pBody->Height, static_cast<int>(parts.size()),
				width, height, output.Width, output.Height);
		}

		if (ShouldLogModel(pType))
		{
			Debug::Log("[UnitPortrait] Voxel type '%s' body=%dx%d parts=%d composite=%dx%d content=%d,%d %dx%d out=%dx%d\n",
				pType->ID, pBody->Width, pBody->Height, static_cast<int>(parts.size()),
				width, height, content.X, content.Y, content.Width, content.Height,
				output.Width, output.Height);
		}

		return output;
	}
}

const PortraitImage* UnitPortrait::TryGetCached(TechnoClass* pThis, int width, int height)
{
	if (!pThis || width <= 0 || height <= 0)
		return nullptr;

	auto* pType = pThis->GetTechnoType();

	if (!pType)
		return nullptr;

	const CacheKey key { pType, pThis->Veterancy.IsElite(), width, height };
	auto it = Cache.find(key);

	if (it == Cache.end() || it->second.Width != width || it->second.Height != height)
		return nullptr;

	return &it->second;
}

const PortraitImage* UnitPortrait::Get(TechnoClass* pThis, int width, int height)
{
	if (!pThis || width <= 0 || height <= 0)
		return nullptr;

	auto* pType = pThis->GetTechnoType();

	if (!pType)
		return nullptr;

	const CacheKey key { pType, pThis->Veterancy.IsElite(), width, height };

	if (LoggedRequests.insert(pType).second)
	{
		Debug::Log("[UnitPortrait] Request type='%s' elite=%d voxel=%d image=%p mainCache=%d turretCache=%d barrelCache=%d turret=%d\n",
			pType->ID, key.Elite ? 1 : 0, pType->Voxel ? 1 : 0, pType->GetImage(),
			pType->VoxelMainCache.Count(), pType->VoxelTurretWeaponCache.Count(),
			pType->VoxelTurretBarrelCache.Count(), pType->Turret ? 1 : 0);
	}

	const DWORD now = SystemTimer::GetTime();

	auto it = Cache.find(key);

	if (it != Cache.end())
	{
		if (it->second.Width == width && it->second.Height == height)
			return &it->second;

		Cache.erase(it);
	}

	auto retry = RetryAt.find(key);

	if (retry != RetryAt.end() && now < retry->second)
		return nullptr;

	PortraitImage image = pType->Voxel
		? RenderVoxel(pThis, pType, width, height)
		: RenderSHP(pThis, pType, width, height);

	if (image.Empty())
	{
		RetryAt[key] = now + RetryDelayMs;
		return nullptr;
	}

	RetryAt.erase(key);
	auto [inserted, ok] = Cache.emplace(key, std::move(image));
	return &inserted->second;
}

void UnitPortrait::Clear()
{
	Cache.clear();
	RetryAt.clear();
	LoggedRequests.clear();
	LoggedFailures.clear();
	LoggedModels.clear();
	LoggedBuildingAnims.clear();
	LoggedVoxelDiag.clear();
}
