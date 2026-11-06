#pragma once

class BuildingClass;
class TechnoClass;
class ConvertClass;
class Surface;
class FoggedObjectClass;
class PhobosStreamReader;
class PhobosStreamWriter;
struct VoxelCacheStruct;
struct RectangleStruct;

namespace FoggedVoxel
{
	void Prepare(BuildingClass* building);
	void CapturePlain(TechnoClass* object, ConvertClass* palette, unsigned flags, const unsigned* arguments);
	void SelectPalette(TechnoClass* object, ConvertClass* palette, unsigned flags);
	void Capture(TechnoClass* object, const VoxelCacheStruct* cache,
		const RectangleStruct* clip, const unsigned* arguments);
	void Freeze(BuildingClass* building, FoggedObjectClass* snapshot);
	void Draw(FoggedObjectClass* snapshot, const RectangleStruct* bounds);
	void Remove(FoggedObjectClass* snapshot);
	void Remove(BuildingClass* building);
	void Clear();
	bool Load(PhobosStreamReader& stm);
	bool Save(PhobosStreamWriter& stm);
}
