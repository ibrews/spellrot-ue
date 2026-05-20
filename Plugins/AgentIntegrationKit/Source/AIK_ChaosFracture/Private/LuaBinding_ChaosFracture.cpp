// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "FractureEngineFracturing.h"
#include "FractureEngineEdit.h"
#include "FractureEngineClustering.h"
#include "ScopedTransaction.h"
#include "Dataflow/DataflowSelection.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static UGeometryCollection* LoadGeometryCollection(const std::string& PathStr, FLuaSessionData& Session)
{
	FString AssetPath = UTF8_TO_TCHAR(PathStr.c_str());
	if (!AssetPath.StartsWith(TEXT("/")))
	{
		AssetPath = TEXT("/Game/") + AssetPath;
	}

	UGeometryCollection* GC = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
	if (!GC)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] Could not load GeometryCollection at '%s'"), *AssetPath));
	}
	return GC;
}

static FGeometryCollection* GetCollection(UGeometryCollection* GC, const char* FuncName, FLuaSessionData& Session)
{
	FGeometryCollection* Collection = GC->GetGeometryCollection().Get();
	if (!Collection)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> FGeometryCollection is null"), UTF8_TO_TCHAR(FuncName)));
	}
	return Collection;
}

static bool ValidateBounds(const FBox& Bounds, const char* FuncName, FLuaSessionData& Session)
{
	if (!Bounds.IsValid || Bounds.GetVolume() <= 0.0)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> geometry collection has no valid geometry bounds (empty or degenerate)"), UTF8_TO_TCHAR(FuncName)));
		return false;
	}
	return true;
}

static void PostFractureFinalize(UGeometryCollection* GC, FGeometryCollection* Collection, int32 FirstNewGeometryIndex)
{
	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection, -1);
	FGeometryCollection::UpdateBoundingBox(*Collection);
	if (FirstNewGeometryIndex > INDEX_NONE)
	{
		GeometryCollection::GenerateTemporaryGuids(Collection, FirstNewGeometryIndex, false);
	}
	GC->InvalidateCollection();
#if WITH_EDITOR
	GC->RebuildRenderData();
#endif
	GC->MarkPackageDirty();
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
static EFractureBrickBondEnum ParseBrickBond(const FString& Str)
{
	if (Str.Equals(TEXT("stretcher"), ESearchCase::IgnoreCase))      return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;
	if (Str.Equals(TEXT("stack"), ESearchCase::IgnoreCase))          return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stack;
	if (Str.Equals(TEXT("english"), ESearchCase::IgnoreCase))        return EFractureBrickBondEnum::Dataflow_FractureBrickBond_English;
	if (Str.Equals(TEXT("header"), ESearchCase::IgnoreCase))         return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Header;
	if (Str.Equals(TEXT("flemish"), ESearchCase::IgnoreCase))        return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Flemish;
	return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;
}
#endif // ENGINE_MINOR_VERSION >= 5

static FDataflowTransformSelection BuildAllSelection(FGeometryCollection* Collection)
{
	FDataflowTransformSelection Selection;
	int32 NumTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
	Selection.Initialize(NumTransforms, true);
	return Selection;
}

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> ChaosFractureDocs = {
	{ TEXT("fracture_voronoi(asset_path, params_table)"), TEXT("Voronoi fracture a geometry collection"), TEXT("{new_pieces}") },
	{ TEXT("fracture_plane(asset_path, params_table)"), TEXT("Plane cutter fracture a geometry collection"), TEXT("{new_pieces}") },
	{ TEXT("fracture_slice(asset_path, params_table)"), TEXT("Uniform slice fracture a geometry collection"), TEXT("{new_pieces}") },
	{ TEXT("fracture_brick(asset_path, params_table)"), TEXT("Brick pattern fracture a geometry collection"), TEXT("{new_pieces}") },
	{ TEXT("fracture_uniform(asset_path, params_table)"), TEXT("Uniform per-bone Voronoi fracture"), TEXT("{new_pieces}") },
	{ TEXT("fracture_info(asset_path)"), TEXT("Get info about a geometry collection's hierarchy, bounds, and materials"), TEXT("{transforms, geometries, ...}") },
	{ TEXT("fracture_cluster(asset_path, params_table)"), TEXT("Auto-cluster fractured pieces using K-Means"), TEXT("{ok}") },
	{ TEXT("fracture_delete(asset_path, params_table)"), TEXT("Delete branches from a geometry collection"), TEXT("{ok}") },
	{ TEXT("fracture_merge(asset_path, params_table)"), TEXT("Merge selected bones in a geometry collection"), TEXT("{ok}") },
	{ TEXT("fracture_visibility(asset_path, params_table)"), TEXT("Set visibility of transforms in a geometry collection"), TEXT("{ok}") },
	{ TEXT("fracture_color(asset_path, params_table)"), TEXT("Set bone colors by level/parent/cluster/random"), TEXT("{ok}") },
	{ TEXT("fracture_flatten(asset_path)"), TEXT("Flatten hierarchy to a single level under root"), TEXT("{ok}") },
};

// ============================================================================
// BINDING
// ============================================================================

static void BindChaosFracture(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// fracture_info(asset_path)
	// ================================================================
	Lua.set_function("fracture_info", [&Session](const std::string& AssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_info", Session);
		if (!Collection) return sol::lua_nil;

		int32 NumTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		int32 NumGeometries = Collection->NumElements(FGeometryCollection::GeometryGroup);
		int32 NumVertices   = Collection->NumElements(FGeometryCollection::VerticesGroup);
		int32 NumFaces      = Collection->NumElements(FGeometryCollection::FacesGroup);
		int32 NumMaterials  = GC->Materials.Num();

		FBoxSphereBounds BSB = Collection->GetBoundingBox();
		FBox Bounds = BSB.GetBox();

		sol::table Result = LuaView.create_table();
		Result["transforms"]  = NumTransforms;
		Result["geometries"]  = NumGeometries;
		Result["vertices"]    = NumVertices;
		Result["faces"]       = NumFaces;
		Result["materials"]   = NumMaterials;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Result["is_empty"]    = GC->IsEmpty();
#else
		Result["is_empty"]    = (Collection->NumElements(FGeometryCollection::TransformGroup) == 0);
#endif

		sol::table BoundsT = LuaView.create_table();
		BoundsT["min_x"] = Bounds.Min.X;
		BoundsT["min_y"] = Bounds.Min.Y;
		BoundsT["min_z"] = Bounds.Min.Z;
		BoundsT["max_x"] = Bounds.Max.X;
		BoundsT["max_y"] = Bounds.Max.Y;
		BoundsT["max_z"] = Bounds.Max.Z;
		Result["bounds"] = BoundsT;

		// Hierarchy: list each transform with parent, children, level, simulation type
		if (Collection->HasAttribute("Parent", FGeometryCollection::TransformGroup) &&
			Collection->HasAttribute("Children", FGeometryCollection::TransformGroup))
		{
			const TManagedArray<int32>& Parent = Collection->GetAttribute<int32>("Parent", FGeometryCollection::TransformGroup);
			const TManagedArray<TSet<int32>>& Children = Collection->GetAttribute<TSet<int32>>("Children", FGeometryCollection::TransformGroup);

			bool bHasLevel = Collection->HasAttribute("Level", FGeometryCollection::TransformGroup);
			const TManagedArray<int32>* Levels = bHasLevel
				? &Collection->GetAttribute<int32>("Level", FGeometryCollection::TransformGroup) : nullptr;

			bool bHasSimType = Collection->HasAttribute("SimulationType", FGeometryCollection::TransformGroup);
			const TManagedArray<int32>* SimTypes = bHasSimType
				? &Collection->GetAttribute<int32>("SimulationType", FGeometryCollection::TransformGroup) : nullptr;

			bool bHasBoneName = Collection->HasAttribute("BoneName", FGeometryCollection::TransformGroup);
			const TManagedArray<FString>* BoneNames = bHasBoneName
				? &Collection->GetAttribute<FString>("BoneName", FGeometryCollection::TransformGroup) : nullptr;

			sol::table Hierarchy = LuaView.create_table();
			for (int32 i = 0; i < NumTransforms; ++i)
			{
				sol::table Node = LuaView.create_table();
				Node["index"]  = i;
				Node["parent"] = Parent[i];
				if (bHasBoneName) Node["name"] = std::string(TCHAR_TO_UTF8(*(*BoneNames)[i]));
				if (Levels) Node["level"] = (*Levels)[i];
				if (SimTypes)
				{
					int32 ST = (*SimTypes)[i];
					if (ST == FGeometryCollection::FST_None)      Node["sim_type"] = "none";
					else if (ST == FGeometryCollection::FST_Rigid)     Node["sim_type"] = "rigid";
					else if (ST == FGeometryCollection::FST_Clustered) Node["sim_type"] = "clustered";
				}
				Node["is_geometry"] = Collection->IsGeometry(i);

				sol::table ChildrenT = LuaView.create_table();
				int32 Idx = 1;
				for (int32 ChildIdx : Children[i])
				{
					ChildrenT[Idx++] = ChildIdx;
				}
				Node["children"] = ChildrenT;
				Hierarchy[i + 1] = Node; // 1-indexed Lua table
			}
			Result["hierarchy"] = Hierarchy;
		}

		// Root bones
		TArray<int32> RootBones;
		FGeometryCollectionClusteringUtility::GetRootBones(Collection, RootBones);
		sol::table RootsT = LuaView.create_table();
		for (int32 i = 0; i < RootBones.Num(); ++i)
		{
			RootsT[i + 1] = RootBones[i];
		}
		Result["root_bones"] = RootsT;

		Session.Log(FString::Printf(TEXT("[OK] fracture_info -> %d transforms, %d geometries, %d verts, %d faces"),
			NumTransforms, NumGeometries, NumVertices, NumFaces));
		return Result;
	});

	// ================================================================
	// fracture_voronoi(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_voronoi", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_voronoi", Session);
		if (!Collection) return sol::lua_nil;

		// Parse parameters
		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();
		int32 NumSites          = FMath::Max(1, static_cast<int32>(P.get_or("num_sites", 10)));
		int32 RandomSeed        = static_cast<int32>(P.get_or("random_seed", 0));
		float ChanceToFracture  = static_cast<float>(P.get_or("chance_to_fracture", 1.0));
		bool  bSplitIslands     = P.get_or("split_islands", true);
		float Grout             = static_cast<float>(P.get_or("grout", 0.0));
		float Amplitude         = static_cast<float>(P.get_or("amplitude", 0.0));
		float Frequency         = static_cast<float>(P.get_or("frequency", 0.5));
		float Persistence       = static_cast<float>(P.get_or("persistence", 0.5));
		float Lacunarity        = static_cast<float>(P.get_or("lacunarity", 2.0));
		int32 Octaves           = static_cast<int32>(P.get_or("octaves", 4));
		float PointSpacing      = static_cast<float>(P.get_or("point_spacing", 1.0));
		bool  bCollisionSamples = P.get_or("collision_samples", false);
		float CollisionSpacing  = static_cast<float>(P.get_or("collision_spacing", 50.0));

		FDataflowTransformSelection Selection = BuildAllSelection(Collection);

		FBox Bounds = Collection->GetBoundingBox().GetBox();
		if (!ValidateBounds(Bounds, "fracture_voronoi", Session)) return sol::lua_nil;

		// Generate random Voronoi sites within bounds
		TArray<FVector> Sites;
		Sites.Reserve(NumSites);
		FRandomStream RandStream(RandomSeed);
		for (int32 i = 0; i < NumSites; ++i)
		{
			Sites.Add(FVector(
				RandStream.FRandRange(Bounds.Min.X, Bounds.Max.X),
				RandStream.FRandRange(Bounds.Min.Y, Bounds.Max.Y),
				RandStream.FRandRange(Bounds.Min.Z, Bounds.Max.Z)
			));
		}

		int32 InitialGeometryCount = Collection->NumElements(FGeometryCollection::GeometryGroup);

		// Fracture — Modify() BEFORE mutation for undo support
		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Voronoi Fracture")));
		GC->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		int32 NewPieces = FFractureEngineFracturing::VoronoiFracture(
			*Collection, Selection, Sites, FTransform::Identity,
			RandomSeed, ChanceToFracture, bSplitIslands,
			Grout, Amplitude, Frequency, Persistence, Lacunarity, Octaves,
			PointSpacing, bCollisionSamples, CollisionSpacing);

		PostFractureFinalize(GC, Collection, InitialGeometryCount);

		Session.Log(FString::Printf(TEXT("[OK] fracture_voronoi -> %d new pieces (sites=%d, seed=%d)"), NewPieces, NumSites, RandomSeed));
		sol::table Result = LuaView.create_table();
		Result["new_pieces"] = NewPieces;
		return Result;
#else
		Session.Log(TEXT("[FAIL] fracture_voronoi requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

	// ================================================================
	// fracture_plane(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_plane", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_plane", Session);
		if (!Collection) return sol::lua_nil;

		// Parse parameters
		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();
		int32 NumPlanes         = FMath::Max(1, static_cast<int32>(P.get_or("num_planes", 3)));
		int32 RandomSeed        = static_cast<int32>(P.get_or("random_seed", 0));
		float ChanceToFracture  = static_cast<float>(P.get_or("chance_to_fracture", 1.0));
		bool  bSplitIslands     = P.get_or("split_islands", true);
		float Grout             = static_cast<float>(P.get_or("grout", 0.0));
		float Amplitude         = static_cast<float>(P.get_or("amplitude", 0.0));
		float Frequency         = static_cast<float>(P.get_or("frequency", 0.5));
		float Persistence       = static_cast<float>(P.get_or("persistence", 0.5));
		float Lacunarity        = static_cast<float>(P.get_or("lacunarity", 2.0));
		int32 Octaves           = static_cast<int32>(P.get_or("octaves", 4));
		float PointSpacing      = static_cast<float>(P.get_or("point_spacing", 1.0));
		bool  bCollisionSamples = P.get_or("collision_samples", false);
		float CollisionSpacing  = static_cast<float>(P.get_or("collision_spacing", 50.0));

		FDataflowTransformSelection Selection = BuildAllSelection(Collection);
		FBox Bounds = Collection->GetBoundingBox().GetBox();
		if (!ValidateBounds(Bounds, "fracture_plane", Session)) return sol::lua_nil;

		int32 InitialGeometryCount = Collection->NumElements(FGeometryCollection::GeometryGroup);

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Plane Fracture")));
		GC->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		int32 NewPieces = FFractureEngineFracturing::PlaneCutter(
			*Collection, Selection, Bounds, FTransform::Identity,
			NumPlanes, RandomSeed, ChanceToFracture, bSplitIslands,
			Grout, Amplitude, Frequency, Persistence, Lacunarity, Octaves,
			PointSpacing, bCollisionSamples, CollisionSpacing);

		PostFractureFinalize(GC, Collection, InitialGeometryCount);

		Session.Log(FString::Printf(TEXT("[OK] fracture_plane -> %d new pieces (planes=%d, seed=%d)"), NewPieces, NumPlanes, RandomSeed));
		sol::table Result = LuaView.create_table();
		Result["new_pieces"] = NewPieces;
		return Result;
#else
		Session.Log(TEXT("[FAIL] fracture_plane requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

	// ================================================================
	// fracture_slice(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_slice", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_slice", Session);
		if (!Collection) return sol::lua_nil;

		// Parse parameters
		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();
		int32 SlicesX           = FMath::Max(1, static_cast<int32>(P.get_or("slices_x", 2)));
		int32 SlicesY           = FMath::Max(1, static_cast<int32>(P.get_or("slices_y", 2)));
		int32 SlicesZ           = FMath::Max(1, static_cast<int32>(P.get_or("slices_z", 2)));
		float AngleVariation    = static_cast<float>(P.get_or("angle_variation", 0.0));
		float OffsetVariation   = static_cast<float>(P.get_or("offset_variation", 0.0));
		int32 RandomSeed        = static_cast<int32>(P.get_or("random_seed", 0));
		float ChanceToFracture  = static_cast<float>(P.get_or("chance_to_fracture", 1.0));
		bool  bSplitIslands     = P.get_or("split_islands", true);
		float Grout             = static_cast<float>(P.get_or("grout", 0.0));
		float Amplitude         = static_cast<float>(P.get_or("amplitude", 0.0));
		float Frequency         = static_cast<float>(P.get_or("frequency", 0.5));
		float Persistence       = static_cast<float>(P.get_or("persistence", 0.5));
		float Lacunarity        = static_cast<float>(P.get_or("lacunarity", 2.0));
		int32 Octaves           = static_cast<int32>(P.get_or("octaves", 4));
		float PointSpacing      = static_cast<float>(P.get_or("point_spacing", 1.0));
		bool  bCollisionSamples = P.get_or("collision_samples", false);
		float CollisionSpacing  = static_cast<float>(P.get_or("collision_spacing", 50.0));

		FDataflowTransformSelection Selection = BuildAllSelection(Collection);
		FBox Bounds = Collection->GetBoundingBox().GetBox();
		if (!ValidateBounds(Bounds, "fracture_slice", Session)) return sol::lua_nil;

		int32 InitialGeometryCount = Collection->NumElements(FGeometryCollection::GeometryGroup);

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Slice Fracture")));
		GC->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		int32 NewPieces = FFractureEngineFracturing::SliceCutter(
			*Collection, Selection, Bounds,
			SlicesX, SlicesY, SlicesZ,
			AngleVariation, OffsetVariation,
			RandomSeed, ChanceToFracture, bSplitIslands,
			Grout, Amplitude, Frequency, Persistence, Lacunarity, Octaves,
			PointSpacing, bCollisionSamples, CollisionSpacing);

		PostFractureFinalize(GC, Collection, InitialGeometryCount);

		Session.Log(FString::Printf(TEXT("[OK] fracture_slice -> %d new pieces (slices=%dx%dx%d, seed=%d)"), NewPieces, SlicesX, SlicesY, SlicesZ, RandomSeed));
		sol::table Result = LuaView.create_table();
		Result["new_pieces"] = NewPieces;
		return Result;
#else
		Session.Log(TEXT("[FAIL] fracture_slice -> SliceCutter requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

	// ================================================================
	// fracture_brick(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_brick", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_brick", Session);
		if (!Collection) return sol::lua_nil;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		// Parse parameters
		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();
		std::string BondStr     = P.get_or<std::string>("bond", "stretcher");
		EFractureBrickBondEnum Bond = ParseBrickBond(UTF8_TO_TCHAR(BondStr.c_str()));
		float BrickLength       = static_cast<float>(P.get_or("brick_length", 194.0));
		float BrickHeight       = static_cast<float>(P.get_or("brick_height", 57.0));
		float BrickDepth        = static_cast<float>(P.get_or("brick_depth", 96.0));
		int32 RandomSeed        = static_cast<int32>(P.get_or("random_seed", 0));
		float ChanceToFracture  = static_cast<float>(P.get_or("chance_to_fracture", 1.0));
		bool  bSplitIslands     = P.get_or("split_islands", true);
		float Grout             = static_cast<float>(P.get_or("grout", 0.0));
		float Amplitude         = static_cast<float>(P.get_or("amplitude", 0.0));
		float Frequency         = static_cast<float>(P.get_or("frequency", 0.5));
		float Persistence       = static_cast<float>(P.get_or("persistence", 0.5));
		float Lacunarity        = static_cast<float>(P.get_or("lacunarity", 2.0));
		int32 Octaves           = static_cast<int32>(P.get_or("octaves", 4));
		float PointSpacing      = static_cast<float>(P.get_or("point_spacing", 1.0));
		bool  bCollisionSamples = P.get_or("collision_samples", false);
		float CollisionSpacing  = static_cast<float>(P.get_or("collision_spacing", 50.0));

		FDataflowTransformSelection Selection = BuildAllSelection(Collection);
		FBox Bounds = Collection->GetBoundingBox().GetBox();
		if (!ValidateBounds(Bounds, "fracture_brick", Session)) return sol::lua_nil;

		int32 InitialGeometryCount = Collection->NumElements(FGeometryCollection::GeometryGroup);

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Brick Fracture")));
		GC->Modify();

		int32 NewPieces = FFractureEngineFracturing::BrickCutter(
			*Collection, Selection, Bounds, FTransform::Identity,
			Bond, BrickLength, BrickHeight, BrickDepth,
			RandomSeed, ChanceToFracture, bSplitIslands,
			Grout, Amplitude, Frequency, Persistence, Lacunarity, Octaves,
			PointSpacing, bCollisionSamples, CollisionSpacing);

		PostFractureFinalize(GC, Collection, InitialGeometryCount);

		Session.Log(FString::Printf(TEXT("[OK] fracture_brick -> %d new pieces (bond=%s, seed=%d)"),
			NewPieces, UTF8_TO_TCHAR(BondStr.c_str()), RandomSeed));
		sol::table Result = LuaView.create_table();
		Result["new_pieces"] = NewPieces;
		return Result;
#else
		Session.Log(TEXT("[FAIL] fracture_brick -> BrickCutter requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	// ================================================================
	// fracture_uniform(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_uniform", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_uniform", Session);
		if (!Collection) return sol::lua_nil;

		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();

		FUniformFractureSettings Settings;
		Settings.Transform          = FTransform::Identity;
		Settings.MinVoronoiSites    = FMath::Max(1, static_cast<int32>(P.get_or("min_sites", 5)));
		Settings.MaxVoronoiSites    = FMath::Max(Settings.MinVoronoiSites, static_cast<int32>(P.get_or("max_sites", 15)));
		Settings.InternalMaterialID = static_cast<int32>(P.get_or("internal_material_id", -1));
		Settings.RandomSeed         = static_cast<int32>(P.get_or("random_seed", 0));
		Settings.ChanceToFracture   = static_cast<float>(P.get_or("chance_to_fracture", 1.0));
		Settings.GroupFracture      = P.get_or("group_fracture", false);
		Settings.SplitIslands       = P.get_or("split_islands", true);
		Settings.Grout              = static_cast<float>(P.get_or("grout", 0.0));
		Settings.AddSamplesForCollision  = P.get_or("collision_samples", false);
		Settings.CollisionSampleSpacing  = static_cast<float>(P.get_or("collision_spacing", 50.0));

		FNoiseSettings& Noise = Settings.NoiseSettings;
		Noise.Amplitude   = static_cast<float>(P.get_or("amplitude", 0.0));
		Noise.Frequency   = static_cast<float>(P.get_or("frequency", 0.5));
		Noise.Persistence = static_cast<float>(P.get_or("persistence", 0.5));
		Noise.Lacunarity  = static_cast<float>(P.get_or("lacunarity", 2.0));
		Noise.Octaves    = static_cast<int32>(P.get_or("octaves", 4));
		Noise.PointSpacing = static_cast<float>(P.get_or("point_spacing", 1.0));

		FDataflowTransformSelection Selection = BuildAllSelection(Collection);

		int32 InitialGeometryCount = Collection->NumElements(FGeometryCollection::GeometryGroup);

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Uniform Fracture")));
		GC->Modify();

		int32 NewPieces = FFractureEngineFracturing::UniformFracture(*Collection, Selection, Settings);

		PostFractureFinalize(GC, Collection, InitialGeometryCount);

		Session.Log(FString::Printf(TEXT("[OK] fracture_uniform -> %d new pieces (sites=%d-%d, seed=%d)"),
			NewPieces, Settings.MinVoronoiSites, Settings.MaxVoronoiSites, Settings.RandomSeed));
		sol::table Result = LuaView.create_table();
		Result["new_pieces"] = NewPieces;
		return Result;
	});
#endif

	// ================================================================
	// fracture_cluster(asset_path, params_table)
	// ================================================================
	Lua.set_function("fracture_cluster", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_cluster", Session);
		if (!Collection) return sol::lua_nil;

		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();

		// Parse cluster size method
		std::string MethodStr = P.get_or<std::string>("method", "by_number");
		EFractureEngineClusterSizeMethod Method = EFractureEngineClusterSizeMethod::ByNumber;
		if (MethodStr == "by_fraction")  Method = EFractureEngineClusterSizeMethod::ByFractionOfInput;
		else if (MethodStr == "by_size") Method = EFractureEngineClusterSizeMethod::BySize;
		else if (MethodStr == "by_grid") Method = EFractureEngineClusterSizeMethod::ByGrid;

		uint32 SiteCount          = static_cast<uint32>(FMath::Max(1, static_cast<int32>(P.get_or("cluster_count", 4))));
		float  SiteCountFraction  = static_cast<float>(P.get_or("cluster_fraction", 0.25));
		float  SiteSize           = static_cast<float>(P.get_or("cluster_size", 100.0));
		bool   bEnforceConnectivity = P.get_or("enforce_connectivity", true);
		bool   bAvoidIsolated     = P.get_or("avoid_isolated", true);
		bool   bEnforceSiteParams = P.get_or("enforce_site_parameters", false);
		int32  GridX = FMath::Max(1, static_cast<int32>(P.get_or("grid_x", 2)));
		int32  GridY = FMath::Max(1, static_cast<int32>(P.get_or("grid_y", 2)));
		int32  GridZ = FMath::Max(1, static_cast<int32>(P.get_or("grid_z", 2)));
		float  MinClusterSize     = static_cast<float>(P.get_or("min_cluster_size", 0.0));
		int32  KMeansIterations   = FMath::Max(1, static_cast<int32>(P.get_or("kmeans_iterations", 500)));

		// Cluster index: -1 means use all leaf bones (first root bone)
		int32 ClusterIndex = static_cast<int32>(P.get_or("cluster_index", -1));

		// Find root bone if cluster_index not specified
		if (ClusterIndex < 0)
		{
			TArray<int32> RootBones;
			FGeometryCollectionClusteringUtility::GetRootBones(Collection, RootBones);
			if (RootBones.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] fracture_cluster -> no root bones found"));
				return sol::lua_nil;
			}
			ClusterIndex = RootBones[0];
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Auto Cluster")));
		GC->Modify();

		FFractureEngineClustering::AutoCluster(
			*Collection, ClusterIndex,
			Method, SiteCount, SiteCountFraction, SiteSize,
			bEnforceConnectivity, bAvoidIsolated, bEnforceSiteParams,
			GridX, GridY, GridZ, MinClusterSize, KMeansIterations);

		FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection, -1);
		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] fracture_cluster -> auto-clustered (method=%s, count=%d)"),
			UTF8_TO_TCHAR(MethodStr.c_str()), SiteCount));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});

	// ================================================================
	// fracture_delete(asset_path, {indices={...}})
	// ================================================================
	Lua.set_function("fracture_delete", [&Session](const std::string& AssetPathStr, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_delete", Session);
		if (!Collection) return sol::lua_nil;

		sol::optional<sol::table> IndicesOpt = Params["indices"];
		if (!IndicesOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] fracture_delete -> 'indices' array is required"));
			return sol::lua_nil;
		}

		TArray<int32> BoneSelection;
		sol::table IndicesT = IndicesOpt.value();
		int32 NumTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		for (auto& Pair : IndicesT)
		{
			int32 Idx = Pair.second.as<int32>();
			if (Idx >= 0 && Idx < NumTransforms)
			{
				BoneSelection.Add(Idx);
			}
		}

		if (BoneSelection.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] fracture_delete -> no valid indices provided"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Fracture Delete")));
		GC->Modify();

		FFractureEngineEdit::DeleteBranch(*Collection, BoneSelection);

		FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection, -1);
		FGeometryCollectionClusteringUtility::RemoveDanglingClusters(Collection);
		FGeometryCollection::UpdateBoundingBox(*Collection);
		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] fracture_delete -> deleted %d branches"), BoneSelection.Num()));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});

	// ================================================================
	// fracture_merge(asset_path, {indices={...}})
	// ================================================================
	Lua.set_function("fracture_merge", [&Session](const std::string& AssetPathStr, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_merge", Session);
		if (!Collection) return sol::lua_nil;

		sol::optional<sol::table> IndicesOpt = Params["indices"];
		if (!IndicesOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] fracture_merge -> 'indices' array is required"));
			return sol::lua_nil;
		}

		TArray<int32> BoneSelection;
		sol::table IndicesT = IndicesOpt.value();
		int32 NumTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		for (auto& Pair : IndicesT)
		{
			int32 Idx = Pair.second.as<int32>();
			if (Idx >= 0 && Idx < NumTransforms)
			{
				BoneSelection.Add(Idx);
			}
		}

		if (BoneSelection.Num() < 2)
		{
			Session.Log(TEXT("[FAIL] fracture_merge -> need at least 2 valid indices to merge"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Fracture Merge")));
		GC->Modify();

		FFractureEngineEdit::Merge(*Collection, BoneSelection);

		FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection, -1);
		FGeometryCollection::UpdateBoundingBox(*Collection);
		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] fracture_merge -> merged %d bones"), BoneSelection.Num()));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});

	// ================================================================
	// fracture_visibility(asset_path, {indices={...}, visible=bool})
	// ================================================================
	Lua.set_function("fracture_visibility", [&Session](const std::string& AssetPathStr, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_visibility", Session);
		if (!Collection) return sol::lua_nil;

		sol::optional<sol::table> IndicesOpt = Params["indices"];
		if (!IndicesOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] fracture_visibility -> 'indices' array is required"));
			return sol::lua_nil;
		}

		bool bVisible = Params.get_or("visible", true);

		TArray<int32> TransformSelection;
		sol::table IndicesT = IndicesOpt.value();
		int32 NumTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		for (auto& Pair : IndicesT)
		{
			int32 Idx = Pair.second.as<int32>();
			if (Idx >= 0 && Idx < NumTransforms)
			{
				TransformSelection.Add(Idx);
			}
		}

		if (TransformSelection.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] fracture_visibility -> no valid indices provided"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Fracture Visibility")));
		GC->Modify();

		FFractureEngineEdit::SetVisibilityInCollectionFromTransformSelection(*Collection, TransformSelection, bVisible);

		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] fracture_visibility -> set %d transforms %s"),
			TransformSelection.Num(), bVisible ? TEXT("visible") : TEXT("hidden")));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	// ================================================================
	// fracture_color(asset_path, {mode="...", ...})
	// ================================================================
	Lua.set_function("fracture_color", [&Session](const std::string& AssetPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_color", Session);
		if (!Collection) return sol::lua_nil;

		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();
		std::string ModeStr = P.get_or<std::string>("mode", "level");
		int32 Level     = static_cast<int32>(P.get_or("level", -1));
		int32 Seed      = static_cast<int32>(P.get_or("random_seed", 0));
		int32 ColorMin  = static_cast<int32>(P.get_or("color_range_min", 40));
		int32 ColorMax  = static_cast<int32>(P.get_or("color_range_max", 190));

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Fracture Color")));
		GC->Modify();

		FFractureEngineFracturing::InitColors(*Collection);

		FRandomStream RandStream(Seed);

		if (ModeStr == "level")
		{
			FFractureEngineFracturing::SetBoneColorByLevel(*Collection, Level);
		}
		else if (ModeStr == "parent")
		{
			FFractureEngineFracturing::SetBoneColorByParent(*Collection, RandStream, Level, ColorMin, ColorMax);
		}
		else if (ModeStr == "cluster")
		{
			FFractureEngineFracturing::SetBoneColorByCluster(*Collection, RandStream, Level, ColorMin, ColorMax);
		}
		else if (ModeStr == "random")
		{
			FFractureEngineFracturing::SetBoneColorRandom(*Collection, RandStream);
		}
		else if (ModeStr == "leaf_level")
		{
			FFractureEngineFracturing::SetBoneColorByLeafLevel(*Collection, Level);
		}
		else if (ModeStr == "leaf")
		{
			FFractureEngineFracturing::SetBoneColorByLeaf(*Collection, RandStream, Level, ColorMin, ColorMax);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] fracture_color -> unknown mode '%s' (use: level, parent, cluster, random, leaf_level, leaf)"),
				UTF8_TO_TCHAR(ModeStr.c_str())));
			return sol::lua_nil;
		}

		FFractureEngineFracturing::TransferBoneColorToVertexColor(*Collection);

		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] fracture_color -> colored by %s"), UTF8_TO_TCHAR(ModeStr.c_str())));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});
#endif

	// ================================================================
	// fracture_flatten(asset_path)
	// ================================================================
	Lua.set_function("fracture_flatten", [&Session](const std::string& AssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGeometryCollection* GC = LoadGeometryCollection(AssetPathStr, Session);
		if (!GC) return sol::lua_nil;

		FGeometryCollection* Collection = GetCollection(GC, "fracture_flatten", Session);
		if (!Collection) return sol::lua_nil;

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua Fracture Flatten")));
		GC->Modify();

		// Collapse to level 1 — flattens hierarchy under root
		FGeometryCollectionClusteringUtility::CollapseLevelHierarchy(1, Collection);
		FGeometryCollectionClusteringUtility::RemoveDanglingClusters(Collection);
		FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection, -1);

		GC->InvalidateCollection();
#if WITH_EDITOR
		GC->RebuildRenderData();
#endif
		GC->MarkPackageDirty();

		Session.Log(TEXT("[OK] fracture_flatten -> hierarchy flattened"));
		sol::table Result = LuaView.create_table();
		Result["ok"] = true;
		return Result;
	});
}

REGISTER_LUA_BINDING(ChaosFracture, ChaosFractureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("FractureEngine")))
	{
		Session.Log(TEXT("[WARN] FractureEngine plugin is not loaded. Enable Fracture + Dataflow plugins in Edit > Plugins to use this feature."));
		return;
	}
	BindChaosFracture(Lua, Session);
});
