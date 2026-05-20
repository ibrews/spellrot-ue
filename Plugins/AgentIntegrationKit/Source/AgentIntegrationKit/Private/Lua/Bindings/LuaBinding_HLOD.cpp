// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODBuilder.h"
#include "WorldPartition/HLOD/HLODModifier.h"
#include "WorldPartition/HLOD/HLODSourceActors.h"
#include "WorldPartition/HLOD/HLODSourceActorsFromCell.h"
#include "WorldPartition/HLOD/HLODSourceActorsFromLevel.h"
#include "HierarchicalLOD.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/LODActor.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> HLODDocs = {
	{ TEXT("hlod_list_actors()"), TEXT("List all World Partition HLOD actors in the current level"), TEXT("table[]") },
	{ TEXT("hlod_build(actor_label?, force?)"), TEXT("Build HLOD for a specific actor or all — force=true to rebuild unconditionally"), TEXT("int or nil") },
	{ TEXT("hlod_check_hash(actor_label?)"), TEXT("Check if HLOD actor(s) need rebuild by comparing hashes"), TEXT("table or table[]") },
	{ TEXT("hlod_get_layers()"), TEXT("List all UHLODLayer assets found in the project via asset registry"), TEXT("table[]") },
	{ TEXT("hlod_configure_layer(path, options)"), TEXT("Configure an HLOD layer — options: layer_type, parent_layer, cell_size, loading_range, is_spatially_loaded, linked_layer, hlod_actor_class, hlod_modifier_class"), TEXT("true or nil") },
	{ TEXT("hlod_create_layer(path, options?)"), TEXT("Create a new UHLODLayer asset at the given content path"), TEXT("{name, path} or nil") },
	{ TEXT("hlod_get_source_actors(actor_label)"), TEXT("Get source actor info for a World Partition HLOD actor"), TEXT("table or nil") },
	{ TEXT("hlod_get_stats(actor_label)"), TEXT("Get build stats (triangle counts, texture sizes) for an HLOD actor"), TEXT("table or nil") },
	{ TEXT("hlod_export(actor_label, options?)"), TEXT("Export HLOD assets — options: export_path, mesh_origin ('Actor'/'World')"), TEXT("table or nil") },
	{ TEXT("hlod_legacy_build(force?)"), TEXT("Trigger a legacy (non-WP) HLOD build for the current level"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_clear()"), TEXT("Clear all legacy HLOD data from the current level"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_needs_build(force?)"), TEXT("Check if the legacy HLOD system needs a rebuild"), TEXT("bool") },
	{ TEXT("hlod_list_lod_actors()"), TEXT("List all legacy LOD actors (ALODActor) in the current level — includes triangle counts"), TEXT("table[]") },
};

// ─── Helpers ───

static FString HLODLayerTypeToString(EHLODLayerType Type)
{
	switch (Type)
	{
	case EHLODLayerType::Instancing:       return TEXT("Instancing");
	case EHLODLayerType::MeshMerge:        return TEXT("MeshMerge");
	case EHLODLayerType::MeshSimplify:     return TEXT("MeshSimplify");
	case EHLODLayerType::MeshApproximate:  return TEXT("MeshApproximate");
	case EHLODLayerType::Custom:           return TEXT("Custom");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	case EHLODLayerType::CustomHLODActor:  return TEXT("CustomHLODActor");
#endif
	default:                               return TEXT("Unknown");
	}
}

static bool StringToHLODLayerType(const FString& Str, EHLODLayerType& OutType)
{
	if (Str.Equals(TEXT("Instancing"), ESearchCase::IgnoreCase))           { OutType = EHLODLayerType::Instancing; return true; }
	if (Str.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase))            { OutType = EHLODLayerType::MeshMerge; return true; }
	if (Str.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase))         { OutType = EHLODLayerType::MeshSimplify; return true; }
	if (Str.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase))      { OutType = EHLODLayerType::MeshApproximate; return true; }
	if (Str.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))               { OutType = EHLODLayerType::Custom; return true; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (Str.Equals(TEXT("CustomHLODActor"), ESearchCase::IgnoreCase))      { OutType = EHLODLayerType::CustomHLODActor; return true; }
#endif
	return false;
}

static UHLODLayer* LoadHLODLayerByPath(const FString& Path)
{
	// Try direct load first
	UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *Path);
	if (Layer) return Layer;

	// Try as content path (e.g. "/Game/HLOD/MyLayer")
	FString FullPath = Path;
	if (!FullPath.EndsWith(TEXT(".") + FPackageName::GetAssetPackageExtension()))
	{
		FString PackagePath = FPackageName::ObjectPathToPackageName(FullPath);
		FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		FString ObjectPath = PackagePath + TEXT(".") + AssetName;
		Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
	}
	return Layer;
}

static bool ApplyHLODLayerOptions(UHLODLayer* Layer, const sol::table& Options, FString& OutError)
{
	// --- Phase 1: Validate all options that can fail BEFORE modifying anything ---
	EHLODLayerType NewType = Layer->GetLayerType();
	bool bSetType = false;
	auto TypeOpt = Options.get<sol::optional<std::string>>("layer_type");
	if (TypeOpt.has_value())
	{
		FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
		if (!StringToHLODLayerType(TypeStr, NewType))
		{
			OutError = FString::Printf(TEXT("Unknown layer_type '%s' (valid: Instancing, MeshMerge, MeshSimplify, MeshApproximate, Custom, CustomHLODActor)"), *TypeStr);
			return false;
		}
		bSetType = true;
	}

	UHLODLayer* ResolvedParent = nullptr;
	bool bSetParent = false;
	bool bClearParent = false;
	auto ParentOpt = Options.get<sol::optional<std::string>>("parent_layer");
	if (ParentOpt.has_value())
	{
		FString ParentPath = UTF8_TO_TCHAR(ParentOpt.value().c_str());
		if (ParentPath.IsEmpty())
		{
			bClearParent = true;
		}
		else
		{
			ResolvedParent = LoadHLODLayerByPath(ParentPath);
			if (!ResolvedParent)
			{
				OutError = FString::Printf(TEXT("Could not load parent_layer '%s'"), *ParentPath);
				return false;
			}
			bSetParent = true;
		}
	}

	UHLODLayer* ResolvedLinked = nullptr;
	bool bSetLinked = false;
	bool bClearLinked = false;
	auto LinkedOpt = Options.get<sol::optional<std::string>>("linked_layer");
	if (LinkedOpt.has_value())
	{
		FString LinkedPath = UTF8_TO_TCHAR(LinkedOpt.value().c_str());
		if (LinkedPath.IsEmpty())
		{
			bClearLinked = true;
		}
		else
		{
			ResolvedLinked = LoadHLODLayerByPath(LinkedPath);
			if (!ResolvedLinked)
			{
				OutError = FString::Printf(TEXT("Could not load linked_layer '%s'"), *LinkedPath);
				return false;
			}
			bSetLinked = true;
		}
	}

	// Validate hlod_actor_class if provided
	UClass* ResolvedActorClass = nullptr;
	bool bSetActorClass = false;
	auto ActorClassOpt = Options.get<sol::optional<std::string>>("hlod_actor_class");
	if (ActorClassOpt.has_value())
	{
		FString ClassPath = UTF8_TO_TCHAR(ActorClassOpt.value().c_str());
		if (ClassPath.IsEmpty())
		{
			bSetActorClass = true; // will clear
		}
		else
		{
			ResolvedActorClass = LoadClass<AWorldPartitionHLOD>(nullptr, *ClassPath);
			if (!ResolvedActorClass)
			{
				OutError = FString::Printf(TEXT("Could not load hlod_actor_class '%s'"), *ClassPath);
				return false;
			}
			bSetActorClass = true;
		}
	}

	// Validate hlod_modifier_class if provided
	UClass* ResolvedModifierClass = nullptr;
	bool bSetModifierClass = false;
	auto ModifierClassOpt = Options.get<sol::optional<std::string>>("hlod_modifier_class");
	if (ModifierClassOpt.has_value())
	{
		FString ClassPath = UTF8_TO_TCHAR(ModifierClassOpt.value().c_str());
		if (ClassPath.IsEmpty())
		{
			bSetModifierClass = true; // will clear
		}
		else
		{
			ResolvedModifierClass = LoadClass<UWorldPartitionHLODModifier>(nullptr, *ClassPath);
			if (!ResolvedModifierClass)
			{
				OutError = FString::Printf(TEXT("Could not load hlod_modifier_class '%s'"), *ClassPath);
				return false;
			}
			bSetModifierClass = true;
		}
	}

	// --- Phase 2: All validation passed — now apply changes ---
	FScopedTransaction Transaction(FText::FromString(TEXT("Configure HLOD Layer")));
	Layer->Modify();

	if (bSetType) Layer->SetLayerType(NewType);
	if (bClearParent) Layer->SetParentLayer(nullptr);
	if (bSetParent) Layer->SetParentLayer(ResolvedParent);

	// cell_size and loading_range are deprecated in 5.7 but the properties still exist
	sol::optional<double> CellSizeOpt = Options.get<sol::optional<double>>("cell_size");
	if (CellSizeOpt.has_value())
	{
		FProperty* Prop = Layer->GetClass()->FindPropertyByName(TEXT("CellSize"));
		if (Prop)
		{
			int32 Val = static_cast<int32>(CellSizeOpt.value());
			Prop->SetValue_InContainer(Layer, &Val);
		}
	}

	sol::optional<double> LoadingRangeOpt = Options.get<sol::optional<double>>("loading_range");
	if (LoadingRangeOpt.has_value())
	{
		FProperty* Prop = Layer->GetClass()->FindPropertyByName(TEXT("LoadingRange"));
		if (Prop)
		{
			double Val = LoadingRangeOpt.value();
			Prop->SetValue_InContainer(Layer, &Val);
		}
	}

	// bIsSpatiallyLoaded is a bitfield (uint32:1) — must use FBoolProperty typed accessor
	auto SpatialOpt = Options.get<sol::optional<bool>>("is_spatially_loaded");
	if (SpatialOpt.has_value())
	{
		FBoolProperty* BoolProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bIsSpatiallyLoaded")));
		if (BoolProp)
		{
			BoolProp->SetPropertyValue_InContainer(Layer, SpatialOpt.value());
		}
	}

	// Linked layer via reflection (FObjectProperty)
	if (bClearLinked || bSetLinked)
	{
		FObjectProperty* ObjProp = CastField<FObjectProperty>(Layer->GetClass()->FindPropertyByName(TEXT("LinkedLayer")));
		if (ObjProp)
		{
			ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Layer), bSetLinked ? ResolvedLinked : nullptr);
		}
	}

	// HLOD actor class via reflection (TSubclassOf stored as FClassProperty)
	if (bSetActorClass)
	{
		FClassProperty* ClassProp = CastField<FClassProperty>(Layer->GetClass()->FindPropertyByName(TEXT("HLODActorClass")));
		if (ClassProp)
		{
			ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(Layer), ResolvedActorClass);
		}
	}

	// HLOD modifier class via reflection
	if (bSetModifierClass)
	{
		FClassProperty* ClassProp = CastField<FClassProperty>(Layer->GetClass()->FindPropertyByName(TEXT("HLODModifierClass")));
		if (ClassProp)
		{
			ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(Layer), ResolvedModifierClass);
		}
	}

	Layer->PostEditChange();
	Layer->MarkPackageDirty();
	return true;
}

// ─── Binding ───

static void BindHLOD(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- hlod_list_actors() ----
	Lua.set_function("hlod_list_actors", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_list_actors -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetName()));
			Entry["lod_level"] = (int)HLODActor->GetLODLevel();
			Entry["min_visible_distance"] = HLODActor->GetMinVisibleDistance();
			Entry["hash"] = (double)HLODActor->GetHLODHash();

			FBox Bounds = HLODActor->GetHLODBounds();
			if (Bounds.IsValid)
			{
				sol::table BEntry = Lua.create_table();
				BEntry["min_x"] = Bounds.Min.X;
				BEntry["min_y"] = Bounds.Min.Y;
				BEntry["min_z"] = Bounds.Min.Z;
				BEntry["max_x"] = Bounds.Max.X;
				BEntry["max_y"] = Bounds.Max.Y;
				BEntry["max_z"] = Bounds.Max.Z;
				Entry["bounds"] = BEntry;
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_list_actors -> %d HLOD actors"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_build(actor_label?, force?) ----
	Lua.set_function("hlod_build", [&Session](sol::optional<std::string> ActorLabel, sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_build -> no editor world"));
			return sol::lua_nil;
		}

		bool bForce = Force.value_or(false);
		FString FilterLabel = ActorLabel.has_value() ? UTF8_TO_TCHAR(ActorLabel.value().c_str()) : FString();
		int32 BuiltCount = 0;

		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			if (!FilterLabel.IsEmpty() && HLODActor->GetActorLabel() != FilterLabel)
				continue;

			HLODActor->BuildHLOD(bForce);
			BuiltCount++;
		}

		if (BuiltCount == 0 && !FilterLabel.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_build -> no HLOD actor found with label '%s'"), *FilterLabel));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_build -> built %d HLOD actor(s)%s"), BuiltCount, bForce ? TEXT(" (forced)") : TEXT("")));
		return sol::make_object(Lua, BuiltCount);
	});

	// ---- hlod_check_hash(actor_label?) ----
	Lua.set_function("hlod_check_hash", [&Session](sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_check_hash -> no editor world"));
			return sol::lua_nil;
		}

		FString FilterLabel = ActorLabel.has_value() ? UTF8_TO_TCHAR(ActorLabel.value().c_str()) : FString();
		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			if (!FilterLabel.IsEmpty() && HLODActor->GetActorLabel() != FilterLabel)
				continue;

			uint32 StoredHash = HLODActor->GetHLODHash();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			uint32 CurrentHash = HLODActor->ComputeHLODHash();
#else
			uint32 CurrentHash = StoredHash; // ComputeHLODHash not available pre-5.7
#endif

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetActorLabel()));
			Entry["stored_hash"] = (double)StoredHash;
			Entry["current_hash"] = (double)CurrentHash;
			Entry["needs_rebuild"] = (StoredHash != CurrentHash);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_check_hash -> checked %d HLOD actor(s)"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_get_layers() ----
	Lua.set_function("hlod_get_layers", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		// Use asset registry to find persisted UHLODLayer assets (not transient/PIE objects)
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssetsByClass(UHLODLayer::StaticClass()->GetClassPathName(), AssetDataList);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const FAssetData& AssetData : AssetDataList)
		{
			UHLODLayer* Layer = Cast<UHLODLayer>(AssetData.GetAsset());
			if (!Layer) continue;

			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Layer->GetName()));
			Entry["path"] = std::string(TCHAR_TO_UTF8(*Layer->GetPathName()));
			Entry["layer_type"] = std::string(TCHAR_TO_UTF8(*HLODLayerTypeToString(Layer->GetLayerType())));
			Entry["requires_warmup"] = Layer->DoesRequireWarmup();

			UHLODLayer* Parent = Layer->GetParentLayer();
			if (Parent)
			{
				Entry["parent_layer"] = std::string(TCHAR_TO_UTF8(*Parent->GetPathName()));
			}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UHLODLayer* Linked = Layer->GetLinkedLayer();
			if (Linked)
			{
				Entry["linked_layer"] = std::string(TCHAR_TO_UTF8(*Linked->GetPathName()));
			}
#endif

			const TSubclassOf<UHLODBuilder> BuilderClass = Layer->GetHLODBuilderClass();
			if (BuilderClass)
			{
				Entry["builder_class"] = std::string(TCHAR_TO_UTF8(*BuilderClass->GetName()));
			}

			const TSubclassOf<AWorldPartitionHLOD> ActorClass = Layer->GetHLODActorClass();
			if (ActorClass && ActorClass != AWorldPartitionHLOD::StaticClass())
			{
				Entry["hlod_actor_class"] = std::string(TCHAR_TO_UTF8(*ActorClass->GetPathName()));
			}

			const TSubclassOf<UWorldPartitionHLODModifier> ModifierClass = Layer->GetHLODModifierClass();
			if (ModifierClass)
			{
				Entry["hlod_modifier_class"] = std::string(TCHAR_TO_UTF8(*ModifierClass->GetPathName()));
			}

			// cell_size/loading_range/is_spatially_loaded — deprecated in 5.7 but still accessible via reflection
			FProperty* CellSizeProp = Layer->GetClass()->FindPropertyByName(TEXT("CellSize"));
			if (CellSizeProp)
			{
				int32 CellSize = 0;
				CellSizeProp->GetValue_InContainer(Layer, &CellSize);
				Entry["cell_size"] = CellSize;
			}

			FProperty* LoadingRangeProp = Layer->GetClass()->FindPropertyByName(TEXT("LoadingRange"));
			if (LoadingRangeProp)
			{
				double LR = 0.0;
				LoadingRangeProp->GetValue_InContainer(Layer, &LR);
				Entry["loading_range"] = LR;
			}

			// bIsSpatiallyLoaded is a bitfield (uint32:1) — must use FBoolProperty typed accessor
			FBoolProperty* SpatialProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bIsSpatiallyLoaded")));
			if (SpatialProp)
			{
				Entry["is_spatially_loaded"] = SpatialProp->GetPropertyValue_InContainer(Layer);
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_get_layers -> %d HLOD layers"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_configure_layer(path, options) ----
	Lua.set_function("hlod_configure_layer", [&Session](const std::string& Path, sol::table Options, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(Path.c_str());

		UHLODLayer* Layer = LoadHLODLayerByPath(FPath);
		if (!Layer)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_configure_layer -> could not load HLODLayer at '%s'"), *FPath));
			return sol::lua_nil;
		}

		FString Error;
		if (!ApplyHLODLayerOptions(Layer, Options, Error))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_configure_layer -> %s"), *Error));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_configure_layer -> configured '%s'"), *Layer->GetName()));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_create_layer(path, options?) ----
	Lua.set_function("hlod_create_layer", [&Session](const std::string& Path, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(Path.c_str());

		// Parse path into package and asset name
		FString PackagePath = FPackageName::ObjectPathToPackageName(FPath);
		FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		if (AssetName.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_create_layer -> invalid path '%s'"), *FPath));
			return sol::lua_nil;
		}

		// Check if already exists
		FString ObjectPath = PackagePath + TEXT(".") + AssetName;
		if (LoadObject<UHLODLayer>(nullptr, *ObjectPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_create_layer -> asset already exists at '%s'"), *FPath));
			return sol::lua_nil;
		}

		// Create package and object
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_create_layer -> failed to create package '%s'"), *PackagePath));
			return sol::lua_nil;
		}

		// Use factory for proper defaults — LoadClass is more robust than FindObject
		UClass* FactoryClass = LoadClass<UFactory>(nullptr, TEXT("/Script/WorldPartitionEditor.HLODLayerFactory"));
		UHLODLayer* Layer = nullptr;
		if (FactoryClass)
		{
			UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
			if (Factory)
			{
				Layer = Cast<UHLODLayer>(Factory->FactoryCreateNew(UHLODLayer::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr, GWarn));
			}
		}

		if (!Layer)
		{
			// Fallback: direct creation
			Layer = NewObject<UHLODLayer>(Package, *AssetName, RF_Public | RF_Standalone);
		}

		if (!Layer)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_create_layer -> failed to create HLODLayer '%s'"), *AssetName));
			return sol::lua_nil;
		}

		// Apply options if provided
		if (Options.has_value())
		{
			FString Error;
			if (!ApplyHLODLayerOptions(Layer, Options.value(), Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] hlod_create_layer -> created but failed to configure: %s"), *Error));
				return sol::lua_nil;
			}
		}

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(Layer);
		Layer->MarkPackageDirty();

		sol::table Result = Lua.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*Layer->GetName()));
		Result["path"] = std::string(TCHAR_TO_UTF8(*Layer->GetPathName()));

		Session.Log(FString::Printf(TEXT("[OK] hlod_create_layer -> created '%s'"), *Layer->GetPathName()));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_get_source_actors(actor_label) ----
	Lua.set_function("hlod_get_source_actors", [&Session](const std::string& ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_get_source_actors -> no editor world"));
			return sol::lua_nil;
		}

		FString FilterLabel = UTF8_TO_TCHAR(ActorLabel.c_str());

		AWorldPartitionHLOD* FoundActor = nullptr;
		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;
			if (HLODActor->GetActorLabel() == FilterLabel)
			{
				FoundActor = HLODActor;
				break;
			}
		}

		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_source_actors -> no HLOD actor with label '%s'"), *FilterLabel));
			return sol::lua_nil;
		}

		UWorldPartitionHLODSourceActors* SourceActors = FoundActor->GetSourceActors();
		if (!SourceActors)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_source_actors -> HLOD actor '%s' has no source actors data"), *FilterLabel));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();

		// Report HLOD layer if available
		const UHLODLayer* HLODLayer = SourceActors->GetHLODLayer();
		if (HLODLayer)
		{
			Result["hlod_layer"] = std::string(TCHAR_TO_UTF8(*HLODLayer->GetPathName()));
		}

		// Check if cell-based source actors
		if (UWorldPartitionHLODSourceActorsFromCell* CellSource = Cast<UWorldPartitionHLODSourceActorsFromCell>(SourceActors))
		{
			Result["source_type"] = std::string("cell");
			const TArray<FWorldPartitionRuntimeCellObjectMapping>& Actors = CellSource->GetActors();
			sol::table ActorsTable = Lua.create_table();
			int32 Idx = 1;
			for (const FWorldPartitionRuntimeCellObjectMapping& Mapping : Actors)
			{
				sol::table Entry = Lua.create_table();
				Entry["path"] = std::string(TCHAR_TO_UTF8(*Mapping.Path.ToString()));
				Entry["package"] = std::string(TCHAR_TO_UTF8(*Mapping.Package.ToString()));
				Entry["base_class"] = std::string(TCHAR_TO_UTF8(*Mapping.BaseClass.ToString()));
				Entry["native_class"] = std::string(TCHAR_TO_UTF8(*Mapping.NativeClass.ToString()));
				ActorsTable[Idx++] = Entry;
			}
			Result["actors"] = ActorsTable;
			Result["actor_count"] = Actors.Num();

			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has %d cell-based source actors"), *FilterLabel, Actors.Num()));
		}
		// Check if level-based source actors
		else if (UWorldPartitionHLODSourceActorsFromLevel* LevelSource = Cast<UWorldPartitionHLODSourceActorsFromLevel>(SourceActors))
		{
			Result["source_type"] = std::string("level");
			const TSoftObjectPtr<UWorld>& SourceLevel = LevelSource->GetSourceLevel();
			Result["source_level"] = std::string(TCHAR_TO_UTF8(*SourceLevel.ToString()));

			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has level-based source: %s"), *FilterLabel, *SourceLevel.ToString()));
		}
		else
		{
			Result["source_type"] = std::string(TCHAR_TO_UTF8(*SourceActors->GetClass()->GetName()));
			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has %s source actors"), *FilterLabel, *SourceActors->GetClass()->GetName()));
		}

		return sol::make_object(Lua, Result);
	});

	// ---- hlod_get_stats(actor_label) ----
	Lua.set_function("hlod_get_stats", [&Session](const std::string& ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_get_stats -> no editor world"));
			return sol::lua_nil;
		}

		FString FilterLabel = UTF8_TO_TCHAR(ActorLabel.c_str());

		AWorldPartitionHLOD* FoundActor = nullptr;
		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;
			if (HLODActor->GetActorLabel() == FilterLabel)
			{
				FoundActor = HLODActor;
				break;
			}
		}

		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_stats -> no HLOD actor with label '%s'"), *FilterLabel));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["label"] = std::string(TCHAR_TO_UTF8(*FoundActor->GetActorLabel()));
		Result["lod_level"] = (int)FoundActor->GetLODLevel();
		Result["hash"] = (double)FoundActor->GetHLODHash();
		Result["min_visible_distance"] = FoundActor->GetMinVisibleDistance();

		// Query known stat names
		static const FName StatNames[] = {
			FName(TEXT("MeshTriangleCount")),
			FName(TEXT("MeshVertexCount")),
			FName(TEXT("MeshUVChannelCount")),
			FName(TEXT("MaterialBaseColorTextureSize")),
			FName(TEXT("MaterialNormalTextureSize")),
			FName(TEXT("MaterialMetallicTextureSize")),
			FName(TEXT("MaterialRoughnessTextureSize")),
			FName(TEXT("MaterialSpecularTextureSize")),
			FName(TEXT("InputTriangleCount")),
			FName(TEXT("InputVertexCount")),
		};

		sol::table StatsTable = Lua.create_table();
		int32 StatCount = 0;
		for (const FName& StatName : StatNames)
		{
			int64 Value = FoundActor->GetStat(StatName);
			if (Value != 0)
			{
				StatsTable[std::string(TCHAR_TO_UTF8(*StatName.ToString()))] = (double)Value;
				StatCount++;
			}
		}
		Result["stats"] = StatsTable;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		// Input stats — referenced assets per builder
		const FHLODBuildInputStats& InputStats = FoundActor->GetInputStats();
		if (InputStats.BuildersReferencedAssets.Num() > 0)
		{
			sol::table InputTable = Lua.create_table();
			for (const auto& Pair : InputStats.BuildersReferencedAssets)
			{
				sol::table BuilderEntry = Lua.create_table();
				sol::table MeshesTable = Lua.create_table();
				int32 MeshIdx = 1;
				for (const auto& MeshPair : Pair.Value.StaticMeshes)
				{
					sol::table MeshEntry = Lua.create_table();
					MeshEntry["asset"] = std::string(TCHAR_TO_UTF8(*MeshPair.Key.ToString()));
					MeshEntry["count"] = (int)MeshPair.Value;
					MeshesTable[MeshIdx++] = MeshEntry;
				}
				BuilderEntry["static_meshes"] = MeshesTable;
				InputTable[std::string(TCHAR_TO_UTF8(*Pair.Key.ToString()))] = BuilderEntry;
			}
			Result["input_stats"] = InputTable;
		}
#endif

		Session.Log(FString::Printf(TEXT("[OK] hlod_get_stats -> '%s' has %d stats"), *FilterLabel, StatCount));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_export(actor_label, options?) ----
	Lua.set_function("hlod_export", [&Session](const std::string& ActorLabel, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_export -> no editor world"));
			return sol::lua_nil;
		}

		FString FilterLabel = UTF8_TO_TCHAR(ActorLabel.c_str());

		AWorldPartitionHLOD* FoundActor = nullptr;
		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;
			if (HLODActor->GetActorLabel() == FilterLabel)
			{
				FoundActor = HLODActor;
				break;
			}
		}

		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_export -> no HLOD actor with label '%s'"), *FilterLabel));
			return sol::lua_nil;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FExportHLODAssetsParams Params;
		Params.MeshOrigin = EExportHLODMeshOrigin::World;

		if (Options.has_value())
		{
			auto OriginOpt = Options.value().get<sol::optional<std::string>>("mesh_origin");
			if (OriginOpt.has_value())
			{
				FString OriginStr = UTF8_TO_TCHAR(OriginOpt.value().c_str());
				if (OriginStr.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
					Params.MeshOrigin = EExportHLODMeshOrigin::Actor;
			}

			auto PathOpt = Options.value().get<sol::optional<std::string>>("export_path");
			if (PathOpt.has_value())
			{
				Params.ExportRootPath.Path = UTF8_TO_TCHAR(PathOpt.value().c_str());
			}
		}

		FString ErrorMessage;
		TArray<UObject*> ExportedAssets = FoundActor->ExportHLODAssets(Params, ErrorMessage);

		if (!ErrorMessage.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_export -> %s"), *ErrorMessage));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		sol::table AssetsTable = Lua.create_table();
		int32 Idx = 1;
		for (UObject* Asset : ExportedAssets)
		{
			if (Asset)
			{
				AssetsTable[Idx++] = std::string(TCHAR_TO_UTF8(*Asset->GetPathName()));
			}
		}
		Result["assets"] = AssetsTable;
		Result["count"] = ExportedAssets.Num();

		Session.Log(FString::Printf(TEXT("[OK] hlod_export -> exported %d assets from '%s'"), ExportedAssets.Num(), *FilterLabel));
		return sol::make_object(Lua, Result);
#else
		Session.Log(TEXT("[FAIL] hlod_export requires UE 5.7+"));
		return sol::lua_nil;
#endif
	});

	// ---- hlod_legacy_build(force?) ----
	Lua.set_function("hlod_legacy_build", [&Session](sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_build -> no editor world"));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);

		bool bForce = Force.value_or(false);
		if (!bForce && !Builder.NeedsBuild())
		{
			Session.Log(TEXT("[OK] hlod_legacy_build -> already up to date (use force=true to rebuild)"));
			return sol::make_object(Lua, true);
		}

		Builder.Build();
		Session.Log(TEXT("[OK] hlod_legacy_build -> build complete"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_clear() ----
	Lua.set_function("hlod_legacy_clear", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_clear -> no editor world"));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.ClearHLODs();

		Session.Log(TEXT("[OK] hlod_legacy_clear -> cleared all legacy HLODs"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_needs_build(force?) ----
	Lua.set_function("hlod_legacy_needs_build", [&Session](sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_needs_build -> no editor world"));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		bool bNeedsBuild = Builder.NeedsBuild(Force.value_or(false));

		Session.Log(FString::Printf(TEXT("[OK] hlod_legacy_needs_build -> %s"), bNeedsBuild ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bNeedsBuild);
	});

	// ---- hlod_list_lod_actors() ----
	Lua.set_function("hlod_list_lod_actors", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_list_lod_actors -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (TActorIterator<ALODActor> It(World); It; ++It)
		{
			ALODActor* LODActor = *It;
			if (!LODActor) continue;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*LODActor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*LODActor->GetName()));
			Entry["lod_level"] = (int)LODActor->LODLevel;
			Entry["sub_actor_count"] = LODActor->SubActors.Num();
			Entry["is_dirty"] = !LODActor->IsBuilt();
			Entry["draw_distance"] = (double)LODActor->GetDrawDistance();
			Entry["triangles_in_sub_actors"] = (double)LODActor->GetNumTrianglesInSubActors();
			Entry["triangles_in_merged_mesh"] = (double)LODActor->GetNumTrianglesInMergedMesh();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_list_lod_actors -> %d legacy LOD actors"), Idx - 1));
		return sol::make_object(Lua, Result);
	});
}

REGISTER_LUA_BINDING(HLOD, HLODDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindHLOD(Lua, Session);
});
