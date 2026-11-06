#include "FoggedVoxel.h"
#include "FoggedVoxelData.h"

#include <BuildingClass.h>
#include <FootClass.h>
#include <ColorScheme.h>
#include <ConvertClass.h>
#include <ScenarioClass.h>
#include <TacticalClass.h>
#include <Utilities/Stream.h>
#include <Utilities/Swizzle.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <unordered_map>

namespace
{
	using FoggedVoxelData::Image;
	struct CapturedPart { const void* Cache; Image Picture; };
	struct VisibleImage
	{
		int Frame = -1;
		std::vector<CapturedPart> Parts;
		unsigned Palettes = 0, PaletteMisses = 0, Captures = 0, PlainCalls = 0, Rejected = 0;
	};
	struct FrozenImage
	{
		CoordStruct Position;
		int TypeIndex = -1;
		std::vector<Image> Parts;
		FoggedObjectClass* Owner = nullptr;
		bool LegacyUnbound = false;
		bool DrawReported = false;
	};
	// Ownership is per native snapshot, not per cell. Invisible snapshots and
	// attached buildings can share coordinates with a visible frozen building.
	// Only the native owner pointer is saved and swizzled; pixels/palette data
	// are independent of live buildings and renderer cache/converter pointers.
	std::unordered_map<BuildingClass*, VisibleImage> Visible;
	std::vector<FrozenImage> Frozen;
	BuildingClass* Preparing = nullptr;
	TechnoClass* PaletteObject = nullptr;
	Image PaletteState;
	unsigned FreezeMessages = 0;
	unsigned DrawMessages = 0;

	struct SnapshotFields
	{
		char Padding[0x30];
		int Kind;
		CoordStruct Position;
		RectangleStruct Bounds;
		char Padding50[0x24];
		bool Visible;
		char Padding75[3];
	};
	static_assert(sizeof(SnapshotFields) == 0x78);
	static_assert(sizeof(SHPFile) == 32);

	bool IsVoxel(const BuildingClass* building)
	{
		return building && building->Type
			&& (building->Type->TurretAnimIsVoxel || building->Type->BarrelAnimIsVoxel);
	}

	auto Find(FoggedObjectClass* snapshot)
	{
		auto found = std::find_if(Frozen.begin(), Frozen.end(),
			[&](const auto& item) { return item.Owner == snapshot; });
		if (found != Frozen.end() || !snapshot)
			return found;
		// FVX1 saves did not record ownership. Bind those once, using the first
		// native draw record's BuildingType as well as position and visibility.
		const auto fields = reinterpret_cast<const SnapshotFields*>(snapshot);
		if (fields->Kind != 6 || !fields->Visible
			|| std::none_of(Frozen.begin(), Frozen.end(), [](const auto& image) { return image.LegacyUnbound; }))
			return Frozen.end();
		const auto bytes = reinterpret_cast<const unsigned char*>(snapshot);
		const auto records = *reinterpret_cast<void* const* const*>(bytes + 0x60);
		const int count = *reinterpret_cast<const int*>(bytes + 0x6C);
		if (!records || count <= 0)
			return Frozen.end();
		int typeIndex = -1;
		for (int i = 0; i < BuildingTypeClass::Array.Count; ++i)
			if (BuildingTypeClass::Array.Items[i] == records[0]) { typeIndex = i; break; }
		if (typeIndex < 0)
			return Frozen.end();
		found = std::find_if(Frozen.begin(), Frozen.end(), [&](const auto& image)
		{
			return image.LegacyUnbound && image.TypeIndex == typeIndex && image.Position == fields->Position;
		});
		if (found != Frozen.end())
		{
			found->Owner = snapshot;
			found->LegacyUnbound = false;
		}
		return found;
	}

	Point2D Anchor(const CoordStruct& position)
	{
		auto point = TacticalClass::Instance->CoordsToClient(position).first;
		point.X += DSurface::ViewBounds.X;
		point.Y += DSurface::ViewBounds.Y;
		return point;
	}
}

void FoggedVoxel::Prepare(BuildingClass* building)
{
	if (Preparing || !building || building->IsFogged
		|| !ScenarioClass::Instance->SpecialFlags.FogOfWar
		|| !(building->Type->TurretAnimIsVoxel || building->Type->BarrelAnimIsVoxel))
		return;
	Visible[building] = VisibleImage {};
	Visible[building].Frame = Unsorted::CurrentFrame;
	Preparing = building;
	// Draw the native building-overlay pass while it is still visible, before
	// FreezeInFog changes IsFogged. An empty clip generates voxel images without
	// writing any destination pixels. This also handles buildings off camera and
	// uncached/recoiling barrels. No gameplay object or hidden-building proxy.
	const auto point = Anchor(building->GetRenderCoords());
	const RectangleStruct empty { 0, 0, 0, 0 };
	building->Draw(point, empty);
	Preparing = nullptr;
	PaletteObject = nullptr;
}

void FoggedVoxel::SelectPalette(TechnoClass* object, ConvertClass* palette, unsigned flags)
{
	PaletteObject = nullptr;
	if (object != Preparing || (flags & unsigned(BlitterFlags::Darken))
		|| !ScenarioClass::Instance->SpecialFlags.FogOfWar || !object
		|| object->WhatAmI() != AbstractType::Building || !palette)
		return;
	const auto building = static_cast<BuildingClass*>(object);
	if (building->IsFogged || !(building->Type->TurretAnimIsVoxel || building->Type->BarrelAnimIsVoxel))
		return;
	++Visible[building].Palettes;
	for (int family = 0; family < 2; ++family)
	{
		const auto schemes = family ? building->Type->Palette : &ColorScheme::Array;
		if (!schemes)
			continue;
		for (int i = 0; i < schemes->Count; ++i)
		{
			if (schemes->Items[i] && schemes->Items[i]->LightConvert == palette)
			{
				PaletteObject = object;
				PaletteState.PaletteIndex = i;
				PaletteState.CustomPalette = family != 0;
				// Match the native voxel blitter's remap mask. A frozen image
				// uses a captured top-left offset rather than SHP centering.
				PaletteState.Flags = flags & ~unsigned(BlitterFlags::Centered | BlitterFlags::ZRemap);
				return;
			}
		}
	}
	++Visible[building].PaletteMisses;
}

void FoggedVoxel::Capture(TechnoClass* object, const VoxelCacheStruct* cache,
	const RectangleStruct* clip, const unsigned* arguments)
{
	if (!object || object != Preparing || !cache || !clip || !arguments)
		return;
	auto building = static_cast<BuildingClass*>(object);
	++Visible[building].Captures;
	if (PaletteObject != object)
		return;
	if (building->IsFogged)
		return;
	const auto destination = reinterpret_cast<const RectangleStruct*>(arguments[0]);
	if (!destination)
		return;
	Image image = PaletteState;
	if (!image.Capture(cache->Buffer, cache->Width, cache->Height))
	{
		++Visible[building].Rejected;
		return;
	}
	const auto anchor = Anchor(building->Location);
	image.X = destination->X + clip->X - anchor.X;
	image.Y = destination->Y + clip->Y - anchor.Y;
	image.ZAdjust = static_cast<int>(arguments[5]);
	image.Gradient = static_cast<int>(arguments[6]);
	image.Intensity = static_cast<int>(arguments[7]);
	image.Tint = static_cast<int>(arguments[12]);
	if (!image.Valid())
	{
		++Visible[building].Rejected;
		return;
	}
	auto& visible = Visible[building];
	if (visible.Frame != Unsorted::CurrentFrame)
	{
		visible.Frame = Unsorted::CurrentFrame;
		visible.Parts.clear();
	}
	// Native dirty-rectangle redraws can blit the same component repeatedly.
	// Preserve the first draw order (barrel behind/in front of turret) once.
	for (auto& part : visible.Parts)
	{
		if (part.Cache == cache && part.Picture.X == image.X && part.Picture.Y == image.Y
			&& part.Picture.Flags == image.Flags && part.Picture.Shape == image.Shape)
		{
			part.Picture = std::move(image);
			return;
		}
	}
	if (visible.Parts.size() < FoggedVoxelData::MaxParts)
		visible.Parts.push_back({ cache, std::move(image) });
}

void FoggedVoxel::CapturePlain(TechnoClass* object, ConvertClass* palette, unsigned flags,
	const unsigned* arguments)
{
	if (object && object == Preparing)
		++Visible[static_cast<BuildingClass*>(object)].PlainCalls;
	SelectPalette(object, palette, flags);
	if (PaletteObject != object || !object || !arguments)
		return;
	const auto source = reinterpret_cast<Surface*>(arguments[0]);
	const auto rect = reinterpret_cast<const RectangleStruct*>(arguments[1]);
	const auto point = reinterpret_cast<const Point2D*>(arguments[2]);
	const auto clip = reinterpret_cast<const RectangleStruct*>(arguments[3]);
	if (!source || !rect || !point || !clip || source->GetBytesPerPixel() != 1
		|| rect->X < 0 || rect->Y < 0 || rect->Width <= 0 || rect->Height <= 0
		|| rect->Width > FoggedVoxelData::MaxDimension || rect->Height > FoggedVoxelData::MaxDimension
		|| rect->X > source->Width - rect->Width || rect->Y > source->Height - rect->Height)
		return;
	const int pitch = source->GetPitch();
	const auto pixels = static_cast<const unsigned char*>(source->Lock(rect->X, rect->Y));
	if (!pixels)
		return;
	Image image;
	const bool copied = image.CapturePlain(pixels, pitch, rect->Width, rect->Height);
	source->Unlock();
	if (!copied)
		return;
	// Feed the owned RLE result through the same metadata/order path as cache hits.
	const VoxelCacheStruct cache { 0, 0, static_cast<unsigned short>(rect->Width),
		static_cast<unsigned short>(rect->Height), image.Shape.data() + 32 };
	const RectangleStruct destination { point->X, point->Y, rect->Width, rect->Height };
	unsigned translated[13] {};
	translated[0] = reinterpret_cast<unsigned>(&destination);
	translated[5] = arguments[6]; translated[6] = arguments[7];
	translated[7] = arguments[8]; translated[12] = arguments[9];
	Capture(object, &cache, clip, translated);
}

void FoggedVoxel::Freeze(BuildingClass* building, FoggedObjectClass* snapshot)
{
	const auto fields = reinterpret_cast<SnapshotFields*>(snapshot);
	if (!snapshot || fields->Kind != 6 || !fields->Visible || !IsVoxel(building))
		return;
	Remove(snapshot);
	const auto found = Visible.find(building);
	if (found == Visible.end() || found->second.Parts.empty())
	{
		const VisibleImage empty;
		const auto& stats = found == Visible.end() ? empty : found->second;
		if (FreezeMessages++ < 16)
			Debug::Log("[FogVoxel v3] no image: id=%s type=%d position=%d,%d,%d prepared=%d palettes=%u misses=%u captures=%u plain=%u rejected=%u\n",
				building->Type->ID, building->Type->ArrayIndex, fields->Position.X, fields->Position.Y,
				fields->Position.Z, found != Visible.end(), stats.Palettes, stats.PaletteMisses,
				stats.Captures, stats.PlainCalls, stats.Rejected);
		Visible.erase(building);
		return;
	}
	FrozenImage image { fields->Position, building->Type->ArrayIndex, {}, snapshot };
	auto anchor = Anchor(image.Position);
	anchor.X -= DSurface::ViewBounds.X;
	anchor.Y -= DSurface::ViewBounds.Y;
	// At 4D10D3 the main SHP bounds are client-relative. The native constructor
	// unions animation bounds afterwards, then adds TacticalPos. Include voxels
	// now while EBX still identifies the source building (not the animation list).
	for (const auto& part : found->second.Parts)
	{
		image.Parts.push_back(part.Picture);
		const auto& p = part.Picture;
		auto& b = fields->Bounds;
		const int left = std::min(b.X, anchor.X + p.X);
		const int top = std::min(b.Y, anchor.Y + p.Y);
		const int right = std::max(b.X + b.Width, anchor.X + p.X + p.Width);
		const int bottom = std::max(b.Y + b.Height, anchor.Y + p.Y + p.Height);
		b = { left, top, right - left, bottom - top };
	}
	if (FreezeMessages++ < 16)
		Debug::Log("[FogVoxel v3] frozen: type=%d position=%d,%d,%d parts=%u\n",
			image.TypeIndex, image.Position.X, image.Position.Y, image.Position.Z,
			static_cast<unsigned>(image.Parts.size()));
	Frozen.push_back(std::move(image));
	Visible.erase(found);
}

void FoggedVoxel::Draw(FoggedObjectClass* snapshot, const RectangleStruct* bounds)
{
	if (!snapshot || !bounds || !DSurface::Temp || bounds->Width <= 0 || bounds->Height <= 0)
		return;
	const auto fields = reinterpret_cast<const SnapshotFields*>(snapshot);
	const auto found = Find(snapshot);
	if (fields->Kind != 6 || found == Frozen.end() || found->TypeIndex < 0
		|| found->TypeIndex >= BuildingTypeClass::Array.Count)
		return;
	const auto type = BuildingTypeClass::Array.Items[found->TypeIndex];
	if (!type)
		return;
	const auto anchor = Anchor(found->Position);
	unsigned submitted = 0;
	for (auto& image : found->Parts)
	{
		const auto schemes = image.CustomPalette ? type->Palette : &ColorScheme::Array;
		if (!schemes || image.PaletteIndex < 0 || image.PaletteIndex >= schemes->Count)
			continue;
		const auto scheme = schemes->Items[image.PaletteIndex];
		if (!scheme || !scheme->LightConvert)
			continue;
		// Match the native frozen-SHP path's dirty-rectangle coordinate system.
		const Point2D point { anchor.X + image.X - bounds->X, anchor.Y + image.Y - bounds->Y };
		DSurface::Temp->DrawSHP(scheme->LightConvert, reinterpret_cast<SHPStruct*>(image.Shape.data()),
			0, &point, bounds, static_cast<BlitterFlags>(image.Flags), 0, image.ZAdjust,
			static_cast<ZGradient>(image.Gradient), image.Intensity, image.Tint, nullptr, 0, 0, 0);
		++submitted;
	}
	if (!found->DrawReported)
	{
		found->DrawReported = true;
		if (DrawMessages++ < 16)
			Debug::Log("[FogVoxel v3] draw: type=%d parts=%u submitted=%u clip=%d,%d,%d,%d\n",
				found->TypeIndex, static_cast<unsigned>(found->Parts.size()), submitted,
				bounds->X, bounds->Y, bounds->Width, bounds->Height);
	}
}

void FoggedVoxel::Remove(FoggedObjectClass* snapshot)
{
	if (!snapshot)
		return;
	const auto fields = reinterpret_cast<const SnapshotFields*>(snapshot);
	if (fields->Kind == 6)
	{
		const auto found = Find(snapshot);
		if (found != Frozen.end())
			Frozen.erase(found);
	}
}

void FoggedVoxel::Remove(BuildingClass* building) { Visible.erase(building); }

void FoggedVoxel::Clear()
{
	Visible.clear();
	Preparing = nullptr;
	Frozen.clear();
	FreezeMessages = DrawMessages = 0;
	PaletteObject = nullptr;
}

bool FoggedVoxel::Save(PhobosStreamWriter& stm)
{
	stm.Save(FoggedVoxelData::SaveMagic);
	stm.Save(static_cast<unsigned>(Frozen.size()));
	for (const auto& image : Frozen)
	{
		stm.Save(image.Owner);
		stm.Save(image.Position); stm.Save(image.TypeIndex);
		stm.Save(static_cast<unsigned>(image.Parts.size()));
		for (const auto& part : image.Parts)
			part.Save(stm);
	}
	return stm.Success();
}

bool FoggedVoxel::Load(PhobosStreamReader& stm)
{
	Clear();
	// Building globals were an empty independent block in previous saves.
	if (stm.AtEndOfBlock())
		return stm.Success();
	unsigned magic = 0, count = 0;
	if (!stm.Load(magic) || (magic != FoggedVoxelData::SaveMagic && magic != FoggedVoxelData::LegacySaveMagic) || !stm.Load(count)
		|| count > FoggedVoxelData::MaxBuildings)
		return false;
	std::vector<FrozenImage> restored;
	for (unsigned i = 0; i < count; ++i)
	{
		FrozenImage image;
		unsigned parts = 0;
		if (magic == FoggedVoxelData::SaveMagic && !stm.Load(image.Owner))
			return false;
		image.LegacyUnbound = !image.Owner;
		if (!stm.Load(image.Position) || !stm.Load(image.TypeIndex) || !stm.Load(parts)
			|| image.TypeIndex < 0 || parts > FoggedVoxelData::MaxParts)
			return false;
		for (unsigned j = 0; j < parts; ++j)
		{
			Image part;
			if (!part.Load(stm))
				return false;
			image.Parts.push_back(std::move(part));
		}
		restored.push_back(std::move(image));
	}
	if (!stm.ExpectEndOfBlock())
		return false;
	Frozen = std::move(restored);
	// Native AbstractClass::Load registers old/new addresses through 410380 ->
	// 6CF2C0. Register only after all records validate and the vector is stable.
	for (auto& image : Frozen)
		if (image.Owner)
			PhobosSwizzle::RegisterPointerForChange(image.Owner);
	return true;
}
