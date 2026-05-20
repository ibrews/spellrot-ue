#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Editor.h"

#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "StaticToSkeletalMeshConverter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"

static TArray<FLuaFunctionDoc> GeometryScriptingDocs = {
	{ TEXT("geometry_create(params)"), TEXT("Create primitive meshes (box/sphere/cylinder/cone/capsule/torus) or perform boolean ops, optionally save and spawn. params: type, save?, spawn_location?, label?, size/radius/height/steps, boolean: target+tool+operation(union|intersect|subtract)"), TEXT("table or nil") },
	{ TEXT("convert_to_skeletal_mesh(static_mesh_path, dest_path, skeleton_path, opts?)"), TEXT("Convert a StaticMesh to a SkeletalMesh bound to a skeleton. opts: bind_bone (name of bone to bind all verts to, defaults to root)"), TEXT("table or nil") },
	{ TEXT("geometry_query(mesh_path)"), TEXT("Query mesh info: vertex_count, triangle_count, bounding_box, surface_area, volume, center_of_mass, is_closed, border_edge_count, connected_components, has_attribute_set, is_dense"), TEXT("table or nil") },
	{ TEXT("geometry_simplify(mesh_path, opts)"), TEXT("Simplify a static mesh. opts: target_count (triangles), method (to_triangle_count|to_vertex_count|to_tolerance|to_planar|to_edge_length), tolerance, vertex_count, edge_length"), TEXT("table or nil") },
	{ TEXT("geometry_repair(mesh_path, opts?)"), TEXT("Repair a static mesh. opts: operations (array of: weld_edges|fill_holes|resolve_t_junctions|remove_degenerates|compact|remove_hidden|split_bowties|remove_small_components|remove_unused_vertices), tolerance"), TEXT("table or nil") },
	{ TEXT("geometry_normals(mesh_path, opts?)"), TEXT("Recompute normals/tangents. opts: mode (recompute|split|flip|per_vertex|per_face|auto_repair|compute_tangents), split_angle"), TEXT("table or nil") },
	{ TEXT("geometry_remesh(mesh_path, opts?)"), TEXT("Uniform remesh a static mesh. opts: target_count (triangle target), target_edge_length, iterations, smoothing_rate"), TEXT("table or nil") },
	{ TEXT("geometry_transform(mesh_path, opts)"), TEXT("Transform mesh vertices. opts: translate {x,y,z}, rotate {pitch,yaw,roll}, scale {x,y,z} or number, scale_origin {x,y,z}"), TEXT("table or nil") },
	{ TEXT("geometry_boolean(target_path, tool_path, operation, opts?)"), TEXT("Boolean op on two static meshes. operation: union|intersect|subtract. opts: save (output path)"), TEXT("table or nil") },
	{ TEXT("geometry_plane_cut(mesh_path, opts)"), TEXT("Cut/slice/mirror a mesh with a plane. opts: mode (cut|slice|mirror), origin {x,y,z}, normal {x,y,z}, fill_holes?"), TEXT("table or nil") },
	{ TEXT("geometry_subdivide(mesh_path, opts?)"), TEXT("Subdivide/tessellate a mesh. opts: method (uniform|pn), level (tessellation level, default 3)"), TEXT("table or nil") },
	{ TEXT("geometry_uvs(mesh_path, opts?)"), TEXT("Recompute or repack UVs. opts: mode (recompute|repack|layout), uv_channel (default 0), resolution"), TEXT("table or nil") },
	{ TEXT("geometry_materials(mesh_path, verb, opts?)"), TEXT("Query or modify mesh material IDs. verb: info|remap|clear|delete_by_id. opts: from_id, to_id, material_id, clear_value"), TEXT("table or nil") },
	{ TEXT("geometry_smooth(mesh_path, opts?)"), TEXT("Apply smoothing to a mesh. opts: iterations (default 10), speed (default 0.25)"), TEXT("table or nil") },
};

static UWorld* GetEditorWorldForGeometry()
{
	if (!GEditor) return nullptr;
	return GEditor->GetEditorWorldContext().World();
}

static FVector GS_TableToVector(const sol::table& T)
{
	return FVector(
		T.get_or("x", 0.0),
		T.get_or("y", 0.0),
		T.get_or("z", 0.0));
}

static FRotator GS_TableToRotator(const sol::table& T)
{
	return FRotator(
		T.get_or("pitch", 0.0),
		T.get_or("yaw", 0.0),
		T.get_or("roll", 0.0));
}

// Helper: Load a StaticMesh and copy it to a transient UDynamicMesh
static UDynamicMesh* LoadStaticMeshToDynamic(const FString& AssetPath, FLuaSessionData& Session, const TCHAR* CallerName)
{
	FString FullPath = AssetPath;
	if (!FullPath.Contains(TEXT(".")))
	{
		FString Name = FPackageName::GetLongPackageAssetName(FullPath);
		FullPath = FullPath + TEXT(".") + Name;
	}
	UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *FullPath);
	if (!SM)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> static mesh not found: %s"), CallerName, *AssetPath));
		return nullptr;
	}

	UDynamicMesh* DynMesh = NewObject<UDynamicMesh>(GetTransientPackage());
	FGeometryScriptCopyMeshFromAssetOptions CopyFromOpts;
	FGeometryScriptMeshReadLOD ReadLOD;
	EGeometryScriptOutcomePins Outcome;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(SM, DynMesh, CopyFromOpts, ReadLOD, Outcome);
	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> failed to copy mesh data from %s"), CallerName, *AssetPath));
		return nullptr;
	}
	return DynMesh;
}

// Helper: Save a UDynamicMesh back to a StaticMesh asset (overwrite or create)
static bool SaveDynamicMeshToAsset(UDynamicMesh* DynMesh, const FString& AssetPath, FLuaSessionData& Session, const TCHAR* CallerName)
{
	FString PackageName = AssetPath;
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	UStaticMesh* ExistingSM = LoadObject<UStaticMesh>(nullptr, *(AssetPath + TEXT(".") + AssetName));

	UPackage* Package = ExistingSM ? ExistingSM->GetOutermost() : CreatePackage(*PackageName);
	if (!Package)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> cannot create package '%s'"), CallerName, *PackageName));
		return false;
	}

	UStaticMesh* TargetSM = ExistingSM ? ExistingSM : NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone);

	FGeometryScriptCopyMeshToAssetOptions CopyOpts;
	CopyOpts.bReplaceMaterials = false;
	FGeometryScriptMeshWriteLOD WriteLOD;
	EGeometryScriptOutcomePins Outcome;

	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(DynMesh, TargetSM, CopyOpts, WriteLOD, Outcome);

	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> CopyMeshToStaticMesh failed for %s"), CallerName, *AssetPath));
		return false;
	}

	TargetSM->Build(true);
	TargetSM->PostEditChange();

	if (!ExistingSM)
	{
		FAssetRegistryModule::AssetCreated(TargetSM);
	}
	Package->MarkPackageDirty();
	return true;
}

static void BindGeometryScripting(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// geometry_create(params) — create primitives, booleans, save, spawn
	// ================================================================
	Lua.set_function("geometry_create", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* W = GetEditorWorldForGeometry();
		if (!W) { Session.Log(TEXT("[FAIL] geometry_create -> no editor world")); return sol::lua_nil; }

		std::string TypeStr = Params.get_or("type", std::string());
		FString FType = UTF8_TO_TCHAR(TypeStr.c_str());
		std::string SavePath = Params.get_or("save", std::string());

		UDynamicMesh* DynMesh = NewObject<UDynamicMesh>(GetTransientPackage());
		DynMesh->Reset();

		FGeometryScriptPrimitiveOptions PrimOpts;
		FTransform Xform = FTransform::Identity;

		if (auto LT = Params.get<sol::optional<sol::table>>("location"))
			Xform.SetLocation(GS_TableToVector(LT.value()));
		if (auto RT = Params.get<sol::optional<sol::table>>("rotation"))
			Xform.SetRotation(FQuat(GS_TableToRotator(RT.value())));

		bool bMeshCreated = false;

		if (FType.Equals(TEXT("box"), ESearchCase::IgnoreCase))
		{
			double SizeX = 100, SizeY = 100, SizeZ = 100;
			if (auto ST = Params.get<sol::optional<sol::table>>("size"))
			{
				SizeX = ST.value().get_or("x", 100.0);
				SizeY = ST.value().get_or("y", 100.0);
				SizeZ = ST.value().get_or("z", 100.0);
			}
			else
			{
				double Sz = Params.get_or("size", 100.0);
				SizeX = SizeY = SizeZ = Sz;
			}
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				DynMesh, PrimOpts, Xform,
				static_cast<float>(SizeX), static_cast<float>(SizeY), static_cast<float>(SizeZ));
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			float Radius = static_cast<float>(Params.get_or("radius", 50.0));
			int32 Steps = Params.get_or("steps", 16);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
				DynMesh, PrimOpts, Xform, Radius, Steps, Steps);
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase))
		{
			float Radius = static_cast<float>(Params.get_or("radius", 50.0));
			float Height = static_cast<float>(Params.get_or("height", 100.0));
			int32 Steps = Params.get_or("steps", 12);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
				DynMesh, PrimOpts, Xform, Radius, Height, Steps);
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("cone"), ESearchCase::IgnoreCase))
		{
			float BaseRadius = static_cast<float>(Params.get_or("base_radius", 50.0));
			float TopRadius = static_cast<float>(Params.get_or("top_radius", 5.0));
			float Height = static_cast<float>(Params.get_or("height", 100.0));
			int32 Steps = Params.get_or("steps", 12);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
				DynMesh, PrimOpts, Xform, BaseRadius, TopRadius, Height, Steps);
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
		{
			float Radius = static_cast<float>(Params.get_or("radius", 30.0));
			float Length = static_cast<float>(Params.get_or("length", 75.0));
			int32 Steps = Params.get_or("steps", 8);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCapsule(
				DynMesh, PrimOpts, Xform, Radius, Length, Steps, Steps, 0);
#else
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCapsule(
				DynMesh, PrimOpts, Xform, Radius, Length, Steps, Steps);
#endif
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("torus"), ESearchCase::IgnoreCase))
		{
			float MajorRadius = static_cast<float>(Params.get_or("major_radius", 50.0));
			float MinorRadius = static_cast<float>(Params.get_or("minor_radius", 25.0));
			int32 MajorSteps = Params.get_or("major_steps", 16);
			int32 MinorSteps = Params.get_or("minor_steps", 8);
			FGeometryScriptRevolveOptions RevolveOpts;
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
				DynMesh, PrimOpts, Xform, RevolveOpts, MajorRadius, MinorRadius, MajorSteps, MinorSteps);
			bMeshCreated = true;
		}
		else if (FType.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))
		{
			std::string TargetPath = Params.get_or("target", std::string());
			std::string ToolPath = Params.get_or("tool", std::string());
			std::string OpStr = Params.get_or("operation", std::string("subtract"));

			UStaticMesh* TargetSM = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(TargetPath.c_str()));
			UStaticMesh* ToolSM = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(ToolPath.c_str()));
			if (!TargetSM || !ToolSM)
			{
				Session.Log(TEXT("[FAIL] geometry_create(\"boolean\") -> target or tool mesh not found"));
				return sol::lua_nil;
			}

			UDynamicMesh* TargetDyn = NewObject<UDynamicMesh>(GetTransientPackage());
			UDynamicMesh* ToolDyn = NewObject<UDynamicMesh>(GetTransientPackage());
			EGeometryScriptOutcomePins CopyOutcome;
			FGeometryScriptCopyMeshFromAssetOptions CopyFromOpts;
			FGeometryScriptMeshReadLOD ReadLOD;

			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
				TargetSM, TargetDyn, CopyFromOpts, ReadLOD, CopyOutcome);
			if (CopyOutcome != EGeometryScriptOutcomePins::Success)
			{
				Session.Log(TEXT("[FAIL] geometry_create(\"boolean\") -> failed to copy target mesh"));
				return sol::lua_nil;
			}

			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
				ToolSM, ToolDyn, CopyFromOpts, ReadLOD, CopyOutcome);
			if (CopyOutcome != EGeometryScriptOutcomePins::Success)
			{
				Session.Log(TEXT("[FAIL] geometry_create(\"boolean\") -> failed to copy tool mesh"));
				return sol::lua_nil;
			}

			EGeometryScriptBooleanOperation BoolOp = EGeometryScriptBooleanOperation::Subtract;
			FString FOp = UTF8_TO_TCHAR(OpStr.c_str());
			if (FOp.Equals(TEXT("union"), ESearchCase::IgnoreCase)) BoolOp = EGeometryScriptBooleanOperation::Union;
			else if (FOp.Equals(TEXT("intersect"), ESearchCase::IgnoreCase)) BoolOp = EGeometryScriptBooleanOperation::Intersection;

			FGeometryScriptMeshBooleanOptions BoolOpts;
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
				TargetDyn, FTransform::Identity, ToolDyn, FTransform::Identity, BoolOp, BoolOpts);

			DynMesh = TargetDyn;
			bMeshCreated = true;
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_create -> unknown type '%s' (box|sphere|cylinder|cone|capsule|torus|boolean)"), *FType));
			return sol::lua_nil;
		}

		if (!bMeshCreated)
		{
			Session.Log(TEXT("[FAIL] geometry_create -> mesh creation failed"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["type"] = TypeStr;

		// Save to StaticMesh asset if path provided
		if (!SavePath.empty())
		{
			FString FullPath = UTF8_TO_TCHAR(SavePath.c_str());

			FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryCreate", "Geometry Create"));

			if (SaveDynamicMeshToAsset(DynMesh, FullPath, Session, TEXT("geometry_create")))
			{
				Result["saved"] = SavePath;
				Result["asset"] = SavePath;
				Session.Log(FString::Printf(TEXT("[OK] geometry_create(\"%s\") -> saved to %s"), *FType, *FullPath));
			}
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[OK] geometry_create(\"%s\") -> created (not saved, provide save=\"/Game/Path\" to persist)"), *FType));
		}

		// Optionally spawn in level
		if (auto LT = Params.get<sol::optional<sol::table>>("spawn_location"))
		{
			FString AssetPath = SavePath.empty() ? FString() : UTF8_TO_TCHAR(SavePath.c_str());
			if (AssetPath.IsEmpty())
			{
				Session.Log(TEXT("[WARN] geometry_create -> spawn_location requires save path"));
			}
			else
			{
				FString ObjPath = AssetPath;
				if (!ObjPath.Contains(TEXT(".")))
				{
					FString Name = FPackageName::GetLongPackageAssetName(ObjPath);
					ObjPath = ObjPath + TEXT(".") + Name;
				}
				UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *ObjPath);
				if (SM)
				{
					FVector SpawnLoc = GS_TableToVector(LT.value());
					AStaticMeshActor* SMA = W->SpawnActor<AStaticMeshActor>(SpawnLoc, FRotator::ZeroRotator);
					if (SMA)
					{
						SMA->SetFlags(RF_Transactional);
						if (UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent())
						{
							SMC->SetStaticMesh(SM);
						}
						std::string Label = Params.get_or("label", std::string());
						if (!Label.empty()) SMA->SetActorLabel(UTF8_TO_TCHAR(Label.c_str()));
						Result["actor"] = TCHAR_TO_UTF8(*SMA->GetActorLabel());
					}
				}
			}
		}

		return Result;
	});

	// ================================================================
	// convert_to_skeletal_mesh(static_mesh_path, dest_path, skeleton_path, opts?)
	// ================================================================
	Lua.set_function("convert_to_skeletal_mesh", [&Session](
		const std::string& StaticMeshPath,
		const std::string& DestPath,
		const std::string& SkeletonPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		FString FSMPath = UTF8_TO_TCHAR(StaticMeshPath.c_str());
		UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *FSMPath);
		if (!StaticMesh)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] convert_to_skeletal_mesh -> static mesh not found: %s"), *FSMPath));
			return sol::lua_nil;
		}

		FString FSkelPath = UTF8_TO_TCHAR(SkeletonPath.c_str());
		if (!FSkelPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(FSkelPath);
			FSkelPath = FSkelPath + TEXT(".") + AssetName;
		}
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *FSkelPath);
		if (!Skeleton)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] convert_to_skeletal_mesh -> skeleton not found: %s"), *FSkelPath));
			return sol::lua_nil;
		}

		FName BindBone = NAME_None;
		if (OptsOpt.has_value())
		{
			sol::optional<std::string> BoneOpt = OptsOpt.value().get<sol::optional<std::string>>("bind_bone");
			if (BoneOpt.has_value())
			{
				BindBone = FName(UTF8_TO_TCHAR(BoneOpt.value().c_str()));
			}
		}

		FString FDestPath = UTF8_TO_TCHAR(DestPath.c_str());
		FString AssetName = FPackageName::GetLongPackageAssetName(FDestPath);
		UPackage* Package = CreatePackage(*FDestPath);
		if (!Package)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] convert_to_skeletal_mesh -> cannot create package: %s"), *FDestPath));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "ConvertToSkeletal", "Convert To Skeletal Mesh"));

		USkeletalMesh* NewSkelMesh = NewObject<USkeletalMesh>(Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional);

		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
		bool bSuccess = FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromStaticMesh(
			NewSkelMesh, StaticMesh, RefSkel, BindBone);

		if (!bSuccess)
		{
			Session.Log(TEXT("[FAIL] convert_to_skeletal_mesh -> InitializeSkeletalMeshFromStaticMesh failed"));
			return sol::lua_nil;
		}

		NewSkelMesh->SetSkeleton(Skeleton);
		Skeleton->MergeAllBonesToBoneTree(NewSkelMesh);
		NewSkelMesh->PostEditChange();

		FAssetRegistryModule::AssetCreated(NewSkelMesh);
		Package->MarkPackageDirty();

		sol::table Result = Lua.create_table();
		Result["path"] = TCHAR_TO_UTF8(*NewSkelMesh->GetPathName());
		Result["skeleton"] = TCHAR_TO_UTF8(*Skeleton->GetPathName());
		Result["bone_count"] = RefSkel.GetNum();

		Session.Log(FString::Printf(TEXT("[OK] convert_to_skeletal_mesh -> created %s (%d bones)"),
			*FDestPath, RefSkel.GetNum()));
		return Result;
	});

	// ================================================================
	// geometry_query(mesh_path) — query mesh info
	// ================================================================
	Lua.set_function("geometry_query", [&Session](const std::string& MeshPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_query"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Result = Lua.create_table();

		Result["vertex_count"] = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(DynMesh);
		Result["triangle_count"] = DynMesh->GetTriangleCount();

		FBox BBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(DynMesh);
		sol::table BBoxTable = Lua.create_table();
		BBoxTable["min_x"] = BBox.Min.X; BBoxTable["min_y"] = BBox.Min.Y; BBoxTable["min_z"] = BBox.Min.Z;
		BBoxTable["max_x"] = BBox.Max.X; BBoxTable["max_y"] = BBox.Max.Y; BBoxTable["max_z"] = BBox.Max.Z;
		Result["bounding_box"] = BBoxTable;

		float SurfaceArea = 0, Volume = 0;
		FVector CenterOfMass = FVector::ZeroVector;
		UGeometryScriptLibrary_MeshQueryFunctions::GetMeshVolumeAreaCenter(DynMesh, SurfaceArea, Volume, CenterOfMass);
		Result["surface_area"] = SurfaceArea;
		Result["volume"] = Volume;
		sol::table COM = Lua.create_table();
		COM["x"] = CenterOfMass.X; COM["y"] = CenterOfMass.Y; COM["z"] = CenterOfMass.Z;
		Result["center_of_mass"] = COM;

		Result["is_closed"] = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(DynMesh);
		Result["border_edge_count"] = UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(DynMesh);
		Result["connected_components"] = UGeometryScriptLibrary_MeshQueryFunctions::GetNumConnectedComponents(DynMesh);
		Result["has_attribute_set"] = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshHasAttributeSet(DynMesh);
		Result["is_dense"] = UGeometryScriptLibrary_MeshQueryFunctions::GetIsDenseMesh(DynMesh);
		Result["info"] = TCHAR_TO_UTF8(*UGeometryScriptLibrary_MeshQueryFunctions::GetMeshInfoString(DynMesh));

		bool bAmbiguous = false;
		Result["open_border_loops"] = UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderLoops(DynMesh, bAmbiguous);

		Session.Log(FString::Printf(TEXT("[OK] geometry_query -> %s (%d tris, %.0f area)"),
			*FPath, DynMesh->GetTriangleCount(), SurfaceArea));
		return Result;
	});

	// ================================================================
	// geometry_simplify(mesh_path, opts) — simplify a static mesh
	// ================================================================
	Lua.set_function("geometry_simplify", [&Session](const std::string& MeshPath, sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_simplify"));
		if (!DynMesh) return sol::lua_nil;

		std::string MethodStr = Opts.get_or("method", std::string("to_triangle_count"));
		FString Method = UTF8_TO_TCHAR(MethodStr.c_str());

		int32 TrisBefore = DynMesh->GetTriangleCount();

		if (Method.Equals(TEXT("to_triangle_count"), ESearchCase::IgnoreCase))
		{
			int32 TargetCount = Opts.get_or("target_count", TrisBefore / 2);
			FGeometryScriptSimplifyMeshOptions SimplifyOpts;
			UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(DynMesh, TargetCount, SimplifyOpts);
		}
		else if (Method.Equals(TEXT("to_vertex_count"), ESearchCase::IgnoreCase))
		{
			int32 VertexCount = Opts.get_or("vertex_count", 500);
			FGeometryScriptSimplifyMeshOptions SimplifyOpts;
			UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToVertexCount(DynMesh, VertexCount, SimplifyOpts);
		}
		else if (Method.Equals(TEXT("to_tolerance"), ESearchCase::IgnoreCase))
		{
			float Tolerance = static_cast<float>(Opts.get_or("tolerance", 1.0));
			FGeometryScriptSimplifyMeshOptions SimplifyOpts;
			UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTolerance(DynMesh, Tolerance, SimplifyOpts);
		}
		else if (Method.Equals(TEXT("to_planar"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptPlanarSimplifyOptions PlanarOpts;
			PlanarOpts.AngleThreshold = static_cast<float>(Opts.get_or("angle_threshold", 0.001));
			UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToPlanar(DynMesh, PlanarOpts);
		}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		else if (Method.Equals(TEXT("to_edge_length"), ESearchCase::IgnoreCase))
		{
			double EdgeLength = Opts.get_or("edge_length", 10.0);
			FGeometryScriptSimplifyMeshOptions SimplifyOpts;
			UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToEdgeLength(DynMesh, EdgeLength, SimplifyOpts);
		}
#endif
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_simplify -> unknown method '%s'"), *Method));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometrySimplify", "Geometry Simplify"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_simplify")))
			return sol::lua_nil;

		int32 TrisAfter = DynMesh->GetTriangleCount();
		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["triangles_before"] = TrisBefore;
		Result["triangles_after"] = TrisAfter;
		Session.Log(FString::Printf(TEXT("[OK] geometry_simplify -> %s (%d -> %d tris)"), *FPath, TrisBefore, TrisAfter));
		return Result;
	});

	// ================================================================
	// geometry_repair(mesh_path, opts?) — repair a static mesh
	// ================================================================
	Lua.set_function("geometry_repair", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_repair"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;

		// Default: run all common repair ops
		sol::optional<sol::table> OpsOpt = Opts.get<sol::optional<sol::table>>("operations");
		TArray<FString> Ops;
		if (OpsOpt.has_value())
		{
			sol::table OpsTable = OpsOpt.value();
			for (auto& kv : OpsTable)
			{
				if (kv.second.is<std::string>())
					Ops.Add(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()));
			}
		}
		else
		{
			Ops = { TEXT("weld_edges"), TEXT("fill_holes"), TEXT("resolve_t_junctions"), TEXT("remove_degenerates"), TEXT("compact") };
		}

		float Tolerance = static_cast<float>(Opts.get_or("tolerance", 0.001));
		sol::table Applied = Lua.create_table();
		int Idx = 1;

		for (const FString& Op : Ops)
		{
			if (Op.Equals(TEXT("weld_edges"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptWeldEdgesOptions WeldOpts;
				WeldOpts.Tolerance = Tolerance;
				UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(DynMesh, WeldOpts);
				Applied[Idx++] = "weld_edges";
			}
			else if (Op.Equals(TEXT("fill_holes"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptFillHolesOptions FillOpts;
				int32 Filled = 0, Failed = 0;
				UGeometryScriptLibrary_MeshRepairFunctions::FillAllMeshHoles(DynMesh, FillOpts, Filled, Failed);
				Result["holes_filled"] = Filled;
				Result["holes_failed"] = Failed;
				Applied[Idx++] = "fill_holes";
			}
			else if (Op.Equals(TEXT("resolve_t_junctions"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptResolveTJunctionOptions TJOpts;
				TJOpts.Tolerance = Tolerance;
				UGeometryScriptLibrary_MeshRepairFunctions::ResolveMeshTJunctions(DynMesh, TJOpts);
				Applied[Idx++] = "resolve_t_junctions";
			}
			else if (Op.Equals(TEXT("remove_degenerates"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptDegenerateTriangleOptions DegOpts;
				UGeometryScriptLibrary_MeshRepairFunctions::RepairMeshDegenerateGeometry(DynMesh, DegOpts);
				Applied[Idx++] = "remove_degenerates";
			}
			else if (Op.Equals(TEXT("compact"), ESearchCase::IgnoreCase))
			{
				UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(DynMesh);
				Applied[Idx++] = "compact";
			}
			else if (Op.Equals(TEXT("remove_hidden"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptRemoveHiddenTrianglesOptions HiddenOpts;
				UGeometryScriptLibrary_MeshRepairFunctions::RemoveHiddenTriangles(DynMesh, HiddenOpts);
				Applied[Idx++] = "remove_hidden";
			}
			else if (Op.Equals(TEXT("split_bowties"), ESearchCase::IgnoreCase))
			{
				UGeometryScriptLibrary_MeshRepairFunctions::SplitMeshBowties(DynMesh);
				Applied[Idx++] = "split_bowties";
			}
			else if (Op.Equals(TEXT("remove_small_components"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptRemoveSmallComponentOptions SmallOpts;
				UGeometryScriptLibrary_MeshRepairFunctions::RemoveSmallComponents(DynMesh, SmallOpts);
				Applied[Idx++] = "remove_small_components";
			}
			else if (Op.Equals(TEXT("remove_unused_vertices"), ESearchCase::IgnoreCase))
			{
				UGeometryScriptLibrary_MeshRepairFunctions::RemoveUnusedVertices(DynMesh);
				Applied[Idx++] = "remove_unused_vertices";
			}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			else if (Op.Equals(TEXT("snap_boundaries"), ESearchCase::IgnoreCase))
			{
				FGeometryScriptSnapBoundariesOptions SnapOpts;
				SnapOpts.Tolerance = Tolerance;
				UGeometryScriptLibrary_MeshRepairFunctions::SnapMeshOpenBoundaries(DynMesh, SnapOpts);
				Applied[Idx++] = "snap_boundaries";
			}
#endif
		}

		Result["operations_applied"] = Applied;

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryRepair", "Geometry Repair"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_repair")))
			return sol::lua_nil;

		Result["triangle_count"] = DynMesh->GetTriangleCount();
		Session.Log(FString::Printf(TEXT("[OK] geometry_repair -> %s (%d ops applied)"), *FPath, Idx - 1));
		return Result;
	});

	// ================================================================
	// geometry_normals(mesh_path, opts?) — recompute normals/tangents
	// ================================================================
	Lua.set_function("geometry_normals", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_normals"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		std::string ModeStr = Opts.get_or("mode", std::string("recompute"));
		FString Mode = UTF8_TO_TCHAR(ModeStr.c_str());

		if (Mode.Equals(TEXT("recompute"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptCalculateNormalsOptions CalcOpts;
			UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(DynMesh, CalcOpts);
		}
		else if (Mode.Equals(TEXT("split"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptSplitNormalsOptions SplitOpts;
			SplitOpts.OpeningAngleDeg = static_cast<float>(Opts.get_or("split_angle", 15.0));
			FGeometryScriptCalculateNormalsOptions CalcOpts;
			UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(DynMesh, SplitOpts, CalcOpts);
		}
		else if (Mode.Equals(TEXT("flip"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshNormalsFunctions::FlipNormals(DynMesh);
		}
		else if (Mode.Equals(TEXT("per_vertex"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshNormalsFunctions::SetPerVertexNormals(DynMesh);
		}
		else if (Mode.Equals(TEXT("per_face"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshNormalsFunctions::SetPerFaceNormals(DynMesh);
		}
		else if (Mode.Equals(TEXT("auto_repair"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshNormalsFunctions::AutoRepairNormals(DynMesh);
		}
		else if (Mode.Equals(TEXT("compute_tangents"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptTangentsOptions TangentOpts;
			UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(DynMesh, TangentOpts);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_normals -> unknown mode '%s'"), *Mode));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryNormals", "Geometry Normals"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_normals")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["mode"] = ModeStr;
		Session.Log(FString::Printf(TEXT("[OK] geometry_normals(%s) -> %s"), *Mode, *FPath));
		return Result;
	});

	// ================================================================
	// geometry_remesh(mesh_path, opts?) — uniform remesh
	// ================================================================
	Lua.set_function("geometry_remesh", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_remesh"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();

		int32 TrisBefore = DynMesh->GetTriangleCount();

		FGeometryScriptRemeshOptions RemeshOpts;
		RemeshOpts.RemeshIterations = Opts.get_or("iterations", 20);
		RemeshOpts.SmoothingRate = static_cast<float>(Opts.get_or("smoothing_rate", 0.25));

		FGeometryScriptUniformRemeshOptions UniformOpts;
		sol::optional<double> EdgeLenOpt = Opts.get<sol::optional<double>>("target_edge_length");
		if (EdgeLenOpt.has_value())
		{
			UniformOpts.TargetType = EGeometryScriptUniformRemeshTargetType::TargetEdgeLength;
			UniformOpts.TargetEdgeLength = static_cast<float>(EdgeLenOpt.value());
		}
		else
		{
			UniformOpts.TargetType = EGeometryScriptUniformRemeshTargetType::TriangleCount;
			UniformOpts.TargetTriangleCount = Opts.get_or("target_count", 5000);
		}

		UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(DynMesh, RemeshOpts, UniformOpts);

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryRemesh", "Geometry Remesh"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_remesh")))
			return sol::lua_nil;

		int32 TrisAfter = DynMesh->GetTriangleCount();
		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["triangles_before"] = TrisBefore;
		Result["triangles_after"] = TrisAfter;
		Session.Log(FString::Printf(TEXT("[OK] geometry_remesh -> %s (%d -> %d tris)"), *FPath, TrisBefore, TrisAfter));
		return Result;
	});

	// ================================================================
	// geometry_transform(mesh_path, opts) — transform mesh vertices
	// ================================================================
	Lua.set_function("geometry_transform", [&Session](const std::string& MeshPath, sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_transform"));
		if (!DynMesh) return sol::lua_nil;

		bool bDidTransform = false;

		if (auto TT = Opts.get<sol::optional<sol::table>>("translate"))
		{
			FVector Translation = GS_TableToVector(TT.value());
			UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(DynMesh, Translation);
			bDidTransform = true;
		}
		if (auto RT = Opts.get<sol::optional<sol::table>>("rotate"))
		{
			FRotator Rotation = GS_TableToRotator(RT.value());
			FVector RotOrigin = FVector::ZeroVector;
			if (auto RO = Opts.get<sol::optional<sol::table>>("rotation_origin"))
				RotOrigin = GS_TableToVector(RO.value());
			UGeometryScriptLibrary_MeshTransformFunctions::RotateMesh(DynMesh, Rotation, RotOrigin);
			bDidTransform = true;
		}
		if (auto ST = Opts.get<sol::optional<sol::table>>("scale"))
		{
			FVector Scale = GS_TableToVector(ST.value());
			FVector ScaleOrigin = FVector::ZeroVector;
			if (auto SO = Opts.get<sol::optional<sol::table>>("scale_origin"))
				ScaleOrigin = GS_TableToVector(SO.value());
			UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(DynMesh, Scale, ScaleOrigin);
			bDidTransform = true;
		}
		else
		{
			// Support scalar scale
			sol::optional<double> ScalarScale = Opts.get<sol::optional<double>>("scale");
			if (ScalarScale.has_value())
			{
				double S_val = ScalarScale.value();
				FVector Scale(S_val, S_val, S_val);
				UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(DynMesh, Scale);
				bDidTransform = true;
			}
		}

		if (!bDidTransform)
		{
			Session.Log(TEXT("[FAIL] geometry_transform -> no transform specified (translate, rotate, or scale)"));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryTransform", "Geometry Transform"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_transform")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Session.Log(FString::Printf(TEXT("[OK] geometry_transform -> %s"), *FPath));
		return Result;
	});

	// ================================================================
	// geometry_boolean(target_path, tool_path, operation, opts?) — boolean on two meshes
	// ================================================================
	Lua.set_function("geometry_boolean", [&Session](
		const std::string& TargetPath,
		const std::string& ToolPath,
		const std::string& OperationStr,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FTargetPath = UTF8_TO_TCHAR(TargetPath.c_str());
		FString FToolPath = UTF8_TO_TCHAR(ToolPath.c_str());

		UDynamicMesh* TargetDyn = LoadStaticMeshToDynamic(FTargetPath, Session, TEXT("geometry_boolean"));
		if (!TargetDyn) return sol::lua_nil;
		UDynamicMesh* ToolDyn = LoadStaticMeshToDynamic(FToolPath, Session, TEXT("geometry_boolean"));
		if (!ToolDyn) return sol::lua_nil;

		FString FOp = UTF8_TO_TCHAR(OperationStr.c_str());
		EGeometryScriptBooleanOperation BoolOp = EGeometryScriptBooleanOperation::Subtract;
		if (FOp.Equals(TEXT("union"), ESearchCase::IgnoreCase)) BoolOp = EGeometryScriptBooleanOperation::Union;
		else if (FOp.Equals(TEXT("intersect"), ESearchCase::IgnoreCase)) BoolOp = EGeometryScriptBooleanOperation::Intersection;

		FGeometryScriptMeshBooleanOptions BoolOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
			TargetDyn, FTransform::Identity, ToolDyn, FTransform::Identity, BoolOp, BoolOpts);

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		std::string SavePathStr = Opts.get_or("save", TargetPath);
		FString SavePath = UTF8_TO_TCHAR(SavePathStr.c_str());

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryBoolean", "Geometry Boolean"));
		if (!SaveDynamicMeshToAsset(TargetDyn, SavePath, Session, TEXT("geometry_boolean")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = SavePathStr;
		Result["operation"] = OperationStr;
		Result["triangle_count"] = TargetDyn->GetTriangleCount();
		Session.Log(FString::Printf(TEXT("[OK] geometry_boolean(%s) -> %s"), *FOp, *SavePath));
		return Result;
	});

	// ================================================================
	// geometry_plane_cut(mesh_path, opts) — cut/slice/mirror with a plane
	// ================================================================
	Lua.set_function("geometry_plane_cut", [&Session](const std::string& MeshPath, sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_plane_cut"));
		if (!DynMesh) return sol::lua_nil;

		std::string ModeStr = Opts.get_or("mode", std::string("cut"));
		FString Mode = UTF8_TO_TCHAR(ModeStr.c_str());

		FTransform CutFrame = FTransform::Identity;
		if (auto OT = Opts.get<sol::optional<sol::table>>("origin"))
			CutFrame.SetLocation(GS_TableToVector(OT.value()));
		if (auto NT = Opts.get<sol::optional<sol::table>>("normal"))
		{
			FVector Normal = GS_TableToVector(NT.value()).GetSafeNormal();
			if (!Normal.IsNearlyZero())
			{
				FQuat Rot = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
				CutFrame.SetRotation(Rot);
			}
		}

		if (Mode.Equals(TEXT("cut"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptMeshPlaneCutOptions CutOpts;
			CutOpts.bFillHoles = Opts.get_or("fill_holes", true);
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneCut(DynMesh, CutFrame, CutOpts);
		}
		else if (Mode.Equals(TEXT("slice"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptMeshPlaneSliceOptions SliceOpts;
			SliceOpts.bFillHoles = Opts.get_or("fill_holes", true);
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneSlice(DynMesh, CutFrame, SliceOpts);
		}
		else if (Mode.Equals(TEXT("mirror"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptMeshMirrorOptions MirrorOpts;
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshMirror(DynMesh, CutFrame, MirrorOpts);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_plane_cut -> unknown mode '%s' (cut|slice|mirror)"), *Mode));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryPlaneCut", "Geometry Plane Cut"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_plane_cut")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["mode"] = ModeStr;
		Result["triangle_count"] = DynMesh->GetTriangleCount();
		Session.Log(FString::Printf(TEXT("[OK] geometry_plane_cut(%s) -> %s"), *Mode, *FPath));
		return Result;
	});

	// ================================================================
	// geometry_subdivide(mesh_path, opts?) — tessellate/subdivide
	// ================================================================
	Lua.set_function("geometry_subdivide", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_subdivide"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		std::string MethodStr = Opts.get_or("method", std::string("uniform"));
		FString Method = UTF8_TO_TCHAR(MethodStr.c_str());
		int32 Level = Opts.get_or("level", 3);

		int32 TrisBefore = DynMesh->GetTriangleCount();

		if (Method.Equals(TEXT("uniform"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyUniformTessellation(DynMesh, Level);
		}
		else if (Method.Equals(TEXT("pn"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptPNTessellateOptions PNOpts;
			UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(DynMesh, PNOpts, Level);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_subdivide -> unknown method '%s' (uniform|pn)"), *Method));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometrySubdivide", "Geometry Subdivide"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_subdivide")))
			return sol::lua_nil;

		int32 TrisAfter = DynMesh->GetTriangleCount();
		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["method"] = MethodStr;
		Result["triangles_before"] = TrisBefore;
		Result["triangles_after"] = TrisAfter;
		Session.Log(FString::Printf(TEXT("[OK] geometry_subdivide(%s, level=%d) -> %s (%d -> %d tris)"),
			*Method, Level, *FPath, TrisBefore, TrisAfter));
		return Result;
	});

	// ================================================================
	// geometry_uvs(mesh_path, opts?) — UV operations
	// ================================================================
	Lua.set_function("geometry_uvs", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_uvs"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		std::string ModeStr = Opts.get_or("mode", std::string("recompute"));
		FString Mode = UTF8_TO_TCHAR(ModeStr.c_str());
		int32 UVChannel = Opts.get_or("uv_channel", 0);

		if (Mode.Equals(TEXT("recompute"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptRecomputeUVsOptions RecompOpts;
			FGeometryScriptMeshSelection EmptySelection;
			UGeometryScriptLibrary_MeshUVFunctions::RecomputeMeshUVs(DynMesh, UVChannel, RecompOpts, EmptySelection);
		}
		else if (Mode.Equals(TEXT("repack"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptRepackUVsOptions RepackOpts;
			RepackOpts.TargetImageWidth = Opts.get_or("resolution", 512);
			UGeometryScriptLibrary_MeshUVFunctions::RepackMeshUVs(DynMesh, UVChannel, RepackOpts);
		}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		else if (Mode.Equals(TEXT("layout"), ESearchCase::IgnoreCase))
		{
			FGeometryScriptLayoutUVsOptions LayoutOpts;
			LayoutOpts.TextureResolution = Opts.get_or("resolution", 1024);
			FGeometryScriptMeshSelection EmptySelection;
			UGeometryScriptLibrary_MeshUVFunctions::LayoutMeshUVs(DynMesh, UVChannel, LayoutOpts, EmptySelection);
		}
#endif
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_uvs -> unknown mode '%s' (recompute|repack|layout)"), *Mode));
			return sol::lua_nil;
		}

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryUVs", "Geometry UVs"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_uvs")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["mode"] = ModeStr;
		Result["uv_channel"] = UVChannel;
		Session.Log(FString::Printf(TEXT("[OK] geometry_uvs(%s, ch=%d) -> %s"), *Mode, UVChannel, *FPath));
		return Result;
	});

	// ================================================================
	// geometry_materials(mesh_path, verb, opts?) — material ID operations
	// ================================================================
	Lua.set_function("geometry_materials", [&Session](const std::string& MeshPath, const std::string& Verb, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());
		FString FVerb = UTF8_TO_TCHAR(Verb.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_materials"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();
		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		bool bNeedsSave = false;

		if (FVerb.Equals(TEXT("info"), ESearchCase::IgnoreCase))
		{
			bool bHasMaterialIDs = false;
			int32 MaxID = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(DynMesh, bHasMaterialIDs);
			Result["has_material_ids"] = bHasMaterialIDs;
			Result["max_material_id"] = MaxID;
			Session.Log(FString::Printf(TEXT("[OK] geometry_materials(info) -> %s (has_ids=%s, max=%d)"),
				*FPath, bHasMaterialIDs ? TEXT("true") : TEXT("false"), MaxID));
		}
		else if (FVerb.Equals(TEXT("remap"), ESearchCase::IgnoreCase))
		{
			int32 FromID = Opts.get_or("from_id", 0);
			int32 ToID = Opts.get_or("to_id", 0);
			UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(DynMesh, FromID, ToID);
			bNeedsSave = true;
			Session.Log(FString::Printf(TEXT("[OK] geometry_materials(remap) -> %s (%d -> %d)"), *FPath, FromID, ToID));
		}
		else if (FVerb.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
		{
			int32 ClearVal = Opts.get_or("clear_value", 0);
			UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(DynMesh, ClearVal);
			bNeedsSave = true;
			Session.Log(FString::Printf(TEXT("[OK] geometry_materials(clear) -> %s (value=%d)"), *FPath, ClearVal));
		}
		else if (FVerb.Equals(TEXT("delete_by_id"), ESearchCase::IgnoreCase))
		{
			int32 MatID = Opts.get_or("material_id", 0);
			int32 NumDeleted = 0;
			UGeometryScriptLibrary_MeshMaterialFunctions::DeleteTrianglesByMaterialID(DynMesh, MatID, NumDeleted);
			Result["deleted_triangles"] = NumDeleted;
			bNeedsSave = true;
			Session.Log(FString::Printf(TEXT("[OK] geometry_materials(delete_by_id) -> %s (id=%d, deleted=%d)"), *FPath, MatID, NumDeleted));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] geometry_materials -> unknown verb '%s' (info|remap|clear|delete_by_id)"), *FVerb));
			return sol::lua_nil;
		}

		if (bNeedsSave)
		{
			FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometryMaterials", "Geometry Materials"));
			if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_materials")))
				return sol::lua_nil;
		}

		return Result;
	});

	// ================================================================
	// geometry_smooth(mesh_path, opts?) — apply smoothing
	// ================================================================
	Lua.set_function("geometry_smooth", [&Session](const std::string& MeshPath, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(MeshPath.c_str());

		UDynamicMesh* DynMesh = LoadStaticMeshToDynamic(FPath, Session, TEXT("geometry_smooth"));
		if (!DynMesh) return sol::lua_nil;

		sol::table Opts = OptsOpt.has_value() ? OptsOpt.value() : Lua.create_table();

		FGeometryScriptIterativeMeshSmoothingOptions SmoothOpts;
		SmoothOpts.NumIterations = Opts.get_or("iterations", 10);
		SmoothOpts.Alpha = static_cast<float>(Opts.get_or("speed", 0.25));

		FGeometryScriptMeshSelection EmptySelection;
		UGeometryScriptLibrary_MeshDeformFunctions::ApplyIterativeSmoothingToMesh(DynMesh, EmptySelection, SmoothOpts);

		FScopedTransaction Txn(NSLOCTEXT("AIK", "GeometrySmooth", "Geometry Smooth"));
		if (!SaveDynamicMeshToAsset(DynMesh, FPath, Session, TEXT("geometry_smooth")))
			return sol::lua_nil;

		sol::table Result = Lua.create_table();
		Result["path"] = MeshPath;
		Result["triangle_count"] = DynMesh->GetTriangleCount();
		Session.Log(FString::Printf(TEXT("[OK] geometry_smooth -> %s"), *FPath));
		return Result;
	});
}

REGISTER_LUA_BINDING(GeometryScripting, GeometryScriptingDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindGeometryScripting(Lua, Session);
});
