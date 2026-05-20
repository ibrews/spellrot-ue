// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/StaticMeshSourceData.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/BodySetup.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* CollisionTraceFlagToString(ECollisionTraceFlag Flag)
{
	switch (Flag)
	{
	case CTF_UseDefault:            return "Default";
	case CTF_UseSimpleAndComplex:   return "SimpleAndComplex";
	case CTF_UseSimpleAsComplex:    return "SimpleAsComplex";
	case CTF_UseComplexAsSimple:    return "ComplexAsSimple";
	default:                        return "Unknown";
	}
}

static void AddLODGroupInfo(UStaticMesh* Mesh, sol::table& Result)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	const FName LODGroup = Mesh->GetLODGroup();
	if (!LODGroup.IsNone())
	{
		Result["lod_group"] = TCHAR_TO_UTF8(*LODGroup.ToString());
	}
#else
	(void)Mesh;
	(void)Result;
#endif
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> StaticMeshDocs = {};

static void BindStaticMesh(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_static_mesh", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FPath);
		if (!Mesh) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"StaticMesh enrichment.\n"
			"\n"
			"info() — comprehensive mesh summary (LODs, bounds, materials, collision, Nanite, etc.)\n"
			"\n"
			"list(type):\n"
			"  list(\"lods\")       — detailed per-LOD info (vertices, triangles, sections, UV channels, screen_size, build+reduction settings)\n"
			"  list(\"materials\")  — material slots with paths and names\n"
			"  list(\"sockets\")    — sockets with location, rotation, scale, tag\n"
			"  list(\"collision\")  — collision shape counts and trace flag\n"
			"  list(\"nanite\")     — Nanite settings (if enabled)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"lod\")                  — add a new LOD level (copies reduction settings from previous LOD)\n"
			"  add(\"socket\", {name, bone?, location?, rotation?, scale?, tag?})\n"
			"  add(\"collision\", {type=\"box\"|\"sphere\"|\"capsule\"|\"tapered_capsule\", center?, rotation?, ...})\n"
			"    box: {x, y, z}  sphere: {radius}  capsule: {radius, length}  tapered_capsule: {radius0, radius1, length}\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"lod\", index)        — remove LOD at index (cannot remove LOD 0)\n"
			"  remove(\"socket\", name)      — remove socket by name\n"
			"  remove(\"collision\", {type, index})  — remove collision shape\n"
			"  remove(\"material\")          — remove unused trailing material slots\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"nanite\", {enabled, keep_percent_triangles?, trim_relative_error?, max_edge_length_factor?,\n"
			"       separable?, voxel_ndf?, voxel_opacity?, shape_preservation?, tangent_precision?, bone_weight_precision?, ...})\n"
			"  configure(\"lod\", index, {screen_size?, reduction={percent_triangles?, max_deviation?,\n"
			"       termination_criterion=\"triangles\"|\"vertices\"|\"any\",\n"
			"       silhouette_importance?, texture_importance?, shading_importance?,\n"
			"       visibility_aggressiveness?, vertex_color_importance? (off/lowest/low/normal/high/highest)}, build={...}})\n"
			"  configure(\"material\", index, {material=\"/Path/To/Material\", slot_name?})\n"
			"  configure(\"collision\", {type, index, center?, rotation?, radius?, ...})\n"
			"  configure(\"socket\", name, {location?, rotation?, scale?, tag?})\n"
			"  configure(\"physics\", {collision_trace_flag?, generate_distance_field?, lod_for_collision?})\n"
			"  configure(\"lightmap\", {resolution?, uv_index?})\n"
			"  configure(\"lod_group\", {name?})  — set or clear LOD group\n"
			"  configure(\"min_lod\", {index})    — set minimum LOD index\n"
			"\n"
			"build() — rebuild mesh after changes (REQUIRED after configure/add/remove)\n";

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [Mesh, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("lods") ----
			if (FType.Equals(TEXT("lods"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
				const int32 NumSourceModels = Mesh->GetNumSourceModels();
				if (RenderData)
				{
					for (int32 i = 0; i < RenderData->LODResources.Num(); i++)
					{
						const FStaticMeshLODResources& LODRes = RenderData->LODResources[i];
						sol::table E = Lua.create_table();
						E["index"] = i;
						E["vertices"] = LODRes.GetNumVertices();
						E["triangles"] = LODRes.GetNumTriangles();
						E["sections"] = static_cast<int>(LODRes.Sections.Num());
						E["uv_channels"] = LODRes.GetNumTexCoords();

						// Screen size from render data
						if (i < MAX_STATIC_MESH_LODS)
						{
							E["screen_size"] = RenderData->ScreenSize[i].GetDefault();
						}

						// Build settings from source model (editor data)
						if (i < NumSourceModels && Mesh->IsSourceModelValid(i))
						{
							const FStaticMeshSourceModel& SrcModel = Mesh->GetSourceModel(i);
							const FMeshBuildSettings& BS = SrcModel.BuildSettings;

							sol::table BuildT = Lua.create_table();
							BuildT["recompute_normals"] = static_cast<bool>(BS.bRecomputeNormals);
							BuildT["recompute_tangents"] = static_cast<bool>(BS.bRecomputeTangents);
							BuildT["use_mikk_tangent_space"] = static_cast<bool>(BS.bUseMikkTSpace);
							BuildT["remove_degenerates"] = static_cast<bool>(BS.bRemoveDegenerates);
							BuildT["compute_weighted_normals"] = static_cast<bool>(BS.bComputeWeightedNormals);
							BuildT["build_reversed_index_buffer"] = static_cast<bool>(BS.bBuildReversedIndexBuffer);
							BuildT["use_high_precision_tangent_basis"] = static_cast<bool>(BS.bUseHighPrecisionTangentBasis);
							BuildT["use_full_precision_uvs"] = static_cast<bool>(BS.bUseFullPrecisionUVs);
							BuildT["generate_lightmap_uvs"] = static_cast<bool>(BS.bGenerateLightmapUVs);
							BuildT["generate_distance_field_as_if_two_sided"] = static_cast<bool>(BS.bGenerateDistanceFieldAsIfTwoSided);
							BuildT["min_lightmap_resolution"] = BS.MinLightmapResolution;
							BuildT["src_lightmap_index"] = BS.SrcLightmapIndex;
							BuildT["dst_lightmap_index"] = BS.DstLightmapIndex;
							E["build_settings"] = BuildT;

							// Reduction settings
							const FMeshReductionSettings& RS = SrcModel.ReductionSettings;
							sol::table RedT = Lua.create_table();
							RedT["percent_triangles"] = RS.PercentTriangles;
							RedT["percent_vertices"] = RS.PercentVertices;
							RedT["max_deviation"] = RS.MaxDeviation;
							RedT["pixel_error"] = RS.PixelError;
							RedT["welding_threshold"] = RS.WeldingThreshold;
							RedT["hard_angle_threshold"] = RS.HardAngleThreshold;
							RedT["base_lod_model"] = RS.BaseLODModel;
							RedT["max_num_of_triangles"] = static_cast<int>(RS.MaxNumOfTriangles);
							RedT["max_num_of_verts"] = static_cast<int>(RS.MaxNumOfVerts);
							RedT["recalculate_normals"] = static_cast<bool>(RS.bRecalculateNormals);
							switch (RS.TerminationCriterion)
							{
							case EStaticMeshReductionTerimationCriterion::Triangles: RedT["termination_criterion"] = "triangles"; break;
							case EStaticMeshReductionTerimationCriterion::Vertices:  RedT["termination_criterion"] = "vertices"; break;
							case EStaticMeshReductionTerimationCriterion::Any:       RedT["termination_criterion"] = "any"; break;
							}
							E["reduction_settings"] = RedT;

							// Source model screen size (may differ from render data)
							E["source_screen_size"] = SrcModel.ScreenSize.GetDefault();
						}

						// Per-section details
						sol::table SectionsTable = Lua.create_table();
						for (int32 s = 0; s < LODRes.Sections.Num(); s++)
						{
							const FStaticMeshSection& Sec = LODRes.Sections[s];
							sol::table SecT = Lua.create_table();
							SecT["index"] = s;
							SecT["material_index"] = Sec.MaterialIndex;
							SecT["triangles"] = static_cast<int>(Sec.NumTriangles);
							SecT["collision_enabled"] = Sec.bEnableCollision;
							SecT["casts_shadow"] = Sec.bCastShadow;
							SecT["visible_in_ray_tracing"] = Sec.bVisibleInRayTracing;
							SectionsTable[s + 1] = SecT;
						}
						E["section_details"] = SectionsTable;

						Result[i + 1] = E;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"lods\") -> %d LODs"), RenderData ? RenderData->LODResources.Num() : 0));
				return Result;
			}

			// ---- list("materials") ----
			if (FType.Equals(TEXT("materials"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Materials.Num(); i++)
				{
					const FStaticMaterial& Mat = Materials[i];
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["slot_name"] = TCHAR_TO_UTF8(*Mat.MaterialSlotName.ToString());
					if (Mat.MaterialInterface)
					{
						E["material_path"] = TCHAR_TO_UTF8(*Mat.MaterialInterface->GetPathName());
						E["material_name"] = TCHAR_TO_UTF8(*Mat.MaterialInterface->GetName());
					}
					else
					{
						E["material_path"] = "";
						E["material_name"] = "None";
					}
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"materials\") -> %d"), Materials.Num()));
				return Result;
			}

			// ---- list("sockets") ----
			if (FType.Equals(TEXT("sockets"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<TObjectPtr<UStaticMeshSocket>>& Sockets = Mesh->Sockets;
				for (int32 i = 0; i < Sockets.Num(); i++)
				{
					UStaticMeshSocket* Socket = Sockets[i];
					if (!Socket) continue;

					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());

					sol::table Loc = Lua.create_table();
					Loc["x"] = Socket->RelativeLocation.X;
					Loc["y"] = Socket->RelativeLocation.Y;
					Loc["z"] = Socket->RelativeLocation.Z;
					E["location"] = Loc;

					sol::table Rot = Lua.create_table();
					Rot["pitch"] = Socket->RelativeRotation.Pitch;
					Rot["yaw"] = Socket->RelativeRotation.Yaw;
					Rot["roll"] = Socket->RelativeRotation.Roll;
					E["rotation"] = Rot;

					sol::table Scale = Lua.create_table();
					Scale["x"] = Socket->RelativeScale.X;
					Scale["y"] = Socket->RelativeScale.Y;
					Scale["z"] = Socket->RelativeScale.Z;
					E["scale"] = Scale;

					E["tag"] = TCHAR_TO_UTF8(*Socket->Tag);

					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sockets\") -> %d"), Sockets.Num()));
				return Result;
			}

			// ---- list("collision") ----
			if (FType.Equals(TEXT("collision"), ESearchCase::IgnoreCase))
			{
				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup)
				{
					Session.Log(TEXT("[OK] list(\"collision\") -> no collision setup"));
					sol::table Result = Lua.create_table();
					Result["has_collision"] = false;
					return sol::make_object(Lua, Result);
				}

				sol::table Result = Lua.create_table();
				Result["has_collision"] = true;
				Result["total_shapes"] = BodySetup->AggGeom.GetElementCount();
				Result["boxes"] = static_cast<int>(BodySetup->AggGeom.BoxElems.Num());
				Result["spheres"] = static_cast<int>(BodySetup->AggGeom.SphereElems.Num());
				Result["capsules"] = static_cast<int>(BodySetup->AggGeom.SphylElems.Num());
				Result["convex_elements"] = static_cast<int>(BodySetup->AggGeom.ConvexElems.Num());
				Result["trace_flag"] = CollisionTraceFlagToString(BodySetup->GetCollisionTraceFlag());

				Session.Log(FString::Printf(TEXT("[OK] list(\"collision\") -> %d shapes, flag=%s"),
					BodySetup->AggGeom.GetElementCount(),
					UTF8_TO_TCHAR(CollisionTraceFlagToString(BodySetup->GetCollisionTraceFlag()))));
				return Result;
			}

			// ---- list("nanite") ----
			if (FType.Equals(TEXT("nanite"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::table Result = Lua.create_table();
				Result["enabled"] = Mesh->IsNaniteEnabled();

				if (Mesh->IsNaniteEnabled())
				{
					const FMeshNaniteSettings& NS = Mesh->GetNaniteSettings();
					Result["explicit_tangents"] = static_cast<bool>(NS.bExplicitTangents);
					Result["lerp_uvs"] = static_cast<bool>(NS.bLerpUVs);
					Result["separable"] = static_cast<bool>(NS.bSeparable);
					Result["voxel_ndf"] = static_cast<bool>(NS.bVoxelNDF);
					Result["voxel_opacity"] = static_cast<bool>(NS.bVoxelOpacity);
					Result["keep_percent_triangles"] = NS.KeepPercentTriangles;
					Result["trim_relative_error"] = NS.TrimRelativeError;
					Result["fallback_percent_triangles"] = NS.FallbackPercentTriangles;
					Result["fallback_relative_error"] = NS.FallbackRelativeError;
					Result["max_edge_length_factor"] = NS.MaxEdgeLengthFactor;
					Result["position_precision"] = NS.PositionPrecision;
					Result["normal_precision"] = NS.NormalPrecision;
					Result["tangent_precision"] = NS.TangentPrecision;
					Result["bone_weight_precision"] = NS.BoneWeightPrecision;
					Result["target_minimum_residency_kb"] = static_cast<int>(NS.TargetMinimumResidencyInKB);
					switch (NS.ShapePreservation)
					{
					case ENaniteShapePreservation::None:         Result["shape_preservation"] = "none"; break;
					case ENaniteShapePreservation::PreserveArea: Result["shape_preservation"] = "preserve_area"; break;
					case ENaniteShapePreservation::Voxelize:     Result["shape_preservation"] = "voxelize"; break;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"nanite\") -> enabled=%s"),
					Mesh->IsNaniteEnabled() ? TEXT("true") : TEXT("false")));
				return Result;
#else
				sol::table Result = Lua.create_table();
				Result["error"] = "Nanite settings requires UE 5.7+";
				return Result;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: lods, materials, sockets, collision, nanite"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Mesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] add -> asset no longer valid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());

			// ---- add("lod") ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				Mesh->Modify();
				int32 NewIdx = Mesh->GetNumSourceModels();
				Mesh->SetNumSourceModels(NewIdx + 1);
				// Copy reduction settings from previous LOD if available
				if (NewIdx > 0 && Mesh->IsSourceModelValid(NewIdx - 1))
				{
					FStaticMeshSourceModel& PrevModel = Mesh->GetSourceModel(NewIdx - 1);
					FStaticMeshSourceModel& NewModel = Mesh->GetSourceModel(NewIdx);
					NewModel.ReductionSettings = PrevModel.ReductionSettings;
					NewModel.ReductionSettings.PercentTriangles = FMath::Max(0.1f, PrevModel.ReductionSettings.PercentTriangles * 0.5f);
					NewModel.ScreenSize = FPerPlatformFloat(FMath::Max(0.01f, PrevModel.ScreenSize.GetDefault() * 0.5f));
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"lod\") -> LOD %d added (total: %d)"), NewIdx, Mesh->GetNumSourceModels()));
				sol::table R = Lua.create_table();
				R["index"] = NewIdx;
				R["total_lods"] = Mesh->GetNumSourceModels();
				return R;
			}

			// ---- add("socket", {name, location?, rotation?, scale?, tag?}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> params required: {name}")); return sol::lua_nil; }
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> 'name' is required")); return sol::lua_nil; }

				FName SocketFName(UTF8_TO_TCHAR(Name.c_str()));
				if (Mesh->FindSocket(SocketFName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"socket\") -> socket \"%s\" already exists"), *SocketFName.ToString()));
					return sol::lua_nil;
				}

				Mesh->Modify();
				UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
				Socket->SocketName = SocketFName;

				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value())
				{
					Socket->RelativeLocation.X = LocOpt.value().get_or("x", 0.0);
					Socket->RelativeLocation.Y = LocOpt.value().get_or("y", 0.0);
					Socket->RelativeLocation.Z = LocOpt.value().get_or("z", 0.0);
				}
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					Socket->RelativeRotation.Pitch = RotOpt.value().get_or("pitch", 0.0);
					Socket->RelativeRotation.Yaw = RotOpt.value().get_or("yaw", 0.0);
					Socket->RelativeRotation.Roll = RotOpt.value().get_or("roll", 0.0);
				}
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value())
				{
					Socket->RelativeScale.X = ScaleOpt.value().get_or("x", 1.0);
					Socket->RelativeScale.Y = ScaleOpt.value().get_or("y", 1.0);
					Socket->RelativeScale.Z = ScaleOpt.value().get_or("z", 1.0);
				}
				Socket->Tag = UTF8_TO_TCHAR(P.get_or<std::string>("tag", "").c_str());

				Mesh->AddSocket(Socket);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\") -> \"%s\""), *Socket->SocketName.ToString()));
				sol::table R = Lua.create_table();
				R["name"] = Name;
				return R;
			}

			// ---- add("collision", {type, center?, rotation?, ...}) ----
			if (FType.Equals(TEXT("collision"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"collision\") -> params required: {type}")); return sol::lua_nil; }
				sol::table P = ParamsOpt.value();
				FString ShapeType = UTF8_TO_TCHAR(P.get_or<std::string>("type", "box").c_str());

				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup)
				{
					Mesh->CreateBodySetup();
					BodySetup = Mesh->GetBodySetup();
				}
				if (!BodySetup) { Session.Log(TEXT("[FAIL] add(\"collision\") -> could not create BodySetup")); return sol::lua_nil; }

				Mesh->Modify();
				BodySetup->Modify();

				FVector Center(0, 0, 0);
				sol::optional<sol::table> CenterOpt = P.get<sol::optional<sol::table>>("center");
				if (CenterOpt.has_value())
				{
					Center.X = CenterOpt.value().get_or("x", 0.0);
					Center.Y = CenterOpt.value().get_or("y", 0.0);
					Center.Z = CenterOpt.value().get_or("z", 0.0);
				}
				FRotator Rotation(0, 0, 0);
				sol::optional<sol::table> RotOpt2 = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt2.has_value())
				{
					Rotation.Pitch = RotOpt2.value().get_or("pitch", 0.0);
					Rotation.Yaw = RotOpt2.value().get_or("yaw", 0.0);
					Rotation.Roll = RotOpt2.value().get_or("roll", 0.0);
				}

				int32 NewIndex = -1;
				if (ShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase))
				{
					FKBoxElem Box;
					Box.Center = Center;
					Box.Rotation = Rotation;
					Box.X = P.get_or("x", 32.0);
					Box.Y = P.get_or("y", 32.0);
					Box.Z = P.get_or("z", 32.0);
					NewIndex = BodySetup->AggGeom.BoxElems.Add(Box);
				}
				else if (ShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
				{
					FKSphereElem Sphere;
					Sphere.Center = Center;
					Sphere.Radius = P.get_or("radius", 16.0);
					NewIndex = BodySetup->AggGeom.SphereElems.Add(Sphere);
				}
				else if (ShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
				{
					FKSphylElem Capsule;
					Capsule.Center = Center;
					Capsule.Rotation = Rotation;
					Capsule.Radius = P.get_or("radius", 16.0);
					Capsule.Length = P.get_or("length", 32.0);
					NewIndex = BodySetup->AggGeom.SphylElems.Add(Capsule);
				}
				else if (ShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase))
				{
					FKTaperedCapsuleElem TapCap;
					TapCap.Center = Center;
					TapCap.Rotation = Rotation;
					TapCap.Radius0 = P.get_or("radius0", 16.0);
					TapCap.Radius1 = P.get_or("radius1", 8.0);
					TapCap.Length = P.get_or("length", 32.0);
					NewIndex = BodySetup->AggGeom.TaperedCapsuleElems.Add(TapCap);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"collision\") -> unknown shape type \"%s\". Use: box, sphere, capsule, tapered_capsule"), *ShapeType));
					return sol::lua_nil;
				}

				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"collision\") -> %s at index %d"), *ShapeType, NewIndex));
				sol::table R = Lua.create_table();
				R["type"] = TCHAR_TO_UTF8(*ShapeType);
				R["index"] = NewIndex;
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: lod, socket, collision"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [Mesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::object IdObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] remove -> asset no longer valid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());

			// ---- remove("lod", index) ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				int32 Index = IdObj.as<int32>();
				if (Index <= 0) { Session.Log(TEXT("[FAIL] remove(\"lod\") -> cannot remove LOD 0")); return sol::lua_nil; }
				if (Index >= Mesh->GetNumSourceModels()) { Session.Log(TEXT("[FAIL] remove(\"lod\") -> index out of range")); return sol::lua_nil; }
				Mesh->Modify();
				Mesh->RemoveSourceModel(Index);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"lod\", %d) -> remaining: %d"), Index, Mesh->GetNumSourceModels()));
				sol::table R = Lua.create_table();
				R["removed"] = Index;
				R["total_lods"] = Mesh->GetNumSourceModels();
				return R;
			}

			// ---- remove("socket", name) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = UTF8_TO_TCHAR(IdObj.as<std::string>().c_str());
				UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"socket\") -> \"%s\" not found"), *SocketName)); return sol::lua_nil; }
				Mesh->Modify();
				Mesh->RemoveSocket(Socket);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["removed"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- remove("collision", {type, index}) ----
			if (FType.Equals(TEXT("collision"), ESearchCase::IgnoreCase))
			{
				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup) { Session.Log(TEXT("[FAIL] remove(\"collision\") -> no collision setup")); return sol::lua_nil; }

				sol::table P = IdObj.as<sol::table>();
				FString ShapeType = UTF8_TO_TCHAR(P.get_or<std::string>("type", "").c_str());
				int32 Index = P.get_or("index", 0);

				if (Index < 0) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"collision\") -> negative index %d"), Index)); return sol::lua_nil; }

				Mesh->Modify();
				BodySetup->Modify();

				bool bRemoved = false;
				if (ShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.BoxElems.Num())
				{
					BodySetup->AggGeom.BoxElems.RemoveAt(Index); bRemoved = true;
				}
				else if (ShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.SphereElems.Num())
				{
					BodySetup->AggGeom.SphereElems.RemoveAt(Index); bRemoved = true;
				}
				else if (ShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.SphylElems.Num())
				{
					BodySetup->AggGeom.SphylElems.RemoveAt(Index); bRemoved = true;
				}
				else if (ShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.TaperedCapsuleElems.Num())
				{
					BodySetup->AggGeom.TaperedCapsuleElems.RemoveAt(Index); bRemoved = true;
				}

				if (!bRemoved) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"collision\") -> %s[%d] not found"), *ShapeType, Index)); return sol::lua_nil; }

				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"collision\") -> %s[%d]"), *ShapeType, Index));
				sol::table R = Lua.create_table();
				R["removed"] = true;
				return R;
			}

			// ---- remove("material") — remove unused trailing material slots ----
			if (FType.Equals(TEXT("material"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("materials"), ESearchCase::IgnoreCase))
			{
				int32 Before = Mesh->GetStaticMaterials().Num();
				Mesh->Modify();
				UStaticMesh::RemoveUnusedMaterialSlots(Mesh);
				int32 After = Mesh->GetStaticMaterials().Num();
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"material\") -> removed %d unused slots (%d -> %d)"), Before - After, Before, After));
				sol::table R = Lua.create_table();
				R["removed_count"] = Before - After;
				R["total_materials"] = After;
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: lod, socket, collision, material"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, id_or_params, params?)
		// ================================================================
		AssetObj.set_function("configure", [Mesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::object Arg2, sol::optional<sol::table> Arg3, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] configure -> asset no longer valid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());

			// ---- configure("nanite", {enabled, ...}) ----
			if (FType.Equals(TEXT("nanite"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::table P = Arg2.as<sol::table>();
				FMeshNaniteSettings NS = Mesh->GetNaniteSettings();

				if (P.get<sol::optional<bool>>("enabled").has_value()) NS.bEnabled = P["enabled"];
				if (P.get<sol::optional<bool>>("explicit_tangents").has_value()) NS.bExplicitTangents = P["explicit_tangents"];
				if (P.get<sol::optional<bool>>("lerp_uvs").has_value()) NS.bLerpUVs = P["lerp_uvs"];
				if (P.get<sol::optional<bool>>("separable").has_value()) NS.bSeparable = P["separable"];
				if (P.get<sol::optional<bool>>("voxel_ndf").has_value()) NS.bVoxelNDF = P["voxel_ndf"];
				if (P.get<sol::optional<bool>>("voxel_opacity").has_value()) NS.bVoxelOpacity = P["voxel_opacity"];
				if (P.get<sol::optional<double>>("keep_percent_triangles").has_value()) NS.KeepPercentTriangles = P["keep_percent_triangles"];
				if (P.get<sol::optional<double>>("trim_relative_error").has_value()) NS.TrimRelativeError = P["trim_relative_error"];
				if (P.get<sol::optional<double>>("fallback_percent_triangles").has_value()) NS.FallbackPercentTriangles = P["fallback_percent_triangles"];
				if (P.get<sol::optional<double>>("fallback_relative_error").has_value()) NS.FallbackRelativeError = P["fallback_relative_error"];
				if (P.get<sol::optional<double>>("max_edge_length_factor").has_value()) NS.MaxEdgeLengthFactor = P["max_edge_length_factor"];
				if (P.get<sol::optional<int>>("position_precision").has_value()) NS.PositionPrecision = P["position_precision"];
				if (P.get<sol::optional<int>>("normal_precision").has_value()) NS.NormalPrecision = P["normal_precision"];
				if (P.get<sol::optional<int>>("tangent_precision").has_value()) NS.TangentPrecision = P["tangent_precision"];
				if (P.get<sol::optional<int>>("bone_weight_precision").has_value()) NS.BoneWeightPrecision = P["bone_weight_precision"];
				if (P.get<sol::optional<int>>("target_minimum_residency_kb").has_value()) NS.TargetMinimumResidencyInKB = P["target_minimum_residency_kb"];

				sol::optional<std::string> ShapePresOpt = P.get<sol::optional<std::string>>("shape_preservation");
				if (ShapePresOpt.has_value())
				{
					FString SP = UTF8_TO_TCHAR(ShapePresOpt.value().c_str());
					if (SP.Equals(TEXT("none"), ESearchCase::IgnoreCase)) NS.ShapePreservation = ENaniteShapePreservation::None;
					else if (SP.Equals(TEXT("preserve_area"), ESearchCase::IgnoreCase)) NS.ShapePreservation = ENaniteShapePreservation::PreserveArea;
					else if (SP.Equals(TEXT("voxelize"), ESearchCase::IgnoreCase)) NS.ShapePreservation = ENaniteShapePreservation::Voxelize;
				}

				Mesh->SetNaniteSettings(NS);
				Mesh->NotifyNaniteSettingsChanged();
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"nanite\") -> enabled=%s"), NS.bEnabled ? TEXT("true") : TEXT("false")));
				sol::table R = Lua.create_table();
				R["enabled"] = static_cast<bool>(NS.bEnabled);
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "Nanite settings requires UE 5.7+";
				return R;
#endif
			}

			// ---- configure("lod", index, {screen_size?, reduction?, build?}) ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"lod\") -> params table required as 3rd argument")); return sol::lua_nil; }
				sol::table P = Arg3.value();

				if (Index < 0 || Index >= Mesh->GetNumSourceModels())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"lod\", %d) -> out of range (0-%d)"), Index, Mesh->GetNumSourceModels() - 1));
					return sol::lua_nil;
				}
				Mesh->Modify();
				FStaticMeshSourceModel& Model = Mesh->GetSourceModel(Index);

				if (P.get<sol::optional<double>>("screen_size").has_value())
					Model.ScreenSize = FPerPlatformFloat(static_cast<float>(P.get<double>("screen_size")));

				// Reduction settings
				sol::optional<sol::table> RedOpt = P.get<sol::optional<sol::table>>("reduction");
				if (RedOpt.has_value())
				{
					sol::table R = RedOpt.value();
					FMeshReductionSettings& RS = Model.ReductionSettings;
					if (R.get<sol::optional<double>>("percent_triangles").has_value()) RS.PercentTriangles = R["percent_triangles"];
					if (R.get<sol::optional<double>>("percent_vertices").has_value()) RS.PercentVertices = R["percent_vertices"];
					if (R.get<sol::optional<double>>("max_deviation").has_value()) RS.MaxDeviation = R["max_deviation"];
					if (R.get<sol::optional<double>>("pixel_error").has_value()) RS.PixelError = R["pixel_error"];
					if (R.get<sol::optional<double>>("welding_threshold").has_value()) RS.WeldingThreshold = R["welding_threshold"];
					if (R.get<sol::optional<double>>("hard_angle_threshold").has_value()) RS.HardAngleThreshold = R["hard_angle_threshold"];
					if (R.get<sol::optional<int>>("base_lod_model").has_value()) RS.BaseLODModel = R["base_lod_model"];
					if (R.get<sol::optional<int>>("max_num_of_triangles").has_value()) RS.MaxNumOfTriangles = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_triangles")));
					if (R.get<sol::optional<int>>("max_num_of_verts").has_value()) RS.MaxNumOfVerts = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_verts")));
					if (R.get<sol::optional<bool>>("recalculate_normals").has_value()) RS.bRecalculateNormals = R["recalculate_normals"];

					// Termination criterion: "triangles", "vertices", "any"
					sol::optional<std::string> TermCrit = R.get<sol::optional<std::string>>("termination_criterion");
					if (TermCrit.has_value())
					{
						FString TC = UTF8_TO_TCHAR(TermCrit.value().c_str());
						if (TC.Equals(TEXT("triangles"), ESearchCase::IgnoreCase))
							RS.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Triangles;
						else if (TC.Equals(TEXT("vertices"), ESearchCase::IgnoreCase))
							RS.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Vertices;
						else if (TC.Equals(TEXT("any"), ESearchCase::IgnoreCase))
							RS.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Any;
					}

					// Importance settings: off/lowest/low/normal/high/highest
					auto ParseImportance = [](const FString& Str) -> EMeshFeatureImportance::Type
					{
						if (Str.Equals(TEXT("off"), ESearchCase::IgnoreCase)) return EMeshFeatureImportance::Off;
						if (Str.Equals(TEXT("lowest"), ESearchCase::IgnoreCase)) return EMeshFeatureImportance::Lowest;
						if (Str.Equals(TEXT("low"), ESearchCase::IgnoreCase)) return EMeshFeatureImportance::Low;
						if (Str.Equals(TEXT("high"), ESearchCase::IgnoreCase)) return EMeshFeatureImportance::High;
						if (Str.Equals(TEXT("highest"), ESearchCase::IgnoreCase)) return EMeshFeatureImportance::Highest;
						return EMeshFeatureImportance::Normal;
					};

					sol::optional<std::string> SilOpt = R.get<sol::optional<std::string>>("silhouette_importance");
					if (SilOpt.has_value()) RS.SilhouetteImportance = ParseImportance(UTF8_TO_TCHAR(SilOpt.value().c_str()));

					sol::optional<std::string> TexOpt = R.get<sol::optional<std::string>>("texture_importance");
					if (TexOpt.has_value()) RS.TextureImportance = ParseImportance(UTF8_TO_TCHAR(TexOpt.value().c_str()));

					sol::optional<std::string> ShadOpt = R.get<sol::optional<std::string>>("shading_importance");
					if (ShadOpt.has_value()) RS.ShadingImportance = ParseImportance(UTF8_TO_TCHAR(ShadOpt.value().c_str()));

					sol::optional<std::string> VisOpt = R.get<sol::optional<std::string>>("visibility_aggressiveness");
					if (VisOpt.has_value()) RS.VisibilityAggressiveness = ParseImportance(UTF8_TO_TCHAR(VisOpt.value().c_str()));

					sol::optional<std::string> VCOpt = R.get<sol::optional<std::string>>("vertex_color_importance");
					if (VCOpt.has_value()) RS.VertexColorImportance = ParseImportance(UTF8_TO_TCHAR(VCOpt.value().c_str()));
				}

				// Build settings
				sol::optional<sol::table> BuildOpt = P.get<sol::optional<sol::table>>("build");
				if (BuildOpt.has_value())
				{
					sol::table B = BuildOpt.value();
					FMeshBuildSettings& BS = Model.BuildSettings;
					if (B.get<sol::optional<bool>>("recompute_normals").has_value()) BS.bRecomputeNormals = B["recompute_normals"];
					if (B.get<sol::optional<bool>>("recompute_tangents").has_value()) BS.bRecomputeTangents = B["recompute_tangents"];
					if (B.get<sol::optional<bool>>("use_mikkt").has_value()) BS.bUseMikkTSpace = B["use_mikkt"];
					if (B.get<sol::optional<bool>>("compute_weighted_normals").has_value()) BS.bComputeWeightedNormals = B["compute_weighted_normals"];
					if (B.get<sol::optional<bool>>("remove_degenerates").has_value()) BS.bRemoveDegenerates = B["remove_degenerates"];
					if (B.get<sol::optional<bool>>("use_full_precision_uvs").has_value()) BS.bUseFullPrecisionUVs = B["use_full_precision_uvs"];
					if (B.get<sol::optional<bool>>("use_high_precision_tangent_basis").has_value()) BS.bUseHighPrecisionTangentBasis = B["use_high_precision_tangent_basis"];
					if (B.get<sol::optional<bool>>("generate_lightmap_uvs").has_value()) BS.bGenerateLightmapUVs = B["generate_lightmap_uvs"];
					if (B.get<sol::optional<int>>("min_lightmap_resolution").has_value()) BS.MinLightmapResolution = B["min_lightmap_resolution"];
					if (B.get<sol::optional<int>>("src_lightmap_index").has_value()) BS.SrcLightmapIndex = B["src_lightmap_index"];
					if (B.get<sol::optional<int>>("dst_lightmap_index").has_value()) BS.DstLightmapIndex = B["dst_lightmap_index"];
				}

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod\", %d) -> updated"), Index));
				sol::table Res = Lua.create_table();
				Res["index"] = Index;
				Res["screen_size"] = Model.ScreenSize.GetDefault();
				return Res;
			}

			// ---- configure("material", index, {material, slot_name?}) ----
			if (FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"material\") -> params required")); return sol::lua_nil; }
				sol::table P = Arg3.value();
				TArray<FStaticMaterial> Materials = Mesh->GetStaticMaterials();
				if (Index < 0 || Index >= Materials.Num()) { Session.Log(TEXT("[FAIL] configure(\"material\") -> index out of range")); return sol::lua_nil; }

				sol::optional<std::string> MatPathOpt = P.get<sol::optional<std::string>>("material");
				if (MatPathOpt.has_value())
				{
					FString MatPath = UTF8_TO_TCHAR(MatPathOpt.value().c_str());
					UMaterialInterface* MatIface = LoadObject<UMaterialInterface>(nullptr, *MatPath);
					if (MatIface) Materials[Index].MaterialInterface = MatIface;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"material\") -> could not load \"%s\""), *MatPath));
				}
				sol::optional<std::string> SlotNameOpt = P.get<sol::optional<std::string>>("slot_name");
				if (SlotNameOpt.has_value())
					Materials[Index].MaterialSlotName = FName(UTF8_TO_TCHAR(SlotNameOpt.value().c_str()));

				Mesh->SetStaticMaterials(Materials);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"material\", %d)"), Index));
				sol::table R = Lua.create_table();
				R["index"] = Index;
				return R;
			}

			// ---- configure("collision", {type, index, ...}) ----
			if (FType.Equals(TEXT("collision"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup) { Session.Log(TEXT("[FAIL] configure(\"collision\") -> no BodySetup")); return sol::lua_nil; }

				FString ShapeType = UTF8_TO_TCHAR(P.get_or<std::string>("type", "").c_str());
				int32 Index = P.get_or("index", 0);

				if (Index < 0) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"collision\") -> negative index %d"), Index)); return sol::lua_nil; }

				Mesh->Modify();
				BodySetup->Modify();

				sol::optional<sol::table> CenterOpt = P.get<sol::optional<sol::table>>("center");

				bool bUpdated = false;
				if (ShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.BoxElems.Num())
				{
					FKBoxElem& Box = BodySetup->AggGeom.BoxElems[Index];
					if (CenterOpt.has_value()) { Box.Center.X = CenterOpt.value().get_or("x", Box.Center.X); Box.Center.Y = CenterOpt.value().get_or("y", Box.Center.Y); Box.Center.Z = CenterOpt.value().get_or("z", Box.Center.Z); }
					if (P.get<sol::optional<double>>("x").has_value()) Box.X = P["x"];
					if (P.get<sol::optional<double>>("y").has_value()) Box.Y = P["y"];
					if (P.get<sol::optional<double>>("z").has_value()) Box.Z = P["z"];
					bUpdated = true;
				}
				else if (ShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.SphereElems.Num())
				{
					FKSphereElem& Sphere = BodySetup->AggGeom.SphereElems[Index];
					if (CenterOpt.has_value()) { Sphere.Center.X = CenterOpt.value().get_or("x", Sphere.Center.X); Sphere.Center.Y = CenterOpt.value().get_or("y", Sphere.Center.Y); Sphere.Center.Z = CenterOpt.value().get_or("z", Sphere.Center.Z); }
					if (P.get<sol::optional<double>>("radius").has_value()) Sphere.Radius = P["radius"];
					bUpdated = true;
				}
				else if (ShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.SphylElems.Num())
				{
					FKSphylElem& Cap = BodySetup->AggGeom.SphylElems[Index];
					if (CenterOpt.has_value()) { Cap.Center.X = CenterOpt.value().get_or("x", Cap.Center.X); Cap.Center.Y = CenterOpt.value().get_or("y", Cap.Center.Y); Cap.Center.Z = CenterOpt.value().get_or("z", Cap.Center.Z); }
					if (P.get<sol::optional<double>>("radius").has_value()) Cap.Radius = P["radius"];
					if (P.get<sol::optional<double>>("length").has_value()) Cap.Length = P["length"];
					bUpdated = true;
				}
				else if (ShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase) && Index < BodySetup->AggGeom.TaperedCapsuleElems.Num())
				{
					FKTaperedCapsuleElem& TapCap = BodySetup->AggGeom.TaperedCapsuleElems[Index];
					if (CenterOpt.has_value()) { TapCap.Center.X = CenterOpt.value().get_or("x", TapCap.Center.X); TapCap.Center.Y = CenterOpt.value().get_or("y", TapCap.Center.Y); TapCap.Center.Z = CenterOpt.value().get_or("z", TapCap.Center.Z); }
					if (P.get<sol::optional<double>>("radius0").has_value()) TapCap.Radius0 = P["radius0"];
					if (P.get<sol::optional<double>>("radius1").has_value()) TapCap.Radius1 = P["radius1"];
					if (P.get<sol::optional<double>>("length").has_value()) TapCap.Length = P["length"];
					bUpdated = true;
				}

				if (!bUpdated) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"collision\") -> %s[%d] not found"), *ShapeType, Index)); return sol::lua_nil; }
				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"collision\") -> %s[%d]"), *ShapeType, Index));
				sol::table R = Lua.create_table();
				R["updated"] = true;
				return R;
			}

			// ---- configure("socket", name, {location?, rotation?, scale?, tag?}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = UTF8_TO_TCHAR(Arg2.as<std::string>().c_str());
				UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"socket\") -> \"%s\" not found"), *SocketName)); return sol::lua_nil; }
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"socket\") -> params required")); return sol::lua_nil; }
				sol::table P = Arg3.value();

				Mesh->Modify();
				Socket->Modify();

				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value()) { Socket->RelativeLocation.X = LocOpt.value().get_or("x", Socket->RelativeLocation.X); Socket->RelativeLocation.Y = LocOpt.value().get_or("y", Socket->RelativeLocation.Y); Socket->RelativeLocation.Z = LocOpt.value().get_or("z", Socket->RelativeLocation.Z); }
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value()) { Socket->RelativeRotation.Pitch = RotOpt.value().get_or("pitch", Socket->RelativeRotation.Pitch); Socket->RelativeRotation.Yaw = RotOpt.value().get_or("yaw", Socket->RelativeRotation.Yaw); Socket->RelativeRotation.Roll = RotOpt.value().get_or("roll", Socket->RelativeRotation.Roll); }
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value()) { Socket->RelativeScale.X = ScaleOpt.value().get_or("x", Socket->RelativeScale.X); Socket->RelativeScale.Y = ScaleOpt.value().get_or("y", Socket->RelativeScale.Y); Socket->RelativeScale.Z = ScaleOpt.value().get_or("z", Socket->RelativeScale.Z); }
				sol::optional<std::string> TagOpt = P.get<sol::optional<std::string>>("tag");
				if (TagOpt.has_value()) Socket->Tag = UTF8_TO_TCHAR(TagOpt.value().c_str());

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- configure("lightmap", {resolution?, uv_index?}) ----
			if (FType.Equals(TEXT("lightmap"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				if (P.get<sol::optional<int>>("resolution").has_value()) Mesh->SetLightMapResolution(P["resolution"]);
				if (P.get<sol::optional<int>>("uv_index").has_value()) Mesh->SetLightMapCoordinateIndex(P["uv_index"]);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"lightmap\") -> res=%d, uv=%d"), Mesh->GetLightMapResolution(), Mesh->GetLightMapCoordinateIndex()));
				sol::table R = Lua.create_table();
				R["resolution"] = Mesh->GetLightMapResolution();
				R["uv_index"] = Mesh->GetLightMapCoordinateIndex();
				return R;
			}

			// ---- configure("physics", {collision_trace_flag, generate_distance_field, lod_for_collision}) ----
			if (FType.Equals(TEXT("physics"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup) { Session.Log(TEXT("[FAIL] configure(\"physics\") -> no BodySetup")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSMPhys", "Configure StaticMesh Physics"));
				BodySetup->Modify();

				sol::optional<std::string> TraceFlag = P.get<sol::optional<std::string>>("collision_trace_flag");
				if (TraceFlag.has_value())
				{
					FString TF = UTF8_TO_TCHAR(TraceFlag.value().c_str());
					if (TF.Equals(TEXT("default"), ESearchCase::IgnoreCase) || TF.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
						BodySetup->CollisionTraceFlag = CTF_UseDefault;
					else if (TF.Equals(TEXT("simple_and_complex"), ESearchCase::IgnoreCase))
						BodySetup->CollisionTraceFlag = CTF_UseSimpleAndComplex;
					else if (TF.Equals(TEXT("simple_as_complex"), ESearchCase::IgnoreCase))
						BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
					else if (TF.Equals(TEXT("complex_as_simple"), ESearchCase::IgnoreCase))
						BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
				}

				sol::optional<bool> DistField = P.get<sol::optional<bool>>("generate_distance_field");
				if (DistField.has_value()) Mesh->bGenerateMeshDistanceField = DistField.value();

				sol::optional<bool> GenDF = P.get<sol::optional<bool>>("generate_mesh_distance_field");
				if (GenDF.has_value()) Mesh->bGenerateMeshDistanceField = GenDF.value();

				// LOD for collision — which LOD level to use for collision mesh generation
				sol::optional<int> LODForCol = P.get<sol::optional<int>>("lod_for_collision");
				if (LODForCol.has_value())
				{
					int32 Val = LODForCol.value();
					if (Val >= 0 && Val < Mesh->GetNumSourceModels())
					{
						Mesh->LODForCollision = Val;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"physics\") -> lod_for_collision=%d out of range (0-%d)"), Val, Mesh->GetNumSourceModels() - 1));
					}
				}

				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				Mesh->MarkPackageDirty();

				Session.Log(TEXT("[OK] configure(\"physics\")"));
				sol::table R = Lua.create_table();
				R["updated"] = true;
				R["lod_for_collision"] = Mesh->LODForCollision;
				return R;
			}

			// ---- configure("section", {lod, index, collision_enabled, casts_shadow, visible_in_ray_tracing}) ----
			if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				int32 LODIdx = static_cast<int32>(P.get_or("lod", 0.0));
				int32 SecIdx = static_cast<int32>(P.get_or("index", 0.0));

				if (LODIdx < 0 || LODIdx >= Mesh->GetNumSourceModels())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"section\") -> LOD %d out of range (0-%d source models)"), LODIdx, Mesh->GetNumSourceModels() - 1));
					return sol::lua_nil;
				}

				// Validate section index against render data
				const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
				if (!RenderData || LODIdx >= RenderData->LODResources.Num() || SecIdx < 0 || SecIdx >= RenderData->LODResources[LODIdx].Sections.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"section\") -> section %d out of range"), SecIdx));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSMSection", "Configure StaticMesh Section"));
				Mesh->Modify();

				// Update the persistent SectionInfoMap (render data sections are regenerated on Build)
				FMeshSectionInfo Info = Mesh->GetSectionInfoMap().Get(LODIdx, SecIdx);

				sol::optional<bool> CollEnabled = P.get<sol::optional<bool>>("collision_enabled");
				if (CollEnabled.has_value()) Info.bEnableCollision = CollEnabled.value();

				sol::optional<bool> CastsShadow = P.get<sol::optional<bool>>("casts_shadow");
				if (CastsShadow.has_value()) Info.bCastShadow = CastsShadow.value();

				sol::optional<bool> VisRT = P.get<sol::optional<bool>>("visible_in_ray_tracing");
				if (VisRT.has_value()) Info.bVisibleInRayTracing = VisRT.value();

				sol::optional<bool> AffectDF = P.get<sol::optional<bool>>("affect_distance_field_lighting");
				if (AffectDF.has_value()) Info.bAffectDistanceFieldLighting = AffectDF.value();

				sol::optional<bool> ForceOpaque = P.get<sol::optional<bool>>("force_opaque");
				if (ForceOpaque.has_value()) Info.bForceOpaque = ForceOpaque.value();

				sol::optional<int> MaterialIdx = P.get<sol::optional<int>>("material_index");
				if (MaterialIdx.has_value())
				{
					int32 MatIdx = MaterialIdx.value();
					if (MatIdx >= 0 && MatIdx < Mesh->GetStaticMaterials().Num())
						Info.MaterialIndex = MatIdx;
					else
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"section\") -> material_index %d out of range"), MatIdx));
				}

				Mesh->GetSectionInfoMap().Set(LODIdx, SecIdx, Info);

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"section\", lod=%d, index=%d)"), LODIdx, SecIdx));
				sol::table R = Lua.create_table();
				R["updated"] = true;
				return R;
			}

			// ---- configure("lod_group", {name?}) ----
			if (FType.Equals(TEXT("lod_group"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
				FName NewGroup = NameOpt.has_value() ? FName(UTF8_TO_TCHAR(NameOpt.value().c_str())) : NAME_None;
				Mesh->SetLODGroup(NewGroup);
				Mesh->MarkPackageDirty();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				FName Actual = Mesh->GetLODGroup();
#else
				FName Actual = Mesh->LODGroup;
#endif
				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod_group\") -> %s"), Actual.IsNone() ? TEXT("None") : *Actual.ToString()));
				sol::table R = Lua.create_table();
				R["lod_group"] = Actual.IsNone() ? "" : TCHAR_TO_UTF8(*Actual.ToString());
				return R;
			}

			// ---- configure("min_lod", {index}) ----
			if (FType.Equals(TEXT("min_lod"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				int32 MinLOD = P.get_or("index", 0);
				if (MinLOD < 0) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"min_lod\") -> negative index %d"), MinLOD)); return sol::lua_nil; }
				Mesh->SetMinLODIdx(MinLOD);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"min_lod\") -> %d"), Mesh->GetMinLODIdx()));
				sol::table R = Lua.create_table();
				R["min_lod"] = Mesh->GetMinLODIdx();
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: nanite, lod, material, collision, socket, lightmap, physics, section, lod_group, min_lod"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// build() — rebuild mesh after changes
		// ================================================================
		AssetObj.set_function("build", [Mesh, &Session](sol::table /*Self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] build -> asset no longer valid")); return sol::lua_nil; }
			Mesh->Build(true);
			Mesh->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] build() -> %s rebuilt"), *Mesh->GetName()));
			sol::table R = Lua.create_table();
			R["rebuilt"] = true;
			R["lod_count"] = Mesh->GetNumLODs();
			return R;
		});

		// ================================================================
		// info() — comprehensive summary
		// ================================================================
		AssetObj.set_function("info", [Mesh, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}
			sol::table Result = Lua.create_table();

			Result["name"] = TCHAR_TO_UTF8(*Mesh->GetName());
			Result["path"] = TCHAR_TO_UTF8(*Mesh->GetPathName());

			// LOD count
			int32 NumLODs = Mesh->GetNumLODs();
			Result["lod_count"] = NumLODs;

			// Materials
			Result["material_count"] = static_cast<int>(Mesh->GetStaticMaterials().Num());

			// Bounds
			FBoxSphereBounds Bounds = Mesh->GetBounds();
			sol::table BoundsT = Lua.create_table();
			{
				sol::table Origin = Lua.create_table();
				Origin["x"] = Bounds.Origin.X;
				Origin["y"] = Bounds.Origin.Y;
				Origin["z"] = Bounds.Origin.Z;
				BoundsT["origin"] = Origin;

				sol::table Extent = Lua.create_table();
				Extent["x"] = Bounds.BoxExtent.X;
				Extent["y"] = Bounds.BoxExtent.Y;
				Extent["z"] = Bounds.BoxExtent.Z;
				BoundsT["extent"] = Extent;

				BoundsT["radius"] = Bounds.SphereRadius;
			}
			Result["bounds"] = BoundsT;

			// Nanite
			Result["nanite_enabled"] = Mesh->IsNaniteEnabled();

			// Lightmap
			Result["lightmap_resolution"] = Mesh->GetLightMapResolution();
			Result["lightmap_uv_index"] = Mesh->GetLightMapCoordinateIndex();

			// Collision
			UBodySetup* BodySetup = Mesh->GetBodySetup();
			Result["has_collision"] = (BodySetup != nullptr);
			if (BodySetup)
			{
				Result["collision_complexity"] = CollisionTraceFlagToString(BodySetup->GetCollisionTraceFlag());
				Result["collision_shapes"] = BodySetup->AggGeom.GetElementCount();
			}

			// Sockets
			Result["socket_count"] = static_cast<int>(Mesh->Sockets.Num());

			// Flags
			Result["supports_ray_tracing"] = static_cast<bool>(Mesh->bSupportRayTracing);
			Result["allows_cpu_access"] = static_cast<bool>(Mesh->bAllowCPUAccess);
			Result["generates_distance_field"] = static_cast<bool>(Mesh->bGenerateMeshDistanceField);

			// LOD Group
			AddLODGroupInfo(Mesh, Result);

			// Min LOD
			Result["min_lod"] = Mesh->GetMinLODIdx();
			Result["lod_for_collision"] = Mesh->LODForCollision;

			// Per-LOD summary from render data
			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (RenderData && RenderData->LODResources.Num() > 0)
			{
				sol::table LODs = Lua.create_table();
				for (int32 i = 0; i < RenderData->LODResources.Num(); i++)
				{
					const FStaticMeshLODResources& LODRes = RenderData->LODResources[i];
					sol::table L = Lua.create_table();
					L["index"] = i;
					L["vertices"] = LODRes.GetNumVertices();
					L["triangles"] = LODRes.GetNumTriangles();
					L["sections"] = static_cast<int>(LODRes.Sections.Num());
					if (i < MAX_STATIC_MESH_LODS)
					{
						L["screen_size"] = RenderData->ScreenSize[i].GetDefault();
					}
					LODs[i + 1] = L;
				}
				Result["lods"] = LODs;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d LODs, %d materials"),
				*Mesh->GetName(), NumLODs, Mesh->GetStaticMaterials().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(StaticMesh, StaticMeshDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindStaticMesh(Lua, Session);
});
