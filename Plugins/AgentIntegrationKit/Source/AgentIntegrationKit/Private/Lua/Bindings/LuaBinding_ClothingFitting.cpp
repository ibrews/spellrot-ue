// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "ClothingAssetBase.h"
#include "ClothingAsset.h"
#include "ClothingAssetFactory.h"
#include "ClothPhysicalMeshData.h"
#include "PointWeightMap.h"
#include "ClothLODData.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static USkeletalMesh* LoadSkeletalMeshFromPath(const FString& Path)
{
	FSoftObjectPath SoftPath(Path);
	UObject* Obj = SoftPath.TryLoad();
	return Cast<USkeletalMesh>(Obj);
}

static bool ResolveWeightMapTarget(const std::string& TargetStr, EWeightMapTargetCommon& OutTarget)
{
	FString Target = UTF8_TO_TCHAR(TargetStr.c_str());
	if (Target.Equals(TEXT("max_distance"), ESearchCase::IgnoreCase))
	{
		OutTarget = EWeightMapTargetCommon::MaxDistance;
		return true;
	}
	if (Target.Equals(TEXT("backstop_distance"), ESearchCase::IgnoreCase))
	{
		OutTarget = EWeightMapTargetCommon::BackstopDistance;
		return true;
	}
	if (Target.Equals(TEXT("backstop_radius"), ESearchCase::IgnoreCase))
	{
		OutTarget = EWeightMapTargetCommon::BackstopRadius;
		return true;
	}
	if (Target.Equals(TEXT("anim_drive_stiffness"), ESearchCase::IgnoreCase))
	{
		OutTarget = EWeightMapTargetCommon::AnimDriveStiffness;
		return true;
	}
	return false;
}

static FString WeightMapTargetToString(uint32 TargetId)
{
	switch (static_cast<EWeightMapTargetCommon>(TargetId))
	{
	case EWeightMapTargetCommon::MaxDistance: return TEXT("max_distance");
	case EWeightMapTargetCommon::BackstopDistance: return TEXT("backstop_distance");
	case EWeightMapTargetCommon::BackstopRadius: return TEXT("backstop_radius");
	case EWeightMapTargetCommon::AnimDriveStiffness: return TEXT("anim_drive_stiffness");
	default:
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		if (TargetId >= static_cast<uint32>(EWeightMapTargetCommon::FirstUserTarget)
			&& TargetId <= static_cast<uint32>(EWeightMapTargetCommon::LastUserTarget))
		{
			return FString::Printf(TEXT("user_target_%u"), TargetId);
		}
#endif
		return FString::Printf(TEXT("target_%u"), TargetId);
	}
}

static UClothingAssetCommon* FindClothingAsset(USkeletalMesh* Mesh, sol::object Identifier)
{
	if (!Mesh) return nullptr;
	const TArray<UClothingAssetBase*>& ClothingAssets = Mesh->GetMeshClothingAssets();
	if (ClothingAssets.Num() == 0) return nullptr;

	if (Identifier.is<int>())
	{
		int32 ZeroIndex = Identifier.as<int>() - 1;
		if (ZeroIndex >= 0 && ZeroIndex < ClothingAssets.Num())
		{
			return Cast<UClothingAssetCommon>(ClothingAssets[ZeroIndex]);
		}
	}
	else if (Identifier.is<std::string>())
	{
		FString SearchName = UTF8_TO_TCHAR(Identifier.as<std::string>().c_str());
		// Try exact match first
		for (UClothingAssetBase* CA : ClothingAssets)
		{
			if (CA && CA->GetName().Equals(SearchName, ESearchCase::IgnoreCase))
			{
				return Cast<UClothingAssetCommon>(CA);
			}
		}
		// Fallback to substring match
		for (UClothingAssetBase* CA : ClothingAssets)
		{
			if (CA && CA->GetName().Contains(SearchName, ESearchCase::IgnoreCase))
			{
				return Cast<UClothingAssetCommon>(CA);
			}
		}
	}
	return nullptr;
}

static float LuaToFloat(sol::object V)
{
	if (V.is<double>()) return static_cast<float>(V.as<double>());
	if (V.is<int>()) return static_cast<float>(V.as<int>());
	return 0.f;
}

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> ClothingFittingDocs = {
	// Creation & deletion
	{ TEXT("clothing_create(mesh_path, params)"),
	  TEXT("Create a clothing asset from a skeletal mesh section. params: {lod=0, section=0, name='ClothingAsset'}. Returns the clothing asset name or nil on failure."),
	  TEXT("string or nil") },
	{ TEXT("clothing_remove(mesh_path, lod, section)"),
	  TEXT("Remove a clothing asset from a skeletal mesh at the specified LOD and section."),
	  TEXT("bool") },

	// Binding
	{ TEXT("clothing_bind(mesh_path, clothing_name_or_index, lod, section, asset_lod?)"),
	  TEXT("Bind a clothing asset to a skeletal mesh section. asset_lod defaults to 0."),
	  TEXT("bool") },
	{ TEXT("clothing_unbind(mesh_path, clothing_name_or_index, lod?)"),
	  TEXT("Unbind a clothing asset from a skeletal mesh. If lod omitted, unbinds from all LODs."),
	  TEXT("bool") },
	{ TEXT("clothing_list_bindings(mesh_path, lod?)"),
	  TEXT("List all clothing bindings on a skeletal mesh. If lod provided, filters to that LOD only."),
	  TEXT("table[] or nil") },

	// Asset listing
	{ TEXT("clothing_list_assets(mesh_path)"),
	  TEXT("List all clothing assets on a skeletal mesh (whether bound or not)."),
	  TEXT("table[] or nil") },

	// Weight maps
	{ TEXT("clothing_set_weight_map(mesh_path, clothing_name_or_index, lod, target, values)"),
	  TEXT("Set a weight map on a clothing asset's physical mesh. target: 'max_distance', 'backstop_distance', 'backstop_radius', 'anim_drive_stiffness'. values: table of floats (one per sim vertex) or a single float for uniform fill."),
	  TEXT("bool") },
	{ TEXT("clothing_get_weight_map(mesh_path, clothing_name_or_index, lod, target)"),
	  TEXT("Get a weight map from a clothing asset. Returns table of float values."),
	  TEXT("table or nil") },

	// Sim mesh info & vertex data
	{ TEXT("clothing_get_sim_mesh_info(mesh_path, clothing_name_or_index, lod?)"),
	  TEXT("Get simulation mesh info: vertex_count, triangle_count, max_bone_weights, num_fixed_verts, num_lods, asset_name, reference_bone_index, physics_asset, used_bones, weight_maps, cloth_configs, lod_settings."),
	  TEXT("table or nil") },
	{ TEXT("clothing_get_vertices(mesh_path, clothing_name_or_index, lod?)"),
	  TEXT("Get sim mesh vertex positions, normals, indices, and inverse masses."),
	  TEXT("table or nil") },

	// Config via UE reflection
	{ TEXT("clothing_configure(mesh_path, clothing_name_or_index, config_name, property, value)"),
	  TEXT("Set a property on a clothing config (e.g., 'UChaosClothConfig', 'DampingCoefficient', 0.05). Uses UE reflection."),
	  TEXT("bool") },

	// Maintenance ops
	{ TEXT("clothing_refresh_bones(mesh_path, clothing_name_or_index?)"),
	  TEXT("Refresh bone mapping for clothing asset(s). If clothing not specified, refreshes all."),
	  TEXT("bool") },
	{ TEXT("clothing_rebuild_lod_transition(mesh_path, clothing_name_or_index?)"),
	  TEXT("Build LOD transition data for clothing asset(s)."),
	  TEXT("bool") },
	{ TEXT("clothing_invalidate_cache(mesh_path, clothing_name_or_index?)"),
	  TEXT("Invalidate cached data for clothing asset(s), forcing regeneration."),
	  TEXT("bool") },

	// Physics asset
	{ TEXT("clothing_set_physics_asset(mesh_path, clothing_name_or_index, physics_asset_path)"),
	  TEXT("Set the physics asset used for collision on a clothing asset. Pass empty string to clear."),
	  TEXT("bool") },
};

// ============================================================================
// BINDING
// ============================================================================

static void BindClothingFitting(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// clothing_create
	// ================================================================
	Lua.set_function("clothing_create", [&Session](const std::string& MeshPathStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_create -> could not load skeletal mesh")); return sol::lua_nil; }

		int32 LodIndex = 0, SectionIndex = 0;
		FString AssetName = TEXT("ClothingAsset");
		if (ParamsOpt.has_value())
		{
			sol::table P = ParamsOpt.value();
			LodIndex = P.get_or("lod", 0);
			SectionIndex = P.get_or("section", 0);
			std::string NameStr = P.get_or<std::string>("name", "ClothingAsset");
			AssetName = UTF8_TO_TCHAR(NameStr.c_str());
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Create Clothing Asset")));
		FSkeletalMeshClothBuildParams BuildParams;
		BuildParams.LodIndex = LodIndex;
		BuildParams.SourceSection = SectionIndex;
		BuildParams.AssetName = AssetName;
		BuildParams.bRemapParameters = true;

		UClothingAssetFactory* Factory = NewObject<UClothingAssetFactory>();
		UClothingAssetBase* NewAsset = Factory->CreateFromSkeletalMesh(Mesh, BuildParams);
		if (!NewAsset) { Session.Log(TEXT("[FAIL] clothing_create -> factory returned null")); return sol::lua_nil; }

		FString ResultName = NewAsset->GetName();
		Session.Log(TEXT("[OK] clothing_create -> created '") + ResultName + TEXT("'"));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ResultName)));
	});

	// ================================================================
	// clothing_remove
	// ================================================================
	Lua.set_function("clothing_remove", [&Session](const std::string& MeshPathStr, int LodIndex, int SectionIndex, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_remove -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		FScopedTransaction Transaction(FText::FromString(TEXT("Remove Clothing Asset")));
		Mesh->Modify();
		Mesh->RemoveClothingAsset(LodIndex, SectionIndex);
		Mesh->Build();
		Mesh->MarkPackageDirty();

		Session.Log(TEXT("[OK] clothing_remove -> removed clothing from LOD ") + FString::FromInt(LodIndex) + TEXT(" section ") + FString::FromInt(SectionIndex));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_bind
	// ================================================================
	Lua.set_function("clothing_bind", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		int LodIndex, int SectionIndex, sol::optional<int> AssetLodOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_bind -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_bind -> clothing asset not found")); return sol::make_object(LuaView, false); }

		int32 AssetLod = AssetLodOpt.has_value() ? AssetLodOpt.value() : 0;
		FScopedTransaction Transaction(FText::FromString(TEXT("Bind Clothing")));
		Mesh->Modify();

		bool bSuccess = ClothAsset->BindToSkeletalMesh(Mesh, LodIndex, SectionIndex, AssetLod);
		if (bSuccess) { Mesh->Build(); Mesh->MarkPackageDirty(); }
		Session.Log(bSuccess
			? TEXT("[OK] clothing_bind -> bound '") + ClothAsset->GetName() + TEXT("'")
			: TEXT("[FAIL] clothing_bind -> BindToSkeletalMesh returned false"));
		return sol::make_object(LuaView, bSuccess);
	});

	// ================================================================
	// clothing_unbind
	// ================================================================
	Lua.set_function("clothing_unbind", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		sol::optional<int> LodOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_unbind -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_unbind -> clothing asset not found")); return sol::make_object(LuaView, false); }

		FScopedTransaction Transaction(FText::FromString(TEXT("Unbind Clothing")));
		Mesh->Modify();
		if (LodOpt.has_value()) { ClothAsset->UnbindFromSkeletalMesh(Mesh, LodOpt.value()); }
		else { ClothAsset->UnbindFromSkeletalMesh(Mesh); }
		Mesh->Build();
		Mesh->MarkPackageDirty();

		Session.Log(TEXT("[OK] clothing_unbind -> unbound '") + ClothAsset->GetName() + TEXT("'"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_list_bindings
	// ================================================================
	Lua.set_function("clothing_list_bindings", [&Session](const std::string& MeshPathStr, sol::optional<int> LodOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_list_bindings -> could not load skeletal mesh")); return sol::lua_nil; }

		TArray<ClothingAssetUtils::FClothingAssetMeshBinding> Bindings;
		if (LodOpt.has_value()) { ClothingAssetUtils::GetAllLodMeshClothingAssetBindings(Mesh, Bindings, LodOpt.value()); }
		else { ClothingAssetUtils::GetAllMeshClothingAssetBindings(Mesh, Bindings); }

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Bindings.Num(); ++i)
		{
			const auto& B = Bindings[i];
			sol::table Entry = LuaView.create_table();
			Entry["asset_name"] = B.Asset ? TCHAR_TO_UTF8(*B.Asset->GetName()) : "(null)";
			Entry["asset_guid"] = B.Asset ? TCHAR_TO_UTF8(*B.Asset->GetAssetGuid().ToString()) : "";
			Entry["lod_index"] = B.LODIndex;
			Entry["section_index"] = B.SectionIndex;
			Entry["asset_internal_lod"] = B.AssetInternalLodIndex;
			Result[i + 1] = Entry;
		}

		Session.Log(TEXT("[OK] clothing_list_bindings -> ") + FString::FromInt(Bindings.Num()) + TEXT(" bindings"));
		return Result;
	});

	// ================================================================
	// clothing_list_assets
	// ================================================================
	Lua.set_function("clothing_list_assets", [&Session](const std::string& MeshPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_list_assets -> could not load skeletal mesh")); return sol::lua_nil; }

		const TArray<UClothingAssetBase*>& ClothingAssets = Mesh->GetMeshClothingAssets();
		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < ClothingAssets.Num(); ++i)
		{
			UClothingAssetBase* CA = ClothingAssets[i];
			if (!CA) continue;
			sol::table Entry = LuaView.create_table();
			Entry["index"] = i + 1;
			Entry["name"] = TCHAR_TO_UTF8(*CA->GetName());
			Entry["guid"] = TCHAR_TO_UTF8(*CA->GetAssetGuid().ToString());
			Entry["class"] = TCHAR_TO_UTF8(*CA->GetClass()->GetName());

			UClothingAssetCommon* Common = Cast<UClothingAssetCommon>(CA);
			if (Common)
			{
				Entry["num_lods"] = Common->GetNumLods();
				Entry["reference_bone_index"] = Common->ReferenceBoneIndex;
				Entry["physics_asset"] = Common->PhysicsAsset ? TCHAR_TO_UTF8(*Common->PhysicsAsset->GetPathName()) : "";

				sol::table BonesTable = LuaView.create_table();
				for (int32 j = 0; j < Common->UsedBoneNames.Num(); ++j)
					BonesTable[j + 1] = TCHAR_TO_UTF8(*Common->UsedBoneNames[j].ToString());
				Entry["used_bones"] = BonesTable;

				sol::table ConfigsTable = LuaView.create_table();
				int32 CfgIdx = 1;
				for (const auto& ConfigPair : Common->ClothConfigs)
				{
					if (ConfigPair.Value)
					{
						sol::table CfgEntry = LuaView.create_table();
						CfgEntry["name"] = TCHAR_TO_UTF8(*ConfigPair.Key.ToString());
						CfgEntry["class"] = TCHAR_TO_UTF8(*ConfigPair.Value->GetClass()->GetName());
						ConfigsTable[CfgIdx++] = CfgEntry;
					}
				}
				Entry["cloth_configs"] = ConfigsTable;
			}
			Result[i + 1] = Entry;
		}
		Session.Log(TEXT("[OK] clothing_list_assets -> ") + FString::FromInt(ClothingAssets.Num()) + TEXT(" assets"));
		return Result;
	});

	// ================================================================
	// clothing_set_weight_map
	// ================================================================
	Lua.set_function("clothing_set_weight_map", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		int LodIndex, const std::string& TargetStr, sol::object ValuesObj, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_set_weight_map -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_set_weight_map -> clothing asset not found")); return sol::make_object(LuaView, false); }
		if (!ClothAsset->IsValidLod(LodIndex)) { Session.Log(TEXT("[FAIL] clothing_set_weight_map -> invalid LOD index")); return sol::make_object(LuaView, false); }

		EWeightMapTargetCommon Target;
		if (!ResolveWeightMapTarget(TargetStr, Target))
		{
			Session.Log(TEXT("[FAIL] clothing_set_weight_map -> unknown target. Valid: max_distance, backstop_distance, backstop_radius, anim_drive_stiffness"));
			return sol::make_object(LuaView, false);
		}

		FClothLODDataCommon& LodData = ClothAsset->LodData[LodIndex];
		FClothPhysicalMeshData& PhysMesh = LodData.PhysicalMeshData;
		int32 NumVerts = PhysMesh.Vertices.Num();
		FPointWeightMap& WeightMap = PhysMesh.FindOrAddWeightMap(Target);

		FScopedTransaction Transaction(FText::FromString(TEXT("Set Cloth Weight Map")));
		ClothAsset->Modify();

		if (ValuesObj.is<double>() || ValuesObj.is<int>())
		{
			WeightMap.Values.Init(LuaToFloat(ValuesObj), NumVerts);
		}
		else if (ValuesObj.is<sol::table>())
		{
			sol::table ValsTable = ValuesObj.as<sol::table>();
			int32 TableSize = static_cast<int32>(ValsTable.size());
			if (TableSize != NumVerts)
			{
				Session.Log(TEXT("[FAIL] clothing_set_weight_map -> value count (") + FString::FromInt(TableSize) + TEXT(") != vertex count (") + FString::FromInt(NumVerts) + TEXT(")"));
				return sol::make_object(LuaView, false);
			}
			WeightMap.Values.SetNumUninitialized(NumVerts);
			for (int32 i = 0; i < NumVerts; ++i)
				WeightMap.Values[i] = LuaToFloat(ValsTable[i + 1]);
		}
		else
		{
			Session.Log(TEXT("[FAIL] clothing_set_weight_map -> values must be a number (uniform) or table of floats"));
			return sol::make_object(LuaView, false);
		}

#if WITH_EDITORONLY_DATA
		WeightMap.CurrentTarget = static_cast<uint8>(Target);
		WeightMap.bEnabled = true;

		bool bFoundInEditor = false;
		for (FPointWeightMap& EditorMap : LodData.PointWeightMaps)
		{
			if (EditorMap.CurrentTarget == static_cast<uint8>(Target))
			{
				EditorMap.Values = WeightMap.Values;
				EditorMap.bEnabled = true;
				bFoundInEditor = true;
				break;
			}
		}
		if (!bFoundInEditor)
		{
			FPointWeightMap NewMap;
			NewMap.Values = WeightMap.Values;
			NewMap.CurrentTarget = static_cast<uint8>(Target);
			NewMap.bEnabled = true;
			LodData.PointWeightMaps.Add(MoveTemp(NewMap));
		}
#endif

		ClothAsset->ApplyParameterMasks(true, true);
		Mesh->MarkPackageDirty();
		Session.Log(TEXT("[OK] clothing_set_weight_map -> set ") + FString::FromInt(NumVerts) + TEXT(" weights"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_get_weight_map
	// ================================================================
	Lua.set_function("clothing_get_weight_map", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		int LodIndex, const std::string& TargetStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_get_weight_map -> could not load skeletal mesh")); return sol::lua_nil; }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_get_weight_map -> clothing asset not found")); return sol::lua_nil; }
		if (!ClothAsset->IsValidLod(LodIndex)) { Session.Log(TEXT("[FAIL] clothing_get_weight_map -> invalid LOD index")); return sol::lua_nil; }

		EWeightMapTargetCommon Target;
		if (!ResolveWeightMapTarget(TargetStr, Target)) { Session.Log(TEXT("[FAIL] clothing_get_weight_map -> unknown target")); return sol::lua_nil; }

		const FClothPhysicalMeshData& PhysMesh = ClothAsset->LodData[LodIndex].PhysicalMeshData;
		const FPointWeightMap* Map = PhysMesh.FindWeightMap(Target);
		if (!Map || Map->Num() == 0) { Session.Log(TEXT("[FAIL] clothing_get_weight_map -> no weight map for target")); return sol::lua_nil; }

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Map->Num(); ++i)
			Result[i + 1] = Map->Values[i];

		Session.Log(TEXT("[OK] clothing_get_weight_map -> ") + FString::FromInt(Map->Num()) + TEXT(" values"));
		return Result;
	});

	// ================================================================
	// clothing_get_sim_mesh_info
	// ================================================================
	Lua.set_function("clothing_get_sim_mesh_info", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		sol::optional<int> LodOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_get_sim_mesh_info -> could not load skeletal mesh")); return sol::lua_nil; }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_get_sim_mesh_info -> clothing asset not found")); return sol::lua_nil; }

		int32 LodIndex = LodOpt.has_value() ? LodOpt.value() : 0;
		if (!ClothAsset->IsValidLod(LodIndex)) { Session.Log(TEXT("[FAIL] clothing_get_sim_mesh_info -> invalid LOD index")); return sol::lua_nil; }

		const FClothLODDataCommon& LodData = ClothAsset->LodData[LodIndex];
		const FClothPhysicalMeshData& PhysMesh = LodData.PhysicalMeshData;

		sol::table Result = LuaView.create_table();
		Result["vertex_count"] = PhysMesh.Vertices.Num();
		Result["triangle_count"] = PhysMesh.Indices.Num() / 3;
		Result["max_bone_weights"] = PhysMesh.MaxBoneWeights;
		Result["num_fixed_verts"] = PhysMesh.NumFixedVerts;
		Result["num_lods"] = ClothAsset->GetNumLods();
		Result["asset_name"] = TCHAR_TO_UTF8(*ClothAsset->GetName());
		Result["reference_bone_index"] = ClothAsset->ReferenceBoneIndex;
		Result["physics_asset"] = ClothAsset->PhysicsAsset ? TCHAR_TO_UTF8(*ClothAsset->PhysicsAsset->GetPathName()) : "";

		sol::table BonesTable = LuaView.create_table();
		for (int32 i = 0; i < ClothAsset->UsedBoneNames.Num(); ++i)
			BonesTable[i + 1] = TCHAR_TO_UTF8(*ClothAsset->UsedBoneNames[i].ToString());
		Result["used_bones"] = BonesTable;

		sol::table WeightMaps = LuaView.create_table();
		int32 WmIdx = 1;
		for (const auto& Pair : PhysMesh.WeightMaps)
		{
			sol::table WmEntry = LuaView.create_table();
			WmEntry["target_id"] = static_cast<int>(Pair.Key);
			WmEntry["target_name"] = TCHAR_TO_UTF8(*WeightMapTargetToString(Pair.Key));
			WmEntry["num_values"] = Pair.Value.Num();
			WeightMaps[WmIdx++] = WmEntry;
		}
		Result["weight_maps"] = WeightMaps;

		sol::table Configs = LuaView.create_table();
		int32 CfgIdx = 1;
		for (const auto& ConfigPair : ClothAsset->ClothConfigs)
		{
			if (ConfigPair.Value)
			{
				sol::table CfgEntry = LuaView.create_table();
				CfgEntry["name"] = TCHAR_TO_UTF8(*ConfigPair.Key.ToString());
				CfgEntry["class"] = TCHAR_TO_UTF8(*ConfigPair.Value->GetClass()->GetName());
				Configs[CfgIdx++] = CfgEntry;
			}
		}
		Result["cloth_configs"] = Configs;

		sol::table LodSettings = LuaView.create_table();
		LodSettings["use_multiple_influences"] = LodData.bUseMultipleInfluences;
		LodSettings["skinning_kernel_radius"] = LodData.SkinningKernelRadius;
		LodSettings["smooth_transition"] = LodData.bSmoothTransition;
		Result["lod_settings"] = LodSettings;

		Session.Log(TEXT("[OK] clothing_get_sim_mesh_info -> ") + FString::FromInt(PhysMesh.Vertices.Num()) + TEXT(" verts"));
		return Result;
	});

	// ================================================================
	// clothing_get_vertices
	// ================================================================
	Lua.set_function("clothing_get_vertices", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		sol::optional<int> LodOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_get_vertices -> could not load skeletal mesh")); return sol::lua_nil; }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_get_vertices -> clothing asset not found")); return sol::lua_nil; }

		int32 LodIndex = LodOpt.has_value() ? LodOpt.value() : 0;
		if (!ClothAsset->IsValidLod(LodIndex)) { Session.Log(TEXT("[FAIL] clothing_get_vertices -> invalid LOD index")); return sol::lua_nil; }

		const FClothPhysicalMeshData& PhysMesh = ClothAsset->LodData[LodIndex].PhysicalMeshData;
		sol::table Result = LuaView.create_table();

		sol::table VertsTable = LuaView.create_table();
		for (int32 i = 0; i < PhysMesh.Vertices.Num(); ++i)
		{
			const FVector3f& V = PhysMesh.Vertices[i];
			sol::table Vert = LuaView.create_table();
			Vert["x"] = V.X; Vert["y"] = V.Y; Vert["z"] = V.Z;
			VertsTable[i + 1] = Vert;
		}
		Result["vertices"] = VertsTable;

		sol::table NormalsTable = LuaView.create_table();
		for (int32 i = 0; i < PhysMesh.Normals.Num(); ++i)
		{
			const FVector3f& N = PhysMesh.Normals[i];
			sol::table Norm = LuaView.create_table();
			Norm["x"] = N.X; Norm["y"] = N.Y; Norm["z"] = N.Z;
			NormalsTable[i + 1] = Norm;
		}
		Result["normals"] = NormalsTable;

		sol::table IndicesTable = LuaView.create_table();
		for (int32 i = 0; i < PhysMesh.Indices.Num(); ++i)
			IndicesTable[i + 1] = static_cast<int>(PhysMesh.Indices[i]) + 1; // 1-based Lua
		Result["indices"] = IndicesTable;

		sol::table InvMassTable = LuaView.create_table();
		for (int32 i = 0; i < PhysMesh.InverseMasses.Num(); ++i)
			InvMassTable[i + 1] = PhysMesh.InverseMasses[i];
		Result["inverse_masses"] = InvMassTable;

		Session.Log(TEXT("[OK] clothing_get_vertices -> ") + FString::FromInt(PhysMesh.Vertices.Num()) + TEXT(" vertices"));
		return Result;
	});

	// ================================================================
	// clothing_configure
	// ================================================================
	Lua.set_function("clothing_configure", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		const std::string& ConfigNameStr, const std::string& PropertyStr, sol::object Value, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_configure -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_configure -> clothing asset not found")); return sol::make_object(LuaView, false); }

		FString ConfigName = UTF8_TO_TCHAR(ConfigNameStr.c_str());
		FString PropertyName = UTF8_TO_TCHAR(PropertyStr.c_str());

		UClothConfigBase* TargetConfig = nullptr;
		for (const auto& ConfigPair : ClothAsset->ClothConfigs)
		{
			if (ConfigPair.Value && ConfigPair.Value->GetClass()->GetName().Contains(ConfigName, ESearchCase::IgnoreCase))
			{
				TargetConfig = ConfigPair.Value;
				break;
			}
		}
		if (!TargetConfig) { Session.Log(TEXT("[FAIL] clothing_configure -> config not found: ") + ConfigName); return sol::make_object(LuaView, false); }

		FProperty* Prop = TargetConfig->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Prop) { Session.Log(TEXT("[FAIL] clothing_configure -> property not found: ") + PropertyName); return sol::make_object(LuaView, false); }

		FScopedTransaction Transaction(FText::FromString(TEXT("Configure Clothing")));
		TargetConfig->Modify();

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetConfig);
		bool bSet = false;

		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{ FloatProp->SetPropertyValue(ValuePtr, LuaToFloat(Value)); bSet = true; }
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{ DoubleProp->SetPropertyValue(ValuePtr, Value.is<double>() ? Value.as<double>() : static_cast<double>(LuaToFloat(Value))); bSet = true; }
		else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{ BoolProp->SetPropertyValue(ValuePtr, Value.is<bool>() ? Value.as<bool>() : false); bSet = true; }
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{ IntProp->SetPropertyValue(ValuePtr, Value.is<int>() ? Value.as<int>() : static_cast<int>(LuaToFloat(Value))); bSet = true; }
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{ ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(Value.is<int>() ? Value.as<int>() : 0)); bSet = true; }
		else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			if (UnderlyingProp && Value.is<int>())
			{ UnderlyingProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value.as<int>())); bSet = true; }
		}

		if (!bSet) { Session.Log(TEXT("[FAIL] clothing_configure -> unsupported property type for: ") + PropertyName); return sol::make_object(LuaView, false); }

		FPropertyChangedEvent ChangedEvent(Prop);
		TargetConfig->PostEditChangeProperty(ChangedEvent);
		ClothAsset->MarkPackageDirty();

		Session.Log(TEXT("[OK] clothing_configure -> set ") + PropertyName + TEXT(" on ") + TargetConfig->GetClass()->GetName());
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_refresh_bones
	// ================================================================
	Lua.set_function("clothing_refresh_bones", [&Session](const std::string& MeshPathStr,
		sol::optional<sol::object> ClothIdOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_refresh_bones -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		if (ClothIdOpt.has_value())
		{
			UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothIdOpt.value());
			if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_refresh_bones -> clothing asset not found")); return sol::make_object(LuaView, false); }
			ClothAsset->RefreshBoneMapping(Mesh);
			ClothAsset->CalculateReferenceBoneIndex();
			Session.Log(TEXT("[OK] clothing_refresh_bones -> refreshed '") + ClothAsset->GetName() + TEXT("'"));
		}
		else
		{
			for (UClothingAssetBase* CA : Mesh->GetMeshClothingAssets())
			{
				UClothingAssetCommon* Common = Cast<UClothingAssetCommon>(CA);
				if (Common) { Common->RefreshBoneMapping(Mesh); Common->CalculateReferenceBoneIndex(); }
			}
			Session.Log(TEXT("[OK] clothing_refresh_bones -> refreshed all"));
		}
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_rebuild_lod_transition
	// ================================================================
	Lua.set_function("clothing_rebuild_lod_transition", [&Session](const std::string& MeshPathStr,
		sol::optional<sol::object> ClothIdOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_rebuild_lod_transition -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		if (ClothIdOpt.has_value())
		{
			UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothIdOpt.value());
			if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_rebuild_lod_transition -> clothing asset not found")); return sol::make_object(LuaView, false); }
			ClothAsset->BuildLodTransitionData();
			Session.Log(TEXT("[OK] clothing_rebuild_lod_transition -> rebuilt '") + ClothAsset->GetName() + TEXT("'"));
		}
		else
		{
			for (UClothingAssetBase* CA : Mesh->GetMeshClothingAssets())
			{
				UClothingAssetCommon* Common = Cast<UClothingAssetCommon>(CA);
				if (Common) { Common->BuildLodTransitionData(); }
			}
			Session.Log(TEXT("[OK] clothing_rebuild_lod_transition -> rebuilt all"));
		}
		Mesh->MarkPackageDirty();
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// clothing_invalidate_cache
	// ================================================================
	Lua.set_function("clothing_invalidate_cache", [&Session](const std::string& MeshPathStr,
		sol::optional<sol::object> ClothIdOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_invalidate_cache -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

#if WITH_EDITORONLY_DATA
		if (ClothIdOpt.has_value())
		{
			UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothIdOpt.value());
			if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_invalidate_cache -> clothing asset not found")); return sol::make_object(LuaView, false); }
			ClothAsset->InvalidateFlaggedCachedData(EClothingCachedDataFlagsCommon::All);
			Session.Log(TEXT("[OK] clothing_invalidate_cache -> invalidated '") + ClothAsset->GetName() + TEXT("'"));
		}
		else
		{
			for (UClothingAssetBase* CA : Mesh->GetMeshClothingAssets())
			{
				UClothingAssetCommon* Common = Cast<UClothingAssetCommon>(CA);
				if (Common) { Common->InvalidateFlaggedCachedData(EClothingCachedDataFlagsCommon::All); }
			}
			Session.Log(TEXT("[OK] clothing_invalidate_cache -> invalidated all"));
		}
		Mesh->MarkPackageDirty();
		return sol::make_object(LuaView, true);
#else
		Session.Log(TEXT("[FAIL] clothing_invalidate_cache -> only available in editor builds"));
		return sol::make_object(LuaView, false);
#endif
	});

	// ================================================================
	// clothing_set_physics_asset
	// ================================================================
	Lua.set_function("clothing_set_physics_asset", [&Session](const std::string& MeshPathStr, sol::object ClothId,
		const std::string& PhysAssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(UTF8_TO_TCHAR(MeshPathStr.c_str()));
		if (!Mesh) { Session.Log(TEXT("[FAIL] clothing_set_physics_asset -> could not load skeletal mesh")); return sol::make_object(LuaView, false); }

		UClothingAssetCommon* ClothAsset = FindClothingAsset(Mesh, ClothId);
		if (!ClothAsset) { Session.Log(TEXT("[FAIL] clothing_set_physics_asset -> clothing asset not found")); return sol::make_object(LuaView, false); }

		FString PhysAssetPath = UTF8_TO_TCHAR(PhysAssetPathStr.c_str());
		UPhysicsAsset* PhysAsset = nullptr;
		if (!PhysAssetPath.IsEmpty())
		{
			FSoftObjectPath SoftPath(PhysAssetPath);
			PhysAsset = Cast<UPhysicsAsset>(SoftPath.TryLoad());
			if (!PhysAsset) { Session.Log(TEXT("[FAIL] clothing_set_physics_asset -> could not load: ") + PhysAssetPath); return sol::make_object(LuaView, false); }
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Set Clothing Physics Asset")));
		ClothAsset->Modify();
		ClothAsset->PhysicsAsset = PhysAsset;
		ClothAsset->MarkPackageDirty();

		Session.Log(TEXT("[OK] clothing_set_physics_asset -> set to '") + (PhysAsset ? PhysAsset->GetName() : TEXT("(none)")) + TEXT("'"));
		return sol::make_object(LuaView, true);
	});
}

REGISTER_LUA_BINDING(ClothingFitting, ClothingFittingDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindClothingFitting(Lua, Session);
});
