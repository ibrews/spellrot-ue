#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Blueprint/BlueprintUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

static TArray<FLuaFunctionDoc> CreateAssetDocs = {
	{ TEXT("create_asset(path, type, options?)"), TEXT("Create any Unreal asset — auto-discovers factory; 30+ types supported; call help() on result\n"
		"  For Blueprints: options={ParentClass=\"Character\"} (default: Actor)\n"
		"    Common parents: Actor, Character, Pawn, PlayerController, GameModeBase, HUD, ActorComponent, AnimInstance"), TEXT("{path, type, class} or bp object or nil") },
	{ TEXT("list_asset_types()"), TEXT("List all supported asset type aliases for create_asset()"), TEXT("{alias=class_path, ...}") },
};

REGISTER_LUA_BINDING(CreateAsset, CreateAssetDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// create_asset(path, type, options?) — generic asset creation
	Lua.set_function("create_asset", [&Session](const std::string& Path,
		const std::string& TypeName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FPath = UTF8_TO_TCHAR(Path.c_str());
		FString FType = UTF8_TO_TCHAR(TypeName.c_str());

		// Convert options table to TMap<FString, FString>
		TMap<FString, FString> OptionsMap;
		if (Options.has_value())
		{
			for (auto& Pair : Options.value())
			{
				if (!Pair.first.is<std::string>()) continue;
				FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());

				FString Value;
				if (Pair.second.is<std::string>())
				{
					Value = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
				}
				else if (Pair.second.is<bool>())
				{
					Value = Pair.second.as<bool>() ? TEXT("true") : TEXT("false");
				}
				else if (Pair.second.is<double>())
				{
					double V = Pair.second.as<double>();
					if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
						Value = FString::Printf(TEXT("%lld"), (long long)V);
					else
						Value = FString::Printf(TEXT("%g"), V);
				}
				OptionsMap.Add(Key, Value);
			}
		}

		NeoBlueprint::FCreateAssetResult Result = NeoBlueprint::CreateAssetGeneric(FPath, FType, OptionsMap);

		if (!Result.Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] create_asset(\"%s\", \"%s\") -> %s"),
				*FPath, *FType, *Result.Error));
			return sol::lua_nil;
		}

		FString AssetClassName = Result.Asset->GetClass()->GetName();
		Session.Log(FString::Printf(TEXT("[OK] create_asset(\"%s\", \"%s\") -> created (%s)"),
			*FPath, *FType, *AssetClassName));

		// Delegate to open_asset for full method handle (works for both BP and non-BP)
		sol::protected_function openAsset = LuaView["open_asset"];
		if (openAsset.valid())
		{
			auto assetResult = openAsset(Path);
			if (assetResult.valid()) return assetResult;
		}

		// Fallback: return basic info if open_asset isn't available
		sol::table Info = LuaView.create_table();
		Info["path"] = TCHAR_TO_UTF8(*FPath);
		Info["type"] = TCHAR_TO_UTF8(*AssetClassName);
		Info["class"] = TCHAR_TO_UTF8(*Result.Asset->GetClass()->GetPathName());
		return Info;
	});

	// list_asset_types() — enumerate all supported type aliases
	Lua.set_function("list_asset_types", [&Session](sol::this_state S) -> sol::table
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		TArray<TPair<FString, FString>> Types;
		NeoBlueprint::ListAssetTypeAliases(Types);

		for (const auto& Pair : Types)
		{
			Result[TCHAR_TO_UTF8(*Pair.Key)] = TCHAR_TO_UTF8(*Pair.Value);
		}

		Session.Log(FString::Printf(TEXT("[OK] list_asset_types() -> %d types"), Types.Num()));
		return Result;
	});
});
