// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "ClothingAssetBase.h"
#include "ClothingAsset.h"
#include "Engine/SkeletalMesh.h"
#include "ScopedTransaction.h"

#if WITH_CHAOS_OUTFIT_ASSET
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "ChaosOutfitAsset/CollectionOutfitFacade.h"
#include "ChaosOutfitAsset/BodyUserData.h"
#include "ChaosOutfitAsset/OutfitCollection.h"
#endif

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static USkeletalMesh* LoadSkeletalMeshFromPath_Outfit(const FString& Path)
{
	FSoftObjectPath SoftPath(Path);
	UObject* Obj = SoftPath.TryLoad();
	return Cast<USkeletalMesh>(Obj);
}

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> ChaosOutfitDocs = {
#if WITH_CHAOS_OUTFIT_ASSET
	{ TEXT("outfit_list_body_sizes(outfit_asset_path)"),
	  TEXT("List all body sizes defined in a ChaosOutfitAsset. Returns table of {index, name, body_parts, measurements}. Index is 1-based."),
	  TEXT("table[] or nil") },
	{ TEXT("outfit_find_closest_size(outfit_asset_path, measurements)"),
	  TEXT("Find the closest body size to a set of measurements. measurements: {height=180.0, chest=95.0, ...}. Returned index is 1-based."),
	  TEXT("table or nil") },
	{ TEXT("outfit_get_measurements(outfit_asset_path, size_name_or_index)"),
	  TEXT("Get stored body measurements for a specific body size. Index is 1-based."),
	  TEXT("table or nil") },
	{ TEXT("outfit_get_pieces(outfit_asset_path, guid?, body_size?)"),
	  TEXT("Get outfit pieces (cloth asset GUIDs and names) for a specific outfit GUID and body size."),
	  TEXT("table or nil") },
	{ TEXT("outfit_list_guids(outfit_asset_path)"),
	  TEXT("List all outfit GUIDs in a ChaosOutfitAsset."),
	  TEXT("table[] or nil") },
	{ TEXT("body_set_measurements(mesh_path, measurements)"),
	  TEXT("Set body measurements on a skeletal mesh via UChaosOutfitAssetBodyUserData. measurements: {height=180.0, ...}"),
	  TEXT("bool") },
	{ TEXT("body_get_measurements(mesh_path)"),
	  TEXT("Get body measurements from a skeletal mesh's UChaosOutfitAssetBodyUserData."),
	  TEXT("table or nil") },
#endif
};

// ============================================================================
// BINDING
// ============================================================================

static void BindChaosOutfit(sol::state& Lua, FLuaSessionData& Session)
{
#if WITH_CHAOS_OUTFIT_ASSET
	// ================================================================
	// outfit_list_body_sizes(outfit_asset_path)
	// ================================================================
	Lua.set_function("outfit_list_body_sizes", [&Session](const std::string& AssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		FSoftObjectPath SoftPath(AssetPath);
		UObject* Obj = SoftPath.TryLoad();
		UChaosOutfitAsset* OutfitAsset = Cast<UChaosOutfitAsset>(Obj);
		if (!OutfitAsset)
		{
			Session.Log(TEXT("[FAIL] outfit_list_body_sizes -> could not load ChaosOutfitAsset: ") + AssetPath);
			return sol::lua_nil;
		}

		const FManagedArrayCollection& Collection = OutfitAsset->GetOutfitCollection();
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Collection);

		if (!Facade.IsValid())
		{
			Session.Log(TEXT("[FAIL] outfit_list_body_sizes -> outfit collection is not valid"));
			return sol::lua_nil;
		}

		int32 NumSizes = Facade.GetNumBodySizes();
		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < NumSizes; ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["index"] = i + 1; // Lua 1-based
			Entry["name"] = TCHAR_TO_UTF8(*Facade.GetBodySizeName(i));

			sol::table PartsTable = LuaView.create_table();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			TConstArrayView<FSoftObjectPath> BodyParts = Facade.GetBodySizeBodyPartsSkeletalMeshPaths(i);
			for (int32 j = 0; j < BodyParts.Num(); ++j)
			{
				PartsTable[j + 1] = TCHAR_TO_UTF8(*BodyParts[j].ToString());
			}
#else
			TConstArrayView<FString> BodyParts = Facade.GetBodySizeBodyPartsSkeletalMeshes(i);
			for (int32 j = 0; j < BodyParts.Num(); ++j)
			{
				PartsTable[j + 1] = TCHAR_TO_UTF8(*BodyParts[j]);
			}
#endif
			Entry["body_parts"] = PartsTable;

			TMap<FString, float> Measurements = Facade.GetBodySizeMeasurements(i);
			sol::table MeasTable = LuaView.create_table();
			for (const auto& Pair : Measurements)
			{
				MeasTable[TCHAR_TO_UTF8(*Pair.Key)] = Pair.Value;
			}
			Entry["measurements"] = MeasTable;

			Result[i + 1] = Entry;
		}

		Session.Log(TEXT("[OK] outfit_list_body_sizes -> ") + FString::FromInt(NumSizes) + TEXT(" sizes"));
		return Result;
	});

	// ================================================================
	// outfit_find_closest_size(outfit_asset_path, measurements)
	// ================================================================
	Lua.set_function("outfit_find_closest_size", [&Session](const std::string& AssetPathStr,
		sol::table MeasurementsTable, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		FSoftObjectPath SoftPath(AssetPath);
		UObject* Obj = SoftPath.TryLoad();
		UChaosOutfitAsset* OutfitAsset = Cast<UChaosOutfitAsset>(Obj);
		if (!OutfitAsset)
		{
			Session.Log(TEXT("[FAIL] outfit_find_closest_size -> could not load ChaosOutfitAsset: ") + AssetPath);
			return sol::lua_nil;
		}

		const FManagedArrayCollection& Collection = OutfitAsset->GetOutfitCollection();
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Collection);

		if (!Facade.IsValid())
		{
			Session.Log(TEXT("[FAIL] outfit_find_closest_size -> outfit collection is not valid"));
			return sol::lua_nil;
		}

		TMap<FString, float> Measurements;
		for (const auto& Pair : MeasurementsTable)
		{
			if (Pair.first.is<std::string>() && (Pair.second.is<double>() || Pair.second.is<int>()))
			{
				FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
				float Val = Pair.second.is<double>() ? static_cast<float>(Pair.second.as<double>()) : static_cast<float>(Pair.second.as<int>());
				Measurements.Add(Key, Val);
			}
		}

		int32 ClosestIdx = Facade.FindClosestBodySize(Measurements);
		if (ClosestIdx == INDEX_NONE)
		{
			Session.Log(TEXT("[FAIL] outfit_find_closest_size -> no matching body size found"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["index"] = ClosestIdx + 1; // Lua 1-based
		Result["name"] = TCHAR_TO_UTF8(*Facade.GetBodySizeName(ClosestIdx));

		Session.Log(TEXT("[OK] outfit_find_closest_size -> '") + Facade.GetBodySizeName(ClosestIdx) +
			TEXT("' (index ") + FString::FromInt(ClosestIdx + 1) + TEXT(")"));
		return Result;
	});

	// ================================================================
	// outfit_get_measurements(outfit_asset_path, size_name_or_index)
	// ================================================================
	Lua.set_function("outfit_get_measurements", [&Session](const std::string& AssetPathStr,
		sol::object SizeId, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		FSoftObjectPath SoftPath(AssetPath);
		UObject* Obj = SoftPath.TryLoad();
		UChaosOutfitAsset* OutfitAsset = Cast<UChaosOutfitAsset>(Obj);
		if (!OutfitAsset)
		{
			Session.Log(TEXT("[FAIL] outfit_get_measurements -> could not load ChaosOutfitAsset: ") + AssetPath);
			return sol::lua_nil;
		}

		const FManagedArrayCollection& Collection = OutfitAsset->GetOutfitCollection();
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Collection);

		if (!Facade.IsValid())
		{
			Session.Log(TEXT("[FAIL] outfit_get_measurements -> outfit collection is not valid"));
			return sol::lua_nil;
		}

		int32 BodySizeIdx = INDEX_NONE;
		if (SizeId.is<int>())
		{
			BodySizeIdx = SizeId.as<int>() - 1; // Lua 1-based to 0-based
		}
		else if (SizeId.is<std::string>())
		{
			FString SizeName = UTF8_TO_TCHAR(SizeId.as<std::string>().c_str());
			BodySizeIdx = Facade.FindBodySize(SizeName);
		}

		if (BodySizeIdx < 0 || BodySizeIdx >= Facade.GetNumBodySizes())
		{
			Session.Log(TEXT("[FAIL] outfit_get_measurements -> body size not found"));
			return sol::lua_nil;
		}

		TMap<FString, float> Measurements = Facade.GetBodySizeMeasurements(BodySizeIdx);

		sol::table Result = LuaView.create_table();
		for (const auto& Pair : Measurements)
		{
			Result[TCHAR_TO_UTF8(*Pair.Key)] = Pair.Value;
		}

		Session.Log(TEXT("[OK] outfit_get_measurements -> ") + FString::FromInt(Measurements.Num()) + TEXT(" measurements for '") +
			Facade.GetBodySizeName(BodySizeIdx) + TEXT("'"));
		return Result;
	});

	// ================================================================
	// outfit_get_pieces(outfit_asset_path, guid?, body_size?)
	// ================================================================
	Lua.set_function("outfit_get_pieces", [&Session](const std::string& AssetPathStr,
		sol::optional<std::string> GuidOpt, sol::optional<int> BodySizeOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		FSoftObjectPath SoftPath(AssetPath);
		UObject* Obj = SoftPath.TryLoad();
		UChaosOutfitAsset* OutfitAsset = Cast<UChaosOutfitAsset>(Obj);
		if (!OutfitAsset)
		{
			Session.Log(TEXT("[FAIL] outfit_get_pieces -> could not load ChaosOutfitAsset: ") + AssetPath);
			return sol::lua_nil;
		}

		const FManagedArrayCollection& Collection = OutfitAsset->GetOutfitCollection();
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Collection);

		if (!Facade.IsValid())
		{
			Session.Log(TEXT("[FAIL] outfit_get_pieces -> outfit collection is not valid"));
			return sol::lua_nil;
		}

		TArray<FGuid> Guids = Facade.GetOutfitGuids();
		if (Guids.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] outfit_get_pieces -> no outfits in collection"));
			return sol::lua_nil;
		}

		FGuid TargetGuid = Guids[0];
		if (GuidOpt.has_value())
		{
			FGuid::Parse(UTF8_TO_TCHAR(GuidOpt.value().c_str()), TargetGuid);
		}

		int32 BodySize = BodySizeOpt.has_value() ? BodySizeOpt.value() : 0;

		TMap<FGuid, FString> Pieces = Facade.GetOutfitPieces(TargetGuid, BodySize);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const auto& Pair : Pieces)
		{
			sol::table PieceEntry = LuaView.create_table();
			PieceEntry["guid"] = TCHAR_TO_UTF8(*Pair.Key.ToString());
			PieceEntry["name"] = TCHAR_TO_UTF8(*Pair.Value);
			Result[Idx++] = PieceEntry;
		}

		Session.Log(TEXT("[OK] outfit_get_pieces -> ") + FString::FromInt(Pieces.Num()) + TEXT(" pieces"));
		return Result;
	});

	// ================================================================
	// outfit_list_guids(outfit_asset_path)
	// ================================================================
	Lua.set_function("outfit_list_guids", [&Session](const std::string& AssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		FSoftObjectPath SoftPath(AssetPath);
		UObject* Obj = SoftPath.TryLoad();
		UChaosOutfitAsset* OutfitAsset = Cast<UChaosOutfitAsset>(Obj);
		if (!OutfitAsset)
		{
			Session.Log(TEXT("[FAIL] outfit_list_guids -> could not load ChaosOutfitAsset: ") + AssetPath);
			return sol::lua_nil;
		}

		const FManagedArrayCollection& Collection = OutfitAsset->GetOutfitCollection();
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Collection);

		if (!Facade.IsValid())
		{
			Session.Log(TEXT("[FAIL] outfit_list_guids -> outfit collection is not valid"));
			return sol::lua_nil;
		}

		TArray<FGuid> Guids = Facade.GetOutfitGuids();

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Guids.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["guid"] = TCHAR_TO_UTF8(*Guids[i].ToString());

			TArray<int32> BodySizes = Facade.GetOutfitBodySizes(Guids[i]);
			sol::table SizesTable = LuaView.create_table();
			for (int32 j = 0; j < BodySizes.Num(); ++j)
			{
				SizesTable[j + 1] = BodySizes[j];
			}
			Entry["body_sizes"] = SizesTable;

			Result[i + 1] = Entry;
		}

		Session.Log(TEXT("[OK] outfit_list_guids -> ") + FString::FromInt(Guids.Num()) + TEXT(" outfit GUIDs"));
		return Result;
	});

	// ================================================================
	// body_set_measurements(mesh_path, measurements)
	// ================================================================
	Lua.set_function("body_set_measurements", [&Session](const std::string& MeshPathStr,
		sol::table MeasurementsTable, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString MeshPath = UTF8_TO_TCHAR(MeshPathStr.c_str());

		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath_Outfit(MeshPath);
		if (!Mesh)
		{
			Session.Log(TEXT("[FAIL] body_set_measurements -> could not load skeletal mesh: ") + MeshPath);
			return sol::make_object(LuaView, false);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Set Body Measurements")));
		Mesh->Modify();

		UChaosOutfitAssetBodyUserData* BodyData = Cast<UChaosOutfitAssetBodyUserData>(
			Mesh->GetAssetUserDataOfClass(UChaosOutfitAssetBodyUserData::StaticClass()));

		if (!BodyData)
		{
			BodyData = NewObject<UChaosOutfitAssetBodyUserData>(Mesh);
			Mesh->AddAssetUserData(BodyData);
		}

		BodyData->Measurements.Empty();
		for (const auto& Pair : MeasurementsTable)
		{
			if (Pair.first.is<std::string>() && (Pair.second.is<double>() || Pair.second.is<int>()))
			{
				FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
				float Val = Pair.second.is<double>() ? static_cast<float>(Pair.second.as<double>()) : static_cast<float>(Pair.second.as<int>());
				BodyData->Measurements.Add(Key, Val);
			}
		}

		Mesh->MarkPackageDirty();
		Session.Log(TEXT("[OK] body_set_measurements -> set ") + FString::FromInt(BodyData->Measurements.Num()) + TEXT(" measurements"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// body_get_measurements(mesh_path)
	// ================================================================
	Lua.set_function("body_get_measurements", [&Session](const std::string& MeshPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString MeshPath = UTF8_TO_TCHAR(MeshPathStr.c_str());

		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath_Outfit(MeshPath);
		if (!Mesh)
		{
			Session.Log(TEXT("[FAIL] body_get_measurements -> could not load skeletal mesh: ") + MeshPath);
			return sol::lua_nil;
		}

		UChaosOutfitAssetBodyUserData* BodyData = Cast<UChaosOutfitAssetBodyUserData>(
			Mesh->GetAssetUserDataOfClass(UChaosOutfitAssetBodyUserData::StaticClass()));

		if (!BodyData || BodyData->Measurements.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] body_get_measurements -> no body measurements found"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		for (const auto& Pair : BodyData->Measurements)
		{
			Result[TCHAR_TO_UTF8(*Pair.Key)] = Pair.Value;
		}

		Session.Log(TEXT("[OK] body_get_measurements -> ") + FString::FromInt(BodyData->Measurements.Num()) + TEXT(" measurements"));
		return Result;
	});
#else
	(void)Session;
	(void)Lua;
#endif // WITH_CHAOS_OUTFIT_ASSET
}

static void ChaosOutfit_MaybeBindOutfit(sol::state& Lua, FLuaSessionData& Session)
{
#if !WITH_CHAOS_OUTFIT_ASSET
	Session.Log(TEXT("[WARN] ChaosOutfitAsset plugin is not loaded. Enable ChaosOutfitAsset in Edit > Plugins to use outfit features."));
	return;
#endif
	BindChaosOutfit(Lua, Session);
}

REGISTER_LUA_BINDING(ChaosOutfit, ChaosOutfitDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	ChaosOutfit_MaybeBindOutfit(Lua, Session);
});
