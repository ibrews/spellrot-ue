// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Animation/BoneReference.h"
#include "Animation/MorphTarget.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "ClothingAssetBase.h"
#include "Animation/MeshDeformer.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SkeletalMeshDocs = {};

static void BindSkeletalMesh(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_skeletal_mesh", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FPath);
		if (!Mesh) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SkeletalMesh enrichment:\n"
			"\n"
			"info() — comprehensive summary\n"
			"\n"
			"list(type, opts?):\n"
			"  list(\"bones\")                        — flat array: {index, name, parent_index, parent_name}\n"
			"  list(\"bones\", {tree=true})            — hierarchical tree with nested children\n"
			"  list(\"bones\", {transforms=true})      — flat list with reference pose transform\n"
			"  list(\"materials\")                     — {index, slot_name, material_path, material_name}\n"
			"  list(\"sockets\")                       — {name, bone_name, location, rotation, scale}\n"
			"  list(\"morph_targets\")                  — {index, name}\n"
			"  list(\"lods\")                          — per-LOD stats\n"
			"  list(\"clothing\")                      — clothing assets\n"
			"\n"
			"add(type, params):\n"
			"  add(\"lod\")                            — add auto-generated LOD level\n"
			"  add(\"socket\", {name, bone, location?, rotation?, scale?})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"lod\", index)                  — remove LOD (cannot remove LOD 0)\n"
			"  remove(\"socket\", name)                — remove socket by name\n"
			"  remove(\"morph_target\", name)           — remove morph target by name\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"lod\", index, {screen_size?, lod_hysteresis?, reduction={...}, bones_to_remove={...}, bones_to_prioritize={...}, weight_of_prioritization?})\n"
			"  configure(\"material\", index, {material=\"/Path\", slot_name?})\n"
			"  configure(\"socket\", name, {bone?, location?, rotation?, scale?})\n"
			"  configure(\"physics\", {physics_asset?, shadow_physics_asset?, enable_per_poly_collision?})\n"
			"  configure(\"nanite\", nil, {enabled=true, explicit_tangents?, lerp_uvs?, separable?, voxel_ndf?})\n"
			"  configure(\"skeleton\", nil, {skeleton=\"/Game/Path/MySkeleton\"})\n"
			"  configure(\"lod_settings\", nil, {min_lod=2, disable_below_min_lod_stripping=true})\n"
			"  configure(\"bounds\", nil, {positive_extension={x,y,z}, negative_extension={x,y,z}})\n"
			"  configure(\"post_process_anim_bp\", nil, {blueprint=\"/Game/Path/AnimBP\", lod_threshold?})\n"
			"  configure(\"animating_rig\", nil, {rig=\"/Game/Path/ControlRig\"})\n"
			"  configure(\"ray_tracing\", nil, {enabled=true, min_lod?})\n"
			"  configure(\"overlay_material\", nil, {material=\"/Path\", max_draw_distance?})\n"
			"  configure(\"mesh_deformer\", nil, {deformer=\"/Path\"})\n"
			"  configure(\"floor_offset\", nil, {offset=0.0})\n"
			"\n"
			"build() — rebuild mesh after changes\n";

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
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Mesh->Modify();
				int32 NewIdx = Mesh->GetLODNum();
				Mesh->AddSourceModel(true);
				FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(NewIdx);
				if (LODInfo)
				{
					// Set reasonable defaults based on previous LOD
					FSkeletalMeshLODInfo* PrevInfo = (NewIdx > 0) ? Mesh->GetLODInfo(NewIdx - 1) : nullptr;
					if (PrevInfo)
					{
						LODInfo->ScreenSize = FPerPlatformFloat(FMath::Max(0.01f, PrevInfo->ScreenSize.GetDefault() * 0.5f));
						LODInfo->ReductionSettings = PrevInfo->ReductionSettings;
						LODInfo->ReductionSettings.NumOfTrianglesPercentage = FMath::Max(0.1, PrevInfo->ReductionSettings.NumOfTrianglesPercentage * 0.5);
					}
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"lod\") -> LOD %d added (total: %d)"), NewIdx, Mesh->GetLODNum()));
				sol::table R = Lua.create_table();
				R["index"] = NewIdx;
				R["total_lods"] = Mesh->GetLODNum();
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "AddSourceModel requires UE 5.7+";
				return R;
#endif
			}

			// ---- add("socket", {name, bone, location?, rotation?, scale?}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> params required: {name, bone}")); return sol::lua_nil; }
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				std::string BoneName = P.get_or<std::string>("bone", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> 'name' is required")); return sol::lua_nil; }
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> 'bone' is required")); return sol::lua_nil; }

				// Validate bone exists in reference skeleton
				FName BoneFName = FName(UTF8_TO_TCHAR(BoneName.c_str()));
				const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
				if (RefSkel.FindBoneIndex(BoneFName) == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[WARN] add(\"socket\") -> bone \"%s\" not found in skeleton. Socket will be created but may not function."), *BoneFName.ToString()));
				}

				Mesh->Modify();
				USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
				Socket->SocketName = FName(UTF8_TO_TCHAR(Name.c_str()));
				Socket->BoneName = BoneFName;

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
				if (P.get<sol::optional<bool>>("force_always_animated").has_value())
					Socket->bForceAlwaysAnimated = P.get<bool>("force_always_animated");

				Mesh->AddSocket(Socket);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\") -> \"%s\" on bone \"%s\""),
					*Socket->SocketName.ToString(), *Socket->BoneName.ToString()));
				sol::table R = Lua.create_table();
				R["name"] = Name;
				R["bone"] = BoneName;
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: lod, socket"), *FType));
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
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				int32 Index = IdObj.as<int32>();
				if (Index <= 0) { Session.Log(TEXT("[FAIL] remove(\"lod\") -> cannot remove LOD 0")); return sol::lua_nil; }
				if (Index >= Mesh->GetLODNum()) { Session.Log(TEXT("[FAIL] remove(\"lod\") -> index out of range")); return sol::lua_nil; }
				Mesh->Modify();
				Mesh->RemoveSourceModel(Index);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"lod\", %d) -> remaining: %d"), Index, Mesh->GetLODNum()));
				sol::table R = Lua.create_table();
				R["removed"] = Index;
				R["total_lods"] = Mesh->GetLODNum();
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "RemoveSourceModel requires UE 5.7+";
				return R;
#endif
			}

			// ---- remove("socket", name) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = UTF8_TO_TCHAR(IdObj.as<std::string>().c_str());
				USkeletalMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"socket\") -> \"%s\" not found"), *SocketName)); return sol::lua_nil; }

				// Check if it's on the mesh (not the skeleton)
				TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Mesh->GetMeshOnlySocketList();
				bool bFound = false;
				for (int32 i = 0; i < MeshSockets.Num(); ++i)
				{
					if (MeshSockets[i] == Socket)
					{
						Mesh->Modify();
						MeshSockets.RemoveAt(i);
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"socket\") -> \"%s\" is on the Skeleton, not the mesh. Edit the Skeleton to remove it."), *SocketName));
					return sol::lua_nil;
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["removed"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- remove("morph_target", name) ----
			if (FType.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase))
			{
				FString MorphName = UTF8_TO_TCHAR(IdObj.as<std::string>().c_str());
				UMorphTarget* MorphTarget = Mesh->FindMorphTarget(FName(*MorphName));
				if (!MorphTarget) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"morph_target\") -> \"%s\" not found"), *MorphName)); return sol::lua_nil; }

				Mesh->Modify();
				TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();
				MorphTargets.Remove(MorphTarget);
				Mesh->InitMorphTargets();
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"morph_target\") -> \"%s\""), *MorphName));
				sol::table R = Lua.create_table();
				R["removed"] = TCHAR_TO_UTF8(*MorphName);
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: lod, socket, morph_target"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [Mesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::object Arg2, sol::optional<sol::table> Arg3, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] configure -> asset no longer valid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());

			// ---- configure("lod", index, {screen_size?, lod_hysteresis?, reduction?}) ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"lod\") -> params required")); return sol::lua_nil; }
				sol::table P = Arg3.value();

				FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(Index);
				if (!LODInfo) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"lod\", %d) -> out of range"), Index)); return sol::lua_nil; }

				Mesh->Modify();

				if (P.get<sol::optional<double>>("screen_size").has_value())
					LODInfo->ScreenSize = FPerPlatformFloat(static_cast<float>(P.get<double>("screen_size")));
				if (P.get<sol::optional<double>>("lod_hysteresis").has_value())
					LODInfo->LODHysteresis = static_cast<float>(P.get<double>("lod_hysteresis"));
				if (P.get<sol::optional<bool>>("allow_cpu_access").has_value())
					LODInfo->bAllowCPUAccess = P.get<bool>("allow_cpu_access");
				if (P.get<sol::optional<double>>("morph_target_position_error_tolerance").has_value())
					LODInfo->MorphTargetPositionErrorTolerance = static_cast<float>(P.get<double>("morph_target_position_error_tolerance"));

				// BonesToRemove
				sol::optional<sol::table> BonesToRemoveOpt = P.get<sol::optional<sol::table>>("bones_to_remove");
				if (BonesToRemoveOpt.has_value())
				{
					LODInfo->BonesToRemove.Empty();
					sol::table BoneArr = BonesToRemoveOpt.value();
					for (auto& Pair : BoneArr)
					{
						if (Pair.second.is<std::string>())
						{
							FBoneReference Ref;
							Ref.BoneName = FName(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
							LODInfo->BonesToRemove.Add(Ref);
						}
					}
				}

				// BonesToPrioritize
				sol::optional<sol::table> BonesToPrioritizeOpt = P.get<sol::optional<sol::table>>("bones_to_prioritize");
				if (BonesToPrioritizeOpt.has_value())
				{
					LODInfo->BonesToPrioritize.Empty();
					sol::table BoneArr = BonesToPrioritizeOpt.value();
					for (auto& Pair : BoneArr)
					{
						if (Pair.second.is<std::string>())
						{
							FBoneReference Ref;
							Ref.BoneName = FName(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
							LODInfo->BonesToPrioritize.Add(Ref);
						}
					}
				}

				// WeightOfPrioritization
				if (P.get<sol::optional<double>>("weight_of_prioritization").has_value())
					LODInfo->WeightOfPrioritization = static_cast<float>(P.get<double>("weight_of_prioritization"));

				// Reduction settings
				sol::optional<sol::table> RedOpt = P.get<sol::optional<sol::table>>("reduction");
				if (RedOpt.has_value())
				{
					sol::table R = RedOpt.value();
					FSkeletalMeshOptimizationSettings& RS = LODInfo->ReductionSettings;
					if (R.get<sol::optional<double>>("num_of_triangles_percentage").has_value()) RS.NumOfTrianglesPercentage = R.get<double>("num_of_triangles_percentage");
					if (R.get<sol::optional<double>>("num_of_vert_percentage").has_value()) RS.NumOfVertPercentage = R.get<double>("num_of_vert_percentage");
					if (R.get<sol::optional<double>>("max_deviation").has_value()) RS.MaxDeviationPercentage = R.get<double>("max_deviation");
					if (R.get<sol::optional<int>>("max_num_of_triangles").has_value()) RS.MaxNumOfTriangles = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_triangles")));
					if (R.get<sol::optional<int>>("max_num_of_verts").has_value()) RS.MaxNumOfVerts = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_verts")));
					if (R.get<sol::optional<int>>("base_lod").has_value()) RS.BaseLOD = R.get<int>("base_lod");
					if (R.get<sol::optional<double>>("welding_threshold").has_value()) RS.WeldingThreshold = R.get<double>("welding_threshold");
					if (R.get<sol::optional<bool>>("recalculate_normals").has_value()) RS.bRecalcNormals = R.get<bool>("recalculate_normals");
					if (R.get<sol::optional<double>>("normals_threshold").has_value()) RS.NormalsThreshold = R.get<double>("normals_threshold");
					if (R.get<sol::optional<bool>>("lock_edges").has_value()) RS.bLockEdges = R.get<bool>("lock_edges");
					if (R.get<sol::optional<bool>>("lock_colorBounaries").has_value()) RS.bLockColorBounaries = R.get<bool>("lock_colorBounaries");
				}

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod\", %d) -> updated"), Index));
				sol::table Res = Lua.create_table();
				Res["index"] = Index;
				Res["screen_size"] = LODInfo->ScreenSize.GetDefault();
				return Res;
			}

			// ---- configure("material", index, {material, slot_name?}) ----
			if (FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"material\") -> params required")); return sol::lua_nil; }
				Mesh->Modify();
				sol::table P = Arg3.value();

				TArray<FSkeletalMaterial> Materials = Mesh->GetMaterials();
				if (Index < 0 || Index >= Materials.Num()) { Session.Log(TEXT("[FAIL] configure(\"material\") -> index out of range")); return sol::lua_nil; }

				sol::optional<std::string> MatPathOpt = P.get<sol::optional<std::string>>("material");
				if (MatPathOpt.has_value())
				{
					UMaterialInterface* MatIface = LoadObject<UMaterialInterface>(nullptr, *FString(UTF8_TO_TCHAR(MatPathOpt.value().c_str())));
					if (MatIface) Materials[Index].MaterialInterface = MatIface;
					else Session.Log(TEXT("[WARN] configure(\"material\") -> material not found"));
				}
				sol::optional<std::string> SlotOpt = P.get<sol::optional<std::string>>("slot_name");
				if (SlotOpt.has_value())
					Materials[Index].MaterialSlotName = FName(UTF8_TO_TCHAR(SlotOpt.value().c_str()));

				Mesh->SetMaterials(Materials);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"material\", %d)"), Index));
				sol::table Res = Lua.create_table();
				Res["index"] = Index;
				return Res;
			}

			// ---- configure("socket", name, {bone?, location?, rotation?, scale?}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = UTF8_TO_TCHAR(Arg2.as<std::string>().c_str());
				USkeletalMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"socket\") -> \"%s\" not found"), *SocketName)); return sol::lua_nil; }
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"socket\") -> params required")); return sol::lua_nil; }
				sol::table P = Arg3.value();

				Socket->Modify();

				sol::optional<std::string> BoneOpt = P.get<sol::optional<std::string>>("bone");
				if (BoneOpt.has_value())
				{
					FName NewBone = FName(UTF8_TO_TCHAR(BoneOpt.value().c_str()));
					if (Mesh->GetRefSkeleton().FindBoneIndex(NewBone) == INDEX_NONE)
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"socket\") -> bone \"%s\" not found in skeleton"), *NewBone.ToString()));
					}
					Socket->BoneName = NewBone;
				}

				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value()) { Socket->RelativeLocation.X = LocOpt.value().get_or("x", Socket->RelativeLocation.X); Socket->RelativeLocation.Y = LocOpt.value().get_or("y", Socket->RelativeLocation.Y); Socket->RelativeLocation.Z = LocOpt.value().get_or("z", Socket->RelativeLocation.Z); }
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value()) { Socket->RelativeRotation.Pitch = RotOpt.value().get_or("pitch", Socket->RelativeRotation.Pitch); Socket->RelativeRotation.Yaw = RotOpt.value().get_or("yaw", Socket->RelativeRotation.Yaw); Socket->RelativeRotation.Roll = RotOpt.value().get_or("roll", Socket->RelativeRotation.Roll); }
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value()) { Socket->RelativeScale.X = ScaleOpt.value().get_or("x", Socket->RelativeScale.X); Socket->RelativeScale.Y = ScaleOpt.value().get_or("y", Socket->RelativeScale.Y); Socket->RelativeScale.Z = ScaleOpt.value().get_or("z", Socket->RelativeScale.Z); }

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- configure("physics", {physics_asset?, shadow_physics_asset?}) ----
			if (FType.Equals(TEXT("physics"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<std::string> PAOpt = P.get<sol::optional<std::string>>("physics_asset");
				if (PAOpt.has_value())
				{
					UPhysicsAsset* PA = LoadObject<UPhysicsAsset>(nullptr, *FString(UTF8_TO_TCHAR(PAOpt.value().c_str())));
					if (PA) Mesh->SetPhysicsAsset(PA);
					else Session.Log(TEXT("[WARN] configure(\"physics\") -> physics_asset not found"));
				}
				sol::optional<std::string> SPAOpt = P.get<sol::optional<std::string>>("shadow_physics_asset");
				if (SPAOpt.has_value())
				{
					UPhysicsAsset* SPA = LoadObject<UPhysicsAsset>(nullptr, *FString(UTF8_TO_TCHAR(SPAOpt.value().c_str())));
					if (SPA) Mesh->SetShadowPhysicsAsset(SPA);
					else Session.Log(TEXT("[WARN] configure(\"physics\") -> shadow_physics_asset not found"));
				}
				if (P.get<sol::optional<bool>>("enable_per_poly_collision").has_value())
					Mesh->SetEnablePerPolyCollision(P.get<bool>("enable_per_poly_collision"));

				Mesh->MarkPackageDirty();
				Session.Log(TEXT("[OK] configure(\"physics\") -> updated"));
				sol::table R = Lua.create_table();
				R["physics_asset"] = Mesh->GetPhysicsAsset() ? TCHAR_TO_UTF8(*Mesh->GetPhysicsAsset()->GetPathName()) : "None";
				return R;
			}

			// ---- configure("nanite", nil, {enabled?, explicit_tangents?, lerp_uvs?, separable?, voxel_ndf?}) ----
			if (FType.Equals(TEXT("nanite"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				FScopedTransaction Tx(FText::FromString(TEXT("Configure Nanite Settings")));
				Mesh->Modify();

				FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
				if (P.get<sol::optional<bool>>("enabled").has_value())
					NaniteSettings.bEnabled = P.get<bool>("enabled");
				if (P.get<sol::optional<bool>>("explicit_tangents").has_value())
					NaniteSettings.bExplicitTangents = P.get<bool>("explicit_tangents");
				if (P.get<sol::optional<bool>>("lerp_uvs").has_value())
					NaniteSettings.bLerpUVs = P.get<bool>("lerp_uvs");
				if (P.get<sol::optional<bool>>("separable").has_value())
					NaniteSettings.bSeparable = P.get<bool>("separable");
				if (P.get<sol::optional<bool>>("voxel_ndf").has_value())
					NaniteSettings.bVoxelNDF = P.get<bool>("voxel_ndf");

				Mesh->SetNaniteSettings(NaniteSettings);
				Mesh->NotifyNaniteSettingsChanged();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"nanite\") -> enabled=%s"), NaniteSettings.bEnabled ? TEXT("true") : TEXT("false")));
				sol::table R = Lua.create_table();
				R["enabled"] = static_cast<bool>(NaniteSettings.bEnabled);
				R["explicit_tangents"] = static_cast<bool>(NaniteSettings.bExplicitTangents);
				R["lerp_uvs"] = static_cast<bool>(NaniteSettings.bLerpUVs);
				R["separable"] = static_cast<bool>(NaniteSettings.bSeparable);
				R["voxel_ndf"] = static_cast<bool>(NaniteSettings.bVoxelNDF);
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "Nanite settings requires UE 5.7+";
				return R;
#endif
			}

			// ---- configure("skeleton", nil, {skeleton="/Game/Path/..."}) ----
			if (FType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string SkelPath = P.get_or<std::string>("skeleton", "");
				if (SkelPath.empty()) { Session.Log(TEXT("[FAIL] configure(\"skeleton\") -> 'skeleton' path required")); return sol::lua_nil; }

				USkeleton* NewSkeleton = LoadObject<USkeleton>(nullptr, *FString(UTF8_TO_TCHAR(SkelPath.c_str())));
				if (!NewSkeleton) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"skeleton\") -> skeleton not found: %s"), UTF8_TO_TCHAR(SkelPath.c_str()))); return sol::lua_nil; }

				FScopedTransaction Tx(FText::FromString(TEXT("Set Skeleton")));
				Mesh->Modify();
				Mesh->SetSkeleton(NewSkeleton);
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"skeleton\") -> %s"), *NewSkeleton->GetPathName()));
				sol::table R = Lua.create_table();
				R["skeleton"] = TCHAR_TO_UTF8(*NewSkeleton->GetPathName());
				return R;
			}

			// ---- configure("lod_settings", nil, {min_lod?, disable_below_min_lod_stripping?}) ----
			if (FType.Equals(TEXT("lod_settings"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				FScopedTransaction Tx(FText::FromString(TEXT("Configure LOD Settings")));
				Mesh->Modify();

				if (P.get<sol::optional<int>>("min_lod").has_value())
				{
					int32 MinLod = P.get<int>("min_lod");
					Mesh->SetMinLodIdx(MinLod);
				}
				if (P.get<sol::optional<bool>>("disable_below_min_lod_stripping").has_value())
				{
					FPerPlatformBool Val;
					Val.Default = P.get<bool>("disable_below_min_lod_stripping");
					Mesh->SetDisableBelowMinLodStripping(Val);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod_settings\") -> min_lod=%d"), Mesh->GetMinLodIdx()));
				sol::table R = Lua.create_table();
				R["min_lod"] = Mesh->GetMinLodIdx();
				R["disable_below_min_lod_stripping"] = Mesh->GetDisableBelowMinLodStripping().Default;
				return R;
			}

			// ---- configure("bounds", nil, {positive_extension?, negative_extension?}) ----
			if (FType.Equals(TEXT("bounds"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<sol::table> PosOpt = P.get<sol::optional<sol::table>>("positive_extension");
				if (PosOpt.has_value())
				{
					sol::table V = PosOpt.value();
					FVector Ext;
					Ext.X = V.get_or("x", 0.0);
					Ext.Y = V.get_or("y", 0.0);
					Ext.Z = V.get_or("z", 0.0);
					Mesh->SetPositiveBoundsExtension(Ext);
				}
				sol::optional<sol::table> NegOpt = P.get<sol::optional<sol::table>>("negative_extension");
				if (NegOpt.has_value())
				{
					sol::table V = NegOpt.value();
					FVector Ext;
					Ext.X = V.get_or("x", 0.0);
					Ext.Y = V.get_or("y", 0.0);
					Ext.Z = V.get_or("z", 0.0);
					Mesh->SetNegativeBoundsExtension(Ext);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				const FVector& PosExt = Mesh->GetPositiveBoundsExtension();
				const FVector& NegExt = Mesh->GetNegativeBoundsExtension();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"bounds\") -> pos=(%.1f, %.1f, %.1f) neg=(%.1f, %.1f, %.1f)"),
					PosExt.X, PosExt.Y, PosExt.Z, NegExt.X, NegExt.Y, NegExt.Z));
				sol::table R = Lua.create_table();
				sol::table RPosExt = Lua.create_table();
				RPosExt["x"] = PosExt.X; RPosExt["y"] = PosExt.Y; RPosExt["z"] = PosExt.Z;
				R["positive_extension"] = RPosExt;
				sol::table RNegExt = Lua.create_table();
				RNegExt["x"] = NegExt.X; RNegExt["y"] = NegExt.Y; RNegExt["z"] = NegExt.Z;
				R["negative_extension"] = RNegExt;
				return R;
			}

			// ---- configure("post_process_anim_bp", nil, {blueprint?, lod_threshold?}) ----
			if (FType.Equals(TEXT("post_process_anim_bp"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<std::string> BPOpt = P.get<sol::optional<std::string>>("blueprint");
				if (BPOpt.has_value())
				{
					FString BPPath = UTF8_TO_TCHAR(BPOpt.value().c_str());
					if (BPPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || BPPath.IsEmpty())
					{
						Mesh->SetPostProcessAnimBlueprint(nullptr);
					}
					else
					{
						UClass* BPClass = LoadObject<UClass>(nullptr, *BPPath);
						if (BPClass && BPClass->IsChildOf(UAnimInstance::StaticClass()))
						{
							Mesh->SetPostProcessAnimBlueprint(BPClass);
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"post_process_anim_bp\") -> \"%s\" is not a valid AnimInstance class"), *BPPath));
							return sol::lua_nil;
						}
					}
				}
				if (P.get<sol::optional<int>>("lod_threshold").has_value())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					Mesh->SetPostProcessAnimGraphLODThreshold(P.get<int>("lod_threshold"));
#else
					Mesh->SetPostProcessAnimBPLODThreshold(P.get<int>("lod_threshold"));
#endif
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				TSubclassOf<UAnimInstance> PostProcBP = Mesh->GetPostProcessAnimBlueprint();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"post_process_anim_bp\") -> %s"),
					PostProcBP.Get() ? *PostProcBP.Get()->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["blueprint"] = PostProcBP.Get() ? TCHAR_TO_UTF8(*PostProcBP.Get()->GetPathName()) : "None";
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				R["lod_threshold"] = Mesh->GetPostProcessAnimGraphLODThreshold();
#else
				R["lod_threshold"] = Mesh->GetPostProcessAnimBPLODThreshold();
#endif
				return R;
			}

			// ---- configure("animating_rig", nil, {rig="/Game/Path/..."}) ----
			if (FType.Equals(TEXT("animating_rig"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string RigPath = P.get_or<std::string>("rig", "");
				Mesh->Modify();

				if (RigPath.empty() || FString(UTF8_TO_TCHAR(RigPath.c_str())).Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					Mesh->SetDefaultAnimatingRig(nullptr);
				}
				else
				{
					TSoftObjectPtr<UObject> RigRef(FSoftObjectPath(UTF8_TO_TCHAR(RigPath.c_str())));
					Mesh->SetDefaultAnimatingRig(RigRef);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				TSoftObjectPtr<UObject> CurrentRig = Mesh->GetDefaultAnimatingRig();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"animating_rig\") -> %s"),
					CurrentRig.IsNull() ? TEXT("None") : *CurrentRig.ToString()));
				sol::table R = Lua.create_table();
				R["rig"] = CurrentRig.IsNull() ? "None" : TCHAR_TO_UTF8(*CurrentRig.ToString());
				return R;
			}

			// ---- configure("ray_tracing", nil, {enabled?, min_lod?}) ----
			if (FType.Equals(TEXT("ray_tracing"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				if (P.get<sol::optional<bool>>("enabled").has_value())
					Mesh->SetSupportRayTracing(P.get<bool>("enabled"));
				if (P.get<sol::optional<int>>("min_lod").has_value())
					Mesh->SetRayTracingMinLOD(P.get<int>("min_lod"));

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"ray_tracing\") -> enabled=%s, min_lod=%d"),
					Mesh->GetSupportRayTracing() ? TEXT("true") : TEXT("false"), Mesh->GetRayTracingMinLOD()));
				sol::table R = Lua.create_table();
				R["enabled"] = Mesh->GetSupportRayTracing();
				R["min_lod"] = Mesh->GetRayTracingMinLOD();
				return R;
			}

			// ---- configure("overlay_material", nil, {material?, max_draw_distance?}) ----
			if (FType.Equals(TEXT("overlay_material"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<std::string> MatOpt = P.get<sol::optional<std::string>>("material");
				if (MatOpt.has_value())
				{
					FString MatPath = UTF8_TO_TCHAR(MatOpt.value().c_str());
					if (MatPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || MatPath.IsEmpty())
					{
						Mesh->SetOverlayMaterial(nullptr);
					}
					else
					{
						UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
						if (Mat) Mesh->SetOverlayMaterial(Mat);
						else Session.Log(TEXT("[WARN] configure(\"overlay_material\") -> material not found"));
					}
				}
				if (P.get<sol::optional<double>>("max_draw_distance").has_value())
					Mesh->SetOverlayMaterialMaxDrawDistance(static_cast<float>(P.get<double>("max_draw_distance")));

				Mesh->MarkPackageDirty();

				UMaterialInterface* OverlayMat = Mesh->GetOverlayMaterial();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"overlay_material\") -> %s"),
					OverlayMat ? *OverlayMat->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["material"] = OverlayMat ? TCHAR_TO_UTF8(*OverlayMat->GetPathName()) : "None";
				R["max_draw_distance"] = Mesh->GetOverlayMaterialMaxDrawDistance();
				return R;
			}

			// ---- configure("mesh_deformer", nil, {deformer="/Path"}) ----
			if (FType.Equals(TEXT("mesh_deformer"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string DeformerPath = P.get_or<std::string>("deformer", "");
				Mesh->Modify();

				if (DeformerPath.empty() || FString(UTF8_TO_TCHAR(DeformerPath.c_str())).Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					Mesh->SetDefaultMeshDeformer(nullptr);
				}
				else
				{
					UMeshDeformer* Deformer = LoadObject<UMeshDeformer>(nullptr, *FString(UTF8_TO_TCHAR(DeformerPath.c_str())));
					if (Deformer) Mesh->SetDefaultMeshDeformer(Deformer);
					else Session.Log(TEXT("[WARN] configure(\"mesh_deformer\") -> deformer not found"));
				}

				Mesh->MarkPackageDirty();

				UMeshDeformer* CurrentDeformer = Mesh->GetDefaultMeshDeformer();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"mesh_deformer\") -> %s"),
					CurrentDeformer ? *CurrentDeformer->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["deformer"] = CurrentDeformer ? TCHAR_TO_UTF8(*CurrentDeformer->GetPathName()) : "None";
				return R;
			}

			// ---- configure("floor_offset", nil, {offset=0.0}) ----
			if (FType.Equals(TEXT("floor_offset"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				if (P.get<sol::optional<double>>("offset").has_value())
					Mesh->SetFloorOffset(static_cast<float>(P.get<double>("offset")));

				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"floor_offset\") -> %.2f"), Mesh->GetFloorOffset()));
				sol::table R = Lua.create_table();
				R["offset"] = Mesh->GetFloorOffset();
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: lod, material, socket, physics, nanite, skeleton, lod_settings, bounds, post_process_anim_bp, animating_rig, ray_tracing, overlay_material, mesh_deformer, floor_offset"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// build() — rebuild mesh after changes
		// ================================================================
		AssetObj.set_function("build", [Mesh, &Session](sol::table /*Self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh)) { Session.Log(TEXT("[FAIL] build -> asset no longer valid")); return sol::lua_nil; }
			Mesh->Build();
			Mesh->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] build() -> %s rebuilt"), *Mesh->GetName()));
			sol::table R = Lua.create_table();
			R["rebuilt"] = true;
			R["lod_count"] = Mesh->GetLODNum();
			return R;
		});

		// ================================================================
		// info()
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

			const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
			Result["bone_count"] = RefSkel.GetNum();
			Result["lod_count"] = Mesh->GetLODNum();

			const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
			Result["material_count"] = Materials.Num();

			const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();
			Result["morph_target_count"] = MorphTargets.Num();

			// Sockets: mesh-only + skeleton sockets
			const auto& MeshSockets = Mesh->GetMeshOnlySocketList();
			int32 SkeletonSocketCount = 0;
			USkeleton* Skeleton = Mesh->GetSkeleton();
			if (Skeleton)
			{
				SkeletonSocketCount = Skeleton->Sockets.Num();
			}
			Result["socket_count"] = MeshSockets.Num() + SkeletonSocketCount;
			Result["mesh_socket_count"] = MeshSockets.Num();
			Result["skeleton_socket_count"] = SkeletonSocketCount;

			// Skeleton path
			Result["skeleton"] = Skeleton ? TCHAR_TO_UTF8(*Skeleton->GetPathName()) : "None";

			// Physics assets
			UPhysicsAsset* PhysAsset = Mesh->GetPhysicsAsset();
			Result["physics_asset"] = PhysAsset ? TCHAR_TO_UTF8(*PhysAsset->GetPathName()) : "None";

			UPhysicsAsset* ShadowPhysAsset = Mesh->GetShadowPhysicsAsset();
			Result["shadow_physics_asset"] = ShadowPhysAsset ? TCHAR_TO_UTF8(*ShadowPhysAsset->GetPathName()) : "None";

			// Nanite
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			const FMeshNaniteSettings& NaniteSettings = Mesh->GetNaniteSettings();
			Result["nanite_enabled"] = static_cast<bool>(NaniteSettings.bEnabled);
#else
			Result["nanite_enabled"] = false;
#endif

			// MinLod
			Result["min_lod"] = Mesh->GetMinLodIdx();
			Result["disable_below_min_lod_stripping"] = Mesh->GetDisableBelowMinLodStripping().Default;

			// Vertex colors
			Result["has_vertex_colors"] = Mesh->GetHasVertexColors();

			// Post-process anim BP
			TSubclassOf<UAnimInstance> PostProcBP = Mesh->GetPostProcessAnimBlueprint();
			if (PostProcBP)
			{
				UClass* BPClass = PostProcBP.Get();
				Result["post_process_anim_bp"] = BPClass ? TCHAR_TO_UTF8(*BPClass->GetPathName()) : "None";
			}
			else
			{
				Result["post_process_anim_bp"] = "None";
			}

			// Default animating rig
			TSoftObjectPtr<UObject> DefaultRig = Mesh->GetDefaultAnimatingRig();
			if (!DefaultRig.IsNull())
			{
				Result["default_animating_rig"] = TCHAR_TO_UTF8(*DefaultRig.ToString());
			}
			else
			{
				Result["default_animating_rig"] = "None";
			}

			// Bounds extension
			const FVector& PosExt = Mesh->GetPositiveBoundsExtension();
			const FVector& NegExt = Mesh->GetNegativeBoundsExtension();
			sol::table BoundsInfo = Lua.create_table();
			sol::table PosExtT = Lua.create_table();
			PosExtT["x"] = PosExt.X; PosExtT["y"] = PosExt.Y; PosExtT["z"] = PosExt.Z;
			BoundsInfo["positive_extension"] = PosExtT;
			sol::table NegExtT = Lua.create_table();
			NegExtT["x"] = NegExt.X; NegExtT["y"] = NegExt.Y; NegExtT["z"] = NegExt.Z;
			BoundsInfo["negative_extension"] = NegExtT;
			FBoxSphereBounds Bounds = Mesh->GetBounds();
			sol::table BoundsOrigin = Lua.create_table();
			BoundsOrigin["x"] = Bounds.Origin.X; BoundsOrigin["y"] = Bounds.Origin.Y; BoundsOrigin["z"] = Bounds.Origin.Z;
			BoundsInfo["origin"] = BoundsOrigin;
			sol::table BoundsExtent = Lua.create_table();
			BoundsExtent["x"] = Bounds.BoxExtent.X; BoundsExtent["y"] = Bounds.BoxExtent.Y; BoundsExtent["z"] = Bounds.BoxExtent.Z;
			BoundsInfo["box_extent"] = BoundsExtent;
			BoundsInfo["sphere_radius"] = Bounds.SphereRadius;
			Result["bounds"] = BoundsInfo;

			// Ray tracing
			Result["support_ray_tracing"] = Mesh->GetSupportRayTracing();
			Result["ray_tracing_min_lod"] = Mesh->GetRayTracingMinLOD();

			// Overlay material
			UMaterialInterface* OverlayMat = Mesh->GetOverlayMaterial();
			Result["overlay_material"] = OverlayMat ? TCHAR_TO_UTF8(*OverlayMat->GetPathName()) : "None";
			Result["overlay_material_max_draw_distance"] = Mesh->GetOverlayMaterialMaxDrawDistance();

			// Mesh deformer
			UMeshDeformer* MeshDeformer = Mesh->GetDefaultMeshDeformer();
			Result["default_mesh_deformer"] = MeshDeformer ? TCHAR_TO_UTF8(*MeshDeformer->GetPathName()) : "None";

			// Floor offset
			Result["floor_offset"] = Mesh->GetFloorOffset();

			// Post-process anim graph LOD threshold
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["post_process_anim_graph_lod_threshold"] = Mesh->GetPostProcessAnimGraphLODThreshold();
#else
			Result["post_process_anim_graph_lod_threshold"] = Mesh->GetPostProcessAnimBPLODThreshold();
#endif

			// Clothing
			Result["has_active_clothing"] = Mesh->HasActiveClothingAssets();
			const TArray<UClothingAssetBase*>& ClothingAssets = Mesh->GetMeshClothingAssets();
			Result["clothing_asset_count"] = ClothingAssets.Num();

			// Per-LOD stats from render data
			FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
			if (RenderData)
			{
				sol::table LODs = Lua.create_table();
				for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
				{
					const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIdx];
					sol::table LODEntry = Lua.create_table();
					LODEntry["index"] = LODIdx;
					LODEntry["vertices"] = static_cast<int>(LODData.GetNumVertices());

					uint32 TotalTriangles = 0;
					bool bHasCloth = false;
					for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
					{
						TotalTriangles += Section.NumTriangles;
						if (Section.HasClothingData())
						{
							bHasCloth = true;
						}
					}
					LODEntry["triangles"] = static_cast<int>(TotalTriangles);
					LODEntry["sections"] = LODData.RenderSections.Num();
					LODEntry["has_cloth"] = bHasCloth;

					// LOD info (screen size, hysteresis)
					const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
					if (LODInfo)
					{
						LODEntry["screen_size"] = LODInfo->ScreenSize.GetDefault();
						LODEntry["lod_hysteresis"] = LODInfo->LODHysteresis;
					}

					LODs[LODIdx + 1] = LODEntry;
				}
				Result["lods"] = LODs;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s: %d bones, %d LODs, %d materials, %d morph targets"),
				*Mesh->GetName(), RefSkel.GetNum(), Mesh->GetLODNum(), Materials.Num(), MorphTargets.Num()));
			return Result;
		});

		// ================================================================
		// list(type, opts?)
		// ================================================================
		AssetObj.set_function("list", [Mesh, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			// ---- list() / list("all") -> info() ----
			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("bones") / list("bones", {tree=true}) / list("bones", {transforms=true}) ----
			if (FType.Equals(TEXT("bones"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("bone"), ESearchCase::IgnoreCase))
			{
				const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
				const TArray<FMeshBoneInfo>& BoneInfos = RefSkel.GetRefBoneInfo();
				const TArray<FTransform>& RefBonePose = RefSkel.GetRefBonePose();
				int32 NumBones = RefSkel.GetNum();

				// Check for options
				bool bTree = false;
				bool bTransforms = false;
				if (OptsOpt.has_value())
				{
					bTree = OptsOpt.value().get_or("tree", false);
					bTransforms = OptsOpt.value().get_or("transforms", false);
				}

				if (bTree)
				{
					// Pre-compute children map in O(n)
					TMap<int32, TArray<int32>> ChildrenMap;
					TArray<int32> RootBones;
					for (int32 i = 0; i < NumBones; ++i)
					{
						int32 ParentIdx = BoneInfos[i].ParentIndex;
						if (ParentIdx == INDEX_NONE)
						{
							RootBones.Add(i);
						}
						else
						{
							ChildrenMap.FindOrAdd(ParentIdx).Add(i);
						}
					}

					// Build hierarchical tree using pre-computed map
					TFunction<sol::table(int32)> BuildBoneTree = [&](int32 BoneIndex) -> sol::table
					{
						sol::table BoneEntry = Lua.create_table();
						BoneEntry["index"] = BoneIndex;
						BoneEntry["name"] = TCHAR_TO_UTF8(*BoneInfos[BoneIndex].Name.ToString());
						BoneEntry["parent_index"] = BoneInfos[BoneIndex].ParentIndex;
						if (BoneInfos[BoneIndex].ParentIndex >= 0 && BoneInfos[BoneIndex].ParentIndex < NumBones)
						{
							BoneEntry["parent_name"] = TCHAR_TO_UTF8(*BoneInfos[BoneInfos[BoneIndex].ParentIndex].Name.ToString());
						}
						else
						{
							BoneEntry["parent_name"] = "None";
						}

						const TArray<int32>* ChildIndices = ChildrenMap.Find(BoneIndex);
						if (ChildIndices && ChildIndices->Num() > 0)
						{
							sol::table Children = Lua.create_table();
							int32 ChildIdx = 1;
							for (int32 ChildBoneIdx : *ChildIndices)
							{
								Children[ChildIdx++] = BuildBoneTree(ChildBoneIdx);
							}
							BoneEntry["children"] = Children;
						}
						return BoneEntry;
					};

					sol::table Result = Lua.create_table();
					int32 RootIdx = 1;
					for (int32 RootBoneIdx : RootBones)
					{
						Result[RootIdx++] = BuildBoneTree(RootBoneIdx);
					}

					Session.Log(FString::Printf(TEXT("[OK] list(\"bones\", {tree=true}) -> %d bones"), NumBones));
					return Result;
				}
				else
				{
					// Flat list (optionally with transforms)
					sol::table Result = Lua.create_table();
					for (int32 i = 0; i < NumBones; ++i)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						E["name"] = TCHAR_TO_UTF8(*BoneInfos[i].Name.ToString());
						E["parent_index"] = BoneInfos[i].ParentIndex;
						if (BoneInfos[i].ParentIndex >= 0 && BoneInfos[i].ParentIndex < NumBones)
						{
							E["parent_name"] = TCHAR_TO_UTF8(*BoneInfos[BoneInfos[i].ParentIndex].Name.ToString());
						}
						else
						{
							E["parent_name"] = "None";
						}

						if (bTransforms && i < RefBonePose.Num())
						{
							const FTransform& BoneTransform = RefBonePose[i];

							sol::table Loc = Lua.create_table();
							FVector Translation = BoneTransform.GetTranslation();
							Loc["x"] = Translation.X;
							Loc["y"] = Translation.Y;
							Loc["z"] = Translation.Z;
							E["location"] = Loc;

							sol::table Rot = Lua.create_table();
							FRotator Rotation = BoneTransform.Rotator();
							Rot["pitch"] = Rotation.Pitch;
							Rot["yaw"] = Rotation.Yaw;
							Rot["roll"] = Rotation.Roll;
							E["rotation"] = Rot;

							sol::table Scl = Lua.create_table();
							FVector Scale = BoneTransform.GetScale3D();
							Scl["x"] = Scale.X;
							Scl["y"] = Scale.Y;
							Scl["z"] = Scale.Z;
							E["scale"] = Scl;
						}

						Result[i + 1] = E;
					}

					if (bTransforms)
					{
						Session.Log(FString::Printf(TEXT("[OK] list(\"bones\", {transforms=true}) -> %d"), NumBones));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[OK] list(\"bones\") -> %d"), NumBones));
					}
					return Result;
				}
			}

			// ---- list("materials") ----
			if (FType.Equals(TEXT("materials"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Materials.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["slot_name"] = TCHAR_TO_UTF8(*Materials[i].MaterialSlotName.ToString());
					if (Materials[i].MaterialInterface)
					{
						E["material_path"] = TCHAR_TO_UTF8(*Materials[i].MaterialInterface->GetPathName());
						E["material_name"] = TCHAR_TO_UTF8(*Materials[i].MaterialInterface->GetName());
					}
					else
					{
						E["material_path"] = "None";
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
				int32 Idx = 1;

				// Mesh-only sockets
				const auto& MeshSockets = Mesh->GetMeshOnlySocketList();
				for (USkeletalMeshSocket* Socket : MeshSockets)
				{
					if (!Socket) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());
					E["bone_name"] = TCHAR_TO_UTF8(*Socket->BoneName.ToString());
					E["source"] = "mesh";

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

					E["force_always_animated"] = Socket->bForceAlwaysAnimated;

					Result[Idx++] = E;
				}

				// Skeleton sockets
				USkeleton* Skeleton = Mesh->GetSkeleton();
				if (Skeleton)
				{
					for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
					{
						if (!Socket) continue;
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());
						E["bone_name"] = TCHAR_TO_UTF8(*Socket->BoneName.ToString());
						E["source"] = "skeleton";

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

						E["force_always_animated"] = Socket->bForceAlwaysAnimated;

						Result[Idx++] = E;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"sockets\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("morph_targets") ----
			if (FType.Equals(TEXT("morph_targets"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase))
			{
				const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MorphTargets.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = MorphTargets[i] ? TCHAR_TO_UTF8(*MorphTargets[i]->GetName()) : "(null)";
					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"morph_targets\") -> %d"), MorphTargets.Num()));
				return Result;
			}

			// ---- list("lods") ----
			if (FType.Equals(TEXT("lods"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();

				if (RenderData)
				{
					for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
					{
						const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIdx];
						sol::table E = Lua.create_table();
						E["index"] = LODIdx;
						E["vertices"] = static_cast<int>(LODData.GetNumVertices());

						uint32 TotalTriangles = 0;
						bool bHasCloth = false;
						sol::table Sections = Lua.create_table();
						for (int32 SecIdx = 0; SecIdx < LODData.RenderSections.Num(); ++SecIdx)
						{
							const FSkelMeshRenderSection& Section = LODData.RenderSections[SecIdx];
							TotalTriangles += Section.NumTriangles;
							if (Section.HasClothingData())
							{
								bHasCloth = true;
							}

							sol::table SecEntry = Lua.create_table();
							SecEntry["index"] = SecIdx;
							SecEntry["material_index"] = static_cast<int>(Section.MaterialIndex);
							SecEntry["vertices"] = static_cast<int>(Section.NumVertices);
							SecEntry["triangles"] = static_cast<int>(Section.NumTriangles);
							SecEntry["max_bone_influences"] = Section.MaxBoneInfluences;
							SecEntry["has_cloth"] = Section.HasClothingData();
							SecEntry["disabled"] = Section.bDisabled;
							Sections[SecIdx + 1] = SecEntry;
						}

						E["triangles"] = static_cast<int>(TotalTriangles);
						E["section_count"] = LODData.RenderSections.Num();
						E["sections"] = Sections;
						E["has_cloth"] = bHasCloth;

						// LOD settings from FSkeletalMeshLODInfo
						const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
						if (LODInfo)
						{
							E["screen_size"] = LODInfo->ScreenSize.GetDefault();
							E["lod_hysteresis"] = LODInfo->LODHysteresis;
							E["has_been_simplified"] = static_cast<bool>(LODInfo->bHasBeenSimplified);
							E["weight_of_prioritization"] = LODInfo->WeightOfPrioritization;
							E["morph_target_position_error_tolerance"] = LODInfo->MorphTargetPositionErrorTolerance;

							// Bones to remove — actual names
							sol::table BRemove = Lua.create_table();
							for (int32 bi = 0; bi < LODInfo->BonesToRemove.Num(); ++bi)
							{
								BRemove[bi + 1] = TCHAR_TO_UTF8(*LODInfo->BonesToRemove[bi].BoneName.ToString());
							}
							E["bones_to_remove"] = BRemove;
							E["bones_to_remove_count"] = LODInfo->BonesToRemove.Num();

							// Bones to prioritize — actual names
							sol::table BPrioritize = Lua.create_table();
							for (int32 bi = 0; bi < LODInfo->BonesToPrioritize.Num(); ++bi)
							{
								BPrioritize[bi + 1] = TCHAR_TO_UTF8(*LODInfo->BonesToPrioritize[bi].BoneName.ToString());
							}
							E["bones_to_prioritize"] = BPrioritize;
							E["bones_to_prioritize_count"] = LODInfo->BonesToPrioritize.Num();
						}

						Result[LODIdx + 1] = E;
					}
				}
				else
				{
					// Fallback: just LOD count without render data details
					for (int32 LODIdx = 0; LODIdx < Mesh->GetLODNum(); ++LODIdx)
					{
						sol::table E = Lua.create_table();
						E["index"] = LODIdx;

						const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
						if (LODInfo)
						{
							E["screen_size"] = LODInfo->ScreenSize.GetDefault();
							E["lod_hysteresis"] = LODInfo->LODHysteresis;
							E["has_been_simplified"] = static_cast<bool>(LODInfo->bHasBeenSimplified);
						}

						Result[LODIdx + 1] = E;
					}
				}

				int32 Count = RenderData ? RenderData->LODRenderData.Num() : Mesh->GetLODNum();
				Session.Log(FString::Printf(TEXT("[OK] list(\"lods\") -> %d"), Count));
				return Result;
			}

			// ---- list("clothing") ----
			if (FType.Equals(TEXT("clothing"), ESearchCase::IgnoreCase))
			{
				const TArray<UClothingAssetBase*>& ClothingAssets = Mesh->GetMeshClothingAssets();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ClothingAssets.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					if (ClothingAssets[i])
					{
						E["name"] = TCHAR_TO_UTF8(*ClothingAssets[i]->GetName());
						E["guid"] = TCHAR_TO_UTF8(*ClothingAssets[i]->GetAssetGuid().ToString());
					}
					else
					{
						E["name"] = "(null)";
						E["guid"] = "";
					}
					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"clothing\") -> %d"), ClothingAssets.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: bones, materials, sockets, morph_targets, lods, clothing"), *FType));
			return sol::lua_nil;
		});

		// ----------------------------------------------------------------
		// get_clothing_asset(index_or_name) -> sub-object with get/set/list_properties
		// Enables full reflection into cloth config (stiffness, damping, etc.)
		// ----------------------------------------------------------------
		AssetObj.set_function("get_clothing_asset", [Mesh, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Mesh))
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> mesh no longer valid"));
				return sol::lua_nil;
			}

			const TArray<UClothingAssetBase*>& ClothingAssets = Mesh->GetMeshClothingAssets();
			if (ClothingAssets.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> no clothing assets attached"));
				return sol::lua_nil;
			}

			UClothingAssetBase* FoundCloth = nullptr;

			if (Identifier.is<int>())
			{
				int32 ZeroIndex = Identifier.as<int>() - 1;
				if (ZeroIndex >= 0 && ZeroIndex < ClothingAssets.Num())
				{
					FoundCloth = ClothingAssets[ZeroIndex];
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_clothing_asset(%d) -> index out of range (count=%d)"), ZeroIndex + 1, ClothingAssets.Num()));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				FString SearchName = UTF8_TO_TCHAR(Identifier.as<std::string>().c_str());
				for (UClothingAssetBase* CA : ClothingAssets)
				{
					if (CA && CA->GetName().Contains(SearchName, ESearchCase::IgnoreCase))
					{
						FoundCloth = CA;
						break;
					}
				}
				if (!FoundCloth)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_clothing_asset(\"%s\") -> not found"), *SearchName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> pass name (string) or index (number, 1-based)"));
				return sol::lua_nil;
			}

			if (!FoundCloth)
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> clothing asset is null"));
				return sol::lua_nil;
			}

			// Build sub-object table with get/set/list_properties
			sol::table ClothObj = Lua.create_table();
			ClothObj["name"] = TCHAR_TO_UTF8(*FoundCloth->GetName());
			ClothObj["class_name"] = TCHAR_TO_UTF8(*FoundCloth->GetClass()->GetName());

			// get(property)
			ClothObj.set_function("get", [FoundCloth, &Session](sol::table /*self*/,
				const std::string& PropertyName, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

				if (!IsValid(FoundCloth))
				{
					Session.Log(TEXT("[FAIL] clothing:get -> no longer valid"));
					return sol::lua_nil;
				}

				FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*FProp), FoundCloth->GetClass());
				if (!Prop)
				{
					// Case-insensitive fallback
					for (TFieldIterator<FProperty> PropIt(FoundCloth->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName().Equals(FProp, ESearchCase::IgnoreCase))
						{
							Prop = *PropIt;
							break;
						}
					}
				}
				if (!Prop)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] clothing:get(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				FString Value = NeoStackToolUtils::GetPropertyValueAsString(FoundCloth, Prop, FoundCloth);
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Value)));
			});

			// set(property, value)
			ClothObj.set_function("set", [FoundCloth, Mesh, &Session](sol::table /*self*/,
				const std::string& PropertyName, const std::string& Value, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
				FString FValue = UTF8_TO_TCHAR(Value.c_str());

				if (!IsValid(FoundCloth))
				{
					Session.Log(TEXT("[FAIL] clothing:set -> no longer valid"));
					return sol::lua_nil;
				}

				FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*FProp), FoundCloth->GetClass());
				if (!Prop)
				{
					for (TFieldIterator<FProperty> PropIt(FoundCloth->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName().Equals(FProp, ESearchCase::IgnoreCase))
						{
							Prop = *PropIt;
							break;
						}
					}
				}
				if (!Prop)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] clothing:set(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				FoundCloth->Modify();
				FoundCloth->PreEditChange(Prop);

				const TCHAR* Result = Prop->ImportText_InContainer(*FValue, FoundCloth, FoundCloth, PPF_None);
				if (!Result)
				{
					FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::ValueSet);
					FoundCloth->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] clothing:set(\"%s\", \"%s\") -> ImportText failed"), *FProp, *FValue));
					return sol::lua_nil;
				}

				Mesh->MarkPackageDirty();
				FPropertyChangedEvent SuccessEvent(Prop, EPropertyChangeType::ValueSet);
				FoundCloth->PostEditChangeProperty(SuccessEvent);

				Session.Log(FString::Printf(TEXT("[OK] clothing:set(\"%s\") = \"%s\""), *FProp, *FValue));
				return sol::make_object(Lua, true);
			});

			// list_properties(filter?)
			ClothObj.set_function("list_properties", [FoundCloth, &Session](sol::table /*self*/,
				sol::optional<std::string> Filter, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				if (!IsValid(FoundCloth))
				{
					Session.Log(TEXT("[FAIL] clothing:list_properties -> no longer valid"));
					return sol::lua_nil;
				}

				FString FFilter = Filter.has_value() ? UTF8_TO_TCHAR(Filter.value().c_str()) : TEXT("");
				sol::table Result = Lua.create_table();
				int32 Index = 1;

				for (TFieldIterator<FProperty> PropIt(FoundCloth->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;

					FString Name = Property->GetName();
					if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

					FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
					FString Value = NeoStackToolUtils::GetPropertyValueAsString(FoundCloth, Property, FoundCloth);
					FString Category = Property->GetMetaData(TEXT("Category"));
					if (Category.IsEmpty()) Category = TEXT("Default");
					if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Name);
					Entry["type"] = TCHAR_TO_UTF8(*Type);
					Entry["value"] = TCHAR_TO_UTF8(*Value);
					Entry["category"] = TCHAR_TO_UTF8(*Category);
					Result[Index++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] clothing:list_properties(%s) -> %d properties"),
					FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
				return Result;
			});

			Session.Log(FString::Printf(TEXT("[OK] get_clothing_asset(\"%s\") -> sub-object with get/set/list_properties"),
				*FoundCloth->GetName()));
			return ClothObj;
		});
	});
}

REGISTER_LUA_BINDING(SkeletalMesh, SkeletalMeshDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSkeletalMesh(Lua, Session);
});
