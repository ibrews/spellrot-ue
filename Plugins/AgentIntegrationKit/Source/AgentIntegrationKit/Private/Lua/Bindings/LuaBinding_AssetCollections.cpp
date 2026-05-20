// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "CollectionManagerModule.h"
#include "ICollectionManager.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "ICollectionContainer.h"
#endif
#include "CollectionManagerTypes.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> AssetCollectionsDocs = {
	{ TEXT("list_collections(share_type?)"), TEXT("List asset collections. share_type: local|private|shared|system (default: all). Returns name, type, storage_mode, asset_count, parent info"), TEXT("table") },
	{ TEXT("create_collection(name, share_type?, storage_mode?)"), TEXT("Create a new asset collection. share_type default 'local', storage_mode: 'static'|'dynamic' (default 'static')"), TEXT("table") },
	{ TEXT("delete_collection(name, share_type?)"), TEXT("Delete an asset collection"), TEXT("table") },
	{ TEXT("add_to_collection(collection_name, asset_paths, share_type?)"), TEXT("Add asset(s) to a collection. asset_paths: single string or table of strings"), TEXT("table") },
	{ TEXT("remove_from_collection(collection_name, asset_paths, share_type?)"), TEXT("Remove asset(s) from a collection. asset_paths: single string or table of strings"), TEXT("table") },
	{ TEXT("get_assets_in_collection(collection_name, share_type?, recursive?)"), TEXT("Get all asset paths in a collection. recursive: 'self' (default), 'children', 'parents', 'all'"), TEXT("table") },
	{ TEXT("rename_collection(old_name, new_name, share_type?)"), TEXT("Rename an asset collection"), TEXT("table") },
	{ TEXT("collection_contains(collection_name, asset_path, share_type?)"), TEXT("Check if an asset is in a collection"), TEXT("table") },
	{ TEXT("empty_collection(name, share_type?)"), TEXT("Remove all assets from a collection (keeps the collection itself)"), TEXT("table") },
	{ TEXT("reparent_collection(name, share_type?, parent_name?, parent_share_type?)"), TEXT("Set parent of a collection. Omit parent_name to make it a root collection"), TEXT("table") },
	{ TEXT("get_child_collections(name, share_type?)"), TEXT("Get direct child collections of a collection"), TEXT("table") },
	{ TEXT("get_parent_collection(name, share_type?)"), TEXT("Get the parent collection of a collection, if any"), TEXT("table") },
	{ TEXT("get_root_collections(share_type?)"), TEXT("Get all root-level (no parent) collections"), TEXT("table") },
	{ TEXT("get_collections_containing(asset_path, share_type?)"), TEXT("Reverse lookup: find all collections containing a given asset"), TEXT("table") },
	{ TEXT("set_dynamic_query(collection_name, query_text, share_type?)"), TEXT("Set the dynamic query text for a dynamic collection"), TEXT("table") },
	{ TEXT("get_dynamic_query(collection_name, share_type?)"), TEXT("Get the dynamic query text for a dynamic collection"), TEXT("table") },
	{ TEXT("collection_info(name, share_type?)"), TEXT("Get detailed info: storage_mode, status, color, parent, child count, asset count"), TEXT("table") },
	{ TEXT("set_collection_color(name, share_type?, r?, g?, b?, a?)"), TEXT("Set collection color. Omit r/g/b/a to clear color"), TEXT("table") },
	{ TEXT("save_collection(name, share_type?)"), TEXT("Save a collection to disk and check into source control if applicable"), TEXT("table") },
	{ TEXT("update_collection(name, share_type?)"), TEXT("Update a collection from source control to get latest version"), TEXT("table") },
	{ TEXT("is_valid_collection_name(name, share_type?)"), TEXT("Check if a name is valid for a new collection. share_type: local|private|shared|system|all (default 'all')"), TEXT("table") },
	{ TEXT("create_unique_collection_name(base_name, share_type?)"), TEXT("Generate a unique collection name from a base name (appends number if needed)"), TEXT("table") },
	{ TEXT("is_collection_read_only(share_type)"), TEXT("Check if collections of a given share type are read-only"), TEXT("table") },
	{ TEXT("is_valid_parent_collection(name, share_type, parent_name, parent_share_type?)"), TEXT("Check if a collection can be parented to another collection"), TEXT("table") },
	{ TEXT("get_classes_in_collection(collection_name, share_type?, recursive?)"), TEXT("Get all class paths in a collection. recursive: 'self' (default), 'children', 'parents', 'all'"), TEXT("table") },
};

// ─── Helpers ───

static ECollectionShareType::Type ParseShareType(const std::string& TypeStr)
{
	FString T = UTF8_TO_TCHAR(TypeStr.c_str());
	if (T.Equals(TEXT("local"), ESearchCase::IgnoreCase)) return ECollectionShareType::CST_Local;
	if (T.Equals(TEXT("private"), ESearchCase::IgnoreCase)) return ECollectionShareType::CST_Private;
	if (T.Equals(TEXT("shared"), ESearchCase::IgnoreCase)) return ECollectionShareType::CST_Shared;
	if (T.Equals(TEXT("system"), ESearchCase::IgnoreCase)) return ECollectionShareType::CST_System;
	if (T.Equals(TEXT("all"), ESearchCase::IgnoreCase)) return ECollectionShareType::CST_All;
	return ECollectionShareType::CST_Local; // default
}

static ECollectionStorageMode::Type ParseStorageMode(const std::string& ModeStr)
{
	FString M = UTF8_TO_TCHAR(ModeStr.c_str());
	if (M.Equals(TEXT("dynamic"), ESearchCase::IgnoreCase)) return ECollectionStorageMode::Dynamic;
	return ECollectionStorageMode::Static; // default
}

static const TCHAR* ShareTypeToString(ECollectionShareType::Type ShareType)
{
	switch (ShareType)
	{
	case ECollectionShareType::CST_Local:   return TEXT("local");
	case ECollectionShareType::CST_Private: return TEXT("private");
	case ECollectionShareType::CST_Shared:  return TEXT("shared");
	case ECollectionShareType::CST_System:  return TEXT("system");
	default:                                return TEXT("unknown");
	}
}

static const TCHAR* StorageModeToString(ECollectionStorageMode::Type Mode)
{
	switch (Mode)
	{
	case ECollectionStorageMode::Static:  return TEXT("static");
	case ECollectionStorageMode::Dynamic: return TEXT("dynamic");
	default:                              return TEXT("unknown");
	}
}

static ECollectionRecursionFlags::Flags ParseRecursionFlags(const std::string& ModeStr)
{
	FString M = UTF8_TO_TCHAR(ModeStr.c_str());
	if (M.Equals(TEXT("children"), ESearchCase::IgnoreCase)) return ECollectionRecursionFlags::SelfAndChildren;
	if (M.Equals(TEXT("parents"), ESearchCase::IgnoreCase)) return ECollectionRecursionFlags::SelfAndParents;
	if (M.Equals(TEXT("all"), ESearchCase::IgnoreCase)) return ECollectionRecursionFlags::All;
	return ECollectionRecursionFlags::Self; // default
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static const TSharedRef<ICollectionContainer>& GetContainer()
{
	FCollectionManagerModule& Module = FCollectionManagerModule::GetModule();
	ICollectionManager& Manager = Module.Get();
	return Manager.GetProjectCollectionContainer();
}
#endif

// Parse asset_paths argument: accepts single string or table of strings
static TArray<FSoftObjectPath> ParseAssetPaths(const sol::object& Arg)
{
	TArray<FSoftObjectPath> Paths;
	if (Arg.get_type() == sol::type::string)
	{
		std::string S = Arg.as<std::string>();
		if (!S.empty())
		{
			Paths.Add(FSoftObjectPath(UTF8_TO_TCHAR(S.c_str())));
		}
	}
	else if (Arg.get_type() == sol::type::table)
	{
		sol::table Tbl = Arg.as<sol::table>();
		for (auto& Pair : Tbl)
		{
			if (Pair.second.get_type() == sol::type::string)
			{
				std::string S = Pair.second.as<std::string>();
				if (!S.empty())
				{
					Paths.Add(FSoftObjectPath(UTF8_TO_TCHAR(S.c_str())));
				}
			}
		}
	}
	return Paths;
}

// ─── Implementation ───

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
REGISTER_LUA_BINDING(AssetCollections, AssetCollectionsDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ════════════════════════════════════════════════════════════════════
	// list_collections(share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("list_collections", [&Session](sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		bool bFilterByType = ShareTypeArg.has_value() && !ShareTypeArg.value().empty();
		ECollectionShareType::Type FilterType = bFilterByType
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		TArray<FCollectionNameType> Collections;
		Container->GetCollections(Collections);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (const FCollectionNameType& Collection : Collections)
		{
			if (bFilterByType && Collection.Type != FilterType)
			{
				continue;
			}

			ECollectionShareType::Type ShareType = Collection.Type;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Collection.Name.ToString()));
			Entry["type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(ShareType)));

			// Storage mode
			ECollectionStorageMode::Type StorageMode;
			if (Container->GetCollectionStorageMode(Collection.Name, ShareType, StorageMode))
			{
				Entry["storage_mode"] = std::string(TCHAR_TO_UTF8(StorageModeToString(StorageMode)));
			}

			// Asset count
			TArray<FSoftObjectPath> Assets;
			Container->GetAssetsInCollection(Collection.Name, ShareType, Assets);
			Entry["asset_count"] = static_cast<int>(Assets.Num());

			// Parent info
			TOptional<FCollectionNameType> Parent = Container->GetParentCollection(Collection.Name, ShareType);
			if (Parent.IsSet())
			{
				Entry["parent_name"] = std::string(TCHAR_TO_UTF8(*Parent->Name.ToString()));
				Entry["parent_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Parent->Type)));
			}

			// Child count
			TArray<FCollectionNameType> Children;
			Container->GetChildCollections(Collection.Name, ShareType, Children);
			Entry["child_count"] = static_cast<int>(Children.Num());

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] list_collections → %d collections found"), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// create_collection(name, share_type?, storage_mode?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("create_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::optional<std::string> StorageModeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] create_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;
		ECollectionStorageMode::Type StorageMode = StorageModeArg.has_value()
			? ParseStorageMode(StorageModeArg.value()) : ECollectionStorageMode::Static;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FText ErrorText;
		bool bSuccess = Container->CreateCollection(CollectionName, ShareType, StorageMode, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;
		Result["share_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(ShareType)));
		Result["storage_mode"] = std::string(TCHAR_TO_UTF8(StorageModeToString(StorageMode)));

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] create_collection '%s' (%s, %s)"),
				UTF8_TO_TCHAR(Name.c_str()), ShareTypeToString(ShareType), StorageModeToString(StorageMode)));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to create collection. It may already exist.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] create_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// delete_collection(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("delete_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] delete_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FText ErrorText;
		bool bSuccess = Container->DestroyCollection(CollectionName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] delete_collection '%s' (%s)"),
				UTF8_TO_TCHAR(Name.c_str()), ShareTypeToString(ShareType)));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to delete collection. It may not exist.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] delete_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// add_to_collection(collection_name, asset_paths, share_type?)
	//   asset_paths: single string or table of strings (bulk)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("add_to_collection", [&Session](const std::string& CollectionNameStr,
		sol::object AssetPathsArg, sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] add_to_collection: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		TArray<FSoftObjectPath> Paths = ParseAssetPaths(AssetPathsArg);
		if (Paths.Num() == 0)
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "At least one asset path is required";
			Session.Log(TEXT("[FAIL] add_to_collection: asset path is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));

		sol::table Result = LuaView.create_table();
		Result["collection"] = CollectionNameStr;

		if (Paths.Num() == 1)
		{
			FText ErrorText;
			bool bSuccess = Container->AddToCollection(CollectionName, ShareType, Paths[0], &ErrorText);
			Result["success"] = bSuccess;
			Result["asset_path"] = std::string(TCHAR_TO_UTF8(*Paths[0].ToString()));
			if (!bSuccess)
			{
				FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to add asset to collection. Check that the collection and asset path exist.") : ErrorText.ToString();
				Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			}
			Session.Log(FString::Printf(TEXT("[%s] add_to_collection '%s' -> '%s'"),
				bSuccess ? TEXT("OK") : TEXT("FAIL"),
				*Paths[0].ToString(), UTF8_TO_TCHAR(CollectionNameStr.c_str())));
		}
		else
		{
			int32 NumAdded = 0;
			FText ErrorText;
			bool bSuccess = Container->AddToCollection(CollectionName, ShareType, Paths, &NumAdded, &ErrorText);
			Result["success"] = bSuccess;
			Result["requested"] = static_cast<int>(Paths.Num());
			Result["added"] = NumAdded;
			if (!bSuccess)
			{
				FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to add some assets to collection.") : ErrorText.ToString();
				Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			}
			Session.Log(FString::Printf(TEXT("[%s] add_to_collection %d/%d assets -> '%s'"),
				bSuccess ? TEXT("OK") : TEXT("FAIL"),
				NumAdded, Paths.Num(), UTF8_TO_TCHAR(CollectionNameStr.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// remove_from_collection(collection_name, asset_paths, share_type?)
	//   asset_paths: single string or table of strings (bulk)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("remove_from_collection", [&Session](const std::string& CollectionNameStr,
		sol::object AssetPathsArg, sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] remove_from_collection: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		TArray<FSoftObjectPath> Paths = ParseAssetPaths(AssetPathsArg);
		if (Paths.Num() == 0)
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "At least one asset path is required";
			Session.Log(TEXT("[FAIL] remove_from_collection: asset path is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));

		sol::table Result = LuaView.create_table();
		Result["collection"] = CollectionNameStr;

		if (Paths.Num() == 1)
		{
			FText ErrorText;
			bool bSuccess = Container->RemoveFromCollection(CollectionName, ShareType, Paths[0], &ErrorText);
			Result["success"] = bSuccess;
			Result["asset_path"] = std::string(TCHAR_TO_UTF8(*Paths[0].ToString()));
			if (!bSuccess)
			{
				FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to remove asset from collection.") : ErrorText.ToString();
				Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			}
			Session.Log(FString::Printf(TEXT("[%s] remove_from_collection '%s' from '%s'"),
				bSuccess ? TEXT("OK") : TEXT("FAIL"),
				*Paths[0].ToString(), UTF8_TO_TCHAR(CollectionNameStr.c_str())));
		}
		else
		{
			int32 NumRemoved = 0;
			FText ErrorText;
			bool bSuccess = Container->RemoveFromCollection(CollectionName, ShareType, Paths, &NumRemoved, &ErrorText);
			Result["success"] = bSuccess;
			Result["requested"] = static_cast<int>(Paths.Num());
			Result["removed"] = NumRemoved;
			if (!bSuccess)
			{
				FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to remove some assets from collection.") : ErrorText.ToString();
				Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			}
			Session.Log(FString::Printf(TEXT("[%s] remove_from_collection %d/%d assets from '%s'"),
				bSuccess ? TEXT("OK") : TEXT("FAIL"),
				NumRemoved, Paths.Num(), UTF8_TO_TCHAR(CollectionNameStr.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_assets_in_collection(collection_name, share_type?, recursive?)
	//   recursive: 'self' (default), 'children', 'parents', 'all'
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_assets_in_collection", [&Session](const std::string& CollectionNameStr,
		sol::optional<std::string> ShareTypeArg, sol::optional<std::string> RecursiveArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] get_assets_in_collection: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;
		ECollectionRecursionFlags::Flags RecursionMode = RecursiveArg.has_value()
			? ParseRecursionFlags(RecursiveArg.value()) : ECollectionRecursionFlags::Self;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));

		TArray<FSoftObjectPath> Assets;
		Container->GetAssetsInCollection(CollectionName, ShareType, Assets, RecursionMode);

		sol::table Result = LuaView.create_table();
		Result["collection"] = CollectionNameStr;
		Result["count"] = static_cast<int>(Assets.Num());

		sol::table AssetTable = LuaView.create_table();
		int32 Idx = 1;
		for (const FSoftObjectPath& Asset : Assets)
		{
			AssetTable[Idx++] = std::string(TCHAR_TO_UTF8(*Asset.ToString()));
		}
		Result["assets"] = AssetTable;

		Session.Log(FString::Printf(TEXT("[OK] get_assets_in_collection '%s' → %d assets"),
			UTF8_TO_TCHAR(CollectionNameStr.c_str()), Assets.Num()));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// rename_collection(old_name, new_name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("rename_collection", [&Session](const std::string& OldNameStr,
		const std::string& NewNameStr, sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (OldNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Old collection name is required";
			Session.Log(TEXT("[FAIL] rename_collection: old name is required"));
			return sol::make_object(LuaView, Err);
		}

		if (NewNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "New collection name is required";
			Session.Log(TEXT("[FAIL] rename_collection: new name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		FName OldName = FName(UTF8_TO_TCHAR(OldNameStr.c_str()));
		FName NewName = FName(UTF8_TO_TCHAR(NewNameStr.c_str()));

		FText ErrorText;
		bool bSuccess = Container->RenameCollection(OldName, ShareType, NewName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["old_name"] = OldNameStr;
		Result["new_name"] = NewNameStr;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] rename_collection '%s' -> '%s'"),
				UTF8_TO_TCHAR(OldNameStr.c_str()), UTF8_TO_TCHAR(NewNameStr.c_str())));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to rename collection.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] rename_collection '%s' -> '%s': %s"),
				UTF8_TO_TCHAR(OldNameStr.c_str()), UTF8_TO_TCHAR(NewNameStr.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// collection_contains(collection_name, asset_path, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("collection_contains", [&Session](const std::string& CollectionNameStr,
		const std::string& AssetPathStr, sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] collection_contains: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		if (AssetPathStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Asset path is required";
			Session.Log(TEXT("[FAIL] collection_contains: asset path is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));
		FSoftObjectPath TargetAsset(UTF8_TO_TCHAR(AssetPathStr.c_str()));

		FText ErrorText;
		bool bContains = Container->IsObjectInCollection(TargetAsset, CollectionName, ShareType,
			ECollectionRecursionFlags::Self, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["contains"] = bContains;
		Result["collection"] = CollectionNameStr;
		Result["asset_path"] = AssetPathStr;
		if (!ErrorText.IsEmpty())
		{
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrorText.ToString()));
		}

		Session.Log(FString::Printf(TEXT("[OK] collection_contains '%s' in '%s' → %s"),
			UTF8_TO_TCHAR(AssetPathStr.c_str()), UTF8_TO_TCHAR(CollectionNameStr.c_str()),
			bContains ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// empty_collection(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("empty_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] empty_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		FText ErrorText;
		bool bSuccess = Container->EmptyCollection(CollectionName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] empty_collection '%s'"),
				UTF8_TO_TCHAR(Name.c_str())));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to empty collection.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] empty_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// reparent_collection(name, share_type?, parent_name?, parent_share_type?)
	//   Omit parent_name to make it a root collection
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("reparent_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::optional<std::string> ParentNameArg,
		sol::optional<std::string> ParentShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] reparent_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		FName ParentName = NAME_None;
		ECollectionShareType::Type ParentShareType = ShareType;
		if (ParentNameArg.has_value() && !ParentNameArg.value().empty())
		{
			ParentName = FName(UTF8_TO_TCHAR(ParentNameArg.value().c_str()));
			ParentShareType = ParentShareTypeArg.has_value()
				? ParseShareType(ParentShareTypeArg.value()) : ShareType;
		}

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		FText ErrorText;
		bool bSuccess = Container->ReparentCollection(CollectionName, ShareType, ParentName, ParentShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (ParentName != NAME_None)
		{
			Result["parent_name"] = std::string(TCHAR_TO_UTF8(*ParentName.ToString()));
			Result["parent_share_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(ParentShareType)));
		}

		if (bSuccess)
		{
			if (ParentName != NAME_None)
			{
				Session.Log(FString::Printf(TEXT("[OK] reparent_collection '%s' under '%s'"),
					UTF8_TO_TCHAR(Name.c_str()), *ParentName.ToString()));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[OK] reparent_collection '%s' → root"),
					UTF8_TO_TCHAR(Name.c_str())));
			}
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to reparent collection.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] reparent_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_child_collections(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_child_collections", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] get_child_collections: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		TArray<FCollectionNameType> Children;
		Container->GetChildCollections(CollectionName, ShareType, Children);

		sol::table Result = LuaView.create_table();
		Result["parent"] = Name;
		Result["count"] = static_cast<int>(Children.Num());

		sol::table ChildTable = LuaView.create_table();
		int32 Idx = 1;
		for (const FCollectionNameType& Child : Children)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Child.Name.ToString()));
			Entry["type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Child.Type)));
			ChildTable[Idx++] = Entry;
		}
		Result["children"] = ChildTable;

		Session.Log(FString::Printf(TEXT("[OK] get_child_collections '%s' → %d children"),
			UTF8_TO_TCHAR(Name.c_str()), Children.Num()));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_parent_collection(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_parent_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] get_parent_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		TOptional<FCollectionNameType> Parent = Container->GetParentCollection(CollectionName, ShareType);

		sol::table Result = LuaView.create_table();
		Result["collection"] = Name;
		Result["has_parent"] = Parent.IsSet();

		if (Parent.IsSet())
		{
			Result["parent_name"] = std::string(TCHAR_TO_UTF8(*Parent->Name.ToString()));
			Result["parent_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Parent->Type)));
		}

		Session.Log(FString::Printf(TEXT("[OK] get_parent_collection '%s' → %s"),
			UTF8_TO_TCHAR(Name.c_str()),
			Parent.IsSet() ? *Parent->Name.ToString() : TEXT("(root)")));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_root_collections(share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_root_collections", [&Session](sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		TArray<FCollectionNameType> Roots;

		if (ShareTypeArg.has_value() && !ShareTypeArg.value().empty())
		{
			ECollectionShareType::Type ShareType = ParseShareType(ShareTypeArg.value());
			TArray<FName> Names;
			Container->GetRootCollectionNames(ShareType, Names);
			for (const FName& N : Names)
			{
				Roots.Add(FCollectionNameType(N, ShareType));
			}
		}
		else
		{
			Container->GetRootCollections(Roots);
		}

		sol::table Result = LuaView.create_table();
		Result["count"] = static_cast<int>(Roots.Num());

		sol::table CollTable = LuaView.create_table();
		int32 Idx = 1;
		for (const FCollectionNameType& Root : Roots)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Root.Name.ToString()));
			Entry["type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Root.Type)));
			CollTable[Idx++] = Entry;
		}
		Result["collections"] = CollTable;

		Session.Log(FString::Printf(TEXT("[OK] get_root_collections → %d root collections"), Roots.Num()));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_collections_containing(asset_path, share_type?)
	//   Reverse lookup: find all collections that contain a given asset
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_collections_containing", [&Session](const std::string& AssetPathStr,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (AssetPathStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Asset path is required";
			Session.Log(TEXT("[FAIL] get_collections_containing: asset path is required"));
			return sol::make_object(LuaView, Err);
		}

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FSoftObjectPath ObjectPath(UTF8_TO_TCHAR(AssetPathStr.c_str()));

		sol::table Result = LuaView.create_table();
		Result["asset_path"] = AssetPathStr;

		if (ShareTypeArg.has_value() && !ShareTypeArg.value().empty())
		{
			// Filter by share type — returns FName array
			ECollectionShareType::Type ShareType = ParseShareType(ShareTypeArg.value());
			TArray<FName> CollectionNames;
			Container->GetCollectionsContainingObject(ObjectPath, ShareType, CollectionNames);

			Result["count"] = static_cast<int>(CollectionNames.Num());
			sol::table CollTable = LuaView.create_table();
			int32 Idx = 1;
			for (const FName& N : CollectionNames)
			{
				sol::table Entry = LuaView.create_table();
				Entry["name"] = std::string(TCHAR_TO_UTF8(*N.ToString()));
				Entry["type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(ShareType)));
				CollTable[Idx++] = Entry;
			}
			Result["collections"] = CollTable;

			Session.Log(FString::Printf(TEXT("[OK] get_collections_containing '%s' → %d collections"),
				UTF8_TO_TCHAR(AssetPathStr.c_str()), CollectionNames.Num()));
		}
		else
		{
			// All share types — returns FCollectionNameType array
			TArray<FCollectionNameType> Collections;
			Container->GetCollectionsContainingObject(ObjectPath, Collections);

			Result["count"] = static_cast<int>(Collections.Num());
			sol::table CollTable = LuaView.create_table();
			int32 Idx = 1;
			for (const FCollectionNameType& Coll : Collections)
			{
				sol::table Entry = LuaView.create_table();
				Entry["name"] = std::string(TCHAR_TO_UTF8(*Coll.Name.ToString()));
				Entry["type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Coll.Type)));
				CollTable[Idx++] = Entry;
			}
			Result["collections"] = CollTable;

			Session.Log(FString::Printf(TEXT("[OK] get_collections_containing '%s' → %d collections"),
				UTF8_TO_TCHAR(AssetPathStr.c_str()), Collections.Num()));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// set_dynamic_query(collection_name, query_text, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("set_dynamic_query", [&Session](const std::string& CollectionNameStr,
		const std::string& QueryText, sol::optional<std::string> ShareTypeArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] set_dynamic_query: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));
		FString Query = UTF8_TO_TCHAR(QueryText.c_str());

		FText ErrorText;
		bool bSuccess = Container->SetDynamicQueryText(CollectionName, ShareType, Query, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["collection"] = CollectionNameStr;
		Result["query"] = QueryText;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] set_dynamic_query '%s' = '%s'"),
				UTF8_TO_TCHAR(CollectionNameStr.c_str()), *Query));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to set dynamic query. Is the collection dynamic?") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] set_dynamic_query '%s': %s"),
				UTF8_TO_TCHAR(CollectionNameStr.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_dynamic_query(collection_name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_dynamic_query", [&Session](const std::string& CollectionNameStr,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] get_dynamic_query: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));

		FString QueryText;
		FText ErrorText;
		bool bSuccess = Container->GetDynamicQueryText(CollectionName, ShareType, QueryText, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["collection"] = CollectionNameStr;

		if (bSuccess)
		{
			Result["query"] = std::string(TCHAR_TO_UTF8(*QueryText));
			Session.Log(FString::Printf(TEXT("[OK] get_dynamic_query '%s' = '%s'"),
				UTF8_TO_TCHAR(CollectionNameStr.c_str()), *QueryText));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to get dynamic query. Is the collection dynamic?") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] get_dynamic_query '%s': %s"),
				UTF8_TO_TCHAR(CollectionNameStr.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// collection_info(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("collection_info", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] collection_info: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		if (!Container->CollectionExists(CollectionName, ShareType))
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection does not exist";
			Session.Log(FString::Printf(TEXT("[FAIL] collection_info '%s': does not exist"),
				UTF8_TO_TCHAR(Name.c_str())));
			return sol::make_object(LuaView, Err);
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = Name;
		Result["share_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(ShareType)));

		// Storage mode
		ECollectionStorageMode::Type StorageMode = ECollectionStorageMode::Static;
		bool bGotStorageMode = Container->GetCollectionStorageMode(CollectionName, ShareType, StorageMode);
		if (bGotStorageMode)
		{
			Result["storage_mode"] = std::string(TCHAR_TO_UTF8(StorageModeToString(StorageMode)));
		}

		// Status info
		FCollectionStatusInfo StatusInfo;
		if (Container->GetCollectionStatusInfo(CollectionName, ShareType, StatusInfo))
		{
			Result["is_dirty"] = StatusInfo.bIsDirty;
			Result["is_empty"] = StatusInfo.bIsEmpty;
			Result["uses_scc"] = StatusInfo.bUseSCC;
			Result["num_objects"] = StatusInfo.NumObjects;
		}

		// Asset count (for dynamic collections, NumObjects may be 0)
		TArray<FSoftObjectPath> Assets;
		Container->GetAssetsInCollection(CollectionName, ShareType, Assets);
		Result["asset_count"] = static_cast<int>(Assets.Num());

		// Parent
		TOptional<FCollectionNameType> Parent = Container->GetParentCollection(CollectionName, ShareType);
		Result["has_parent"] = Parent.IsSet();
		if (Parent.IsSet())
		{
			Result["parent_name"] = std::string(TCHAR_TO_UTF8(*Parent->Name.ToString()));
			Result["parent_type"] = std::string(TCHAR_TO_UTF8(ShareTypeToString(Parent->Type)));
		}

		// Child count
		TArray<FCollectionNameType> Children;
		Container->GetChildCollections(CollectionName, ShareType, Children);
		Result["child_count"] = static_cast<int>(Children.Num());

		// Color
		TOptional<FLinearColor> Color;
		if (Container->GetCollectionColor(CollectionName, ShareType, Color))
		{
			if (Color.IsSet())
			{
				sol::table ColorTable = LuaView.create_table();
				ColorTable["r"] = Color->R;
				ColorTable["g"] = Color->G;
				ColorTable["b"] = Color->B;
				ColorTable["a"] = Color->A;
				Result["color"] = ColorTable;
			}
		}

		// Dynamic query (if dynamic)
		if (bGotStorageMode && StorageMode == ECollectionStorageMode::Dynamic)
		{
			FString QueryText;
			if (Container->GetDynamicQueryText(CollectionName, ShareType, QueryText))
			{
				Result["dynamic_query"] = std::string(TCHAR_TO_UTF8(*QueryText));
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] collection_info '%s'"), UTF8_TO_TCHAR(Name.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// set_collection_color(name, share_type?, r?, g?, b?, a?)
	//   Omit r/g/b/a to clear the color
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("set_collection_color", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg,
		sol::optional<double> R, sol::optional<double> G, sol::optional<double> B, sol::optional<double> A,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] set_collection_color: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		TOptional<FLinearColor> NewColor;
		if (R.has_value())
		{
			NewColor = FLinearColor(
				static_cast<float>(R.value_or(0.0)),
				static_cast<float>(G.value_or(0.0)),
				static_cast<float>(B.value_or(0.0)),
				static_cast<float>(A.value_or(1.0))
			);
		}

		FText ErrorText;
		bool bSuccess = Container->SetCollectionColor(CollectionName, ShareType, NewColor, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (bSuccess)
		{
			if (NewColor.IsSet())
			{
				Session.Log(FString::Printf(TEXT("[OK] set_collection_color '%s' = (%.2f, %.2f, %.2f, %.2f)"),
					UTF8_TO_TCHAR(Name.c_str()), NewColor->R, NewColor->G, NewColor->B, NewColor->A));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[OK] set_collection_color '%s' → cleared"),
					UTF8_TO_TCHAR(Name.c_str())));
			}
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to set collection color.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] set_collection_color '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// save_collection(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("save_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] save_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		FText ErrorText;
		bool bSuccess = Container->SaveCollection(CollectionName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] save_collection '%s'"), UTF8_TO_TCHAR(Name.c_str())));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to save collection.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] save_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// update_collection(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("update_collection", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] update_collection: name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));

		FText ErrorText;
		bool bSuccess = Container->UpdateCollection(CollectionName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["name"] = Name;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] update_collection '%s'"), UTF8_TO_TCHAR(Name.c_str())));
		}
		else
		{
			FString ErrMsg = ErrorText.IsEmpty() ? TEXT("Failed to update collection.") : ErrorText.ToString();
			Result["error"] = std::string(TCHAR_TO_UTF8(*ErrMsg));
			Session.Log(FString::Printf(TEXT("[FAIL] update_collection '%s': %s"),
				UTF8_TO_TCHAR(Name.c_str()), *ErrMsg));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// is_valid_collection_name(name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("is_valid_collection_name", [&Session](const std::string& Name,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] is_valid_collection_name: name is required"));
			return sol::make_object(LuaView, Err);
		}

		// Default to CST_All to check against all share types
		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_All;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FString CollectionName = UTF8_TO_TCHAR(Name.c_str());

		FText ErrorText;
		bool bValid = Container->IsValidCollectionName(CollectionName, ShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["valid"] = bValid;
		Result["name"] = Name;

		if (!bValid && !ErrorText.IsEmpty())
		{
			Result["reason"] = std::string(TCHAR_TO_UTF8(*ErrorText.ToString()));
		}

		Session.Log(FString::Printf(TEXT("[OK] is_valid_collection_name '%s' → %s"),
			UTF8_TO_TCHAR(Name.c_str()), bValid ? TEXT("valid") : TEXT("invalid")));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// create_unique_collection_name(base_name, share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("create_unique_collection_name", [&Session](const std::string& BaseName,
		sol::optional<std::string> ShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (BaseName.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Base name is required";
			Session.Log(TEXT("[FAIL] create_unique_collection_name: base name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName Base = FName(UTF8_TO_TCHAR(BaseName.c_str()));
		FName UniqueName;
		Container->CreateUniqueCollectionName(Base, ShareType, UniqueName);

		sol::table Result = LuaView.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*UniqueName.ToString()));
		Result["base_name"] = BaseName;

		Session.Log(FString::Printf(TEXT("[OK] create_unique_collection_name '%s' → '%s'"),
			UTF8_TO_TCHAR(BaseName.c_str()), *UniqueName.ToString()));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// is_collection_read_only(share_type)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("is_collection_read_only", [&Session](const std::string& ShareTypeStr,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (ShareTypeStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Share type is required (local|private|shared|system)";
			Session.Log(TEXT("[FAIL] is_collection_read_only: share type is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ParseShareType(ShareTypeStr);
		const TSharedRef<ICollectionContainer>& Container = GetContainer();

		bool bReadOnly = Container->IsReadOnly(ShareType);

		sol::table Result = LuaView.create_table();
		Result["read_only"] = bReadOnly;
		Result["share_type"] = ShareTypeStr;

		Session.Log(FString::Printf(TEXT("[OK] is_collection_read_only '%s' → %s"),
			UTF8_TO_TCHAR(ShareTypeStr.c_str()), bReadOnly ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// is_valid_parent_collection(name, share_type, parent_name, parent_share_type?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("is_valid_parent_collection", [&Session](const std::string& Name,
		const std::string& ShareTypeStr, const std::string& ParentName,
		sol::optional<std::string> ParentShareTypeArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] is_valid_parent_collection: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		if (ParentName.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Parent collection name is required";
			Session.Log(TEXT("[FAIL] is_valid_parent_collection: parent name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ParseShareType(ShareTypeStr);
		ECollectionShareType::Type ParentShareType = ParentShareTypeArg.has_value()
			? ParseShareType(ParentShareTypeArg.value()) : ShareType;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FName ParentCollectionName = FName(UTF8_TO_TCHAR(ParentName.c_str()));

		FText ErrorText;
		bool bValid = Container->IsValidParentCollection(CollectionName, ShareType,
			ParentCollectionName, ParentShareType, &ErrorText);

		sol::table Result = LuaView.create_table();
		Result["valid"] = bValid;
		Result["collection"] = Name;
		Result["parent"] = ParentName;

		if (!bValid && !ErrorText.IsEmpty())
		{
			Result["reason"] = std::string(TCHAR_TO_UTF8(*ErrorText.ToString()));
		}

		Session.Log(FString::Printf(TEXT("[OK] is_valid_parent_collection '%s' under '%s' → %s"),
			UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(ParentName.c_str()),
			bValid ? TEXT("valid") : TEXT("invalid")));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// get_classes_in_collection(collection_name, share_type?, recursive?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("get_classes_in_collection", [&Session](const std::string& CollectionNameStr,
		sol::optional<std::string> ShareTypeArg, sol::optional<std::string> RecursiveArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CollectionNameStr.empty())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Collection name is required";
			Session.Log(TEXT("[FAIL] get_classes_in_collection: collection name is required"));
			return sol::make_object(LuaView, Err);
		}

		ECollectionShareType::Type ShareType = ShareTypeArg.has_value()
			? ParseShareType(ShareTypeArg.value()) : ECollectionShareType::CST_Local;
		ECollectionRecursionFlags::Flags RecursionMode = RecursiveArg.has_value()
			? ParseRecursionFlags(RecursiveArg.value()) : ECollectionRecursionFlags::Self;

		const TSharedRef<ICollectionContainer>& Container = GetContainer();
		FName CollectionName = FName(UTF8_TO_TCHAR(CollectionNameStr.c_str()));

		TArray<FTopLevelAssetPath> ClassPaths;
		Container->GetClassesInCollection(CollectionName, ShareType, ClassPaths, RecursionMode);

		sol::table Result = LuaView.create_table();
		Result["collection"] = CollectionNameStr;
		Result["count"] = static_cast<int>(ClassPaths.Num());

		sol::table ClassTable = LuaView.create_table();
		int32 Idx = 1;
		for (const FTopLevelAssetPath& ClassPath : ClassPaths)
		{
			ClassTable[Idx++] = std::string(TCHAR_TO_UTF8(*ClassPath.ToString()));
		}
		Result["classes"] = ClassTable;

		Session.Log(FString::Printf(TEXT("[OK] get_classes_in_collection '%s' → %d classes"),
			UTF8_TO_TCHAR(CollectionNameStr.c_str()), ClassPaths.Num()));
		return sol::make_object(LuaView, Result);
	});
});
#endif // ENGINE_MINOR_VERSION >= 6
